// SPDX-License-Identifier: Apache-2.0

// dummy_feed.h の実装。

#include "sources/dummy_feed.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QImage>

#include <obs-module.h>
#include <util/platform.h>

#include "diag/diag_log.h"
#include "sources/composite_source.h"
#include "sources/decor_registry.h"
#include "sources/decor_paint.h"
#include "sources/dummy_tiles.h"
#include "sources/scene_names.h"
#include "sources/screen_feed.h"

namespace cordmux::sources {

namespace detail {

std::atomic<bool> g_dummy_active{false};

} // namespace detail

namespace {

// 1 往復の時間の下限 (2 秒)。
constexpr int kMinDummyCycleMs = 2000;

// フレームの間隔 (ナノ秒)。
constexpr std::uint64_t kDummyFrameIntervalNs = 1'000'000'000ULL / static_cast<std::uint64_t>(kDummyFps);

// 枠 1 つ分のフレーム。前のフレームの絵をそのまま持ち、フレームごとに動く線の列だけを書き換える (move_dummy_line)。
struct SlotFrame {
	std::vector<std::uint8_t> pixels; // NV12。大きさは dummy_state の width x height (dummy_nv12_layout)
	DummyLineStrip strip;             // 前のフレームの線の下にあった画素
};

// ダミーの状態。このロックは合成のソースのモジュールのロックより先に取る (このロックを持ったまま
// screen_frame_target_exists と detail::output_screen_video を呼ぶ。順は sources/composite_source.h)。
struct dummy_state {
	std::mutex mutex;
	// フレームの大きさ。OBS のキャンバスの大きさで、渡すスレッドが背景を描画して合わせる (ensure_backgrounds)。0 はまだ
	// 描画していない。
	int width = 0;
	int height = 0;
	std::uint64_t cycle_ns = kDefaultDummyCycleNs;
	std::uint64_t first_timestamp_ns = 0; // 線の位相の起点 (渡した最初のフレーム)
	int speaker_slot = 0;                 // いま話している人として光らせている枠 (0 は光らせていない)
	bool tick_registered = false;
	std::array<SlotFrame, kScreenSlotCount> frames;
	std::string label_format; // 空なら dummy_display_name を使う
};

dummy_state &state()
{
	static dummy_state instance;
	return instance;
}

// フレームを作って libobs に渡す専用のスレッドと、tick のコールバックからの知らせ。本物の画面共有の映像が pipeline/ の
// スレッドから渡すのと同じく、libobs がフレームをコピーする処理を、OBS の描画のスレッドの外で行うためである。渡す時機は
// tick のコールバックが描画の刻みの時刻で決め (dummy_frame_due)、このスレッドはその時刻のフレームを作る。前のフレームを
// 作り終える前に次の知らせが来たら、新しい方だけを作る (遅れた分を続けて渡さない)。
// ロックの順は、mu を持ったまま state().mutex を取らない (渡すスレッドは mu を放してから state().mutex を取る)。
struct dummy_worker {
	std::mutex mu;
	std::condition_variable cv;
	std::thread thread;
	bool stop = false;
	bool pending = false;        // 作るフレームの知らせがある
	std::uint64_t frame_time_ns = 0; // 作るフレームの時刻 (描画の刻みの時刻)
	int canvas_w = 0;
	int canvas_h = 0;
	DummyPacer pacer; // tick のコールバックだけが使う

	~dummy_worker()
	{
		// set_dummy_mode(false) が止めて待つ約束なので、ここに来るときは動いていない。約束が破られても、プロセスを
		// std::terminate で落とさないように切り離す。
		if (thread.joinable())
			thread.detach();
	}
};

dummy_worker &worker()
{
	static dummy_worker instance;
	return instance;
}

// OBS のキャンバスの大きさ (映像の基本の解像度)。読めなければ false。
bool read_canvas_size(int *width, int *height)
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0)
		return false;
	*width = static_cast<int>(ovi.base_width);
	*height = static_cast<int>(ovi.base_height);
	return true;
}

// 枠ごとの背景の画像を描画して、NV12 にする (ロックを持たずに渡すスレッドで呼び出す。番号は decor_paint.cpp が Qt の
// QImage と QPainter で描画する。名前ラベルも、描き直しのスレッドで同じように描画している)。描画するのは、ダミー表示を
// 始めたときと、キャンバスの大きさが変わったときだけである。NV12 への変換もこのときの 1 回だけで、フレームごとには変換
// しない。描画できなかった枠は空のまま返す。ダミー表示が OFF になったら、残りの枠を描画せずに戻る (OFF にした UI のスレッドは
// 渡すスレッドが終わるのを待つので、待つのを 1 つの枠の分までにする)。
std::array<std::vector<std::uint8_t>, kScreenSlotCount> paint_backgrounds(int width, int height)
{
	std::array<std::vector<std::uint8_t>, kScreenSlotCount> out;
	const std::size_t needed = dummy_nv12_layout(width, height).total_bytes();
	for (int slot = 1; slot <= kScreenSlotCount; ++slot) {
		if (!dummy_mode_active())
			break;
		const QImage image = paint_dummy_background(slot, width, height);
		if (image.isNull() || image.format() != QImage::Format_ARGB32_Premultiplied ||
		    static_cast<std::size_t>(image.bytesPerLine()) != static_cast<std::size_t>(width) * 4u)
			continue; // 描画できなければ単色の背景になる (install_backgrounds_locked が単色で埋める)
		// QImage の ARGB32 は、リトルエンディアンの環境では B、G、R、A の順 (dummy_nv12_from_bgra が読む並び)。
		std::vector<std::uint8_t> &bytes = out[static_cast<std::size_t>(slot - 1)];
		bytes.resize(needed);
		if (!dummy_nv12_from_bgra(image.constBits(), static_cast<std::size_t>(image.sizeInBytes()), width, height,
					  bytes.data(), bytes.size()))
			bytes.clear();
	}
	return out;
}

// 背景の画像 (NV12。大きさは width x height) を、そのままフレームにし、フレームの大きさを width x height にする (呼び出し元が
// state().mutex を持っていること)。画像はコピーせずにフレームへ移す。描画できなかった枠は単色にする。
void install_backgrounds_locked(dummy_state &s, int width, int height,
				std::array<std::vector<std::uint8_t>, kScreenSlotCount> &backgrounds)
{
	s.width = width;
	s.height = height;
	const std::size_t needed = dummy_nv12_layout(s.width, s.height).total_bytes();
	for (int slot = 1; slot <= kScreenSlotCount; ++slot) {
		SlotFrame &frame = s.frames[static_cast<std::size_t>(slot - 1)];
		std::vector<std::uint8_t> &bytes = backgrounds[static_cast<std::size_t>(slot - 1)];
		if (bytes.size() == needed) {
			frame.pixels = std::move(bytes);
		} else {
			frame.pixels.assign(needed, 0u);
			fill_dummy_base(slot, s.width, s.height, nullptr, 0u, frame.pixels.data(), frame.pixels.size());
		}
		frame.strip = DummyLineStrip{};
	}
}

// ダミーの人の名前 (「ダミー %1」の %1 に枠の番号)。書式が渡されていなければ、ini の
// cordmux.source.placeholder.label から作る (scene_names.h の dummy_display_name)。
std::string dummy_label(const std::string &format, int slot)
{
	if (format.empty())
		return dummy_display_name(slot);
	return fill_template(format, {std::to_string(slot)});
}

// ダミーの人の名前ラベルの情報を入れる (state().mutex を持たずに呼び出す。中で名前ラベルの表のロックを取る)。
void register_dummy_people(const std::string &format)
{
	for (int slot = 1; slot <= kScreenSlotCount; ++slot) {
		PersonDecor decor;
		decor.name = dummy_label(format, slot);
		set_person_decor(dummy_person_id(slot), decor);
	}
}

void forget_dummy_people()
{
	for (int slot = 1; slot <= kScreenSlotCount; ++slot)
		forget_person_decor(dummy_person_id(slot));
}

// フレームの大きさがキャンバスの大きさと違えば (ダミー表示を始めたときと、キャンバスの大きさが変わったとき)、番号の入った
// 背景を新しい大きさで描画して NV12 にし、フレームに入れる (渡すスレッドで呼ぶ)。描画と変換はロックの外で行う (UI の
// スレッドがロックを待たないように)。その間 (4 つの枠の 1080p で約 0.1 秒、2160p で約 0.35 秒。dummy_feed.h) はフレームを
// 渡さないので、枠は前の表示のまま止まる。描画している間に止められたら入れない。
void ensure_backgrounds(int canvas_w, int canvas_h)
{
	{
		dummy_state &s = state();
		std::lock_guard<std::mutex> lock(s.mutex);
		if (!dummy_mode_active() || (s.width == canvas_w && s.height == canvas_h))
			return;
	}
	auto backgrounds = paint_backgrounds(canvas_w, canvas_h);
	dummy_state &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	if (dummy_mode_active())
		install_backgrounds_locked(s, canvas_w, canvas_h, backgrounds);
}

// 時刻 frame_time のフレームを作って、すべての枠に渡す (渡すスレッドで、ensure_backgrounds の後に呼ぶ)。
void produce_frames(std::uint64_t frame_time, int canvas_w, int canvas_h)
{
	dummy_state &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	// ロックを取ってから読む。止めた後に知らせが残っていても、色のタイルを渡さない。背景がまだ入っていない (大きさが違う)
	// ときも渡さない。
	if (!dummy_mode_active() || canvas_w != s.width || canvas_h != s.height)
		return;
	if (s.first_timestamp_ns == 0)
		s.first_timestamp_ns = frame_time;
	// 線の位相はフレームの番号ではなく時刻から求める (フレームレートによらず同じ速さにするため)。
	const std::uint64_t elapsed = frame_time - s.first_timestamp_ns;

	// 話している人の光を、枠の番号の順に回す。ダミーの人の発言の時刻は、声が鳴る時刻と同じ時計 (os_gettime_ns) で渡す。
	const int speaker = dummy_speaker_slot(elapsed, s.cycle_ns, kScreenSlotCount);
	if (speaker != s.speaker_slot) {
		const std::uint64_t now = os_gettime_ns();
		if (s.speaker_slot != 0)
			set_voice_speaking(dummy_person_id(s.speaker_slot), 0, now);
		set_voice_speaking(dummy_person_id(speaker), now, 0);
		s.speaker_slot = speaker;
	}

	const DummyNv12Layout layout = dummy_nv12_layout(s.width, s.height);
	for (int slot = 1; slot <= kScreenSlotCount; ++slot) {
		// 渡す先 (合成のソースの子) がまだなければ飛ばす (ソースはここでは作らない)。
		if (!screen_frame_target_exists(slot))
			continue;

		SlotFrame &tile = s.frames[static_cast<std::size_t>(slot - 1)];
		if (!move_dummy_line(s.width, s.height, elapsed, s.cycle_ns, tile.pixels.data(), tile.pixels.size(), tile.strip))
			continue;

		// 本物の画面共有の映像 (pipeline/ の hand_frame_to_obs) と同じく、YUV 4:2:0 で渡す。色は dummy_tiles.h の
		// dummy_yuv_of_rgb で作った BT.709 のフルレンジである。
		obs_source_frame2 frame = {};
		frame.data[0] = tile.pixels.data();
		frame.data[1] = tile.pixels.data() + layout.y_bytes();
		frame.linesize[0] = static_cast<std::uint32_t>(layout.width);
		frame.linesize[1] = static_cast<std::uint32_t>(layout.uv_stride());
		frame.width = static_cast<std::uint32_t>(s.width);
		frame.height = static_cast<std::uint32_t>(s.height);
		frame.timestamp = frame_time;
		frame.format = VIDEO_FORMAT_NV12;
		frame.range = VIDEO_RANGE_FULL;
		frame.trc = VIDEO_TRC_DEFAULT;
		frame.flip = false;
		video_format_get_parameters_for_format(VIDEO_CS_709, frame.range, frame.format, frame.color_matrix,
							frame.color_range_min, frame.color_range_max);

		// push_screen_video ではなく、内側の関数に渡す。push_screen_video はダミー表示の間はフレームを
		// 捨てるので、そこに渡すと、ダミーのフレームも捨てられる。
		// 合成のソースの子は緩衝なしで受ける (pipeline の渡し方と同じ。ダミーのフレームは待ち行列を通さず、
		// 作ったその場で渡すので、緩衝なしなら最新の 1 つがそのまま表示される)。libobs はここでフレームの全体を
		// 自分の領域へコピーする (負荷は dummy_feed.h の set_dummy_mode のコメント)。
		detail::output_screen_video(slot, &frame, composite_present());
	}
}

void worker_main()
{
	dummy_worker &w = worker();
	std::unique_lock<std::mutex> lock(w.mu);
	for (;;) {
		w.cv.wait(lock, [&w]() { return w.stop || w.pending; });
		if (w.stop)
			return;
		const std::uint64_t frame_time = w.frame_time_ns;
		const int canvas_w = w.canvas_w;
		const int canvas_h = w.canvas_h;
		w.pending = false;
		lock.unlock();
		ensure_backgrounds(canvas_w, canvas_h);
		produce_frames(frame_time, canvas_w, canvas_h);
		lock.lock();
	}
}

// OBS の tick のコールバック (OBS の描画のスレッドで、描画の刻みごとに呼ばれる)。ダミー表示が OFF の間は、ロックも
// 取らずに戻る。ここでは渡す時機を決めて、渡すスレッドに知らせるだけにする (フレームを作る処理と libobs のコピーで、
// OBS の描画を待たせないため)。Qt も使わない。
void dummy_tick(void * /*param*/, float /*seconds*/)
{
	if (!dummy_mode_active())
		return;
	int canvas_w = 0;
	int canvas_h = 0;
	if (!read_canvas_size(&canvas_w, &canvas_h))
		return;
	// 時刻は OBS の描画の刻みの時刻 (obs_get_video_frame_time。刻みごとに決まった間隔で進む) を使う。コールバックが
	// 呼ばれた時刻 (os_gettime_ns) は刻みごとに揺れるので、渡すかどうかの判定と線の位置がその分揺れる。
	const std::uint64_t frame_time = obs_get_video_frame_time();
	if (frame_time == 0)
		return;
	dummy_worker &w = worker();
	{
		std::lock_guard<std::mutex> lock(w.mu);
		if (w.stop || !dummy_frame_due(w.pacer, frame_time, kDummyFrameIntervalNs))
			return;
		w.pending = true;
		w.frame_time_ns = frame_time;
		w.canvas_w = canvas_w;
		w.canvas_h = canvas_h;
	}
	w.cv.notify_one();
}

// 渡すスレッドを始める (UI のスレッドから、set_dummy_mode(true) の中で呼ぶ)。
void start_worker()
{
	dummy_worker &w = worker();
	std::lock_guard<std::mutex> lock(w.mu);
	if (w.thread.joinable())
		return;
	w.stop = false;
	w.pending = false;
	w.pacer = DummyPacer{};
	w.thread = std::thread(worker_main);
}

// 渡すスレッドを止めて、終わるのを待つ (ロックを持たずに呼ぶ)。戻った後は、ダミーのフレームを渡さない。
void stop_worker()
{
	dummy_worker &w = worker();
	{
		std::lock_guard<std::mutex> lock(w.mu);
		if (!w.thread.joinable())
			return;
		w.stop = true;
	}
	w.cv.notify_one();
	w.thread.join();
	std::lock_guard<std::mutex> lock(w.mu);
	w.thread = std::thread();
	w.pending = false;
}

} // namespace

void set_dummy_mode(bool on)
{
	bool add_tick = false;
	bool remove_tick = false;
	int width = 0;
	int height = 0;
	std::string format;
	int lit_slot = 0;

	if (on)
		read_canvas_size(&width, &height); // 読めなければ 0 のまま (渡すスレッドが読めたときに合わせる)

	{
		dummy_state &s = state();
		std::lock_guard<std::mutex> lock(s.mutex);
		if (detail::g_dummy_active.load(std::memory_order_relaxed) == on)
			return;

		if (on) {
			s.first_timestamp_ns = 0;
			s.speaker_slot = 0;
			// 0 にして、渡すスレッドが最初のフレームの前に背景を描画するようにする (ensure_backgrounds)。
			s.width = 0;
			s.height = 0;
			for (SlotFrame &frame : s.frames)
				frame = SlotFrame{};
			add_tick = !s.tick_registered;
			s.tick_registered = true;
		} else {
			detail::g_dummy_active.store(false, std::memory_order_relaxed);
			lit_slot = s.speaker_slot;
			s.speaker_slot = 0;
			remove_tick = s.tick_registered;
			s.tick_registered = false;
			for (SlotFrame &frame : s.frames) {
				frame.pixels.clear();
				frame.pixels.shrink_to_fit();
				frame.strip = DummyLineStrip{};
			}
			s.width = 0;
			s.height = 0;
		}
		format = s.label_format;
	}

	if (on) {
		// 名前ラベルの情報を先に入れてから、フラグを true にする (最初のフレームから名前が表示される)。背景の画像は
		// 渡すスレッドが最初のフレームの前に描画する (ensure_backgrounds。UI のスレッドで描画すると、4 つの枠の 1080p で
		// 約 0.1 秒、UI が止まるため)。
		register_dummy_people(format);
		{
			dummy_state &s = state();
			std::lock_guard<std::mutex> lock(s.mutex);
			detail::g_dummy_active.store(true, std::memory_order_relaxed);
		}
		start_worker();
	}

	// コールバックの登録と解除は、ロックの外で行う。obs_remove_tick_callback は実行中のコールバックが終わるのを
	// 待つので、ロックを持ったまま呼ぶと、そのコールバックがロックを待って止まる。
	if (add_tick)
		obs_add_tick_callback(dummy_tick, nullptr);
	if (remove_tick)
		obs_remove_tick_callback(dummy_tick, nullptr);

	if (!on) {
		// 渡すスレッドを止めてから枠を空ける (止める前に空けると、作りかけのフレームが空けた後に渡される)。
		stop_worker();
		// 止めたら、すべての画面共有の枠をすぐ空ける (フレームが来ないだけでは枠を空けないので)。実際の
		// フレームが来ている枠は、次のフレームで表示し直す。
		for (int slot = 1; slot <= kScreenSlotCount; ++slot)
			clear_screen_now(slot);
		// 光らせていた枠は、話し終えたことにする (止めた後に光り続けない)。
		if (lit_slot != 0)
			set_voice_speaking(dummy_person_id(lit_slot), 0, os_gettime_ns());
		forget_dummy_people();
	}

	// ダミー表示の ON と OFF のたびに、ログに 1 行書く (ダミーを表示した時間を、実際の映像と区別できるように)。
	// ダミーの時間の一覧 (PipelineSnapshot::dummy_intervals) と、診断情報への書き出しは pipeline/ が行い、
	// ここでは切り替わったことだけをログに書く。
	if (on && width > 0 && height > 0) {
		diag::logf(LOG_INFO, "Placeholders turned on (%dx%d, %d fps); video and voice from Vesktop aren't shown", width,
			   height, kDummyFps);
	} else if (on) {
		// キャンバスの大きさがまだ読めない (渡すスレッドが読めたときに合わせる)。
		diag::logf(LOG_INFO, "Placeholders turned on (canvas size unknown, %d fps); video and voice from Vesktop aren't shown",
			   kDummyFps);
	} else {
		diag::logf(LOG_INFO, "Placeholders turned off; video and voice from Vesktop are shown again");
	}
}

void set_dummy_cycle_ms(int ms)
{
	dummy_state &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.cycle_ns = static_cast<std::uint64_t>(std::max(ms, kMinDummyCycleMs)) * 1'000'000ULL;
}

void set_dummy_label_format(const std::string &format_with_percent1)
{
	bool active = false;
	{
		dummy_state &s = state();
		std::lock_guard<std::mutex> lock(s.mutex);
		if (s.label_format == format_with_percent1)
			return;
		s.label_format = format_with_percent1;
		active = detail::g_dummy_active.load(std::memory_order_relaxed);
	}
	if (active)
		register_dummy_people(format_with_percent1);
}

} // namespace cordmux::sources

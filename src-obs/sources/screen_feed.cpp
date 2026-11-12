// SPDX-License-Identifier: Apache-2.0

// screen_feed.h の実装。

#include "sources/screen_feed.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

#include <util/platform.h>

#include "sources/composite_source.h"
#include "sources/dummy_feed.h"
#include "sources/scene_names.h"

namespace cordmux::sources {

namespace {

// 表示の遅れの統計 (set_screen_shown_correction_us の宣言)。プロセスの間ずっと持つ。表示に選ばれたフレームは OBS の
// 映像のスレッドが async_mutex を持ったまま伝えてくるので、ほかのロックを取らない mu だけを取る。取り出す側
// (pipeline の統計) も mu だけで取り出す。
struct ShownSlot {
	// 補正が設定されていないことを示す値 (実際の補正は何 ms にもならないので、この値にはならない)。
	static constexpr std::int64_t kUnset = std::numeric_limits<std::int64_t>::min();

	std::atomic<std::int64_t> correction_us{kUnset};
	std::mutex mu;
	pipeline::ShownFrameWindow window;
};

ShownSlot g_shown[kScreenTileCount];

// OBS の時計 (os_gettime_ns) の us。pipeline.cpp の obs_now_us と同じ時計で、set_screen_shown_correction_us の補正を
// 引いたタイムスタンプ (送信側のタイムスタンプを OBS の時計に換算した値) と並べられる。
std::int64_t obs_now_us()
{
	return std::int64_t(os_gettime_ns() / 1000ULL);
}

} // namespace

namespace detail {

void output_screen_video(int slot, const obs_source_frame2 *frame, bool unbuffered)
{
	// 合成のソースの子に渡す。合成のソースがなければ何もしない。
	if (composite_present())
		output_tile_video(slot, frame, unbuffered);
}

} // namespace detail

bool screen_frame_target_exists(int /*slot*/)
{
	return composite_present();
}

void set_screen_shown_correction_us(int slot, std::int64_t correction_us)
{
	if (slot < 1 || slot > kScreenTileCount)
		return;
	g_shown[slot - 1].correction_us.store(correction_us, std::memory_order_relaxed);
}

void note_screen_frame_shown(int slot, std::uint64_t frame_timestamp_ns)
{
	if (slot < 1 || slot > kScreenTileCount)
		return;
	ShownSlot &shown = g_shown[slot - 1];
	const std::int64_t correction_us = shown.correction_us.load(std::memory_order_relaxed);
	if (correction_us == ShownSlot::kUnset)
		return;
	// 時計はロックを取る前に読む (ロックの中は足し上げの 1 回だけにする)。
	const std::int64_t now_us = obs_now_us();
	std::lock_guard<std::mutex> lock(shown.mu);
	shown.window.note(frame_timestamp_ns, now_us, correction_us);
}

pipeline::LatencySummary take_screen_shown_latency(int slot)
{
	if (slot < 1 || slot > kScreenTileCount)
		return pipeline::LatencySummary{};
	ShownSlot &shown = g_shown[slot - 1];
	std::lock_guard<std::mutex> lock(shown.mu);
	return shown.window.take();
}

void clear_screen_now(int slot)
{
	clear_tile_now(slot);
}

void hold_then_clear_screen(int slot, std::uint64_t hold_ns)
{
	hold_then_clear_tile(slot, hold_ns);
}

void push_screen_video(int slot, const obs_source_frame2 *frame, bool unbuffered)
{
	// ダミー表示の間は、実際のフレームをここで捨てる (色のタイルのフレームは、dummy_feed.cpp が
	// detail::output_screen_video を直接呼んで渡す)。実際の音は pipeline/ の add_mixed_audio_locked が捨てる。
	if (dummy_mode_active())
		return;

	detail::output_screen_video(slot, frame, unbuffered);
}

} // namespace cordmux::sources

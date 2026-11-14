// SPDX-License-Identifier: Apache-2.0

// composite_source.h の実装。

#include <algorithm>
#include "sources/composite_source.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "diag/diag_log.h"
#include "install/uninstall.h"
#include "ledger/text_ids.h"
#include "sources/composite_decor.h"
#include "sources/composite_frame_plan.h"
#include "sources/composite_layout.h"
#include "sources/frame_origin.h"
#include "sources/frame_sink.h"
#include "sources/scene_names.h"
#include "sources/screen_feed.h"
#include "sources/target_ref.h"

namespace cordmux::sources {

namespace {

// 合成のソース 1 つ分の状態。OBS の context.data は CompositePtr (shared_ptr) を指す。合成のソースの一覧
// (g_composites) と、他のスレッドから出す音 (output_composite_audio) と、自分の枠の映像の選び直し
// (composite_bind_self_frame) も同じ状態を持つので、shared_ptr で持つ。
struct composite_data {
	// 作った合成のソース。destroy で nullptr にする (life_mu の下)。
	obs_source_t *source = nullptr;

	// 名前ラベルと話している人の光の画像とテクスチャ。描画のスレッドが使い、破棄は composite_destroy が
	// グラフィックスのコンテキストの中で行う (composite_decor.h)。
	CompositeDecor decor;

	// source が生きているかを守る。output_composite_audio と composite_bind_self_frame は、一覧から取り出した状態に
	// 対して OBS の関数を呼ぶので、destroy と同時に source を使わないようにする。持ったまま呼ぶ OBS の関数が cordmux の
	// このロックを取ることはない。
	std::mutex life_mu;

	// 描画の途中か。自分の枠に映すシーンが、この合成のソースを含むと、描画の中でまたこの描画が呼ばれる (循環)。
	// そのときは 2 回目を何もせずに返し、OBS を落とさない。
	std::atomic<bool> rendering{false};
};

using CompositePtr = std::shared_ptr<composite_data>;

// 子に渡したフレームがダミーかの記録と、子のテクスチャに入っているフレームがダミーか (sources/frame_origin.h)。渡す側
// (output_tile_video)、tick のコールバック (composite_tick)、子の測定のフィルタの filter_video (OBS の tick の中、子の
// async_mutex の下)、合成のソースの描画 (子を描いた後) が mu を取る。mu は葉で、持ったまま何も取らない。
struct TileOrigins {
	std::mutex mu;
	FrameOriginLog log;
	TileTextureOrigin texture;
};

// 画面の枠 1 つ分の子。FrameSink が出力のロックと空ける予約を持つ。
struct TileState {
	int slot = 0;
	obs_source_t *source = nullptr; // 作った参照 1 つを持つ。解放は release_children_locked だけ
	FrameSink sink;
	// 子の測定のフィルタも同じ物を持つ (子より先に TileState が消えても、フィルタが読み続けられるように shared_ptr)。
	std::shared_ptr<TileOrigins> origins;
};

// 子と tick のコールバックの作成と解放を、1 つずつ順に行うためのロック。作成と解放は OBS の関数を長く呼ぶので、
// module のロックとは別にする。描画のスレッドは取らない。
std::mutex g_lifecycle_mu;
bool g_tick_registered = false;

// 番組の描画の前と後のコールバック (obs_add_main_render_callback と obs_add_main_rendered_callback。下の composite_main_render
// と composite_main_rendered)。
void composite_main_render(void *param, uint32_t cx, uint32_t cy);
void composite_main_rendered(void *param);

// module のロック。次のものだけを守る (composite_source.h のロックの説明)。
std::mutex g_mu;
std::vector<CompositePtr> g_composites;
std::array<std::shared_ptr<TileState>, kScreenTileCount> g_tiles;
CompositeInput g_input;
std::string g_last_layout_line;
std::size_t g_last_dropped = 0;

// 合成のソースの数。ロックを取らずに composite_present が読む。書くのは g_mu の下。
std::atomic<int> g_composite_count{0};

// 自分の枠の映像を描くソース。設定ウィンドウで選んだ 1 つを、すべての合成のソースが描く (TargetRef の弱参照は自分のロックで守る)。
// g_self_frame_mu は、選び直しと、各合成のソースへの活性の伝え方を 1 つずつ順に行うためのロックで、描画のスレッドは取らない。
TargetRef g_self_frame;
std::mutex g_self_frame_mu;

// 動画のフレーム 1 つ分の、合成のソースが描くもの。そのフレームで最初に描いた合成のソースが作り、同じフレームの合成のソースの
// 描画 (出力とプレビューとプロジェクター、合成のソースが複数あるときのほかの合成のソース) は同じ値を使う。子の大きさは、渡すスレッドの
// obs_source_output_video が libobs の async_active を書き換えるので、描画の途中でも 0 と 0 以外の間で変わる (obs-source.c の
// obs_source_output_video_internal と get_base_width)。読むたびに決めると、合成のソースとフィルタで同じフレームの判定が
// 食い違うためである。canvas_w と canvas_h は、キャンバスの大きさを読めなかったとき 0 (何も描かない)。
struct CompositeFrame {
	int canvas_w = 0;
	int canvas_h = 0;
	CompositeInput input;
	CompositeFramePlan plan;
};

CompositeFrameMemo<CompositeFrame> g_frame;

// 合成のソースの tick (video_tick) が最後に来たフレームの時刻 (obs_get_video_frame_time)。この値がそのフレームの時刻と同じ
// 描画のスレッドの描画だけが、g_frame の値を共にする (composite_frame_plan.h の composite_frame_scope)。
std::atomic<std::uint64_t> g_composite_tick_time{0};

// 番組の描画の印 (composite_frame_plan.h の ProgramPassMark)。立て降ろしも読み書きも描画のスレッドだけなので、スレッドごとに
// 持つ。ほかのスレッド (obs-websocket のスクリーンショット) の印は立たない。
thread_local ProgramPassMark t_program_pass;

// 直前の番組の描画で、合成のソースが 1 人の映像だけを全面に描いたか。番組の描画の後のコールバックが書き、番組の描画の外の
// フィルタ (プレビュー、プロジェクター、スクリーンショット) が読む。
std::atomic<bool> g_last_program_solo{false};

// ---- 子 (cordmux_tile) と表示の測定のフィルタ (cordmux_tile_probe) ----

struct tile_data {
	int slot = 0;
};

const char *tile_get_name(void * /*type_data*/)
{
	return kCompositeDisplayName;
}

void *tile_create(obs_data_t *settings, obs_source_t * /*source*/)
{
	// 非同期の映像のソースは、context.data が nullptr だと描画されない (OBS は create が値を返さないソースを
	// 描画しない)。値の中身は使わない。
	auto *data = new tile_data();
	data->slot = static_cast<int>(obs_data_get_int(settings, kSlotSettingKey));
	return data;
}

void tile_destroy(void *vptr)
{
	delete static_cast<tile_data *>(vptr);
}

struct probe_data {
	obs_source_t *filter = nullptr;
	int slot = 0;
	// make_tile が子に付ける前に入れる (付ける前は filter_video が呼ばれない)。
	std::shared_ptr<TileOrigins> origins;
};

const char *probe_get_name(void * /*type_data*/)
{
	return kCompositeDisplayName;
}

void *probe_create(obs_data_t *settings, obs_source_t *filter)
{
	auto *data = new probe_data();
	data->filter = filter;
	data->slot = static_cast<int>(obs_data_get_int(settings, kSlotSettingKey));
	return data;
}

void probe_destroy(void *vptr)
{
	delete static_cast<probe_data *>(vptr);
}

// 子で OBS が表示に選んだフレームを受け取る (理由は composite_source.h)。OBS の映像のスレッドが子の async_mutex を持ったまま呼ぶので、OBS の関数を呼ばず、
// note_screen_frame_shown が取るロックと、ダミーかの記録のロック (TileOrigins::mu) を 1 つずつ取る。どちらも葉である。
// フレームはそのまま返す。
obs_source_frame *probe_filter_video(void *vptr, obs_source_frame *frame)
{
	if (!frame)
		return frame;
	const auto *data = static_cast<const probe_data *>(vptr);
	note_screen_frame_shown(data->slot, frame->timestamp);
	if (data->origins) {
		std::lock_guard<std::mutex> lock(data->origins->mu);
		bool dummy = false;
		if (data->origins->log.lookup(frame->timestamp, &dummy))
			data->origins->texture.on_selected(dummy);
	}
	return frame;
}

void probe_video_render(void *vptr, gs_effect_t * /*effect*/)
{
	// 映像は加工せずに通す。
	obs_source_skip_video_filter(static_cast<probe_data *>(vptr)->filter);
}

// 枠 slot の子を作る。非公開のソースで、OBS の全ソースの一覧には入るが、シーンコレクションには保存されず、
// obs_enum_sources にも出ない (obs_source_create_private)。OBS_SOURCE_CAP_DISABLED は「ソースを追加」の一覧から隠す
// 印で、非公開の作成 (obs_source_create_internal) は見ないので作れる。
std::shared_ptr<TileState> make_tile(int slot)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, kSlotSettingKey, slot);

	const std::string name = "cordmux tile " + std::to_string(slot);
	auto origins = std::make_shared<TileOrigins>();
	obs_source_t *tile = obs_source_create_private(kTileSourceTypeId, name.c_str(), settings);
	if (tile) {
		const std::string probe_name = name + " probe";
		obs_source_t *probe = obs_source_create_private(kTileProbeFilterTypeId, probe_name.c_str(), settings);
		if (probe) {
			if (auto *probe_state = static_cast<probe_data *>(obs_obj_get_data(probe)))
				probe_state->origins = origins;
			obs_source_filter_add(tile, probe);
			obs_source_release(probe); // 子が自分の参照を持つ
		} else {
			diag::logf(LOG_WARNING, "Couldn't create the display delay probe for tile %d", slot);
		}
	}
	obs_data_release(settings);

	if (!tile) {
		diag::logf(LOG_WARNING, "Couldn't create the screen share tile for slot %d", slot);
		return nullptr;
	}
	auto state = std::make_shared<TileState>();
	state->slot = slot;
	state->source = tile;
	state->sink.source = tile;
	state->origins = std::move(origins);
	return state;
}

std::shared_ptr<TileState> tile_state(int slot)
{
	if (slot < 1 || slot > kScreenTileCount)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_mu);
	return g_tiles[static_cast<std::size_t>(slot - 1)];
}

// 子の空ける予約の期限を見る tick のコールバック。OBS は draw_callbacks_mutex を持ったまま呼ぶので、取るのは
// module のロックだけにして、子の出力のロックは待たずに試す (FrameSink::tick)。ソースの tick (子がフレームを選ぶ) より前に
// 呼ばれるので、前の tick で選ばれて描かれなかったフレームを、子のテクスチャの元の候補から外す (TileTextureOrigin::on_tick)。
void composite_tick(void * /*param*/, float /*seconds*/)
{
	std::array<std::shared_ptr<TileState>, kScreenTileCount> tiles;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		tiles = g_tiles;
	}
	// 描画のスレッドの 1 周の始め。番組の描画の印を降ろす (前の周の描画の途中でコールバックが外れて、降ろされずに残った印も)。
	program_pass_tick(t_program_pass);
	for (const std::shared_ptr<TileState> &tile : tiles) {
		if (!tile)
			continue;
		tile->sink.tick();
		if (tile->origins) {
			std::lock_guard<std::mutex> lock(tile->origins->mu);
			tile->origins->texture.on_tick();
		}
	}
}

// 子を作って module の表に入れる。呼び出し元が g_lifecycle_mu を持っていること。
void create_children_locked()
{
	std::array<std::shared_ptr<TileState>, kScreenTileCount> made;
	for (int slot = 1; slot <= kScreenTileCount; ++slot)
		made[static_cast<std::size_t>(slot - 1)] = make_tile(slot);

	if (!g_tick_registered) {
		obs_add_tick_callback(composite_tick, nullptr);
		obs_add_main_render_callback(composite_main_render, nullptr);
		obs_add_main_rendered_callback(composite_main_rendered, nullptr);
		g_tick_registered = true;
	}

	std::lock_guard<std::mutex> lock(g_mu);
	g_tiles = std::move(made);
}

// 子を表から外して解放する。呼び出し元が g_lifecycle_mu を持っていること。外した後は、フレームの出力も空ける処理も
// 子に届かない。出力の途中の呼び出しがあれば、FrameSink::detach が終わるのを待つ。描画のスレッドが取った参照は
// 描画が終わるまで子を生かす。
void release_children_locked()
{
	std::array<std::shared_ptr<TileState>, kScreenTileCount> taken;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		taken = std::move(g_tiles);
		for (std::shared_ptr<TileState> &tile : g_tiles)
			tile.reset();
		g_last_layout_line.clear();
		g_last_dropped = 0;
	}

	if (g_tick_registered) {
		obs_remove_tick_callback(composite_tick, nullptr);
		obs_remove_main_render_callback(composite_main_render, nullptr);
		obs_remove_main_rendered_callback(composite_main_rendered, nullptr);
		g_tick_registered = false;
	}

	for (const std::shared_ptr<TileState> &tile : taken) {
		if (!tile)
			continue;
		tile->sink.detach();
		obs_source_release(tile->source);
	}
}

void add_composite(const CompositePtr &data)
{
	std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
	bool first = false;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		first = g_composites.empty();
	}
	// アンインストールの最中は子を作らない (OBS を閉じるまで cordmux のソースを作らない)。合成のソースが先に
	// 現れてから、composite_present が真になる。
	if (first && !install::is_uninstalling())
		create_children_locked();

	std::lock_guard<std::mutex> lock(g_mu);
	g_composites.push_back(data);
	g_composite_count.store(static_cast<int>(g_composites.size()));
}

void remove_composite(const CompositePtr &data)
{
	std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
	bool last = false;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		for (auto it = g_composites.begin(); it != g_composites.end(); ++it) {
			if (*it == data) {
				g_composites.erase(it);
				break;
			}
		}
		g_composite_count.store(static_cast<int>(g_composites.size()));
		last = g_composites.empty();
	}
	if (last)
		release_children_locked();
}

// ---- 合成のソース (cordmux_composite) ----

// プロパティの [cordmuxの設定を開く] で呼ぶ関数 (set_composite_open_settings)。OBS の UI のスレッドが読み、登録と解除は
// obs_module_load と obs_module_unload が行う。
std::atomic<void (*)()> g_open_settings{nullptr};

const char *composite_get_name(void * /*type_data*/)
{
	return obs_module_text(ledger::text::keys::kSourceCompositeLabel);
}

bool composite_open_settings_clicked(obs_properties_t * /*props*/, obs_property_t * /*property*/, void * /*data*/)
{
	if (void (*open)() = g_open_settings.load(std::memory_order_acquire))
		open();
	return false; // プロパティを作り直さない
}

// プロパティは [cordmuxの設定を開く] のボタンと、その上の説明の 1 行だけ。合成のソースには自分の設定がなく、cordmux の設定は
// 設定ウィンドウにあり、枠と名前ラベルはドックの [表示] タブにある。
// OBS は [+] で追加した後にプロパティのウィンドウを必ず開く (frontend の OBSBasicSourceSelect::createNew) ので、
// 何もないウィンドウにしないで、設定ウィンドウへの入口を置く。
obs_properties_t *composite_get_properties(void * /*vptr*/)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "display_note", obs_module_text(ledger::text::keys::kSourceCompositeDisplayNoteText),
				OBS_TEXT_INFO);
	obs_properties_add_button(props, "open_settings", obs_module_text(ledger::text::keys::kSourceOpenSettingsButton),
				  composite_open_settings_clicked);
	return props;
}

void *composite_create(obs_data_t * /*settings*/, obs_source_t *source)
{
	auto data = std::make_shared<composite_data>();
	data->source = source;
	add_composite(data);
	return new CompositePtr(data);
}

void composite_destroy(void *vptr)
{
	auto *holder = static_cast<CompositePtr *>(vptr);
	const CompositePtr data = *holder;
	delete holder;


	obs_source_t *target = nullptr;
	{
		// 自分の枠の映像へのアクティブの伝達を外す。選び直しと重ならないよう、g_self_frame_mu の下で行う。
		std::lock_guard<std::mutex> self_frame(g_self_frame_mu);
		std::lock_guard<std::mutex> life(data->life_mu);
		if (TargetRef::parent_is_active(data->source)) {
			target = g_self_frame.strong_ref();
			if (target)
				obs_source_remove_active_child(data->source, target);
		}
		data->source = nullptr;
	}
	// 参照を手放すと、そのソースの破棄が合成のソースの破棄を呼ぶことがあるので、ロックを放してから解放する。
	if (target)
		obs_source_release(target);
	// 描き直しのスレッドは、コンテキストに入る前に止めて待つ (描きかけの仕事は描き終えるまで待ち、画像は捨てる)。
	data->decor.stop_painter();
	// テクスチャはグラフィックスのコンテキストの中でしか破棄できない。destroy は描画のスレッドから呼ばれるとは
	// 限らないので、自分でコンテキストに入る。
	obs_enter_graphics();
	data->decor.release();
	obs_leave_graphics();
	remove_composite(data);
}

// キャンバスの大きさ (OBS の映像の基本の解像度)。読めなければ false。
bool read_canvas(int *width, int *height)
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0)
		return false;
	*width = static_cast<int>(ovi.base_width);
	*height = static_cast<int>(ovi.base_height);
	return true;
}

uint32_t composite_get_width(void * /*vptr*/)
{
	int width = 0;
	int height = 0;
	return read_canvas(&width, &height) ? static_cast<uint32_t>(width) : 0;
}

uint32_t composite_get_height(void * /*vptr*/)
{
	int width = 0;
	int height = 0;
	return read_canvas(&width, &height) ? static_cast<uint32_t>(height) : 0;
}

// module の表から、子の参照を取る (OBS の参照を 1 つずつ。呼び出し元が obs_source_release で解放する)。
std::array<obs_source_t *, kScreenTileCount> tile_refs()
{
	std::array<obs_source_t *, kScreenTileCount> refs{};
	std::lock_guard<std::mutex> lock(g_mu);
	for (std::size_t i = 0; i < refs.size(); ++i) {
		if (g_tiles[i] && g_tiles[i]->source)
			refs[i] = obs_source_get_ref(g_tiles[i]->source);
	}
	return refs;
}

// 配置が変わったときに 1 回だけログを書く。同じ配置の間は書かない (描画のたびに呼ばれる)。置けなかった画面共有が
// あれば、その数が変わったときに別の行で書く (黙って消さない)。人の ID と名前は書かない。
void log_layout_if_changed(int canvas_w, int canvas_h, const CompositeLayout &layout)
{
	const std::string line = composite_layout_log_line(canvas_w, canvas_h, layout.tiles);
	bool line_changed = false;
	bool dropped_changed = false;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		line_changed = line != g_last_layout_line;
		if (line_changed)
			g_last_layout_line = line;
		dropped_changed = layout.dropped.size() != g_last_dropped;
		if (dropped_changed)
			g_last_dropped = layout.dropped.size();
	}
	if (line_changed)
		diag::logf(LOG_INFO, "%s", line.c_str());
	if (dropped_changed && !layout.dropped.empty())
		diag::logf(LOG_INFO, "Composite layout has no room for %zu screen share(s)", layout.dropped.size());
}

// 描画の再入の守り。描画の途中の印を、抜けるときに必ず下ろす。
struct RenderGuard {
	explicit RenderGuard(std::atomic<bool> &flag) : flag_(flag), entered_(!flag.exchange(true)) {}
	~RenderGuard()
	{
		if (entered_)
			flag_.store(false);
	}
	RenderGuard(const RenderGuard &) = delete;
	RenderGuard &operator=(const RenderGuard &) = delete;
	bool entered() const { return entered_; }

private:
	std::atomic<bool> &flag_;
	bool entered_;
};

// ソースの映像の大きさ (libobs の obs_source_get_width と obs_source_get_height)。
ContentSize content_size(obs_source_t *source)
{
	ContentSize size;
	size.w = static_cast<int>(obs_source_get_width(source));
	size.h = static_cast<int>(obs_source_get_height(source));
	return size;
}

// 今のフレームに描くものを作る (CompositeFrame)。取るロックは module のロック (入力と子の表をコピーする間だけ) と、自分の枠の
// TargetRef のロックで、どちらも持ったまま次のロックを取らない。子と自分の枠の映像の参照は、大きさを読んだらすぐ解放する
// (覚えるのは値だけ)。
CompositeFrame make_frame()
{
	CompositeFrame frame;
	CompositeFrameFacts facts;
	if (!read_canvas(&facts.canvas_w, &facts.canvas_h))
		return frame;

	{
		std::lock_guard<std::mutex> lock(g_mu);
		frame.input = g_input;
	}
	const std::array<obs_source_t *, kScreenTileCount> tiles = tile_refs();
	for (std::size_t i = 0; i < tiles.size(); ++i) {
		if (!tiles[i])
			continue;
		facts.tile_sizes[i] = content_size(tiles[i]);
		obs_source_release(tiles[i]);
	}
	if (obs_source_t *self = g_self_frame.strong_ref()) {
		facts.self_size = content_size(self);
		obs_source_release(self);
	}

	frame.canvas_w = facts.canvas_w;
	frame.canvas_h = facts.canvas_h;
	frame.plan = plan_composite_frame(frame.input, facts);
	return frame;
}

// 今のフレームに描くもの。そのフレームで最初に呼んだときに作り、同じフレームの間は同じ値を返す。OBS の動画の
// フレームの時刻 (obs_get_video_frame_time) は、描画のスレッドの 1 周 (tick、出力の描画、プレビューの描画) の間は
// 変わらず、周の終わりの video_sleep で進む (obs-video.c の obs_graphics_thread_loop)。描画のスレッドで合成のソースの tick の
// 後の描画は g_frame を共にし、それ以外 (tick のコールバックの中のスクリーンショット、obs-websocket のスレッドの
// スクリーンショット) はスレッドごとの入れ物を使う (composite_frame_scope)。
std::shared_ptr<const CompositeFrame> current_frame()
{
	thread_local CompositeFrameMemo<CompositeFrame> t_local;
	const std::uint64_t now = obs_get_video_frame_time();
	const CompositeFrameScope scope =
		composite_frame_scope(obs_in_task_thread(OBS_TASK_GRAPHICS), now, g_composite_tick_time.load());
	CompositeFrameMemo<CompositeFrame> &memo = scope == CompositeFrameScope::Shared ? g_frame : t_local;
	return memo.get(now, make_frame);
}

// 番組の描画の前と後 (libobs の render_main_texture が、ミックスごとに obs_view_render の前と後に、描画のスレッドで呼ぶ)。
// 番組の描画の印 (t_program_pass) は、その周で最初のミックス (メインのキャンバスのミックス。libobs は常に先頭に置く) の描画の
// ときだけ立てる (program_pass_begin)。周の区切りは tick のコールバック (composite_tick の program_pass_tick)。メインの
// キャンバスのミックスは、ほかのミックスのテクスチャを使い回さない (使い回すのは、同じビューの先のミックスがあるときだけ)。
void composite_main_render(void * /*param*/, uint32_t /*cx*/, uint32_t /*cy*/)
{
	program_pass_begin(t_program_pass);
}

void composite_main_rendered(void * /*param*/)
{
	bool solo = false;
	if (program_pass_end(t_program_pass, &solo))
		g_last_program_solo.store(solo);
}

void composite_video_render(void *vptr, gs_effect_t * /*effect*/)
{
	composite_data &data = **static_cast<CompositePtr *>(vptr);

	// 描画の中でまたこの描画が呼ばれたら (自分の枠に映すシーンが、この合成のソースを含むとき)、何もしない。
	RenderGuard guard(data.rendering);
	if (!guard.entered())
		return;

	// 配置と、どのタイルに映像を描くかは、このフレームの値 (current_frame) のとおりにする。同じフレームの描画 (出力、プレビュー、
	// プロジェクター) が同じ配置と大きさで描くためである。
	const std::shared_ptr<const CompositeFrame> frame = current_frame();
	const int canvas_w = frame->canvas_w;
	const int canvas_h = frame->canvas_h;
	if (canvas_w <= 0 || canvas_h <= 0)
		return;
	const CompositeInput &input = frame->input;
	const CompositeFramePlan &plan = frame->plan;
	const CompositeLayout &layout = plan.layout;
	log_layout_if_changed(canvas_w, canvas_h, layout);

	// 子の参照は描く間だけ持つ (module のロックの中は参照を取るだけ)。
	const std::array<obs_source_t *, kScreenTileCount> tiles = tile_refs();

	// 混ぜ方を、OBS のシーンが項目を描くときと同じ状態にしてから描く (libobs の obs-scene.c の scene_video_render)。
	// 呼び出し元の状態は決まっていない。HDR のキャンバスでは、libobs がこのソースを色空間の変換用のテクスチャに描かせ、
	// その間は混ぜるのを止めている (obs-source.c の source_render)。そのまま描くと、名前ラベルの背景の半透明の板などが、
	// 下の映像と混ざらずに置き換わる。
	gs_blend_state_push();
	gs_reset_blend_state();

	// 枠の背景は、ほかの何よりも先に、キャンバス全体に 1 枚の矩形で敷く。映像は不透明なので、映像のない所にだけ見える。
	data.decor.draw_background(canvas_w, canvas_h);

	std::vector<bool> has_video(layout.tiles.size(), false);
	// 描いた映像がダミーのフレームか (子のテクスチャに入っているフレームの元。自分の枠は偽)。
	std::vector<bool> dummy(layout.tiles.size(), false);
	for (std::size_t index = 0; index < layout.tiles.size(); ++index) {
		// 子の大きさは、最初のフレームが届くまでと、空けた後は 0 になる。そのときは何も描かない (composite_tile_draw の draw が偽)。
		const CompositeTileDraw draw = composite_tile_draw(plan, index);
		if (!draw.draw)
			continue;
		const CompositeTile &tile = layout.tiles[index];
		obs_source_t *content = nullptr;
		bool own_ref = false;
		if (tile.self) {
			content = g_self_frame.strong_ref();
			own_ref = true;
		} else if (tile.slot >= 1 && tile.slot <= kScreenTileCount) {
			content = tiles[static_cast<std::size_t>(tile.slot - 1)];
		}
		if (!content)
			continue;

		gs_matrix_push();
		gs_matrix_translate3f(static_cast<float>(draw.fit.x), static_cast<float>(draw.fit.y), 0.0f);
		gs_matrix_scale3f(draw.scale_x, draw.scale_y, 1.0f);
		obs_source_video_render(content);
		gs_matrix_pop();
		has_video[index] = true;
		if (own_ref)
			obs_source_release(content);
		// 子を描いた。この tick で選ばれたフレームがあれば、今の描画でテクスチャに入ったので、テクスチャの元として確定する。
		if (!tile.self) {
			if (const std::shared_ptr<TileState> state = tile_state(tile.slot)) {
				if (state->origins) {
					std::lock_guard<std::mutex> lock(state->origins->mu);
					state->origins->texture.on_drawn();
					dummy[index] = state->origins->texture.texture_dummy();
				}
			}
		}
	}

	// 番組の描画の中なら、1 人の映像だけを全面に描いたかを残す (cordmux_solo_filter が読む)。実際に描いたもの (has_video と
	// dummy) で決める。
	program_pass_note_composite(t_program_pass,
				    composite_shows_one_full(layout.tiles, has_video, dummy, canvas_w, canvas_h));

	// 名前ラベルと光は、全部のタイルの映像を描いた後に重ねる。光は隣のタイルへ広がるので、タイルごとに重ねると、後に
	// 描く隣の映像の下に隠れる。映像がまだないタイルと空けた後のタイルには、何も重ねない。画面共有していない参加者の
	// 並びは、その上に重ねる。OBS のミキサーの、合成のソースの行のミュートと音量も渡す。ミュートか音量 0 のときは声が
	// 配信に出ていないので、話している人の光を出さず、話していない人も暗くしない (読むだけで、書き換えない)。cordmux の
	// ロックは持たずに読む。タイルに描く人も渡す (名前ラベルを隠す全面の 1 枚の人を、並びに描くため)。
	data.decor.draw(canvas_w, canvas_h, layout.tiles, has_video, composite_tile_hears_voice(input, layout.tiles),
			composite_tile_user_ids(input, layout.tiles), input.roster, obs_source_muted(data.source),
			obs_source_get_volume(data.source));
	gs_blend_state_pop();

	for (obs_source_t *tile : tiles) {
		if (tile)
			obs_source_release(tile);
	}
}

// 合成のソースの tick。このフレームの時刻を覚える (g_composite_tick_time)。libobs はすべてのソースの tick を、tick の
// コールバックの後、描画の前に呼ぶ (obs-video.c の tick_sources)。
void composite_video_tick(void * /*vptr*/, float /*seconds*/)
{
	g_composite_tick_time.store(obs_get_video_frame_time());
}

// 子と自分の枠の映像を返す。アクティブにするものも、全部のものも同じ (子は画面の枠の 4 つと全画面だけで映す人の 1 つ、
// 自分の枠は 1 つ)。
void composite_enum_children(void *vptr, obs_source_enum_proc_t enum_callback, void *param)
{
	composite_data &data = **static_cast<CompositePtr *>(vptr);
	const std::array<obs_source_t *, kScreenTileCount> tiles = tile_refs();
	for (obs_source_t *tile : tiles) {
		if (!tile)
			continue;
		enum_callback(data.source, tile, param);
		obs_source_release(tile);
	}
	g_self_frame.enumerate(data.source, enum_callback, param);
}

// ---- 1 人の映像だけを全面に描いている間だけ、付けたソースを見せるフィルタ (cordmux_solo_filter) ----

struct solo_filter_data {
	obs_source_t *filter = nullptr;
};

const char *solo_filter_get_name(void * /*type_data*/)
{
	return obs_module_text(ledger::text::keys::kSourceSoloFilterLabel);
}

// create が nullptr を返すと、libobs はフィルタを素通しにして、付けたソースを常に描く (obs-source.c の render_video)。
// 値の中身は描画で使うフィルタのソースだけ。
void *solo_filter_create(obs_data_t * /*settings*/, obs_source_t *filter)
{
	auto *data = new solo_filter_data();
	data->filter = filter;
	return data;
}

void solo_filter_destroy(void *vptr)
{
	delete static_cast<solo_filter_data *>(vptr);
}

// 合成のソースが 1 人の映像だけを全面に描いたときだけ、付けたソースをそのまま描く (その手前のフィルタとソースを、加工せずに
// 描く)。それ以外のときは何も描かない (composite_frame_plan.h の solo_filter_on)。番組の描画の中では、このフレームの番組の
// 描画で合成のソースが自分より先に描いたもので決め、番組の描画の外では直前の番組の描画の値を使う。合成のソースがなければ
// 描かない。
void solo_filter_video_render(void *vptr, gs_effect_t * /*effect*/)
{
	const auto *data = static_cast<const solo_filter_data *>(vptr);
	if (!composite_present())
		return;
	if (!solo_filter_on(t_program_pass, g_last_program_solo.load()))
		return;
	obs_source_skip_video_filter(data->filter);
}

} // namespace

void set_composite_open_settings(void (*open)())
{
	g_open_settings.store(open, std::memory_order_release);
}

obs_source_info make_composite_source_info()
{
	obs_source_info info = {};
	info.id = kCompositeSourceTypeId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	// OBS_SOURCE_COMPOSITE は付けない。OBS は COMPOSITE と AUDIO の組の登録を断る (libobs/obs-module.c の
	// obs_register_source_s)。
	info.output_flags =
		OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	// OBS の一覧のアイコン (ウィンドウキャプチャと同じアイコン)。
	info.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE;
	info.get_name = composite_get_name;
	info.get_properties = composite_get_properties;
	info.create = composite_create;
	info.destroy = composite_destroy;
	info.video_render = composite_video_render;
	info.video_tick = composite_video_tick;
	info.enum_active_sources = composite_enum_children;
	info.enum_all_sources = composite_enum_children;
	info.get_width = composite_get_width;
	info.get_height = composite_get_height;
	return info;
}

obs_source_info make_tile_source_info()
{
	obs_source_info info = {};
	info.id = kTileSourceTypeId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CAP_DISABLED;
	info.get_name = tile_get_name;
	info.create = tile_create;
	info.destroy = tile_destroy;
	return info;
}

// 測定のフィルタは、子に付けたときに OBS の filter_compatible (libobs/obs-source.c) を通らなければならず、通らないと
// obs_source_filter_add は何も言わずに付けない。見るのは output_flags のうち OBS_SOURCE_ASYNC_VIDEO と OBS_SOURCE_AUDIO
// だけで、フィルタの映像の形 (OBS_SOURCE_VIDEO) が子の OBS_SOURCE_ASYNC_VIDEO に含まれれば通る。OBS_SOURCE_CAP_DISABLED は
// 見ない。付いたかは、統計の shown_n が 0 でないことで確認する。
obs_source_info make_tile_probe_filter_info()
{
	obs_source_info info = {};
	info.id = kTileProbeFilterTypeId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED;
	info.get_name = probe_get_name;
	info.create = probe_create;
	info.destroy = probe_destroy;
	info.video_render = probe_video_render;
	info.filter_video = probe_filter_video;
	return info;
}

// 利用者が自分のソースに付けるフィルタで、OBS の [フィルタ] の [エフェクトフィルタ] の一覧に出す (OBS_SOURCE_CAP_DISABLED を
// 付けない)。映像のフィルタ (OBS_SOURCE_VIDEO) なので、同期のソース (テキスト、画像) にも非同期のソース (メディア) にも
// 付けられる (obs-source.c の filter_compatible)。音声は扱わない (付けたソースの音はそのまま出る)。大きさは返さず、付けた
// ソースの大きさのままにする (get_width のないフィルタの大きさは、libobs が付けた先の大きさにする。見えない間も、シーンの
// 中の位置と大きさの枠は変わらない)。プロパティは持たない。どの合成のソースの判定かを選ぶ必要がない (合成のソースは
// 全部同じ配置を描く) ためである。OBS_SOURCE_SRGB を付けるのは、付けないと libobs がこのフィルタの描画の間 (付けたソースを
// 描く間も) 線形の sRGB を切り (obs-source.c の obs_source_main_render)、半透明の縁などの混ざり方が、フィルタを付けない
// ときと変わるためである。このフィルタは色を扱わず、付けたソースをそのまま描くだけなので、状態を変えない。OBS の
// フィルタのうち描き方を変えないもの (クロップ/パッド、スクロール) も同じ印を付けている。
obs_source_info make_solo_filter_info()
{
	obs_source_info info = {};
	info.id = kSoloFilterTypeId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = solo_filter_get_name;
	info.create = solo_filter_create;
	info.destroy = solo_filter_destroy;
	info.video_render = solo_filter_video_render;
	return info;
}

bool composite_present()
{
	return g_composite_count.load(std::memory_order_relaxed) > 0;
}

void set_composite_input(const CompositeInput &input)
{
	std::lock_guard<std::mutex> lock(g_mu);
	g_input = input;
}

obs_source_t *composite_self_frame_ref()
{
	return g_self_frame.strong_ref();
}

bool composite_bind_self_frame(const char *uuid)
{
	TargetRef::Change change;
	// 活性を伝えられなかったこと (子にすると循環になる) で外した、選んだソースの強い参照。ロックを放してから解放する。
	obs_source_t *rejected = nullptr;
	bool bound = true;
	{
		std::lock_guard<std::mutex> self_frame(g_self_frame_mu);
		change = g_self_frame.rebind(uuid);
		if (change.old_target != change.new_target) {
			std::vector<CompositePtr> list;
			{
				std::lock_guard<std::mutex> lock(g_mu);
				list = g_composites;
			}
			// 選び直したことは、アクティブか表示中の合成のソースにだけ伝える。libobs は、呼んだ時点の親の状態しか子に
			// 伝えない。
			std::vector<CompositePtr> added;
			for (const CompositePtr &data : list) {
				std::lock_guard<std::mutex> life(data->life_mu);
				if (!data->source || !TargetRef::parent_is_active(data->source))
					continue;
				if (change.old_target)
					obs_source_remove_active_child(data->source, change.old_target);
				if (!change.new_target)
					continue;
				// 偽が返るのは、新しい子が親を含んでいて、子にすると循環になるとき。
				if (obs_source_add_active_child(data->source, change.new_target))
					added.push_back(data);
				else
					bound = false;
			}
			// 1 つでも循環になったら、結び付けない。伝えた分は取り消して、選んだソースを外す。
			if (!bound) {
				for (const CompositePtr &data : added) {
					std::lock_guard<std::mutex> life(data->life_mu);
					if (data->source)
						obs_source_remove_active_child(data->source, change.new_target);
				}
				rejected = g_self_frame.rebind("").old_target;
			}
		}
	}
	// 参照を手放すと、そのソースの破棄が合成のソースの破棄を呼ぶことがあるので、ロックを放してから解放する。
	if (change.old_target)
		obs_source_release(change.old_target);
	if (change.new_target)
		obs_source_release(change.new_target);
	if (rejected)
		obs_source_release(rejected);
	return bound;
}

bool self_frame_shown()
{
	if (!composite_present())
		return false;
	obs_source_t *target = g_self_frame.strong_ref();
	if (!target)
		return false;
	obs_source_release(target);
	return true;
}

void output_tile_video(int slot, const obs_source_frame2 *frame, bool unbuffered, bool dummy)
{
	const std::shared_ptr<TileState> tile = tile_state(slot);
	if (!tile)
		return;
	// OBS がこのフレームを表示に選ぶ前に記録する (測定のフィルタが表示に選ばれたフレームを引く)。
	if (frame && tile->origins) {
		std::lock_guard<std::mutex> lock(tile->origins->mu);
		tile->origins->log.note_output(frame->timestamp, dummy);
	}
	tile->sink.output(frame, unbuffered);
}

void clear_tile_now(int slot)
{
	if (const std::shared_ptr<TileState> tile = tile_state(slot))
		tile->sink.clear_now();
}

void hold_then_clear_tile(int slot, std::uint64_t hold_ns)
{
	if (const std::shared_ptr<TileState> tile = tile_state(slot))
		tile->sink.hold_then_clear(hold_ns);
}

void output_composite_audio(const obs_source_audio *audios, std::size_t count)
{
	if (audios == nullptr || count == 0)
		return;
	std::vector<CompositePtr> list;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		list = g_composites;
	}
	// 先に作られたもののうち、参照が取れる最初の 1 つ。life_mu を持ったまま呼ぶのは obs_source_get_ref だけ。
	obs_source_t *source = nullptr;
	for (const CompositePtr &data : list) {
		std::lock_guard<std::mutex> life(data->life_mu);
		if (data->source)
			source = obs_source_get_ref(data->source);
		if (source)
			break;
	}
	if (!source)
		return;
	for (std::size_t i = 0; i < count; ++i)
		obs_source_output_audio(source, &audios[i]);
	obs_source_release(source);
}

void release_composite_children()
{
	{
		std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
		release_children_locked();
	}

	// 残っている合成のソースの画像を破棄し、明るさを掛ける効果も破棄する。OBS はモジュールを閉じる前に
	// ソースを破棄するので、通常は何も残っていない。
	std::vector<CompositePtr> list;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		list = g_composites;
	}
	for (const CompositePtr &data : list)
		data->decor.stop_painter();
	obs_enter_graphics();
	for (const CompositePtr &data : list)
		data->decor.release();
	obs_leave_graphics();
	release_composite_decor_shared();
}

CompositeDecorPaintStats take_composite_decor_paint_stats()
{
	// 一覧をコピーしてから、モジュールのロックを放して読む (読む先は、ほかのロックを取らない葉のロックと内部のロックだけ)。
	std::vector<CompositePtr> list;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		list = g_composites;
	}
	CompositeDecorPaintStats total;
	for (const CompositePtr &data : list) {
		const CompositeDecorPaintStats one = data->decor.take_paint_stats();
		total.paint.submitted += one.paint.submitted;
		total.paint.finished += one.paint.finished;
		total.paint.dropped += one.paint.dropped;
		total.paint.max_paint_ns = std::max(total.paint.max_paint_ns, one.paint.max_paint_ns);
		total.uploads += one.uploads;
		total.max_upload_ns = std::max(total.max_upload_ns, one.max_upload_ns);
		total.max_swap_ns = std::max(total.max_swap_ns, one.max_swap_ns);
	}
	return total;
}

} // namespace cordmux::sources

// SPDX-License-Identifier: Apache-2.0

// composite_decor.h の実装。

#include "sources/composite_decor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QImage>

#include <util/platform.h>

#include "diag/diag_log.h"
#include "sources/composite_background.h"
#include "sources/composite_draw_plan.h"
#include "sources/composite_label.h"
#include "sources/composite_label_plan.h"
#include "sources/composite_paint_handoff.h"
#include "sources/composite_texture_strips.h"
#include "sources/decor_registry.h"
#include "sources/label_border.h"

namespace cordmux::sources {

namespace {

// ---- 明るさと暗さを掛けて置く効果 ----
//
// 光の画像は最大の明るさで描いてあり、描くたびに明るさ level (0 から 1) を掛けて置く。OBS の標準の効果には、
// 乗算済みアルファの画像の全チャンネル (色とアルファ) に同じ倍率を掛けて置くものがない。default.effect の
// DrawMultiply は色だけに掛けるので、アルファが残り、混ぜ方 (GS_BLEND_ONE と GS_BLEND_INVSRCALPHA) では背景が
// アルファの分だけ暗くなる。明るさを変えるたびにテクスチャを作り直すと、描き直しと同じ重さになる。そこで、画像の
// 色とアルファに level を掛け、さらに色だけに暗さの倍率 dim を掛ける効果を、文字列から 1 度だけ作る。名前ラベルは
// level 1 と dim で置き、光は level と dim 1 で置く (CPU で重ねる composite_draw_cpu.cpp の draw_image と同じ式。あちらも
// level と dim を、この効果と同じ 32 ビットの小数で掛ける)。
// 効果のファイルを置くと、同梱するファイルの一覧とアンインストールで消すものの一覧に足すことになるので、文字列にした。
// 作れなかったとき (fade_effect が nullptr) は、暗さを掛けず、明るさが半分以上のとき最大の明るさで置き、未満なら置かない。
constexpr const char *kFadeEffect = R"(uniform float4x4 ViewProj;
uniform texture2d image;
uniform float level;
uniform float dim;

sampler_state def_sampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertInOut {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertInOut VSDefault(VertInOut vert_in)
{
	VertInOut vert_out;
	vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = vert_in.uv;
	return vert_out;
}

float4 PSFade(VertInOut vert_in) : TARGET
{
	float4 rgba = image.Sample(def_sampler, vert_in.uv);
	rgba.rgb *= level * dim;
	rgba.a *= level;
	return rgba;
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(vert_in);
		pixel_shader  = PSFade(vert_in);
	}
}
)";

// グラフィックスのコンテキストの中でだけ触る。作るのは最初に使うときで、破棄は release_composite_decor_shared。
gs_effect_t *g_fade_effect = nullptr;
bool g_fade_effect_tried = false;

gs_effect_t *fade_effect()
{
	if (g_fade_effect_tried)
		return g_fade_effect;
	g_fade_effect_tried = true;
	char *error = nullptr;
	g_fade_effect = gs_effect_create(kFadeEffect, "cordmux_fade.effect", &error);
	if (!g_fade_effect) {
		diag::logf(LOG_WARNING, "Couldn't create the speaking glow effect: %s", error ? error : "unknown error");
	}
	bfree(error);
	return g_fade_effect;
}

// ---- テクスチャ ----

// 画像 1 つのテクスチャ (帯ごとに 1 つ) の所有者。壊れるときに、グラフィックスのコンテキストに 1 回入って全部を
// 破棄する。描画のスレッドではすでにコンテキストの中なので、入り直しは参照の数を増やすだけである (libobs の
// gs_enter_context)。ほかのスレッドではコンテキストのミューテックスを待つので、cordmux のロックを持ったまま壊さない
// (composite_paint_handoff.h は、受け渡しのロックを放してから画像を壊す)。
class Textures {
public:
	Textures() = default;
	~Textures() { reset(); }
	Textures(Textures &&other) noexcept : textures_(std::move(other.textures_)) { other.textures_.clear(); }
	Textures &operator=(Textures &&other) noexcept
	{
		if (this != &other) {
			reset();
			textures_ = std::move(other.textures_);
			other.textures_.clear();
		}
		return *this;
	}
	Textures(const Textures &) = delete;
	Textures &operator=(const Textures &) = delete;

	// テクスチャを作る前に、足す数の場所を取っておく。add が例外を投げず、作ったテクスチャが所有者なしに残らない。
	void reserve(std::size_t count) { textures_.reserve(count); }
	void add(gs_texture_t *texture) { textures_.push_back(texture); }

	std::size_t size() const { return textures_.size(); }
	bool empty() const { return textures_.empty(); }
	gs_texture_t *at(std::size_t index) const { return textures_[index]; }

	void reset()
	{
		if (textures_.empty())
			return;
		obs_enter_graphics();
		for (gs_texture_t *texture : textures_)
			gs_texture_destroy(texture);
		obs_leave_graphics();
		textures_.clear();
	}

private:
	std::vector<gs_texture_t *> textures_;
};

// テクスチャにした画像 1 つ。textures は strips と同じ並びで、帯ごとに 1 つ持つ。テクスチャのない画像は textures が空。
struct GpuImage {
	Textures textures;
	std::vector<CompositeTextureStrip> strips;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	// セルの左上から見た画像の位置。
	int x = 0;
	int y = 0;

	bool present() const { return !textures.empty(); }
};

// 1 つの仕事で、テクスチャを作るためにグラフィックスのコンテキストに入っていた時間。total_ns は合計、holds は入った回数
// (帯の数)、max_ns は 1 回 (帯 1 つ) の最大である。描き直しのスレッドが入るのは 1 フレームに 1 回までなので
// (CompositeUploadTurn)、描画のスレッドを 1 フレームに待たせうる時間の上限は max_ns になる。
struct UploadTimes {
	std::int64_t total_ns = 0;
	std::int64_t max_ns = 0;
	int holds = 0;
};

// QImage を、帯 (plan_composite_texture_strips) ごとに 1 つのテクスチャにする。描き直しのスレッドから呼ぶ。帯 1 つを
// 作る間だけグラフィックスのコンテキストに入り、入る前に turn で、描画のスレッドが次のフレームに進むのを待つ。times に、
// コンテキストに入っていた時間を足す。全部の帯ができてから返すので、途中の帯だけで描くことはない。画像が空のとき、どれかの
// 帯のテクスチャを作れないとき、止めるとき (turn.wait が false) は、作った帯を捨て、テクスチャのない画像を返す。
//
// 描画のスレッドで作らないのは、OBS のグラフィックスのコンテキストが 1 つのミューテックスで、描画のスレッドはフレームを
// 描く間だけそれを持ち、次のフレームまで待つ間は放しているためである (OBS 31.1.1 の libobs/obs-video.c の
// obs_graphics_thread_loop。gs_begin_frame、output_frame、render_displays の間だけ gs_enter_context し、
// tick_sources と video_sleep の間は持たない)。描画のスレッドの中で作ると、作る時間がそのままフレームを描く時間に
// 足される。ここで作ると、描画のスレッドが待っている間に作れる。ただし、作る間はコンテキストを持つので、描画のスレッドが
// 次にコンテキストに入るまでに終わらなければ、その分だけ描画のスレッドを待たせる。持つ時間を短くするため、大きな画像は
// 帯に分けて帯 1 つごとに入り直し (上限は kCompositeStripMaxBytes)、形式の変換はコンテキストの外で行う。
//
// テクスチャは中身を書き換えないので、GS_DYNAMIC を付けずに作り、作るときに中身を渡す。libobs の D3D11 は、渡した中身の
// コピーを持ち (libobs-d3d11 の gs_texture_2d::BackupTexture)、デバイスを作り直したとき (GPU のドライバーのリセットなど)
// にそのコピーから作り直す (gs_texture_2d::Rebuild)。中身を渡さずに作って後から書くと、このコピーの分だけ早いが、作り
// 修正したときに透明なテクスチャになり、次に描き直すまで名前ラベルが消えるので、そうしない。
GpuImage make_image(const QImage &source, int x, int y, CompositeUploadTurn &turn, UploadTimes *times)
{
	GpuImage image;
	if (source.isNull())
		return image;
	// QImage の ARGB32 は、リトルエンディアンの環境では B、G、R、A の順に並ぶ。libobs の GS_BGRA と同じ並びなので、
	// ピクセルを並べ替えずにそのまま渡せる。1 行の長さは幅 x 4 で、
	// gs_texture_create が読む行の長さと同じである。行は続けて並ぶので、帯の中身は、帯の最初の行の位置を渡せばコピーせずに済む。
	const QImage pixels = source.format() == QImage::Format_ARGB32_Premultiplied
				      ? source
				      : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const std::vector<CompositeTextureStrip> strips =
		plan_composite_texture_strips(pixels.width(), pixels.height(), kCompositeStripMaxBytes);
	const auto width = static_cast<std::uint32_t>(pixels.width());
	const std::uint8_t *bits = pixels.constBits();
	const auto stride = static_cast<std::ptrdiff_t>(pixels.bytesPerLine());
	Textures textures;
	textures.reserve(strips.size());
	for (const CompositeTextureStrip &strip : strips) {
		if (!turn.wait())
			return image;
		const std::uint8_t *data = bits + static_cast<std::ptrdiff_t>(strip.texture_top()) * stride;
		obs_enter_graphics();
		const std::uint64_t started = os_gettime_ns();
		gs_texture_t *texture = gs_texture_create(width, static_cast<std::uint32_t>(strip.texture_rows()),
							  GS_BGRA, 1, &data, 0);
		const auto held = static_cast<std::int64_t>(os_gettime_ns() - started);
		obs_leave_graphics();
		turn.left();
		times->total_ns += held;
		times->max_ns = std::max(times->max_ns, held);
		++times->holds;
		if (!texture)
			return image;
		textures.add(texture);
	}
	if (textures.empty())
		return image;
	image.textures = std::move(textures);
	image.strips = strips;
	image.width = width;
	image.height = static_cast<std::uint32_t>(pixels.height());
	image.x = x;
	image.y = y;
	return image;
}

// 計画の 1 つ (draw) の画像を、draw.image の位置に等倍で置き、draw.clip の中だけを描く。切り抜きはテクスチャの
// 一部を描くことで行う (gs_set_scissor_rect の矩形は描画先のピクセルで、シーンでの合成のソースの変換を受けない
// ので使わない)。帯に分けた画像は、切り抜きと各帯の置く行が重なる部分だけを、帯ごとに置く。effect は "Draw" の
// 技法を持つ効果で、テクスチャ以外の値は呼び出し元が設定する。
void draw_region(gs_effect_t *effect, const GpuImage &image, const CompositeDraw &draw)
{
	const int sub_x = draw.clip.x - draw.image.x;
	const int sub_y = draw.clip.y - draw.image.y;
	if (!image.present() || image.textures.size() != image.strips.size() || draw.clip.w <= 0 || draw.clip.h <= 0 ||
	    sub_x < 0 || sub_y < 0 || sub_x + draw.clip.w > static_cast<int>(image.width) ||
	    sub_y + draw.clip.h > static_cast<int>(image.height))
		return;
	gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
	const auto place = [&](std::size_t index, int texture_row, int image_row, int rows) {
		gs_texture_t *texture = image.textures.at(index);
		gs_effect_set_texture(param, texture);
		gs_matrix_push();
		gs_matrix_translate3f(static_cast<float>(draw.clip.x), static_cast<float>(draw.image.y + image_row), 0.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite_subregion(texture, 0, static_cast<std::uint32_t>(sub_x),
						 static_cast<std::uint32_t>(texture_row),
						 static_cast<std::uint32_t>(draw.clip.w), static_cast<std::uint32_t>(rows));
		gs_matrix_pop();
	};
	for_each_composite_strip_piece(image.strips, sub_y, draw.clip.h, place);
}

// 画像をそのまま置く。混ぜ方は呼び出し元が設定する。
void draw_plain(const GpuImage &image, const CompositeDraw &draw)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (effect)
		draw_region(effect, image, draw);
}

// 画像の 4 つのチャンネルに明るさ draw.level を、色の 3 つのチャンネルに暗さの倍率 draw.dim を掛けて置く。
void draw_faded(const GpuImage &image, const CompositeDraw &draw)
{
	if (!image.present() || !(draw.level > 0.0))
		return;
	gs_effect_t *effect = fade_effect();
	gs_eparam_t *level_param = effect ? gs_effect_get_param_by_name(effect, "level") : nullptr;
	gs_eparam_t *dim_param = effect ? gs_effect_get_param_by_name(effect, "dim") : nullptr;
	if (!effect || !level_param || !dim_param) {
		if (draw.level >= 0.5)
			draw_plain(image, draw);
		return;
	}
	gs_effect_set_float(level_param, static_cast<float>(std::min(draw.level, 1.0)));
	gs_effect_set_float(dim_param, static_cast<float>(std::clamp(draw.dim, 0.0, 1.0)));
	draw_region(effect, image, draw);
}

// 名前ラベルを置く。計画では明るさが 1 で、暗くしないとき (dim が 1) は効果を通さずにそのまま置く (同じ値になる)。
void draw_label(const GpuImage &image, const CompositeDraw &draw)
{
	if (draw.level >= 1.0 && draw.dim >= 1.0)
		draw_plain(image, draw);
	else
		draw_faded(image, draw);
}

BorderTiming timing_of(const DecorLabelParams &params)
{
	BorderTiming timing;
	timing.rise_ms = params.border_rise_ms;
	timing.hold_ms = params.border_hold_ms;
	timing.fade_ms = params.border_fade_ms;
	return timing;
}

CompositeDrawImage image_of(const GpuImage &image, int dx = 0, int dy = 0)
{
	CompositeDrawImage out;
	out.present = image.present();
	out.x = image.x + dx;
	out.y = image.y + dy;
	out.w = static_cast<int>(image.width);
	out.h = static_cast<int>(image.height);
	return out;
}

// 最大の値を入れる (どのスレッドから呼んでもよい)。
void store_max(std::atomic<std::int64_t> &slot, std::int64_t value)
{
	std::int64_t seen = slot.load(std::memory_order_relaxed);
	while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
	}
}

int whole_ms(std::int64_t ns)
{
	return static_cast<int>(ns / 1000000LL);
}

double ms_of(std::int64_t ns)
{
	return static_cast<double>(ns) / 1e6;
}

} // namespace

// 描き直しのスレッドへ渡す仕事の入力。描き直しのスレッドは、この値だけを見て描く (表には触らない)。画面共有していない
// 参加者の名前ラベルも、セルと余白を変えて同じ形で渡す。
struct LabelJob {
	LabelContent person;
	CompositeLabelLook look;
	double name_px = 0.0;
	double margin_px = 0.0;
	int cell_w = 0;
	int cell_h = 0;
};

struct FrameJob {
	int cell_w = 0;
	int cell_h = 0;
	std::uint32_t argb = 0;
	double blur_px = 0.0;
	double spread_px = 0.0;
};

// 名前ラベルの仕事の結果。box は名前ラベルの矩形 (セルの左上から見た位置。並びの位置を決めるのに使う)。paint_ns は
// QPainter で描いた時間、upload はテクスチャを作るためにグラフィックスのコンテキストに入っていた時間 (名前ラベルと光の
// 2 つの画像の分)。
struct LabelImages {
	GpuImage label;
	GpuImage glow;
	int box_x = 0;
	int box_y = 0;
	int box_w = 0;
	int box_h = 0;
	std::int64_t paint_ns = 0;
	UploadTimes upload;
};

struct FrameImage {
	GpuImage frame;
	std::int64_t paint_ns = 0;
	UploadTimes upload;
};

using PaintKey = std::variant<CompositeLabelKey, CompositeFrameGlowKey, CompositeRosterLabelKey>;
using PaintInput = std::variant<LabelJob, FrameJob>;
using PaintOutput = std::variant<LabelImages, FrameImage>;
using Handoff = PaintHandoff<PaintKey, PaintInput, PaintOutput>;

namespace {

// 描き直しのスレッドが数える、この周期の値 (CompositeDecorPaintStats の max_paint_ns と max_upload_ns)。
// max_upload_ns は、コンテキストに 1 回 (帯 1 つ) 入っていた時間の最大である。
struct PainterStats {
	std::atomic<std::int64_t> max_paint_ns{0};
	std::atomic<std::int64_t> max_upload_ns{0};
};

// 描き直しのスレッドで呼ぶ。QImage と QPainter で描き、テクスチャを作るときだけ OBS の関数を呼ぶ (make_image)。
PaintOutput paint_job(const PaintInput &input, PainterStats &stats, CompositeUploadTurn &turn)
{
	const std::uint64_t started = os_gettime_ns();
	if (const auto *job = std::get_if<LabelJob>(&input)) {
		const CompositeLabel label =
			paint_composite_label(job->person, job->look, job->name_px, job->margin_px, job->cell_w, job->cell_h);
		LabelImages out;
		out.paint_ns = static_cast<std::int64_t>(os_gettime_ns() - started);
		out.label = make_image(label.label, label.label_x, label.label_y, turn, &out.upload);
		out.glow = make_image(label.glow, label.glow_x, label.glow_y, turn, &out.upload);
		out.box_x = label.box_x;
		out.box_y = label.box_y;
		out.box_w = label.box_w;
		out.box_h = label.box_h;
		store_max(stats.max_paint_ns, out.paint_ns);
		store_max(stats.max_upload_ns, out.upload.max_ns);
		return out;
	}
	const FrameJob &job = std::get<FrameJob>(input);
	const QImage frame = paint_frame_glow(job.cell_w, job.cell_h, job.argb, job.blur_px, job.spread_px);
	FrameImage out;
	out.paint_ns = static_cast<std::int64_t>(os_gettime_ns() - started);
	out.frame = make_image(frame, 0, 0, turn, &out.upload);
	store_max(stats.max_paint_ns, out.paint_ns);
	store_max(stats.max_upload_ns, out.upload.max_ns);
	return out;
}

// 保存先の番号。タイル (並びの位置) ごとに、名前ラベルと枠の光の 2 つ。画面共有していない参加者の保存先は、その後に
// 参加者ごとに足す (Handoff::add_slot)。
std::size_t label_slot(std::size_t index)
{
	return index * 2;
}

std::size_t frame_slot(std::size_t index)
{
	return index * 2 + 1;
}

} // namespace

// タイル (並びの位置) 1 つ分のテクスチャと、その鍵。位置は並びの位置で持つ。人が替わっても同じ位置なら、鍵の owner が
// 替わるので描き直し、同じ人が別の位置に動いたときも、その位置の鍵と違えば描き直す。label_key と frame_key は、いま
// テクスチャにある画像の鍵である。
struct TileCache {
	bool label_shown = false;
	CompositeLabelKey label_key;
	GpuImage label; // 名前ラベル
	GpuImage glow;  // アバターと名前の光

	bool frame_shown = false;
	CompositeFrameGlowKey frame_key;
	GpuImage frame; // 枠の光
};

// 画面共有していない参加者 1 人分のテクスチャと、その鍵と、表から最後にコピーした人の情報。参加者ごと (user_id) に持ち、
// 並びから消えたら捨てる。
struct RosterCache {
	std::size_t slot = 0; // 描き直しの保存先
	bool shown = false;
	CompositeRosterLabelKey key; // いまテクスチャにある画像の鍵
	GpuImage label;
	GpuImage glow;
	int box_x = 0;
	int box_y = 0;
	int box_w = 0;
	int box_h = 0;

	bool present = false;      // 表に人の情報がある
	std::uint64_t version = 0; // コピーした人の情報の version
	std::string name;
	QImage avatar;
};

struct CompositeDecor::Impl {
	// 表から最後にコピーした値。version が 0 のうちは、まだコピーしていない。
	CompositeDecorView view;
	std::array<TileCache, kCompositeCellCount> tiles;
	std::map<std::string, RosterCache> roster;
	std::vector<std::size_t> free_roster_slots; // 並びから消えた人の、使い回す保存先
	int roster_not_placed = 0;                  // 最後にログに書いた、置けなかった人の数
	int roster_shown_logged = -1;               // 最後にログに書いた、並べた人の数 (-1 はまだ書いていない)

	// 枠の背景の 1 画素のテクスチャ (乗算済みアルファの BGRA)。色か不透明度が変わったときだけ作り直し、描かないときは
	// 持たない。background_pixel は、いま作ったテクスチャ (作れなかったときは空) の画素の値で、background_made が偽の間は
	// まだ作っていない。
	Textures background;
	std::uint32_t background_pixel = 0;
	bool background_made = false;

	std::uint64_t uploads = 0;    // この周期の、受け取って置き換えた仕事の数
	std::int64_t max_swap_ns = 0; // この周期の、1 つの仕事を受け取って置き換えた時間の最大
	std::uint64_t label_uploads_total = 0;
	std::uint64_t frame_uploads_total = 0;
	std::uint64_t roster_uploads_total = 0;

	// 描き直しのスレッドが書く値と、描き直しのスレッドがグラフィックスのコンテキストに入る間合い。handoff より先に作り、
	// 後に壊す (handoff が描き直しのスレッドを止めて待ってから壊れる)。描き直しのスレッドを止めると (handoff.stop)、
	// 帯の間の待ちはすぐ終わる。
	PainterStats painter_stats;
	CompositeUploadTurn upload_turn{CompositeUploadClock{[] { return obs_get_video_frame_time(); },
							     [] { return os_gettime_ns(); }, [] { os_sleep_ms(1); },
							     [this] { return handoff.stopped(); }}};

	// 保存先はタイルごとに名前ラベルと枠の光の 2 つと、画面共有していない参加者ごとに 1 つ。mu_ とは別のロックを持つ
	// (葉のロック)。
	Handoff handoff{kCompositeCellCount * 2,
			[this](const PaintInput &input) { return paint_job(input, painter_stats, upload_turn); }};

	// 1 つの仕事を受け取って置き換えた時間を数える。
	void note_swap(std::int64_t swap_ns)
	{
		++uploads;
		max_swap_ns = std::max(max_swap_ns, swap_ns);
	}

	// タイル 1 つの描き直しの仕事を渡し、できたテクスチャを受け取って、計画の入力 out を埋める。roster_names は
	// 画面共有していない参加者の名前 (ほかの人と見分けるための名前に足す)。labeled が偽なら、名前ラベルとその光は描かず
	// (画像も持たない)、枠の光と話している明るさだけを埋める (名前ラベルを枠に描かない設定。composite_tiles_labeled)。
	void update_tile(std::size_t index, const CompositeTile &tile, bool labeled, double name_px, double margin_px,
			 std::uint64_t now_ns, bool hears_voice, bool muted, float volume,
			 const std::vector<std::string> &roster_names, CompositeDrawTile &out);

	// 名前ラベルを出す人それぞれの並びのキャッシュを、描く順 (order) に用意し、表からコピーした人の情報を入れる。いま枠に名前を
	// 出している人のキャッシュも持ち、並びの名前ラベルを先に描いておく (全画面の始まりなどで並びに移ったとき、描き終えるのを
	// 待つ間に並びから一瞬消えないように)。名前ラベルを出す人でなくなった人と、並びを出さない形 (show が偽) のときの全員の
	// キャッシュは捨てる。返す並びは order と同じで、show が偽なら空。
	std::vector<RosterCache *> refresh_roster(const std::vector<CompositeRosterPerson> &people,
						  const std::vector<std::size_t> &order,
						  const CompositeRosterView &roster_view, bool show);

	// 並びの名前ラベルの描き直しの仕事を渡し、できたテクスチャを受け取って、置く位置を決め、計画の入力 (request.roster) を
	// 埋める。caches は refresh_roster が返した並びで、shown は同じ並びの、並びに描くか (composite_roster_shows)。描き直しは
	// キャッシュの全員について行い、置くのは shown の人だけにする。tile_names は名前ラベル付きで描いている枠の人の名前
	// (ほかの人と見分けるための名前に使う)。placed には request.roster と同じ並びで、その人のキャッシュを入れる。
	void update_roster(int canvas_w, int canvas_h, const std::vector<std::string> &tile_names, double name_px,
			   double margin_px, std::uint64_t now_ns, bool muted, float volume,
			   const std::vector<CompositeRosterPerson> &people, const std::vector<std::size_t> &order,
			   const std::vector<bool> &shown, const CompositeRosterView &roster_view,
			   const std::vector<RosterCache *> &caches, CompositeDrawRequest &request,
			   std::vector<const RosterCache *> &placed);

	// 並べた人の数が変わったときだけ、数をログに書く (名前と userId は書かない)。並びに誰が出ているかを、配信の絵を
	// 見ずに記録から確認するため。
	void log_roster_shown(int shown)
	{
		if (shown == roster_shown_logged)
			return;
		roster_shown_logged = shown;
		diag::logf(LOG_INFO, "Composite roster shows %d participant(s)", shown);
	}

	// 人のキャッシュを捨てる。テクスチャを破棄し、描き直しの保存先を空けて使い回しに回す。
	void forget_roster_person(std::map<std::string, RosterCache>::iterator it)
	{
		// 描き終えて受け取っていない画像は、ここで壊れる (描画のスレッドはコンテキストの中にいる)。
		handoff.clear_slot(it->second.slot);
		free_roster_slots.push_back(it->second.slot);
		roster.erase(it);
	}
};

CompositeDecor::CompositeDecor() : impl_(std::make_unique<Impl>()) {}

CompositeDecor::~CompositeDecor() = default;

void CompositeDecor::Impl::update_tile(std::size_t index, const CompositeTile &tile, bool labeled, double name_px,
				       double margin_px, std::uint64_t now_ns, bool hears_voice, bool muted, float volume,
				       const std::vector<std::string> &roster_names, CompositeDrawTile &out)
{
	const int owner_index = tile.self ? 0 : tile.slot;
	if (index >= tiles.size() || owner_index < 0 || owner_index >= static_cast<int>(view.owners.size()))
		return;
	const auto oi = static_cast<std::size_t>(owner_index);
	const CompositeLabelLook &look = view.look;
	const PixelRect &cell = tile.rect;
	TileCache &cache = tiles[index];
	const CompositeSpeakingParts parts = composite_speaking_parts(look);

	// 画像は描画のスレッドでは描かない。鍵が変わったら描き直しのスレッドに仕事を渡し、できたテクスチャを受け取るだけ
	// である。QImage の入力は暗黙の共有で、表から読んだ値は書き換えないので、そのまま渡せる。
	CompositeLabelKey label_key =
		make_composite_label_key(view.version, view.look_version, view.owners[oi], cell, name_px, margin_px);
	label_key.roster_names = roster_names;
	if (labeled && view.person_present[oi] && !view.owners[oi].empty()) {
		if (auto done = handoff.take(label_slot(index), PaintKey(label_key))) {
			const std::uint64_t started = os_gettime_ns();
			LabelImages &images = std::get<LabelImages>(done->output);
			cache.label = std::move(images.label);
			cache.glow = std::move(images.glow);
			cache.label_shown = true;
			cache.label_key = std::get<CompositeLabelKey>(done->key);
			note_swap(static_cast<std::int64_t>(os_gettime_ns() - started));
			++label_uploads_total;
			// upload_ms はテクスチャを作るためにコンテキストに入っていた時間の合計、strips は入った回数 (帯の数)、
			// upload_max_ms はその 1 回の最大 (統計の decor_upload_max_ms と同じ意味) である。
			diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				   "Composite label ready: tile=%zu cell=%dx%d paint_ms=%d upload_ms=%d total=%llu strips=%d "
				   "upload_max_ms=%.2f",
				   index, cell.w, cell.h, whole_ms(images.paint_ns), whole_ms(images.upload.total_ns),
				   static_cast<unsigned long long>(label_uploads_total), images.upload.holds,
				   ms_of(images.upload.max_ns));
		}
		if (!cache.label_shown || cache.label_key != label_key) {
			handoff.submit(label_slot(index), PaintKey(label_key), [&]() -> PaintInput {
				LabelJob job;
				job.person = view.person[oi];
				// 表のほかの人の名前は映像の枠の人 (画面共有の人と自分の枠の人) だけなので、画面共有していない
				// 参加者の名前を足す。
				job.person.other_visible_names.insert(job.person.other_visible_names.end(),
								      roster_names.begin(), roster_names.end());
				job.look = look;
				// 光を描かない形のときは、光の画像を作らない (作ると名前の大きさの 2 乗に近く時間がかかる)。
				if (!parts.label_glow) {
					job.look.glow_avatar = false;
					job.look.glow_name = false;
				}
				job.name_px = name_px;
				job.margin_px = margin_px;
				job.cell_w = cell.w;
				job.cell_h = cell.h;
				return job;
			});
		}
	} else {
		// 描く人がいない枠と、人の情報がまだ届いていない枠と、名前ラベルを枠に描かない設定の枠には、名前ラベルを出さない
		// (前の人の名前を残さない)。
		cache.label = GpuImage();
		cache.glow = GpuImage();
		cache.label_shown = false;
	}

	const CompositeFrameGlowKey frame_key = make_composite_frame_glow_key(cell, look, name_px);
	if (parts.frame_glow) {
		if (auto done = handoff.take(frame_slot(index), PaintKey(frame_key))) {
			const std::uint64_t started = os_gettime_ns();
			FrameImage &image = std::get<FrameImage>(done->output);
			cache.frame = std::move(image.frame);
			cache.frame_shown = true;
			cache.frame_key = std::get<CompositeFrameGlowKey>(done->key);
			note_swap(static_cast<std::int64_t>(os_gettime_ns() - started));
			++frame_uploads_total;
			// upload_ms、strips、upload_max_ms は名前ラベルの行と同じ。
			diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				   "Composite frame glow ready: tile=%zu cell=%dx%d paint_ms=%d upload_ms=%d total=%llu strips=%d "
				   "upload_max_ms=%.2f",
				   index, cell.w, cell.h, whole_ms(image.paint_ns), whole_ms(image.upload.total_ns),
				   static_cast<unsigned long long>(frame_uploads_total), image.upload.holds,
				   ms_of(image.upload.max_ns));
		}
		if (!cache.frame_shown || cache.frame_key != frame_key) {
			handoff.submit(frame_slot(index), PaintKey(frame_key), [&]() -> PaintInput {
				FrameJob job;
				job.cell_w = cell.w;
				job.cell_h = cell.h;
				job.argb = look.glow_argb;
				job.blur_px = look.glow_blur_ratio * name_px;
				job.spread_px = look.glow_spread_ratio * name_px;
				return job;
			});
		}
	} else if (cache.frame_shown || cache.frame.present()) {
		// 枠の光を描かない形に替わったら、大きなテクスチャを持ち続けない。
		cache.frame = GpuImage();
		cache.frame_shown = false;
	}

	const BorderTiming timing = timing_of(view.params);
	out.level = composite_speaking_level(view.speaking[oi], now_ns, timing, view.params.show_speaking, muted, volume);
	out.label_dim = composite_label_dim_at(view.speaking[oi], now_ns, timing, view.dim_pct, hears_voice, muted, volume);
	out.label = image_of(cache.label);
	out.glow = image_of(cache.glow);
	out.label_current = cache.label_shown && cache.label_key == label_key;
	out.label_owner = cache.label_shown ? cache.label_key.owner : std::string();
	out.owner = label_key.owner;
	out.frame = image_of(cache.frame);
	out.frame_current = cache.frame_shown && cache.frame_key == frame_key;
}

std::vector<RosterCache *> CompositeDecor::Impl::refresh_roster(const std::vector<CompositeRosterPerson> &people,
								  const std::vector<std::size_t> &order,
								  const CompositeRosterView &roster_view, bool show)
{
	// 並びから消えた人と、並びを出さない形のときの全員のテクスチャを捨てる。
	std::vector<std::string> drawn_ids;
	if (show) {
		for (const std::size_t index : order)
			drawn_ids.push_back(people[index].user_id);
	}
	std::vector<std::string> cached_ids;
	for (const auto &entry : roster)
		cached_ids.push_back(entry.first);
	for (const std::string &id : composite_roster_departed(cached_ids, drawn_ids))
		forget_roster_person(roster.find(id));

	// 参加者ごとのキャッシュを用意し、表からコピーした人の情報を入れる。
	std::vector<RosterCache *> caches;
	for (std::size_t k = 0; k < drawn_ids.size(); ++k) {
		auto found = roster.find(drawn_ids[k]);
		if (found == roster.end()) {
			RosterCache fresh;
			if (!free_roster_slots.empty()) {
				fresh.slot = free_roster_slots.back();
				free_roster_slots.pop_back();
			} else {
				fresh.slot = handoff.add_slot();
			}
			found = roster.emplace(drawn_ids[k], std::move(fresh)).first;
		}
		RosterCache &cache = found->second;
		const CompositeRosterMaterial *material = k < roster_view.people.size() ? &roster_view.people[k] : nullptr;
		cache.present = material && material->present;
		if (!cache.present) {
			cache.version = 0;
			cache.name.clear();
			cache.avatar = QImage();
		} else if (material->copied) {
			cache.version = material->version;
			cache.name = material->name;
			cache.avatar = material->avatar;
		}
		caches.push_back(&cache);
	}
	return caches;
}

void CompositeDecor::Impl::update_roster(int canvas_w, int canvas_h, const std::vector<std::string> &tile_names,
					 double name_px, double margin_px, std::uint64_t now_ns, bool muted, float volume,
					 const std::vector<CompositeRosterPerson> &people,
					 const std::vector<std::size_t> &order, const std::vector<bool> &shown,
					 const CompositeRosterView &roster_view,
					 const std::vector<RosterCache *> &caches, CompositeDrawRequest &request,
					 std::vector<const RosterCache *> &placed)
{
	const CompositeRosterParams &params = roster_view.params;
	if (!(params.show && name_px > 0.0)) {
		roster_not_placed = 0;
		log_roster_shown(0);
		return;
	}

	// ほかの人と見分けるための名前は、いまキャンバスに見えている名前の全部 (名前ラベル付きで描いている枠の人と、並びに描く人の
	// うち人の情報が届いている人) から、本人を除いたもの (composite_roster_others)。
	std::vector<std::string> visible_names = tile_names;
	for (std::size_t k = 0; k < caches.size(); ++k) {
		if (k < shown.size() && shown[k] && caches[k]->present)
			visible_names.push_back(caches[k]->name);
	}

	const int cap_w = composite_roster_cap_width(canvas_w, margin_px);
	const CompositeSpeakingParts parts = composite_speaking_parts(view.look);
	CompositeLabelLook look = view.look;
	look.avatar_side = params.avatar_side;
	look.max_name_em = params.name_max_em;
	if (!parts.label_glow) {
		look.glow_avatar = false;
		look.glow_name = false;
	}

	for (std::size_t k = 0; k < caches.size(); ++k) {
		RosterCache &cache = *caches[k];
		if (!cache.present) {
			// 人の情報がまだ届いていない人は描かない (ID を名前の代わりに描かない)。
			cache.label = GpuImage();
			cache.glow = GpuImage();
			cache.shown = false;
			continue;
		}
		CompositeRosterLabelKey key;
		key.user_id = people[order[k]].user_id;
		key.person_version = cache.version;
		key.look_version = view.look_version;
		key.avatar_side = params.avatar_side;
		key.name_max_em = params.name_max_em;
		key.name_q = composite_quantize(name_px);
		key.cell_w = cap_w;
		key.cell_h = canvas_h;
		key.others = composite_roster_others(visible_names, cache.name);

		if (auto done = handoff.take(cache.slot, PaintKey(key))) {
			const std::uint64_t started = os_gettime_ns();
			LabelImages &images = std::get<LabelImages>(done->output);
			cache.label = std::move(images.label);
			cache.glow = std::move(images.glow);
			cache.box_x = images.box_x;
			cache.box_y = images.box_y;
			cache.box_w = images.box_w;
			cache.box_h = images.box_h;
			cache.shown = true;
			cache.key = std::get<CompositeRosterLabelKey>(done->key);
			note_swap(static_cast<std::int64_t>(os_gettime_ns() - started));
			++roster_uploads_total;
			// upload_ms、strips、upload_max_ms は名前ラベルの行と同じ。
			diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				   "Composite roster label ready: index=%zu paint_ms=%d upload_ms=%d total=%llu strips=%d "
				   "upload_max_ms=%.2f",
				   k, whole_ms(images.paint_ns), whole_ms(images.upload.total_ns),
				   static_cast<unsigned long long>(roster_uploads_total), images.upload.holds,
				   ms_of(images.upload.max_ns));
		}
		if (!cache.shown || cache.key != key) {
			handoff.submit(cache.slot, PaintKey(key), [&]() -> PaintInput {
				// 名前の幅の上限の幅と、キャンバスの高さのセルに、余白 0 で描く。位置はセルの左上 (隅は左上) で、
				// 並べる位置は plan_composite_roster が決める。
				LabelJob job;
				job.person.name = cache.name;
				job.person.avatar = cache.avatar;
				job.person.corner = DecorCorner::TopLeft;
				job.person.other_visible_names = key.others;
				job.look = look;
				job.name_px = name_px;
				job.margin_px = 0.0;
				job.cell_w = cap_w;
				job.cell_h = canvas_h;
				return job;
			});
		}
	}

	// 並べる位置。名前ラベルの画像がまだない人は並べず、数えない。描き直しを待つ間は、同じ人の前の画像を置く。
	CompositeRosterRequest layout;
	layout.canvas_w = canvas_w;
	layout.canvas_h = canvas_h;
	layout.margin_px = static_cast<int>(std::lround(margin_px));
	layout.gap_px = composite_roster_gap_px(params.gap_ratio, name_px);
	layout.anchor = params.anchor;
	layout.direction = params.direction;
	layout.avatar_side = params.avatar_side;
	layout.offset_x_px = params.offset_x_px;
	layout.offset_y_px = params.offset_y_px;
	for (std::size_t k = 0; k < caches.size(); ++k) {
		const RosterCache *cache = caches[k];
		CompositeRosterBox box;
		if (k < shown.size() && shown[k] && cache->shown && cache->label.present()) {
			box.w = cache->box_w;
			box.h = cache->box_h;
		}
		layout.boxes.push_back(box);
	}
	const CompositeRosterPlan plan = plan_composite_roster(layout);
	if (plan.not_placed != roster_not_placed) {
		roster_not_placed = plan.not_placed;
		if (plan.not_placed > 0)
			diag::logf(LOG_INFO, "Composite roster has no room for %d participant(s)", plan.not_placed);
	}
	log_roster_shown(static_cast<int>(plan.placed.size()));

	// 画像の位置は、名前ラベルを描いたセルの左上から見た位置なので、矩形の左上 (box_x、box_y) と置く位置の差だけずらす。
	const BorderTiming timing = timing_of(view.params);
	for (const CompositeRosterPlaced &spot : plan.placed) {
		const RosterCache &cache = *caches[spot.index];
		const int dx = spot.x - cache.box_x;
		const int dy = spot.y - cache.box_y;
		const SpeakingTimes &speaking =
			spot.index < roster_view.speaking.size() ? roster_view.speaking[spot.index] : SpeakingTimes{};
		CompositeDrawRosterEntry entry;
		entry.label = image_of(cache.label, dx, dy);
		entry.glow = image_of(cache.glow, dx, dy);
		entry.level = composite_speaking_level(speaking, now_ns, timing, view.params.show_speaking, muted, volume);
		const bool hears = source_voice_hears(
			spot.index < roster_view.source_voice.size() ? roster_view.source_voice[spot.index]
								      : SourceVoice::NotJudged,
			people[order[spot.index]].hears_voice);
		entry.dim = composite_label_dim_at(speaking, now_ns, timing, view.dim_pct, hears, muted, volume);
		request.roster.push_back(entry);
		placed.push_back(&cache);
	}
}

void CompositeDecor::draw_background(int canvas_w, int canvas_h)
{
	const CompositeBackground background = read_composite_background();
	const bool drawn = composite_background_drawn(background) && canvas_w > 0 && canvas_h > 0;
	std::lock_guard<std::mutex> lock(mu_);
	Impl &m = *impl_;
	if (!drawn) {
		// 描かないときは、1 画素のテクスチャも持たない。
		m.background.reset();
		m.background_made = false;
		return;
	}
	const std::uint32_t pixel = composite_background_premultiplied(background.argb);
	if (!m.background_made || m.background_pixel != pixel) {
		m.background.reset();
		m.background_made = true;
		m.background_pixel = pixel;
		// 画素の並びは、QImage の ARGB32 と同じ B、G、R、A (make_image と同じ)。テクスチャは中身を書き換えないので、
		// GS_DYNAMIC を付けずに作り、作るときに中身を渡す (理由は make_image)。描画のスレッドで作るのは、1 画素だけで、
		// 色か不透明度を変えたときの 1 回だけだからである。
		m.background.reserve(1); // 作ったテクスチャが所有者なしに残らないよう、先に場所を取る
		const std::uint8_t *data = reinterpret_cast<const std::uint8_t *>(&m.background_pixel);
		gs_texture_t *texture = gs_texture_create(1, 1, GS_BGRA, 1, &data, 0);
		if (!texture) {
			diag::logf(LOG_WARNING, "Couldn't create the slot background texture");
			return;
		}
		m.background.add(texture);
	}
	if (m.background.empty())
		return;

	// 名前ラベルの背景の画像と同じ置き方: 標準の効果の Draw と、乗算済みアルファの混ぜ方 (GS_BLEND_ONE と
	// GS_BLEND_INVSRCALPHA。混ぜるのが止まっていても有効にする)。1 画素のテクスチャを、キャンバスの大きさに伸ばして置く
	// (どの画素も同じ色なので、補間しても色は変わらない)。
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *param = effect ? gs_effect_get_param_by_name(effect, "image") : nullptr;
	if (!param)
		return;
	gs_texture_t *texture = m.background.at(0);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(param, texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, static_cast<std::uint32_t>(canvas_w), static_cast<std::uint32_t>(canvas_h));
	gs_blend_state_pop();
}

void CompositeDecor::draw(int canvas_w, int canvas_h, const std::vector<CompositeTile> &tiles,
			  const std::vector<bool> &has_video, const std::vector<bool> &hears_voice,
			  const std::vector<std::string> &tile_user_ids,
			  const std::vector<CompositeRosterPerson> &roster, bool muted, float volume)
{
	// 名前ラベルを出す人 (ドックが渡した、通話の参加者のうち名前ラベルを出す人) は、描く順に並べ、user_id が空の人と 2 人目
	// 以降の同じ人を除く。誰を並びに描くか (名前ラベル付きで描いている枠がない人) は、タイルに名前ラベルを描くかを決めた後に
	// 決める (下の composite_tiles_labeled)。その判断は表の見た目の値 (label_hide_when_single) を使い、その値は表を読んでから
	// 分かるので、表は全員の分を読む。
	const std::vector<std::size_t> order = composite_roster_drawn(roster);
	std::vector<std::string> roster_ids;
	for (const std::size_t index : order)
		roster_ids.push_back(roster[index].user_id);

	// 表のロックを取る間は内部のロックを持たない (どちらも持ったままもう一方を取らないので、ロックの順を作らない)。
	// 前にコピーしたときの値だけを内部のロックの下で取り出し、表から読んだ後に、また内部のロックの下で入れる。
	std::uint64_t known_version = 0;
	std::uint64_t known_look_version = 0;
	std::array<std::string, kScreenTileCount + 1> known_owners;
	std::vector<std::uint64_t> known_roster_versions(roster_ids.size(), 0);
	{
		std::lock_guard<std::mutex> lock(mu_);
		known_version = impl_->view.version;
		known_look_version = impl_->view.look_version;
		known_owners = impl_->view.owners;
		for (std::size_t k = 0; k < roster_ids.size(); ++k) {
			const auto found = impl_->roster.find(roster_ids[k]);
			if (found != impl_->roster.end() && found->second.present)
				known_roster_versions[k] = found->second.version;
		}
	}
	// 人の情報と見た目の値は、前にコピーしたものと違うときだけコピーされる。
	CompositeDecorView fresh = read_composite_decor(known_version, known_look_version, known_owners);
	const CompositeRosterView roster_view = read_composite_roster(roster_ids, known_roster_versions);

	std::lock_guard<std::mutex> lock(mu_);
	Impl &m = *impl_;
	if (fresh.material_valid) {
		m.view = std::move(fresh);
	} else {
		m.view.speaking = fresh.speaking;
		m.view.source_voice = fresh.source_voice;
		m.view.dim_pct = fresh.dim_pct;
	}

	const double name_px = composite_name_px(canvas_h, m.view.params.em_ratio);
	const double margin_px = composite_margin_px(canvas_h, m.view.look.margin_ratio);
	const std::uint64_t now_ns = os_gettime_ns();

	// タイルごとに名前ラベルを描くか。同じ値で、名前ラベルを出す人をタイルと並びのどちらに描くかも決める (同じ人が両方に
	// 出たり、どちらにも出なかったりしないように)。枠の光は、名前ラベルを枠に描かない設定でも描くので、別に決める。
	const std::vector<bool> tile_framed = composite_tiles_framed(m.view.look, canvas_w, canvas_h, tiles, has_video);
	const std::vector<bool> tile_labeled =
		composite_tiles_labeled(m.view.look, canvas_w, canvas_h, tiles, has_video);
	const std::vector<std::string> labeled_users = composite_labeled_tile_users(tile_labeled, tile_user_ids);
	std::vector<bool> roster_shown;
	for (const std::size_t index : order)
		roster_shown.push_back(composite_roster_shows(roster[index], labeled_users));

	CompositeDrawRequest request;
	request.canvas_w = canvas_w;
	request.canvas_h = canvas_h;
	request.hide = composite_hides_label(m.view.look, canvas_w, canvas_h, tiles) || !(name_px > 0.0);
	request.parts = composite_speaking_parts(m.view.look);

	// 名前ラベルを出す人の情報を先にコピーする。並びに描く人の名前は、枠の名前ラベルでも、ほかの人と見分けるために使う
	// (並びを出さない形では、名前が見えないので使わない)。
	const std::vector<RosterCache *> roster_caches =
		m.refresh_roster(roster, order, roster_view, roster_view.params.show && name_px > 0.0);
	std::vector<std::string> roster_names;
	for (std::size_t k = 0; k < roster_caches.size(); ++k) {
		if (roster_shown[k] && roster_caches[k]->present)
			roster_names.push_back(roster_caches[k]->name);
	}

	// 名前ラベル付きで描くタイルの人の名前 (並びの名前ラベルが、ほかの人と見分けるために使う)。
	std::vector<std::string> tile_names;
	request.tiles.resize(tiles.size());
	for (std::size_t index = 0; index < tiles.size(); ++index) {
		CompositeDrawTile &out = request.tiles[index];
		out.cell = tiles[index].rect;
		out.has_video = index < has_video.size() && has_video[index];
		if (!tile_framed[index] || !(name_px > 0.0))
			continue;
		// 音声ソースで発言を決める人は、ソースが見つかっていれば声を聞けるものとして、判定できなければ聞けないものとして扱う。
		const int owner_index = tiles[index].self ? 0 : tiles[index].slot;
		const bool discord_hears = index < hears_voice.size() && hears_voice[index];
		const bool hears = owner_index >= 0 && owner_index < static_cast<int>(m.view.source_voice.size())
					   ? source_voice_hears(m.view.source_voice[static_cast<std::size_t>(owner_index)], discord_hears)
					   : discord_hears;
		const auto oi = static_cast<std::size_t>(owner_index);
		if (tile_labeled[index] && oi < m.view.owners.size() && m.view.person_present[oi] && !m.view.owners[oi].empty())
			tile_names.push_back(m.view.person[oi].name);
		m.update_tile(index, tiles[index], tile_labeled[index], name_px, margin_px, now_ns, hears, muted, volume,
			      roster_names, out);
	}
	// 並びは、画面共有の枠の名前ラベルを隠すとき (hide) にも描く (並びは自分の設定で出すかを決める)。
	std::vector<const RosterCache *> placed;
	m.update_roster(canvas_w, canvas_h, tile_names, name_px, margin_px, now_ns, muted, volume, roster, order,
			roster_shown, roster_view, roster_caches, request, placed);

	// 置く物と順と切り抜きは計画が決める (composite_draw_plan.h)。CPU で重ねる composite_draw_cpu.cpp と同じ式に
	// するため、混ぜ方は GS_BLEND_ONE と GS_BLEND_INVSRCALPHA (色もアルファも) だけを使う。
	const std::vector<CompositeDraw> draws = plan_composite_draws(request);
	if (draws.empty())
		return;
	// 混ぜるのが止まっていると、乗算済みアルファの画像が下の映像を置き換えるので、混ぜるのも有効にする (pop で戻る)。
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	for (const CompositeDraw &draw : draws) {
		switch (draw.kind) {
		case CompositeDrawKind::FrameGlow:
			if (draw.tile < m.tiles.size())
				draw_faded(m.tiles[draw.tile].frame, draw);
			break;
		case CompositeDrawKind::LabelGlow:
			if (draw.tile < m.tiles.size())
				draw_faded(m.tiles[draw.tile].glow, draw);
			break;
		case CompositeDrawKind::Label:
			if (draw.tile < m.tiles.size())
				draw_label(m.tiles[draw.tile].label, draw);
			break;
		case CompositeDrawKind::RosterGlow:
			if (draw.tile < placed.size())
				draw_faded(placed[draw.tile]->glow, draw);
			break;
		case CompositeDrawKind::RosterLabel:
			if (draw.tile < placed.size())
				draw_label(placed[draw.tile]->label, draw);
			break;
		}
	}
	gs_blend_state_pop();
}

void CompositeDecor::release()
{
	std::lock_guard<std::mutex> lock(mu_);
	Impl &m = *impl_;
	for (TileCache &cache : m.tiles) {
		cache.label = GpuImage();
		cache.glow = GpuImage();
		cache.frame = GpuImage();
		cache.label_shown = false;
		cache.frame_shown = false;
	}
	while (!m.roster.empty())
		m.forget_roster_person(m.roster.begin());
	m.roster_not_placed = 0;
	m.roster_shown_logged = -1;
	m.background.reset();
	m.background_made = false;
	// 描き終えて受け取っていない画像も、ここ (グラフィックスのコンテキストの中) で壊れる。
	m.handoff.drain_done();
}

void CompositeDecor::stop_painter()
{
	impl_->handoff.stop();
}

CompositeDecorPaintStats CompositeDecor::take_paint_stats()
{
	CompositeDecorPaintStats stats;
	stats.paint = impl_->handoff.take_counts();
	// 描き直しの受け渡しが数える時間は、テクスチャを作る時間、グラフィックスのコンテキストを待つ時間、帯の間に次の
	// フレームを待つ時間を含むので、描いた時間は描き直しのスレッドが QPainter の分だけ数えた値にする。
	stats.paint.max_paint_ns = impl_->painter_stats.max_paint_ns.exchange(0);
	stats.max_upload_ns = impl_->painter_stats.max_upload_ns.exchange(0);
	std::lock_guard<std::mutex> lock(mu_);
	stats.uploads = impl_->uploads;
	stats.max_swap_ns = impl_->max_swap_ns;
	impl_->uploads = 0;
	impl_->max_swap_ns = 0;
	return stats;
}

void release_composite_decor_shared()
{
	if (!g_fade_effect_tried)
		return; // 作っていないので、グラフィックスのコンテキストに入らない
	obs_enter_graphics();
	if (g_fade_effect) {
		gs_effect_destroy(g_fade_effect);
		g_fade_effect = nullptr;
	}
	g_fade_effect_tried = false;
	obs_leave_graphics();
}

} // namespace cordmux::sources

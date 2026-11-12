// SPDX-License-Identifier: Apache-2.0

// ダミー表示の色のタイルに動く線を重ねる純粋関数と、フレームを渡す間隔を決める純粋関数。Qt にも libobs にも依存しない。
// フレームは NV12 (Y の面の後に、U と V を交互に並べた面。色差は縦横とも半分) で、本物の画面共有の映像 (pipeline/ が I420 か
// NV12 で渡す) と同じく 1 画素 1.5 バイトで libobs に渡す。色は BT.709 のフルレンジで YUV にする。色の定数は 0xAARRGGBB
// (sources/decor_paint.h と同じ) で書く。枠の番号は、文字を描画すると Qt に依存するので、ここでは描画しない。番号を描画した
// 背景の画像 (BGRA) は decor_paint.cpp の paint_dummy_background が枠ごと・大きさごとに 1 回だけ描画し、dummy_nv12_from_bgra で
// 1 回だけ NV12 にする。フレームごとには、前のフレームの線の列だけを戻して線を描き直す (move_dummy_line)。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cordmux::sources {

// 色の数 (画面共有の枠 4 つ。ダミー映像の間は自分の枠を描かないので、自分の枠の色は持たない。sources/composite_input.h の
// dummy_composite_input)。枠の番号をこの数で巡回させるので、番号が範囲の外でも色は必ず
// 1 つ決まる。ダミーは見た目を確認するためのものなので、番号を取り違えて真っ黒になるより、何かが表示される
// ほうがよい。
inline constexpr int kDummyTileColorCount = 4;

// 動く線の色 (0xAARRGGBB)。どの枠の色の上でも目立つ白にする。
inline constexpr std::uint32_t kDummyMarkColor = 0xFFFFFFFFu;

// ダミー映像のフレームレート。普通の映像が滑らかに動くかを確かめるための値で、設定では変えない (細かく合わせる
// 必要のある値ではないため)。OBS のフレームレートがこれより低いときは、OBS の描画の刻みごとに 1 フレームになる。
inline constexpr int kDummyFps = 60;

// 線の 1 往復の既定値 (ナノ秒。設定の dummy_cycle_ms で変えられる)。線をフレームごとに少しずつ動かし、
// 1 往復を 2 秒以上にするのは、同じ場所が明暗で何度も入れ替わる点滅を作らないためである。
inline constexpr std::uint64_t kDefaultDummyCycleNs = 4'000'000'000ULL;

// 画面共有の枠 slot (1 から 4) の背景の色 (0xAARRGGBB)。範囲の外の番号は巡回させる (5 は 1 と、0 は 4 と同じ色)。
// 色は、隣り合う枠を見分けられることだけを条件にした値である。
inline std::uint32_t dummy_tile_color(int slot)
{
	static constexpr std::uint32_t colors[kDummyTileColorCount] = {
		0xFF7A2E2Eu, // 画面共有の枠 1
		0xFF2E7A3Au, // 画面共有の枠 2
		0xFF2E3A7Au, // 画面共有の枠 3
		0xFF7A6A2Eu, // 画面共有の枠 4
	};
	int index = (slot - 1) % kDummyTileColorCount;
	if (index < 0)
		index += kDummyTileColorCount;
	return colors[index];
}

// ---- NV12 の形と色 ----

// 1 フレームの NV12 の並び。Y の面 (width x height、1 行 width バイト) の後に、色差の面 (chroma_width x chroma_height の
// 標本を U、V の順に 2 バイトずつ。1 行 chroma_width * 2 バイト) を置く。色差の標本は、縦横 2 x 2 の画素の組に 1 つで、
// 幅か高さが奇数のときは、右端と下端の組は 1 列か 1 行だけになる。
struct DummyNv12Layout {
	int width = 0;
	int height = 0;
	int chroma_width = 0;
	int chroma_height = 0;

	std::size_t y_bytes() const { return static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
	std::size_t uv_stride() const { return static_cast<std::size_t>(chroma_width) * 2u; }
	std::size_t uv_bytes() const { return uv_stride() * static_cast<std::size_t>(chroma_height); }
	std::size_t total_bytes() const { return y_bytes() + uv_bytes(); }
};

// width と height の NV12 の並び。どちらかが 0 以下なら、すべて 0 の並びを返す。
inline DummyNv12Layout dummy_nv12_layout(int width, int height)
{
	DummyNv12Layout layout;
	if (width <= 0 || height <= 0)
		return layout;
	layout.width = width;
	layout.height = height;
	layout.chroma_width = (width + 1) / 2;
	layout.chroma_height = (height + 1) / 2;
	return layout;
}

// YUV の 1 組 (8 ビット、フルレンジ)。
struct DummyYuv {
	std::uint8_t y = 0;
	std::uint8_t u = 128;
	std::uint8_t v = 128;
};

// RGB (0 から 255。端数を持ってよい) を BT.709 のフルレンジの YUV にする。OBS が NV12 を RGB に戻す式
// (libobs の video_format_get_parameters_for_format に VIDEO_CS_709 と VIDEO_RANGE_FULL を渡した行列) の逆である。
// 呼び出し元は、フレームにその 2 つを付けて渡す (dummy_feed.cpp)。
inline DummyYuv dummy_yuv_of_rgb(double r, double g, double b)
{
	constexpr double kKr = 0.2126;
	constexpr double kKb = 0.0722;
	const double y = kKr * r + (1.0 - kKr - kKb) * g + kKb * b;
	const double u = (b - y) / (2.0 * (1.0 - kKb)) + 128.0;
	const double v = (r - y) / (2.0 * (1.0 - kKr)) + 128.0;
	const auto to_byte = [](double value) {
		return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
	};
	DummyYuv out;
	out.y = to_byte(y);
	out.u = to_byte(u);
	out.v = to_byte(v);
	return out;
}

// 0xAARRGGBB の色 (アルファは見ない) の YUV。
inline DummyYuv dummy_yuv_of_argb(std::uint32_t argb)
{
	return dummy_yuv_of_rgb(static_cast<double>((argb >> 16) & 0xFFu), static_cast<double>((argb >> 8) & 0xFFu),
				static_cast<double>(argb & 0xFFu));
}

// 不透明な BGRA の画像 (1 行 width * 4 バイト。QImage の Format_ARGB32_Premultiplied と同じ並び) を、out に NV12 で書く。
// 色差は 2 x 2 の組の RGB の平均から求める (YUV は RGB の 1 次式なので、組の 4 画素の YUV の平均と同じ)。大きさが 0 以下の
// とき、bgra か out が nullptr のとき、bgra_size が width * height * 4 と違うとき、out_size が足りないときは、何も書かずに
// false を返す。背景の画像の大きさが変わったときに 1 回だけ呼ぶ (フレームごとには呼ばない)。
// 背景の画像はほとんどが枠の色の単色なので、直前の画素 (色差は直前の組) と同じ色なら、計算した値をそのまま使う (結果は
// 1 画素ずつ計算したときと同じ)。1 画素ずつ dummy_yuv_of_rgb で計算すると、4 つの枠の 1080p で約 0.7 秒、2160p で約 3 秒
// かかった (使い回すと約 75 ms と約 300 ms。dummy_feed.h の set_dummy_mode のコメントと同じ測り方)。
inline bool dummy_nv12_from_bgra(const std::uint8_t *bgra, std::size_t bgra_size, int width, int height,
				 std::uint8_t *out, std::size_t out_size)
{
	const DummyNv12Layout layout = dummy_nv12_layout(width, height);
	if (bgra == nullptr || out == nullptr || layout.width == 0 || out_size < layout.total_bytes() ||
	    bgra_size != layout.y_bytes() * 4u)
		return false;
	const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
	const auto pixel = [&](int x, int y) { return bgra + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(x) * 4u; };
	// 直前に計算した色 (R、G、B を 1 つの数にしたもの) と、その Y。
	std::uint32_t last_rgb = 0xFFFFFFFFu; // どの色とも違う値 (色は 24 ビットに収まる)
	std::uint8_t last_y = 0;
	for (int y = 0; y < height; ++y) {
		std::uint8_t *row_out = out + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
		for (int x = 0; x < width; ++x) {
			const std::uint8_t *p = pixel(x, y);
			const std::uint32_t rgb = (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
			if (rgb != last_rgb) {
				last_rgb = rgb;
				last_y = dummy_yuv_of_rgb(p[2], p[1], p[0]).y;
			}
			row_out[x] = last_y;
		}
	}
	// 直前に計算した組 (R、G、B の合計と画素の数) と、その YUV。
	int last_r = -1;
	int last_g = -1;
	int last_b = -1;
	int last_count = 0;
	DummyYuv last_yuv;
	std::uint8_t *uv = out + layout.y_bytes();
	for (int cy = 0; cy < layout.chroma_height; ++cy) {
		for (int cx = 0; cx < layout.chroma_width; ++cx) {
			int r = 0;
			int g = 0;
			int b = 0;
			int count = 0;
			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					const int x = cx * 2 + dx;
					const int y = cy * 2 + dy;
					if (x >= width || y >= height)
						continue;
					const std::uint8_t *p = pixel(x, y);
					r += p[2];
					g += p[1];
					b += p[0];
					++count;
				}
			}
			if (r != last_r || g != last_g || b != last_b || count != last_count) {
				last_r = r;
				last_g = g;
				last_b = b;
				last_count = count;
				last_yuv = dummy_yuv_of_rgb(static_cast<double>(r) / count, static_cast<double>(g) / count,
							    static_cast<double>(b) / count);
			}
			std::uint8_t *sample = uv + static_cast<std::size_t>(cy) * layout.uv_stride() + static_cast<std::size_t>(cx) * 2u;
			sample[0] = last_yuv.u;
			sample[1] = last_yuv.v;
		}
	}
	return true;
}

// ---- 動く線 ----

// 動く線の太さ (ピクセル)。高さの 180 分の 1 (1080 で 6 px) で、2 px より細くしない (配信の画面を 360p に
// 縮めても残る)。
inline int dummy_line_width(int height)
{
	return std::max(2, static_cast<int>(std::lround(static_cast<double>(height) / 180.0)));
}

// 時刻 elapsed_ns (ダミー表示を始めてからの時間) での線の左端 (ピクセル。端数を持つ)。三角波で左右に動き、
// cycle_ns で 1 往復する。位相はフレームの番号ではなく時刻から求める (フレームレートによらず同じ速さにする
// ため)。cycle_ns が 0 なら左端に止める。
inline double dummy_line_left(int width, int height, std::uint64_t elapsed_ns, std::uint64_t cycle_ns)
{
	const double line = static_cast<double>(dummy_line_width(height));
	const double travel = std::max(0.0, static_cast<double>(width) - line);
	if (cycle_ns == 0)
		return 0.0;
	const double phase = static_cast<double>(elapsed_ns % cycle_ns) / static_cast<double>(cycle_ns);
	const double tri = phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0;
	return travel * tri;
}

// ダミー表示の間に、話している人として光らせる枠の番号 (1 から slot_count)。話している人の光の見た目を、通話なしで確認する
// ためで、枠の番号の順に 1 つずつ回す。cycle_ns (線の 1 往復と同じ時間) で全部の枠を 1 周するので、1 つの枠は cycle_ns /
// slot_count の間光る。elapsed_ns はダミー表示を始めてからの時間。cycle_ns か slot_count が 0 以下のときは、枠 1 に止める。
inline int dummy_speaker_slot(std::uint64_t elapsed_ns, std::uint64_t cycle_ns, int slot_count)
{
	if (cycle_ns == 0 || slot_count <= 0)
		return 1;
	const std::uint64_t each = cycle_ns / static_cast<std::uint64_t>(slot_count);
	if (each == 0)
		return 1;
	return static_cast<int>((elapsed_ns / each) % static_cast<std::uint64_t>(slot_count)) + 1;
}

// 左端 left の線が書き換える列の範囲 (first から last まで。last < first なら書き換えない)。
inline void dummy_line_columns(int width, int height, double left, int *first, int *last)
{
	const double right = left + static_cast<double>(dummy_line_width(height));
	*first = std::max(0, static_cast<int>(std::floor(left)));
	*last = std::min(width - 1, static_cast<int>(std::ceil(right)) - 1);
}

namespace detail {

// 列 x のうち、左端 left の線が覆う割合を 0 から 256 にした重み。
inline int dummy_line_weight(int height, double left, int x)
{
	const double right = left + static_cast<double>(dummy_line_width(height));
	const double cover = std::clamp(std::min(right, static_cast<double>(x + 1)) - std::max(left, static_cast<double>(x)),
					0.0, 1.0);
	return cover <= 0.0 ? 0 : static_cast<int>(std::lround(cover * 256.0));
}

// NV12 の out の、左端 left から線の太さの分に白の線を重ねる。線の端の画素には、線が覆う幅の割合で白を混ぜる (端数の位置を
// なめらかに見せるため。1 往復 4 秒、幅 1920、60 fps では、1 フレームで約 16 ピクセル動く)。白は Y が 255、U と V が 128
// である。YUV は RGB の 1 次式なので、YUV のまま混ぜても、RGB で混ぜてから変換したのと同じになる。色差の標本は、組の 2 列の
// 重みの平均で混ぜる。重ねる列は dummy_line_columns の範囲、色差の標本はその列を含む組である。
inline void blend_dummy_line(const DummyNv12Layout &layout, double left, std::uint8_t *out)
{
	int first = 0;
	int last = -1;
	dummy_line_columns(layout.width, layout.height, left, &first, &last);
	if (last < first)
		return;
	const DummyYuv white = dummy_yuv_of_argb(kDummyMarkColor);
	for (int x = first; x <= last; ++x) {
		const int weight = dummy_line_weight(layout.height, left, x);
		if (weight == 0)
			continue;
		for (int y = 0; y < layout.height; ++y) {
			std::uint8_t &value = out[static_cast<std::size_t>(y) * static_cast<std::size_t>(layout.width) +
						  static_cast<std::size_t>(x)];
			value = static_cast<std::uint8_t>((value * (256 - weight) + white.y * weight) >> 8);
		}
	}
	std::uint8_t *uv = out + layout.y_bytes();
	for (int cx = first / 2; cx <= last / 2; ++cx) {
		const int left_weight = dummy_line_weight(layout.height, left, cx * 2);
		const int right_weight = cx * 2 + 1 < layout.width ? dummy_line_weight(layout.height, left, cx * 2 + 1) : left_weight;
		const int weight = (left_weight + right_weight) / 2;
		if (weight == 0)
			continue;
		for (int cy = 0; cy < layout.chroma_height; ++cy) {
			std::uint8_t *sample = uv + static_cast<std::size_t>(cy) * layout.uv_stride() + static_cast<std::size_t>(cx) * 2u;
			sample[0] = static_cast<std::uint8_t>((sample[0] * (256 - weight) + white.u * weight) >> 8);
			sample[1] = static_cast<std::uint8_t>((sample[1] * (256 - weight) + white.v * weight) >> 8);
		}
	}
}

} // namespace detail

// 枠 1 つ分の、線のない絵を out に NV12 で書く。background が同じ大きさの NV12 の背景 (dummy_nv12_from_bgra で番号を描いた
// 画像から作ったもの) ならそれをコピーし、なければ (nullptr か大きさが違えば) 枠の色の単色にする。width か height が 0 以下の
// とき、out が nullptr のとき、out_size が足りないときは、何も書かずに false を返す。
inline bool fill_dummy_base(int slot, int width, int height, const std::uint8_t *background, std::size_t background_size,
			    std::uint8_t *out, std::size_t out_size)
{
	const DummyNv12Layout layout = dummy_nv12_layout(width, height);
	if (out == nullptr || layout.width == 0 || out_size < layout.total_bytes())
		return false;
	if (background != nullptr && background_size == layout.total_bytes()) {
		std::memcpy(out, background, layout.total_bytes());
		return true;
	}
	const DummyYuv base = dummy_yuv_of_argb(dummy_tile_color(slot));
	std::memset(out, base.y, layout.y_bytes());
	std::uint8_t *uv = out + layout.y_bytes();
	for (std::size_t i = 0; i < layout.uv_bytes(); i += 2u) {
		uv[i] = base.u;
		uv[i + 1] = base.v;
	}
	return true;
}

// 枠 1 つ分のフレームを out に NV12 で書く (fill_dummy_base の絵に、時刻 elapsed_ns の位置の動く線を重ねる)。返す値は
// fill_dummy_base と同じ。フレームごとの絵は move_dummy_line で作り、この関数は 1 フレームを最初から作るときに使う。
inline bool fill_dummy_tile(int slot, int width, int height, std::uint64_t elapsed_ns, std::uint64_t cycle_ns,
			    const std::uint8_t *background, std::size_t background_size, std::uint8_t *out,
			    std::size_t out_size)
{
	if (!fill_dummy_base(slot, width, height, background, background_size, out, out_size))
		return false;
	detail::blend_dummy_line(dummy_nv12_layout(width, height), dummy_line_left(width, height, elapsed_ns, cycle_ns), out);
	return true;
}

// 動く線の下にあった画素 (枠ごとに 1 つ持つ)。move_dummy_line が、次のフレームで線を消すために覚える。
struct DummyLineStrip {
	int first = 0; // 覚えている Y の列の範囲 (first から last まで。last < first なら何も覚えていない)
	int last = -1;
	std::vector<std::uint8_t> saved_y;  // Y の面の first から last の列 (行ごとに last - first + 1 バイト)
	std::vector<std::uint8_t> saved_uv; // 色差の面の first / 2 から last / 2 の標本 (行ごとに 2 バイトずつ)
};

// 前のフレームの絵を持つ out (NV12) の動く線を、時刻 elapsed_ns の位置に描き直す。strip が覚えている前の線の下の画素を戻し、
// 新しい線の下の画素を strip に覚えてから、線を重ねる。書き換えるのは、前の線と新しい線の列だけである。フレームの全体を
// 背景からコピーし直すと、1080p の 1 枠で 1 フレームに 3.1 MB のコピーになるが、線の列だけなら、1080p (線の太さ 6 px、
// 列は 7 列まで) で Y と色差を合わせて約 12 KB を、戻す、覚える、重ねるの 3 回である。結果は、同じ絵に fill_dummy_tile で
// 線を重ねたものと同じになる。
// out の絵を描き直したとき (fill_dummy_base、背景の入れ替え) は、呼び出し元が strip を空 (DummyLineStrip{}) にする。
// 覚えた画素は前の絵のものなので、戻すと前の絵が混ざる。strip の大きさが width と height に合わないときは、戻さずに描く。
// width か height が 0 以下のとき、out が nullptr のとき、out_size が足りないときは、何も書かずに false を返す。
inline bool move_dummy_line(int width, int height, std::uint64_t elapsed_ns, std::uint64_t cycle_ns, std::uint8_t *out,
			    std::size_t out_size, DummyLineStrip &strip)
{
	const DummyNv12Layout layout = dummy_nv12_layout(width, height);
	if (out == nullptr || layout.width == 0 || out_size < layout.total_bytes())
		return false;
	std::uint8_t *uv = out + layout.y_bytes();
	const auto y_at = [&](int x, int y) {
		return out + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
	};
	const auto uv_at = [&](int cx, int cy) {
		return uv + static_cast<std::size_t>(cy) * layout.uv_stride() + static_cast<std::size_t>(cx) * 2u;
	};

	if (strip.last >= strip.first && strip.first >= 0 && strip.last < width) {
		const std::size_t y_cols = static_cast<std::size_t>(strip.last - strip.first + 1);
		const std::size_t uv_cols = static_cast<std::size_t>(strip.last / 2 - strip.first / 2 + 1) * 2u;
		if (strip.saved_y.size() == y_cols * static_cast<std::size_t>(height) &&
		    strip.saved_uv.size() == uv_cols * static_cast<std::size_t>(layout.chroma_height)) {
			for (int y = 0; y < height; ++y)
				std::memcpy(y_at(strip.first, y), strip.saved_y.data() + static_cast<std::size_t>(y) * y_cols, y_cols);
			for (int cy = 0; cy < layout.chroma_height; ++cy)
				std::memcpy(uv_at(strip.first / 2, cy), strip.saved_uv.data() + static_cast<std::size_t>(cy) * uv_cols,
					    uv_cols);
		}
	}

	const double left = dummy_line_left(width, height, elapsed_ns, cycle_ns);
	int first = 0;
	int last = -1;
	dummy_line_columns(width, height, left, &first, &last);
	strip.first = first;
	strip.last = last;
	if (last < first) {
		strip.saved_y.clear();
		strip.saved_uv.clear();
		return true;
	}
	const std::size_t y_cols = static_cast<std::size_t>(last - first + 1);
	const std::size_t uv_cols = static_cast<std::size_t>(last / 2 - first / 2 + 1) * 2u;
	strip.saved_y.resize(y_cols * static_cast<std::size_t>(height));
	strip.saved_uv.resize(uv_cols * static_cast<std::size_t>(layout.chroma_height));
	for (int y = 0; y < height; ++y)
		std::memcpy(strip.saved_y.data() + static_cast<std::size_t>(y) * y_cols, y_at(first, y), y_cols);
	for (int cy = 0; cy < layout.chroma_height; ++cy)
		std::memcpy(strip.saved_uv.data() + static_cast<std::size_t>(cy) * uv_cols, uv_at(first / 2, cy), uv_cols);
	detail::blend_dummy_line(layout, left, out);
	return true;
}

// ---- フレームを渡す間隔 ----

// ダミーのフレームを渡す予定 (dummy_frame_due が進める)。
struct DummyPacer {
	std::uint64_t next_ns = 0; // 次にフレームを渡す予定の時刻 (0 はまだ 1 つも渡していない)
};

// OBS の描画の刻みごとに、その刻みの時刻 now_ns で呼ぶ。この刻みでダミーのフレームを渡すなら true を返し、次の予定を
// 進める。
// - 予定は、前の予定から interval_ns ずつ進める。渡した時刻から数えると、刻みの時刻の揺れで予定が少しずつ後ろへずれ、
//   OBS のフレームレートがダミーと同じとき、揺れで少し早く来た刻みを飛ばして、1 つおきにしか渡さないことがある。
// - 予定より interval_ns の 4 分の 1 までは、早く来た刻みでも渡す (刻みの揺れを吸う)。半分まで許すと、ダミーの 2 倍の
//   フレームレートの OBS で、予定の半分前に来る刻みでも渡してしまう。
// - 予定より interval_ns 以上遅れたら (OBS のフレームレートがダミーより低いときと、描画が止まったとき)、この刻みから
//   数え直す (遅れを取り戻そうと、続けて渡さない)。時刻が予定より interval_ns の 2 倍を超えて戻ったときも数え直す。
// interval_ns が 0 なら、いつも渡す。
inline bool dummy_frame_due(DummyPacer &pacer, std::uint64_t now_ns, std::uint64_t interval_ns)
{
	if (interval_ns == 0)
		return true;
	if (pacer.next_ns != 0 && pacer.next_ns > now_ns + interval_ns * 2u)
		pacer.next_ns = 0;
	if (pacer.next_ns != 0 && now_ns + interval_ns / 4u < pacer.next_ns)
		return false;
	if (pacer.next_ns == 0 || now_ns >= pacer.next_ns + interval_ns)
		pacer.next_ns = now_ns + interval_ns;
	else
		pacer.next_ns += interval_ns;
	return true;
}

} // namespace cordmux::sources

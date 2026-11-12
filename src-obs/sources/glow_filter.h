// SPDX-License-Identifier: Apache-2.0

// 発言中のグローを作る、8 ビットのアルファ面の関数。円の範囲で膨らませる処理 (dilate_disk)、ぼかし (gaussian_blur)、
// 枠の内側に向かうグロー (inset_glow_alpha)、アルファの重ね合わせ (screen_alpha)。libobs にも Qt にも依存しない。
// 整数の重みだけで計算するので、同じ入力から同じ値を返し、描画した画像をスナップショットと完全一致で比べられる
// (inset_glow_alpha だけは std::erf を使うが、8 ビットに丸める前の値がちょうど丸めの境目に当たらない限り同じ値になる)。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cordmux::sources {

// 8 ビットのアルファ面。行は上から下、左から右の順に詰める。面の外は 0 として扱う。
struct AlphaPlane {
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> pixels;

	// (x, y) のピクセルの、pixels の中の位置。
	std::size_t offset(int x, int y) const
	{
		return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
	}
	std::uint8_t at(int x, int y) const { return pixels[offset(x, y)]; }
	std::uint8_t &at(int x, int y) { return pixels[offset(x, y)]; }
};

// 0 で埋めた面。幅か高さが 0 以下なら、幅と高さが 0 の空の面を返す。
inline AlphaPlane make_alpha_plane(int width, int height)
{
	AlphaPlane plane;
	if (width <= 0 || height <= 0)
		return plane;
	plane.width = width;
	plane.height = height;
	plane.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
	return plane;
}

namespace glow_detail {

// ガウス関数の重みの合計。重みは 16 ビットの固定小数点で持ち、合計をちょうどこの値にする。
inline constexpr std::int64_t kWeightOne = 65536;

// 1 行のうち、0 でない値がある範囲。全部 0 なら first が last より大きい。
struct Span {
	int first = 0;
	int last = -1;
	bool empty() const { return first > last; }
};

inline Span row_span(const AlphaPlane &plane, int y)
{
	Span span;
	span.first = plane.width;
	span.last = -1;
	for (int x = 0; x < plane.width; ++x) {
		if (plane.at(x, y) != 0) {
			span.first = std::min(span.first, x);
			span.last = x;
		}
	}
	return span;
}

} // namespace glow_detail

// ガウスぼかしの重み。中心と、中心から 1 つずつ離れた側の重みを、中心から順に返す (全体の長さは 2 * 返した長さ - 1)。
// 半径は標準偏差の 3 倍を切り上げた値 (3 倍の外の重みは合計の 0.3% 未満)。標準偏差が 0 以下なら、中心だけの
// {65536} を返す。重みは左右対称で、合計 (中心 + 2 * 残り) がちょうど 65536 になる。端数は中心に入れる。
inline std::vector<std::int32_t> gaussian_weights(double sigma)
{
	if (!(sigma > 0.0))
		return {static_cast<std::int32_t>(glow_detail::kWeightOne)};
	const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
	std::vector<double> raw(static_cast<std::size_t>(radius) + 1);
	double total = 0.0;
	for (int i = 0; i <= radius; ++i) {
		raw[static_cast<std::size_t>(i)] = std::exp(-static_cast<double>(i) * i / (2.0 * sigma * sigma));
		total += (i == 0 ? 1.0 : 2.0) * raw[static_cast<std::size_t>(i)];
	}
	std::vector<std::int32_t> weights(raw.size());
	std::int64_t sum = 0;
	for (int i = 1; i <= radius; ++i) {
		const double share = raw[static_cast<std::size_t>(i)] / total;
		const double scaled = static_cast<double>(glow_detail::kWeightOne) * share;
		const auto q = static_cast<std::int32_t>(std::llround(scaled));
		weights[static_cast<std::size_t>(i)] = q;
		sum += 2 * static_cast<std::int64_t>(q);
	}
	weights[0] = static_cast<std::int32_t>(glow_detail::kWeightOne - sum);
	return weights;
}

// 標準偏差 sigma (ピクセル) のガウスぼかし。縦横に分けて計算し、面の外は 0 として扱うので、グローが面の端で
// 切れないよう、呼び出し元が 3 * sigma 以上の余白を面に持たせる。sigma が 0 以下なら、そのままの複製を返す。
// 0 だけの行は計算を飛ばすので、文字のように一部の行にしか値がない面は速い。
inline AlphaPlane gaussian_blur(const AlphaPlane &src, double sigma)
{
	if (src.width <= 0 || src.height <= 0 || !(sigma > 0.0))
		return src;
	const std::vector<std::int32_t> w = gaussian_weights(sigma);
	const int radius = static_cast<int>(w.size()) - 1;
	const int width = src.width;
	const int height = src.height;

	// 横方向。結果は 8 ビットの値の 256 倍 (0 から 65280) で持つ。段階ごとに 8 ビットへ丸めると誤差がたまる。
	std::vector<std::uint16_t> mid(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
	std::vector<bool> row_has_value(static_cast<std::size_t>(height), false);
	for (int y = 0; y < height; ++y) {
		const glow_detail::Span span = glow_detail::row_span(src, y);
		if (span.empty())
			continue;
		row_has_value[static_cast<std::size_t>(y)] = true;
		const int x_begin = std::max(0, span.first - radius);
		const int x_end = std::min(width - 1, span.last + radius);
		for (int x = x_begin; x <= x_end; ++x) {
			std::int64_t acc = 0;
			const int k_begin = std::max(-radius, span.first - x);
			const int k_end = std::min(radius, span.last - x);
			for (int k = k_begin; k <= k_end; ++k) {
				const std::int64_t weight = w[static_cast<std::size_t>(k < 0 ? -k : k)];
				acc += weight * src.at(x + k, y);
			}
			mid[src.offset(x, y)] = static_cast<std::uint16_t>((acc + 128) >> 8);
		}
	}

	// 縦方向。重みの合計 65536 と 256 倍の分で、2^24 で割って 8 ビットに戻す。
	AlphaPlane out = make_alpha_plane(width, height);
	std::vector<std::int64_t> acc(static_cast<std::size_t>(width));
	for (int y = 0; y < height; ++y) {
		std::fill(acc.begin(), acc.end(), 0);
		bool any = false;
		const int k_begin = std::max(-radius, -y);
		const int k_end = std::min(radius, height - 1 - y);
		for (int k = k_begin; k <= k_end; ++k) {
			if (!row_has_value[static_cast<std::size_t>(y + k)])
				continue;
			any = true;
			const std::int64_t weight = w[static_cast<std::size_t>(k < 0 ? -k : k)];
			const std::uint16_t *row =
				&mid[static_cast<std::size_t>(y + k) * static_cast<std::size_t>(width)];
			for (int x = 0; x < width; ++x)
				acc[static_cast<std::size_t>(x)] += weight * row[x];
		}
		if (!any)
			continue;
		for (int x = 0; x < width; ++x)
			out.at(x, y) = static_cast<std::uint8_t>(
				std::min<std::int64_t>(255, (acc[static_cast<std::size_t>(x)] + (1 << 23)) >> 24));
	}
	return out;
}

namespace glow_detail {

// 整数の半径 n の円に近いピクセルの範囲を、行のずれ dy (0 から n) ごとの横の半分の幅で返す。範囲は、中心の差
// (dx, dy) が dx * dx + dy * dy <= n * n + k のピクセルで、k (0 から 2n) は、斜め 45 度の向きの広がり
// ((dx + dy) / sqrt(2) の最大) が n に最も近くなるものを選ぶ (同じ近さなら k が n に近いもの)。縦横の広がりは
// どの k でもちょうど n である。整数のピクセルの並びでは、斜めの広がりは 1 / sqrt(2) 刻みでしか変えられないので、
// n との差は 0.36 ピクセル以下に収まる (k を決めずに n * n とすると、斜めだけが 0.7 ピクセルほど狭くなる)。
inline std::vector<int> disk_half_widths(int n)
{
	std::vector<int> best(static_cast<std::size_t>(n) + 1, 0);
	double best_error = 1e9;
	int best_distance = 0;
	std::vector<int> half(static_cast<std::size_t>(n) + 1, 0);
	for (int k = 0; k <= 2 * n; ++k) {
		int diagonal = 0;
		for (int dy = 0; dy <= n; ++dy) {
			const int a = static_cast<int>(std::floor(std::sqrt(static_cast<double>(n * n + k - dy * dy))));
			half[static_cast<std::size_t>(dy)] = a;
			diagonal = std::max(diagonal, a + dy);
		}
		const double error = std::abs(diagonal / std::sqrt(2.0) - n);
		const int distance = k > n ? k - n : n - k;
		if (error < best_error - 1e-9 || (error < best_error + 1e-9 && distance < best_distance)) {
			best_error = error;
			best_distance = distance;
			best = half;
		}
	}
	return best;
}

// 整数の半径 n の円の範囲 (disk_half_widths) の最大値で膨らませる。値の最大を取るので、元の面の縁のなめらかさ
// (アンチエイリアスの中間の値) は、そのまま外側へ移る。面の外は 0。n が 0 以下なら、そのままの複製を返す。
//
// 行ごとに、横の範囲 [x - a, x + a] の最大値を a = 0 から n まで順に作り、行のずれ dy ごとに、円の幅の半分に当たる
// a の値を、上下の行の結果に重ねる。計算量は「値のある行の数 x 幅 x n」で、文字のように一部の行にしか値がない面は速い。
inline AlphaPlane dilate_disk_whole(const AlphaPlane &src, int n)
{
	if (src.width <= 0 || src.height <= 0 || n <= 0)
		return src;
	const int width = src.width;
	const int height = src.height;

	// 行のずれ dy (0 から n) ごとの、横の範囲の半分の幅。
	const std::vector<int> half = disk_half_widths(n);

	AlphaPlane out = make_alpha_plane(width, height);
	// window[a][x] は、元の行の [x - a, x + a] の最大値。計算した範囲 [x_begin, x_end] の外は 0 として扱う。
	std::vector<std::vector<std::uint8_t>> window(static_cast<std::size_t>(n) + 1,
						      std::vector<std::uint8_t>(static_cast<std::size_t>(width), 0));
	for (int y = 0; y < height; ++y) {
		const Span span = row_span(src, y);
		if (span.empty())
			continue;
		const int x_begin = std::max(0, span.first - n);
		const int x_end = std::min(width - 1, span.last + n);
		std::vector<std::uint8_t> &first = window[0];
		for (int x = x_begin; x <= x_end; ++x)
			first[static_cast<std::size_t>(x)] = src.at(x, y);
		for (int a = 1; a <= n; ++a) {
			const std::vector<std::uint8_t> &prev = window[static_cast<std::size_t>(a) - 1];
			std::vector<std::uint8_t> &cur = window[static_cast<std::size_t>(a)];
			for (int x = x_begin; x <= x_end; ++x) {
				std::uint8_t best = prev[static_cast<std::size_t>(x)];
				if (x > x_begin)
					best = std::max(best, prev[static_cast<std::size_t>(x) - 1]);
				if (x < x_end)
					best = std::max(best, prev[static_cast<std::size_t>(x) + 1]);
				cur[static_cast<std::size_t>(x)] = best;
			}
		}
		for (int dy = -n; dy <= n; ++dy) {
			const int oy = y + dy;
			if (oy < 0 || oy >= height)
				continue;
			const int a = half[static_cast<std::size_t>(dy < 0 ? -dy : dy)];
			const std::vector<std::uint8_t> &row = window[static_cast<std::size_t>(a)];
			for (int x = x_begin; x <= x_end; ++x) {
				std::uint8_t &dst = out.at(x, oy);
				dst = std::max(dst, row[static_cast<std::size_t>(x)]);
			}
		}
	}
	return out;
}

} // namespace glow_detail

// 円の範囲で膨らませる (半径 radius ピクセル。端数があってよい)。縁から外へ、どの向きにもほぼ同じ距離だけ広がり
// (縦横はちょうど radius、斜め 45 度は radius との差が 0.36 ピクセル以下)、角は丸く広がる。端数は、半径の整数部 n と
// n + 1 で膨らませた 2 つの面を、端数の割合で混ぜて表す (縦横の縁は、端数の分だけ外へ動く)。面の外は 0。radius が
// 0 以下なら、そのままの複製を返す。面の端で切れないよう、呼び出し元が radius + 1 以上の余白を面に持たせる。
inline AlphaPlane dilate_disk(const AlphaPlane &src, double radius)
{
	if (src.width <= 0 || src.height <= 0 || !(radius > 0.0))
		return src;
	int n = static_cast<int>(std::floor(radius));
	// 端数を 1/256 の単位にする。丸めて 256 になったら、次の整数の半径そのもの。
	int t = static_cast<int>(std::lround((radius - n) * 256.0));
	if (t >= 256) {
		++n;
		t = 0;
	}
	AlphaPlane low = glow_detail::dilate_disk_whole(src, n);
	if (t == 0)
		return low;
	const AlphaPlane high = glow_detail::dilate_disk_whole(src, n + 1);
	for (std::size_t i = 0; i < low.pixels.size(); ++i) {
		const int mixed = (low.pixels[i] * (256 - t) + high.pixels[i] * t + 128) >> 8;
		low.pixels[i] = static_cast<std::uint8_t>(mixed);
	}
	return low;
}

// 2 つのアルファを重ねた値 (a + b - a * b / 255)。どちらかが 0 なら、もう一方の値のまま。どちらかが 255 なら
// 255 で、順序によらず、255 を超えない。
inline std::uint8_t screen_alpha(std::uint8_t a, std::uint8_t b)
{
	return static_cast<std::uint8_t>(a + b - (static_cast<int>(a) * b + 127) / 255);
}

namespace glow_detail {

// 区間 [lo, hi] を標準偏差 sigma でぼかしたときの、位置 x (ピクセルの中心) での値。sigma が 0 以下ならぼかさない。
inline double blurred_interval(double x, double lo, double hi, double sigma)
{
	if (!(hi > lo))
		return 0.0;
	if (!(sigma > 0.0))
		return (x >= lo && x <= hi) ? 1.0 : 0.0;
	const double scale = sigma * std::sqrt(2.0);
	return 0.5 * (std::erf((x - lo) / scale) - std::erf((x - hi) / scale));
}

} // namespace glow_detail

// 幅 width、高さ height の枠の内側に向かうグローのアルファ。CSS の box-shadow の inset 0 0 blur spread と同じ形で、
// 枠の外側が光る元になり、広がり spread_px の分だけ内側へ入り、ぼかし blur_px (標準偏差はその半分) で内側へ薄れる。
// 長方形のガウスぼかしは縦横の積になるので、縦横それぞれの値を erf で求めて掛ける (全画面でも速く、ぼかしの範囲を
// 3 倍の標準偏差で切らない)。広がりが枠の半分以上なら、全体が最大の明るさになる。
inline AlphaPlane inset_glow_alpha(int width, int height, double blur_px, double spread_px)
{
	AlphaPlane out = make_alpha_plane(width, height);
	if (out.width == 0)
		return out;
	const double sigma = std::max(0.0, blur_px) / 2.0;
	const double spread = std::max(0.0, spread_px);

	std::vector<double> inside_x(static_cast<std::size_t>(width));
	std::vector<double> inside_y(static_cast<std::size_t>(height));
	for (int x = 0; x < width; ++x)
		inside_x[static_cast<std::size_t>(x)] =
			glow_detail::blurred_interval(x + 0.5, spread, width - spread, sigma);
	for (int y = 0; y < height; ++y)
		inside_y[static_cast<std::size_t>(y)] =
			glow_detail::blurred_interval(y + 0.5, spread, height - spread, sigma);

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const double inside_h = inside_x[static_cast<std::size_t>(x)];
			const double inside = inside_h * inside_y[static_cast<std::size_t>(y)];
			const double lit = std::clamp(1.0 - inside, 0.0, 1.0);
			out.at(x, y) = static_cast<std::uint8_t>(std::lround(255.0 * lit));
		}
	}
	return out;
}

} // namespace cordmux::sources

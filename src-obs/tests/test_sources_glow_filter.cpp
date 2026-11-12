// SPDX-License-Identifier: Apache-2.0

// グローのぼかし、円の範囲で膨らませる処理、枠の内側に向かうグロー (sources/glow_filter.h) のテスト。どれも整数の重みで
// 計算する純粋関数なので、値の性質 (対称、合計、範囲) を厳密に確認する。

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "sources/glow_filter.h"

using cordmux::sources::AlphaPlane;
using cordmux::sources::dilate_disk;
using cordmux::sources::gaussian_blur;
using cordmux::sources::gaussian_weights;
using cordmux::sources::inset_glow_alpha;
using cordmux::sources::make_alpha_plane;
using cordmux::sources::screen_alpha;

namespace {

long sum_of(const AlphaPlane &plane)
{
	return std::accumulate(plane.pixels.begin(), plane.pixels.end(), 0L);
}

// 中心に 255 の点が 1 つだけある size x size の面。
AlphaPlane impulse(int size)
{
	AlphaPlane plane = make_alpha_plane(size, size);
	plane.at(size / 2, size / 2) = 255;
	return plane;
}

} // namespace

TEST_CASE("ガウスぼかしの重みは左右対称で、合計がちょうど 65536 になり、中心から離れるほど小さい")
{
	for (const double sigma : {0.3, 1.0, 2.5, 7.75, 11.6, 40.0}) {
		CAPTURE(sigma);
		const std::vector<std::int32_t> w = gaussian_weights(sigma);
		REQUIRE(w.size() >= 2);
		long total = w[0];
		for (std::size_t i = 1; i < w.size(); ++i)
			total += 2L * w[i];
		CHECK(total == 65536);
		for (std::size_t i = 1; i < w.size(); ++i)
			CHECK(w[i] <= w[i - 1]);
		// 半径は標準偏差の 3 倍を切り上げた値。
		CHECK(static_cast<int>(w.size()) - 1 == std::max(1, static_cast<int>(std::ceil(sigma * 3.0))));
	}
	CHECK(gaussian_weights(0.0) == std::vector<std::int32_t>{65536});
	CHECK(gaussian_weights(-3.0) == std::vector<std::int32_t>{65536});
}

TEST_CASE("標準偏差が 0 以下のぼかしは、入力と同じ面を返す")
{
	AlphaPlane plane = impulse(9);
	plane.at(1, 2) = 77;
	CHECK(gaussian_blur(plane, 0.0).pixels == plane.pixels);
	CHECK(gaussian_blur(plane, -1.0).pixels == plane.pixels);
	CHECK(gaussian_blur(AlphaPlane{}, 3.0).pixels.empty());
}

TEST_CASE("点をぼかした面は、上下左右で対称で、中心が最大で、合計がほぼ保たれる")
{
	const int size = 41;
	const AlphaPlane out = gaussian_blur(impulse(size), 3.0);
	REQUIRE(out.width == size);
	REQUIRE(out.height == size);
	int max_value = 0;
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			CAPTURE(x);
			CAPTURE(y);
			CHECK(out.at(x, y) == out.at(size - 1 - x, y));
			CHECK(out.at(x, y) == out.at(x, size - 1 - y));
			max_value = std::max<int>(max_value, out.at(x, y));
		}
	}
	CHECK(out.at(size / 2, size / 2) == max_value);
	// 255 / (2 pi sigma^2) = 4.5。
	CHECK(max_value >= 4);
	CHECK(max_value <= 5);
	// 0.5 未満の裾は 0 に丸まるので合計は少し減り、0.5 から 1.5 の値が 1 に丸まるので少し増える。
	CHECK(sum_of(out) <= 255 + 30);
	CHECK(sum_of(out) >= 255 * 80 / 100);
}

TEST_CASE("一様な面をぼかしても、面の中は元の値のままで、面の端に近い所だけが薄れる")
{
	const int size = 60;
	AlphaPlane plane = make_alpha_plane(size, size);
	std::fill(plane.pixels.begin(), plane.pixels.end(), std::uint8_t{200});
	const AlphaPlane out = gaussian_blur(plane, 2.0); // 半径 6
	for (int y = 6; y < size - 6; ++y)
		for (int x = 6; x < size - 6; ++x)
			CHECK(out.at(x, y) == 200);
	// 面の外は 0 として扱うので、端の 1 ピクセルは薄れる。
	CHECK(out.at(0, size / 2) < 200);
	CHECK(out.at(0, 0) < out.at(0, size / 2));
	// 255 を超えない (面は 8 ビット)。
	for (const std::uint8_t v : out.pixels)
		CHECK(v <= 200);
}

TEST_CASE("段差をぼかすと、段差の手前から値が増え、左から右へ増えるだけで戻らない")
{
	const int width = 80;
	AlphaPlane plane = make_alpha_plane(width, 3);
	for (int y = 0; y < 3; ++y)
		for (int x = width / 2; x < width; ++x)
			plane.at(x, y) = 255;
	const AlphaPlane out = gaussian_blur(plane, 4.0);
	// 縦には一様なので、縦にぼかすと中央の行は、面の外の 0 の影響で薄れる。横方向の形だけを中央の行で見る。
	const int y = 1;
	CHECK(out.at(width / 2 - 1, y) < out.at(width / 2, y));
	for (int x = 1; x < width - 20; ++x)
		CHECK(out.at(x, y) >= out.at(x - 1, y));
}

TEST_CASE("円の範囲で膨らませると、点は中心から半径 + 0.5 以内のピクセルに広がり、範囲の外は 0 のまま")
{
	AlphaPlane plane = make_alpha_plane(21, 21);
	plane.at(10, 10) = 200;
	const AlphaPlane out = dilate_disk(plane, 4.0);
	for (int y = 0; y < 21; ++y) {
		for (int x = 0; x < 21; ++x) {
			CAPTURE(x);
			CAPTURE(y);
			const int dx = x - 10;
			const int dy = y - 10;
			const bool inside = dx * dx + dy * dy <= 4.5 * 4.5;
			CHECK(out.at(x, y) == (inside ? 200 : 0));
		}
	}
	// 縦横には半径ちょうど、斜めには半径との差が 0.5 ピクセル以内に広がる (正方形なら斜めは半径の 1.41 倍)。
	CHECK(out.at(14, 10) == 200);
	CHECK(out.at(15, 10) == 0);
	CHECK(out.at(13, 13) == 200);  // 中心から 4.24
	CHECK(out.at(14, 13) == 0);    // 中心から 5.0
	CHECK(out.at(14, 14) == 0);    // 中心から 5.66 (正方形なら 255 の範囲)
	CHECK(dilate_disk(plane, 0.0).pixels == plane.pixels);
	CHECK(dilate_disk(plane, -2.0).pixels == plane.pixels);
}

TEST_CASE("円の範囲は、どの半径でも縦横の広がりがちょうど半径で、斜め 45 度の広がりと半径の差が 0.36 ピクセル以下")
{
	for (int n = 1; n <= 150; ++n) {
		CAPTURE(n);
		const std::vector<int> half = cordmux::sources::glow_detail::disk_half_widths(n);
		REQUIRE(half.size() == static_cast<std::size_t>(n) + 1);
		CHECK(half[0] == n);
		int diagonal = 0;
		for (int dy = 0; dy <= n; ++dy) {
			diagonal = std::max(diagonal, half[static_cast<std::size_t>(dy)] + dy);
			// 行が中心から離れるほど、幅は狭くなるか同じ。
			if (dy > 0)
				CHECK(half[static_cast<std::size_t>(dy)] <= half[static_cast<std::size_t>(dy) - 1]);
		}
		CHECK(std::abs(diagonal / std::sqrt(2.0) - n) <= 0.36);
	}
}

TEST_CASE("円の範囲で膨らませると、縁のなめらかな中間の値は形を保ったまま外へ移り、重なる所は大きいほうが残る")
{
	// 縦の縁 (x = 5 までが 255、x = 6 が 100) を 3 ピクセル膨らませる。
	AlphaPlane plane = make_alpha_plane(20, 9);
	for (int y = 0; y < 9; ++y) {
		for (int x = 0; x <= 5; ++x)
			plane.at(x, y) = 255;
		plane.at(6, y) = 100;
	}
	const AlphaPlane out = dilate_disk(plane, 3.0);
	CHECK(out.at(8, 4) == 255);
	CHECK(out.at(9, 4) == 100);
	CHECK(out.at(10, 4) == 0);
}

TEST_CASE("円の範囲で膨らませる半径の端数は、縦横の縁を端数の分だけ外へ動かす")
{
	AlphaPlane plane = make_alpha_plane(20, 9);
	for (int y = 0; y < 9; ++y)
		for (int x = 0; x <= 5; ++x)
			plane.at(x, y) = 255;
	// 半径 2.5 では、2 ピクセル先までが 255、3 ピクセル先が半分。
	const AlphaPlane half = dilate_disk(plane, 2.5);
	CHECK(half.at(7, 4) == 255);
	CHECK(half.at(8, 4) == 128);
	CHECK(half.at(9, 4) == 0);
	// 端数の割合に比例する。
	const AlphaPlane quarter = dilate_disk(plane, 2.25);
	CHECK(quarter.at(8, 4) == 64);
}

TEST_CASE("円の範囲で膨らませる処理は面の端で切れても落ちず、端の外は 0 として扱う")
{
	AlphaPlane plane = make_alpha_plane(5, 4);
	plane.at(0, 0) = 255;
	const AlphaPlane out = dilate_disk(plane, 3.0);
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 5; ++x)
			CHECK(out.at(x, y) == ((x * x + y * y <= 3.5 * 3.5) ? 255 : 0));
}

TEST_CASE("アルファの重ね合わせは、0 を重ねても変わらず、255 を重ねると 255 で、順序によらず、255 を超えない")
{
	for (int a = 0; a < 256; ++a) {
		CHECK(screen_alpha(static_cast<std::uint8_t>(a), 0) == a);
		CHECK(screen_alpha(0, static_cast<std::uint8_t>(a)) == a);
		CHECK(screen_alpha(static_cast<std::uint8_t>(a), 255) == 255);
		for (int b = 0; b < 256; b += 5) {
			const auto ua = static_cast<std::uint8_t>(a);
			const auto ub = static_cast<std::uint8_t>(b);
			const std::uint8_t ab = screen_alpha(ua, ub);
			CHECK(ab == screen_alpha(ub, ua));
			CHECK(ab >= std::max(a, b));
		}
	}
}

TEST_CASE("枠の内側のグロー: ぼかしなしでは、広がりの幅の帯だけが 255 になる")
{
	const AlphaPlane out = inset_glow_alpha(20, 12, 0.0, 3.0);
	for (int y = 0; y < 12; ++y) {
		for (int x = 0; x < 20; ++x) {
			CAPTURE(x);
			CAPTURE(y);
			const bool band = x < 3 || x >= 17 || y < 3 || y >= 9;
			CHECK(out.at(x, y) == (band ? 255 : 0));
		}
	}
	// 広がりもぼかしも 0 なら、枠の内側は光らない。
	for (const std::uint8_t v : inset_glow_alpha(20, 12, 0.0, 0.0).pixels)
		CHECK(v == 0);
}

TEST_CASE("枠の内側のグロー: 広がりが枠の半分以上なら全体が 255 になり、大きさが 0 以下なら空")
{
	for (const std::uint8_t v : inset_glow_alpha(20, 12, 4.0, 6.0).pixels)
		CHECK(v == 255);
	CHECK(inset_glow_alpha(0, 12, 4.0, 1.0).pixels.empty());
	CHECK(inset_glow_alpha(20, -1, 4.0, 1.0).pixels.empty());
}

TEST_CASE("枠の内側のグロー: 上下左右で対称で、端が最も明るく、内側へ減るだけで、中心はほぼ 0")
{
	const int w = 160;
	const int h = 90;
	const AlphaPlane out = inset_glow_alpha(w, h, 16.0, 2.0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			CAPTURE(x);
			CAPTURE(y);
			CHECK(out.at(x, y) == out.at(w - 1 - x, y));
			CHECK(out.at(x, y) == out.at(x, h - 1 - y));
		}
	}
	// 中央の行を左端から中心へ見る: 減るだけ。
	for (int x = 1; x < w / 2; ++x)
		CHECK(out.at(x, h / 2) <= out.at(x - 1, h / 2));
	CHECK(out.at(0, h / 2) > 120);
	CHECK(out.at(w / 2, h / 2) == 0);
	// 角は 2 辺から光が来るので、辺の途中より明るい。
	CHECK(out.at(0, 0) >= out.at(0, h / 2));
}

TEST_CASE("枠の内側のグロー: 縦横の積で求めた値は、枠の外側を光らせて 2 次元でぼかした面と、数段階の違いに収まる")
{
	// 比べる方法: 枠の外側 (周りの余白を含む) を 255、広がりを引いた内側の矩形を 0 にした大きい面を、2 次元で
	// ぼかして、枠の中を切り取る。
	const int w = 40;
	const int h = 30;
	const double blur = 8.0;
	const double spread = 2.0;
	const double sigma = blur / 2.0;
	const int pad = static_cast<int>(std::ceil(sigma * 3.0)) + 2;
	AlphaPlane big = make_alpha_plane(w + pad * 2, h + pad * 2);
	std::fill(big.pixels.begin(), big.pixels.end(), std::uint8_t{255});
	for (int y = pad + 2; y < pad + h - 2; ++y)
		for (int x = pad + 2; x < pad + w - 2; ++x)
			big.at(x, y) = 0;
	const AlphaPlane blurred = gaussian_blur(big, sigma);
	const AlphaPlane fast = inset_glow_alpha(w, h, blur, spread);
	int worst = 0;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			worst = std::max(worst, std::abs(int{blurred.at(x + pad, y + pad)} - int{fast.at(x, y)}));
	CHECK(worst <= 4);
}

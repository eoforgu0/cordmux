// SPDX-License-Identifier: Apache-2.0

// sources/dummy_tiles.h (ダミー表示の色のタイルと、動く線と、フレームを渡す間隔) のテスト。枠ごとの色、NV12 への変換、線の太さ、
// 時刻から求める位相 (フレームレートによらない)、1 フレームの動きが小さいこと (点滅にならないこと)、背景の画像のコピー、境界
// (大きさが 0 以下、書き込む領域が足りない)、話している人として光らせる枠の回し方、線の列だけを描き直したフレームが
// 最初から作ったフレームと同じになること、OBS の描画の刻みに対してフレームを渡す間隔を確認する。

#include "doctest.h"

#include "sources/dummy_tiles.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <utility>
#include <vector>

using cordmux::sources::dummy_frame_due;
using cordmux::sources::dummy_nv12_from_bgra;
using cordmux::sources::dummy_nv12_layout;
using cordmux::sources::dummy_yuv_of_argb;
using cordmux::sources::dummy_yuv_of_rgb;
using cordmux::sources::DummyNv12Layout;
using cordmux::sources::DummyYuv;
using cordmux::sources::dummy_line_left;
using cordmux::sources::dummy_speaker_slot;
using cordmux::sources::dummy_line_width;
using cordmux::sources::dummy_tile_color;
using cordmux::sources::DummyLineStrip;
using cordmux::sources::DummyPacer;
using cordmux::sources::fill_dummy_base;
using cordmux::sources::fill_dummy_tile;
using cordmux::sources::kDefaultDummyCycleNs;
using cordmux::sources::kDummyFps;
using cordmux::sources::move_dummy_line;
using cordmux::sources::kDummyMarkColor;
using cordmux::sources::kDummyTileColorCount;

namespace {

constexpr std::uint64_t kMs = 1'000'000ULL;

// YUV の 1 組を 0x00YYUUVV にまとめる (比べやすくするため)。
std::uint32_t packed(const DummyYuv &c)
{
	return (std::uint32_t(c.y) << 16) | (std::uint32_t(c.u) << 8) | std::uint32_t(c.v);
}

// 色 (0xAARRGGBB) の YUV を 0x00YYUUVV で。
std::uint32_t yuv_of(std::uint32_t argb)
{
	return packed(dummy_yuv_of_argb(argb));
}

// NV12 で書かれた画素 (x, y) の Y と、その画素の組の U と V を、0x00YYUUVV で返す。
std::uint32_t yuv_at(const std::vector<std::uint8_t> &pixels, int width, int height, int x, int y)
{
	const DummyNv12Layout layout = dummy_nv12_layout(width, height);
	const std::uint32_t luma = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
	const std::size_t uv = layout.y_bytes() + static_cast<std::size_t>(y / 2) * layout.uv_stride() + static_cast<std::size_t>(x / 2) * 2u;
	return (luma << 16) | (std::uint32_t(pixels[uv]) << 8) | std::uint32_t(pixels[uv + 1]);
}

// 画素 (x, y) の Y だけ。
int luma_at(const std::vector<std::uint8_t> &pixels, int width, int x, int y)
{
	return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
}

// 線 (白) がすべて覆っている x の一覧 (1 行目の Y が白の Y)。
std::set<int> marked_columns(const std::vector<std::uint8_t> &pixels, int width)
{
	const int white = dummy_yuv_of_argb(kDummyMarkColor).y;
	std::set<int> columns;
	for (int x = 0; x < width; ++x) {
		if (luma_at(pixels, width, x, 0) == white)
			columns.insert(x);
	}
	return columns;
}

// width x height の NV12 のフレームを入れる領域 (大きさが 0 以下なら空)。
std::vector<std::uint8_t> make_buffer(int width, int height)
{
	return std::vector<std::uint8_t>(dummy_nv12_layout(width, height).total_bytes(), 0u);
}

bool fill(int slot, int width, int height, std::uint64_t elapsed_ns, std::vector<std::uint8_t> &pixels)
{
	return fill_dummy_tile(slot, width, height, elapsed_ns, kDefaultDummyCycleNs, nullptr, 0u, pixels.data(),
			       pixels.size());
}

} // namespace

TEST_CASE("枠ごとに背景の色が違う")
{
	std::set<std::uint32_t> colors;
	for (int slot = 1; slot <= 4; ++slot)
		colors.insert(dummy_tile_color(slot));
	CHECK(colors.size() == static_cast<std::size_t>(kDummyTileColorCount));
	for (const std::uint32_t color : colors)
		CHECK(color != kDummyMarkColor); // 線が背景に溶け込まない

	// 実際に描画した画素でも、枠ごとに違う (線から外れた右端で見る。時刻 0 の線は左端にある)。
	const int width = 360;
	const int height = 36;
	std::set<std::uint32_t> painted;
	for (int slot = 1; slot <= 4; ++slot) {
		std::vector<std::uint8_t> pixels = make_buffer(width, height);
		REQUIRE(fill(slot, width, height, 0u, pixels));
		CHECK(yuv_at(pixels, width, height, width - 1, height - 1) == yuv_of(dummy_tile_color(slot)));
		painted.insert(yuv_at(pixels, width, height, width - 1, height - 1));
	}
	CHECK(painted.size() == static_cast<std::size_t>(kDummyTileColorCount));

	// 範囲の外の番号でも色は決まる (巡回する)。
	CHECK(dummy_tile_color(5) == dummy_tile_color(1));
	CHECK(dummy_tile_color(99) == dummy_tile_color(3));
	CHECK(dummy_tile_color(0) == dummy_tile_color(4));
	CHECK(dummy_tile_color(-1) == dummy_tile_color(3));
}

TEST_CASE("線の太さは高さの 180 分の 1 で、2 px より細くしない")
{
	CHECK(dummy_line_width(1080) == 6);
	CHECK(dummy_line_width(720) == 4);
	CHECK(dummy_line_width(360) == 2);
	CHECK(dummy_line_width(100) == 2);

	std::vector<std::uint8_t> pixels = make_buffer(1920, 1080);
	REQUIRE(fill(1, 1920, 1080, 0u, pixels));
	CHECK(marked_columns(pixels, 1920) == std::set<int>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("線は三角波で左右に動き、1 往復は 4 秒で、位相は時刻から求める")
{
	const int width = 1920;
	const int height = 1080;
	const double travel = width - 6.0;
	CHECK(dummy_line_left(width, height, 0u, kDefaultDummyCycleNs) == doctest::Approx(0.0));
	CHECK(dummy_line_left(width, height, 1'000 * kMs, kDefaultDummyCycleNs) == doctest::Approx(travel * 0.5));
	CHECK(dummy_line_left(width, height, 2'000 * kMs, kDefaultDummyCycleNs) == doctest::Approx(travel));
	CHECK(dummy_line_left(width, height, 3'000 * kMs, kDefaultDummyCycleNs) == doctest::Approx(travel * 0.5));
	CHECK(dummy_line_left(width, height, 4'000 * kMs, kDefaultDummyCycleNs) == doctest::Approx(0.0));

	// フレームレートによらず、同じ時刻なら同じ位置 (30 FPS の 15 フレーム目と 60 FPS の 30 フレーム目)。
	const std::uint64_t at30 = 15ULL * 1'000'000'000ULL / 30ULL;
	const std::uint64_t at60 = 30ULL * 1'000'000'000ULL / 60ULL;
	CHECK(dummy_line_left(width, height, at30, kDefaultDummyCycleNs) ==
	      doctest::Approx(dummy_line_left(width, height, at60, kDefaultDummyCycleNs)));

	// 1 往復を 0 にすると左端に止まる (0 で割らない)。
	CHECK(dummy_line_left(width, height, 1'234 * kMs, 0u) == doctest::Approx(0.0));
}

TEST_CASE("1 フレームで線が動く幅は小さく、同じ場所が明暗で何度も入れ替わらない")
{
	// ダミーのフレームレート (60 fps) の隣り合う 2 フレームで、線の左端の動きは、1 往復 4 秒、幅 1920 で約 16 ピクセル
	// (線の太さ 6 の数倍で、線がとびとびに現れるのではなく、続けて動いて見える量)。
	CHECK(kDummyFps == 60);
	const int width = 1920;
	const int height = 1080;
	double previous = dummy_line_left(width, height, 0u, kDefaultDummyCycleNs);
	double max_step = 0.0;
	for (int frame = 1; frame <= 240; ++frame) {
		const std::uint64_t t =
			static_cast<std::uint64_t>(frame) * 1'000'000'000ULL / static_cast<std::uint64_t>(kDummyFps);
		const double now = dummy_line_left(width, height, t, kDefaultDummyCycleNs);
		max_step = std::max(max_step, std::fabs(now - previous));
		previous = now;
	}
	CHECK(max_step < 17.0);

	// ある列が白になり始める回数を、ずらしながら取ったどの 1 秒の区間でも数える (区切りを固定しない)。線は
	// 1 往復 4 秒なので、どの列も白になり始めるのは 1 秒に 1 回で、折り返しの近くの列だけは、行きと帰りが同じ
	// 1 秒に入ることがあるので 2 回までになる (WCAG 2.3.1 の 1 秒に 3 回を下回る)。
	for (const int column : {0, 3, 960, 1500, 1914, 1919}) {
		std::vector<int> starts; // 白になり始めたフレーム (60 FPS で 8 秒)
		bool lit = false;
		for (int frame = 0; frame < 480; ++frame) {
			const std::uint64_t t = static_cast<std::uint64_t>(frame) * 1'000'000'000ULL / 60ULL;
			const double left = dummy_line_left(width, height, t, kDefaultDummyCycleNs);
			const bool now = column + 1 > left && column < left + dummy_line_width(height);
			if (now && !lit)
				starts.push_back(frame);
			lit = now;
		}
		CAPTURE(column);
		REQUIRE_FALSE(starts.empty());
		int worst = 0;
		for (std::size_t i = 0; i < starts.size(); ++i) {
			int in_window = 0;
			for (std::size_t j = i; j < starts.size() && starts[j] < starts[i] + 60; ++j)
				++in_window;
			worst = std::max(worst, in_window);
		}
		CHECK(worst <= 2);
	}
}

TEST_CASE("線の端のピクセルには、線が覆う幅の割合で白を混ぜる")
{
	const int width = 360;
	const int height = 360; // 線の太さ 2
	std::vector<std::uint8_t> pixels = make_buffer(width, height);
	// 左端が 10.5 になる時刻 (travel = 358、tri = 10.5 / 358)。
	const double tri = 10.5 / 358.0;
	const std::uint64_t t = static_cast<std::uint64_t>(std::llround(tri / 2.0 * static_cast<double>(kDefaultDummyCycleNs)));
	REQUIRE(fill(1, width, height, t, pixels));
	// Y で見る (色差は 2 列の組で混ぜるので、列ごとには決まらない)。
	const int base = dummy_yuv_of_argb(dummy_tile_color(1)).y;
	const int white = dummy_yuv_of_argb(kDummyMarkColor).y;
	CHECK(luma_at(pixels, width, 9, 0) == base);
	CHECK(luma_at(pixels, width, 11, 0) == white);
	const int half_left = luma_at(pixels, width, 10, 0);
	const int half_right = luma_at(pixels, width, 12, 0);
	CHECK(half_left > base);
	CHECK(half_left < white);
	CHECK(half_right > base);
	CHECK(half_right < white);
	CHECK(luma_at(pixels, width, 13, 0) == base);
	// 色差は、線が全く掛からない組では枠の色のまま、線が掛かる組では白 (U と V が 128) の方へ寄る。
	const DummyYuv base_yuv = dummy_yuv_of_argb(dummy_tile_color(1));
	CHECK(yuv_at(pixels, width, height, 8, 0) == packed(base_yuv));
	const std::uint32_t mixed = yuv_at(pixels, width, height, 10, 0);
	CHECK(std::abs(int((mixed >> 8) & 0xFFu) - 128) < std::abs(int(base_yuv.u) - 128));
	CHECK(std::abs(int(mixed & 0xFFu) - 128) < std::abs(int(base_yuv.v) - 128));
}

TEST_CASE("背景の画像があればコピーし、その上に線を重ねる")
{
	const int width = 64;
	const int height = 36;
	std::vector<std::uint8_t> background = make_buffer(width, height);
	const DummyNv12Layout layout = dummy_nv12_layout(width, height);
	std::fill(background.begin(), background.begin() + static_cast<std::ptrdiff_t>(layout.y_bytes()), std::uint8_t{0x40});
	for (std::size_t i = layout.y_bytes(); i < background.size(); i += 2u) {
		background[i] = 0x50u;
		background[i + 1] = 0x60u;
	}
	std::vector<std::uint8_t> pixels = make_buffer(width, height);
	REQUIRE(fill_dummy_tile(2, width, height, 0u, kDefaultDummyCycleNs, background.data(), background.size(),
				pixels.data(), pixels.size()));
	CHECK(yuv_at(pixels, width, height, 40, 20) == 0x405060u);
	CHECK(yuv_at(pixels, width, height, 0, 20) == yuv_of(kDummyMarkColor));

	// 大きさの違う背景の画像は使わない (単色になる)。
	REQUIRE(fill_dummy_tile(2, width, height, 0u, kDefaultDummyCycleNs, background.data(), background.size() - 1u,
				pixels.data(), pixels.size()));
	CHECK(yuv_at(pixels, width, height, 40, 20) == yuv_of(dummy_tile_color(2)));
}

TEST_CASE("YUV への変換は、OBS が NV12 を RGB に戻す式 (BT.709 のフルレンジ) で元の色に戻る")
{
	// OBS の式 (libobs の video-matrices.c の initialize_matrix を、フルレンジの 8 ビットで書いたもの)。
	constexpr double kKr = 0.2126;
	constexpr double kKb = 0.0722;
	constexpr double kKg = 1.0 - kKr - kKb;
	const auto back_to_rgb = [&](const DummyYuv &c, double *r, double *g, double *b) {
		const double y = c.y;
		const double u = c.u - 128.0;
		const double v = c.v - 128.0;
		*r = y + 2.0 * (1.0 - kKr) * v;
		*g = y - 2.0 * (1.0 - kKb) * kKb / kKg * u - 2.0 * (1.0 - kKr) * kKr / kKg * v;
		*b = y + 2.0 * (1.0 - kKb) * u;
	};
	std::vector<std::uint32_t> colors{kDummyMarkColor, 0xFF000000u, 0xFF808080u, 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu};
	for (int slot = 1; slot <= kDummyTileColorCount; ++slot)
		colors.push_back(dummy_tile_color(slot));
	for (const std::uint32_t argb : colors) {
		CAPTURE(argb);
		double r = 0.0;
		double g = 0.0;
		double b = 0.0;
		back_to_rgb(dummy_yuv_of_argb(argb), &r, &g, &b);
		// 8 ビットに丸めた分の差 (U と V の 0.5 の誤差に係数を掛けた分) まで許す。
		CHECK(std::fabs(r - double((argb >> 16) & 0xFFu)) <= 1.5);
		CHECK(std::fabs(g - double((argb >> 8) & 0xFFu)) <= 1.5);
		CHECK(std::fabs(b - double(argb & 0xFFu)) <= 1.5);
	}
	// 白は Y が 255、U と V が 128 (色がない)。
	CHECK(yuv_of(kDummyMarkColor) == 0xFF8080u);
}

TEST_CASE("BGRA の画像を NV12 にすると、Y は画素ごと、色差は 2 x 2 の組 (端は残りの画素) の RGB の平均から決まる")
{
	// 3 x 3 (色差の組は 2 x 2。右と下の組は 1 列か 1 行)。
	const int width = 3;
	const int height = 3;
	const std::uint32_t argb[3][3] = {
		{0xFFFF0000u, 0xFF0000FFu, 0xFF00FF00u},
		{0xFF000000u, 0xFFFFFFFFu, 0xFF00FF00u},
		{0xFF808080u, 0xFF808080u, 0xFF102030u},
	};
	std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4u);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			std::uint8_t *p = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
			p[0] = std::uint8_t(argb[y][x] & 0xFFu);
			p[1] = std::uint8_t((argb[y][x] >> 8) & 0xFFu);
			p[2] = std::uint8_t((argb[y][x] >> 16) & 0xFFu);
			p[3] = 0xFFu;
		}
	}
	std::vector<std::uint8_t> nv12 = make_buffer(width, height);
	REQUIRE(nv12.size() == 9u + 2u * 2u * 2u);
	REQUIRE(dummy_nv12_from_bgra(bgra.data(), bgra.size(), width, height, nv12.data(), nv12.size()));
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x)
			CHECK(luma_at(nv12, width, x, y) == dummy_yuv_of_argb(argb[y][x]).y);
	}
	const auto average = [&](std::initializer_list<std::pair<int, int>> at) {
		double r = 0.0;
		double g = 0.0;
		double b = 0.0;
		for (const auto &[x, y] : at) {
			r += double((argb[y][x] >> 16) & 0xFFu);
			g += double((argb[y][x] >> 8) & 0xFFu);
			b += double(argb[y][x] & 0xFFu);
		}
		const double n = double(at.size());
		const DummyYuv c = dummy_yuv_of_rgb(r / n, g / n, b / n);
		return (std::uint32_t(c.u) << 8) | std::uint32_t(c.v);
	};
	const auto chroma_at = [&](int x, int y) { return yuv_at(nv12, width, height, x, y) & 0xFFFFu; };
	CHECK(chroma_at(0, 0) == average({{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
	CHECK(chroma_at(2, 0) == average({{2, 0}, {2, 1}}));
	CHECK(chroma_at(0, 2) == average({{0, 2}, {1, 2}}));
	CHECK(chroma_at(2, 2) == average({{2, 2}}));

	// 大きさの合わない画像と、足りない領域には書かない。
	CHECK_FALSE(dummy_nv12_from_bgra(bgra.data(), bgra.size() - 4u, width, height, nv12.data(), nv12.size()));
	CHECK_FALSE(dummy_nv12_from_bgra(bgra.data(), bgra.size(), width, height, nv12.data(), nv12.size() - 1u));
	CHECK_FALSE(dummy_nv12_from_bgra(nullptr, bgra.size(), width, height, nv12.data(), nv12.size()));
	CHECK_FALSE(dummy_nv12_from_bgra(bgra.data(), bgra.size(), 0, height, nv12.data(), nv12.size()));
}

TEST_CASE("同じ色が続く画像でも、NV12 の値は 1 画素ずつ (色差は 1 組ずつ) 計算した値と同じになる")
{
	// 変換は、直前の画素や組と同じ色なら計算した値を使い回す。単色の帯の中に違う色の画素を置き、前の色に戻る所、行の
	// 変わり目、右と下の端の組 (奇数の幅と高さ) で、使い回しが値を取り違えないかを見る。
	const int width = 7;
	const int height = 5;
	const std::uint32_t base = 0xFF7A2E2Eu;
	std::vector<std::uint32_t> argb(static_cast<std::size_t>(width) * height, base);
	argb[3] = 0xFFFFFFFFu;                           // 1 行目の途中に白を 1 つ
	argb[static_cast<std::size_t>(width) + 0] = 0xFF102030u; // 2 行目の先頭 (前の行の最後と違う色)
	argb[static_cast<std::size_t>(width) * 2 + 6] = 0xFFFFFFFFu; // 3 行目の右端
	argb[static_cast<std::size_t>(width) * 4 + 5] = 0xFF808080u; // 下の端の組
	std::vector<std::uint8_t> bgra(argb.size() * 4u);
	for (std::size_t i = 0; i < argb.size(); ++i) {
		bgra[i * 4u + 0u] = std::uint8_t(argb[i] & 0xFFu);
		bgra[i * 4u + 1u] = std::uint8_t((argb[i] >> 8) & 0xFFu);
		bgra[i * 4u + 2u] = std::uint8_t((argb[i] >> 16) & 0xFFu);
		bgra[i * 4u + 3u] = 0xFFu;
	}
	std::vector<std::uint8_t> nv12 = make_buffer(width, height);
	REQUIRE(dummy_nv12_from_bgra(bgra.data(), bgra.size(), width, height, nv12.data(), nv12.size()));
	const auto at = [&](int x, int y) { return argb[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)]; };
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x)
			CHECK(luma_at(nv12, width, x, y) == dummy_yuv_of_argb(at(x, y)).y);
	}
	for (int cy = 0; cy < (height + 1) / 2; ++cy) {
		for (int cx = 0; cx < (width + 1) / 2; ++cx) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			int n = 0;
			for (int y = cy * 2; y < std::min(height, cy * 2 + 2); ++y) {
				for (int x = cx * 2; x < std::min(width, cx * 2 + 2); ++x) {
					r += double((at(x, y) >> 16) & 0xFFu);
					g += double((at(x, y) >> 8) & 0xFFu);
					b += double(at(x, y) & 0xFFu);
					++n;
				}
			}
			const DummyYuv c = dummy_yuv_of_rgb(r / n, g / n, b / n);
			CHECK((yuv_at(nv12, width, height, cx * 2, cy * 2) & 0xFFFFu) ==
			      ((std::uint32_t(c.u) << 8) | std::uint32_t(c.v)));
		}
	}
}

TEST_CASE("大きさが 0 以下のときと、書き込む領域が足りないときは、何も書かずに false を返す")
{
	const int width = 16;
	const int height = 8;
	std::vector<std::uint8_t> pixels = make_buffer(width, height);
	CHECK_FALSE(fill(1, 0, height, 0u, pixels));
	CHECK_FALSE(fill(1, width, 0, 0u, pixels));
	CHECK_FALSE(fill(1, -8, height, 0u, pixels));
	CHECK_FALSE(fill(1, width, -2, 0u, pixels));
	CHECK_FALSE(fill_dummy_tile(1, width, height, 0u, kDefaultDummyCycleNs, nullptr, 0u, nullptr, pixels.size()));
	CHECK_FALSE(fill_dummy_tile(1, width, height, 0u, kDefaultDummyCycleNs, nullptr, 0u, pixels.data(),
				    pixels.size() - 1u));
	for (const std::uint8_t byte : pixels)
		CHECK(byte == 0u);
	CHECK(fill(1, width, height, 0u, pixels));
}

TEST_CASE("話している人として光らせる枠は、枠の番号の順に、1 周の時間を等分して回る")
{
	const std::uint64_t cycle = kDefaultDummyCycleNs; // 4 秒。枠 4 つなので 1 つの枠は 1 秒
	CHECK(dummy_speaker_slot(0, cycle, 4) == 1);
	CHECK(dummy_speaker_slot(999 * kMs, cycle, 4) == 1);
	CHECK(dummy_speaker_slot(1000 * kMs, cycle, 4) == 2);
	CHECK(dummy_speaker_slot(2500 * kMs, cycle, 4) == 3);
	CHECK(dummy_speaker_slot(3999 * kMs, cycle, 4) == 4);
	// 1 周したら最初の枠に戻る。
	CHECK(dummy_speaker_slot(4000 * kMs, cycle, 4) == 1);
	CHECK(dummy_speaker_slot(4000 * kMs * 25 + 1500 * kMs, cycle, 4) == 2);
	// どの時刻でも、1 から枠の数の間に収まる。
	for (std::uint64_t t = 0; t < 20'000 * kMs; t += 37 * kMs) {
		const int slot = dummy_speaker_slot(t, cycle, 4);
		CHECK(slot >= 1);
		CHECK(slot <= 4);
	}
}

TEST_CASE("光らせる枠は、1 周の時間か枠の数が 0 以下のときは、枠 1 に止める")
{
	CHECK(dummy_speaker_slot(5000 * kMs, 0u, 4) == 1);
	CHECK(dummy_speaker_slot(5000 * kMs, kDefaultDummyCycleNs, 0) == 1);
	CHECK(dummy_speaker_slot(5000 * kMs, kDefaultDummyCycleNs, -3) == 1);
	// 枠の数に対して 1 周の時間が短すぎる (1 つの枠の時間が 0 ナノ秒になる) ときも止める。
	CHECK(dummy_speaker_slot(5000 * kMs, 3u, 4) == 1);
}

namespace {

// 位置で色の変わる背景 (NV12。戻す画素を取り違えると、別の画素の値になる)。BGRA で描いてから dummy_nv12_from_bgra で変える
// (製品の背景と同じ作り方)。
std::vector<std::uint8_t> gradient_background(int width, int height)
{
	std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t offset =
				(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
			bgra[offset] = static_cast<std::uint8_t>(x * 7 + y);
			bgra[offset + 1] = static_cast<std::uint8_t>(x * 3);
			bgra[offset + 2] = static_cast<std::uint8_t>(y * 5);
			bgra[offset + 3] = 0xFFu;
		}
	}
	std::vector<std::uint8_t> background = make_buffer(width, height);
	REQUIRE(dummy_nv12_from_bgra(bgra.data(), bgra.size(), width, height, background.data(), background.size()));
	return background;
}

// OBS の描画の刻み (obs_fps の刻みに jitter_ns の揺れを足した時刻) を count 回並べ、dummy_frame_due が渡すと答えた刻みの
// 番号を返す。揺れは刻みごとに +jitter_ns、-jitter_ns、0 の順に繰り返す。
std::vector<int> due_ticks(double obs_fps, std::int64_t jitter_ns, int count)
{
	DummyPacer pacer;
	const std::uint64_t interval = 1'000'000'000ULL / static_cast<std::uint64_t>(kDummyFps);
	const std::int64_t start = 1'000'000'000'000LL; // 0 は「まだ渡していない」と区別できないので避ける
	std::vector<int> due;
	for (int i = 0; i < count; ++i) {
		const std::int64_t jitter = i % 3 == 0 ? jitter_ns : (i % 3 == 1 ? -jitter_ns : 0);
		const std::int64_t t = start + static_cast<std::int64_t>(std::llround(i * 1e9 / obs_fps)) + jitter;
		if (dummy_frame_due(pacer, static_cast<std::uint64_t>(t), interval))
			due.push_back(i);
	}
	return due;
}

} // namespace

TEST_CASE("線の列だけを描き直したフレームは、毎回最初から作ったフレームと同じになる")
{
	// 奇数の幅と高さ (右端と下端の色差の組が 1 列と 1 行) も確かめる。
	for (const auto &[width, height] :
	     {std::pair<int, int>{640, 360}, std::pair<int, int>{1280, 720}, std::pair<int, int>{641, 361}}) {
		CAPTURE(width);
		const std::vector<std::uint8_t> background = gradient_background(width, height);
		std::vector<std::uint8_t> moved = make_buffer(width, height);
		std::vector<std::uint8_t> full = make_buffer(width, height);
		REQUIRE(fill_dummy_base(3, width, height, background.data(), background.size(), moved.data(), moved.size()));
		DummyLineStrip strip;
		// 60 fps で 5 秒 (端での折り返しを含み、線の端に端数が出る時刻も含む)。
		int mismatches = 0;
		for (int frame = 0; frame < 300; ++frame) {
			const std::uint64_t t =
				static_cast<std::uint64_t>(frame) * 1'000'000'000ULL / static_cast<std::uint64_t>(kDummyFps);
			REQUIRE(move_dummy_line(width, height, t, kDefaultDummyCycleNs, moved.data(), moved.size(), strip));
			REQUIRE(fill_dummy_tile(3, width, height, t, kDefaultDummyCycleNs, background.data(), background.size(),
						full.data(), full.size()));
			if (moved != full)
				++mismatches;
		}
		CHECK(mismatches == 0);
		// 覚えている列は、線の太さに端数の 1 列を足した幅まで。
		CHECK(strip.last - strip.first + 1 <= dummy_line_width(height) + 1);
	}
}

TEST_CASE("絵を描き直したときに strip を空にすれば、前の絵を戻さず、最初から作ったフレームと同じになる")
{
	const int width = 320;
	const int height = 180;
	const std::vector<std::uint8_t> background = gradient_background(width, height);
	std::vector<std::uint8_t> moved = make_buffer(width, height);
	std::vector<std::uint8_t> full = make_buffer(width, height);
	DummyLineStrip strip;
	// 単色で描いた後に、背景の画像へ入れ替える (キャンバスの大きさが変わった後の、背景の描き直しと同じ流れ)。
	REQUIRE(fill_dummy_base(2, width, height, nullptr, 0u, moved.data(), moved.size()));
	REQUIRE(move_dummy_line(width, height, 500 * kMs, kDefaultDummyCycleNs, moved.data(), moved.size(), strip));
	REQUIRE(fill_dummy_base(2, width, height, background.data(), background.size(), moved.data(), moved.size()));
	strip = DummyLineStrip{};
	REQUIRE(move_dummy_line(width, height, 700 * kMs, kDefaultDummyCycleNs, moved.data(), moved.size(), strip));
	REQUIRE(fill_dummy_tile(2, width, height, 700 * kMs, kDefaultDummyCycleNs, background.data(), background.size(),
				full.data(), full.size()));
	CHECK(moved == full);

	// 大きさの合わない strip (別の高さで覚えたもの) は戻さずに描き、領域の外に書かない。
	DummyLineStrip wrong;
	wrong.first = 4;
	wrong.last = 9;
	wrong.saved_y.assign(6u * 7u, 0xEEu);
	wrong.saved_uv.assign(3u * 2u * 7u, 0xEEu);
	REQUIRE(fill_dummy_base(2, width, height, background.data(), background.size(), moved.data(), moved.size()));
	REQUIRE(move_dummy_line(width, height, 700 * kMs, kDefaultDummyCycleNs, moved.data(), moved.size(), wrong));
	CHECK(moved == full);
	// 書き込む領域が足りないときと大きさが 0 のときは、何も書かずに false を返す。
	std::vector<std::uint8_t> small = make_buffer(width, height - 1);
	DummyLineStrip none;
	CHECK_FALSE(move_dummy_line(width, height, 0u, kDefaultDummyCycleNs, small.data(), small.size(), none));
	CHECK_FALSE(move_dummy_line(0, height, 0u, kDefaultDummyCycleNs, moved.data(), moved.size(), none));
	CHECK(none.saved_y.empty());
	CHECK(none.saved_uv.empty());
}

TEST_CASE("OBS が 60 fps なら、描画の刻みの時刻が揺れても、刻みごとにフレームを渡す")
{
	// 揺れなし、+-1 ms、+-2 ms の揺れ (最も早い刻みと最も遅い刻みの差が、間隔の 4 分の 1 の約 4.17 ms に収まる)。製品は
	// OBS の描画の刻みの時刻 (obs_get_video_frame_time。決まった間隔で進む) を渡すので、揺れはこれより小さい。
	for (const std::int64_t jitter : {std::int64_t{0}, std::int64_t{1'000'000}, std::int64_t{2'000'000}}) {
		CAPTURE(jitter);
		CHECK(due_ticks(60.0, jitter, 600).size() == 600u);
	}
}

TEST_CASE("OBS が 30 fps と 50 fps なら刻みごとに、120 fps なら 1 つおきに、144 fps なら 1 秒に約 60 回渡し、続けては渡さない")
{
	CHECK(due_ticks(30.0, 500'000, 300).size() == 300u);
	CHECK(due_ticks(50.0, 500'000, 300).size() == 300u);

	const std::vector<int> at120 = due_ticks(120.0, 500'000, 1200);
	CHECK(at120.size() == 600u);
	for (std::size_t i = 1; i < at120.size(); ++i)
		CHECK(at120[i] - at120[i - 1] == 2);

	// 144 fps (10 秒) は、刻みの間隔が 60 fps の間隔に合わないので、2 つおきか 3 つおきに渡す。
	const std::vector<int> at144 = due_ticks(144.0, 0, 1440);
	CHECK(at144.size() >= 590u);
	CHECK(at144.size() <= 610u);
	for (std::size_t i = 1; i < at144.size(); ++i) {
		CHECK(at144[i] - at144[i - 1] >= 2);
		CHECK(at144[i] - at144[i - 1] <= 3);
	}
}

TEST_CASE("描画が止まって遅れたときと時刻が戻ったときは、続けて渡さずにその刻みから数え直す")
{
	const std::uint64_t interval = 1'000'000'000ULL / static_cast<std::uint64_t>(kDummyFps);
	const std::uint64_t t0 = 1'000'000'000'000ULL;
	DummyPacer pacer;
	CHECK(dummy_frame_due(pacer, t0, interval));
	CHECK(dummy_frame_due(pacer, t0 + interval, interval));
	// 0.5 秒止まった後の刻みは渡し、そのすぐ後 (1 ms 後) の刻みでは遅れを取り戻そうとしない。
	const std::uint64_t resumed = t0 + 500 * kMs;
	CHECK(dummy_frame_due(pacer, resumed, interval));
	CHECK_FALSE(dummy_frame_due(pacer, resumed + kMs, interval));
	CHECK(dummy_frame_due(pacer, resumed + interval, interval));
	// 時刻が大きく戻ったら (OBS の映像の作り直しなど)、待たずに渡す。
	CHECK(dummy_frame_due(pacer, t0 - 10'000 * kMs, interval));
	CHECK_FALSE(dummy_frame_due(pacer, t0 - 10'000 * kMs + kMs, interval));
	// 間隔が 0 なら、いつも渡す。
	DummyPacer always;
	CHECK(dummy_frame_due(always, t0, 0u));
	CHECK(dummy_frame_due(always, t0, 0u));
}

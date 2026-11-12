// SPDX-License-Identifier: Apache-2.0

// sources/composite_texture_strips.h (大きな画像を帯に分けてテクスチャにするための分け方と間合い) のテスト。全部の行が
// ちょうど 1 回ずつ置かれること、余分の行が画像の外に出ないこと、帯の大きさの上限、1 行が上限を超える幅、高さ 1 と 0、
// 切り抜きと帯の重なり、コンテキストに入る前の待ち (フレームが進むまで、上限、止めるとき) を確認する。

#include "doctest.h"

#include "sources/composite_texture_strips.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using cordmux::sources::CompositeTextureStrip;
using cordmux::sources::CompositeUploadClock;
using cordmux::sources::CompositeUploadTurn;
using cordmux::sources::for_each_composite_strip_piece;
using cordmux::sources::kCompositeStripMaxBytes;
using cordmux::sources::plan_composite_texture_strips;

namespace {

// 帯の並びが守るはずのこと。どの行もちょうど 1 回置かれ、余分の行は隣の帯の置く行で、画像の外に出ない。
void check_strips(const std::vector<CompositeTextureStrip> &strips, int height)
{
	REQUIRE_FALSE(strips.empty());
	int next = 0;
	for (std::size_t i = 0; i < strips.size(); ++i) {
		const CompositeTextureStrip &s = strips[i];
		CHECK(s.top == next);
		CHECK(s.rows >= 1);
		CHECK(s.texture_top() >= 0);
		CHECK(s.texture_top() + s.texture_rows() <= height);
		if (strips.size() == 1) {
			CHECK(s.pad_top == 0);
			CHECK(s.pad_bottom == 0);
		} else {
			CHECK(s.pad_top == (i > 0 ? 1 : 0));
			CHECK(s.pad_bottom == (i + 1 < strips.size() ? 1 : 0));
		}
		next = s.top + s.rows;
	}
	CHECK(next == height);
}

// 置く部分を集める。
struct Piece {
	std::size_t index = 0;
	int texture_row = 0;
	int image_row = 0;
	int rows = 0;
};

std::vector<Piece> pieces_of(const std::vector<CompositeTextureStrip> &strips, int first, int count)
{
	std::vector<Piece> out;
	for_each_composite_strip_piece(strips, first, count,
				       [&](std::size_t index, int texture_row, int image_row, int rows) {
					       out.push_back(Piece{index, texture_row, image_row, rows});
				       });
	return out;
}

// テスト用の時計。sleep を呼ぶたびに 1 ms 進み、advance_after 回目の sleep でフレームが進み、stop_after 回目の sleep の
// 後から止める (どちらも 0 なら起きない)。
struct FakeClock {
	std::uint64_t now = 1'000'000'000u;
	std::uint64_t frame = 5'000u;
	int sleeps = 0;
	int advance_after = 0;
	int stop_after = 0;
	bool stopping = false;

	CompositeUploadClock clock()
	{
		CompositeUploadClock c;
		c.frame_time = [this] { return frame; };
		c.now_ns = [this] { return now; };
		c.sleep = [this] {
			++sleeps;
			now += 1'000'000u;
			if (advance_after > 0 && sleeps == advance_after)
				frame += 16'666'667u;
			if (stop_after > 0 && sleeps >= stop_after)
				stopping = true;
		};
		c.stopping = [this] { return stopping; };
		return c;
	}
};

} // namespace

TEST_CASE("上限以下の画像は、余分の行のない帯 1 つにする")
{
	const auto exact = plan_composite_texture_strips(100, 50, 100u * 4u * 50u);
	REQUIRE(exact.size() == 1);
	CHECK(exact[0].top == 0);
	CHECK(exact[0].rows == 50);
	CHECK(exact[0].pad_top == 0);
	CHECK(exact[0].pad_bottom == 0);
	CHECK(plan_composite_texture_strips(100, 50, kCompositeStripMaxBytes).size() == 1);
	// 1 行だけ上限を超えると分ける。
	CHECK(plan_composite_texture_strips(100, 51, 100u * 4u * 50u).size() == 2);
}

TEST_CASE("上限を超える画像は、余分の行を含めて上限以下の帯に分ける")
{
	// 1 行 4 バイトで、上限は 4 行。
	const auto strips = plan_composite_texture_strips(1, 10, 16u);
	REQUIRE(strips.size() == 4);
	CHECK((strips[0].top == 0 && strips[0].rows == 3 && strips[0].pad_top == 0 && strips[0].pad_bottom == 1));
	CHECK((strips[1].top == 3 && strips[1].rows == 2 && strips[1].pad_top == 1 && strips[1].pad_bottom == 1));
	CHECK((strips[2].top == 5 && strips[2].rows == 2 && strips[2].pad_top == 1 && strips[2].pad_bottom == 1));
	CHECK((strips[3].top == 7 && strips[3].rows == 3 && strips[3].pad_top == 1 && strips[3].pad_bottom == 0));
	for (const CompositeTextureStrip &s : strips)
		CHECK(s.texture_rows() == 4);
	check_strips(strips, 10);
}

TEST_CASE("どの大きさでも、全部の行がちょうど 1 回置かれ、余分の行は画像の外に出ず、帯は上限以下になる")
{
	for (const int width : {1, 3, 100, 1920}) {
		for (int height = 1; height <= 40; ++height) {
			for (const int limit_rows : {1, 2, 3, 4, 5, 7, 39, 40, 41}) {
				CAPTURE(width);
				CAPTURE(height);
				CAPTURE(limit_rows);
				const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4u;
				const std::uint64_t max_bytes = row_bytes * static_cast<std::uint64_t>(limit_rows);
				const auto strips = plan_composite_texture_strips(width, height, max_bytes);
				check_strips(strips, height);
				for (const CompositeTextureStrip &s : strips) {
					// 置く行が 1 行の帯だけは、余分の行の分だけ上限を超えてよい。
					if (s.rows > 1)
						CHECK(static_cast<std::uint64_t>(s.texture_rows()) * row_bytes <=
						      max_bytes);
				}
				if (height <= limit_rows)
					CHECK(strips.size() == 1);
			}
		}
	}
}

TEST_CASE("製品の上限で、大きな光の画像を 512 KB 以下の帯に分ける")
{
	// 6.5 MB ほどの画像。
	const int width = 1920;
	const int height = 850;
	const auto strips = plan_composite_texture_strips(width, height, kCompositeStripMaxBytes);
	check_strips(strips, height);
	CHECK(strips.size() > 1);
	for (const CompositeTextureStrip &s : strips)
		CHECK(static_cast<std::uint64_t>(s.texture_rows()) * static_cast<std::uint64_t>(width) * 4u <=
		      512u * 1024u);
	CHECK(kCompositeStripMaxBytes == 512u * 1024u);
}

TEST_CASE("1 行が上限を超える幅では、置く行を 1 行ずつの帯にする")
{
	const auto strips = plan_composite_texture_strips(1000, 5, 1000u);
	REQUIRE(strips.size() == 5);
	for (std::size_t i = 0; i < strips.size(); ++i) {
		CHECK(strips[i].top == static_cast<int>(i));
		CHECK(strips[i].rows == 1);
	}
	check_strips(strips, 5);
}

TEST_CASE("高さ 1 の画像は 1 行の帯 1 つにし、高さか幅が 0 の画像は帯を作らない")
{
	const auto one = plan_composite_texture_strips(1000, 1, 16u);
	REQUIRE(one.size() == 1);
	CHECK((one[0].top == 0 && one[0].rows == 1 && one[0].pad_top == 0 && one[0].pad_bottom == 0));
	CHECK(plan_composite_texture_strips(100, 0, kCompositeStripMaxBytes).empty());
	CHECK(plan_composite_texture_strips(0, 100, kCompositeStripMaxBytes).empty());
	CHECK(plan_composite_texture_strips(-1, 100, kCompositeStripMaxBytes).empty());
	CHECK(plan_composite_texture_strips(100, -1, kCompositeStripMaxBytes).empty());
}

TEST_CASE("切り抜きと帯の置く行が重なる部分だけを、帯ごとに置く")
{
	const auto strips = plan_composite_texture_strips(1, 10, 16u);
	// 画像の 2 行目から 6 行。
	const auto pieces = pieces_of(strips, 2, 6);
	REQUIRE(pieces.size() == 4);
	CHECK((pieces[0].index == 0 && pieces[0].texture_row == 2 && pieces[0].image_row == 2 && pieces[0].rows == 1));
	CHECK((pieces[1].index == 1 && pieces[1].texture_row == 1 && pieces[1].image_row == 3 && pieces[1].rows == 2));
	CHECK((pieces[2].index == 2 && pieces[2].texture_row == 1 && pieces[2].image_row == 5 && pieces[2].rows == 2));
	CHECK((pieces[3].index == 3 && pieces[3].texture_row == 1 && pieces[3].image_row == 7 && pieces[3].rows == 1));
	// 1 つの帯の中に収まる切り抜きは、その帯だけを置く。
	const auto inside = pieces_of(strips, 3, 2);
	REQUIRE(inside.size() == 1);
	CHECK((inside[0].index == 1 && inside[0].texture_row == 1 && inside[0].rows == 2));
	// 帯 1 つの画像は、テクスチャの同じ行から置く (1 枚のテクスチャのときと同じ)。
	const auto whole = pieces_of(plan_composite_texture_strips(1, 10, 1000u), 4, 3);
	REQUIRE(whole.size() == 1);
	CHECK((whole[0].index == 0 && whole[0].texture_row == 4 && whole[0].image_row == 4 && whole[0].rows == 3));
}

TEST_CASE("どの切り抜きでも、置く部分は切り抜きの行をちょうど 1 回ずつ覆い、帯の置く行だけを読む")
{
	for (const int limit_rows : {1, 2, 3, 4, 6, 20}) {
		const int height = 17;
		const auto strips =
			plan_composite_texture_strips(1, height, static_cast<std::uint64_t>(limit_rows) * 4u);
		for (int first = 0; first < height; ++first) {
			for (int count = 1; first + count <= height; ++count) {
				CAPTURE(limit_rows);
				CAPTURE(first);
				CAPTURE(count);
				int next = first;
				for (const Piece &p : pieces_of(strips, first, count)) {
					const CompositeTextureStrip &s = strips[p.index];
					CHECK(p.image_row == next);
					CHECK(p.rows >= 1);
					// テクスチャの行は、1 枚のテクスチャで置いたときと同じ画像の行を指す。
					CHECK(s.texture_top() + p.texture_row == p.image_row);
					// 余分の行は置かない。
					CHECK(p.texture_row >= s.pad_top);
					CHECK(p.texture_row + p.rows <= s.pad_top + s.rows);
					next = p.image_row + p.rows;
				}
				CHECK(next == first + count);
			}
		}
	}
}

TEST_CASE("初めてコンテキストに入るときは待たない")
{
	FakeClock fake;
	CompositeUploadTurn turn(fake.clock());
	CHECK(turn.wait());
	CHECK(fake.sleeps == 0);
}

TEST_CASE("コンテキストを出た後は、描画のスレッドが次のフレームに進むまで待つ")
{
	FakeClock fake;
	CompositeUploadTurn turn(fake.clock());
	REQUIRE(turn.wait());
	turn.left();
	fake.advance_after = 3;
	CHECK(turn.wait());
	CHECK(fake.sleeps == 3);
	// 進んだ後に出たら、また次のフレームまで待つ。
	turn.left();
	fake.sleeps = 0;
	fake.advance_after = 2;
	CHECK(turn.wait());
	CHECK(fake.sleeps == 2);
}

TEST_CASE("コンテキストを出た後にすでにフレームが進んでいれば、待たない")
{
	FakeClock fake;
	CompositeUploadTurn turn(fake.clock());
	turn.left();
	fake.frame += 16'666'667u;
	CHECK(turn.wait());
	CHECK(fake.sleeps == 0);
}

TEST_CASE("フレームが進まなくても、上限まで待ったら入る")
{
	FakeClock fake;
	CompositeUploadTurn turn(fake.clock(), 100'000'000u);
	turn.left();
	CHECK(turn.wait());
	CHECK(fake.sleeps == 100);
}

TEST_CASE("止めるときは、帯の間の待ちをやめて false を返す")
{
	FakeClock fake;
	CompositeUploadTurn turn(fake.clock());
	turn.left();
	fake.stop_after = 2;
	CHECK_FALSE(turn.wait());
	CHECK(fake.sleeps == 2);
	// 止めた後は、待つ必要がなくても入らない。
	FakeClock stopped;
	stopped.stopping = true;
	CompositeUploadTurn first(stopped.clock());
	CHECK_FALSE(first.wait());
	CHECK(stopped.sleeps == 0);
}

// SPDX-License-Identifier: Apache-2.0

// sources/composite_frame_plan.h (合成のソースが 1 フレームに描くものと、1 人の映像だけを全面に描いたかの純粋関数と、
// フレームごとの値の入れ物と、番組の描画の印) のテスト。合成のソースが 1 人を全面に描いたと決まる場合と決まらない場合、
// 全画面への切り替えの途中、タイルごとに描く矩形と拡大の倍率、番組の描画の中と外のフィルタの ON と OFF、値をほかの描画と
// 共にする条件 (描画のスレッドと tick) を確認する。

#include "doctest.h"

#include "config/allocation.h"
#include "sources/composite_frame_plan.h"
#include "sources/composite_input.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using cordmux::sources::composite_frame_scope;
using cordmux::sources::composite_shows_one_full;
using cordmux::sources::composite_tile_draw;
using cordmux::sources::CompositeFrameFacts;
using cordmux::sources::CompositeFrameMemo;
using cordmux::sources::CompositeFrameScope;
using cordmux::sources::CompositeFramePlan;
using cordmux::sources::CompositeInput;
using cordmux::sources::CompositeShare;
using cordmux::sources::CompositeTile;
using cordmux::sources::CompositeTileDraw;
using cordmux::sources::ContentSize;
using cordmux::sources::program_pass_begin;
using cordmux::sources::program_pass_end;
using cordmux::sources::program_pass_note_composite;
using cordmux::sources::program_pass_tick;
using cordmux::sources::ProgramPassMark;
using cordmux::sources::solo_filter_on;
using cordmux::sources::PixelRect;
using cordmux::sources::plan_composite_frame;

namespace {

constexpr int kCanvasW = 1920;
constexpr int kCanvasH = 1080;

CompositeShare share(int slot, const std::string &name, const std::string &user_id)
{
	CompositeShare s;
	s.slot = slot;
	s.name = name;
	s.user_id = user_id;
	return s;
}

// 画面共有の人を shares のとおりに載せ、自分の枠は出さない入力。
CompositeInput shares_input(std::vector<CompositeShare> shares)
{
	CompositeInput input;
	input.gap_px = 8;
	input.shares = std::move(shares);
	return input;
}

// 合成のソースが映っていて、ダミーでなく、全部の子と自分の枠の映像が 1280x720 で届いている値。
CompositeFrameFacts facts_all_ready()
{
	CompositeFrameFacts facts;
	facts.canvas_w = kCanvasW;
	facts.canvas_h = kCanvasH;
	for (ContentSize &size : facts.tile_sizes)
		size = ContentSize{1280, 720};
	facts.self_size = ContentSize{1280, 720};
	return facts;
}

void set_tile_size(CompositeFrameFacts &facts, int slot, int w, int h)
{
	facts.tile_sizes[static_cast<std::size_t>(slot - 1)] = ContentSize{w, h};
}

bool is_full_canvas(const PixelRect &rect)
{
	return rect.x == 0 && rect.y == 0 && rect.w == kCanvasW && rect.h == kCanvasH;
}

// 合成のソースが plan のとおりに描き (描いたタイルは has_video)、描いた映像のダミーかが dummy (省くと全部偽) のときに、1 人の
// 映像だけを全面に描いたか。合成のソースの描画 (sources/composite_source.cpp) が描き終えたときに決めるのと同じ呼び方。
bool drew_solo(const CompositeFramePlan &plan, std::vector<bool> dummy = {})
{
	if (dummy.empty())
		dummy.assign(plan.layout.tiles.size(), false);
	return composite_shows_one_full(plan.layout.tiles, plan.has_video, dummy, kCanvasW, kCanvasH);
}

} // namespace

// ---- ON になる場合 ----

TEST_CASE("1 人だけが枠に載り、全面に映像を描いているときは ON")
{
	const CompositeFramePlan plan = plan_composite_frame(shares_input({share(2, "Bob", "u2")}), facts_all_ready());
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(is_full_canvas(plan.layout.tiles[0].rect));
	CHECK(plan.has_video[0]);
	CHECK(drew_solo(plan));
}

TEST_CASE("4 等分に並んでいるときに、枠に載っている 1 人を [全画面で表示] にすると ON")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1"), share(2, "Bob", "u2"), share(3, "Carol", "u3")});
	input.focus_user_id = "u2";
	const CompositeFramePlan plan = plan_composite_frame(input, facts_all_ready());
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(plan.layout.tiles[0].slot == 2);
	CHECK(plan.layout.focused);
	CHECK(drew_solo(plan));
}

TEST_CASE("枠に載せていない人を [全画面で表示] にして、その人の子に映像が届いたら ON")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1"), share(2, "Bob", "u2")});
	input.focus_user_id = "u9";
	input.focus_extra_slot = cordmux::config::kFocusOnlySlot;
	const CompositeFramePlan plan = plan_composite_frame(input, facts_all_ready());
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(plan.layout.tiles[0].slot == cordmux::config::kFocusOnlySlot);
	CHECK(drew_solo(plan));
}

TEST_CASE("自分の枠だけが全面に映っているときは ON")
{
	CompositeInput input;
	input.self_shown = true;
	input.self_user_id = "self";
	input.self_name = "Me";
	const CompositeFramePlan plan = plan_composite_frame(input, facts_all_ready());
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(plan.layout.tiles[0].self);
	CHECK(drew_solo(plan));
}

TEST_CASE("映像の縦横比がキャンバスと違い、上下か左右に余白があっても、全面のタイルに映像を描いていれば ON")
{
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, 1, 1080, 1920);
	const CompositeFramePlan plan = plan_composite_frame(shares_input({share(1, "Alice", "u1")}), facts);
	REQUIRE(plan.fits.size() == 1);
	CHECK(plan.fits[0].w < kCanvasW);
	CHECK(drew_solo(plan));
}

TEST_CASE("枠に載せていない人の全画面への切り替えの途中でも、全画面でないときの配置が 1 人の全面なら ON")
{
	// 映像が届くまでは全画面でないときと同じに描く。そのときの配置が 1 人の全面なら、全面に 1 人を映している。
	CompositeInput input = shares_input({share(1, "Alice", "u1")});
	input.focus_user_id = "u9";
	input.focus_extra_slot = cordmux::config::kFocusOnlySlot;
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, cordmux::config::kFocusOnlySlot, 0, 0);
	const CompositeFramePlan plan = plan_composite_frame(input, facts);
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(plan.layout.tiles[0].slot == 1);
	CHECK_FALSE(plan.layout.focused);
	CHECK(drew_solo(plan));
}

TEST_CASE("自分の枠を [全画面で表示] にして、自分の枠の映像があれば ON、大きさが 0 なら OFF")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1")});
	input.self_shown = true;
	input.self_user_id = "self";
	input.self_name = "Me";
	input.focus_user_id = "self";
	const CompositeFramePlan plan = plan_composite_frame(input, facts_all_ready());
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(plan.layout.tiles[0].self);
	CHECK(plan.layout.focused);
	CHECK(drew_solo(plan));

	CompositeFrameFacts no_self = facts_all_ready();
	no_self.self_size = ContentSize{};
	const CompositeFramePlan empty = plan_composite_frame(input, no_self);
	REQUIRE(empty.layout.tiles.size() == 1);
	CHECK_FALSE(drew_solo(empty));
}

TEST_CASE("置く場所が足りずに描かなかった (dropped の) 人を [全画面で表示] にすると、その人の枠の子に映像があれば ON")
{
	// 自分の枠と 4 人で 5 つになり、名前順の最後の Dave (枠 4) が dropped になる。全画面は Dave の枠の子を全面に描く。
	CompositeInput input = shares_input(
		{share(1, "Alice", "u1"), share(2, "Bob", "u2"), share(3, "Carol", "u3"), share(4, "Dave", "u4")});
	input.self_shown = true;
	input.self_user_id = "self";
	input.self_name = "Me";
	input.focus_user_id = "u4";
	input.focus_extra_slot = 4;
	const CompositeFramePlan plan = plan_composite_frame(input, facts_all_ready());
	REQUIRE(plan.layout.dropped.size() == 1);
	CHECK(plan.layout.dropped[0].user_id == "u4");
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(plan.layout.tiles[0].slot == 4);
	CHECK(drew_solo(plan));

	// その子に映像がまだなければ、全画面でないときの 4 等分のまま描くので OFF。
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, 4, 0, 0);
	const CompositeFramePlan waiting = plan_composite_frame(input, facts);
	CHECK(waiting.layout.tiles.size() == 4);
	CHECK_FALSE(drew_solo(waiting));
}

// ---- OFF になる場合 ----

TEST_CASE("誰も映っていないときは OFF")
{
	const CompositeFramePlan plan = plan_composite_frame(CompositeInput{}, facts_all_ready());
	CHECK(plan.layout.tiles.empty());
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("2 人以上が 4 等分に並んでいるときは OFF")
{
	const CompositeFramePlan plan =
		plan_composite_frame(shares_input({share(1, "Alice", "u1"), share(2, "Bob", "u2")}), facts_all_ready());
	CHECK(plan.layout.tiles.size() == 2);
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("自分の枠とほかの 1 人が並んでいるときは OFF")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1")});
	input.self_shown = true;
	input.self_user_id = "self";
	input.self_name = "Me";
	const CompositeFramePlan plan = plan_composite_frame(input, facts_all_ready());
	CHECK(plan.layout.tiles.size() == 2);
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("枠に載せていない人の全画面への切り替えの途中 (その人の子に映像がまだない) で、4 等分に並んでいるときは OFF")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1"), share(2, "Bob", "u2")});
	input.focus_user_id = "u9";
	input.focus_extra_slot = cordmux::config::kFocusOnlySlot;
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, cordmux::config::kFocusOnlySlot, 0, 0);
	const CompositeFramePlan plan = plan_composite_frame(input, facts);
	CHECK(plan.layout.tiles.size() == 2);
	CHECK_FALSE(plan.layout.focused);
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("枠に載っている人を [全画面で表示] にしても、その人の子に映像がなければ (大きさ 0) OFF")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1"), share(2, "Bob", "u2")});
	input.focus_user_id = "u2";
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, 2, 0, 0);
	const CompositeFramePlan plan = plan_composite_frame(input, facts);
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK(is_full_canvas(plan.layout.tiles[0].rect));
	CHECK_FALSE(plan.has_video[0]);
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("1 人だけが全面のタイルにいても、その子に映像がまだ届いていなければ (大きさ 0) OFF")
{
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, 1, 0, 0);
	const CompositeFramePlan plan = plan_composite_frame(shares_input({share(1, "Alice", "u1")}), facts);
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK_FALSE(plan.has_video[0]);
	CHECK_FALSE(drew_solo(plan));

	// 幅か高さの片方だけが 0 のときも、映像を描かない。
	set_tile_size(facts, 1, 1280, 0);
	CHECK_FALSE(drew_solo(plan_composite_frame(shares_input({share(1, "Alice", "u1")}), facts)));
}

TEST_CASE("自分の枠だけが全面のタイルにいても、自分の枠の映像のソースの大きさが 0 なら OFF")
{
	CompositeInput input;
	input.self_shown = true;
	input.self_user_id = "self";
	CompositeFrameFacts facts = facts_all_ready();
	facts.self_size = ContentSize{};
	const CompositeFramePlan plan = plan_composite_frame(input, facts);
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("1 人のときに全面にしない設定 (full_when_single が偽) で 4 等分の 1 つに映っているときは OFF")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1")});
	input.full_when_single = false;
	const CompositeFramePlan plan = plan_composite_frame(input, facts_all_ready());
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK_FALSE(is_full_canvas(plan.layout.tiles[0].rect));
	CHECK(plan.has_video[0]);
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("キャンバスの大きさが読めない (0) ときは、何も描かず OFF")
{
	CompositeFrameFacts facts = facts_all_ready();
	facts.canvas_w = 0;
	facts.canvas_h = 0;
	const CompositeFramePlan plan = plan_composite_frame(shares_input({share(1, "Alice", "u1")}), facts);
	CHECK(plan.layout.tiles.empty());
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("composite_shows_one_full はタイルの矩形がキャンバス全体と 1 ピクセルでも違えば偽")
{
	CompositeTile tile;
	tile.rect = PixelRect{0, 0, kCanvasW, kCanvasH};
	CHECK(composite_shows_one_full({tile}, {true}, {false}, kCanvasW, kCanvasH));
	tile.rect = PixelRect{0, 0, kCanvasW - 1, kCanvasH};
	CHECK_FALSE(composite_shows_one_full({tile}, {true}, {false}, kCanvasW, kCanvasH));
	tile.rect = PixelRect{1, 0, kCanvasW, kCanvasH};
	CHECK_FALSE(composite_shows_one_full({tile}, {true}, {false}, kCanvasW, kCanvasH));
	// has_video と dummy が足りないときは偽 (描くか分からないものを ON にしない)。
	tile.rect = PixelRect{0, 0, kCanvasW, kCanvasH};
	CHECK_FALSE(composite_shows_one_full({tile}, {}, {false}, kCanvasW, kCanvasH));
	CHECK_FALSE(composite_shows_one_full({tile}, {true}, {}, kCanvasW, kCanvasH));
}

TEST_CASE("描いた映像がダミーのフレームなら、ダミーの 1 つを全画面にしていても OFF")
{
	std::vector<cordmux::sources::SlotPerson> people;
	for (int slot = 1; slot <= 4; ++slot) {
		cordmux::sources::SlotPerson person;
		person.slot = slot;
		person.name = "Placeholder " + std::to_string(slot);
		person.user_id = "dummy:" + std::to_string(slot);
		people.push_back(person);
	}
	CompositeInput input = cordmux::sources::dummy_composite_input(people, true, 8);
	input.focus_user_id = "dummy:3";
	const CompositeFramePlan focused = plan_composite_frame(input, facts_all_ready());
	REQUIRE(focused.layout.tiles.size() == 1);
	CHECK(focused.has_video[0]);
	CHECK_FALSE(drew_solo(focused, {true}));
	// 同じ配置でも、描いた映像が実際の人のフレームなら ON (ダミーを止めた後、テクスチャが実際のフレームに替わったとき)。
	CHECK(drew_solo(focused, {false}));
}

TEST_CASE("映像を描かなかったタイル (描く前に子がなくなった) は、全面の 1 枚でも OFF")
{
	const CompositeFramePlan plan = plan_composite_frame(shares_input({share(1, "Alice", "u1")}), facts_all_ready());
	REQUIRE(plan.layout.tiles.size() == 1);
	CHECK_FALSE(composite_shows_one_full(plan.layout.tiles, {false}, {false}, kCanvasW, kCanvasH));
}

// ---- フィルタの ON と OFF (番組の描画の中と外) ----

TEST_CASE("番組の描画の中では、合成のソースが先に 1 人を全面に描いたときだけ ON")
{
	ProgramPassMark mark;
	program_pass_tick(mark);
	program_pass_begin(mark);
	// まだ合成のソースが描かれていない (フィルタを付けたソースが下にある、合成のソースが隠れている、トランジションで描かれない)。
	CHECK_FALSE(solo_filter_on(mark, true));
	// 合成のソースが 4 等分を描いた。
	program_pass_note_composite(mark, false);
	CHECK_FALSE(solo_filter_on(mark, true));
	// 合成のソースが 1 人を全面に描いた (合成のソースが 2 つあり、片方だけが全面でも ON)。
	program_pass_note_composite(mark, true);
	CHECK(solo_filter_on(mark, false));
	program_pass_note_composite(mark, false);
	CHECK(solo_filter_on(mark, false));
}

TEST_CASE("番組の描画を終えると印を降ろし、出した値を番組の描画の外のフィルタが使う")
{
	ProgramPassMark mark;
	program_pass_tick(mark);
	program_pass_begin(mark);
	program_pass_note_composite(mark, true);
	bool published = false;
	CHECK(program_pass_end(mark, &published));
	CHECK(published);
	CHECK_FALSE(mark.in_pass);
	// 番組の描画の外 (プレビュー、プロジェクター、スクリーンショット) は直前の値。
	CHECK(solo_filter_on(mark, published));
	CHECK_FALSE(solo_filter_on(mark, false));

	// 次の周の番組の描画で合成のソースが描かれなければ、前の値を残さず偽を出す。
	program_pass_tick(mark);
	program_pass_begin(mark);
	CHECK(program_pass_end(mark, &published));
	CHECK_FALSE(published);
}

TEST_CASE("その周で 2 つ目からのミックスの描画では印を立てず、合成のソースを描いても何も出さない")
{
	ProgramPassMark mark;
	program_pass_tick(mark);
	program_pass_begin(mark); // メインのキャンバスのミックス (常に先頭)
	bool published = false;
	CHECK(program_pass_end(mark, &published));
	program_pass_begin(mark); // ほかのミックス (縦の配信のキャンバス、拡大の違う出力)
	CHECK_FALSE(mark.in_pass);
	program_pass_note_composite(mark, true);
	CHECK_FALSE(mark.drew_solo);
	published = true;
	CHECK_FALSE(program_pass_end(mark, &published));
	CHECK(published); // 書き換えない
	// 番組の描画の外なので、直前の番組の描画の値を使う。
	CHECK(solo_filter_on(mark, true));
	CHECK_FALSE(solo_filter_on(mark, false));
}

TEST_CASE("出力を始めた周 (メインのミックスのテクスチャがまだない周) でも、tick の後の最初の描画を番組の描画と見なす")
{
	// 前の形は描画先を obs_get_main_texture と比べていて、配信、録画、仮想カメラ、リプレイバッファーの開始とビデオのリセットの
	// 周 (テクスチャを作り直して texture_rendered が偽) を番組の描画と見なさなかった。今は周の最初の描画で決める。
	ProgramPassMark mark;
	program_pass_tick(mark);
	program_pass_begin(mark);
	CHECK(mark.in_pass);
	program_pass_note_composite(mark, true);
	bool published = false;
	CHECK(program_pass_end(mark, &published));
	CHECK(published);
}

TEST_CASE("描画の途中でコールバックが外れて印が立ったまま残っても、次の周の tick で降ろす")
{
	ProgramPassMark mark;
	program_pass_tick(mark);
	program_pass_begin(mark);
	program_pass_note_composite(mark, true);
	// 後のコールバック (program_pass_end) が呼ばれないまま、次の周 (合成のソースを作り直してコールバックを付け直した後)。
	program_pass_tick(mark);
	CHECK_FALSE(mark.in_pass);
	CHECK_FALSE(mark.drew_solo);
	CHECK_FALSE(solo_filter_on(mark, false));
	program_pass_begin(mark);
	CHECK(mark.in_pass);
}

TEST_CASE("番組の描画の外で合成のソースを描いても、番組の描画の値は変わらない")
{
	ProgramPassMark mark;
	program_pass_tick(mark);
	program_pass_note_composite(mark, true);
	CHECK_FALSE(mark.drew_solo);
	bool published = true;
	CHECK_FALSE(program_pass_end(mark, &published));
}

// ---- 合成のソースの描画の結果を変えないこと ----

TEST_CASE("タイルごとに描くかと描く矩形と拡大の倍率は、すき間 8 の 4 等分の枠に縦横比を保って収めた値になる")
{
	// 自分の枠 (640x480) を左上、Alice (1920x1080) を右上、Bob (映像なし) を左下に描く。キャンバス 1920x1080、すき間 8 の枠は
	// 左上 (0,0,956,536)、右上 (964,0,956,536)。左上は高さに合わせて幅 715 にし、左右の余り 241 の小さいほう 120 を左に置く。
	// 右上は高さに合わせて幅 953 にし、余り 3 の 1 を左に置く。
	CompositeInput input = shares_input({share(1, "Alice", "u1"), share(2, "Bob", "u2")});
	input.self_shown = true;
	input.self_user_id = "self";
	input.self_name = "Me";
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, 1, 1920, 1080);
	set_tile_size(facts, 2, 0, 0);
	facts.self_size = ContentSize{640, 480};
	const CompositeFramePlan plan = plan_composite_frame(input, facts);
	REQUIRE(plan.layout.tiles.size() == 3);

	REQUIRE(plan.layout.tiles[0].self);
	const CompositeTileDraw self = composite_tile_draw(plan, 0);
	CHECK(self.draw);
	CHECK(self.fit.x == 120);
	CHECK(self.fit.y == 0);
	CHECK(self.fit.w == 715);
	CHECK(self.fit.h == 536);
	CHECK(self.scale_x == doctest::Approx(715.0 / 640.0));
	CHECK(self.scale_y == doctest::Approx(536.0 / 480.0));

	REQUIRE(plan.layout.tiles[1].slot == 1);
	const CompositeTileDraw alice = composite_tile_draw(plan, 1);
	CHECK(alice.draw);
	CHECK(alice.fit.x == 965);
	CHECK(alice.fit.y == 0);
	CHECK(alice.fit.w == 953);
	CHECK(alice.fit.h == 536);
	CHECK(alice.scale_x == doctest::Approx(953.0 / 1920.0));
	CHECK(alice.scale_y == doctest::Approx(536.0 / 1080.0));

	REQUIRE(plan.layout.tiles[2].slot == 2);
	const CompositeTileDraw bob = composite_tile_draw(plan, 2);
	CHECK_FALSE(bob.draw);
	CHECK(bob.scale_x == 0.0f);
	CHECK_FALSE(plan.has_video[2]);

	// 範囲の外の添字は描かない。
	CHECK_FALSE(composite_tile_draw(plan, 3).draw);
	CHECK_FALSE(drew_solo(plan));
}

TEST_CASE("全画面で映す人の子に映像があるかは、その子の大きさで決まり、前の描画の focus_extra_ready と同じ")
{
	CompositeInput input = shares_input({share(1, "Alice", "u1"), share(2, "Bob", "u2")});
	input.focus_user_id = "u9";
	input.focus_extra_slot = cordmux::config::kFocusOnlySlot;
	CompositeFrameFacts facts = facts_all_ready();
	set_tile_size(facts, cordmux::config::kFocusOnlySlot, 1280, 0);
	CHECK_FALSE(plan_composite_frame(input, facts).layout.focused);
	set_tile_size(facts, cordmux::config::kFocusOnlySlot, 1280, 720);
	CHECK(plan_composite_frame(input, facts).layout.focused);
}

// ---- OBS のトランジションが描く側 ----

// ---- フレームごとの値を共にするか ----

TEST_CASE("描画のスレッドで、そのフレームの合成のソースの tick の後の描画だけが値を共にする")
{
	// 出力とプレビューの描画 (tick の後)。
	CHECK(composite_frame_scope(true, 1000, 1000) == CompositeFrameScope::Shared);
	// tick のコールバックの中のスクリーンショット (ソースの tick の前。tick の時刻は前のフレームのまま)。
	CHECK(composite_frame_scope(true, 1016, 1000) == CompositeFrameScope::Local);
	// 合成のソースの tick がまだ一度も来ていない。
	CHECK(composite_frame_scope(true, 1000, 0) == CompositeFrameScope::Local);
	// 描画のスレッドの外 (obs-websocket のスクリーンショット)。tick の時刻が同じでも共にしない。
	CHECK(composite_frame_scope(false, 1000, 1000) == CompositeFrameScope::Local);
	CHECK(composite_frame_scope(false, 1016, 1000) == CompositeFrameScope::Local);
}

TEST_CASE("tick の前に作った値と tick の後に作った値は、別の入れ物に入り、tick の後の描画は tick の後の値を使う")
{
	// 同じフレームの時刻 2000 で、tick の前の描画 (Local の入れ物) が先に作っても、tick の後の描画 (Shared の入れ物) は
	// 作り直す。current_frame (sources/composite_source.cpp) と同じ選び方を、入れ物 2 つで行う。
	CompositeFrameMemo<int> shared;
	CompositeFrameMemo<int> local;
	int child_width = 0; // tick の前は子の大きさが 0、tick で 1280 になる
	const auto make = [&child_width] { return child_width; };
	const auto pick = [&](bool graphics_thread, std::uint64_t tick_time) -> CompositeFrameMemo<int> & {
		return composite_frame_scope(graphics_thread, 2000, tick_time) == CompositeFrameScope::Shared ? shared : local;
	};
	const auto before_tick = pick(true, 1984).get(2000, make);
	child_width = 1280;
	const auto after_tick = pick(true, 2000).get(2000, make);
	const auto preview = pick(true, 2000).get(2000, make);
	CHECK(*before_tick == 0);
	CHECK(*after_tick == 1280);
	CHECK(preview.get() == after_tick.get());
}

// ---- フレームごとの値の入れ物 ----

TEST_CASE("同じフレームの時刻で呼ぶと、最初に作った値を返し、作り直さない")
{
	CompositeFrameMemo<int> memo;
	int made = 0;
	const auto make = [&made] { return ++made; };
	const auto first = memo.get(100, make);
	const auto again = memo.get(100, make);
	CHECK(*first == 1);
	CHECK(first.get() == again.get());
	CHECK(made == 1);
}

TEST_CASE("フレームの時刻が変わると、作り直す")
{
	CompositeFrameMemo<int> memo;
	int made = 0;
	const auto make = [&made] { return ++made; };
	CHECK(*memo.get(100, make) == 1);
	CHECK(*memo.get(116, make) == 2);
	CHECK(*memo.get(116, make) == 2);
	CHECK(made == 2);
}

TEST_CASE("最初の値を作る前の時刻 0 でも、値を作って返す")
{
	CompositeFrameMemo<int> memo;
	const auto value = memo.get(0, [] { return 7; });
	REQUIRE(value.get() != nullptr);
	CHECK(*value == 7);
}

// SPDX-License-Identifier: Apache-2.0

// sources/self_frame_plan.h の単体テスト。自分の枠に映すソースの候補にしてよい種類と、設定の UUID と名前から候補を探した
// 結果 (UUID で見つかる、名前で見つかる、名前が変わっている、見つからない、候補にしない種類だけが合う) と、書き直しの要否を
// 確認する。あわせて、自分の枠に映すシーンの候補にしてよいかと、自分の枠を出すかの状態 (ドックの付け外しと、自分の
// Discord の共有の始まりと終わり。ほかの人の行と同じ動き) の次の状態を確認する。

#include "doctest.h"

#include "sources/self_frame_plan.h"

#include <string>
#include <utility>
#include <vector>

using namespace cordmux::sources;

TEST_CASE("自分の枠に映すソースの候補: 映像を持つ入力のソースで、cordmux の種類でないものだけ")
{
	CHECK(self_frame_source_eligible(true, "dshow_input"));
	CHECK(self_frame_source_eligible(true, "game_capture"));
	// 映像を持たないソース (マイクなど) は描けない。
	CHECK_FALSE(self_frame_source_eligible(false, "wasapi_input_capture"));
	// cordmux の種類を選ぶと、合成のソースが自分自身を描く循環ができる。
	CHECK_FALSE(self_frame_source_eligible(true, "cordmux_composite"));
	CHECK_FALSE(self_frame_source_eligible(true, "cordmux_tile"));
}

TEST_CASE("自分の枠に映すソースを探す: UUID で見つかり、設定と同じなら書き直さない")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "キャプチャーボード", true},
		{"u-2", "ゲーム画面", true},
	};
	const SelfFrameLookup found = lookup_self_frame(all, "u-2", "ゲーム画面");
	CHECK(found.index == 1);
	CHECK(found.by_uuid);
	CHECK_FALSE(found.rewrite);
}

TEST_CASE("自分の枠に映すソースを探す: UUID で見つかり、名前が変わっていれば名前を書き直す")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "新しい名前", true},
	};
	const SelfFrameLookup found = lookup_self_frame(all, "u-1", "古い名前");
	CHECK(found.index == 0);
	CHECK(found.by_uuid);
	CHECK(found.rewrite);
	CHECK(found.uuid == "u-1");
	CHECK(found.name == "新しい名前");
}

TEST_CASE("自分の枠に映すソースを探す: UUID が変わっていても名前で見つかり、UUID を書き直す")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "キャプチャーボード", true},
		{"u-9", "ゲーム画面", true},
	};
	const SelfFrameLookup found = lookup_self_frame(all, "u-old", "ゲーム画面");
	CHECK(found.index == 1);
	CHECK_FALSE(found.by_uuid);
	CHECK(found.rewrite);
	CHECK(found.uuid == "u-9");
	CHECK(found.name == "ゲーム画面");
}

TEST_CASE("自分の枠に映すソースを探す: UUID が空なら名前だけで探し、UUID を書き直す")
{
	const std::vector<SelfSourceCandidate> all = {{"u-1", "キャプチャーボード", true}};
	const SelfFrameLookup found = lookup_self_frame(all, "", "キャプチャーボード");
	CHECK(found.index == 0);
	CHECK_FALSE(found.by_uuid);
	CHECK(found.rewrite);
	CHECK(found.uuid == "u-1");
}

TEST_CASE("自分の枠に映すソースを探す: 見つからない形では何も書き直さない")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "キャプチャーボード", true},
		{"u-2", "cordmux", false}, // 候補にしない種類は、UUID も名前も合っていても見つからない
	};
	for (const SelfFrameLookup &missing :
	     {lookup_self_frame(all, "u-9", "存在しない"), lookup_self_frame(all, "", ""), lookup_self_frame(all, "u-2", ""),
	      lookup_self_frame(all, "", "cordmux"), lookup_self_frame({}, "u-1", "キャプチャーボード")}) {
		CHECK(missing.index == -1);
		CHECK_FALSE(missing.by_uuid);
		CHECK_FALSE(missing.rewrite);
		CHECK(missing.uuid.empty());
		CHECK(missing.name.empty());
	}
}

TEST_CASE("自分の枠に映すソースを探す: シーンも UUID と名前で見つかる")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "キャプチャーボード", true, false},
		{"u-5", "ゲーム用", true, true},
	};
	const SelfFrameLookup by_uuid = lookup_self_frame(all, "u-5", "ゲーム用");
	CHECK(by_uuid.index == 1);
	CHECK_FALSE(by_uuid.rewrite);
	const SelfFrameLookup by_name = lookup_self_frame(all, "", "ゲーム用");
	CHECK(by_name.index == 1);
	CHECK(by_name.uuid == "u-5");
}

TEST_CASE("自分の枠に映すソースを探す: OBS で削除したソースは、UUID も名前も合っていても見つからない")
{
	// 削除したソースは、参照が残る間は OBS の列挙に出続ける。見つけると、削除したソースを自分の枠に描き続ける。
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "ゲーム画面", true, false, true},
		{"u-2", "キャプチャーボード", true, true, true},
	};
	CHECK(lookup_self_frame(all, "u-1", "ゲーム画面").index == -1);
	CHECK(lookup_self_frame(all, "", "ゲーム画面").index == -1);
	CHECK(lookup_self_frame(all, "u-2", "キャプチャーボード").index == -1);
	CHECK_FALSE(usable_self_source(all[0]));

	// 同じ名前で作り直したソースは、削除したソースより後に列挙されても見つかる (名前で探し、UUID を書き直す)。
	const std::vector<SelfSourceCandidate> recreated = {
		{"u-1", "ゲーム画面", true, false, true},
		{"u-3", "ゲーム画面", true, false, false},
	};
	const SelfFrameLookup found = lookup_self_frame(recreated, "u-1", "ゲーム画面");
	CHECK(found.index == 1);
	CHECK_FALSE(found.by_uuid);
	CHECK(found.rewrite);
	CHECK(found.uuid == "u-3");
}

namespace {

SceneNode node(const char *type_id, std::vector<SceneNode> children = {})
{
	SceneNode n;
	n.type_id = type_id;
	n.children = std::move(children);
	return n;
}

SceneNode truncated_node()
{
	SceneNode n;
	n.type_id = "scene";
	n.truncated = true;
	return n;
}

} // namespace

TEST_CASE("自分の枠に映すシーン: cordmux の合成のソースを直接含むシーンは候補にしない")
{
	CHECK_FALSE(self_frame_scene_eligible(false, node("scene", {node("dshow_input"), node("cordmux_composite")})));
	CHECK_FALSE(self_frame_scene_eligible(false, node("scene", {node("cordmux_tile")})));
}

TEST_CASE("自分の枠に映すシーン: 入れ子のシーンとグループの中にあっても候補にしない")
{
	const SceneNode nested = node("scene", {node("scene", {node("scene", {node("cordmux_composite")})})});
	CHECK_FALSE(self_frame_scene_eligible(false, nested));
	const SceneNode in_group = node("scene", {node("dshow_input"), node("group", {node("cordmux_composite")})});
	CHECK_FALSE(self_frame_scene_eligible(false, in_group));
	CHECK(scene_contains_cordmux(in_group));
}

TEST_CASE("自分の枠に映すシーン: cordmux を含まないシーンは候補にする。空のシーンも同じ")
{
	CHECK(self_frame_scene_eligible(false, node("scene")));
	CHECK(self_frame_scene_eligible(false,
					node("scene", {node("dshow_input"), node("group", {node("game_capture")})})));
	CHECK(self_frame_scene_eligible(false, node("scene", {node("scene", {node("image_source")})})));
}

TEST_CASE("自分の枠に映すシーン: グループそのものは候補にしない。展開を打ち切った所は cordmux があるものと扱う")
{
	CHECK_FALSE(self_frame_scene_eligible(true, node("group", {node("dshow_input")})));
	CHECK_FALSE(self_frame_scene_eligible(false, node("scene", {truncated_node()})));
}

namespace {

// 周期の処理の入力。映像は OBS のソースを選んでいて結び付いている。載せる人の選び方は手動で、[自動で載せる] はなし。
SelfFrameInput poll(bool on, SelfShare prev, SelfShare share)
{
	SelfFrameInput in;
	in.state.on = on;
	in.state.prev = prev;
	in.share = share;
	in.selected = true;
	in.bound = true;
	return in;
}

SelfFrameInput poll_auto(bool on, SelfShare prev, SelfShare share)
{
	SelfFrameInput in = poll(on, prev, share);
	in.automatic = true;
	return in;
}

SelfFrameInput poll_always_on(bool on, SelfShare prev, SelfShare share)
{
	SelfFrameInput in = poll(on, prev, share);
	in.always_on = true;
	return in;
}

// 状態だけを引き継いで、次の入力を作る。
SelfFrameInput next(SelfFrameInput in, const SelfFrameResult &previous, SelfShare share)
{
	in.state = previous.state;
	in.share = share;
	in.event = SelfFrameEvent::Poll;
	return in;
}

} // namespace

TEST_CASE("自分の枠の状態: 手動で [自動で載せる] なしなら、共有を始めても付かない")
{
	for (const SelfShare prev : {SelfShare::Unknown, SelfShare::Off}) {
		const SelfFrameResult r = plan_self_frame(poll(false, prev, SelfShare::On));
		CHECK_FALSE(r.state.on);
		CHECK_FALSE(r.shown);
		CHECK_FALSE(r.persist);
		CHECK(r.state.prev == SelfShare::On);
	}
	// 共有の間ずっとでも付かない。
	const SelfFrameResult r = plan_self_frame(poll(false, SelfShare::On, SelfShare::On));
	CHECK_FALSE(r.state.on);
}

TEST_CASE("自分の枠の状態: 自動なら、共有の始まりで付く。on の変化は書き換えの対象になる")
{
	for (const SelfShare prev : {SelfShare::Unknown, SelfShare::Off}) {
		const SelfFrameResult r = plan_self_frame(poll_auto(false, prev, SelfShare::On));
		CHECK(r.state.on);
		CHECK(r.shown);
		CHECK(r.persist);
		CHECK_FALSE(r.state.excluded);
		CHECK_FALSE(r.deferred);
	}
	// すでに on なら何も変わらず、書き換えない。
	const SelfFrameResult same = plan_self_frame(poll_auto(true, SelfShare::On, SelfShare::On));
	CHECK(same.state.on);
	CHECK_FALSE(same.persist);
}

TEST_CASE("自分の枠の状態: [自動で載せる] が付いていれば、手動でも共有の始まりで付く")
{
	for (const SelfShare prev : {SelfShare::Unknown, SelfShare::Off}) {
		const SelfFrameResult r = plan_self_frame(poll_always_on(false, prev, SelfShare::On));
		CHECK(r.state.on);
		CHECK(r.persist);
	}
	// 始まりだけを見る。共有を続けている間に外れていても、付け直さない。
	const SelfFrameResult r = plan_self_frame(poll_always_on(false, SelfShare::On, SelfShare::On));
	CHECK_FALSE(r.state.on);
	CHECK_FALSE(r.persist);
}

TEST_CASE("自分の枠の状態: 自動では、共有の間ずっと、枠が空けば付く")
{
	SelfFrameInput in = poll_auto(false, SelfShare::On, SelfShare::On);
	in.discord_slotted = 4;
	SelfFrameResult r = plan_self_frame(in);
	CHECK_FALSE(r.state.on);
	CHECK(r.deferred);
	in.discord_slotted = 3;
	r = plan_self_frame(in);
	CHECK(r.state.on);
	CHECK_FALSE(r.deferred);
}

TEST_CASE("自分の枠の状態: ドックで付けると on が真、excluded が偽になり、書き換える。外すと逆")
{
	SelfFrameInput in;
	in.event = SelfFrameEvent::DockOn;
	in.state.excluded = true;
	SelfFrameResult r = plan_self_frame(in);
	CHECK(r.state.on);
	CHECK_FALSE(r.state.excluded);
	CHECK(r.persist);

	in.event = SelfFrameEvent::DockOff;
	in.state.on = true;
	in.state.excluded = false;
	r = plan_self_frame(in);
	CHECK_FALSE(r.state.on);
	CHECK(r.state.excluded);
	CHECK(r.persist);

	// すでに on が偽なら書き換えない。
	in.state.on = false;
	r = plan_self_frame(in);
	CHECK_FALSE(r.persist);
}

TEST_CASE("自分の枠の状態: Pipeline が断ると on が偽になり、excluded は変えず、書き換える")
{
	// ドックで付けた直後に断られた。外したのではないので、excluded にはしない。
	SelfFrameInput in = poll_auto(true, SelfShare::On, SelfShare::On);
	in.event = SelfFrameEvent::Refused;
	SelfFrameResult r = plan_self_frame(in);
	CHECK_FALSE(r.state.on);
	CHECK_FALSE(r.state.excluded);
	CHECK_FALSE(r.shown);
	CHECK(r.persist);
	CHECK(r.state.prev == SelfShare::On);
	CHECK_FALSE(r.deferred);

	// 自動では、次の周期に枠が埋まっていれば見送り、空けば付く (見送ったときと同じ扱い)。
	SelfFrameInput after = next(in, r, SelfShare::On);
	after.discord_slotted = 4;
	r = plan_self_frame(after);
	CHECK_FALSE(r.state.on);
	CHECK(r.deferred);
	after = next(after, r, SelfShare::On);
	after.discord_slotted = 3;
	r = plan_self_frame(after);
	CHECK(r.state.on);

	// [自動で載せる] で共有の始まりに付いた分が断られたときは、始め直すまで付かない (見送ったときと同じ)。
	SelfFrameInput always = poll_always_on(false, SelfShare::Off, SelfShare::On);
	r = plan_self_frame(always);
	REQUIRE(r.state.on);
	always.state = r.state;
	always.event = SelfFrameEvent::Refused;
	r = plan_self_frame(always);
	CHECK_FALSE(r.state.on);
	CHECK_FALSE(r.state.excluded);
	r = plan_self_frame(next(always, r, SelfShare::On));
	CHECK_FALSE(r.state.on);

	// 外した後に断られても (起きないが)、excluded は残る。
	in.state.excluded = true;
	CHECK(plan_self_frame(in).state.excluded);
}

TEST_CASE("自分の枠の状態: ドックの出来事は prev を変えない")
{
	SelfFrameInput in;
	in.state.prev = SelfShare::Off;
	in.event = SelfFrameEvent::DockOn;
	CHECK(plan_self_frame(in).state.prev == SelfShare::Off);
	in.event = SelfFrameEvent::DockOff;
	CHECK(plan_self_frame(in).state.prev == SelfShare::Off);
}

TEST_CASE("自分の枠の状態: 自動で外した後は、共有をやめて始め直すまで付かない")
{
	const SelfFrameInput base = poll_auto(true, SelfShare::On, SelfShare::On);
	SelfFrameInput in = base;
	in.event = SelfFrameEvent::DockOff;
	SelfFrameResult r = plan_self_frame(in);
	CHECK_FALSE(r.state.on);
	CHECK(r.state.excluded);
	// 共有を続けている間は、枠が空いていても付かない。
	r = plan_self_frame(next(base, r, SelfShare::On));
	CHECK_FALSE(r.state.on);
	CHECK(r.state.excluded);
	// 共有をやめる。on は変えず、excluded だけ外れる。
	r = plan_self_frame(next(base, r, SelfShare::Off));
	CHECK_FALSE(r.state.on);
	CHECK_FALSE(r.state.excluded);
	CHECK_FALSE(r.persist);
	// 始め直すと付く。
	r = plan_self_frame(next(base, r, SelfShare::On));
	CHECK(r.state.on);
	CHECK(r.persist);
}

TEST_CASE("自分の枠の状態: [自動で載せる] でも、外した後は始め直すまで付かない")
{
	const SelfFrameInput base = poll_always_on(false, SelfShare::Off, SelfShare::On);
	SelfFrameResult r = plan_self_frame(base);
	CHECK(r.state.on);
	SelfFrameInput in = base;
	in.state = r.state;
	in.event = SelfFrameEvent::DockOff;
	r = plan_self_frame(in);
	CHECK(r.state.excluded);
	// 共有を続けている間は、始まりでもないので付かない。
	r = plan_self_frame(next(base, r, SelfShare::On));
	CHECK_FALSE(r.state.on);
	// 共有をやめて、始め直すと付く。
	r = plan_self_frame(next(base, r, SelfShare::Off));
	CHECK_FALSE(r.state.excluded);
	r = plan_self_frame(next(base, r, SelfShare::On));
	CHECK(r.state.on);
}

TEST_CASE("自分の枠の状態: 共有の終わりで外れない。ドックで付けた分も、共有が付けた分も残る")
{
	for (const bool automatic : {false, true}) {
		SelfFrameInput in = poll(true, SelfShare::On, SelfShare::Off);
		in.automatic = automatic;
		const SelfFrameResult r = plan_self_frame(in);
		CHECK(r.state.on);
		CHECK(r.shown);
		CHECK_FALSE(r.persist);
		CHECK(r.state.prev == SelfShare::Off);
	}
}

TEST_CASE("自分の枠の状態: 枠が埋まっていれば付かず、見送ったことが分かる。押しのけない")
{
	SelfFrameInput in = poll_auto(false, SelfShare::Off, SelfShare::On);
	in.discord_slotted = 4;
	const SelfFrameResult r = plan_self_frame(in);
	CHECK_FALSE(r.state.on);
	CHECK(r.deferred);
	CHECK_FALSE(r.persist);
	in = poll_always_on(false, SelfShare::Off, SelfShare::On);
	in.discord_slotted = 4;
	CHECK(plan_self_frame(in).deferred);
	// 3 人なら、自分の枠と合わせて 4 つなので付く。
	in.discord_slotted = 3;
	CHECK(plan_self_frame(in).state.on);
}

TEST_CASE("自分の枠の状態: 共有が分からない間は、状態も prev も変えない")
{
	for (const bool on : {false, true}) {
		for (const bool excluded : {false, true}) {
			SelfFrameInput in = poll_auto(on, SelfShare::On, SelfShare::Unknown);
			in.state.excluded = excluded;
			const SelfFrameResult r = plan_self_frame(in);
			CHECK(r.state.on == on);
			CHECK(r.state.excluded == excluded);
			CHECK(r.state.prev == SelfShare::On);
			CHECK_FALSE(r.persist);
		}
	}
}

TEST_CASE("自分の枠の状態: 映像を OBS から選んでいない間は on を変えないが、prev と excluded は進める")
{
	SelfFrameInput in = poll_auto(false, SelfShare::Off, SelfShare::On);
	in.selected = false;
	SelfFrameResult r = plan_self_frame(in);
	CHECK_FALSE(r.state.on);
	CHECK_FALSE(r.persist);
	CHECK(r.state.prev == SelfShare::On);
	// on が真のままなら、そのまま残る (選び直したときに適用される)。
	in = poll_auto(true, SelfShare::On, SelfShare::Off);
	in.selected = false;
	in.state.excluded = true;
	r = plan_self_frame(in);
	CHECK(r.state.on);
	CHECK_FALSE(r.state.excluded);
	CHECK_FALSE(r.persist);
}

TEST_CASE("自分の枠の状態: 見せるのは、映像が結び付いていて on のときだけ")
{
	SelfFrameInput in = poll_auto(true, SelfShare::On, SelfShare::On);
	in.bound = false;
	CHECK_FALSE(plan_self_frame(in).shown);
	in.bound = true;
	CHECK(plan_self_frame(in).shown);
	in.state.on = false;
	in.automatic = false;
	CHECK_FALSE(plan_self_frame(in).shown);
}

TEST_CASE("自分の枠を載せた順の印は、チェックが付いていて自分の枠が映り始めた時に決め、外すまで変えない")
{
	SelfOrderMark mark;
	// チェックが付いていても、映っていない間 (映像が「Discordの画面共有」の間、ソースが見つからない間) は決めない。
	mark = next_self_order_mark(mark, true, false, 3);
	CHECK_FALSE(mark.decided);
	// 映り始めた時に、その時に載っていた人の番号の最大に決める。
	mark = next_self_order_mark(mark, true, true, 5);
	CHECK(mark.decided);
	CHECK(mark.mark == 5);
	// 後から人が載っても、一時的に映らなくなっても、チェックが付いている間は変えない。
	mark = next_self_order_mark(mark, true, true, 9);
	CHECK(mark.mark == 5);
	mark = next_self_order_mark(mark, true, false, 9);
	mark = next_self_order_mark(mark, true, true, 12);
	CHECK(mark.decided);
	CHECK(mark.mark == 5);
	// チェックを外すと決め直す状態に戻り、付け直して映った時の最大になる。
	mark = next_self_order_mark(mark, false, false, 12);
	CHECK_FALSE(mark.decided);
	mark = next_self_order_mark(mark, true, true, 12);
	CHECK(mark.mark == 12);
}

TEST_CASE("OBS を起動したときからチェックが付いていて、ほかの人より先に映れば、自分の枠を載せた順の印は 0 になる")
{
	// 割り当て表は起動のたびに空から始まる (番号の最大は 0)。
	SelfOrderMark mark = next_self_order_mark(SelfOrderMark{}, true, true, 0);
	CHECK(mark.decided);
	CHECK(mark.mark == 0);
	// 起動したときから付いていても、映る前にほかの人が載っていれば、その人たちの後になる。
	mark = next_self_order_mark(SelfOrderMark{}, true, false, 0);
	mark = next_self_order_mark(mark, true, true, 2);
	CHECK(mark.mark == 2);
}

TEST_CASE("メインアカウントの画面共有から自分の枠にチェックを引き継ぐときは、メインアカウントの枠がいた位置を印にする")
{
	const SelfOrderMark mark = self_order_mark_from_share(3);
	CHECK(mark.decided);
	// 番号 3 の人がいた位置: 番号 2 の人の後、番号 4 の人の前。
	CHECK(mark.mark == 2);
	CHECK(mark.mark * 2 + 1 > 2 * 2);
	CHECK(mark.mark * 2 + 1 < 4 * 2);
	// 引き継いだ印は、映っていない間もチェックが付いている間は変えない。
	CHECK(next_self_order_mark(mark, true, false, 9).mark == 2);
	CHECK(self_order_mark_from_share(0).mark == 0);
}

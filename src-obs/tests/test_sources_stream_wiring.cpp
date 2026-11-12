// SPDX-License-Identifier: Apache-2.0

// 受信した画面共有を配信の画面に反映する処理のうち、純粋関数の部分のテスト。枠を空けるタイミング
// (sources/screen_end.h)、自動配置が数える枠 (pipeline/pipeline.h)、設定から見た目の値を作る処理
// (sources/decor_params.h)、録画ファイルの名前に使う語 (pipeline/pipeline.h の record_name_words) を確認する。

#include "doctest.h"

#include "config/settings.h"
#include "pipeline/pipeline.h"
#include "pipeline/stream_router.h"
#include "sources/decor_params.h"
#include "sources/screen_end.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using cordmux::config::Settings;
using cordmux::config::SlotKind;
using cordmux::pipeline::mark_share_present;
using cordmux::pipeline::record_name_words;
using cordmux::pipeline::RecordNameWords;
using cordmux::pipeline::Route;
using cordmux::pipeline::RouteChange;
using cordmux::pipeline::screen_slots_share_ended;
using cordmux::pipeline::SlotSnapshot;
using cordmux::pipeline::visible_screen_slots;
using cordmux::sources::decor_label_params_from_settings;
using cordmux::sources::DecorLabelParams;
using cordmux::sources::screen_end_action;
using cordmux::sources::ScreenEndAction;
using cordmux::sources::ScreenEndEvent;

namespace {

Route screen_route(int slot, const std::string &user_id)
{
	return Route{SlotKind::Screen, slot, user_id};
}

// allowed は、映像も音声も出してよいこと (collect_slot_snapshots が
// AllocationTable から値をコピーするときと同じ組み合わせ)。
SlotSnapshot screen_row(int slot, const std::string &owner, bool allowed)
{
	SlotSnapshot row;
	row.kind = SlotKind::Screen;
	row.slot = slot;
	row.owner_user_id = owner;
	row.voice_user_id = owner;
	row.video_allowed = allowed;
	row.output_allowed = allowed;
	return row;
}

} // namespace

// ---- 枠を空けるタイミング ----

TEST_CASE("画面共有が終わったとき、通話から抜けたとき、枠から外したときは、その場ですぐ枠を空けて並べ直す")
{
	for (const ScreenEndEvent event :
	     {ScreenEndEvent::ShareEnded, ScreenEndEvent::LeftCall, ScreenEndEvent::RemovedFromSlot}) {
		const ScreenEndAction action = screen_end_action(event);
		CHECK(action.clear_now);
		CHECK_FALSE(action.hold_then_clear);
		CHECK(action.relayout);
		CHECK(action.clear_owner); // 戻らない人を、空いた枠の名前ラベルに描く人のままにしない
	}
}

TEST_CASE("Vesktop との接続かハートビートが切れたら、最後のフレームをしばらく表示してから空け、並べ直さない")
{
	const ScreenEndAction action = screen_end_action(ScreenEndEvent::LinkLost);
	CHECK(action.hold_then_clear);
	CHECK_FALSE(action.clear_now);
	CHECK_FALSE(action.relayout);
	CHECK_FALSE(action.clear_owner); // 再接続して同じ人が画面共有し直せば、同じ人が載ったまま、また映る
}

TEST_CASE("フレームが来ないだけでは枠を空けず、並べ直しもしない")
{
	const ScreenEndAction action = screen_end_action(ScreenEndEvent::FramesStopped);
	CHECK_FALSE(action.clear_now);
	CHECK_FALSE(action.hold_then_clear);
	CHECK_FALSE(action.relayout);
	CHECK_FALSE(action.clear_owner);
}

TEST_CASE("受信をやり直しても、枠を空けず、最後のフレームと名前ラベルを残し、並べ直さない")
{
	const ScreenEndAction action = screen_end_action(ScreenEndEvent::ReceiveRestarted);
	CHECK_FALSE(action.clear_now);
	CHECK_FALSE(action.hold_then_clear);
	CHECK_FALSE(action.relayout);
	CHECK_FALSE(action.clear_owner);
}

TEST_CASE("ダミー表示を止めたら、ダミーの画像を残さないように枠を空ける")
{
	const ScreenEndAction action = screen_end_action(ScreenEndEvent::DummyStopped);
	CHECK(action.clear_now);
	CHECK_FALSE(action.hold_then_clear);
	CHECK_FALSE(action.clear_owner); // ダミー表示の間も、枠に載っている人は実際の人のまま
}

TEST_CASE("configure を適用した後に画面の経路がなくなった枠だけを、画面共有が終わった枠とする")
{
	// 枠 1 の人は画面共有をやめた (ストリームが消えた)。枠 2 は同じ人が別の stream_id で画面共有し直し、
	// 枠 3 は別の人が取り上げた。
	std::vector<RouteChange> changes;
	changes.push_back(RouteChange{10, screen_route(1, "100"), std::nullopt, "removed"});
	changes.push_back(RouteChange{11, screen_route(2, "200"), std::nullopt, "removed"});
	changes.push_back(RouteChange{21, std::nullopt, screen_route(2, "200"), "already_owned"});
	changes.push_back(RouteChange{12, screen_route(3, "300"), std::nullopt, "removed"});
	changes.push_back(RouteChange{22, std::nullopt, screen_route(3, "400"), "assigned"});
	// 声の経路が消えても画面の枠は空けない。枠 0 (枠から外した人の経路) も数えない。
	changes.push_back(RouteChange{13, Route{SlotKind::Voice, 1, "100"}, std::nullopt, "removed"});
	changes.push_back(RouteChange{14, screen_route(0, "500"), std::nullopt, "removed"});

	const std::set<int> live_after = {2, 3};
	CHECK(screen_slots_share_ended(changes, live_after) == std::set<int>{1});
}

TEST_CASE("再接続した後の最初の configure (前の経路がない) では、どの枠も空けない")
{
	std::vector<RouteChange> changes;
	changes.push_back(RouteChange{1, std::nullopt, screen_route(1, "100"), "assigned"});
	CHECK(screen_slots_share_ended(changes, {1}).empty());
}

TEST_CASE("自動配置が数える枠は、映像を出してよく、載っている人の画面共有のストリームがいまある枠だけ")
{
	std::vector<SlotSnapshot> slots = {screen_row(1, "100", true), screen_row(2, "200", true),
					   screen_row(3, "300", false)};
	SlotSnapshot voice;
	voice.kind = SlotKind::Voice;
	voice.slot = 1;
	voice.owner_user_id = "100";
	voice.output_allowed = true;
	slots.push_back(voice);

	// 枠 2 の人は画面共有をやめた (割り当ては残る)。枠 3 は映像を出していない (allowed が偽)。
	mark_share_present(slots, {1, 3});
	CHECK(slots[0].share_present);
	CHECK_FALSE(slots[1].share_present);
	CHECK(slots[2].share_present);
	CHECK_FALSE(slots[3].share_present); // 声の枠では使わない
	CHECK(visible_screen_slots(slots) == std::vector<int>{1});
}

TEST_CASE("声の枠の行の voice_present は、いま声の経路がある枠だけが真で、画面の枠では使わない")
{
	std::vector<SlotSnapshot> slots = {screen_row(1, "100", true)};
	for (int slot = 1; slot <= 2; ++slot) {
		SlotSnapshot voice;
		voice.kind = SlotKind::Voice;
		voice.slot = slot;
		voice.owner_user_id = slot == 1 ? "100" : "200";
		slots.push_back(voice);
	}
	// 声の枠 2 の人は通話から抜けた (割り当ては残る)。
	cordmux::pipeline::mark_voice_present(slots, {1});
	CHECK_FALSE(slots[0].voice_present);
	CHECK(slots[1].voice_present);
	CHECK_FALSE(slots[2].voice_present);
	// 画面の枠の share_present とは別の値。
	cordmux::pipeline::mark_share_present(slots, {1});
	CHECK(slots[0].share_present);
	CHECK_FALSE(slots[1].share_present);
	CHECK(slots[1].voice_present);
}

TEST_CASE("Vesktop との接続が切れても、切れたときに映っていた枠は並べ直さずに数え、映像を空けた後は全画面にできる人から除く")
{
	using cordmux::pipeline::link_lost_emptied_owners;
	using cordmux::pipeline::link_lost_layout_owners;
	using cordmux::pipeline::LinkLostLayoutHold;
	using cordmux::pipeline::mark_link_lost_hold;
	constexpr std::int64_t kSec = 1000000;
	constexpr std::int64_t kWindow = 10 * kSec;

	LinkLostLayoutHold hold;
	hold.owners = {{1, "100"}, {2, "200"}};
	hold.lost_at_us = 5 * kSec;

	// 切れた後は経路がない (share_present が偽) が、同じ人が載っている枠は数える。枠 3 は切れる前から映っていなかった。
	std::vector<SlotSnapshot> slots = {screen_row(1, "100", true), screen_row(2, "200", true),
					   screen_row(3, "300", true)};
	mark_share_present(slots, {});
	mark_link_lost_hold(slots, link_lost_layout_owners(hold, 6 * kSec, kWindow), false);
	CHECK(visible_screen_slots(slots) == std::vector<int>{1, 2});
	CHECK(link_lost_emptied_owners(slots).empty()); // 最後のフレームを残している間は、全画面を続けられる

	// 最後のフレームを残す長さがたった後も配置は保つが、全画面にできる人からは除く。
	mark_link_lost_hold(slots, link_lost_layout_owners(hold, 60 * kSec, kWindow), true);
	CHECK(visible_screen_slots(slots) == std::vector<int>{1, 2});
	CHECK(link_lost_emptied_owners(slots) == std::set<std::string>{"100", "200"});

	// 切れている間に枠 2 が空いて別の人が載ったら、その枠は保たない。
	slots[1].owner_user_id = "400";
	mark_link_lost_hold(slots, link_lost_layout_owners(hold, 60 * kSec, kWindow), true);
	CHECK(visible_screen_slots(slots) == std::vector<int>{1});

	// 接続し直した後、映像が戻った枠は経路で数え、戻らない枠は window の間だけ保つ。
	slots[1].owner_user_id = "200";
	hold.reconnected_at_us = 70 * kSec;
	mark_share_present(slots, {1});
	mark_link_lost_hold(slots, link_lost_layout_owners(hold, 75 * kSec, kWindow), true);
	CHECK(visible_screen_slots(slots) == std::vector<int>{1, 2});
	CHECK(link_lost_emptied_owners(slots) == std::set<std::string>{"200"}); // 映像が戻った人は除かない
	mark_link_lost_hold(slots, link_lost_layout_owners(hold, 80 * kSec, kWindow), true);
	CHECK(visible_screen_slots(slots) == std::vector<int>{1});
	CHECK(link_lost_emptied_owners(slots).empty());
}

// ---- 設定から見た目の値を作る ----

TEST_CASE("既定の設定から作った見た目の値は、DecorLabelParams の既定値と同じになる")
{
	const Settings settings;
	const DecorLabelParams from_settings = decor_label_params_from_settings(settings);
	const DecorLabelParams defaults;
	CHECK(from_settings.em_ratio == doctest::Approx(defaults.em_ratio));
	CHECK(from_settings.plate_argb == defaults.plate_argb); // #000000 の 50% = 0x80000000
	CHECK(from_settings.text_argb == defaults.text_argb);
	CHECK(from_settings.border_light_argb == defaults.border_light_argb);
	CHECK(from_settings.border_rise_ms == defaults.border_rise_ms);
	CHECK(from_settings.border_hold_ms == defaults.border_hold_ms);
	CHECK(from_settings.border_fade_ms == defaults.border_fade_ms);
	CHECK(from_settings.font_weight == defaults.font_weight);
	CHECK(from_settings.font_family == defaults.font_family);
	CHECK(from_settings.font_italic == defaults.font_italic);
	CHECK(from_settings.show_name == defaults.show_name);
	CHECK(from_settings.show_avatar == defaults.show_avatar);
	CHECK(from_settings.show_speaking == defaults.show_speaking);
}

TEST_CASE("利用者の値はそのまま使い、割合は 1/100 にし、不透明度は背景の色にだけかける")
{
	Settings settings;
	settings.label_em_ratio_pct = 6.5;
	settings.label_plate_color = "#102030";
	settings.label_plate_opacity_pct = 50;
	settings.label_text_color = "#FFEEDD";
	settings.label_border_color = "#3bd67f";
	settings.label_font_family = "Yu Gothic UI";
	settings.label_font_weight = 350; // フォントを選ぶダイアログが返すことがある、100 刻みでない値 (Semilight)
	settings.label_show_avatar = false;
	settings.label_show_speaking = false;
	settings.label_border_rise_ms = 120;

	const DecorLabelParams p = decor_label_params_from_settings(settings);
	CHECK(p.em_ratio == doctest::Approx(0.065));
	CHECK(p.plate_argb == 0x80102030u); // 50% = 128 (四捨五入)
	CHECK(p.text_argb == 0xFFFFEEDDu);  // 文字の色には不透明度をかけない
	CHECK(p.border_light_argb == 0xFF3BD67Fu);
	CHECK(p.font_family == "Yu Gothic UI");
	CHECK(p.font_weight == 350);
	CHECK_FALSE(p.font_italic); // 斜体は利用者に選ばせない (設定のキーがない)
	CHECK(p.show_name);
	CHECK_FALSE(p.show_avatar);
	CHECK_FALSE(p.show_speaking);
	CHECK(p.border_rise_ms == 120);

	settings.label_font_weight = 950;
	CHECK(decor_label_params_from_settings(settings).font_weight == 950);
	settings.label_plate_opacity_pct = 0;
	CHECK((decor_label_params_from_settings(settings).plate_argb >> 24) == 0u);
	settings.label_plate_opacity_pct = 100;
	CHECK((decor_label_params_from_settings(settings).plate_argb >> 24) == 0xFFu);
}

TEST_CASE("形の崩れた色は既定の色で描画する (設定を読むときに弾くので、来ないはずの値)")
{
	Settings settings;
	settings.label_border_color = "57F287";
	settings.label_plate_color = "#12345G";
	const DecorLabelParams p = decor_label_params_from_settings(settings);
	CHECK(p.border_light_argb == 0xFF57F287u);
	CHECK((p.plate_argb & 0x00FFFFFFu) == 0x000000u);
}

// ---- 録画ファイルの名前に使う語と書式 ----

TEST_CASE("録画ファイルの名前に使う語と書式は ini の 3 つのキーから引き、録画の側の英語の既定値にならない")
{
	// ini の文字列の代わり (ja-JP.ini の文字列の形)。引かれたキーを数える。
	const std::map<std::string, std::string> ini = {
		{cordmux::ledger::text::keys::kSourceFileScreenShareLabel, "画面共有"},
		{cordmux::ledger::text::keys::kSourceFileVoiceLabel, "声"},
		{cordmux::ledger::text::keys::kSourceFileNameLabel, "%1 %2 %3"},
	};
	std::vector<std::string> asked;
	const RecordNameWords words = record_name_words([&](const char *key) -> const char * {
		asked.emplace_back(key);
		const auto found = ini.find(key);
		return found == ini.end() ? nullptr : found->second.c_str();
	});
	CHECK(words.screen_kind_word == "画面共有");
	CHECK(words.voice_kind_word == "声");
	CHECK(words.output_name_format == "%1 %2 %3");
	CHECK(asked.size() == 3);
	// 録画の側の既定値 (record_types.h) と同じ文字列ではない (コンストラクターで埋めた後は、既定値にならない)。
	CHECK(std::strcmp(words.screen_kind_word.c_str(), cordmux::record::kDefaultScreenKindWord) != 0);
	CHECK(std::strcmp(words.voice_kind_word.c_str(), cordmux::record::kDefaultVoiceKindWord) != 0);
}

TEST_CASE("obs_module_text と同じく、キーがなければキーそのものを返す関数でも、3 つとも空にならない")
{
	// obs_module_text は、ini にキーがないとキーの文字列を返す (空を返さない)。空の語は録画の側の既定値 (英語) を
	// 選ばせるが、製品の処理の流れ (コンストラクターで obs_module_text を渡す) では既定値にならない。
	const RecordNameWords words = record_name_words([](const char *key) { return key; });
	CHECK_FALSE(words.screen_kind_word.empty());
	CHECK_FALSE(words.voice_kind_word.empty());
	CHECK_FALSE(words.output_name_format.empty());
}

TEST_CASE("文字列を引く関数がないときだけ空になり、そのときは録画の側の既定値を使う")
{
	const RecordNameWords words = record_name_words(nullptr);
	CHECK(words.screen_kind_word.empty());
	CHECK(words.voice_kind_word.empty());
	CHECK(words.output_name_format.empty());
}

TEST_CASE("member_change は、新しい一覧の数と、増えた数と、減った数を返す (出来事の行に、参加者の出入りとストリームの追加と終了を書く)")
{
	using cordmux::pipeline::member_change;
	using cordmux::pipeline::MemberChange;
	const MemberChange c = member_change(std::vector<std::string>{"a", "b", "c"}, std::vector<std::string>{"b", "c", "d", "e"});
	CHECK(c.now == 4);
	CHECK(c.added == 2);
	CHECK(c.removed == 1);
	CHECK(c.changed());
	// 並びが替わっただけでは変わっていない。
	CHECK_FALSE(member_change(std::vector<std::uint32_t>{1, 2}, std::vector<std::uint32_t>{2, 1}).changed());
	// 空から始まると、全員が増えた数になる。
	CHECK(member_change(std::vector<std::uint32_t>{}, std::vector<std::uint32_t>{5, 6}).added == 2);
}

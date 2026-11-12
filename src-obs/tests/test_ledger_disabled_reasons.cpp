// SPDX-License-Identifier: Apache-2.0

// 押せない理由のマーカー (ledger/actions.h の reason) が decide_dock() の View に残らないことと、メッセージのボタンが
// その状態で押せることを確認する。マーカーは押せない理由を画面に表示しない操作のためのもので、View に残ると、
// キーの形の文字列が画面に表示されることがある。状態を広く作り、View のすべてのテキストにマーカーがないことを見る。

#include "doctest.h"

#include "ledger/actions.h"
#include "ledger/rules.h"
#include "ledger_dock_test_helpers.h"

#include <string>
#include <vector>

using namespace ledger_dock_test;

namespace {

bool is_reason_tag(const std::string &id)
{
	return id.rfind("disabled.", 0) == 0;
}

// 理由のマーカーが出やすい状態を広く作る。人の数と空いている枠、Vesktop のアカウント、使う声の設定の組、名前の
// 上書き、保留、機能の OFF、アンインストールの 3 つの進み具合、インストールの 3 つの食い違い、録画ファイルの作成が終わって
// いない録画の有無。
std::vector<Observation> states()
{
	std::vector<Observation> out;
	const Observation base = healthy();
	out.push_back(base);
	out.push_back(not_connected());
	out.push_back(Observation{});

	// 枠がすべて使用中で、載せていない人が 2 人いる (チェックは押せず、理由は一覧の上のテキスト)。
	Observation full = healthy();
	for (int slot = 1; slot <= 4; ++slot)
		full.people.push_back(showing("u" + std::to_string(slot), "name" + std::to_string(slot), slot));
	full.people.push_back(present("w1", "waiting1"));
	full.people.push_back(present("w2", "waiting2"));
	out.push_back(full);

	// Vesktop のアカウントと、使う声の設定 (1 対 1、連鎖、本人に戻す) と、名前の上書き。
	Observation voices = healthy();
	PersonFact receiver = present("rx", "receiver");
	receiver.is_receiver = true;
	voices.people.push_back(receiver);
	voices.people.push_back(present("a", "alpha"));
	voices.people.push_back(present("b", "bravo"));
	voices.people.push_back(present("c", "charlie"));
	SeenPerson a_uses_b;
	a_uses_b.user_id = "a";
	a_uses_b.display_name = "alpha";
	a_uses_b.voice_source_user_id = "b";
	a_uses_b.name_override = "A";
	voices.seen.people.push_back(a_uses_b);
	SeenPerson c_uses_a;
	c_uses_a.user_id = "c";
	c_uses_a.display_name = "charlie";
	c_uses_a.voice_source_user_id = "a";
	voices.seen.people.push_back(c_uses_a);
	SeenPerson past;
	past.user_id = "p";
	past.display_name = "past";
	past.always_on = true;
	voices.seen.people.push_back(past);
	out.push_back(voices);

	// 機能の OFF。
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	out.push_back(off);

	// インストールの食い違い (接続している間と、接続していない間)。
	for (const VencordDirState dir :
	     {VencordDirState::HashMismatch, VencordDirState::PointsElsewhere, VencordDirState::NotConfigured}) {
		Observation online = healthy();
		online.install.dir = dir;
		out.push_back(online);
		Observation offline = not_connected();
		offline.install.dir = dir;
		out.push_back(offline);
	}

	// cordmux のソースがない、録画ファイルの作成が終わっていない録画がある、アンインストールの 3 つの進み具合 (書き込む操作は
	// すべて押せない)。
	Observation frames = healthy();
	frames.choices.composite_present = false;
	frames.recording.pending_present = true;
	out.push_back(frames);
	for (const UninstallPhase phase : {UninstallPhase::Running, UninstallPhase::Done, UninstallPhase::Stopped}) {
		Observation u = frames;
		u.uninstall.phase = phase;
		u.uninstall.folder_to_delete = "C:/cordmux";
		out.push_back(u);
	}
	return out;
}

} // namespace

TEST_CASE("押せない理由のマーカーは、decide_dock の View のどこにも残らない")
{
	std::size_t refs_seen = 0;
	std::size_t tags_seen = 0;
	for (const Observation &obs : states()) {
		std::vector<TextRef> refs;
		collect_refs(decide_dock(obs), &refs);
		refs_seen += refs.size();
		for (const TextRef &t : refs) {
			CAPTURE(t.id);
			if (is_reason_tag(t.id))
				++tags_seen;
			CHECK_FALSE(is_reason_tag(t.id));
		}
	}
	MESSAGE("refs=" << refs_seen);
	CHECK(refs_seen > 0);
	CHECK(tags_seen == 0);
}

TEST_CASE("押せない理由のマーカーは cordmux. で始まらず、ini のキーの形にならない")
{
	for (const char *tag : {reason::kDirMatches, reason::kDirNotElsewhere, reason::kFilesMatch,
				reason::kNoUser, reason::kBadSlot, reason::kNoOverride, reason::kSameName,
				reason::kNoVoiceSource, reason::kSameVoiceSource, reason::kVoiceSourceLent,
				reason::kSlotTaken, reason::kAlreadyOnSlot, reason::kNotOnSlot, reason::kNoPendingFinalize,
				reason::kBadVolume}) {
		CAPTURE(tag);
		CHECK(is_reason_tag(tag));
		CHECK(std::string(tag).rfind("cordmux.", 0) == std::string::npos);
	}
}

TEST_CASE("メッセージのボタンは、その状態で View の引数のまま押せる")
{
	// 表の各行がボタンを持つ状態を作る。押せないボタンは理由を持つので、理由のないボタンだけを確認する。
	std::size_t pressable = 0;
	for (const Observation &obs : states()) {
		const dockview::DockViewV2 view = decide_dock(obs);
		for (const dockview::Notice &n : view.notices) {
			for (const dockview::ViewButton &b : n.buttons) {
				CAPTURE(n.rule_name);
				if (b.disabled_reason.has_value())
					continue;
				CHECK(apply(obs, b.id, b.args).can_run());
				++pressable;
			}
		}
		if (view.first_line.button.has_value() && !view.first_line.button->disabled_reason.has_value()) {
			CHECK(apply(obs, view.first_line.button->id, view.first_line.button->args).can_run());
			++pressable;
		}
	}
	MESSAGE("pressable=" << pressable);
	CHECK(pressable > 0);
}

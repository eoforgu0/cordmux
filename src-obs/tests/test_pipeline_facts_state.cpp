// SPDX-License-Identifier: Apache-2.0

// pipeline/facts_state.h の単体テスト。facts のキーの読み方 (読めないキーだけを捨てて数える、一覧は
// キーごと捨てる、省かれたキーと空の値を区別する) と、画面共有していると分かった時刻の引き継ぎを確認する。

#include "doctest.h"

#include "pipeline/facts_state.h"
#include "pipeline/profiles_state.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

using cordmux::pipeline::FactsSnapshot;
using cordmux::pipeline::parse_facts_payload;

namespace {

nlohmann::json json_of(const std::string &text)
{
	return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

} // namespace

TEST_CASE("warnings、sharing、roster の 3 つのキーをそのまま読む")
{
	const auto facts = parse_facts_payload(json_of(R"({
		"warnings": ["pc-hook", "ssrc-map"],
		"sharing": {"count": 2, "user_ids": ["1", "2"]},
		"roster": {"count": 3, "user_ids": ["1", "2", "3"]}
	})"));

	REQUIRE(facts.warnings.size() == 2);
	CHECK(facts.warnings[0] == "pc-hook");
	CHECK(facts.sharing.user_ids.size() == 2);
	CHECK(facts.sharing.sent_count == 2);
	CHECK(facts.sharing.count_matches());
	REQUIRE(facts.roster.has_value());
	CHECK(facts.roster->user_ids.size() == 3);
	CHECK_FALSE(facts.stages.has_value());
	CHECK(facts.unreadable_keys == 0);
	CHECK_FALSE(facts.received); // received を立てるのは呼び出し元 (時計を読むのもそちら)
}

TEST_CASE("roster のキーが省かれた facts と、0 人の roster を区別する")
{
	const auto absent = parse_facts_payload(json_of(R"({"sharing": {"count": 0, "user_ids": []}})"));
	CHECK_FALSE(absent.roster.has_value());
	CHECK(absent.unreadable_keys == 0); // 省かれているのは異常ではない

	const auto empty = parse_facts_payload(json_of(R"({"roster": {"count": 0, "user_ids": []}})"));
	REQUIRE(empty.roster.has_value());
	CHECK(empty.roster->user_ids.empty());
}

TEST_CASE("count が user_ids の数と合わない facts は、合っていないことが分かる形で保持する")
{
	const auto facts = parse_facts_payload(json_of(R"({"sharing": {"count": 5, "user_ids": ["1"]}})"));
	CHECK(facts.sharing.user_ids.size() == 1);
	CHECK(facts.sharing.sent_count == 5);
	CHECK_FALSE(facts.sharing.count_matches());
	CHECK(facts.unreadable_keys == 0); // 形は読めている (数が食い違っているだけ)
}

TEST_CASE("任意のキー stages を読み、知らない段階の文字列もそのまま持つ")
{
	const auto facts = parse_facts_payload(json_of(R"({
		"stages": [{"stream_id": 1, "stage": "native"}, {"stream_id": 7, "stage": "halffps"},
			   {"stream_id": 9, "stage": "これから増える綴り"}]
	})"));

	REQUIRE(facts.stages.has_value());
	REQUIRE(facts.stages->size() == 3);
	CHECK((*facts.stages)[0].stream_id == 1u);
	CHECK((*facts.stages)[0].stage == "native");
	CHECK((*facts.stages)[1].stage == "halffps");
	// 知らない値が来ても捨てない (閉じた列挙にしない)。
	CHECK((*facts.stages)[2].stage == "これから増える綴り");
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("stages の要素が 1 つでも読めなければ、キーごと捨てて数える")
{
	const auto bad_stage = parse_facts_payload(
		json_of(R"({"stages": [{"stream_id": 1, "stage": "native"}, {"stream_id": 2, "stage": 3}]})"));
	CHECK_FALSE(bad_stage.stages.has_value());
	CHECK(bad_stage.unreadable_keys == 1);

	const auto bad_id = parse_facts_payload(json_of(R"({"stages": [{"stream_id": -1, "stage": "native"}]})"));
	CHECK_FALSE(bad_id.stages.has_value());
	CHECK(bad_id.unreadable_keys == 1);

	const auto not_array = parse_facts_payload(json_of(R"({"stages": {"stream_id": 1}})"));
	CHECK_FALSE(not_array.stages.has_value());
	CHECK(not_array.unreadable_keys == 1);

	const auto missing_key = parse_facts_payload(json_of(R"({"stages": [{"stage": "native"}]})"));
	CHECK_FALSE(missing_key.stages.has_value());
	CHECK(missing_key.unreadable_keys == 1);
}

TEST_CASE("読めないキーはそのキーだけを捨て、ほかのキーは読む")
{
	const auto facts = parse_facts_payload(json_of(R"({
		"warnings": ["pc-hook", 42],
		"sharing": "文字列",
		"stages": [{"stream_id": 1, "stage": "native"}],
		"roster": {"user_ids": ["1"]}
	})"));

	CHECK(facts.warnings.empty());              // 1 つでも文字列でなければ、このキーごと捨てる
	CHECK(facts.sharing.user_ids.empty());
	REQUIRE(facts.stages.has_value());          // 読めるキーは残る
	CHECK_FALSE(facts.roster.has_value());      // count がない
	CHECK(facts.unreadable_keys == 3);          // warnings / sharing / roster の 3 つ
}

TEST_CASE("キーが 1 つもない facts は、何も数えずに空のまま読む")
{
	const auto facts = parse_facts_payload(json_of("{}"));
	CHECK(facts.warnings.empty());
	CHECK(facts.sharing.user_ids.empty());
	CHECK_FALSE(facts.roster.has_value());
	CHECK_FALSE(facts.stages.has_value());
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("オブジェクトでない facts は、読めないキーを 1 つとして数える")
{
	CHECK(parse_facts_payload(json_of("[1, 2, 3]")).unreadable_keys == 1);
	CHECK(parse_facts_payload(json_of("42")).unreadable_keys == 1);
	CHECK(parse_facts_payload(json_of("{壊れている")).unreadable_keys == 1); // 読めない JSON
}

TEST_CASE("知らないキーは無視して数えない")
{
	const auto facts = parse_facts_payload(
		json_of(R"({"speaking": {"count": 0, "user_ids": []}, "これから増えるキー": {"x": 1}})"));
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("極端に大きい count は、形が違うものとしてキーごと捨てる")
{
	const auto facts =
		parse_facts_payload(json_of(R"({"sharing": {"count": 99999999999999, "user_ids": ["1"]}})"));
	CHECK(facts.sharing.user_ids.empty());
	CHECK(facts.unreadable_keys == 1);
}

// ---------------------------------------------------------------------------
// 録画に関わる 2 つのキー
// ---------------------------------------------------------------------------

TEST_CASE("recording と sps_pps_prepended のキーをそのまま読む")
{
	const auto facts = parse_facts_payload(json_of(R"({
		"recording": true,
		"sps_pps_prepended": 7
	})"));

	REQUIRE(facts.recording.has_value());
	CHECK(*facts.recording);
	REQUIRE(facts.sps_pps_prepended.has_value());
	CHECK(*facts.sps_pps_prepended == 7u);
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("recording が false の facts も値を持ち、「まだ分からない」と区別する")
{
	const auto facts = parse_facts_payload(json_of(R"({"recording": false, "sps_pps_prepended": 0})"));

	REQUIRE(facts.recording.has_value());
	CHECK_FALSE(*facts.recording);
	REQUIRE(facts.sps_pps_prepended.has_value());
	CHECK(*facts.sps_pps_prepended == 0u);
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("録画のキーを送らない古い送信側の facts では、値を持たず、読めないキーにも入れない")
{
	// 送信側は値にかかわらず必ずこのキーを載せる約束なので、キーがないのは OFF ではなく、送信側の
	// バージョンが古いことを表す。異常ではないので unreadable_keys には数えない (roster と同じ)。
	const auto facts = parse_facts_payload(json_of(R"({"sharing": {"count": 0, "user_ids": []}})"));

	CHECK_FALSE(facts.recording.has_value());
	CHECK_FALSE(facts.sps_pps_prepended.has_value());
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("recording と sps_pps_prepended は、形が違えばそのキーだけを捨てて数える")
{
	SUBCASE("recording が真偽値でない")
	{
		const auto facts = parse_facts_payload(json_of(R"({"recording": "on", "sps_pps_prepended": 3})"));
		CHECK_FALSE(facts.recording.has_value());
		REQUIRE(facts.sps_pps_prepended.has_value()); // もう一方のキーは残る
		CHECK(*facts.sps_pps_prepended == 3u);
		CHECK(facts.unreadable_keys == 1);
	}
	SUBCASE("recording が 0 か 1 の数なら受け付けない")
	{
		const auto facts = parse_facts_payload(json_of(R"({"recording": 1})"));
		CHECK_FALSE(facts.recording.has_value());
		CHECK(facts.unreadable_keys == 1);
	}
	SUBCASE("sps_pps_prepended が負の数")
	{
		const auto facts = parse_facts_payload(json_of(R"({"recording": true, "sps_pps_prepended": -1})"));
		REQUIRE(facts.recording.has_value()); // もう一方のキーは残る
		CHECK(*facts.recording);
		CHECK_FALSE(facts.sps_pps_prepended.has_value());
		CHECK(facts.unreadable_keys == 1);
	}
	SUBCASE("sps_pps_prepended が小数")
	{
		const auto facts = parse_facts_payload(json_of(R"({"sps_pps_prepended": 1.5})"));
		CHECK_FALSE(facts.sps_pps_prepended.has_value());
		CHECK(facts.unreadable_keys == 1);
	}
	SUBCASE("両方とも形が違えば 2 つ数える")
	{
		const auto facts = parse_facts_payload(json_of(R"({"recording": [], "sps_pps_prepended": {}})"));
		CHECK_FALSE(facts.recording.has_value());
		CHECK_FALSE(facts.sps_pps_prepended.has_value());
		CHECK(facts.unreadable_keys == 2);
	}
}

// ---------------------------------------------------------------------------
// self_user_id (Vesktop のアカウントの userId)
// ---------------------------------------------------------------------------

TEST_CASE("self_user_id は、roster がなくてもそのまま読む")
{
	const auto facts = parse_facts_payload(json_of(R"({
		"roster": {"count": 2, "user_ids": ["1", "2"]},
		"self_user_id": "2"
	})"));
	REQUIRE(facts.self_user_id.has_value());
	CHECK(*facts.self_user_id == "2");
	CHECK(facts.unreadable_keys == 0);

	// roster が省かれていても読む (送信側は Vesktop のアカウントを参加者の一覧とは別に読む)。
	const auto alone = parse_facts_payload(json_of(R"({"self_user_id": "9"})"));
	REQUIRE(alone.self_user_id.has_value());
	CHECK(*alone.self_user_id == "9");
	CHECK_FALSE(alone.roster.has_value());
}

TEST_CASE("self_user_id が省かれていれば値を持たず、数えない")
{
	const auto facts = parse_facts_payload(json_of(R"({"roster": {"count": 1, "user_ids": ["1"]}})"));
	CHECK_FALSE(facts.self_user_id.has_value());
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("self_user_id が文字列でないか空なら、そのキーだけを捨てて数える")
{
	SUBCASE("値が数値")
	{
		const auto facts = parse_facts_payload(
			json_of(R"({"roster": {"count": 1, "user_ids": ["1"]}, "self_user_id": 1})"));
		CHECK_FALSE(facts.self_user_id.has_value());
		REQUIRE(facts.roster.has_value()); // ほかのキーは残る
		CHECK(facts.roster->user_ids.size() == 1);
		CHECK(facts.unreadable_keys == 1);
	}
	SUBCASE("値が空の文字列")
	{
		const auto facts = parse_facts_payload(json_of(R"({"self_user_id": ""})"));
		CHECK_FALSE(facts.self_user_id.has_value());
		CHECK(facts.unreadable_keys == 1);
	}
	SUBCASE("値が null (分からないときはキーを省く約束)")
	{
		const auto facts = parse_facts_payload(json_of(R"({"self_user_id": null})"));
		CHECK_FALSE(facts.self_user_id.has_value());
		CHECK(facts.unreadable_keys == 1);
	}
	SUBCASE("値がオブジェクト")
	{
		const auto facts = parse_facts_payload(json_of(R"({"self_user_id": {"user_id": "1"}})"));
		CHECK_FALSE(facts.self_user_id.has_value());
		CHECK(facts.unreadable_keys == 1);
	}
}

// ---- sharing が分かっているか (sharing_valid) と、画面共有していると分かった時刻 ----

TEST_CASE("sharing_valid は、sharing のキーがあり形が正しいときだけ true になる")
{
	CHECK(parse_facts_payload(json_of(R"({"sharing": {"count": 0, "user_ids": []}})")).sharing_valid);
	CHECK_FALSE(parse_facts_payload(json_of(R"({"warnings": []})")).sharing_valid); // キーがない
	CHECK_FALSE(parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": [1]}})")).sharing_valid);
	CHECK_FALSE(parse_facts_payload(json_of(R"([1, 2])")).sharing_valid); // オブジェクトでない
	CHECK_FALSE(FactsSnapshot{}.sharing_valid);                         // まだ届いていない
}

TEST_CASE("parse_facts_payload は画面共有していると分かった時刻を埋めない")
{
	const auto facts = parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": ["1"]}})"));
	CHECK(facts.sharing_since_us.empty());
}

TEST_CASE("carry_sharing_since は sharing に入ったときの時刻を入れ、いる間は引き継ぎ、出たら消す")
{
	using cordmux::pipeline::carry_sharing_since;
	FactsSnapshot first = parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": ["1"]}})"));
	carry_sharing_since(FactsSnapshot{}, first, 1000);
	CHECK(first.sharing_since_us == std::map<std::string, std::int64_t>{{"1", 1000}});

	FactsSnapshot second = parse_facts_payload(json_of(R"({"sharing": {"count": 2, "user_ids": ["1", "2"]}})"));
	carry_sharing_since(first, second, 2000);
	CHECK(second.sharing_since_us == std::map<std::string, std::int64_t>{{"1", 1000}, {"2", 2000}});

	FactsSnapshot third = parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": ["2"]}})"));
	carry_sharing_since(second, third, 3000);
	CHECK(third.sharing_since_us == std::map<std::string, std::int64_t>{{"2", 2000}});

	// 出た人がまた入ったら、そのときの時刻から数え直す
	FactsSnapshot fourth = parse_facts_payload(json_of(R"({"sharing": {"count": 2, "user_ids": ["1", "2"]}})"));
	carry_sharing_since(third, fourth, 4000);
	CHECK(fourth.sharing_since_us == std::map<std::string, std::int64_t>{{"1", 4000}, {"2", 2000}});
}

TEST_CASE("carry_sharing_since は sharing_valid が false の facts では誰の時刻も持たず、次の facts で数え直す")
{
	using cordmux::pipeline::carry_sharing_since;
	FactsSnapshot first = parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": ["1"]}})"));
	carry_sharing_since(FactsSnapshot{}, first, 1000);

	FactsSnapshot broken = parse_facts_payload(json_of(R"({"sharing": "x"})"));
	carry_sharing_since(first, broken, 2000);
	CHECK_FALSE(broken.sharing_valid);
	CHECK(broken.sharing_since_us.empty());

	FactsSnapshot again = parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": ["1"]}})"));
	carry_sharing_since(broken, again, 3000);
	CHECK(again.sharing_since_us == std::map<std::string, std::int64_t>{{"1", 3000}});
}

TEST_CASE("carry_sharing_since に空の前の facts を渡すと、全員の時刻を数え直す")
{
	using cordmux::pipeline::carry_sharing_since;
	FactsSnapshot first = parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": ["1"]}})"));
	carry_sharing_since(FactsSnapshot{}, first, 1000);

	FactsSnapshot after_reconnect = parse_facts_payload(json_of(R"({"sharing": {"count": 1, "user_ids": ["1"]}})"));
	carry_sharing_since(FactsSnapshot{}, after_reconnect, 5000);
	CHECK(after_reconnect.sharing_since_us == std::map<std::string, std::int64_t>{{"1", 5000}});
}

TEST_CASE("carry_sharing_since は空の userId の時刻を持たない")
{
	using cordmux::pipeline::carry_sharing_since;
	FactsSnapshot next = parse_facts_payload(json_of(R"({"sharing": {"count": 2, "user_ids": ["", "1"]}})"));
	carry_sharing_since(FactsSnapshot{}, next, 1000);
	CHECK(next.sharing_since_us == std::map<std::string, std::int64_t>{{"1", 1000}});
}

// ---- profiles のユーザー名 ----

TEST_CASE("make_profiles_snapshot はユーザー名を保持し、ユーザー名のない人と知らない人は値を持たない")
{
	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry taro;
	taro.user_id = "1";
	taro.display_name = "taro";
	taro.username = "taro_1";
	cordmux::ws::ProfileEntry old_sender;
	old_sender.user_id = "2";
	old_sender.display_name = "taro";
	message.profiles = {taro, old_sender};

	const auto snapshot = cordmux::pipeline::make_profiles_snapshot(message, 1000);
	CHECK(snapshot.username_of("1") == std::optional<std::string>("taro_1"));
	CHECK_FALSE(snapshot.username_of("2").has_value());
	CHECK_FALSE(snapshot.username_of("3").has_value());
	CHECK(snapshot.display_name_of("1") == std::optional<std::string>("taro"));
}

// ---------------------------------------------------------------------------
// call_name (通話の名前) と main_call (メインアカウントがいる通話)
// ---------------------------------------------------------------------------

TEST_CASE("call_name と main_call は、読めた値を持つ")
{
	const auto facts = parse_facts_payload(json_of(R"({
		"call_name": "Server / Room",
		"main_call": {"user_id": "5", "name": "Other", "joinable": false}
	})"));
	REQUIRE(facts.call_name.has_value());
	CHECK(*facts.call_name == "Server / Room");
	REQUIRE(facts.main_call.has_value());
	CHECK(facts.main_call->user_id == "5");
	REQUIRE(facts.main_call->name.has_value());
	CHECK(*facts.main_call->name == "Other");
	REQUIRE(facts.main_call->joinable.has_value());
	CHECK_FALSE(*facts.main_call->joinable);
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("main_call の name と joinable は省ける。省かれたキーは値を持たない")
{
	const auto facts = parse_facts_payload(json_of(R"({"main_call": {"user_id": "5"}})"));
	REQUIRE(facts.main_call.has_value());
	CHECK_FALSE(facts.main_call->name.has_value());
	CHECK_FALSE(facts.main_call->joinable.has_value());
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("call_name と main_call が省かれていれば値を持たず、数えない (分からない)")
{
	const auto facts = parse_facts_payload(json_of(R"({"roster": {"count": 1, "user_ids": ["1"]}})"));
	CHECK_FALSE(facts.call_name.has_value());
	CHECK_FALSE(facts.main_call.has_value());
	CHECK(facts.unreadable_keys == 0);
}

TEST_CASE("形が違う call_name と main_call は、そのキーだけを捨てて数える (一部だけを使わない)")
{
	for (const char *text : {R"({"call_name": ""})", R"({"call_name": 1})", R"({"main_call": 1})",
				 R"({"main_call": {}})", R"({"main_call": {"user_id": ""}})",
				 R"({"main_call": {"user_id": "5", "name": 1}})",
				 R"({"main_call": {"user_id": "5", "joinable": "yes"}})"}) {
		CAPTURE(text);
		const auto facts = parse_facts_payload(json_of(text));
		CHECK_FALSE(facts.call_name.has_value());
		CHECK_FALSE(facts.main_call.has_value());
		CHECK(facts.unreadable_keys == 1);
	}
}

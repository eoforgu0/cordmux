// SPDX-License-Identifier: Apache-2.0

// config/seen_people.h (これまでに見た人の一覧) のテスト。

#include "doctest.h"

#include "config/seen_people.h"

#include <set>
#include <string>
#include <vector>

using cordmux::config::ensure_seen_person;
using cordmux::config::seen_people_from_json;
using cordmux::config::seen_people_to_json;
using cordmux::config::seen_person_has_mark;
using cordmux::config::SeenTrimResult;
using cordmux::config::trim_seen_people;
using cordmux::ledger::SeenPerson;

namespace {

SeenPerson person(const std::string &user_id, std::int64_t last_seen_at_us)
{
	SeenPerson out;
	out.user_id = user_id;
	out.display_name = "name-" + user_id;
	out.last_seen_at_us = last_seen_at_us;
	return out;
}

bool has_user(const std::vector<SeenPerson> &people, const std::string &user_id)
{
	for (const SeenPerson &row : people) {
		if (row.user_id == user_id)
			return true;
	}
	return false;
}

} // namespace

TEST_CASE("「自動で載せる」「cordmuxでの名前」「使う声」は指定に数え、空の値は数えない")
{
	SeenPerson plain = person("1", 10);
	CHECK_FALSE(seen_person_has_mark(plain));

	SeenPerson always_on = plain;
	always_on.always_on = true;
	CHECK(seen_person_has_mark(always_on));

	SeenPerson override_name = plain;
	override_name.name_override = "呼び名";
	CHECK(seen_person_has_mark(override_name));

	// cordmux での名前を消すと、空の文字列で届く (ledger::ActionArgs::name)。空の上書きが指定として
	// 残ると、捨てられない行だけが増えていく。
	SeenPerson empty_override = plain;
	empty_override.name_override = "";
	CHECK_FALSE(seen_person_has_mark(empty_override));

	SeenPerson voice = plain;
	voice.voice_source_user_id = "2";
	CHECK(seen_person_has_mark(voice));

	SeenPerson empty_voice = plain;
	empty_voice.voice_source_user_id = "";
	CHECK_FALSE(seen_person_has_mark(empty_voice));
}

TEST_CASE("上限を超えたら、指定のない行から古い順に捨てる")
{
	std::vector<SeenPerson> people = {person("old", 100), person("new", 300), person("mid", 200)};
	const SeenTrimResult result = trim_seen_people(&people, 2);
	CHECK(result.discarded == 1);
	CHECK(result.discarded_with_marks == 0);
	REQUIRE(people.size() == 2);
	CHECK_FALSE(has_user(people, "old"));
	// 残った行の並びは元のまま (並べ方を決めるのは表示の側)。
	CHECK(people[0].user_id == "new");
	CHECK(people[1].user_id == "mid");
}

TEST_CASE("指定の付いた行は、ほかの行より古くても捨てない")
{
	std::vector<SeenPerson> people = {person("marked", 1), person("plain1", 100), person("plain2", 200)};
	people[0].always_on = true;

	const SeenTrimResult result = trim_seen_people(&people, 2);
	CHECK(result.discarded == 1);
	CHECK(result.discarded_with_marks == 0);
	REQUIRE(people.size() == 2);
	CHECK(has_user(people, "marked"));   // いちばん古いが、指定がある
	CHECK_FALSE(has_user(people, "plain1")); // 指定のない行の中でいちばん古い
}

TEST_CASE("指定の付いた行だけで上限を超えたら、その中から古い順に捨てて、捨てた数を返す")
{
	std::vector<SeenPerson> people = {person("a", 100), person("b", 200), person("c", 300)};
	for (SeenPerson &row : people)
		row.always_on = true;

	const SeenTrimResult result = trim_seen_people(&people, 2);
	CHECK(result.discarded == 1);
	CHECK(result.discarded_with_marks == 1); // ドックに表示する数
	REQUIRE(people.size() == 2);
	CHECK_FALSE(has_user(people, "a"));
}

TEST_CASE("上限の中なら 1 行も捨てない")
{
	std::vector<SeenPerson> people = {person("a", 100), person("b", 200)};
	const SeenTrimResult result = trim_seen_people(&people, 200);
	CHECK(result.discarded == 0);
	CHECK(people.size() == 2);
}

TEST_CASE("上限が 0 ならすべての行を捨て、指定の付いた行の数も返す")
{
	std::vector<SeenPerson> people = {person("a", 100), person("b", 200)};
	people[1].name_override = "呼び名";
	const SeenTrimResult result = trim_seen_people(&people, 0);
	CHECK(result.discarded == 2);
	CHECK(result.discarded_with_marks == 1);
	CHECK(people.empty());
}

TEST_CASE("書いて読み直すと同じ一覧に戻り、持っていない指定はキーごと省く")
{
	std::vector<SeenPerson> people = {person("1", 100), person("2", 200)};
	people[0].always_on = true;
	people[0].name_override = "呼び名";
	people[1].voice_source_user_id = "1";
	people[1].display_name = ""; // 名前が読めなかったときの行

	const std::string text = seen_people_to_json(people);
	CHECK(text.find("name_override") != std::string::npos);
	// 持っていない指定は、キーごと書かない (ないことと、空文字列を書き分ける)。
	CHECK(text.find("voice_source_user_id") != std::string::npos);

	std::vector<SeenPerson> back;
	std::string error;
	REQUIRE(seen_people_from_json(text, &back, &error));
	CHECK(error.empty());
	REQUIRE(back.size() == 2);
	CHECK(back[0].user_id == "1");
	CHECK(back[0].always_on);
	REQUIRE(back[0].name_override.has_value());
	CHECK(*back[0].name_override == "呼び名");
	CHECK_FALSE(back[0].voice_source_user_id.has_value());
	CHECK(back[1].display_name.empty());
	REQUIRE(back[1].voice_source_user_id.has_value());
	CHECK(*back[1].voice_source_user_id == "1");
	CHECK(back[1].last_seen_at_us == 200);
}

TEST_CASE("空の配列は空の一覧として読める")
{
	std::vector<SeenPerson> people = {person("1", 1)};
	std::string error;
	CHECK(seen_people_from_json("[]", &people, &error));
	CHECK(people.empty());
	CHECK(error.empty());
}

TEST_CASE("形が違うファイルは、1 行だけ落とさずに丸ごと拒む")
{
	std::vector<SeenPerson> people;
	std::string error;

	CHECK_FALSE(seen_people_from_json("{", &people, &error));
	CHECK_FALSE(error.empty());
	CHECK_FALSE(seen_people_from_json("{\"user_id\":\"1\"}", &people, &error)); // 配列でない
	CHECK_FALSE(seen_people_from_json("[1, 2]", &people, &error));
	CHECK_FALSE(seen_people_from_json("[{\"display_name\":\"x\"}]", &people, &error)); // user_id がない
	CHECK_FALSE(seen_people_from_json("[{\"user_id\":\"\"}]", &people, &error));
	CHECK_FALSE(seen_people_from_json("[{\"user_id\":\"1\",\"always_on\":1}]", &people, &error));
	CHECK_FALSE(seen_people_from_json("[{\"user_id\":\"1\",\"last_seen_at_us\":\"x\"}]", &people, &error));
	CHECK_FALSE(seen_people_from_json("[{\"user_id\":\"1\",\"name_override\":5}]", &people, &error));
	// 同じ userId が 2 行あると、どちらの指定が正しいか決まらない。
	CHECK_FALSE(seen_people_from_json("[{\"user_id\":\"1\"},{\"user_id\":\"1\"}]", &people, &error));
	CHECK(people.empty());
}

TEST_CASE("ensure_seen_person はその userId の行を返し、なければ追加する")
{
	std::vector<SeenPerson> people;
	ensure_seen_person(people, "1").last_seen_at_us = 100;
	ensure_seen_person(people, "2").always_on = true;
	CHECK(people.size() == 2);

	// 2 度目は同じ行を返す (同じ人の行を 2 つ作らない)。
	ensure_seen_person(people, "1").display_name = "名前";
	REQUIRE(people.size() == 2);
	CHECK(people[0].last_seen_at_us == 100);
	CHECK(people[0].display_name == "名前");
}

TEST_CASE("今見えている人の時刻を先に今にすると、上限ちょうどの一覧でも、捨てられては追加される、を繰り返さない")
{
	// 上限は 3。通話に長く参加している人 (in-call) の時刻は、参加した時刻 (古い) のままで、
	// 後から抜けた人のほうが新しい。
	std::vector<SeenPerson> people;
	people.push_back(person("in-call", 100));
	people.push_back(person("left-a", 200));
	people.push_back(person("left-b", 300));
	people.push_back(person("left-c", 400));

	SUBCASE("時刻を変えないと、通話に参加している人が最も古い行として捨てられる")
	{
		std::vector<SeenPerson> copy = people;
		const SeenTrimResult trim = trim_seen_people(&copy, 3);
		CHECK(trim.discarded == 1);
		REQUIRE(copy.size() == 3);
		for (const SeenPerson &row : copy)
			CHECK(row.user_id != "in-call");
	}

	SUBCASE("先に今にしておくと、捨てられるのは本当に古い行になる")
	{
		std::set<std::string> in_call;
		in_call.insert("in-call");
		in_call.insert(""); // 空の userId は飛ばす (行を作らない)
		cordmux::config::touch_seen_people(people, in_call, 500);
		CHECK(people.size() == 4);

		const SeenTrimResult trim = trim_seen_people(&people, 3);
		CHECK(trim.discarded == 1);
		REQUIRE(people.size() == 3);
		bool kept_in_call = false;
		bool kept_oldest = false;
		for (const SeenPerson &row : people) {
			if (row.user_id == "in-call") {
				kept_in_call = true;
				CHECK(row.last_seen_at_us == 500);
			}
			if (row.user_id == "left-a")
				kept_oldest = true;
		}
		CHECK(kept_in_call);
		CHECK_FALSE(kept_oldest);
	}
}

TEST_CASE("今見えている人の行がなければ作る")
{
	std::vector<SeenPerson> people;
	std::set<std::string> in_call;
	in_call.insert("new-one");
	cordmux::config::touch_seen_people(people, in_call, 700);
	REQUIRE(people.size() == 1);
	CHECK(people[0].user_id == "new-one");
	CHECK(people[0].last_seen_at_us == 700);
}

// ---- 参加者ごとの名前ラベルの角 (今は持たない) ----

TEST_CASE("参加者ごとの label_corner が残っているファイルは、どんな値でも未知のキーとして読み飛ばし、一覧全体を読め、書き戻さない")
{
	const char *previous = R"([
  {"user_id": "1", "display_name": "a", "last_seen_at_us": 10, "always_on": true, "label_corner": "top_right"},
  {"user_id": "2", "display_name": "b", "last_seen_at_us": 20, "label_corner": "left"},
  {"user_id": "3", "display_name": "c", "last_seen_at_us": 30, "label_corner": 2},
  {"user_id": "4", "display_name": "d", "last_seen_at_us": 40, "label_corner": null}
])";
	std::vector<SeenPerson> back;
	std::string error;
	REQUIRE(seen_people_from_json(previous, &back, &error));
	CHECK(error.empty());
	REQUIRE(back.size() == 4);
	CHECK(back[0].always_on);
	CHECK(back[1].display_name == "b");
	CHECK(seen_people_to_json(back).find("label_corner") == std::string::npos);
}

TEST_CASE("角を選んだだけの行は指定に数えず、上限に合わせて古い順に捨てる")
{
	SeenPerson plain = person("1", 1);
	CHECK_FALSE(seen_person_has_mark(plain));

	// 古い行に残っていた label_corner は読み飛ばされるので、指定のない行として最初に捨てられる。
	std::vector<SeenPerson> people;
	REQUIRE(seen_people_from_json(
		R"([{"user_id":"old","last_seen_at_us":1,"label_corner":"top_left"},
		    {"user_id":"mid","last_seen_at_us":2},{"user_id":"new","last_seen_at_us":3}])",
		&people, nullptr));
	const SeenTrimResult result = trim_seen_people(&people, 2);
	CHECK(result.discarded == 1);
	CHECK(result.discarded_with_marks == 0);
	CHECK_FALSE(has_user(people, "old"));
	CHECK(has_user(people, "mid"));
}

// ---- 列「通話のみ時ラベル非表示」(画面共有していない参加者の並びに出すか) ----

TEST_CASE("「通話のみ時ラベル非表示」を付けた行は、付けたときだけ roster_hidden を書き、読み直すと同じ値に戻る")
{
	std::vector<SeenPerson> people = {person("1", 100), person("2", 200)};
	people[1].roster_hidden = true;

	const std::string text = seen_people_to_json(people);
	std::vector<SeenPerson> back;
	std::string error;
	REQUIRE(seen_people_from_json(text, &back, &error));
	REQUIRE(back.size() == 2);
	CHECK_FALSE(back[0].roster_hidden);
	CHECK(back[1].roster_hidden);

	// 既定 (出す) の行には書かない。
	const std::string plain = seen_people_to_json({person("1", 100)});
	CHECK(plain.find("roster_hidden") == std::string::npos);
	CHECK(text.find("roster_hidden") != std::string::npos);
}

TEST_CASE("roster_hidden のない行は出す人として読み、真偽値でない roster_hidden は一覧を丸ごと拒む")
{
	std::vector<SeenPerson> people;
	std::string error;
	REQUIRE(seen_people_from_json(R"([{"user_id":"1"},{"user_id":"2","roster_hidden":false}])", &people, &error));
	REQUIRE(people.size() == 2);
	CHECK_FALSE(people[0].roster_hidden);
	CHECK_FALSE(people[1].roster_hidden);

	CHECK_FALSE(seen_people_from_json(R"([{"user_id":"1","roster_hidden":1}])", &people, &error));
	CHECK_FALSE(error.empty());
	CHECK_FALSE(seen_people_from_json(R"([{"user_id":"1"},{"user_id":"2","roster_hidden":"true"}])", &people, &error));
	CHECK(people.empty());
}

TEST_CASE("「通話のみ時ラベル非表示」を付けた行は指定に数え、上限を超えても指定のない行より後に捨てる")
{
	SeenPerson hidden = person("bot", 1);
	hidden.roster_hidden = true;
	CHECK(seen_person_has_mark(hidden));

	// 付けた Bot の行がいちばん古くても残す (捨てると、その Bot がメッセージもなく並びに戻る)。
	std::vector<SeenPerson> people = {hidden, person("mid", 2), person("new", 3)};
	const SeenTrimResult result = trim_seen_people(&people, 2);
	CHECK(result.discarded == 1);
	CHECK(result.discarded_with_marks == 0);
	CHECK(has_user(people, "bot"));
	CHECK_FALSE(has_user(people, "mid"));
}

TEST_CASE("参加者ごとの音量は 100% でないときだけ書き、読み直すと同じ値に戻る (ミュートは書かない)")
{
	std::vector<SeenPerson> people = {person("1", 100), person("2", 200), person("3", 300)};
	people[1].voice_volume_pct = 30;
	people[2].screen_volume_pct = 200;
	people[2].voice_volume_pct = 0;

	const std::string text = seen_people_to_json(people);
	std::vector<SeenPerson> back;
	std::string error;
	REQUIRE(seen_people_from_json(text, &back, &error));
	REQUIRE(back.size() == 3);
	CHECK(back[0].voice_volume_pct == 100);
	CHECK(back[0].screen_volume_pct == 100);
	CHECK(back[1].voice_volume_pct == 30);
	CHECK(back[1].screen_volume_pct == 100);
	CHECK(back[2].voice_volume_pct == 0);
	CHECK(back[2].screen_volume_pct == 200);

	// 既定 (100%) の行にはキーを書かない。ミュートのキーはどこにも書かない。
	const std::string plain = seen_people_to_json({person("1", 100)});
	CHECK(plain.find("volume") == std::string::npos);
	CHECK(text.find("mute") == std::string::npos);
	CHECK(text.find("\"voice_volume_pct\": 30") != std::string::npos);
	CHECK(text.find("\"screen_volume_pct\": 200") != std::string::npos);
}

TEST_CASE("音量のキーがない行は 100%、範囲 (0 から 200) の端は読め、外れた値と整数でない値は一覧を丸ごと拒む")
{
	std::vector<SeenPerson> people;
	std::string error;
	REQUIRE(seen_people_from_json(R"([{"user_id":"1"},{"user_id":"2","voice_volume_pct":0,"screen_volume_pct":200}])",
				      &people, &error));
	REQUIRE(people.size() == 2);
	CHECK(people[0].voice_volume_pct == 100);
	CHECK(people[0].screen_volume_pct == 100);
	CHECK(people[1].voice_volume_pct == 0);
	CHECK(people[1].screen_volume_pct == 200);

	for (const char *bad : {
		     R"([{"user_id":"1","voice_volume_pct":-1}])",
		     R"([{"user_id":"1","voice_volume_pct":201}])",
		     R"([{"user_id":"1","screen_volume_pct":-1}])",
		     R"([{"user_id":"1","screen_volume_pct":201}])",
		     R"([{"user_id":"1","voice_volume_pct":50.5}])",
		     R"([{"user_id":"1","voice_volume_pct":"50"}])",
		     R"([{"user_id":"1","voice_volume_pct":true}])",
		     R"([{"user_id":"1","voice_volume_pct":null}])",
		     R"([{"user_id":"1","screen_volume_pct":18446744073709551615}])",
		     R"([{"user_id":"1"},{"user_id":"2","voice_volume_pct":9999999999}])",
	     }) {
		CAPTURE(bad);
		people.clear();
		error.clear();
		CHECK_FALSE(seen_people_from_json(bad, &people, &error));
		CHECK_FALSE(error.empty());
		CHECK(people.empty()); // 1 行だけ落とさず、丸ごと拒む
	}
}

TEST_CASE("100% でない音量の行は指定に数え、上限を超えても指定のない行より後に捨てる")
{
	SeenPerson voice = person("v", 1);
	voice.voice_volume_pct = 40;
	CHECK(seen_person_has_mark(voice));
	SeenPerson screen = person("s", 2);
	screen.screen_volume_pct = 150;
	CHECK(seen_person_has_mark(screen));
	CHECK_FALSE(seen_person_has_mark(person("p", 3)));

	// 下げた人の行がいちばん古くても残す (捨てると、メッセージもなく 100% に戻る)。
	std::vector<SeenPerson> people = {voice, person("mid", 2), person("new", 3)};
	const SeenTrimResult result = trim_seen_people(&people, 2);
	CHECK(result.discarded == 1);
	CHECK(result.discarded_with_marks == 0);
	CHECK(has_user(people, "v"));
	CHECK_FALSE(has_user(people, "mid"));

	// 音量の指定だけで上限を超えたら、捨てた指定の数を返す。
	std::vector<SeenPerson> only_marks = {voice, screen};
	const SeenTrimResult dropped = trim_seen_people(&only_marks, 1);
	CHECK(dropped.discarded == 1);
	CHECK(dropped.discarded_with_marks == 1);
}

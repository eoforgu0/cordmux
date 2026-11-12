// SPDX-License-Identifier: Apache-2.0

// ws/control_messages.h の単体テスト。制御用の JSON を組み立てる関数と読む関数を確認する。読む関数が、壊れた入力に
// 例外を投げず nullopt を返すことを重点的に確認する。

#include "doctest.h"

#include "ws/control_messages.h"

#include <optional>
#include <string>
#include <vector>

using namespace cordmux::ws;

TEST_CASE("build_hello で作った hello を parse_hello で読むと、元の値に戻る")
{
	const std::string text = build_hello({0, "vencord-1.2.3", "deadbeef"});
	const auto j = try_parse_json(text);
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kHello));

	const auto h = parse_hello(*j);
	REQUIRE(h.has_value());
	CHECK(h->wire == 0);
	CHECK(h->version == "vencord-1.2.3");
	CHECK(h->secret == "deadbeef");
}

TEST_CASE("hello と pong の clock_us (送信側の時計) は往復し、0 以上の整数でなければそのキーだけをないものとする")
{
	HelloMessage hello{1, "vencord-1.2.3", "deadbeef"};
	hello.clock_us = 1'700'000'000'123'456;
	const auto h = parse_hello(*try_parse_json(build_hello(hello)));
	REQUIRE(h.has_value());
	CHECK(h->clock_us == std::optional<std::int64_t>(1'700'000'000'123'456));

	PongMessage pong{4};
	pong.clock_us = 77;
	const auto p = parse_pong(*try_parse_json(build_pong(pong)));
	REQUIRE(p.has_value());
	CHECK(p->seq == 4);
	CHECK(p->clock_us == std::optional<std::int64_t>(77));

	// キーがない pong と、値が整数でない、負、int64_t に収まらない pong は、clock_us だけを持たない。
	for (const char *text : {R"({"type":"pong","seq":1})", R"({"type":"pong","seq":1,"clock_us":"5"})",
				 R"({"type":"pong","seq":1,"clock_us":-5})", R"({"type":"pong","seq":1,"clock_us":1.5})",
				 R"({"type":"pong","seq":1,"clock_us":18446744073709551615})"}) {
		CAPTURE(text);
		const auto q = parse_pong(*try_parse_json(text));
		REQUIRE(q.has_value());
		CHECK_FALSE(q->clock_us.has_value());
	}
	const auto bad_hello =
		parse_hello(*try_parse_json(R"({"type":"hello","wire":1,"secret":"x","version":"v","clock_us":"1"})"));
	REQUIRE(bad_hello.has_value());
	CHECK_FALSE(bad_hello->clock_us.has_value());
}

TEST_CASE("secret か wire がないか型が違う hello には、parse_hello が例外を投げずに nullopt を返す")
{
	CHECK_FALSE(parse_hello(*try_parse_json(R"({"type":"hello","wire":0})")).has_value());
	CHECK_FALSE(parse_hello(*try_parse_json(R"({"type":"hello","secret":"x"})")).has_value());
	CHECK_FALSE(parse_hello(*try_parse_json(R"({"type":"hello","wire":"0","secret":"x"})")).has_value());
	CHECK_FALSE(parse_hello(*try_parse_json(R"([1,2,3])")).has_value());
}

TEST_CASE("version がないか文字列でない hello には、parse_hello が nullopt を返す")
{
	CHECK_FALSE(parse_hello(*try_parse_json(R"({"type":"hello","wire":0,"secret":"x"})")).has_value());
	CHECK_FALSE(parse_hello(*try_parse_json(R"({"type":"hello","wire":0,"secret":"x","version":123})")).has_value());
	CHECK(parse_hello(*try_parse_json(R"({"type":"hello","wire":0,"secret":"x","version":""})")).has_value());
}

TEST_CASE("任意のキー vesktop が文字列なら parse_hello が値を読み、キーがなければ値を持たない")
{
	// 送信側 (src-vencord/transport/client.ts の buildHello) が Vesktop のバージョンを読めたときの形。
	const auto with = parse_hello(*try_parse_json(R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0+dev","vesktop":"1.6.7"})"));
	REQUIRE(with.has_value());
	CHECK(with->version == "0.1.0+dev");
	REQUIRE(with->vesktop.has_value());
	CHECK(*with->vesktop == "1.6.7");

	// キーがないのは、送信側が Vesktop のバージョンを読めなかったとき。hello はそのまま読める。
	const auto without = parse_hello(*try_parse_json(R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0"})"));
	REQUIRE(without.has_value());
	CHECK_FALSE(without->vesktop.has_value());

	// 空の文字列も文字列なので読む (空は分からないことを示し、画面には "-" を表示する)。
	const auto empty = parse_hello(*try_parse_json(R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0","vesktop":""})"));
	REQUIRE(empty.has_value());
	REQUIRE(empty->vesktop.has_value());
	CHECK(empty->vesktop->empty());
}

TEST_CASE("vesktop が文字列でなければ、そのキーだけをないものとし、hello は捨てない")
{
	for (const char *text : {R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0","vesktop":167})",
				 R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0","vesktop":null})",
				 R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0","vesktop":true})",
				 R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0","vesktop":["1.6.7"]})",
				 R"({"type":"hello","wire":0,"secret":"x","version":"0.1.0","vesktop":{"v":"1.6.7"}})"}) {
		CAPTURE(text);
		const auto h = parse_hello(*try_parse_json(text));
		REQUIRE(h.has_value());
		CHECK(h->version == "0.1.0");
		CHECK(h->secret == "x");
		CHECK_FALSE(h->vesktop.has_value());
	}
}

TEST_CASE("build_hello は vesktop の値があるときだけキーを書き、parse_hello で読むと元の値に戻る")
{
	HelloMessage m{0, "0.1.0+dev", "deadbeef"};
	{
		const auto j = try_parse_json(build_hello(m));
		REQUIRE(j.has_value());
		CHECK(j->find("vesktop") == j->end()); // 値がなければキーを省く (null を書かない)
	}
	m.vesktop = "1.6.0-beta.2+build.5";
	const auto j = try_parse_json(build_hello(m));
	REQUIRE(j.has_value());
	const auto back = parse_hello(*j);
	REQUIRE(back.has_value());
	REQUIRE(back->vesktop.has_value());
	CHECK(*back->vesktop == "1.6.0-beta.2+build.5");
}

TEST_CASE("try_parse_json は壊れた JSON に例外を投げず、nullopt を返す")
{
	CHECK_FALSE(try_parse_json("").has_value());
	CHECK_FALSE(try_parse_json("{").has_value());
	CHECK_FALSE(try_parse_json("not json at all").has_value());
	CHECK_FALSE(try_parse_json(std::string(1, '\0')).has_value());
}

TEST_CASE("type がないか文字列でなければ、peek_type が nullopt を返す")
{
	CHECK_FALSE(peek_type(*try_parse_json(R"({"type":123})")).has_value());
	CHECK_FALSE(peek_type(*try_parse_json(R"({"notype":"hello"})")).has_value());
	CHECK_FALSE(peek_type(*try_parse_json(R"("just a string")")).has_value());
}

TEST_CASE("hello_ng は理由のコードだけを持ち、受け取ったトークンを含まない")
{
	const std::string text = build_hello_ng({HelloNgReason::BadSecret});
	CHECK(text.find("bad_secret") != std::string::npos); // 理由のコードに含まれる "secret" という語はよい
	// HelloNgMessage のフィールドは理由のコードだけで、受け取ったトークンを持つ場所がないので、トークンを送ることも
	// ない。Listener のログにトークンが含まれないことは、tests/test_ws_listener.cpp が確認する。
	const HelloNgMessage m{HelloNgReason::BadSecret};
	CHECK(m.reason == HelloNgReason::BadSecret);
}

TEST_CASE("build_ping で作った ping には type が入り、build_pong で作った pong の seq は parse_pong で読める")
{
	const auto ping_json = try_parse_json(build_ping({42}));
	REQUIRE(ping_json.has_value());
	CHECK(peek_type(*ping_json) == std::string(control_type::kPing));

	const std::string pong_text = build_pong({42});
	const auto pong_json = try_parse_json(pong_text);
	REQUIRE(pong_json.has_value());
	const auto pong = parse_pong(*pong_json);
	REQUIRE(pong.has_value());
	CHECK(pong->seq == 42);
}

TEST_CASE("seq がないか、数値でないか、負の pong には、parse_pong が nullopt を返す")
{
	CHECK_FALSE(parse_pong(*try_parse_json(R"({"type":"pong","seq":"42"})")).has_value());
	CHECK_FALSE(parse_pong(*try_parse_json(R"({"type":"pong","seq":-1})")).has_value());
	CHECK_FALSE(parse_pong(*try_parse_json(R"({"type":"pong"})")).has_value());
}

TEST_CASE("画面と声のストリームを持つ configure を、parse_configure が読む")
{
	// width、height、fps は任意だが、書くなら正の値でなければならない。voice のストリーム (画面共有ではなく
	// ボイス接続の音声) にも書いてよいので、ここでは両方に正の値を入れる (0 のときは別の TEST_CASE で確認する)。
	ConfigureMessage m;
	m.streams.push_back({1, "user-a", StreamKind::Screen, 1920, 1080, 60.0, true});
	m.streams.push_back({2, "user-b", StreamKind::Voice, 1280, 720, 30.0, true});

	nlohmann::json j;
	j["type"] = control_type::kConfigure;
	nlohmann::json streams = nlohmann::json::array();
	for (const auto &s : m.streams) {
		nlohmann::json sj;
		sj["stream_id"] = s.stream_id;
		sj["user_id"] = s.user_id;
		sj["kind"] = to_string(s.kind);
		sj["width"] = s.width;
		sj["height"] = s.height;
		sj["fps"] = s.fps;
		sj["audio"] = s.audio;
		streams.push_back(sj);
	}
	j["streams"] = streams;

	const auto parsed = parse_configure(j);
	REQUIRE(parsed.has_value());
	REQUIRE(parsed->streams.size() == 2);
	CHECK(parsed->streams[0].stream_id == 1);
	CHECK(parsed->streams[0].user_id == "user-a");
	CHECK(parsed->streams[0].kind == StreamKind::Screen);
	CHECK(parsed->streams[0].width == 1920);
	CHECK(parsed->streams[0].fps == 60.0);
	CHECK(parsed->streams[1].kind == StreamKind::Voice);
	CHECK(parsed->streams[1].audio == true);
}

TEST_CASE("streams が配列でないか、壊れた要素が 1 つでもある configure には、parse_configure が nullopt を返す")
{
	CHECK_FALSE(parse_configure(*try_parse_json(R"({"type":"configure","streams":"nope"})")).has_value());
	CHECK_FALSE(parse_configure(*try_parse_json(R"({"type":"configure"})")).has_value());
	// kind が screen でも voice でもない
	CHECK_FALSE(parse_configure(*try_parse_json(
	                    R"({"type":"configure","streams":[{"stream_id":1,"user_id":"a","kind":"video"}]})"))
	                    .has_value());
	// 2 つ目の要素が壊れている (1 つ目は正しい)
	CHECK_FALSE(
		parse_configure(
			*try_parse_json(
				R"({"type":"configure","streams":[{"stream_id":1,"user_id":"a","kind":"screen"},{"stream_id":"x"}]})"))
			.has_value());
}

TEST_CASE("stream_id が重複した configure には、ストリームの種類にかかわらず parse_configure が nullopt を返す")
{
	// screen の中に stream_id=1 が 2 つある (送信側か通信が壊れている)。
	CHECK_FALSE(
		parse_configure(
			*try_parse_json(
				R"({"type":"configure","streams":[
				      {"stream_id":1,"user_id":"a","kind":"screen"},
				      {"stream_id":1,"user_id":"b","kind":"screen"}]})"))
			.has_value());
	// voice でも同じ。
	CHECK_FALSE(
		parse_configure(
			*try_parse_json(
				R"({"type":"configure","streams":[
				      {"stream_id":9,"user_id":"a","kind":"voice"},
				      {"stream_id":9,"user_id":"b","kind":"voice"}]})"))
			.has_value());

	// 種類が違っても、重複 (画面の stream_id=1 と声の stream_id=1) は拒む。受け入れると、音声が画面共有の音か声かを
	// 決められず、画面共有の音が声の枠に出る。
	CHECK_FALSE(parse_configure(
	                    *try_parse_json(
	                            R"({"type":"configure","streams":[
	                                  {"stream_id":1,"user_id":"a","kind":"screen"},
	                                  {"stream_id":1,"user_id":"b","kind":"voice"}]})"))
	                    .has_value());

	// stream_id が違えば、画面と声が同じ configure に並んでいても読める (正しい送信側の形)。
	CHECK(parse_configure(
	              *try_parse_json(
	                      R"({"type":"configure","streams":[
	                            {"stream_id":1,"user_id":"a","kind":"screen"},
	                            {"stream_id":2,"user_id":"b","kind":"voice"}]})"))
	              .has_value());
}

TEST_CASE("width、height、fps、audio は省いてよいが、型か範囲が不正なら parse_configure が nullopt を返す")
{
	// 必須のフィールドだけの configure は読める (任意のフィールドは省いてよい)。
	CHECK(parse_configure(*try_parse_json(
	                    R"({"type":"configure","streams":[{"stream_id":1,"user_id":"a","kind":"screen"}]})"))
	                    .has_value());

	// width が文字列
	CHECK_FALSE(parse_configure(*try_parse_json(
	                    R"({"type":"configure","streams":[
	                          {"stream_id":1,"user_id":"a","kind":"screen","width":"1920"}]})"))
	                    .has_value());
	// width が 0 (意味のない値)
	CHECK_FALSE(parse_configure(*try_parse_json(
	                    R"({"type":"configure","streams":[
	                          {"stream_id":1,"user_id":"a","kind":"screen","width":0}]})"))
	                    .has_value());
	// fps が負
	CHECK_FALSE(parse_configure(*try_parse_json(
	                    R"({"type":"configure","streams":[
	                          {"stream_id":1,"user_id":"a","kind":"screen","fps":-1}]})"))
	                    .has_value());
	// height が 0
	CHECK_FALSE(parse_configure(*try_parse_json(
	                    R"({"type":"configure","streams":[
	                          {"stream_id":1,"user_id":"a","kind":"screen","height":0}]})"))
	                    .has_value());
	// audio が真偽値でない
	CHECK_FALSE(parse_configure(*try_parse_json(
	                    R"({"type":"configure","streams":[
	                          {"stream_id":1,"user_id":"a","kind":"screen","audio":"yes"}]})"))
	                    .has_value());
}

TEST_CASE("build_keyframe_request で作ったメッセージに、type と stream_id が入る")
{
	const std::string text = build_keyframe_request({99});
	const auto j = try_parse_json(text);
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kKeyframeRequest));
	CHECK(j->at("stream_id").get<uint32_t>() == 99);
}

TEST_CASE("parse_log は、lines が文字列の配列の log だけを読む")
{
	const auto ok = parse_log(*try_parse_json(R"({"type":"log","lines":["a","b"]})"));
	REQUIRE(ok.has_value());
	REQUIRE(ok->lines.size() == 2);
	CHECK(ok->lines[0] == "a");

	CHECK_FALSE(parse_log(*try_parse_json(R"({"type":"log","lines":["a",1]})")).has_value());
	CHECK_FALSE(parse_log(*try_parse_json(R"({"type":"log","lines":"a"})")).has_value());
	CHECK_FALSE(parse_log(*try_parse_json(R"({"type":"log"})")).has_value());
}

TEST_CASE("parse_log は、任意の alerts と stats を別々に読み、形が違えば全体を捨てる")
{
	const auto ok = parse_log(*try_parse_json(R"({"type":"log","lines":["e"],"alerts":["a1","a2"],"stats":["s"]})"));
	REQUIRE(ok.has_value());
	CHECK(ok->lines == std::vector<std::string>{"e"});
	CHECK(ok->alerts == std::vector<std::string>{"a1", "a2"});
	CHECK(ok->stats == std::vector<std::string>{"s"});

	// 古い送信側は lines だけを送る。
	const auto old = parse_log(*try_parse_json(R"({"type":"log","lines":["e"]})"));
	REQUIRE(old.has_value());
	CHECK(old->alerts.empty());
	CHECK(old->stats.empty());

	CHECK_FALSE(parse_log(*try_parse_json(R"({"type":"log","lines":[],"alerts":"a"})")).has_value());
	CHECK_FALSE(parse_log(*try_parse_json(R"({"type":"log","lines":[],"stats":[1]})")).has_value());
	CHECK_FALSE(parse_log(*try_parse_json(R"({"type":"log","alerts":["a"]})")).has_value());
}

TEST_CASE("build_stream_check は type と stream_id を書き、parse_stream_check_result は結果を読む")
{
	const auto j = try_parse_json(build_stream_check({7}));
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kStreamCheck));
	CHECK(j->at("stream_id").get<uint32_t>() == 7);

	const auto ok = parse_stream_check_result(
		*try_parse_json(R"({"type":"stream_check_result","stream_id":7,"outcome":"restarted","detail":"rtp=+30"})"));
	REQUIRE(ok.has_value());
	CHECK(ok->stream_id == 7);
	CHECK(ok->outcome == "restarted");
	CHECK(ok->detail == "rtp=+30");

	// 知らない結果の名前も読む。detail が文字列でなければ、そのキーだけをないものとする。
	const auto unknown =
		parse_stream_check_result(*try_parse_json(R"({"type":"stream_check_result","stream_id":1,"outcome":"x","detail":3})"));
	REQUIRE(unknown.has_value());
	CHECK(unknown->outcome == "x");
	CHECK(unknown->detail.empty());

	CHECK_FALSE(parse_stream_check_result(*try_parse_json(R"({"type":"stream_check_result","outcome":"x"})")).has_value());
	CHECK_FALSE(parse_stream_check_result(*try_parse_json(R"({"type":"stream_check_result","stream_id":-1,"outcome":"x"})"))
			    .has_value());
	CHECK_FALSE(
		parse_stream_check_result(*try_parse_json(R"({"type":"stream_check_result","stream_id":4294967296,"outcome":"x"})"))
			.has_value());
	CHECK_FALSE(parse_stream_check_result(*try_parse_json(R"({"type":"stream_check_result","stream_id":1,"outcome":2})"))
			    .has_value());
}

TEST_CASE("parse_facts は facts の中身を読まずにそのまま渡し、facts のキーがなければ nullopt を返す")
{
	const auto ok = parse_facts(*try_parse_json(R"({"type":"facts","facts":{"anything":[1,2,3]}})"));
	REQUIRE(ok.has_value());
	CHECK(ok->facts["anything"][1] == 2);

	CHECK_FALSE(parse_facts(*try_parse_json(R"({"type":"facts"})")).has_value());
}

// facts の中身 (stages など) を読むのは pipeline/facts_state.h なので、そのテストは
// tests/test_pipeline_facts_state.cpp にある。

TEST_CASE("parse_profiles は user_id、display_name、avatar_key、bot を読み、省いたフィールドは値を持たない")
{
	// profiles は受信側が受け取るだけなので、build_profiles はない (configure、log、facts と同じ)。
	const auto j = try_parse_json(
	        R"({"type":"profiles","profiles":[
	              {"user_id":"111","display_name":"たろう","avatar_key":"a1b2","bot":false},
	              {"user_id":"222","avatar_key":"","bot":true},
	              {"user_id":"333"}]})");
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kProfiles));

	const auto m = parse_profiles(*j);
	REQUIRE(m.has_value());
	REQUIRE(m->profiles.size() == 3);

	CHECK(m->profiles[0].user_id == "111");
	REQUIRE(m->profiles[0].display_name.has_value());
	CHECK(*m->profiles[0].display_name == "たろう");
	CHECK(m->profiles[0].avatar_key == std::optional<std::string>("a1b2"));
	CHECK_FALSE(m->profiles[0].bot);

	// 空の avatar_key は、アバターを設定していない人の値として持つ (値がないこととは区別する)。
	CHECK(m->profiles[1].avatar_key == std::optional<std::string>(""));
	CHECK(m->profiles[1].bot);

	// display_name、avatar_key、bot は任意で、省いても壊れているものとしない。bot を省いた人は Bot でない。
	CHECK(m->profiles[2].user_id == "333");
	CHECK_FALSE(m->profiles[2].display_name.has_value());
	CHECK_FALSE(m->profiles[2].avatar_key.has_value());
	CHECK_FALSE(m->profiles[2].bot);
}

TEST_CASE("profiles が空の配列でも、parse_profiles は読める")
{
	const auto m = parse_profiles(*try_parse_json(R"({"type":"profiles","profiles":[]})"));
	REQUIRE(m.has_value());
	CHECK(m->profiles.empty());
}

TEST_CASE("user_id、display_name、avatar_key のどれかの型が違うか、bot が真偽値でない要素があれば、parse_profiles が nullopt を返す")
{
	CHECK_FALSE(parse_profiles(*try_parse_json(R"({"type":"profiles","profiles":"nope"})")).has_value());
	CHECK_FALSE(parse_profiles(*try_parse_json(R"({"type":"profiles"})")).has_value());

	// user_id がない。
	CHECK_FALSE(parse_profiles(*try_parse_json(R"({"type":"profiles","profiles":[{}]})")).has_value());
	// user_id が文字列でない。
	CHECK_FALSE(parse_profiles(*try_parse_json(R"({"type":"profiles","profiles":[{"user_id":1}]})")).has_value());
	// display_name が文字列でない。
	CHECK_FALSE(parse_profiles(*try_parse_json(
	                    R"({"type":"profiles","profiles":[{"user_id":"1","display_name":1}]})"))
	                    .has_value());
	// avatar_key が文字列でない。
	CHECK_FALSE(parse_profiles(*try_parse_json(
	                    R"({"type":"profiles","profiles":[{"user_id":"1","avatar_key":1}]})"))
	                    .has_value());
	CHECK_FALSE(parse_profiles(*try_parse_json(
	                    R"({"type":"profiles","profiles":[{"user_id":"1","avatar_key":null}]})"))
	                    .has_value());
	// bot が真偽値でない。
	CHECK_FALSE(parse_profiles(*try_parse_json(
	                    R"({"type":"profiles","profiles":[{"user_id":"1","bot":"true"}]})"))
	                    .has_value());
	CHECK_FALSE(parse_profiles(*try_parse_json(
	                    R"({"type":"profiles","profiles":[{"user_id":"1","bot":1}]})"))
	                    .has_value());
	// 2 つ目の要素が壊れている (1 つ目は正しい)。
	CHECK_FALSE(parse_profiles(*try_parse_json(
	                    R"({"type":"profiles","profiles":[{"user_id":"1"},{"user_id":2}]})"))
	                    .has_value());
}

TEST_CASE("profiles にアバターの画像 (avatar_png) が載っていても、読まずにほかの値を読む")
{
	// アバターの画像を profiles に載せていた送信側の形。画像は avatar で届くものだけを使う。
	const auto m = parse_profiles(*try_parse_json(
		R"({"type":"profiles","profiles":[{"user_id":"1","display_name":"taro","avatar_png":"aGVsbG8="}]})"));
	REQUIRE(m.has_value());
	REQUIRE(m->profiles.size() == 1);
	CHECK(m->profiles[0].display_name == std::optional<std::string>("taro"));
	CHECK_FALSE(m->profiles[0].avatar_key.has_value());
}

// ---- avatar (送信側 -> 受信側。1 人のアバターの画像) ----

TEST_CASE("parse_avatar は user_id、avatar_key、avatar_png を読む")
{
	const auto j = try_parse_json(R"({"type":"avatar","user_id":"111","avatar_key":"a1b2","avatar_png":"aGVsbG8="})");
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kAvatar));

	const auto m = parse_avatar(*j);
	REQUIRE(m.has_value());
	CHECK(m->user_id == "111");
	CHECK(m->avatar_key == "a1b2");
	CHECK(m->avatar_png == "aGVsbG8=");

	// アバターを設定していない人の画像 (Discord の既定の画像) は、空の avatar_key で届く。
	const auto empty_key = parse_avatar(*try_parse_json(R"({"type":"avatar","user_id":"1","avatar_key":"","avatar_png":"YQ=="})"));
	REQUIRE(empty_key.has_value());
	CHECK(empty_key->avatar_key.empty());
}

TEST_CASE("3 つのキーのどれかがないか文字列でない avatar には、parse_avatar が nullopt を返す")
{
	CHECK_FALSE(parse_avatar(*try_parse_json(R"({"type":"avatar","avatar_key":"k","avatar_png":"YQ=="})")).has_value());
	CHECK_FALSE(parse_avatar(*try_parse_json(R"({"type":"avatar","user_id":"1","avatar_png":"YQ=="})")).has_value());
	CHECK_FALSE(parse_avatar(*try_parse_json(R"({"type":"avatar","user_id":"1","avatar_key":"k"})")).has_value());
	CHECK_FALSE(parse_avatar(*try_parse_json(R"({"type":"avatar","user_id":1,"avatar_key":"k","avatar_png":"YQ=="})"))
			    .has_value());
	CHECK_FALSE(parse_avatar(*try_parse_json(R"({"type":"avatar","user_id":"1","avatar_key":null,"avatar_png":"YQ=="})"))
			    .has_value());
	CHECK_FALSE(parse_avatar(*try_parse_json(R"({"type":"avatar","user_id":"1","avatar_key":"k","avatar_png":5})"))
			    .has_value());
	CHECK_FALSE(parse_avatar(*try_parse_json(R"([1,2,3])")).has_value());
}

TEST_CASE("avatar_png が上限を超える avatar は捨て、ちょうど上限は読む")
{
	const std::string within_limit(kMaxAvatarPngBase64Bytes, 'a');
	const std::string over_limit(kMaxAvatarPngBase64Bytes + 1, 'a');

	nlohmann::json within;
	within["type"] = control_type::kAvatar;
	within["user_id"] = "1";
	within["avatar_key"] = "k";
	within["avatar_png"] = within_limit;
	const auto read = parse_avatar(within);
	REQUIRE(read.has_value());
	CHECK(read->avatar_png.size() == kMaxAvatarPngBase64Bytes);

	nlohmann::json over = within;
	over["avatar_png"] = over_limit;
	CHECK_FALSE(parse_avatar(over).has_value());
}

// ---- recording (受信側 -> 送信側) ----

TEST_CASE("build_recording で作った recording を parse_recording で読むと、元の値に戻る")
{
	for (const bool on : {true, false}) {
		const std::string text = build_recording({on});
		const auto j = try_parse_json(text);
		REQUIRE(j.has_value());
		CHECK(peek_type(*j) == std::string(control_type::kRecording));

		const auto m = parse_recording(*j);
		REQUIRE(m.has_value());
		CHECK(m->on == on);
	}
}

TEST_CASE("recording のキーは type と on の 2 つだけである")
{
	const auto j = try_parse_json(build_recording({true}));
	REQUIRE(j.has_value());
	REQUIRE(j->is_object());
	CHECK(j->size() == 2);
	CHECK((*j)["on"].is_boolean());
	CHECK((*j)["on"].get<bool>());
}

TEST_CASE("on がないか真偽値でない recording には、parse_recording が例外を投げずに nullopt を返す")
{
	const char *bad[] = {
		R"({"type":"recording"})",           // on がない
		R"({"type":"recording","on":1})",    // 整数の 1 は真偽値でない
		R"({"type":"recording","on":"true"})", // 文字列も真偽値でない
		R"({"type":"recording","on":null})",
		R"(["recording",true])", // オブジェクトでない
	};
	for (const char *text : bad) {
		CAPTURE(text);
		const auto j = try_parse_json(text);
		REQUIRE(j.has_value());
		CHECK_FALSE(parse_recording(*j).has_value());
	}
}

// ---- media、limits、hello_ok の任意のキー (受信側 -> 送信側) ----

TEST_CASE("build_media で作った media のキーは type と on の 2 つで、parse_media で読むと元の値に戻る")
{
	for (const bool on : {true, false}) {
		const auto j = try_parse_json(build_media({on}));
		REQUIRE(j.has_value());
		CHECK(peek_type(*j) == std::string(control_type::kMedia));
		CHECK(j->size() == 2);
		const auto m = parse_media(*j);
		REQUIRE(m.has_value());
		CHECK(m->on == on);
	}
}

TEST_CASE("on がないか真偽値でない media には、parse_media が nullopt を返す")
{
	const char *bad[] = {
		R"({"type":"media"})",
		R"({"type":"media","on":0})",
		R"({"type":"media","on":"false"})",
		R"({"type":"media","on":null})",
		R"(["media",true])",
	};
	for (const char *text : bad) {
		CAPTURE(text);
		const auto j = try_parse_json(text);
		REQUIRE(j.has_value());
		CHECK_FALSE(parse_media(*j).has_value());
	}
}

TEST_CASE("build_limits は値のあるキーだけを書き、parse_limits で読むと元の値に戻る")
{
	LimitsMessage both;
	both.max_long_edge = 1280;
	both.fps_divisor = 2;
	LimitsMessage edge_only;
	edge_only.max_long_edge = 640;
	LimitsMessage fps_only;
	fps_only.fps_divisor = 1;
	LimitsMessage encoder_only;
	encoder_only.encoder_preference = "software";
	LimitsMessage latency_only;
	latency_only.latency_mode = "quality";
	LimitsMessage all_four;
	all_four.max_long_edge = 1920;
	all_four.fps_divisor = 1;
	all_four.encoder_preference = "hardware";
	all_four.latency_mode = "realtime";
	const LimitsMessage none{};

	for (const LimitsMessage &sent : {both, edge_only, fps_only, encoder_only, latency_only, all_four, none}) {
		const auto j = try_parse_json(build_limits(sent));
		REQUIRE(j.has_value());
		CHECK(peek_type(*j) == std::string(control_type::kLimits));
		// キーは type と、値のあるキーだけ (省いたキーは書かず、制限しないことを示す)。
		const std::size_t expected = 1 + (sent.max_long_edge ? 1u : 0u) + (sent.fps_divisor ? 1u : 0u) +
					     (sent.encoder_preference ? 1u : 0u) + (sent.latency_mode ? 1u : 0u);
		CHECK(j->size() == expected);
		const auto back = parse_limits(*j);
		REQUIRE(back.has_value());
		CHECK(*back == sent);
	}
}

TEST_CASE("型の違う値か範囲の外の値を含む limits には parse_limits が nullopt を返し、知らないキーは無視する")
{
	const char *bad[] = {
		R"({"type":"limits","max_long_edge":0})",
		R"({"type":"limits","max_long_edge":-640})",
		R"({"type":"limits","max_long_edge":1280.5})",
		R"({"type":"limits","max_long_edge":"1280"})",
		R"({"type":"limits","max_long_edge":4294967296})",
		R"({"type":"limits","fps_divisor":0})",
		R"({"type":"limits","fps_divisor":3})",
		R"({"type":"limits","fps_divisor":true})",
		R"({"type":"limits","max_long_edge":1280,"fps_divisor":4})", // 片方だけが壊れていても全体を捨てる
		R"({"type":"limits","encoder_preference":"gpu"})",
		R"({"type":"limits","encoder_preference":1})",
		R"({"type":"limits","latency_mode":"fast"})",
		R"({"type":"limits","latency_mode":true})",
		R"({"type":"limits","fps_divisor":2,"latency_mode":"slow"})",
		R"(["limits"])",
	};
	for (const char *text : bad) {
		CAPTURE(text);
		const auto j = try_parse_json(text);
		REQUIRE(j.has_value());
		CHECK_FALSE(parse_limits(*j).has_value());
	}

	// 知らないキーは無視する。
	const auto j = try_parse_json(R"({"type":"limits","fps_divisor":2,"prefer_software":true})");
	REQUIRE(j.has_value());
	const auto m = parse_limits(*j);
	REQUIRE(m.has_value());
	CHECK(m->fps_divisor == 2);
	CHECK_FALSE(m->max_long_edge.has_value());
}

TEST_CASE("limits_in_range は、max_long_edge が 0 のときと、fps_divisor が 1 でも 2 でもないときに false を返す")
{
	LimitsMessage ok;
	ok.max_long_edge = 1;
	ok.fps_divisor = 2;
	CHECK(limits_in_range(ok));
	CHECK(limits_in_range(LimitsMessage{}));

	LimitsMessage zero_edge;
	zero_edge.max_long_edge = 0;
	CHECK_FALSE(limits_in_range(zero_edge));
	LimitsMessage bad_fps;
	bad_fps.fps_divisor = 3;
	CHECK_FALSE(limits_in_range(bad_fps));

	// エンコードの設定は、決まった語だけが範囲の中にある。
	for (const char *v : {"hardware", "software"}) {
		LimitsMessage m;
		m.encoder_preference = v;
		CHECK(limits_in_range(m));
	}
	for (const char *v : {"realtime", "quality"}) {
		LimitsMessage m;
		m.latency_mode = v;
		CHECK(limits_in_range(m));
	}
	LimitsMessage bad_encoder;
	bad_encoder.encoder_preference = "gpu";
	CHECK_FALSE(limits_in_range(bad_encoder));
	LimitsMessage bad_latency;
	bad_latency.latency_mode = "fast";
	CHECK_FALSE(limits_in_range(bad_latency));
}

TEST_CASE("エンコードの設定のキーを知らない古い送信側は、キーを無視して 4 つのうち知っているキーだけを読む")
{
	// 古い送信側は max_long_edge と fps_divisor しか読まない。新しいキーが付いた limits でも、メッセージは捨てられず、
	// 知っているキーの値は変わらない (src-vencord/transport/control.ts の readLimitsFields は知らないキーを無視する)。
	// ここでは、新しい受信側が書いた JSON の、古いキーの部分が変わらないことを押さえる。
	LimitsMessage m;
	m.max_long_edge = 1280;
	m.fps_divisor = 2;
	m.encoder_preference = "software";
	m.latency_mode = "quality";
	const auto j = try_parse_json(build_limits(m));
	REQUIRE(j.has_value());
	CHECK((*j)["max_long_edge"] == 1280);
	CHECK((*j)["fps_divisor"] == 2);
	CHECK((*j)["encoder_preference"] == "software");
	CHECK((*j)["latency_mode"] == "quality");
}

TEST_CASE("hello_ok の limits にもエンコードの設定が往復し、壊れていればそのキーだけをないものとする")
{
	HelloOkMessage m;
	m.version = "0.1.0";
	LimitsMessage limits;
	limits.encoder_preference = "software";
	limits.latency_mode = "quality";
	m.limits = limits;
	const auto j = try_parse_json(build_hello_ok(m));
	REQUIRE(j.has_value());
	const auto back = parse_hello_ok(*j);
	REQUIRE(back.has_value());
	REQUIRE(back->limits.has_value());
	CHECK(*back->limits == limits);

	const auto broken = try_parse_json(
		R"({"type":"hello_ok","wire":0,"version":"v","limits":{"encoder_preference":"gpu"}})");
	REQUIRE(broken.has_value());
	const auto parsed = parse_hello_ok(*broken);
	REQUIRE(parsed.has_value());
	CHECK_FALSE(parsed->limits.has_value());
}

TEST_CASE("任意のキーの値がない hello_ok には、type と wire と version だけを書く")
{
	HelloOkMessage m;
	m.version = "0.1.0+dev";
	const auto j = try_parse_json(build_hello_ok(m));
	REQUIRE(j.has_value());
	CHECK(j->size() == 3);
	CHECK(j->find("media") == j->end());
	CHECK(j->find("limits") == j->end());

	const auto back = parse_hello_ok(*j);
	REQUIRE(back.has_value());
	CHECK(back->wire == kWireVersion);
	CHECK(back->version == "0.1.0+dev");
	CHECK_FALSE(back->media.has_value());
	CHECK_FALSE(back->limits.has_value());
}

TEST_CASE("hello_ok は media と limits の初期値を任意のキーとして書き、parse_hello_ok で読める")
{
	HelloOkMessage m;
	m.version = "0.1.0+abc1234";
	m.media = false;
	LimitsMessage limits;
	limits.max_long_edge = 960;
	m.limits = limits;

	const auto j = try_parse_json(build_hello_ok(m));
	REQUIRE(j.has_value());
	REQUIRE(j->find("limits") != j->end());
	CHECK((*j)["limits"].is_object());
	CHECK((*j)["limits"].size() == 1); // 省いた fps_divisor は書かない

	const auto back = parse_hello_ok(*j);
	REQUIRE(back.has_value());
	REQUIRE(back->media.has_value());
	CHECK_FALSE(*back->media);
	REQUIRE(back->limits.has_value());
	CHECK(*back->limits == limits);
}

TEST_CASE("範囲の外の limits は、build_hello_ok が書かない")
{
	HelloOkMessage m;
	LimitsMessage bad;
	bad.fps_divisor = 5;
	m.limits = bad;
	const auto j = try_parse_json(build_hello_ok(m));
	REQUIRE(j.has_value());
	CHECK(j->find("limits") == j->end());
}

TEST_CASE("任意のキーが壊れていても hello_ok は捨てず、そのキーだけをないものとする")
{
	const char *texts[] = {
		R"({"type":"hello_ok","wire":0,"version":"v","media":1})",
		R"({"type":"hello_ok","wire":0,"version":"v","limits":{"fps_divisor":3}})",
		R"({"type":"hello_ok","wire":0,"version":"v","limits":[1280]})",
	};
	for (const char *text : texts) {
		CAPTURE(text);
		const auto j = try_parse_json(text);
		REQUIRE(j.has_value());
		const auto m = parse_hello_ok(*j);
		REQUIRE(m.has_value());
		CHECK_FALSE(m->media.has_value());
		CHECK_FALSE(m->limits.has_value());
	}
	// 必須のキーがなければ、hello_ok が不正である。
	const auto no_version = try_parse_json(R"({"type":"hello_ok","wire":0})");
	REQUIRE(no_version.has_value());
	CHECK_FALSE(parse_hello_ok(*no_version).has_value());
}

// ---- profiles の username (表示名が同じ人を見分けるのに使う) ----

TEST_CASE("parse_profiles は username を読み、キーがなければ値を持たない")
{
	const auto m = parse_profiles(*try_parse_json(
		R"({"type":"profiles","profiles":[{"user_id":"1","display_name":"taro","username":"taro_1"},{"user_id":"2","display_name":"taro"}]})"));
	REQUIRE(m.has_value());
	REQUIRE(m->profiles.size() == 2);
	CHECK(m->profiles[0].username == std::optional<std::string>("taro_1"));
	CHECK_FALSE(m->profiles[1].username.has_value());
}

TEST_CASE("username が文字列でない要素があれば、parse_profiles が nullopt を返す")
{
	CHECK_FALSE(parse_profiles(*try_parse_json(R"({"type":"profiles","profiles":[{"user_id":"1","username":5}]})"))
			    .has_value());
	CHECK_FALSE(parse_profiles(*try_parse_json(R"({"type":"profiles","profiles":[{"user_id":"1","username":null}]})"))
			    .has_value());
}

TEST_CASE("空の username は、値がないものとして扱う")
{
	const auto m = parse_profiles(*try_parse_json(R"({"type":"profiles","profiles":[{"user_id":"1","username":""}]})"));
	REQUIRE(m.has_value());
	CHECK_FALSE(m->profiles[0].username.has_value());
}

// ---- main_account、receive_scope、move_to_main_call、leave_call (受信側 -> 送信側) ----

TEST_CASE("build_main_account で作った main_account を parse_main_account で読むと、元の値に戻る")
{
	for (const std::string &id : {std::string("1234"), std::string()}) {
		const auto j = try_parse_json(build_main_account({id}));
		REQUIRE(j.has_value());
		CHECK(peek_type(*j) == std::string(control_type::kMainAccount));
		CHECK(j->size() == 2);
		const auto m = parse_main_account(*j);
		REQUIRE(m.has_value());
		CHECK(m->user_id == id);
	}
}

TEST_CASE("user_id がないか文字列でない main_account には、parse_main_account が nullopt を返す")
{
	for (const char *text : {R"({"type":"main_account"})", R"({"type":"main_account","user_id":1})", "[]"}) {
		const auto j = try_parse_json(text);
		REQUIRE(j.has_value());
		CHECK_FALSE(parse_main_account(*j).has_value());
	}
}

TEST_CASE("receive_scope: 全員は user_ids を持たず、枠に載せた人だけは user_ids を持ち、どちらも往復する")
{
	ReceiveScopeMessage everyone;
	const auto j_all = try_parse_json(build_receive_scope(everyone));
	REQUIRE(j_all.has_value());
	CHECK(peek_type(*j_all) == std::string(control_type::kReceiveScope));
	CHECK((*j_all)["scope"] == "everyone");
	CHECK_FALSE(j_all->contains("user_ids"));
	const auto parsed_all = parse_receive_scope(*j_all);
	REQUIRE(parsed_all.has_value());
	CHECK(*parsed_all == everyone);

	ReceiveScopeMessage slots;
	slots.slots_only = true;
	slots.user_ids = {"1", "2"};
	const auto j_slots = try_parse_json(build_receive_scope(slots));
	REQUIRE(j_slots.has_value());
	CHECK((*j_slots)["scope"] == "slots");
	const auto parsed_slots = parse_receive_scope(*j_slots);
	REQUIRE(parsed_slots.has_value());
	CHECK(*parsed_slots == slots);

	// 誰も載せていなくても、枠に載せた人だけのままで、user_ids は空の配列になる。
	ReceiveScopeMessage nobody;
	nobody.slots_only = true;
	const auto parsed_nobody = parse_receive_scope(*try_parse_json(build_receive_scope(nobody)));
	REQUIRE(parsed_nobody.has_value());
	CHECK(parsed_nobody->slots_only);
	CHECK(parsed_nobody->user_ids.empty());
}

TEST_CASE("形が違う receive_scope には、parse_receive_scope が nullopt を返す")
{
	for (const char *text : {R"({"type":"receive_scope"})", R"({"type":"receive_scope","scope":"all"})",
				 R"({"type":"receive_scope","scope":"slots"})",
				 R"({"type":"receive_scope","scope":"slots","user_ids":[1]})",
				 R"({"type":"receive_scope","scope":1})"}) {
		const auto j = try_parse_json(text);
		REQUIRE(j.has_value());
		CHECK_FALSE(parse_receive_scope(*j).has_value());
	}
}

TEST_CASE("move_to_main_call と leave_call は type だけを持ち、移動先を運ばない")
{
	const auto move = try_parse_json(build_move_to_main_call());
	REQUIRE(move.has_value());
	CHECK(peek_type(*move) == std::string(control_type::kMoveToMainCall));
	CHECK(move->size() == 1);
	const auto leave = try_parse_json(build_leave_call());
	REQUIRE(leave.has_value());
	CHECK(peek_type(*leave) == std::string(control_type::kLeaveCall));
	CHECK(leave->size() == 1);
}

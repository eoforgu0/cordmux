// SPDX-License-Identifier: Apache-2.0

// bundle/bundle_files.h と bundle/bundle_manifest.h の単体テスト。トークンを 2 段階で守ることを確認する。伏せれば 2 段目で
// 検出されず、伏せる処理をやめると同じデータで検出されることで、2 段目の確認が働いていることも確認する。

#include "doctest.h"

#include "bundle/bundle_files.h"
#include "ws/log_sink.h"

#include <string>
#include <vector>

using cordmux::bundle::build_bundle_entries;
using cordmux::bundle::build_manifest_json;
using cordmux::bundle::BundleMaterials;
using cordmux::bundle::bundle_reveals_secret;
using cordmux::bundle::make_redact;
using cordmux::bundle::ManifestContact;
using cordmux::bundle::ManifestInput;
using cordmux::bundle::ManifestVersion;
using cordmux::bundle::redact_secret_keys;
using cordmux::bundle::RedactFn;
using cordmux::bundle::RevealFn;
using cordmux::bundle::ZipEntry;
using cordmux::pipeline::DummyInterval;

namespace {

// テストだけで使うトークン (実際のトークンと同じ 16 進の 64 文字の形)。
const std::string kSecret = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

RevealFn make_reveal()
{
	// ポート番号は調べない (manifest.json に開いたポート番号を入れるため。bundle/bundle_files.h の RevealFn)。
	return [](std::string_view text) { return cordmux::ws::text_reveals_secret_or_port(text, kSecret, 0); };
}

const ZipEntry &entry_named(const std::vector<ZipEntry> &entries, const std::string &name)
{
	for (const ZipEntry &entry : entries) {
		if (entry.name == name)
			return entry;
	}
	REQUIRE_MESSAGE(false, "その名前の中身が zip にない");
	return entries.front();
}

ManifestInput sample_manifest()
{
	ManifestInput input;
	input.taken_at_us = 946'684'800'000'000;
	input.utc_offset_sec = 9 * 3600;
	input.versions.push_back(ManifestVersion{"plugin", "0.1.0"});
	input.versions.push_back(ManifestVersion{"discord_side", ""}); // 読めていないバージョンは空のまま
	input.contacts.push_back(ManifestContact{"pc-hook", "unavailable", 0});
	input.contacts.push_back(ManifestContact{"sharing", "ok", 946'684'800'000'000});
	input.dummy_intervals.push_back(DummyInterval{100, 200});
	input.dummy_intervals.push_back(DummyInterval{300, 0}); // まだ続いている
	input.facts.listening_port = 4471;
	input.facts.listener_outcome = "opened";
	input.facts.listener_open_problem = "session_file_not_saved";
	input.facts.secret_mismatches = 2;
	return input;
}

} // namespace

TEST_CASE("manifest.json には、バージョン、Discord の内部への依存箇所、ダミーの時間帯、cordmux.dll だけで分かる値が入る")
{
	const std::string text = build_manifest_json(sample_manifest());
	const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
	REQUIRE_FALSE(j.is_discarded());

	CHECK(j.at("versions").at("plugin") == "0.1.0");
	CHECK(j.at("versions").at("discord_side") == ""); // 空のまま入る
	REQUIRE(j.at("contacts").size() == 2);
	CHECK(j.at("contacts")[0].at("id") == "pc-hook");
	CHECK(j.at("contacts")[1].at("last_ok_at_us") == 946'684'800'000'000);

	REQUIRE(j.at("dummy_intervals").size() == 2);
	CHECK(j.at("dummy_intervals")[0].at("ended_at_us") == 200);
	// 続いている時間帯は終わりの時刻を持たない (0 を、1970 年に終わったと読ませない)。
	CHECK(j.at("dummy_intervals")[1].contains("ended_at_us") == false);
	CHECK(j.at("dummy_intervals")[1].at("open") == true);

	CHECK(j.at("cordmux_only_facts").at("listening_port") == 4471); // ポート番号は伏せない
	CHECK(j.at("cordmux_only_facts").at("secret_mismatches") == 2);
	// 待ち受けは始めたが session.json を保存できなかったときは、outcome の "opened" と一緒に problem が入る。
	CHECK(j.at("cordmux_only_facts").at("listener_outcome") == "opened");
	CHECK(j.at("cordmux_only_facts").at("listener_open_problem") == "session_file_not_saved");
	CHECK_FALSE(j.contains("verified_environment"));
}

TEST_CASE("Vesktop が一度も繋がっていない日の manifest.json にも、OBS 側だけで分かる事実が全部入る")
{
	ManifestInput input;
	input.facts.listener_attempted = true;
	input.facts.listener_outcome = "opened";
	input.facts.listener_open_problem = "none";
	input.facts.listening = true;
	input.facts.listening_port = 4470;
	input.facts.last_disconnect_reason = "unknown";
	input.facts.secret_mismatches = 3;
	input.facts.state_json_written_at_us = 946'684'800'000'000;
	input.facts.bundle_hash_state = "matches";
	input.facts.bundle_hash_checked_at_us = 946'684'900'000'000;
	input.facts.vesktop_running = true;
	input.facts.rejected_clients = 4;
	input.facts.hw_fallback_streams = 2;

	const nlohmann::json j = nlohmann::json::parse(build_manifest_json(input), nullptr, false);
	REQUIRE_FALSE(j.is_discarded());
	const nlohmann::json &facts = j.at("cordmux_only_facts");

	// 接続の来た時刻と切れた時刻は、一度も来ていないので 0 のまま入る (読む人が、来ていないと読める)。
	CHECK(facts.at("connections_accepted") == 0);
	CHECK(facts.at("connection_opened_at_us") == 0);
	CHECK(facts.at("hello_ok_at_us") == 0);
	CHECK(facts.at("connection_closed_at_us") == 0);
	CHECK(facts.at("last_disconnect_reason") == "unknown");
	// トークンが合わずに切った回数と、開いたポート、Vesktop の設定を書いた時刻、同梱の Vencord の 4 つのファイルの照合。
	CHECK(facts.at("secret_mismatches") == 3);
	CHECK(facts.at("listening") == true);
	CHECK(facts.at("listening_port") == 4470);
	CHECK(facts.at("state_json_written_at_us") == 946'684'800'000'000);
	CHECK(facts.at("bundle_hash_state") == "matches");
	CHECK(facts.at("bundle_hash_checked_at_us") == 946'684'900'000'000);
	CHECK(facts.at("bundle_hash_missing").is_array());
	CHECK(facts.at("bundle_hash_differing").is_array());
	CHECK(facts.at("vesktop_running") == true);
	// 2 つ目以降の接続を断った回数と、ソフトウェアデコードに切り替えた画面のストリームの数。
	CHECK(facts.at("rejected_clients") == 4);
	CHECK(facts.at("hw_fallback_streams") == 2);

	// Discord 側の記録は何も届いていないので、discord.log は空で、捨てた行の注記も付かない。
	BundleMaterials materials;
	materials.manifest = input;
	const std::vector<ZipEntry> entries = build_bundle_entries(materials, make_redact(kSecret));
	CHECK(entry_named(entries, "discord.log").content.empty());
}

TEST_CASE("同じ入力には、build_manifest_json が 1 文字も違わない文字列を返す (時計もファイルも読まない)")
{
	CHECK(build_manifest_json(sample_manifest()) == build_manifest_json(sample_manifest()));
}

TEST_CASE("redact_secret_keys は、秘密の値を持つキーの値だけを伏せる")
{
	// 語に分けてから完全一致で比べるので、camelCase も snake_case も検出する。
	CHECK(redact_secret_keys("{\"accessToken\":\"abc\"}") == "{\"accessToken\":\"[redacted]\"}");
	CHECK(redact_secret_keys("{\"pairing_code\":\"1234\"}") == "{\"pairing_code\":\"[redacted]\"}");
	CHECK(redact_secret_keys("{\"secret\": \"xyz\"}") == "{\"secret\": \"[redacted]\"}");

	// 部分一致で比べると伏せてしまう語が、残ること。
	CHECK(redact_secret_keys("{\"codec\":\"h264\"}") == "{\"codec\":\"h264\"}");
	CHECK(redact_secret_keys("{\"encoder\":\"x264\"}") == "{\"encoder\":\"x264\"}");
	CHECK(redact_secret_keys("{\"keyframe_request\":\"1\"}") == "{\"keyframe_request\":\"1\"}");

	// 値が文字列でないキーと、キーに見えるだけの文字列は変えない。
	CHECK(redact_secret_keys("{\"token\":12}") == "{\"token\":12}");
	CHECK(redact_secret_keys("\"secret\" はトークンのこと") == "\"secret\" はトークンのこと");
}

TEST_CASE("make_redact が作る関数は、トークンの文字列も伏せる")
{
	const RedactFn redact = make_redact(kSecret);
	const std::string line = "hello secret=" + kSecret + " の後ろ";
	const std::string redacted = redact(line);
	CHECK(redacted.find(kSecret) == std::string::npos);
	CHECK(redacted.find("[redacted]") != std::string::npos);
	CHECK(redacted.find("の後ろ") != std::string::npos); // 行全体は消さない

	// トークンがないとき (待ち受けを始めていないとき) は、トークンを伏せる処理は何もしない。
	const RedactFn no_secret = make_redact("");
	CHECK(no_secret("ふつうの行") == "ふつうの行");
}

TEST_CASE("zip に入るのは 6 つのファイルで、出来事の行と統計の行を別のファイルにし、捨てたログの行の数も書く")
{
	BundleMaterials materials;
	materials.manifest = sample_manifest();
	materials.plugin_log = {"1 行目", "2 行目"};
	materials.plugin_log_dropped = 7;
	materials.plugin_stats_log = {"Stats stream=1"};
	materials.plugin_stats_log_dropped = 3;
	materials.discord_log = {"js の行"};
	materials.discord_stats_log = {"playout stream 1"};
	materials.discord_stats_log_dropped = 5;
	materials.manifest.facts.plugin_stats_log_dropped = 3;
	materials.manifest.facts.discord_stats_log_dropped = 5;
	materials.settings_json = "{\"decoder\":\"software\"}\n";

	const std::vector<ZipEntry> entries = build_bundle_entries(materials, make_redact(kSecret));
	REQUIRE(entries.size() == 6);
	CHECK(entries[0].name == "manifest.json");
	CHECK(entries[1].name == "plugin.log");
	CHECK(entries[2].name == "plugin-stats.log");
	CHECK(entries[3].name == "discord.log");
	CHECK(entries[4].name == "discord-stats.log");
	CHECK(entries[5].name == "settings.json");

	// 捨てた行があることを、ログのファイルにも書く (途中から始まるログを、そうと分からないまま渡さない)。
	CHECK(entry_named(entries, "plugin.log").content.find("dropped 7") != std::string::npos);
	CHECK(entry_named(entries, "plugin.log").content.find("2 行目\n") != std::string::npos);
	CHECK(entry_named(entries, "plugin.log").content.find("Stats stream=1") == std::string::npos);
	CHECK(entry_named(entries, "plugin-stats.log").content.find("dropped 3") != std::string::npos);
	CHECK(entry_named(entries, "plugin-stats.log").content.find("Stats stream=1\n") != std::string::npos);
	CHECK(entry_named(entries, "discord.log").content.find("dropped") == std::string::npos);
	CHECK(entry_named(entries, "discord-stats.log").content.find("dropped 5") != std::string::npos);
	CHECK(entry_named(entries, "discord-stats.log").content.find("playout stream 1\n") != std::string::npos);

	// manifest にも、4 つのログの捨てた行の数を別々に書く。
	const nlohmann::json j = nlohmann::json::parse(entry_named(entries, "manifest.json").content, nullptr, false);
	REQUIRE(j.is_object());
	CHECK(j.at("cordmux_only_facts").at("plugin_stats_log_dropped") == 3);
	CHECK(j.at("cordmux_only_facts").at("discord_stats_log_dropped") == 5);
}

TEST_CASE("トークンを含むデータは、伏せれば 2 段目で検出されず、伏せなければ検出される")
{
	BundleMaterials materials;
	materials.manifest = sample_manifest();
	// トークンがデータに混ざった状態を作る (送信側のログの行にトークンが入ることがある)。
	materials.discord_log = {"ws open secret=" + kSecret};
	materials.settings_json = "{\"decoder\":\"software\"}\n";

	const RevealFn reveals = make_reveal();

	// (1) 通常の処理。伏せてから zip の中身を作れば、2 段目で検出されない。
	const std::vector<ZipEntry> redacted = build_bundle_entries(materials, make_redact(kSecret));
	CHECK_FALSE(bundle_reveals_secret(redacted, reveals));

	// (2) 伏せる関数を何もしない関数にすると、同じデータで検出される (2 段目の確認が働いていることを確認する)。
	const RedactFn identity = [](const std::string &text) { return text; };
	CHECK(bundle_reveals_secret(build_bundle_entries(materials, identity), reveals));

	// (3) 確認する関数を渡さなければ検出しない (呼び出し元が渡し忘れると止まらないことを、テストで示す)。
	CHECK_FALSE(bundle_reveals_secret(build_bundle_entries(materials, identity), RevealFn{}));
}

TEST_CASE("bundle_reveals_secret は、どのファイルの中身にトークンがあっても検出する")
{
	std::vector<ZipEntry> entries = {{"manifest.json", "何もない"}};
	CHECK_FALSE(bundle_reveals_secret(entries, make_reveal()));
	entries.push_back({"plugin.log", "前の行\n" + kSecret + "\n後の行"});
	CHECK(bundle_reveals_secret(entries, make_reveal()));
}

namespace {

// 17 桁から 20 桁の数が文字列に残っているか。
bool has_user_id_like_number(const std::string &text)
{
	std::size_t i = 0;
	while (i < text.size()) {
		if (text[i] < '0' || text[i] > '9') {
			++i;
			continue;
		}
		std::size_t end = i;
		while (end < text.size() && text[end] >= '0' && text[end] <= '9')
			++end;
		if (end - i >= 17 && end - i <= 20)
			return true;
		i = end;
	}
	return false;
}

} // namespace

TEST_CASE("Discord のユーザーIDは、同じIDなら全部のファイルで同じ印、違うIDなら違う印に置き換わり、17 桁から 20 桁の数が残らない")
{
	BundleMaterials materials;
	materials.manifest = sample_manifest();
	materials.plugin_log = {"main user 123456789012345678", "peer 98765432109876543 joined"};
	materials.discord_log = {"screen entry: pcId=1 userId=98765432109876543 streamId=2",
				 "voice entry: userId=123456789012345678 other=12345678901234567890"};
	materials.settings_json = "{\"broadcaster_user_id\":\"123456789012345678\",\"other\":\"11111111111111111\"}\n";

	const std::vector<ZipEntry> entries = build_bundle_entries(materials, make_redact(kSecret));

	// 現れた順 (manifest、plugin.log、plugin-stats.log、discord.log、discord-stats.log、settings.json の順)。
	const std::string &plugin = entry_named(entries, "plugin.log").content;
	CHECK(plugin == "main user user-1\npeer user-2 joined\n");
	const std::string &discord = entry_named(entries, "discord.log").content;
	CHECK(discord == "screen entry: pcId=1 userId=user-2 streamId=2\nvoice entry: userId=user-1 other=user-3\n");
	CHECK(entry_named(entries, "settings.json").content ==
	      "{\"broadcaster_user_id\":\"user-1\",\"other\":\"user-4\"}\n");

	for (const ZipEntry &entry : entries)
		CHECK_FALSE(has_user_id_like_number(entry.content));
}

TEST_CASE("16 桁以下と 21 桁以上の数は置き換えない")
{
	BundleMaterials materials;
	materials.plugin_log = {"t=1234567890123456 u=123456789012345678901 v=12345"};
	const std::vector<ZipEntry> entries = build_bundle_entries(materials, RedactFn{});
	CHECK(entry_named(entries, "plugin.log").content == "t=1234567890123456 u=123456789012345678901 v=12345\n");
}

TEST_CASE("英字につながった数字の並び (16 進のハッシュの中など) は、17 桁以上でも置き換えない")
{
	// 英字を含む 64 桁の 16 進で、中に 17 桁の数字だけの並びがある。
	const std::string hash = "ab" + std::string("12345678901234567") + "cd" + std::string(43, 'e');
	REQUIRE(hash.size() == 64);
	BundleMaterials materials;
	materials.settings_json = "sha256 " + hash + "\n";
	materials.plugin_log = {"x" + std::string("12345678901234567"), std::string("12345678901234567") + "y"};
	const std::vector<ZipEntry> entries = build_bundle_entries(materials, RedactFn{});
	CHECK(entry_named(entries, "settings.json").content == "sha256 " + hash + "\n");
	CHECK(entry_named(entries, "plugin.log").content == "x12345678901234567\n12345678901234567y\n");
}

TEST_CASE("ユーザーIDは、=・引用符・/・空白・コンマ・行頭・行末で区切られていれば置き換える")
{
	const std::string id = "12345678901234567";
	BundleMaterials materials;
	materials.plugin_log = {id + " start", "end " + id, "a=" + id, "\"" + id + "\"", "p/" + id + "/q", "1," + id + ",2",
				"(" + id + ")", "k:" + id};
	const std::vector<ZipEntry> entries = build_bundle_entries(materials, RedactFn{});
	CHECK(entry_named(entries, "plugin.log").content ==
	      "user-1 start\nend user-1\na=user-1\n\"user-1\"\np/user-1/q\n1,user-1,2\n(user-1)\nk:user-1\n");
}

TEST_CASE("プロファイルのフォルダーは、区切りが \\ と / と JSON の \\\\ の3つの形で %USERPROFILE% に置き換わる")
{
	BundleMaterials materials;
	materials.user_profile_dir = "D:\\home\\taro";
	materials.plugin_log = {"saved to D:\\home\\taro\\Videos\\cordmux", "also D:/home/taro/AppData",
				"lower d:\\home\\TARO\\x", "other D:\\home\\taro2\\y"};
	materials.settings_json = "{\"record_dir\":\"D:\\\\home\\\\taro\\\\Videos\"}\n";

	const std::vector<ZipEntry> entries = build_bundle_entries(materials, RedactFn{});
	CHECK(entry_named(entries, "plugin.log").content ==
	      "saved to %USERPROFILE%\\Videos\\cordmux\nalso %USERPROFILE%/AppData\nlower %USERPROFILE%\\x\n"
	      "other D:\\home\\taro2\\y\n");
	CHECK(entry_named(entries, "settings.json").content == "{\"record_dir\":\"%USERPROFILE%\\\\Videos\"}\n");
	for (const ZipEntry &entry : entries)
		CHECK(entry.content.find("taro\\") == std::string::npos);
}

TEST_CASE("プロファイルのフォルダーが分からない (空) ときは、名前を推測して置き換えない")
{
	BundleMaterials materials;
	materials.plugin_log = {"saved to D:\\home\\taro\\Videos"};
	const std::vector<ZipEntry> entries = build_bundle_entries(materials, RedactFn{});
	CHECK(entry_named(entries, "plugin.log").content == "saved to D:\\home\\taro\\Videos\n");
}

TEST_CASE("ユーザーIDとプロファイルのフォルダーの置き換えは、トークンを伏せる処理と一緒に動く")
{
	BundleMaterials materials;
	materials.manifest = sample_manifest();
	materials.user_profile_dir = "D:\\home\\taro";
	materials.discord_log = {"ws open secret=" + kSecret + " userId=123456789012345678 dir=D:\\home\\taro\\x"};
	materials.settings_json = "{\"accessToken\":\"abc\",\"id\":\"123456789012345678\"}\n";

	const std::vector<ZipEntry> entries = build_bundle_entries(materials, make_redact(kSecret));
	CHECK_FALSE(bundle_reveals_secret(entries, make_reveal()));
	CHECK(entry_named(entries, "discord.log").content ==
	      "ws open secret=[redacted] userId=user-1 dir=%USERPROFILE%\\x\n");
	CHECK(entry_named(entries, "settings.json").content == "{\"accessToken\":\"[redacted]\",\"id\":\"user-1\"}\n");
}

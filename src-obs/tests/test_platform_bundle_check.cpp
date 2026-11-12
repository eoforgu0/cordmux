// SPDX-License-Identifier: Apache-2.0

// platform/sha256.h と platform/bundle_check.h の単体テスト。
//
// SHA-256 のテストベクターは、FIPS 180-4 とその例で広く使われている 4 つの入力 (空、"abc"、56 バイト、112 バイト) と、
// 100 万文字の "a" である。1 つでも合わなければ、この実装を使う照合を信用できないので、テストを失敗にする。

#include "doctest.h"

#include "platform/bundle_check.h"
#include "platform/platform.h"
#include "platform/sha256.h"

#include <chrono>
#include <string>
#include <vector>

using cordmux::platform::BundleState;
using cordmux::platform::compare_bundle;
using cordmux::platform::FileDigest;
using cordmux::platform::join_path;
using cordmux::platform::kVencordBundleFileCount;
using cordmux::platform::kVencordBundleFiles;
using cordmux::platform::normalize_dir_path;
using cordmux::platform::parse_vencord_dir;
using cordmux::platform::parse_vencord_dir_backup;
using cordmux::platform::same_dir_path;
using cordmux::platform::Sha256;
using cordmux::platform::sha256_hex;

namespace {

// 4 つのファイルがすべて、同じ中身で読めた状態 (照合で一致するはずの形)。
std::vector<FileDigest> digests_all(const std::string &hex, bool found = true)
{
	std::vector<FileDigest> list;
	for (std::size_t i = 0; i < kVencordBundleFileCount; ++i) {
		FileDigest d;
		d.name = kVencordBundleFiles[i];
		d.found = found;
		d.sha256_hex = found ? hex : std::string();
		list.push_back(d);
	}
	return list;
}

} // namespace

TEST_CASE("sha256_hex は、公表されているテストベクターと同じ値を返す")
{
	CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
	      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopq"
			  "rsmnopqrstnopqrstu") == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

	// 100 万文字の "a" (長さの数え方と、ブロックの区切りをまたぐ処理を確認する)。
	const std::string million(1000000, 'a');
	CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Sha256 は、データをどこで区切って渡しても同じ値を返す")
{
	// 区切りの位置を 1 バイトずつ変えても、1 回で渡したときと同じ値でなければならない (ファイルを分けて読むので、
	// この性質が必要である)。長さは、ブロック (64 バイト) の区切りの前後をまたぐようにする。
	std::string data;
	for (int i = 0; i < 200; ++i)
		data.push_back(char('A' + (i % 26)));
	const std::string expected = sha256_hex(data);

	for (std::size_t split = 0; split <= data.size(); ++split) {
		Sha256 hasher;
		hasher.update(data.substr(0, split));
		hasher.update(data.substr(split));
		CHECK(hasher.final_hex() == expected);
	}

	// 3 つに分けても同じ。
	Sha256 three;
	three.update(data.substr(0, 63));
	three.update(data.substr(63, 2));
	three.update(data.substr(65));
	CHECK(three.final_hex() == expected);
}

TEST_CASE("1 バイトでも違うデータには、sha256_hex が違う値を返す")
{
	CHECK(sha256_hex("abc") != sha256_hex("abd"));
	CHECK(sha256_hex(std::string("a\0b", 3)) != sha256_hex("a"));
}

TEST_CASE("4 つのファイルがどれもコピー元と同じなら、compare_bundle は Matches を返す")
{
	const auto same = digests_all(sha256_hex("payload"));
	const auto result = compare_bundle(same, same);
	CHECK(result.state == BundleState::Matches);
	CHECK(result.differing.empty());
	CHECK(result.missing.empty());
}

TEST_CASE("中身が違うファイルが 1 つでもあれば、compare_bundle は Mismatch を返し、その名前を differing に入れる")
{
	const auto original = digests_all(sha256_hex("payload"));
	auto shown = original;
	shown[2].sha256_hex = sha256_hex("overwritten by force update");

	const auto result = compare_bundle(original, shown);
	CHECK(result.state == BundleState::Mismatch);
	REQUIRE(result.differing.size() == 1);
	CHECK(result.differing[0] == std::string(kVencordBundleFiles[2]));
	CHECK(result.missing.empty());
}

TEST_CASE("data/vencord から読めないファイルの名前は、missing に入る")
{
	const auto original = digests_all(sha256_hex("payload"));
	auto shown = original;
	shown[0].found = false;
	shown[0].sha256_hex.clear();

	const auto result = compare_bundle(original, shown);
	CHECK(result.state == BundleState::Mismatch);
	REQUIRE(result.missing.size() == 1);
	CHECK(result.missing[0] == std::string(kVencordBundleFiles[0]));
}

TEST_CASE("data/vencord の一覧が空なら、4 つのファイルがないものとして Mismatch を返す")
{
	const auto result = compare_bundle(digests_all(sha256_hex("payload")), {});
	CHECK(result.state == BundleState::Mismatch);
	CHECK(result.missing.size() == kVencordBundleFileCount);
}

TEST_CASE("コピー元が読めなければ、compare_bundle は Unknown を返し、違うとは言わない")
{
	auto original = digests_all(sha256_hex("payload"));
	original[1].found = false;
	const auto shown = digests_all(sha256_hex("something else"));

	const auto result = compare_bundle(original, shown);
	CHECK(result.state == BundleState::Unknown);
	CHECK(result.differing.empty()); // 比べられないので、名前も入れない
	CHECK(result.missing.empty());
}

TEST_CASE("コピー元の一覧が空なら、compare_bundle は Unknown を返す")
{
	CHECK(compare_bundle({}, digests_all(sha256_hex("x"))).state == BundleState::Unknown);
}

TEST_CASE("一覧の並びが違っても、compare_bundle はファイルの名前で対応させて比べる")
{
	auto original = digests_all(sha256_hex("payload"));
	auto shown = original;
	std::swap(shown[0], shown[3]);
	CHECK(compare_bundle(original, shown).state == BundleState::Matches);
}

TEST_CASE("parse_vencord_dir は state.json の vencordDir を読む")
{
	const auto dir = parse_vencord_dir(R"({"vencordDir": "C:\\x\\cordmux\\data\\vencord", "other": 1})");
	REQUIRE(dir.has_value());
	CHECK(*dir == "C:\\x\\cordmux\\data\\vencord");
}

TEST_CASE("vencordDir のキーがないか、文字列でないか、JSON が壊れているときは、parse_vencord_dir が nullopt を返す")
{
	CHECK_FALSE(parse_vencord_dir(R"({"other": 1})").has_value());
	CHECK_FALSE(parse_vencord_dir(R"({"vencordDir": 42})").has_value());
	CHECK_FALSE(parse_vencord_dir(R"({"vencordDir": null})").has_value());
	CHECK_FALSE(parse_vencord_dir("{not json").has_value());
	CHECK_FALSE(parse_vencord_dir("[]").has_value());
	CHECK_FALSE(parse_vencord_dir("").has_value());
}

TEST_CASE("vencordDir が空の文字列なら、parse_vencord_dir は空の文字列を返し、キーがないことと区別する")
{
	const auto dir = parse_vencord_dir(R"({"vencordDir": ""})");
	REQUIRE(dir.has_value());
	CHECK(dir->empty());
}

TEST_CASE("parse_vencord_dir_backup は、バックアップの written と written_at を読む")
{
	const auto backup = parse_vencord_dir_backup(
		R"({"previous": null, "written": "C:/x/data/vencord", "written_at": 946684800000000, "dll_dir": "C:/x"})");
	REQUIRE(backup.has_value());
	REQUIRE(backup->written.has_value());
	CHECK(*backup->written == "C:/x/data/vencord");
	REQUIRE(backup->written_at_us.has_value());
	CHECK(*backup->written_at_us == 946684800000000LL);
}

TEST_CASE("written_at が壊れていても、parse_vencord_dir_backup は written を読む")
{
	const auto backup = parse_vencord_dir_backup(R"({"written": "C:/x", "written_at": "きのう"})");
	REQUIRE(backup.has_value());
	CHECK(backup->written.has_value());
	CHECK_FALSE(backup->written_at_us.has_value());
}

TEST_CASE("JSON として読めないバックアップには、parse_vencord_dir_backup が nullopt を返す")
{
	CHECK_FALSE(parse_vencord_dir_backup("{壊れている").has_value());
	CHECK_FALSE(parse_vencord_dir_backup("[]").has_value());

	// キーが 1 つもないバックアップは、読めたが何も書いていないので、値を返す (中身は空)。
	const auto empty = parse_vencord_dir_backup("{}");
	REQUIRE(empty.has_value());
	CHECK_FALSE(empty->written.has_value());
	CHECK_FALSE(empty->written_at_us.has_value());
}

TEST_CASE("same_dir_path は、区切りの種類、大文字と小文字、末尾の区切りの違いを無視して比べる")
{
	CHECK(same_dir_path("C:\\x\\data\\vencord", "c:/x/data/vencord"));
	CHECK(same_dir_path("C:/x/data/vencord/", "C:/x/data/vencord"));
	CHECK(same_dir_path("C:/x/data/vencord//", "C:/x/data/vencord"));
	CHECK_FALSE(same_dir_path("C:/x/data/vencord", "C:/x/data/vencord-src"));
	CHECK_FALSE(same_dir_path("C:/x/data/vencord", "D:/x/data/vencord"));
	CHECK_FALSE(same_dir_path("", "C:/x"));
	CHECK(same_dir_path("", ""));
	CHECK(normalize_dir_path("C:\\X\\Y\\") == "c:/x/y");
}

TEST_CASE("join_path は、区切りを重ねずにフォルダーとファイル名をつなぐ")
{
	CHECK(join_path("C:/x/data/vencord", "package.json") == "C:/x/data/vencord/package.json");
	CHECK(join_path("C:/x/data/vencord/", "package.json") == "C:/x/data/vencord/package.json");
	CHECK(join_path("C:\\x\\data\\vencord\\", "package.json") == "C:\\x\\data\\vencord/package.json");
}

TEST_CASE("比べるのは、Vesktop が Vencord のフォルダーで読む 4 つのファイルである")
{
	REQUIRE(kVencordBundleFileCount == 4);
	CHECK(std::string(kVencordBundleFiles[0]) == "vencordDesktopMain.js");
	CHECK(std::string(kVencordBundleFiles[1]) == "vencordDesktopPreload.js");
	CHECK(std::string(kVencordBundleFiles[2]) == "vencordDesktopRenderer.js");
	CHECK(std::string(kVencordBundleFiles[3]) == "vencordDesktopRenderer.css");
}

TEST_CASE("current_process_started_at_us は、このプロセスの起動の時刻をエポックからのマイクロ秒で返し、その値はいまより前である")
{
	// find_vesktop_process が返す起動の時刻と同じ変換を通る値である (platform/platform.h の current_process_started_at_us)。
	// 1601 年からの値のままだったり、100 ナノ秒単位のままだったりすると、この範囲を大きく外れる。
	const std::int64_t started = cordmux::platform::current_process_started_at_us();
	REQUIRE(started > 0);

	const std::int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
					 std::chrono::system_clock::now().time_since_epoch())
					 .count();
	CHECK(started <= now + 60LL * 1000 * 1000);            // 未来ではない (1 分の余裕を見る)
	CHECK(started >= now - 24LL * 60 * 60 * 1000 * 1000);  // テストは 1 日以上前には始まっていない
}

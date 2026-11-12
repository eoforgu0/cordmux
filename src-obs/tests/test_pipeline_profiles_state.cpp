// SPDX-License-Identifier: Apache-2.0

// pipeline/profiles_state.h の単体テスト。base64 のデコードと、届いた profiles の保持の仕方 (1 通で人の表を
// 置き換える) と、参加者ごとに届くアバターの保持の仕方 (バージョンが変わった人と抜けた人の画像を捨てる、読めない画像を捨てて
// 数える) を確認する。

#include "doctest.h"

#include "pipeline/profiles_state.h"

#include <string>
#include <vector>

using cordmux::pipeline::apply_avatar;
using cordmux::pipeline::decode_base64;
using cordmux::pipeline::make_profiles_snapshot;
using cordmux::pipeline::next_profiles_snapshot;
using cordmux::pipeline::ProfilesSnapshot;
using cordmux::ws::AvatarMessage;
using cordmux::ws::ProfileEntry;
using cordmux::ws::ProfilesMessage;

namespace {

ProfileEntry entry(const std::string &user_id)
{
	ProfileEntry out;
	out.user_id = user_id;
	return out;
}

ProfileEntry keyed(const std::string &user_id, const std::string &avatar_key)
{
	ProfileEntry out = entry(user_id);
	out.avatar_key = avatar_key;
	return out;
}

AvatarMessage avatar(const std::string &user_id, const std::string &avatar_key, const std::string &png_base64)
{
	AvatarMessage out;
	out.user_id = user_id;
	out.avatar_key = avatar_key;
	out.avatar_png = png_base64;
	return out;
}

ProfilesMessage message_of(const std::vector<ProfileEntry> &entries)
{
	ProfilesMessage out;
	out.profiles = entries;
	return out;
}

std::string bytes_to_string(const std::vector<std::uint8_t> &bytes)
{
	return std::string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("パディングの有無にかかわらず base64 をデコードできる")
{
	std::vector<std::uint8_t> out;
	REQUIRE(decode_base64("", &out));
	CHECK(out.empty());

	REQUIRE(decode_base64("YQ==", &out));
	CHECK(bytes_to_string(out) == "a");
	REQUIRE(decode_base64("YWI=", &out));
	CHECK(bytes_to_string(out) == "ab");
	REQUIRE(decode_base64("YWJj", &out));
	CHECK(bytes_to_string(out) == "abc");

	// PNG の先頭の 8 バイト (実際に届く形)。
	REQUIRE(decode_base64("iVBORw0KGgo=", &out));
	REQUIRE(out.size() == 8);
	CHECK(out[0] == 0x89);
	CHECK(out[1] == 'P');
	CHECK(out[2] == 'N');
	CHECK(out[3] == 'G');
}

TEST_CASE("読めない base64 は false を返し、途中までのバイト列を残さない")
{
	std::vector<std::uint8_t> out;
	CHECK_FALSE(decode_base64("YWJ", &out));    // 4 の倍数でない
	CHECK(out.empty());
	CHECK_FALSE(decode_base64("YW J", &out));   // 空白は受け付けない
	CHECK_FALSE(decode_base64("YWJ\n", &out));  // 改行も受け付けない
	CHECK_FALSE(decode_base64("YW=j", &out));   // パディングが途中にある
	CHECK_FALSE(decode_base64("YQ=x", &out));
	CHECK_FALSE(decode_base64("!!!!", &out));
	CHECK_FALSE(decode_base64("YR==", &out));   // パディングの分の余りのビットが 0 でない
	CHECK(out.empty());
}

TEST_CASE("届いた 1 通から表示名、アバターのバージョン、Bot の印を参加者ごとに保持する")
{
	ProfilesMessage message;
	ProfileEntry with_name = keyed("1", "k1");
	with_name.display_name = "きつね";
	message.profiles.push_back(with_name);
	message.profiles.push_back(entry("2")); // 表示名を読めなかった人 (キーごと省かれる)
	ProfileEntry bot = entry("3");
	bot.display_name = "ボット";
	bot.bot = true;
	message.profiles.push_back(bot);

	const ProfilesSnapshot snapshot = make_profiles_snapshot(message, 1234);
	CHECK(snapshot.received);
	CHECK(snapshot.received_at_us == 1234);
	REQUIRE(snapshot.people.size() == 3);

	REQUIRE(snapshot.display_name_of("1").has_value());
	CHECK(*snapshot.display_name_of("1") == "きつね");
	CHECK(snapshot.people.at("1").avatar_key == std::optional<std::string>("k1"));
	CHECK_FALSE(snapshot.bot_of("1"));
	CHECK(snapshot.bot_of("3"));

	// 「読めなかった」と「ない」を区別する。行はあるが、表示名もアバターのバージョンも持たない。
	CHECK_FALSE(snapshot.display_name_of("2").has_value());
	CHECK_FALSE(snapshot.people.at("2").avatar_key.has_value());

	// アバターの画像は profiles では届かない。
	CHECK(snapshot.avatar_of("1").empty());

	// 1 回も載っていない userId は行そのものがない。
	CHECK_FALSE(snapshot.display_name_of("9").has_value());
	CHECK_FALSE(snapshot.bot_of("9"));
}

TEST_CASE("まだ 1 通も届いていない ProfilesSnapshot は received が false で、人もアバターも持たない")
{
	const ProfilesSnapshot snapshot;
	CHECK_FALSE(snapshot.received);
	CHECK(snapshot.people.empty());
	CHECK(snapshot.avatars.empty());
	CHECK_FALSE(snapshot.display_name_of("1").has_value());
	CHECK(snapshot.avatar_of("1").empty());
}

TEST_CASE("avatar で届いた画像を参加者ごとに保持し、同じ人の新しいバージョンで入れ替える")
{
	ProfilesSnapshot snapshot = make_profiles_snapshot(message_of({keyed("1", "k1"), entry("2")}), 1);
	CHECK(apply_avatar(&snapshot, avatar("1", "k1", "YWJj")));
	CHECK(bytes_to_string(snapshot.avatar_of("1")) == "abc");
	CHECK(snapshot.avatar_of("2").empty());

	// バージョンが変わったと profiles で分かり、そのバージョンの画像が届いた。
	snapshot = next_profiles_snapshot(snapshot, message_of({keyed("1", "k2"), entry("2")}), 2);
	CHECK(apply_avatar(&snapshot, avatar("1", "k2", "eHl6")));
	CHECK(bytes_to_string(snapshot.avatar_of("1")) == "xyz");
	CHECK(snapshot.avatar_decode_failures == 0);
}

TEST_CASE("profiles でバージョンが変わった人と、アバターを外した人 (空の版) と、抜けた人の画像を捨てる")
{
	ProfilesSnapshot snapshot =
		make_profiles_snapshot(message_of({keyed("1", "k1"), keyed("2", "k2"), keyed("3", "k3")}), 1);
	REQUIRE(apply_avatar(&snapshot, avatar("1", "k1", "YWJj")));
	REQUIRE(apply_avatar(&snapshot, avatar("2", "k2", "YWJj")));
	REQUIRE(apply_avatar(&snapshot, avatar("3", "k3", "YWJj")));

	// 1 は別のバージョンになり、2 はアバターを外し (空の版)、3 は通話から抜けた。
	snapshot = next_profiles_snapshot(snapshot, message_of({keyed("1", "k1-new"), keyed("2", "")}), 2);
	CHECK(snapshot.avatar_of("1").empty());
	CHECK(snapshot.avatar_of("2").empty());
	CHECK(snapshot.avatar_of("3").empty());
	CHECK(snapshot.avatars.empty());
}

TEST_CASE("profiles の版と同じ画像と、版をまだ読めていない人の画像は、次の profiles でも残す")
{
	ProfilesSnapshot snapshot = make_profiles_snapshot(message_of({keyed("1", "k1"), keyed("2", "k2")}), 1);
	REQUIRE(apply_avatar(&snapshot, avatar("1", "k1", "YWJj")));
	REQUIRE(apply_avatar(&snapshot, avatar("2", "k2", "eHl6")));

	// 2 は送信側がまだ版を読めていない (キーごと省かれた)。分からないだけなので画像を残す (捨てると、送信側は
	// 同じバージョンを送り直さないので、接続し直すまで戻らない)。
	ProfileEntry renamed = keyed("1", "k1");
	renamed.display_name = "名前を変えた";
	snapshot = next_profiles_snapshot(snapshot, message_of({renamed, entry("2")}), 2);
	CHECK(bytes_to_string(snapshot.avatar_of("1")) == "abc");
	CHECK(bytes_to_string(snapshot.avatar_of("2")) == "xyz");
}

TEST_CASE("profiles の版と違う版の画像は入れず、profiles がまだない人の画像は入れる")
{
	ProfilesSnapshot snapshot = make_profiles_snapshot(message_of({keyed("1", "k2")}), 1);
	// 1 の profiles は k2 なので、k1 の画像は古い。
	CHECK_FALSE(apply_avatar(&snapshot, avatar("1", "k1", "YWJj")));
	CHECK(snapshot.avatar_of("1").empty());
	CHECK(snapshot.avatar_decode_failures == 0);

	// 9 の profiles はまだ届いていない。入れておき、profiles が届いたときに確認する。
	CHECK(apply_avatar(&snapshot, avatar("9", "k9", "YWJj")));
	CHECK(bytes_to_string(snapshot.avatar_of("9")) == "abc");
	snapshot = next_profiles_snapshot(snapshot, message_of({keyed("1", "k2"), keyed("9", "k9")}), 2);
	CHECK(bytes_to_string(snapshot.avatar_of("9")) == "abc");

	// userId のない画像は入れない。
	CHECK_FALSE(apply_avatar(&snapshot, avatar("", "k", "YWJj")));
}

TEST_CASE("base64 として読めない画像は、その人の現在の画像も捨てて数え、ほかの人の画像は残す")
{
	ProfilesSnapshot snapshot = make_profiles_snapshot(message_of({entry("1"), entry("2")}), 1);
	REQUIRE(apply_avatar(&snapshot, avatar("1", "k1", "YWJj")));
	REQUIRE(apply_avatar(&snapshot, avatar("2", "k2", "eHl6")));

	CHECK_FALSE(apply_avatar(&snapshot, avatar("1", "k1b", "これは base64 ではない")));
	CHECK(snapshot.avatar_of("1").empty());
	CHECK(snapshot.avatar_decode_failures == 1);
	CHECK(bytes_to_string(snapshot.avatar_of("2")) == "xyz");

	// 空の画像も読めないものとして数える。
	CHECK_FALSE(apply_avatar(&snapshot, avatar("2", "k2", "")));
	CHECK(snapshot.avatar_of("2").empty());
	CHECK(snapshot.avatar_decode_failures == 2);

	// 数は次の profiles でも引き継ぐ (この接続の間の数)。
	snapshot = next_profiles_snapshot(snapshot, message_of({entry("1")}), 2);
	CHECK(snapshot.avatar_decode_failures == 2);
}

TEST_CASE("新しい 1 通で全体が置き換わり、前のメッセージにだけいた人は残らない")
{
	ProfilesMessage first;
	ProfileEntry left = entry("1");
	left.display_name = "抜けた人";
	first.profiles.push_back(left);
	first.profiles.push_back(entry("2"));
	const ProfilesSnapshot before = make_profiles_snapshot(first, 1);
	CHECK(before.people.size() == 2);

	ProfilesMessage second;
	second.profiles.push_back(entry("2"));
	const ProfilesSnapshot after = next_profiles_snapshot(before, second, 2);
	REQUIRE(after.people.size() == 1);
	CHECK(after.people.count("1") == 0); // 通話から抜けた人の名前を持ち続けない
}

TEST_CASE("userId のない行は持たず、同じ userId が 2 回あれば後の方を使う")
{
	ProfilesMessage message;
	message.profiles.push_back(entry("")); // 誰のことか分からない行
	ProfileEntry first = entry("1");
	first.display_name = "前";
	message.profiles.push_back(first);
	ProfileEntry second = entry("1");
	second.display_name = "後";
	message.profiles.push_back(second);

	const ProfilesSnapshot snapshot = make_profiles_snapshot(message, 1);
	REQUIRE(snapshot.people.size() == 1);
	CHECK(*snapshot.display_name_of("1") == "後");
}

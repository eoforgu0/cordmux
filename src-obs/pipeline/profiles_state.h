// SPDX-License-Identifier: Apache-2.0

// 送信側から届く profiles (参加者ごとの表示名、ユーザー名、アバターのバージョン、Bot の印) と avatar (参加者ごとのアバターの画像) を
// 保持する構造体と、それを作る純粋関数。形は docs/wire_format.md の「profiles」と「avatar」にある。profiles の 1 通は
// 送信側が読めた通話の参加者全員の分なので、届いた 1 通で人の表を置き換える。前のメッセージの分を残すと、通話から
// 抜けた人の名前が画面と OBS のソース名に残る。アバターは参加者ごとに届くので、置き換えずに参加者ごとに足し、profiles が
// 届いたときに、もういない人と、版 (avatar_key) が変わった人の画像を捨てる。アバターの base64 のデコードはここで
// 1 回だけ行う (ws/ は base64 の文字列のまま持つ)。読めないアバターは捨てて数える。libobs にも Qt にも依存しない。

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ws/control_messages.h"

namespace cordmux::pipeline {

// 1 人分の表示の情報。
struct ProfileInfo {
	// 表示名。値を持たないのは、送信側が読めなかったとき (そのキーは省かれる)。
	// そのとき、画面と OBS のソース名には userId をそのまま表示する。
	std::optional<std::string> display_name;

	// Discord のユーザー名 (任意)。表示名が同じ人を見分けるために、ドックの行の 2 行目に表示する。
	// 値を持たないのは、送信側が読めなかったか、ユーザー名を送らない古い送信側のとき。
	std::optional<std::string> username;

	// アバターのバージョン (Discord の user.avatar の値)。空の文字列はアバターを設定していない人、値を持たないのは送信側が
	// まだ読めていない人 (ws::ProfileEntry と同じ)。
	std::optional<std::string> avatar_key;

	// Bot のアカウントか (送信側が読めなかった人は偽)。
	bool bot = false;
};

// 1 人分のアバターの画像 (avatar で届いたもの)。
struct AvatarImage {
	std::string key; // この画像を取ったときの avatar_key
	// PNG のバイト列 (base64 のデコードは済んでいる)。スナップショットのコピー (ドックの周期ごと) で画像を
	// 複製しないように、共有して持つ。書き換えない。
	std::shared_ptr<const std::vector<std::uint8_t>> png;
};

// 直近に届いた profiles の 1 通分と、参加者ごとのアバター。
struct ProfilesSnapshot {
	bool received = false;           // 1 通でも届いたか (false は「まだ分からない」)
	std::int64_t received_at_us = 0; // 最後に届いた時刻 (エポックからのマイクロ秒)

	// user_id -> 表示の情報。届いた 1 通で全体が置き換わる (ファイルの先頭のコメント)。
	std::map<std::string, ProfileInfo> people;

	// user_id -> アバターの画像。avatar が届くたびに、その人の分だけ入れ替わる。profiles が届いたときに、people に
	// いない人と、people の avatar_key とバージョンが違う人の分を捨てる (next_profiles_snapshot)。
	std::map<std::string, AvatarImage> avatars;

	// この接続の間に、デコードできずに捨てたアバターの数。診断情報に入り、facts の unreadable_keys と同じく、
	// 送信側と受信側のバージョンが合っていないことを疑う手がかりにする。
	int avatar_decode_failures = 0;

	// その userId の表示名 (なければ値を持たない)。表示名はここからだけ引く。ドックの人の一覧、OBS の
	// ソース名、名前ラベルが同じ文字列を使うためである。
	std::optional<std::string> display_name_of(const std::string &user_id) const
	{
		const auto found = people.find(user_id);
		if (found == people.end())
			return std::nullopt;
		return found->second.display_name;
	}

	// その userId のユーザー名 (なければ値を持たない)。
	std::optional<std::string> username_of(const std::string &user_id) const
	{
		const auto found = people.find(user_id);
		if (found == people.end())
			return std::nullopt;
		return found->second.username;
	}

	// その userId が Bot のアカウントか (profiles にいない人は偽)。
	bool bot_of(const std::string &user_id) const
	{
		const auto found = people.find(user_id);
		return found != people.end() && found->second.bot;
	}

	// その userId のアバター (なければ空)。
	const std::vector<std::uint8_t> &avatar_of(const std::string &user_id) const
	{
		static const std::vector<std::uint8_t> empty;
		const auto found = avatars.find(user_id);
		if (found == avatars.end() || !found->second.png)
			return empty;
		return *found->second.png;
	}
};

namespace detail {

// base64 の 1 文字を 0 から 63 の値にする。base64 の文字でなければ -1。
inline int base64_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

} // namespace detail

// base64 をデコードする。空白も改行も受け付けない (送信側は OffscreenCanvas が作った data URL の後半をそのまま
// 載せるので、区切りの入らない 1 つの文字列である)。読めなければ false を返し、*out を空にする (途中までのバイト列を
// 渡さない)。
inline bool decode_base64(std::string_view text, std::vector<std::uint8_t> *out)
{
	if (out)
		out->clear();
	if (text.size() % 4 != 0)
		return false;
	if (text.empty())
		return true;

	// 末尾のパディング ("=") は 0 から 2 個。
	std::size_t padding = 0;
	while (padding < 2 && padding < text.size() && text[text.size() - 1 - padding] == '=')
		++padding;

	std::vector<std::uint8_t> bytes;
	bytes.reserve(text.size() / 4 * 3);

	const std::size_t body = text.size() - padding;
	std::uint32_t buffer = 0;
	int filled = 0;
	for (std::size_t i = 0; i < body; ++i) {
		const int value = detail::base64_value(text[i]);
		if (value < 0)
			return false;
		buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
		filled += 6;
		if (filled >= 8) {
			filled -= 8;
			bytes.push_back(static_cast<std::uint8_t>((buffer >> filled) & 0xFFu));
		}
	}
	// パディングの分の余りのビットは 0 でなければならない (0 でなければ、正しい base64 ではない)。
	if (filled > 0 && (buffer & ((1u << filled) - 1u)) != 0)
		return false;
	if (out)
		*out = std::move(bytes);
	return true;
}

namespace detail {

// 持っているアバターの画像を、profiles の人の情報と比べて残してよいか。その人が profiles にいて、avatar_key が
// 画像のバージョンと同じか、送信側がまだ読めていない (値を持たない) ときに残す。avatar_key が画像のバージョンと違えば捨てる
// (アバターを外した人は空の文字列になり、前の画像のバージョンと違う)。読めていないだけの人の画像を捨てると、送信側は
// 同じバージョンを送り直さないので、その人のアバターが接続し直すまで戻らない。
inline bool avatar_still_valid(const std::map<std::string, ProfileInfo> &people, const std::string &user_id,
			       const AvatarImage &image)
{
	const auto found = people.find(user_id);
	if (found == people.end())
		return false; // 通話から抜けた人
	const std::optional<std::string> &key = found->second.avatar_key;
	return !key.has_value() || *key == image.key;
}

} // namespace detail

// before の後に 1 通の profiles が届いたときの ProfilesSnapshot を作る。人の表は届いた 1 通で置き換え、アバターと
// デコードできなかった数は before から引き継ぐ。引き継ぐアバターは、新しい人の表で残してよいもの
// (detail::avatar_still_valid) だけである。received_at_us は呼び出し元が渡す (ここでは時計を読まない)。
inline ProfilesSnapshot next_profiles_snapshot(const ProfilesSnapshot &before, const ws::ProfilesMessage &message,
					       std::int64_t received_at_us)
{
	ProfilesSnapshot out;
	out.received = true;
	out.received_at_us = received_at_us;
	out.avatar_decode_failures = before.avatar_decode_failures;

	for (const ws::ProfileEntry &entry : message.profiles) {
		if (entry.user_id.empty())
			continue; // 誰のことか分からない行は持たない
		ProfileInfo info;
		info.display_name = entry.display_name;
		info.username = entry.username;
		info.avatar_key = entry.avatar_key;
		info.bot = entry.bot;
		// 同じ userId が 2 回載っていたら後の方を使う (送信側は参加者の一覧から作るので起きないはずだが、
		// 起きたときの扱いを決めておく)。
		out.people[entry.user_id] = std::move(info);
	}
	for (const auto &[user_id, image] : before.avatars) {
		if (detail::avatar_still_valid(out.people, user_id, image))
			out.avatars.emplace(user_id, image);
	}
	return out;
}

// まだ何も持っていないところに 1 通の profiles が届いたときの ProfilesSnapshot を作る。
inline ProfilesSnapshot make_profiles_snapshot(const ws::ProfilesMessage &message, std::int64_t received_at_us)
{
	return next_profiles_snapshot(ProfilesSnapshot{}, message, received_at_us);
}

// avatar の 1 通をデコードした結果。png が空なら、base64 として読めなかった。
struct DecodedAvatar {
	std::string user_id;
	AvatarImage image;
};

// avatar の 1 通をデコードする (表に入れる apply_decoded_avatar と分けるのは、呼び出し元がロックの外でデコード
// できるようにするためである)。
inline DecodedAvatar decode_avatar(const ws::AvatarMessage &message)
{
	DecodedAvatar out;
	out.user_id = message.user_id;
	out.image.key = message.avatar_key;
	std::vector<std::uint8_t> bytes;
	if (decode_base64(message.avatar_png, &bytes) && !bytes.empty())
		out.image.png = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
	return out;
}

// デコードしたアバターを表に入れる。入れたら true を返す。入れないのは次のとき。
//   - user_id が空 (誰のことか分からない)。
//   - その人の profiles の avatar_key が値を持ち、画像のバージョンと違う (古い画像。送信側は、バージョンが変わった後に取り直した
//     画像を送る)。
//   - base64 として読めなかった。このときはその人の現在の画像も捨て (バージョンが変わったので、古い画像は事実と違う)、
//     avatar_decode_failures を数える。
// profiles がまだ届いていない人の画像は入れる (profiles が届いたときに next_profiles_snapshot が確認する)。
inline bool apply_decoded_avatar(ProfilesSnapshot *snapshot, const DecodedAvatar &decoded)
{
	if (snapshot == nullptr || decoded.user_id.empty())
		return false;
	const auto person = snapshot->people.find(decoded.user_id);
	if (person != snapshot->people.end() && person->second.avatar_key.has_value() &&
	    *person->second.avatar_key != decoded.image.key)
		return false;
	if (!decoded.image.png) {
		snapshot->avatars.erase(decoded.user_id);
		snapshot->avatar_decode_failures++;
		return false;
	}
	snapshot->avatars[decoded.user_id] = decoded.image;
	return true;
}

// avatar の 1 通を表に入れる (decode_avatar と apply_decoded_avatar を続けて呼ぶ)。
inline bool apply_avatar(ProfilesSnapshot *snapshot, const ws::AvatarMessage &message)
{
	return apply_decoded_avatar(snapshot, decode_avatar(message));
}

} // namespace cordmux::pipeline

// SPDX-License-Identifier: Apache-2.0

// 「これまでに見た人の一覧」(seen_people.json) の読み書きと、上限に合わせて行を捨てる処理。純粋関数で、libobs
// にも Qt にも依存しない。一覧は settings.json とは別のファイルにする (大きくなるファイルと、大きくならない
// ファイルを混ぜない)。形が違うファイルは丸ごと拒む (1 行だけ落として残りを使うことはしない)。途中まで読めた
// 一覧を書き戻すと、読めなかった行がその時点で本当に消えるためである (config/settings.h の、形が違う値が 1 つ
// でもあればすべてを既定値に戻すのと同じ考え方)。

#pragma once

// nlohmann/json はそのまま使うだけなので、ヘッダーが出す警告は抑える (ws/control_messages.h と同じ)。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "config/person_audio.h"

// 1 行の型は ledger::SeenPerson をそのまま使う。同じ構造体を config/ にもう 1 つ作ってコピーし合うと、フィールドを
// 追加したときに 2 か所を修正することになり、片方だけ直すと、エラーにならないまま値が落ちるフィールドができる。
// config/ から ledger/ に依存するのは、この型 1 つだけである (判断も文言も持ち込まない)。
#include "ledger/observation.h"

namespace cordmux::config {

// 一覧に持つ人数の上限 (既定値は 200 人) は config/settings.h が持つ (利用者が変えられる値なので、ほかの設定と
// 同じ場所に置く)。ここが持つのは、その上限に合わせて行を捨てる方法だけである。

// 利用者が参加者の設定ウィンドウで指定した値 (「自動で載せる」(always_on)、「cordmuxでの名前」(name_override)、
// 「使う声」(voice_source_user_id)、「通話のみ時ラベル非表示」を付けた (roster_hidden)、100% でない音量 (声と画面共有の音)) が 1 つでも
// あるか。name_override は、空文字列を指定と数えない (ledger::override_present と同じ判断を使う。空の上書きは上書きを
// やめた結果なので、指定と数えると、捨てられない行が増え続ける)。roster_hidden を数えないと、「通話のみ時ラベル非表示」を付けた Bot の
// 行が上限を超えたときに捨てられ、その Bot がメッセージもなく配信の並びに戻る。音量も同じで、数えないと、利用者が下げた
// 人の音量が、メッセージもなく 100% に戻る。
inline bool seen_person_has_mark(const ledger::SeenPerson &person)
{
	if (person.always_on)
		return true;
	if (person.roster_hidden)
		return true;
	if (person.voice_volume_pct != kPersonVolumeDefaultPct || person.screen_volume_pct != kPersonVolumeDefaultPct)
		return true;
	if (ledger::override_present(person))
		return true;
	return person.voice_source_user_id.has_value() && !person.voice_source_user_id->empty();
}

// 上限に合わせて捨てた結果。
struct SeenTrimResult {
	int discarded = 0;            // 捨てた行の総数
	int discarded_with_marks = 0; // そのうち、利用者の指定が付いていた行 (画面に表示する。ledger::Seen)
};

// 上限 limit に合わせて行を捨てる。最後に見た時刻 (last_seen_at_us) が古い順に捨てるが、利用者の指定が付いた行は
// 最後まで残し、指定の付いた行だけで上限を超えたときだけ、その中から古い順に捨てる。一律に古い順に捨てると、
// 利用者の指定が、メッセージもログも出さずに消えるためである。捨てた数は返し、呼び出し元がドックに表示する。残った
// 行の並びは元のまま (並べ方は表示の側の ledger::decide が決める。ここで並べ替えると、並べ方を決める所が 2 つに
// なる)。
inline SeenTrimResult trim_seen_people(std::vector<ledger::SeenPerson> *people, int limit)
{
	SeenTrimResult result;
	if (people == nullptr)
		return result;
	const std::size_t max_size = limit > 0 ? static_cast<std::size_t>(limit) : 0u;
	if (people->size() <= max_size)
		return result;

	// 捨てる候補を「指定のない行 -> 指定の付いた行」の順に、それぞれ古い順で並べる。
	std::vector<std::size_t> order(people->size());
	for (std::size_t i = 0; i < order.size(); ++i)
		order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&people](std::size_t a, std::size_t b) {
		const bool mark_a = seen_person_has_mark((*people)[a]);
		const bool mark_b = seen_person_has_mark((*people)[b]);
		if (mark_a != mark_b)
			return !mark_a; // 指定のないほうを先に捨てる
		return (*people)[a].last_seen_at_us < (*people)[b].last_seen_at_us;
	});

	const std::size_t to_drop = people->size() - max_size;
	std::set<std::size_t> dropped;
	for (std::size_t i = 0; i < to_drop; ++i) {
		const std::size_t index = order[i];
		dropped.insert(index);
		result.discarded++;
		if (seen_person_has_mark((*people)[index]))
			result.discarded_with_marks++;
	}

	std::vector<ledger::SeenPerson> kept;
	kept.reserve(max_size);
	for (std::size_t i = 0; i < people->size(); ++i) {
		if (dropped.count(i) == 0)
			kept.push_back((*people)[i]);
	}
	*people = std::move(kept);
	return result;
}

// その userId の行を返す (なければ末尾に作る)。参照は、次に一覧を変更するまでの間だけ有効である。
inline ledger::SeenPerson &ensure_seen_person(std::vector<ledger::SeenPerson> &people, const std::string &user_id)
{
	for (ledger::SeenPerson &person : people) {
		if (person.user_id == user_id)
			return person;
	}
	ledger::SeenPerson added;
	added.user_id = user_id;
	people.push_back(std::move(added));
	return people.back();
}

// 今見えている人の、最後に見た時刻を now_us にする (行がなければ作る)。空の userId は飛ばす。
//
// 上限に合わせて捨てる前に呼び出すこと。今通話に参加している人の時刻を参加した時刻のままにしておくと、長く参加
// している人の行が、さっき抜けた人の行より古くなり、上限ちょうどの一覧では、その人だけが最も古い行として捨て
// られる (次の周期に追加し直されて、また捨てられる、を繰り返す)。今見ている人の最後に見た時刻は、今である。
inline void touch_seen_people(std::vector<ledger::SeenPerson> &people, const std::set<std::string> &user_ids,
			      std::int64_t now_us)
{
	for (const std::string &user_id : user_ids) {
		if (user_id.empty())
			continue;
		ensure_seen_person(people, user_id).last_seen_at_us = now_us;
	}
}

// 一覧を seen_people.json の中身にする (1 人の行を 1 つの要素にした配列で、入れ子にしない)。持っていない指定は、
// キーごと省く (ないことと、空文字列を書き分けるため)。
inline std::string seen_people_to_json(const std::vector<ledger::SeenPerson> &people)
{
	nlohmann::json array = nlohmann::json::array();
	for (const ledger::SeenPerson &person : people) {
		nlohmann::json row = nlohmann::json::object();
		row["user_id"] = person.user_id;
		row["display_name"] = person.display_name;
		row["last_seen_at_us"] = person.last_seen_at_us;
		row["always_on"] = person.always_on;
		if (person.name_override.has_value())
			row["name_override"] = *person.name_override;
		if (person.voice_source_user_id.has_value())
			row["voice_source_user_id"] = *person.voice_source_user_id;
		// 「通話のみ時ラベル非表示」を付けたときだけ書く (既定の値は書かない。roster_hidden を知らない cordmux でも読める)。
		if (person.roster_hidden)
			row["roster_hidden"] = true;
		// 音量は 100% でないときだけ書く (既定の値は書かない。音量を知らない cordmux でも読める)。ミュートは書かない。
		if (person.voice_volume_pct != kPersonVolumeDefaultPct)
			row["voice_volume_pct"] = person.voice_volume_pct;
		if (person.screen_volume_pct != kPersonVolumeDefaultPct)
			row["screen_volume_pct"] = person.screen_volume_pct;
		array.push_back(std::move(row));
	}
	return array.dump(2) + "\n";
}

namespace detail {

// 1 行の音量のキーを 1 つ読む。キーがなければ *out を変えずに true を返す。整数でないか、範囲 (0 から 200) の外なら、
// 理由を *error に入れて false を返す。符号なしとして持っている大きな値は、int64 に変換する前に上限と比べて拒む
// (先に変換すると、負の値になって範囲の中に見える。config/settings.h の read_bounded_int と同じ理由)。
inline bool read_volume_pct(const nlohmann::json &row, const char *key, int *out, std::string *error)
{
	const auto it = row.find(key);
	if (it == row.end())
		return true;
	if (!it->is_number_integer()) {
		if (error)
			*error = std::string(key) + " が整数でない";
		return false;
	}
	bool in_range = false;
	std::int64_t value = 0;
	if (it->is_number_unsigned()) {
		const std::uint64_t raw = it->get<std::uint64_t>();
		in_range = raw <= static_cast<std::uint64_t>(kPersonVolumeMaxPct);
		value = in_range ? static_cast<std::int64_t>(raw) : 0;
	} else {
		value = it->get<std::int64_t>();
		in_range = value >= kPersonVolumeMinPct && value <= kPersonVolumeMaxPct;
	}
	if (!in_range) {
		if (error)
			*error = std::string(key) + " が範囲 (0 から 200) の外";
		return false;
	}
	*out = static_cast<int>(value);
	return true;
}

} // namespace detail

// seen_people.json を読む。成功なら true を返す。形が違えば false を返して *out を空にし、理由を *error に入れる
// (1 行だけ落とすことはしない。このファイルの先頭のコメント)。
inline bool seen_people_from_json(std::string_view text, std::vector<ledger::SeenPerson> *out, std::string *error)
{
	if (out)
		out->clear();
	if (error)
		error->clear();

	const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded()) {
		if (error)
			*error = "壊れた JSON";
		return false;
	}
	if (!j.is_array()) {
		if (error)
			*error = "JSON が配列でない";
		return false;
	}

	std::vector<ledger::SeenPerson> parsed;
	parsed.reserve(j.size());
	std::set<std::string> seen_ids;
	for (const auto &row : j) {
		if (!row.is_object()) {
			if (error)
				*error = "配列の要素がオブジェクトでない";
			return false;
		}
		const auto user_id = row.find("user_id");
		if (user_id == row.end() || !user_id->is_string() || user_id->get<std::string>().empty()) {
			if (error)
				*error = "user_id がない、または文字列でない";
			return false;
		}

		ledger::SeenPerson person;
		person.user_id = user_id->get<std::string>();
		if (!seen_ids.insert(person.user_id).second) {
			if (error)
				*error = "同じ user_id が 2 行ある";
			return false;
		}

		const auto display_name = row.find("display_name");
		if (display_name != row.end()) {
			if (!display_name->is_string()) {
				if (error)
					*error = "display_name が文字列でない";
				return false;
			}
			person.display_name = display_name->get<std::string>();
		}

		const auto last_seen = row.find("last_seen_at_us");
		if (last_seen != row.end()) {
			if (!last_seen->is_number_integer()) {
				if (error)
					*error = "last_seen_at_us が整数でない";
				return false;
			}
			person.last_seen_at_us = last_seen->get<std::int64_t>();
		}

		const auto always_on = row.find("always_on");
		if (always_on != row.end()) {
			if (!always_on->is_boolean()) {
				if (error)
					*error = "always_on が真偽値でない";
				return false;
			}
			person.always_on = always_on->get<bool>();
		}

		const auto name_override = row.find("name_override");
		if (name_override != row.end()) {
			if (!name_override->is_string()) {
				if (error)
					*error = "name_override が文字列でない";
				return false;
			}
			person.name_override = name_override->get<std::string>();
		}

		const auto voice_source = row.find("voice_source_user_id");
		if (voice_source != row.end()) {
			if (!voice_source->is_string()) {
				if (error)
					*error = "voice_source_user_id が文字列でない";
				return false;
			}
			person.voice_source_user_id = voice_source->get<std::string>();
		}

		// 「通話のみ時ラベル非表示」を付けたか。キーがなければ偽 (出す)。
		const auto roster_hidden = row.find("roster_hidden");
		if (roster_hidden != row.end()) {
			if (!roster_hidden->is_boolean()) {
				if (error)
					*error = "roster_hidden が真偽値でない";
				return false;
			}
			person.roster_hidden = roster_hidden->get<bool>();
		}

		// 音量 (%)。整数で、0 から 200 の中でなければ、ファイルごと拒む (config/settings.h の整数の値と同じ扱い。
		// 範囲の外の値を端に収めて使うと、手で書き間違えた値を、伝えずに別の値として使う)。
		if (!detail::read_volume_pct(row, "voice_volume_pct", &person.voice_volume_pct, error) ||
		    !detail::read_volume_pct(row, "screen_volume_pct", &person.screen_volume_pct, error))
			return false;

		parsed.push_back(std::move(person));
	}

	if (out)
		*out = std::move(parsed);
	return true;
}

} // namespace cordmux::config

// SPDX-License-Identifier: Apache-2.0

// 送信側から届く facts (通話の参加者、画面共有と発言の状態、送信側の録画の状態など) を保持する構造体と、
// それを読む純粋関数。facts の形は docs/wire_format.md の「facts」にある。ws/ はキーがあることだけを見て中身を
// 渡すので、受信側で中身を読むのはここだけである。
// 読めないキーは、そのキーだけを捨てて unreadable_keys に数える (全体は捨てない)。ただし一覧のキー
// (stages、sharing、roster) は、要素が 1 つでも読めなければキーごと捨てる (途中までの一覧を
// 作らない)。libobs にも Qt にも依存しない。

#pragma once

// nlohmann/json のヘッダーが出す警告は抑える (外部のライブラリで、こちらでは修正できないため)。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cordmux::pipeline {

// {count, user_ids} を読んだ値。count は user_ids の数と一致する約束なので、一致しない facts が届いたことが
// 分かるように両方を持つ (画面に表示する人数は user_ids の数で、送信側の count は使わない)。
struct UserIdSet {
	int sent_count = 0;
	std::vector<std::string> user_ids;

	bool count_matches() const { return sent_count == static_cast<int>(user_ids.size()); }
};

// stages の 1 要素 (ストリームごとのエンコードの段階)。stage は閉じた列挙にせず、文字列のまま持つ
// (知らない値が来ても、画面にそのまま表示する)。
struct StreamStage {
	std::uint32_t stream_id = 0;
	std::string stage;
};

// メインアカウントがいる通話 (facts の main_call)。Vesktop のアカウントから見える通話に参加しているときだけ、送信側が送る。
// どの通話にもいないことは、送信側が区別できないので、このキーでは表さない (キーがないのは「分からない」)。
struct MainCallFact {
	std::string user_id; // 送信側が読んだときのメインアカウントの userId (設定を変えた直後の古い値を捨てるために照合する)
	std::optional<std::string> name;
	std::optional<bool> joinable; // Vesktop のアカウントが参加できるか。持たない = 読めなかった
};

// 直近に届いた facts の 1 通分。
struct FactsSnapshot {
	bool received = false;          // 1 通でも届いたか (false は「まだ分からない」)
	std::int64_t received_at_us = 0; // 最後に届いた時刻 (エポックからのマイクロ秒)

	// Discord の内部への依存箇所のうち、状態が missing か lost のものの id (src-vencord/core/state.ts の warnings)
	std::vector<std::string> warnings;
	UserIdSet sharing;
	std::optional<UserIdSet> roster; // 送信側がまだ読めていない間はキーごと省かれる

	// この facts に sharing のキーがあり、形が正しかったか。false の間の sharing は「誰も画面共有していない」では
	// なく「分からない」である (facts が届いていない、sharing を送らない古い送信側、形が違って捨てた)。
	// received だけを見ると、キーがないか壊れた facts でも true になり、「分からない」を「画面共有していない」と
	// 表示してしまう。
	bool sharing_valid = false;

	// userId -> 画面共有していると分かった時刻 (システム時刻のマイクロ秒)。画面共有しているのに最初の
	// フレームが届かない、というメッセージの時刻の基準になる。parse_facts_payload は埋めない。前の facts と比べて
	// 決める値なので、呼び出し元が carry_sharing_since で埋める。sharing にいる人だけを持つ。
	std::map<std::string, std::int64_t> sharing_since_us;
	std::optional<std::vector<StreamStage>> stages; // 任意のキー

	// 送信側が持っている録画の状態。値を持たないのは「まだ分からない」とき (facts が 1 通も届いていないか、
	// このキーを送らない古い送信側)。送信側は値にかかわらず必ずこのキーを載せる約束なので、キーがないのは
	// OFF ではなく、送信側のバージョンが古いことを表す。受信側は自分の録画の状態と比べ、食い違いを画面に
	// 表示する。
	std::optional<bool> recording;

	// 送信側が SPS と PPS をキーフレームの前に付けた回数の累計。受信側からは見えないので送信側が送り、
	// 録画ログに入る。値を持たない理由は recording と同じ。送信側が動き始めてからの累計で、ストリームが
	// なくなっても減らない。
	std::optional<std::uint64_t> sps_pps_prepended;

	// Vesktop のアカウント (Vesktop にログインしている Discord のアカウント) の userId。値を持たないのは、
	// 送信側がまだ読めていない (キーごと省く)、このキーを送らない古い送信側、形が違って捨てた、のどれか。
	// どの場合も、ドックは「使う声」の候補から誰も外さない。空の文字列は持たない (userId として意味がない)。
	std::optional<std::string> self_user_id;

	// Vesktop のアカウントがいる通話の名前。持たない = 通話にいないか、名前を読めない (キーごと省かれる)、このキーを
	// 送らない古い送信側、形が違って捨てた、のどれか。どの場合も、画面には「-」を表示する (空の文字列は持たない)。
	std::optional<std::string> call_name;

	// メインアカウントがいる通話。持たない = 分からない (メインアカウントを知らない、見えない通話に参加している、読めない、古い
	// 送信側、形が違って捨てた)。「どの通話にもいない」とは扱わない。
	std::optional<MainCallFact> main_call;

	// 形が読めずに捨てたキーの数。全体は捨てずに数を残し、ドックと診断情報で、送信側と受信側のバージョンが
	// 合っていないことを疑う手がかりにする。
	int unreadable_keys = 0;
};

namespace detail {

// {count, user_ids} を 1 つ読む。形が違えば値を持たない (数えるのは呼び出し元)。
inline std::optional<UserIdSet> parse_user_id_set(const nlohmann::json &value)
{
	if (!value.is_object())
		return std::nullopt;
	const auto ids = value.find("user_ids");
	if (ids == value.end() || !ids->is_array())
		return std::nullopt;

	UserIdSet out;
	out.user_ids.reserve(ids->size());
	for (const auto &id : *ids) {
		if (!id.is_string())
			return std::nullopt; // 1 つでも文字列でなければ、このキーごと捨てる
		out.user_ids.push_back(id.get<std::string>());
	}

	const auto count = value.find("count");
	if (count == value.end() || !count->is_number_integer())
		return std::nullopt;
	const std::int64_t raw = count->is_number_unsigned()
					 ? static_cast<std::int64_t>(count->get<std::uint64_t>())
					 : count->get<std::int64_t>();
	// 負の count と 1000000 を超える count は、形が違うものとしてこのキーごと捨てる。丸めて持つと、送信側が
	// 送った値と違う値が残って読み違える (count は画面に表示しない値で、一致しない facts を見分けるためだけに
	// 持つ)。
	if (raw < 0 || raw > 1000000)
		return std::nullopt;
	out.sent_count = static_cast<int>(raw);
	return out;
}

// [{stream_id, stage}] を読む。
inline std::optional<std::vector<StreamStage>> parse_stages(const nlohmann::json &value)
{
	if (!value.is_array())
		return std::nullopt;
	std::vector<StreamStage> out;
	out.reserve(value.size());
	for (const auto &item : value) {
		if (!item.is_object())
			return std::nullopt;
		const auto id = item.find("stream_id");
		const auto stage = item.find("stage");
		if (id == item.end() || !id->is_number_unsigned())
			return std::nullopt;
		if (stage == item.end() || !stage->is_string())
			return std::nullopt;
		const std::uint64_t raw = id->get<std::uint64_t>();
		if (raw > 0xFFFFFFFFull)
			return std::nullopt;
		StreamStage entry;
		entry.stream_id = static_cast<std::uint32_t>(raw);
		entry.stage = stage->get<std::string>();
		out.push_back(std::move(entry));
	}
	return out;
}

} // namespace detail

// facts の中身 (facts メッセージの facts キーのオブジェクト) を読む。
// received と received_at_us は呼び出し元が埋める (ここでは時計を読まない)。
inline FactsSnapshot parse_facts_payload(const nlohmann::json &facts)
{
	FactsSnapshot out;
	if (!facts.is_object()) {
		out.unreadable_keys = 1; // オブジェクトでない (中身を 1 つも読めない)
		return out;
	}

	const auto warnings = facts.find("warnings");
	if (warnings != facts.end()) {
		bool ok = warnings->is_array();
		std::vector<std::string> ids;
		if (ok) {
			ids.reserve(warnings->size());
			for (const auto &id : *warnings) {
				if (!id.is_string()) {
					ok = false;
					break;
				}
				ids.push_back(id.get<std::string>());
			}
		}
		if (ok)
			out.warnings = std::move(ids);
		else
			out.unreadable_keys++;
	}

	const auto sharing = facts.find("sharing");
	if (sharing != facts.end()) {
		if (const auto parsed = detail::parse_user_id_set(*sharing)) {
			out.sharing = *parsed;
			out.sharing_valid = true;
		} else {
			out.unreadable_keys++;
		}
	}

	// roster は、送信側がまだ一度も読めていない間はキーごと省かれる。省かれているのは異常ではないので
	// 数えず、形が違うときだけ数える。
	const auto roster = facts.find("roster");
	if (roster != facts.end()) {
		if (const auto parsed = detail::parse_user_id_set(*roster))
			out.roster = *parsed;
		else
			out.unreadable_keys++;
	}

	// stages も任意のキーで、なくても異常ではない。
	const auto stages = facts.find("stages");
	if (stages != facts.end()) {
		if (auto parsed = detail::parse_stages(*stages))
			out.stages = std::move(*parsed);
		else
			out.unreadable_keys++;
	}

	// 録画に関わる 2 つのキー。どちらもキーごと省かれていれば数えない (省くのは古い送信側で、異常ではない)。
	// 形が違うときだけ数える。
	const auto recording = facts.find("recording");
	if (recording != facts.end()) {
		if (recording->is_boolean())
			out.recording = recording->get<bool>();
		else
			out.unreadable_keys++;
	}

	const auto prepended = facts.find("sps_pps_prepended");
	if (prepended != facts.end()) {
		// 累計なので負にならない。負の数と小数は形が違うものとして捨てる (count と同じく、丸めて持つより
		// 捨てるほうが読み違えない)。
		if (prepended->is_number_unsigned())
			out.sps_pps_prepended = prepended->get<std::uint64_t>();
		else
			out.unreadable_keys++;
	}

	// Vesktop のアカウントの userId。省かれていれば数えない (送信側がまだ読めていない間と、古い送信側)。
	// 文字列でない値と空の文字列は、形が違うものとして捨てて数える。数字だけかどうかは見ない (user_ids の
	// 要素も見ていない)。
	const auto self_user_id = facts.find("self_user_id");
	if (self_user_id != facts.end()) {
		if (self_user_id->is_string() && !self_user_id->get_ref<const std::string &>().empty())
			out.self_user_id = self_user_id->get<std::string>();
		else
			out.unreadable_keys++;
	}

	// 通話の名前と、メインアカウントがいる通話。どちらも、省かれていれば数えない (読めない間と古い送信側)。形が違うとき
	// だけ数える。main_call は、user_id が空でない文字列でなければ捨て、name と joinable は、型が違えば main_call ごと
	// 捨てる (一部だけを使わない)。
	const auto call_name = facts.find("call_name");
	if (call_name != facts.end()) {
		if (call_name->is_string() && !call_name->get_ref<const std::string &>().empty())
			out.call_name = call_name->get<std::string>();
		else
			out.unreadable_keys++;
	}

	const auto main_call = facts.find("main_call");
	if (main_call != facts.end()) {
		bool ok = main_call->is_object();
		MainCallFact fact;
		if (ok) {
			const auto user_id = main_call->find("user_id");
			ok = user_id != main_call->end() && user_id->is_string() &&
			     !user_id->get_ref<const std::string &>().empty();
			if (ok)
				fact.user_id = user_id->get<std::string>();
		}
		if (ok) {
			const auto name = main_call->find("name");
			if (name != main_call->end()) {
				if (name->is_string())
					fact.name = name->get<std::string>();
				else
					ok = false;
			}
			const auto joinable = main_call->find("joinable");
			if (joinable != main_call->end()) {
				if (joinable->is_boolean())
					fact.joinable = joinable->get<bool>();
				else
					ok = false;
			}
		}
		if (ok)
			out.main_call = std::move(fact);
		else
			out.unreadable_keys++;
	}

	return out;
}

// 画面共有していると分かった時刻を、前の facts (prev) から次の facts (next) に引き継ぐ (時刻は呼び出し元が渡す)。
//   - next の sharing に入った人: prev にもいれば prev の時刻を引き継ぎ、いなければ now_us にする。
//   - sharing から出た人: 持たない (次にまた入ったら、そのときの時刻から数え直す)。
//   - next の sharing_valid が false: 誰の時刻も持たない (分からない間に「画面共有している」と数えない)。
//   - 接続が切れたとき: 呼び出し元が prev を空の FactsSnapshot にしてから next を渡す (切れている間に
//     起きたことは分からないので、再接続の後は全員を「いま分かった」から数え直す)。
// prev の sharing_valid が false だったときは prev の時刻も空なので、全員を now_us から数え直す
// (最初のフレームを待つ時間を長めに取り、映像が届かないというメッセージを早く出しすぎない)。
inline void carry_sharing_since(const FactsSnapshot &prev, FactsSnapshot &next, std::int64_t now_us)
{
	next.sharing_since_us.clear();
	if (!next.sharing_valid)
		return;
	for (const std::string &user_id : next.sharing.user_ids) {
		if (user_id.empty())
			continue; // 誰のことか分からない行は持たない (profiles_state.h と同じ)
		const auto found = prev.sharing_since_us.find(user_id);
		next.sharing_since_us[user_id] = found != prev.sharing_since_us.end() ? found->second : now_us;
	}
}

} // namespace cordmux::pipeline

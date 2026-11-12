// SPDX-License-Identifier: Apache-2.0

// 混ぜた区間ごとの「聞こえる音があった声の組」から、参加者ごとの話し始めと話し終わりの時刻を作る (VoiceActivity)。
// 合成のソースは、この時刻から話している人の枠の光を描く。時刻は AudioMixer の区間の開始の時刻 (音が鳴る時刻) を
// そのまま使うので、光と声がそろう。時計は読まず、時刻はすべて引数で受け取る。libobs にも asio にも依存しない。
// ロックは持たず、取り出すスレッド (Pipeline::Impl::mixer_loop) の中だけで使う。

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cordmux::pipeline {

// 聞こえる音がない区間が続いても、話し終わりにしない長さ (マイクロ秒)。話している途中の息継ぎや語の間で、10 ms の
// 区間が 1 つ無音になるたびに話し終わりと話し始めを繰り返さないための長さで、話し終わりの時刻は最後に音があった
// 区間の終わりにするので、遅れて分かった分だけ光が長く残ることはない (止まった後の明るさは、枠の光の保持と
// 消える時間が決める)。100 ms は目安で、実際の通話での息継ぎの長さは測っていない。定数で、設定のキーにはしていない。
constexpr std::int64_t kVoiceActivityGapUs = 100000;

// 発言の 1 つの変化 (sources::set_voice_speaking に渡す形)。start_ns が 0 でなければ話し始め、stop_ns が 0 でなければ
// 話し終わり。時刻は OBS の時計 (os_gettime_ns) の上にある。
struct VoiceActivityUpdate {
	std::string user_id;
	std::uint64_t start_ns = 0;
	std::uint64_t stop_ns = 0;
};

class VoiceActivity {
public:
	// 区間を 1 つ見る。区間は時刻の順に渡す。
	//   block_start_us  区間の開始の時刻 (AudioMixer の MixedBlock::start_us)
	//   block_us        区間の長さ
	//   audible         この区間に聞こえる音があった声の組の user_id (空の文字列は読み飛ばす)
	// 戻り値は、話し終わり、話し始めの順で、それぞれ user_id の昇順 (同じ入力から同じ並びを作る)。話し始めの時刻は
	// その区間の開始、話し終わりの時刻は最後に音があった区間の終わりである。
	std::vector<VoiceActivityUpdate> observe(std::int64_t block_start_us, std::int64_t block_us,
						 const std::vector<std::string> &audible)
	{
		std::vector<VoiceActivityUpdate> stops;
		std::vector<VoiceActivityUpdate> starts;

		std::set<std::string> heard;
		for (const std::string &user_id : audible) {
			if (!user_id.empty())
				heard.insert(user_id);
		}

		for (auto it = talking_.begin(); it != talking_.end();) {
			if (heard.count(it->first) == 0 && block_start_us >= it->second + kVoiceActivityGapUs) {
				stops.push_back(VoiceActivityUpdate{it->first, 0, to_ns(it->second)});
				it = talking_.erase(it);
			} else {
				++it;
			}
		}

		const std::int64_t block_end_us = block_start_us + (block_us > 0 ? block_us : 0);
		for (const std::string &user_id : heard) {
			if (talking_.count(user_id) == 0)
				starts.push_back(VoiceActivityUpdate{user_id, to_ns(block_start_us), 0});
			talking_[user_id] = block_end_us;
		}

		stops.insert(stops.end(), starts.begin(), starts.end());
		return stops;
	}

	// 覚えている状態を捨てる (話し終わりは返さない)。合成のソースが現れた、なくなった、停止した、のときに呼ぶ。
	void reset() { talking_.clear(); }

	// 話している (話し終わりにしていない) 人の数。
	std::size_t talking_count() const { return talking_.size(); }

	// 話している (話し終わりにしていない) 人の user_id (昇順)。ドックのマイクの光に渡す。名前ラベルの光は observe の
	// 戻り値から作るので、どちらも同じ判定 (talking_) から出る。
	std::vector<std::string> talking_users() const
	{
		std::vector<std::string> out;
		out.reserve(talking_.size());
		for (const auto &entry : talking_)
			out.push_back(entry.first);
		return out;
	}

private:
	// 時刻 0 は「話していない」を表すので、0 以下の時刻は 1 ナノ秒にする。
	static std::uint64_t to_ns(std::int64_t us) { return us > 0 ? static_cast<std::uint64_t>(us) * 1000ULL : 1ULL; }

	// 話している参加者ごとの、最後に音があった区間の終わりの時刻。
	std::map<std::string, std::int64_t> talking_;
};

} // namespace cordmux::pipeline

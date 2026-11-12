// SPDX-License-Identifier: Apache-2.0

// AudioMixer に今ある組 (人と種類) を、ストリームごとに覚える名簿 (MixerRoster) と、参加者ごとの音量とミュートの表
// (MixerLevels)、画面共有の始まりを見分ける ScreenShareTracker。どれも OBS に依らない純粋なクラスで、ロックは持たず、
// 呼び出し元 (pipeline.cpp) が使う。名簿と表は mixer_mu の下で、ScreenShareTracker は Impl::mu の下で使う。
//
// 名簿の役目は、組が出入りしたことを呼び出し元に教えることである。AudioMixer の remove しないと、枠を空けられた
// 前の人の残りの音が、D の分だけ鳴り続ける。AudioMixer は remove すると利得も捨てるので、組が新しく名簿に入るたびに、
// 呼び出し元が表の利得とミュートを AudioMixer に当て直す (しないと、枠を替えるたびに音量が 100% に戻る)。
// libobs にも asio にも依存しない。

#pragma once

#include "config/person_audio.h"
#include "pipeline/audio_mixer.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cordmux::pipeline {

class MixerRoster {
public:
	// 組の出入り。added は名簿に新しく入った組、removed はどのストリームも使わなくなった組。
	struct Change {
		std::vector<MixerKey> added;
		std::vector<MixerKey> removed;

		bool empty() const { return added.empty() && removed.empty(); }
	};

	// ストリーム 1 つの組を設定する (値を持たなければ、そのストリームは組を使わない)。音のチャンクが届くたびに呼んで
	// よい (組が同じなら何も起きない)。
	Change set_stream(std::uint32_t stream_id, const std::optional<MixerKey> &key)
	{
		const auto it = streams_.find(stream_id);
		if (it != streams_.end() && key && it->second == *key)
			return {};
		if (it == streams_.end() && !key)
			return {};
		const std::set<MixerKey> before = active_keys();
		if (key)
			streams_[stream_id] = *key;
		else
			streams_.erase(stream_id);
		return diff(before, active_keys());
	}

	// ストリームごとの組を丸ごと入れ替える。経路や割り当てが変わったとき (configure、枠の付け替え、切断) に使う。
	Change sync(const std::map<std::uint32_t, MixerKey> &desired)
	{
		const std::set<MixerKey> before = active_keys();
		streams_ = desired;
		return diff(before, active_keys());
	}

	// すべて外す。
	Change clear() { return sync({}); }

	bool contains(const MixerKey &key) const
	{
		for (const auto &entry : streams_) {
			if (entry.second == key)
				return true;
		}
		return false;
	}

	// 名簿にある、ある種類の組。
	std::vector<MixerKey> keys(MixerKind kind) const
	{
		std::vector<MixerKey> out;
		for (const MixerKey &key : active_keys()) {
			if (key.kind == kind)
				out.push_back(key);
		}
		return out;
	}

private:
	std::set<MixerKey> active_keys() const
	{
		std::set<MixerKey> keys;
		for (const auto &entry : streams_)
			keys.insert(entry.second);
		return keys;
	}

	static Change diff(const std::set<MixerKey> &before, const std::set<MixerKey> &after)
	{
		Change change;
		for (const MixerKey &key : after) {
			if (before.count(key) == 0)
				change.added.push_back(key);
		}
		for (const MixerKey &key : before) {
			if (after.count(key) == 0)
				change.removed.push_back(key);
		}
		return change;
	}

	std::map<std::uint32_t, MixerKey> streams_;
};

// 参加者ごと (user_id と種類) の音量 (%) とミュート。既定は音量 100%、ミュートなし。表に載っていない組は既定の値を返す。
// AudioMixer の remove で消える利得を持ち続けるために、pipeline が持つ。音量の範囲は config/person_audio.h で、利得は
// 音量を 100 で割った線形の倍率である。
//
// 既定の例外が 2 つある。どちらもこのクラスが 1 か所で決める。
//   - 自分の声 (set_self_voice_user で決めた人の声の組) のミュートは、参加者ごとの値ではなく self_voice_muted_ 1 つが持つ。
//     既定は true (ミュート)。settings.json に覚えている値を pipeline が渡す (set_self_voice_muted)。メインアカウントの
//     userId が替わっても、新しい人の声に同じ値が当たる。
//   - 画面共有の音は、共有が始まるたびにミュートから始める (mute_for_share_start)。
struct MixerLevel {
	int volume_pct = config::kPersonVolumeDefaultPct;
	bool muted = false;

	float gain() const { return config::gain_of_person_volume(volume_pct); }
};

class MixerLevels {
public:
	MixerLevel get(const MixerKey &key) const
	{
		MixerLevel level;
		const auto it = levels_.find(key);
		if (it != levels_.end())
			level = it->second;
		if (is_self_voice(key))
			level.muted = self_voice_muted_;
		return level;
	}

	// 範囲 (0 から 200) の外の音量は端に収める。
	void set_volume_pct(const MixerKey &key, int volume_pct)
	{
		entry(key).volume_pct = config::clamp_person_volume_pct(volume_pct);
	}

	// 自分の声の組のミュートは、参加者ごとの値ではなく self_voice_muted_ に入れる。
	void set_mute(const MixerKey &key, bool muted)
	{
		if (is_self_voice(key))
			self_voice_muted_ = muted;
		else
			entry(key).muted = muted;
	}

	// 自分の声のミュート (settings.json の self_voice_muted)。変わったかを返す (呼び出し元は、自分の声の組を AudioMixer に
	// 当て直す)。
	bool set_self_voice_muted(bool muted)
	{
		if (self_voice_muted_ == muted)
			return false;
		self_voice_muted_ = muted;
		return true;
	}

	bool self_voice_muted() const { return self_voice_muted_; }

	// 画面共有が始まったときに、その人の画面の音をミュートにする。音量は変えない。友人のゲームの音などが、不意に配信に
	// 乗らないようにするためである。
	void mute_for_share_start(const std::string &user_id)
	{
		entry(MixerKey{user_id, MixerKind::ScreenAudio}).muted = true;
	}

	// 値を持っている人 (表に載っている人と、自分の声の人) の userId。画面に値を渡すとき、get() を引く人を決める。
	std::set<std::string> user_ids() const
	{
		std::set<std::string> ids;
		for (const auto &entry : levels_)
			ids.insert(entry.first.user_id);
		if (!self_voice_user_.empty())
			ids.insert(self_voice_user_);
		return ids;
	}

	// 自分の声の人 (メインアカウントの userId。空なら誰もいない)。この人の声の組は、ミュートを self_voice_muted_
	// (既定はミュート) にし、話しているかの判定を利得とミュートを掛ける前の音で行う (activity_before_gain)。利用者の声は
	// OBS のマイクで配信に出ていることが多く、Discord を通った声も鳴らすと遅れて二重になる。それでも、自分の枠を
	// 話しているときに光らせ、黙ると暗くするには、受け取った声で判定する。変わったかを返す (呼び出し元は、前と現在の人の
	// 組を AudioMixer に当て直す)。
	bool set_self_voice_user(const std::string &user_id)
	{
		if (self_voice_user_ == user_id)
			return false;
		self_voice_user_ = user_id;
		return true;
	}

	const std::string &self_voice_user() const { return self_voice_user_; }

	// 自分の声の組か。
	bool is_self_voice(const MixerKey &key) const
	{
		return key.kind == MixerKind::Voice && !self_voice_user_.empty() && key.user_id == self_voice_user_;
	}

	// 話しているかの判定を、利得とミュートを掛ける前の音で行う組か (AudioMixer::set_activity_before_gain)。自分の声
	// だけである。ほかの人は、利得とミュートを掛けた後の音で判定する (利用者がミュートした人は光らない)。
	bool activity_before_gain(const MixerKey &key) const { return is_self_voice(key); }

private:
	MixerLevel &entry(const MixerKey &key)
	{
		auto it = levels_.find(key);
		if (it == levels_.end())
			it = levels_.emplace(key, MixerLevel{}).first;
		return it->second;
	}

	std::map<MixerKey, MixerLevel> levels_;
	std::string self_voice_user_;
	bool self_voice_muted_ = true;
};

// 画面共有の始まりを見分ける。configure に画面のストリームがある人を「共有している人」として毎回渡すと、新しく共有を
// 始めた人を返す。画面の音はこの人たちをミュートから始める (MixerLevels::mute_for_share_start)。
//
// 組が名簿に入ったときではなく configure の人で見るのは、枠の付け替え (チェックを外して付け直す、枠を空けて別の人が載る)
// で組が出入りしても、共有は続いているためである。利用者が外したミュートが、共有の途中で戻らない。
//
// つなぎ直しは、同じ共有の続きとして扱う。次の 2 つで共有している人が一度いなくなっても、kShareResumeWindowUs の間に
// 戻れば、始まりとしない。
//   - 送信側との接続が切れた (on_link_lost)。経路も共有している人の記憶も消えるので、つなぎ直した後の最初の configure
//     が始まりに見える。送信側の再接続は、切れてから 0、2、4、5 秒の間隔で試す (src-vencord/transport/backoff.ts)。
//   - Discord の側で共有の流れが切れて、configure から消えて戻った。
// 猶予時間を過ぎてから戻った人は、新しい共有として扱う。同じ共有の途中で猶予時間より長く切れたのか、共有をやめて始め直したのかは
// 見分けられない。見分けられないときは、ミュートに戻す側にする (友人の音が不意に乗るより、音が出ないほうが害が小さい)。
inline constexpr std::int64_t kShareResumeWindowUs = 10000000;

// 画面共有している人。configure に画面のストリームがある人と、facts の sharing にいる人の和である (sharing_valid が偽の
// ときは、sharing の値が事実でないので configure の人だけ)。configure だけで見分けると、受け取る範囲が「枠に載せた人
// だけ」のとき、視聴を始めるまでストリームが現れず、視聴の範囲に入れ直すたびにストリームが現れ直すので、共有の途中で
// 始まりと見分けてしまう (載せる前に外したミュートが、載せた瞬間に戻る。外して 10 秒以上たって付け直すと戻る)。facts の
// sharing は、視聴しているかに関係なく、共有している間は載り続ける。
inline std::set<std::string> screen_sharers_of(const std::set<std::string> &configure_screen_users, bool sharing_valid,
					       const std::vector<std::string> &facts_sharing)
{
	std::set<std::string> out = configure_screen_users;
	if (sharing_valid)
		out.insert(facts_sharing.begin(), facts_sharing.end());
	return out;
}

class ScreenShareTracker {
public:
	// 今 configure に画面のストリームがある人。新しい共有を始めた人を返す。now_us は単調な時計の今 (マイクロ秒)。
	std::vector<std::string> update(const std::set<std::string> &sharing, std::int64_t now_us)
	{
		std::vector<std::string> started;
		for (const std::string &user_id : sharing) {
			if (user_id.empty())
				continue;
			auto it = entries_.find(user_id);
			if (it == entries_.end()) {
				started.push_back(user_id);
				entries_.emplace(user_id, Entry{true, now_us});
			} else if (!it->second.present) {
				if (now_us - it->second.left_us > kShareResumeWindowUs)
					started.push_back(user_id);
				it->second.present = true;
			}
		}
		for (auto it = entries_.begin(); it != entries_.end();) {
			Entry &entry = it->second;
			if (entry.present && sharing.count(it->first) == 0) {
				entry.present = false;
				entry.left_us = now_us;
			}
			// 猶予時間を過ぎた人は忘れる (表を増やし続けない)。戻ってきたら新しい共有として扱う結果は同じである。
			if (!entry.present && now_us - entry.left_us > kShareResumeWindowUs)
				it = entries_.erase(it);
			else
				++it;
		}
		return started;
	}

	// 送信側との接続が切れた。共有している人を、いま切れたことにする。
	void on_link_lost(std::int64_t now_us)
	{
		for (auto &entry : entries_) {
			if (entry.second.present) {
				entry.second.present = false;
				entry.second.left_us = now_us;
			}
		}
	}

private:
	struct Entry {
		bool present = false;
		std::int64_t left_us = 0; // present が偽のとき、いなくなった時刻
	};

	std::map<std::string, Entry> entries_;
};

// 表の利得とミュートと、話しているかの判定の仕方を、AudioMixer の 1 つの組に当てる。組が名簿に入ったときと、自分の声の
// 人が替わったときに、呼び出し元 (pipeline.cpp) が mixer_mu の下で呼ぶ。
inline void apply_mixer_level(AudioMixer &mixer, const MixerLevels &levels, const MixerKey &key)
{
	const MixerLevel level = levels.get(key);
	mixer.set_gain(key, level.gain());
	mixer.set_mute(key, level.muted);
	mixer.set_activity_before_gain(key, levels.activity_before_gain(key));
}

} // namespace cordmux::pipeline

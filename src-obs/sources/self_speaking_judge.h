// SPDX-License-Identifier: Apache-2.0

// 自分の枠の人が話しているかを、OBS の音声ソースの音で決める純粋関数と判定 (SelfSpeakingJudge)。音声ソースのパケットの
// 大きさ (peak) としきい値から、話し始めと話し終わりの時刻を作り、ソースを探した結果と最後のパケットの時刻から、
// 判定できるかを求める。時計は読まず、時刻はすべて引数で受け取る。libobs にも Qt にも依存しない。
// 判定はコールバックのスレッドの中だけで使い、ロックは持たない。

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config/settings.h"
#include "sources/label_border.h"

namespace cordmux::sources {

// 続けるしきい値は、話し始めのしきい値のこの dB 下。話し始めの付近で音が揺れても、話し始めと話し終わりを繰り返さない
// ための固定の幅で、OBS のノイズゲートの既定の開放しきい値と閉鎖しきい値の差 (-26 dB と -32 dB) と同じ。
constexpr int kSelfSpeakingHysteresisDb = 6;

// しきい値の範囲 (dBFS) と既定は config/settings.h の kSelfSpeakingThresholdMinDb、kSelfSpeakingThresholdMaxDb、
// kDefaultSelfSpeakingThresholdDb にある。

// しきい値を下回ってから話し終わりにするまでの長さ (ナノ秒)。pipeline/voice_activity.h の kVoiceActivityGapUs と同じ
// 100 ms で、Discord から受け取った声の判定と切れ目をそろえる。
constexpr std::int64_t kSelfSpeakingGapNs = 100'000'000;

// パケットが届かない状態がこの長さ続いたら、判定できないとみなす (Advanced Scene Switcher の音声の条件と同じ 250 ms)。
constexpr std::int64_t kSelfSpeakingStallNs = 250'000'000;

// peak (線形。0 から 1 が満度) を dBFS にする。0 以下と NaN は dBFS の下限 (-96) にする。
inline double peak_to_db(double peak)
{
	if (!(peak > 0.0))
		return static_cast<double>(config::kSelfSpeakingThresholdMinDb);
	return std::max(20.0 * std::log10(peak), static_cast<double>(config::kSelfSpeakingThresholdMinDb));
}

// 1 つのチャンネルのパケットの peak (サンプルの絶対値の最大)。samples が空か frames が 0 なら 0。NaN のサンプルは無視する。
inline float plane_peak(const float *samples, std::size_t frames)
{
	float peak = 0.0f;
	if (samples == nullptr)
		return peak;
	for (std::size_t i = 0; i < frames; ++i) {
		const float value = std::fabs(samples[i]);
		if (value > peak)
			peak = value;
	}
	return peak;
}

// しきい値の設定を範囲に収める。
inline int clamp_self_speaking_threshold_db(int db)
{
	return std::min(std::max(db, config::kSelfSpeakingThresholdMinDb), config::kSelfSpeakingThresholdMaxDb);
}

// 配信に声が出ているか。OBS のミュート (プッシュトゥトークで押していないときを含む)、ソースのフェーダーが 0 以下
// (NaN を含む)、ソースが配信に出ていない、のどれかなら偽。
inline bool self_source_audible(bool muted, float fader, bool source_active)
{
	return !muted && fader > 0.0f && source_active;
}

// 1 つのパケットの情報。
struct SelfSpeakingPacket {
	std::uint64_t start_ns = 0; // 配信で鳴る時刻 (timestamp と同期のオフセットの和。0 は使わない)
	std::int64_t length_ns = 0; // パケットの長さ
	double peak_db = -96.0;     // 全チャンネルの peak (dBFS)
	bool audible = true;        // self_source_audible の値。偽の間は、音の大きさによらず話していないとみなす
};

// 判定が出す、話し始めと話し終わりの時刻。start_ns が 0 でなければ話し始め、stop_ns が 0 でなければ話し終わり
// (意味は decor_registry.h の set_voice_speaking と同じ)。
struct SelfSpeakingChange {
	std::uint64_t start_ns = 0;
	std::uint64_t stop_ns = 0;
};

// パケットの peak から、話しているかを決める。話し始めのしきい値以上で話し始め、続けるしきい値 (6 dB 下) 以上の間は
// 話し続け、下回ってから kSelfSpeakingGapNs たつと話し終わりにする。話し終わりの時刻は、最後に音があったパケットの終わり。
class SelfSpeakingJudge {
public:
	// 話し始めのしきい値 (dBFS)。呼び出し元が範囲に収めてから渡す。
	void set_threshold_db(int db) { threshold_db_ = db; }

	// パケットを 1 つ見る。パケットは時刻の順に渡す。
	//   now_ns  パケットを受け取った時刻 (os_gettime_ns)。前のパケットから kSelfSpeakingStallNs より空いていたら、
	//           前の状態を捨てる (restarted を真にする)
	// 戻り値の restarted が真なら、呼び出し元は先に表の話している状態を消す。changes は、話し終わり、話し始めの順。
	struct Result {
		bool restarted = false;
		std::vector<SelfSpeakingChange> changes;
	};
	Result observe(const SelfSpeakingPacket &packet, std::uint64_t now_ns)
	{
		Result result;
		if (seen_ns_ != 0 && now_ns > seen_ns_ &&
		    static_cast<std::int64_t>(now_ns - seen_ns_) > kSelfSpeakingStallNs) {
			reset();
			result.restarted = true;
		}
		seen_ns_ = now_ns;

		bool loud = false;
		if (packet.audible) {
			const double start_line = static_cast<double>(threshold_db_);
			const double keep_line = start_line - static_cast<double>(kSelfSpeakingHysteresisDb);
			if (packet.peak_db >= start_line)
				open_ = true;
			else if (packet.peak_db < keep_line)
				open_ = false;
			loud = open_;
		} else {
			open_ = false;
		}

		const std::uint64_t length = packet.length_ns > 0 ? static_cast<std::uint64_t>(packet.length_ns) : 0;
		const std::uint64_t start = packet.start_ns > 0 ? packet.start_ns : 1;
		if (loud) {
			if (!talking_)
				result.changes.push_back(SelfSpeakingChange{start, 0});
			talking_ = true;
			last_loud_end_ns_ = start + length;
		} else if (talking_ && start >= last_loud_end_ns_ + static_cast<std::uint64_t>(kSelfSpeakingGapNs)) {
			result.changes.push_back(SelfSpeakingChange{0, last_loud_end_ns_});
			talking_ = false;
		}
		// 話し終わり、話し始めの順にする (話し終わりと話し始めが同じパケットで続くことはないが、順を固定しておく)。
		std::stable_sort(result.changes.begin(), result.changes.end(),
				 [](const SelfSpeakingChange &a, const SelfSpeakingChange &b) {
					 return a.stop_ns != 0 && b.stop_ns == 0;
				 });
		return result;
	}

	// 覚えている状態を捨てる (話し終わりは返さない)。ソースを付け直したときと、見失ったときに呼ぶ。
	void reset()
	{
		open_ = false;
		talking_ = false;
		last_loud_end_ns_ = 0;
		seen_ns_ = 0;
	}

	// 話している (話し終わりにしていない) か。
	bool talking() const { return talking_; }

private:
	int threshold_db_ = config::kDefaultSelfSpeakingThresholdDb;
	bool open_ = false;
	bool talking_ = false;
	std::uint64_t last_loud_end_ns_ = 0;
	std::uint64_t seen_ns_ = 0;
};

// 自分の枠の人の話している・話していないを、音声ソースで決めているか。
//   NotJudged       音声ソースを指定していない (または判定する人が決まっていない)。Discord から受け取った声で決める
//   Determinable    ソースが見つかっていて、パケットが届いている。話している・話していないを、ソースの判定で決める
//   Undeterminable  指定があるのに、ソースが見つからないか、パケットが届かない。光らせず、暗くもしない
enum class SourceVoice : std::uint8_t { NotJudged, Determinable, Undeterminable };

// 時刻 now_ns の SourceVoice。
//   designated       音声ソースの指定があり、判定する人の user_id が決まっているか
//   found            指定したソースを見つけてコールバックを付けているか
//   last_packet_ns   最後にパケットを受け取った時刻 (0 は一度も受け取っていない)
// now_ns が last_packet_ns より前のときは (別のスレッドの時刻の読みの前後)、届いているとみなす。
inline SourceVoice source_voice_at(bool designated, bool found, std::uint64_t last_packet_ns, std::uint64_t now_ns)
{
	if (!designated)
		return SourceVoice::NotJudged;
	if (!found || last_packet_ns == 0)
		return SourceVoice::Undeterminable;
	if (now_ns > last_packet_ns && static_cast<std::int64_t>(now_ns - last_packet_ns) > kSelfSpeakingStallNs)
		return SourceVoice::Undeterminable;
	return SourceVoice::Determinable;
}

// cordmux がその人の声を聞けるか (名前ラベルを暗くするかの判断に使う)。ソースで決める間は、見つかっているので真、
// 判定できない間は偽。discord_hears は Discord から受け取った声の経路があるか。
inline bool source_voice_hears(SourceVoice voice, bool discord_hears)
{
	switch (voice) {
	case SourceVoice::Determinable:
		return true;
	case SourceVoice::Undeterminable:
		return false;
	case SourceVoice::NotJudged:
		break;
	}
	return discord_hears;
}

// 名前ラベルの光に使う発言の時刻。ソースで決める間は source、判定できない間は光らせない空の時刻、指定がなければ
// discord (Discord から受け取った声の区間から求めた時刻)。
inline SpeakingTimes source_voice_speaking(SourceVoice voice, const SpeakingTimes &discord, const SpeakingTimes &source)
{
	switch (voice) {
	case SourceVoice::Determinable:
		return source;
	case SourceVoice::Undeterminable:
		return SpeakingTimes{};
	case SourceVoice::NotJudged:
		break;
	}
	return discord;
}

// 音声ソースの候補 1 つ (OBS の列挙で集めた値)。eligible は、候補にしてよい種類か (音声を持つ入力のソースで、cordmux の
// 種類ではない)。
struct SelfSourceCandidate {
	std::string uuid;
	std::string name;
	bool eligible = false;
	bool is_scene = false; // OBS のシーン (自分の枠に映す映像の候補にだけ入る)
	// 利用者が OBS で削除したソース (obs_source_removed)。削除したソースも、参照が残る間は OBS の列挙に出続けるので、
	// 列挙で集めた後にここで除く。OBS 自身のソースの一覧も、削除したソースを出さない。
	bool removed = false;
};

// 候補として使ってよいか (種類が合い、削除されていない)。
inline bool usable_self_source(const SelfSourceCandidate &candidate)
{
	return candidate.eligible && !candidate.removed;
}

// OBS のソースの種類の ID が cordmux の種類 (合成のソースと、その子) か。cordmux の種類は音声ソースの候補にしない。
// 合成のソースを選ぶと、誰が話しても自分の枠の人が話しているとみなされる。
inline bool is_cordmux_source_id(std::string_view id)
{
	return id.substr(0, 8) == "cordmux_";
}

// 指定した UUID と名前に合う候補の添字を返す (なければ -1)。UUID が合う候補を先に探し、なければ名前が合う最初の候補を
// 探す (名前を変えても UUID は変わらないが、OBS の「マイク音声」を無効にして戻したときとシーンコレクションを切り替えた
// ときは UUID が変わる)。使ってよい候補 (usable_self_source) だけを探す。uuid と name が両方とも空なら -1。
inline int find_self_source(const std::vector<SelfSourceCandidate> &all, const std::string &uuid,
			    const std::string &name)
{
	if (!uuid.empty()) {
		for (std::size_t i = 0; i < all.size(); ++i) {
			if (usable_self_source(all[i]) && all[i].uuid == uuid)
				return static_cast<int>(i);
		}
	}
	if (!name.empty()) {
		for (std::size_t i = 0; i < all.size(); ++i) {
			if (usable_self_source(all[i]) && all[i].name == name)
				return static_cast<int>(i);
		}
	}
	return -1;
}

// 1 dB 刻みの peak の分布。しきい値を決め直す材料として、一定の間のパケットの peak の分布と、話していると判定した
// 割合をログに書くために使う。
class PeakHistogram {
public:
	struct Summary {
		std::uint64_t packets = 0;
		int p50_db = -96;
		int p95_db = -96;
		int max_db = -96;
		double talking_ratio = 0.0; // 話していると判定したパケットの割合 (0 から 1)
	};

	// パケット 1 つを数える。peak_db は dBFS、talking はそのパケットを受け取った後の判定。
	void add(double peak_db, bool talking)
	{
		const int clipped =
			std::min(std::max(static_cast<int>(std::floor(peak_db)), config::kSelfSpeakingThresholdMinDb),
				 config::kSelfSpeakingThresholdMaxDb);
		++bins_[static_cast<std::size_t>(clipped - config::kSelfSpeakingThresholdMinDb)];
		++packets_;
		if (talking)
			++talking_packets_;
	}

	// 数えた分をまとめて、数えた値を 0 に戻す。パケットがなければ packets が 0 の Summary を返す。
	Summary take()
	{
		Summary summary;
		summary.packets = packets_;
		if (packets_ > 0) {
			summary.p50_db = percentile(0.50);
			summary.p95_db = percentile(0.95);
			for (std::size_t i = bins_.size(); i-- > 0;) {
				if (bins_[i] > 0) {
					summary.max_db = static_cast<int>(i) + config::kSelfSpeakingThresholdMinDb;
					break;
				}
			}
			summary.talking_ratio = static_cast<double>(talking_packets_) / static_cast<double>(packets_);
		}
		bins_.fill(0);
		packets_ = 0;
		talking_packets_ = 0;
		return summary;
	}

private:
	// 小さいほうから数えて、全体の fraction に届く分布の位置の dB (最近順位法)。
	int percentile(double fraction) const
	{
		const auto target = static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(packets_)));
		std::uint64_t seen = 0;
		for (std::size_t i = 0; i < bins_.size(); ++i) {
			seen += bins_[i];
			if (seen >= target && seen > 0)
				return static_cast<int>(i) + config::kSelfSpeakingThresholdMinDb;
		}
		return config::kSelfSpeakingThresholdMaxDb;
	}

	std::array<std::uint64_t, config::kSelfSpeakingThresholdMaxDb - config::kSelfSpeakingThresholdMinDb + 1> bins_{};
	std::uint64_t packets_ = 0;
	std::uint64_t talking_packets_ = 0;
};

} // namespace cordmux::sources

// SPDX-License-Identifier: Apache-2.0

// 処理の段階ごとの件数 (受信、デコード、OBS に渡した、捨てた)。ストリームごとに、映像と音声を分けて数える。
// 数えるだけで、合否は判定しない。合否の判定には OBS が出力した映像を使い、この件数は、どの段階で落ちたかを
// 切り分けるために使う。libobs にも Qt にも依存せず、C++17 の標準ライブラリだけを使う。ヘッダーだけで実装する
// 理由は allocation.h と同じ。

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace cordmux::config {

// 映像 (H.264 のアクセスユニット) か、音声 (PCM のチャンク) か。画面共有のストリームは、同じ stream_id で映像と
// 音声の両方を送る (音声は 10 ms = 480 サンプルごとのチャンクで、毎秒 100 個。docs/wire_format.md の「音声の
// ペイロード」)。stream_id だけをキーにすると、画面共有のストリームでは、映像 (毎秒 30 前後) と音声 (毎秒 100) の
// 件数が 1 つの数に混ざり、段階ごとの毎秒の件数を読めないので、MediaKind もキーにする。
enum class MediaKind { Video, Audio };

// 受信、デコード、OBS に渡した、捨てた、の 4 つの段階の件数。
struct StageCounters {
	std::uint64_t received = 0;
	std::uint64_t decoded = 0;
	std::uint64_t handed_to_obs = 0;
	std::uint64_t dropped = 0;
};

// 1 つのストリームの累積の件数。増やすだけで、現在の値は snapshot() でしか読めない (呼び出すたびに、副作用なく
// 同じ値を返す)。
class StageCounterAccumulator {
public:
	void add_received(std::uint64_t n = 1) { totals_.received += n; }
	void add_decoded(std::uint64_t n = 1) { totals_.decoded += n; }
	void add_handed_to_obs(std::uint64_t n = 1) { totals_.handed_to_obs += n; }
	void add_dropped(std::uint64_t n = 1) { totals_.dropped += n; }

	StageCounters snapshot() const { return totals_; }

private:
	StageCounters totals_{};
};

// 複数のストリーム (stream_id ごと。stream_id は u32) の件数を、映像と音声に分けて持つ。画面共有の 4 つと、声の
// 8 つを同時に数える。
//
// stream_id ごとの表は際限なく大きくしない。経路 (pipeline::StreamRouter) を持たない stream_id のメッセージは、
// stream_id ごとの表に追加せず、no_route(media) の 1 つの件数にまとめる。経路のないメッセージは捨てるしかなく、
// stream_id ごとに分けて見る意味がないうえ、壊れた送信側や古い送信側が任意の stream_id を送るだけで、表が大きく
// なるためである。configure から消えた stream_id の項目は retain() で削除する。削除した項目の値は、media ごとの
// 「削除した分の合計」に足すので、total_snapshot() は削除しても減らない (rate_between が負の値を返さない)。
class StreamStageCounters {
public:
	StageCounterAccumulator &stream(std::uint32_t stream_id, MediaKind media)
	{
		return streams_[std::make_pair(stream_id, media)];
	}

	// 経路のない stream_id のメッセージの件数 (クラスのコメント)。
	StageCounterAccumulator &no_route(MediaKind media) { return no_route_[media]; }

	// media の側の、すべてのストリームの合計。stream_id ごとの表にある分に加えて、経路のないメッセージの分と、
	// retain() で削除した分も足す。
	StageCounters total_snapshot(MediaKind media) const
	{
		StageCounters total;
		const auto add = [&total](const StageCounters &s) {
			total.received += s.received;
			total.decoded += s.decoded;
			total.handed_to_obs += s.handed_to_obs;
			total.dropped += s.dropped;
		};
		for (const auto &[key, acc] : streams_) {
			if (key.second == media)
				add(acc.snapshot());
		}
		if (const auto it = no_route_.find(media); it != no_route_.end())
			add(it->second.snapshot());
		if (const auto it = retired_.find(media); it != retired_.end())
			add(it->second);
		return total;
	}

	// media の側の、経路のないメッセージの分だけのスナップショット。
	StageCounters no_route_snapshot(MediaKind media) const
	{
		const auto it = no_route_.find(media);
		return it == no_route_.end() ? StageCounters{} : it->second.snapshot();
	}

	// media の側の、stream_id ごとのスナップショットの一覧 (経路のないメッセージの分と、削除した分は含まない。
	// それぞれ no_route_snapshot と total_snapshot で見る)。
	std::map<std::uint32_t, StageCounters> per_stream_snapshot(MediaKind media) const
	{
		std::map<std::uint32_t, StageCounters> result;
		for (const auto &[key, acc] : streams_) {
			if (key.second == media)
				result[key.first] = acc.snapshot();
		}
		return result;
	}

	// keep にない stream_id の項目を削除する (configure から消えた stream_id)。削除する前の値は、media ごとの
	// 「削除した分の合計」に足すので、total_snapshot() は削除しても減らない。
	void retain(const std::set<std::uint32_t> &keep)
	{
		for (auto it = streams_.begin(); it != streams_.end();) {
			if (keep.count(it->first.first) != 0) {
				++it;
				continue;
			}
			StageCounters &r = retired_[it->first.second];
			const StageCounters s = it->second.snapshot();
			r.received += s.received;
			r.decoded += s.decoded;
			r.handed_to_obs += s.handed_to_obs;
			r.dropped += s.dropped;
			it = streams_.erase(it);
		}
	}

private:
	std::map<std::pair<std::uint32_t, MediaKind>, StageCounterAccumulator> streams_;
	std::map<MediaKind, StageCounterAccumulator> no_route_;
	std::map<MediaKind, StageCounters> retired_;
};

// 段階ごとの毎秒の件数 (Hz)。
struct StageRates {
	double received_hz = 0.0;
	double decoded_hz = 0.0;
	double handed_to_obs_hz = 0.0;
	double dropped_hz = 0.0;
};

// 2 つの snapshot の差から、毎秒の件数を段階ごとに求める純粋関数 (elapsed_us はマイクロ秒)。elapsed_us が 0 以下
// なら、すべての段階で 0 を返す。cur < prev (件数が巻き戻った) の段階は、その段階だけ 0 にする (0 で割らず、
// 負の値も返さない)。合計 (total_snapshot) は retain() で削除しても巻き戻らないが、stream_id ごとの値は、その
// stream_id が一度削除されて、同じ番号で作り直されると巻き戻ることがある。
inline StageRates rate_between(const StageCounters &prev, const StageCounters &cur, std::int64_t elapsed_us)
{
	StageRates rates;
	if (elapsed_us <= 0)
		return rates;

	const double elapsed_s = static_cast<double>(elapsed_us) / 1000000.0;

	auto hz = [&](std::uint64_t before, std::uint64_t after) -> double {
		if (after < before)
			return 0.0;
		return static_cast<double>(after - before) / elapsed_s;
	};

	rates.received_hz = hz(prev.received, cur.received);
	rates.decoded_hz = hz(prev.decoded, cur.decoded);
	rates.handed_to_obs_hz = hz(prev.handed_to_obs, cur.handed_to_obs);
	rates.dropped_hz = hz(prev.dropped, cur.dropped);
	return rates;
}

} // namespace cordmux::config

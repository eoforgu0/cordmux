// SPDX-License-Identifier: Apache-2.0

// 合成のソースから出す音の、置いてよいかの判断と置く時刻の計算と、ミキサーの組 (人と種類) の決め方。どれも OBS に
// 依らない純粋な関数で、pipeline.cpp の handle_audio_frame と組の出入りの処理が使う。時計は読まず、時刻はすべて
// 引数で受け取る。
//
// 時刻の領域。送信側のタイムスタンプ (整列した値) は送信側の単調な時計の上にあり、AudioMixer の時刻は OBS の時計
// (os_gettime_ns / 1000) の上にある。換算は、接続ごとに決めた 2 つの時計の差を足すだけで (pipeline/sender_clock.h)、
// 映像を渡す時刻の換算 (pipeline.cpp の push_decoded_frame) と同じ差を使う。呼び出し元が換算してから渡す。
// libobs にも asio にも依存しない。

#pragma once

#include "config/allocation.h"
#include "pipeline/audio_mixer.h"
#include "pipeline/stream_router.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace cordmux::pipeline {

// 声の届き方の揺れを吸う幅 J (マイクロ秒)。声の at_us は「届いた時の OBS の時計 + J + 声の遅延」である。
// 先渡しの幅 (kMixerLookaheadUs) 以上が要る: AudioMixer は区間の終わり近くの音を、その時刻の先渡しの幅だけ前に出す
// ので、J が短いと、チャンクの先頭が届く前にその区間を出してしまい、遅れて届いた分として捨てる。先渡しの幅に足す
// 10 ms は、届き方の揺れの余裕である。実際の通話で測って決め直す値。声は J の分だけ遅れて鳴る。
constexpr std::int64_t kVoiceMixJitterUs = kMixerLookaheadUs + 10000;
static_assert(kVoiceMixJitterUs >= kMixerLookaheadUs, "J は AudioMixer の先渡しの幅以上にする");

// 画面の音に足す、OBS の描く刻みの半分の長さ。映像は「時刻 + D の後の最初の刻み」に表示される (frame_pacer.h)
// ので、刻みの上に来るまで 0 から 1 刻み、平均で半刻み遅れて表示される。画面の音も同じだけ遅らせて、映像と
// 平均でそろえる。刻みの間隔が分からない (OBS の映像が動いていない。0 以下) ときは、映像も刻みに合わせず
// 時刻 + D に渡すので、0 を返す。
inline std::int64_t screen_audio_half_tick_us(std::int64_t tick_interval_us)
{
	return tick_interval_us > 0 ? tick_interval_us / 2 : 0;
}

// 画面の音を置く時刻。
//   mapped_obs_us     av_offset の map_audio を通した送信側のタイムスタンプ (画面の音と映像のずれの補正のうち、
//                     音を遅らせる正の補正を含む) を、OBS の時計に換算した値 (pipeline/sender_clock.h)
//   delay_us          表示の緩衝 D
//   tick_interval_us  OBS の描く刻みの間隔 (読めなければ 0)
//   early_us          負の補正の絶対値 (Impl の audio_early_us)。合成のソースの映像は表示の時刻が変わらないので、
//                     音を置く時刻から引いて音を早める
// 下限は obs_now_us + J より前にしない。D が小さい (合成のときは D = 0 も選べ、映像は待ち行列を通る) と、計算した
// 時刻が AudioMixer の受け付ける位置 (出し終えた位置。今から先渡しの幅ほど先) より前になり、画面の音が全部「遅れて
// 届いた分」として捨てられて無音になる。下限があれば、映像より少し遅れても音は出る。そのときの映像とのずれは
// 最大で J - D - 半刻みほど (D = 0 で約 57 ms) で、音をなくすよりこちらを選ぶ。
inline std::int64_t screen_audio_at_us(std::int64_t mapped_obs_us, std::int64_t obs_now_us, std::int64_t delay_us,
				       std::int64_t tick_interval_us, std::int64_t early_us)
{
	const std::int64_t at = mapped_obs_us + std::max<std::int64_t>(delay_us, 0) +
				screen_audio_half_tick_us(tick_interval_us) - std::max<std::int64_t>(early_us, 0);
	return std::max(at, obs_now_us + kVoiceMixJitterUs);
}

// 声を置く時刻。D では遅らせない。届いた時の OBS の時計に、揺れを吸う幅 J と声の遅延 (voice_audio_delay_ms) を足す。
inline std::int64_t voice_audio_at_us(std::int64_t obs_now_us, std::int64_t voice_delay_us)
{
	return obs_now_us + kVoiceMixJitterUs + std::max<std::int64_t>(voice_delay_us, 0);
}

// 受け取った音のチャンク 1 つ (画面の音か声) を、AudioMixer に置いてよいか。置かない音は、呼び出し元が捨てて数える
// (pipeline.cpp の add_mixed_audio_locked)。
//   composite_present  合成のソースが現在のシーンコレクションにあるか。音はいつも合成のソースから出すので、ない間は
//                      出口がない (取り出すスレッドも、ない間は取り出さない)
//   dummy_mode         ダミー表示の間か (sources/dummy_feed.h)。ダミー表示は通話がなくても見た目を確認するための
//                      表示で、映像を色のタイルに替えている間は、実際の画面共有の音と声も配信に出さない
inline bool mixed_audio_admitted(bool composite_present, bool dummy_mode)
{
	return composite_present && !dummy_mode;
}

// ストリームの経路から、ミキサーの組を決める。枠の外 (slot が 1 未満。枠から外した人) は組を持たない。全画面だけで映す人の
// 経路 (config::kFocusOnlySlot) も組を持たない (画面共有の音は出さない。stream_router.h の AssignPolicy::focus_user_id)。
// 画面の音は画面共有している人の組。声は枠に載っている人 (slot_owner) の組で、使う声 (voice_links) で別の人の声を
// 運んでいる枠でも、枠に載っている人の利得が動作する。枠に載っている人が分からなければ経路の人にする。
inline std::optional<MixerKey> mixer_key_for(const Route &route, const std::string &slot_owner)
{
	if (route.slot < 1)
		return std::nullopt;
	if (route.kind == config::SlotKind::Screen && route.slot == config::kFocusOnlySlot)
		return std::nullopt;
	if (route.kind == config::SlotKind::Screen)
		return MixerKey{route.user_id, MixerKind::ScreenAudio};
	return MixerKey{slot_owner.empty() ? route.user_id : slot_owner, MixerKind::Voice};
}

// 現在の経路と割り当て表から、ストリームごとの組を作る。組の出入り (MixerRoster::sync) の入力になる。
inline std::map<std::uint32_t, MixerKey> desired_mixer_keys(const StreamRouter &router,
							    const config::AllocationTable &table)
{
	std::map<std::uint32_t, MixerKey> out;
	for (const std::uint32_t stream_id : router.route_ids()) {
		const std::optional<Route> route = router.route(stream_id);
		if (!route)
			continue;
		const config::SlotAssignment *assignment = table.assignment(route->kind, route->slot);
		const std::string owner = assignment ? assignment->owner_user_id : std::string();
		if (const std::optional<MixerKey> key = mixer_key_for(*route, owner))
			out.emplace(stream_id, *key);
	}
	return out;
}

} // namespace cordmux::pipeline

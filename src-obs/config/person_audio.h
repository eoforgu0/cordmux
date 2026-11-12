// SPDX-License-Identifier: Apache-2.0

// 参加者ごとの音量 (声と画面共有の音のそれぞれ) の範囲と、音量と利得の変換。純粋な定数と関数で、libobs にも Qt にも依存
// しない。音量は 1 刻みの整数の % で持つ (Discord のユーザー音量と同じ 0 から 200)。利得は音量を 100 で割った線形の
// 倍率で、ミキサーに掛ける値である。音量を整数で持つのは、スライダー、ファイル、表示が同じ 1 刻みの値を行き来する
// ときに、小数の利得を通して値がずれないようにするためである。

#pragma once

#include <algorithm>

namespace cordmux::config {

inline constexpr int kPersonVolumeMinPct = 0;
inline constexpr int kPersonVolumeMaxPct = 200;
inline constexpr int kPersonVolumeDefaultPct = 100;

// 範囲の外の音量を端に収める。
inline int clamp_person_volume_pct(int pct)
{
	return std::clamp(pct, kPersonVolumeMinPct, kPersonVolumeMaxPct);
}

// 音量が範囲の中か。
inline bool person_volume_in_range(int pct)
{
	return pct >= kPersonVolumeMinPct && pct <= kPersonVolumeMaxPct;
}

// 音量 (%) をミキサーの利得 (線形の倍率) にする。範囲の外は端に収める。
inline float gain_of_person_volume(int pct)
{
	return static_cast<float>(clamp_person_volume_pct(pct)) / 100.0f;
}

} // namespace cordmux::config

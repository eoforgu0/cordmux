// SPDX-License-Identifier: Apache-2.0

// 参加者ごとの音の大きさ (ドックの音声タブのメーターと、参加者タブのマイクの光) を作るコンポーネント。AudioMixer が区間ごとに
// 組 (人と種類) の振れ幅の最大を足し、ドックが読む。libobs にも Qt にも依存しない。ロックは持たず、呼び出し元
// (Pipeline::Impl の mixer_mu) の中で使う。
//
// 値は線形の振れ幅 (1.0 が 0 dBFS) で持ち、画面に出す dB と 0 から 1 の位置への変換はこのファイルの関数が行う。
// メーターの動きは、すぐ上がり、ゆっくり下がる形にする (音量メーターの普通の動きで、短い声の山を見逃さない)。
// 下がる処理を読む側に任せると、ドックの呼び出しの間隔で見え方が変わり、読むたびに値が消える形だと読む側が
// 2 つあると取り合うので、ミキサーの側で区間ごとに下げ、読んでも値は変わらない形にする。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace cordmux::pipeline {

// メーターに出す範囲 (dBFS)。これより小さい値は下端に、0 より大きい値は上端に収める。
constexpr float kMeterFloorDb = -48.0f;
constexpr float kMeterCeilingDb = 0.0f;

// 振れ幅が 0 のときの dBFS (log10(0) を避けるための下端)。
constexpr float kSilenceDb = -120.0f;

// ピークが下がる速さ (dB/秒)。20 dB/秒なら、0 dBFS の山が表示の下端 (-48 dB) まで下がるのに 2.4 秒かかる。
// 言葉の途切れ目でメーターが跳ねず、止まった声は 2 秒余りで消える速さとして選んだ目安の値である。
constexpr float kMeterFallDbPerSecond = 20.0f;

// これより小さい保持値は 0 にする (-100 dBFS。下がり続ける値が非正規化数になるのを避ける)。
constexpr float kMeterZeroPeak = 1e-5f;

// 線形の振れ幅を dBFS にする。0 以下と NaN は kSilenceDb。
inline float peak_to_dbfs(float peak)
{
	if (!(peak > 0.0f))
		return kSilenceDb;
	return std::max(20.0f * std::log10(peak), kSilenceDb);
}

// dBFS をメーターの位置 (0 が下端 kMeterFloorDb、1 が上端 kMeterCeilingDb) にする。範囲の外は端に収め、NaN は 0。
inline float meter_position(float dbfs)
{
	if (!(dbfs > kMeterFloorDb))
		return 0.0f;
	if (dbfs >= kMeterCeilingDb)
		return 1.0f;
	return (dbfs - kMeterFloorDb) / (kMeterCeilingDb - kMeterFloorDb);
}

// 1 区間ぶんピークが下がるときの倍率 (0 より大きく 1 以下)。block_frames は区間のフレーム数、sample_rate は
// 1 秒のフレーム数。
inline float meter_decay_per_block(std::int64_t block_frames, std::int64_t sample_rate)
{
	if (sample_rate <= 0)
		return 1.0f;
	const float seconds = static_cast<float>(std::max<std::int64_t>(block_frames, 0)) /
			      static_cast<float>(sample_rate);
	return std::pow(10.0f, -kMeterFallDbPerSecond * seconds / 20.0f);
}

// 1 つの値のピークホールド。区間のピークが保持値より大きければすぐ上がり、小さければ区間ごとに decay を掛けて下がる。
class PeakHold {
public:
	void update(float block_peak, float decay)
	{
		const float peak = block_peak > 0.0f ? block_peak : 0.0f; // 負の値と NaN は 0
		held_ = std::max(peak, held_ * decay);
		if (held_ < kMeterZeroPeak)
			held_ = 0.0f;
	}

	void clear() { held_ = 0.0f; }
	float value() const { return held_; }

private:
	float held_ = 0.0f;
};

} // namespace cordmux::pipeline

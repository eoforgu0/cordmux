// SPDX-License-Identifier: Apache-2.0

// 映像の整列で付いたオフセット (出力 - 送信側) を、同じ画面共有の音声のタイムスタンプにも足す。
// 映像だけを整列すると、同じ時計で付いた映像と音声のタイムスタンプの関係が崩れ、音声と映像がずれる。
// 同期用のマーカーは使わず、映像のフレームが来るたびにオフセットを保存し、音声には直近の値を足す。
// libobs にも Qt にも依存しない。C++17 の標準ライブラリだけを使う。

#pragma once

#include <cstdint>

namespace cordmux::align {

struct AvOffsetConfig {
	// 画面共有の音声のタイムスタンプに足す定数 (us)。正の値は音声を遅らせる。0 なら何も足さない。
	// pipeline/ が設定の screen_audio_offset_ms の正の部分を渡す (pipeline/av_offset_plan.h)。
	int64_t constant_offset_us = 0;

	// 音声のタイムスタンプを単調増加にする (前の値より戻さない) か。false なら映像のオフセットをそのまま足す。
	// 映像のオフセットが減ると、その直後の音声のタイムスタンプが前の値より戻ることがある。
	// OBS が戻った音声のタイムスタンプをどう扱うかは確認していないので、既定は false にしてある。
	// オフセットが減った直後に、OBS の音声が途切れたりずれたりするかを確認すれば決まる。
	bool enforce_monotonic_audio = false;

	// enforce_monotonic_audio が true のときに、直前に返した音声のタイムスタンプから進める最小の間隔 (us)。
	int64_t min_audio_step_us = 1;
};

// 統計。pipeline/ が OBS のログの統計の行に書く。
struct AvOffsetStats {
	int64_t video_frames = 0;            // 受け取った映像のフレーム数
	int64_t audio_frames = 0;            // 受け取った音声のチャンクの数
	int64_t passthrough_audio_frames = 0; // 最初の映像より前に、そのまま返した音声のチャンクの数
	int64_t video_offset_us = 0;         // 直近の映像のオフセット (出力 - 送信側)
	int64_t offset_changes = 0;          // オフセットが変わった回数 (最初の映像も 1 回に数える)
	int64_t monotonic_fix_count = 0;     // 音声のタイムスタンプを進めて、戻りを防いだ回数
};

// 画面共有のストリームごとに 1 つ作る。スレッドセーフではない。note_video と map_audio を別のスレッドから
// 呼び出すときは、呼び出し元が同じロックで守る (pipeline/ はデコードのスレッドと受信のスレッドから、
// 同じロックの下で呼び出す)。
class AvOffsetTranscriber {
public:
	AvOffsetTranscriber() = default;
	explicit AvOffsetTranscriber(const AvOffsetConfig &cfg) : cfg_(cfg) {}

	// 映像の 1 フレームのオフセットを保存する。sender_ts_us は送信側のタイムスタンプ、out_ts_us は
	// TimestampAligner::align が返したタイムスタンプ。
	void note_video(int64_t sender_ts_us, int64_t out_ts_us);

	// 音声の 1 チャンクのタイムスタンプに、直近の映像のオフセットと constant_offset_us を足して返す。
	// 最初の映像が来る前は、足すオフセットがまだないので、受け取った値をそのまま返す。
	int64_t map_audio(int64_t audio_sender_ts_us);

	bool has_video() const { return has_video_; }
	int64_t video_offset_us() const { return offset_us_; }

	void set_constant_offset_us(int64_t us) { cfg_.constant_offset_us = us; }
	int64_t constant_offset_us() const { return cfg_.constant_offset_us; }

	const AvOffsetConfig &config() const { return cfg_; }
	const AvOffsetStats &stats() const { return stats_; }
	// 統計を含めて、作った直後の状態に戻す。設定は変えない。
	void reset();

private:
	AvOffsetConfig cfg_{};
	AvOffsetStats stats_{};
	bool has_video_ = false;
	int64_t offset_us_ = 0;
	bool has_last_audio_ = false;
	int64_t last_audio_out_us_ = 0;
};

} // namespace cordmux::align

// SPDX-License-Identifier: Apache-2.0

// 録画の音声の書き込み位置の計算。セグメントの先頭のチャンクのタイムスタンプを原点にし、各チャンクの「タイムスタンプ -
// 原点」の位置を目標にして、書く位置を align/audio_drift.h で目標に追従させる (配信の AudioMixer と同じ決め方)。
// 送信側のタイムスタンプは映像と同じ時計 (送信側の単調な時計) の上にあるので、音を取り出す時計との速さの差は、
// 書くサンプルの数を少しずつ変えて吸収する。差がしきい値を超えたら置き直し、後ろなら 0 のサンプルで埋め、前なら
// 書いたサンプルを消せないので呼び出し元がセグメントを分ける。
// 表示のための 2 つの補正 (screen_audio_offset_ms と voice_audio_delay_ms) は録画に適用しない。補正は OBS のソースでの
// 表示のずれを修正するためのもので、録画は libobs を通らないためである。libobs、Qt、avformat に依存しない。

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "align/audio_drift.h"
#include "record/record_types.h"

namespace cordmux::record {

// 長さ (us) をサンプル数に修正する (最も近い整数に丸める)。
inline std::int64_t silence_samples_for(std::int64_t gap_us, int sample_rate)
{
	if (gap_us <= 0 || sample_rate <= 0)
		return 0;
	return (gap_us * sample_rate + 500000) / 1000000;
}

// サンプル数を長さ (us) に修正する。
inline std::int64_t audio_samples_to_us(std::int64_t samples, int sample_rate)
{
	if (samples <= 0 || sample_rate <= 0)
		return 0;
	return (samples * 1000000 + sample_rate / 2) / sample_rate;
}

// 1 つの音声のストリーム (画面共有の音声か声) の、開いているセグメントの書き込みの状態。セグメントを開くたびに作り直す。
// サンプル数はすべて 1 チャンネルあたりで数える (2 チャンネルで 480 サンプルのチャンクなら 480 進む)。録画ログの
// sample_count も同じ数え方である。
struct AudioCursor {
	explicit AudioCursor(const align::AudioDriftConfig &config = align::AudioDriftConfig{})
		: corrector(config, 2), sample_rate(config.sample_rate)
	{
	}

	align::AudioDriftCorrector corrector;
	std::int64_t written_samples = 0; // セグメントの先頭から書いたサンプル数 (0 のサンプルを含む)
	int sample_rate = 48000;
};

struct AudioPlacement {
	// 書いたサンプルより前に置き直すことになった。カーソルは変えず、書くものもない。呼び出し元はセグメントを閉じ、
	// 新しいセグメントを開いて、新しいカーソルでもう一度 place_audio を呼ぶ。
	bool backward = false;
	// しきい値を超えた差で置き直した (セグメントの最初のチャンクは含まない)。録画ログの pts_reanchor に残す。
	bool resynced = false;
	std::int64_t drift_us = 0;        // 置き直したときの、目標 - 続けて書いたときの位置 (us。正は目標が後ろ)
	std::int64_t silence_samples = 0; // 書くサンプルの前に入れる 0 のサンプル数
	std::size_t frames = 0;           // out に入れた、書くサンプル数
};

// セグメントの原点からの時刻を、目標の位置 (サンプルの番号。小数も取る) に修正する。
inline double audio_target_pos(std::int64_t pts_us, std::int64_t origin_us, int sample_rate)
{
	return double(pts_us - origin_us) * double(sample_rate > 0 ? sample_rate : 1) / 1e6;
}

// チャンク (frames サンプル、2 チャンネルのインターリーブ) を置き、書くサンプルを out に入れる。chunk_pts_us はチャンクの
// タイムスタンプ、origin_us はセグメントの原点 (最初のチャンクのタイムスタンプ)。backward でなければ、カーソルの
// written_samples を、0 のサンプルと書くサンプルの分だけ進める (呼び出し元は、その順にファイルに書く)。
inline AudioPlacement place_audio(AudioCursor &cursor, std::int64_t chunk_pts_us, std::int64_t origin_us,
				  const float *samples, std::size_t frames, std::vector<float> *out)
{
	AudioPlacement placement;
	const double target = audio_target_pos(chunk_pts_us, origin_us, cursor.sample_rate);
	const align::AudioDriftTracker &tracker = cursor.corrector.tracker();
	const bool started = tracker.started();
	const double continued = tracker.next_input_pos();
	const double to_us = 1e6 / double(cursor.sample_rate > 0 ? cursor.sample_rate : 1);
	if (started && tracker.would_resync(target) && std::llround(target) < cursor.written_samples) {
		placement.backward = true;
		placement.drift_us = std::llround((target - continued) * to_us);
		out->clear();
		return placement;
	}

	const std::uint64_t resyncs_before = cursor.corrector.stats().resyncs;
	const align::AudioDriftStep step = cursor.corrector.push(target, samples, frames, out);
	if (cursor.corrector.stats().resyncs != resyncs_before) {
		placement.resynced = true;
		placement.drift_us = std::llround((target - continued) * to_us);
	}
	if (step.start_pos > cursor.written_samples)
		placement.silence_samples = step.start_pos - cursor.written_samples;
	placement.frames = step.frames;
	cursor.written_samples += placement.silence_samples + std::int64_t(step.frames);
	return placement;
}

// 今までに書いた末尾の時刻 (原点に、書いたサンプルの長さを足した値)。録画ログの last_pts_us と長さに使う。
inline std::int64_t audio_cursor_end_us(const AudioCursor &cursor, std::int64_t origin_us)
{
	return origin_us + audio_samples_to_us(cursor.written_samples, cursor.sample_rate);
}

// 録画ファイルの作成で音声を書き込む 1 件。録画ファイルの作成は、全体の長さの無音の PCM を作り、各セグメントの音声をその位置に書き込む。
// サンプル数は 1 チャンネルあたりで数える。
struct AudioWrite {
	std::string file;              // 中間ファイルのフォルダーからの相対パス (セグメントの mov)
	std::int64_t offset_samples = 0; // 生の PCM の先頭から数えた書き込み位置
	std::int64_t sample_count = 0;   // そのセグメントが持つサンプル数
};

// セッションの原点からの時刻を、生の PCM の中の位置に修正する。
inline std::int64_t audio_offset_samples(std::int64_t start_us, int sample_rate)
{
	return silence_samples_for(start_us, sample_rate);
}

} // namespace cordmux::record

// SPDX-License-Identifier: Apache-2.0

// 画面のストリームのキュー (ストリームごとのデコードのスレッドの入力) に、届いたアクセスユニットを積むかを
// 決める純粋関数。上限は数とバイト数の両方で持つ。数の上限だけでは、1 メッセージの上限
// (ws::ListenerConfig::max_message_bytes、既定 64 MiB) に近い大きさのアクセスユニットが続いたときに、
// 1 つのキューに 8 x 64 MiB まで積めるためである。libobs にも asio にも依存しない。

#pragma once

#include <cstddef>
#include <cstdint>

namespace cordmux::pipeline {

// decide_queue_admission の結果。呼び出し元 (pipeline.cpp) はそれぞれ次のように扱う。
//   Admit                   そのまま積む。
//   DropWaitingForKeyframe  キーフレームを待っている間に届いた P フレーム。この 1 つだけを捨てる。
//   OverflowClear           上限を超えた。キューに積んであった分も含めて全部捨て、次のキーフレームを待つ。
enum class QueueAdmission { Admit, DropWaitingForKeyframe, OverflowClear };

struct QueueAdmissionInput {
	std::size_t queued_count = 0;   // 積む前のキューの中のアクセスユニットの数
	std::size_t queued_bytes = 0;   // 積む前のキューの合計のバイト数
	std::size_t incoming_bytes = 0; // これから積むアクセスユニットのバイト数
	bool is_keyframe = false;       // 届いたアクセスユニットがキーフレームか
	bool waiting_for_keyframe = false; // 上限を超えて捨てた後、キーフレームを待っているか
};

struct QueueLimits {
	// どちらも目安の値で、測って決めたものではない。max_bytes は 1 メッセージの上限 (既定 64 MiB) の
	// max_count 倍にした。
	std::size_t max_count = 8;
	std::size_t max_bytes = 8ull * 64ull * 1024ull * 1024ull;
};

inline QueueAdmission decide_queue_admission(const QueueAdmissionInput &in, const QueueLimits &limits = QueueLimits{})
{
	if (in.waiting_for_keyframe) {
		// キーフレームを待っている間は P フレームを積まない (参照するフレームを捨てたので、デコードできない)。
		// キーフレームなら積む (呼び出し元が waiting_for_keyframe を false にする)。上限より先にここを
		// 見るので、呼び出し元は、積んだら待つのをやめるだけでよい。
		return in.is_keyframe ? QueueAdmission::Admit : QueueAdmission::DropWaitingForKeyframe;
	}
	if (in.queued_count >= limits.max_count)
		return QueueAdmission::OverflowClear;
	if (in.queued_bytes + in.incoming_bytes > limits.max_bytes)
		return QueueAdmission::OverflowClear;
	return QueueAdmission::Admit;
}

} // namespace cordmux::pipeline

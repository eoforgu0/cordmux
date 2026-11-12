// SPDX-License-Identifier: Apache-2.0

// ワイヤー形式のバイナリメッセージ (映像と音声) の 14 バイトのヘッダーを読み書きする。形式は docs/wire_format.md の
// 「バイナリメッセージ」にある。type が映像でも音声でもないメッセージと、flags の使っていないビットが立っている
// メッセージは、ここで読めないものとして扱い、受信側は捨てる。中身から種類を推測しない。
// libobs にも asio にも websocketpp にも依存しない。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace cordmux::ws {

enum class FrameType : uint8_t {
	Video = 1,
	Audio = 2,
};

// flags のビット。
inline constexpr uint8_t kFlagKeyframe = 0x01;        // bit0: キーフレーム (映像でだけ意味を持つ)
inline constexpr uint8_t kFlagRecordingOnly = 0x02;   // bit1: 録画専用の黒埋め用のフレーム
inline constexpr uint8_t kFlagReservedMask = 0xFC;    // bit2-7: 使っていない。立っていたらメッセージを捨てる

inline constexpr size_t kHeaderSize = 14; // ヘッダーの長さ (バイト)

struct FrameHeader {
	FrameType type = FrameType::Video;
	uint32_t stream_id = 0;
	uint64_t timestamp_us = 0;
	uint8_t flags = 0;

	bool is_keyframe() const { return (flags & kFlagKeyframe) != 0; }
	bool is_recording_only() const { return (flags & kFlagRecordingOnly) != 0; }
};

// message は、ヘッダー 14 バイトとペイロードを合わせた WebSocket の 1 メッセージの全体である。ペイロードの長さは
// メッセージの長さから分かるので、ヘッダーには持たない。
// 短すぎる、type が不正、使っていない flags のビットが立っている、のどれかなら nullopt を返す。
inline std::optional<FrameHeader> decode_frame_header(const uint8_t *message, size_t length)
{
	if (message == nullptr || length < kHeaderSize)
		return std::nullopt;

	const uint8_t type_byte = message[0];
	if (type_byte != uint8_t(FrameType::Video) && type_byte != uint8_t(FrameType::Audio))
		return std::nullopt;

	const uint8_t flags = message[13];
	if ((flags & kFlagReservedMask) != 0)
		return std::nullopt;

	FrameHeader h;
	h.type = FrameType(type_byte);
	// ワイヤー形式の整数はリトルエンディアンで、x86 と x64 もリトルエンディアンなので、memcpy でそのまま読む。
	std::memcpy(&h.stream_id, message + 1, sizeof(h.stream_id));
	std::memcpy(&h.timestamp_us, message + 5, sizeof(h.timestamp_us));
	h.flags = flags;
	return h;
}

// ペイロードの先頭。message は decode_frame_header が読めたメッセージ (kHeaderSize バイト以上) であること。
inline const uint8_t *frame_payload(const uint8_t *message) { return message + kHeaderSize; }

inline size_t frame_payload_size(size_t message_length)
{
	return message_length >= kHeaderSize ? message_length - kHeaderSize : 0;
}

// decode_frame_header の逆で、ヘッダーとペイロードから 1 メッセージを作る。単体テストがメッセージを作るのに使う。
inline std::vector<uint8_t> encode_frame(const FrameHeader &h, const uint8_t *payload, size_t payload_len)
{
	std::vector<uint8_t> out(kHeaderSize + payload_len);
	out[0] = uint8_t(h.type);
	std::memcpy(out.data() + 1, &h.stream_id, sizeof(h.stream_id));
	std::memcpy(out.data() + 5, &h.timestamp_us, sizeof(h.timestamp_us));
	out[13] = h.flags;
	if (payload_len > 0)
		std::memcpy(out.data() + kHeaderSize, payload, payload_len);
	return out;
}

} // namespace cordmux::ws

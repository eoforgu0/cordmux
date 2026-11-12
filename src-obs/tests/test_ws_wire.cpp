// SPDX-License-Identifier: Apache-2.0

// ws/wire.h の単体テスト。バイナリメッセージのヘッダーの読み書きと、不正な type と flags の扱いを確認する。

#include "doctest.h"

#include "ws/wire.h"

#include <cstdint>
#include <vector>

using cordmux::ws::FrameHeader;
using cordmux::ws::FrameType;
using cordmux::ws::decode_frame_header;
using cordmux::ws::encode_frame;
using cordmux::ws::frame_payload;
using cordmux::ws::frame_payload_size;
using cordmux::ws::kFlagKeyframe;
using cordmux::ws::kFlagRecordingOnly;
using cordmux::ws::kHeaderSize;

TEST_CASE("キーフレームの映像のヘッダーとペイロードは、encode_frame で作って decode_frame_header で読むと元に戻る")
{
	FrameHeader h;
	h.type = FrameType::Video;
	h.stream_id = 7;
	h.timestamp_us = 123456789012345ull;
	h.flags = kFlagKeyframe;

	const uint8_t payload[] = {1, 2, 3, 4, 5};
	const auto msg = encode_frame(h, payload, sizeof(payload));
	REQUIRE(msg.size() == kHeaderSize + sizeof(payload));

	const auto decoded = decode_frame_header(msg.data(), msg.size());
	REQUIRE(decoded.has_value());
	CHECK(decoded->type == FrameType::Video);
	CHECK(decoded->stream_id == 7);
	CHECK(decoded->timestamp_us == 123456789012345ull);
	CHECK(decoded->flags == kFlagKeyframe);
	CHECK(decoded->is_keyframe());
	CHECK_FALSE(decoded->is_recording_only());

	CHECK(frame_payload_size(msg.size()) == sizeof(payload));
	const uint8_t *body = frame_payload(msg.data());
	for (size_t i = 0; i < sizeof(payload); i++)
		CHECK(body[i] == payload[i]);
}

TEST_CASE("録画専用のフラグが立ったペイロードのない音声のヘッダーも、encode_frame と decode_frame_header で元に戻る")
{
	FrameHeader h;
	h.type = FrameType::Audio;
	h.stream_id = 0xFFFFFFFFu;
	h.timestamp_us = 0;
	h.flags = kFlagRecordingOnly;

	const auto msg = encode_frame(h, nullptr, 0);
	REQUIRE(msg.size() == kHeaderSize);

	const auto decoded = decode_frame_header(msg.data(), msg.size());
	REQUIRE(decoded.has_value());
	CHECK(decoded->type == FrameType::Audio);
	CHECK(decoded->stream_id == 0xFFFFFFFFu);
	CHECK(decoded->is_recording_only());
	CHECK_FALSE(decoded->is_keyframe());
	CHECK(frame_payload_size(msg.size()) == 0);
}

TEST_CASE("14 バイトに満たないメッセージには、decode_frame_header が nullopt を返す")
{
	std::vector<uint8_t> msg(kHeaderSize - 1, 0);
	msg[0] = uint8_t(FrameType::Video);
	CHECK_FALSE(decode_frame_header(msg.data(), msg.size()).has_value());
	CHECK_FALSE(decode_frame_header(nullptr, 0).has_value());
	CHECK_FALSE(decode_frame_header(msg.data(), 0).has_value());
}

TEST_CASE("type が 0 か 3 以上のメッセージには、decode_frame_header が nullopt を返す")
{
	for (const uint8_t bad_type : {uint8_t(0), uint8_t(3), uint8_t(4), uint8_t(255)}) {
		FrameHeader h;
		h.type = FrameType::Video; // 後で上書きする
		const auto msg = encode_frame(h, nullptr, 0);
		std::vector<uint8_t> mutated(msg);
		mutated[0] = bad_type;
		INFO("bad_type = ", int(bad_type));
		CHECK_FALSE(decode_frame_header(mutated.data(), mutated.size()).has_value());
	}
}

TEST_CASE("flags の bit2 から bit7 が 1 つでも立っていれば nullopt を返し、bit0 と bit1 だけなら読める")
{
	for (int bit = 2; bit <= 7; bit++) {
		FrameHeader h;
		h.type = FrameType::Video;
		h.flags = uint8_t(1u << bit);
		const auto msg = encode_frame(h, nullptr, 0);
		INFO("bit = ", bit);
		CHECK_FALSE(decode_frame_header(msg.data(), msg.size()).has_value());
	}
	// bit0 (キーフレーム) と bit1 (録画専用) は、片方だけでも両方でも読める。
	for (uint8_t flags : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x02), uint8_t(0x03)}) {
		FrameHeader h;
		h.type = FrameType::Video;
		h.flags = flags;
		const auto msg = encode_frame(h, nullptr, 0);
		const auto decoded = decode_frame_header(msg.data(), msg.size());
		INFO("flags = ", int(flags));
		REQUIRE(decoded.has_value());
		CHECK(decoded->flags == flags);
	}
}

TEST_CASE("stream_id と timestamp_us を、決まったオフセットからリトルエンディアンで読む")
{
	std::vector<uint8_t> msg(kHeaderSize, 0);
	msg[0] = uint8_t(FrameType::Video);
	// stream_id = 0x04030201 (オフセット 1-4)
	msg[1] = 0x01;
	msg[2] = 0x02;
	msg[3] = 0x03;
	msg[4] = 0x04;
	// timestamp_us = 0x0807060504030201 (オフセット 5-12)
	for (int i = 0; i < 8; i++)
		msg[5 + i] = uint8_t(0x01 + i);
	msg[13] = 0;

	const auto decoded = decode_frame_header(msg.data(), msg.size());
	REQUIRE(decoded.has_value());
	CHECK(decoded->stream_id == 0x04030201u);
	CHECK(decoded->timestamp_us == 0x0807060504030201ull);
}

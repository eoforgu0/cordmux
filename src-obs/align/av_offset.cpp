// SPDX-License-Identifier: Apache-2.0

// align/av_offset.h の実装。

#include "align/av_offset.h"

namespace cordmux::align {

void AvOffsetTranscriber::reset()
{
	stats_ = AvOffsetStats{};
	has_video_ = false;
	offset_us_ = 0;
	has_last_audio_ = false;
	last_audio_out_us_ = 0;
}

void AvOffsetTranscriber::note_video(int64_t sender_ts_us, int64_t out_ts_us)
{
	const int64_t offset = out_ts_us - sender_ts_us;
	if (!has_video_ || offset != offset_us_)
		stats_.offset_changes++;
	has_video_ = true;
	offset_us_ = offset;
	stats_.video_frames++;
	stats_.video_offset_us = offset;
}

int64_t AvOffsetTranscriber::map_audio(int64_t audio_sender_ts_us)
{
	stats_.audio_frames++;

	int64_t out = audio_sender_ts_us;
	if (!has_video_) {
		// 最初の映像より前は、足すオフセットがまだないのでそのまま返す。
		// 合わせる相手の映像がまだないので、constant_offset_us も足さない。
		stats_.passthrough_audio_frames++;
	} else {
		// 映像に付いたのと同じオフセットと、constant_offset_us を足す。
		out = audio_sender_ts_us + offset_us_ + cfg_.constant_offset_us;
	}

	if (cfg_.enforce_monotonic_audio && has_last_audio_ && out <= last_audio_out_us_) {
		out = last_audio_out_us_ + cfg_.min_audio_step_us;
		stats_.monotonic_fix_count++;
	}
	has_last_audio_ = true;
	last_audio_out_us_ = out;
	return out;
}

} // namespace cordmux::align

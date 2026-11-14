// SPDX-License-Identifier: Apache-2.0

// 合成のソースの子を空けるとき (sources/frame_sink.h の FrameSink) の、空ける時機を決める純粋関数。空けるのは OBS の tick の
// コールバックの中だけで行い、すぐ空ける要求も次の tick まで待つ。libobs にも Qt にも依存しない。

#pragma once

#include <algorithm>
#include <cstdint>

namespace cordmux::sources {

// 空ける予約の期限の値。0 は予約なし、kClearAtNextTick は次の tick で空ける (すぐ空ける要求)。それより大きい値は、その
// 時刻 (os_gettime_ns) を過ぎた最初の tick で空ける (保ってから空ける予約)。
inline constexpr std::uint64_t kNoClear = 0;
inline constexpr std::uint64_t kClearAtNextTick = 1;

// tick で空けるか。予約があり、まだ空いておらず、期限を過ぎていれば真。
inline bool frame_sink_clear_due(std::uint64_t deadline_ns, bool cleared, std::uint64_t now_ns)
{
	return deadline_ns != kNoClear && !cleared && now_ns >= deadline_ns;
}

// 保ってから空ける予約を置くときの新しい期限。すぐ空ける要求が既にあれば、それを保つ (後から来た保つ予約で、空けるのを
// 遅らせない)。それ以外は now_ns + hold_ns (0 と kClearAtNextTick にはしない)。
inline std::uint64_t frame_sink_hold_deadline(std::uint64_t current_deadline_ns, std::uint64_t now_ns, std::uint64_t hold_ns)
{
	if (current_deadline_ns == kClearAtNextTick)
		return kClearAtNextTick;
	return std::max<std::uint64_t>(kClearAtNextTick + 1, now_ns + hold_ns);
}

} // namespace cordmux::sources

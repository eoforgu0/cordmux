// SPDX-License-Identifier: Apache-2.0

// 設定 record_with_stream (配信の開始で cordmux の録画を始め、終了で録画を終える) の判断と、録画の経過時間の文字列。
// OBS の配信の開始と終了のイベントを受けたときに、録画の操作を出すかを決める純粋関数で、libobs にも Qt にも依存しない。
// 録画の開始と終了は、押されたボタンと同じ操作 (ActionId::ToggleRecording を ledger::apply に通す経路) で行う。

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace cordmux::ledger {

// OBS の配信に起きたこと (obs_frontend_event の STREAMING_STARTED と STREAMING_STOPPED)。
enum class StreamEvent { Started, Stopped };

// イベントのときに録画へ出す操作。
enum class RecordFollow { None, Start, Stop };

// 設定がオフなら何もしない (配信の開始と終了で録画を変えない)。オンのとき、配信の開始では録画していなければ始め、
// 配信の終了では録画していれば、誰が始めた録画でも終える。設定を途中でオンにしても、その時点では何も起きず、次の
// イベントから働く (この関数はイベントのときにしか呼ばれない)。
inline RecordFollow record_follow_of(bool record_with_stream, StreamEvent event, bool recording_now)
{
	if (!record_with_stream)
		return RecordFollow::None;
	if (event == StreamEvent::Started)
		return recording_now ? RecordFollow::None : RecordFollow::Start;
	return recording_now ? RecordFollow::Stop : RecordFollow::None;
}

// 録画のボタンに出す経過時間。1 時間までは「12:34」、それより長ければ「1:02:34」(OBS のステータスバーの録画の時間に
// 近い形)。elapsed_us はマイクロ秒で、負の値 (システム時刻が戻ったとき) は 0 として扱う。
inline std::string record_elapsed_text(std::int64_t elapsed_us)
{
	const std::int64_t total = elapsed_us > 0 ? elapsed_us / 1'000'000 : 0;
	const long long hours = total / 3600;
	const int minutes = static_cast<int>(total / 60 % 60);
	const int seconds = static_cast<int>(total % 60);
	char text[32];
	if (hours > 0)
		std::snprintf(text, sizeof(text), "%lld:%02d:%02d", hours, minutes, seconds);
	else
		std::snprintf(text, sizeof(text), "%02d:%02d", minutes, seconds);
	return text;
}

// 録画のボタンの幅を決めるときに使う、いちばん広い経過時間。数字の幅はフォントで違うので、広い 8 で測る。
inline constexpr const char *kRecordElapsedWidest = "8:88:88";

} // namespace cordmux::ledger

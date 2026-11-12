// SPDX-License-Identifier: Apache-2.0

// ログの行を決まった数まで保持するリングバッファー。Pipeline は 4 つ持つ。Vesktop の送信側から届いたログの行と、
// cordmux.dll 自身のログの行を、それぞれ出来事の行 (診断情報の discord.log と plugin.log) と統計の行
// (discord-stats.log と plugin-stats.log) に分ける。行の数の上限は settings の js_event_log_lines、
// plugin_event_log_lines、js_log_ring_lines、plugin_log_ring_lines。
// 行の中身はチェックしない。トークンとポート番号を含む行を積まないのは呼び出し元の役目である。
// libobs にも Qt にも依存しない。ロックは持たず、呼び出し元のロックの中で使う。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace cordmux::pipeline {

// plugin.log と plugin-stats.log の行の先頭に付ける時刻「HH:MM:SS.mmm: 」(ローカル時刻) を作る。時計を読むのは呼び出し元
// (Pipeline::push_plugin_log) で、ここは整形だけをする。
// 10 秒ごとの統計の行が 200 行のリングバッファーを数分で入れ替えるので、残った行がいつのものかを読めるように
// 時刻を付ける。形は OBS のログの行の先頭と同じにした。OBS 31.1.1 の CurrentTimeString と LogStringChunk
// (https://github.com/obsproject/obs-studio/blob/31.1.1/frontend/obs-main.cpp) は、ローカル時刻を "%T" と
// ミリ秒 3 桁で書き、": " を続ける。日付付きや UTC にすると、OBS のログと並べて読むときに読み替えが必要になる。
// test/tools/notice-count もこの形で読む。範囲の外の値は端の値にそろえる (文字の幅を変えない)。
inline std::string log_clock_prefix(int hour, int minute, int second, int millisecond)
{
	const auto clamp = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d: ", clamp(hour, 23), clamp(minute, 59), clamp(second, 60),
		      clamp(millisecond, 999));
	return std::string(buf);
}

class LogRing {
public:
	explicit LogRing(std::size_t capacity = 0) : capacity_(capacity) {}

	// 1 行積む。上限を超えたら古い行から捨てて数える (後から読みたいのは直前の行なので、新しい行は捨てない)。
	// 上限が 0 なら 1 行も持たない。
	void push(std::string line)
	{
		if (capacity_ == 0) {
			dropped_++;
			return;
		}
		while (lines_.size() >= capacity_) {
			lines_.pop_front();
			dropped_++;
		}
		lines_.push_back(std::move(line));
	}

	// 古い順のコピー。
	std::vector<std::string> snapshot() const { return std::vector<std::string>(lines_.begin(), lines_.end()); }

	std::size_t size() const { return lines_.size(); }
	std::size_t capacity() const { return capacity_; }

	// 上限を超えて捨てた行の総数 (減らない)。
	std::uint64_t dropped() const { return dropped_; }

	bool empty() const { return lines_.empty(); }

	// 行だけを捨てる。捨てた行の総数は残す (ログに出した数を巻き戻さない)。
	void clear() { lines_.clear(); }

private:
	std::size_t capacity_;
	std::deque<std::string> lines_;
	std::uint64_t dropped_ = 0;
};

} // namespace cordmux::pipeline

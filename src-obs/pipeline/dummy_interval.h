// SPDX-License-Identifier: Apache-2.0

// ダミーを表示していた時間帯の一覧を書き留める純粋関数。一覧は診断情報と録画ログに入る。
// 「いまダミーか」だけでは、後から診断情報を読んだときに、ある時刻の映像がダミーだったかを言えないので、
// 時間帯で持つ。libobs にも Qt にも依存しない。ロックは持たず、呼び出し元のロックの中で使う。

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cordmux::pipeline {

// ダミーを表示していた 1 つの時間帯 (どちらもエポックからのマイクロ秒)。
// ended_at_us == 0 は「まだ続いている」を表す (1970 年に終わったという意味ではない)。表示している最中にも
// 診断情報に入るので、終わりのない時間帯を表せる必要がある。
struct DummyInterval {
	std::int64_t started_at_us = 0;
	std::int64_t ended_at_us = 0;

	bool open() const { return ended_at_us == 0; }
};

// 一覧に持てる時間帯の数の上限。目安の値で、測って決めたものではない。1 回の配信でダミーを切り替える
// 回数 (数回) より十分大きく、診断情報に入れても読める大きさにした。
inline constexpr std::size_t kDummyIntervalMax = 200;

// ダミーの表示の切り替えを 1 回書き留める。同じ値を続けて渡しても一覧は変わらない
// (sources::set_dummy_mode は同じ値で何度呼んでもよいので、こちらも合わせる)。
// 上限を超えたら古い時間帯から捨てる (後から読みたいのは直前の時間帯である)。
// 一覧が変わったら true を返す (呼び出し元はそのときだけログに 1 行書く)。intervals が nullptr なら false を返す。
inline bool note_dummy_interval(std::vector<DummyInterval> *intervals, bool on, std::int64_t now_us,
				std::size_t max_intervals = kDummyIntervalMax)
{
	if (intervals == nullptr)
		return false;

	const bool currently_on = !intervals->empty() && intervals->back().open();
	if (on == currently_on)
		return false;

	if (on) {
		if (max_intervals == 0)
			return false; // 上限が 0 なら何も書き留めない
		DummyInterval started;
		started.started_at_us = now_us;
		started.ended_at_us = 0;
		intervals->push_back(started);
		while (intervals->size() > max_intervals)
			intervals->erase(intervals->begin());
		return true;
	}

	intervals->back().ended_at_us = now_us;
	return true;
}

// いまダミーを表示している時間帯が続いているか。別のフラグを持たずに、一覧から読む。
inline bool dummy_interval_open(const std::vector<DummyInterval> &intervals)
{
	return !intervals.empty() && intervals.back().open();
}

} // namespace cordmux::pipeline

// SPDX-License-Identifier: Apache-2.0

// DockPoll の実装を分けた .cpp のうち、2 つ以上が使う関数を置く。1 つの .cpp だけが使う関数は、その .cpp の無名の
// 名前空間に置く。Qt (QDateTime) を含む。

#pragma once

#include <cstdint>

#include <QDateTime>

namespace cordmux::dock::detail {

// 現在のシステム時刻 (Unix エポックからのマイクロ秒)。UTC からの時差 (DockPoll::current_facts) と同じく Qt から取得し、
// <ctime> は使わない。ledger/ は時刻を読まないので、ドックが読んで渡す。
inline std::int64_t wall_now_us()
{
	return QDateTime::currentMSecsSinceEpoch() * 1000;
}

} // namespace cordmux::dock::detail

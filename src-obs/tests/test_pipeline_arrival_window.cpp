// SPDX-License-Identifier: Apache-2.0

// pipeline/arrival_window.h の単体テスト。遅れの列から、X ごとの遅れたフレームの数と率、続けて遅れたまとまりの数、
// 平均、最大、分位点を求める関数を確認する。

#include "doctest.h"

#include "pipeline/arrival_window.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using cordmux::pipeline::ArrivalWindow;
using cordmux::pipeline::LateCount;
using cordmux::pipeline::delay_quantile_ms;
using cordmux::pipeline::frame_count;
using cordmux::pipeline::late_count;
using cordmux::pipeline::max_delay_ms;
using cordmux::pipeline::mean_delay_ms;

namespace {

ArrivalWindow window_of(std::vector<std::uint16_t> delays)
{
	ArrivalWindow w;
	w.delays_ms = std::move(delays);
	double sum = 0.0;
	for (const std::uint16_t d : w.delays_ms) {
		sum += d;
		w.max_ms = std::max<double>(w.max_ms, d);
	}
	w.mean_ms = w.delays_ms.empty() ? 0.0 : sum / static_cast<double>(w.delays_ms.size());
	return w;
}

} // namespace

TEST_CASE("X ごとの遅れたフレームの数と率は、遅れが X ms 以上のフレーム")
{
	const ArrivalWindow w = window_of({10, 50, 99, 100, 101, 250, 30, 100});
	LateCount c = late_count(w, 100);
	CHECK(c.frames == 8);
	CHECK(c.late == 4); // 100、101、250、100 (ちょうど X は間に合わない)
	CHECK(c.fraction() == doctest::Approx(0.5));
	CHECK(late_count(w, 101).late == 2);
	CHECK(late_count(w, 251).late == 0);
	CHECK(late_count(w, 0).late == 8); // X が 0 なら全部
	CHECK(late_count(w, -5).late == 8);
	CHECK(late_count(w, 1000).late == 0);
}

TEST_CASE("続けて遅れたフレームは X ごとに 1 つのまとまりと数える")
{
	//             0   1    2   3    4    5   6    7   8
	const ArrivalWindow w = window_of({10, 120, 130, 20, 200, 10, 300, 310, 320});
	CHECK(late_count(w, 100).runs == 3);
	CHECK(late_count(w, 100).late == 6);
	// X を上げるとまとまりは減り、分かれもする: X=125 では 130 / 200 / 300 310 320 の 3 つ。
	CHECK(late_count(w, 125).runs == 3);
	CHECK(late_count(w, 250).runs == 1);
	CHECK(late_count(w, 330).runs == 0);
	// 全部が遅れるなら 1 つのまとまり、列の先頭が遅れでも数える。
	CHECK(late_count(w, 0).runs == 1);
	CHECK(late_count(window_of({500, 10}), 100).runs == 1);
}

TEST_CASE("空の列は全部 0")
{
	const ArrivalWindow w;
	const LateCount c = late_count(w, 100);
	CHECK(c.frames == 0);
	CHECK(c.late == 0);
	CHECK(c.runs == 0);
	CHECK(c.fraction() == 0.0);
	const std::vector<ArrivalWindow> none;
	CHECK(mean_delay_ms(none) == 0.0);
	CHECK(max_delay_ms(none) == 0.0);
	CHECK(delay_quantile_ms(none, 95) == 0);
}

TEST_CASE("複数のストリームは数を足し、まとまりはストリームをまたいで続けない")
{
	const std::vector<ArrivalWindow> ws = {window_of({200, 10, 10}), window_of({300, 300, 10, 20})};
	const LateCount c = late_count(ws, 100);
	CHECK(c.frames == 7);
	CHECK(c.late == 3);
	CHECK(c.runs == 2); // 先頭のストリームの末尾が遅れでも、次のストリームの先頭と続かない
	CHECK(frame_count(ws) == 7);
	const std::vector<ArrivalWindow> tail_head = {window_of({10, 200}), window_of({300, 10})};
	CHECK(late_count(tail_head, 100).runs == 2);
	CHECK(mean_delay_ms(ws) == doctest::Approx((220.0 + 630.0) / 7.0));
	CHECK(max_delay_ms(ws) == doctest::Approx(300.0));
}

TEST_CASE("平均と最大は丸める前の値で、分位点は順位の方法")
{
	ArrivalWindow w = window_of({10, 20, 30, 40});
	w.max_ms = 41.5; // 列は切り捨てた値で、最大は実際の値
	const std::vector<ArrivalWindow> ws = {w};
	CHECK(max_delay_ms(ws) == doctest::Approx(41.5));
	CHECK(mean_delay_ms(ws) == doctest::Approx(25.0));
	std::vector<std::uint16_t> hundred;
	for (int i = 1; i <= 100; ++i)
		hundred.push_back(static_cast<std::uint16_t>(i));
	const std::vector<ArrivalWindow> h = {window_of(hundred)};
	CHECK(delay_quantile_ms(h, 50) == 50);
	CHECK(delay_quantile_ms(h, 95) == 95);
	CHECK(delay_quantile_ms(h, 99) == 99);
	CHECK(delay_quantile_ms(h, 100) == 100);
	CHECK(delay_quantile_ms(h, 0) == 1);
	// 分位点の D にすると、その率までが間に合う (遅れ >= D が遅れなので、1 ms 足す)。
	CHECK(late_count(h, delay_quantile_ms(h, 95) + 1).fraction() == doctest::Approx(0.05));
}

// SPDX-License-Identifier: Apache-2.0

// record/record_clock.h の単体テスト。タイムスタンプが戻ったときの 3 段階の扱いを確認する。
// 境界は両側から確認する。ちょうど 1 フレームの間隔 (Pushed と Reanchored の境) と、ちょうど 1 秒 (Reanchored と
// Jumped の境) を、1 us 手前の値と、その値そのもので確認する。

#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "record/record_clock.h"

using cordmux::record::advance;
using cordmux::record::backward_is_periodic;
using cordmux::record::classify_pts;
using cordmux::record::ClockDecision;
using cordmux::record::ClockState;
using cordmux::record::ClockStep;
using cordmux::record::ClockThresholds;
using cordmux::record::PtsReanchor;

namespace {

// 60 fps のフレームの間隔 (us)。16666.66... を整数に丸めた値で、しきい値の境そのものになる。
constexpr std::int64_t kPeriod60 = 16667;

ClockState started_state(std::int64_t last_out_pts_us, std::int64_t frame_period_us = kPeriod60)
{
	ClockState state;
	state.last_out_pts_us = last_out_pts_us;
	// まだ 1 度もずらしていないストリーム (ずれは 0) を作る。段階は届いたタイムスタンプどうしの差で決まるので、
	// 届いた値も同じ値から始める (record_clock.h の classify_pts)。
	state.last_in_pts_us = last_out_pts_us;
	state.offset_us = 0;
	state.frame_period_us = frame_period_us;
	state.started = true;
	return state;
}

// 並びをそのまま流して、段階ごとの件数と最後の状態を見る (1 回だけ呼ぶテストでは、1 回の後戻りがセグメントの残りの
// 全部に伝わる形が見えない)。
struct RunResult {
	int pushed = 0;
	int reanchored = 0;
	int jumped = 0;
	int forward = 0;
	std::vector<PtsReanchor> reanchors;
	std::vector<std::int64_t> out;
};

RunResult run_sequence(const std::vector<std::int64_t> &incoming, std::int64_t frame_period_us = kPeriod60,
		       ClockThresholds thresholds = ClockThresholds{})
{
	RunResult result;
	ClockState state;
	state.frame_period_us = frame_period_us;
	for (const std::int64_t pts : incoming) {
		const ClockDecision decision = classify_pts(state, pts, thresholds);
		switch (decision.step) {
		case ClockStep::Forward:
			++result.forward;
			break;
		case ClockStep::Pushed:
			++result.pushed;
			break;
		case ClockStep::Reanchored: {
			++result.reanchored;
			PtsReanchor one;
			one.delta_us = decision.backward_us;
			one.at_pts_us = pts;
			result.reanchors.push_back(one);
			break;
		}
		case ClockStep::Jumped:
			++result.jumped;
			break;
		}
		advance(state, decision);
		result.out.push_back(state.last_out_pts_us);
	}
	return result;
}

bool strictly_increasing(const std::vector<std::int64_t> &values)
{
	for (std::size_t i = 1; i < values.size(); ++i) {
		if (values[i] <= values[i - 1])
			return false;
	}
	return true;
}

} // namespace

TEST_CASE("セグメントの最初のフレームは、ずらさずにそのまま受け取る")
{
	ClockState state;
	state.frame_period_us = kPeriod60;
	const ClockDecision decision = classify_pts(state, 1'234'567, ClockThresholds{});
	CHECK(decision.step == ClockStep::Forward);
	CHECK(decision.out_pts_us == 1'234'567);
	CHECK(decision.backward_us == 0);
}

TEST_CASE("通常どおり進んだフレームは、タイムスタンプをそのまま書く")
{
	const ClockState state = started_state(1'000'000);
	const ClockDecision decision = classify_pts(state, 1'000'000 + kPeriod60, ClockThresholds{});
	CHECK(decision.step == ClockStep::Forward);
	CHECK(decision.out_pts_us == 1'000'000 + kPeriod60);
}

TEST_CASE("1 フレームの間隔より 1 us 小さく戻ったフレームはずらす (Pushed)")
{
	const ClockState state = started_state(1'000'000);
	const ClockDecision decision = classify_pts(state, 1'000'000 - (kPeriod60 - 1), ClockThresholds{});
	CHECK(decision.step == ClockStep::Pushed);
	CHECK(decision.out_pts_us == 1'000'000 + kPeriod60);
	CHECK(decision.backward_us == kPeriod60 - 1);
}

TEST_CASE("ちょうど 1 フレームの間隔だけ戻ったフレームは、原点の置き直しになる (Reanchored)")
{
	const ClockState state = started_state(1'000'000);
	const ClockDecision decision = classify_pts(state, 1'000'000 - kPeriod60, ClockThresholds{});
	CHECK(decision.step == ClockStep::Reanchored);
	CHECK(decision.out_pts_us == 1'000'000 + kPeriod60); // ずらす先は Pushed と同じ
	CHECK(decision.backward_us == kPeriod60);
}

TEST_CASE("同じタイムスタンプが 2 つ続いたら、重ならないようにずらす")
{
	const ClockState state = started_state(1'000'000);
	const ClockDecision decision = classify_pts(state, 1'000'000, ClockThresholds{});
	CHECK(decision.step == ClockStep::Pushed);
	CHECK(decision.out_pts_us == 1'000'000 + kPeriod60);
	CHECK(decision.backward_us == 0);
}

TEST_CASE("1 秒より 1 us 小さい後戻りは原点の置き直し、ちょうど 1 秒の後戻りは不連続になる")
{
	const ClockState state = started_state(10'000'000);

	const ClockDecision near = classify_pts(state, 10'000'000 - 999'999, ClockThresholds{});
	CHECK(near.step == ClockStep::Reanchored);
	CHECK(near.backward_us == 999'999);

	const ClockDecision exact = classify_pts(state, 10'000'000 - 1'000'000, ClockThresholds{});
	CHECK(exact.step == ClockStep::Jumped);
	CHECK(exact.backward_us == 1'000'000);
	CHECK(exact.out_pts_us == 9'000'000); // 不連続ではずらさず、新しいセグメントの先頭になる
}

TEST_CASE("前に 1 秒より 1 us 小さく進むと欠落として扱い、ちょうど 1 秒進むとセグメントを閉じる")
{
	const ClockState state = started_state(10'000'000);

	const ClockDecision near = classify_pts(state, 10'000'000 + 999'999, ClockThresholds{});
	CHECK(near.step == ClockStep::Forward);
	CHECK(near.out_pts_us == 10'999'999);

	const ClockDecision exact = classify_pts(state, 10'000'000 + 1'000'000, ClockThresholds{});
	CHECK(exact.step == ClockStep::Jumped);
	CHECK(exact.out_pts_us == 11'000'000);
	CHECK(exact.backward_us == 0); // 前に進んだので、戻った量は 0
}

TEST_CASE("Pushed のしきい値は、フレームの間隔に record_clock_push_max_frames を掛けた値になる")
{
	const ClockState state = started_state(1'000'000);
	ClockThresholds thresholds;
	thresholds.push_max_frames = 2;

	// 2 フレームの間隔より 1 us 小さい後戻りまでが Pushed。
	const ClockDecision pushed = classify_pts(state, 1'000'000 - (2 * kPeriod60 - 1), thresholds);
	CHECK(pushed.step == ClockStep::Pushed);

	const ClockDecision reanchored = classify_pts(state, 1'000'000 - 2 * kPeriod60, thresholds);
	CHECK(reanchored.step == ClockStep::Reanchored);
}

TEST_CASE("Reanchored のしきい値も、設定の値で変わる (record_clock_reanchor_max_ms)")
{
	const ClockState state = started_state(10'000'000);
	ClockThresholds thresholds;
	thresholds.reanchor_max_ms = 200;

	CHECK(classify_pts(state, 10'000'000 - 199'999, thresholds).step == ClockStep::Reanchored);
	CHECK(classify_pts(state, 10'000'000 - 200'000, thresholds).step == ClockStep::Jumped);
}

TEST_CASE("30 fps でも、境はそのフレームの間隔で決まる")
{
	constexpr std::int64_t kPeriod30 = 33333;
	const ClockState state = started_state(5'000'000, kPeriod30);
	CHECK(classify_pts(state, 5'000'000 - (kPeriod30 - 1), ClockThresholds{}).step == ClockStep::Pushed);
	CHECK(classify_pts(state, 5'000'000 - kPeriod30, ClockThresholds{}).step == ClockStep::Reanchored);
	// 60 fps で不連続にならない量は、30 fps でも同じく Reanchored になる (この境は ms のまま)。
	CHECK(classify_pts(state, 5'000'000 - 999'999, ClockThresholds{}).step == ClockStep::Reanchored);
}

TEST_CASE("フレームの間隔が分からないストリームでも、1 us ずらして重なりを避ける")
{
	const ClockState state = started_state(1'000'000, 0);
	const ClockDecision decision = classify_pts(state, 1'000'000, ClockThresholds{});
	CHECK(decision.step == ClockStep::Pushed);
	CHECK(decision.out_pts_us == 1'000'001);
}

TEST_CASE("advance は、書いたタイムスタンプを状態に反映する")
{
	ClockState state;
	state.frame_period_us = kPeriod60;

	ClockDecision first = classify_pts(state, 500'000, ClockThresholds{});
	advance(state, first);
	CHECK(state.started);
	CHECK(state.last_out_pts_us == 500'000);

	// 後戻りをずらした後は、ずらした値が次の基準になる。
	ClockDecision second = classify_pts(state, 500'000 - 10, ClockThresholds{});
	advance(state, second);
	CHECK(state.last_out_pts_us == 500'000 + kPeriod60);

	// ずらした後に普通のフレームが来ても、単調に増える。
	ClockDecision third = classify_pts(state, 500'000 + 2 * kPeriod60, ClockThresholds{});
	CHECK(third.step == ClockStep::Forward);
	CHECK(third.out_pts_us > state.last_out_pts_us);
}

TEST_CASE("タイムスタンプが 1 つ重なっても、ずらすのはその 1 つだけで、後のフレームは Forward に戻る")
{
	// 0, P, P (重なり), 2P, 3P, 4P, 5P, 6P
	std::vector<std::int64_t> incoming = {0, kPeriod60, kPeriod60};
	for (int i = 2; i <= 6; ++i)
		incoming.push_back(i * kPeriod60);

	const RunResult result = run_sequence(incoming);
	CHECK(result.pushed == 1); // セグメントのフレーム数まで増えない
	CHECK(result.reanchored == 0);
	CHECK(result.jumped == 0);
	CHECK(result.forward == 7);
	CHECK(strictly_increasing(result.out)); // タイムスタンプは重ならない
}

TEST_CASE("送信側の通常の原点の置き直しは 1 回だけ数え、B フレームの疑いにしない")
{
	// 0, P, 2P, 3P までは正常。次の 1 つで 3 フレーム分戻り、その後はまた正常に進む。
	std::vector<std::int64_t> incoming = {0, kPeriod60, 2 * kPeriod60, 3 * kPeriod60, 0};
	for (int i = 1; i <= 8; ++i)
		incoming.push_back(i * kPeriod60);

	const RunResult result = run_sequence(incoming);
	CHECK(result.reanchored == 1);
	CHECK(result.pushed == 0);
	CHECK(result.reanchors.size() == 1);
	CHECK(result.reanchors.front().delta_us == 3 * kPeriod60);
	// 1 回だけの置き直しは規則的な後戻りではない (真なら、ProRes で録画ファイルを作成することになる)。
	CHECK_FALSE(backward_is_periodic(result.reanchors, kPeriod60));
	CHECK(strictly_increasing(result.out));
}

TEST_CASE("B フレームの並びは、規則的な後戻りとして残る")
{
	// 表示の順とエンコードの順が入れ替わる並び (2 フレーム分の後戻りが規則的に続く)。
	const std::int64_t p = kPeriod60;
	const std::vector<std::int64_t> incoming = {0, 3 * p, p, 2 * p, 6 * p, 4 * p, 5 * p, 9 * p, 7 * p, 8 * p};

	const RunResult result = run_sequence(incoming);
	CHECK(result.reanchored == 3);
	CHECK(backward_is_periodic(result.reanchors, kPeriod60));
	CHECK(strictly_increasing(result.out));
}

TEST_CASE("フレームの間隔の整数倍の後戻りが 3 回続いたら、規則的な後戻りとみなす")
{
	std::vector<PtsReanchor> reanchors;
	for (int i = 0; i < 3; ++i) {
		PtsReanchor one;
		one.delta_us = 2 * kPeriod60;
		one.at_pts_us = 1'000'000 * (i + 1);
		reanchors.push_back(one);
	}
	CHECK(backward_is_periodic(reanchors, kPeriod60));
}

TEST_CASE("倍数がばらばらでも、どれも整数倍なら規則的な後戻りとみなす (B ピラミッド)")
{
	std::vector<PtsReanchor> reanchors;
	const std::int64_t multiples[] = {1, 3, 1, 3};
	for (const std::int64_t multiple : multiples) {
		PtsReanchor one;
		one.delta_us = multiple * kPeriod60;
		reanchors.push_back(one);
	}
	CHECK(backward_is_periodic(reanchors, kPeriod60));
}

TEST_CASE("原点の置き直しが 2 回までなら、規則的な後戻りとみなさない")
{
	std::vector<PtsReanchor> reanchors;
	for (int i = 0; i < 2; ++i) {
		PtsReanchor one;
		one.delta_us = kPeriod60;
		reanchors.push_back(one);
	}
	CHECK_FALSE(backward_is_periodic(reanchors, kPeriod60));
	CHECK_FALSE(backward_is_periodic({}, kPeriod60));
}

TEST_CASE("整数倍から外れた後戻りは、規則的な後戻りとみなさない (clock_map の置き直しの形)")
{
	std::vector<PtsReanchor> reanchors;
	// src-vencord/tests/encode_session.test.ts が確認している -33334 us のような値は、60 fps のフレームの間隔の
	// 2 倍に近いが、189000 us (src-vencord/tests/audio_chunk.test.ts) のような値は整数倍から外れる。
	const std::int64_t deltas[] = {189'000, 2 * kPeriod60, 3 * kPeriod60};
	for (const std::int64_t delta : deltas) {
		PtsReanchor one;
		one.delta_us = delta;
		reanchors.push_back(one);
	}
	CHECK_FALSE(backward_is_periodic(reanchors, kPeriod60));
}

TEST_CASE("送信側のタイムスタンプがいったん下がって後で戻っても、ずらしたずれを残さない")
{
	// 30 fps のストリームで、録画を ON にした時点で送信側のタイムスタンプが 78 ms 下がり、後で元に戻った形。実際に取り込んだ時刻は t_k = k * フレームの間隔で、
	// 届いたタイムスタンプは k = 3..10 の間だけ 78 ms 早い。
	constexpr std::int64_t kPeriod30 = 33333;
	constexpr std::int64_t kDrop = 78'000;
	std::vector<std::int64_t> truth;
	std::vector<std::int64_t> incoming;
	for (int k = 0; k <= 14; ++k) {
		const std::int64_t t = k * kPeriod30;
		truth.push_back(t);
		incoming.push_back(k >= 3 && k <= 10 ? t - kDrop : t);
	}

	const RunResult result = run_sequence(incoming, kPeriod30);
	CHECK(result.reanchored == 1); // 下がった瞬間の 1 回だけ
	CHECK(result.jumped == 0);
	CHECK(strictly_increasing(result.out));
	// 下がっている間は、ずらしたずれが実際の時刻を保ち、戻った後はずれを戻してタイムスタンプのとおりに書く
	// (戻さないと、k = 11 以降が 78 ms 遅れたまま、セグメントの終わりまで残る)。
	REQUIRE(result.out.size() == truth.size());
	for (std::size_t k = 0; k < truth.size(); ++k)
		CHECK(result.out[k] == truth[k]);
}

TEST_CASE("ずれを戻すのはフレームの間隔を超えて進んだ分だけで、出力の間隔はフレームの間隔より詰めない")
{
	constexpr std::int64_t kPeriod30 = 33333;
	ClockState state = started_state(1'000'000, kPeriod30);
	state.last_out_pts_us = 1'078'000; // 前に 78 ms ずらしたストリーム
	state.offset_us = 78'000;

	// 普通の間隔 (ちょうど 1 フレーム) では戻さない。
	const ClockDecision normal = classify_pts(state, 1'000'000 + kPeriod30, ClockThresholds{});
	CHECK(normal.step == ClockStep::Forward);
	CHECK(normal.offset_us == 78'000);
	CHECK(normal.out_pts_us == 1'078'000 + kPeriod30);

	// 2 フレーム進んだ (1 つ欠けた): 超えた 1 フレーム分だけ戻し、出力の間隔は 1 フレームになる。
	const ClockDecision skipped = classify_pts(state, 1'000'000 + 2 * kPeriod30, ClockThresholds{});
	CHECK(skipped.step == ClockStep::Forward);
	CHECK(skipped.offset_us == 78'000 - kPeriod30);
	CHECK(skipped.out_pts_us == 1'078'000 + kPeriod30);

	// 大きく進んだ: ずれより多くは戻さない (届いた値より前には書かない)。
	const ClockDecision far = classify_pts(state, 1'500'000, ClockThresholds{});
	CHECK(far.offset_us == 0);
	CHECK(far.out_pts_us == 1'500'000);
}

TEST_CASE("フレームの間隔が分からないストリームでは、ずらしたずれを戻さない")
{
	// 通常の間隔と欠落を見分けられない。戻すと、フレームの間隔を 1 us まで詰めてしまう。
	ClockState state = started_state(1'000'000, 0);
	state.last_out_pts_us = 1'005'000;
	state.offset_us = 5'000;
	const ClockDecision decision = classify_pts(state, 1'050'000, ClockThresholds{});
	CHECK(decision.step == ClockStep::Forward);
	CHECK(decision.offset_us == 5'000);
	CHECK(decision.out_pts_us == 1'055'000);
}

TEST_CASE("すべてのストリームに同時に起きた不連続は、時計の側で起きたとみなす")
{
	std::vector<cordmux::record::JumpObservation> jumps;
	jumps.push_back({1, 10'000'000});
	jumps.push_back({2, 10'020'000});
	jumps.push_back({3, 10'050'000});
	CHECK(cordmux::record::jump_is_global(jumps, 3));
	// 受信中のストリームが 4 つなら、3 つだけでは、すべてにならない。
	CHECK_FALSE(cordmux::record::jump_is_global(jumps, 4));
}

TEST_CASE("1 つのストリームだけの不連続 (途切れや再接続) は、時計の側で起きたとみなさない")
{
	std::vector<cordmux::record::JumpObservation> jumps;
	jumps.push_back({1, 10'000'000});
	CHECK_FALSE(cordmux::record::jump_is_global(jumps, 3));

	// 同じストリームで何度も不連続が起きても、すべてのストリームにはならない。
	jumps.push_back({1, 10'010'000});
	jumps.push_back({1, 10'020'000});
	CHECK_FALSE(cordmux::record::jump_is_global(jumps, 3));
}

TEST_CASE("離れた時刻に起きた不連続は、同時とみなさない")
{
	std::vector<cordmux::record::JumpObservation> jumps;
	jumps.push_back({1, 10'000'000});
	jumps.push_back({2, 11'000'000});
	jumps.push_back({3, 12'000'000});
	CHECK_FALSE(cordmux::record::jump_is_global(jumps, 3));
	// 幅を広げれば同時とみなせる (幅は設定ではなく、呼び出し元が決める)。
	CHECK(cordmux::record::jump_is_global(jumps, 3, 2'500'000));
}

TEST_CASE("受信中のストリームが 1 つだけなら、同時かどうかは判断しない")
{
	std::vector<cordmux::record::JumpObservation> jumps;
	jumps.push_back({1, 10'000'000});
	CHECK_FALSE(cordmux::record::jump_is_global(jumps, 1));
	CHECK_FALSE(cordmux::record::jump_is_global({}, 0));
}

TEST_CASE("フレームの間隔が分からなければ、規則的な後戻りとはみなさない")
{
	std::vector<PtsReanchor> reanchors(3);
	for (PtsReanchor &one : reanchors)
		one.delta_us = kPeriod60;
	CHECK_FALSE(backward_is_periodic(reanchors, 0));
}

// SPDX-License-Identifier: Apache-2.0

// 録画のタイムスタンプの処理。送信側のタイムスタンプが戻ったときの扱いを 3 段階に分け (classify_pts)、セグメントに
// 書くタイムスタンプを作る。録画の時計には送信側のタイムスタンプだけを使い、到着の時刻は使わない。到着の時刻には
// エンコードと転送の遅延が加わるので、ストリームごとのばらつきが大きくなるためである。
// libobs、Qt、avformat に依存しない。

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "record/record_types.h"

namespace cordmux::record {

// 設定の record_clock_push_max_frames と record_clock_reanchor_max_ms のコピー。既定の値は config/settings.h と同じ
// 1 と 1000 である。
struct ClockThresholds {
	int push_max_frames = 1;
	int reanchor_max_ms = 1000;
};

// 1 つのストリーム (stream_id と user_id の組) の時計の状態。セグメントをまたいで持ち越さない (セグメントを閉じたら、
// 呼び出し元が started を false にしてやり直す)。
struct ClockState {
	std::int64_t last_out_pts_us = 0; // 直前に書いたタイムスタンプ (ずらした後の値)
	std::int64_t last_in_pts_us = 0;  // 直前に届いたタイムスタンプ (段階はこちらと比べて決める)
	std::int64_t offset_us = 0;       // 原点のずれ (書く値 = 届いた値 + この値)
	std::int64_t frame_period_us = 0; // configure の fps から求めたフレームの間隔 (0 は不明)
	bool started = false;             // まだ 1 つも受け取っていなければ false
};

struct ClockDecision {
	ClockStep step = ClockStep::Forward;
	std::int64_t out_pts_us = 0;  // 実際に書くタイムスタンプ
	std::int64_t backward_us = 0; // 戻った量 (正の値。前へ進んだときは 0)
	std::int64_t in_pts_us = 0;   // 届いたタイムスタンプそのもの (advance が状態にコピーする)
	std::int64_t offset_us = 0;   // このフレームを書いた後の原点のずれ (同上)
};

// 規則的な後戻り (B フレームの疑い) とみなす回数と、整数倍とみなす許容 (フレームの間隔の 1/4)。
// 2 回では、別々の置き直しが 2 回あったことと区別できないので、3 回以上にする。真になるとその人は ProRes で
// 録画ファイルを作成するだけなので、誤って真にしたときの害は小さい。そのため許容は広めに取る。
inline constexpr std::size_t kPeriodicBackwardMinCount = 3;
inline constexpr std::int64_t kPeriodicBackwardToleranceDiv = 4;

// Pushed と Reanchored の境 (この値より小さく戻ったら Pushed)。必ず 1 us 以上にする。フレームの間隔が分からない
// ストリームでも、同じタイムスタンプが 2 つ続いたときにずらせるようにするためである。
inline std::int64_t clock_push_limit_us(const ClockState &state, const ClockThresholds &thresholds)
{
	const int frames = thresholds.push_max_frames > 0 ? thresholds.push_max_frames : 1;
	const std::int64_t limit = state.frame_period_us > 0 ? state.frame_period_us * frames : 0;
	return limit > 0 ? limit : 1;
}

// Reanchored と Jumped の境 (この値以上戻るか、この値以上前へ進むと Jumped)。前へ進む側にも同じ値を使い、設定の
// キーを増やさない。前へ 1 秒進むのは 1 秒分のフレームが届かなかったときで、黒で埋める欠落そのものである。
// Pushed の境より小さい値が設定されても順序が崩れないように、必ず Pushed の境以上にする。
inline std::int64_t clock_jump_limit_us(const ClockState &state, const ClockThresholds &thresholds)
{
	const int ms = thresholds.reanchor_max_ms > 0 ? thresholds.reanchor_max_ms : 1;
	const std::int64_t limit = static_cast<std::int64_t>(ms) * 1000;
	const std::int64_t push = clock_push_limit_us(state, thresholds);
	return limit > push ? limit : push;
}

// ずらす量。フレームの間隔だけずらし、通常どおり届いた次のフレームと同じ位置にする。
// align/timestamp_aligner.h の min_output_step_us は 1 us だが、セグメントファイルの timescale は 60000 (1 単位が約
// 16.7 us) なので、1 us ずらしても同じ単位に丸められてタイムスタンプが重なり、muxer が重なったタイムスタンプを
// 自分でずらしてしまう。フレームの間隔が分からないとき (0) だけ 1 us にする。
inline std::int64_t clock_push_step_us(const ClockState &state)
{
	return state.frame_period_us > 0 ? state.frame_period_us : 1;
}

// 届いたタイムスタンプを、直前に届いた値との差で 3 段階に分ける。状態は変えない (変えるのは advance)。
//
//     戻った量                            ClockStep    録画ログに残すもの
//     ----------------------------------  -----------  -------------------------
//     1 フレームの間隔より小さい          Pushed       pts_pushed_count
//     1 フレームの間隔以上 1 秒未満       Reanchored   pts_reanchor (量と時刻)
//     1 秒以上戻ったか、1 秒以上進んだ    Jumped       close_reason = clock_jump
//
// 送信側は、換算の定数を下げたとき (src-vencord/encode/clock_map.ts) に、1 フレームの間隔より大きく 1 秒より小さい量だけ
// 戻ることがある。しきい値 1 つで分けると、この戻りをずらすだけで済ませるか、セグメントを
// 閉じるかのどちらかになって誤るので、3 段階に分ける。
// 比べるのは、書いた値 (ずらした後の値) ではなく、届いた値どうしである。書いた値と比べると、1 度ずらしただけで出力が
// 入力より 1 フレーム先に進んだまま戻らず、セグメントの残りのフレームが全部 Pushed か Reanchored になる。すると
// pts_reanchor がフレームの数だけ増え、通常の置き直し 1 回で backward_is_periodic が真になる (その人を ProRes で
// 録画ファイルを作成することになる)。書く値は「届いた値 + offset_us」で作り、ずらしたときだけ offset_us を置き直す。こうすると
// 出力は単調に増え、フレームの間隔は入力のまま残る。
inline ClockDecision classify_pts(const ClockState &state, std::int64_t incoming_pts_us,
				  const ClockThresholds &thresholds)
{
	ClockDecision decision;
	decision.in_pts_us = incoming_pts_us;
	if (!state.started) {
		// セグメントの最初のフレーム。比べる値がないので、そのまま受け取る (セグメントの先頭の pts を 0 にする
		// 引き算は、書き込む側で行う)。
		decision.step = ClockStep::Forward;
		decision.out_pts_us = incoming_pts_us;
		decision.offset_us = 0;
		return decision;
	}

	const std::int64_t jump_limit = clock_jump_limit_us(state, thresholds);
	// 比べるのは直前に届いたタイムスタンプである (上のコメント)。
	const std::int64_t delta = incoming_pts_us - state.last_in_pts_us;

	if (delta > 0) {
		// 前へ進んだ。大きく進んだときだけセグメントを閉じる (clock_jump_limit_us)。
		decision.step = delta >= jump_limit ? ClockStep::Jumped : ClockStep::Forward;
		if (decision.step == ClockStep::Jumped) {
			// 閉じた後は新しい原点になるので、ずれを持ち越さない。
			decision.out_pts_us = incoming_pts_us;
			decision.offset_us = 0;
			return decision;
		}
		decision.offset_us = state.offset_us;
		// ずらしたときに足したずれを、フレームの間隔を超えて進んだ分だけ 0 に戻す。戻さないと、ずれが
		// セグメントの終わりまで残る。送信側のタイムスタンプがいったん下がって後で元に戻ると (上流の描画の時刻の
		// 段など)、下がった分はずれとして足されたまま、戻った分はそのまま反映されるので、そのストリームの映像が
		// 下がった量だけ遅れ続ける。セグメントの位置は先頭の送信側のタイムスタンプで決まる
		// (record_args.h の build_session_timeline) ので、セグメントの中の時計も送信側のタイムスタンプに
		// 戻っていないと、音声とずれる。
		// 出力の間隔は delta - 戻した量 >= フレームの間隔 なので、単調さは崩れない。フレームの間隔が分からない
		// ストリーム (0) では戻さない。通常の間隔と欠落を見分けられず、戻すと間隔を 1 us まで詰めてしまう。
		// ずれを一定の時間で少しずつ戻す方法は使わない。間隔を詰める量が時間で決まって届いた値の進み方と
		// 関係がなくなり、間隔がフレームの間隔より詰まることがあるためである。
		if (state.offset_us > 0 && state.frame_period_us > 0 && delta > state.frame_period_us) {
			const std::int64_t room = delta - state.frame_period_us;
			decision.offset_us = state.offset_us - (room < state.offset_us ? room : state.offset_us);
		}
		decision.out_pts_us = incoming_pts_us + decision.offset_us;
		// 単調に増えることを保証する (普通は通らない。届いた値が増え、ずれは変わらないため)。
		if (decision.out_pts_us <= state.last_out_pts_us) {
			decision.out_pts_us = state.last_out_pts_us + clock_push_step_us(state);
			decision.offset_us = decision.out_pts_us - incoming_pts_us;
		}
		return decision;
	}

	// 進まなかった (delta == 0) か、戻った。どちらもずらす。
	decision.backward_us = -delta;
	if (decision.backward_us >= jump_limit) {
		decision.step = ClockStep::Jumped;
		decision.out_pts_us = incoming_pts_us;
		decision.offset_us = 0;
		return decision;
	}
	decision.step = decision.backward_us < clock_push_limit_us(state, thresholds) ? ClockStep::Pushed
										     : ClockStep::Reanchored;
	decision.out_pts_us = state.last_out_pts_us + clock_push_step_us(state);
	// 原点を置き直す。以後に届くタイムスタンプにはこのずれを足すので、次のフレームはまた Forward に戻る。
	// 置き直さないと、1 回戻っただけで、セグメントの残りのフレームが全部 Pushed か Reanchored になる。
	decision.offset_us = decision.out_pts_us - incoming_pts_us;
	return decision;
}

// 判定の結果を状態に反映する。Jumped でも反映する。呼び出し元はセグメントを閉じた後、新しいセグメントの ClockState を
// started = false から作り直すが、ここで反映しておくと、閉じずに続ける場合にもタイムスタンプが単調に増える。
inline void advance(ClockState &state, const ClockDecision &decision)
{
	state.last_out_pts_us = decision.out_pts_us;
	state.last_in_pts_us = decision.in_pts_us;
	state.offset_us = decision.offset_us;
	state.started = true;
}

// 記録した後戻りが、B フレームの疑い (フレームの間隔の整数倍で規則的に繰り返す) か。偽なら、送信側の通常の
// 置き直し (1 度だけで続かない) とみなす。真なら、その人は ProRes で録画ファイルを作成する (record_branch.h の decide_branch)。
// 倍数が毎回同じであることは求めない (B ピラミッドでは、1 フレームと 3 フレームの後戻りが混ざる)。
inline bool backward_is_periodic(const std::vector<PtsReanchor> &reanchors, std::int64_t frame_period_us)
{
	if (frame_period_us <= 0)
		return false;
	if (reanchors.size() < kPeriodicBackwardMinCount)
		return false;

	const std::int64_t tolerance = frame_period_us / kPeriodicBackwardToleranceDiv;
	for (const PtsReanchor &reanchor : reanchors) {
		if (reanchor.delta_us <= 0)
			return false;
		// 最も近い整数倍との差を見る。
		const std::int64_t multiple = (reanchor.delta_us + frame_period_us / 2) / frame_period_us;
		if (multiple < 1)
			return false;
		const std::int64_t expected = multiple * frame_period_us;
		const std::int64_t diff = reanchor.delta_us > expected ? reanchor.delta_us - expected
								      : expected - reanchor.delta_us;
		if (diff > tolerance)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// すべてのストリームに同時に起きた不連続
// ---------------------------------------------------------------------------

// 1 つのストリームで不連続と判定した 1 件。到着の時刻 (受信側の steady_clock) で並べる。送信側のタイムスタンプは
// 不連続を起こした当の値なので、同時かどうかの基準に使えない。
struct JumpObservation {
	std::uint32_t stream_id = 0;
	std::int64_t at_steady_us = 0;
};

// 同時とみなす時間の幅。到着の遅延が数十 ms なので、その数倍にした。
inline constexpr std::int64_t kGlobalJumpWindowUs = 200000;

// 受信中のすべてのストリームに、同時に不連続が起きたか。真なら、原因はストリームの側ではなく時計の側にある
// (1 つのストリームだけが途切れたときや、再接続したときと区別できる)。受信中のストリームが 1 つだけのときは偽を
// 返す。すべてに起きたことと、1 つに起きたことを見分けられないためである。
inline bool jump_is_global(const std::vector<JumpObservation> &jumps, std::size_t live_stream_count,
			   std::int64_t window_us = kGlobalJumpWindowUs)
{
	if (live_stream_count < 2 || jumps.size() < live_stream_count)
		return false;
	const std::int64_t width = window_us > 0 ? window_us : 0;

	for (const JumpObservation &start : jumps) {
		std::vector<std::uint32_t> seen;
		for (const JumpObservation &one : jumps) {
			if (one.at_steady_us < start.at_steady_us || one.at_steady_us > start.at_steady_us + width)
				continue;
			bool known = false;
			for (const std::uint32_t id : seen) {
				if (id == one.stream_id) {
					known = true;
					break;
				}
			}
			if (!known)
				seen.push_back(one.stream_id);
		}
		if (seen.size() >= live_stream_count)
			return true;
	}
	return false;
}

} // namespace cordmux::record

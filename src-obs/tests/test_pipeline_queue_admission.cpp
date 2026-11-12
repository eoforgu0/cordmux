// SPDX-License-Identifier: Apache-2.0

// pipeline/queue_admission.h の単体テスト。画面のストリームのキューに積むか、捨てるかの判定を確認する。

#include "doctest.h"

#include "pipeline/queue_admission.h"

using cordmux::pipeline::decide_queue_admission;
using cordmux::pipeline::QueueAdmission;
using cordmux::pipeline::QueueAdmissionInput;
using cordmux::pipeline::QueueLimits;

TEST_CASE("数もバイト数も上限より少なければ積む")
{
	QueueAdmissionInput in;
	in.queued_count = 3;
	in.queued_bytes = 1000;
	in.incoming_bytes = 500;
	CHECK(decide_queue_admission(in) == QueueAdmission::Admit);
}

TEST_CASE("キューの中の数が max_count 以上なら、キューを全部捨てる")
{
	QueueAdmissionInput in;
	in.queued_count = 8;
	QueueLimits limits;
	limits.max_count = 8;
	CHECK(decide_queue_admission(in, limits) == QueueAdmission::OverflowClear);
}

TEST_CASE("数が上限より少なくても、合計のバイト数が"
          "上限を超えるならキューを全部捨てる")
{
	QueueAdmissionInput in;
	in.queued_count = 1; // 数は上限よりずっと少ない
	in.queued_bytes = 900;
	in.incoming_bytes = 200; // 合計 1100 > 上限 1000
	QueueLimits limits;
	limits.max_count = 8;
	limits.max_bytes = 1000;
	CHECK(decide_queue_admission(in, limits) == QueueAdmission::OverflowClear);
}

TEST_CASE("合計のバイト数がちょうど上限なら積む")
{
	QueueAdmissionInput in;
	in.queued_bytes = 800;
	in.incoming_bytes = 200;
	QueueLimits limits;
	limits.max_bytes = 1000;
	CHECK(decide_queue_admission(in, limits) == QueueAdmission::Admit);
}

TEST_CASE("キーフレームを待っている間は、キーフレームでないアクセスユニットを 1 つだけ捨てる")
{
	QueueAdmissionInput in;
	in.waiting_for_keyframe = true;
	in.is_keyframe = false;
	// キューが空で上限に余裕があっても、キーフレームでなければ積まない。
	in.queued_count = 0;
	CHECK(decide_queue_admission(in) == QueueAdmission::DropWaitingForKeyframe);
}

TEST_CASE("キーフレームを待っている間にキーフレームが届いたら積む")
{
	QueueAdmissionInput in;
	in.waiting_for_keyframe = true;
	in.is_keyframe = true;
	CHECK(decide_queue_admission(in) == QueueAdmission::Admit);
}

TEST_CASE("キーフレームを待っている間は、上限より先にキーフレームかどうかで決める")
{
	// waiting_for_keyframe が true の間は、上限を超えていなくてもキーフレームでなければ捨てる
	// (呼び出し元は、積んだら待つのをやめるだけでよい)。
	QueueAdmissionInput in;
	in.waiting_for_keyframe = true;
	in.is_keyframe = false;
	in.queued_count = 0;
	in.queued_bytes = 0;
	QueueLimits limits;
	limits.max_count = 100;
	limits.max_bytes = 100000;
	CHECK(decide_queue_admission(in, limits) == QueueAdmission::DropWaitingForKeyframe);
}

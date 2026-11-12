// SPDX-License-Identifier: Apache-2.0

// pipeline/receive_restart.h の単体テスト。枠に載せている人の映像が届かなくなったときに、待つ時間の後に受信のやり直しを
// 頼み、届かないままなら間隔を広げて頼み直し、届いたら戻ったこととして返して間隔を最初に戻すことを確認する。頼まない
// 場合 (configure にない、枠に載せていない、1 度も届いていない、ダミー、無効、接続していない、待つ時間が 0) も確認する。

#include "doctest.h"

#include "pipeline/receive_restart.h"

#include <cstdint>
#include <string>
#include <vector>

using cordmux::pipeline::plan_receive_restart;
using cordmux::pipeline::ReceiveRestartInput;
using cordmux::pipeline::ReceiveRestartPlan;
using cordmux::pipeline::ReceiveRestartState;
using cordmux::pipeline::ReceiveRestartStream;

namespace {

constexpr std::int64_t kSec = 1'000'000;

ReceiveRestartStream screen(std::uint32_t id, const std::string &user, bool in_slot, std::optional<std::int64_t> last)
{
	ReceiveRestartStream s;
	s.stream_id = id;
	s.user_id = user;
	s.in_slot = in_slot;
	s.last_frame_us = last;
	return s;
}

ReceiveRestartInput input_at(std::int64_t now_us, std::vector<ReceiveRestartStream> streams)
{
	ReceiveRestartInput in;
	in.now_us = now_us;
	in.wait_us = 5 * kSec;
	in.connected = true;
	in.feature_enabled = true;
	in.dummy = false;
	in.streams = std::move(streams);
	return in;
}

} // namespace

TEST_CASE("受信のやり直し: 最後のフレームから待つ時間がたったら 1 回目を頼み、10 秒後、30 秒後、以後 30 秒ごとに頼み直す")
{
	ReceiveRestartState state;
	const std::int64_t last = 100 * kSec;
	std::vector<std::int64_t> asked_at;
	std::vector<int> numbers;
	for (std::int64_t t = last; t <= last + 120 * kSec; t += kSec) {
		const ReceiveRestartPlan plan = plan_receive_restart(state, input_at(t, {screen(7, "u1", true, last)}));
		for (const auto &req : plan.requests) {
			asked_at.push_back((t - last) / kSec);
			numbers.push_back(req.check_number);
			CHECK(req.user_id == "u1");
			CHECK(req.stream_ids == std::vector<std::uint32_t>{7});
			CHECK(req.stopped_for_us == t - last);
		}
		CHECK(plan.recoveries.empty());
		state = plan.next;
	}
	CHECK(asked_at == std::vector<std::int64_t>{5, 15, 45, 75, 105});
	CHECK(numbers == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("受信のやり直し: 頼んだ後に映像が届いたら、戻ったこととして返し、次に止まったときは間隔を最初から数える")
{
	ReceiveRestartState state;
	const std::int64_t last = 100 * kSec;
	ReceiveRestartPlan plan = plan_receive_restart(state, input_at(last + 5 * kSec, {screen(7, "u1", true, last)}));
	REQUIRE(plan.requests.size() == 1);
	state = plan.next;
	plan = plan_receive_restart(state, input_at(last + 15 * kSec, {screen(7, "u1", true, last)}));
	REQUIRE(plan.requests.size() == 1);
	state = plan.next;

	// 18 秒後に届いた。
	const std::int64_t back = last + 18 * kSec;
	plan = plan_receive_restart(state, input_at(back + kSec / 2, {screen(7, "u1", true, back)}));
	CHECK(plan.requests.empty());
	REQUIRE(plan.recoveries.size() == 1);
	CHECK(plan.recoveries[0].user_id == "u1");
	CHECK(plan.recoveries[0].stopped_for_us == 18 * kSec);
	CHECK(plan.recoveries[0].since_first_check_us == 13 * kSec);
	CHECK(plan.recoveries[0].checks == 2);
	CHECK(plan.next.people.empty());
	state = plan.next;

	// また止まったら、待つ時間の 5 秒後に 1 回目から頼む。
	plan = plan_receive_restart(state, input_at(back + 5 * kSec, {screen(7, "u1", true, back)}));
	REQUIRE(plan.requests.size() == 1);
	CHECK(plan.requests[0].check_number == 1);
}

TEST_CASE("受信のやり直し: 待つ時間より前に届けば頼まず、頼む前に戻ったときは戻ったことも返さない")
{
	ReceiveRestartState state;
	const std::int64_t last = 100 * kSec;
	ReceiveRestartPlan plan = plan_receive_restart(state, input_at(last + 4 * kSec, {screen(7, "u1", true, last)}));
	CHECK(plan.requests.empty());
	CHECK(plan.next.people.empty());
	plan = plan_receive_restart(plan.next, input_at(last + 4 * kSec + 1, {screen(7, "u1", true, last + 4 * kSec)}));
	CHECK(plan.requests.empty());
	CHECK(plan.recoveries.empty());
}

TEST_CASE("受信のやり直し: 同じ人の別のストリームの映像が届いていれば、止まっていないとみなす。頼むのは枠に載っているストリームだけ")
{
	const std::int64_t now = 200 * kSec;
	// 古い接続のストリーム 3 は止まっているが、新しい接続のストリーム 9 は届いている。
	ReceiveRestartPlan plan = plan_receive_restart(
		ReceiveRestartState{}, input_at(now, {screen(3, "u1", true, 100 * kSec), screen(9, "u1", true, now - kSec)}));
	CHECK(plan.requests.empty());

	// 両方止まっていれば、両方のストリームを頼む。枠に載っていないストリーム (経路が枠 0) は頼みに入れない。
	plan = plan_receive_restart(ReceiveRestartState{}, input_at(now, {screen(3, "u1", true, 100 * kSec),
									  screen(9, "u1", true, 150 * kSec),
									  screen(11, "u1", false, 120 * kSec)}));
	REQUIRE(plan.requests.size() == 1);
	CHECK(plan.requests[0].stream_ids == std::vector<std::uint32_t>{3, 9});
	CHECK(plan.requests[0].stopped_for_us == 50 * kSec);
}

TEST_CASE("受信のやり直し: 枠に載せていない人、1 度も届いていない人、configure にない人には頼まない")
{
	const std::int64_t now = 200 * kSec;
	ReceiveRestartPlan plan = plan_receive_restart(
		ReceiveRestartState{}, input_at(now, {screen(3, "u1", false, 100 * kSec), screen(4, "u2", true, std::nullopt)}));
	CHECK(plan.requests.empty());
	CHECK(plan.next.people.empty());

	// 頼んでいた人のストリームが configure から消えたら、状態を捨てる (戻ったことも返さない)。
	ReceiveRestartState state;
	plan = plan_receive_restart(state, input_at(now, {screen(5, "u3", true, 100 * kSec)}));
	REQUIRE(plan.requests.size() == 1);
	plan = plan_receive_restart(plan.next, input_at(now + kSec, {}));
	CHECK(plan.requests.empty());
	CHECK(plan.recoveries.empty());
	CHECK(plan.next.people.empty());
}

TEST_CASE("受信のやり直し: ダミー映像の間、無効の間、接続していない間、待つ時間が 0 の間は頼まず、状態も捨てる")
{
	const std::int64_t now = 200 * kSec;
	ReceiveRestartState state;
	ReceiveRestartPlan plan = plan_receive_restart(state, input_at(now, {screen(5, "u1", true, 100 * kSec)}));
	REQUIRE(plan.requests.size() == 1);
	state = plan.next;
	REQUIRE(state.people.size() == 1);

	for (int which = 0; which < 4; ++which) {
		ReceiveRestartInput in = input_at(now + 60 * kSec, {screen(5, "u1", true, 100 * kSec)});
		if (which == 0)
			in.dummy = true;
		else if (which == 1)
			in.feature_enabled = false;
		else if (which == 2)
			in.connected = false;
		else
			in.wait_us = 0;
		const ReceiveRestartPlan off = plan_receive_restart(state, in);
		CHECK(off.requests.empty());
		CHECK(off.recoveries.empty());
		CHECK(off.next.people.empty());
	}
}

TEST_CASE("受信のやり直し: 待つ時間は設定の値で変わる")
{
	ReceiveRestartInput in = input_at(110 * kSec, {screen(5, "u1", true, 100 * kSec)});
	in.wait_us = 12 * kSec;
	CHECK(plan_receive_restart(ReceiveRestartState{}, in).requests.empty());
	in.now_us = 112 * kSec;
	CHECK(plan_receive_restart(ReceiveRestartState{}, in).requests.size() == 1);
}

// ---- デコードの経路が黙って止まったとき (plan_decode_restart) ----

namespace {

using cordmux::pipeline::DecodeRestartInput;
using cordmux::pipeline::DecodeRestartPlan;
using cordmux::pipeline::DecodeRestartState;
using cordmux::pipeline::DecodeRestartStream;
using cordmux::pipeline::plan_decode_restart;

// arrival は最後の到着、first は出力がないまま届き始めた時刻 (なければ出力の後にまだ届いていない)、decoded は最後の出力。
DecodeRestartStream decoding(std::uint32_t id, bool in_slot, std::optional<std::int64_t> arrival,
			     std::optional<std::int64_t> first, std::optional<std::int64_t> decoded)
{
	DecodeRestartStream s;
	s.stream_id = id;
	s.in_slot = in_slot;
	s.last_arrival_us = arrival;
	s.first_undecoded_arrival_us = first;
	s.last_decoded_us = decoded;
	return s;
}

DecodeRestartInput decode_input_at(std::int64_t now_us, std::vector<DecodeRestartStream> streams)
{
	DecodeRestartInput in;
	in.now_us = now_us;
	in.wait_us = 5 * kSec;
	in.connected = true;
	in.feature_enabled = true;
	in.dummy = false;
	in.streams = std::move(streams);
	return in;
}

} // namespace

TEST_CASE("デコードの監視: 出力がないまま待つ時間届き続けたら作り直し、10 秒後、30 秒後、以後 30 秒ごとに作り直す")
{
	// 10 秒まで出力があり、11 秒から毎秒フレームが届き続けるが、出力が出ない。作り直した経路は、作った後の最初の到着
	// (作り直しの 1 秒後) から数え直す。
	DecodeRestartState state;
	std::int64_t first = 11 * kSec;
	std::vector<std::int64_t> at;
	std::vector<int> numbers;
	for (std::int64_t t = 11 * kSec; t <= 120 * kSec; t += kSec) {
		const DecodeRestartPlan plan =
			plan_decode_restart(state, decode_input_at(t, {decoding(7, true, t, first, 10 * kSec)}));
		CHECK(plan.recoveries.empty());
		for (const auto &req : plan.requests) {
			at.push_back(t / kSec);
			numbers.push_back(req.check_number);
			CHECK(req.stream_id == 7);
			CHECK(req.stalled_for_us == t - first);
			first = t + kSec; // 作り直した。次の到着から数える
		}
		state = plan.next;
	}
	CHECK(at == std::vector<std::int64_t>{16, 26, 56, 86, 116});
	CHECK(numbers == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("デコードの監視: 静止した画面の共有で 10 秒届かなかった後に 1 枚届き、まだ出力がない時点では作り直さない")
{
	// 10 秒に最後の出力。20 秒に 1 枚届き、20.005 秒の確認の時点ではまだデコードし終えていない。最後の出力から数えると
	// 10 秒たっているが、出力がないまま届いている時間は 5 ms なので、作り直さない。
	const DecodeRestartPlan plan = plan_decode_restart(
		DecodeRestartState{}, decode_input_at(20 * kSec + 5'000, {decoding(7, true, 20 * kSec, 20 * kSec, 10 * kSec)}));
	CHECK(plan.requests.empty());
	CHECK(plan.next.streams.empty());
	// その後、出力がないまま 5 秒届き続ければ作り直す。
	const DecodeRestartPlan later = plan_decode_restart(
		plan.next, decode_input_at(25 * kSec, {decoding(7, true, 25 * kSec, 20 * kSec, 10 * kSec)}));
	REQUIRE(later.requests.size() == 1);
	CHECK(later.requests[0].stalled_for_us == 5 * kSec);
}

TEST_CASE("デコードの監視: 作り直した後に出力が出たら、戻ったこととして返し、状態を捨てる")
{
	DecodeRestartState state;
	DecodeRestartPlan plan =
		plan_decode_restart(state, decode_input_at(20 * kSec, {decoding(7, true, 20 * kSec, 15 * kSec, 14 * kSec)}));
	REQUIRE(plan.requests.size() == 1);
	CHECK(plan.requests[0].stalled_for_us == 5 * kSec);
	state = plan.next;
	// 作り直した経路に 21 秒から届き、22 秒に出力が出た (出力が出たので、出力のないまま届き始めた時刻は空に戻る)。
	plan = plan_decode_restart(state, decode_input_at(23 * kSec, {decoding(7, true, 23 * kSec, std::nullopt, 22 * kSec)}));
	CHECK(plan.requests.empty());
	REQUIRE(plan.recoveries.size() == 1);
	CHECK(plan.recoveries[0].stream_id == 7);
	CHECK(plan.recoveries[0].stopped_for_us == 7 * kSec);
	CHECK(plan.recoveries[0].since_first_check_us == 2 * kSec);
	CHECK(plan.recoveries[0].checks == 1);
	CHECK(plan.next.streams.empty());
}

TEST_CASE("デコードの監視: フレームが届いていないとき、待つ時間に届かないとき、出力が出ているときは作り直さない")
{
	// 出力がないまま届き始めてから 10 秒たったが、最後の到着が 10 秒前 (その後は届いていない)。
	CHECK(plan_decode_restart(DecodeRestartState{},
				  decode_input_at(30 * kSec, {decoding(7, true, 20 * kSec, 20 * kSec, 10 * kSec)}))
		      .requests.empty());
	// 出力がないまま届いている時間が 4 秒 (待つ時間に届かない)。
	CHECK(plan_decode_restart(DecodeRestartState{},
				  decode_input_at(24 * kSec, {decoding(7, true, 24 * kSec, 20 * kSec, 19 * kSec)}))
		      .requests.empty());
	// 出力が出ている (出力のないまま届いているフレームがない)。
	CHECK(plan_decode_restart(DecodeRestartState{},
				  decode_input_at(24 * kSec, {decoding(7, true, 24 * kSec, std::nullopt, 24 * kSec)}))
		      .requests.empty());
	// 1 度も出力が出ていない経路でも、作った後の最初の到着から待つ時間届き続ければ作り直す。
	CHECK(plan_decode_restart(DecodeRestartState{},
				  decode_input_at(6 * kSec, {decoding(7, true, 6 * kSec, 1 * kSec, std::nullopt)}))
		      .requests.size() == 1);
	// 最初のキーフレームを待っている間 (作った後の最初の到着から 3 秒) は作り直さない。
	CHECK(plan_decode_restart(DecodeRestartState{},
				  decode_input_at(4 * kSec, {decoding(7, true, 4 * kSec, 1 * kSec, std::nullopt)}))
		      .requests.empty());
}

TEST_CASE("デコードの監視: 枠に載せていないストリーム、ダミー、無効、接続していない、待つ時間が 0 の間は作り直さない")
{
	CHECK(plan_decode_restart(DecodeRestartState{},
				  decode_input_at(20 * kSec, {decoding(7, false, 20 * kSec, 10 * kSec, 9 * kSec)}))
		      .requests.empty());
	for (int which = 0; which < 4; ++which) {
		DecodeRestartInput in = decode_input_at(20 * kSec, {decoding(7, true, 20 * kSec, 10 * kSec, 9 * kSec)});
		if (which == 0)
			in.dummy = true;
		else if (which == 1)
			in.feature_enabled = false;
		else if (which == 2)
			in.connected = false;
		else
			in.wait_us = 0;
		const DecodeRestartPlan plan = plan_decode_restart(DecodeRestartState{}, in);
		CHECK(plan.requests.empty());
		CHECK(plan.next.streams.empty());
	}
}

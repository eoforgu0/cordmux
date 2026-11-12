// SPDX-License-Identifier: Apache-2.0

// 枠に載せている人の画面共有の映像が届かなくなったときに、送信側に受信のやり直し (そのストリームの確認と、必要なら
// 読み出しとエンコードを始め直すこと) を頼むかを決める純粋関数 (plan_receive_restart)。Pipeline が 1 秒ごとに呼び、
// 返った頼みを stream_check で送る (docs/wire_format.md の「メッセージの種類」の stream_check の扱い)。libobs に依存しない。
//
// 利用者にとっての意味: 実際の 4 人の通話で、Vesktop には映像が届いているのに cordmux の枠の映像が止まり、枠の
// チェックを付け直すまで戻らなかった。付け直しは、送信側の読み出しとエンコードを止めて始め直す。同じことを、枠の
// 割り当てを変えずに自動で行い、利用者の操作を待たずに戻す。
//
// 頼むかの決め方:
// - 頼むのは、枠 (1 から 4) に載せていて映像を出してよい人の、configure にある画面共有のストリームだけである。
//   枠に載せていない人の映像は配信に出ていないので、止まっても困らない。
// - 1 人ずつ、その人の画面のストリームのうち最も新しいフレームの時刻を見る。同じ人に 2 つのストリームがあり
//   (Discord が接続を作り直した後など)、片方の映像が届いていれば、止まっていない。
// - 1 度も届いていない人には頼まない。届いていた映像が止まったことだけを扱う (最初のフレームを待つ間は、
//   「映像を待っています」の表示が扱う)。
// - 最後のフレームから wait_us (settings の receive_restart_wait_ms。既定 5 秒) たったら 1 回目を頼む。届かないままなら、
//   1 回目の 10 秒後、その 30 秒後、以後 30 秒ごとに頼み直す。画面が動かない共有では Discord がフレームを送らないことが
//   あり、そのときは送信側が始め直さずに答える (not_receiving) ので、頼み直しは確認だけになる。間隔を広げるのは、
//   その確認とログの行を、止まっている間ずっと数秒ごとに繰り返さないためである。30 秒は、利用者が自分で気付いて
//   チェックを付け直すより前に、もう一度確認できる長さとして決めた目安の値である。
// - 映像が届いたら (最も新しいフレームの時刻が進んだら)、戻ったこととして返し、間隔を最初に戻す。
// - 接続していない間、cordmux を無効にしている間、ダミー映像を表示している間、wait_us が 0 以下の間は頼まず、
//   数えていた状態も捨てる (ダミーの間は実際のフレームを配信に出さないので、止まっても配信に影響しない)。
// - 頼んだときに受信側のデコードの経路は作り直さない。この頼みは、フレームが受信側に届いていないとき (ws に届いた
//   時刻で見る) だけに出すので、デコーダーには何も来ておらず、作り直しても戻る理由がない。送信側が始め直したとき
//   (restarted) も作り直さない。新しいエンコーダの最初のフレームは SPS と PPS を前に付けたキーフレームで
//   (src-vencord/encode/session.ts)、FFmpeg の H.264 のデコーダーは、ビットストリームの中の SPS と PPS が変わっても
//   そのまま読み直す。同じ stream_id でエンコーダを作り直すのは、画質の上限の変更やフォールバックと同じ道である。
//
// もう 1 つの判定 (plan_decode_restart) は、受信側だけで閉じる。フレームは届いているのに、そのストリームのデコード済みの
// フレームが出ない状態を見る。デコードの経路は、次のように黙って出力を止めることがある (src-obs/decode/h264_decoder.cpp)。
// - drain() で convert_frame() が失敗したフレーム (ハードウェアのデコーダーから CPU のメモリへの読み戻し
//   av_hwframe_transfer_data の失敗、対応していないピクセルフォーマット) は、エラーにもキーフレームの要求にもせずに捨てる。
//   ハードウェアのデバイスが失われると、すべてのフレームがこうなる。
// - avcodec がエラーを返し続ける (ハードウェアのデバイスが失われたときなど) と、push() はキーフレームを求め続ける
//   (pipeline.cpp の screen_worker_main が 1 秒に 1 回まで送る) が、同じ H264Decoder を使い続けるので、キーフレームが
//   届いても戻らない。
// そのため、フレームが届き続けているのに、デコード済みのフレームが wait_us の間 1 枚も出なければ、そのストリームの
// デコードの経路を作り直し (新しい H264Decoder と、ハードウェアのデバイス)、キーフレームを求める。頼み直しの間隔は
// plan_receive_restart と同じである。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cordmux::pipeline {

// 1 回目の頼みの後、2 回目までの間隔と、3 回目からの間隔 (us)。決め方はファイルの先頭のコメントにある。
inline constexpr std::int64_t kReceiveRestartSecondGapUs = 10'000'000;
inline constexpr std::int64_t kReceiveRestartLaterGapUs = 30'000'000;

// configure にある画面共有のストリーム 1 つ。
struct ReceiveRestartStream {
	std::uint32_t stream_id = 0;
	std::string user_id;
	// 経路が画面の枠 (1 から 4) を指していて、映像を出してよいか (StreamRouter::video_allowed)。
	bool in_slot = false;
	// 最後にフレームが届いた時刻 (steady の us)。1 度も届いていなければ持たない。
	std::optional<std::int64_t> last_frame_us;
};

struct ReceiveRestartInput {
	std::int64_t now_us = 0; // steady の us (last_frame_us と同じ時計)
	std::int64_t wait_us = 0;
	bool connected = false;
	bool feature_enabled = false;
	bool dummy = false;
	std::vector<ReceiveRestartStream> streams;
};

// 映像が止まっている人 1 人の、頼みの状態。
struct ReceiveRestartEpisode {
	std::int64_t stopped_at_us = 0;     // 止まる前の最後のフレームの時刻
	int checks = 0;                     // 頼んだ回数
	std::int64_t first_check_at_us = 0; // 1 回目を頼んだ時刻
	std::int64_t next_check_at_us = 0;  // 次に頼む時刻
};

inline bool operator==(const ReceiveRestartEpisode &a, const ReceiveRestartEpisode &b)
{
	return a.stopped_at_us == b.stopped_at_us && a.checks == b.checks && a.first_check_at_us == b.first_check_at_us &&
	       a.next_check_at_us == b.next_check_at_us;
}

struct ReceiveRestartState {
	std::map<std::string, ReceiveRestartEpisode> people; // user_id -> 状態
};

// 頼み 1 つ (1 人の、枠に載っている画面のストリームすべて)。
struct ReceiveRestartRequest {
	std::string user_id;
	std::vector<std::uint32_t> stream_ids;
	std::int64_t stopped_for_us = 0; // 最後のフレームからの時間
	int check_number = 0;            // 何回目の頼みか (1 から)
};

// 頼んだ後に映像が戻ったこと。
struct ReceiveRestartRecovery {
	std::string user_id;
	std::int64_t stopped_for_us = 0;          // 止まる前の最後のフレームから、戻ったフレームまで
	std::int64_t since_first_check_us = 0;    // 1 回目の頼みから、戻ったフレームまで
	int checks = 0;
};

struct ReceiveRestartPlan {
	ReceiveRestartState next;
	std::vector<ReceiveRestartRequest> requests;
	std::vector<ReceiveRestartRecovery> recoveries;
};

// 何回目の頼みの後に、次の頼みまで空ける時間。
inline std::int64_t receive_restart_gap_us(int checks_done)
{
	return checks_done <= 1 ? kReceiveRestartSecondGapUs : kReceiveRestartLaterGapUs;
}

inline ReceiveRestartPlan plan_receive_restart(const ReceiveRestartState &prev, const ReceiveRestartInput &in)
{
	ReceiveRestartPlan plan;
	if (!in.connected || !in.feature_enabled || in.dummy || in.wait_us <= 0)
		return plan; // 頼まない間は、数えていた状態も捨てる。

	// 1 人ずつ、枠に載っているストリームと、最も新しいフレームの時刻をまとめる。
	struct Person {
		std::vector<std::uint32_t> slotted;
		std::optional<std::int64_t> latest;
	};
	std::map<std::string, Person> people;
	for (const ReceiveRestartStream &s : in.streams) {
		if (s.user_id.empty())
			continue;
		Person &p = people[s.user_id];
		if (s.in_slot)
			p.slotted.push_back(s.stream_id);
		if (s.last_frame_us && (!p.latest || *s.last_frame_us > *p.latest))
			p.latest = s.last_frame_us;
	}

	for (const auto &[user_id, person] : people) {
		if (person.slotted.empty() || !person.latest)
			continue; // 枠に載っていない人と、1 度も届いていない人には頼まない。
		const std::int64_t latest = *person.latest;
		const auto found = prev.people.find(user_id);
		std::optional<ReceiveRestartEpisode> episode;
		if (found != prev.people.end())
			episode = found->second;
		// 前の頼みの後にフレームが届いた。戻ったこととして返し、状態を捨てる (まだ止まっていれば、下で数え直す)。
		if (episode && latest > episode->stopped_at_us) {
			if (episode->checks > 0) {
				ReceiveRestartRecovery r;
				r.user_id = user_id;
				r.stopped_for_us = latest - episode->stopped_at_us;
				r.since_first_check_us = latest - episode->first_check_at_us;
				r.checks = episode->checks;
				plan.recoveries.push_back(std::move(r));
			}
			episode.reset();
		}
		if (in.now_us - latest < in.wait_us)
			continue; // 止まっていない (待つ時間に届かない)。
		if (!episode) {
			ReceiveRestartEpisode fresh;
			fresh.stopped_at_us = latest;
			fresh.next_check_at_us = latest + in.wait_us;
			episode = fresh;
		}
		if (in.now_us >= episode->next_check_at_us) {
			episode->checks++;
			if (episode->checks == 1)
				episode->first_check_at_us = in.now_us;
			episode->next_check_at_us = in.now_us + receive_restart_gap_us(episode->checks);
			ReceiveRestartRequest req;
			req.user_id = user_id;
			req.stream_ids = person.slotted;
			req.stopped_for_us = in.now_us - latest;
			req.check_number = episode->checks;
			plan.requests.push_back(std::move(req));
		}
		plan.next.people[user_id] = *episode;
	}
	return plan;
}

// ---- デコードの経路が黙って止まったとき (plan_decode_restart) ----

// デコードの経路があるストリーム 1 つ。
struct DecodeRestartStream {
	std::uint32_t stream_id = 0;
	// 経路が画面の枠 (1 から 4) を指していて、映像を出してよいか。
	bool in_slot = false;
	// 最後にフレームが ws に届いた時刻 (steady の us)。1 度も届いていなければ持たない。
	std::optional<std::int64_t> last_arrival_us;
	// 最後のデコードの出力より後に届いた最初のフレームの到着時刻 (steady の us)。出力が出たら持たない (空に戻す)。経路を
	// 作った直後は、作った後の最初の到着になる。出力がないままフレームが届き続けている時間を、この時刻から数える。
	std::optional<std::int64_t> first_undecoded_arrival_us;
	// デコード済みのフレームが最後に出た時刻 (steady の us)。1 枚も出ていなければ持たない。戻ったかを見るためだけに使う。
	std::optional<std::int64_t> last_decoded_us;
};

struct DecodeRestartInput {
	std::int64_t now_us = 0;
	std::int64_t wait_us = 0;
	bool connected = false;
	bool feature_enabled = false;
	bool dummy = false;
	std::vector<DecodeRestartStream> streams;
};

// デコードの出力が止まっているストリーム 1 つの、作り直しの状態。
struct DecodeRestartEpisode {
	std::int64_t stalled_since_us = 0;  // 出力がないまま届き始めた時刻 (first_undecoded_arrival_us)
	int checks = 0;                     // 作り直した回数
	std::int64_t first_check_at_us = 0; // 1 回目に作り直した時刻
	std::int64_t next_check_at_us = 0;  // 次に作り直してよい時刻
};

struct DecodeRestartState {
	std::map<std::uint32_t, DecodeRestartEpisode> streams; // stream_id -> 状態
};

struct DecodeRestartRequest {
	std::uint32_t stream_id = 0;
	std::int64_t stalled_for_us = 0; // 出力がないままフレームが届き続けている時間 (now - first_undecoded_arrival_us)
	int check_number = 0;            // 何回目の作り直しか (1 から)
};

struct DecodeRestartRecovery {
	std::uint32_t stream_id = 0;
	std::int64_t stopped_for_us = 0;       // 出力がないまま届き始めた時刻から、戻った出力まで
	std::int64_t since_first_check_us = 0; // 1 回目の作り直しから、戻った出力まで
	int checks = 0;
};

struct DecodeRestartPlan {
	DecodeRestartState next;
	std::vector<DecodeRestartRequest> requests;
	std::vector<DecodeRestartRecovery> recoveries;
};

// フレームが届き続けているのに、デコード済みのフレームが wait_us の間出ないストリームを、作り直すかを決める。
// - 届き続けている: 最後の到着から wait_us たっていない。届かなくなった (静止した画面の共有など) ストリームは、
//   デコーダーに渡すものがないので、出力がなくても止まったとはみなさない (届かないことは plan_receive_restart が扱う)。
// - 出力が止まっている: 最後の出力より後に届いた最初のフレーム (first_undecoded_arrival_us) から、出力がないまま wait_us
//   以上たった。最後の出力の時刻から数えないのは、静止した画面の共有で長くフレームが届かなかった後に 1 枚届くと、
//   最後の出力の時刻からはすぐに wait_us を超え、その 1 枚をデコードし終えるまでの数 ms から数十 ms の間に確認が当たると、
//   正常なのに作り直してしまうためである。作り直した直後は、作った後の最初の到着から数え直すので、続けて作り直さない。
// - 作り直した後に出力が出たら (最後の出力が 1 回目の作り直しより後になったら)、戻ったこととして返す。
// - 頼み直しの間隔、頼まない条件 (接続、無効、ダミー、wait_us、枠) は plan_receive_restart と同じである。
inline DecodeRestartPlan plan_decode_restart(const DecodeRestartState &prev, const DecodeRestartInput &in)
{
	DecodeRestartPlan plan;
	if (!in.connected || !in.feature_enabled || in.dummy || in.wait_us <= 0)
		return plan;
	for (const DecodeRestartStream &s : in.streams) {
		if (!s.in_slot)
			continue;
		const auto found = prev.streams.find(s.stream_id);
		std::optional<DecodeRestartEpisode> episode;
		if (found != prev.streams.end())
			episode = found->second;
		if (episode && episode->checks > 0 && s.last_decoded_us && *s.last_decoded_us > episode->first_check_at_us) {
			DecodeRestartRecovery r;
			r.stream_id = s.stream_id;
			r.stopped_for_us = *s.last_decoded_us - episode->stalled_since_us;
			r.since_first_check_us = *s.last_decoded_us - episode->first_check_at_us;
			r.checks = episode->checks;
			plan.recoveries.push_back(r);
			episode.reset();
		}
		const bool arriving = s.last_arrival_us && in.now_us - *s.last_arrival_us < in.wait_us;
		const bool stalled =
			arriving && s.first_undecoded_arrival_us && in.now_us - *s.first_undecoded_arrival_us >= in.wait_us;
		if (stalled) {
			if (!episode) {
				DecodeRestartEpisode fresh;
				fresh.stalled_since_us = *s.first_undecoded_arrival_us;
				fresh.next_check_at_us = in.now_us;
				episode = fresh;
			}
			if (in.now_us >= episode->next_check_at_us) {
				episode->checks++;
				if (episode->checks == 1)
					episode->first_check_at_us = in.now_us;
				episode->next_check_at_us = in.now_us + receive_restart_gap_us(episode->checks);
				DecodeRestartRequest req;
				req.stream_id = s.stream_id;
				req.stalled_for_us = in.now_us - *s.first_undecoded_arrival_us;
				req.check_number = episode->checks;
				plan.requests.push_back(req);
			}
		}
		if (episode)
			plan.next.streams[s.stream_id] = *episode;
	}
	return plan;
}

} // namespace cordmux::pipeline

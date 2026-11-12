// SPDX-License-Identifier: Apache-2.0

// 録画の空き容量の計算。録画中の消費 (bytes_per_second_of と remaining_seconds) と、録画ファイルの作成の途中で必要になる空き容量の
// 最大値 (estimate_finalize_need_bytes) を求める。録画中の消費は、再エンコードしないので正確に計算できる。録画ファイルの作成では、
// セグメントファイルと録画ファイルが同時にディスクに載るので、別に見積もる。
// libobs、Qt、avformat に依存しない。

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "record/record_branch.h"
#include "record/record_ledger.h"
#include "record/record_types.h"

namespace cordmux::record {

// 見積もりに使う値。ProRes の大きさは、H.264 のバイト数からの倍率ではなく、解像度 x fps x 時間から求める。H.264 に
// 対する ProRes の大きさの倍率は、映像の内容によって 40 倍以上変わるためである。
// prores_bits_per_pixel の既定 3.5 は目安の値で、測って決めたものではない。ProRes 422 HQ の公称のビットレート
// (1920x1080 の 30p で約 220 Mbps) から逆算した。実際の画面共有の映像での値は確認していない (実際の録画を ProRes で
// 作成した録画ファイルの大きさを測れば決まる)。
struct FinalizeSpaceParams {
	double prores_bits_per_pixel = 3.5;
	int pcm_bytes_per_sample = 4; // 録画ファイルの PCM の形 (fl32 は 4、in24 は 3)
	int pcm_channels = 2;         // ワイヤー形式の音声のチャンネル数
	int pcm_sample_rate = 48000;  // ワイヤー形式の音声のサンプリングレート
};

// 録画中の消費 (書いたバイト数 / 経過時間)。再エンコードしないので正確に計算できる。
// 書いたバイト数を直接受け取る。録画ログに行が載るのはセグメントを閉じたときだけなので、録画ログから数えると、切れ目の
// ない長い録画では消費が 0 のままになり、残り時間が分からないまま、空き容量がなくなるまで録画が続いてしまう。
inline std::int64_t bytes_per_second_of(std::int64_t written_bytes, std::int64_t elapsed_us)
{
	if (elapsed_us <= 0 || written_bytes <= 0)
		return 0;
	return (written_bytes * 1000000) / elapsed_us;
}

// 録画ログに載っているセグメント (閉じたセグメント) だけから求める消費。開いたままのセグメントは 1 バイトも数えない
// ので、今開いているセグメントの分を呼び出し元が持っているなら、bytes_per_second_of を使う。
inline std::int64_t bytes_per_second(const LedgerDocument &doc, std::int64_t elapsed_us)
{
	std::int64_t total = 0;
	for (const LedgerSegmentRow &segment : doc.segments)
		total += segment.bytes > 0 ? segment.bytes : 0;
	return bytes_per_second_of(total, elapsed_us);
}

// 録画できる残り時間 (秒)。消費が分からなければ値を持たない (0 を返すと、もう書けないと読まれるため)。
inline std::optional<std::int64_t> remaining_seconds(std::uint64_t free_bytes, std::int64_t bytes_per_second)
{
	if (bytes_per_second <= 0)
		return std::nullopt;
	return static_cast<std::int64_t>(free_bytes / static_cast<std::uint64_t>(bytes_per_second));
}

// このセッションの長さ。録画ファイルの長さの見積もりに使う。ファイルごとの正確な長さ (セグメントと欠落の合計) は
// record_args.h の build_finalize_plan が求めるが、空き容量の監視は 5 分ごとの概算なので、session の行の時間 (なければ
// 映像のセグメントの長さの合計) を使う。
inline std::int64_t session_span_us(const LedgerDocument &doc)
{
	const LedgerSessionRow &session = doc.session;
	if (session.ended_at_steady_us > session.started_at_steady_us)
		return session.ended_at_steady_us - session.started_at_steady_us;

	std::int64_t span = 0;
	for (const LedgerSegmentRow &segment : doc.segments) {
		if (segment.media != MediaKind::Video || segment.timescale <= 0)
			continue;
		span += (segment.duration_ticks * 1000000) / segment.timescale;
	}
	return span;
}

namespace detail {

inline std::uint64_t pcm_bytes_for(std::int64_t span_us, const FinalizeSpaceParams &params)
{
	if (span_us <= 0 || params.pcm_sample_rate <= 0 || params.pcm_channels <= 0 ||
	    params.pcm_bytes_per_sample <= 0)
		return 0;
	const std::int64_t samples = (span_us * params.pcm_sample_rate) / 1000000;
	return static_cast<std::uint64_t>(samples) * static_cast<std::uint64_t>(params.pcm_channels) *
	       static_cast<std::uint64_t>(params.pcm_bytes_per_sample);
}

} // namespace detail

// 録画ファイルの作成の途中で必要になる空き容量の最大値。これから書くもの (録画ファイルと一時ファイル) の合計で見積もる。
// セグメントファイルはすでにディスクにあり、録画ファイルの作成は削除しないためである。足りない量は、表示の規則
// (ledger/) が求める。
// `branches` は finalize_outputs(doc) と同じ順に、ファイルごとの作成方法を渡す。長さが食い違うときは、すべてを
// Remux とみなす (小さいほうの見積もりになる)。大きく見積もると、呼び出し元の誤りが見積もりの数字に紛れて、
// 気付けなくなるためである。
inline std::uint64_t estimate_finalize_need_bytes(const LedgerDocument &doc,
						  const std::vector<FinalizeBranch> &branches,
						  const FinalizeSpaceParams &params = FinalizeSpaceParams{})
{
	const std::vector<OutputKey> outputs = finalize_outputs(doc);
	const std::int64_t span_us = session_span_us(doc);
	const std::uint64_t pcm_bytes = detail::pcm_bytes_for(span_us, params);

	std::uint64_t need = 0;
	for (std::size_t i = 0; i < outputs.size(); ++i) {
		const FinalizeBranch branch = (branches.size() == outputs.size()) ? branches[i]
										 : FinalizeBranch::Remux;
		// 全体の長さの無音の PCM を一時ファイルに作ってから多重化するので、その PCM と、録画ファイルに入る同じ量が
		// 同時にディスクに載る。
		need += pcm_bytes * 2;

		if (outputs[i].kind == StreamKind::Voice)
			continue; // 声は映像を持たない

		if (branch == FinalizeBranch::Remux) {
			// 再エンコードしないので、録画ファイルの映像はセグメントファイルと同じ大きさになる。
			for (const LedgerSegmentRow &segment : doc.segments) {
				if (segment.user_id != outputs[i].user_id || segment.kind != outputs[i].kind)
					continue;
				if (segment.media == MediaKind::Video && segment.bytes > 0)
					need += static_cast<std::uint64_t>(segment.bytes);
			}
			continue;
		}

		// ProRes で録画ファイルを作成するときは、解像度 x fps x 時間から求める (FinalizeSpaceParams)。
		int fps_num = 60;
		int fps_den = 1;
		for (const LedgerSegmentRow &segment : doc.segments) {
			if (segment.user_id == outputs[i].user_id && segment.kind == outputs[i].kind &&
			    segment.media == MediaKind::Video && segment.fps_num > 0 && segment.fps_den > 0) {
				fps_num = segment.fps_num;
				fps_den = segment.fps_den;
				break;
			}
		}
		const DominantSize size = dominant_size(doc.segments, outputs[i].user_id, outputs[i].kind);
		const double seconds = static_cast<double>(span_us) / 1000000.0;
		const double pixels = static_cast<double>(size.width) * static_cast<double>(size.height);
		const double fps = static_cast<double>(fps_num) / static_cast<double>(fps_den);
		const double bytes = seconds * pixels * fps * params.prores_bits_per_pixel / 8.0;
		if (bytes > 0.0)
			need += static_cast<std::uint64_t>(bytes);
	}
	return need;
}

// 録画できる残り時間が、しきい値を下回ったか。残り時間が分からない (値を持たない) ときは偽を返す。分からないことを、
// 少ないと断定しないためである。
inline bool space_low(std::optional<std::int64_t> remaining_seconds, int warn_minutes)
{
	if (!remaining_seconds.has_value())
		return false;
	const std::int64_t threshold = static_cast<std::int64_t>(warn_minutes > 0 ? warn_minutes : 0) * 60;
	return *remaining_seconds < threshold;
}

} // namespace cordmux::record

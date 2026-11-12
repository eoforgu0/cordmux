// SPDX-License-Identifier: Apache-2.0

// 録画の作成方法の判定 (decide_branch) と、録画ファイルの作成で作るファイルの一覧、ProRes で録画ファイルを作成するときの解像度、声のファイルを
// 書くかどうかの判定。
// libobs、Qt、avformat に依存しない。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "record/record_avcc.h"
#include "record/record_ledger.h"
#include "record/record_types.h"

namespace cordmux::record {

struct BranchInput {
	std::vector<std::string> avcc_hexes; // その人のその種類の、すべてのセグメントと黒埋め用のフレームの avcC
	bool any_periodic_backward = false;  // 規則的な後戻り (B フレームの疑い) があったか
	bool any_truncated = false;          // 書き込みの途中で止まったセグメントがあるか
	bool any_material_missing = false;   // 黒埋め用のフレームがない欠落があるか
};

// 作成方法を決める。すべてのセグメントと黒埋め用のフレームの avcC が一致し、規則的な後戻りがなく、途中で止まった
// セグメントも黒埋め用のフレームがない欠落もないときだけ、Remux で録画ファイルを作成する。1 つでも欠ければ、その人のその種類の
// ファイルだけを ProRes で録画ファイルを作成する。
// avcC の比較は、念のためのチェックではなく、問題を見つける唯一の手段である。avcC が食い違うセグメントを concat で
// 並べても、FFmpeg は警告を出さずに終了コード 0 で終わり、コンテナーは 640x360 を示すのに途中から 640x480 になる
// ファイルができる。
inline FinalizeBranch decide_branch(const BranchInput &in)
{
	if (in.any_periodic_backward || in.any_truncated || in.any_material_missing)
		return FinalizeBranch::ProRes;
	if (!avcc_all_equal(in.avcc_hexes))
		return FinalizeBranch::ProRes;
	return FinalizeBranch::Remux;
}

// 録画ログから、その人のその種類の判定に使う値を集める。録画ファイルの作成の引数 (record_args.h) と空き容量の見積もり
// (record_space.h) が同じ値を使うので、ここにまとめる。
// 音声だけの種類 (声) では avcc_hex が空なので、比べるものが 1 つもなく、常に一致する。
inline BranchInput collect_branch_input(const LedgerDocument &doc, const std::string &user_id, StreamKind kind)
{
	BranchInput in;
	for (const LedgerSegmentRow &segment : doc.segments) {
		if (segment.user_id != user_id || segment.kind != kind)
			continue;
		if (segment.truncated)
			in.any_truncated = true;
		if (segment.pts_periodic_backward)
			in.any_periodic_backward = true;
		if (segment.media == MediaKind::Video && !segment.avcc_hex.empty())
			in.avcc_hexes.push_back(segment.avcc_hex);
	}

	// 黒埋め用のフレームは参加者ごとにある (録画ログの black の行は種類を持たない)。画面共有の映像があるときだけ見る。
	bool has_video = false;
	for (const LedgerSegmentRow &segment : doc.segments) {
		if (segment.user_id == user_id && segment.kind == kind && segment.media == MediaKind::Video) {
			has_video = true;
			break;
		}
	}
	if (!has_video)
		return in;

	bool has_material = false;
	for (const LedgerBlackRow &black : doc.blacks) {
		if (black.user_id != user_id)
			continue;
		has_material = true;
		if (!black.avcc_hex.empty())
			in.avcc_hexes.push_back(black.avcc_hex);
	}
	if (!has_material)
		in.any_material_missing = true;
	return in;
}

// 録画ファイルの作成で作るファイル 1 つ (録画ログの finalize の行と同じ単位。種類は StreamKind で持つ)。
struct OutputKey {
	std::string user_id;
	StreamKind kind = StreamKind::Screen;
};

inline bool operator==(const OutputKey &a, const OutputKey &b)
{
	return a.user_id == b.user_id && a.kind == b.kind;
}

// 録画ログにセグメントがある (user_id, 種類) の組を、最初に出た順に並べる。一度も画面共有をしなかった人には、画面共有の
// 録画ファイル (「alice 画面共有 2000-01-01 00-00-00.mov」の形) を作らない。
inline std::vector<OutputKey> finalize_outputs(const LedgerDocument &doc)
{
	std::vector<OutputKey> outputs;
	for (const LedgerSegmentRow &segment : doc.segments) {
		OutputKey key;
		key.user_id = segment.user_id;
		key.kind = segment.kind;
		bool found = false;
		for (const OutputKey &existing : outputs) {
			if (existing == key) {
				found = true;
				break;
			}
		}
		if (!found)
			outputs.push_back(key);
	}
	return outputs;
}

struct DominantSize {
	int width = 0;
	int height = 0;
};

// ProRes で録画ファイルを作成するときにそろえる解像度。その人のその種類のセグメントのうち、長さの合計が最も長い解像度を
// 選ぶ。ほかの人のセグメントは見ない。別の人の解像度にそろえると、その人の映像が拡大か縮小されて画質が落ちるためである。
// 映像のセグメントが 1 つもなければ 0 を返す。長さが同じ解像度が並んだときは、ピクセル数の多いほうを選ぶ。小さいほうに
// そろえると画質が落ちるためである。
inline DominantSize dominant_size(const std::vector<LedgerSegmentRow> &segments, const std::string &user_id,
				  StreamKind kind)
{
	struct Entry {
		DominantSize size;
		std::int64_t ticks = 0;
	};
	std::vector<Entry> entries;

	for (const LedgerSegmentRow &segment : segments) {
		if (segment.user_id != user_id || segment.kind != kind)
			continue;
		if (segment.media != MediaKind::Video || segment.width <= 0 || segment.height <= 0)
			continue;
		bool found = false;
		for (Entry &entry : entries) {
			if (entry.size.width == segment.width && entry.size.height == segment.height) {
				entry.ticks += segment.duration_ticks;
				found = true;
				break;
			}
		}
		if (!found) {
			Entry entry;
			entry.size.width = segment.width;
			entry.size.height = segment.height;
			entry.ticks = segment.duration_ticks;
			entries.push_back(entry);
		}
	}

	DominantSize best;
	std::int64_t best_ticks = -1;
	for (const Entry &entry : entries) {
		const std::int64_t best_pixels = static_cast<std::int64_t>(best.width) * best.height;
		const std::int64_t pixels = static_cast<std::int64_t>(entry.size.width) * entry.size.height;
		// 長さが同じなら、ピクセル数の多いほうを選ぶ。
		if (entry.ticks > best_ticks || (entry.ticks == best_ticks && pixels > best_pixels)) {
			best = entry.size;
			best_ticks = entry.ticks;
		}
	}
	return best;
}

// 声のファイルを書くかどうかの判定の入力。実際に話した人の声だけを残す条件を後で決めるときに、判定の関数だけを
// 修正すればよいように、入力は録画ログに書ける値 (セグメントの数、サンプル数の合計、0 でないサンプルの数、最大の振幅) に
// 限る。
struct VoiceWriteInput {
	std::int64_t segment_count = 0;
	std::int64_t sample_total = 0;
	std::int64_t nonzero_samples = 0;
	double peak_amplitude = 0.0;
};

// 声のファイルを書くか。今は入力を読まずに常に true を返す。条件はまだ決まっていない (話していない人の音声の
// トラックに何が届くかを、実際の通話で確認してから決める)。判断がつかないときも false は返さない。話した人の
// ファイルまで失われうるためである。
inline bool should_write_voice(const VoiceWriteInput &in)
{
	(void)in;
	return true;
}

} // namespace cordmux::record

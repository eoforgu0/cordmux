// SPDX-License-Identifier: Apache-2.0

// record/record_branch.h の単体テスト。作成方法の判定、録画ファイルの作成で作るファイルの一覧、ProRes で録画ファイルを作成するときの解像度、
// 声のファイルを書くかどうかの判定を確認する。

#include "doctest.h"

#include "record/record_branch.h"

using cordmux::record::BranchInput;
using cordmux::record::collect_branch_input;
using cordmux::record::decide_branch;
using cordmux::record::dominant_size;
using cordmux::record::DominantSize;
using cordmux::record::finalize_outputs;
using cordmux::record::FinalizeBranch;
using cordmux::record::LedgerBlackRow;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::MediaKind;
using cordmux::record::OutputKey;
using cordmux::record::should_write_voice;
using cordmux::record::StreamKind;
using cordmux::record::VoiceWriteInput;

namespace {

LedgerSegmentRow video(const char *user, const char *file, const char *avcc, int width, int height,
		       std::int64_t ticks)
{
	LedgerSegmentRow row;
	row.user_id = user;
	row.kind = StreamKind::Screen;
	row.media = MediaKind::Video;
	row.file = file;
	row.avcc_hex = avcc;
	row.width = width;
	row.height = height;
	row.timescale = 60000;
	row.duration_ticks = ticks;
	return row;
}

} // namespace

TEST_CASE("すべての条件がそろっていれば Remux で録画ファイルを作成する")
{
	BranchInput in;
	in.avcc_hexes = {"aa", "aa", "aa"};
	CHECK(decide_branch(in) == FinalizeBranch::Remux);
	CHECK(decide_branch(BranchInput{}) == FinalizeBranch::Remux);
}

TEST_CASE("4 つの条件のどれか 1 つでも欠ければ ProRes で録画ファイルを作成する")
{
	BranchInput mismatch;
	mismatch.avcc_hexes = {"aa", "bb"};
	CHECK(decide_branch(mismatch) == FinalizeBranch::ProRes);

	BranchInput periodic;
	periodic.avcc_hexes = {"aa"};
	periodic.any_periodic_backward = true;
	CHECK(decide_branch(periodic) == FinalizeBranch::ProRes);

	BranchInput truncated;
	truncated.avcc_hexes = {"aa"};
	truncated.any_truncated = true;
	CHECK(decide_branch(truncated) == FinalizeBranch::ProRes);

	BranchInput missing;
	missing.avcc_hexes = {"aa"};
	missing.any_material_missing = true;
	CHECK(decide_branch(missing) == FinalizeBranch::ProRes);
}

TEST_CASE("録画ログから判定の値を集めると、セグメントと黒埋め用のフレームの avcC が両方入る")
{
	LedgerDocument doc;
	doc.segments.push_back(video("1111", "segments/a.mov", "aa", 1280, 720, 60000));
	doc.segments.push_back(video("1111", "segments/b.mov", "aa", 1280, 720, 60000));
	doc.segments.push_back(video("2222", "segments/c.mov", "cc", 640, 360, 60000));
	LedgerBlackRow black;
	black.user_id = "1111";
	black.file = "black/1111_e1.mov";
	black.avcc_hex = "aa";
	doc.blacks.push_back(black);

	const BranchInput in = collect_branch_input(doc, "1111", StreamKind::Screen);
	CHECK(in.avcc_hexes.size() == 3); // セグメント 2 つ + 黒埋め用のフレーム 1 つ。別の人のセグメントは入らない
	CHECK_FALSE(in.any_material_missing);
	CHECK(decide_branch(in) == FinalizeBranch::Remux);
}

TEST_CASE("黒埋め用のフレームが 1 つもない人は ProRes で録画ファイルを作成する")
{
	LedgerDocument doc;
	doc.segments.push_back(video("1111", "segments/a.mov", "aa", 1280, 720, 60000));

	const BranchInput in = collect_branch_input(doc, "1111", StreamKind::Screen);
	CHECK(in.any_material_missing);
	CHECK(decide_branch(in) == FinalizeBranch::ProRes);
}

TEST_CASE("映像を持たない種類 (声) では、黒埋め用のフレームがなくても Remux で録画ファイルを作成する")
{
	LedgerDocument doc;
	LedgerSegmentRow voice;
	voice.user_id = "1111";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/1111_voice_000001.mov";
	doc.segments.push_back(voice);

	const BranchInput in = collect_branch_input(doc, "1111", StreamKind::Voice);
	CHECK(in.avcc_hexes.empty());
	CHECK_FALSE(in.any_material_missing);
	CHECK(decide_branch(in) == FinalizeBranch::Remux);
}

TEST_CASE("途中で切れたセグメントと規則的な後戻りを、録画ログから読み取る")
{
	LedgerDocument doc;
	LedgerSegmentRow row = video("1111", "segments/a.mov", "aa", 1280, 720, 60000);
	row.truncated = true;
	doc.segments.push_back(row);
	LedgerSegmentRow other = video("1111", "segments/b.mov", "aa", 1280, 720, 60000);
	other.pts_periodic_backward = true;
	doc.segments.push_back(other);
	LedgerBlackRow black;
	black.user_id = "1111";
	black.avcc_hex = "aa";
	doc.blacks.push_back(black);

	const BranchInput in = collect_branch_input(doc, "1111", StreamKind::Screen);
	CHECK(in.any_truncated);
	CHECK(in.any_periodic_backward);
	CHECK(decide_branch(in) == FinalizeBranch::ProRes);
}

TEST_CASE("ProRes で録画ファイルを作成するときの解像度には、長さの合計が最も長い解像度を選ぶ")
{
	std::vector<LedgerSegmentRow> segments;
	segments.push_back(video("1111", "segments/a.mov", "aa", 1920, 1080, 10000));
	segments.push_back(video("1111", "segments/b.mov", "aa", 1280, 720, 60000));
	segments.push_back(video("1111", "segments/c.mov", "aa", 1280, 720, 60000));

	const DominantSize size = dominant_size(segments, "1111", StreamKind::Screen);
	CHECK(size.width == 1280);
	CHECK(size.height == 720);
}

TEST_CASE("長さが同じ解像度が並んだら、画質を落とさないようにピクセル数の多いほうを選ぶ")
{
	std::vector<LedgerSegmentRow> segments;
	segments.push_back(video("1111", "segments/a.mov", "aa", 640, 360, 60000));
	segments.push_back(video("1111", "segments/b.mov", "aa", 1920, 1080, 60000));

	const DominantSize size = dominant_size(segments, "1111", StreamKind::Screen);
	CHECK(size.width == 1920);
	CHECK(size.height == 1080);
}

TEST_CASE("解像度は、その人のその種類のセグメントだけで選び、別の人の解像度にそろえない")
{
	std::vector<LedgerSegmentRow> segments;
	// 2222 の 1920x1080 のほうが長い。1111 の録画ファイルを 1920x1080 に拡大しない。
	segments.push_back(video("1111", "segments/a.mov", "aa", 1280, 720, 60000));
	segments.push_back(video("2222", "segments/b.mov", "bb", 1920, 1080, 600000));

	const DominantSize first = dominant_size(segments, "1111", StreamKind::Screen);
	CHECK(first.width == 1280);
	CHECK(first.height == 720);
	const DominantSize second = dominant_size(segments, "2222", StreamKind::Screen);
	CHECK(second.width == 1920);
	CHECK(second.height == 1080);
	CHECK(dominant_size(segments, "1111", StreamKind::Voice).width == 0);
}

TEST_CASE("映像のセグメントが 1 つもなければ、解像度は 0 になる")
{
	std::vector<LedgerSegmentRow> segments;
	LedgerSegmentRow audio;
	audio.user_id = "1111";
	audio.media = MediaKind::Audio;
	audio.width = 1280;
	audio.height = 720;
	segments.push_back(audio);

	const DominantSize size = dominant_size(segments, "1111", StreamKind::Screen);
	CHECK(size.width == 0);
	CHECK(size.height == 0);
	CHECK(dominant_size({}, "1111", StreamKind::Screen).width == 0);
}

TEST_CASE("録画ファイルの作成で作るファイルは、録画ログにセグメントがある人と種類の組だけになる")
{
	LedgerDocument doc;
	doc.segments.push_back(video("1111", "segments/a.mov", "aa", 1280, 720, 60000));
	LedgerSegmentRow screen_audio = video("1111", "segments/b.mov", "", 0, 0, 60000);
	screen_audio.media = MediaKind::Audio;
	doc.segments.push_back(screen_audio);
	LedgerSegmentRow voice;
	voice.user_id = "2222";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/2222_voice_000001.mov";
	doc.segments.push_back(voice);

	const std::vector<OutputKey> outputs = finalize_outputs(doc);
	REQUIRE(outputs.size() == 2); // 1111 の画面共有 (映像と音声で 1 つ) と、2222 の声
	CHECK(outputs[0].user_id == "1111");
	CHECK(outputs[0].kind == StreamKind::Screen);
	CHECK(outputs[1].user_id == "2222");
	CHECK(outputs[1].kind == StreamKind::Voice);
	CHECK(finalize_outputs(LedgerDocument{}).empty());
}

TEST_CASE("声のファイルを書くかどうかの判定は、今は常に真を返す")
{
	CHECK(should_write_voice(VoiceWriteInput{}));

	VoiceWriteInput silent;
	silent.segment_count = 3;
	silent.sample_total = 48000 * 600;
	silent.nonzero_samples = 0;
	silent.peak_amplitude = 0.0;
	CHECK(should_write_voice(silent)); // 無音でも false を返さない

	VoiceWriteInput spoke;
	spoke.segment_count = 1;
	spoke.sample_total = 48000;
	spoke.nonzero_samples = 48000;
	spoke.peak_amplitude = 0.9;
	CHECK(should_write_voice(spoke));
}

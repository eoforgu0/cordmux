// SPDX-License-Identifier: Apache-2.0

// record/record_ledger.h の単体テスト。録画ログの 5 種類の行を書いて読み、同じ値に戻ることと、壊れた行の扱いを
// 確認する。

#include "doctest.h"

#include "record/record_ledger.h"

#include <string>

using cordmux::record::CloseReason;
using cordmux::record::FinalizeBranch;
using cordmux::record::FinalizeCheck;
using cordmux::record::FinalizeState;
using cordmux::record::GapFill;
using cordmux::record::kLedgerSchema;
using cordmux::record::LedgerBlackRow;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerFinalizeRow;
using cordmux::record::LedgerPersonRow;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::LedgerSessionRow;
using cordmux::record::ledger_schema_supported;
using cordmux::record::MaterialOrigin;
using cordmux::record::MediaKind;
using cordmux::record::parse_ledger;
using cordmux::record::PtsReanchor;
using cordmux::record::StopReason;
using cordmux::record::StreamKind;
using cordmux::record::to_jsonl;
using cordmux::record::TrackKind;

namespace {

LedgerSessionRow sample_session()
{
	LedgerSessionRow row;
	row.session_id = "2000-01-01 00-00-00";
	row.started_at_wall_us = 946'684'800'000'000;
	row.started_at_steady_us = 5'000'000;
	row.ended_at_steady_us = 65'000'000;
	row.stopped_reason = StopReason::TurnedOff;
	row.finalize_state = FinalizeState::Pending;
	row.plugin_version = "0.6.0";
	row.ffmpeg_version = "n7.1.1";
	cordmux::pipeline::DummyInterval interval;
	interval.started_at_us = 10;
	interval.ended_at_us = 20;
	row.dummy_intervals.push_back(interval);
	return row;
}

LedgerSegmentRow sample_segment()
{
	LedgerSegmentRow row;
	row.user_id = "1111";
	row.kind = StreamKind::Screen;
	row.stream_id = 7;
	row.encoder_instance_id = "e1";
	row.file = "segments/1111_screen_video_000001.mov";
	row.media = MediaKind::Video;
	row.avcc_hex = "0142c00a";
	row.width = 1280;
	row.height = 720;
	row.fps_num = 60000;
	row.fps_den = 1001;
	row.timescale = 60000;
	row.first_pts_us = 123;
	row.first_pts_steady_us = 456;
	row.first_pts_wall_us = 789;
	row.last_pts_us = 1000;
	row.sample_count = 300;
	row.duration_ticks = 300300;
	row.bytes = 4567;
	row.starts_with_idr = true;
	row.pts_pushed_count = 3;
	PtsReanchor reanchor;
	reanchor.delta_us = 33334;
	reanchor.at_pts_us = 500;
	reanchor.at_steady_us = 600;
	row.pts_reanchor.push_back(reanchor);
	row.pts_periodic_backward = true;
	row.sps_pps_prepended_count = 2;
	row.au_hash = "abc";
	row.truncated = true;
	row.close_reason = CloseReason::FormatChanged;
	return row;
}

} // namespace

TEST_CASE("session の行は、書いて読むと同じ値に戻る")
{
	const LedgerSessionRow row = sample_session();
	const LedgerDocument doc = parse_ledger(to_jsonl(row) + "\n");

	CHECK(doc.session.schema == kLedgerSchema);
	CHECK(doc.session.session_id == row.session_id);
	CHECK(doc.session.started_at_wall_us == row.started_at_wall_us);
	CHECK(doc.session.started_at_steady_us == row.started_at_steady_us);
	CHECK(doc.session.ended_at_steady_us == row.ended_at_steady_us);
	CHECK(doc.session.stopped_reason == StopReason::TurnedOff);
	CHECK(doc.session.finalize_state == FinalizeState::Pending);
	CHECK(doc.session.plugin_version == "0.6.0");
	CHECK(doc.session.ffmpeg_version == "n7.1.1");
	REQUIRE(doc.session.dummy_intervals.size() == 1);
	CHECK(doc.session.dummy_intervals[0].started_at_us == 10);
	CHECK(doc.session.dummy_intervals[0].ended_at_us == 20);
	CHECK(doc.schema_seen == kLedgerSchema);
	CHECK(doc.skipped_lines == 0);
}

TEST_CASE("segment の行は、書いて読むと同じ値に戻る")
{
	const LedgerSegmentRow row = sample_segment();
	const LedgerDocument doc = parse_ledger(to_jsonl(row));

	REQUIRE(doc.segments.size() == 1);
	const LedgerSegmentRow &read = doc.segments[0];
	CHECK(read.user_id == row.user_id);
	CHECK(read.kind == row.kind);
	CHECK(read.stream_id == row.stream_id);
	CHECK(read.encoder_instance_id == row.encoder_instance_id);
	CHECK(read.file == row.file);
	CHECK(read.media == row.media);
	CHECK(read.avcc_hex == row.avcc_hex);
	CHECK(read.width == row.width);
	CHECK(read.height == row.height);
	CHECK(read.fps_num == row.fps_num);
	CHECK(read.fps_den == row.fps_den);
	CHECK(read.timescale == row.timescale);
	CHECK(read.first_pts_us == row.first_pts_us);
	CHECK(read.first_pts_steady_us == row.first_pts_steady_us);
	CHECK(read.first_pts_wall_us == row.first_pts_wall_us);
	CHECK(read.last_pts_us == row.last_pts_us);
	CHECK(read.sample_count == row.sample_count);
	CHECK(read.duration_ticks == row.duration_ticks);
	CHECK(read.bytes == row.bytes);
	CHECK(read.starts_with_idr);
	CHECK(read.pts_pushed_count == 3);
	REQUIRE(read.pts_reanchor.size() == 1);
	CHECK(read.pts_reanchor[0].delta_us == 33334);
	CHECK(read.pts_reanchor[0].at_pts_us == 500);
	CHECK(read.pts_reanchor[0].at_steady_us == 600);
	CHECK(read.pts_periodic_backward);
	CHECK(read.sps_pps_prepended_count == 2);
	CHECK(read.au_hash == "abc");
	CHECK(read.truncated);
	CHECK(read.close_reason == CloseReason::FormatChanged);
}

TEST_CASE("書き込めなかったセグメントは、キューのあふれと区別して write_failed で残る")
{
	LedgerSegmentRow row;
	row.user_id = "3333";
	row.close_reason = CloseReason::WriteFailed;
	row.truncated = true;
	const std::string line = to_jsonl(row);
	CHECK(line.find("\"close_reason\":\"write_failed\"") != std::string::npos);
	CHECK(line.find("queue_overflow") == std::string::npos);

	const LedgerDocument doc = parse_ledger(line + "\n");
	REQUIRE(doc.segments.size() == 1);
	CHECK(doc.segments.front().close_reason == CloseReason::WriteFailed);
	CHECK(doc.segments.front().truncated);
}

TEST_CASE("person、black、finalize の行も、書いて読むと同じ値に戻る")
{
	LedgerPersonRow person;
	person.user_id = "2222";
	person.display_name = "名前";
	person.kinds = {TrackKind::ScreenVideo, TrackKind::ScreenAudio, TrackKind::Voice};

	LedgerBlackRow black;
	black.user_id = "2222";
	black.encoder_instance_id = "e2";
	black.file = "black/2222_e2.mov";
	black.avcc_hex = "0142c00a";
	black.width = 1280;
	black.height = 720;
	black.fps_num = 60;
	black.fps_den = 1;
	black.frame_count = 60;
	black.duration_ticks = 60000;
	black.origin = MaterialOrigin::RecordingTurnedOn;
	black.first_frame_delay_us = 123456;

	LedgerFinalizeRow finalize;
	finalize.user_id = "2222";
	finalize.kind = StreamKind::Voice;
	finalize.branch = FinalizeBranch::ProRes;
	finalize.output_file = "名前 声 2000-01-01 00-00-00.mov";
	finalize.args = {"-xerror", "-i", "concat.txt"};
	finalize.exit_code = 1;
	finalize.output_duration_ticks = 60000;
	finalize.output_timescale = 60000;
	finalize.check = FinalizeCheck::DurationMismatch;
	GapFill gap;
	gap.start_us = 1000;
	gap.duration_us = 2000;
	gap.repeat_count = 3;
	gap.remainder_ticks = 500;
	gap.black_encoder_instance_id = "e2";
	finalize.gaps.push_back(gap);

	const std::string text = to_jsonl(person) + "\n" + to_jsonl(black) + "\n" + to_jsonl(finalize) + "\n";
	const LedgerDocument doc = parse_ledger(text);

	REQUIRE(doc.people.size() == 1);
	CHECK(doc.people[0].display_name == "名前");
	REQUIRE(doc.people[0].kinds.size() == 3);
	CHECK(doc.people[0].kinds[2] == TrackKind::Voice);

	REQUIRE(doc.blacks.size() == 1);
	CHECK(doc.blacks[0].file == "black/2222_e2.mov");
	CHECK(doc.blacks[0].frame_count == 60);
	CHECK(doc.blacks[0].origin == MaterialOrigin::RecordingTurnedOn);
	CHECK(doc.blacks[0].first_frame_delay_us == 123456);

	REQUIRE(doc.finalizes.size() == 1);
	CHECK(doc.finalizes[0].kind == StreamKind::Voice);
	CHECK(doc.finalizes[0].branch == FinalizeBranch::ProRes);
	CHECK(doc.finalizes[0].args.size() == 3);
	CHECK(doc.finalizes[0].exit_code == 1);
	CHECK(doc.finalizes[0].check == FinalizeCheck::DurationMismatch);
	REQUIRE(doc.finalizes[0].gaps.size() == 1);
	CHECK(doc.finalizes[0].gaps[0].repeat_count == 3);
	CHECK(doc.finalizes[0].gaps[0].black_encoder_instance_id == "e2");
}

TEST_CASE("書き出した行は改行を含まない")
{
	CHECK(to_jsonl(sample_session()).find('\n') == std::string::npos);
	CHECK(to_jsonl(sample_segment()).find('\n') == std::string::npos);
}

TEST_CASE("書き込みの途中で切れた末尾の 1 行は、飛ばして数える")
{
	std::string text = to_jsonl(sample_session()) + "\n" + to_jsonl(sample_segment()) + "\n";
	const std::string torn = to_jsonl(sample_segment());
	text += torn.substr(0, torn.size() / 2); // 書き込みの途中で止まった形

	const LedgerDocument doc = parse_ledger(text);
	CHECK(doc.segments.size() == 1);
	CHECK(doc.skipped_lines == 1);
	CHECK(doc.session.session_id == "2000-01-01 00-00-00");
}

TEST_CASE("種類を示さない行と、壊れた行も飛ばして数える")
{
	const std::string text = std::string("{\"type\":\"unknown\"}\n") + "not json at all\n" + "[1,2,3]\n" +
				 to_jsonl(sample_segment()) + "\n";
	const LedgerDocument doc = parse_ledger(text);
	CHECK(doc.segments.size() == 1);
	CHECK(doc.skipped_lines == 3);
}

TEST_CASE("同じものを指す行が 2 回出たら後の行で上書きし、並びは最初に出た位置のままにする")
{
	LedgerSegmentRow opened = sample_segment();
	opened.truncated = false;
	opened.duration_ticks = 0;
	opened.close_reason = CloseReason::RecordingToggled;

	LedgerSegmentRow other = sample_segment();
	other.file = "segments/1111_screen_video_000002.mov";

	LedgerSegmentRow closed = sample_segment();
	closed.duration_ticks = 999;

	const std::string text = to_jsonl(opened) + "\n" + to_jsonl(other) + "\n" + to_jsonl(closed) + "\n";
	const LedgerDocument doc = parse_ledger(text);

	REQUIRE(doc.segments.size() == 2);
	CHECK(doc.segments[0].file == opened.file); // 最初に出た位置のまま
	CHECK(doc.segments[0].duration_ticks == 999); // 中身は後の行
	CHECK(doc.segments[1].file == other.file);
}

TEST_CASE("person の行は、表示名が読めたときの行で上書きされる")
{
	LedgerPersonRow first;
	first.user_id = "3333";
	first.display_name = "";
	LedgerPersonRow later;
	later.user_id = "3333";
	later.display_name = "後から読めた名前";

	const LedgerDocument doc = parse_ledger(to_jsonl(first) + "\n" + to_jsonl(later) + "\n");
	REQUIRE(doc.people.size() == 1);
	CHECK(doc.people[0].display_name == "後から読めた名前");
}

TEST_CASE("空の録画ログ、空行、行末の CR を正しく扱う")
{
	CHECK(parse_ledger("").segments.empty());
	CHECK(parse_ledger("\n\n\n").skipped_lines == 0);

	const std::string text = to_jsonl(sample_segment()) + "\r\n";
	const LedgerDocument doc = parse_ledger(text);
	CHECK(doc.segments.size() == 1);
	CHECK(doc.skipped_lines == 0);
}

TEST_CASE("録画ログの形式は、現在のバージョンだけを読める")
{
	CHECK(ledger_schema_supported(kLedgerSchema));
	CHECK_FALSE(ledger_schema_supported(0));
	CHECK_FALSE(ledger_schema_supported(kLedgerSchema + 1));
	// schema を持たない録画ログは 0 として読む (キーがないことを、バージョン 1 と区別する)。
	CHECK(parse_ledger("{\"type\":\"session\"}").schema_seen == 0);
}

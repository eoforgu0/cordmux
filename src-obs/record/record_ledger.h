// SPDX-License-Identifier: Apache-2.0

// 録画ログ (JSONL) を書き出し、読み込む。録画ファイルの作成はメディアのファイルを調べずに、録画ログだけから FFmpeg の引数を
// 作るので、録画ファイルの作成に要る値はすべて録画ログに書く。
// 行の種類とフィールドは docs/architecture.md の「セグメントファイルと録画ログ」にある。
// libobs、Qt、avformat に依存しない。nlohmann/json は、config/ や ws/ と同じく外部のライブラリとして
// 読み込むだけである。

#pragma once

// nlohmann/json は外部のライブラリなので、そのヘッダーが出す警告は抑える (config/settings.h と同じ)。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "record/record_segment.h"
#include "record/record_types.h"

namespace cordmux::record {

// ---------------------------------------------------------------------------
// 列挙の値の名前。録画ログに書き、test/tools/obs-run の判定スクリプト (ledger_check.py など) も読むので、
// 変えるときは判定スクリプトも修正する。
// ---------------------------------------------------------------------------

inline const char *close_reason_name(CloseReason reason)
{
	switch (reason) {
	case CloseReason::Disconnected:
		return "disconnected";
	case CloseReason::StreamGone:
		return "stream_gone";
	case CloseReason::FormatChanged:
		return "format_changed";
	case CloseReason::ClockJump:
		return "clock_jump";
	case CloseReason::RecordingToggled:
		return "recording_toggled";
	case CloseReason::QueueOverflow:
		return "queue_overflow";
	case CloseReason::WriteFailed:
		return "write_failed";
	case CloseReason::NotClosed:
		return "not_closed";
	}
	return "recording_toggled";
}

inline CloseReason close_reason_from_name(const std::string &name)
{
	if (name == "disconnected")
		return CloseReason::Disconnected;
	if (name == "stream_gone")
		return CloseReason::StreamGone;
	if (name == "format_changed")
		return CloseReason::FormatChanged;
	if (name == "clock_jump")
		return CloseReason::ClockJump;
	if (name == "queue_overflow")
		return CloseReason::QueueOverflow;
	if (name == "write_failed")
		return CloseReason::WriteFailed;
	if (name == "not_closed")
		return CloseReason::NotClosed;
	return CloseReason::RecordingToggled;
}

inline const char *finalize_state_name(FinalizeState state)
{
	switch (state) {
	case FinalizeState::NotStarted:
		return "not_started";
	case FinalizeState::Running:
		return "running";
	case FinalizeState::Done:
		return "done";
	case FinalizeState::Pending:
		return "pending";
	case FinalizeState::Failed:
		return "failed";
	}
	return "not_started";
}

inline FinalizeState finalize_state_from_name(const std::string &name)
{
	if (name == "running")
		return FinalizeState::Running;
	if (name == "done")
		return FinalizeState::Done;
	if (name == "pending")
		return FinalizeState::Pending;
	if (name == "failed")
		return FinalizeState::Failed;
	return FinalizeState::NotStarted;
}

inline const char *finalize_branch_name(FinalizeBranch branch)
{
	return branch == FinalizeBranch::ProRes ? "prores" : "remux";
}

inline FinalizeBranch finalize_branch_from_name(const std::string &name)
{
	return name == "prores" ? FinalizeBranch::ProRes : FinalizeBranch::Remux;
}

inline const char *material_origin_name(MaterialOrigin origin)
{
	switch (origin) {
	case MaterialOrigin::EncoderCreated:
		return "encoder_created";
	case MaterialOrigin::RecordingTurnedOn:
		return "recording_turned_on";
	case MaterialOrigin::EncoderTeardown:
		return "encoder_teardown";
	}
	return "encoder_created";
}

inline MaterialOrigin material_origin_from_name(const std::string &name)
{
	if (name == "recording_turned_on")
		return MaterialOrigin::RecordingTurnedOn;
	if (name == "encoder_teardown")
		return MaterialOrigin::EncoderTeardown;
	return MaterialOrigin::EncoderCreated;
}

inline const char *stop_reason_name(StopReason reason)
{
	switch (reason) {
	case StopReason::StillRunning:
		return "still_running";
	case StopReason::TurnedOff:
		return "turned_off";
	case StopReason::ObsExit:
		return "obs_exit";
	case StopReason::SpaceExhausted:
		return "space_exhausted";
	case StopReason::Interrupted:
		return "interrupted";
	}
	return "still_running";
}

inline StopReason stop_reason_from_name(const std::string &name)
{
	if (name == "turned_off")
		return StopReason::TurnedOff;
	if (name == "obs_exit")
		return StopReason::ObsExit;
	if (name == "space_exhausted")
		return StopReason::SpaceExhausted;
	if (name == "interrupted")
		return StopReason::Interrupted;
	return StopReason::StillRunning;
}

inline const char *finalize_check_name(FinalizeCheck check)
{
	switch (check) {
	case FinalizeCheck::NotChecked:
		return "not_checked";
	case FinalizeCheck::Matched:
		return "matched";
	case FinalizeCheck::DurationMismatch:
		return "duration_mismatch";
	case FinalizeCheck::ProcessFailed:
		return "process_failed";
	case FinalizeCheck::SpaceExhausted:
		return "space_exhausted";
	}
	return "not_checked";
}

inline FinalizeCheck finalize_check_from_name(const std::string &name)
{
	if (name == "matched")
		return FinalizeCheck::Matched;
	if (name == "duration_mismatch")
		return FinalizeCheck::DurationMismatch;
	if (name == "process_failed")
		return FinalizeCheck::ProcessFailed;
	if (name == "space_exhausted")
		return FinalizeCheck::SpaceExhausted;
	return FinalizeCheck::NotChecked;
}

inline const char *stream_kind_name(StreamKind kind)
{
	return kind == StreamKind::Voice ? "voice" : "screen";
}

inline StreamKind stream_kind_from_name(const std::string &name)
{
	return name == "voice" ? StreamKind::Voice : StreamKind::Screen;
}

inline const char *media_kind_name(MediaKind media)
{
	return media == MediaKind::Audio ? "audio" : "video";
}

inline MediaKind media_kind_from_name(const std::string &name)
{
	return name == "audio" ? MediaKind::Audio : MediaKind::Video;
}

// ---------------------------------------------------------------------------
// 1 行の書き出し
// ---------------------------------------------------------------------------

namespace detail {

// 壊れた UTF-8 が混ざっていても、例外を投げずに書く (不正なバイトは置き換える)。nlohmann/json の既定は例外を投げるが、
// 表示名に壊れた UTF-8 が混ざっただけで録画を止めるわけにはいかない。
inline std::string dump_line(const nlohmann::json &j)
{
	return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline nlohmann::json reanchor_to_json(const PtsReanchor &reanchor)
{
	nlohmann::json j = nlohmann::json::object();
	j["delta_us"] = reanchor.delta_us;
	j["at_pts_us"] = reanchor.at_pts_us;
	j["at_steady_us"] = reanchor.at_steady_us;
	return j;
}

inline nlohmann::json gap_to_json(const GapFill &gap)
{
	nlohmann::json j = nlohmann::json::object();
	j["start_us"] = gap.start_us;
	j["duration_us"] = gap.duration_us;
	j["repeat_count"] = gap.repeat_count;
	j["remainder_ticks"] = gap.remainder_ticks;
	j["black_encoder_instance_id"] = gap.black_encoder_instance_id;
	return j;
}

inline std::string read_string(const nlohmann::json &j, const char *key)
{
	const auto it = j.find(key);
	return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

inline std::int64_t read_i64(const nlohmann::json &j, const char *key)
{
	const auto it = j.find(key);
	return (it != j.end() && it->is_number_integer()) ? it->get<std::int64_t>() : 0;
}

inline int read_int(const nlohmann::json &j, const char *key)
{
	return static_cast<int>(read_i64(j, key));
}

inline bool read_bool(const nlohmann::json &j, const char *key)
{
	const auto it = j.find(key);
	return (it != j.end() && it->is_boolean()) ? it->get<bool>() : false;
}

} // namespace detail

// 行を、`type` で種類を示す 1 行の JSON にする (下の 5 つとも同じ)。行末の改行は付けない (書き込む側が付ける)。
// 例外を投げない (壊れた UTF-8 は置き換える)。
inline std::string to_jsonl(const LedgerSessionRow &row)
{
	nlohmann::json j = nlohmann::json::object();
	j["type"] = "session";
	j["schema"] = row.schema;
	j["session_id"] = row.session_id;
	j["started_at_wall_us"] = row.started_at_wall_us;
	j["started_at_steady_us"] = row.started_at_steady_us;
	j["ended_at_steady_us"] = row.ended_at_steady_us;
	j["stopped_reason"] = stop_reason_name(row.stopped_reason);
	j["finalize_state"] = finalize_state_name(row.finalize_state);
	j["plugin_version"] = row.plugin_version;
	j["ffmpeg_version"] = row.ffmpeg_version;
	nlohmann::json intervals = nlohmann::json::array();
	for (const pipeline::DummyInterval &interval : row.dummy_intervals) {
		nlohmann::json one = nlohmann::json::object();
		one["started_at_us"] = interval.started_at_us;
		one["ended_at_us"] = interval.ended_at_us;
		intervals.push_back(one);
	}
	j["dummy_intervals"] = intervals;
	return detail::dump_line(j);
}

inline std::string to_jsonl(const LedgerPersonRow &row)
{
	nlohmann::json j = nlohmann::json::object();
	j["type"] = "person";
	j["user_id"] = row.user_id;
	j["display_name"] = row.display_name;
	nlohmann::json kinds = nlohmann::json::array();
	for (const TrackKind track : row.kinds)
		kinds.push_back(track_name(track));
	j["kinds"] = kinds;
	return detail::dump_line(j);
}

inline std::string to_jsonl(const LedgerSegmentRow &row)
{
	nlohmann::json j = nlohmann::json::object();
	j["type"] = "segment";
	j["user_id"] = row.user_id;
	j["kind"] = stream_kind_name(row.kind);
	j["stream_id"] = row.stream_id;
	j["encoder_instance_id"] = row.encoder_instance_id;
	j["file"] = row.file;
	j["media"] = media_kind_name(row.media);
	j["avcc_hex"] = row.avcc_hex;
	j["width"] = row.width;
	j["height"] = row.height;
	j["fps_num"] = row.fps_num;
	j["fps_den"] = row.fps_den;
	j["timescale"] = row.timescale;
	j["first_pts_us"] = row.first_pts_us;
	j["first_pts_steady_us"] = row.first_pts_steady_us;
	j["first_pts_wall_us"] = row.first_pts_wall_us;
	j["last_pts_us"] = row.last_pts_us;
	j["sample_count"] = row.sample_count;
	j["duration_ticks"] = row.duration_ticks;
	j["bytes"] = row.bytes;
	j["starts_with_idr"] = row.starts_with_idr;
	j["pts_pushed_count"] = row.pts_pushed_count;
	nlohmann::json reanchors = nlohmann::json::array();
	for (const PtsReanchor &reanchor : row.pts_reanchor)
		reanchors.push_back(detail::reanchor_to_json(reanchor));
	j["pts_reanchor"] = reanchors;
	j["pts_periodic_backward"] = row.pts_periodic_backward;
	j["sps_pps_prepended_count"] = row.sps_pps_prepended_count;
	j["au_hash"] = row.au_hash;
	j["truncated"] = row.truncated;
	j["close_reason"] = close_reason_name(row.close_reason);
	j["dropped_tail_bytes"] = row.dropped_tail_bytes;
	return detail::dump_line(j);
}

inline std::string to_jsonl(const LedgerBlackRow &row)
{
	nlohmann::json j = nlohmann::json::object();
	j["type"] = "black";
	j["user_id"] = row.user_id;
	j["encoder_instance_id"] = row.encoder_instance_id;
	j["file"] = row.file;
	j["avcc_hex"] = row.avcc_hex;
	j["width"] = row.width;
	j["height"] = row.height;
	j["fps_num"] = row.fps_num;
	j["fps_den"] = row.fps_den;
	j["frame_count"] = row.frame_count;
	j["duration_ticks"] = row.duration_ticks;
	j["origin"] = material_origin_name(row.origin);
	j["first_frame_delay_us"] = row.first_frame_delay_us;
	return detail::dump_line(j);
}

inline std::string to_jsonl(const LedgerFinalizeRow &row)
{
	nlohmann::json j = nlohmann::json::object();
	j["type"] = "finalize";
	j["user_id"] = row.user_id;
	j["kind"] = stream_kind_name(row.kind);
	j["branch"] = finalize_branch_name(row.branch);
	j["output_file"] = row.output_file;
	nlohmann::json args = nlohmann::json::array();
	for (const std::string &arg : row.args)
		args.push_back(arg);
	j["args"] = args;
	j["exit_code"] = row.exit_code;
	j["output_duration_ticks"] = row.output_duration_ticks;
	j["output_timescale"] = row.output_timescale;
	j["check"] = finalize_check_name(row.check);
	nlohmann::json gaps = nlohmann::json::array();
	for (const GapFill &gap : row.gaps)
		gaps.push_back(detail::gap_to_json(gap));
	j["gaps"] = gaps;
	return detail::dump_line(j);
}

// ---------------------------------------------------------------------------
// 読み込み
// ---------------------------------------------------------------------------

struct LedgerDocument {
	LedgerSessionRow session;
	std::vector<LedgerPersonRow> people;
	std::vector<LedgerSegmentRow> segments;
	std::vector<LedgerBlackRow> blacks;
	std::vector<LedgerFinalizeRow> finalizes;
	std::int64_t skipped_lines = 0; // 読めなかった行と、種類を示さない行の数
	std::int64_t schema_seen = 0;   // 読めた session の行の schema (session の行がなければ 0)
};

// 次の起動の後に録画ファイルの作成を再開するとき、この形式のバージョンの録画ログを読めるか。
inline bool ledger_schema_supported(int schema)
{
	return schema >= 1 && schema <= kLedgerSchema;
}

namespace detail {

inline LedgerSessionRow session_from_json(const nlohmann::json &j)
{
	LedgerSessionRow row;
	row.schema = read_int(j, "schema");
	row.session_id = read_string(j, "session_id");
	row.started_at_wall_us = read_i64(j, "started_at_wall_us");
	row.started_at_steady_us = read_i64(j, "started_at_steady_us");
	row.ended_at_steady_us = read_i64(j, "ended_at_steady_us");
	row.stopped_reason = stop_reason_from_name(read_string(j, "stopped_reason"));
	row.finalize_state = finalize_state_from_name(read_string(j, "finalize_state"));
	row.plugin_version = read_string(j, "plugin_version");
	row.ffmpeg_version = read_string(j, "ffmpeg_version");
	const auto it = j.find("dummy_intervals");
	if (it != j.end() && it->is_array()) {
		for (const nlohmann::json &one : *it) {
			if (!one.is_object())
				continue;
			pipeline::DummyInterval interval;
			interval.started_at_us = read_i64(one, "started_at_us");
			interval.ended_at_us = read_i64(one, "ended_at_us");
			row.dummy_intervals.push_back(interval);
		}
	}
	return row;
}

inline LedgerPersonRow person_from_json(const nlohmann::json &j)
{
	LedgerPersonRow row;
	row.user_id = read_string(j, "user_id");
	row.display_name = read_string(j, "display_name");
	const auto it = j.find("kinds");
	if (it != j.end() && it->is_array()) {
		for (const nlohmann::json &one : *it) {
			if (!one.is_string())
				continue;
			const std::optional<TrackKind> track = track_kind_from_name(one.get<std::string>());
			if (track.has_value())
				row.kinds.push_back(*track);
		}
	}
	return row;
}

inline LedgerSegmentRow segment_from_json(const nlohmann::json &j)
{
	LedgerSegmentRow row;
	row.user_id = read_string(j, "user_id");
	row.kind = stream_kind_from_name(read_string(j, "kind"));
	row.stream_id = static_cast<std::uint32_t>(read_i64(j, "stream_id"));
	row.encoder_instance_id = read_string(j, "encoder_instance_id");
	row.file = read_string(j, "file");
	row.media = media_kind_from_name(read_string(j, "media"));
	row.avcc_hex = read_string(j, "avcc_hex");
	row.width = read_int(j, "width");
	row.height = read_int(j, "height");
	row.fps_num = read_int(j, "fps_num");
	row.fps_den = read_int(j, "fps_den");
	row.timescale = read_int(j, "timescale");
	row.first_pts_us = read_i64(j, "first_pts_us");
	row.first_pts_steady_us = read_i64(j, "first_pts_steady_us");
	row.first_pts_wall_us = read_i64(j, "first_pts_wall_us");
	row.last_pts_us = read_i64(j, "last_pts_us");
	row.sample_count = read_i64(j, "sample_count");
	row.duration_ticks = read_i64(j, "duration_ticks");
	row.bytes = read_i64(j, "bytes");
	row.starts_with_idr = read_bool(j, "starts_with_idr");
	row.pts_pushed_count = read_i64(j, "pts_pushed_count");
	const auto it = j.find("pts_reanchor");
	if (it != j.end() && it->is_array()) {
		for (const nlohmann::json &one : *it) {
			if (!one.is_object())
				continue;
			PtsReanchor reanchor;
			reanchor.delta_us = read_i64(one, "delta_us");
			reanchor.at_pts_us = read_i64(one, "at_pts_us");
			reanchor.at_steady_us = read_i64(one, "at_steady_us");
			row.pts_reanchor.push_back(reanchor);
		}
	}
	row.pts_periodic_backward = read_bool(j, "pts_periodic_backward");
	row.sps_pps_prepended_count = read_i64(j, "sps_pps_prepended_count");
	row.au_hash = read_string(j, "au_hash");
	row.truncated = read_bool(j, "truncated");
	row.close_reason = close_reason_from_name(read_string(j, "close_reason"));
	row.dropped_tail_bytes = read_i64(j, "dropped_tail_bytes"); // このフィールドがない行 (古いバージョン) は 0
	return row;
}

inline LedgerBlackRow black_from_json(const nlohmann::json &j)
{
	LedgerBlackRow row;
	row.user_id = read_string(j, "user_id");
	row.encoder_instance_id = read_string(j, "encoder_instance_id");
	row.file = read_string(j, "file");
	row.avcc_hex = read_string(j, "avcc_hex");
	row.width = read_int(j, "width");
	row.height = read_int(j, "height");
	row.fps_num = read_int(j, "fps_num");
	row.fps_den = read_int(j, "fps_den");
	row.frame_count = read_i64(j, "frame_count");
	row.duration_ticks = read_i64(j, "duration_ticks");
	row.origin = material_origin_from_name(read_string(j, "origin"));
	row.first_frame_delay_us = read_i64(j, "first_frame_delay_us");
	return row;
}

inline LedgerFinalizeRow finalize_from_json(const nlohmann::json &j)
{
	LedgerFinalizeRow row;
	row.user_id = read_string(j, "user_id");
	row.kind = stream_kind_from_name(read_string(j, "kind"));
	row.branch = finalize_branch_from_name(read_string(j, "branch"));
	row.output_file = read_string(j, "output_file");
	const auto args_it = j.find("args");
	if (args_it != j.end() && args_it->is_array()) {
		for (const nlohmann::json &one : *args_it) {
			if (one.is_string())
				row.args.push_back(one.get<std::string>());
		}
	}
	row.exit_code = read_int(j, "exit_code");
	row.output_duration_ticks = read_i64(j, "output_duration_ticks");
	row.output_timescale = read_int(j, "output_timescale");
	row.check = finalize_check_from_name(read_string(j, "check"));
	const auto gaps_it = j.find("gaps");
	if (gaps_it != j.end() && gaps_it->is_array()) {
		for (const nlohmann::json &one : *gaps_it) {
			if (!one.is_object())
				continue;
			GapFill gap;
			gap.start_us = read_i64(one, "start_us");
			gap.duration_us = read_i64(one, "duration_us");
			gap.repeat_count = read_int(one, "repeat_count");
			gap.remainder_ticks = read_i64(one, "remainder_ticks");
			gap.black_encoder_instance_id = read_string(one, "black_encoder_instance_id");
			row.gaps.push_back(gap);
		}
	}
	return row;
}

// 同じキーの行は後の行で上書きし、並びは最初に出た位置を保つ (録画ファイルを作成する順が、書いた順のままになる)。
template<typename Row, typename KeyOf> void upsert(std::vector<Row> &rows, Row row, KeyOf key_of)
{
	for (Row &existing : rows) {
		if (key_of(existing) == key_of(row)) {
			existing = std::move(row);
			return;
		}
	}
	rows.push_back(std::move(row));
}

} // namespace detail

// 録画ログを読む。録画ログには追記だけをするので、同じものの行が何度か出る (セグメントは開いたときと閉じたとき、
// 人は表示名が読めたとき、session は録画ファイルの作成の状態が変わったとき)。後の行が前の行を上書きする。キーは、session は
// 1 つだけ、person は `user_id`、segment と black は `file`、finalize は `output_file` である (stream_id は、再接続で
// 別の人に割り当てられるのでキーにしない)。
// 行の種類は `type` で決める。フィールドの並びから推測すると、フィールドが増えるたびに推測が変わるためである。
// 読めなかった行と、種類を示さない行は飛ばして skipped_lines に数える。書き込みの途中で止まった末尾の 1 行も、
// ディスクの不調で壊れた途中の行も同じに扱い、読めた範囲で録画ファイルを作成する。
inline LedgerDocument parse_ledger(std::string_view jsonl)
{
	LedgerDocument doc;

	std::size_t begin = 0;
	while (begin <= jsonl.size()) {
		std::size_t end = jsonl.find('\n', begin);
		const bool last = end == std::string_view::npos;
		if (last)
			end = jsonl.size();

		std::string_view line = jsonl.substr(begin, end - begin);
		begin = end + 1;
		// 行末の CR と空白も除く (ほかのツールが書き足した録画ログも読めるように)。
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.remove_suffix(1);
		if (line.empty()) {
			if (last)
				break;
			continue;
		}

		const nlohmann::json parsed = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
		if (parsed.is_discarded() || !parsed.is_object()) {
			++doc.skipped_lines;
			if (last)
				break;
			continue;
		}

		const std::string type = detail::read_string(parsed, "type");
		if (type == "session") {
			doc.session = detail::session_from_json(parsed);
			doc.schema_seen = doc.session.schema;
		} else if (type == "person") {
			detail::upsert(doc.people, detail::person_from_json(parsed),
				       [](const LedgerPersonRow &row) { return row.user_id; });
		} else if (type == "segment") {
			detail::upsert(doc.segments, detail::segment_from_json(parsed),
				       [](const LedgerSegmentRow &row) { return row.file; });
		} else if (type == "black") {
			detail::upsert(doc.blacks, detail::black_from_json(parsed),
				       [](const LedgerBlackRow &row) { return row.file; });
		} else if (type == "finalize") {
			detail::upsert(doc.finalizes, detail::finalize_from_json(parsed),
				       [](const LedgerFinalizeRow &row) { return row.output_file; });
		} else {
			++doc.skipped_lines;
		}

		if (last)
			break;
	}
	return doc;
}

} // namespace cordmux::record

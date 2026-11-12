// SPDX-License-Identifier: Apache-2.0

// record/record_segment.h の単体テスト。セグメントを閉じる理由の判定と、保存先の中のファイルとフォルダーの名前を
// 確認する。録画が OFF の間に I/O の関数を呼ばないための判定のテストは test_record_gate.cpp にある。

#include "doctest.h"

#include "record/record_segment.h"

using cordmux::record::black_file_name;
using cordmux::record::CloseReason;
using cordmux::record::resolve_name_collisions;
using cordmux::record::sanitize_display_name;
using cordmux::record::segment_cut_reason;
using cordmux::record::segment_file_name;
using cordmux::record::SegmentCutInput;
using cordmux::record::session_dir_name;
using cordmux::record::StreamFormatMemo;
using cordmux::record::StreamKind;
using cordmux::record::track_kind_from_name;
using cordmux::record::track_name;
using cordmux::record::TrackKind;
using cordmux::record::ClockStep;

namespace {

StreamFormatMemo format_720p60()
{
	StreamFormatMemo memo;
	memo.width = 1280;
	memo.height = 720;
	memo.fps_num = 60;
	memo.fps_den = 1;
	return memo;
}

SegmentCutInput running_input()
{
	SegmentCutInput in;
	in.connected = true;
	in.present_in_configure = true;
	in.format = format_720p60();
	in.clock = ClockStep::Forward;
	in.recording_enabled = true;
	in.queue_overflowed = false;
	return in;
}

} // namespace

TEST_CASE("何も起きていなければ、セグメントを閉じない")
{
	CHECK_FALSE(segment_cut_reason(format_720p60(), running_input()).has_value());
}

TEST_CASE("接続が切れたら、セグメントを閉じる")
{
	SegmentCutInput in = running_input();
	in.connected = false;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::Disconnected);
}

TEST_CASE("configure からその stream が消えたら、セグメントを閉じる")
{
	SegmentCutInput in = running_input();
	in.present_in_configure = false;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::StreamGone);
}

TEST_CASE("解像度か fps が変わったら、セグメントを閉じる")
{
	SegmentCutInput in = running_input();
	in.format.width = 1920;
	in.format.height = 1080;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::FormatChanged);

	SegmentCutInput fps = running_input();
	fps.format.fps_num = 30000;
	fps.format.fps_den = 1001;
	CHECK(segment_cut_reason(format_720p60(), fps) == CloseReason::FormatChanged);
}

TEST_CASE("タイムスタンプが不連続になったら、セグメントを閉じる")
{
	SegmentCutInput in = running_input();
	in.clock = ClockStep::Jumped;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::ClockJump);

	// Pushed と Reanchored では、セグメントを閉じない。
	for (const ClockStep step : {ClockStep::Forward, ClockStep::Pushed, ClockStep::Reanchored}) {
		SegmentCutInput kept = running_input();
		kept.clock = step;
		CHECK_FALSE(segment_cut_reason(format_720p60(), kept).has_value());
	}
}

TEST_CASE("録画を OFF にしたら、セグメントを閉じる")
{
	SegmentCutInput in = running_input();
	in.recording_enabled = false;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::RecordingToggled);
}

TEST_CASE("録画のキューがあふれたら、セグメントを閉じる")
{
	SegmentCutInput in = running_input();
	in.queue_overflowed = true;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::QueueOverflow);
}

TEST_CASE("開いた直後で比べる値がないときは、解像度が変わったとみなさない")
{
	StreamFormatMemo unset;
	CHECK_FALSE(segment_cut_reason(unset, running_input()).has_value());
}

TEST_CASE("理由が重なったときは、外側の事情を理由にする")
{
	SegmentCutInput in = running_input();
	in.connected = false;
	in.present_in_configure = false;
	in.recording_enabled = false;
	in.queue_overflowed = true;
	in.clock = ClockStep::Jumped;
	in.format.width = 640;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::Disconnected);

	in.connected = true;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::StreamGone);

	in.present_in_configure = true;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::RecordingToggled);

	in.recording_enabled = true;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::QueueOverflow);

	in.queue_overflowed = false;
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::FormatChanged);

	in.format = format_720p60();
	CHECK(segment_cut_reason(format_720p60(), in) == CloseReason::ClockJump);
}

TEST_CASE("表示するソースや枠の番号が変わっただけでは、セグメントを閉じない (入力に含まれないので、構造で保証される)")
{
	// SegmentCutInput は、user_id も枠の番号も持たない。同じストリームについて枠を並べ替えただけなら、渡す値は何も
	// 変わらないので、閉じることはありえない。
	SegmentCutInput before = running_input();
	SegmentCutInput after = running_input();
	CHECK_FALSE(segment_cut_reason(format_720p60(), before).has_value());
	CHECK_FALSE(segment_cut_reason(format_720p60(), after).has_value());
}

TEST_CASE("セッションのフォルダーの名前は、時差を足した現地の日時で、OBS の録画の既定と同じ形にする")
{
	// 2000-01-01 00:00:00 UTC は、時差が +9 時間なら 09:00 になる。形は OBS の既定 `%CCYY-%MM-%DD %hh-%mm-%ss`。
	CHECK(session_dir_name(946'684'800'000'000, 9 * 3600) == "2000-01-01 09-00-00");
	CHECK(session_dir_name(946'684'800'000'000, 0) == "2000-01-01 00-00-00");
	CHECK(session_dir_name(0, 0) == "1970-01-01 00-00-00");
	// うるう日と、エポックより前の時刻 (時計が狂っている PC) でも桁が崩れない。
	CHECK(session_dir_name(951'868'799'000'000, 0) == "2000-02-29 23-59-59");
	CHECK(session_dir_name(-1'000'000, 0) == "1969-12-31 23-59-59");
	// 同じ名前の 2 つ目には「 (2)」を付ける (前のセッションの録画ログを切り詰めないため)。
	CHECK(cordmux::record::numbered_session_dir_name("2000-01-01 09-00-00", 1) == "2000-01-01 09-00-00");
	CHECK(cordmux::record::numbered_session_dir_name("2000-01-01 09-00-00", 2) == "2000-01-01 09-00-00 (2)");
}

TEST_CASE("録画ごとのフォルダーの名前の形は、日時の名前と「 (2)」付きの名前だけが当てはまる")
{
	using cordmux::record::is_session_dir_name;
	CHECK(is_session_dir_name("2000-01-01 15-18-12"));
	CHECK(is_session_dir_name("2000-01-01 15-18-12 (2)"));
	CHECK(is_session_dir_name("2000-01-01 15-18-12 (100)"));
	CHECK_FALSE(is_session_dir_name(""));
	CHECK_FALSE(is_session_dir_name("cordmux"));
	CHECK_FALSE(is_session_dir_name("2000-01-01 15-18-1"));
	CHECK_FALSE(is_session_dir_name("2000-01-01_15-18-12"));
	CHECK_FALSE(is_session_dir_name("2000-01-01 15-18-12 "));
	CHECK_FALSE(is_session_dir_name("2000-01-01 15-18-12 ()"));
	CHECK_FALSE(is_session_dir_name("2000-01-01 15-18-12 (x)"));
	CHECK_FALSE(is_session_dir_name("2000-01-01 15-18-12 (2"));
	CHECK_FALSE(is_session_dir_name("2000-01-01 15-18-12 backup"));
}

TEST_CASE("録画ファイルの名前は、人の名前、種類の語、セッションの日時を書式に従って並べる")
{
	using cordmux::record::output_file_name_for_session;
	// 既定の書式と語 (en-US.ini と同じ文字列)。
	CHECK(output_file_name_for_session("", "たろう", "Screen Share", "2000-01-01 00-00-00") ==
	      "たろう Screen Share 2000-01-01 00-00-00.mov");
	// ini から渡した書式と語 (ja-JP)。
	CHECK(output_file_name_for_session("%1 %2 %3", "たろう", "画面共有", "2000-01-01 00-00-00") ==
	      "たろう 画面共有 2000-01-01 00-00-00.mov");
	// 並びを変えた書式も、1 回の走査で置き換える (名前の中の %2 を、もう 1 度置き換えない)。
	CHECK(output_file_name_for_session("%3 %1 %2", "a%2b", "声", "2000-01-01 00-00-00") ==
	      "2000-01-01 00-00-00 a%2b 声.mov");
	// 語や書式に使えない文字が入っていても、ファイルを作れる形にする。
	CHECK(output_file_name_for_session("%1: %2 %3", "taro", "A/B", "d") == "taro_ A_B d.mov");
}

TEST_CASE("セグメントと黒埋め用のフレームのファイルの名前は、中間ファイルのフォルダーからの相対パスで返す")
{
	CHECK(segment_file_name("1234", TrackKind::ScreenVideo, 1) == "segments/1234_screen_video_000001.mov");
	CHECK(segment_file_name("1234", TrackKind::ScreenAudio, 42) == "segments/1234_screen_audio_000042.mov");
	CHECK(segment_file_name("1234", TrackKind::Voice, 999999) == "segments/1234_voice_999999.mov");
	// 6 桁を超えても切り詰めない (切り詰めると、名前が重なるため)。
	CHECK(segment_file_name("1234", TrackKind::Voice, 1234567) == "segments/1234_voice_1234567.mov");
	CHECK(black_file_name("1234", "e7") == "black/1234_e7.mov");
}

TEST_CASE("種類の名前は録画ログと保存先で同じ語を使い、名前から種類に戻せる")
{
	for (const TrackKind track : {TrackKind::ScreenVideo, TrackKind::ScreenAudio, TrackKind::Voice})
		CHECK(track_kind_from_name(track_name(track)) == track);
	CHECK_FALSE(track_kind_from_name("screen").has_value());
}

TEST_CASE("表示名のうち使えない文字を置き換え、前後の空白と点を除く")
{
	CHECK(sanitize_display_name("a/b\\c:d*e?f\"g<h>i|j", "u1") == "a_b_c_d_e_f_g_h_i_j");
	CHECK(sanitize_display_name("  . name .  ", "u1") == "name");
	CHECK(sanitize_display_name(std::string("tab\there"), "u1") == "tab_here");
}

TEST_CASE("表示名が空なら、user_id を仮の名前にする")
{
	CHECK(sanitize_display_name("", "112233") == "112233");
	CHECK(sanitize_display_name("   ", "112233") == "112233");
	CHECK(sanitize_display_name("", "") == "unnamed");
}

TEST_CASE("長い表示名は、多バイトの文字の途中で切らない")
{
	// 3 バイトの文字を 30 個 = 90 バイト。上限が 64 バイトなら、21 文字 (63 バイト) で切れる。
	std::string long_name;
	for (int i = 0; i < 30; ++i)
		long_name += "\xE3\x81\x82"; // ひらがなの 1 文字
	const std::string rounded = sanitize_display_name(long_name, "u1");
	CHECK(rounded.size() == 63);
	CHECK(rounded.size() % 3 == 0); // 文字の途中で切れていない
	for (std::size_t i = 0; i < rounded.size(); i += 3)
		CHECK(rounded.compare(i, 3, "\xE3\x81\x82") == 0);
}

TEST_CASE("同じ名前の人が 2 人いたら、両方に user_id の先頭を付ける")
{
	const std::vector<std::string> names = {"same", "other", "same"};
	const std::vector<std::string> ids = {"1111111111", "2222222222", "3333333333"};
	const std::vector<std::string> resolved = resolve_name_collisions(names, ids);
	CHECK(resolved[0] == "same_111111");
	CHECK(resolved[1] == "other");
	CHECK(resolved[2] == "same_333333");
}

TEST_CASE("user_id の先頭でも分かれないときは、user_id を全部使う")
{
	const std::vector<std::string> names = {"same", "same"};
	const std::vector<std::string> ids = {"1234567890", "1234569999"};
	const std::vector<std::string> resolved = resolve_name_collisions(names, ids);
	CHECK(resolved[0] == "same_1234567890");
	CHECK(resolved[1] == "same_1234569999");
}

TEST_CASE("names と user_ids の長さが食い違う呼び出しでは、何も変えない")
{
	const std::vector<std::string> names = {"same", "same"};
	const std::vector<std::string> ids = {"1"};
	CHECK(resolve_name_collisions(names, ids) == names);
}

// SPDX-License-Identifier: Apache-2.0

// config/settings.h (設定の値と、JSON の文字列との変換) のテスト。

#include "doctest.h"

#include "align/audio_drift.h"
#include "config/settings.h"
#include "pipeline/audio_mixer.h"
#include "ledger/observation.h"
#include "record/record_types.h"
#include "sources/decor_params.h"

#include <cmath>
#include <cstdint>
#include <set>
#include <string>

using cordmux::config::AssignMode;
using cordmux::config::DecoderChoice;
using cordmux::config::parse_settings;
using cordmux::config::RecordPcmFormat;
using cordmux::config::Settings;

TEST_CASE("空のオブジェクトを読むと、既定値 (Software、画面共有の補正 0、声の補正 0) になる")
{
	Settings s;
	std::string error;
	CHECK(parse_settings("{}", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK(s.screen_audio_offset_ms == 0);
	CHECK(s.voice_audio_delay_ms == 0);
	CHECK(error.empty());
}

TEST_CASE("decoder のキーがなければ既定値になる")
{
	Settings s;
	CHECK(parse_settings(R"({"other_key": 1})", &s, nullptr));
	CHECK(s.decoder == DecoderChoice::Software);
}

TEST_CASE("decoder に software を書くと Software になる")
{
	Settings s;
	CHECK(parse_settings(R"({"decoder": "software"})", &s, nullptr));
	CHECK(s.decoder == DecoderChoice::Software);
}

TEST_CASE("decoder に hardware を書くと D3D11VA になる")
{
	Settings s;
	CHECK(parse_settings(R"({"decoder": "hardware"})", &s, nullptr));
	CHECK(s.decoder == DecoderChoice::D3D11VA);
}

TEST_CASE("未知のキーは無視する")
{
	Settings s;
	CHECK(parse_settings(R"({"decoder": "hardware", "unknown_future_key": [1, 2, 3]})", &s, nullptr));
	CHECK(s.decoder == DecoderChoice::D3D11VA);
}

TEST_CASE("構文の壊れた JSON は false を返し、既定値に戻して、理由を返す")
{
	Settings s;
	s.decoder = DecoderChoice::D3D11VA; // 既定値に戻ることを見るため、あえて崩しておく
	std::string error;
	CHECK_FALSE(parse_settings("{not json", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software); // 壊れていれば既定値に戻す
	CHECK_FALSE(error.empty());
}

TEST_CASE("オブジェクトでない JSON (配列) は false を返し、既定値に戻す")
{
	Settings s;
	s.decoder = DecoderChoice::D3D11VA;
	std::string error;
	CHECK_FALSE(parse_settings("[1, 2, 3]", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK_FALSE(error.empty());
}

TEST_CASE("オブジェクトでない JSON (数値だけ) も false を返し、既定値に戻す")
{
	Settings s;
	std::string error;
	CHECK_FALSE(parse_settings("42", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK_FALSE(error.empty());
}

TEST_CASE("decoder の値が文字列でなければ (数値)、false を返し、既定値に戻す")
{
	Settings s;
	s.decoder = DecoderChoice::D3D11VA;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"decoder": 1})", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK_FALSE(error.empty());
}

TEST_CASE("decoder の値が未知の文字列なら、false を返し、既定値に戻す")
{
	Settings s;
	s.decoder = DecoderChoice::D3D11VA;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"decoder": "cuda"})", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK_FALSE(error.empty());
}

TEST_CASE("out と error が nullptr でもクラッシュしない")
{
	CHECK(parse_settings(R"({"decoder": "hardware"})", nullptr, nullptr));
	CHECK_FALSE(parse_settings("{not json", nullptr, nullptr));
}

TEST_CASE("空文字列の入力は壊れた JSON として扱う")
{
	Settings s;
	std::string error;
	CHECK_FALSE(parse_settings("", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK_FALSE(error.empty());
}

// ---- 2 つの A/V の補正 ----

TEST_CASE("2 つの補正を両方とも読む")
{
	Settings s;
	std::string error;
	CHECK(parse_settings(R"({"screen_audio_offset_ms": -125, "voice_audio_delay_ms": 60})", &s, &error));
	CHECK(s.screen_audio_offset_ms == -125);
	CHECK(s.voice_audio_delay_ms == 60);
	CHECK(s.decoder == DecoderChoice::Software); // 書いていないキーは既定値のまま
	CHECK(error.empty());
}

TEST_CASE("デコーダーの種類と補正は、同じ JSON で同時に動作する")
{
	Settings s;
	CHECK(parse_settings(R"({"decoder": "hardware", "screen_audio_offset_ms": 33, "voice_audio_delay_ms": 0})", &s,
			     nullptr));
	CHECK(s.decoder == DecoderChoice::D3D11VA);
	CHECK(s.screen_audio_offset_ms == 33);
	CHECK(s.voice_audio_delay_ms == 0);
}

TEST_CASE("補正のキーがなければ既定値 (画面共有 0、声 0) になる")
{
	Settings s;
	CHECK(parse_settings(R"({"decoder": "software"})", &s, nullptr));
	CHECK(s.screen_audio_offset_ms == 0);
	CHECK(s.voice_audio_delay_ms == 0);
}

TEST_CASE("範囲の端の値はそのまま読める (-2000 から 2000、0 から 2000)")
{
	Settings s;
	CHECK(parse_settings(R"({"screen_audio_offset_ms": -2000, "voice_audio_delay_ms": 0})", &s, nullptr));
	CHECK(s.screen_audio_offset_ms == -2000);
	CHECK(s.voice_audio_delay_ms == 0);

	CHECK(parse_settings(R"({"screen_audio_offset_ms": 2000, "voice_audio_delay_ms": 2000})", &s, nullptr));
	CHECK(s.screen_audio_offset_ms == 2000);
	CHECK(s.voice_audio_delay_ms == 2000);
}

TEST_CASE("画面共有の補正が範囲の外なら、false を返し、既定値に戻す")
{
	Settings s;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"screen_audio_offset_ms": 2001})", &s, &error));
	CHECK(s.screen_audio_offset_ms == 0);
	CHECK_FALSE(error.empty());

	CHECK_FALSE(parse_settings(R"({"screen_audio_offset_ms": -2001})", &s, nullptr));
	CHECK(s.screen_audio_offset_ms == 0);
}

TEST_CASE("声の補正は負の値を受け付けない (0 以上)")
{
	Settings s;
	std::string error;
	s.voice_audio_delay_ms = 45;
	CHECK_FALSE(parse_settings(R"({"voice_audio_delay_ms": -1})", &s, &error));
	CHECK(s.voice_audio_delay_ms == 0);
	CHECK_FALSE(error.empty());

	s.voice_audio_delay_ms = 45;
	CHECK_FALSE(parse_settings(R"({"voice_audio_delay_ms": 2001})", &s, nullptr));
	CHECK(s.voice_audio_delay_ms == 0);
}

TEST_CASE("小数、文字列、真偽値、null は整数でないので、false を返し、既定値に戻す")
{
	Settings s;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"screen_audio_offset_ms": 100.5})", &s, &error));
	CHECK(s.screen_audio_offset_ms == 0);
	CHECK_FALSE(error.empty());

	// 100.0 も JSON では小数なので受け付けない (書く人に形をそろえてもらう)。
	CHECK_FALSE(parse_settings(R"({"screen_audio_offset_ms": 100.0})", &s, nullptr));
	CHECK(s.screen_audio_offset_ms == 0);

	s.voice_audio_delay_ms = 45;
	CHECK_FALSE(parse_settings(R"({"voice_audio_delay_ms": "60"})", &s, nullptr));
	CHECK(s.voice_audio_delay_ms == 0);

	s.voice_audio_delay_ms = 45;
	CHECK_FALSE(parse_settings(R"({"voice_audio_delay_ms": true})", &s, nullptr));
	CHECK(s.voice_audio_delay_ms == 0);

	s.voice_audio_delay_ms = 45;
	CHECK_FALSE(parse_settings(R"({"voice_audio_delay_ms": null})", &s, nullptr));
	CHECK(s.voice_audio_delay_ms == 0);
}

TEST_CASE("int64 の範囲を超える大きな数も、範囲の外として拒む")
{
	// 符号なしのまま持っている値を int64 に変換すると負の値になり、範囲の中に見えてしまう。
	Settings s;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"screen_audio_offset_ms": 18446744073709551615})", &s, &error));
	CHECK(s.screen_audio_offset_ms == 0);
	CHECK_FALSE(error.empty());

	s.voice_audio_delay_ms = 45;
	CHECK_FALSE(parse_settings(R"({"voice_audio_delay_ms": 9223372036854775808})", &s, nullptr));
	CHECK(s.voice_audio_delay_ms == 0);
}

TEST_CASE("形が違う値が 1 つでもあれば、ほかのキーも既定値に戻す")
{
	Settings s;
	std::string error;
	s.voice_audio_delay_ms = 45;
	CHECK_FALSE(parse_settings(R"({"decoder": "hardware", "voice_audio_delay_ms": 5000})", &s, &error));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK(s.voice_audio_delay_ms == 0);
	CHECK_FALSE(error.empty());
}

TEST_CASE("補正のキーがあっても、out が nullptr でクラッシュしない")
{
	CHECK(parse_settings(R"({"screen_audio_offset_ms": -10, "voice_audio_delay_ms": 10})", nullptr, nullptr));
	CHECK_FALSE(parse_settings(R"({"voice_audio_delay_ms": -10})", nullptr, nullptr));
}

// ---- 送信側のログを保つ行数 ----

TEST_CASE("送信側のログを保つ行数の既定値は 200")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.js_log_ring_lines == 200);
}

TEST_CASE("送信側のログを保つ行数は設定で変えられる (0 なら 1 行も持たない)")
{
	Settings s;
	CHECK(parse_settings(R"({"js_log_ring_lines": 500})", &s, nullptr));
	CHECK(s.js_log_ring_lines == 500);

	CHECK(parse_settings(R"({"js_log_ring_lines": 0})", &s, nullptr));
	CHECK(s.js_log_ring_lines == 0);

	CHECK(parse_settings(R"({"js_log_ring_lines": 10000})", &s, nullptr));
	CHECK(s.js_log_ring_lines == 10000);
}

TEST_CASE("送信側のログを保つ行数が範囲の外なら、false を返し、既定値に戻す")
{
	Settings s;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"js_log_ring_lines": -1})", &s, &error));
	CHECK(s.js_log_ring_lines == 200);
	CHECK_FALSE(error.empty());

	CHECK_FALSE(parse_settings(R"({"js_log_ring_lines": 10001})", &s, nullptr));
	CHECK(s.js_log_ring_lines == 200);

	CHECK_FALSE(parse_settings(R"({"js_log_ring_lines": "たくさん"})", &s, nullptr));
	CHECK(s.js_log_ring_lines == 200);
}

TEST_CASE("送信側のログを保つ行数が壊れていれば、ほかのキーも既定値に戻す")
{
	Settings s;
	CHECK_FALSE(parse_settings(R"({"decoder": "hardware", "js_log_ring_lines": 99999})", &s, nullptr));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK(s.js_log_ring_lines == 200);
}

// ---- ドックの周期、映像が止まったと判断する長さ、state.json への書き込みの可否 ----

TEST_CASE("ドックの 3 つのキーの既定値")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.dock_poll_ms == 200);
	CHECK(s.video_stall_ms == 2000);
	// 既定値は true (配布用の zip の既定のままで、[Vesktopに追加] が state.json に書く)。
	CHECK(s.allow_state_json_write);
}

TEST_CASE("ドックの周期と、映像が止まったと判断する長さは、設定で変えられる")
{
	Settings s;
	CHECK(parse_settings(R"({"dock_poll_ms": 250, "video_stall_ms": 5000})", &s, nullptr));
	CHECK(s.dock_poll_ms == 250);
	CHECK(s.video_stall_ms == 5000);

	CHECK(parse_settings(R"({"dock_poll_ms": 50, "video_stall_ms": 100})", &s, nullptr));
	CHECK(s.dock_poll_ms == 50);
	CHECK(s.video_stall_ms == 100);

	CHECK(parse_settings(R"({"dock_poll_ms": 60000, "video_stall_ms": 60000})", &s, nullptr));
	CHECK(s.dock_poll_ms == 60000);
	CHECK(s.video_stall_ms == 60000);
}

TEST_CASE("ドックの周期が範囲の外なら、false を返し、既定値に戻す (0 は画面が更新されなくなるので受け付けない)")
{
	Settings s;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"dock_poll_ms": 0})", &s, &error));
	CHECK(s.dock_poll_ms == 200);
	CHECK_FALSE(error.empty());

	CHECK_FALSE(parse_settings(R"({"dock_poll_ms": 60001})", &s, nullptr));
	CHECK(s.dock_poll_ms == 200);

	CHECK_FALSE(parse_settings(R"({"video_stall_ms": 99})", &s, nullptr));
	CHECK(s.video_stall_ms == 2000);
}

TEST_CASE("state.json への書き込みの可否は、真偽値だけを受け付ける")
{
	Settings s;
	CHECK(parse_settings(R"({"allow_state_json_write": true})", &s, nullptr));
	CHECK(s.allow_state_json_write);

	CHECK(parse_settings(R"({"allow_state_json_write": false})", &s, nullptr));
	CHECK_FALSE(s.allow_state_json_write);

	// 0 と 1 や文字列は、形が違う設定として既定値に戻す。
	// 既定値は true なので、戻った値は true になる。
	std::string error;
	CHECK_FALSE(parse_settings(R"({"allow_state_json_write": 0})", &s, &error));
	CHECK(s.allow_state_json_write);
	CHECK_FALSE(error.empty());

	CHECK_FALSE(parse_settings(R"({"allow_state_json_write": "false"})", &s, nullptr));
	CHECK(s.allow_state_json_write);
}

TEST_CASE("ドックのキーが壊れていれば、ほかのキーも既定値に戻す")
{
	Settings s;
	CHECK_FALSE(parse_settings(R"({"decoder": "hardware", "dock_poll_ms": 1})", &s, nullptr));
	CHECK(s.decoder == DecoderChoice::Software);
	CHECK(s.dock_poll_ms == 200);
}

// ---- ダミー表示 ----

TEST_CASE("ダミーの大きさとフレームレートのキー (dummy_width、dummy_height、dummy_fps) は未知のキーとして読み飛ばし、書き出さない")
{
	// 既定値の設定に、3 つのキーを足したファイル (ダミー映像はキャンバスの大きさと 60 fps で作るので、設定に持たない)。
	nlohmann::json file = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	file["dummy_width"] = 1280;
	file["dummy_height"] = 720;
	file["dummy_fps"] = 30;
	file["label_dim_pct"] = 40; // 一緒に書いたほかのキーは、書いてあるとおりに読む
	Settings s;
	std::string error;
	REQUIRE(parse_settings(file.dump(2), &s, &error));
	CHECK(error.empty());
	CHECK(s.label_dim_pct == 40);
	const std::string written = cordmux::config::settings_to_json(s);
	for (const char *key : {"\"dummy_width\"", "\"dummy_height\"", "\"dummy_fps\""})
		CHECK(written.find(key) == std::string::npos);

	// どんな値でも読み飛ばすだけで、ほかのキーは既定値に戻らない。
	Settings t;
	REQUIRE(parse_settings(R"({"dummy_width": 15, "dummy_height": "720", "dummy_fps": 1000, "feature_enabled": false})", &t,
			       &error));
	CHECK(error.empty());
	CHECK_FALSE(t.feature_enabled);
}

// ---- 一覧の上限、cordmux.dll のログの行数、メインアカウント、書き出し ----

TEST_CASE("一覧の上限、ログの行数、メインアカウントの既定値")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.seen_people_max == 200);
	CHECK(s.plugin_log_ring_lines == 200);
	CHECK(s.broadcaster_user_id.empty()); // 空なら、メインアカウントだけの構成
}

TEST_CASE("一覧の上限とログの行数は設定で変えられ、範囲の外なら既定値に戻す")
{
	Settings s;
	CHECK(parse_settings(R"({"seen_people_max": 1, "plugin_log_ring_lines": 0})", &s, nullptr));
	CHECK(s.seen_people_max == 1);
	CHECK(s.plugin_log_ring_lines == 0);

	CHECK(parse_settings(R"({"seen_people_max": 10000, "plugin_log_ring_lines": 10000})", &s, nullptr));
	CHECK(s.seen_people_max == 10000);
	CHECK(s.plugin_log_ring_lines == 10000);

	// 上限の 0 人は受け付けない (0 にすると一覧が働かなくなる)。
	CHECK_FALSE(parse_settings(R"({"seen_people_max": 0})", &s, nullptr));
	CHECK(s.seen_people_max == 200);
	CHECK_FALSE(parse_settings(R"({"seen_people_max": 10001})", &s, nullptr));
	CHECK(s.seen_people_max == 200);
	CHECK_FALSE(parse_settings(R"({"plugin_log_ring_lines": -1})", &s, nullptr));
	CHECK(s.plugin_log_ring_lines == 200);
}

TEST_CASE("メインアカウントのユーザー ID は、文字列ならそのまま持つ (cordmux は形を決めない)")
{
	Settings s;
	CHECK(parse_settings(R"({"broadcaster_user_id": "123456789012345678"})", &s, nullptr));
	CHECK(s.broadcaster_user_id == "123456789012345678");

	std::string error;
	CHECK_FALSE(parse_settings(R"({"broadcaster_user_id": 123})", &s, &error));
	CHECK(s.broadcaster_user_id.empty());
	CHECK_FALSE(error.empty());
}

TEST_CASE("書き出した設定をもう一度読むと、同じ設定に戻る")
{
	Settings written;
	written.decoder = DecoderChoice::D3D11VA;
	written.screen_audio_offset_ms = -120;
	written.voice_audio_delay_ms = 45;
	written.js_log_ring_lines = 300;
	written.plugin_log_ring_lines = 150;
	written.dock_poll_ms = 250;
	written.video_stall_ms = 1500;
	written.assign_mode = AssignMode::Auto;
	written.seen_people_max = 50;
	written.broadcaster_user_id = "123456789012345678";
	// 既定値 (true) と違う値を書く。
	written.allow_state_json_write = false;
	// 機能の ON と OFF、画質の上限、同梱のファイルを確認する周期の 5 つ。どれも既定値と違う値にする。
	written.feature_enabled = false;
	written.limit_max_long_edge = 1280;
	written.limit_fps_divisor = 2;
	written.encoder_preference = cordmux::config::EncoderPreference::Software;
	written.encode_priority = cordmux::config::EncodePriority::Quality;
	written.vencord_check_disconnected_s = 5;
	written.vencord_check_connected_s = 20;
	// 録画の 8 つ。
	written.record_dir = "D:/rec";
	written.record_output_pcm = RecordPcmFormat::Int24;
	written.record_space_check_interval_s = 60;
	written.record_space_warn_minutes = 10;
	// 音声の置く位置を時刻に追従させる 3 つ。
	written.audio_resync_ms = 25;
	written.audio_drift_max_ppm = 2000;
	written.audio_drift_time_constant_ms = 3000;
	written.voice_audio_resync_ms = 120;
	written.record_finalize_wait_on_exit_s = 120;
	written.record_queue_limit = 2048;
	written.record_clock_push_max_frames = 2;
	written.record_clock_reanchor_max_ms = 750;

	Settings back;
	std::string error;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, &error));
	CHECK(error.empty());
	CHECK(back.decoder == written.decoder);
	CHECK(back.screen_audio_offset_ms == written.screen_audio_offset_ms);
	CHECK(back.voice_audio_delay_ms == written.voice_audio_delay_ms);
	CHECK(back.js_log_ring_lines == written.js_log_ring_lines);
	CHECK(back.plugin_log_ring_lines == written.plugin_log_ring_lines);
	CHECK(back.dock_poll_ms == written.dock_poll_ms);
	CHECK(back.video_stall_ms == written.video_stall_ms);
	CHECK(back.assign_mode == written.assign_mode);
	CHECK(back.seen_people_max == written.seen_people_max);
	CHECK(back.broadcaster_user_id == written.broadcaster_user_id);
	CHECK(back.allow_state_json_write == written.allow_state_json_write);
	CHECK(back.record_dir == written.record_dir);
	CHECK(back.record_output_pcm == written.record_output_pcm);
	CHECK(back.record_space_check_interval_s == written.record_space_check_interval_s);
	CHECK(back.record_space_warn_minutes == written.record_space_warn_minutes);
	CHECK(back.audio_resync_ms == written.audio_resync_ms);
	CHECK(back.audio_drift_max_ppm == written.audio_drift_max_ppm);
	CHECK(back.audio_drift_time_constant_ms == written.audio_drift_time_constant_ms);
	CHECK(back.voice_audio_resync_ms == written.voice_audio_resync_ms);
	CHECK(back.record_finalize_wait_on_exit_s == written.record_finalize_wait_on_exit_s);
	CHECK(back.record_queue_limit == written.record_queue_limit);
	CHECK(back.record_clock_push_max_frames == written.record_clock_push_max_frames);
	CHECK(back.record_clock_reanchor_max_ms == written.record_clock_reanchor_max_ms);
	CHECK(back.feature_enabled == written.feature_enabled);
	CHECK(back.limit_max_long_edge == written.limit_max_long_edge);
	CHECK(back.limit_fps_divisor == written.limit_fps_divisor);
	CHECK(back.encoder_preference == written.encoder_preference);
	CHECK(back.encode_priority == written.encode_priority);
	CHECK(back.vencord_check_disconnected_s == written.vencord_check_disconnected_s);
	CHECK(back.vencord_check_connected_s == written.vencord_check_connected_s);
}

TEST_CASE("既定値を書き出して読み直しても既定値のまま")
{
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(Settings{}), &back, nullptr));
	CHECK(back.decoder == DecoderChoice::Software);
	// 書き出して読み直しても手動のまま。
	CHECK(back.assign_mode == AssignMode::Manual);
	CHECK(back.voice_audio_delay_ms == 0);
	CHECK(back.seen_people_max == 200);
	CHECK(back.allow_state_json_write);
	CHECK(back.feature_enabled);
	CHECK(back.limit_max_long_edge == 0);
	CHECK(back.limit_fps_divisor == 1);
	CHECK(back.encoder_preference == cordmux::config::EncoderPreference::Hardware);
	CHECK(back.encode_priority == cordmux::config::EncodePriority::Speed);
	CHECK(back.vencord_check_disconnected_s == 30);
	CHECK(back.vencord_check_connected_s == 300);
}

// ---- 誰を枠に載せるかの決め方 ----

TEST_CASE("誰を枠に載せるかの決め方の既定値は手動")
{
	Settings s;
	REQUIRE(parse_settings("{}", &s, nullptr));
	CHECK(s.assign_mode == AssignMode::Manual);
}

TEST_CASE("assign_mode は manual と auto の 2 つを受け付ける")
{
	Settings manual;
	REQUIRE(parse_settings(R"({"assign_mode":"manual"})", &manual, nullptr));
	CHECK(manual.assign_mode == AssignMode::Manual);

	Settings automatic;
	REQUIRE(parse_settings(R"({"assign_mode":"auto"})", &automatic, nullptr));
	CHECK(automatic.assign_mode == AssignMode::Auto);
}

TEST_CASE("assign_mode の形が違えば、false を返し、すべてのキーを既定値に戻す")
{
	Settings unknown;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"assign_mode":"both","dock_poll_ms":250})", &unknown, &error));
	CHECK_FALSE(error.empty());
	CHECK(unknown.assign_mode == AssignMode::Manual);
	CHECK(unknown.dock_poll_ms == 200);

	Settings not_string;
	CHECK_FALSE(parse_settings(R"({"assign_mode":true})", &not_string, &error));
	CHECK_FALSE(error.empty());
	CHECK(not_string.assign_mode == AssignMode::Manual);
}

TEST_CASE("書き出した設定に assign_mode のキーがある (次の起動に残る)")
{
	Settings written;
	written.assign_mode = AssignMode::Auto;
	const std::string text = cordmux::config::settings_to_json(written);
	CHECK(text.find("\"assign_mode\"") != std::string::npos);
	CHECK(text.find("\"auto\"") != std::string::npos);
}

// ---- 録画 ----

TEST_CASE("録画の 8 つのキーの既定値")
{
	Settings s;
	REQUIRE(parse_settings("{}", &s, nullptr));
	// 保存先は空 (既定の保存先を使う)。
	CHECK(s.record_dir.empty());
	// 既定値は fl32。
	CHECK(s.record_output_pcm == RecordPcmFormat::Float32);
	CHECK(s.record_space_check_interval_s == 300);
	CHECK(s.record_space_warn_minutes == 30);
	CHECK(s.record_finalize_wait_on_exit_s == 30);
	CHECK(s.record_queue_limit == 1024);
	CHECK(s.record_clock_push_max_frames == 1);
	CHECK(s.record_clock_reanchor_max_ms == 1000);
}

TEST_CASE("録画の PCM の形式は fl32 と in24 の 2 つを受け付ける")
{
	Settings f32;
	REQUIRE(parse_settings(R"({"record_output_pcm":"fl32"})", &f32, nullptr));
	CHECK(f32.record_output_pcm == RecordPcmFormat::Float32);

	Settings i24;
	REQUIRE(parse_settings(R"({"record_output_pcm":"in24"})", &i24, nullptr));
	CHECK(i24.record_output_pcm == RecordPcmFormat::Int24);

	// 未知の値は、形が違う設定として、すべてを既定値に戻す。
	Settings unknown;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"record_output_pcm":"s16","record_queue_limit":2048})", &unknown, &error));
	CHECK_FALSE(error.empty());
	CHECK(unknown.record_output_pcm == RecordPcmFormat::Float32);
	CHECK(unknown.record_queue_limit == 1024);

	Settings not_string;
	CHECK_FALSE(parse_settings(R"({"record_output_pcm":32})", &not_string, nullptr));
	CHECK(not_string.record_output_pcm == RecordPcmFormat::Float32);
}

TEST_CASE("録画の保存先は、文字列ならそのまま持つ (cordmux は形を決めない)")
{
	Settings s;
	REQUIRE(parse_settings(R"({"record_dir":"D:/rec/cordmux"})", &s, nullptr));
	CHECK(s.record_dir == "D:/rec/cordmux");

	// 空も正しい値である (既定の保存先を使う)。
	Settings empty;
	REQUIRE(parse_settings(R"({"record_dir":""})", &empty, nullptr));
	CHECK(empty.record_dir.empty());

	Settings not_string;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"record_dir":7})", &not_string, &error));
	CHECK_FALSE(error.empty());
	CHECK(not_string.record_dir.empty());
}

TEST_CASE("録画の 6 つの整数のキーは、設定で変えられる")
{
	Settings s;
	REQUIRE(parse_settings(R"({"record_space_check_interval_s":60,"record_space_warn_minutes":5,
		"record_finalize_wait_on_exit_s":0,
		"record_queue_limit":4096,"record_clock_push_max_frames":3,
		"record_clock_reanchor_max_ms":500})",
			      &s, nullptr));
	CHECK(s.record_space_check_interval_s == 60);
	CHECK(s.record_space_warn_minutes == 5);
	// 0 は、待たずに次の起動で録画ファイルを作成する値 (いつも次の起動で録画ファイルを作成する動作を、テストで確認するための値)。
	CHECK(s.record_finalize_wait_on_exit_s == 0);
	CHECK(s.record_queue_limit == 4096);
	CHECK(s.record_clock_push_max_frames == 3);
	CHECK(s.record_clock_reanchor_max_ms == 500);
}

TEST_CASE("録画の整数のキーは範囲の端の値を受け付け、範囲の外なら false を返して既定値に戻す")
{
	Settings low;
	REQUIRE(parse_settings(R"({"record_space_check_interval_s":1,"record_space_warn_minutes":1,
		"record_finalize_wait_on_exit_s":0,
		"record_queue_limit":16,"record_clock_push_max_frames":1,
		"record_clock_reanchor_max_ms":1})",
			       &low, nullptr));
	CHECK(low.record_queue_limit == 16);
	CHECK(low.record_clock_reanchor_max_ms == 1);

	Settings high;
	REQUIRE(parse_settings(R"({"record_space_check_interval_s":86400,"record_space_warn_minutes":1440,
		"record_finalize_wait_on_exit_s":3600,
		"record_queue_limit":65536,"record_clock_push_max_frames":60,
		"record_clock_reanchor_max_ms":60000})",
			       &high, nullptr));
	CHECK(high.record_queue_limit == 65536);
	CHECK(high.record_clock_push_max_frames == 60);

	// 周期は 0 にできない (確認が止まり、空き容量がなくなる前に伝えることも、録画を止めることもできなくなる)。
	Settings zero_interval;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"record_space_check_interval_s":0})", &zero_interval, &error));
	CHECK_FALSE(error.empty());
	CHECK(zero_interval.record_space_check_interval_s == 300);

	// フレームの間隔の数も 0 にできない (段階の境がなくなる)。
	Settings zero_frames;
	CHECK_FALSE(parse_settings(R"({"record_clock_push_max_frames":0})", &zero_frames, nullptr));
	CHECK(zero_frames.record_clock_push_max_frames == 1);

	// 上限の外。
	Settings too_big;
	CHECK_FALSE(parse_settings(R"({"record_queue_limit":65537,"record_space_warn_minutes":5})", &too_big,
				   nullptr));
	CHECK(too_big.record_queue_limit == 1024);
	CHECK(too_big.record_space_warn_minutes == 30);

	// 整数でない値。
	Settings not_int;
	CHECK_FALSE(parse_settings(R"({"record_queue_limit":"1024"})", &not_int, nullptr));
	CHECK(not_int.record_queue_limit == 1024);
}

TEST_CASE("音声の置く位置を時刻に追従させる 3 つのキーの既定値は align::AudioDriftConfig の既定値と同じ")
{
	Settings s;
	REQUIRE(parse_settings("{}", &s, nullptr));
	CHECK(s.audio_resync_ms == 45);
	CHECK(s.audio_drift_max_ppm == 1000);
	CHECK(s.audio_drift_time_constant_ms == 8000);
	const cordmux::align::AudioDriftConfig from_settings = cordmux::align::make_audio_drift_config(
		s.audio_resync_ms, s.audio_drift_max_ppm, s.audio_drift_time_constant_ms);
	const cordmux::align::AudioDriftConfig defaults{};
	CHECK(from_settings.resync_us == defaults.resync_us);
	CHECK(from_settings.max_ppm == defaults.max_ppm);
	CHECK(from_settings.time_constant_us == defaults.time_constant_us);
	// 配信の声の置き直しのしきい値の既定は、AudioMixerConfig::voice_resync_us と同じ 70 ms。
	CHECK(s.voice_audio_resync_ms == 70);
	CHECK(std::int64_t(s.voice_audio_resync_ms) * 1000 == cordmux::pipeline::AudioMixerConfig{}.voice_resync_us);
	const std::string text = cordmux::config::settings_to_json(Settings{});
	for (const char *key : {"\"audio_resync_ms\"", "\"audio_drift_max_ppm\"", "\"audio_drift_time_constant_ms\"",
				"\"voice_audio_resync_ms\""})
		CHECK(text.find(key) != std::string::npos);
}

TEST_CASE("音声の置く位置を時刻に追従させる 3 つのキーは範囲の端の値を受け付け、範囲の外なら false を返して既定値に戻す")
{
	Settings low;
	REQUIRE(parse_settings(R"({"audio_resync_ms":1,"audio_drift_max_ppm":1,"audio_drift_time_constant_ms":100})",
			       &low, nullptr));
	CHECK(low.audio_resync_ms == 1);
	CHECK(low.audio_drift_max_ppm == 1);
	CHECK(low.audio_drift_time_constant_ms == 100);

	Settings high;
	REQUIRE(parse_settings(
		R"({"audio_resync_ms":2000,"audio_drift_max_ppm":50000,"audio_drift_time_constant_ms":600000})", &high,
		nullptr));
	CHECK(high.audio_resync_ms == 2000);
	CHECK(high.audio_drift_max_ppm == 50000);
	CHECK(high.audio_drift_time_constant_ms == 600000);

	Settings zero;
	CHECK_FALSE(parse_settings(R"({"audio_resync_ms":0})", &zero, nullptr));
	CHECK(zero.audio_resync_ms == 45);
	Settings too_big;
	CHECK_FALSE(parse_settings(R"({"audio_drift_max_ppm":50001})", &too_big, nullptr));
	CHECK(too_big.audio_drift_max_ppm == 1000);
	Settings too_short;
	CHECK_FALSE(parse_settings(R"({"audio_drift_time_constant_ms":99})", &too_short, nullptr));
	CHECK(too_short.audio_drift_time_constant_ms == 8000);
}

TEST_CASE("書き出した設定に録画の 8 つのキーがある (次の起動に残る)")
{
	const std::string text = cordmux::config::settings_to_json(Settings{});
	CHECK(text.find("\"record_dir\"") != std::string::npos);
	CHECK(text.find("\"record_output_pcm\"") != std::string::npos);
	CHECK(text.find("\"fl32\"") != std::string::npos);
	CHECK(text.find("\"record_space_check_interval_s\"") != std::string::npos);
	CHECK(text.find("\"record_space_warn_minutes\"") != std::string::npos);
	CHECK(text.find("\"record_finalize_wait_on_exit_s\"") != std::string::npos);
	CHECK(text.find("\"record_queue_limit\"") != std::string::npos);
	CHECK(text.find("\"record_clock_push_max_frames\"") != std::string::npos);
	CHECK(text.find("\"record_clock_reanchor_max_ms\"") != std::string::npos);
	// 録画の ON と OFF は書かない (保存せず、起動するたびに OFF から始める)。
	CHECK(text.find("\"recording\"") == std::string::npos);
}

// ---- 機能の ON と OFF、画質の上限、同梱のファイルを確認する周期 ----

TEST_CASE("機能の ON と OFF、画質の上限、同梱のファイルを確認する周期の既定値 (ON、制限なし、そのまま、30 秒、300 秒)")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.feature_enabled);
	CHECK(s.limit_max_long_edge == cordmux::config::kLimitMaxLongEdgeNone);
	CHECK(s.limit_fps_divisor == 1);
	CHECK(s.vencord_check_disconnected_s == 30);
	CHECK(s.vencord_check_connected_s == 300);
}

TEST_CASE("機能の ON と OFF は真偽値だけを受け付け、選んだ値を保存する")
{
	Settings s;
	CHECK(parse_settings(R"({"feature_enabled": false})", &s, nullptr));
	CHECK_FALSE(s.feature_enabled);

	// 0 と 1 や文字列は、形が違う設定として、すべてを既定値に戻す。
	std::string error;
	CHECK_FALSE(parse_settings(R"({"feature_enabled": 0, "dock_poll_ms": 250})", &s, &error));
	CHECK(s.feature_enabled);
	CHECK(s.dock_poll_ms == 200);
	CHECK_FALSE(error.empty());
}

TEST_CASE("長い辺の上限は 0 (制限なし) か、16 から 7680 を受け付ける")
{
	// 画面の選択肢の 5 つは、どれも読める。
	for (const int value : {0, 1920, 1280, 960, 640}) {
		Settings s;
		CHECK(parse_settings(R"({"limit_max_long_edge": )" + std::to_string(value) + "}", &s, nullptr));
		CHECK(s.limit_max_long_edge == value);
	}
	// 範囲の端。
	Settings edge;
	CHECK(parse_settings(R"({"limit_max_long_edge": 16})", &edge, nullptr));
	CHECK(edge.limit_max_long_edge == 16);
	CHECK(parse_settings(R"({"limit_max_long_edge": 7680})", &edge, nullptr));
	CHECK(edge.limit_max_long_edge == 7680);

	// 1 から 15 (0 でも 16 以上でもない)、負の値、上限の外、整数でない値は、すべてを既定値に戻す。
	for (const char *bad : {R"({"limit_max_long_edge": 1})", R"({"limit_max_long_edge": 15})",
				R"({"limit_max_long_edge": -1})", R"({"limit_max_long_edge": 7681})",
				R"({"limit_max_long_edge": 1280.5})", R"({"limit_max_long_edge": "1280"})"}) {
		Settings s;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK(s.limit_max_long_edge == 0);
		CHECK_FALSE(error.empty());
	}
}

TEST_CASE("フレームレートの割る数は 1 と 2 だけを受け付ける")
{
	Settings s;
	CHECK(parse_settings(R"({"limit_fps_divisor": 2})", &s, nullptr));
	CHECK(s.limit_fps_divisor == 2);
	CHECK(parse_settings(R"({"limit_fps_divisor": 1})", &s, nullptr));
	CHECK(s.limit_fps_divisor == 1);

	for (const char *bad : {R"({"limit_fps_divisor": 0})", R"({"limit_fps_divisor": 3})",
				R"({"limit_fps_divisor": true})"}) {
		Settings b;
		CHECK_FALSE(parse_settings(bad, &b, nullptr));
		CHECK(b.limit_fps_divisor == 1);
	}
}

TEST_CASE("エンコードの設定の既定値は、ハードウェアと速さ優先 (今までの送信側の動き)")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.encoder_preference == cordmux::config::EncoderPreference::Hardware);
	CHECK(s.encode_priority == cordmux::config::EncodePriority::Speed);
}

TEST_CASE("エンコードの設定は、決まった語だけを受け付ける")
{
	Settings s;
	CHECK(parse_settings(R"({"encoder_preference": "software", "encode_priority": "quality"})", &s, nullptr));
	CHECK(s.encoder_preference == cordmux::config::EncoderPreference::Software);
	CHECK(s.encode_priority == cordmux::config::EncodePriority::Quality);
	CHECK(parse_settings(R"({"encoder_preference": "hardware", "encode_priority": "speed"})", &s, nullptr));
	CHECK(s.encoder_preference == cordmux::config::EncoderPreference::Hardware);
	CHECK(s.encode_priority == cordmux::config::EncodePriority::Speed);

	// 未知の語と型の違う値は、ファイルごと既定値に戻す (ほかのキーと同じ扱い)。
	for (const char *bad : {R"({"encoder_preference": "gpu"})", R"({"encoder_preference": 1})",
				R"({"encoder_preference": true})", R"({"encode_priority": "fast"})",
				R"({"encode_priority": 0})", R"({"encode_priority": "realtime"})"}) {
		Settings b;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		CHECK(b.encoder_preference == cordmux::config::EncoderPreference::Hardware);
		CHECK(b.encode_priority == cordmux::config::EncodePriority::Speed);
	}
}

TEST_CASE("同梱のファイルを確認する 2 つの周期は、1 から 86400 秒を受け付ける")
{
	Settings s;
	CHECK(parse_settings(R"({"vencord_check_disconnected_s": 1, "vencord_check_connected_s": 86400})", &s, nullptr));
	CHECK(s.vencord_check_disconnected_s == 1);
	CHECK(s.vencord_check_connected_s == 86400);

	for (const char *bad : {R"({"vencord_check_disconnected_s": 0})", R"({"vencord_check_connected_s": 0})",
				R"({"vencord_check_disconnected_s": 86401})", R"({"vencord_check_connected_s": "300"})"}) {
		Settings b;
		CHECK_FALSE(parse_settings(bad, &b, nullptr));
		CHECK(b.vencord_check_disconnected_s == 30);
		CHECK(b.vencord_check_connected_s == 300);
	}
}

TEST_CASE("自分の声のミュートは既定が true で、書き出して読み直すと同じ値に戻り、真偽値でなければ丸ごと拒む")
{
	Settings defaults;
	CHECK(defaults.self_voice_muted);
	CHECK(cordmux::config::settings_to_json(defaults).find("\"self_voice_muted\": true") != std::string::npos);

	Settings written;
	written.self_voice_muted = false;
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, nullptr));
	CHECK_FALSE(back.self_voice_muted);

	// キーがなければ既定 (ミュート)。
	Settings none;
	none.self_voice_muted = false;
	CHECK(parse_settings(R"({"dock_poll_ms": 250})", &none, nullptr));
	CHECK(none.self_voice_muted);

	Settings bad;
	std::string error;
	CHECK_FALSE(parse_settings(R"({"self_voice_muted": 0, "dock_poll_ms": 250})", &bad, &error));
	CHECK_FALSE(error.empty());
	CHECK(bad.self_voice_muted);
	CHECK(bad.dock_poll_ms == 200);
}

TEST_CASE("書き出した設定に、機能の ON と OFF、画質の上限、同梱のファイルを確認する周期のキーがある (次の起動に残る)")
{
	const std::string text = cordmux::config::settings_to_json(Settings{});
	for (const char *key : {"\"feature_enabled\"", "\"limit_max_long_edge\"", "\"limit_fps_divisor\"",
				"\"encoder_preference\"", "\"encode_priority\"",
				"\"vencord_check_disconnected_s\"", "\"vencord_check_connected_s\"",
				"\"allow_state_json_write\""})
		CHECK(text.find(key) != std::string::npos);
}

// ---- 22 本のキー (メッセージ、名前ラベル、話している人の光、ダミー、録画ファイルの作成、ダイアログ) ----

namespace {

// この 22 本と表示の緩衝の 1 本と、専用のテストが確認するキー以外が 27 本。
const char *const kDocs07Keys[] = {
	"notice_show_delay_ms",   "notice_clear_delay_ms",   "first_frame_wait_ms",     "link_lost_hold_ms",
	"tile_gap_px",            "label_em_ratio_pct",      "label_show_name",         "label_show_avatar",
	"label_show_speaking",    "label_border_color",      "label_plate_color",       "label_plate_opacity_pct",
	"label_text_color",       "label_border_rise_ms",    "label_border_hold_ms",    "label_border_fade_ms",
	"dummy_cycle_ms",         "record_finalize_stall_s", "settings_window_geometry", "participants_window_geometry",
	// 名前ラベルのフォント。
	"label_font_family",      "label_font_weight",
};

// 枠の背景のキー。
const char *const kSlotBackgroundKeys[] = {
	"slot_background_show",
	"slot_background_color",
	"slot_background_opacity_pct",
};

// 合成のソースの名前ラベルのキー。
const char *const kCompositeLabelKeys[] = {
	"label_margin_pct",       "label_outline_color",     "label_outline_ratio",     "label_show_outline",
	"label_opacity_pct",
	"label_show_plate",       "label_avatar_ratio",      "label_hide_when_single",  "label_show_in_slot",
	"label_glow_frame",       "label_glow_avatar",       "label_glow_name",         "label_glow_blur_ratio",
	"label_glow_spread_ratio", "label_corner",          "label_avatar_side",       "label_dim_pct",
	"roster_show",             "roster_anchor",         "roster_direction",        "roster_avatar_side",
	"roster_gap_ratio",        "roster_show_bots",        "roster_name_max_em",      "label_avatar_shape",
	"roster_offset_x_px",      "roster_offset_y_px",
};

// 自分の枠の音声ソースのキー。
const char *const kSelfSpeakingKeys[] = {
	"self_speaking_source_uuid",
	"self_speaking_source_name",
	"self_speaking_threshold_db",
};

// 自分の枠に映すソースのキー。
const char *const kSelfFrameKeys[] = {
	"self_frame_source_uuid",
	"self_frame_source_name",
	"self_frame_on",
	"self_slot_first",
};

// 22 本以外の 27 本の鍵の全部を持つ settings.json (22 本は書いていない)。下のテストが、この 27 本と
// 「現在の鍵 - 22 本」が一致することも当てる。
const char *const kBaseKeysSettings = R"({
  "allow_state_json_write": true,
  "assign_mode": "manual",
  "broadcaster_user_id": "",
  "decoder": "software",
  "dock_poll_ms": 500,
  "encode_priority": "speed",
  "encoder_preference": "hardware",
  "feature_enabled": true,
  "js_log_ring_lines": 200,
  "limit_fps_divisor": 1,
  "limit_max_long_edge": 0,
  "plugin_log_ring_lines": 200,
  "record_clock_push_max_frames": 1,
  "record_clock_reanchor_max_ms": 1000,
  "record_dir": "",
  "record_finalize_wait_on_exit_s": 30,
  "record_output_pcm": "fl32",
  "record_queue_limit": 1024,
  "record_space_check_interval_s": 300,
  "record_space_warn_minutes": 30,
  "screen_audio_offset_ms": 0,
  "seen_people_max": 200,
  "self_voice_muted": true,
  "vencord_check_connected_s": 300,
  "vencord_check_disconnected_s": 30,
  "video_stall_ms": 2000,
  "voice_audio_delay_ms": 0
})";

std::set<std::string> keys_of_json(const std::string &text)
{
	std::set<std::string> keys;
	const nlohmann::json j = nlohmann::json::parse(text);
	for (auto it = j.begin(); it != j.end(); ++it)
		keys.insert(it.key());
	return keys;
}

// 22 本のキーをすべて既定値と違う値にした設定 (範囲の中で、刻みに合う値)。
Settings docs07_all_changed()
{
	Settings s;
	s.notice_show_delay_ms = 2500;
	s.notice_clear_delay_ms = 0;
	s.first_frame_wait_ms = 8000;
	s.link_lost_hold_ms = 60000;
	s.tile_gap_px = 8;
	s.label_em_ratio_pct = 5.0;
	s.label_show_name = false;
	s.label_show_avatar = false;
	s.label_show_speaking = false;
	s.label_border_color = "#3bd67f";
	s.label_plate_color = "#202020";
	s.label_plate_opacity_pct = 60;
	s.label_text_color = "#F0F0F0";
	s.label_font_family = "Yu Gothic UI";
	s.label_font_weight = 350;
	s.label_border_rise_ms = 100;
	s.label_border_hold_ms = 0;
	s.label_border_fade_ms = 5000;
	s.dummy_cycle_ms = 2000;
	s.record_finalize_stall_s = 240;
	s.settings_window_geometry = "AdnQywADAAA=";
	s.participants_window_geometry = "AdnQywADAAB=";
	return s;
}

} // namespace

TEST_CASE("22 本のキーの既定値")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.notice_show_delay_ms == 2000);
	CHECK(s.notice_clear_delay_ms == 1000);
	CHECK(s.first_frame_wait_ms == 5000);
	CHECK(s.link_lost_hold_ms == 2000);
	CHECK(s.reconnect_layout_hold_ms == 10000);
	CHECK(s.tile_gap_px == 0);
	CHECK(s.label_em_ratio_pct == 2.5);
	CHECK(s.label_show_name);
	CHECK(s.label_show_avatar);
	CHECK(s.label_show_speaking);
	CHECK(s.label_border_color == "#57F287");
	CHECK(s.label_plate_color == "#000000");
	CHECK(s.label_plate_opacity_pct == 50);
	CHECK(s.label_text_color == "#FFFFFF");
	CHECK(s.label_font_family.empty());
	CHECK(s.label_font_weight == 700);
	CHECK(s.label_border_rise_ms == 150);
	CHECK(s.label_border_hold_ms == 200);
	CHECK(s.label_border_fade_ms == 150);
	CHECK(s.dummy_cycle_ms == 4000);
	CHECK(s.record_finalize_stall_s == 120);
	CHECK(s.settings_window_geometry.empty());
	CHECK(s.participants_window_geometry.empty());
}

TEST_CASE("既定値は自分の範囲と刻みの中にあり、既定値を書き出すと必ず読める")
{
	// 既定値を書き出して読めなければ、初めての起動で書いた設定が、次の起動ですべて既定値に戻る。
	Settings back;
	std::string error;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(Settings{}), &back, &error));
	CHECK(error.empty());
	// clamp_settings が既定値を 1 つも変えない (既定値が範囲と刻みの中にある)。
	Settings clamped;
	cordmux::config::clamp_settings(clamped);
	CHECK(cordmux::config::settings_to_json(clamped) == cordmux::config::settings_to_json(Settings{}));
}

TEST_CASE("書き出した設定は 102 本のキーを持ち、22 本がその中にある")
{
	const std::set<std::string> keys = keys_of_json(cordmux::config::settings_to_json(Settings{}));
	CHECK(keys.size() == 102);
	for (const char *key : kDocs07Keys) {
		CAPTURE(key);
		CHECK(keys.count(key) == 1);
	}
}

TEST_CASE("22 本のキーがない settings.json はそのまま読め、ない鍵は既定になる")
{
	// 見本が本当に 27 本の鍵の全部であること: 現在の鍵から 22 本を除いた集合と一致する。
	std::set<std::string> expected = keys_of_json(cordmux::config::settings_to_json(Settings{}));
	for (const char *key : kDocs07Keys)
		expected.erase(key);
	// 表示の緩衝のキーと合成のソースの名前ラベルのキーは、この見本の 27 本にも 22 本にも入らない (専用のテストが
	// 確認する)。
	expected.erase("display_buffer_ms");
	for (const char *key : kCompositeLabelKeys)
		expected.erase(key);
	for (const char *key : kSelfSpeakingKeys)
		expected.erase(key);
	for (const char *key : kSelfFrameKeys)
		expected.erase(key);
	for (const char *key : kSlotBackgroundKeys)
		expected.erase(key);
	for (const char *key : {"slot_order", "record_with_stream", "confirm_while_streaming", "receive_scope",
				"layout_hold_idle_ms", "dock_tab", "call_action_timeout_ms", "reconnect_layout_hold_ms"})
		expected.erase(key);
	// 出来事の行の行数と、受信のやり直しを頼むまでの時間も、この見本より後に足したキーである (専用のテストが確認する)。
	for (const char *key : {"js_event_log_lines", "plugin_event_log_lines", "receive_restart_wait_ms"})
		expected.erase(key);
	// 音声の置く位置を時刻に追従させる 3 つのキーも、この見本より後に足したキーである (専用のテストが確認する)。
	for (const char *key :
	     {"audio_resync_ms", "audio_drift_max_ppm", "audio_drift_time_constant_ms", "voice_audio_resync_ms"})
		expected.erase(key);
	CHECK(keys_of_json(kBaseKeysSettings) == expected);
	CHECK(expected.size() == 27);

	Settings s;
	std::string error;
	REQUIRE(parse_settings(kBaseKeysSettings, &s, &error));
	CHECK(error.empty());
	// 書いてある鍵は書いてある値のまま (既定値の 200 と違う 500)。
	CHECK(s.dock_poll_ms == 500);
	// 書いていない鍵は全部既定。
	const Settings defaults{};
	const nlohmann::json got = nlohmann::json::parse(cordmux::config::settings_to_json(s));
	const nlohmann::json want = nlohmann::json::parse(cordmux::config::settings_to_json(defaults));
	for (const char *key : kDocs07Keys) {
		CAPTURE(key);
		CHECK(got.at(key) == want.at(key));
	}
}

TEST_CASE("追加した整数のキーは範囲の端の値を受け付け、範囲の外なら false を返して、すべてを既定値に戻す")
{
	struct Case {
		const char *key;
		int lo;
		int hi;
	};
	const Case cases[] = {
		{"notice_show_delay_ms", 0, 60000},     {"notice_clear_delay_ms", 0, 60000},
		{"first_frame_wait_ms", 1000, 120000},  {"link_lost_hold_ms", 0, 60000},
		{"reconnect_layout_hold_ms", 0, 60000},
		{"tile_gap_px", 0, 64},                 {"label_plate_opacity_pct", 0, 100},
		{"label_border_hold_ms", 0, 5000},      {"label_border_fade_ms", 0, 5000},
		{"dummy_cycle_ms", 2000, 60000},        {"record_finalize_stall_s", 10, 3600},
	};
	for (const Case &c : cases) {
		CAPTURE(c.key);
		for (const int ok : {c.lo, c.hi}) {
			Settings s;
			const std::string text = std::string("{\"") + c.key + "\": " + std::to_string(ok) + "}";
			CHECK(parse_settings(text, &s, nullptr));
			const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(s));
			CHECK(j.at(c.key).get<int>() == ok);
		}
		for (const int bad : {c.lo - 1, c.hi + 1}) {
			Settings s;
			std::string error;
			const std::string text = std::string("{\"feature_enabled\": false, \"") + c.key + "\": " +
						 std::to_string(bad) + "}";
			CHECK_FALSE(parse_settings(text, &s, &error));
			CHECK_FALSE(error.empty());
			CHECK(s.feature_enabled); // ほかのキーも既定値に戻る
		}
		Settings s;
		CHECK_FALSE(parse_settings(std::string("{\"") + c.key + "\": 1.5}", &s, nullptr));
		CHECK_FALSE(parse_settings(std::string("{\"") + c.key + "\": \"1\"}", &s, nullptr));
	}
}

TEST_CASE("名前の大きさ label_em_ratio_pct は 1.0 から 8.5 の 0.1 刻みで、整数で書かれていても読む")
{
	// 範囲の中のすべての刻みを読める。0.1 は 2 進数で正確に表せないので、10 倍して整数かどうかを確認する判定が、
	// どの刻みでも誤って拒まないことを確認する。
	for (int tenth = 10; tenth <= 85; ++tenth) {
		const double value = tenth / 10.0;
		Settings s;
		const std::string text = "{\"label_em_ratio_pct\": " + nlohmann::json(value).dump() + "}";
		CAPTURE(text);
		CHECK(parse_settings(text, &s, nullptr));
		CHECK(s.label_em_ratio_pct == value);
	}
	Settings s;
	CHECK(parse_settings(R"({"label_em_ratio_pct": 5})", &s, nullptr));
	CHECK(s.label_em_ratio_pct == 5.0);

	// 刻みに合わない値、範囲の外の値、数でない値は、形が違う設定として扱う (読むときは丸めない)。
	for (const char *bad : {R"({"label_em_ratio_pct": 2.95})", R"({"label_em_ratio_pct": 4.55})",
				R"({"label_em_ratio_pct": 0.9})", R"({"label_em_ratio_pct": 8.6})",
				R"({"label_em_ratio_pct": "4.5"})", R"({"label_em_ratio_pct": true})"}) {
		CAPTURE(bad);
		Settings b;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		CHECK(b.label_em_ratio_pct == 2.5);
	}
}

TEST_CASE("合成のソースの名前ラベルのキーの既定値")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.label_margin_pct == 0.5);
	CHECK(s.label_outline_color == "#000000");
	CHECK(s.label_outline_ratio == 0.1);
	CHECK(s.label_show_outline);
	CHECK(s.label_opacity_pct == 100);
	CHECK(s.label_show_plate);
	CHECK(s.label_avatar_ratio == 1.5);
	CHECK(s.label_hide_when_single);
	CHECK_FALSE(s.label_glow_frame);
	CHECK(s.label_glow_avatar);
	CHECK(s.label_glow_name);
	CHECK(s.label_glow_blur_ratio == 0.10);
	CHECK(s.label_glow_spread_ratio == 0.10);
}

TEST_CASE("合成のソースの名前ラベルの数値のキーは範囲の端を受け付け、範囲の外なら false を返す")
{
	struct Case {
		const char *key;
		const char *lo;
		const char *hi;
		const char *below;
		const char *above;
	};
	const Case cases[] = {
		{"label_margin_pct", "0", "13.9", "-0.1", "14.0"},
		{"label_outline_ratio", "0", "0.4", "-0.01", "0.41"},
		{"label_opacity_pct", "0", "100", "-1", "101"},
		{"label_avatar_ratio", "0.4", "3.0", "0.39", "3.01"},
		{"label_glow_blur_ratio", "0", "3", "-0.01", "3.01"},
		{"label_glow_spread_ratio", "0", "1", "-0.01", "1.01"},
	};
	for (const Case &c : cases) {
		CAPTURE(c.key);
		for (const char *ok : {c.lo, c.hi}) {
			Settings s;
			CHECK(parse_settings(std::string("{\"") + c.key + "\": " + ok + "}", &s, nullptr));
			const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(s));
			CHECK(j.at(c.key).get<double>() == nlohmann::json::parse(ok).get<double>());
		}
		for (const char *bad : {c.below, c.above}) {
			Settings s;
			std::string error;
			CHECK_FALSE(parse_settings(std::string("{\"feature_enabled\": false, \"") + c.key + "\": " + bad + "}", &s,
						   &error));
			CHECK_FALSE(error.empty());
			CHECK(s.feature_enabled); // ほかのキーも既定値に戻る
		}
		Settings s;
		CHECK_FALSE(parse_settings(std::string("{\"") + c.key + "\": \"1\"}", &s, nullptr));
		CHECK_FALSE(parse_settings(std::string("{\"") + c.key + "\": null}", &s, nullptr));
	}
	// 不透明度は整数のキー。小数は受け付けない。
	Settings s;
	CHECK_FALSE(parse_settings(R"({"label_opacity_pct": 88.5})", &s, nullptr));
}

TEST_CASE("合成のソースの名前ラベルの真偽値と色のキーは、形が違えば false を返す")
{
	Settings s;
	CHECK(parse_settings(R"({"label_show_outline": false})", &s, nullptr));
	CHECK_FALSE(s.label_show_outline);
	CHECK_FALSE(parse_settings(R"({"label_show_outline": 0})", &s, nullptr));
	CHECK(parse_settings(R"({"label_show_plate": false, "label_hide_when_single": false, "label_glow_frame": false,
				 "label_glow_avatar": false, "label_glow_name": false,
				 "label_outline_color": "#A0B0C0"})",
			     &s, nullptr));
	CHECK_FALSE(s.label_show_plate);
	CHECK_FALSE(s.label_hide_when_single);
	CHECK_FALSE(s.label_glow_frame);
	CHECK_FALSE(s.label_glow_avatar);
	CHECK_FALSE(s.label_glow_name);
	CHECK(s.label_outline_color == "#A0B0C0");

	for (const char *bad : {R"({"label_show_plate": 1})", R"({"label_hide_when_single": "true"})",
				R"({"label_glow_frame": null})", R"({"label_glow_avatar": 0})",
				R"({"label_glow_name": "no"})", R"({"label_outline_color": "000000"})",
				R"({"label_outline_color": 0})"}) {
		CAPTURE(bad);
		Settings b;
		b.label_show_plate = false;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		CHECK(b.label_show_plate); // ほかのキーも既定値に戻る
	}
}

TEST_CASE("label_speaking_style は未知のキーとして読み飛ばし、どんな値でも読め、書き出さない")
{
	for (const char *text : {R"({"label_speaking_style": "line"})", R"({"label_speaking_style": "glow"})",
				 R"({"label_speaking_style": "box"})", R"({"label_speaking_style": 1})"}) {
		CAPTURE(text);
		Settings s;
		std::string error;
		CHECK(parse_settings(text, &s, &error));
		CHECK(error.empty());
		CHECK(cordmux::config::settings_to_json(s) == cordmux::config::settings_to_json(Settings{}));
	}
	CHECK(cordmux::config::settings_to_json(Settings{}).find("label_speaking_style") == std::string::npos);
}

TEST_CASE("合成のソースの名前ラベルのキーは、すべて変えて書き出して読み直すと同じ値に戻り、clamp_settings は何も変えない")
{
	Settings written;
	written.label_margin_pct = 3.25;
	written.label_outline_color = "#102030";
	written.label_outline_ratio = 0.125;
	written.label_show_outline = false;
	written.label_opacity_pct = 63;
	written.label_show_plate = false;
	written.label_avatar_ratio = 2.05;
	written.label_hide_when_single = false;
	written.label_show_in_slot = false;
	written.label_glow_frame = true;
	written.label_glow_avatar = false;
	written.label_glow_name = false;
	written.label_glow_blur_ratio = 1.6;
	written.label_glow_spread_ratio = 0.35;
	written.label_corner = cordmux::config::LabelCorner::TopRight;
	written.label_avatar_side = cordmux::config::AvatarSide::Right;
	written.label_dim_pct = 65;
	written.roster_show = false;
	written.roster_anchor = cordmux::config::RosterAnchor::TopRight;
	written.roster_direction = cordmux::config::RosterDirection::Row;
	written.roster_avatar_side = cordmux::config::AvatarSide::Left;
	written.roster_gap_ratio = 1.35;
	written.roster_name_max_em = 20.5;
	written.label_avatar_shape = cordmux::config::AvatarShape::Rounded;
	written.roster_show_bots = true;
	written.roster_offset_x_px = -200;
	written.roster_offset_y_px = 7680;
	const nlohmann::json def = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	const nlohmann::json chg = nlohmann::json::parse(cordmux::config::settings_to_json(written));
	for (const char *key : kCompositeLabelKeys) {
		CAPTURE(key);
		CHECK(def.at(key) != chg.at(key));
	}
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, nullptr));
	CHECK(cordmux::config::settings_to_json(back) == cordmux::config::settings_to_json(written));
	cordmux::config::clamp_settings(back);
	CHECK(cordmux::config::settings_to_json(back) == cordmux::config::settings_to_json(written));
}

TEST_CASE("clamp_settings は合成のソースの名前ラベルの値を範囲の端に収め、形の違う色と NaN を既定値に戻す")
{
	Settings s;
	s.label_margin_pct = 50.0;
	s.label_outline_color = "black";
	s.label_outline_ratio = -1.0;
	s.label_opacity_pct = 150;
	s.label_avatar_ratio = 0.0;
	s.label_glow_blur_ratio = std::nan("");
	s.label_glow_spread_ratio = 9.0;
	cordmux::config::clamp_settings(s);
	CHECK(s.label_margin_pct == 13.9);
	CHECK(s.label_outline_color == "#000000");
	CHECK(s.label_outline_ratio == 0.0);
	CHECK(s.label_opacity_pct == 100);
	CHECK(s.label_avatar_ratio == 0.4);
	CHECK(s.label_glow_blur_ratio == 0.10);
	CHECK(s.label_glow_spread_ratio == 1.0);
}

TEST_CASE("名前ラベルの隅とアバターの位置と暗さと画面共有していない参加者の並びのキーの既定値")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.label_corner == cordmux::config::LabelCorner::BottomLeft);
	CHECK(s.label_avatar_side == cordmux::config::AvatarSide::Left);
	CHECK(s.label_dim_pct == 30);
	CHECK(s.roster_show);
	CHECK(s.roster_anchor == cordmux::config::RosterAnchor::BottomRight);
	CHECK(s.roster_direction == cordmux::config::RosterDirection::Column);
	CHECK(s.roster_avatar_side == cordmux::config::AvatarSide::Right);
	CHECK(s.roster_gap_ratio == 0.25);
	CHECK(s.roster_name_max_em == 12.0);
	CHECK(s.label_avatar_shape == cordmux::config::AvatarShape::Circle);
	CHECK_FALSE(s.roster_show_bots); // Bot は既定で並びに出さない
	CHECK(s.roster_offset_x_px == 0); // 並びの位置は既定でずらさない
	CHECK(s.roster_offset_y_px == 0);
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	CHECK(j.at("roster_show_bots") == false);
	CHECK(j.at("label_corner") == "bl");
	CHECK(j.at("label_avatar_side") == "left");
	CHECK(j.at("label_avatar_shape") == "circle");
	CHECK(j.at("roster_anchor") == "br");
	CHECK(j.at("roster_direction") == "column");
	CHECK(j.at("roster_avatar_side") == "right");
}

TEST_CASE("暗さと間隔のキーは範囲の端と刻みを確認する")
{
	for (const char *ok : {"0", "5", "30", "90"}) {
		CAPTURE(ok);
		Settings s;
		CHECK(parse_settings(std::string("{\"label_dim_pct\": ") + ok + "}", &s, nullptr));
		CHECK(s.label_dim_pct == std::stoi(ok));
	}
	// 範囲の外、5 刻みでない値、小数、文字列は形の違う設定として扱う。
	for (const char *bad : {R"({"label_dim_pct": -5})", R"({"label_dim_pct": 95})", R"({"label_dim_pct": 91})",
				R"({"label_dim_pct": 32})", R"({"label_dim_pct": 30.0})", R"({"label_dim_pct": "30"})",
				R"({"label_dim_pct": null})"}) {
		CAPTURE(bad);
		Settings s;
		s.label_dim_pct = 70;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK_FALSE(error.empty());
		CHECK(s.label_dim_pct == 30); // 既定値に戻る
	}
	for (const char *ok : {"0", "0.05", "0.5", "1.35", "3", "3.0"}) {
		CAPTURE(ok);
		Settings s;
		CHECK(parse_settings(std::string("{\"roster_gap_ratio\": ") + ok + "}", &s, nullptr));
		CHECK(s.roster_gap_ratio == std::stod(ok));
	}
	for (const char *bad : {R"({"roster_gap_ratio": -0.05})", R"({"roster_gap_ratio": 3.05})",
				R"({"roster_gap_ratio": 0.51})", R"({"roster_gap_ratio": "0.5"})",
				R"({"roster_gap_ratio": true})"}) {
		CAPTURE(bad);
		Settings s;
		CHECK_FALSE(parse_settings(bad, &s, nullptr));
		CHECK(s.roster_gap_ratio == 0.25);
	}
}

TEST_CASE("アバターの位置と配置と並びのキーは、知らない値と文字列でない値を形の違う設定として扱う")
{
	Settings s;
	CHECK(parse_settings(R"({"label_avatar_side": "right"})", &s, nullptr));
	CHECK(s.label_avatar_side == cordmux::config::AvatarSide::Right);
	CHECK(parse_settings(R"({"roster_avatar_side": "left"})", &s, nullptr));
	CHECK(s.roster_avatar_side == cordmux::config::AvatarSide::Left);
	for (const char *bad :
	     {R"({"roster_avatar_side": "center"})", R"({"roster_avatar_side": 1})", R"({"label_corner": "bottom_left"})",
	      R"({"label_corner": "TL"})", R"({"label_corner": 0})", R"({"label_corner": null})",
	      R"({"label_avatar_side": "center"})", R"({"label_avatar_side": 1})", R"({"label_avatar_side": null})",
	      R"({"roster_anchor": "center"})", R"({"roster_anchor": "TL"})", R"({"roster_anchor": 0})",
	      R"({"roster_direction": "diagonal"})", R"({"roster_direction": false})", R"({"roster_show": 1})",
	      R"({"roster_show": "true"})", R"({"roster_show_bots": 1})", R"({"roster_show_bots": "false"})",
	      R"({"roster_show_bots": null})"}) {
		CAPTURE(bad);
		Settings b;
		b.roster_show = false;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		CHECK(b.roster_show); // ほかのキーも既定値に戻る
		CHECK(b.label_avatar_side == cordmux::config::AvatarSide::Left);
	}
}

TEST_CASE("アバターの形は 3 つの値を書いたとおりに読み、知らない値と文字列でない値を形の違う設定として扱う")
{
	using cordmux::config::AvatarShape;
	const std::pair<const char *, AvatarShape> shapes[] = {
		{"circle", AvatarShape::Circle}, {"rounded", AvatarShape::Rounded}, {"square", AvatarShape::Square}};
	for (const auto &[name, shape] : shapes) {
		CAPTURE(name);
		Settings s;
		// 文字列はマクロの外で作る。MSVC 19.44 の前処理は、マクロの引数の中の raw 文字列リテラルの " を区切りと
		// 読み違え、この形の行をコンパイルできない (19.51 ではできる)。
		const std::string json = std::string("{\"label_avatar_shape\": \"") + name + "\"}";
		REQUIRE(parse_settings(json, &s, nullptr));
		CHECK(s.label_avatar_shape == shape);
		const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(s));
		CHECK(j.at("label_avatar_shape") == name);
	}
	for (const char *bad : {R"({"label_avatar_shape": "oval"})", R"({"label_avatar_shape": 1})",
				R"({"label_avatar_shape": null})"}) {
		CAPTURE(bad);
		Settings s;
		CHECK_FALSE(parse_settings(bad, &s, nullptr));
		CHECK(s.label_avatar_shape == AvatarShape::Circle);
	}
}

TEST_CASE("名前ラベルの 4 つの隅は、書いたとおりに読める")
{
	using cordmux::config::LabelCorner;
	const std::pair<const char *, LabelCorner> corners[] = {
		{"tl", LabelCorner::TopLeft},
		{"tr", LabelCorner::TopRight},
		{"bl", LabelCorner::BottomLeft},
		{"br", LabelCorner::BottomRight},
	};
	for (const auto &[text, corner] : corners) {
		CAPTURE(text);
		Settings s;
		CHECK(parse_settings(std::string("{\"label_corner\": \"") + text + "\"}", &s, nullptr));
		CHECK(s.label_corner == corner);
		CHECK(nlohmann::json::parse(cordmux::config::settings_to_json(s)).at("label_corner") == text);
	}
}

TEST_CASE("8 か所の配置は、書いたとおりに読める")
{
	using cordmux::config::RosterAnchor;
	const std::pair<const char *, RosterAnchor> anchors[] = {
		{"tl", RosterAnchor::TopLeft},     {"tc", RosterAnchor::TopCenter},    {"tr", RosterAnchor::TopRight},
		{"ml", RosterAnchor::MiddleLeft},  {"mr", RosterAnchor::MiddleRight},  {"bl", RosterAnchor::BottomLeft},
		{"bc", RosterAnchor::BottomCenter}, {"br", RosterAnchor::BottomRight},
	};
	for (const auto &[text, anchor] : anchors) {
		CAPTURE(text);
		Settings s;
		CHECK(parse_settings(std::string("{\"roster_anchor\": \"") + text + "\"}", &s, nullptr));
		CHECK(s.roster_anchor == anchor);
		CHECK(nlohmann::json::parse(cordmux::config::settings_to_json(s)).at("roster_anchor") == text);
	}
}

TEST_CASE("配置と並びの合わない組は、配置に合う並びに修正する")
{
	using cordmux::config::RosterAnchor;
	using cordmux::config::RosterDirection;
	// 純粋関数: 上と下の中央は横並び、左と右の中央は縦並び、四隅は渡した並びのまま。
	for (const RosterDirection d : {RosterDirection::Row, RosterDirection::Column}) {
		CHECK(cordmux::config::roster_direction_for(RosterAnchor::TopCenter, d) == RosterDirection::Row);
		CHECK(cordmux::config::roster_direction_for(RosterAnchor::BottomCenter, d) == RosterDirection::Row);
		CHECK(cordmux::config::roster_direction_for(RosterAnchor::MiddleLeft, d) == RosterDirection::Column);
		CHECK(cordmux::config::roster_direction_for(RosterAnchor::MiddleRight, d) == RosterDirection::Column);
		for (const RosterAnchor corner : {RosterAnchor::TopLeft, RosterAnchor::TopRight, RosterAnchor::BottomLeft,
						  RosterAnchor::BottomRight})
			CHECK(cordmux::config::roster_direction_for(corner, d) == d);
	}
	// 読むとき: 合わない組があっても設定全体を既定値に戻さず、並びだけを修正する。
	Settings s;
	std::string error;
	CHECK(parse_settings(R"({"roster_anchor": "tc", "roster_direction": "column", "tile_gap_px": 8})", &s, &error));
	CHECK(error.empty());
	CHECK(s.roster_anchor == RosterAnchor::TopCenter);
	CHECK(s.roster_direction == RosterDirection::Row);
	CHECK(s.tile_gap_px == 8);
	CHECK(parse_settings(R"({"roster_anchor": "mr", "roster_direction": "row"})", &s, nullptr));
	CHECK(s.roster_direction == RosterDirection::Column);
	// 並びを書かなければ、既定の横並びが配置に合うように直る。
	CHECK(parse_settings(R"({"roster_anchor": "ml"})", &s, nullptr));
	CHECK(s.roster_direction == RosterDirection::Column);
	// 四隅は書いたとおり。
	CHECK(parse_settings(R"({"roster_anchor": "br", "roster_direction": "column"})", &s, nullptr));
	CHECK(s.roster_direction == RosterDirection::Column);
	// 書き出した設定を読み直しても同じ。
	CHECK(parse_settings(cordmux::config::settings_to_json(s), &s, nullptr));
	CHECK(s.roster_anchor == RosterAnchor::BottomRight);
	CHECK(s.roster_direction == RosterDirection::Column);
}

TEST_CASE("自分の枠の音声ソースのキーの既定値は、指定なしでしきい値が -24 dB")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.self_speaking_source_uuid.empty());
	CHECK(s.self_speaking_source_name.empty());
	CHECK(s.self_speaking_threshold_db == -24);
	CHECK(s.self_speaking_threshold_db == cordmux::config::kDefaultSelfSpeakingThresholdDb);
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	CHECK(j.at("self_speaking_source_uuid") == "");
	CHECK(j.at("self_speaking_source_name") == "");
	CHECK(j.at("self_speaking_threshold_db") == -24);
}

TEST_CASE("自分の枠の音声ソースのしきい値は、範囲の端の -96 と 0 を読め、外の値と整数でない値は既定値に戻す")
{
	for (const char *ok : {"-96", "-60", "-26", "-1", "0"}) {
		CAPTURE(ok);
		Settings s;
		CHECK(parse_settings(std::string("{\"self_speaking_threshold_db\": ") + ok + "}", &s, nullptr));
		CHECK(s.self_speaking_threshold_db == std::stoi(ok));
	}
	for (const char *bad : {R"({"self_speaking_threshold_db": -97})", R"({"self_speaking_threshold_db": 1})",
				R"({"self_speaking_threshold_db": -26.5})", R"({"self_speaking_threshold_db": "-26"})",
				R"({"self_speaking_threshold_db": null})"}) {
		CAPTURE(bad);
		Settings s;
		s.self_speaking_threshold_db = -40;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK_FALSE(error.empty());
		CHECK(s.self_speaking_threshold_db == -24);
	}
}

TEST_CASE("自分の枠の音声ソースの UUID と名前は、文字列でない値を読めない設定として扱う")
{
	for (const char *bad : {R"({"self_speaking_source_uuid": 5})", R"({"self_speaking_source_uuid": null})",
				R"({"self_speaking_source_name": true})", R"({"self_speaking_source_name": []})"}) {
		CAPTURE(bad);
		Settings s;
		s.self_speaking_source_name = "mic";
		CHECK_FALSE(parse_settings(bad, &s, nullptr));
		CHECK(s.self_speaking_source_name.empty());
	}
	Settings s;
	REQUIRE(parse_settings(R"({"self_speaking_source_uuid": "u-1", "self_speaking_source_name": "マイク"})", &s, nullptr));
	CHECK(s.self_speaking_source_uuid == "u-1");
	CHECK(s.self_speaking_source_name == "マイク");
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(s), &back, nullptr));
	CHECK(back.self_speaking_source_uuid == "u-1");
	CHECK(back.self_speaking_source_name == "マイク");
}

TEST_CASE("自分の枠に映すソースのキーの既定値は、指定なし")
{
	Settings s;
	s.self_frame_source_uuid = "u-1";
	s.self_frame_source_name = "キャプチャーボード";
	CHECK(parse_settings("{}", &s, nullptr));
	// キーがなければ既定 (指定なし)。
	CHECK(s.self_frame_source_uuid.empty());
	CHECK(s.self_frame_source_name.empty());
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	CHECK(j.at("self_frame_source_uuid") == "");
	CHECK(j.at("self_frame_source_name") == "");
}

TEST_CASE("自分の枠に映すソースの UUID と名前は、文字列でない値を読めない設定として扱う")
{
	for (const char *bad : {R"({"self_frame_source_uuid": 5})", R"({"self_frame_source_uuid": null})",
				R"({"self_frame_source_name": true})", R"({"self_frame_source_name": []})",
				R"({"self_frame_source_name": {}})"}) {
		CAPTURE(bad);
		Settings s;
		s.self_frame_source_uuid = "u-1";
		s.self_frame_source_name = "キャプチャーボード";
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK_FALSE(error.empty());
		// 読めない設定は、全部の値が既定に戻る。
		CHECK(s.self_frame_source_uuid.empty());
		CHECK(s.self_frame_source_name.empty());
	}
}

TEST_CASE("自分の枠に映すソースの UUID と名前は、書き出して読み直すと同じ値に戻り、空の文字列も読める")
{
	Settings s;
	REQUIRE(parse_settings(R"({"self_frame_source_uuid": "u-1", "self_frame_source_name": "キャプチャーボード"})", &s,
			       nullptr));
	CHECK(s.self_frame_source_uuid == "u-1");
	CHECK(s.self_frame_source_name == "キャプチャーボード");
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(s), &back, nullptr));
	CHECK(back.self_frame_source_uuid == "u-1");
	CHECK(back.self_frame_source_name == "キャプチャーボード");
	// 片方だけが空でも、そのまま持つ (名前だけで探す、UUID だけで探す)。
	Settings only_name;
	REQUIRE(parse_settings(R"({"self_frame_source_uuid": "", "self_frame_source_name": "ゲーム画面"})", &only_name, nullptr));
	CHECK(only_name.self_frame_source_uuid.empty());
	CHECK(only_name.self_frame_source_name == "ゲーム画面");
}

TEST_CASE("自分の枠のチェックの鍵は、既定が self_frame_on=false で、キーがなければ既定に戻る")
{
	const Settings defaults;
	CHECK_FALSE(defaults.self_frame_on);
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(defaults));
	CHECK(j.at("self_frame_on") == false);
	CHECK_FALSE(j.contains("self_frame_by_share"));

	Settings s;
	s.self_frame_on = true;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK_FALSE(s.self_frame_on);
}

TEST_CASE("自分の枠のチェックの鍵は、書き出して読み直すと同じ値に戻る")
{
	Settings written;
	written.self_frame_on = true;
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, nullptr));
	CHECK(back.self_frame_on);
	Settings only_on;
	REQUIRE(parse_settings(R"({"self_frame_on": true})", &only_on, nullptr));
	CHECK(only_on.self_frame_on);
}

TEST_CASE("自分の枠のチェックの鍵は、真偽値でない値を読めない設定として扱う")
{
	for (const char *bad : {R"({"self_frame_on": 1})", R"({"self_frame_on": "true"})", R"({"self_frame_on": null})"}) {
		CAPTURE(bad);
		Settings s;
		s.self_frame_on = true;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK_FALSE(error.empty());
		// 読めない設定は、全部の値が既定に戻る。
		CHECK_FALSE(s.self_frame_on);
	}
}

TEST_CASE("自分の枠を先頭に置く鍵は、既定が self_slot_first=true で、キーがなければ true (今までと同じ配置) になる")
{
	const Settings defaults;
	CHECK(defaults.self_slot_first);
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(defaults));
	CHECK(j.at("self_slot_first") == true);

	Settings s;
	s.self_slot_first = false;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.self_slot_first);
	// clamp_settings は値を変えない (範囲のない真偽値)。
	Settings off;
	off.self_slot_first = false;
	cordmux::config::clamp_settings(off);
	CHECK_FALSE(off.self_slot_first);
}

TEST_CASE("自分の枠を先頭に置く鍵は、書き出して読み直すと同じ値に戻り、真偽値でない値を読めない設定として扱う")
{
	Settings written;
	written.self_slot_first = false;
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, nullptr));
	CHECK_FALSE(back.self_slot_first);
	Settings only_off;
	REQUIRE(parse_settings(R"({"self_slot_first": false})", &only_off, nullptr));
	CHECK_FALSE(only_off.self_slot_first);

	for (const char *bad : {R"({"self_slot_first": 0})", R"({"self_slot_first": "false"})", R"({"self_slot_first": null})"}) {
		CAPTURE(bad);
		Settings b;
		b.self_slot_first = false;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		// 読めない設定は、全部の値が既定に戻る。
		CHECK(b.self_slot_first);
	}
}

TEST_CASE("名前ラベルを枠に描く鍵は、既定が label_show_in_slot=true で、キーがなければ true (今までと同じ) になる")
{
	const Settings defaults;
	CHECK(defaults.label_show_in_slot);
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(defaults));
	CHECK(j.at("label_show_in_slot") == true);
	Settings s;
	s.label_show_in_slot = false;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.label_show_in_slot);

	Settings written;
	written.label_show_in_slot = false;
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, nullptr));
	CHECK_FALSE(back.label_show_in_slot);
	for (const char *bad : {R"({"label_show_in_slot": 0})", R"({"label_show_in_slot": "false"})"}) {
		CAPTURE(bad);
		Settings b;
		b.label_show_in_slot = false;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		CHECK(b.label_show_in_slot);
	}
}

TEST_CASE("前のバージョンの self_frame_by_share を持つ settings.json は、知らない鍵として読み飛ばし、値の形も確認しない")
{
	for (const char *old : {R"({"self_frame_on": true, "self_frame_by_share": false})",
				R"({"self_frame_on": true, "self_frame_by_share": true})",
				R"({"self_frame_on": true, "self_frame_by_share": "false"})"}) {
		CAPTURE(old);
		Settings s;
		std::string error;
		REQUIRE(parse_settings(old, &s, &error));
		CHECK(error.empty());
		CHECK(s.self_frame_on);
		// 書き出し直すと、その鍵は残らない。
		CHECK_FALSE(nlohmann::json::parse(cordmux::config::settings_to_json(s)).contains("self_frame_by_share"));
	}
	// 鍵がない前のバージョンのファイルも読める。
	Settings none;
	REQUIRE(parse_settings(R"({"self_frame_on": false})", &none, nullptr));
	CHECK_FALSE(none.self_frame_on);
}

TEST_CASE("clamp_settings は自分の枠の音声ソースのしきい値を -96 から 0 に収める")
{
	Settings s;
	s.self_speaking_threshold_db = -200;
	cordmux::config::clamp_settings(s);
	CHECK(s.self_speaking_threshold_db == -96);
	s.self_speaking_threshold_db = 12;
	cordmux::config::clamp_settings(s);
	CHECK(s.self_speaking_threshold_db == 0);
	s.self_speaking_threshold_db = -40;
	cordmux::config::clamp_settings(s);
	CHECK(s.self_speaking_threshold_db == -40);
}

TEST_CASE("clamp_settings は暗さを 5 刻みの範囲に、間隔を 0.05 刻みの範囲に収め、配置に合わない並びを修正する")
{
	Settings s;
	s.label_dim_pct = 200;
	s.roster_gap_ratio = 9.0;
	s.roster_anchor = cordmux::config::RosterAnchor::BottomCenter;
	s.roster_direction = cordmux::config::RosterDirection::Column;
	cordmux::config::clamp_settings(s);
	CHECK(s.label_dim_pct == 90);
	CHECK(s.roster_gap_ratio == 3.0);
	CHECK(s.roster_direction == cordmux::config::RosterDirection::Row);

	s.label_dim_pct = -10;
	s.roster_gap_ratio = -1.0;
	cordmux::config::clamp_settings(s);
	CHECK(s.label_dim_pct == 0);
	CHECK(s.roster_gap_ratio == 0.0);

	// 刻みに合わない値は近い刻みに丸める。
	s.label_dim_pct = 33;
	s.roster_gap_ratio = 0.52;
	cordmux::config::clamp_settings(s);
	CHECK(s.label_dim_pct == 35);
	CHECK(s.roster_gap_ratio == doctest::Approx(0.5));

	s.roster_gap_ratio = std::nan("");
	cordmux::config::clamp_settings(s);
	CHECK(s.roster_gap_ratio == 0.25);
}

TEST_CASE("色の 3 つのキーは #RRGGBB の形だけを受け付け、書かれた文字のまま持つ")
{
	Settings s;
	CHECK(parse_settings(R"({"label_border_color": "#3bd67f", "label_plate_color": "#ABCDEF", "label_text_color": "#000000"})",
			     &s, nullptr));
	CHECK(s.label_border_color == "#3bd67f");
	CHECK(s.label_plate_color == "#ABCDEF");
	CHECK(s.label_text_color == "#000000");
	for (const char *bad : {R"({"label_border_color": "57F287"})", R"({"label_border_color": "#57F28"})",
				R"({"label_border_color": "#57F2877"})", R"({"label_border_color": "#57F28G"})",
				R"({"label_plate_color": "black"})", R"({"label_text_color": 16777215})",
				R"({"label_plate_color": ""})"}) {
		CAPTURE(bad);
		Settings b;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		CHECK(b.label_border_color == "#57F287");
	}
}

TEST_CASE("枠の背景の 3 つのキーの既定値は、表示する、黒、不透明度 100%")
{
	Settings s;
	CHECK(parse_settings("{}", &s, nullptr));
	CHECK(s.slot_background_show);
	CHECK(s.slot_background_color == "#000000");
	CHECK(s.slot_background_opacity_pct == 100);
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	CHECK(j.at("slot_background_show") == true);
	CHECK(j.at("slot_background_color") == "#000000");
	CHECK(j.at("slot_background_opacity_pct") == 100);
}

TEST_CASE("枠の背景の 3 つのキーは、書き出して読み直すと同じ値に戻る")
{
	Settings written;
	written.slot_background_show = false;
	written.slot_background_color = "#1a2B3c";
	written.slot_background_opacity_pct = 37;
	Settings back;
	std::string error;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, &error));
	CHECK(error.empty());
	CHECK_FALSE(back.slot_background_show);
	CHECK(back.slot_background_color == "#1a2B3c");
	CHECK(back.slot_background_opacity_pct == 37);
}

TEST_CASE("枠の背景の 3 つのキーは、形の違う値なら false を返し、すべてを既定値に戻す")
{
	for (const char *bad : {R"({"slot_background_show": 1})", R"({"slot_background_show": "true"})",
				R"({"slot_background_color": "black"})", R"({"slot_background_color": "#12345"})",
				R"({"slot_background_color": ""})", R"({"slot_background_color": 0})",
				R"({"slot_background_opacity_pct": -1})", R"({"slot_background_opacity_pct": 101})",
				R"({"slot_background_opacity_pct": 50.5})", R"({"slot_background_opacity_pct": "50"})"}) {
		CAPTURE(bad);
		Settings b;
		b.feature_enabled = false;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &b, &error));
		CHECK_FALSE(error.empty());
		CHECK(b.feature_enabled); // ほかのキーも既定値に戻る
		CHECK(b.slot_background_show);
	}
	// 範囲の端は受け付ける。
	for (const int ok : {0, 100}) {
		Settings s;
		CHECK(parse_settings(std::string("{\"slot_background_opacity_pct\": ") + std::to_string(ok) + "}", &s, nullptr));
		CHECK(s.slot_background_opacity_pct == ok);
	}
}

TEST_CASE("clamp_settings は、枠の背景の不透明度を範囲に収め、形の違う色を既定値に戻す")
{
	Settings s;
	s.slot_background_opacity_pct = 140;
	s.slot_background_color = "#12345";
	cordmux::config::clamp_settings(s);
	CHECK(s.slot_background_opacity_pct == 100);
	CHECK(s.slot_background_color == "#000000");
	s.slot_background_opacity_pct = -3;
	cordmux::config::clamp_settings(s);
	CHECK(s.slot_background_opacity_pct == 0);
}

TEST_CASE("名前ラベルに表示するものの 3 つのキーは、真偽値だけを受け付ける")
{
	Settings s;
	CHECK(parse_settings(R"({"label_show_name": false, "label_show_avatar": false, "label_show_speaking": false})", &s,
			     nullptr));
	CHECK_FALSE(s.label_show_name);
	CHECK_FALSE(s.label_show_avatar);
	CHECK_FALSE(s.label_show_speaking);
	CHECK_FALSE(parse_settings(R"({"label_show_name": 0})", &s, nullptr));
	CHECK(s.label_show_name);
}

TEST_CASE("ダイアログの大きさと位置は、文字列ならそのまま持つ (cordmux は形を決めない)")
{
	Settings s;
	CHECK(parse_settings(R"({"settings_window_geometry": "AdnQywADAAA=", "participants_window_geometry": ""})", &s,
			     nullptr));
	CHECK(s.settings_window_geometry == "AdnQywADAAA=");
	CHECK(s.participants_window_geometry.empty());
	CHECK_FALSE(parse_settings(R"({"settings_window_geometry": 1})", &s, nullptr));
	CHECK(s.settings_window_geometry.empty());
}

TEST_CASE("22 本のキーは、書き出して読み直すと同じ値に戻る")
{
	const Settings written = docs07_all_changed();
	// すべてのキーが既定値と違う (違わないキーがあると、コピーし忘れを見落とす)。
	const nlohmann::json def = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	const nlohmann::json chg = nlohmann::json::parse(cordmux::config::settings_to_json(written));
	for (const char *key : kDocs07Keys) {
		CAPTURE(key);
		CHECK(def.at(key) != chg.at(key));
	}
	Settings back;
	std::string error;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, &error));
	CHECK(error.empty());
	CHECK(cordmux::config::settings_to_json(back) == cordmux::config::settings_to_json(written));
}

TEST_CASE("clamp_settings は範囲の外の値を端に、刻みに合わない値を近い刻みに丸める")
{
	Settings s;
	s.notice_show_delay_ms = -5;
	s.notice_clear_delay_ms = 999999;
	s.first_frame_wait_ms = 10;
	s.tile_gap_px = 100;
	s.label_em_ratio_pct = 4.66;     // 近い刻みは 4.7
	s.label_plate_opacity_pct = 101;
	s.dummy_cycle_ms = 1000;
	s.record_finalize_stall_s = 0;
	s.dock_poll_ms = 0;
	s.limit_fps_divisor = 3;
	cordmux::config::clamp_settings(s);
	CHECK(s.notice_show_delay_ms == 0);
	CHECK(s.notice_clear_delay_ms == 60000);
	CHECK(s.first_frame_wait_ms == 1000);
	CHECK(s.tile_gap_px == 64);
	CHECK(s.label_em_ratio_pct == 4.7);
	CHECK(s.label_plate_opacity_pct == 100);
	CHECK(s.dummy_cycle_ms == 2000);
	CHECK(s.record_finalize_stall_s == 10);
	CHECK(s.dock_poll_ms == 50);
	CHECK(s.limit_fps_divisor == 2);

	// 丸めた後の設定は、必ず設定ファイルとして読める (ダイアログから入った値で、次の起動ですべてが既定値に
	// 戻らない)。
	Settings back;
	CHECK(parse_settings(cordmux::config::settings_to_json(s), &back, nullptr));
}

TEST_CASE("clamp_settings は長い辺の 1 から 15 を 16 に上げ、形の違う色と NaN を既定値に戻す")
{
	Settings s;
	s.limit_max_long_edge = 8; // 制限なし (0) に下げず、いちばん小さい上限に上げる
	s.label_border_color = "green";
	s.label_plate_color = "#12345";
	s.label_text_color = "";
	s.label_em_ratio_pct = std::nan("");
	cordmux::config::clamp_settings(s);
	CHECK(s.limit_max_long_edge == 16);
	CHECK(s.label_border_color == "#57F287");
	CHECK(s.label_plate_color == "#000000");
	CHECK(s.label_text_color == "#FFFFFF");
	CHECK(s.label_em_ratio_pct == 2.5);

	// 0 (制限なし) は 0 のまま。範囲の中の値と、範囲を持たない文字列は変えない。
	Settings t = docs07_all_changed();
	t.limit_max_long_edge = 0;
	t.broadcaster_user_id = "<user_id>";
	t.record_dir = "D:/rec";
	const std::string before = cordmux::config::settings_to_json(t);
	cordmux::config::clamp_settings(t);
	CHECK(cordmux::config::settings_to_json(t) == before);
}

TEST_CASE("ledger の既定値と settings の既定値は同じ値である")
{
	const Settings s{};
	CHECK(static_cast<std::int64_t>(s.first_frame_wait_ms) * 1000 == cordmux::ledger::kDefaultFirstFrameWaitUs);
	CHECK(static_cast<std::int64_t>(s.video_stall_ms) * 1000 == cordmux::ledger::kDefaultVideoStallUs);
	CHECK(static_cast<std::int64_t>(s.record_space_warn_minutes) * 60 == cordmux::ledger::kDefaultSpaceWarnSeconds);
	CHECK(s.label_em_ratio_pct == cordmux::config::kDefaultLabelEmRatioPct);
}

TEST_CASE("名前ラベルのフォントの名前は文字列のまま持ち、太さは 1 から 1000 の整数を受け付ける")
{
	Settings s;
	CHECK(parse_settings(R"({"label_font_family": "Meiryo UI", "label_font_weight": 350})", &s, nullptr));
	CHECK(s.label_font_family == "Meiryo UI");
	// 100 刻みでない太さも読む (フォントを選ぶダイアログが返した Semilight の 350 などを、次の起動で読めるように)。
	CHECK(s.label_font_weight == 350);

	// 範囲の端の値は読める。
	CHECK(parse_settings(R"({"label_font_weight": 1})", &s, nullptr));
	CHECK(s.label_font_weight == 1);
	CHECK(parse_settings(R"({"label_font_weight": 1000})", &s, nullptr));
	CHECK(s.label_font_weight == 1000);

	// 形が違えば、すべてを既定値に戻す。
	for (const char *bad : {R"({"label_font_weight": 0})", R"({"label_font_weight": 1001})",
				R"({"label_font_weight": 700.5})", R"({"label_font_weight": "bold"})",
				R"({"label_font_family": 1})"}) {
		CAPTURE(bad);
		Settings t;
		t.label_font_family = "x";
		CHECK_FALSE(parse_settings(bad, &t, nullptr));
		CHECK(t.label_font_family.empty());
		CHECK(t.label_font_weight == 700);
	}
}

TEST_CASE("斜体のキー label_font_italic は読み飛ばし、書き出さない")
{
	// 手で書いた settings.json の斜体のキーは、値が true でも、形が違っても読み飛ばす
	// (知らないキーと同じ。ほかのキーを既定値に戻さない)。
	for (const char *old_file : {R"({"label_font_weight": 400, "label_font_italic": true})",
				     R"({"label_font_weight": 400, "label_font_italic": 1})",
				     R"({"label_font_weight": 400, "label_font_italic": "yes"})"}) {
		CAPTURE(old_file);
		Settings s;
		std::string error;
		CHECK(parse_settings(old_file, &s, &error));
		CHECK(error.empty());
		CHECK(s.label_font_weight == 400);
		// 読んだ値を書き出しても、斜体のキーは出てこない (次に保存するときに消える)。
		CHECK(keys_of_json(cordmux::config::settings_to_json(s)).count("label_font_italic") == 0);
	}
}

TEST_CASE("clamp_settings は名前ラベルの太さを 1 から 1000 に収め、フォントの名前は変えない")
{
	Settings s;
	s.label_font_weight = 0;
	s.label_font_family = "  any name  ";
	cordmux::config::clamp_settings(s);
	CHECK(s.label_font_weight == 1);
	CHECK(s.label_font_family == "  any name  ");
	s.label_font_weight = 5000;
	cordmux::config::clamp_settings(s);
	CHECK(s.label_font_weight == 1000);
	s.label_font_weight = 350;
	cordmux::config::clamp_settings(s);
	CHECK(s.label_font_weight == 350);
}

TEST_CASE("settings の既定値と、描画する側と録画の側の既定値は同じ値である")
{
	const Settings s{};
	// 名前ラベルの大きさ。settings は %、sources は割合で持つ。
	CHECK(s.label_em_ratio_pct / 100.0 == doctest::Approx(cordmux::sources::kDefaultLabelEmRatio).epsilon(1e-12));
	// 録画ファイルの作成が止まったとみなすまでの時間 (秒)。
	CHECK(s.record_finalize_stall_s == cordmux::record::kDefaultFinalizeStallS);
	// 名前ラベルのフォントの太さ。ここでは定数で比べる (sources::DecorLabelParams の既定値との比較は
	// test_sources_stream_wiring.cpp にある)。
	CHECK(s.label_font_weight == cordmux::config::kDefaultLabelFontWeight);
}

// ---- 画面共有の映像の表示の緩衝 ----

TEST_CASE("表示の緩衝の既定値は 100 で、キーがない設定でも 100 になり、書き出した設定にキーがある")
{
	CHECK(Settings{}.display_buffer_ms == 100);
	Settings s;
	s.display_buffer_ms = 500;
	REQUIRE(parse_settings("{}", &s, nullptr));
	CHECK(s.display_buffer_ms == 100);
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	CHECK(j.at("display_buffer_ms").get<int>() == 100);
}

TEST_CASE("表示の緩衝は 0 から 1000 ms を受け付け、範囲の外や整数でない値なら false を返して、すべてを既定値に戻す")
{
	for (const int ok : {0, 1, 250, 1000}) {
		Settings s;
		REQUIRE(parse_settings("{\"display_buffer_ms\": " + std::to_string(ok) + "}", &s, nullptr));
		CHECK(s.display_buffer_ms == ok);
	}
	for (const char *bad : {R"({"feature_enabled": false, "display_buffer_ms": -1})",
				R"({"feature_enabled": false, "display_buffer_ms": 1001})",
				R"({"feature_enabled": false, "display_buffer_ms": 1.5})",
				R"({"feature_enabled": false, "display_buffer_ms": "100"})"}) {
		CAPTURE(bad);
		Settings s;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK_FALSE(error.empty());
		CHECK(s.feature_enabled);
		CHECK(s.display_buffer_ms == 100);
	}
}

TEST_CASE("表示の緩衝は、書き出して読み直すと同じ値に戻り、clamp_settings は範囲の端に収める")
{
	Settings written;
	written.display_buffer_ms = 300;
	Settings back;
	REQUIRE(parse_settings(cordmux::config::settings_to_json(written), &back, nullptr));
	CHECK(back.display_buffer_ms == 300);

	Settings high;
	high.display_buffer_ms = 99999;
	cordmux::config::clamp_settings(high);
	CHECK(high.display_buffer_ms == 1000);
	Settings low;
	low.display_buffer_ms = -7;
	cordmux::config::clamp_settings(low);
	CHECK(low.display_buffer_ms == 0);
}

TEST_CASE("自分の枠に映す映像は、OBS のソースかシーンの UUID か名前があれば選んでいる扱いで、どちらも空なら選んでいない")
{
	cordmux::config::Settings settings;
	CHECK_FALSE(cordmux::config::self_frame_video_selected(settings));
	settings.self_frame_source_name = "camera";
	CHECK(cordmux::config::self_frame_video_selected(settings));
	settings.self_frame_source_name.clear();
	settings.self_frame_source_uuid = "uuid";
	CHECK(cordmux::config::self_frame_video_selected(settings));
}

TEST_CASE("並び順、受信する人、開いているタブは、決まった文字列だけを読む")
{
	Settings s;
	REQUIRE(parse_settings(R"({"slot_order":"added","receive_scope":"slots","dock_tab":"display"})", &s, nullptr));
	CHECK(s.slot_order == cordmux::config::SlotOrder::Added);
	CHECK(s.receive_scope == cordmux::config::ReceiveScope::Slots);
	CHECK(s.dock_tab == cordmux::config::DockTab::Display);
	REQUIRE(parse_settings(R"({"slot_order":"name","dock_tab":"audio"})", &s, nullptr));
	CHECK(s.slot_order == cordmux::config::SlotOrder::Name);
	CHECK(s.dock_tab == cordmux::config::DockTab::Audio);

	for (const char *bad : {R"({"slot_order":"size"})", R"({"slot_order":1})", R"({"receive_scope":"all"})",
				R"({"receive_scope":true})", R"({"dock_tab":"mixer"})", R"({"dock_tab":0})"}) {
		CAPTURE(bad);
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK_FALSE(error.empty());
	}
}

TEST_CASE("「位置を固定」を外す前の設定の slot_order が fixed のときは、エラーにせず名前順として読み、保存では name で書く")
{
	Settings s;
	s.slot_order = cordmux::config::SlotOrder::Added;
	std::string error;
	REQUIRE(parse_settings(R"({"slot_order":"fixed","dock_tab":"audio"})", &s, &error));
	CHECK(error.empty());
	CHECK(s.slot_order == cordmux::config::SlotOrder::Name);
	CHECK(s.dock_tab == cordmux::config::DockTab::Audio);

	const nlohmann::json written = nlohmann::json::parse(cordmux::config::settings_to_json(s));
	CHECK(written["slot_order"] == "name");
}

TEST_CASE("新しい設定の既定値は、並び順が名前順、録画は配信と連動しない、配信中の退出を確認、枠に載せた人だけを受信する、参加者タブ")
{
	const Settings s;
	CHECK(s.slot_order == cordmux::config::SlotOrder::Name);
	CHECK_FALSE(s.record_with_stream);
	CHECK(s.confirm_while_streaming);
	CHECK(s.receive_scope == cordmux::config::ReceiveScope::Slots);
	CHECK(s.layout_hold_idle_ms == 2000);
	CHECK(s.dock_tab == cordmux::config::DockTab::People);
}

TEST_CASE("ポインターが止まってから反映するまでの時間は、範囲の外なら読まず、ウィンドウの値は範囲に収める")
{
	Settings s;
	REQUIRE(parse_settings(R"({"layout_hold_idle_ms":0})", &s, nullptr));
	CHECK(s.layout_hold_idle_ms == 0);
	CHECK_FALSE(parse_settings(R"({"layout_hold_idle_ms":-1})", &s, nullptr));
	CHECK_FALSE(parse_settings(R"({"layout_hold_idle_ms":60001})", &s, nullptr));
	CHECK_FALSE(parse_settings(R"({"record_with_stream":1})", &s, nullptr));
	CHECK_FALSE(parse_settings(R"({"confirm_while_streaming":"yes"})", &s, nullptr));

	Settings window;
	window.layout_hold_idle_ms = 100000;
	cordmux::config::clamp_settings(window);
	CHECK(window.layout_hold_idle_ms == 60000);
	window.layout_hold_idle_ms = -5;
	cordmux::config::clamp_settings(window);
	CHECK(window.layout_hold_idle_ms == 0);
}

TEST_CASE("settings.json に残っている people_sharing_only は、知らないキーとして読み飛ばし、書き出さない")
{
	for (const char *text : {R"({"people_sharing_only": true})", R"({"people_sharing_only": "yes"})"}) {
		Settings s;
		std::string error;
		CHECK(parse_settings(text, &s, &error));
		CHECK(error.empty());
		CHECK_FALSE(nlohmann::json::parse(cordmux::config::settings_to_json(s)).contains("people_sharing_only"));
	}
}

TEST_CASE("call_action_timeout_ms は既定が 5000 ms で、範囲の端を受け付け、範囲の外なら false を返す")
{
	Settings s;
	REQUIRE(parse_settings("{}", &s, nullptr));
	CHECK(s.call_action_timeout_ms == 5000);
	for (const int ok : {1000, 60000}) {
		Settings t;
		CHECK(parse_settings("{\"call_action_timeout_ms\": " + std::to_string(ok) + "}", &t, nullptr));
		CHECK(t.call_action_timeout_ms == ok);
	}
	for (const int bad : {999, 60001}) {
		Settings t;
		std::string error;
		CHECK_FALSE(parse_settings("{\"call_action_timeout_ms\": " + std::to_string(bad) + "}", &t, &error));
		CHECK_FALSE(error.empty());
	}
}

TEST_CASE("並びのずれ (roster_offset_x_px と roster_offset_y_px) は、-7680 から 7680 の整数で、範囲の外と整数でない値は読まない")
{
	for (const char *ok : {"-7680", "-200", "0", "35", "7680"}) {
		CAPTURE(ok);
		Settings s;
		const std::string json = std::string(R"({"roster_offset_x_px": )") + ok + R"(, "roster_offset_y_px": )" + ok + "}";
		REQUIRE(parse_settings(json, &s, nullptr));
		CHECK(s.roster_offset_x_px == std::stoi(ok));
		CHECK(s.roster_offset_y_px == std::stoi(ok));
	}
	for (const char *bad : {R"({"roster_offset_x_px": -7681})", R"({"roster_offset_x_px": 7681})",
				R"({"roster_offset_y_px": 1.5})", R"({"roster_offset_y_px": "10"})", R"({"roster_offset_x_px": null})"}) {
		CAPTURE(bad);
		Settings s;
		std::string error;
		CHECK_FALSE(parse_settings(bad, &s, &error));
		CHECK_FALSE(error.empty());
	}
	// 設定ウィンドウから入る値は、範囲の端に収める。
	Settings wide;
	wide.roster_offset_x_px = -9000;
	wide.roster_offset_y_px = 9000;
	cordmux::config::clamp_settings(wide);
	CHECK(wide.roster_offset_x_px == cordmux::config::kRosterOffsetMinPx);
	CHECK(wide.roster_offset_y_px == cordmux::config::kRosterOffsetMaxPx);
}

TEST_CASE("出来事の行の行数と受信のやり直しを頼むまでの時間は、既定値を持ち、範囲の中なら読み、書き出しで戻る")
{
	Settings s;
	REQUIRE(parse_settings("{}", &s, nullptr));
	CHECK(s.js_event_log_lines == 5000);
	CHECK(s.plugin_event_log_lines == 5000);
	CHECK(s.receive_restart_wait_ms == 5000);

	REQUIRE(parse_settings(R"({"js_event_log_lines": 50000, "plugin_event_log_lines": 0, "receive_restart_wait_ms": 0})",
			       &s, nullptr));
	CHECK(s.js_event_log_lines == 50000);
	CHECK(s.plugin_event_log_lines == 0);
	CHECK(s.receive_restart_wait_ms == 0);

	Settings back;
	REQUIRE(parse_settings(settings_to_json(s), &back, nullptr));
	CHECK(back.js_event_log_lines == 50000);
	CHECK(back.plugin_event_log_lines == 0);
	CHECK(back.receive_restart_wait_ms == 0);

	CHECK_FALSE(parse_settings(R"({"js_event_log_lines": 50001})", &s, nullptr));
	CHECK_FALSE(parse_settings(R"({"receive_restart_wait_ms": 600001})", &s, nullptr));
	CHECK_FALSE(parse_settings(R"({"receive_restart_wait_ms": -1})", &s, nullptr));
}

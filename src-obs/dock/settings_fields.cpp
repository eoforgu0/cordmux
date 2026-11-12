// SPDX-License-Identifier: Apache-2.0

// settings.json のキーごとの表と、それを使う関数 (settings_fields.h) を実装する。

#include "dock/settings_fields.h"

#include <cstddef>
#include <string>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace cordmux::dock {

namespace {

using Page = ledger::SettingsPage;

// 行の並びは config::settings_to_json のキーの並びと同じ。キーと書き換える所を並べて読めるように、1 行を 1 行の
// ソースに書く。列はキー、書き換える所、コピーする関数、ウィンドウのページ、再起動の要否。ページと再起動の列は、
// tests/test_dock_settings_fields.cpp が期待値と 1 行ずつ比べる。ウィンドウに置かない行のページは
// std::nullopt にする。
// clang-format off
const SettingsFieldRow kSettingsFields[] = {
	// 受信側が起動のときに読む 3 つは、変えても OBS を再起動するまで反映されない。声の遅延は入力欄を持たず、
	// 再起動を求める文を表示する場所がないので、再起動の列は偽にする。
	{"decoder", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.decoder = f.decoder; }, Page::Advanced, true},
	{"screen_audio_offset_ms", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.screen_audio_offset_ms = f.screen_audio_offset_ms; }, Page::Advanced, true},
	{"voice_audio_delay_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.voice_audio_delay_ms = f.voice_audio_delay_ms; }, std::nullopt, false},
	{"js_log_ring_lines", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.js_log_ring_lines = f.js_log_ring_lines; }, std::nullopt, false},
	{"js_event_log_lines", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.js_event_log_lines = f.js_event_log_lines; }, std::nullopt, false},
	{"plugin_log_ring_lines", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.plugin_log_ring_lines = f.plugin_log_ring_lines; }, std::nullopt, false},
	{"plugin_event_log_lines", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.plugin_event_log_lines = f.plugin_event_log_lines; }, std::nullopt, false},
	// 時間の設定 (メッセージを表示するまでと消すまでの待ち、映像が届き始めるまでの待ち、一覧の並びを動かすまでの時間、
	// 接続が切れてから枠を空にするまでの時間) とドックの更新の周期は、設定ウィンドウに置かない。変えられるのは settings.json だけ (測った更新 1 回の時間は docs/architecture.md)。
	{"dock_poll_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.dock_poll_ms = f.dock_poll_ms; }, std::nullopt, false},
	// 映像が止まったと判断するまでの時間。メッセージを表示するまでの待ち (notice_*) とは別の値。
	{"video_stall_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.video_stall_ms = f.video_stall_ms; }, std::nullopt, false},
	{"assign_mode", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.assign_mode = f.assign_mode; }, std::nullopt, false},
	// 載せた順などの並び順は、ドックの表示タブで変える。
	{"slot_order", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.slot_order = f.slot_order; }, std::nullopt, false},
	{"record_with_stream", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.record_with_stream = f.record_with_stream; }, Page::Recording, false},
	{"confirm_while_streaming", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.confirm_while_streaming = f.confirm_while_streaming; }, Page::Advanced, false},
	{"receive_scope", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.receive_scope = f.receive_scope; }, Page::Receive, false},
	{"layout_hold_idle_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.layout_hold_idle_ms = f.layout_hold_idle_ms; }, std::nullopt, false},
	{"dock_tab", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.dock_tab = f.dock_tab; }, std::nullopt, false},
	{"seen_people_max", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.seen_people_max = f.seen_people_max; }, std::nullopt, false},
	// メインアカウントは、ドックの [変更] から ActionId::SetMainAccount を通って変わる。設定ウィンドウは書き換えない。
	{"broadcaster_user_id", SettingsOwner::Action, [](const config::Settings &f, config::Settings &t) { t.broadcaster_user_id = f.broadcaster_user_id; }, std::nullopt, false},
	// 自分の枠に映す OBS のソース。表示タブの 1 つのコンボボックスが UUID と名前を書く (ドックも、見つけたソースに
	// 合わせて書き直す)。
	{"self_frame_source_uuid", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.self_frame_source_uuid = f.self_frame_source_uuid; }, std::nullopt, false},
	{"self_frame_source_name", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.self_frame_source_name = f.self_frame_source_name; }, std::nullopt, false},
	// 自分の枠に映す映像を先頭に置くか。表示タブの [先頭に配置] が書く。
	{"self_slot_first", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.self_slot_first = f.self_slot_first; }, std::nullopt, false},
	// 自分の枠の付け外しは、ドックの自分の行から ActionId::SetSelfFrame を通る。
	{"self_frame_on", SettingsOwner::Action, [](const config::Settings &f, config::Settings &t) { t.self_frame_on = f.self_frame_on; }, std::nullopt, false},
	// 実機テストで Vesktop の state.json に書き込ませないためのキー。画面には表示しない。
	{"allow_state_json_write", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.allow_state_json_write = f.allow_state_json_write; }, std::nullopt, false},
	// 録画の値は録画を始める時点で読むので、再起動は不要 (次に録画を始めたときから反映される)。
	{"record_dir", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.record_dir = f.record_dir; }, Page::Recording, false},
	{"record_output_pcm", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.record_output_pcm = f.record_output_pcm; }, Page::Recording, false},
	{"record_space_check_interval_s", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.record_space_check_interval_s = f.record_space_check_interval_s; }, std::nullopt, false},
	{"record_space_warn_minutes", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.record_space_warn_minutes = f.record_space_warn_minutes; }, Page::Recording, false},
	// 音声の置く位置を時刻に追従させる 3 つの値と、配信の声だけの置き直しのしきい値。起動のときに読み、画面には表示しない。
	{"audio_resync_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.audio_resync_ms = f.audio_resync_ms; }, std::nullopt, false},
	{"audio_drift_max_ppm", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.audio_drift_max_ppm = f.audio_drift_max_ppm; }, std::nullopt, false},
	{"audio_drift_time_constant_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.audio_drift_time_constant_ms = f.audio_drift_time_constant_ms; }, std::nullopt, false},
	{"voice_audio_resync_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.voice_audio_resync_ms = f.voice_audio_resync_ms; }, std::nullopt, false},
	{"record_finalize_wait_on_exit_s", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.record_finalize_wait_on_exit_s = f.record_finalize_wait_on_exit_s; }, std::nullopt, false},
	{"record_queue_limit", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.record_queue_limit = f.record_queue_limit; }, std::nullopt, false},
	{"record_clock_push_max_frames", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.record_clock_push_max_frames = f.record_clock_push_max_frames; }, std::nullopt, false},
	{"record_clock_reanchor_max_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.record_clock_reanchor_max_ms = f.record_clock_reanchor_max_ms; }, std::nullopt, false},
	// cordmux の有効と無効の切り替えは、ドックのメニューから ActionId::ToggleFeature を通る。
	{"feature_enabled", SettingsOwner::Action, [](const config::Settings &f, config::Settings &t) { t.feature_enabled = f.feature_enabled; }, std::nullopt, false},
	// メインアカウントの声のミュートの切り替えは、ドックの人の行から ActionId::SetVoiceMute を通る。
	{"self_voice_muted", SettingsOwner::Action, [](const config::Settings &f, config::Settings &t) { t.self_voice_muted = f.self_voice_muted; }, std::nullopt, false},
	// 受け取る画質の上限の 2 つ。OBS の実行中に反映される (呼び出し元が送信側に渡す)。
	{"limit_max_long_edge", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.limit_max_long_edge = f.limit_max_long_edge; }, Page::Receive, false},
	{"limit_fps_divisor", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.limit_fps_divisor = f.limit_fps_divisor; }, Page::Receive, false},
	// 送信側のエンコードの設定の 2 つ。上の上限と同じく OBS の実行中に送信側へ渡し、次に始まる画面共有から反映される。
	{"encoder_preference", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.encoder_preference = f.encoder_preference; }, Page::Receive, false},
	{"encode_priority", SettingsOwner::SettingsWindow, [](const config::Settings &f, config::Settings &t) { t.encode_priority = f.encode_priority; }, Page::Receive, false},
	// 同梱の 4 つのファイルを照合する周期は、画面に表示しない。
	{"vencord_check_disconnected_s", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.vencord_check_disconnected_s = f.vencord_check_disconnected_s; }, std::nullopt, false},
	{"vencord_check_connected_s", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.vencord_check_connected_s = f.vencord_check_connected_s; }, std::nullopt, false},
	{"notice_show_delay_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.notice_show_delay_ms = f.notice_show_delay_ms; }, std::nullopt, false},
	{"notice_clear_delay_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.notice_clear_delay_ms = f.notice_clear_delay_ms; }, std::nullopt, false},
	{"first_frame_wait_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.first_frame_wait_ms = f.first_frame_wait_ms; }, std::nullopt, false},
	{"receive_restart_wait_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.receive_restart_wait_ms = f.receive_restart_wait_ms; }, std::nullopt, false},
	{"link_lost_hold_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.link_lost_hold_ms = f.link_lost_hold_ms; }, std::nullopt, false},
	{"reconnect_layout_hold_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.reconnect_layout_hold_ms = f.reconnect_layout_hold_ms; }, std::nullopt, false},
	// [移動] と [退出] の結果を待つ時間は、画面に表示しない。
	{"call_action_timeout_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.call_action_timeout_ms = f.call_action_timeout_ms; }, std::nullopt, false},
	// 画面共有の映像の表示の緩衝。ドックの状態タブの「映像のバッファ」で選び、すぐ Pipeline に渡す。
	{"display_buffer_ms", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.display_buffer_ms = f.display_buffer_ms; }, std::nullopt, false},
	{"tile_gap_px", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.tile_gap_px = f.tile_gap_px; }, std::nullopt, false},
	// 枠の背景 (映像を描かない所の色)。ドックの [表示] タブの枠のまとまりの、背景で選ぶ。
	{"slot_background_show", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.slot_background_show = f.slot_background_show; }, std::nullopt, false},
	{"slot_background_color", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.slot_background_color = f.slot_background_color; }, std::nullopt, false},
	{"slot_background_opacity_pct", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.slot_background_opacity_pct = f.slot_background_opacity_pct; }, std::nullopt, false},
	// 名前ラベルのページ。
	{"label_em_ratio_pct", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_em_ratio_pct = f.label_em_ratio_pct; }, std::nullopt, false},
	{"label_show_name", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_show_name = f.label_show_name; }, std::nullopt, false},
	{"label_show_avatar", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_show_avatar = f.label_show_avatar; }, std::nullopt, false},
	{"label_show_speaking", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_show_speaking = f.label_show_speaking; }, std::nullopt, false},
	{"label_border_color", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_border_color = f.label_border_color; }, std::nullopt, false},
	{"label_plate_color", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_plate_color = f.label_plate_color; }, std::nullopt, false},
	{"label_plate_opacity_pct", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_plate_opacity_pct = f.label_plate_opacity_pct; }, std::nullopt, false},
	// 文字の色は、名前のまとまりの色の欄で選ぶ。
	{"label_text_color", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_text_color = f.label_text_color; }, std::nullopt, false},
	// フォントと太さは、名前ラベルのページの名前のまとまりにあるフォントの一覧と太さの選択肢で選ぶ。
	// 斜体は選ばせないので、斜体のキーは持たない。
	{"label_font_family", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_font_family = f.label_font_family; }, std::nullopt, false},
	{"label_font_weight", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_font_weight = f.label_font_weight; }, std::nullopt, false},
	{"label_border_rise_ms", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_border_rise_ms = f.label_border_rise_ms; }, std::nullopt, false},
	{"label_border_hold_ms", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_border_hold_ms = f.label_border_hold_ms; }, std::nullopt, false},
	{"label_border_fade_ms", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_border_fade_ms = f.label_border_fade_ms; }, std::nullopt, false},
	// 合成のソースの名前ラベルと、話している人を示す形。名前ラベルのページの、輪郭、アバター、背景、名前ラベル全体、
	// 話している人の強調のまとまりで選ぶ。
	{"label_margin_pct", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_margin_pct = f.label_margin_pct; }, std::nullopt, false},
	{"label_outline_color", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_outline_color = f.label_outline_color; }, std::nullopt, false},
	{"label_outline_ratio", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_outline_ratio = f.label_outline_ratio; }, std::nullopt, false},
	{"label_show_outline", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_show_outline = f.label_show_outline; }, std::nullopt, false},
	{"label_opacity_pct", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_opacity_pct = f.label_opacity_pct; }, std::nullopt, false},
	{"label_show_plate", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_show_plate = f.label_show_plate; }, std::nullopt, false},
	{"label_avatar_ratio", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_avatar_ratio = f.label_avatar_ratio; }, std::nullopt, false},
	{"label_hide_when_single", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_hide_when_single = f.label_hide_when_single; }, std::nullopt, false},
	{"label_show_in_slot", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_show_in_slot = f.label_show_in_slot; }, std::nullopt, false},
	{"label_glow_frame", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_glow_frame = f.label_glow_frame; }, std::nullopt, false},
	{"label_glow_avatar", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_glow_avatar = f.label_glow_avatar; }, std::nullopt, false},
	{"label_glow_name", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_glow_name = f.label_glow_name; }, std::nullopt, false},
	{"label_glow_blur_ratio", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_glow_blur_ratio = f.label_glow_blur_ratio; }, std::nullopt, false},
	{"label_glow_spread_ratio", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_glow_spread_ratio = f.label_glow_spread_ratio; }, std::nullopt, false},
	// 画面共有している参加者の名前ラベルの隅とアバターの位置、話していない人の暗さ、画面共有していない参加者の並び。
	// 名前ラベルのページの、話している人の強調、画面共有している参加者、画面共有していない参加者のまとまりで選ぶ。
	{"label_corner", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_corner = f.label_corner; }, std::nullopt, false},
	{"label_avatar_side", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_avatar_side = f.label_avatar_side; }, std::nullopt, false},
	{"label_avatar_shape", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_avatar_shape = f.label_avatar_shape; }, std::nullopt, false},
	{"label_dim_pct", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.label_dim_pct = f.label_dim_pct; }, std::nullopt, false},
	// 自分の枠の人が話しているかを決める OBS の音声ソースとしきい値。名前ラベルのページの、話している人の強調のまとまりで選ぶ。
	// 音声ソースの UUID と名前は、設定ウィンドウの 1 つのコンボボックスが書く (ドックも、見つけたソースに合わせて書き直す)。
	{"self_speaking_source_uuid", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.self_speaking_source_uuid = f.self_speaking_source_uuid; }, std::nullopt, false},
	{"self_speaking_source_name", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.self_speaking_source_name = f.self_speaking_source_name; }, std::nullopt, false},
	{"self_speaking_threshold_db", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.self_speaking_threshold_db = f.self_speaking_threshold_db; }, std::nullopt, false},
	{"roster_show", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_show = f.roster_show; }, std::nullopt, false},
	{"roster_anchor", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_anchor = f.roster_anchor; }, std::nullopt, false},
	{"roster_direction", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_direction = f.roster_direction; }, std::nullopt, false},
	{"roster_avatar_side", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_avatar_side = f.roster_avatar_side; }, std::nullopt, false},
	{"roster_gap_ratio", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_gap_ratio = f.roster_gap_ratio; }, std::nullopt, false},
	{"roster_name_max_em", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_name_max_em = f.roster_name_max_em; }, std::nullopt, false},
	{"roster_show_bots", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_show_bots = f.roster_show_bots; }, std::nullopt, false},
	{"roster_offset_x_px", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_offset_x_px = f.roster_offset_x_px; }, std::nullopt, false},
	{"roster_offset_y_px", SettingsOwner::DockTab, [](const config::Settings &f, config::Settings &t) { t.roster_offset_y_px = f.roster_offset_y_px; }, std::nullopt, false},
	{"dummy_cycle_ms", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.dummy_cycle_ms = f.dummy_cycle_ms; }, std::nullopt, false},
	{"record_finalize_stall_s", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.record_finalize_stall_s = f.record_finalize_stall_s; }, std::nullopt, false},
	// ウィンドウの大きさと位置は、ウィンドウを閉じたときに製品が書く (利用者には見せないので FileOnly)。
	{"settings_window_geometry", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.settings_window_geometry = f.settings_window_geometry; }, std::nullopt, false},
	{"participants_window_geometry", SettingsOwner::FileOnly, [](const config::Settings &f, config::Settings &t) { t.participants_window_geometry = f.participants_window_geometry; }, std::nullopt, false},
};
// clang-format on

constexpr std::size_t kSettingsFieldCount = sizeof(kSettingsFields) / sizeof(kSettingsFields[0]);

} // namespace

std::size_t settings_field_count()
{
	return kSettingsFieldCount;
}

const SettingsFieldRow &settings_field_at(std::size_t index)
{
	return kSettingsFields[index < kSettingsFieldCount ? index : kSettingsFieldCount - 1];
}

void apply_settings_page_fields(const config::Settings &from_page, config::Settings &current)
{
	for (const SettingsFieldRow &row : kSettingsFields) {
		if (row.owner == SettingsOwner::SettingsWindow)
			row.copy(from_page, current);
	}
}

void apply_look_tab_fields(const config::Settings &from_tab, config::Settings &current)
{
	for (const SettingsFieldRow &row : kSettingsFields) {
		if (row.owner == SettingsOwner::DockTab)
			row.copy(from_tab, current);
	}
}

bool look_tab_fields_differ(const config::Settings &a, const config::Settings &b)
{
	const nlohmann::json ja = nlohmann::json::parse(config::settings_to_json(a));
	const nlohmann::json jb = nlohmann::json::parse(config::settings_to_json(b));
	for (const SettingsFieldRow &row : kSettingsFields) {
		if (row.owner != SettingsOwner::DockTab)
			continue;
		const auto x = ja.find(row.json_key);
		const auto y = jb.find(row.json_key);
		if (x == ja.end() || y == jb.end())
			continue;
		if (*x != *y)
			return true;
	}
	return false;
}

bool restart_pending_on_page(const config::Settings &effective_at_start, const config::Settings &edited,
			     ledger::SettingsPage page)
{
	const nlohmann::json before = nlohmann::json::parse(config::settings_to_json(effective_at_start));
	const nlohmann::json after = nlohmann::json::parse(config::settings_to_json(edited));
	for (const SettingsFieldRow &row : kSettingsFields) {
		if (row.owner != SettingsOwner::SettingsWindow || row.page != std::optional<ledger::SettingsPage>(page) ||
		    !row.restart_required)
			continue;
		const auto b = before.find(row.json_key);
		const auto a = after.find(row.json_key);
		if (b == before.end() || a == after.end())
			continue; // settings_to_json が書かないキーは表にない (テストがキーの集合の一致を確認する)
		if (*b != *a)
			return true;
	}
	return false;
}

} // namespace cordmux::dock

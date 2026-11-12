// SPDX-License-Identifier: Apache-2.0

// dock/settings_fields.h の単体テスト。値を書き換える所の表 (settings_fields.cpp) が settings.json のすべての
// キーを持つことと、設定ウィンドウがどの値をコピーするか、どのページに置くか、再起動が必要かが期待どおりかを
// 確認する。Qt を起動しない (settings_fields.h は Qt にも libobs にも依存しない)。

#include "doctest.h"

#include "config/settings.h"
#include "dock/settings_fields.h"
#include "ledger/observation.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using cordmux::config::Settings;
using cordmux::dock::apply_look_tab_fields;
using cordmux::dock::apply_settings_page_fields;
using cordmux::dock::look_tab_fields_differ;
using cordmux::dock::restart_pending_on_page;
using cordmux::dock::settings_field_at;
using cordmux::dock::settings_field_count;
using cordmux::dock::SettingsOwner;
using cordmux::ledger::SettingsPage;

namespace {

// settings_to_json が書くキーの集合 (settings.json のすべてのキー)。
std::set<std::string> json_keys_of(const Settings &settings)
{
	std::set<std::string> keys;
	const nlohmann::json j = nlohmann::json::parse(cordmux::config::settings_to_json(settings));
	for (auto it = j.begin(); it != j.end(); ++it)
		keys.insert(it.key());
	return keys;
}

// すべてのフィールドを既定値と違う値にした設定 (範囲の中の値。parse_settings が読めることも下で確認する)。
// settings.h にフィールドを追加したら、ここにも追加する (追加し忘れると、下の「すべてのキーで既定値と違う」の
// テストが失敗する)。
Settings all_changed()
{
	Settings s;
	s.decoder = cordmux::config::DecoderChoice::D3D11VA;
	s.screen_audio_offset_ms = 120;
	s.voice_audio_delay_ms = 55;
	s.js_log_ring_lines = 321;
	s.js_event_log_lines = 4321;
	s.plugin_log_ring_lines = 123;
	s.plugin_event_log_lines = 1234;
	s.dock_poll_ms = 777;
	s.video_stall_ms = 3333;
	s.assign_mode = cordmux::config::AssignMode::Auto;
	s.slot_order = cordmux::config::SlotOrder::Added;
	s.record_with_stream = true;
	s.confirm_while_streaming = false;
	s.receive_scope = cordmux::config::ReceiveScope::Everyone;
	s.layout_hold_idle_ms = 3500;
	s.dock_tab = cordmux::config::DockTab::Status;
	s.seen_people_max = 50;
	s.broadcaster_user_id = "<user_id>";
	s.self_frame_source_uuid = "66666666-7777-8888-9999-000000000000";
	s.self_frame_source_name = "capture";
	s.self_frame_on = true;
	s.allow_state_json_write = false;
	s.record_dir = "D:/rec";
	s.record_output_pcm = cordmux::config::RecordPcmFormat::Int24;
	s.record_space_check_interval_s = 60;
	s.record_space_warn_minutes = 10;
	s.audio_resync_ms = 20;
	s.audio_drift_max_ppm = 500;
	s.audio_drift_time_constant_ms = 4000;
	s.voice_audio_resync_ms = 90;
	s.record_finalize_wait_on_exit_s = 5;
	s.record_queue_limit = 2048;
	s.record_clock_push_max_frames = 3;
	s.record_clock_reanchor_max_ms = 2000;
	s.feature_enabled = false;
	s.self_voice_muted = false;
	s.limit_max_long_edge = 1280;
	s.limit_fps_divisor = 2;
	s.encoder_preference = cordmux::config::EncoderPreference::Software;
	s.encode_priority = cordmux::config::EncodePriority::Quality;
	s.vencord_check_disconnected_s = 5;
	s.vencord_check_connected_s = 60;
	s.notice_show_delay_ms = 2500;
	s.notice_clear_delay_ms = 0;
	s.first_frame_wait_ms = 8000;
	s.receive_restart_wait_ms = 9000;
	s.link_lost_hold_ms = 500;
	s.reconnect_layout_hold_ms = 4000;
	s.call_action_timeout_ms = 8000;
	s.display_buffer_ms = 250;
	s.tile_gap_px = 4;
	s.slot_background_show = false;
	s.self_slot_first = false;
	s.slot_background_color = "#203040";
	s.slot_background_opacity_pct = 40;
	s.label_em_ratio_pct = 5.5;
	s.label_show_name = false;
	s.label_show_avatar = false;
	s.label_show_speaking = false;
	s.label_border_color = "#3BD67F";
	s.label_plate_color = "#101010";
	s.label_plate_opacity_pct = 60;
	s.label_text_color = "#EEEEEE";
	s.label_font_family = "Meiryo UI";
	s.label_font_weight = 400;
	s.label_border_rise_ms = 120;
	s.label_border_hold_ms = 600;
	s.label_border_fade_ms = 300;
	s.label_margin_pct = 3.0;
	s.label_outline_color = "#102030";
	s.label_outline_ratio = 0.2;
	s.label_show_outline = false;
	s.label_opacity_pct = 70;
	s.label_show_plate = false;
	s.label_avatar_ratio = 2.0;
	s.label_hide_when_single = false;
	s.label_show_in_slot = false;
	s.label_glow_frame = true;
	s.label_glow_avatar = false;
	s.label_glow_name = false;
	s.label_glow_blur_ratio = 1.5;
	s.label_glow_spread_ratio = 0.25;
	s.label_corner = cordmux::config::LabelCorner::TopRight;
	s.label_avatar_side = cordmux::config::AvatarSide::Right;
	s.label_avatar_shape = cordmux::config::AvatarShape::Rounded;
	s.label_dim_pct = 60;
	s.self_speaking_source_uuid = "11111111-2222-3333-4444-555555555555";
	s.self_speaking_source_name = "mic";
	s.self_speaking_threshold_db = -40;
	s.roster_show = false;
	s.roster_anchor = cordmux::config::RosterAnchor::TopRight;
	s.roster_direction = cordmux::config::RosterDirection::Row;
	s.roster_avatar_side = cordmux::config::AvatarSide::Left;
	s.roster_gap_ratio = 1.25;
	s.roster_name_max_em = 8.5;
	s.roster_show_bots = true;
	s.roster_offset_x_px = -200;
	s.roster_offset_y_px = 35;
	s.dummy_cycle_ms = 6000;
	s.record_finalize_stall_s = 300;
	s.settings_window_geometry = "AdnQywADAAA=";
	s.participants_window_geometry = "AdnQywADAAB=";
	return s;
}

// 値を書き換える所の表の、期待値の 1 行。page は owner == SettingsWindow の行だけが持つ (ほかの行は std::nullopt)。
struct T12Row {
	const char *key;
	SettingsOwner owner;
	std::optional<SettingsPage> page;
	bool restart;
};

constexpr SettingsOwner kWindow = SettingsOwner::SettingsWindow;
constexpr SettingsOwner kTab = SettingsOwner::DockTab;
constexpr SettingsOwner kAction = SettingsOwner::Action;
constexpr SettingsOwner kFile = SettingsOwner::FileOnly;

// 期待値の表。製品の表 (settings_fields.cpp) を意図して変えるときは、ここも同じように修正する (意図しない変更を、この
// テストで見つけるため)。
// clang-format off
const T12Row kT12[] = {
	{"decoder", kWindow, SettingsPage::Advanced, true},
	{"screen_audio_offset_ms", kWindow, SettingsPage::Advanced, true},
	{"voice_audio_delay_ms", kFile, std::nullopt, false},
	{"js_log_ring_lines", kFile, std::nullopt, false},
	{"js_event_log_lines", kFile, std::nullopt, false},
	{"plugin_log_ring_lines", kFile, std::nullopt, false},
	{"plugin_event_log_lines", kFile, std::nullopt, false},
	{"dock_poll_ms", kFile, std::nullopt, false},
	{"video_stall_ms", kFile, std::nullopt, false},
	{"assign_mode", kTab, std::nullopt, false},
	{"slot_order", kTab, std::nullopt, false},
	{"record_with_stream", kWindow, SettingsPage::Recording, false},
	{"confirm_while_streaming", kWindow, SettingsPage::Advanced, false},
	{"receive_scope", kWindow, SettingsPage::Receive, false},
	{"layout_hold_idle_ms", kFile, std::nullopt, false},
	{"dock_tab", kFile, std::nullopt, false},
	{"seen_people_max", kFile, std::nullopt, false},
	{"broadcaster_user_id", kAction, std::nullopt, false},
	{"self_frame_source_uuid", kTab, std::nullopt, false},
	{"self_frame_source_name", kTab, std::nullopt, false},
	{"self_slot_first", kTab, std::nullopt, false},
	{"self_frame_on", kAction, std::nullopt, false},
	{"allow_state_json_write", kFile, std::nullopt, false},
	{"record_dir", kWindow, SettingsPage::Recording, false},
	{"record_output_pcm", kWindow, SettingsPage::Recording, false},
	{"record_space_check_interval_s", kFile, std::nullopt, false},
	{"record_space_warn_minutes", kWindow, SettingsPage::Recording, false},
	{"audio_resync_ms", kFile, std::nullopt, false},
	{"audio_drift_max_ppm", kFile, std::nullopt, false},
	{"audio_drift_time_constant_ms", kFile, std::nullopt, false},
	{"voice_audio_resync_ms", kFile, std::nullopt, false},
	{"record_finalize_wait_on_exit_s", kFile, std::nullopt, false},
	{"record_queue_limit", kFile, std::nullopt, false},
	{"record_clock_push_max_frames", kFile, std::nullopt, false},
	{"record_clock_reanchor_max_ms", kFile, std::nullopt, false},
	{"feature_enabled", kAction, std::nullopt, false},
	{"self_voice_muted", kAction, std::nullopt, false},
	{"limit_max_long_edge", kWindow, SettingsPage::Receive, false},
	{"limit_fps_divisor", kWindow, SettingsPage::Receive, false},
	{"encoder_preference", kWindow, SettingsPage::Receive, false},
	{"encode_priority", kWindow, SettingsPage::Receive, false},
	{"vencord_check_disconnected_s", kFile, std::nullopt, false},
	{"vencord_check_connected_s", kFile, std::nullopt, false},
	{"notice_show_delay_ms", kFile, std::nullopt, false},
	{"notice_clear_delay_ms", kFile, std::nullopt, false},
	{"first_frame_wait_ms", kFile, std::nullopt, false},
	{"receive_restart_wait_ms", kFile, std::nullopt, false},
	{"link_lost_hold_ms", kFile, std::nullopt, false},
	{"reconnect_layout_hold_ms", kFile, std::nullopt, false},
	{"call_action_timeout_ms", kFile, std::nullopt, false},
	{"display_buffer_ms", kTab, std::nullopt, false},
	{"tile_gap_px", kTab, std::nullopt, false},
	{"slot_background_show", kTab, std::nullopt, false},
	{"slot_background_color", kTab, std::nullopt, false},
	{"slot_background_opacity_pct", kTab, std::nullopt, false},
	{"label_em_ratio_pct", kTab, std::nullopt, false},
	{"label_show_name", kTab, std::nullopt, false},
	{"label_show_avatar", kTab, std::nullopt, false},
	{"label_show_speaking", kTab, std::nullopt, false},
	{"label_border_color", kTab, std::nullopt, false},
	{"label_plate_color", kTab, std::nullopt, false},
	{"label_plate_opacity_pct", kTab, std::nullopt, false},
	{"label_text_color", kTab, std::nullopt, false},
	// 名前ラベルのフォントの名前と太さ (表示タブの名前ラベルの組)。斜体のキーは持たない (日本語の
	// 斜体は文字を傾けるだけで読みにくいので、選べるようにしない)。
	{"label_font_family", kTab, std::nullopt, false},
	{"label_font_weight", kTab, std::nullopt, false},
	{"label_border_rise_ms", kTab, std::nullopt, false},
	{"label_border_hold_ms", kTab, std::nullopt, false},
	{"label_border_fade_ms", kTab, std::nullopt, false},
	{"label_margin_pct", kTab, std::nullopt, false},
	{"label_outline_color", kTab, std::nullopt, false},
	{"label_outline_ratio", kTab, std::nullopt, false},
	{"label_show_outline", kTab, std::nullopt, false},
	{"label_opacity_pct", kTab, std::nullopt, false},
	{"label_show_plate", kTab, std::nullopt, false},
	{"label_avatar_ratio", kTab, std::nullopt, false},
	{"label_hide_when_single", kTab, std::nullopt, false},
	{"label_show_in_slot", kTab, std::nullopt, false},
	{"label_glow_frame", kTab, std::nullopt, false},
	{"label_glow_avatar", kTab, std::nullopt, false},
	{"label_glow_name", kTab, std::nullopt, false},
	{"label_glow_blur_ratio", kTab, std::nullopt, false},
	{"label_glow_spread_ratio", kTab, std::nullopt, false},
	{"label_corner", kTab, std::nullopt, false},
	{"label_avatar_side", kTab, std::nullopt, false},
	{"label_avatar_shape", kTab, std::nullopt, false},
	{"label_dim_pct", kTab, std::nullopt, false},
	{"self_speaking_source_uuid", kTab, std::nullopt, false},
	{"self_speaking_source_name", kTab, std::nullopt, false},
	{"self_speaking_threshold_db", kTab, std::nullopt, false},
	{"roster_show", kTab, std::nullopt, false},
	{"roster_anchor", kTab, std::nullopt, false},
	{"roster_direction", kTab, std::nullopt, false},
	{"roster_avatar_side", kTab, std::nullopt, false},
	{"roster_gap_ratio", kTab, std::nullopt, false},
	{"roster_name_max_em", kTab, std::nullopt, false},
	{"roster_show_bots", kTab, std::nullopt, false},
	{"roster_offset_x_px", kTab, std::nullopt, false},
	{"roster_offset_y_px", kTab, std::nullopt, false},
	{"dummy_cycle_ms", kFile, std::nullopt, false},
	{"record_finalize_stall_s", kFile, std::nullopt, false},
	{"settings_window_geometry", kFile, std::nullopt, false},
	{"participants_window_geometry", kFile, std::nullopt, false},
};
// clang-format on

} // namespace

TEST_CASE("値を書き換える所の表のキーは、settings.json のすべてのキーとちょうど一致する")
{
	const std::set<std::string> json_keys = json_keys_of(Settings{});

	std::set<std::string> table_keys;
	std::size_t duplicates = 0;
	for (std::size_t i = 0; i < settings_field_count(); ++i) {
		const std::string key = settings_field_at(i).json_key;
		if (!table_keys.insert(key).second)
			++duplicates;
		CHECK(settings_field_at(i).copy != nullptr);
	}

	CHECK(duplicates == 0);
	CHECK(table_keys.size() == settings_field_count());
	// settings.h にキーを追加して表に行を追加し忘れると、ここで失敗する (そのキーを画面から変えられるように
	// するかを、必ず決めさせるため)。逆に、表にだけあるキー (綴りの誤り) もここで失敗する。
	CHECK(table_keys == json_keys);
	for (const std::string &key : json_keys) {
		CAPTURE(key);
		CHECK(table_keys.count(key) == 1);
	}
}

TEST_CASE("すべてを変えた設定は、すべてのキーで既定値と違い、設定のファイルとして読める (ほかのテストの前提)")
{
	const Settings changed = all_changed();
	const nlohmann::json a = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	const nlohmann::json b = nlohmann::json::parse(cordmux::config::settings_to_json(changed));
	for (auto it = a.begin(); it != a.end(); ++it) {
		CAPTURE(it.key());
		CHECK(it.value() != b.at(it.key()));
	}
	Settings parsed;
	std::string error;
	CHECK(cordmux::config::parse_settings(cordmux::config::settings_to_json(changed), &parsed, &error));
}

TEST_CASE("設定ウィンドウから返った設定からは、設定ウィンドウが持つフィールドだけをコピーする")
{
	const Settings from_page = all_changed();
	Settings current; // 既定値 (ウィンドウが持たない値、ActionId を通る切り替え、ドックのフラグを持っている側)
	apply_settings_page_fields(from_page, current);

	const nlohmann::json page = nlohmann::json::parse(cordmux::config::settings_to_json(from_page));
	const nlohmann::json before = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	const nlohmann::json after = nlohmann::json::parse(cordmux::config::settings_to_json(current));

	std::size_t window_rows = 0;
	for (std::size_t i = 0; i < settings_field_count(); ++i) {
		const auto &row = settings_field_at(i);
		CAPTURE(row.json_key);
		if (row.owner == SettingsOwner::SettingsWindow) {
			++window_rows;
			// ウィンドウで変えた値がコピーされる (コピーし忘れると、変えた値がメッセージもログも出さずに
			// 捨てられる)。
			CHECK(after.at(row.json_key) == page.at(row.json_key));
		} else {
			// ウィンドウの外の値は元のまま (ウィンドウが最後に描画したときの古い値で上書きしない)。
			CHECK(after.at(row.json_key) == before.at(row.json_key));
		}
	}
	// 設定ウィンドウが入力欄を持つ値の数。
	CHECK(window_rows == 12);
}

TEST_CASE("すべての行の、値を書き換える所、ページ、再起動の要否が期待値の表と一致する")
{
	// 期待値の表の行数、製品の表の行数、settings.json のキーの数が同じ (どれかの追加し忘れで失敗する)。
	CHECK(sizeof(kT12) / sizeof(kT12[0]) == settings_field_count());
	CHECK(settings_field_count() == json_keys_of(Settings{}).size());

	std::map<std::string, const cordmux::dock::SettingsFieldRow *> product;
	for (std::size_t i = 0; i < settings_field_count(); ++i)
		product[settings_field_at(i).json_key] = &settings_field_at(i);

	std::map<SettingsOwner, int> owners;
	std::map<SettingsPage, int> window_pages;
	int window_rows_without_page = 0;
	int restart_rows = 0;
	for (const T12Row &want : kT12) {
		CAPTURE(want.key);
		REQUIRE(product.count(want.key) == 1);
		const auto &got = *product.at(want.key);
		CHECK(got.owner == want.owner);
		CHECK(got.page == want.page);
		CHECK(got.restart_required == want.restart);
		++owners[got.owner];
		if (got.owner == SettingsOwner::SettingsWindow) {
			if (got.page.has_value())
				++window_pages[*got.page];
			else
				++window_rows_without_page;
		}
		if (got.restart_required) {
			++restart_rows;
			// 再起動が必要なのは、ウィンドウの入力欄の値だけ (ウィンドウの外の値には、再起動を求める文を
			// 表示する場所がない)。
			CHECK(got.owner == SettingsOwner::SettingsWindow);
		}
		// ウィンドウの外の行はページを持たない。ウィンドウの行は必ずページを持つ。
		CHECK((got.owner == SettingsOwner::SettingsWindow) == got.page.has_value());
	}
	CHECK(window_rows_without_page == 0);
	// 書き換える所ごとの数: ウィンドウ 12、ドックのタブ 53 (表示タブ 52、状態タブの映像のバッファ)、Action 4、FileOnly 33 (ドックの更新間隔 dock_poll_ms と、メッセージと一覧の並びと枠を空にするまでの時間 5 つと、接続し直した後に配置を保つ時間と、出来事の行の行数 2 つと、受信のやり直しを頼むまでの時間と、音声の置く位置を時刻に追従させる値 3 つと、配信の声の置き直しのしきい値は設定ファイルでだけ変える)。
	CHECK(owners[SettingsOwner::SettingsWindow] == 12);
	CHECK(owners[SettingsOwner::DockTab] == 53);
	CHECK(owners[SettingsOwner::Action] == 4);
	CHECK(owners[SettingsOwner::FileOnly] == 33);
	// ページごとの入力欄の数 (録画 4、映像の受信 5、詳細設定 3、バージョン情報 0)。
	CHECK(window_pages[SettingsPage::Recording] == 4);
	CHECK(window_pages[SettingsPage::Receive] == 5);
	CHECK(window_pages[SettingsPage::Advanced] == 3);
	CHECK(window_pages[SettingsPage::About] == 0);
	// 再起動が必要なのは、受信側が起動するときにだけ読む入力欄の 2 つの値。
	CHECK(restart_rows == 2);
}

TEST_CASE("オンとオフの切り替えは ActionId を通し、照合の間隔と書き込みの許可は画面に表示しない")
{
	std::map<std::string, SettingsOwner> owner_of;
	for (std::size_t i = 0; i < settings_field_count(); ++i)
		owner_of[settings_field_at(i).json_key] = settings_field_at(i).owner;

	// cordmux の有効と無効は ActionId を通る (押せるかどうかは ledger::apply が決める)。
	// 製品の側でも変わる値は、[OK] で反映するウィンドウに置かない。メインアカウントはドックの [変更] で変わるので、
	// [適用] が古い値で上書きしない。
	CHECK(owner_of.at("feature_enabled") == SettingsOwner::Action);
	CHECK(owner_of.at("self_voice_muted") == SettingsOwner::Action);
	CHECK(owner_of.at("self_frame_on") == SettingsOwner::Action);
	CHECK(owner_of.at("broadcaster_user_id") == SettingsOwner::Action);
	// 画面に表示しないキー (state.json に書き込まないテストのためのキーと、同梱のファイルの照合の間隔)。
	CHECK(owner_of.at("allow_state_json_write") == SettingsOwner::FileOnly);
	CHECK(owner_of.at("vencord_check_disconnected_s") == SettingsOwner::FileOnly);
	CHECK(owner_of.at("vencord_check_connected_s") == SettingsOwner::FileOnly);
}

TEST_CASE("再起動を求める文は、そのページの再起動が必要な入力欄が、起動のときの値と違う間だけ表示する")
{
	const Settings at_start{};
	Settings edited = at_start;
	for (const SettingsPage page : {SettingsPage::Recording, SettingsPage::Receive, SettingsPage::Advanced,
					SettingsPage::About})
		CHECK_FALSE(restart_pending_on_page(at_start, edited, page));

	// 詳細設定のデコーダーを変えると、詳細設定のページだけが再起動を求める。
	edited.decoder = cordmux::config::DecoderChoice::D3D11VA;
	CHECK(restart_pending_on_page(at_start, edited, SettingsPage::Advanced));
	CHECK_FALSE(restart_pending_on_page(at_start, edited, SettingsPage::Receive));

	// 元に戻せば、文は消える。
	edited.decoder = at_start.decoder;
	CHECK_FALSE(restart_pending_on_page(at_start, edited, SettingsPage::Advanced));

	// 再起動が不要な入力欄 (録画の保存先、ドックの更新間隔、表示タブの値) をいくら変えても表示しない。
	edited = all_changed();
	edited.decoder = at_start.decoder;
	edited.screen_audio_offset_ms = at_start.screen_audio_offset_ms;
	edited.voice_audio_delay_ms = at_start.voice_audio_delay_ms;
	for (const SettingsPage page : {SettingsPage::Recording, SettingsPage::Receive, SettingsPage::Advanced,
					SettingsPage::About})
		CHECK_FALSE(restart_pending_on_page(at_start, edited, page));

	// 入力欄を持たない声の遅延を変えても表示しない。
	edited.voice_audio_delay_ms = 60;
	CHECK_FALSE(restart_pending_on_page(at_start, edited, SettingsPage::Advanced));

	// 画面の音の補正だけを変えても表示する (入力欄を持つ 2 つのどちらを変えても表示する)。
	edited.screen_audio_offset_ms = at_start.screen_audio_offset_ms + 1;
	CHECK(restart_pending_on_page(at_start, edited, SettingsPage::Advanced));
}

TEST_CASE("表示タブから返った設定からは、表示タブが持つフィールドだけをコピーする")
{
	const Settings from_tab = all_changed();
	Settings current; // 既定値 (ウィンドウの値、Action の値、ファイルだけの値を持っている側)
	apply_look_tab_fields(from_tab, current);

	const nlohmann::json tab = nlohmann::json::parse(cordmux::config::settings_to_json(from_tab));
	const nlohmann::json before = nlohmann::json::parse(cordmux::config::settings_to_json(Settings{}));
	const nlohmann::json after = nlohmann::json::parse(cordmux::config::settings_to_json(current));
	std::size_t tab_rows = 0;
	for (std::size_t i = 0; i < settings_field_count(); ++i) {
		const auto &row = settings_field_at(i);
		CAPTURE(row.json_key);
		if (row.owner == SettingsOwner::DockTab) {
			++tab_rows;
			CHECK(after.at(row.json_key) == tab.at(row.json_key));
		} else {
			CHECK(after.at(row.json_key) == before.at(row.json_key));
		}
	}
	CHECK(tab_rows == 53);
}

TEST_CASE("設定ウィンドウの [適用] は、表示タブで変えた値とメインアカウントを古い値で上書きしない")
{
	// ウィンドウを開いた時点の設定 (表示タブとメインアカウントの値が古い)。
	Settings page_copy;
	page_copy.tile_gap_px = 0;
	page_copy.label_show_name = true;
	page_copy.slot_order = cordmux::config::SlotOrder::Name;
	page_copy.broadcaster_user_id = "<user_id_old>";
	page_copy.record_dir = "D:/rec";
	// その後、ドックの表示タブと [変更] で変えた設定。
	Settings current;
	current.tile_gap_px = 24;
	current.label_show_name = false;
	current.slot_order = cordmux::config::SlotOrder::Added;
	current.broadcaster_user_id = "<user_id_new>";

	apply_settings_page_fields(page_copy, current);

	CHECK(current.tile_gap_px == 24);
	CHECK_FALSE(current.label_show_name);
	CHECK(current.slot_order == cordmux::config::SlotOrder::Added);
	CHECK(current.broadcaster_user_id == "<user_id_new>");
	// ウィンドウの入力欄の値はコピーされる。
	CHECK(current.record_dir == "D:/rec");
}

TEST_CASE("表示タブの値が違うかは、表示タブの行だけで比べる")
{
	Settings a;
	Settings b = a;
	CHECK_FALSE(look_tab_fields_differ(a, b));
	b.record_dir = "D:/rec";             // ウィンドウの値は比べない
	b.broadcaster_user_id = "<user_id>"; // Action の値も比べない
	CHECK_FALSE(look_tab_fields_differ(a, b));
	b.label_glow_name = !b.label_glow_name;
	CHECK(look_tab_fields_differ(a, b));
	b = a;
	b.self_frame_source_name = "capture";
	CHECK(look_tab_fields_differ(a, b));
}

// SPDX-License-Identifier: Apache-2.0

// cordmux.dll のエントリポイント (obs_module_load と obs_module_unload)。settings.json を読み、
// Listener (ws/listener.h)、Pipeline (pipeline/pipeline.h)、OsProbe (platform/os_probe.h)、ドック (dock/) を作って
// 結び付け、OBS のフロントエンドのイベントを Pipeline とドックに渡す。obs_module_unload は、作ったものを止めてから、
// 作ったのと逆の順に破棄する。cordmux.dll が書くログの行は、すべてこのファイルの guard_and_log を通る (トークンと
// ポート番号のチェック)。ログの出力先は Listener の io_thread からも呼ばれるので、出力先が読む値はロックで守る。

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QtGlobal>

#include "bundle/bundle_files.h"
#include "config/allocation.h"
#include "config/settings.h"
#include "decode/ffmpeg_link_versions.h"
#include "diag/diag_log.h"
#include "dock/dock_bind.h"
#include "dock/dock_poll.h"
#include "dock/dock_widget.h"
#include "dock/ui/focus_ring.h"
#include "install/uninstall.h"
#include "ledger/text_ids.h"
#include "pipeline/pipeline.h"
#include "platform/os_probe.h"
#include "platform/platform.h"
#include "sources/composite_source.h"
#include "sources/self_frame_source.h"
#include "sources/self_speaking_source.h"
#include "sources/sources.h"
#include "ws/listener.h"
#include "ws/log_sink.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("cordmux", "en-US")

// OBS がプラグインの名前と説明として読む (OBS 31.1.1 の libobs/obs-module.c が、この 2 つの関数を名前で探す)。名前は
// 製品の名前なので、言語によって変えない。説明は ini の cordmux.plugin.description.text である。宣言は libobs の
// obs-module.h にあり、extern "C" でエクスポートされる。
const char *obs_module_name(void)
{
	return "cordmux";
}

const char *obs_module_description(void)
{
	return obs_module_text(cordmux::ledger::text::keys::kPluginDescriptionText);
}

namespace {

// cordmux.dll のバージョン (<version>+<短いコミット>。作り方は ws/ws.cmake)。送信側に hello_ok で伝える
// ListenerConfig::server_version、ドックに表示する DockPollConfig::plugin_version、録画ログに書く
// Pipeline::set_plugin_version に、この 1 つの値を渡す (別々に決めると、表示と送信側に伝える値が食い違う)。
// CORDMUX_VERSION_STRING が定義されていないビルドでは空にする (推測したバージョンを埋め込まない)。空のバージョンは、
// 画面では "-" と表示する (ledger/observation.h の Versions)。
#if defined(CORDMUX_VERSION_STRING)
constexpr const char *kPluginVersion = CORDMUX_VERSION_STRING;
#else
constexpr const char *kPluginVersion = "";
#endif

// obs_module_load で作り、obs_module_unload で、作ったのと逆の順に破棄する (Pipeline が Listener と
// AllocationTable を参照で使うため)。
std::unique_ptr<cordmux::config::AllocationTable> g_table;
std::unique_ptr<cordmux::ws::Listener> g_listener;
std::unique_ptr<cordmux::pipeline::Pipeline> g_pipeline;

// ドックのウィジェットの所有者は OBS である。OBS のメインウィンドウと一緒に、obs_module_unload より前に破棄されるので、
// QPointer で持ち、obs_module_unload では触らない (dock/dock_bind.h の add_dock)。
QPointer<cordmux::dock::DockWidget> g_dock_widget;
std::unique_ptr<cordmux::platform::OsProbe> g_os_probe;
std::unique_ptr<cordmux::dock::DockPoll> g_dock_poll;

// ログの行をチェックするときに使うトークンとポート番号。ログの出力先は Listener の io_thread からも呼ばれ、値を入れる
// のは obs_module_load (UI のスレッド) なので、このロックで守る。
std::mutex g_session_guard_mutex;
std::string g_secret_hex;
std::uint16_t g_port = 0;

// ログの行を積むリングバッファー (診断情報の plugin.log) を持つ Pipeline。ログの出力先は io_thread からも呼ばれるので、
// g_pipeline を直接読まずに、g_session_guard_mutex で守ったこのポインタを読む。Pipeline が生きている間だけ値を入れる。
cordmux::pipeline::Pipeline *g_pipeline_for_log = nullptr;

// ログの行にトークンかポート番号が含まれていれば捨て、そうでなければ OBS のログに書く。cordmux.dll が書くログの行は、
// すべてこの関数を通る。
//   - cordmux.dll 自身の行 (cordmux_diag_log_sink。diag/diag_log.h から)
//   - ws/ の行 (cordmux_log_sink。ws::ListenerConfig::log から)
//   - 送信側の行 (cordmux_js_log_sink。Pipeline::set_js_log_sink から)
// チェックを 1 か所にまとめるのは、経路ごとに判定が食い違わないようにするためである。prefix は経路を区別する固定の
// 文字列で、トークンもポート番号も含まないので、チェックしない。
// to_plugin_ring が true なら、チェックを通った行を、cordmux.dll のログのリングバッファーにも積む (g_pipeline_for_log
// がある間だけ)。kind が統計の行なら plugin-stats.log の、ほかは plugin.log のリングバッファーに入る。
// to_obs_log が false なら、OBS のログには書かない (送信側の統計の行。チェックはする)。
// blog() を直接呼ぶのは、この関数の中だけにする。ほかの所から呼ぶと、その行はチェックを通らず、plugin.log にも
// 入らない。obs-plugintemplate の plugin-support (obs_log) も、同じ理由で使わない。
bool guard_and_log(int level, std::string_view line, const char *prefix, bool to_plugin_ring,
		   cordmux::diag::LineKind kind = cordmux::diag::LineKind::Event, bool to_obs_log = true)
{
	std::string secret_hex_copy;
	std::uint16_t port_copy = 0;
	cordmux::pipeline::Pipeline *pipeline_for_log = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_session_guard_mutex);
		secret_hex_copy = g_secret_hex;
		port_copy = g_port;
		pipeline_for_log = g_pipeline_for_log;
	}
	if (cordmux::ws::text_reveals_secret_or_port(line, secret_hex_copy, port_copy)) {
		blog(LOG_WARNING, "[cordmux] Dropped a log line because it contained connection details");
		return false;
	}
	if (to_obs_log)
		blog(level, "%s%.*s", prefix, int(line.size()), line.data());
	if (to_plugin_ring && pipeline_for_log != nullptr)
		pipeline_for_log->push_plugin_log(line, kind);
	return true;
}

// diag/diag_log.h の出力先。チェックを通った行は plugin.log にも積む。行の先頭の「[cordmux] 」と、obs_module_unload が
// 書く「Module unloaded」は、test/tools/notice-count/notice_count.py がログを読むときの目印である (--selftest が、この
// ファイルに同じ文字列があることを確認する)。
void cordmux_diag_log_sink(int level, cordmux::diag::LineKind kind, std::string_view line)
{
	guard_and_log(level, line, "[cordmux] ", true, kind);
}

void cordmux_log_sink(std::string_view line)
{
	// ws/ の行も cordmux.dll 自身の行なので、plugin.log にも積む。ws::LogSink はログレベルを持たないので、LOG_INFO で
	// 書く。
	guard_and_log(LOG_INFO, line, "[cordmux] ", true);
}

// 送信側のログの行の出力先 (Pipeline::set_js_log_sink に渡す)。チェックは guard_and_log を共有し、先頭の文字列だけを
// 変える。戻り値は、チェックを通ったか (Pipeline がリングバッファーに入れてよいか) である。
bool cordmux_js_log_sink(std::string_view line, cordmux::pipeline::Pipeline::JsLineKind kind)
{
	// 送信側の行は plugin.log に積まない。Pipeline が別のリングバッファー (js_log と js_stats_log) に持ち、診断情報では
	// discord.log と discord-stats.log として plugin.log と分けて入る。
	//
	// OBS のログへの書き方は、行の種類で決める (docs/wire_format.md の「メッセージの種類」の log の扱い)。
	//   - 出来事の行: LOG_DEBUG で書く。OBS は LOG_DEBUG の行を、--verbose で起動したときだけログに書く (OBS 31.1.1 の
	//     frontend/obs-main.cpp の do_log)。実機テストの実行スクリプトは OBS を --verbose で起動する
	//     (test/tools/obs-run/run-window-plan.mjs の OBS_LAUNCH_ARGS) ので、送信側の行と受信側の行を 1 つのログで時刻の
	//     順に読める。
	//   - 異常に関わる出来事の行 (alerts): LOG_WARNING で書く。--verbose なしの利用者のログにも残し、映像が止まったときの
	//     原因を、診断情報がなくても追えるようにする。送信側は、この行に Discord のユーザー ID を書かない
	//     (src-vencord/index.ts の logAlert)。
	//   - 統計の行: OBS のログには書かない。数秒ごとに何行も出るので、--verbose のログを読みにくくする。診断情報の
	//     discord-stats.log にだけ残る。
	using Kind = cordmux::pipeline::Pipeline::JsLineKind;
	if (kind == Kind::Alert)
		return guard_and_log(LOG_WARNING, line, "[cordmux/js] ", false);
	if (kind == Kind::Stats)
		return guard_and_log(LOG_DEBUG, line, "[cordmux/js] ", false, cordmux::diag::LineKind::Stats, false);
	return guard_and_log(LOG_DEBUG, line, "[cordmux/js] ", false);
}

// cordmux の設定フォルダー (obs_module_config_path) の中の settings.json。ドックが書き込むファイルと同じである。
constexpr const char *kSettingsFileName = "settings.json";

// settings.json を読み、ないか、読めたか、あるのに読めないかを返す。
//
// platform::read_file_bytes で読む。std::ifstream は UTF-8 のパスをアクティブコードページの文字列として扱うので、
// ASCII でない文字を含むフォルダーのファイルを開けない。開けないと「ない」と同じに扱われ、ドックが次に設定を
// 保存したときに、すべてのキーを既定値で上書きする。read_file_bytes は「ない」と「開けない」を同じ false で返すので、
// 読めなかったときは、フォルダーの一覧にその名前があるかで分ける (pipeline::classify_settings_file)。大きさの上限は
// state.json と同じ kDefaultMaxFileBytes (platform/os_probe.h) で、それより大きいファイルは「あるのに読めない」になり、
// ドックは settings.json に書き込まない。
cordmux::pipeline::SettingsFileState read_settings_json(std::string &out)
{
	const std::string path = cordmux::dock::module_config_file_path(kSettingsFileName);
	if (path.empty())
		return cordmux::pipeline::SettingsFileState::Absent;
	const bool read_ok = cordmux::platform::read_file_bytes(path, out, cordmux::platform::kDefaultMaxFileBytes);
	bool folder_listed = false;
	bool entry_listed = false;
	if (!read_ok) {
		std::vector<cordmux::platform::DirEntry> entries;
		folder_listed = cordmux::platform::list_directory(cordmux::dock::module_config_dir(), entries);
		// 名前は英字の大文字と小文字を区別せずに比べる。ファイルシステムによっては、大文字と小文字だけが違う
		// 名前で同じファイルを開くためである。
		const auto same_name = [](const std::string &a, const char *b) {
			const std::string_view bv(b);
			if (a.size() != bv.size())
				return false;
			for (std::size_t i = 0; i < a.size(); ++i) {
				const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
				if (lower(a[i]) != lower(bv[i]))
					return false;
			}
			return true;
		};
		for (const cordmux::platform::DirEntry &entry : entries) {
			if (same_name(entry.name, kSettingsFileName))
				entry_listed = true;
		}
	}
	return cordmux::pipeline::classify_settings_file(read_ok, out, folder_listed, entry_listed);
}

// 下の 3 つの関数は、配布用の zip に入っているファイルとフォルダー (cordmux/cordmux.dll、cordmux/data/vencord、
// cordmux/data/vencord-src) の、インストールした後の場所を求める。DLL とデータのフォルダーは libobs が知っているので、
// このファイルに Windows の API を書かずに求められる。
std::string join_dir(const std::string &dir, const char *name)
{
	if (dir.empty())
		return {};
	std::string out = dir;
	if (out.back() != '/' && out.back() != '\\')
		out += '/';
	out += name;
	return out;
}

// cordmux/data のフォルダーを、絶対パスで返す。ポータブル版の OBS では、libobs が OBS の作業フォルダー (bin\64bit) からの
// 相対パス ("../../plugins/cordmux/data") を返す。
// そのまま state.json の vencordDir に書くと、Vesktop は自分の作業フォルダーからの相対パスとして読むので、フォルダーが
// 見つからない。相対パスなら、libobs と同じ作業フォルダーを基準にして絶対パスにする (platform::absolute_path)。この相対パスは
// 作業フォルダーからのものなので、DLL のフォルダーを基準にはしない。
std::string module_data_dir()
{
	const char *path = obs_get_module_data_path(obs_current_module());
	if (path == nullptr)
		return {};
	const std::string raw(path);
	if (std::filesystem::u8path(raw).is_absolute())
		return raw;
	return std::filesystem::u8path(cordmux::platform::absolute_path(raw)).lexically_normal().generic_u8string();
}

// cordmux.dll のフォルダー (バックアップの dll_dir に保存する)。
std::string module_binary_dir()
{
	// まず platform::module_dir() (GetModuleFileNameW を使うので、常に絶対パス) を使う。libobs の
	// obs_get_module_binary_path は、ポータブル版の OBS では相対パス ("../../plugins/cordmux/cordmux.dll") を返す。
	// module_dir() が空を返したときだけ、libobs の値を絶対パスにして使う。
	const std::string absolute = cordmux::platform::module_dir();
	if (!absolute.empty())
		return absolute;
	const char *path = obs_get_module_binary_path(obs_current_module());
	if (path == nullptr)
		return {};
	const std::string full = cordmux::platform::absolute_path(path);
	const std::string::size_type cut = full.find_last_of("/\\");
	return cut == std::string::npos ? std::string() : full.substr(0, cut);
}

// OBS の [ツール] メニューの「cordmuxの設定」(cordmux.settings.tools_menu.menu) から呼ばれる。g_dock_poll がなければ
// 何もしない。メニューの項目は、ドックを追加できたときだけ obs_module_load が追加する (OBS の frontend API には、項目を
// 削除する関数がないため)。
void open_settings_from_tools_menu(void * /*private_data*/)
{
	if (g_dock_poll)
		cordmux::dock::DockPollSettingsPart::open_settings(*g_dock_poll, cordmux::ledger::SettingsPage::Recording,
								   false);
}

// 合成のソースのプロパティの [cordmuxの設定を開く] から呼ばれる (sources::set_composite_open_settings)。[ツール] メニューの
// 項目と同じく [録画] のページで開く。
void open_settings_from_source()
{
	open_settings_from_tools_menu(nullptr);
}

// OBS の [ツール] メニューの「cordmuxをアンインストール...」(cordmux.settings.tools_menu_uninstall.menu) から呼ばれる。
// 押しただけでは何も削除しない。設定ウィンドウを「バージョン情報」のページで開き、そのページのボタンを押したときと
// 同じ処理をする (DockPollSettingsPart::open_settings_to_uninstall)。メニューから確認のダイアログを直接開かないのは、
// アンインストールできない状態のときに理由を表示する場所がなく、判定もページのボタンと 2 か所に分かれるためである。
// ドックのメニューは配信中に操作するので、この項目を置かない。
void open_uninstall_from_tools_menu(void * /*private_data*/)
{
	if (g_dock_poll)
		cordmux::dock::DockPollSettingsPart::open_settings_to_uninstall(*g_dock_poll);
}

// ---- 全画面のホットキー ----
//
// OBS の [設定] の [ホットキー] に並ぶ 5 つ (4 等分の位置ごとの「全画面で表示」と「全画面を元に戻す」)。登録はドックを
// 追加できたときだけ行う (押した後の処理は DockPoll が持つ)。
// 割り当てたキーは OBS が保存する。OBS は [設定] で変えたフロントエンドのホットキーを、プラグインのものも含めて、
// プロファイルの設定 (basic.ini の [Hotkeys]) に登録した名前で書く (OBS 31.1.1 の frontend/settings/OBSBasicSettings.cpp の
// SaveHotkeySettings)。読み込むのは OBS 自身のホットキーだけなので (frontend/widgets/OBSBasic_Hotkeys.cpp の
// CreateHotkeys)、cordmux のものは、登録した直後とプロファイルを切り替えた後に cordmux が読む。OBS のほかのホットキーと
// 同じく、プロファイルごとに割り当てが分かれる。

// 元に戻すホットキーの位置の値。
constexpr int kFocusRestoreCell = -1;

struct FocusHotkey {
	const char *name;      // プロファイルの設定に書かれる名前。変えると、割り当てたキーを読めなくなる
	const char *label_key; // [ホットキー] に表示する名前の ini のキー
	int cell;              // 4 等分の位置 (0 左上、1 右上、2 左下、3 右下)。kFocusRestoreCell は元に戻す
	obs_hotkey_id id;
};

FocusHotkey g_focus_hotkeys[] = {
	{"cordmux.full_screen_top_left", cordmux::ledger::text::keys::kPluginHotkeyFullScreenTopLeftLabel, 0,
	 OBS_INVALID_HOTKEY_ID},
	{"cordmux.full_screen_top_right", cordmux::ledger::text::keys::kPluginHotkeyFullScreenTopRightLabel, 1,
	 OBS_INVALID_HOTKEY_ID},
	{"cordmux.full_screen_bottom_left", cordmux::ledger::text::keys::kPluginHotkeyFullScreenBottomLeftLabel, 2,
	 OBS_INVALID_HOTKEY_ID},
	{"cordmux.full_screen_bottom_right", cordmux::ledger::text::keys::kPluginHotkeyFullScreenBottomRightLabel, 3,
	 OBS_INVALID_HOTKEY_ID},
	{"cordmux.full_screen_restore", cordmux::ledger::text::keys::kPluginHotkeyFullScreenRestoreLabel,
	 kFocusRestoreCell, OBS_INVALID_HOTKEY_ID},
};

// ホットキーを押したときに、UI のスレッドへ処理を渡す宛先。登録の前に作り、登録を外した後に破棄する (破棄すると、まだ
// 実行されていない呼び出しを Qt が捨てる)。ホットキーのコールバックがどのスレッドから来てもよいように、ロックで守る。
std::mutex g_focus_hotkey_mu;
QObject *g_focus_hotkey_context = nullptr;

// ホットキーのコールバック。OBS のホットキーのスレッドから来ることがあるので、ここでは DockPoll に触らず、UI のスレッドへ
// 渡すだけにする (ドックの状態と ActionId は UI のスレッドで扱う)。押したときだけ動き、離したときは何もしない。
void on_focus_hotkey(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	if (!pressed)
		return;
	const int cell = static_cast<int>(reinterpret_cast<std::intptr_t>(data));
	std::lock_guard<std::mutex> lock(g_focus_hotkey_mu);
	if (g_focus_hotkey_context == nullptr)
		return;
	QMetaObject::invokeMethod(
		g_focus_hotkey_context,
		[cell] {
			if (!g_dock_poll)
				return;
			if (cell == kFocusRestoreCell)
				g_dock_poll->press_focus_restore_hotkey();
			else
				g_dock_poll->press_focus_hotkey(cell);
		},
		Qt::QueuedConnection);
}

// プロファイルの設定から、割り当てたキーを読み込む。プロファイルに割り当てがなければ、割り当てのない状態にする
// (切り替える前のプロファイルのキーを残さない)。
void load_focus_hotkey_bindings()
{
	config_t *config = obs_frontend_get_profile_config();
	for (const FocusHotkey &hotkey : g_focus_hotkeys) {
		if (hotkey.id == OBS_INVALID_HOTKEY_ID)
			continue;
		const char *json = config != nullptr ? config_get_string(config, "Hotkeys", hotkey.name) : nullptr;
		obs_data_t *data = json != nullptr ? obs_data_create_from_json(json) : nullptr;
		obs_data_array_t *bindings = data != nullptr ? obs_data_get_array(data, "bindings") : nullptr;
		obs_hotkey_load(hotkey.id, bindings);
		obs_data_array_release(bindings);
		obs_data_release(data);
	}
}

void register_focus_hotkeys()
{
	{
		std::lock_guard<std::mutex> lock(g_focus_hotkey_mu);
		if (g_focus_hotkey_context == nullptr)
			g_focus_hotkey_context = new QObject();
	}
	for (FocusHotkey &hotkey : g_focus_hotkeys) {
		if (hotkey.id != OBS_INVALID_HOTKEY_ID)
			continue;
		hotkey.id = obs_hotkey_register_frontend(hotkey.name, obs_module_text(hotkey.label_key), on_focus_hotkey,
							 reinterpret_cast<void *>(static_cast<std::intptr_t>(hotkey.cell)));
	}
	load_focus_hotkey_bindings();
}

// 登録を外す。外した後は、コールバックが呼ばれない (libobs がホットキーのロックの下で外す)。その後に宛先を破棄する。
void unregister_focus_hotkeys()
{
	for (FocusHotkey &hotkey : g_focus_hotkeys) {
		if (hotkey.id == OBS_INVALID_HOTKEY_ID)
			continue;
		obs_hotkey_unregister(hotkey.id);
		hotkey.id = OBS_INVALID_HOTKEY_ID;
	}
	QObject *context = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_focus_hotkey_mu);
		context = g_focus_hotkey_context;
		g_focus_hotkey_context = nullptr;
	}
	delete context;
}

void on_frontend_event(enum obs_frontend_event event, void * /*private_data*/)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		// 割り当てたキーはプロファイルごとにあるので、切り替えた後のプロファイルから読み直す。
		load_focus_hotkey_bindings();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// 保留していた configure を適用する (Pipeline::on_frontend_finished_loading)。
		if (g_pipeline)
			g_pipeline->on_frontend_finished_loading();
		// 初めての起動のときだけ、ドックをフローティングで表示する。
		cordmux::dock::show_dock_once_after_loading(g_dock_widget.data());
		if (g_dock_poll)
			g_dock_poll->on_frontend_finished_loading();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// ドックのウィジェットは、この後に OBS のメインウィンドウと一緒に破棄される (obs_module_unload より前)。
		// ウィジェットがまだあるこの時点で、ポーリングを止めてコールバックを外す。
		if (g_dock_poll)
			g_dock_poll->stop();
		// OBS はこの直前にシーンコレクションをクリーンアップしているので、Pipeline も合成のソースの有無を調べるのを
		// やめる (Pipeline::on_frontend_exit)。
		if (g_pipeline)
			g_pipeline->on_frontend_exit();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		// シーンコレクションのクリーンアップの途中で発生する (切り替えのときも、終了のときも)。
		// このイベントは、すべてのソースをシーンから外した直後、破棄が終わるのを待つ前に発生する (OBS 31.1.1 の
		// frontend/widgets/OBSBasic_SceneCollections.cpp の ClearSceneData)。OBS は待った後に、破棄されずに残った
		// ソースをエラーとしてログに書き、それ以後は設定を保存しなくなる。OBS_FRONTEND_EVENT_EXIT はその確認より
		// 後に発生するので、ソースへの参照を解放するのはこのイベントでなければならない。
		// 自分の枠の音声ソースに付けたコールバックと、自分の枠に映すソースへの結び付けを外す。ソースはこの後に破棄される。
		// 切り替えた後のシーンコレクションのソースは、ドックの周期が探し直して付ける。
		cordmux::sources::release_self_speaking();
		cordmux::sources::release_self_frame();
		// 調べてあった合成のソースの有無を、現在の値ではないことにする (Pipeline::on_scene_collection_cleanup)。
		if (g_pipeline)
			g_pipeline->on_scene_collection_cleanup();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		if (g_pipeline)
			g_pipeline->on_scene_collection_changing();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (g_pipeline)
			g_pipeline->on_scene_collection_changed();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		// 配信が実際に始まったとき (始められなかったときは来ない)。[配信時に自動的に録画する] がオンなら録画を始める。
		if (g_dock_poll)
			g_dock_poll->on_streaming_event(cordmux::ledger::StreamEvent::Started);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		if (g_dock_poll)
			g_dock_poll->on_streaming_event(cordmux::ledger::StreamEvent::Stopped);
		break;
	case OBS_FRONTEND_EVENT_THEME_CHANGED:
		// OBS のテーマ、[フォントサイズ]、[密度] を変えたときに発生する。
		if (g_dock_poll)
			g_dock_poll->on_theme_changed();
		break;
	default:
		break;
	}
}

// 待ち受けを閉じた後 (アンインストールの最初の手順。stop_listening)、待ち受けを開き直さないためのフラグ。UI の
// スレッドだけが読み書きする。
bool g_listener_closed_for_good = false;

// Listener::try_open() の結果を、チェックに使う値とログに反映する。obs_module_load と maintain_listener が呼ぶ。
void report_open_result(const cordmux::ws::OpenResult &open_result)
{
	switch (open_result.outcome) {
	case cordmux::ws::OpenOutcome::Opened:
		// トークンとポート番号は try_open() が成功してから決まるので、ここでチェックに使う値を入れる (io_thread の
		// ログの出力先が読むので、ロックの中で書く)。
		{
			std::lock_guard<std::mutex> lock(g_session_guard_mutex);
			g_secret_hex = open_result.secret_hex;
			g_port = open_result.port;
		}
		// トークンファイル (session.json) を保存できなかったときは、待ち受けは開いているが、Vesktop は接続できない。
		// ドックではメッセージのレベル 2 になるので、ログも LOG_WARNING で書く。ポート番号もファイルの場所も書かない。
		if (open_result.problem == cordmux::ws::OpenProblem::SessionFileNotSaved)
			cordmux::diag::logf(LOG_WARNING,
					    "Vesktop can't connect: couldn't save the connection details file");
		else
			cordmux::diag::logf(LOG_INFO, "Waiting for Vesktop to connect");
		break;
	case cordmux::ws::OpenOutcome::BlockedByOtherWriter:
		// ほかの OBS がもう cordmux で待ち受けている。プラグインの読み込みは失敗させず、待ち受けなしで続ける。
		cordmux::diag::logf(LOG_WARNING, "Not waiting for Vesktop: another OBS is already running cordmux");
		break;
	case cordmux::ws::OpenOutcome::PortsExhausted:
		// ポートを開けたが接続の受け付けを始められなかったときも、outcome はこの値になる。原因に合わない文を書かない
		// ように、problem で分ける。
		if (open_result.problem == cordmux::ws::OpenProblem::AcceptFailed)
			cordmux::diag::logf(LOG_WARNING, "Not waiting for Vesktop: couldn't start accepting connections");
		else
			cordmux::diag::logf(LOG_WARNING, "Not waiting for Vesktop: none of the ports could be opened");
		break;
	case cordmux::ws::OpenOutcome::AlreadyOpenedBefore:
		// can_retry_open() が true のときだけ try_open() を呼び直すので、起きない。
		cordmux::diag::logf(LOG_WARNING,
				    "Not waiting for Vesktop: the connection was already opened once (unexpected)");
		break;
	}
}

// DockPoll が 5 秒ごとに UI のスレッドから呼ぶ (dock/dock_poll_config.h の maintain_listener)。
//  - 待ち受けていない (別の OBS が動いていたか、ポートが空いていなかった) ときは、try_open() を呼び直す。別の
//    OBS が終了すれば、この OBS が受け取れるようになる。cordmux を無効にしている間も待ち受けは開いたままなので、
//    無効かどうかでは分けない。
//  - 待ち受けていて Vesktop が接続していないときは、session.json を確認する。
// アンインストールの最中と後は何もしない。待ち受けを開くと、削除したファイルが作り直される。
void maintain_listener()
{
	if (!g_listener || g_listener_closed_for_good || cordmux::install::is_uninstalling())
		return;

	if (g_listener->can_retry_open()) {
		const cordmux::ws::OpenOutcome before = g_listener->stats().open.outcome;
		const cordmux::ws::OpenResult open_result = g_listener->try_open();
		// 失敗が前と同じ理由なら、5 秒ごとに同じ行を書かない。
		if (open_result.outcome == cordmux::ws::OpenOutcome::Opened || open_result.outcome != before)
			report_open_result(open_result);
		return;
	}

	switch (g_listener->check_session_file()) {
	case cordmux::ws::SessionCheck::Rewritten:
		cordmux::diag::logf(LOG_INFO, "Rewrote the connection details file");
		break;
	case cordmux::ws::SessionCheck::WriteFailed:
		cordmux::diag::logf(LOG_WARNING, "Vesktop can't connect: couldn't save the connection details file");
		break;
	default:
		break;
	}
}

// 起動時の vencordDir の書き直しの結果を、ログに 1 行書く。書けなかったとき (BackupWriteFailed と StateWriteFailed)
// だけ LOG_WARNING にする。obs_module_load と、Vesktop の終了を待ってからもう一度試すとき (ドックが呼ぶ) の両方が使う。
void log_startup_rewrite(const cordmux::pipeline::StartupRewriteResult &rewrite)
{
	using cordmux::pipeline::StartupRewriteOutcome;
	const bool warn = rewrite.outcome == StartupRewriteOutcome::BackupWriteFailed ||
			  rewrite.outcome == StartupRewriteOutcome::StateWriteFailed;
	if (rewrite.outcome == StartupRewriteOutcome::Refused) {
		cordmux::diag::logf(LOG_INFO, "Startup check of Vesktop's Vencord location: not changed (%s)",
				    cordmux::pipeline::refuse_reason_name(rewrite.refuse_reason));
	} else if (rewrite.outcome == StartupRewriteOutcome::StateWriteFailed) {
		cordmux::diag::logf(LOG_WARNING,
				    "Startup check of Vesktop's Vencord location: Vesktop's settings file "
				    "couldn't be written (backup %s)",
				    rewrite.backup_restored ? "restored" : "not restored");
	} else {
		cordmux::diag::logf(warn ? LOG_WARNING : LOG_INFO, "Startup check of Vesktop's Vencord location: %s",
				    cordmux::pipeline::startup_rewrite_outcome_name(rewrite.outcome));
	}
}

} // namespace

bool obs_module_load(void)
{
	// ログの出力先を最初に登録する。登録する前に書いた行は捨てられる (diag/diag_log.h の set_log_sink)。
	cordmux::diag::set_log_sink(&cordmux_diag_log_sink);

	// 最初の行に cordmux.dll のバージョンを書き、どのビルドのログかを見分けられるようにする。CORDMUX_VERSION_STRING の
	// ないビルドでは unknown と書く。行の先頭の「[cordmux] 」は出力先が付ける。
	cordmux::diag::logf(LOG_INFO, "Version %s loaded", kPluginVersion[0] != '\0' ? kPluginVersion : "unknown");
	cordmux::diag::logf(LOG_INFO, "OBS %s, Qt %s (built with Qt %s)", obs_get_version_string(), qVersion(),
			    QT_VERSION_STR);

	// リンクした FFmpeg のメジャーバージョンをログに書く。ほかの場所にある同じ名前の DLL が読み込まれたとき
	// (バージョンが合わないとき) を、この行で見分ける。cordmux.dll は FFmpeg のインポートライブラリをリンクしているので、
	// DLL が見つからなければ、OBS が cordmux.dll を読み込む時点で失敗する。呼び出すこと自体の意味 (swresample を直接の
	// 依存にする) は decode/ffmpeg_link_versions.h にある。
	{
		const cordmux::decode::FfmpegLinkVersions ffmpeg = cordmux::decode::ffmpeg_link_versions();
		cordmux::diag::logf(LOG_INFO, "FFmpeg avcodec=%u avutil=%u swresample=%u avformat=%u",
				    ffmpeg.avcodec_major, ffmpeg.avutil_major, ffmpeg.swresample_major,
				    ffmpeg.avformat_major);
	}

	if (!cordmux::platform::startup()) {
		// WinSock を起動できなくても、プラグインの読み込みは失敗させず、ログに書くだけにする。
		cordmux::diag::logf(LOG_WARNING, "Couldn't start Windows networking (WinSock)");
	}

	cordmux::sources::register_source_types();

	// settings.json を読む。なければ既定値を使う。settings.json に書き込むのはドックだけである。
	cordmux::config::Settings settings;
	// ファイルはあるのに読めなかったときだけ true にする (初めての起動では、ファイルがないのが普通である)。開けない
	// ときと、中身の形が違うときの両方が入る。true の間、ドックは settings.json に書き込まない (既定値で利用者の値を
	// 上書きしないため。dock/dock_poll_config.h の settings_file_unreadable)。
	bool settings_file_unreadable = false;
	std::string settings_json;
	switch (read_settings_json(settings_json)) {
	case cordmux::pipeline::SettingsFileState::Absent:
		break;
	case cordmux::pipeline::SettingsFileState::Unreadable:
		settings_file_unreadable = true;
		cordmux::diag::logf(LOG_WARNING, "Couldn't read settings.json; using default settings and not saving "
				    "changes this session");
		break;
	case cordmux::pipeline::SettingsFileState::Read: {
		std::string parse_error;
		if (!cordmux::config::parse_settings(settings_json, &settings, &parse_error)) {
			settings_file_unreadable = true;
			cordmux::diag::logf(LOG_WARNING,
					    "Couldn't read settings.json; using default settings and not saving "
					    "changes this session");
			// 読めなかったキーと理由は LOG_DEBUG で書く。理由の文は日本語 (config/settings.h の parse_settings) なので、
			// OBS のログには --verbose で起動したときだけ書かれる。
			cordmux::diag::logf(LOG_DEBUG, "Read error in settings.json: %s", parse_error.c_str());
		}
		break;
	}
	}
	// settings.json があるのに読めなかったときは、Vesktop の設定 (state.json) を書かない
	// (pipeline/pipeline.h の effective_allow_state_json_write)。起動時の書き直し、ドック、アンインストールが同じ値を
	// 読むように、settings の値そのものを false にする。この起動ではドックが settings.json を書かないので、false が
	// ファイルに保存されることはない。
	{
		const bool allow = cordmux::pipeline::effective_allow_state_json_write(settings.allow_state_json_write,
										     settings_file_unreadable);
		if (allow != settings.allow_state_json_write) {
			settings.allow_state_json_write = allow;
			cordmux::diag::logf(LOG_INFO,
					    "Vesktop's settings aren't changed this session because settings.json "
					    "couldn't be read");
		}
	}
	// 動作を変える設定の値をログに書く。どの値で動いていたかを、ログから確認できるようにするため (A/V の補正の
	// 2 つの値は、測って決め直すときに、そのときに使っていた値が必要になる)。
	cordmux::diag::logf(LOG_INFO, "Settings: decoder=%s",
			    settings.decoder == cordmux::config::DecoderChoice::D3D11VA ? "hardware" : "software");
	cordmux::diag::logf(LOG_INFO, "Settings: screen_audio_offset_ms=%d voice_audio_delay_ms=%d",
			    settings.screen_audio_offset_ms, settings.voice_audio_delay_ms);
	cordmux::diag::logf(LOG_INFO,
			    "Settings: feature_enabled=%d allow_state_json_write=%d limit_max_long_edge=%d "
			    "limit_fps_divisor=%d vencord_check_disconnected_s=%d vencord_check_connected_s=%d",
			    settings.feature_enabled ? 1 : 0, settings.allow_state_json_write ? 1 : 0,
			    settings.limit_max_long_edge, settings.limit_fps_divisor, settings.vencord_check_disconnected_s,
			    settings.vencord_check_connected_s);

	// 同梱の Vencord のフォルダー (data/vencord と data/vencord-src) と、バックアップのファイルの場所。ここで 1 回だけ
	// 作り、起動時の書き直し、OsProbe、ドックが同じ値を使う。
	const std::string data_dir = module_data_dir();
	const std::string bundled_vencord_dir = join_dir(data_dir, "vencord");
	const std::string bundled_vencord_src_dir = join_dir(data_dir, "vencord-src");
	const std::string backup_path = cordmux::dock::module_config_file_path("vencord_dir_backup.json");
	const std::string dll_dir = module_binary_dir();

	// ---- 起動時の vencordDir の書き直し ----
	// OsProbe を起動する前に、このスレッドで 1 回だけ行う。後で行うと、OsProbe が書き直す前の state.json を読み、
	// Vesktop がほかの Vencord を使っているというメッセージを、ドックが一瞬だけ表示することになる。state.json は小さく、
	// 大きさの上限を付けて読むので、UI のスレッドを長く止めない。ドックを追加できなかった起動でも行う。書くかどうかは
	// pipeline::run_startup_vencord_dir_rewrite が決める。
	// Vesktop が起動している間は書かない (起動中の Vesktop は state.json をメモリの値で書き直すので、書いた値が消える。
	// ledger/actions.cpp の vesktop_busy_reason)。起動していた (か、調べられなかった) ときは、ドックが、OsProbe の結果で
	// Vesktop が起動していないと分かったときに、同じ入力でもう一度試す (DockPollConfig::retry_startup_rewrite)。
	// ドックを追加できなかった起動では、もう一度試す所がないので、次の起動まで書き直さない (ログに書いてある)。
	bool rewrote_vencord_dir_at_startup = false;
	std::function<cordmux::dock::StartupRewriteRetry(bool)> retry_startup_rewrite;
	{
		cordmux::pipeline::StartupRewriteInputs rewrite_in;
		rewrite_in.uninstalling = cordmux::install::is_uninstalling();
		rewrite_in.feature_enabled = settings.feature_enabled;
		rewrite_in.allow_state_json_write = settings.allow_state_json_write;
		rewrite_in.target_dir = bundled_vencord_dir;
		rewrite_in.backup_json_path = backup_path;
		rewrite_in.dll_dir = dll_dir;
		rewrite_in.max_json_bytes = cordmux::platform::kDefaultMaxFileBytes;
		rewrite_in.now_us = cordmux::platform::now_wall_us();

		cordmux::pipeline::StartupRewriteOps rewrite_ops;
		rewrite_ops.vesktop_state_json_path = [](std::string &out) {
			return cordmux::platform::vesktop_state_json_path(out);
		};
		rewrite_ops.read_file_bytes = [](const std::string &path, std::string &out, std::size_t max_bytes) {
			return cordmux::platform::read_file_bytes(path, out, max_bytes);
		};
		rewrite_ops.write_file_atomic = [](const std::string &path, const std::string &text) {
			return cordmux::platform::write_file_atomic(path, text);
		};
		rewrite_ops.vesktop_running = []() { return cordmux::platform::vesktop_running_now(); };

		const cordmux::pipeline::StartupRewriteResult rewrite =
			cordmux::pipeline::run_startup_vencord_dir_rewrite(rewrite_in, rewrite_ops);
		rewrote_vencord_dir_at_startup = rewrite.outcome == cordmux::pipeline::StartupRewriteOutcome::Wrote;
		// 結果はどの場合もログに 1 行書く。
		log_startup_rewrite(rewrite);

		if (cordmux::pipeline::startup_rewrite_waits_for_vesktop(rewrite.outcome)) {
			// もう一度試すときは、アンインストールを始めたか、cordmux が有効か、現在の時刻を、呼んだ時点の値にする。
			// ほかの入力 (書きたい場所、バックアップの場所、DLL の場所、書いてよいか) は起動したときに決まり、同じ
			// 起動の中では変わらない。ドックは UI のスレッドから呼ぶ。
			retry_startup_rewrite = [rewrite_in, rewrite_ops](bool feature_enabled) {
				cordmux::pipeline::StartupRewriteInputs in = rewrite_in;
				in.uninstalling = cordmux::install::is_uninstalling();
				in.feature_enabled = feature_enabled;
				in.now_us = cordmux::platform::now_wall_us();
				const cordmux::pipeline::StartupRewriteResult again =
					cordmux::pipeline::run_startup_vencord_dir_rewrite(in, rewrite_ops);
				log_startup_rewrite(again);
				if (again.outcome == cordmux::pipeline::StartupRewriteOutcome::Wrote)
					return cordmux::dock::StartupRewriteRetry::Wrote;
				if (cordmux::pipeline::startup_rewrite_waits_for_vesktop(again.outcome))
					return cordmux::dock::StartupRewriteRetry::Waiting;
				return cordmux::dock::StartupRewriteRetry::Done;
			};
		}
	}

	g_table = std::make_unique<cordmux::config::AllocationTable>();
	// 枠の割り当ては保存しない。OBS を起動し直すと空から始まる。

	cordmux::ws::ListenerConfig listener_cfg;
	listener_cfg.log = &cordmux_log_sink; // トークンとポート番号のチェックを通す (guard_and_log)
	listener_cfg.server_version = kPluginVersion; // 送信側に hello_ok で伝えるバージョン
	g_listener = std::make_unique<cordmux::ws::Listener>(listener_cfg, cordmux::ws::make_real_platform_ops());

	g_pipeline = std::make_unique<cordmux::pipeline::Pipeline>(*g_listener, *g_table, settings);
	{
		// これより後に書いたログの行は、診断情報の plugin.log のリングバッファーにも入る。
		std::lock_guard<std::mutex> lock(g_session_guard_mutex);
		g_pipeline_for_log = g_pipeline.get();
	}
	// 送信側のログの行も、同じチェックを通す。start() より前に呼ぶ (Pipeline::set_js_log_sink)。
	g_pipeline->set_js_log_sink(&cordmux_js_log_sink);
	// 録画ログに書くバージョン。送信側に伝えるバージョンと同じ値を渡す。
	g_pipeline->set_plugin_version(kPluginVersion);
	// start() は Listener のコールバック (on_video_frame など) を登録する。コールバックは io_thread が読むので、
	// try_open() で io_thread を動かす前に登録する。start() が Listener に渡す media と limits の初期値は、最初の
	// hello_ok に載る。
	g_pipeline->start();

	// アンインストールの最中なら待ち受けを開かない。フラグはメモリにだけあるので、読み込みの時点では立っていないが、
	// 待ち受けを開く前にはいつも確認する (install/uninstall.h の is_uninstalling)。
	if (cordmux::install::is_uninstalling()) {
		cordmux::diag::logf(LOG_INFO, "Not waiting for Vesktop: cordmux is being uninstalled");
	} else {
		report_open_result(g_listener->try_open());
	}

	// ---- ドック ----
	// OsProbe は、OS への問い合わせ (Vesktop が動いているか、同梱した Vencord のファイルの照合、state.json と
	// バックアップの読み取り) を 1 つのスレッドで行う。UI のスレッドで行うと OBS の画面が止まるので、ドックは結果を
	// 読むだけにする。
	cordmux::platform::OsProbeConfig probe_cfg;
	probe_cfg.bundled_vencord_dir = bundled_vencord_dir;
	probe_cfg.bundled_vencord_src_dir = bundled_vencord_src_dir;
	probe_cfg.backup_json_path = backup_path;
	// 照合の間隔は settings の 2 つのキーから決める (秒からミリ秒への変換は platform/os_probe.h の
	// probe_interval_ms_from_seconds)。
	probe_cfg.disconnected_interval_ms = cordmux::platform::probe_interval_ms_from_seconds(
		settings.vencord_check_disconnected_s, probe_cfg.disconnected_interval_ms);
	probe_cfg.connected_interval_ms = cordmux::platform::probe_interval_ms_from_seconds(
		settings.vencord_check_connected_s, probe_cfg.connected_interval_ms);
	// 録画していない間も空き容量を表示するために、録画の保存先を渡す。保存先を決めるのは Pipeline だけなので、
	// Pipeline::record_base_dir の値を使う。保存先は録画を始めるときと設定を変えたときに決め直すので、調べるたびに、
	// 関数で現在の保存先を取得する。関数の中で g_pipeline を読んでよいのは、obs_module_unload が OsProbe を先に止める
	// ためである。
	probe_cfg.free_space_probe_path = g_pipeline->record_base_dir();
	probe_cfg.free_space_probe_path_fn = []() { return g_pipeline ? g_pipeline->record_base_dir() : std::string(); };
	g_os_probe = std::make_unique<cordmux::platform::OsProbe>(probe_cfg,
								  cordmux::platform::make_real_os_probe_ops());
	g_os_probe->start();

	// ドックのウィジェットを OBS に追加する。追加した後は OBS が所有者になる (dock/dock_bind.h の add_dock)。
	cordmux::dock::DockWidget *dock_widget = new cordmux::dock::DockWidget();
	if (cordmux::dock::add_dock(dock_widget)) {
		g_dock_widget = dock_widget;

		cordmux::dock::DockPollConfig poll_cfg;
		poll_cfg.poll_ms = settings.dock_poll_ms;
		poll_cfg.video_stall_threshold_us = std::int64_t(settings.video_stall_ms) * 1000;
		poll_cfg.allow_state_json_write = settings.allow_state_json_write;
		poll_cfg.plugin_version = kPluginVersion;
		poll_cfg.bundled_vencord_dir = probe_cfg.bundled_vencord_dir;
		poll_cfg.dll_dir = dll_dir;
		poll_cfg.backup_json_path = backup_path;
		// state.json を読む大きさの上限を、OsProbe とそろえる。そろっていないと、その間の大きさのファイルで、画面の表示と
		// 操作の結果が食い違う。
		poll_cfg.max_json_bytes = probe_cfg.max_file_bytes;

		// 設定のコピーと保存先、これまでに見た人の一覧 (seen_people.json)。
		// settings.json に書き込むのはドックだけである。
		poll_cfg.settings = settings;
		poll_cfg.settings_json_path = cordmux::dock::module_config_file_path(kSettingsFileName);
		poll_cfg.settings_file_unreadable = settings_file_unreadable;
		poll_cfg.seen_people_json_path = cordmux::dock::module_config_file_path("seen_people.json");

		// 診断情報をエクスポートするときの、トークンの 2 段階のチェック (bundle/bundle_files.h の RedactFn と RevealFn)。
		// トークンを持つのはこのファイルだけなので、ドックには関数として渡す。
		poll_cfg.redact = [](const std::string &text) {
			std::string secret_hex_copy;
			{
				std::lock_guard<std::mutex> lock(g_session_guard_mutex);
				secret_hex_copy = g_secret_hex;
			}
			return cordmux::bundle::make_redact(secret_hex_copy)(text);
		};
		poll_cfg.reveals_secret = [](std::string_view text) {
			std::string secret_hex_copy;
			{
				std::lock_guard<std::mutex> lock(g_session_guard_mutex);
				secret_hex_copy = g_secret_hex;
			}
			// ポート番号は調べない (理由は bundle/bundle_files.h の RevealFn)。
			return cordmux::ws::text_reveals_secret_or_port(text, secret_hex_copy, 0);
		};

		poll_cfg.bundled_vencord_src_dir = probe_cfg.bundled_vencord_src_dir;
		poll_cfg.rewrote_vencord_dir_at_startup = rewrote_vencord_dir_at_startup;
		poll_cfg.retry_startup_rewrite = retry_startup_rewrite;
		// アンインストールの最初の手順で待ち受けを閉じる関数。Listener::stop() は on_disconnected を呼ばないので、
		// 接続が切れたときと同じ後始末を Pipeline::on_listener_stopped で行う。Listener と Pipeline はドックより長く
		// 生きる (obs_module_unload はドックを先に止めて破棄する) ので、生のポインタで持ってよい。
		poll_cfg.stop_listening = [listener = g_listener.get(), pipeline = g_pipeline.get()]() {
			g_listener_closed_for_good = true; // maintain_listener が、閉じた待ち受けを開き直さないように
			listener->stop();
			pipeline->on_listener_stopped();
		};
		poll_cfg.maintain_listener = &maintain_listener;
		// 動いている間に変えられる設定 (cordmux を有効にするかどうかと、画質の上限) を Pipeline に渡す。どちらの関数も、
		// 値が変わっていなければ何もしないので、毎回両方を呼んでよい。
		poll_cfg.apply_runtime_settings = [pipeline = g_pipeline.get()](const cordmux::config::Settings &s) {
			pipeline->set_feature_enabled(s.feature_enabled);
			pipeline->set_limits(s.limit_max_long_edge, s.limit_fps_divisor, s.encoder_preference,
					     s.encode_priority);
		};

		g_dock_poll = std::make_unique<cordmux::dock::DockPoll>(*g_pipeline, *g_os_probe, dock_widget,
									poll_cfg);
		g_dock_poll->start();
		cordmux::diag::logf(LOG_INFO, "Dock added (refresh every %d ms)", settings.dock_poll_ms);
		// 全画面のホットキー。押した後の処理は DockPoll が持つので、ドックを追加できたときだけ登録する。
		register_focus_hotkeys();

		// 設定ウィンドウを開く入口 ([ツール] メニューの 2 つの項目)。どちらも DockPoll がウィンドウを作るので、
		// ドックを追加できたときだけ登録する。2 つは続けて追加する。OBS は
		// 追加した順に [ツール] の末尾に並べる (OBS 31.1.1 の frontend/OBSStudioAPI.cpp の
		// obs_frontend_add_tools_menu_item) ので、「cordmuxをアンインストール...」が「cordmuxの設定」のすぐ下になる。
		obs_frontend_add_tools_menu_item(obs_module_text(cordmux::ledger::text::keys::kSettingsToolsMenuMenu),
						 open_settings_from_tools_menu, nullptr);
		obs_frontend_add_tools_menu_item(
			obs_module_text(cordmux::ledger::text::keys::kSettingsToolsMenuUninstallMenu),
			open_uninstall_from_tools_menu, nullptr);
		cordmux::sources::set_composite_open_settings(open_settings_from_source);
	} else {
		// 追加できなかったときは、OBS がウィジェットを受け取っていないので、ここで破棄する。ドックは表示されないが、
		// ほかの処理は続ける。
		delete dock_widget;
		cordmux::diag::logf(LOG_WARNING, "Couldn't add the dock");
		// 起動時の書き直しを Vesktop の終了の後に試すのはドックなので、ドックがない起動では試さない。そのことを
		// 1 行書く (上の「waiting」の行だけだと、後で書き直すように読める)。
		if (retry_startup_rewrite)
			cordmux::diag::logf(LOG_INFO,
					    "Startup check of Vesktop's Vencord location: not retried this session "
					    "because the dock couldn't be added");
	}

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	// フォーカスリングのイベントフィルター (アプリケーション全体) を外す。モーダルでない設定ウィンドウを開いたまま OBS を
	// 終了すると、フォーカスリングが cordmux.dll より後まで残るので、その後に Qt がフィルターを呼ばないようにする
	// (dock/ui/focus_ring.h の release_focus_input_watch)。
	cordmux::dock::ui::release_focus_input_watch();

	// DockPoll は Pipeline と OsProbe を参照で使うので、先に止めて破棄する。ドックのウィジェットには触らない。
	// ウィジェットは OBS のメインウィンドウと一緒にもう破棄されている (dock/dock_bind.h の add_dock)。
	// obs_frontend_remove_dock も呼ばない。この時点では frontend API のコールバックが外れていて、ログにエラーが書かれる
	// だけである。
	// 合成のソースのプロパティのボタンが、破棄したドックを呼ばないように先に外す。
	cordmux::sources::set_composite_open_settings(nullptr);
	// 全画面のホットキーも、DockPoll を破棄する前に外す。
	unregister_focus_hotkeys();
	if (g_dock_poll)
		g_dock_poll->stop();
	g_dock_poll.reset();
	// ドックの周期がもう動かないので、音声ソースに付けたコールバックと、自分の枠に映すソースへの結び付けが残っていれば外す。
	cordmux::sources::release_self_speaking();
	cordmux::sources::release_self_frame();
	if (g_os_probe)
		g_os_probe->stop();
	g_os_probe.reset();
	g_dock_widget.clear();

	// Pipeline::stop() を Listener::stop() より先に呼ぶ。その間に io_thread から届いたコールバックは、メッセージを
	// 捨てるだけで安全に終わる (pipeline/pipeline.cpp の Pipeline::stop() の前のコメント)。
	if (g_pipeline)
		g_pipeline->stop();
	if (g_listener) {
		g_listener->stop();
		// 送信側が、終了した OBS の session.json を読んで、無関係のポートへ繋ぎ直しを試し続けないように、自分が書いた
		// ファイルを削除する。アンインストールが済んだ後は、削除したフォルダーを読み取りが作り直すので触らない。
		if (!cordmux::install::is_uninstalling()) {
			if (g_listener->remove_session_file_if_mine())
				cordmux::diag::logf(LOG_INFO, "Removed the connection details file");
		}
	}

	// Pipeline を破棄する前に、ログの出力先がリングバッファーに積むのをやめる。Listener::stop() が io_thread を
	// join した後なので、io_thread からのログの行も、もう来ない。
	{
		std::lock_guard<std::mutex> lock(g_session_guard_mutex);
		g_pipeline_for_log = nullptr;
	}

	// 合成のソースの子を解放する。Pipeline::stop() の後なので、子にフレームを渡すスレッドはもうない。最後の合成の
	// ソースを削除したときに解放済みならば何もしない。
	cordmux::sources::release_composite_children();

	// Pipeline が Listener と AllocationTable を参照で使うので、Pipeline から破棄する。
	g_pipeline.reset();
	g_listener.reset();
	g_table.reset();

	cordmux::platform::shutdown();
	cordmux::diag::logf(LOG_INFO, "Module unloaded");

	// 最後の行を書いてから、ログの出力先の登録を解除する。解除した後に書いた行は捨てられる (diag/diag_log.h の
	// set_log_sink)。
	cordmux::diag::set_log_sink(nullptr);
}

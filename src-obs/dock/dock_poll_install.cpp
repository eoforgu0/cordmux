// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、Vesktop への追加、同梱の Vencord の再インストール、照合の結果の読み直し、
// アンインストール。宣言は dock_poll.h と dock_poll_install.h にある。

#include "dock/dock_poll.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "diag/diag_log.h"
#include "dock/dock_bind.h"
#include "dock/dock_poll_internal.h"
#include "dock/install_dialog_plan.h"
#include "dock/install_dialogs.h"
#include "dock/install_flow.h"
#include "dock/install_runtime_files.h"
#include "dock/ui/message_dialog.h"
#include "install/backup_first_write.h"
#include "install/build_mark.h"
#include "install/reinstall_plan.h"
#include "install/uninstall.h"
#include "install/vencord_dir.h"
#include "ledger/dock_view.h"
#include "ledger/rules.h"
#include "platform/bundle_check.h"
#include "platform/platform.h"

namespace cordmux::dock {

using detail::wall_now_us;

namespace {

// アンインストールの手順を、ログに書くときの語句。画面には表示しないので ini を使わない。OBS のログは英語で書き、
// 列挙子の名前は書かない。
const char *uninstall_step_name(install::UninstallStep step)
{
	switch (step) {
	case install::UninstallStep::MarkUninstalling:
		return "stop waiting for Vesktop";
	case install::UninstallStep::SettingsAndSecret:
		return "remove settings";
	case install::UninstallStep::SourcesAndScenes:
		return "remove sources and scenes";
	case install::UninstallStep::RestoreVencordDir:
		return "restore Vesktop's Vencord location";
	case install::UninstallStep::ShowFolder:
		return "show the folder to delete";
	case install::UninstallStep::NotRemoved:
		return "none";
	}
	return "unknown";
}

// 手順書と違う置き方 (DLL のフォルダーが plugins\<名前> でない)で、アンインストールが終わったときのダイアログに表示する、利用者に削除してもらうファイルを、
// 現在のフォルダーから作る。cordmux.dll のフォルダーと名前は現在の値から、同梱のファイルの名前はビルドのときに CMake の
// 一覧から生成したヘッダー (dock/install_ui.cmake) から、データのフォルダーは同梱の Vencord のフォルダーの親から取る
// (dock/install_flow.h の loose_install_files_of)。
std::optional<LooseInstallFiles> loose_install_files_now(const DockPollConfig &cfg)
{
	const char *module_file = obs_get_module_file_name(obs_current_module());
	const std::vector<std::string> runtime(std::begin(generated::kBundledRuntimeFiles),
					       std::end(generated::kBundledRuntimeFiles));
	return loose_install_files_of(platform::module_dir(), module_file != nullptr ? module_file : "", runtime,
				      cfg.bundled_vencord_dir);
}

// data/vencord-src か data/vencord にある cordmux-build.txt のパス (install/build_mark.h の kBuildMarkFileName)。
// フォルダーが分からなければ空にし、読めないものとして扱う。区切り文字は、ほかの所 (plugin-main.cpp の join_dir) と
// 同じく "/" にする (Windows でもそのまま開ける)。
std::string build_mark_path(const std::string &dir)
{
	if (dir.empty())
		return std::string();
	return dir + "/" + install::kBuildMarkFileName;
}

// cordmux-build.txt は数十バイトなので、読む大きさの上限は小さくし、間違って大きなファイルを読まないようにする。
// 目安の値で、測って決めたものではない (実際のファイルより十分大きければよい)。
constexpr std::size_t kBuildMarkMaxBytes = 64 * 1024;

std::optional<install::BuildMark> read_build_mark(const std::string &dir)
{
	const std::string path = build_mark_path(dir);
	if (path.empty())
		return std::nullopt;
	std::string text;
	if (!platform::read_file_bytes(path, text, kBuildMarkMaxBytes))
		return std::nullopt;
	return install::parse_build_mark(text);
}

// 書き込みを断った理由 (install::RefuseReason) を、ログに書くときの語句。
const char *refuse_name(install::RefuseReason reason)
{
	switch (reason) {
	case install::RefuseReason::BrokenJson:
		return "Vesktop's settings file isn't valid JSON";
	case install::RefuseReason::NotObject:
		return "Vesktop's settings file has an unexpected format";
	case install::RefuseReason::VencordDirNotString:
		return "Vesktop's Vencord location has an unexpected value";
	case install::RefuseReason::ForeignChange:
		return "Vesktop's Vencord location was changed outside cordmux";
	case install::RefuseReason::SceneNotAllowed:
		return "this case isn't changed automatically";
	}
	return "unknown";
}

// 書き込む状況 (install::WriteScene) を、ログに書くときの語句。
const char *scene_name(install::WriteScene scene)
{
	switch (scene) {
	case install::WriteScene::Pressed:
		return "Add to Vesktop pressed";
	case install::WriteScene::FixRequested:
		return "switch confirmed";
	case install::WriteScene::Startup:
		return "at startup";
	}
	return "unknown";
}

// [Vesktopから削除] の結果 (install::VesktopRemoveOutcome) を、ログに書くときの語句。
const char *remove_outcome_name(install::VesktopRemoveOutcome outcome)
{
	switch (outcome) {
	case install::VesktopRemoveOutcome::Restored:
		return "restored Vesktop's previous Vencord location";
	case install::VesktopRemoveOutcome::AlreadyRestored:
		return "Vesktop already uses its previous Vencord location";
	case install::VesktopRemoveOutcome::VesktopRunning:
		return "not changed, Vesktop is running";
	case install::VesktopRemoveOutcome::VesktopUnknown:
		return "not changed, couldn't check whether Vesktop is running";
	case install::VesktopRemoveOutcome::StatePathUnknown:
		return "couldn't, the location of Vesktop's settings file is unknown";
	case install::VesktopRemoveOutcome::StateUnreadable:
		return "couldn't, Vesktop's settings file couldn't be read";
	case install::VesktopRemoveOutcome::NoBackup:
		return "couldn't, the backup of Vesktop's previous setting couldn't be read";
	case install::VesktopRemoveOutcome::BackupInvalid:
		return "couldn't, the backup of Vesktop's previous setting has an unexpected format";
	case install::VesktopRemoveOutcome::Refused:
		return "couldn't, Vesktop's settings file isn't in a state cordmux can restore";
	case install::VesktopRemoveOutcome::WritesDisabled:
		return "couldn't, writing Vesktop's settings is turned off in settings.json";
	case install::VesktopRemoveOutcome::StateWriteFailed:
		return "couldn't, Vesktop's settings file couldn't be written";
	}
	return "unknown";
}

// state.json とバックアップを読む。write_vencord_dir の中で、確認のダイアログの前と、書く直前の 2 回読むので、
// 関数にしてある。
struct VencordDirInputs {
	std::string state_path;
	std::string state_text;
	std::optional<install::Backup> backup;
};

// 読めたら nullopt を返し、読めなければ [Vesktopに追加] の失敗の理由 (ledger/observation.h の VesktopAddFailure) を
// 返す。バックアップを読めないことは失敗にせず、なかったものとして続ける。
std::optional<ledger::VesktopAddFailure> read_vencord_dir_inputs(const std::string &backup_json_path,
								  std::size_t max_bytes, VencordDirInputs *out)
{
	if (!platform::vesktop_state_json_path(out->state_path)) {
		diag::logf(LOG_WARNING,
			   "Couldn't add cordmux to Vesktop: the location of Vesktop's settings file is unknown");
		return ledger::VesktopAddFailure::StatePathUnknown;
	}
	if (!platform::read_file_bytes(out->state_path, out->state_text, max_bytes)) {
		diag::logf(LOG_WARNING, "Couldn't add cordmux to Vesktop: Vesktop's settings file couldn't be read");
		return ledger::VesktopAddFailure::StateUnreadable;
	}
	// 現在のバックアップ。読めなければ、なかったものとして渡す (install/vencord_dir.h の plan_write の current)。
	out->backup.reset();
	if (!backup_json_path.empty()) {
		std::string backup_text;
		if (platform::read_file_bytes(backup_json_path, backup_text, max_bytes)) {
			install::Backup backup;
			if (install::backup_from_json(backup_text, &backup))
				out->backup = backup;
			else
				diag::logf(LOG_WARNING,
					   "Couldn't read the backup of Vesktop's previous setting; continuing "
					   "without it");
		}
	}
	return std::nullopt;
}

// path のファイルがないと確認できたときだけ true を返す。親のフォルダーを読めて、その中にないときである。
// 場所が分からない、フォルダーを読めないときは false にする。
bool file_known_absent(const std::string &path)
{
	const std::size_t cut = path.find_last_of("/\\");
	if (cut == std::string::npos)
		return false;
	std::vector<platform::DirEntry> entries;
	if (!platform::list_directory(path.substr(0, cut), entries))
		return false;
	const std::string name = path.substr(cut + 1);
	for (const platform::DirEntry &entry : entries) {
		if (!entry.is_directory && entry.name.size() == name.size() &&
		    std::equal(name.begin(), name.end(), entry.name.begin(), [](char a, char b) {
			    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		    }))
			return false;
	}
	return true;
}

// 現在の Vencord の設定 (Vesktop の settings/settings.json) の cordmux の項をバックアップの形で返す (Backup::plugin_entry)。
// ファイルがないと確認できたとき (親のフォルダーを読めて、その中にない) だけ「項がない」とし、場所が分からない、
// 読めない、確認できないときは「分からない」にする。分からないと、アンインストールは Vencord の設定に触らない。
install::PluginEntryBackup read_plugin_entry_now(std::size_t max_bytes)
{
	std::string path;
	if (!platform::vesktop_settings_json_path(path))
		return {};
	std::string text;
	if (platform::read_file_bytes(path, text, max_bytes))
		return install::snapshot_plugin_entry(text, install::kVencordPluginName);

	if (!file_known_absent(path))
		return {}; // ファイルはあるのに読めない、または確認できない
	install::PluginEntryBackup absent;
	absent.state = install::PluginEntryState::Absent;
	return absent;
}

// 押された操作の work を、押したボタンの見た目と処理中の表示を描画した後に実行する。tick で処理中の表示を View に
// 入れ、ポストされている描き直しのイベント (UpdateRequest) をその場で送って画面まで描画させてから、0 ms のタイマーで
// work をイベントループの次の周回に送る。
// 描き直しのイベントを待たずに送るのは、Qt 6.8.3 が UpdateRequest を Qt::LowEventPriority でポストするのに対し、
// 0 ms のタイマーは普通の優先度で処理されるので、待つと work が先に実行されて UI スレッドを止めるためである
// (Qt のソースの sendUpdateRequest で確認した)。
// https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/kernel/qwidgetrepaintmanager.cpp
// 送るのは描き直しのイベントだけで、入力のイベントは送らないので、work の途中で別のボタンが押されることはない。
// ポーリングが止まった後 (timer がない。OBS を閉じている途中) は、QTimer::singleShot に渡す
// コンテキストオブジェクトがないので、work をすぐに実行する。コンテキストオブジェクトを timer にするので、待つ間に
// ポーリングが止まれば、work は実行されずに捨てられる (止まった DockPoll に触らない)。
template <typename Tick, typename Work> void after_paint(QTimer *timer, const Tick &tick, Work work)
{
	if (timer == nullptr) {
		work();
		return;
	}
	tick();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
	QTimer::singleShot(0, timer, std::move(work));
}

} // namespace

void DockPoll::write_vencord_dir(install::WriteScene scene)
{
	if (writes_stopped()) {
		// 押せる条件は ledger::apply が先に確認している。ここに来るのは、ダイアログを待つ間に
		// アンインストールしたときだけ。
		diag::logf(LOG_INFO, "Didn't add cordmux to Vesktop: cordmux has been uninstalled");
		return;
	}
	if (install_state_.adding_to_vesktop) {
		diag::logf(LOG_INFO, "Add to Vesktop is already running; the second press was ignored");
		return;
	}
	if (cfg_.bundled_vencord_dir.empty()) {
		diag::logf(LOG_WARNING, "Couldn't add cordmux to Vesktop: the bundled Vencord folder is unknown");
		ops_.vesktop_add_failure = ledger::VesktopAddFailure::BundledDirUnknown;
		return;
	}

	// 動いているポータブル版の Vesktop を見たなら、書かない。ポータブル版の設定は、Vesktop の実行ファイルの隣の Data に
	// あり、OBS の側からは場所を知れない。書くと、Vesktop が読まない場所の state.json を変えて、書いたことになる。
	if (platform::vesktop_portable_seen()) {
		diag::logf(LOG_WARNING,
			   "Didn't add cordmux to Vesktop: a portable Vesktop was seen, and its settings location is unknown");
		ops_.vesktop_add_failure = ledger::VesktopAddFailure::PortableVesktop;
		return;
	}

	VencordDirInputs inputs;
	if (const std::optional<ledger::VesktopAddFailure> failure =
		    read_vencord_dir_inputs(cfg_.backup_json_path, cfg_.max_json_bytes, &inputs)) {
		ops_.vesktop_add_failure = failure;
		return;
	}

	// ほかの Vencord から切り替えるかを確認する。確認が必要かは、純粋関数 other_vencord_dialog_needed
	// (dock/install_flow.h) が決める。ダイアログには押した時点の場所を表示し、閉じた後に読み直してから書く。
	// ダイアログを開いている間に Vesktop か利用者が書き換えたら、新しい値で、書くかどうかを判断し直すためである。
	// ダイアログの親はドックにし、操作を止めるのは親ウィンドウだけにする (OwnerOnly。install_dialogs.cpp の
	// spec_of)。[Vesktopに追加] は配信中も押せるので、フローティングのドックから開いたときに、
	// OBS のメインウィンドウの操作を止めないためである。
	const std::optional<std::string> dir_now = platform::parse_vencord_dir(inputs.state_text);
	const std::optional<std::string> last_written =
		inputs.backup.has_value() ? std::optional<std::string>(inputs.backup->written) : std::nullopt;
	if (other_vencord_dialog_needed(dir_now, cfg_.bundled_vencord_dir, last_written)) {
		const int choice = ui::run_message_dialog(widget_.data(), switch_confirm_spec(*dir_now));
		// ダイアログを開いている間に OBS の終了が始まったら、何もしない (ポーリングもウィジェットも、
		// もうない)。
		if (!timer_ || widget_.isNull()) {
			diag::logf(LOG_INFO,
				   "Didn't add cordmux to Vesktop: the dock closed before the dialog was answered");
			return;
		}
		// ダイアログを開いている間に、メインウィンドウから開いた設定ウィンドウでアンインストールしていたら、
		// 書かない。書くと、アンインストールで元に戻した Vesktop の設定を、また書き換えることになる
		// (OwnerOnly でメインウィンドウを操作できるので起きる)。ログには、この関数の最初の確認と同じ行を書く。
		if (writes_stopped()) {
			diag::logf(LOG_INFO, "Didn't add cordmux to Vesktop: cordmux has been uninstalled");
			return;
		}
		if (choice != 0) {
			diag::logf(LOG_INFO,
				   "Didn't add cordmux to Vesktop: the switch from another Vencord was canceled (%s)",
				   scene_name(scene));
			// [キャンセル] は失敗ではない。前の失敗のメッセージも、利用者が選び直したので消す。
			ops_.vesktop_add_failure.reset();
			return;
		}
		// [切り替え] を選んだので、利用者が切り替えを指示した場合 (WriteScene::FixRequested) として書く。
		// 押したのが追加されていないときの [Vesktopに追加] (Pressed) でも、ダイアログを開いている間に利用者か
		// ほかのツールが別の値を書いていると、Pressed のままでは何も書かずに終わり、確認のダイアログ
		// (cordmux.dialog.vesktop_switch.headline) で切り替えを選んだのに切り替わらない。ほかの場合は、
		// Pressed と同じに扱われる (install/vencord_dir.h の plan_write)。
		scene = install::WriteScene::FixRequested;
	}

	// 書き込みの処理 (読み直し、計画、バックアップ、Vesktop の設定)。失敗したらその理由を返し、書けたときと、
	// 既に同じ場所を指していたときは nullopt を返す。
	const auto write_now = [this](install::WriteScene write_scene) -> std::optional<ledger::VesktopAddFailure> {
		VencordDirInputs now;
		if (const std::optional<ledger::VesktopAddFailure> failure =
			    read_vencord_dir_inputs(cfg_.backup_json_path, cfg_.max_json_bytes, &now))
			return failure;

		// 現在の Vencord の設定の cordmux の項。plan_write が、前の項としてバックアップに入れるかを決める。
		const install::PluginEntryBackup plugin_entry_now = read_plugin_entry_now(cfg_.max_json_bytes);
		const install::WritePlan plan = install::plan_write(now.state_text, cfg_.bundled_vencord_dir, now.backup,
								    wall_now_us(), cfg_.dll_dir, write_scene, plugin_entry_now);
		switch (plan.action) {
		case install::WriteAction::AlreadyOurs:
			diag::logf(LOG_INFO, "Vesktop already uses the Vencord bundled with cordmux");
			// 前の [Vesktopから削除] の失敗は、もう表示しない (追加されている状態を、利用者が選び直した)。
			ops_.vesktop_remove_failure.reset();
			return std::nullopt;
		case install::WriteAction::Refuse:
			diag::logf(LOG_WARNING,
				   "Couldn't add cordmux to Vesktop: %s (%s)", refuse_name(plan.refuse_reason),
				   scene_name(write_scene));
			return ledger::VesktopAddFailure::Refused;
		case install::WriteAction::Write:
			break;
		}

		// この起動で Vesktop の設定への書き込みを止めているとき (allow_state_json_write が false。
		// settings.json で無効にしたときと、起動時に settings.json を読めなかったとき) は、書き込まずに終わる。
		// ここまでの処理 (読み込み、照合、書く文字列の作成) は、止めているときも毎回通る。
		if (!cfg_.allow_state_json_write) {
			diag::logf(LOG_INFO,
				   "Didn't add cordmux to Vesktop: writing Vesktop's settings is turned off in "
				   "settings.json");
			return ledger::VesktopAddFailure::WritesDisabled;
		}

		// バックアップを先に書き、書けなければ state.json に触らない。バックアップには、元の値と cordmux が
		// 書いた値の両方を残す。state.json を書けなかったら、バックアップを書く前の状態に戻す
		// (install/backup_first_write.h)。進んだままにすると、cordmux のフォルダーを移した後で、押し直しが
		// 利用者の元の値ではなく cordmux の古い場所を元の値として保存し、アンインストールがそこへ戻す。
		// どちらも、一時ファイルに書いてから置き換える (platform.h の write_file_atomic)。その場で切り詰めて
		// 書くと、途中で OBS がクラッシュしたときに、Vesktop が壊れた state.json を読む。
		if (!plan.new_backup.has_value() || cfg_.backup_json_path.empty()) {
			diag::logf(LOG_WARNING,
				   "Couldn't add cordmux to Vesktop: there's no place to keep a backup of the "
				   "previous setting");
			return ledger::VesktopAddFailure::NoBackupPlace;
		}
		// 書く直前に、Vesktop が起動していないことを確認する。押せるとした判断は、最大で 30 秒 (接続している間は
		// 5 分) 前の OsProbe の結果によるので、その後に Vesktop が起動していることがある。確認のダイアログを開いて
		// いる間に起動されることもある。起動中に書くと、Vesktop がメモリの値で state.json を書き直し、書いた値が
		// 消える (ledger/actions.cpp の vesktop_busy_reason)。起動していたら書かず、失敗にもしない。押したボタンが、
		// 押せない理由と一緒に押せない見た目に変わる (vesktop_free_to_write)。
		if (!vesktop_free_to_write()) {
			diag::logf(LOG_INFO, "Didn't add cordmux to Vesktop: Vesktop is running, or it couldn't be checked (%s)",
				   scene_name(write_scene));
			return std::nullopt;
		}
		install::BackupFirstOps write_ops;
		write_ops.read_file_bytes = [](const std::string &path, std::string &out, std::size_t max_bytes) {
			return platform::read_file_bytes(path, out, max_bytes);
		};
		write_ops.write_file_atomic = [](const std::string &path, const std::string &text) {
			return platform::write_file_atomic(path, text);
		};
		write_ops.delete_file = [](const std::string &path) { return platform::delete_file(path); };
		write_ops.file_known_absent = [](const std::string &path) { return file_known_absent(path); };
		// 書く場所をバックアップに残す。元に戻すときは、現在の環境変数ではなく、この場所を使う。
		install::Backup new_backup = *plan.new_backup;
		new_backup.state_path = now.state_path;
		const install::BackupFirstResult written = install::write_backup_then_state(
			write_ops, cfg_.max_json_bytes, cfg_.backup_json_path, install::backup_to_json(new_backup),
			now.state_path, plan.new_text);
		if (written.outcome == install::BackupFirstOutcome::BackupWriteFailed) {
			diag::logf(LOG_WARNING,
				   "Couldn't add cordmux to Vesktop: the backup of the previous setting couldn't be "
				   "saved");
			return ledger::VesktopAddFailure::BackupWriteFailed;
		}
		if (written.outcome == install::BackupFirstOutcome::StateWriteFailed) {
			diag::logf(LOG_WARNING,
				   "Couldn't add cordmux to Vesktop: Vesktop's settings file couldn't be written "
				   "(backup %s)",
				   written.backup_restored ? "restored" : "not restored");
			return ledger::VesktopAddFailure::StateWriteFailed;
		}
		diag::logf(LOG_INFO, "Added cordmux to Vesktop (%s)", scene_name(write_scene));
		ops_.vesktop_remove_failure.reset();
		return std::nullopt;
	};

	// 押したボタンの見た目と「追加しています...」を先に描画してから書く。失敗の表示は、押した人が次の操作をするまで
	// 残し、書けたときと、既に同じ場所を指していたときに消す。
	install_state_.adding_to_vesktop = true;
	after_paint(
		timer_.get(), [this]() { tick(); },
		[this, scene, write_now]() {
			ops_.vesktop_add_failure = write_now(scene);
			install_state_.adding_to_vesktop = false;
			// 書き換えたのは OS の側の状態なので、OsProbe の次の問い合わせ (30 秒か 5 分ごと) を待たずに、
			// もう 1 回問い合わせる。押した結果 (Vesktop の設定の変化や「Vesktopを再起動してください」) が
			// 表示されるまで、何分も待たせないためである。書けなかったときも問い合わせ直し、
			// 書けなかった理由がファイルの側にあれば、現在の状態を画面に反映する。
			probe_.request_probe_now();
			tick();
		});
}

bool DockPoll::vesktop_free_to_write()
{
	const std::optional<bool> running = platform::vesktop_running_now();
	if (running.has_value() && !*running)
		return true;
	// 起動していた (か、調べられなかった) ことを覚え、OsProbe の新しい結果が届くまで、画面はこの値で作る
	// (DockFacts::vesktop_process_seen)。OsProbe にはすぐ問い合わせ直させ、30 秒の間隔を待たない。
	vesktop_process_seen_ = running.has_value() ? ledger::VesktopProcess::Running : ledger::VesktopProcess::Unknown;
	vesktop_process_seen_at_us_ = wall_now_us();
	probe_.request_probe_now();
	return false;
}

void DockPoll::remove_from_vesktop()
{
	if (writes_stopped()) {
		// 押せる条件は ledger::apply が先に確認している (アンインストールを始めた後は押せない)。
		diag::logf(LOG_INFO, "Didn't remove cordmux from Vesktop: cordmux has been uninstalled");
		return;
	}
	// アンインストールの段階 (3) と同じ入力と関数を使う (dock/install_flow.h の uninstall_context_of と、
	// install/uninstall.h の make_real_uninstall_file_ops)。Vesktop が起動していないかの確認だけは、ドックの関数に
	// 替える。起動していたことを覚えて、OsProbe の次の結果を待たずに「Vesktop」の組を表示しないようにするためである
	// (vesktop_free_to_write。組は Vesktop が起動していない間だけ表示する)。
	const install::UninstallContext context =
		uninstall_context_of(cfg_, module_config_dir(), pipeline_.record_base_dir());
	install::UninstallFileOps ops = install::make_real_uninstall_file_ops();
	ops.vesktop_running = [this]() -> std::optional<bool> {
		if (vesktop_free_to_write())
			return false;
		if (vesktop_process_seen_ == ledger::VesktopProcess::Running)
			return true;
		return std::nullopt;
	};
	const install::VesktopRemoveOutcome outcome = install::remove_from_vesktop(ops, context);
	diag::logf(outcome == install::VesktopRemoveOutcome::Restored ||
				   outcome == install::VesktopRemoveOutcome::AlreadyRestored ||
				   outcome == install::VesktopRemoveOutcome::VesktopRunning ||
				   outcome == install::VesktopRemoveOutcome::VesktopUnknown
			   ? LOG_INFO
			   : LOG_WARNING,
		   "Remove from Vesktop: %s", remove_outcome_name(outcome));
	// 失敗は利用者が次に押すまで残す。成功したときは、前の失敗と、前の [Vesktopに追加] の失敗を消す (どちらも、
	// 現在の状態と合わない)。書く直前に Vesktop が起動していたときは、前の失敗を残したまま、組を表示しない。
	if (outcome == install::VesktopRemoveOutcome::Restored ||
	    outcome == install::VesktopRemoveOutcome::AlreadyRestored) {
		ops_.vesktop_remove_failure.reset();
		ops_.vesktop_add_failure.reset();
	} else if (const std::optional<ledger::VesktopRemoveFailure> failure = remove_failure_of(outcome)) {
		ops_.vesktop_remove_failure = failure;
	}
	// 書き換えたのは OS の側の状態なので、OsProbe の次の問い合わせを待たずに、もう 1 回問い合わせる
	// (write_vencord_dir の最後と同じ理由)。
	probe_.request_probe_now();
}

bool DockPoll::reinstall_vencord_files(bool advance_written_at)
{
	// コピー元の data/vencord-src と、コピー先の data/vencord。どちらかが分からなければコピーできない。
	const std::string &src_dir = cfg_.bundled_vencord_src_dir;
	const std::string &shown_dir = cfg_.bundled_vencord_dir;

	// data/vencord-src のファイルの一覧 (Vencord のファイル、.LEGAL.txt、cordmux-build.txt)。フォルダーは
	// コピーしない。
	std::vector<std::string> names;
	if (!src_dir.empty()) {
		std::vector<platform::DirEntry> entries;
		if (platform::list_directory(src_dir, entries)) {
			for (const platform::DirEntry &entry : entries) {
				if (!entry.is_directory)
					names.push_back(entry.name);
			}
		}
	}

	// コピーの順 (cordmux-build.txt が最後。install/reinstall_plan.h) を作り、1 つずつコピーする。
	// data/vencord-src のファイルを数えられなかったときは、data/vencord-src の場所を、コピーできなかったものとして
	// 表示する。空の計画を、コピーするものがないと解釈しないためである (dock/install_flow.h の run_copy_steps)。
	const std::vector<install::CopyStep> steps =
		src_dir.empty() || shown_dir.empty() ? std::vector<install::CopyStep>{}
						     : install::plan_reinstall(src_dir, shown_dir, names);
	const std::vector<std::string> failed = run_copy_steps(
		steps,
		[](const std::string &from, const std::string &to) { return platform::copy_file_replace(from, to); },
		src_dir.empty() ? std::string("vencord-src") : src_dir);
	// 自動のコピーし直しが、前と同じファイルで失敗したときは、ログに書かない。前のバージョンのファイルが
	// 見つかるたびにコピーし直すので、コピーできないままだと、問い合わせのたびに同じ行が並ぶためである。
	// [再インストール] を押したとき (advance_written_at) は、利用者の操作なので毎回書く。
	const bool same_failure_as_before = !advance_written_at && !failed.empty() && failed == reinstall_failed_files_;
	reinstall_failed_files_ = failed;

	if (!failed.empty()) {
		if (!same_failure_as_before)
			diag::logf(LOG_WARNING, "Couldn't reinstall the bundled Vencord files (%zu files failed)",
				   failed.size());
		// 自動のコピーし直しで失敗したときは、問い合わせ直さない。問い合わせ直すと、次の結果でまた
		// コピーし直し、コピーできない間はそれを繰り返すためである (dock/install_flow.h の
		// probe_after_reinstall)。
		if (probe_after_reinstall(false, advance_written_at))
			probe_.request_probe_now();
		return false;
	}
	diag::logf(LOG_INFO, "Reinstalled the bundled Vencord files from the original (%zu files)", steps.size());

	// [再インストール] を押したときは、バックアップの written_at だけを進める。
	// これで「Vesktopを再起動してください」のメッセージが表示される (ledger/observation.h の restart_needed)。
	// バックアップがない (まだ 1 度も Vesktop の設定を書いていない) なら、進める値がない。
	if (advance_written_at && !cfg_.backup_json_path.empty()) {
		std::string backup_text;
		install::Backup backup;
		if (platform::read_file_bytes(cfg_.backup_json_path, backup_text, cfg_.max_json_bytes) &&
		    install::backup_from_json(backup_text, &backup)) {
			const install::Backup advanced = install::advance_written_at(backup, wall_now_us());
			if (!platform::write_file_atomic(cfg_.backup_json_path, install::backup_to_json(advanced)))
				diag::logf(LOG_WARNING,
					   "Couldn't save the reinstall time in the backup of Vesktop's previous "
					   "setting");
		} else {
			diag::logf(LOG_INFO,
				   "No backup of Vesktop's previous setting, so the reinstall time wasn't saved");
		}
	}

	// コピーしたのは OS の側の状態なので、すぐに問い合わせ直す (write_vencord_dir の最後と同じ理由。
	// probe_after_reinstall)。
	if (probe_after_reinstall(true, advance_written_at))
		probe_.request_probe_now();
	return true;
}

void DockPoll::on_probe_result(const platform::OsProbeResult &probe)
{
	// OsProbe が新しい結果を置いたときだけ読み、周期ごとには読まない。起動直後はまだ問い合わせていない
	// (probed_at_us が 0) が、そのときも 1 回読む。起動時と、照合のたびに読むためである。
	if (marks_read_for_probe_at_.has_value() && *marks_read_for_probe_at_ == probe.probed_at_us)
		return;
	marks_read_for_probe_at_ = probe.probed_at_us;

	original_mark_ = read_build_mark(cfg_.bundled_vencord_src_dir);
	shown_mark_ = read_build_mark(cfg_.bundled_vencord_dir);

	if (!probe.probed)
		return;

	// 起動時の書き直しを、Vesktop が起動していたので待っているとき。OsProbe の新しい結果で Vesktop が起動していないと
	// 分かったら、もう一度試す (plugin-main.cpp の起動時の書き直しのコメント)。試す関数は、書く直前にも Vesktop が起動して
	// いないことを確認する。Waiting でない結果が返ったら、もう試さない (書き直すのは 1 回だけ)。アンインストールを始めた
	// 後は試さない (アンインストールが Vesktop の設定を戻すので、書き直すと戻した値を上書きする)。
	if (startup_rewrite_waiting_ && !probe.vesktop_process && !writes_stopped() && cfg_.retry_startup_rewrite) {
		switch (cfg_.retry_startup_rewrite(settings_.feature_enabled)) {
		case StartupRewriteRetry::Wrote:
			rewrote_at_startup_ = true;
			startup_rewrite_waiting_ = false;
			// 書き換えたのは OS の側の状態なので、すぐ問い合わせ直す (write_vencord_dir の最後と同じ理由)。
			probe_.request_probe_now();
			break;
		case StartupRewriteRetry::Waiting:
			break;
		case StartupRewriteRetry::Done:
			startup_rewrite_waiting_ = false;
			break;
		}
	}

	// 照合で一致したら、前にコピーできなかったファイルの名前を消す (消すのはコピーする側の役目。
	// ledger/observation.h の Install::reinstall_failed_files)。
	if (probe.bundle_hash.state == platform::BundleState::Matches)
		reinstall_failed_files_.clear();

	// 前のバージョンの cordmux のファイルなら、メッセージを表示せずに data/vencord-src からコピーし直す (ログには
	// 1 行書く)。cordmux を無効にしているときと、アンインストールを始めた後はコピーしない。
	const install::ShownBundleVerdict verdict =
		install::classify_shown_bundle(original_mark_, shown_mark_, hashes_match_of(probe.bundle_hash.state));
	if (verdict != install::ShownBundleVerdict::StaleOwnBundle)
		return;
	if (!settings_.feature_enabled || writes_stopped())
		return;
	// コピーし直しに失敗した後のやり直しでは、ログに書かない (失敗したことは reinstall_vencord_files が
	// 書いている)。失敗した後のやり直しは、定期的な問い合わせの結果からだけ来る (自動のコピーで失敗したら、
	// 問い合わせ直さない。dock/install_flow.h の probe_after_reinstall)。
	if (reinstall_failed_files_.empty())
		diag::logf(LOG_INFO,
			   "The Vencord folder Vesktop uses has files from an earlier cordmux; reinstalling them");
	if (reinstall_vencord_files(false))
		stale_copied_for_probe_at_ = probe.probed_at_us;
}

void DockPoll::uninstall_now()
{
	// 処理は DockPollInstallPart::uninstall にある (親ウィンドウを受け取る)。ドックから押したときの親は
	// ドックにする。
	DockPollInstallPart::uninstall(*this, widget_.data());
}

void DockPollInstallPart::uninstall(DockPoll &self, QWidget *owner, QWidget *focus_fallback)
{
	if (self.install_state_.uninstalling || self.install_state_.uninstall_confirm_open ||
	    self.uninstall_phase_ == ledger::UninstallPhase::Running ||
	    self.uninstall_phase_ == ledger::UninstallPhase::Done) {
		diag::logf(LOG_INFO, "Ignored Uninstall: it's already done or running");
		return;
	}

	// アンインストールできるかを、配信と録画の現在の状態を取得し直して判断する (ページのボタンと同じ ledger::apply を
	// 使う。dock/install_flow.h の uninstall_can_start)。確認のダイアログの前と後の 2 回判断する。
	// 前に判断するのは、ボタンと [ツール] メニューの項目が読む View が最後のポーリングのもので、その後に配信や
	// 録画を始めていたときに、開いても後で断る確認のダイアログを開かないためである。アンインストールできないときは
	// ダイアログを開かず、下の 0 ms のタイマーの tick で、ページに理由を描く (設定ウィンドウは既に
	// スクロールしてある。settings_window.cpp の start_uninstall)。
	// 同じ情報を持つ 2 か所 (uninstall と obs_outputs) の両方に入れる。仮想カメラとリプレイバッファも、
	// ここで取得し直す (どちらもアンインストールできない理由になる。ポーリングでの取得は dock_poll.cpp)。
	// 取得し直さないと、確認のダイアログを開いている間に仮想カメラを始めたときに、最後のポーリングの値で
	// 判断してしまう。この 2 つは uninstall にだけあり、obs_outputs には配信と録画だけがある。
	const auto available_now = [&self]() {
		const bool streaming = obs_frontend_streaming_active();
		const bool recording = obs_frontend_recording_active();
		self.last_observation_.uninstall.obs_streaming = streaming;
		self.last_observation_.uninstall.obs_recording = recording;
		self.last_observation_.obs_outputs.streaming = streaming;
		self.last_observation_.obs_outputs.recording = recording;
		self.last_observation_.uninstall.obs_virtual_camera = obs_frontend_virtualcam_active();
		self.last_observation_.uninstall.obs_replay_buffer = obs_frontend_replay_buffer_active();
		return uninstall_can_start(self.last_observation_);
	};
	if (!available_now()) {
		diag::logf(LOG_INFO, "Didn't open the Uninstall confirmation: it's unavailable right now");
		// 取得し直した状態で、画面 (押せないボタンの見た目と理由) をイベントループの次の周回で描き直す。
		// 押されたボタンのシグナルの中では描き直さない (dock_poll.h の run_dialog_action)。ポーリングを待つと、
		// 押しても何も起きない時間が、最大で 1 回のポーリングの間残る。
		if (self.timer_)
			QTimer::singleShot(0, self.timer_.get(), [&self]() { self.tick(); });
		return;
	}

	// 確認のダイアログを開く。親は押した所のウィンドウにする。2 つ目の確認のダイアログを開かないように、開く前に
	// フラグを立てる (閉じたら false にする)。
	// 操作を止めるのは、どこから始めても親ウィンドウだけにする (OwnerOnly。理由は install_dialogs.cpp の spec_of)。
	// 開いている間も、親ではないウィンドウ (メインウィンドウ、設定ウィンドウ、ドック) を操作できる。
	// もう 1 つの確認のダイアログは、上のフラグで開かない。押した後の配信と録画の状態は、下で再確認する。
	// 親が破棄されるとダイアログは -1 を返し、下の choice != 0 でキャンセルとして扱う。
	self.install_state_.uninstall_confirm_open = true;
	const QPointer<QWidget> owner_ptr(owner);
	const QPointer<QWidget> fallback_ptr(focus_fallback);
	// 確認のダイアログの最後の文 (フォルダーの場所を表示するか、ファイルとフォルダーの場所を表示するか) は、
	// 結果のダイアログの形と同じ判断で決める (消すフォルダーが 1 つに決まるか。install/uninstall.h の
	// install::real_folder_to_delete)。
	ui::MessageSpec confirm = uninstall_confirm_spec(install::real_folder_to_delete().empty());
	confirm.focus_fallback = fallback_ptr;
	const int choice = ui::run_message_dialog(owner, confirm);
	self.install_state_.uninstall_confirm_open = false;
	if (!self.timer_ || self.widget_.isNull()) {
		diag::logf(LOG_INFO, "Didn't uninstall: the dock closed before the dialog was answered");
		return;
	}
	if (choice != 0) {
		diag::logf(LOG_INFO, "Uninstall canceled");
		return;
	}

	// ダイアログを開いている間に配信や録画を始めていたら、アンインストールしない。判断は画面に表示していたのと同じ
	// apply() で行い、入力だけを現在の状態で取得し直す (上の available_now)。
	if (!available_now()) {
		diag::logf(LOG_INFO, "Didn't uninstall: it became unavailable while the dialog was open");
		return;
	}

	self.uninstall_phase_ = ledger::UninstallPhase::Running;
	self.install_state_.uninstalling = true;
	diag::logf(LOG_INFO, "Uninstall started");

	// 前の再インストールでコピーできなかったファイルの名前を捨てる。アンインストールした後は data/vencord ごと
	// 削除してもらうので、もう押せない [再インストール] を勧め続けない。
	self.reinstall_failed_files_.clear();

	const auto work = [&self, owner_ptr, fallback_ptr]() {
		// 実際のアンインストールの手順に渡す入力 (install/uninstall.h の UninstallContext)。どのフィールドに
		// 何を入れるかは純粋関数 uninstall_context_of (dock/install_flow.h) が決め、渡し忘れは単体テストで
		// 確認する。
		const install::UninstallContext context =
			uninstall_context_of(self.cfg_, module_config_dir(), self.pipeline_.record_base_dir());

		install::UninstallReport report = install::run_uninstall(install::make_real_uninstall_ops(context));
		self.uninstall_report_ = report;
		self.uninstall_phase_ = phase_after(report);
		self.install_state_.uninstalling = false;
		if (report.completed)
			diag::logf(LOG_INFO, "Uninstall completed");
		else
			diag::logf(LOG_WARNING, "Uninstall stopped (last finished step: %s; %zu items left)",
				   uninstall_step_name(report.reached),
				   report.leftovers.size() + report.collections_not_opened.size() +
					   report.collections_duplicate_names.size() +
					   (report.collection_folder_unreadable ? 1 : 0));

		// 結果を画面に反映してから、同じ View から結果のダイアログを開く。ドック、設定ウィンドウ、
		// 結果のダイアログが、同じ View から表示されるようにするためである。ダイアログの親は確認のダイアログと
		// 同じにする (破棄されていたらドック)。
		self.tick();
		if (self.widget_.isNull())
			return;
		const ledger::dockview::DockViewV2 view = ledger::decide_dock(self.last_observation_);
		if (!view.uninstall_result.has_value()) {
			diag::logf(LOG_WARNING, "Uninstall finished, but there is no result to show in the window");
			return;
		}
		// 手順書と違う置き方 (DLL のフォルダーが plugins\<名前> でない)で、アンインストールが終わったときのダイアログには、利用者に削除してもらう
		// ファイルの場所と名前、データのフォルダーを表示する。View はこの情報を持たないので、ダイアログを
		// 開く直前に、現在のフォルダーから作る (dock/install_flow.h の LooseInstallFiles)。
		std::optional<LooseInstallFiles> loose;
		if (uninstall_result_is_loose(*view.uninstall_result))
			loose = loose_install_files_now(self.cfg_);
		ui::MessageSpec result = uninstall_result_spec(*view.uninstall_result, loose ? &*loose : nullptr);
		result.focus_fallback = fallback_ptr;
		// [ファイルの場所を開く] は、エクスプローラーでそのファイルを選択した状態で開く。アンインストールが
		// 終わったときのダイアログで、「削除するフォルダー:」の行にある [フォルダーの場所を開く] も同じ関数で、
		// 親フォルダーを開いて、そのフォルダーを選択する。Windows の API は platform/ の中でだけ使う
		// (test/lint/windows_api_confinement.py)。開けなかったときは何も表示しない (platform.h の
		// open_file_location)。
		result.reveal_file = [](const QString &path) {
			platform::open_file_location(path.toUtf8().toStdString());
		};
		// 親が破棄されていたら、ドックを親にする (どちらでも、操作を止めるのは親ウィンドウだけ)。
		QWidget *result_owner = !owner_ptr.isNull() ? owner_ptr.data() : self.widget_.data();
		ui::run_message_dialog(result_owner, result);
	};
	// 「cordmuxをアンインストールしています...」を先に描画してから、アンインストールする。シーンコレクションを
	// 順に切り替えるのは、OBS の UI スレッドのまま行う (install/uninstall.h の run_uninstall)。
	after_paint(
		self.timer_.get(), [&self]() { self.tick(); }, work);
}

} // namespace cordmux::dock

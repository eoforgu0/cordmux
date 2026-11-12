// SPDX-License-Identifier: Apache-2.0

// windows、tools、pages のシナリオ。windows は、設定ウィンドウと参加者の設定ウィンドウでの値の反映、
// 診断情報のエクスポート、設定の保存の失敗と、操作を始めた範囲だけを止めるダイアログが何を止めるかを確認する。
// tools は OBS の [ツール] から始めるアンインストールを、pages は設定ウィンドウのページの一覧とフォーカスと、UI Automation で
// ページを選んだときの動きを確認する。
// DockPoll の側でウィンドウを開く処理は libobs に依存して組み込めないので、Host が同じ順と同じ引数で行う
// (Host のコメント)。

#include "scenarios.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qwindowsysteminterface.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dock/dock_widget.h"
#include "dock/install_dialogs.h"
#include "dock/participants_window.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/settings_fields.h"
#include "dock/settings_window.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/message_dialog.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/wrap_label.h"
#include "drive_env.h"
#include "input.h"
#include "ledger/natural_order.h"
#include "ledger/text_ids.h"
#include "pipeline/pipeline.h"
#include "obs_dock.h"
#include "owner_hold_probe.h"
#include "query.h"
#include "scen_common.h"
#include "script.h"
#include "sim_poll.h"
#include "ws/control_messages.h"

namespace cordmux::dock_drive {

namespace {

const QString kGroup = QStringLiteral("windows");
const QString kGroupM8 = QStringLiteral("save-failed");
const QString kGroupM10 = QStringLiteral("export");

using ledger::SettingsPage;
namespace keys = ledger::text::keys;

// フォーカスのあるウィジェットの名前 (objectName/class。ボタンなら [テキスト] も)。フォーカスの結果の note に書く。
QString focus_name(QWidget *f)
{
	if (f == nullptr)
		return QStringLiteral("(none)");
	QString out = f->objectName() + QLatin1Char('/') + QString::fromLatin1(f->metaObject()->className());
	if (auto *b = qobject_cast<QAbstractButton *>(f))
		out += QStringLiteral(" [") + b->text() + QLatin1Char(']');
	return out;
}

// ウィンドウのアクティブとフォーカスのイベントの記録 (フォーカスの戻り先の不具合を切り分けるため)。start から
// stop までの間、ウィンドウのアクティブの変化と、フォーカスの出入りを時刻付きで記録する (最後の 24 件)。
// アプリケーション全体のイベントフィルターで受け取り、イベントは止めない。
class FocusTrace : public QObject {
public:
	void start()
	{
		lines_.clear();
		clock_.start();
		qApp->installEventFilter(this);
	}
	void stop() { qApp->removeEventFilter(this); }
	QString text() const { return lines_.join(QStringLiteral(" | ")); }
	void mark(const QString &what) { push(QStringLiteral("%1ms %2").arg(clock_.elapsed()).arg(what)); }

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		const QEvent::Type t = event->type();
		const char *name = t == QEvent::WindowActivate     ? "Activate"
				   : t == QEvent::WindowDeactivate ? "Deactivate"
				   : t == QEvent::FocusIn          ? "FocusIn"
				   : t == QEvent::FocusOut         ? "FocusOut"
								   : nullptr;
		// アクティブのイベントは、ウィンドウごとに 1 件だけ記録する (子のウィジェットにも同じイベントが
		// 送られるので、ウィンドウのものだけ)。
		const bool activation = t == QEvent::WindowActivate || t == QEvent::WindowDeactivate;
		if (name != nullptr && obj->isWidgetType() && (!activation || static_cast<QWidget *>(obj)->isWindow()))
			mark(QString::fromLatin1(name) + QLatin1Char(' ') + focus_name(static_cast<QWidget *>(obj)));
		return false;
	}

private:
	void push(const QString &line)
	{
		lines_ << line;
		while (lines_.size() > 24)
			lines_.removeFirst();
	}
	QStringList lines_;
	QElapsedTimer clock_;
};

// 製品の DockPoll がウィンドウを開く処理の代わり。DockPoll の側の処理 (dock/dock_poll_settings.cpp の
// open_settings、open_participants、apply_from_window、refresh_windows と、dock/dock_poll_export.cpp の
// begin -> on_prepared -> 保存のダイアログ -> on_written) は libobs に依存するので組み込めない。同じ順と同じ引数で、
// 次のことを行う: ウィンドウの入力欄に入れる値、親ウィンドウをメインウィンドウにすること、WA_DeleteOnClose、
// 開いていれば前面に出してページを替えること、開いた後の start_export、保存できたかを ops の settings_unsaved に
// 設定して、すぐ周期を 1 回実行すること、エクスポートの終わりに set_export_busy(false) と ops の export_result を
// 設定して、すぐ周期を 1 回実行すること、ウィンドウを閉じたら export_result を消すこと。
// 保存のダイアログ (QFileDialog::getSaveFileName) は、設定ウィンドウを親にしたモーダルの QDialog で代える
// (ファイルを選ばず、テストが閉じる)。実際の QFileDialog は使わない。プレビュー用のプラットフォームプラグインでは
// Qt のウィジェットのダイアログになってフォルダーを読みに行き、Windows の実際の保存のダイアログとも違うためである。
// エクスポートの失敗とアンインストールのダイアログは、DockPoll の側の関数 (dock/dock_poll_export.cpp の
// show_export_failed と dock/dock_poll_install.cpp の uninstall) と同じ親ウィンドウと同じ spec で表示する
// (失敗のダイアログの親と止め方の選び方、確認の答えの番号、結果のダイアログの親を同じにする)。
// アンインストールそのものは実行しない (結果のダイアログは、テストデータ dialog-uninstall-done の View から作る)。
struct Host {
	SimPoll *sim = nullptr;
	QPointer<dock::SettingsWindow> settings;
	QPointer<dock::ParticipantsWindow> participants;
	config::Settings effective_at_start;
	bool fail_save = false;
	int apply_calls = 0;
	config::Settings last_applied;

	// エクスポート
	int export_calls = 0;
	QPointer<QWidget> export_owner;
	bool window_visible_at_export = false;
	SettingsPage page_at_export = SettingsPage::Recording;
	bool save_accept = true;
	// 保存のダイアログが開いている間に見たもの (保存のダイアログは、Host が 300 ms 後に自分で閉じる。
	// 利用者が選んだか、やめたのと同じ)。
	bool save_owner_ok = false;
	bool save_busy_look = false;
	int save_dialogs = 0;
	// エクスポートを始めた時点の、設定ウィンドウのフォーカスのあるウィジェット (objectName/class)。
	// SettingsWindow::start_export は、ボタンを押せなくした後のウィンドウのフォーカスのあるウィジェットを保存し
	// (export_focus_mark_)、終わったときに同じウィジェットなら、ページのボタンへフォーカスを戻す。
	// on_export はその直後に呼ばれるので、ここで見たウィジェットは、保存したウィジェットと同じになる
	// (フォーカスの戻り先の切り分けに使う)。
	QString focus_at_export;
	bool window_active_at_export = false;
	// 保存のダイアログを閉じた直後の、set_export_busy(false) の前と後のウィンドウのフォーカスのあるウィジェットと、
	// ウィンドウがアクティブか (同じ切り分けに使う)。
	QString focus_before_busy_off;
	QString focus_after_busy_off;
	QString focus_after_tick; // その後の周期 (結果のテキストをウィンドウへ渡す) の後
	int prepared_done = 0;    // on_prepared を終えた数 (保存のダイアログを閉じて周期を実行した後。待ち合わせ用)
	FocusTrace *trace = nullptr; // 設定されていれば、on_prepared の区切りを記録する
	bool window_active_after_save = false;
	// 値があれば、エクスポートは保存のダイアログの前で失敗する (dock/dock_poll_export.cpp の on_prepared の
	// 失敗の処理)。
	std::optional<bundle::ExportOutcome> export_fail;
	// アンインストール (設定ウィンドウの [アンインストール]): 確認の答えと、結果のダイアログを表示した数。
	int uninstall_confirm_choice = -2;
	int uninstall_results = 0;
	// true なら、次のアンインストールは、確認のダイアログの前の取り直しで断る (周期の間に配信が始まった場合)。
	// dock/dock_poll_install.cpp の uninstall で available_now が偽のときの処理: 取り直した事実を
	// Observation に設定し、ダイアログを表示せずに、0 ms 後に周期を 1 回実行する。断った数を数える。
	bool streaming_starts_before_confirm = false;
	int uninstall_refused = 0;

	void open_settings(SettingsPage page, bool start_export)
	{
		if (settings.isNull()) {
			dock::SettingsWindowInput in;
			in.current = sim->settings();
			in.effective_at_start = effective_at_start;
			in.page = page;
			in.about = sim->view().about;
			in.record_dir_now = [](const std::string &setting) {
				return setting.empty() ? std::string("D:\\Videos\\cordmux") : setting;
			};
			in.on_apply = [this](const config::Settings &s) { return apply_from_window(s); };
			in.on_export = [this](QWidget *owner) { begin_export(owner); };
			in.on_uninstall = [this](QWidget *owner) { uninstall(owner); };
			in.on_closed = [this](const std::string &) {
				sim->obs().ops.export_result.reset();
				sim->changed();
				sim->tick();
			};
			settings = dock::make_settings_window(main_window(), in);
			settings->setAttribute(Qt::WA_DeleteOnClose, true);
			// ウィンドウは、ドックに重ならない左の方へ置く (利用者がウィンドウを動かしたのと同じ。
			// 製品は親ウィンドウの中央に表示する)。
			settings->move(80, 60);
		} else {
			settings->set_page(page);
		}
		settings->show();
		settings->raise();
		Input::activate_window(settings);
		if (start_export)
			settings->start_export();
	}

	// OBS の [ツール] の「cordmuxをアンインストール...」。dock/dock_poll_settings.cpp の open_settings_to_uninstall
	// と同じ順で、設定ウィンドウを「バージョン情報」で開き (open_settings と同じ処理)、次の周回
	// (0 ms) で SettingsWindow::start_uninstall を呼ぶ。待つ間にウィンドウが閉じられたら何もしない。
	// [ツール] の項目そのもの (plugin-main.cpp の obs_frontend_add_tools_menu_item) は libobs に依存するので
	// 組み込めず、実機テストで確認する。
	void open_settings_to_uninstall()
	{
		open_settings(SettingsPage::About, false);
		const QPointer<dock::SettingsWindow> window = settings;
		if (window.isNull())
			return;
		QTimer::singleShot(0, window.data(), [window]() {
			if (!window.isNull())
				window->start_uninstall();
		});
	}

	void open_participants()
	{
		if (participants.isNull()) {
			participants = dock::make_participants_window(
				main_window(), sim->view().participants, [this](ledger::ActionId id, const ledger::ActionArgs &args) {
					sim->run_action(id, args);
					sim->tick();
				});
			participants->setAttribute(Qt::WA_DeleteOnClose, true);
			participants->move(80, 560);
		}
		participants->show();
		participants->raise();
		Input::activate_window(participants);
	}

	bool apply_from_window(const config::Settings &s)
	{
		++apply_calls;
		last_applied = s;
		// アンインストールの後は、ウィンドウの値を何も適用しない (dock/dock_poll_settings.cpp の
		// apply_from_window の writes_stopped のガード。on_settings_changed も同じときに捨てる)。
		// 保存の失敗とは扱わない (true を返す)。
		if (sim->obs().uninstall.phase != ledger::UninstallPhase::NotStarted)
			return true;
		// ウィンドウが入力欄を持つ項目だけをコピーする (dock/dock_poll_settings.cpp の on_settings_changed と同じ関数)。
		// ドックの表示タブの値とメインアカウントは、ウィンドウを開いた後に変わっていても上書きしない。
		dock::apply_settings_page_fields(s, sim->settings());
		// 保存できたかを ops に設定し (write_settings)、すぐ周期を 1 回実行する (apply_from_window)。
		sim->obs().ops.settings_unsaved = fail_save;
		sim->changed();
		sim->tick();
		return !fail_save;
	}

	void begin_export(QWidget *owner)
	{
		++export_calls;
		export_owner = owner;
		window_visible_at_export = owner != nullptr && owner->isVisible();
		window_active_at_export = owner != nullptr && owner->isActiveWindow();
		focus_at_export = focus_name(owner != nullptr ? owner->focusWidget() : nullptr);
		if (auto *w = dynamic_cast<dock::SettingsWindow *>(owner))
			page_at_export = w->page();
		// 製品は診断情報の準備をワーカースレッドで行い、結果を QueuedConnection で受けてから保存のダイアログを
		// 表示する (dock/dock_poll_export.cpp の export_bundle_now -> on_prepared)。押した操作の呼び出し
		// (ドックのメニューの exec、ボタンの clicked) から一度戻った後の周回で、保存のダイアログが開く。
		// Host も同じく次の周回へ送る。押した操作の中で保存のダイアログを開くと、製品と違う順になり、ドックの
		// メニューを閉じる後処理 (前のウィンドウへアクティブを戻す) が保存のダイアログを閉じた後に来て、設定ウィンドウが
		// アクティブでなくなり、フォーカスのあるウィジェットが変わる。時間を置いて開く方法もとらない。製品の準備に
		// かかる時間は中身で変わり、決まった値に意味がないので、順だけを同じにする。
		const QPointer<QWidget> started(owner);
		QTimer::singleShot(0, sim->widget(), [this, started] { on_prepared(started.data()); });
	}

	// dock/dock_poll_export.cpp の on_prepared と同じ処理 (親ウィンドウは始めたウィンドウ。削除されていればドック。
	// dialog_owner と同じ)。
	void on_prepared(QWidget *started_window)
	{
		if (export_fail.has_value()) {
			// dock/dock_poll_export.cpp の on_prepared の失敗の処理と show_export_failed: 処理中を戻し、
			// 始めた設定ウィンドウがまだあれば、止めるのはそのウィンドウだけにする (OwnerOnly)。
			// なければドックを親にする。
			const QPointer<QWidget> started(started_window);
			if (auto *w = dynamic_cast<dock::SettingsWindow *>(started_window))
				w->set_export_busy(false);
			dock::ui::MessageSpec spec = dock::export_failed_spec(*export_fail);
			if (!started.isNull())
				spec.blocking = dock::ui::MessageBlocking::OwnerOnly;
			dock::ui::run_message_dialog(!started.isNull() ? started.data() : static_cast<QWidget *>(sim->widget()), spec);
			++prepared_done;
			return;
		}
		QWidget *owner = started_window != nullptr ? started_window : static_cast<QWidget *>(sim->widget());
		// 保存のダイアログ (モーダル。親は設定ウィンドウ。dock/dock_poll_export.cpp の on_prepared と同じ)。
		// 閉じるのはテストの次の手順。
		QDialog dialog(owner);
		dialog.setObjectName(QStringLiteral("driveSaveDialog"));
		dialog.setWindowTitle(QStringLiteral("save"));
		dialog.resize(400, 200);
		++save_dialogs;
		QTimer::singleShot(300, &dialog, [this, &dialog, owner] {
			save_owner_ok = dialog.parentWidget() == owner;
			QPushButton *b = nullptr;
			const QString busy = dock::ui::resolve_key(keys::kSettingsExportDiagnosticsBusyButton);
			for (QPushButton *p : owner->findChildren<QPushButton *>()) {
				if (p->text() == busy)
					b = p;
			}
			save_busy_look = b != nullptr && !b->isEnabled();
			if (save_accept)
				dialog.accept();
			else
				dialog.reject();
		});
		if (trace != nullptr)
			trace->mark(QStringLiteral("保存のダイアログを開く"));
		const int r = dialog.exec();
		if (trace != nullptr)
			trace->mark(QStringLiteral("保存のダイアログが閉じた"));
		auto focus_id = [owner]() { return focus_name(owner != nullptr ? owner->focusWidget() : nullptr); };
		focus_before_busy_off = focus_id();
		if (auto *w = dynamic_cast<dock::SettingsWindow *>(owner))
			w->set_export_busy(false);
		focus_after_busy_off = focus_id();
		if (trace != nullptr)
			trace->mark(QStringLiteral("処理中を戻した (ウィンドウのフォーカス ") + focus_after_busy_off + QLatin1Char(')'));
		window_active_after_save = owner != nullptr && owner->isActiveWindow();
		if (r == QDialog::Accepted && save_accept) {
			ledger::ExportResultFact fact;
			fact.kind = ledger::ExportResultKind::Written;
			fact.path = "D:\\Exports\\cordmux-drive-test.zip";
			sim->obs().ops.export_result = fact;
			sim->changed();
		}
		sim->tick();
		focus_after_tick = focus_id();
		if (trace != nullptr)
			trace->mark(QStringLiteral("on_prepared の終わり"));
		++prepared_done;
	}

	void uninstall(QWidget *owner)
	{
		// dock/dock_poll_install.cpp の uninstall のダイアログの処理: 確認のダイアログの親は押したウィンドウ、
		// 閉じた後のフォーカスの移し先は「バージョン情報」の最初のウィジェット (dock/dock_poll_settings.cpp)。
		// 答えが 0 ([アンインストール]) ならアンインストールし (ここではしない)、結果のダイアログを同じ親
		// (削除されていればドック) で表示する。止め方は spec が持つ (dock/install_dialogs.cpp の spec_of)。
		const QPointer<QWidget> owner_ptr(owner);
		auto *settings_window = dynamic_cast<dock::SettingsWindow *>(owner);
		if (streaming_starts_before_confirm) {
			// 確認のダイアログの前の取り直し (dock/dock_poll_install.cpp の available_now): 配信が
			// 始まっていたので、ダイアログを表示しない。Observation の uninstall と obs_outputs の両方に
			// 設定し、押されたシグナルの中では描画し直さず、0 ms 後に周期を 1 回実行する
			// (同じファイルのコメント)。
			streaming_starts_before_confirm = false;
			sim->obs().uninstall.obs_streaming = true;
			sim->obs().obs_outputs.streaming = true;
			sim->changed();
			++uninstall_refused;
			sim->tick_soon();
			return;
		}
		dock::ui::MessageSpec confirm = dock::uninstall_confirm_spec();
		confirm.focus_fallback = settings_window != nullptr ? settings_window->about_focus_fallback() : nullptr;
		uninstall_confirm_choice = dock::ui::run_message_dialog(owner, confirm);
		if (uninstall_confirm_choice != 0)
			return;
		// アンインストールを終えた (ここではしない)。製品と同じく、段階を Done にして周期を 1 回実行してから、
		// 結果のダイアログを表示する (設定ウィンドウの [アンインストール] は押せなくなり、
		// 閉じた後のフォーカスは focus_fallback へ移る)。
		sim->obs().uninstall.phase = ledger::UninstallPhase::Done;
		sim->changed();
		sim->tick();
		const ledger::dockview::DockViewV2 done = ui_preview::view_for(ui_preview::fixture_by_id("dialog-uninstall-done"));
		if (!done.uninstall_result.has_value())
			return;
		dock::ui::MessageSpec result = dock::uninstall_result_spec(*done.uninstall_result);
		result.focus_fallback = confirm.focus_fallback;
		// 製品は platform::open_file_location を設定する (dock/dock_poll_install.cpp)。ここでは何もしない
		// 関数にする (ボタンは製品と同じに表示する)。
		result.reveal_file = [](const QString &) {};
		++uninstall_results;
		dock::ui::run_message_dialog(!owner_ptr.isNull() ? owner_ptr.data() : static_cast<QWidget *>(sim->widget()), result);
	}

	void refresh_windows(const ledger::dockview::DockViewV2 &view)
	{
		if (!settings.isNull() && settings->isVisible()) {
			settings->update_about(view.about);
		}
		if (!participants.isNull() && participants->isVisible())
			participants->update_rows(view.participants);
	}
};

QDialogButtonBox *button_box(QWidget *window)
{
	return window != nullptr ? window->findChild<QDialogButtonBox *>() : nullptr;
}

QPushButton *box_button(QWidget *window, QDialogButtonBox::StandardButton which)
{
	QDialogButtonBox *box = button_box(window);
	return box != nullptr ? box->button(which) : nullptr;
}

QPushButton *export_button(QWidget *window)
{
	const QString normal = dock::ui::resolve_key(keys::kSettingsExportDiagnosticsButton);
	const QString busy = dock::ui::resolve_key(keys::kSettingsExportDiagnosticsBusyButton);
	if (window == nullptr)
		return nullptr;
	for (QPushButton *b : window->findChildren<QPushButton *>()) {
		if (b->text() == normal || b->text() == busy)
			return b;
	}
	return nullptr;
}

// ウィンドウの中の、ページ (cordmuxSettingsPage_<id>) の中の再起動の 1 文。
QLabel *restart_note(QWidget *window, const QString &page_id)
{
	if (window == nullptr)
		return nullptr;
	QWidget *page = window->findChild<QWidget *>(QStringLiteral("cordmuxSettingsPage_") + page_id);
	return page != nullptr ? page->findChild<QLabel *>(QStringLiteral("cordmuxRestartNote")) : nullptr;
}

// 参加者の設定ウィンドウの、その人の行のチェックボックス。name は列のチェックボックスの objectName
// (「自動で載せる」は cordmuxParticipantAutoAdd、「通話のみ時ラベル非表示」は cordmuxParticipantRosterHide)。
QCheckBox *participant_check(QWidget *window, const std::string &user_id, const QString &name)
{
	if (window == nullptr)
		return nullptr;
	for (QCheckBox *c : window->findChildren<QCheckBox *>(name)) {
		if (c->property("userId").toString().toStdString() == user_id)
			return c;
	}
	return nullptr;
}

QCheckBox *participant_auto_add(QWidget *window, const std::string &user_id)
{
	return participant_check(window, user_id, QStringLiteral("cordmuxParticipantAutoAdd"));
}

QCheckBox *participant_roster_hide(QWidget *window, const std::string &user_id)
{
	return participant_check(window, user_id, QStringLiteral("cordmuxParticipantRosterHide"));
}

bool seen_always_on(const ledger::Observation &obs, const std::string &user_id)
{
	for (const ledger::SeenPerson &p : obs.seen.people) {
		if (p.user_id == user_id)
			return p.always_on;
	}
	return false;
}

// その人を、画面共有していない参加者の並びに出すか (seen_people.json の行がなければ出す)。
bool seen_roster_shown(const ledger::Observation &obs, const std::string &user_id)
{
	for (const ledger::SeenPerson &p : obs.seen.people) {
		if (p.user_id == user_id)
			return !p.roster_hidden;
	}
	return true;
}

bool window_blocked(QWidget *w)
{
	if (w == nullptr || w->windowHandle() == nullptr)
		return false;
	return QGuiApplicationPrivate::instance()->isWindowBlocked(w->windowHandle());
}

// 表示されている cordmux の確認、結果、失敗のダイアログ (cordmuxMessageDialog) のうち、親が owner のもの
// (owner が nullptr なら最初のもの)。OwnerOnly の失敗のダイアログはモーダルでない (activeModalWidget に表れない) ので、
// ウィンドウの一覧から探す。
QWidget *visible_message_dialog(QWidget *owner = nullptr)
{
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (w->isVisible() && w->objectName() == QLatin1String("cordmuxMessageDialog") &&
		    (owner == nullptr || w->parentWidget() == owner))
			return w;
	}
	return nullptr;
}

QString describe_focus()
{
	QWidget *f = QApplication::focusWidget();
	return f != nullptr ? f->objectName() + QLatin1Char('/') + QString::fromLatin1(f->metaObject()->className())
			    : QStringLiteral("(none)");
}

bool has_notice(const ledger::dockview::DockViewV2 &view, const char *rule)
{
	for (const ledger::dockview::Notice &n : view.notices) {
		if (n.rule_name == rule)
			return true;
	}
	return false;
}

void click_widget(Input &input, QWidget *w)
{
	if (w == nullptr)
		return;
	const QPoint at = w->mapToGlobal(w->rect().center());
	// 押す相手のウィンドウがほかのウィンドウの後ろなら、先に前面に出す (利用者が、見えている所を押してウィンドウを
	// 前面に出すのと同じ)。
	if (Input::window_at(at) != w->window()->windowHandle() && QApplication::activePopupWidget() == nullptr &&
	    QApplication::activeModalWidget() == nullptr)
		Input::activate_window(w);
	input.move_to(at);
	input.click();
}

QString name_of_widget(QWidget *w)
{
	if (w == nullptr)
		return QStringLiteral("(none)");
	if (auto *b = qobject_cast<QAbstractButton *>(w))
		return QStringLiteral("button:") + b->text();
	return w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName();
}

// 設定ウィンドウを親にした、操作を始めた範囲だけを止めるダイアログ (OwnerOnly) が開いた直後から閉じるまでの確認。
// 設定の保存の失敗、設定ウィンドウから始めたエクスポートの失敗、アンインストールの確認と結果、設定ウィンドウの色の
// ダイアログで、同じ確認を行う。呼び出し元は、この前にダイアログを開く手順 (then_blocking) を置く。
//   find_dialog: 開いているダイアログ。focus_after: 閉じた後にフォーカスが戻るはずのウィジェット。
//   message: cordmux のダイアログか (止め方のプロパティを見る)。
// 確認すること: ダイアログが表示され、親が設定ウィンドウ。モーダルでない (Qt のモーダルを使わない)。
// 設定ウィンドウを灰色にしない。Qt がメインウィンドウとドックを止めていない。メインウィンドウを押すと動作する。
// ドックの行を押すと動作する。その後もダイアログは開いたまま。設定ウィンドウの [キャンセル] を押しても動作せず、
// ダイアログが前面に出てアクティブになる。設定ウィンドウの [x] で閉じない。Esc で閉じる。閉じた後にフォーカスが戻る。
// 閉じた後は、設定ウィンドウがキーを受ける (Tab でフォーカスが移る)。
struct OwnerOnlyCase {
	QString group;
	QString label;
	std::function<QWidget *()> find_dialog;
	std::function<QWidget *()> focus_after;
	bool message = true;
};

void add_owner_only_steps(Script &s, Results &results, SimPoll &sim, Input &input, Host &host, const OwnerOnlyCase &c)
{
	auto probe = std::make_shared<std::unique_ptr<MainWindowProbe>>();
	auto cancel_covered = std::make_shared<bool>(false);
	s.then(400, [&, c, probe] {
		QWidget *d = c.find_dialog();
		results.expect_true(c.group, c.label + QStringLiteral(": ダイアログが表示され、親が設定ウィンドウ"),
				    d != nullptr && !host.settings.isNull() && d->parentWidget() == host.settings.data(),
				    d != nullptr ? QStringLiteral("親 %1").arg(name_of_widget(d->parentWidget())) : QStringLiteral("(開かない)"));
		if (d == nullptr)
			return;
		results.expect_true(c.group, c.label + QStringLiteral(": モーダルでない (Qt のモーダルを使わない)"),
				    d->windowModality() == Qt::NonModal && QApplication::activeModalWidget() == nullptr &&
					    (!c.message || blocking_name_of(d) == QLatin1String("ownerOnly")),
				    QStringLiteral("windowModality %1、止め方 %2").arg(int(d->windowModality())).arg(blocking_name_of(d)));
		results.expect_true(c.group, c.label + QStringLiteral(": 設定ウィンドウを灰色にしない"),
				    !host.settings.isNull() && host.settings->isEnabled());
		results.expect_true(c.group, c.label + QStringLiteral(": Qt が OBS のメインウィンドウとドックを止めていない"),
				    !qt_window_blocked(main_window()) && !qt_window_blocked(sim.dock()));
		*probe = std::make_unique<MainWindowProbe>();
		results.expect_true(c.group, c.label + QStringLiteral(": 開いている間も、OBS のメインウィンドウを押すと動作する"),
				    (*probe)->click_works(input));
		QString note;
		results.expect_true(c.group, c.label + QStringLiteral(": 開いている間も、ドックの行を押すと動作する"),
				    dock_row_click_works(input, sim, &note), note);
		results.expect_true(c.group, c.label + QStringLiteral(": メインウィンドウとドックを押した後も、ダイアログは開いたまま"), c.find_dialog() == d);
	});
	s.then(300, [&, c, cancel_covered] {
		QPushButton *cancel = box_button(host.settings, QDialogButtonBox::Cancel);
		if (cancel == nullptr)
			return;
		const QPoint at = cancel->mapToGlobal(cancel->rect().center());
		*cancel_covered = Input::window_at(at) != host.settings->windowHandle();
		input.move_to(at);
		pump(600);
		input.click();
	});
	s.then(300, [&, c, cancel_covered] {
		QWidget *d = c.find_dialog();
		results.expect_true(c.group, c.label + QStringLiteral(": 開いている間、設定ウィンドウの [キャンセル] を押しても動作しない"),
				    !host.settings.isNull() && host.settings->isVisible() && d != nullptr,
				    *cancel_covered ? QStringLiteral("前提が満たされていない: [キャンセル] がダイアログに覆われていた") : QString());
		results.expect_true(c.group, c.label + QStringLiteral(": 設定ウィンドウを押すと、ダイアログが前面に出てアクティブになる"),
				    d != nullptr && QApplication::activeWindow() == d,
				    QStringLiteral("アクティブなウィンドウ %1").arg(name_of_widget(QApplication::activeWindow())));
		if (host.settings.isNull())
			return;
		QWindowSystemInterface::handleCloseEvent<QWindowSystemInterface::SynchronousDelivery>(host.settings->windowHandle());
		pump(300);
		results.expect_true(c.group, c.label + QStringLiteral(": 開いている間、設定ウィンドウの [x] で設定ウィンドウが閉じない"),
				    !host.settings.isNull() && host.settings->isVisible() && c.find_dialog() != nullptr);
		if (host.settings.isNull() || !host.settings->isVisible())
			host.open_settings(SettingsPage::About, false);
	});
	s.then(300, [&, c] {
		if (QWidget *d = c.find_dialog()) {
			Input::activate_window(d);
			pump(100);
		}
		input.key(Qt::Key_Escape);
	});
	s.then(300, [&, c, probe] {
		results.expect_true(c.group, c.label + QStringLiteral(": Esc で閉じる"), c.find_dialog() == nullptr);
		QWidget *expect = c.focus_after ? c.focus_after() : nullptr;
		results.expect_true(c.group, c.label + QStringLiteral(": 閉じた後、フォーカスが開く前のウィジェットへ戻る"),
				    expect != nullptr && QApplication::focusWidget() == expect,
				    QStringLiteral("フォーカス %1 (%2) / 期待 %3")
					    .arg(name_of_widget(QApplication::focusWidget()), describe_focus(), name_of_widget(expect)));
		bool keys_work = false;
		if (!host.settings.isNull()) {
			Input::activate_window(host.settings);
			pump(100);
			QWidget *before = QApplication::focusWidget();
			input.key(Qt::Key_Tab);
			pump(100);
			keys_work = QApplication::focusWidget() != before && QApplication::focusWidget() != nullptr &&
				    QApplication::focusWidget()->window() == host.settings.data();
			input.key(Qt::Key_Backtab);
		}
		results.expect_true(c.group, c.label + QStringLiteral(": 閉じた後は、設定ウィンドウがキーを受ける (Tab でフォーカスが移る)"), keys_work);
		probe->reset();
	});
}

} // namespace

void run_windows(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	Host host;
	host.sim = &sim;
	host.effective_at_start = sim.settings();
	sim.on_tick = [&](const ledger::StabilizeResult &st) { host.refresh_windows(st.view); };
	// ドックのメニューとメッセージの [診断情報をエクスポート...] は、設定ウィンドウを「バージョン情報」で開いてから
	// 始める (DockPoll::run_action の ExportBundle)。ウィンドウを開く操作も、DockPoll と同じ関数へ渡す。
	sim.on_action = [&](const ActionRecord &rec) {
		if (!rec.ran)
			return false;
		switch (rec.kind) {
		case ledger::IntentKind::ExportBundle:
			host.open_settings(SettingsPage::About, true);
			return true;
		case ledger::IntentKind::OpenSettings:
			host.open_settings(rec.args.page, false);
			return true;
		case ledger::IntentKind::OpenParticipants:
			host.open_participants();
			return true;
		default:
			return false;
		}
	};
	sim.start();
	Script s;

	// ---- (4) 再起動が必要な設定を変えた時点で文が表示され、戻すと消える ----
	s.then(600, [&] { host.open_settings(SettingsPage::Advanced, false); });
	auto restart_before = std::make_shared<bool>(false);
	s.then(300, [&, restart_before] {
		QLabel *note = restart_note(host.settings, QStringLiteral("advanced"));
		*restart_before = note != nullptr && note->isVisible();
		auto *spin = host.settings != nullptr
				     ? host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_screen_audio_offset_ms"))
				     : nullptr;
		results.expect_true(kGroup, QStringLiteral("(4) 再起動が必要な設定 (画面の音の補正) と文が見つかる"), note != nullptr && spin != nullptr);
		results.expect_true(kGroup, QStringLiteral("(4) 声の遅延の欄は設定ウィンドウにない"),
				    host.settings != nullptr &&
					    host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_voice_audio_delay_ms")) == nullptr);
		if (spin != nullptr) {
			spin->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Up);
		}
	});
	s.then(200, [&, restart_before] {
		QLabel *note = restart_note(host.settings, QStringLiteral("advanced"));
		results.expect_true(kGroup, QStringLiteral("(4) 変えた時点で再起動の文が表示される"),
				    !*restart_before && note != nullptr && note->isVisible());
		input.key(Qt::Key_Down);
	});
	s.then(200, [&] {
		QLabel *note = restart_note(host.settings, QStringLiteral("advanced"));
		results.expect_true(kGroup, QStringLiteral("(4) 元の値に戻すと再起動の文が消える"),
				    note != nullptr && !note->isVisible());
		// 再起動が必要でない設定 (同じページの [配信中の退出を確認]) では、文が表示されない。
		auto *check = host.settings->findChild<QCheckBox *>(QStringLiteral("cordmuxField_confirm_while_streaming"));
		results.expect_true(kGroup, QStringLiteral("(4) 同じページの再起動が必要でない設定 ([配信中の退出を確認]) が見つかる"),
				    check != nullptr && !check->property("restartRequired").toBool());
		if (check != nullptr) {
			check->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Space);
		}
	});
	s.then(200, [&] {
		QLabel *note = restart_note(host.settings, QStringLiteral("advanced"));
		auto *check = host.settings->findChild<QCheckBox *>(QStringLiteral("cordmuxField_confirm_while_streaming"));
		results.expect_true(kGroup, QStringLiteral("(4) 再起動が必要でない設定では、文が表示されない"),
				    note != nullptr && !note->isVisible() && check != nullptr &&
					    check->isChecked() != sim.settings().confirm_while_streaming);
		input.key(Qt::Key_Space);
	});
	// 数の欄の上でマウスのホイールを回しても、フォーカスがなければ値は変わらない (ページのスクロールに渡る)。
	s.then(200, [&] {
		auto *spin = host.settings != nullptr
				     ? host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_screen_audio_offset_ms"))
				     : nullptr;
		if (spin == nullptr) {
			results.expect_true(kGroup, QStringLiteral("(4) フォーカスのない数の欄の上のホイールで値が変わらない"), false);
			return;
		}
		spin->clearFocus();
		const int before = spin->value();
		const QPoint at = spin->mapToGlobal(spin->rect().center());
		input.move_to(at);
		const bool under_pointer = !spin->hasFocus() && spin->visibleRegion().contains(spin->rect().center());
		input.wheel(120);
		input.wheel(-120);
		input.wheel(-120);
		results.expect_true(kGroup, QStringLiteral("(4) フォーカスのない数の欄の上のホイールで値が変わらない"),
				    under_pointer && spin->value() == before);
		click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel));
	});

	// ---- (2) [キャンセル] で、ウィンドウの中で変えて反映していない値だけが戻る ----
	auto gap0 = std::make_shared<int>(0);
	s.then(300, [&, gap0] {
		*gap0 = sim.settings().record_space_warn_minutes;
		host.apply_calls = 0;
		host.open_settings(SettingsPage::Recording, false);
	});
	s.then(300, [&] {
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		if (spin != nullptr) {
			spin->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Up);
		}
	});
	s.then(150, [&] { click_widget(input, box_button(host.settings, QDialogButtonBox::Apply)); });
	s.then(200, [&, gap0] {
		results.expect_true(kGroup, QStringLiteral("(2) [適用] でウィンドウの値が渡る"),
				    host.apply_calls == 1 && host.last_applied.record_space_warn_minutes == *gap0 + 1,
				    QStringLiteral("calls=%1 gap=%2 (前 %3)").arg(host.apply_calls).arg(host.last_applied.record_space_warn_minutes).arg(*gap0));
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		if (spin != nullptr) {
			spin->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Up);
		}
	});
	s.then(150, [&] { click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel)); });
	s.then(300, [&, gap0] {
		results.expect_true(kGroup, QStringLiteral("(2) [キャンセル] でウィンドウが閉じる"), host.settings.isNull() || !host.settings->isVisible());
		results.expect_true(kGroup, QStringLiteral("(2) [キャンセル] は反映していない値だけを捨てる ([適用] した値は残る)"),
				    host.apply_calls == 1 && sim.settings().record_space_warn_minutes == *gap0 + 1,
				    QStringLiteral("calls=%1 gap=%2").arg(host.apply_calls).arg(sim.settings().record_space_warn_minutes));
		host.open_settings(SettingsPage::Recording, false);
	});
	s.then(300, [&, gap0] {
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		results.expect_true(kGroup, QStringLiteral("(2) 開き直したウィンドウは、[適用] した値を表示する"),
				    spin != nullptr && spin->value() == *gap0 + 1);
	});

	// ---- (R) [映像の受信] のページには、受信の表と映像のバッファがない (状態タブにある。scen_status.cpp) ----
	s.then(200, [&] {
		host.settings->close();
	});
	s.then(300, [&] {
		pipeline::ArrivalWindow w;
		w.delays_ms = {20, 150, 160, 30, 400, 10};
		w.mean_ms = 128.0;
		w.max_ms = 400.0;
		sim.obs().choices.composite_present = true;
		sim.arrival() = {ledger::ArrivalFact{"arrival-user-1", {w}}};
		sim.settings().display_buffer_ms = 100;
		host.open_settings(SettingsPage::Receive, false);
	});
	s.then(300, [&] {
		sim.changed();
		sim.tick();
		results.expect_true(kGroup, QStringLiteral("(R) [映像の受信] のページに、映像のバッファの入力欄がない"),
				    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxField_display_buffer_ms")) == nullptr);
		results.expect_true(kGroup, QStringLiteral("(R) [映像の受信] のページに、受信の表がない"),
				    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxArrival")) == nullptr &&
					    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxArrivalTable")) == nullptr &&
					    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxArrivalNoComposite")) == nullptr);
		results.expect_true(kGroup, QStringLiteral("(R) 残る入力欄は 最大解像度、最大フレームレート、受信する人、映像エンコーダ、チューニング の 5 つ"),
				    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxField_limit_max_long_edge")) != nullptr &&
					    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxField_limit_fps_divisor")) != nullptr &&
					    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxField_receive_scope")) != nullptr &&
					    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxField_encoder_preference")) != nullptr &&
					    host.settings->findChild<QWidget *>(QStringLiteral("cordmuxField_encode_priority")) != nullptr);
		sim.arrival().clear();
		sim.obs().choices.composite_present = false;
		sim.changed();
		host.settings->close();
	});

	// ---- (E) [受け取る映像] のエンコードの設定: 欄を変えて [適用] すると、送信側へ送る limits の値が変わる ----
	auto encoder_before = std::make_shared<ws::LimitsMessage>();
	s.then(200, [&, encoder_before] {
		sim.settings().encoder_preference = config::EncoderPreference::Hardware;
		sim.settings().encode_priority = config::EncodePriority::Speed;
		*encoder_before = pipeline::limits_from_settings(sim.settings().limit_max_long_edge, sim.settings().limit_fps_divisor,
								 sim.settings().encoder_preference, sim.settings().encode_priority);
		host.apply_calls = 0;
		host.open_settings(SettingsPage::Receive, false);
	});
	s.then(300, [&, encoder_before] {
		auto *encoder = host.settings->findChild<QComboBox *>(QStringLiteral("cordmuxField_encoder_preference"));
		auto *priority = host.settings->findChild<QComboBox *>(QStringLiteral("cordmuxField_encode_priority"));
		results.expect_true(kGroup, QStringLiteral("(E) 符号化の設定の 2 つの欄が [受け取る映像] のページにあり、現在の値 (ハードウェアと低遅延) を表示する"),
				    encoder != nullptr && priority != nullptr && encoder->count() == 2 && priority->count() == 2 &&
					    encoder->currentIndex() == 0 && priority->currentIndex() == 0 && encoder->isVisibleTo(host.settings));
		results.expect_true(kGroup, QStringLiteral("(E) 既定の設定で送る limits は、符号化のキーを持たない"),
				    !encoder_before->encoder_preference.has_value() && !encoder_before->latency_mode.has_value());
		results.expect_true(kGroup, QStringLiteral("(E) 2 つの欄に説明 (ツールチップ) がある"),
				    encoder != nullptr && priority != nullptr && !encoder->toolTip().isEmpty() && !priority->toolTip().isEmpty());
		if (encoder != nullptr)
			encoder->setCurrentIndex(1);
		if (priority != nullptr)
			priority->setCurrentIndex(1);
		results.expect_true(kGroup, QStringLiteral("(E) 欄を変えただけでは設定は変わらない (適用の前)"),
				    sim.settings().encoder_preference == config::EncoderPreference::Hardware &&
					    sim.settings().encode_priority == config::EncodePriority::Speed);
	});
	s.then(150, [&] { click_widget(input, box_button(host.settings, QDialogButtonBox::Apply)); });
	s.then(200, [&, encoder_before] {
		const config::Settings &applied = host.last_applied;
		const ws::LimitsMessage after =
			pipeline::limits_from_settings(applied.limit_max_long_edge, applied.limit_fps_divisor, applied.encoder_preference,
						       applied.encode_priority);
		results.expect_true(kGroup, QStringLiteral("(E) [適用] で、ソフトウェアと高品質が渡る"),
				    host.apply_calls == 1 && applied.encoder_preference == config::EncoderPreference::Software &&
					    applied.encode_priority == config::EncodePriority::Quality);
		results.expect_true(kGroup, QStringLiteral("(E) 送る limits が変わり、encoder_preference は software、latency_mode は quality になる"),
				    after != *encoder_before && after.encoder_preference == std::optional<std::string>("software") &&
					    after.latency_mode == std::optional<std::string>("quality") && ws::limits_in_range(after));
		host.settings->close();
	});

	// ---- (1) と (5): ウィンドウを開いたまま、行のメニューと参加者の設定ウィンドウで参加者ごとの値を変える ----
	auto person_x = std::make_shared<std::string>();
	auto person_y = std::make_shared<std::string>();
	s.then(200, [&, person_x, person_y] {
		// X は、右クリックで行のメニューを開ける人 (押せるチェックの人。押せないチェックの上の右クリックは
		// probe のシナリオで確認する)。
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (!row.checkable || !row.check_enabled)
				continue;
			if (person_x->empty())
				*person_x = row.user_id;
			else if (person_y->empty())
				*person_y = row.user_id;
		}
		host.open_participants();
	});
	auto x_before = std::make_shared<bool>(false);
	// ドックの行のメニューで、X の「自動で載せる」を切り替える (右クリック -> 項目)。
	s.then(300, [&, person_x, x_before] {
		*x_before = seen_always_on(sim.obs(), *person_x);
		dock::PersonRowWidget *row = sim.people()->row_for(*person_x);
		if (row == nullptr)
			return;
		input.move_to(global_rect(row).center());
	});
	s.then_blocking(700, [&] {
		// クリックを捨てるガードの時間の外で押す (ポインターを動かしてから 0.7 秒)。
		input.press(Qt::RightButton);
		input.release(Qt::RightButton);
	});
	s.then(300, [&] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		QAction *item = nullptr;
		if (menu != nullptr) {
			for (QAction *a : menu->actions()) {
				if (a->objectName() == QLatin1String("cordmuxRowAutoAdd"))
					item = a;
			}
		}
		if (item == nullptr) {
			results.add(kGroup, QStringLiteral("(5) 行のメニューの「自動で載せる」が見つかる"), QStringLiteral("no"), QStringLiteral("yes"),
				    Verdict::Fail);
			input.key(Qt::Key_Escape);
			return;
		}
		input.move_to(menu->mapToGlobal(menu->actionGeometry(item).center()));
		input.click();
	});
	s.then(300, [&, person_x, x_before] {
		const bool now = seen_always_on(sim.obs(), *person_x);
		QCheckBox *box = participant_auto_add(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("(5) ドックの行のメニューで変えた値を、参加者の設定ウィンドウが次の周期で表示する"),
				    now != *x_before && box != nullptr && box->isChecked() == now,
				    QStringLiteral("値 %1 -> %2、ウィンドウ %3").arg(*x_before).arg(now).arg(box != nullptr ? int(box->isChecked()) : -1));
		// 続けて、参加者の設定ウィンドウのチェックで同じ値を戻す。
		click_widget(input, box);
	});
	s.then(300, [&, person_x, x_before] {
		const bool now = seen_always_on(sim.obs(), *person_x);
		QMenu *menu = sim.people()->make_menu_for(*person_x, sim.widget());
		bool menu_checked = !now;
		if (menu != nullptr) {
			for (QAction *a : menu->actions()) {
				if (a->objectName() == QLatin1String("cordmuxRowAutoAdd"))
					menu_checked = a->isChecked();
			}
			delete menu;
		}
		QCheckBox *box = participant_auto_add(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("(5) ウィンドウで続けて変えた後の値を、ウィンドウとドックの行のメニューの両方が表示する"),
				    now == *x_before && menu_checked == now && box != nullptr && box->isChecked() == now,
				    QStringLiteral("値 %1、メニュー %2、ウィンドウ %3").arg(now).arg(menu_checked).arg(box != nullptr ? int(box->isChecked()) : -1));
	});
	// 列「通話のみ時ラベル非表示」: 参加者の設定ウィンドウのチェックで X を並びから外し、もう一度押して戻す。チェックは「並びに出さない」の向きで、既定では付いていない。
	auto x_shown_before = std::make_shared<bool>(true);
	s.then(200, [&, person_x, x_shown_before] {
		*x_shown_before = seen_roster_shown(sim.obs(), *person_x);
		QCheckBox *box = participant_roster_hide(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: チェックが見つかり、既定で付いていなくて、値の逆と一致する"),
				    box != nullptr && box->isEnabled() && box->isChecked() == !*x_shown_before && *x_shown_before,
				    QStringLiteral("ウィンドウ %1、値 %2").arg(box != nullptr ? int(box->isChecked()) : -1).arg(*x_shown_before));
		click_widget(input, box);
	});
	s.then(300, [&, person_x, x_shown_before, x_before] {
		const bool now = seen_roster_shown(sim.obs(), *person_x);
		QCheckBox *box = participant_roster_hide(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: 押すと値が逆になり、次の周期でウィンドウが同じ値を表示する"),
				    now != *x_shown_before && box != nullptr && box->isChecked() == !now,
				    QStringLiteral("値 %1 -> %2、ウィンドウ %3").arg(*x_shown_before).arg(now).arg(box != nullptr ? int(box->isChecked()) : -1));
		// 「自動で載せる」の値は変わらない (別の列のチェック)。
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: 押しても「自動で載せる」は変わらない"),
				    seen_always_on(sim.obs(), *person_x) == *x_before);
		click_widget(input, box);
	});
	s.then(300, [&, person_x, x_shown_before] {
		const bool now = seen_roster_shown(sim.obs(), *person_x);
		QCheckBox *box = participant_roster_hide(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: もう一度押すと元に戻る"),
				    now == *x_shown_before && box != nullptr && box->isChecked() == !now);
	});
	// 列「通話のみ時ラベル非表示」は、設定の「画面共有していない参加者」の [表示] がオフの間は全員が、[Botを表示] がオフの間は Bot の行が、
	// 押せなくなる。チェックは付いた形になり、保存の値は変えず、設定を戻すと押せる。
	s.then(200, [&, person_x, x_shown_before] {
		sim.obs().choices.roster_show = false;
		sim.tick();
	});
	s.then(300, [&, person_x, x_shown_before] {
		QCheckBox *box = participant_roster_hide(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: [表示] がオフの間は、押せず、理由がツールチップに出て、チェックが付いた形になり、保存の値は変わらない"),
				    box != nullptr && !box->isEnabled() && !box->toolTip().isEmpty() && box->isChecked() &&
					    seen_roster_shown(sim.obs(), *person_x) == *x_shown_before);
		sim.obs().choices.roster_show = true;
		sim.tick();
	});
	s.then(300, [&, person_x] {
		QCheckBox *box = participant_roster_hide(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: [表示] を戻すと、また押せる"), box != nullptr && box->isEnabled());
		sim.obs().choices.roster_show_bots = false;
		for (ledger::PersonFact &p : sim.obs().people) {
			if (p.user_id == *person_x)
				p.is_bot = true;
		}
		sim.tick();
	});
	s.then(300, [&, person_x] {
		QCheckBox *box = participant_roster_hide(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: [Botを表示] がオフの間は、Bot の行が押せず、理由がツールチップに出て、チェックが付いた形になる"),
				    box != nullptr && !box->isEnabled() && !box->toolTip().isEmpty() && box->isChecked());
		for (ledger::PersonFact &p : sim.obs().people) {
			if (p.user_id == *person_x)
				p.is_bot = false;
		}
		sim.tick();
	});
	s.then(300, [&, person_x] {
		QCheckBox *box = participant_roster_hide(host.participants, *person_x);
		results.expect_true(kGroup, QStringLiteral("列「通話のみ時ラベル非表示」: [Botを表示] がオフでも、Bot と分かっていない人の行は押せる"),
				    box != nullptr && box->isEnabled());
		sim.obs().choices.roster_show_bots = true;
		sim.tick();
	});
	// (1): 参加者ごとの値 (Y を参加者の設定ウィンドウで変える) の後に、設定ウィンドウで [適用] と
	// [キャンセル] を押しても、参加者ごとの値は変わらない。
	auto y_after = std::make_shared<bool>(false);
	s.then(200, [&, person_y, y_after] {
		click_widget(input, participant_auto_add(host.participants, *person_y));
		*y_after = !seen_always_on(sim.obs(), *person_y); // 押した後にもう一度読む (次の手順)
	});
	s.then(200, [&, person_y, y_after] {
		*y_after = seen_always_on(sim.obs(), *person_y);
		host.open_settings(SettingsPage::Recording, false);
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		if (spin != nullptr) {
			spin->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Up);
		}
		click_widget(input, box_button(host.settings, QDialogButtonBox::Apply));
	});
	s.then(200, [&] { click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel)); });
	s.then(300, [&, person_x, person_y, y_after, x_before] {
		results.expect_true(kGroup, QStringLiteral("(1) 設定ウィンドウの [適用] と [キャンセル] の後も、参加者ごとの値は変わらない"),
				    seen_always_on(sim.obs(), *person_y) == *y_after &&
					    seen_always_on(sim.obs(), *person_x) == *x_before,
				    QStringLiteral("設定ウィンドウが渡すのは config::Settings だけ (参加者ごとの値は seen の側)。DockPoll の apply_from_window は"
						   "組み込めないので、ウィンドウのコールバックの型で確認した"));
	});

	// ---- 参加者の設定ウィンドウの Tab の順 ----
	// ウィンドウの最初のウィジェットから Tab を 40 回押す。人の行のウィジェット (userId を持つウィジェット) が、
	// 読む順 (上の行から、行の中は左から) に来るか、すべての人の行に届くか。
	s.then(300, [&] {
		host.open_participants();
		if (host.participants.isNull())
			return;
		// ウィンドウのフォーカスの順で、最初のウィジェットへ (ウィンドウを開いた直後に Qt がフォーカスを置く
		// ウィジェットと同じ)。
		for (QWidget *w = host.participants->nextInFocusChain(); w != nullptr && w != host.participants.data();
		     w = w->nextInFocusChain()) {
			if ((w->focusPolicy() & Qt::TabFocus) && w->isVisible() && w->isEnabled()) {
				w->setFocus(Qt::TabFocusReason);
				break;
			}
		}
		std::vector<std::pair<std::string, QPoint>> stops;
		std::set<std::string> users_reached;
		for (int i = 0; i < 40; ++i) {
			input.key(Qt::Key_Tab);
			QWidget *f = QApplication::focusWidget();
			const std::string u = user_of(f);
			if (!u.empty() && f != nullptr && f->window() == host.participants.data()) {
				stops.push_back({u, f->mapTo(host.participants.data(), QPoint(0, 0))});
				users_reached.insert(u);
			}
		}
		// 行の順: 参加者ごとのウィジェットの上端の最小を行の位置とし、移った人の順が行の位置の順かを見る。
		// 行の中: 同じ人の続くウィジェットが左から来るかを見る (行の中のウィジェットは高さが違うので、
		// y は参加者ごとにまとめてから比べる)。
		int out_of_order = 0;
		QStringList seq;
		std::map<std::string, int> row_y;
		for (const auto &[u, p] : stops) {
			const auto it = row_y.find(u);
			if (it == row_y.end() || p.y() < it->second)
				row_y[u] = p.y();
		}
		std::vector<std::string> visit;
		for (const auto &[u, p] : stops) {
			if (visit.empty() || visit.back() != u)
				visit.push_back(u);
		}
		for (std::size_t i = 1; i < visit.size(); ++i) {
			const bool wrapped = visit[i] == visit.front() && i > 0; // 1 周して先頭の行に戻った
			if (!wrapped && row_y[visit[i]] < row_y[visit[i - 1]])
				++out_of_order;
		}
		for (std::size_t i = 1; i < stops.size(); ++i) {
			if (stops[i].first == stops[i - 1].first && stops[i].second.x() < stops[i - 1].second.x())
				++out_of_order;
		}
		for (const auto &[u, p] : stops)
			seq << QStringLiteral("%1@%2,%3").arg(QString::fromStdString(u).right(2)).arg(p.x()).arg(p.y());
		std::set<std::string> expected_users;
		for (const ledger::dockview::ParticipantRow &r : sim.view().participants)
			expected_users.insert(r.user_id);
		results.expect_eq(kGroup, QStringLiteral("参加者の設定ウィンドウ: Tab が読む順 (上の行から、行の中は左から) を外れた回数"), out_of_order, 0,
				  seq.mid(0, 24).join(QLatin1Char(' ')));
		results.info(kGroup, QStringLiteral("参加者の設定ウィンドウ: Tab 40 回で届いた人 / 行の人"),
			     QStringLiteral("%1 / %2").arg(users_reached.size()).arg(expected_users.size()));
		host.participants->close();
	});

	// ---- 参加者の設定ウィンドウの並べ替え ----
	// 既定の並び (最終参加日時の新しい順 = ledger の既定の並び) と、見出しを押したときの並びと向きの表示、押した 2 回目で逆の順、
	// 行を操作している間は動かさないこと (フォーカスと入力途中の文字を含む)。見出しは click() で押す (押すときのポインターの動きは
	// 別のシナリオで確認している)。
	using ledger::dockview::ParticipantRow;
	// ウィンドウの中の行を、見た目の縦の位置の順に並べた userId (名前のラベルの位置で並べる)。
	// 並べ直した直後は、グリッドの配置が済むまで子の位置が古いので、位置を読む前に配置を済ませる。
	const auto settle_layout = [](QWidget *window) {
		for (QLayout *l : window->findChildren<QLayout *>())
			l->activate();
		QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
	};
	const auto row_y_of = [settle_layout](QWidget *window, const std::string &user) {
		settle_layout(window);
		for (QWidget *w : window->findChildren<QWidget *>(QStringLiteral("cordmuxParticipantRow"))) {
			if (user_of(w) == user)
				return w->mapTo(window, QPoint(0, 0)).y();
		}
		return -2;
	};
	const auto rows_in_window_order = [settle_layout](QWidget *window) {
		settle_layout(window);
		std::vector<std::pair<int, std::string>> found;
		for (QWidget *w : window->findChildren<QWidget *>(QStringLiteral("cordmuxParticipantRow"))) {
			if (w->isVisibleTo(window))
				found.push_back({w->mapTo(window, QPoint(0, 0)).y(), user_of(w)});
		}
		std::sort(found.begin(), found.end());
		std::vector<std::string> ids;
		for (const auto &entry : found)
			ids.push_back(entry.second);
		return ids;
	};
	// ledger の行から、組 (現在、過去) ごとに名前の自然順 (descending なら逆順) に並べた userId。
	const auto expected_by_name = [](const std::vector<ParticipantRow> &rows, bool ascending) {
		std::vector<ParticipantRow> sorted = rows;
		std::stable_sort(sorted.begin(), sorted.end(), [ascending](const ParticipantRow &a, const ParticipantRow &b) {
			if (a.in_call != b.in_call)
				return a.in_call;
			return ascending ? ledger::natural_less(a.name.literal, b.name.literal)
					 : ledger::natural_less(b.name.literal, a.name.literal);
		});
		std::vector<std::string> ids;
		for (const ParticipantRow &r : sorted)
			ids.push_back(r.user_id);
		return ids;
	};
	auto sort_rows_ok = std::make_shared<bool>(false);
	s.then(300, [&, sort_rows_ok, rows_in_window_order] {
		dock::ParticipantsWindow::reset_remembered_sort();
		if (!host.participants.isNull())
			host.participants->close();
		host.open_participants();
		if (host.participants.isNull())
			return;
		std::vector<std::string> ledger_order;
		for (const ParticipantRow &r : sim.view().participants)
			ledger_order.push_back(r.user_id);
		*sort_rows_ok = sim.view().participants.size() >= 2;
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウの既定の並び: 最終参加日時の新しい順 (ledger の既定の並びと同じ)"),
				    rows_in_window_order(host.participants) == ledger_order &&
					    host.participants->sort_column() == dock::ParticipantsWindow::ColLastSeen &&
					    !host.participants->sort_ascending(),
				    QStringLiteral("%1 行").arg(ledger_order.size()));
	});
	s.then(200, [&, sort_rows_ok, rows_in_window_order, expected_by_name] {
		if (host.participants.isNull() || !*sort_rows_ok)
			return;
		const auto *w = host.participants.data();
		QAbstractButton *name_header = w->column_header(dock::ParticipantsWindow::ColName);
		// 見出しの幅 (列の幅)。並べ替えの三角の分は、並べていない列も含めて空けてあるので、並べ替えを変えても動かない。
		const auto header_widths = [w]() {
			QList<int> widths;
			for (int c = 0; c < dock::ParticipantsWindow::ColumnCount; ++c)
				widths << w->column_header(static_cast<dock::ParticipantsWindow::Column>(c))->width();
			return widths;
		};
		const QList<int> widths_before = header_widths();
		name_header->setFocus(Qt::MouseFocusReason); // 押すと、フォーカスは見出しに移る
		name_header->click();
		QApplication::processEvents();
		const QList<int> widths_after = header_widths();
		const auto list_text = [](const QList<int> &v) {
			QStringList out;
			for (int x : v)
				out << QString::number(x);
			return out.join(QLatin1Char(','));
		};
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウ: 並べ替えを変えても、列の見出しの幅が動かない"),
				    widths_before == widths_after,
				    QStringLiteral("前 %1 後 %2").arg(list_text(widths_before), list_text(widths_after)));
		const auto& rows = sim.view().participants;
		bool asc_ok = rows_in_window_order(host.participants) == expected_by_name(rows, true);
		const bool state_asc = w->sort_column() == dock::ParticipantsWindow::ColName && w->sort_ascending() &&
				       !name_header->accessibleDescription().isEmpty() &&
				       w->column_header(dock::ParticipantsWindow::ColLastSeen)->accessibleDescription().isEmpty();
		const QString asc_text = name_header->accessibleDescription();
		name_header->click();
		const bool desc_ok = rows_in_window_order(host.participants) == expected_by_name(rows, false);
		const bool state_desc = !w->sort_ascending() && name_header->accessibleDescription() != asc_text &&
					!name_header->accessibleDescription().isEmpty();
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウ: [名前] の見出しを押すと名前の順 (組の中)、もう一度押すと逆の順"),
				    asc_ok && desc_ok, QStringLiteral("昇順=%1 降順=%2").arg(asc_ok).arg(desc_ok));
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウ: 並べている列だけが向きをアクセシブルな説明で伝える (昇順と降順で違う)"),
				    state_asc && state_desc, QStringLiteral("昇順=%1 降順=%2").arg(state_asc).arg(state_desc));
	});
	// 行を操作している間は動かさない。cordmux での名前の列で降順に並べ、いちばん下の人の入力欄にフォーカスを置いて文字を入れる。
	// その人の名前を変えた (= 降順でいちばん上に来る) 行を渡しても、フォーカスと入力途中の文字を失わず、位置は動かない。
	// フォーカスが行から外れたら、並べ直す。
	auto hold_user = std::make_shared<std::string>();
	auto hold_y = std::make_shared<int>(-1);
	s.then(300, [&, sort_rows_ok, hold_user, hold_y, rows_in_window_order, row_y_of] {
		if (host.participants.isNull() || !*sort_rows_ok)
			return;
		host.participants->set_sort(dock::ParticipantsWindow::ColRename, false);
		const std::vector<std::string> order = rows_in_window_order(host.participants);
		// 現在の参加者の最後の人 (組をまたがず、いちばん上に動く余地がある人)。
		for (const ParticipantRow &r : sim.view().participants) {
			if (r.in_call)
				*hold_user = r.user_id;
		}
		QLineEdit *edit = nullptr;
		for (QLineEdit *e : host.participants->findChildren<QLineEdit *>()) {
			if (user_of(e) == *hold_user)
				edit = e;
		}
		if (edit == nullptr)
			return;
		Input::activate_window(host.participants);
		edit->setFocus(Qt::TabFocusReason);
		edit->setText(QStringLiteral("zzz-typing"));
		*hold_y = row_y_of(host.participants, *hold_user);
		(void)order;
	});
	s.then(300, [&, sort_rows_ok, hold_user, hold_y, row_y_of] {
		if (host.participants.isNull() || !*sort_rows_ok || hold_user->empty())
			return;
		std::vector<ParticipantRow> changed = sim.view().participants;
		for (ParticipantRow &r : changed) {
			if (r.user_id == *hold_user)
				r.name_override = "zzz-confirmed"; // 並べ替えの列の値が変わり、降順ではいちばん上に動く値
		}
		QLineEdit *edit = nullptr;
		for (QLineEdit *e : host.participants->findChildren<QLineEdit *>()) {
			if (user_of(e) == *hold_user)
				edit = e;
		}
		const bool had_focus = edit != nullptr && edit->hasFocus();
		host.participants->update_rows(changed);
		const int y_now = row_y_of(host.participants, *hold_user);
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウ: 行の入力欄を操作している間は、並びが変わる値が来ても、その行を動かさない"),
				    had_focus && y_now == *hold_y, QStringLiteral("フォーカス=%1 y=%2 -> %3").arg(had_focus).arg(*hold_y).arg(y_now));
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウ: 操作している行の入力途中の文字とフォーカスを失わない"),
				    edit != nullptr && edit->hasFocus() && edit->text() == QStringLiteral("zzz-typing"), QString());
	});
	s.then(300, [&, sort_rows_ok, hold_user, rows_in_window_order] {
		if (host.participants.isNull() || !*sort_rows_ok || hold_user->empty())
			return;
		// フォーカスを見出しに移すと、待たせていた並べ直しをする (cordmux での名前の降順で、いちばん上の組の先頭)。
		host.participants->column_header(dock::ParticipantsWindow::ColName)->setFocus(Qt::TabFocusReason);
		std::string first_current;
		for (const std::string &id : rows_in_window_order(host.participants)) {
			for (const ParticipantRow &r : sim.view().participants) {
				if (r.user_id == id && r.in_call && first_current.empty())
					first_current = id;
			}
		}
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウ: フォーカスが行から外れたら、待たせていた並べ直しをする"),
				    first_current == *hold_user, QStringLiteral("先頭の現在の参加者=%1").arg(QString::fromStdString(first_current)));
		// 閉じて開き直しても、選んだ並びを覚えている。
		host.participants->close();
	});
	s.then(300, [&, sort_rows_ok] {
		if (!*sort_rows_ok)
			return;
		host.open_participants();
		if (host.participants.isNull())
			return;
		results.expect_true(kGroup, QStringLiteral("参加者の設定ウィンドウ: 閉じて開き直しても、選んだ並び (cordmuxでの名前の降順) を覚えている"),
				    host.participants->sort_column() == dock::ParticipantsWindow::ColRename &&
					    !host.participants->sort_ascending(),
				    QString());
		dock::ParticipantsWindow::reset_remembered_sort();
		host.participants->close();
	});

	// ---- (3) と export の (c): ウィンドウのボタンからエクスポートし、保存のダイアログを閉じた後に結果が残る ----
	s.then(300, [&] { host.open_settings(SettingsPage::About, false); });
	auto busy_ok = std::make_shared<int>(-1);
	s.then_blocking(300, [&, busy_ok] {
		QPushButton *b = export_button(host.settings);
		host.export_calls = 0;
		host.prepared_done = 0;
		host.save_accept = true;
		if (b == nullptr)
			return;
		b->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Shift); // 最後の入力はキーボードのまま (フォーカスの戻り先を見るため)
		input.move_to(b->mapToGlobal(b->rect().center()));
		input.press();
		input.release(); // 次の周回で保存のダイアログが開く (製品の on_prepared と同じ順。Host::begin_export)
	});
	// 保存のダイアログが閉じて on_prepared が終わるまで待ってから見る (負荷が高いと、保存のダイアログが
	// 開くのが遅れる)。
	s.then_when(300, [&] { return host.prepared_done > 0; }, 8000, [&, busy_ok](bool done) {
		results.expect_true(kGroupM10, QStringLiteral("(c) 保存のダイアログが閉じ、エクスポートの終わりまで進んだ"), done);
		// 保存のダイアログが開いていた間の、親と、押したボタンの処理中の見た目。
		results.expect_true(kGroupM10, QStringLiteral("(c) ウィンドウのボタンから: 保存のダイアログの親が設定ウィンドウ"),
				    host.save_dialogs == 1 && host.save_owner_ok);
		results.expect_true(kGroupM10, QStringLiteral("(c) 保存のダイアログが開いている間、ボタンが処理中のテキストで押せない"),
				    host.save_busy_look);

		const auto &er = sim.view().about.export_result;
		const QString expected = er.has_value() ? dock::ui::resolve(er->text) : QString();
		bool shown = false;
		for (QLabel *l : host.settings->findChildren<QLabel *>()) {
			if (l->isVisible() && !expected.isEmpty() && l->text() == expected)
				shown = true;
		}
		results.expect_true(kGroupM10, QStringLiteral("(c) 書けたら、結果のテキストがボタンの下に表示される"), shown);
		QPushButton *b = export_button(host.settings);
		results.expect_true(kGroupM10, QStringLiteral("(c) 最後にフォーカスがページのボタンに戻る"), QApplication::focusWidget() == b,
				    QStringLiteral("フォーカス %1").arg(QApplication::focusWidget() != nullptr
								     ? QString::fromLatin1(QApplication::focusWidget()->metaObject()->className()) +
									       QLatin1Char('/') + QApplication::focusWidget()->objectName()
								     : QStringLiteral("(none)")));
		// (3): [キャンセル] の前に行ったエクスポートは残る (ウィンドウを閉じても、書いたファイルは戻さない)。
		click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel));
	});
	s.then(300, [&] {
		results.expect_true(kGroup, QStringLiteral("(3) [キャンセル] の前に行ったエクスポートは戻さない (ウィンドウを閉じるだけ)"),
				    host.export_calls == 1 && (host.settings.isNull() || !host.settings->isVisible()),
				    QStringLiteral("エクスポートはウィンドウの中の値ではない。結果のテキストはウィンドウを閉じるまで表示する"));
		results.expect_true(kGroupM10, QStringLiteral("ウィンドウを閉じたら結果は消える (次に開いたときに表示されない)"),
				    !sim.obs().ops.export_result.has_value());
	});
	// export の (c) の取りやめ: 保存のダイアログをやめたら、何も表示されない。
	s.then(300, [&] { host.open_settings(SettingsPage::About, false); });
	s.then_blocking(300, [&] {
		host.save_accept = false;
		host.prepared_done = 0;
		click_widget(input, export_button(host.settings));
	});
	s.then_when(300, [&] { return host.prepared_done > 0; }, 8000, [&](bool done) {
		results.expect_true(kGroupM10, QStringLiteral("(c) 保存のダイアログをやめ、エクスポートの終わりまで進んだ"), done);
		int dialogs = 0;
		for (QWidget *w : QApplication::topLevelWidgets()) {
			if (w->isVisible() && w->objectName() == QLatin1String("cordmuxMessageDialog"))
				++dialogs;
		}
		results.expect_true(kGroupM10, QStringLiteral("(c) 保存のダイアログをやめたときは、何も表示されない"),
				    dialogs == 0 && !sim.obs().ops.export_result.has_value());
		click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel));
	});

	// ---- export の (a): ドックのメニューから始める。設定ウィンドウを開いてから保存のダイアログを表示する ----
	auto m10_trace = std::make_shared<FocusTrace>();
	s.then_blocking(400, [&] {
		host.save_accept = true;
		host.export_calls = 0;
		host.save_dialogs = 0;
		host.prepared_done = 0;
		host.focus_at_export.clear();
		host.focus_before_busy_off.clear();
		host.focus_after_busy_off.clear();
		host.focus_after_tick.clear();
		auto *b = sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"));
		click_widget(input, b); // ドックのメニュー (exec)
	});
	s.then_blocking(300, [&] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		QAction *item = nullptr;
		const QString text = dock::ui::resolve_key(keys::kDockMenuExportDiagnosticsMenu);
		if (menu != nullptr) {
			for (QAction *a : menu->actions()) {
				if (a->text() == text)
					item = a;
			}
		}
		if (menu == nullptr || item == nullptr) {
			results.add(kGroupM10, QStringLiteral("(a) ドックのメニューにエクスポートの項目がある"), QStringLiteral("no"),
				    QStringLiteral("yes"), Verdict::Fail);
			if (menu != nullptr)
				input.key(Qt::Key_Escape);
			return;
		}
		input.move_to(menu->mapToGlobal(menu->actionGeometry(item).center()));
		m10_trace->start();
		host.trace = m10_trace.get();
		input.click(); // 項目 -> ExportBundle -> 設定ウィンドウ。次の周回で保存のダイアログが開く
	});
	s.then_when(400, [&] { return host.prepared_done > 0; }, 8000, [&](bool done) {
		results.expect_true(kGroupM10, QStringLiteral("(a) 保存のダイアログが閉じ、エクスポートの終わりまで進んだ"), done);
		results.expect_true(kGroupM10, QStringLiteral("(a) ドックのメニューから: 設定ウィンドウが先に表示されていた"),
				    host.export_calls == 1 && host.window_visible_at_export);
		results.expect_true(kGroupM10, QStringLiteral("(a) そのページが「バージョン情報」"),
				    host.page_at_export == SettingsPage::About);
		results.expect_true(kGroupM10, QStringLiteral("(a) 保存のダイアログの親が設定ウィンドウ"), host.save_dialogs == 1 && host.save_owner_ok);
		// フォーカスの戻り先。負荷が高いと保存のダイアログが開くのが遅れるので、確認は
		// on_prepared が終わってから行う (then_when)。終わった時点でフォーカスが移っていなければ、
		// さらに 2 秒まで待ち、遅れて移ったか (イベントの周回の遅延) と、移らないまま (製品の戻し方の不具合。
		// dock/settings_window.cpp の set_export_busy) かを分ける。note には、始めた時点のウィンドウの
		// フォーカスのあるウィジェット (保存したウィジェット)、保存のダイアログを閉じた直後、処理中を戻した後、
		// 周期の後のウィンドウのフォーカスと、終わった時点のアプリケーションのフォーカス、
		// ウィンドウのフォーカス、アクティブかを書く。
		// 押してから決まった時間 (700 ms) の 1 点だけで見る方法はとらない。負荷が高いと、保存のダイアログが
		// 開いている間に確認が実行され、フォーカスが戻る前に見てしまうためである。待つ時間を延ばすだけの
		// 方法もとらない。移らない場合と遅い場合が同じに見え、負荷がさらに高いと、
		// また確認が先に実行されるためである。
		QPushButton *b = export_button(host.settings);
		auto id_of = [](QWidget *f) { return focus_name(f); };
		m10_trace->mark(QStringLiteral("確認"));
		const QString at_end = QStringLiteral("終わった時点: アプリケーションのフォーカス %1 / ウィンドウのフォーカス %2 / ウィンドウがアクティブ %3")
					       .arg(id_of(QApplication::focusWidget()))
					       .arg(id_of(!host.settings.isNull() ? host.settings->focusWidget() : nullptr))
					       .arg(!host.settings.isNull() && host.settings->isActiveWindow() ? QStringLiteral("yes")
													  : QStringLiteral("no"));
		int late_ms = 0;
		if (QApplication::focusWidget() != b) {
			QElapsedTimer t;
			t.start();
			while (t.elapsed() < 2000 && QApplication::focusWidget() != export_button(host.settings))
				pump(20);
			late_ms = static_cast<int>(t.elapsed());
			b = export_button(host.settings);
		}
		const bool arrived = b != nullptr && QApplication::focusWidget() == b;
		results.expect_true(kGroupM10, QStringLiteral("(a) 最後にフォーカスがページのボタンに戻る"), arrived,
				    QStringLiteral("始めた時点のウィンドウのフォーカス %1 (ウィンドウがアクティブ %2) / 保存のダイアログを閉じた直後のウィンドウのフォーカス %3 -> 処理中を戻した後 %4 -> 周期の後 %5 (ウィンドウがアクティブ %6) / %7 / %8")
					    .arg(host.focus_at_export)
					    .arg(host.window_active_at_export ? QStringLiteral("yes") : QStringLiteral("no"))
					    .arg(host.focus_before_busy_off, host.focus_after_busy_off, host.focus_after_tick)
					    .arg(host.window_active_after_save ? QStringLiteral("yes") : QStringLiteral("no"))
					    .arg(at_end)
					    .arg(late_ms == 0 ? QStringLiteral("終わった時点で移っていた")
							      : (arrived ? QStringLiteral("さらに %1 ms 後に移った").arg(late_ms)
									 : QStringLiteral("さらに 2000 ms 待っても移らない: アプリケーションのフォーカス %1 / ウィンドウのフォーカス %2")
										   .arg(id_of(QApplication::focusWidget()))
										   .arg(id_of(!host.settings.isNull() ? host.settings->focusWidget() : nullptr)))));
		m10_trace->stop();
		host.trace = nullptr;
		if (late_ms > 0)
			results.info(kGroupM10, QStringLiteral("(a) フォーカスの戻りが、終わった時点に間に合わなかった (記録)"),
				     arrived ? QStringLiteral("遅れて移った") : QStringLiteral("移らない"),
				     at_end + QStringLiteral(" / 項目を押してからのアクティブとフォーカスのイベント: ") + m10_trace->text());
		click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel));
	});

	// ---- export の (b): メッセージのレベル 1 のメッセージのボタンから始める ----
	// Discord の内部への依存箇所が見つからないメッセージ (ContactUnavailable) の [診断情報をエクスポート] を押す。
	s.then(300, [&] {
		sim.obs().contacts.push_back({"pc-hook", ledger::ContactState::Missing});
		sim.changed();
		// メッセージのボタンは状態タブにある。
		sim.widget()->set_tab(config::DockTab::Status);
	});
	s.then_blocking(3500, [&] {
		// 表示するまでの待ち (2 秒) の後に、メッセージのボタンを押す。
		host.export_calls = 0;
		const QString text = dock::ui::resolve_key(keys::kDockExportDiagnosticsButton);
		QPushButton *button = nullptr;
		for (QPushButton *b : sim.widget()->findChildren<QPushButton *>()) {
			if (b->isVisible() && b->text() == text)
				button = b;
		}
		results.expect_true(kGroupM10, QStringLiteral("(b) メッセージのレベル 1 のメッセージに [診断情報をエクスポート] が表示される"), button != nullptr);
		input.move_to(QPoint(10, 10)); // ドックの外から動かす (クリックを捨てるガードの時間の外で押す)
		click_widget(input, button);
	});
	s.then(700, [&] {
		results.expect_true(kGroupM10, QStringLiteral("(b) メッセージのボタンから: 設定ウィンドウが「バージョン情報」で先に表示されていた"),
				    host.export_calls == 1 && host.window_visible_at_export && host.page_at_export == SettingsPage::About);
	});
	s.then(300, [&] {
		click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel));
		sim.obs().contacts.clear();
		sim.changed();
		sim.widget()->set_tab(config::DockTab::People);
	});

	// ---- save-failed: 設定の保存の失敗 ----
	// 失敗のダイアログの止め方は、Windows の所有されたウィンドウと同じ形にする。失敗のダイアログは、
	// 操作を始めたウィンドウ (設定ウィンドウ) を親にしたモーダルでないダイアログで、開いている間は親だけが入力
	// (クリック、キー、閉じる要求) を無視し、押されたら失敗のダイアログを前面に出す。親を灰色にしない。
	// OBS のメインウィンドウとフローティングのドックは止めない (配信中も OBS とドックを使えるようにする)。
	// 設定ウィンドウを親のないウィンドウにする方法はとらない。OBS の後ろに回り、タスクバーにボタンが
	// 表示されるためである。Qt::WindowModal もとらない。親とその祖先のウィンドウ (OBS のメインウィンドウと
	// フローティングのドック) も止めるためである (dock/ui/message_dialog.h の MessageBlocking)。
	auto gap_before = std::make_shared<int>(-1);
	s.then(400, [&] {
		host.fail_save = true;
		host.apply_calls = 0;
		host.open_settings(SettingsPage::Recording, false);
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		if (spin != nullptr) {
			spin->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Up);
		}
	});
	s.then_blocking(200, [&] { click_widget(input, box_button(host.settings, QDialogButtonBox::Apply)); });
	s.then(400, [&] {
		QWidget *failure = visible_message_dialog(host.settings);
		results.expect_true(kGroupM8, QStringLiteral("保存できないと、失敗のダイアログが表示される"), failure != nullptr || visible_message_dialog() != nullptr);
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログの親が設定ウィンドウ"), failure != nullptr);
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログはモーダルでない (Qt::NonModal)"),
				    failure != nullptr && failure->windowModality() == Qt::NonModal &&
					    QApplication::activeModalWidget() == nullptr,
				    failure != nullptr ? QStringLiteral("windowModality %1").arg(int(failure->windowModality())) : QString());
		results.expect_true(kGroupM8, QStringLiteral("設定ウィンドウは閉じない"), !host.settings.isNull() && host.settings->isVisible());
		results.expect_true(kGroupM8, QStringLiteral("設定ウィンドウを灰色にしない"),
				    !host.settings.isNull() && host.settings->isEnabled());
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログが OBS のメインウィンドウを止めない"), !window_blocked(main_window()));
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログがドックを止めない"), !window_blocked(sim.dock()));
		// 実際にドックの行を押して、動作するかを見る (止められていれば何も起きない)。
		const std::size_t before = sim.actions().size();
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && row.check_enabled) {
				const QRect r = check_hit_rect_global(sim.people(), row.user_id);
				input.move_to(r.center());
				break;
			}
		}
		pump(600); // クリックを捨てるガードの時間の外
		input.click();
		const bool dock_click_ran = sim.actions().size() > before;
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログが開いている間も、ドックの行を押すと動作する"), dock_click_ran);
		if (dock_click_ran) {
			// 押した分を戻す (同じ人をもう一度押す)。
			pump(600);
			input.click();
		}
		results.expect_true(kGroupM8, QStringLiteral("ドックを押した後も、失敗のダイアログは開いたまま"),
				    visible_message_dialog(host.settings) != nullptr);
	});
	// 親 (設定ウィンドウ) を押す: [キャンセル] が動作せず (ウィンドウが閉じない)、失敗のダイアログが前面に出て
	// アクティブになる。
	auto cancel_covered = std::make_shared<bool>(false);
	s.then(300, [&, cancel_covered] {
		QPushButton *cancel = box_button(host.settings, QDialogButtonBox::Cancel);
		if (cancel == nullptr)
			return;
		const QPoint at = cancel->mapToGlobal(cancel->rect().center());
		*cancel_covered = Input::window_at(at) != host.settings->windowHandle();
		input.move_to(at);
		pump(600);
		input.click();
	});
	s.then(300, [&, cancel_covered] {
		QWidget *failure = visible_message_dialog(host.settings);
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログが開いている間、設定ウィンドウの [キャンセル] を押しても動作しない"),
				    !host.settings.isNull() && host.settings->isVisible() && failure != nullptr,
				    *cancel_covered ? QStringLiteral("前提が満たされていない: [キャンセル] が失敗のダイアログに覆われていた") : QString());
		results.expect_true(kGroupM8, QStringLiteral("設定ウィンドウを押すと、失敗のダイアログが前面に出てアクティブになる"),
				    failure != nullptr && QApplication::activeWindow() == failure &&
					    Input::window_at(failure->mapToGlobal(failure->rect().center())) == failure->windowHandle(),
				    QStringLiteral("アクティブなウィンドウ %1").arg(QApplication::activeWindow() != nullptr
									   ? QApplication::activeWindow()->objectName()
									   : QStringLiteral("(none)")));
	});
	// 設定ウィンドウへのキー: 設定ウィンドウがアクティブになっても (Alt+Tab やタイトルバーを押したときと同じ)、
	// 入力欄の値は変わらない。
	s.then(300, [&, gap_before] {
		if (host.settings.isNull())
			return;
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		*gap_before = spin != nullptr ? spin->value() : -1;
		const int applies = host.apply_calls;
		// ウィンドウの中のフォーカスを、この入力欄にしておく (入力ではない)。
		if (spin != nullptr)
			spin->setFocus(Qt::OtherFocusReason);
		Input::activate_window(host.settings);
		pump(100);
		// Up だけを送る (Enter が届くと、[OK] から 2 つ目の失敗のダイアログが開き、手順の中で
		// 閉じるまで戻らない)。
		input.key(Qt::Key_Up);
		pump(200);
		int failures = 0;
		for (QWidget *w : QApplication::topLevelWidgets()) {
			if (w->isVisible() && w->objectName() == QLatin1String("cordmuxMessageDialog"))
				++failures;
		}
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログが開いている間、設定ウィンドウへのキーが動作しない"),
				    spin != nullptr && spin->value() == *gap_before && host.apply_calls == applies && failures == 1,
				    QStringLiteral("入力欄 %1 -> %2、適用 %3 -> %4、失敗のダイアログ %5")
					    .arg(*gap_before)
					    .arg(spin != nullptr ? spin->value() : -1)
					    .arg(applies)
					    .arg(host.apply_calls)
					    .arg(failures));
	});
	// 設定ウィンドウの [x] (Windows が送る閉じる要求): 親は閉じない (Windows のオーナーウィンドウは、
	// 止められている間 [x] も Alt+F4 も動作しない)。閉じると、開いている失敗のダイアログごと設定ウィンドウが消える。
	s.then(300, [&] {
		if (host.settings.isNull())
			return;
		QWindowSystemInterface::handleCloseEvent<QWindowSystemInterface::SynchronousDelivery>(host.settings->windowHandle());
		pump(300);
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログが開いている間、設定ウィンドウの [x] で設定ウィンドウが閉じない"),
				    !host.settings.isNull() && host.settings->isVisible() &&
					    visible_message_dialog(host.settings) != nullptr);
		if (host.settings.isNull() || !host.settings->isVisible()) {
			// 閉じてしまったら、続きの手順のために開き直す (結果は上の確認で決まる)。
			host.open_settings(SettingsPage::Recording, false);
		}
	});
	// 失敗のダイアログを閉じる: 親を押して失敗のダイアログを前面に出してから、Esc を押す (利用者と同じ操作)。
	s.then(300, [&] {
		QWidget *failure = visible_message_dialog(host.settings);
		if (failure != nullptr) {
			Input::activate_window(failure);
			pump(100);
		}
		input.key(Qt::Key_Escape);
	});
	s.then(300, [&] {
		QPushButton *apply = box_button(host.settings, QDialogButtonBox::Apply);
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログを閉じた"), visible_message_dialog() == nullptr);
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログを閉じた後、フォーカスが押したボタンへ戻る"),
				    QApplication::focusWidget() == apply, QStringLiteral("フォーカス %1").arg(describe_focus()));
		results.expect_true(kGroupM8, QStringLiteral("失敗の後も [適用] の値は今は動作している"), host.apply_calls == 1);
	});
	s.then(2600, [&, gap_before] {
		results.expect_true(kGroupM8, QStringLiteral("ドックにメッセージのレベル 2 (SettingsUnsaved) が表示される"), has_notice(sim.view(), "SettingsUnsaved"));
		// 書けるように戻して [適用] を押す。失敗のダイアログを閉じた後は、設定ウィンドウがまた入力を受ける
		// (止め方が残らない)。
		host.fail_save = false;
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		if (spin != nullptr) {
			spin->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Down);
		}
		const int applies = host.apply_calls;
		click_widget(input, box_button(host.settings, QDialogButtonBox::Apply));
		results.expect_true(kGroupM8, QStringLiteral("失敗のダイアログを閉じた後は、設定ウィンドウのキーとクリックがまた動作する"),
				    spin != nullptr && spin->value() == *gap_before - 1 && host.apply_calls == applies + 1,
				    QStringLiteral("入力欄 %1 -> %2、適用 %3 -> %4")
					    .arg(*gap_before)
					    .arg(spin != nullptr ? spin->value() : -1)
					    .arg(applies)
					    .arg(host.apply_calls));
	});
	s.then(1800, [&] {
		results.expect_true(kGroupM8, QStringLiteral("保存できたら、メッセージのレベル 2 が消える"), !has_notice(sim.view(), "SettingsUnsaved"));
		// [OK] で失敗したときも、ウィンドウは閉じない。
		host.fail_save = true;
		auto *spin = host.settings->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		if (spin != nullptr) {
			spin->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Up);
		}
	});
	s.then_blocking(200, [&] { click_widget(input, box_button(host.settings, QDialogButtonBox::Ok)); });
	s.then(400, [&] {
		QWidget *failure = visible_message_dialog(host.settings);
		results.expect_true(kGroupM8, QStringLiteral("[OK] で保存できないときも、失敗のダイアログが表示される"), failure != nullptr);
		if (failure != nullptr) {
			Input::activate_window(failure);
			pump(100);
		}
		input.key(Qt::Key_Escape);
	});
	s.then(300, [&] {
		results.expect_true(kGroupM8, QStringLiteral("[OK] で保存できないときは、ウィンドウを閉じない"),
				    !host.settings.isNull() && host.settings->isVisible());
		host.fail_save = false;
		click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel));
		sim.obs().ops.settings_unsaved = false;
		sim.changed();
	});

	// ---- owner-only: 設定ウィンドウから始めたエクスポートの失敗と、アンインストールの確認と結果 ----
	const QString g138 = QStringLiteral("owner-only");
	auto export_button_of = [&]() { return export_button(host.settings); };
	s.then(300, [&] {
		host.export_fail = bundle::ExportOutcome::WriteFailed;
		host.open_settings(SettingsPage::About, false);
	});
	s.then_blocking(300, [&] { click_widget(input, export_button_of()); });
	add_owner_only_steps(s, results, sim, input, host,
			     {g138, QStringLiteral("エクスポートの失敗 (設定ウィンドウから)"),
			      [&]() -> QWidget * { return find_message_dialog(host.settings); },
			      [&]() -> QWidget * { return export_button_of(); }, true});
	auto uninstall_button_of = [&]() -> QPushButton * {
		const QString text = dock::ui::resolve_key(keys::kSettingsUninstallButton);
		if (host.settings.isNull())
			return nullptr;
		for (QPushButton *b : host.settings->findChildren<QPushButton *>()) {
			if (b->text() == text)
				return b;
		}
		return nullptr;
	};
	// アンインストールを押せる状態にする (テストデータ people-8 は Vesktop が起動していて押せない。
	// 押せない理由がどれも当てはまらない状態: 配信と録画をしていない、Vesktop が起動していない)。終わったら戻す。
	auto install_before = std::make_shared<ledger::Observation>();
	s.then(300, [&, install_before] {
		*install_before = sim.obs();
		sim.obs().install.process = ledger::VesktopProcess::NotRunning;
		sim.obs().uninstall.obs_streaming = false;
		sim.obs().uninstall.obs_recording = false;
		sim.obs().choices.recording = ledger::FeatureSwitch::Off;
		sim.changed();
		sim.tick();
	});
	s.then(300, [&] {
		host.export_fail.reset();
		host.uninstall_confirm_choice = -2;
		host.uninstall_results = 0;
		host.open_settings(SettingsPage::About, false);
		QPushButton *b = uninstall_button_of();
		results.expect_true(g138, QStringLiteral("設定ウィンドウの [アンインストール] が押せる (テストデータの前提)"), b != nullptr && b->isEnabled());
	});
	s.then_blocking(300, [&] { click_widget(input, uninstall_button_of()); });
	add_owner_only_steps(s, results, sim, input, host,
			     {g138, QStringLiteral("アンインストールの確認 (設定ウィンドウから)"),
			      [&]() -> QWidget * { return find_message_dialog(host.settings); },
			      [&]() -> QWidget * { return uninstall_button_of(); }, true});
	s.then(200, [&] {
		results.expect_eq(g138, QStringLiteral("アンインストールの確認: Esc の答えは [キャンセル] で、アンインストールしない"),
				  host.uninstall_results, 0, QStringLiteral("答え %1").arg(host.uninstall_confirm_choice));
	});
	// 確認で [アンインストール] を押すと、結果のダイアログが同じ親で表示される。
	s.then_blocking(300, [&] { click_widget(input, uninstall_button_of()); });
	s.then(400, [&] {
		QWidget *confirm = find_message_dialog(host.settings);
		QPushButton *go = nullptr;
		const dock::ui::MessageSpec spec = dock::uninstall_confirm_spec();
		if (confirm != nullptr && !spec.buttons.empty()) {
			for (QPushButton *b : confirm->findChildren<QPushButton *>()) {
				if (b->text() == spec.buttons.front().text)
					go = b;
			}
		}
		results.expect_true(g138, QStringLiteral("アンインストールの確認: [アンインストール] のボタンがある"), go != nullptr);
		if (go != nullptr)
			click_widget(input, go);
	});
	add_owner_only_steps(s, results, sim, input, host,
			     {g138, QStringLiteral("アンインストールの結果 (設定ウィンドウから)"),
			      [&]() -> QWidget * { return host.uninstall_results > 0 ? find_message_dialog(host.settings) : nullptr; },
			      [&]() -> QWidget * {
				      auto *w = dynamic_cast<dock::SettingsWindow *>(host.settings.data());
				      return w != nullptr ? w->about_focus_fallback() : nullptr;
			      },
			      true});
	s.then(200, [&, install_before] {
		results.expect_eq(g138, QStringLiteral("アンインストールの結果のダイアログを表示した数"), host.uninstall_results, 1);
		if (!host.settings.isNull())
			host.settings->close();
		sim.obs().install = install_before->install;
		sim.obs().uninstall = install_before->uninstall;
		sim.obs().choices.recording = install_before->choices.recording;
		sim.changed();
		sim.tick();
	});

	// ---- pickers: 設定ウィンドウから開くフォルダーの選択 ----
	// フォルダーの選択は OS のダイアログで、何が止まるかを記録だけする。色のダイアログはドックの表示タブから開くので、
	// look のシナリオで確認する。
	const QString g10 = QStringLiteral("pickers");
	// フォルダーの選択: 製品の処理 (QFileDialog::getExistingDirectory。中で exec する) のまま開き、
	// 開いている間に何が止まるかを記録する。プレビュー用のプラットフォームプラグインには OS のダイアログがなく、
	// Qt のウィジェットのダイアログになるので、Windows の実際のダイアログとは止め方が違う。
	auto folder_probe = std::make_shared<std::unique_ptr<MainWindowProbe>>();
	auto browse_button_of = [&]() -> QPushButton * {
		const QString text = dock::ui::resolve_key(keys::kSettingsBrowseButton);
		if (host.settings.isNull())
			return nullptr;
		for (QPushButton *b : host.settings->findChildren<QPushButton *>()) {
			if (b->isVisible() && b->text() == text)
				return b;
		}
		return nullptr;
	};
	s.then(300, [&, folder_probe] {
		host.open_settings(SettingsPage::Recording, false);
		*folder_probe = std::make_unique<MainWindowProbe>();
		results.expect_true(g10, QStringLiteral("フォルダーの選択: [参照...] が押せる (テストデータの前提)"),
				    browse_button_of() != nullptr && browse_button_of()->isEnabled());
	});
	s.then_blocking(300, [&] { click_widget(input, browse_button_of()); });
	s.then(500, [&, folder_probe] {
		auto *d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
		results.expect_true(g10, QStringLiteral("フォルダーの選択: 親が設定ウィンドウ (OS のダイアログのまま、親は設定ウィンドウ)"),
				    d != nullptr && d->parentWidget() == host.settings.data(),
				    d != nullptr ? QStringLiteral("親 %1").arg(name_of_widget(d->parentWidget())) : QStringLiteral("(開かない)"));
		if (d == nullptr)
			return;
		const bool main_ok = *folder_probe != nullptr && (*folder_probe)->click_works(input);
		QString note;
		const bool dock_ok = dock_row_click_works(input, sim, &note);
		results.info(g10, QStringLiteral("フォルダーの選択 (プレビュー用のプラットフォームプラグインでは Qt のウィジェットのダイアログ): ダイアログの止め方"),
			     QString::number(int(d->windowModality())),
			     QStringLiteral("0 = モーダルでない、1 = WindowModal、2 = ApplicationModal。QFileDialog の静的な関数は exec で "
					    "ApplicationModal にする (qdialog.cpp の exec が WA_ShowModal を立てる)"));
		results.info(g10, QStringLiteral("フォルダーの選択 (プレビュー用のプラットフォームプラグインでは Qt のウィジェットのダイアログ): 開いている間に OBS のメインウィンドウを押すと動作するか"),
			     main_ok ? QStringLiteral("yes") : QStringLiteral("no"),
			     QStringLiteral("Qt のモーダル %1").arg(qt_window_blocked(main_window()) ? QStringLiteral("あり") : QStringLiteral("なし")));
		results.info(g10, QStringLiteral("フォルダーの選択 (プレビュー用のプラットフォームプラグインでは Qt のウィジェットのダイアログ): 開いている間にドックの行を押すと動作するか"),
			     dock_ok ? QStringLiteral("yes") : QStringLiteral("no"), note);
		d->reject();
	});
	s.then(300, [&, folder_probe] {
		results.expect_true(g10, QStringLiteral("フォルダーの選択: やめた後、フォーカスが [参照...] へ戻る"),
				    QApplication::focusWidget() == browse_button_of(), QStringLiteral("フォーカス %1").arg(describe_focus()));
		folder_probe->reset();
		if (!host.settings.isNull())
			host.settings->close();
	});

	// ---- フォーカスリング (設定ウィンドウと参加者の設定ウィンドウ) ----
	s.then(300, [&] { host.open_settings(SettingsPage::Recording, false); });
	s.then(200, [&] {
		input.key(Qt::Key_Tab);
		const RingMeasure m = measure_ring(host.settings, QApplication::focusWidget());
		results.expect_true(QStringLiteral("focus-ring"), QStringLiteral("設定ウィンドウ: Tab で移ったフォーカスに、cordmux のフォーカスリングが表示される"),
				    m.ring_visible && m.drawn,
				    QStringLiteral("フォーカスリングのウィジェット %1").arg(m.ring_widget_found ? QStringLiteral("あり") : QStringLiteral("なし")));
		click_widget(input, box_button(host.settings, QDialogButtonBox::Cancel));
		host.open_participants();
	});
	s.then(200, [&] {
		input.key(Qt::Key_Tab);
		const RingMeasure m = measure_ring(host.participants, QApplication::focusWidget());
		results.expect_true(QStringLiteral("focus-ring"), QStringLiteral("参加者の設定ウィンドウ: Tab で移ったフォーカスに、cordmux のフォーカスリングが表示される"),
				    m.ring_visible && m.drawn,
				    QStringLiteral("フォーカスリングのウィジェット %1").arg(m.ring_widget_found ? QStringLiteral("あり") : QStringLiteral("なし")));
		if (!host.participants.isNull())
			host.participants->close();
	});

	const bool finished = s.run(420000);
	results.expect_true(kGroup, QStringLiteral("手順が時間内に終わった"), finished);
	sim.on_tick = nullptr;
	sim.stop();
	if (!host.settings.isNull())
		delete host.settings.data();
	if (!host.participants.isNull())
		delete host.participants.data();
	sim.dock()->hide();

	// 起動した時点で設定のファイルが壊れていたら、起動したときからメッセージのレベル 2 を表示する
	// (save-failed の最後)。
	{
		ui_preview::Fixture f = ui_preview::fixture_by_id("people-8");
		SimPoll boot(f, options.style);
		boot.obs().ops.settings_unreadable_at_startup = true;
		boot.dock()->setGeometry(1300, 120, 420, 700);
		boot.dock()->show();
		boot.start();
		pump(2800);
		results.expect_true(kGroupM8, QStringLiteral("起動した時点で読めなかったら、起動したときからメッセージのレベル 2 (SettingsUnreadAtStartup)"),
				    has_notice(boot.view(), "SettingsUnreadAtStartup"));
		boot.stop();
		boot.dock()->hide();
	}
	// OBS の [ツール] の「cordmuxをアンインストール...」。
	run_tools_uninstall(options, results);
	// 設定ウィンドウのページの一覧とフォーカス。
	run_settings_pages(options, results);
}

// ---- tools: OBS の [ツール] の「cordmuxをアンインストール...」 ----

namespace {

// ウィジェット w が、それを含むスクロール領域 (QScrollArea) の表示範囲にすべて入っているか (スクロール領域の外なら、
// ウィンドウの中に表示されているか)。
bool shown_in_viewport(QWidget *w)
{
	if (w == nullptr || !w->isVisible())
		return false;
	for (QWidget *p = w->parentWidget(); p != nullptr; p = p->parentWidget()) {
		if (auto *area = qobject_cast<QScrollArea *>(p)) {
			QWidget *vp = area->viewport();
			const QRect r(w->mapTo(vp, QPoint(0, 0)), w->size());
			return vp->rect().contains(r);
		}
	}
	return w->window()->rect().contains(QRect(w->mapTo(w->window(), QPoint(0, 0)), w->size()));
}

// ウィンドウの中で、テキストが text の QLabel (折り返し位置を入れる WrapLabel は、折り返す前のテキストで比べる)。
QWidget *label_with_text(QWidget *window, const QString &text)
{
	if (window == nullptr || text.isEmpty())
		return nullptr;
	for (QLabel *l : window->findChildren<QLabel *>()) {
		auto *wrap = dynamic_cast<dock::ui::WrapLabel *>(l);
		const QString shown = wrap != nullptr ? wrap->source_text() : l->text();
		if (shown == text && l->isVisible())
			return l;
	}
	return nullptr;
}

} // namespace

// tools: OBS の [ツール] の「cordmuxをアンインストール...」。Host::open_settings_to_uninstall
// (製品の open_settings_to_uninstall と同じ順) を、[ツール] の項目を選んだのと同じ形で呼ぶ。
//   押せる状態 (配信、OBSの録画、cordmuxの録画をしていない。Vesktop が起動していない): 設定ウィンドウが
//   「バージョン情報」で開き、その上に確認のダイアログ (親は設定ウィンドウ) が表示される。[キャンセル] で何も変わらない
//   (アンインストールせず、結果のダイアログを表示せず、段階が NotStarted のまま)。閉じた後のフォーカスは、
//   ページの [cordmuxをアンインストール...]。設定ウィンドウを別のページで開いていたときも同じ。
//   押せない 6 つの状態 (配信中、OBSの録画中、cordmuxの録画中、Vesktopの起動中、状態の確認中、アンインストールを始めた
//   後): 確認のダイアログを表示せず、ページが「バージョン情報」で、ページのボタンが押せず、ボタンとその下の理由が、
//   スクロール領域の表示範囲に入っている。理由のテキストは、押せない理由の規則 (ledger/actions.cpp の順) から
//   決めたもの。スクロール領域をいちばん上に戻してから選んでも同じ。
//   どの状態でも、「確認のダイアログが表示された」と「ページのボタンが押せる」が一致する (同じ判定を通る)。
//   スクロールする場合: 設定ウィンドウを別のページで開いていた回と、スクロール領域をいちばん上にしてから選ぶ回は、
//   ウィンドウを最小の高さまで縮めてから選び、スクロールの範囲が 0 より大きいことを先に確認する (縮めないと中身が
//   ウィンドウに収まり、スクロールしなくても「表示範囲にある」が合格する)。押せる見た目のまま、確認のダイアログの前の
//   取り直しで断るときも、縮めたウィンドウで理由が表示範囲に表示されるかを見る。
void run_tools_uninstall(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("tools-uninstall");
	SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	Host host;
	host.sim = &sim;
	host.effective_at_start = sim.settings();
	sim.on_tick = [&](const ledger::StabilizeResult &st) { host.refresh_windows(st.view); };
	sim.start();

	// ページの [cordmuxをアンインストール...] (アンインストールしている間は、テキストが「アンインストール中...」に
	// 変わる。同じボタン)。
	auto uninstall_button_of = [&]() -> QPushButton * {
		const QString text = dock::ui::resolve_key(keys::kSettingsUninstallButton);
		const QString busy = dock::ui::resolve_key(keys::kSettingsUninstallBusyButton);
		if (host.settings.isNull())
			return nullptr;
		for (QPushButton *b : host.settings->findChildren<QPushButton *>()) {
			if (b->text() == text || b->text() == busy)
				return b;
		}
		return nullptr;
	};
	// 「バージョン情報」のページのスクロール領域 (ページのボタンを含む QScrollArea)。
	auto about_scroll = [&]() -> QScrollArea * {
		QPushButton *b = uninstall_button_of();
		for (QWidget *p = b != nullptr ? b->parentWidget() : nullptr; p != nullptr; p = p->parentWidget()) {
			if (auto *area = qobject_cast<QScrollArea *>(p))
				return area;
		}
		return nullptr;
	};
	// 設定ウィンドウを縮める (利用者がウィンドウの下端をドラッグして低くしたのと同じ。ウィンドウの最小の高さまで)。
	// 「バージョン情報」の中身がウィンドウより高くなり、スクロールの範囲が 0 より大きくなる
	// (中身の高さで開いたままだと、スクロールする処理を通らず、「表示範囲にある」が中身によらず合格する)。
	// 前に閉じたときの大きさ (SettingsWindowInput::geometry) に低いウィンドウを入れて開く方法はとらない。
	// 同じ状態になるが、テストが saveGeometry の中身を作ることになり、製品の復元の処理 (restoreGeometry が
	// 成功するか) までテストの前提に入るためである。
	auto shrink_settings = [&] {
		if (host.settings.isNull())
			return;
		host.settings->resize(host.settings->width(), host.settings->minimumHeight());
		pump(100);
	};
	auto scroll_range_text = [&]() -> QString {
		QScrollArea *area = about_scroll();
		if (area == nullptr || host.settings.isNull())
			return QStringLiteral("(スクロール領域なし)");
		return QStringLiteral("ウィンドウの高さ %1 / スクロール %2 / %3")
			.arg(host.settings->height())
			.arg(area->verticalScrollBar()->value())
			.arg(area->verticalScrollBar()->maximum());
	};
	auto scrollable = [&]() {
		QScrollArea *area = about_scroll();
		return area != nullptr && area->verticalScrollBar()->maximum() > 0;
	};
	// 設定ウィンドウのフォーカスのあるウィジェット (ウィンドウがアクティブでなくても、ウィンドウが覚えている
	// ウィジェット。製品が保存して戻すのもこのウィジェット。dock/settings_window.cpp)。
	auto window_focus = [&]() -> QWidget * { return host.settings.isNull() ? nullptr : host.settings->focusWidget(); };
	auto window_focus_text = [&]() -> QString {
		QWidget *f = window_focus();
		return f != nullptr ? name_of_widget(f) + QLatin1Char('/') + QString::fromLatin1(f->metaObject()->className())
				    : QStringLiteral("(none)");
	};
	// フォーカスのあるウィジェットが表示範囲にある (フォーカスが移ったウィジェットは、
	// 表示される所までスクロールする。WCAG 2.4.11。表示されていない所にフォーカスを置かない)。スクロール領域の外の
	// ウィジェット (項目の一覧など) は、ウィンドウの中に表示されていればよい。
	auto focus_visible = [&]() { return shown_in_viewport(window_focus()); };
	// フォーカスのあるウィジェットがスクロール領域 (QScrollArea) でない (スクロール領域はフォーカスを受けず、
	// タブストップにしない)。
	auto focus_not_box = [&]() { return qobject_cast<QScrollArea *>(window_focus()) == nullptr; };
	// フォーカスが項目の一覧にある (替える前のページの中にフォーカスがあったときは、ページを替えたら一覧に置く)。
	auto focus_on_page_list = [&]() {
		QWidget *f = window_focus();
		return f != nullptr && f->objectName() == QLatin1String("cordmuxSettingsPageList");
	};
	// 現在のページの入力欄 (テキスト、数値、選択、チェック) で、表示されていて操作できる最初のものにフォーカスを置く
	// (利用者が入力欄を触っている状態。別のページから「バージョン情報」へ替わると、Qt はフォーカスを、
	// 新しいページのフォーカスの順の最初のウィジェットへ移す)。置けたウィジェットの名前を返す (置けなければ空)。
	auto focus_page_field = [&]() -> QString {
		if (host.settings.isNull())
			return QString();
		for (QWidget *c : host.settings->findChildren<QWidget *>()) {
			const bool field = qobject_cast<QLineEdit *>(c) != nullptr || qobject_cast<QAbstractSpinBox *>(c) != nullptr ||
					   qobject_cast<QComboBox *>(c) != nullptr || qobject_cast<QCheckBox *>(c) != nullptr;
			if (field && c->isVisible() && c->isEnabled() && (c->focusPolicy() & Qt::TabFocus) != 0) {
				c->setFocus(Qt::TabFocusReason);
				pump(50);
				return host.settings->focusWidget() == c ? name_of_widget(c) : QString();
			}
		}
		return QString();
	};
	// 現在の View の、押せない理由の QLabel。理由がなければ nullptr を返す。
	auto reason_label_now = [&]() -> QWidget * {
		const auto &r = sim.view().about.uninstall_button.disabled_reason;
		return r.has_value() ? label_with_text(host.settings, dock::ui::resolve(*r)) : nullptr;
	};
	// スクロール領域をいちばん下へ (利用者がページのボタンまでスクロールした状態)。
	auto scroll_about_bottom = [&] {
		if (QScrollArea *area = about_scroll())
			area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
		pump(50);
	};
	// 押せる状態 (押せない理由が 1 つも当てはまらない状態)。
	auto set_ready = [&] {
		sim.obs().install.process = ledger::VesktopProcess::NotRunning;
		sim.obs().uninstall.phase = ledger::UninstallPhase::NotStarted;
		sim.obs().uninstall.obs_streaming = false;
		sim.obs().uninstall.obs_recording = false;
		sim.obs().choices.recording = ledger::FeatureSwitch::Off;
		sim.changed();
		sim.tick();
	};
	const auto before = std::make_shared<ledger::Observation>(sim.obs());

	Script s;
	// ---- 押せる状態: 設定ウィンドウが閉じている所から ----
	struct ReadyCase {
		QString label;
		bool open_first; // 先に設定ウィンドウを別のページ ([録画]) で開いておく
	};
	for (const ReadyCase rc : {ReadyCase{QStringLiteral("押せる (設定ウィンドウが閉じている)"), false},
				   ReadyCase{QStringLiteral("押せる (設定ウィンドウを [録画] で開いている)"), true}}) {
		s.then(300, [&, rc] {
			set_ready();
			host.uninstall_confirm_choice = -2;
			host.uninstall_results = 0;
			if (rc.open_first) {
				host.open_settings(SettingsPage::Recording, false);
				// 開いていたウィンドウは縮めておく (スクロールしてボタンを表示する処理を通す)。
				shrink_settings();
			}
			host.open_settings_to_uninstall();
		});
		s.then(500, [&, rc] {
			if (rc.open_first) {
				results.expect_true(group, rc.label + QStringLiteral(": 前提: 縮めたウィンドウで「バージョン情報」の中身がウィンドウより高い (スクロールの範囲が 0 より大きい)"),
						    scrollable(), scroll_range_text());
			}
			QWidget *d = find_message_dialog(host.settings);
			results.expect_true(group, rc.label + QStringLiteral(": 設定ウィンドウが「バージョン情報」のページで開く"),
					    !host.settings.isNull() && host.settings->isVisible() &&
						    host.settings->page() == SettingsPage::About);
			const dock::ui::MessageSpec spec = dock::uninstall_confirm_spec();
			results.expect_true(group, rc.label + QStringLiteral(": 確認のダイアログが、設定ウィンドウを親にして表示される"),
					    d != nullptr && !host.settings.isNull() && d->parentWidget() == host.settings.data() &&
						    d->windowTitle() == spec.title,
					    d != nullptr ? QStringLiteral("タイトル %1 / 親 %2").arg(d->windowTitle(), name_of_widget(d->parentWidget()))
							 : QStringLiteral("(表示されない)"));
			QPushButton *b = uninstall_button_of();
			results.expect_true(group, rc.label + QStringLiteral(": ページのボタンが押せ、スクロール領域の表示範囲にある"),
					    b != nullptr && b->isEnabled() && shown_in_viewport(b));
			// [キャンセル] を押す (確認のダイアログの最後のボタン。戻せない操作の確認では、
			// 既定のボタンと Esc の答え)。
			QPushButton *cancel = nullptr;
			if (d != nullptr && spec.buttons.size() >= 2) {
				for (QPushButton *p : d->findChildren<QPushButton *>()) {
					if (p->text() == spec.buttons.back().text)
						cancel = p;
				}
			}
			results.expect_true(group, rc.label + QStringLiteral(": 確認のダイアログに [キャンセル] がある"), cancel != nullptr);
			click_widget(input, cancel);
		});
		s.then(400, [&, rc] {
			results.expect_true(group, rc.label + QStringLiteral(": [キャンセル] で確認のダイアログが閉じる"),
					    find_message_dialog(host.settings) == nullptr);
			results.expect_true(group, rc.label + QStringLiteral(": [キャンセル] で何も変わらない (アンインストールせず、結果のダイアログを表示せず、段階が NotStarted)"),
					    host.uninstall_confirm_choice != 0 && host.uninstall_results == 0 &&
						    sim.obs().uninstall.phase == ledger::UninstallPhase::NotStarted,
					    QStringLiteral("答え %1 / 結果のダイアログ %2").arg(host.uninstall_confirm_choice).arg(host.uninstall_results));
			results.expect_true(group, rc.label + QStringLiteral(": 閉じた後のフォーカスが、ページの [cordmuxをアンインストール...]"),
					    uninstall_button_of() != nullptr && QApplication::focusWidget() == uninstall_button_of(),
					    QStringLiteral("フォーカス %1").arg(describe_focus()));
			results.expect_true(group, rc.label + QStringLiteral(": 確認のダイアログが表示されたことと、ページのボタンが押せることが一致する"),
					    host.uninstall_confirm_choice != -2 && uninstall_button_of() != nullptr && uninstall_button_of()->isEnabled());
			if (!host.settings.isNull())
				host.settings->close();
		});
	}

	// ---- 押せない 6 つの状態 ----
	struct BlockedCase {
		QString label;
		const char *reason_key; // 押せない理由のテキストのキー
		std::function<void(ledger::Observation &)> apply;
		bool open_scrolled_top; // 先に「バージョン情報」を開いて、スクロール領域をいちばん上にしておく
	};
	const std::vector<BlockedCase> blocked{
		{QStringLiteral("配信中"), keys::kSettingsUninstallStreamingText,
		 [](ledger::Observation &o) { o.uninstall.obs_streaming = true; }, false},
		{QStringLiteral("OBSの録画中"), keys::kSettingsUninstallObsRecordingText,
		 [](ledger::Observation &o) { o.uninstall.obs_recording = true; }, false},
		{QStringLiteral("cordmuxの録画中"), keys::kSettingsUninstallPerPersonRecordingText,
		 [](ledger::Observation &o) { o.choices.recording = ledger::FeatureSwitch::On; }, false},
		{QStringLiteral("Vesktopの起動中"), keys::kSettingsUninstallVesktopRunningText,
		 [](ledger::Observation &o) { o.install.process = ledger::VesktopProcess::Running; }, false},
		{QStringLiteral("状態の確認中"), keys::kSettingsUninstallCheckingText,
		 [](ledger::Observation &o) { o.uninstall.obs_streaming.reset(); }, false},
		{QStringLiteral("アンインストールを始めた後"), keys::kSettingsUninstallStartedText,
		 [](ledger::Observation &o) { o.uninstall.phase = ledger::UninstallPhase::Running; }, false},
		{QStringLiteral("配信中 (「バージョン情報」を開いてスクロール領域をいちばん上にしてから)"), keys::kSettingsUninstallStreamingText,
		 [](ledger::Observation &o) { o.uninstall.obs_streaming = true; }, true},
	};
	for (const BlockedCase &bc : blocked) {
		s.then(300, [&, bc] {
			set_ready();
			bc.apply(sim.obs());
			sim.changed();
			sim.tick();
			host.uninstall_confirm_choice = -2;
			host.uninstall_results = 0;
			if (bc.open_scrolled_top) {
				host.open_settings(SettingsPage::About, false);
				pump(100);
				shrink_settings();
				if (QScrollArea *area = about_scroll())
					area->verticalScrollBar()->setValue(0);
				pump(50);
				// 前提: スクロール領域がいちばん上で、ページのボタンは表示範囲の外
				// (この後の「表示範囲にある」が、[ツール] からの処理のスクロールで初めて
				// 満たされる状態)。
				results.expect_true(group, QStringLiteral("押せない (") + bc.label +
								   QStringLiteral("): 前提: 縮めたウィンドウでスクロールの範囲が 0 より大きく、ボタンが表示範囲の外"),
						    scrollable() && !shown_in_viewport(uninstall_button_of()), scroll_range_text());
				// [ツール] は OBS のメインウィンドウのメニュー (利用者はメインウィンドウへ
				// 戻ってから選ぶ)。
				Input::activate_window(main_window());
			}
			host.open_settings_to_uninstall();
		});
		s.then(500, [&, bc] {
			const QString label = QStringLiteral("押せない (") + bc.label + QLatin1Char(')');
			QWidget *d = find_message_dialog(host.settings);
			results.expect_true(group, label + QStringLiteral(": 確認のダイアログを表示しない"), d == nullptr && host.uninstall_confirm_choice == -2);
			results.expect_true(group, label + QStringLiteral(": 設定ウィンドウが「バージョン情報」のページ"),
					    !host.settings.isNull() && host.settings->isVisible() &&
						    host.settings->page() == SettingsPage::About);
			QPushButton *b = uninstall_button_of();
			results.expect_true(group, label + QStringLiteral(": ページのボタンが押せない"), b != nullptr && !b->isEnabled());
			results.expect_true(group, label + QStringLiteral(": ページのボタンが、スクロール領域の表示範囲にある"), shown_in_viewport(b),
					    scroll_range_text());
			const QString reason = dock::ui::resolve_key(bc.reason_key);
			QWidget *reason_label = label_with_text(host.settings, reason);
			results.expect_true(group, label + QStringLiteral(": ボタンの下に押せない理由があり、表示範囲にある"),
					    reason_label != nullptr && shown_in_viewport(reason_label),
					    QStringLiteral("期待するテキスト %1").arg(reason));
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットが表示範囲にある"), focus_visible(),
					    QStringLiteral("フォーカス %1 / %2").arg(window_focus_text(), scroll_range_text()));
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットがスクロール領域 (QScrollArea) でない"), focus_not_box(),
					    QStringLiteral("フォーカス %1").arg(window_focus_text()));
			results.expect_true(group, label + QStringLiteral(": 確認のダイアログが表示されなかったことと、ページのボタンが押せないことが一致する"),
					    d == nullptr && b != nullptr && !b->isEnabled());
			if (!host.settings.isNull())
				host.settings->close();
		});
	}

	// ---- 押せる見た目のまま、確認のダイアログの前の取り直しで断る回 ----
	// 最後の周期の View では押せる (配信していない) が、[ツール] から選ぶまでの周期の間に配信を始めた。
	// 製品は確認のダイアログを表示せず (dock/dock_poll_install.cpp の available_now)、0 ms 後の周期でページの
	// ボタンを押せなくして理由を表示する。start_uninstall は押す前にフォーカスをボタンへ置き、断ったら押す前の所へ
	// 戻す。[録画] の入力欄にフォーカスがあったときは、ページを替えたときにフォーカスが項目の一覧へ移っているので、
	// 一覧へ戻る (下の確認で見る)。
	// 確認すること: 確認のダイアログが表示されない。ボタンが押せない。ボタンとその下の理由が、縮めたウィンドウの
	// 表示範囲にある (押せないときは、ページのいちばん下と理由を表示する。理由の行が加わって中身が伸びた後も)。
	const QString refused = QStringLiteral("押せる見た目のまま取り直しで断る (周期の間に配信を始めた。[録画] の入力欄にフォーカス)");
	auto refused_field = std::make_shared<QString>();
	s.then(300, [&] {
		set_ready();
		host.uninstall_confirm_choice = -2;
		host.uninstall_results = 0;
		host.uninstall_refused = 0;
		host.open_settings(SettingsPage::Recording, false);
		shrink_settings();
		// [録画] の入力欄にフォーカスを置いておく (置けたウィジェットは、下の note に書く)。
		*refused_field = focus_page_field();
		Input::activate_window(main_window());
		host.streaming_starts_before_confirm = true;
		host.open_settings_to_uninstall();
	});
	s.then(500, [&, refused, refused_field] {
		QWidget *d = find_message_dialog(host.settings);
		QPushButton *b = uninstall_button_of();
		results.expect_true(group, refused + QStringLiteral(": 前提: 押せる見た目で選び、取り直しで断った (縮めたウィンドウでスクロールの範囲が 0 より大きい)"),
				    host.uninstall_refused == 1 && scrollable(),
				    QStringLiteral("断った数 %1 / %2").arg(host.uninstall_refused).arg(scroll_range_text()));
		results.expect_true(group, refused + QStringLiteral(": 確認のダイアログを表示しない"), d == nullptr && host.uninstall_confirm_choice == -2);
		results.expect_true(group, refused + QStringLiteral(": 次の周期でページのボタンが押せなくなる"), b != nullptr && !b->isEnabled());
		results.expect_true(group, refused + QStringLiteral(": ページのボタンが、スクロール領域の表示範囲にある"), shown_in_viewport(b),
				    scroll_range_text());
		const QString reason = dock::ui::resolve_key(keys::kSettingsUninstallStreamingText);
		QWidget *reason_label = label_with_text(host.settings, reason);
		results.expect_true(group, refused + QStringLiteral(": ボタンの下に押せない理由が表示され、表示範囲にある"),
				    reason_label != nullptr && shown_in_viewport(reason_label),
				    QStringLiteral("期待するテキスト %1 / %2").arg(reason, scroll_range_text()));
		results.expect_true(group, refused + QStringLiteral(": フォーカスのあるウィジェットが表示範囲にある"), focus_visible(),
				    QStringLiteral("前にフォーカスを置いた入力欄 %1 / フォーカス %2 / %3")
					    .arg(refused_field->isEmpty() ? QStringLiteral("(置けない)") : *refused_field, window_focus_text(),
						 scroll_range_text()));
		results.expect_true(group, refused + QStringLiteral(": フォーカスのあるウィジェットがスクロール領域 (QScrollArea) でない"), focus_not_box(),
				    QStringLiteral("フォーカス %1").arg(window_focus_text()));
		results.expect_true(group, refused + QStringLiteral(": [録画] の入力欄にあったフォーカスが、ページを替えた後は項目の一覧にある"),
				    focus_on_page_list(), QStringLiteral("フォーカス %1").arg(window_focus_text()));
		if (!host.settings.isNull())
			host.settings->close();
		sim.obs().obs_outputs.streaming = false;
		sim.changed();
	});

	// ---- 縮めたウィンドウでのフォーカスとスクロール ----
	// 3 つの処理で、押せない、断った、アンインストールを終えた後に、フォーカスのあるウィジェットと理由の行が、
	// スクロール領域の表示範囲にあるかを見る。
	//   (1) [ツール] からの処理で、設定ウィンドウを [録画] で開き、入力欄にフォーカスを置いていた回
	//       (押せないとき。断るときは上の確認)。ページが替わるとフォーカスは項目の一覧へ移り (替える前のページの中に
	//       フォーカスがあったとき)、その後いちばん下へスクロールする。
	//   (2) ページのボタンから押した処理: 取り直しで断った回 (押せなくなったボタンのフォーカスの移し先と、
	//       加わった理由の行) と、アンインストールを終えて結果のダイアログを閉じた回 (フォーカスは
	//       [コピー] に戻る)。
	//   (3) 途中で止まった回 (Stopped) の押し直しを [ツール] から選び、取り直しで断った回 (入力欄のページを
	//       操作できなくしている間)。
	// どの場合もウィンドウを最小の高さまで縮め、スクロールの範囲が 0 より大きいことを前提として先に確認する。
	// 合否は「フォーカスのあるウィジェットが表示範囲にある」と「スクロール領域でない」。
	// フォーカスの位置そのものは、製品が決めた所だけを合否にする: [録画] の入力欄にフォーカスがあってページが
	// 替わった 2 回 (この (1) と上の取り直しで断るとき) は項目の一覧。ほかの場合の位置 ([コピー] か一覧か) は記録する。
	// どの場合も、フォーカスの移り先を特定のウィジェットに決めて合否にする方法はとらない。決めていない戻り先まで
	// テストが縛ることになり、求められているのは表示されていることだけだからである。
	// (1) 押せない回: [録画] の入力欄にフォーカスを置いてから [ツール] を選ぶ。
	{
		const QString label = QStringLiteral("縮めたウィンドウ (1) [録画] の入力欄にフォーカス、配信中に [ツール]");
		auto field = std::make_shared<QString>();
		s.then(300, [&, field] {
			set_ready();
			sim.obs().uninstall.obs_streaming = true;
			sim.changed();
			sim.tick();
			host.uninstall_confirm_choice = -2;
			host.open_settings(SettingsPage::Recording, false);
			shrink_settings();
			*field = focus_page_field();
			Input::activate_window(main_window());
			host.open_settings_to_uninstall();
		});
		s.then(500, [&, label, field] {
			QPushButton *b = uninstall_button_of();
			results.expect_true(group, label + QStringLiteral(": 前提: [録画] の入力欄にフォーカスを置け、縮めたウィンドウでスクロールの範囲が 0 より大きい"),
					    !field->isEmpty() && scrollable(),
					    QStringLiteral("入力欄 %1 / %2").arg(field->isEmpty() ? QStringLiteral("(置けない)") : *field, scroll_range_text()));
			results.expect_true(group, label + QStringLiteral(": 確認のダイアログを表示せず、ページのボタンが押せない"),
					    find_message_dialog(host.settings) == nullptr && host.uninstall_confirm_choice == -2 && b != nullptr &&
						    !b->isEnabled());
			QWidget *reason = reason_label_now();
			results.expect_true(group, label + QStringLiteral(": ボタンとその下の押せない理由が表示範囲にある"),
					    shown_in_viewport(b) && reason != nullptr && shown_in_viewport(reason), scroll_range_text());
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットが表示範囲にある"), focus_visible(),
					    QStringLiteral("フォーカス %1 / %2").arg(window_focus_text(), scroll_range_text()));
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットがスクロール領域 (QScrollArea) でない"), focus_not_box(),
					    QStringLiteral("フォーカス %1").arg(window_focus_text()));
			results.expect_true(group, label + QStringLiteral(": [録画] の入力欄にあったフォーカスが、ページを替えた後は項目の一覧にある"),
					    focus_on_page_list(), QStringLiteral("フォーカス %1").arg(window_focus_text()));
			if (!host.settings.isNull())
				host.settings->close();
		});
	}
	// (2) ページのボタンから押し、取り直しで断る。
	{
		const QString label = QStringLiteral("縮めたウィンドウ (2) ページのボタンから押し、取り直しで断る");
		auto pre_ok = std::make_shared<bool>(false);
		auto pre = std::make_shared<QString>();
		s.then(300, [&, pre, pre_ok] {
			set_ready();
			host.uninstall_confirm_choice = -2;
			host.uninstall_refused = 0;
			host.open_settings(SettingsPage::About, false);
			pump(100);
			shrink_settings();
			scroll_about_bottom();
			QPushButton *b = uninstall_button_of();
			*pre_ok = scrollable() && shown_in_viewport(b);
			*pre = scroll_range_text();
			host.streaming_starts_before_confirm = true;
			click_widget(input, b);
		});
		s.then(500, [&, label, pre, pre_ok] {
			QPushButton *b = uninstall_button_of();
			results.expect_true(group, label + QStringLiteral(": 前提: 縮めたウィンドウでいちばん下までスクロールし、表示されているボタンを押して取り直しで断った"),
					    *pre_ok && host.uninstall_refused == 1,
					    QStringLiteral("押す前 %1 / 断った数 %2").arg(*pre).arg(host.uninstall_refused));
			results.expect_true(group, label + QStringLiteral(": 確認のダイアログを表示せず、次の周期でページのボタンが押せなくなる"),
					    find_message_dialog(host.settings) == nullptr && host.uninstall_confirm_choice == -2 && b != nullptr &&
						    !b->isEnabled());
			QWidget *reason = reason_label_now();
			results.expect_true(group, label + QStringLiteral(": 加わった押せない理由が表示範囲にある"),
					    reason != nullptr && shown_in_viewport(reason), scroll_range_text());
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットが表示範囲にある"), focus_visible(),
					    QStringLiteral("フォーカス %1 / %2").arg(window_focus_text(), scroll_range_text()));
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットがスクロール領域 (QScrollArea) でない"), focus_not_box(),
					    QStringLiteral("フォーカス %1").arg(window_focus_text()));
			if (!host.settings.isNull())
				host.settings->close();
			sim.obs().obs_outputs.streaming = false;
			sim.changed();
		});
	}
	// (2) ページのボタンから押してアンインストールを終え、結果のダイアログを閉じる。
	{
		const QString label = QStringLiteral("縮めたウィンドウ (2) ページのボタンからアンインストールを終えて結果のダイアログを閉じる");
		auto pre_ok = std::make_shared<bool>(false);
		auto pre = std::make_shared<QString>();
		s.then(300, [&, pre, pre_ok] {
			set_ready();
			host.uninstall_confirm_choice = -2;
			host.uninstall_results = 0;
			host.open_settings(SettingsPage::About, false);
			pump(100);
			shrink_settings();
			scroll_about_bottom();
			*pre_ok = scrollable() && shown_in_viewport(uninstall_button_of());
			*pre = scroll_range_text();
		});
		// 確認のダイアログが開く (閉じるまで戻らない)。
		s.then_blocking(200, [&] { click_widget(input, uninstall_button_of()); });
		s.then_blocking(500, [&] {
			QWidget *d = find_message_dialog(host.settings);
			const dock::ui::MessageSpec spec = dock::uninstall_confirm_spec();
			QPushButton *go = nullptr;
			if (d != nullptr && !spec.buttons.empty()) {
				for (QPushButton *p : d->findChildren<QPushButton *>()) {
					if (p->text() == spec.buttons.front().text)
						go = p;
				}
			}
			click_widget(input, go); // [アンインストール] -> 終えて結果のダイアログ (閉じるまで戻らない)
		});
		s.then(500, [&, label, pre, pre_ok] {
			results.expect_true(group, label + QStringLiteral(": 前提: 縮めたウィンドウで表示されているボタンを押し、アンインストールを終えて結果のダイアログが表示された"),
					    *pre_ok && host.uninstall_confirm_choice == 0 && host.uninstall_results == 1,
					    QStringLiteral("押す前 %1 / 答え %2 / 結果のダイアログ %3")
						    .arg(*pre)
						    .arg(host.uninstall_confirm_choice)
						    .arg(host.uninstall_results));
			if (auto *result = qobject_cast<QDialog *>(find_message_dialog(host.settings)))
				result->reject();
			else if (auto *result2 = qobject_cast<QDialog *>(find_message_dialog(sim.widget())))
				result2->reject();
		});
		s.then(400, [&, label] {
			const QString copy = dock::ui::resolve_key(keys::kSettingsCopyButton);
			auto *fb = qobject_cast<QAbstractButton *>(window_focus());
			results.info(group, label + QStringLiteral(": 結果のダイアログを閉じた後のフォーカス (戻り先は [コピー])"), window_focus_text(),
				     fb != nullptr && fb->text() == copy ? QStringLiteral("[コピー]") : QStringLiteral("[コピー] 以外"));
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットが表示範囲にある"), focus_visible(),
					    QStringLiteral("フォーカス %1 / %2").arg(window_focus_text(), scroll_range_text()));
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットがスクロール領域 (QScrollArea) でない"), focus_not_box(),
					    QStringLiteral("フォーカス %1").arg(window_focus_text()));
			if (!host.settings.isNull())
				host.settings->close();
		});
	}
	// (3) 途中で止まった回 (Stopped) の押し直しを [ツール] から選び、取り直しで断る。
	{
		const QString label = QStringLiteral("縮めたウィンドウ (3) 途中で止まった後の押し直しを [ツール] から選び、取り直しで断る");
		s.then(300, [&] {
			set_ready();
			sim.obs().uninstall.phase = ledger::UninstallPhase::Stopped;
			sim.changed();
			sim.tick();
			host.uninstall_confirm_choice = -2;
			host.uninstall_refused = 0;
			host.open_settings(SettingsPage::Recording, false);
			shrink_settings();
			Input::activate_window(main_window());
			host.streaming_starts_before_confirm = true;
			host.open_settings_to_uninstall();
		});
		s.then(500, [&, label] {
			QPushButton *b = uninstall_button_of();
			results.expect_true(group, label + QStringLiteral(": 前提: 入力欄のページを操作できなくしている間に押せる見た目で選び、取り直しで断った (スクロールの範囲が 0 より大きい)"),
					    sim.view().about.settings_locked && host.uninstall_refused == 1 && scrollable(),
					    QStringLiteral("断った数 %1 / %2").arg(host.uninstall_refused).arg(scroll_range_text()));
			results.expect_true(group, label + QStringLiteral(": 確認のダイアログを表示せず、次の周期でページのボタンが押せなくなる"),
					    find_message_dialog(host.settings) == nullptr && host.uninstall_confirm_choice == -2 && b != nullptr &&
						    !b->isEnabled());
			QWidget *reason = reason_label_now();
			results.expect_true(group, label + QStringLiteral(": ボタンとその下の押せない理由が表示範囲にある"),
					    shown_in_viewport(b) && reason != nullptr && shown_in_viewport(reason), scroll_range_text());
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットが表示範囲にある"), focus_visible(),
					    QStringLiteral("フォーカス %1 / %2").arg(window_focus_text(), scroll_range_text()));
			results.expect_true(group, label + QStringLiteral(": フォーカスのあるウィジェットがスクロール領域 (QScrollArea) でない"), focus_not_box(),
					    QStringLiteral("フォーカス %1").arg(window_focus_text()));
			if (!host.settings.isNull())
				host.settings->close();
			sim.obs().obs_outputs.streaming = false;
			sim.changed();
		});
	}

	// ---- 反映していない変更を残したままアンインストールし、[OK] を押す ----
	// [詳細設定] で「配信中の退出を確認」を切り替えて [適用] せずに、[ツール] からアンインストールする ([アンインストール] で
	// 答える) -> 結果のダイアログを閉じる -> 設定ウィンドウの [OK] を押す。アンインストールの後は、
	// ウィンドウの値を何も適用しない (操作できなくして見えなくなった入力欄の値が動作するのを防ぐ)。
	// 確認すること: アンインストールの後は入力欄のページが操作できなくなり、[適用] が押せない。[OK] でウィンドウが
	// 閉じる。[OK] がウィンドウの値を渡さない (設定ウィンドウの [OK] のガード。apply_calls が増えない)。「配信中の退出を確認」が
	// 変わらない (sim.settings()。Host が製品の apply_from_window のガードと同じ処理をするので、ウィンドウのガードが
	// なくてもここは変わらない。2 つのガードを分けて見る)。
	const QString locked_ok = QStringLiteral("反映していない変更を残してアンインストールし [OK]");
	auto confirm_before = std::make_shared<bool>(false);
	auto applies_before = std::make_shared<int>(0);
	auto confirm_check_of = [&]() -> QCheckBox * {
		const QString text = dock::ui::resolve_key(keys::kSettingsConfirmWhileStreamingCheck);
		if (host.settings.isNull())
			return nullptr;
		for (QCheckBox *c : host.settings->findChildren<QCheckBox *>()) {
			if (c->text() == text)
				return c;
		}
		return nullptr;
	};
	s.then(300, [&, confirm_before, locked_ok] {
		set_ready();
		sim.settings().confirm_while_streaming = true;
		sim.changed();
		sim.tick();
		*confirm_before = sim.settings().confirm_while_streaming;
		host.uninstall_confirm_choice = -2;
		host.uninstall_results = 0;
		host.open_settings(SettingsPage::Advanced, false);
		pump(100);
		QCheckBox *c = confirm_check_of();
		results.expect_true(group, locked_ok + QStringLiteral(": 前提: [詳細設定] に「配信中の退出を確認」のチェックがあり、オンになっている"),
				    c != nullptr && c->isVisible() && c->isChecked(),
				    QStringLiteral("ページ %1 / 押す位置の下 %2")
					    .arg(!host.settings.isNull() && host.settings->page() == SettingsPage::Advanced ? QStringLiteral("詳細設定")
															  : QStringLiteral("ほか"))
					    .arg(c != nullptr ? name_of_widget(Input::widget_at(c->mapToGlobal(c->rect().center()))) : QString()));
		// チェックボックスの上を押す (QCheckBox は入力欄の行の幅まで伸び、チェックボックスとテキストの外の所は
		// 押しても切り替わらない。hitButton)。
		if (c != nullptr) {
			input.move_to(c->mapToGlobal(QPoint(c->height() / 2, c->height() / 2)));
			input.click();
		}
	});
	s.then(200, [&, locked_ok] {
		QPushButton *apply = box_button(host.settings, QDialogButtonBox::Apply);
		results.expect_true(group, locked_ok + QStringLiteral(": 前提: 切り替えた後、[適用] を押せる (反映していない変更がある)"),
				    confirm_check_of() != nullptr && !confirm_check_of()->isChecked() && apply != nullptr && apply->isEnabled(),
				    QStringLiteral("チェック %1 / [適用] %2 / フォーカス %3")
					    .arg(confirm_check_of() != nullptr && !confirm_check_of()->isChecked() ? QStringLiteral("切れている") : QStringLiteral("入っている"))
					    .arg(apply != nullptr && apply->isEnabled() ? QStringLiteral("押せる") : QStringLiteral("押せない"))
					    .arg(describe_focus()));
		host.open_settings_to_uninstall();
	});
	s.then(500, [&, locked_ok] {
		QWidget *d = find_message_dialog(host.settings);
		const dock::ui::MessageSpec spec = dock::uninstall_confirm_spec();
		QPushButton *go = nullptr;
		if (d != nullptr && !spec.buttons.empty()) {
			for (QPushButton *p : d->findChildren<QPushButton *>()) {
				if (p->text() == spec.buttons.front().text)
					go = p;
			}
		}
		results.expect_true(group, locked_ok + QStringLiteral(": 確認のダイアログに [アンインストール] がある"), go != nullptr);
		click_widget(input, go);
	});
	s.then(500, [&, applies_before, locked_ok] {
		results.expect_true(group, locked_ok + QStringLiteral(": [アンインストール] でアンインストールを終え、結果のダイアログが表示される"),
				    host.uninstall_confirm_choice == 0 && host.uninstall_results == 1 &&
					    sim.obs().uninstall.phase == ledger::UninstallPhase::Done,
				    QStringLiteral("答え %1 / 結果のダイアログ %2").arg(host.uninstall_confirm_choice).arg(host.uninstall_results));
		if (auto *result = qobject_cast<QDialog *>(find_message_dialog(host.settings)))
			result->reject();
		else if (auto *result2 = qobject_cast<QDialog *>(find_message_dialog(sim.widget())))
			result2->reject();
		*applies_before = host.apply_calls;
	});
	s.then(400, [&, applies_before, confirm_before, locked_ok] {
		QPushButton *apply = box_button(host.settings, QDialogButtonBox::Apply);
		QPushButton *ok = box_button(host.settings, QDialogButtonBox::Ok);
		results.expect_true(group, locked_ok + QStringLiteral(": アンインストールの後は [適用] を押せない (入力欄のページを操作できなくする)"),
				    apply != nullptr && !apply->isEnabled());
		results.expect_true(group, locked_ok + QStringLiteral(": [OK] を押せる"), ok != nullptr && ok->isEnabled());
		click_widget(input, ok);
	});
	s.then(300, [&, applies_before, confirm_before, locked_ok] {
		results.expect_true(group, locked_ok + QStringLiteral(": [OK] でウィンドウが閉じる"),
				    host.settings.isNull() || !host.settings->isVisible());
		results.expect_true(group, locked_ok + QStringLiteral(": [OK] がウィンドウの値を渡さない (設定ウィンドウのガード)"),
				    host.apply_calls == *applies_before,
				    QStringLiteral("ウィンドウから適用した数 %1 -> %2").arg(*applies_before).arg(host.apply_calls));
		results.expect_true(group, locked_ok + QStringLiteral(": 「配信中の退出を確認」が変わらない"),
				    sim.settings().confirm_while_streaming == *confirm_before);
		if (!host.settings.isNull())
			host.settings->close();
	});
	s.then(300, [&, before] {
		sim.obs() = *before;
		sim.changed();
		sim.tick();
	});
	const bool finished = s.run(60000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	sim.on_tick = nullptr;
	sim.stop();
	if (!host.settings.isNull())
		delete host.settings.data();
	sim.dock()->hide();
}

// ---- pages: 設定ウィンドウのページの一覧でページを替えたときのフォーカスと、タブストップ ----

namespace {

// フォーカスのあるウィジェットが表示範囲にあるか (WCAG 2.4.11)。スクロール領域の中のウィジェットはスクロール領域の
// 表示範囲に、外のウィジェットはウィンドウの中にあるかを見る。表示範囲より高い (広い) ウィジェットは、
// その向きにはすべてが入らないので、一部が表示されていればよい (すべては隠れていない)。
// shown_in_viewport (すべてが入ることを見る) をそのまま使う方法はとらない。表示範囲より高い入力欄が 1 つあると、
// どうスクロールしても合格しないためである。
bool focus_in_view(QWidget *w)
{
	if (w == nullptr || !w->isVisible())
		return false;
	QWidget *frame = w->window();
	for (QWidget *p = w->parentWidget(); p != nullptr; p = p->parentWidget()) {
		if (auto *area = qobject_cast<QScrollArea *>(p)) {
			frame = area->viewport();
			break;
		}
	}
	const QRect view = frame->rect();
	const QRect r(w->mapTo(frame, QPoint(0, 0)), w->size());
	auto fits = [](int lo, int hi, int vlo, int vhi) {
		return hi - lo > vhi - vlo ? (hi >= vlo && lo <= vhi) : (lo >= vlo && hi <= vhi);
	};
	return fits(r.top(), r.bottom(), view.top(), view.bottom()) && fits(r.left(), r.right(), view.left(), view.right());
}

} // namespace

// pages: 設定ウィンドウの左の一覧でページを替えても、フォーカスは一覧に残る (OBS の設定ウィンドウと同じ)。
// ページのスクロール領域 (QScrollArea) はタブストップでない (止まっても操作できるものがなく、Tab が 1 回むだになる)。
// 確認すること:
//   (1) マウス: 一覧の項目を押してページを替えた後、押したページが表示され、フォーカスが一覧にある
//       (6 ページを 1 周する)。
//   (2) マウス: 現在のページの入力欄にフォーカスを置いてから、別のページの項目を押した後も同じ (入力欄を触っていた
//       利用者が一覧へ移る状態)。
//   (3) キーボード: 一覧で下矢印を 5 回、上矢印を 5 回押す。1 回ごとに同じ。
//   (4) どのページでも、一覧から Tab と Shift+Tab で 1 周したとき、スクロール領域に止まらない。1 周して一覧へ戻る。
//   (5) 1 周で移ったウィジェットは、どれも表示範囲にある。ウィンドウは最小の高さまで縮め、スクロールの範囲が
//       0 より大きいページがあることを先に確認する (縮めないと中身がウィンドウに収まり、スクロールしなくても合格する)。
//       ページへ入る 1 回で表示される所までスクロールするかを見るため、Tab の 1 周はスクロール領域をいちばん
//       下にしてから、Shift+Tab の 1 周はいちばん上にしてから始める (ページの中の
//       1 回は QScrollArea::focusNextPrevChild がスクロールする。外から入る 1 回はそこを通らない)。
//   (6) ドックのメニュー ([設定]、エクスポート、[ツール]) から別のページを開く処理 (SettingsWindow::set_page) でも、
//       替える前のページの入力欄にフォーカスがあったなら、フォーカスは一覧に移る。
//   (7) 同じ処理で、フォーカスがページの外 ([OK] などのボタンのバー) にあったなら動かさない。
//   (8) フォーカスリングが表示範囲の端で切れない。(4) の 1 周で記録したタブストップの順で隣り合う (a, b) で、
//       b がページのスクロール領域の中なら、a にフォーカスを置き、スクロールして b を表示範囲の上か下の端から 0、
//       1、2 px の位置に置き (マウスのホイールでスクロールした後の状態)、同じキーで b へ移る。移った
//       b のフォーカスリングのウィジェット (dock/ui/focus_ring.h) の矩形が、表示範囲に縦に収まっていれば合格。
//       ウィジェットはすべて表示されているので、ウィジェットを表示するためのスクロールは起きず、フォーカスリング
//       (押せる範囲の外へ、線の太さと空きの分だけ広い) だけが切れる状態を確認する。(5) の
//       1 周で移ったウィジェットにも、同じフォーカスリングの確認を行う。横は合否にしない (横のスクロールはなく、
//       縦にスクロールしても横は直らない。切れたときは記録する)。リングの線の太さはここに書かず、リングのウィジェットの
//       geometry を見る (太さが変わっても確認は変えない)。
// フォーカスを一覧に残す実装 (一覧とスクロール領域のフォーカスの扱い) は製品が決めるもので、テストは動作だけを見る。
// スクロール領域の focusPolicy を見る方法はとらない。NoFocus と ClickFocus のどちらを選んでも動作は同じで、
// テストが実装を縛るためである。
void run_settings_pages(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("settings-pages");
	SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	Host host;
	host.sim = &sim;
	host.effective_at_start = sim.settings();
	sim.on_tick = [&](const ledger::StabilizeResult &st) { host.refresh_windows(st.view); };
	sim.start();

	// 一覧の並び (dock/settings_window.cpp の kPages と同じ順)。
	const std::vector<SettingsPage> order{SettingsPage::Recording, SettingsPage::Receive, SettingsPage::Advanced,
					      SettingsPage::About};
	const int count = static_cast<int>(order.size());
	auto list_of = [&]() -> QListWidget * {
		return host.settings.isNull() ? nullptr
					      : host.settings->findChild<QListWidget *>(QStringLiteral("cordmuxSettingsPageList"));
	};
	auto page_text = [&](int i) -> QString {
		QListWidget *list = list_of();
		return list != nullptr && list->item(i) != nullptr ? list->item(i)->text() : QString::number(i);
	};
	auto page_index_now = [&]() -> int {
		if (host.settings.isNull())
			return -1;
		for (int i = 0; i < count; ++i) {
			if (host.settings->page() == order[i])
				return i;
		}
		return -1;
	};
	auto focus_text = [](QWidget *f) -> QString {
		return f != nullptr ? name_of_widget(f) + QLatin1Char('/') + QString::fromLatin1(f->metaObject()->className())
				    : QStringLiteral("(none)");
	};
	// 現在のページのスクロール領域 (表示されている QScrollArea。隠れたページのスクロール領域は表示されていない)。
	auto page_scroll = [&]() -> QScrollArea * {
		if (host.settings.isNull())
			return nullptr;
		for (QScrollArea *a : host.settings->findChildren<QScrollArea *>()) {
			if (a->isVisible())
				return a;
		}
		return nullptr;
	};
	auto scroll_text = [&]() -> QString {
		QScrollArea *a = page_scroll();
		return a != nullptr ? QStringLiteral("スクロール %1 / %2").arg(a->verticalScrollBar()->value()).arg(a->verticalScrollBar()->maximum())
				    : QStringLiteral("(スクロール領域なし)");
	};
	// ウィジェットの位置 (スクロール領域の表示範囲の座標での上端と下端と、表示範囲の高さ。表示されていないときの
	// 切り分けに使う)。
	auto where_text = [&](QWidget *w) -> QString {
		QScrollArea *a = page_scroll();
		if (w == nullptr || a == nullptr || !a->viewport()->isAncestorOf(w))
			return QStringLiteral("スクロール領域の外");
		const QPoint top = w->mapTo(a->viewport(), QPoint(0, 0));
		return QStringLiteral("ウィジェット %1-%2 / 表示範囲 0-%3%4")
			.arg(top.y())
			.arg(top.y() + w->height() - 1)
			.arg(a->viewport()->height() - 1)
			.arg(w->isVisible() ? QString() : QStringLiteral(" / 隠れている"));
	};
	auto scroll_to = [&](bool bottom) {
		if (QScrollArea *a = page_scroll())
			a->verticalScrollBar()->setValue(bottom ? a->verticalScrollBar()->maximum() : 0);
		pump(50);
	};
	// 一覧の i 番目の項目をマウスで押す (最小の高さのウィンドウでは一覧もスクロールすることがあるので、
	// 先に項目を表示する)。
	auto click_item = [&](int i) {
		QListWidget *list = list_of();
		if (list == nullptr || list->item(i) == nullptr)
			return;
		list->scrollToItem(list->item(i));
		pump(30);
		const QRect r = list->visualItemRect(list->item(i));
		const QPoint at = list->viewport()->mapToGlobal(r.center());
		if (Input::window_at(at) != list->window()->windowHandle())
			Input::activate_window(list->window());
		input.move_to(at);
		input.click();
	};
	// 現在のページの入力欄 (テキスト、数値、選択、チェック) で、表示されていて操作できる最初のものに
	// フォーカスを置く。置けたウィジェットを返す。
	auto focus_page_field = [&]() -> QWidget * {
		if (host.settings.isNull())
			return nullptr;
		for (QWidget *c : host.settings->findChildren<QWidget *>()) {
			const bool field = qobject_cast<QLineEdit *>(c) != nullptr || qobject_cast<QAbstractSpinBox *>(c) != nullptr ||
					   qobject_cast<QComboBox *>(c) != nullptr || qobject_cast<QCheckBox *>(c) != nullptr;
			if (field && c->isVisible() && c->isEnabled() && (c->focusPolicy() & Qt::TabFocus) != 0) {
				c->setFocus(Qt::TabFocusReason);
				pump(50);
				return QApplication::focusWidget() == c ? c : nullptr;
			}
		}
		return nullptr;
	};

	// 合否を決めるための記録 (手順をまたいでためる)。
	struct Acc {
		QStringList mouse_miss;     // (1)
		QStringList field_miss;     // (2)
		QStringList field_skipped;  // (2) で入力欄がなかったページ
		QStringList key_miss;       // (3)
		QStringList menu_miss;      // (6)
		QStringList menu_skipped;   // (6) で入力欄がなかったページ
		QStringList bar_miss;       // (7)
		int menu_checks = 0;
		int bar_checks = 0;
		QStringList scroll_stops;   // (4) スクロール領域に止まった所
		QStringList not_closed;     // (4) 1 周で一覧へ戻らなかった所
		QStringList hidden_tab;     // (5) Tab で移って表示されていないウィジェット
		QStringList hidden_backtab; // (5) Shift+Tab で移って表示されていないウィジェット
		QStringList trail_tab;      // 記録: ページごとの Tab のタブストップの数と、最初のタブストップ
		QStringList trail_backtab;
		QStringList scrollable;     // 縮めたウィンドウで、スクロールの範囲が 0 より大きいページ
		int mouse_checks = 0;
		int key_checks = 0;
		int field_checks = 0;
		// (8) と (5) のフォーカスリング
		std::map<int, std::vector<QPointer<QWidget>>> seq_tab;     // ページごとの Tab の順 (一覧から)
		std::map<int, std::vector<QPointer<QWidget>>> seq_backtab; // 同じく Shift+Tab
		QStringList ring_walk_cut;  // (5) の 1 周で移って、フォーカスリングが縦に切れたウィジェット
		QStringList ring_edge_tab;  // (8) Tab で外れた回
		QStringList ring_edge_backtab;
		QStringList edge_unset;     // (8) a へのフォーカスかスクロールを設定できなかった (手順の不具合)
		std::set<QString> ring_side; // 記録だけ: フォーカスリングが横に切れたウィジェット
		std::set<int> ring_pad;     // 記録だけ: 押せる範囲の上の辺から、リングの外側の上の辺までの px
		int edge_tab_checks = 0;
		int edge_backtab_checks = 0;
		int edge_skipped = 0;       // スクロールしても置けない位置 (範囲の外) か、表示範囲より高いウィジェット
	};
	auto acc = std::make_shared<Acc>();

	// フォーカスリング (設定ウィンドウの直下に 1 つある。dock/ui/focus_ring.h) のうち、ウィジェット
	// f を追っているもの。
	auto ring_on = [&](QWidget *f) -> dock::ui::FocusRing * {
		if (host.settings.isNull() || f == nullptr)
			return nullptr;
		for (QWidget *w : host.settings->findChildren<QWidget *>(QStringLiteral("cordmuxFocusRing"), Qt::FindDirectChildrenOnly)) {
			auto *r = dynamic_cast<dock::ui::FocusRing *>(w);
			if (r != nullptr && r->target() == f)
				return r;
		}
		return nullptr;
	};
	// 現在のページのスクロール領域の中のウィジェット f のフォーカスリングが、表示範囲に縦に収まっているか。
	// 収まっていれば空を、外れていればその内容を返す。ページのスクロール領域の外のウィジェット (一覧、ボタンのバー)
	// は見ない (空を返す)。リングが表示範囲より高ければ、一部が表示されていればよい。横に切れていれば記録に加える
	// (合否にしない)。
	auto ring_cut = [&](QWidget *f) -> QString {
		QScrollArea *a = page_scroll();
		if (a == nullptr || a->widget() == nullptr || f == nullptr || !a->widget()->isAncestorOf(f))
			return QString();
		dock::ui::FocusRing *r = ring_on(f);
		if (r == nullptr || !r->isVisible())
			return QStringLiteral("リングが表示されていない");
		QWidget *vp = a->viewport();
		const QRect view(vp->mapTo(host.settings.data(), QPoint(0, 0)), vp->size());
		const QRect ring = r->geometry();
		const QRect hit = dock::ui::focus_hit_rect_of(f);
		acc->ring_pad.insert(f->mapTo(host.settings.data(), hit.topLeft()).y() - ring.top());
		if (ring.left() < view.left() || ring.right() > view.right())
			acc->ring_side.insert(focus_text(f) + QStringLiteral(" (リング x %1-%2 / 表示範囲 x 0-%3)")
								     .arg(ring.left() - view.left())
								     .arg(ring.right() - view.left())
								     .arg(view.width() - 1));
		const bool in = ring.height() > view.height() ? (ring.bottom() >= view.top() && ring.top() <= view.bottom())
							      : (ring.top() >= view.top() && ring.bottom() <= view.bottom());
		if (in)
			return QString();
		return QStringLiteral("リング %1-%2 / 表示範囲 0-%3")
			.arg(ring.top() - view.top())
			.arg(ring.bottom() - view.top())
			.arg(view.height() - 1);
	};

	// 一覧からの 1 周。key は Tab か Shift+Tab。一覧へ戻るか、60 回で止める。タブストップの並び (一覧から始めて、
	// 一覧へ戻る前まで) を seq に残す ((8) が隣り合う組を使う)。
	auto walk = [&](int key, int page, QStringList &hidden, QStringList &trail, std::vector<QPointer<QWidget>> &seq) {
		QListWidget *list = list_of();
		int stops = 0;
		bool closed = false;
		QString first_stop;
		seq.clear();
		seq.emplace_back(list);
		for (int k = 0; k < 60; ++k) {
			input.key(key);
			pump(30);
			QWidget *f = QApplication::focusWidget();
			if (f != nullptr && f == list) {
				closed = true;
				break;
			}
			++stops;
			const QString name = focus_text(f);
			if (first_stop.isEmpty())
				first_stop = name;
			if (qobject_cast<QScrollArea *>(f) != nullptr)
				acc->scroll_stops << page_text(page) + QStringLiteral(" (") +
							     (key == Qt::Key_Tab ? QStringLiteral("Tab") : QStringLiteral("Shift+Tab")) +
							     QStringLiteral(" %1 回目)").arg(stops);
			seq.emplace_back(f);
			if (!focus_in_view(f))
				hidden << page_text(page) + QStringLiteral(": %1 回目 %2 (%3、%4)").arg(stops).arg(name, scroll_text(), where_text(f));
			const QString cut = ring_cut(f);
			if (!cut.isEmpty())
				acc->ring_walk_cut << page_text(page) + QStringLiteral(" (") +
							      (key == Qt::Key_Tab ? QStringLiteral("Tab") : QStringLiteral("Shift+Tab")) +
							      QStringLiteral(" %1 回目) %2: %3、%4").arg(stops).arg(name, cut, scroll_text());
		}
		if (!closed)
			acc->not_closed << page_text(page) + QStringLiteral(" (") +
						   (key == Qt::Key_Tab ? QStringLiteral("Tab") : QStringLiteral("Shift+Tab")) + QLatin1Char(')');
		trail << page_text(page) + QStringLiteral(": %1 か所、最初 %2").arg(stops).arg(first_stop);
	};

	Script s;
	// ウィンドウを [録画] で開き、最小の高さまで縮める (利用者がウィンドウの下端をドラッグして低くしたのと同じ)。
	s.then(300, [&] {
		host.open_settings(SettingsPage::Recording, false);
		if (!host.settings.isNull())
			host.settings->resize(host.settings->width(), host.settings->minimumHeight());
		pump(100);
	});
	s.then(200, [&, acc] {
		QListWidget *list = list_of();
		results.expect_true(group, QStringLiteral("前提: 一覧に 4 ページが並ぶ"), list != nullptr && list->count() == count,
				    list != nullptr ? QStringLiteral("%1 項目").arg(list->count()) : QStringLiteral("(一覧なし)"));
		// 各ページのスクロールの範囲を数える (ページを替えるのは製品の関数で行う。フォーカスを動かさない形で
		// 数だけ見る)。
		for (int i = 0; i < count && !host.settings.isNull(); ++i) {
			host.settings->set_page(order[i]);
			pump(50);
			QScrollArea *a = page_scroll();
			if (a != nullptr && a->verticalScrollBar()->maximum() > 0)
				acc->scrollable << page_text(i) + QStringLiteral(" (%1)").arg(a->verticalScrollBar()->maximum());
		}
		if (!host.settings.isNull())
			host.settings->set_page(SettingsPage::Recording);
		pump(50);
		results.expect_true(group, QStringLiteral("前提: 縮めたウィンドウで、スクロールの範囲が 0 より大きいページがある"),
				    !acc->scrollable.isEmpty(),
				    QStringLiteral("ウィンドウの高さ %1 / %2")
					    .arg(host.settings.isNull() ? 0 : host.settings->height())
					    .arg(acc->scrollable.isEmpty() ? QStringLiteral("(なし)") : acc->scrollable.join(QStringLiteral("、"))));
		// 一覧の [録画] を押して、フォーカスを一覧へ置く (利用者が一覧を押した状態)。
		click_item(0);
	});
	s.then(150, [&] {
		results.expect_true(group, QStringLiteral("前提: 一覧を押すと、フォーカスが一覧に移る"),
				    list_of() != nullptr && QApplication::focusWidget() == list_of(),
				    QStringLiteral("フォーカス %1").arg(focus_text(QApplication::focusWidget())));
	});
	// (1) マウスで 1 周する (2 番目から最後へ、最後に [録画] へ戻る)。
	for (int step = 1; step <= count; ++step) {
		const int target = step % count;
		s.then(100, [&, target] { click_item(target); });
		s.then(150, [&, acc, target] {
			++acc->mouse_checks;
			if (page_index_now() != target || QApplication::focusWidget() != list_of())
				acc->mouse_miss << page_text(target) + QStringLiteral(": 表示されているページ %1 / フォーカス %2")
									.arg(page_text(page_index_now()), focus_text(QApplication::focusWidget()));
		});
	}
	// (2) ページの入力欄にフォーカスを置いてから、次のページの項目を押す。
	for (int from = 0; from < count; ++from) {
		const int target = (from + 1) % count;
		s.then(100, [&, acc, from, target] {
			click_item(from);
			pump(150);
			if (focus_page_field() == nullptr) {
				acc->field_skipped << page_text(from);
				return;
			}
			click_item(target);
		});
		s.then(150, [&, acc, from, target] {
			if (acc->field_skipped.contains(page_text(from)))
				return;
			++acc->field_checks;
			if (page_index_now() != target || QApplication::focusWidget() != list_of())
				acc->field_miss << page_text(from) + QStringLiteral(" の入力欄から ") + page_text(target) +
							   QStringLiteral(": 表示されているページ %1 / フォーカス %2")
								   .arg(page_text(page_index_now()), focus_text(QApplication::focusWidget()));
		});
	}
	// (3) 一覧で下矢印を 5 回、上矢印を 5 回押す。
	s.then(100, [&] { click_item(0); });
	for (int step = 0; step < 2 * (count - 1); ++step) {
		const bool down = step < count - 1;
		const int target = down ? step + 1 : (2 * (count - 1)) - step - 1;
		s.then(100, [&, down] { input.key(down ? Qt::Key_Down : Qt::Key_Up); });
		s.then(150, [&, acc, target, down] {
			++acc->key_checks;
			if (page_index_now() != target || QApplication::focusWidget() != list_of())
				acc->key_miss << (down ? QStringLiteral("下矢印で ") : QStringLiteral("上矢印で ")) + page_text(target) +
							 QStringLiteral(": 表示されているページ %1 / フォーカス %2")
								 .arg(page_text(page_index_now()), focus_text(QApplication::focusWidget()));
		});
	}
	// (6) ページの入力欄にフォーカスを置いてから、ドックのメニューからの処理 (Host::open_settings。
	// 製品の open_settings と同じく、開いているウィンドウの set_page) で次のページを開く。
	for (int from = 0; from < count; ++from) {
		const int target = (from + 1) % count;
		s.then(100, [&, acc, from, target] {
			click_item(from);
			pump(150);
			if (focus_page_field() == nullptr) {
				acc->menu_skipped << page_text(from);
				return;
			}
			host.open_settings(order[target], false);
		});
		s.then(150, [&, acc, from, target] {
			if (acc->menu_skipped.contains(page_text(from)))
				return;
			++acc->menu_checks;
			if (page_index_now() != target || QApplication::focusWidget() != list_of())
				acc->menu_miss << page_text(from) + QStringLiteral(" の入力欄から ") + page_text(target) +
							  QStringLiteral(": 表示されているページ %1 / フォーカス %2")
								  .arg(page_text(page_index_now()), focus_text(QApplication::focusWidget()));
		});
	}
	// (7) [キャンセル] にフォーカスを置いてから、同じ処理で次のページを開く (フォーカスはページの外にあるので
	// 動かさない)。
	for (int from = 0; from < count; ++from) {
		const int target = (from + 1) % count;
		s.then(100, [&, from, target] {
			click_item(from);
			pump(150);
			if (QPushButton *c = box_button(host.settings, QDialogButtonBox::Cancel))
				c->setFocus(Qt::TabFocusReason);
			pump(50);
			host.open_settings(order[target], false);
		});
		s.then(150, [&, acc, target] {
			++acc->bar_checks;
			QPushButton *c = box_button(host.settings, QDialogButtonBox::Cancel);
			if (page_index_now() != target || c == nullptr || QApplication::focusWidget() != c)
				acc->bar_miss << page_text(target) + QStringLiteral(": 表示されているページ %1 / フォーカス %2")
									.arg(page_text(page_index_now()), focus_text(QApplication::focusWidget()));
		});
	}
	// (4) (5) ページごとに、一覧から Tab で 1 周し (スクロール領域をいちばん下にして)、Shift+Tab で
	// 1 周する (いちばん上にして)。
	for (int page = 0; page < count; ++page) {
		s.then(100, [&, page] { click_item(page); });
		s.then(150, [&, acc, page] {
			scroll_to(true);
			if (QApplication::focusWidget() != list_of())
				click_item(page);
			pump(50);
			walk(Qt::Key_Tab, page, acc->hidden_tab, acc->trail_tab, acc->seq_tab[page]);
		});
		s.then(100, [&, page] {
			scroll_to(false);
			if (QApplication::focusWidget() != list_of())
				click_item(page);
		});
		s.then(150, [&, acc, page] {
			walk(Qt::Key_Backtab, page, acc->hidden_backtab, acc->trail_backtab, acc->seq_backtab[page]);
		});
	}
	// (8) 1 回の確認: ウィジェット b を表示範囲の端 (top なら上、そうでなければ下) から d px の位置に置き、
	// a にフォーカスを置いてから key で b へ移る。b がページのスクロール領域の外 (ボタンのバー、
	// 一覧) なら確認しない。スクロールしても置けない位置と、表示範囲より高いウィジェットは、飛ばして数を記録する。
	auto edge_trial = [&](int key, int page, QWidget *a, QWidget *b, bool top, int d, QStringList &miss, int &checks) {
		QScrollArea *sa = page_scroll();
		if (sa == nullptr || sa->widget() == nullptr || b == nullptr || !sa->widget()->isAncestorOf(b))
			return;
		if (a == nullptr) {
			++acc->edge_skipped;
			return;
		}
		QScrollBar *bar = sa->verticalScrollBar();
		const int view_h = sa->viewport()->height();
		const QRect r(b->mapTo(sa->widget(), QPoint(0, 0)), b->size());
		const int value = top ? r.top() - d : r.bottom() + 1 + d - view_h;
		if (r.height() > view_h || value < bar->minimum() || value > bar->maximum()) {
			++acc->edge_skipped;
			return;
		}
		const QString where = page_text(page) + QStringLiteral(": %1 を%2の端から %3 px に置き %4 から")
								.arg(focus_text(b), top ? QStringLiteral("上") : QStringLiteral("下"))
								.arg(d)
								.arg(focus_text(a));
		a->setFocus(Qt::TabFocusReason);
		pump(20);
		bar->setValue(value);
		pump(30);
		if (QApplication::focusWidget() != a || bar->value() != value) {
			acc->edge_unset << where + QStringLiteral(" (フォーカス %1、%2)").arg(focus_text(QApplication::focusWidget()), scroll_text());
			return;
		}
		input.key(key);
		pump(60);
		++checks;
		QWidget *f = QApplication::focusWidget();
		if (f != b) {
			miss << where + QStringLiteral(": 移った先のウィジェットが違う %1").arg(focus_text(f));
			return;
		}
		const QString cut = ring_cut(f);
		if (!cut.isEmpty())
			miss << where + QStringLiteral(": %1、%2").arg(cut, scroll_text());
	};
	for (int page = 0; page < count; ++page) {
		s.then(100, [&, page] { click_item(page); });
		s.then(150, [&, acc, page] {
			if (page_index_now() != page) {
				acc->edge_unset << page_text(page) + QStringLiteral(": ページが表示されていない (表示されているページ %1)").arg(page_text(page_index_now()));
				return;
			}
			const struct {
				int key;
				std::vector<QPointer<QWidget>> *seq;
				QStringList *miss;
				int *checks;
			} ways[] = {{Qt::Key_Tab, &acc->seq_tab[page], &acc->ring_edge_tab, &acc->edge_tab_checks},
				    {Qt::Key_Backtab, &acc->seq_backtab[page], &acc->ring_edge_backtab, &acc->edge_backtab_checks}};
			for (const auto &way : ways) {
				for (size_t i = 0; i + 1 < way.seq->size(); ++i) {
					for (const bool top : {true, false}) {
						for (int d = 0; d <= 2; ++d)
							edge_trial(way.key, page, (*way.seq)[i].data(), (*way.seq)[i + 1].data(), top, d, *way.miss,
								   *way.checks);
					}
				}
			}
		});
	}
	// (9) UI Automation (スクリーンリーダーや音声での操作) でページを選んだとき、そのページが表示されるか。UI Automation の
	// 選択 (SelectionItemPattern の Select) は、Qt のアクセシビリティの選択の操作 (一覧の QAccessibleSelectionInterface の
	// clear と select) になり、選択だけを変えて、現在の行 (currentRow) を変えない。
	auto a11y_miss = std::make_shared<QStringList>();
	auto a11y_checks = std::make_shared<int>(0);
	for (int page = 0; page < count; ++page) {
		s.then(150, [&, page, a11y_miss] {
			QListWidget *list = list_of();
			QAccessibleInterface *li = list != nullptr ? QAccessible::queryAccessibleInterface(list) : nullptr;
			QAccessibleSelectionInterface *sel = li != nullptr ? li->selectionInterface() : nullptr;
			QAccessibleInterface *item = li != nullptr ? li->child(page) : nullptr;
			if (sel == nullptr || item == nullptr) {
				*a11y_miss << page_text(page) + QStringLiteral(": 一覧にアクセシビリティの選択の操作がない");
				return;
			}
			sel->clear();
			sel->select(item);
		});
		s.then(150, [&, page, a11y_miss, a11y_checks] {
			++*a11y_checks;
			if (page_index_now() != page)
				*a11y_miss << page_text(page) + QStringLiteral(": 表示されているページ %1").arg(page_text(page_index_now()));
		});
	}
	s.then(100, [&, a11y_miss, a11y_checks] {
		results.expect_eq(group,
				  QStringLiteral("(9) 一覧の項目をアクセシビリティの選択の操作で選ぶと、そのページが表示される (外れた回数)"),
				  a11y_miss->size(), 0,
				  QStringLiteral("%1 回のうち。%2").arg(*a11y_checks).arg(a11y_miss->join(QStringLiteral(" | "))));
	});
	s.then(100, [&, acc] {
		auto miss_text = [](const QStringList &l) { return l.isEmpty() ? QString() : l.join(QStringLiteral(" | ")); };
		results.expect_eq(group,
				  QStringLiteral("(1) 一覧の項目を押してページを替えた後、押したページが表示され、フォーカスが一覧にある (外れた回数)"),
				  acc->mouse_miss.size(), 0,
				  QStringLiteral("%1 回のうち。%2").arg(acc->mouse_checks).arg(miss_text(acc->mouse_miss)));
		results.expect_eq(group,
				  QStringLiteral("(2) ページの入力欄にフォーカスを置いてから、別のページの項目を押しても同じ (外れた回数)"),
				  acc->field_miss.size(), 0,
				  QStringLiteral("%1 回のうち。入力欄のないページ %2。%3")
					  .arg(acc->field_checks)
					  .arg(acc->field_skipped.isEmpty() ? QStringLiteral("なし") : acc->field_skipped.join(QStringLiteral("、")),
					       miss_text(acc->field_miss)));
		results.expect_eq(group,
				  QStringLiteral("(3) 一覧で上下の矢印でページを替えた後、そのページが表示され、フォーカスが一覧にある (外れた回数)"),
				  acc->key_miss.size(), 0,
				  QStringLiteral("%1 回のうち。%2").arg(acc->key_checks).arg(miss_text(acc->key_miss)));
		results.expect_eq(group,
				  QStringLiteral("(6) ページの入力欄にフォーカスを置いてから、ドックのメニューからの処理で別のページを開くと、フォーカスが一覧にある (外れた回数)"),
				  acc->menu_miss.size(), 0,
				  QStringLiteral("%1 回のうち。入力欄のないページ %2。%3")
					  .arg(acc->menu_checks)
					  .arg(acc->menu_skipped.isEmpty() ? QStringLiteral("なし") : acc->menu_skipped.join(QStringLiteral("、")),
					       miss_text(acc->menu_miss)));
		results.expect_eq(group,
				  QStringLiteral("(7) [キャンセル] にフォーカスを置いてから、同じ処理で別のページを開くと、フォーカスが [キャンセル] のまま (外れた回数)"),
				  acc->bar_miss.size(), 0, QStringLiteral("%1 回のうち。%2").arg(acc->bar_checks).arg(miss_text(acc->bar_miss)));
		results.expect_eq(group,
				  QStringLiteral("(4) どのページでも、Tab と Shift+Tab の 1 周で、ページのスクロール領域に止まらない (止まった回数)"),
				  acc->scroll_stops.size(), 0, miss_text(acc->scroll_stops));
		results.expect_eq(group, QStringLiteral("(4) どのページでも、Tab と Shift+Tab の 1 周が一覧へ戻る (戻らなかった回数)"),
				  acc->not_closed.size(), 0, miss_text(acc->not_closed));
		results.expect_eq(group,
				  QStringLiteral("(5) スクロール領域をいちばん下にしてから、一覧から Tab で 1 周: 移ったウィジェットが表示範囲にある (外れた回数)"),
				  acc->hidden_tab.size(), 0, miss_text(acc->hidden_tab));
		results.expect_eq(group,
				  QStringLiteral("(5) スクロール領域をいちばん上にしてから、一覧から Shift+Tab で 1 周: 移ったウィジェットが表示範囲にある (外れた回数)"),
				  acc->hidden_backtab.size(), 0, miss_text(acc->hidden_backtab));
		results.expect_eq(group,
				  QStringLiteral("(5) Tab と Shift+Tab の 1 周で移ったページのウィジェットのフォーカスリングが、表示範囲に縦に収まる (外れた回数)"),
				  acc->ring_walk_cut.size(), 0, miss_text(acc->ring_walk_cut));
		results.expect_true(group, QStringLiteral("(8) 前提: ウィジェットを表示範囲の端に置いて、Tab と Shift+Tab で確認できたときがある"),
				    acc->edge_tab_checks > 0 && acc->edge_backtab_checks > 0 && acc->edge_unset.isEmpty(),
				    QStringLiteral("Tab %1 回、Shift+Tab %2 回、スクロールしても置けずに飛ばした %3 回、手順を設定できなかった %4 回%5")
					    .arg(acc->edge_tab_checks)
					    .arg(acc->edge_backtab_checks)
					    .arg(acc->edge_skipped)
					    .arg(acc->edge_unset.size())
					    .arg(acc->edge_unset.isEmpty() ? QString() : QStringLiteral("。") + miss_text(acc->edge_unset)));
		results.expect_eq(group,
				  QStringLiteral("(8) スクロールしてウィジェットを表示範囲の上か下の端から 0-2 px に置き、1 つ前のタブストップから Tab で移る: "
						 "フォーカスリングが表示範囲に縦に収まる (外れた回数)"),
				  acc->ring_edge_tab.size(), 0,
				  QStringLiteral("%1 回のうち。%2").arg(acc->edge_tab_checks).arg(miss_text(acc->ring_edge_tab)));
		results.expect_eq(group,
				  QStringLiteral("(8) 同じく Shift+Tab で移る: フォーカスリングが表示範囲に縦に収まる (外れた回数)"),
				  acc->ring_edge_backtab.size(), 0,
				  QStringLiteral("%1 回のうち。%2").arg(acc->edge_backtab_checks).arg(miss_text(acc->ring_edge_backtab)));
		QStringList pads;
		for (int p : acc->ring_pad)
			pads << QString::number(p);
		results.info(group, QStringLiteral("リングの外側の辺と押せる範囲の上の辺の間 (px。リングの線の太さと空きの和)"), QString::number(acc->ring_pad.size()),
			     pads.join(QStringLiteral("、")));
		QStringList sides(acc->ring_side.begin(), acc->ring_side.end());
		results.info(group, QStringLiteral("リングが横に切れたウィジェット (縦にスクロールしても直らない。合否にしない)"),
			     QString::number(sides.size()), sides.join(QStringLiteral(" | ")));
		results.info(group, QStringLiteral("Tab の 1 周のタブストップ (ページごとの数と最初のタブストップ)"),
			     QString::number(acc->trail_tab.size()), acc->trail_tab.join(QStringLiteral(" | ")));
		results.info(group, QStringLiteral("Shift+Tab の 1 周のタブストップ (ページごとの数と最初のタブストップ)"),
			     QString::number(acc->trail_backtab.size()), acc->trail_backtab.join(QStringLiteral(" | ")));
		if (!host.settings.isNull())
			host.settings->close();
	});
	const bool finished = s.run(240000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	sim.on_tick = nullptr;
	sim.stop();
	if (!host.settings.isNull())
		delete host.settings.data();
	sim.dock()->hide();
}

} // namespace cordmux::dock_drive

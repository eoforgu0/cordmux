// SPDX-License-Identifier: Apache-2.0

// dialogs のシナリオ。確認、結果、失敗のダイアログを、ドックの [...] にキーボードでフォーカスを置いてから製品の
// run_message_dialog で開き、最初のフォーカス、既定のボタン、Enter と Esc の答え、閉じた後のフォーカス、
// 親ウィンドウ、タイトル、止める範囲を確認する。元に戻せない操作の確認では、既定と Esc が [キャンセル] であることも
// 確認する。あわせて、ドックから始めた確認と名前の変更のダイアログが開いている間に、OBS のメインウィンドウは操作でき、
// ドックは操作できないことと、結果のダイアログの場所の行のボタン、画面に入らないダイアログの本文の
// スクロールを確認する。

#include "scenarios.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dock/dock_widget.h"
#include "dock/install_dialogs.h"
#include "dock/install_flow.h"
#include "dock/install_runtime_files.h"
#include "dock/rename_dialog.h"
#include "dock/settings_window.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/message_dialog.h"
#include "drive_env.h"
#include "input.h"
#include "ledger/text_ids.h"
#include "obs_dock.h"
#include "owner_hold_probe.h"
#include "query.h"
#include "scen_common.h"
#include "script.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

struct Case {
	QString id;
	dock::ui::MessageSpec spec;
	bool irreversible = false; // 元に戻せない操作の確認 (既定と Esc が [キャンセル])
	bool owner_only = false;   // 親ウィンドウだけを止める (OwnerOnly。インストールのダイアログ)
};

// ファイルの場所の行 (MessageItem::file) のパス。なければ空。
QStringList file_locations_of(const dock::ui::MessageSpec &spec)
{
	QStringList out;
	for (const dock::ui::MessageItem &item : spec.items) {
		if (item.kind == dock::ui::MessageItem::Kind::Location && item.file)
			out << item.path;
	}
	return out;
}

// 親フォルダーを開いて選択した状態で表示するフォルダーの行 (MessageItem::folder_location。結果のダイアログの「削除する
// フォルダー:」の行) のパスと、フォルダーの中を開く [フォルダーを開く] の行 (ほかのフォルダーの行で
// open_folder が真のもの) の数。
QStringList folder_location_paths_of(const dock::ui::MessageSpec &spec)
{
	QStringList out;
	for (const dock::ui::MessageItem &item : spec.items) {
		if (item.kind == dock::ui::MessageItem::Kind::Location && item.folder_location)
			out << item.path;
	}
	return out;
}

int open_folder_lines_of(const dock::ui::MessageSpec &spec)
{
	int n = 0;
	for (const dock::ui::MessageItem &item : spec.items) {
		if (item.kind == dock::ui::MessageItem::Kind::Location && !item.file && !item.folder_location &&
		    item.open_folder)
			++n;
	}
	return n;
}

QString name_of(QWidget *w)
{
	if (w == nullptr)
		return QStringLiteral("(none)");
	if (auto *b = qobject_cast<QAbstractButton *>(w))
		return QStringLiteral("button:") + b->text();
	return w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName();
}

// 開いているダイアログ (モーダルのダイアログか、親ウィンドウだけを止めるダイアログ)。
QDialog *open_dialog()
{
	if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
		return modal;
	return qobject_cast<QDialog *>(find_message_dialog());
}

QPushButton *dock_menu_button(SimPoll &sim)
{
	return sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"));
}

// ダイアログの中で Tab を一周させる。現在のフォーカスから Tab を押し、元のウィジェットに戻るか、ダイアログの外へ出るまで
// 続ける (40 回で打ち切る)。
std::vector<QWidget *> tab_cycle(Input &input, QDialog *dialog)
{
	std::vector<QWidget *> out;
	QWidget *start = QApplication::focusWidget();
	if (start == nullptr || start->window() != dialog)
		return out;
	out.push_back(start);
	for (int i = 0; i < 40; ++i) {
		input.key(Qt::Key_Tab);
		QWidget *f = QApplication::focusWidget();
		if (f == start || f == nullptr || f->window() != dialog)
			break;
		out.push_back(f);
	}
	return out;
}

QString names_of(const std::vector<QWidget *> &chain)
{
	QStringList out;
	for (QWidget *w : chain)
		out << name_of(w);
	return out.join(QStringLiteral(" / "));
}

// 手順書と違う置き方のアンインストールが終わったダイアログに表示する、利用者に削除してもらうファイル。プレビューツールの
// テストデータ (tests/ui_preview/faces.cpp の preview_loose_files) と同じフォルダーと、同梱するファイルの一覧
// (製品と同じ、生成したヘッダー) から、製品の loose_install_files_of と同じ並び (名前の順で、
// 大文字と小文字を区別しない) で作る。製品の関数は dock_drive のビルドに入っていない (dock/install_flow.cpp を
// 加えると、アンインストールの処理の依存がテストに増える) ので、並べ方だけを再現する。ダイアログの中身は製品の
// uninstall_result_spec が作る。
dock::LooseInstallFiles loose_files()
{
	dock::LooseInstallFiles loose;
	loose.files_dir = "C:/obs-studio/plugins/cordmux/bin/64bit";
	loose.file_names.push_back("cordmux.dll");
	for (const char *name : dock::generated::kBundledRuntimeFiles)
		loose.file_names.push_back(name);
	std::stable_sort(loose.file_names.begin(), loose.file_names.end(), [](const std::string &a, const std::string &b) {
		return QString::fromStdString(a).compare(QString::fromStdString(b), Qt::CaseInsensitive) < 0;
	});
	loose.data_dir = "C:/obs-studio/plugins/cordmux/data";
	return loose;
}

} // namespace

void run_dialogs(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("dialogs");
	std::vector<Case> cases;
	cases.push_back({QStringLiteral("uninstall-confirm"), dock::uninstall_confirm_spec(), true, true});
	cases.push_back({QStringLiteral("switch-confirm"), dock::switch_confirm_spec("C:\\Tools\\Vencord"), true, true});
	for (const char *scene : {"dialog-uninstall-done", "dialog-uninstall-stopped"}) {
		const ledger::dockview::DockViewV2 view = ui_preview::view_for(ui_preview::fixture_by_id(scene));
		if (view.uninstall_result.has_value())
			cases.push_back(
				{QString::fromLatin1(scene), dock::uninstall_result_spec(*view.uninstall_result), false, true});
	}
	// [ファイルの場所を開く] の関数 (押されたファイルを覚えるだけ)。
	auto revealed = std::make_shared<QStringList>();
	// [フォルダーの場所を開く] (「削除するフォルダー:」の行) も同じ関数を使う。
	for (Case &c : cases) {
		if (!c.spec.open_file_location_text.isEmpty() || !c.spec.open_folder_location_text.isEmpty())
			c.spec.reveal_file = [revealed](const QString &path) { revealed->push_back(path); };
	}
	cases.push_back({QStringLiteral("export-failed-write"), dock::export_failed_spec(bundle::ExportOutcome::WriteFailed), false, true});
	cases.push_back({QStringLiteral("export-failed-zip"), dock::export_failed_spec(bundle::ExportOutcome::ZipFailed), false, true});
	cases.push_back({QStringLiteral("settings-save-failed"), dock::settings_save_failed_spec(), false, true});

	SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.start();
	Script s;
	auto opener = std::make_shared<QPointer<QWidget>>();
	auto choice = std::make_shared<int>(-2);

	for (const Case &c : cases) {
		for (int how = 0; how < 2; ++how) { // 0 = Enter、1 = Esc
			s.then_blocking(300, [&, c, how, opener, choice] {
				QPushButton *b = dock_menu_button(sim);
				*opener = b;
				if (b == nullptr)
					return;
				b->setFocus(Qt::TabFocusReason);
				input.key(Qt::Key_Shift); // 最後の入力をキーボードにする (フォーカスリングを確認するため)
				*choice = dock::ui::run_message_dialog(sim.widget(), c.spec);
			});
			s.then(300, [&, c, how] {
				QDialog *dialog = open_dialog();
				if (dialog == nullptr) {
					results.add(group, c.id + QStringLiteral(": ダイアログが開く"), QStringLiteral("no"), QStringLiteral("yes"), Verdict::Fail);
					return;
				}
				if (how == 0) {
					// 1 回目だけ、ダイアログの形と最初のフォーカスを確認する。
					results.expect_true(group, c.id + QStringLiteral(": QMessageBox ではない"),
							    qobject_cast<QMessageBox *>(dialog) == nullptr);
					results.expect_true(group, c.id + QStringLiteral(": 親ウィンドウがドック"),
							    dialog->parentWidget() != nullptr && dialog->parentWidget()->window() == sim.dock(),
							    QStringLiteral("親 %1")
								    .arg(dialog->parentWidget() != nullptr
										 ? QString::fromLatin1(dialog->parentWidget()->metaObject()->className())
										 : QStringLiteral("(none)")));
					const bool owner_only_now = dialog->windowModality() == Qt::NonModal &&
								    blocking_name_of(dialog) == QLatin1String("ownerOnly") &&
								    QApplication::activeModalWidget() == nullptr;
					const QString shape = QStringLiteral("モーダル %1、止め方 %2")
								      .arg(int(dialog->windowModality()))
								      .arg(blocking_name_of(dialog));
					if (c.owner_only)
						results.expect_true(group, c.id + QStringLiteral(": 親ウィンドウだけを止める (OwnerOnly、モーダルでない)"),
								    owner_only_now, shape);
					else
						results.info(group,
							     c.id + QStringLiteral(": 止め方 (このダイアログの形は呼び出し元が決める。製品の形は windows のシナリオで確認する)"),
							     owner_only_now ? QStringLiteral("ownerOnly") : QStringLiteral("ownerAndParents"), shape);
					results.expect_true(group, c.id + QStringLiteral(": タイトルがある"), !dialog->windowTitle().isEmpty());
					QPushButton *def = nullptr;
					for (QPushButton *p : dialog->findChildren<QPushButton *>()) {
						if (p->isDefault())
							def = p;
					}
					QString expected_default;
					QString expected_escape;
					for (const dock::ui::MessageButton &mb : c.spec.buttons) {
						if (mb.is_default)
							expected_default = mb.text;
						if (mb.is_escape)
							expected_escape = mb.text;
					}
					results.expect_true(group, c.id + QStringLiteral(": 既定のボタンが仕様のとおり"),
							    def != nullptr && def->text() == expected_default,
							    QStringLiteral("既定 %1 / 仕様 %2、最初のフォーカス %3")
								    .arg(def != nullptr ? def->text() : QStringLiteral("(none)"), expected_default,
									 name_of(QApplication::focusWidget())));
					if (c.irreversible) {
						const QString cancel = dock::obs_frontend_text(ledger::text::obs_keys::kCancel);
						results.expect_true(group, c.id + QStringLiteral(": 元に戻せない操作の確認は、既定と Esc が [キャンセル]"),
								    expected_default == cancel && expected_escape == cancel &&
									    QApplication::focusWidget() != nullptr &&
									    qobject_cast<QAbstractButton *>(QApplication::focusWidget()) != nullptr &&
									    qobject_cast<QAbstractButton *>(QApplication::focusWidget())->text() == cancel,
								    QStringLiteral("既定 %1、Esc %2、最初のフォーカス %3")
									    .arg(expected_default, expected_escape, name_of(QApplication::focusWidget())));
					}
				}
				// ファイルの場所の行の [ファイルの場所を開く] は、Esc で閉じる回だけ押す。
				// Enter で閉じるときに押すと、フォーカスがそのボタンへ移り、Enter がそのボタンを
				// 押してしまう。
				const QStringList files = file_locations_of(c.spec);
				if (how == 1 && !files.isEmpty()) {
					QPushButton *reveal = nullptr;
					for (QPushButton *p : dialog->findChildren<QPushButton *>()) {
						if (p->isVisible() && p->text() == c.spec.open_file_location_text)
							reveal = p;
					}
					results.expect_true(group, c.id + QStringLiteral(": ファイルの場所の行に [ファイルの場所を開く] がある"),
							    reveal != nullptr, QStringLiteral("ファイルの行 %1").arg(files.size()));
					if (reveal != nullptr) {
						revealed->clear();
						input.move_to(reveal->mapToGlobal(reveal->rect().center()));
						input.click();
						pump(200);
						results.expect_true(group,
								    c.id + QStringLiteral(": [ファイルの場所を開く] を押すと、その行のファイルで関数が呼ばれ、ダイアログは閉じない"),
								    revealed->size() == 1 && files.contains(revealed->front()) && open_dialog() == dialog,
								    revealed->isEmpty() ? QStringLiteral("呼ばれない") : revealed->front().right(32));
					}
				}
				// 「削除するフォルダー:」の行の [フォルダーの場所を開く]。押すと、その行のフォルダーを
				// 選択した状態で開く関数が 1 回呼ばれ、ダイアログは閉じない。ほかのフォルダーの行
				// (録画の場所) は、中を開く [フォルダーを開く] のまま (押すとエクスプローラーが
				// 開くので、押さずにテキストと数だけを確認する)。Esc で閉じる回だけ押す
				// (上の [ファイルの場所を開く] と同じ理由)。
				const QStringList folders = folder_location_paths_of(c.spec);
				if (how == 1 && !folders.isEmpty()) {
					QPushButton *select = nullptr;
					int open_folder_buttons = 0;
					for (QPushButton *p : dialog->findChildren<QPushButton *>()) {
						if (!p->isVisible())
							continue;
						if (p->text() == c.spec.open_folder_location_text)
							select = p;
						else if (p->text() == c.spec.open_folder_text)
							++open_folder_buttons;
					}
					results.expect_true(group,
							    c.id + QStringLiteral(": 「削除するフォルダー:」の行に [フォルダーの場所を開く] がある"),
							    select != nullptr, QStringLiteral("行 %1").arg(folders.size()));
					results.expect_eq(group,
							  c.id + QStringLiteral(": ほかのフォルダーの行は [フォルダーを開く] のまま"),
							  open_folder_buttons, open_folder_lines_of(c.spec));
					if (select != nullptr) {
						revealed->clear();
						input.move_to(select->mapToGlobal(select->rect().center()));
						input.click();
						pump(200);
						results.expect_true(group,
								    c.id + QStringLiteral(": [フォルダーの場所を開く] を押すと、その行のフォルダーを選択して開く関数が呼ばれ、ダイアログは閉じない"),
								    revealed->size() == 1 && folders.contains(revealed->front()) && open_dialog() == dialog,
								    revealed->isEmpty() ? QStringLiteral("呼ばれない") : revealed->front().right(32));
					}
				}
				// キーはアクティブなウィンドウに届く (ダイアログは開いたときにアクティブになる)。
				Input::activate_window(dialog);
				input.key(how == 0 ? Qt::Key_Return : Qt::Key_Escape);
			});
			s.then(300, [&, c, how, opener, choice] {
				int expected = -1;
				for (std::size_t i = 0; i < c.spec.buttons.size(); ++i) {
					const dock::ui::MessageButton &mb = c.spec.buttons[i];
					if ((how == 0 && mb.is_default) || (how == 1 && mb.is_escape))
						expected = static_cast<int>(i);
				}
				results.expect_eq(group, c.id + (how == 0 ? QStringLiteral(": Enter の答えが既定のボタン")
									 : QStringLiteral(": Esc の答えが Esc のボタン")),
						  *choice, expected);
				const bool back = QApplication::focusWidget() == opener->data();
				const RingMeasure m = measure_ring(sim.dock(), QApplication::focusWidget());
				results.expect_true(group, c.id + QStringLiteral(": 閉じた後に、フォーカスが開く前のウィジェットへ戻る"), back,
						    name_of(QApplication::focusWidget()));
				if (how == 0)
					results.expect_true(group, c.id + QStringLiteral(": 戻ったフォーカスにフォーカスリングが表示される"), m.ring_visible);
				*choice = -2;
			});
		}
	}

	// ---- ドックから始めた切り替えの確認を開いている間に、何を操作できるか ----
	// 切り替えの確認が開いている間、OBS のメインウィンドウは押すと動作し、ドックは押しても動作せず (押すとダイアログが
	// 前に出る)、Esc で閉じるとフォーカスが戻ることを、フローティングのドックと、メインウィンドウに入れたドックの
	// 2 つの形で確認する。
	auto probe = std::make_shared<std::unique_ptr<MainWindowProbe>>();
	for (int docked = 0; docked < 2; ++docked) {
		const QString form = docked != 0 ? QStringLiteral("メインウィンドウに入れたドック") : QStringLiteral("フローティングのドック");
		const QString g = QStringLiteral("dialogs-dock-started");
		s.then(300, [&, docked, probe] {
			if (docked != 0) {
				sim.dock()->setFloating(false);
				pump(400);
			}
			*probe = std::make_unique<MainWindowProbe>();
			input.activate(sim.dock());
		});
		s.then_blocking(300, [&, opener, choice] {
			QPushButton *b = dock_menu_button(sim);
			*opener = b;
			if (b == nullptr)
				return;
			b->setFocus(Qt::TabFocusReason);
			input.key(Qt::Key_Shift);
			*choice = dock::ui::run_message_dialog(sim.widget(), dock::switch_confirm_spec("C:\\Tools\\Vencord"));
		});
		s.then(300, [&, form, g, probe] {
			QDialog *dialog = open_dialog();
			results.expect_true(g, form + QStringLiteral(": 切り替えの確認が開く"), dialog != nullptr);
			if (dialog == nullptr)
				return;
			results.expect_true(g, form + QStringLiteral(": Qt が OBS のメインウィンドウを止めていない"), !qt_window_blocked(main_window()));
			results.expect_true(g, form + QStringLiteral(": 開いている間も、OBS のメインウィンドウを押すと動作する"),
					    *probe != nullptr && (*probe)->click_works(input));
			QString note;
			const bool dock_ran = dock_row_click_works(input, sim, &note);
			results.expect_true(g, form + QStringLiteral(": 開いている間は、ドックの行を押しても動作しない (止める範囲はドック)"), !dock_ran,
					    note);
			pump(200);
			results.expect_true(g, form + QStringLiteral(": ドックを押すと、確認のダイアログが前に出てアクティブになる"),
					    open_dialog() == dialog && QApplication::activeWindow() == dialog,
					    QStringLiteral("アクティブなウィンドウ %1").arg(name_of(QApplication::activeWindow())));
			Input::activate_window(dialog);
			input.key(Qt::Key_Escape);
		});
		s.then(300, [&, form, g, docked, probe, opener, choice] {
			int escape = -1;
			const dock::ui::MessageSpec spec = dock::switch_confirm_spec("C:\\Tools\\Vencord");
			for (std::size_t i = 0; i < spec.buttons.size(); ++i) {
				if (spec.buttons[i].is_escape)
					escape = static_cast<int>(i);
			}
			results.expect_true(g, form + QStringLiteral(": Esc で閉じて答えは Esc のボタン ([キャンセル])"),
					    open_dialog() == nullptr && escape >= 0 && *choice == escape,
					    QStringLiteral("答え %1 / Esc のボタン %2").arg(*choice).arg(escape));
			results.expect_true(g, form + QStringLiteral(": 閉じた後にフォーカスが [...] へ戻る"), QApplication::focusWidget() == opener->data(),
					    name_of(QApplication::focusWidget()));
			results.expect_true(g, form + QStringLiteral(": 閉じた後はドックの行を押すと動作する"), dock_row_click_works(input, sim));
			probe->reset();
			*choice = -2;
			if (docked != 0) {
				sim.dock()->setFloating(true);
				sim.dock()->setGeometry(1300, 120, 420, 700);
				pump(400);
			}
		});
	}

	// ---- 名前の変更のダイアログ (ドックの行のメニューから開き、ui::run_owner_only_dialog で表示する) ----
	// 止める範囲はドックだけ。開いている間も OBS のメインウィンドウは押すと動作し、ドックの行は動作せずにダイアログが
	// 前に出る。Esc で閉じる。
	auto rename_probe = std::make_shared<std::unique_ptr<MainWindowProbe>>();
	auto rename_dialog = []() -> QDialog * {
		for (QWidget *w : QApplication::topLevelWidgets()) {
			if (w->isVisible() && w->objectName() == QLatin1String("cordmuxRenameDialog"))
				return qobject_cast<QDialog *>(w);
		}
		return nullptr;
	};
	s.then(300, [&, rename_probe] {
		*rename_probe = std::make_unique<MainWindowProbe>();
		input.activate(sim.dock());
	});
	s.then_blocking(300, [&] { (void)dock::run_rename_dialog(sim.widget(), QStringLiteral("alice"), std::string()); });
	s.then(300, [&, rename_probe, rename_dialog] {
		const QString g = QStringLiteral("dialogs-dock-started");
		QDialog *dialog = rename_dialog();
		results.expect_true(g, QStringLiteral("名前の変更のダイアログ (フローティングのドックから): 開いて、親ウィンドウがドック"),
				    dialog != nullptr && dialog->parentWidget() != nullptr && dialog->parentWidget()->window() == sim.dock());
		if (dialog == nullptr)
			return;
		results.expect_true(g, QStringLiteral("名前の変更のダイアログ: 親ウィンドウだけを止める (モーダルでない)"),
				    dialog->windowModality() == Qt::NonModal && QApplication::activeModalWidget() == nullptr,
				    QStringLiteral("モーダル %1、止め方 %2").arg(int(dialog->windowModality())).arg(blocking_name_of(dialog)));
		results.expect_true(g, QStringLiteral("名前の変更のダイアログ: 開いている間も、OBS のメインウィンドウを押すと動作する"),
				    *rename_probe != nullptr && (*rename_probe)->click_works(input));
		QString note;
		results.expect_true(g, QStringLiteral("名前の変更のダイアログ: 開いている間は、ドックの行を押しても動作しない"),
				    !dock_row_click_works(input, sim, &note), note);
		pump(200);
		results.expect_true(g, QStringLiteral("名前の変更のダイアログ: ドックを押すとダイアログが前に出て、アクティブになる"),
				    QApplication::activeWindow() == dialog,
				    QStringLiteral("アクティブなウィンドウ %1").arg(name_of(QApplication::activeWindow())));
		Input::activate_window(dialog);
		input.key(Qt::Key_Escape);
	});
	s.then(300, [&, rename_probe, rename_dialog] {
		results.expect_true(QStringLiteral("dialogs-dock-started"), QStringLiteral("名前の変更のダイアログ: Esc で閉じる"), rename_dialog() == nullptr);
		rename_probe->reset();
	});

	// ---- 画面に入らないダイアログの本文のスクロール ----
	// 製品の関数で開いた、手順書と違う置き方のアンインストールが終わったダイアログ (テストの画面は
	// 1920x1040 で、スクロールせずに入る) に、1366x768 で拡大率 125% の作業領域 (574 px、枠 39 px) を設定し、
	// 表示中のダイアログがスクロールする形へ移る処理 (画面を移ったときと、作業領域が変わったときと同じ
	// fit_to_screen) で確認する。
	//   (1) QScrollArea (cordmuxMessageScroll) が Tab の一周に 1 回だけ、本文の最初のタブストップの直前に来て、
	//       ほかの順はスクロールしない形と同じ。
	//   (2) QScrollArea のアクセシブル名がダイアログのタイトル (QAccessible で読む)。
	//   (3) QScrollArea にフォーカスがあるとき、下の矢印キーと PageDown でスクロールできる。
	//   (4) いちばん上まで戻して [OK] から Shift+Tab を押すと、本文のいちばん下のタブストップに着き、
	//       そのウィジェットとフォーカスリングが表示範囲に入る (ui::focus_mark_shown と、リングの
	//       4 辺が描かれること)。
	//   (5) 作業領域を 4000 に戻すと QScrollArea が消え、ダイアログのサイズと Tab の一周がスクロールしない形と
	//       同じになる。
	// 表示する前に作業領域を決めておく形だけを確認する方法はとらない。製品では、表示する前の調整と、
	// 表示した後の画面の移動の両方が起き、表示した後の処理のほうが、ウィジェットの追加と削除、フォーカスの戻しを
	// 多く通るためである。テスト用に作ったダイアログで確認する方法もとらない (製品のウィジェットの
	// ツリーを通らない)。
	constexpr int kWorkAreaPx = 574;
	constexpr int kFramePx = 39;
	auto loose_spec = std::make_shared<dock::ui::MessageSpec>();
	{
		const ledger::dockview::DockViewV2 view = ui_preview::view_for(ui_preview::fixture_by_id("dialog-uninstall-loose"));
		if (view.uninstall_result.has_value()) {
			const dock::LooseInstallFiles loose = loose_files();
			*loose_spec = dock::uninstall_result_spec(*view.uninstall_result, &loose);
			loose_spec->reveal_file = [](const QString &) {};
		}
	}
	s.then_blocking(300, [&, opener, choice, loose_spec] {
		QPushButton *b = dock_menu_button(sim);
		*opener = b;
		if (b == nullptr || loose_spec->buttons.empty())
			return;
		input.activate(sim.dock());
		b->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Shift);
		*choice = dock::ui::run_message_dialog(sim.widget(), *loose_spec);
	});
	s.then(300, [&, loose_spec] {
		const QString g = QStringLiteral("dialogs-scroll");
		const QString id = QStringLiteral("dialog-uninstall-loose");
		QDialog *dialog = open_dialog();
		results.expect_true(g, id + QStringLiteral(": 製品の関数で開く"), dialog != nullptr && !loose_spec->buttons.empty());
		if (dialog == nullptr)
			return;
		Input::activate_window(dialog);
		pump(100);
		const QSize plain_size = dialog->size();
		const bool scrolled_before = dock::ui::message_dialog_scrolls(dialog);
		results.expect_true(g, id + QStringLiteral(": 1920x1040 の作業領域ではスクロールしない (QScrollArea がない)"),
				    !scrolled_before && dialog->findChild<QScrollArea *>(QStringLiteral("cordmuxMessageScroll")) == nullptr,
				    QStringLiteral("ダイアログ %1x%2").arg(plain_size.width()).arg(plain_size.height()));
		// スクロールしない形の Tab の一周 (開いたときのフォーカスの [OK] から)。
		const std::vector<QWidget *> plain = tab_cycle(input, dialog);
		const bool on = dock::ui::set_message_dialog_work_area(dialog, kWorkAreaPx, kFramePx);
		pump(200);
		QPointer<QScrollArea> box = dialog->findChild<QScrollArea *>(QStringLiteral("cordmuxMessageScroll"));
		results.expect_true(g, id + QStringLiteral(": 作業領域 574 (枠 39) で本文がスクロールする形になり、ダイアログと枠が作業領域に入る"),
				    on && dock::ui::message_dialog_scrolls(dialog) && !box.isNull() && box->isVisible() &&
					    dialog->height() + kFramePx <= kWorkAreaPx,
				    QStringLiteral("ダイアログ %1x%2 (スクロールしない形 %3x%4)")
					    .arg(dialog->width())
					    .arg(dialog->height())
					    .arg(plain_size.width())
					    .arg(plain_size.height()));
		if (box.isNull() || box->widget() == nullptr || plain.empty()) {
			Input::activate_window(dialog);
			input.key(Qt::Key_Escape);
			return;
		}
		QWidget *content = box->widget();
		std::vector<QWidget *> body;
		for (QWidget *w : plain) {
			if (content->isAncestorOf(w))
				body.push_back(w);
		}
		std::vector<QWidget *> expected;
		for (QWidget *w : plain) {
			if (!body.empty() && w == body.front())
				expected.push_back(box.data());
			expected.push_back(w);
		}
		if (body.empty())
			expected.push_back(box.data());
		// (1) 同じウィジェット ([OK]) から一周する。
		Input::activate_window(dialog);
		plain.front()->setFocus(Qt::TabFocusReason);
		const std::vector<QWidget *> chain = tab_cycle(input, dialog);
		const auto box_stops = std::count(chain.begin(), chain.end(), box.data());
		results.expect_true(g,
				    id + QStringLiteral(": QScrollArea が Tab の一周に 1 回だけ、本文の最初のタブストップの直前に来る (ほかの順は同じ)"),
				    box_stops == 1 && chain == expected, names_of(chain));
		// (2) QScrollArea のアクセシブル名。
		QAccessibleInterface *ai = QAccessible::queryAccessibleInterface(box.data());
		const QString a11y_name = ai != nullptr ? ai->text(QAccessible::Name) : QString();
		results.expect_true(g, id + QStringLiteral(": QScrollArea のアクセシブル名がダイアログのタイトル"),
				    !a11y_name.trimmed().isEmpty() && a11y_name == dialog->windowTitle(), a11y_name);
		// (3) QScrollArea にフォーカスがあるときのスクロール。
		QScrollBar *bar = box->verticalScrollBar();
		bar->setValue(0);
		box->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Down);
		const int after_down = bar->value();
		bar->setValue(0);
		input.key(Qt::Key_PageDown);
		const int after_page = bar->value();
		results.expect_true(g, id + QStringLiteral(": QScrollArea にフォーカスがあるとき、下の矢印キーと PageDown でスクロールできる"),
				    bar->maximum() > 0 && after_down > 0 && after_page > 0 &&
					    after_page >= std::min(bar->pageStep(), bar->maximum()),
				    QStringLiteral("下 %1、PageDown %2 (1 ページ %3、最大 %4)")
					    .arg(after_down)
					    .arg(after_page)
					    .arg(bar->pageStep())
					    .arg(bar->maximum()));
		// (4) [OK] から Shift+Tab で本文のいちばん下へ。
		bar->setValue(0);
		QPushButton *ok = nullptr;
		for (QPushButton *p : dialog->findChildren<QPushButton *>()) {
			if (p->isDefault())
				ok = p;
		}
		if (ok != nullptr)
			ok->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Backtab, Qt::ShiftModifier);
		pump(100);
		QWidget *landed = QApplication::focusWidget();
		const RingMeasure m = measure_ring(dialog, landed);
		const int reach = m.ring_visible ? std::max(0, m.hit.top() - m.ring_rect.top()) : 0;
		const bool shown = landed != nullptr && content->isAncestorOf(landed) &&
				   dock::ui::focus_mark_shown(box.data(), landed, reach);
		results.expect_true(g,
				    id + QStringLiteral(": [OK] から Shift+Tab で本文のいちばん下のタブストップに着き、ウィジェットとフォーカスリングが表示範囲に入る"),
				    ok != nullptr && !body.empty() && landed == body.back() && bar->value() > 0 && shown && m.drawn &&
					    ring_sides_drawn(m.side_len, m.side_cover) == 4,
				    QStringLiteral("着いた所 %1、スクロール %2、リングの辺 %3")
					    .arg(name_of(landed))
					    .arg(bar->value())
					    .arg(ring_sides_drawn(m.side_len, m.side_cover)));
		// (5) 広い作業領域へ戻す。
		const bool still = dock::ui::set_message_dialog_work_area(dialog, 4000, kFramePx);
		pump(200);
		Input::activate_window(dialog);
		plain.front()->setFocus(Qt::TabFocusReason);
		const std::vector<QWidget *> back_chain = tab_cycle(input, dialog);
		results.expect_true(g, id + QStringLiteral(": 作業領域を広げると QScrollArea が消え、ダイアログのサイズと Tab の一周がスクロールしない形と同じ"),
				    !still && !dock::ui::message_dialog_scrolls(dialog) && (box.isNull() || !box->isVisible()) &&
					    dialog->size() == plain_size && back_chain == plain,
				    QStringLiteral("ダイアログ %1x%2、一周 %3").arg(dialog->width()).arg(dialog->height()).arg(names_of(back_chain)));
		Input::activate_window(dialog);
		input.key(Qt::Key_Escape);
	});
	s.then(300, [&, opener, choice] {
		results.expect_true(QStringLiteral("dialogs-scroll"), QStringLiteral("dialog-uninstall-loose: Esc で閉じ、フォーカスが [...] へ戻る"),
				    open_dialog() == nullptr && *choice == 0 && QApplication::focusWidget() == opener->data(),
				    QStringLiteral("答え %1、フォーカス %2").arg(*choice).arg(name_of(QApplication::focusWidget())));
		*choice = -2;
	});

	const bool finished = s.run(180000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	sim.stop();
	sim.dock()->hide();
}

} // namespace cordmux::dock_drive

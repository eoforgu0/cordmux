// SPDX-License-Identifier: Apache-2.0

// 「名前を変更」のダイアログ (rename_dialog.h) を実装する。

#include "dock/rename_dialog.h"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMargins>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "dock/ui/dock_metrics.h"
#include "dock/ui/dock_style.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/message_dialog.h"
#include "dock/ui/set_class.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/theme_read.h"
#include "dock/ui/wrap_label.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

// rename_dialog_text が入力欄を探すときの objectName。
constexpr const char *kEditName = "cordmuxRenameEdit";

QString obs_text(const char *key)
{
	return QString::fromUtf8(obs_frontend_get_locale_string(key));
}

} // namespace

QDialog *make_rename_dialog(QWidget *owner, const QString &discord_name, const std::string &current_override)
{
	QWidget *parent = owner != nullptr ? owner->window() : nullptr;
	auto *dialog = new QDialog(parent);
	dialog->setObjectName(QStringLiteral("cordmuxRenameDialog"));
	dialog->setProperty("dialogId", QStringLiteral("rename"));
	dialog->setWindowTitle(ui::resolve_key(ledger::text::keys::kDialogRenameTitle));
	// 入力を止める範囲は、開くときに run_rename_dialog が決める。開かずに使うとき (プレビューツールとテスト) は
	// Qt の既定のままにする。
	dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog->setSizeGripEnabled(false);
	dialog->ensurePolished();

	const ui::DockStyle style;
	const ui::ThemeRead theme = ui::read_theme(dialog, nullptr);
	const ui::DockMetrics m = ui::read_dock_metrics(dialog, theme, style);

	auto *root = new QVBoxLayout(dialog);
	root->setSpacing(m.large_px);

	// ラベルと入力欄を 1 行に並べる。設定ウィンドウと同じ QFormLayout で、幅が足りなければ入力欄をラベルの下に置く。
	auto *form = new QFormLayout();
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	auto *label = new QLabel(ui::resolve_key(ledger::text::keys::kParticipantsColNameInCordmuxLabel));
	label->setTextFormat(Qt::PlainText);
	auto *edit = new QLineEdit(QString::fromStdString(current_override));
	edit->setObjectName(QString::fromLatin1(kEditName));
	// 入力欄を空にすると Discord での名前が使われるので、それをプレースホルダーにする。読めなければ表示しない
	// (代わりのテキストを作らない)。
	edit->setPlaceholderText(discord_name);
	label->setBuddy(edit); // ラベルのテキストが入力欄のアクセシブル名になる
	form->addRow(label, edit);
	root->addLayout(form);

	// 補足の 1 文 (どこに使われる名前か)。折り返して表示し、背景とのコントラスト比が足りるときだけ薄い色にする。
	// 幅は、ダイアログの子にして ensurePolished を呼んだ後のフォント (テーマのフォントサイズ) で測る。親のない
	// ラベルは Qt の既定のフォントで測られ、テーマのフォントより狭くなるので、必要のない所で折り返す。幅は小数で
	// 測って切り上げ、ラベルの余白を足す。整数の幅で測ると、端数のせいで最後の 1 字が次の行に送られる。
	// 上限は段落と同じ 40 em。
	const QString tip = ui::resolve_key(ledger::text::keys::kParticipantsNameInCordmuxTip);
	// フォントサイズが大きいときや英語の長い文では 40 em で折り返すので、最後の行が短くなりすぎないように
	// 折り返す WrapLabel にする。
	auto *note = new ui::WrapLabel(tip, dialog);
	if (ui::muted_ok(theme, theme.window_bg))
		ui::set_class(note, QStringLiteral("text-muted"));
	note->ensurePolished();
	const QMargins nm = note->contentsMargins();
	const int note_w = static_cast<int>(std::ceil(QFontMetricsF(note->font()).horizontalAdvance(tip))) + nm.left() +
			   nm.right() + 2 * note->margin();
	note->setFixedWidth(std::min(m.paragraph_max_px, note_w));
	root->addWidget(note);
	// 補足は入力欄のすぐ下に表示しているので、同じテキストをツールチップにしない。スクリーンリーダーでは入力欄に
	// 来たときに聞こえるように、入力欄の accessibleDescription にする。ダイアログの accessibleDescription には
	// 入れない。入れると、ダイアログが開いてフォーカスが入力欄に来たときに 2 回読まれる。
	edit->setAccessibleDescription(tip);

	// [OK] と [キャンセル]。テキストは OBS の [設定] と同じにするため、OBS の ini から読む。
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setText(obs_text(ledger::text::obs_keys::kOk));
	buttons->button(QDialogButtonBox::Cancel)->setText(obs_text(ledger::text::obs_keys::kCancel));
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	root->addWidget(buttons);
	root->setSizeConstraint(QLayout::SetFixedSize);

	dialog->setAccessibleName(dialog->windowTitle());

	// フォーカスは入力欄に置き、すぐに打ち直せるようにテキストを全部選んでおく。
	auto *ring = new ui::FocusRing(dialog);
	ring->set_color(theme.text);
	ring->set_metrics(m.focus_ring_px, m.focus_ring_gap_px);
	edit->selectAll();
	edit->setFocus(Qt::OtherFocusReason);
	return dialog;
}

QString rename_dialog_text(const QDialog *dialog)
{
	if (dialog == nullptr)
		return QString();
	const auto *edit = dialog->findChild<QLineEdit *>(QString::fromLatin1(kEditName));
	return edit != nullptr ? edit->text().trimmed() : QString();
}

std::optional<std::string> run_rename_dialog(QWidget *owner, const QString &discord_name,
					     const std::string &current_override)
{
	// 閉じた後にフォーカスを戻す先。ダイアログを開いている間に削除されることがあるので QPointer で持つ。
	// make_rename_dialog が入力欄にフォーカスを置くので、ダイアログを作る前に保存する。run_owner_only_dialog も
	// フォーカスを戻すが、戻す先を保存するのがダイアログを作った後なので、ここで保存した先に戻す。同じウィジェットに
	// 2 回フォーカスを置いても、フォーカスは動かない。
	const QPointer<QWidget> before = QApplication::focusWidget();
	QPointer<QDialog> dialog = make_rename_dialog(owner, discord_name, current_override);
	// 入力を止めるのは owner のドック (QDockWidget) だけにする。ドックが OBS のメインウィンドウの中にあっても
	// メインウィンドウは止めないので、配信中に名前を変更している間も OBS の [配信終了] などを押せる。
	// Qt::WindowModal にすると、ドックを含むメインウィンドウごと止まる。止め方は確認のダイアログと同じ
	// ui::run_owner_only_dialog に任せる。開いている間に OBS の終了でドックが削除されると、ダイアログも削除される。
	const int result = ui::run_owner_only_dialog(dialog.data(), owner);
	std::optional<std::string> out;
	if (!dialog.isNull()) {
		if (result == QDialog::Accepted)
			out = rename_dialog_text(dialog.data()).toStdString();
		delete dialog.data();
	}
	if (!before.isNull() && before->isEnabled() && before->isVisible())
		before->setFocus(Qt::OtherFocusReason);
	return out;
}

} // namespace cordmux::dock

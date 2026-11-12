// forbidden_qt_usage.py の NG 例。音の出るメッセージボックス、音を鳴らす関数、スタイルシートに書かない値。
// 行の末尾のコメントの expect: の後ろは、その行で一致するはずの forbidden_qt_names.txt の行である (--selftest が読む)。
#include <QMessageBox> // expect: \bQMessageBox\b
#include <QSoundEffect> // expect: \bQSoundEffect\b

void show_result(QWidget *parent)
{
	QMessageBox::warning(parent, title, text); // expect: \bQMessageBox\b
	QAccessibleEvent event(parent, QAccessible::Alert); // expect: \bQAccessible::Alert\b
	QApplication::beep(); // expect: \bQApplication::beep\b
	QSound::play(path); // expect: \bQSound\b
	MessageBeep(MB_OK); // expect: \bMessageBeep\b
	PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS); // expect: \bPlaySound[AW]?\b
	MessageBoxW(nullptr, L"x", L"y", MB_OK); // expect: \bMessageBox[AW]?\s*\(
	label->setStyleSheet("color: var(--text);"); // expect: string:\bvar\(--
	label->setStyleSheet("padding: calc(4px + 2px);"); // expect: string:\bcalc\(
	label->setStyleSheet("font-size: 1.5em;"); // expect: string:[:\s][0-9]+(?:\.[0-9]+)?em\b
}

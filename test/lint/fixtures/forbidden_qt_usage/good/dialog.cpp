// forbidden_qt_usage.py の OK 例。QDialog で作ったダイアログと、計算した後の数と色だけのスタイルシート。
// コメントの中の QMessageBox や QApplication::beep() は検出しない (コードと文字列だけをチェックする)。
// 文字列の中の QMessageBox (名前を書いただけの文字列) も検出しない (名前の行はコードだけに使う)。
#include <QDialog>

void show_result(QWidget *parent)
{
	QDialog dialog(parent);
	dialog.setAccessibleDescription(main_text);
	label->setStyleSheet("color: #57F287; padding: 4px;");
	const char *note = "QMessageBox is not used";
	const char *system_line = "system memory";
	int themes = 2; // QAccessible::Alert を出さない
}

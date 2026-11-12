// SPDX-License-Identifier: Apache-2.0

// 操作を始めた範囲だけを止めるダイアログ (dock/ui/message_dialog.h の MessageBlocking::OwnerOnly) が開いている間に、
// どのウィンドウが操作できるかを、実際に押して確認する関数。設定の保存の失敗、エクスポートの失敗、設定ウィンドウから
// 始めたアンインストール、ドックから始めた切り替えの確認、設定ウィンドウの色のダイアログで、同じ確認の方法を使う。
// 止まっているかは、Qt がウィンドウを止めているか (QGuiApplicationPrivate::isWindowBlocked) だけでなく、
// 押して動作したかで確認する。OwnerOnly は、Qt のモーダルを使わずに、止める範囲への入力を捨てるので、Qt の状態だけでは、
// 範囲が止まっていることが分からない。

#pragma once

#include <QPointer>
#include <QString>

class QPushButton;
class QWidget;

namespace cordmux::dock_drive {

class Input;
class SimPoll;

// Qt がウィンドウを止めているか (モーダルのダイアログによるもの。OwnerOnly で止めている範囲は、ここには表れない)。
bool qt_window_blocked(QWidget *w);

// 表示されている cordmux の確認、結果、失敗のダイアログ (cordmuxMessageDialog) のうち、親が owner のもの
// (owner が nullptr なら最後に開いたもの)。なければ nullptr を返す。OwnerOnly のダイアログはモーダルでない
// (activeModalWidget に表れない) ので、ウィンドウの一覧から探す。
QWidget *find_message_dialog(QWidget *owner = nullptr);

// ダイアログの止め方の名前 (dock/ui/message_dialog.cpp が設定する動的プロパティ messageBlocking)。なければ空を返す。
QString blocking_name_of(const QWidget *dialog);

// OBS のメインウィンドウの左下 (フローティングのドックと設定ウィンドウに重ならない所) に置く、テスト用のボタン。
// 押されると数える。メインウィンドウがほかのウィジェットで埋まっていても押せるように、メインウィンドウの最前面の
// 子にする。破棄すると外れるので、ほかのシナリオのウィジェットの並びを変えない。
class MainWindowProbe {
public:
	MainWindowProbe();
	~MainWindowProbe();
	MainWindowProbe(const MainWindowProbe &) = delete;
	MainWindowProbe &operator=(const MainWindowProbe &) = delete;

	// ポインターで押して、押したことが届いたかを返す (メインウィンドウが止められていれば届かない)。
	bool click_works(Input &input);

private:
	QPointer<QPushButton> button_;
	int clicks_ = 0;
};

// ドックの押せる行のチェックを 1 回押して、操作が出たかを返す。出たら、クリックを捨てるガードの時間の外で、
// 同じ所をもう一度押して元に戻す。押せる行がなければ、ドックの [...] を押してメニューが開くかで確認する。
// note には押した位置のウィンドウを書く。
bool dock_row_click_works(Input &input, SimPoll &sim, QString *note = nullptr);

} // namespace cordmux::dock_drive

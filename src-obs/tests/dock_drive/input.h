// SPDX-License-Identifier: Apache-2.0

// テストの入力 (ポインターの移動、ボタンを押すことと離すこと、ホイール、キー) を送る。OS の入力と同じ経路で届けるため、
// QWindowSystemInterface に同期で渡す。この経路では、QGuiApplication がポインターの位置 (QCursor::pos が返す値) の
// 書き換え、Enter と Leave、ボタンを押すイベントの送り先 (最前面のウィジェットか、開いているメニュー)、ダブルクリックの
// 合成を、Windows のプラットフォームプラグインから来た入力と同じ順で行う。製品の並びの保留 (dock/layout_hold.cpp) は
// QCursor::pos でポインターがスクロール領域の上にあるかを見るので、ウィジェットへ直接イベントを送る形 (QTest の
// QWidget を受け取る関数や sendEvent) では始まらない。その形では、並びの保留も、クリックを捨てるガードも試せない。
// イベントの時刻は QWindowSystemInterface が自分の時計で付ける。このクラスは時計を進めない (実時間で実行する)。

#pragma once

#include <QEvent>
#include <QPoint>
#include <QPointer>
#include <QString>

class QWidget;
class QWindow;

namespace cordmux::dock_drive {

class Input {
public:
	Input();
	~Input();
	Input(const Input &) = delete;
	Input &operator=(const Input &) = delete;

	// ポインターを global (スクリーン座標) へ動かす。途中の位置は通らないので、細かく動かすときは呼び出し元が刻む。
	void move_to(const QPoint &global);
	void press(Qt::MouseButton button = Qt::LeftButton);
	void release(Qt::MouseButton button = Qt::LeftButton);
	// 押して離す。間にイベントを処理しない (実際の速いクリックと同じ)。
	void click(Qt::MouseButton button = Qt::LeftButton);
	// 縦のホイール。delta は 120 で 1 ノッチ、正の値が上。
	void wheel(int delta);
	// キーを key_window() のウィンドウへ送る。text は、文字を入力するキーのときだけ渡す。
	void key(int qt_key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = QString());
	// Menu キーか Shift+F10 を送る (Windows がキーボードからのコンテキストメニューにする 2 つ。
	// input.cpp のコメント)。
	void context_menu_key(Qt::KeyboardModifiers modifiers = Qt::NoModifier);

	// キーを送るウィンドウ (開いているメニュー、なければアクティブなウィンドウ)。
	QWindow *key_window();
	// ウィンドウをアクティブにする (Windows が、ウィンドウを表示したときと、利用者がタイトルバーを押したときに
	// すること)。
	void activate(QWidget *window);
	// activate と同じことを、Input を持たない所から行う (製品の bring_to_front が呼ぶ activateWindow を Windows が
	// 受けたときと同じ)。
	static void activate_window(QWidget *window);

	QPoint pos() const { return pos_; }
	Qt::MouseButtons buttons() const { return buttons_; }

	// global の位置の下にあるウィジェット。ボタンを押すイベントの送り先と同じ順 (開いているメニュー、最前面の
	// ウィンドウ、その中の子) で探す。
	static QWidget *widget_at(const QPoint &global);
	// global の位置へのイベントを送るウィンドウ (開いているメニューがあればそれ、なければその位置の
	// 最前面のウィンドウ)。
	static QWindow *window_at(const QPoint &global);

	// ボタンを押した回数 (結果に書くため)。
	int presses() const { return presses_; }

private:
	void send_mouse(QWindow *window, QEvent::Type type, Qt::MouseButton button);

	QPoint pos_{-1, -1};
	Qt::MouseButtons buttons_ = Qt::NoButton;
	QPointer<QWindow> under_; // 最後に Enter を送ったウィンドウ
	QPointer<QWindow> grab_;  // ボタンを押したウィンドウ (離すまでイベントを受ける)
	// メニューかダイアログが隠れたら、前のウィンドウをアクティブに戻す (input.cpp の HideWatcher)。
	QObject *watcher_ = nullptr;
	QPointer<QWindow> last_active_; // 最後にアクティブだったウィンドウ (key_window が戻す)
	int presses_ = 0;

public:
	bool last_context_accepted_ = false; // 最後のコンテキストメニューのイベントを Qt が受け取ったか (確認用)
};

} // namespace cordmux::dock_drive

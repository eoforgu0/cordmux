// SPDX-License-Identifier: Apache-2.0

// 押す直前に一覧を動かさないための 2 つのクラス。並びが変わった直後に、利用者が狙った人とは別の人を押すのを防ぐ。
// LayoutHold は、並びの保留の間かを判定する。並びの保留は、ポインターが一覧の上で動いている間などに、行やカードの
// 位置が動く変化をドックが後に回すことである。
// PressGuard は、現れた直後と動いた直後のボタンへのクリックを捨てる。

#pragma once

#include <functional>

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QWidget>

class QTimer;

namespace cordmux::dock {

// 次のどれかが成り立つ間を並びの保留とする (判定は dock/ui/dock_ui_calc.h の layout_held)。
//   - ポインターが見えているタブのスクロール領域 (QScrollArea の viewport) の上で動いている。止まって set_idle_ms の時間が
//     たったら、動いていないとみなす。ホイールを回している間も動いているとみなす
//   - ドックの中でマウスのボタンが押されている
//   - ドックから開いたメニュー (ドックのメニューと行のメニュー) が開いている (set_menu_open で数える)
// 止まったら反映するのは、待っても変化が画面に出ないと、新しい行がいつまでも出ず、故障に見えるためである。
// 並びの保留の間も、行の中のテキスト・アイコン・チェックと 1 行目は書き換えてよい。何を後に回すかは使う側が決める。
class LayoutHold : public QObject {
public:
	// root はドックのルートのウィジェット (マウスのボタンが押されたかを見る範囲)、viewport はスクロール領域の viewport。
	// root を親にするので、root と一緒に削除される。
	LayoutHold(QWidget *root, QWidget *viewport);

	// 見ているスクロール領域を替える (タブを切り替えたとき)。替えた時点のポインターの位置で判定し直す。
	void set_viewport(QWidget *viewport);
	// ポインターが止まってから反映するまでの時間 (ms。設定 layout_hold_idle_ms)。0 なら、動いている間も保留しない。
	void set_idle_ms(int ms);
	~LayoutHold() override;

	LayoutHold(const LayoutHold &) = delete;
	LayoutHold &operator=(const LayoutHold &) = delete;

	bool held() const { return held_; }

	// ドックから開いたメニューの開閉を伝える (開く前に true、閉じた後に false)。入れ子でもよい (数で持つ)。
	void set_menu_open(bool open);

	// 並びの保留が始まったとき (true) と、条件が全部終わったとき (false) に 1 回ずつ呼び出す。使う側は false の
	// ときに、後に回した変化を反映する。
	void set_on_changed(std::function<void(bool held)> cb) { on_changed_ = std::move(cb); }

	// 現在のポインターの位置で判定し直す (ウィジェットを追加したり外したりした直後など)。
	void reevaluate();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	bool pointer_over_viewport() const;
	bool inside_root(QObject *watched) const;
	bool inside_viewport(const QWidget *w) const;
	void note_pointer_moved();
	void update_state();

	QPointer<QWidget> root_;
	QPointer<QWidget> viewport_;
	QTimer *idle_timer_ = nullptr;
	int idle_ms_ = 2000;
	bool pointer_over_ = false;
	bool moving_ = false;
	bool pressed_ = false;
	int menus_open_ = 0;
	bool held_ = false;
	std::function<void(bool)> on_changed_;
};

// 押せるウィジェット 1 つに付けるガード。arm() から QStyleHints::mouseDoubleClickInterval (Windows の既定は
// 500 ms) の間、マウスのボタンを押すイベントとダブルクリックを捨てる。コンストラクターでも arm() を呼ぶ。キーの
// 入力は捨てない (キーの入力はフォーカスのあるウィジェットに届くので、並びが変わっても狙っていないものを押さない)。
// ポインターが外から近づく途中に並びが変わったときに、狙っていないボタンや行を押すのを防ぐ (Chromium の
// InputEventActivationProtector と同じ考え方)。target を親にする。
class PressGuard : public QObject {
public:
	explicit PressGuard(QWidget *target);
	// target が現れたときと動いたときに呼び出す。
	void arm();
	bool armed() const;

	// target にガードが付いていれば、そのガードの arm() を呼ぶ (付いていなければ何もしない)。
	static void arm_widget(QWidget *target);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QElapsedTimer since_;
	bool started_ = false;
};

} // namespace cordmux::dock

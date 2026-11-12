// SPDX-License-Identifier: Apache-2.0

// フォーカスリング (FocusRing) と、フォーカスに合わせたスクロール (reveal_focus_mark)。
// フォーカスを受けるウィジェットの押せる範囲を、1 px 空けて 2 px の線で囲む。色はテーマの本文の色にする。
// リングは、最後の入力がキーボードのときだけ表示する (マウスで押した後は表示しない)。フォーカスの理由
// (QFocusEvent::reason) では決めない。QDialog は開くときに Tab の理由でフォーカスを移すので、マウスで開いた
// ダイアログでもリングが表示されてしまうため。

#pragma once

#include <QColor>
#include <QPointer>
#include <QRect>
#include <QWidget>

class QScrollArea;

#include <vector>

namespace cordmux::dock::ui {

// 押せる範囲を自分で決めるウィジェットが継承するインターフェイス (参加者の行のチェックボックス RowCheck は、自分の
// hitButton と同じ矩形を返す)。ウィジェットの座標で返す。
class FocusHitRect {
public:
	virtual ~FocusHitRect() = default;
	virtual QRect focus_hit_rect() const = 0;
};

// ウィジェットの押せる範囲 (ウィジェットの座標)。FocusHitRect を継承していればその矩形、QPushButton なら
// SE_PushButtonBevel、QCheckBox と QRadioButton なら Qt がクリックを受ける矩形 (SE_CheckBoxClickRect、
// SE_RadioButtonClickRect)、ほかはウィジェットの矩形を返す。w が nullptr なら空の矩形を返す。
QRect focus_hit_rect_of(const QWidget *w);

// 最後の入力の監視 (アプリケーション全体のイベントフィルター) を、リングが残っていても外す。obs_module_unload から
// 呼び出す。モーダルでない設定ウィンドウを開いたまま OBS を終了すると、リングがモジュールより後まで残るので、
// その後に Qt がフィルターを呼ばないようにするため。呼んだ後は、リングを作っても監視を登録し直さない (リングは
// 表示されない)。呼び忘れても、cordmux.dll がアンロードされるときに同じ処理をする (focus_ring.cpp の FilterGuard)。
void release_focus_input_watch();

// フォーカスリングを描く透明なウィジェット (Qt の QFocusFrame と同じ考え)。scope (ドックのルートのウィジェットか
// ダイアログ) の子として 1 つ置き、フォーカスのあるウィジェットを追って描く。ウィジェットのスタイルシートを
// 書き換えず、レイアウトに入らないので配置を動かさない。テーマの標準のフォーカスの見た目 (入力欄の枠線の色など) は
// 消さずに残る。リングは scope の子のままなので、行を作り直しても一緒に消えない。描く範囲は、フォーカスのある
// ウィジェットの祖先のうち、スクロール領域の見える範囲 (QAbstractScrollArea の viewport) と scope だけで切る。
// 最後の入力の監視は、アプリケーション全体のイベントフィルターを 1 つだけ使う。最初のリングを作ったときに登録し、
// 最後のリングを削除したときに外す。プラグインをアンロードした後にフィルターが残ると、OBS がクラッシュするため。
class FocusRing : public QWidget {
public:
	// scope の中のウィジェットにフォーカスがあるときだけ描く。リングは scope の子になる。
	explicit FocusRing(QWidget *scope);
	~FocusRing() override;

	void set_color(const QColor &color);
	void set_metrics(int ring_px, int gap_px);

	QWidget *target() const { return target_.data(); }
	// 最後の入力がキーボードか (アプリケーション全体で 1 つの値)。
	static bool keyboard_mode();

	// フォーカスのあるウィジェットと最後の入力から、表示するかと、どこに表示するかを決め直す (監視する側が
	// 呼び出す)。
	void update_ring();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

private:
	void retarget(QWidget *now);
	void watch(QWidget *target);
	void unwatch();
	void place();

	QPointer<QWidget> scope_;
	QPointer<QWidget> target_;
	std::vector<QPointer<QWidget>> watched_;
	QColor color_;
	int ring_px_ = 2;
	int gap_px_ = 1;
	QRect clip_; // 自分の座標で、描いてよい範囲
};

// ---- フォーカスに合わせたスクロール ----
//
// スクロール領域の中のウィジェット w にフォーカスが移ったとき、ウィジェットの全体 (縦と横) と、フォーカスリング
// (押せる範囲 focus_hit_rect_of を、reach (太さ + 空き) だけ広げた矩形) の縦の範囲が見えていればスクロールせず、
// 見えていなければ、すべてが入るまで縦にスクロールする。リングの横は見ない (横にはスクロールしない)。リングのうち、
// スクロール領域の中身の外にはみ出す部分 (どうスクロールしても見えない) は除く。余白は Qt の ensureWidgetVisible の
// 既定と同じ 50 px。ウィジェットとリングが見える範囲より高ければ、リングの上端に合わせる。
// ドック (dock_widget.cpp)、設定ウィンドウ (settings_window.cpp)、確認・結果・失敗のダイアログ (message_dialog.cpp)
// が、この同じ判定を使う。Qt の ensureWidgetVisible は使わない。ウィジェットの矩形が見えていればスクロールしないので、
// 見える範囲の端で、リングの 1 辺が切れたまま止まるため。
// focus_mark_rect は、scroll の中身 (QScrollArea::widget) の座標で、w のリングまでを含む矩形を返す (中身の外は除く)。
QRect focus_mark_rect(const QScrollArea *scroll, const QWidget *w, int reach);
// w の全体と、w のリングの縦の範囲が、scroll の見える範囲に入っているか。
bool focus_mark_shown(const QScrollArea *scroll, const QWidget *w, int reach);
// 上の規則でスクロールする。scroll か w が nullptr のときと、w が scroll の中身の子孫でないときは何もしない。
void reveal_focus_mark(QScrollArea *scroll, const QWidget *w, int reach);

// フォーカスに合わせたスクロールをしてよいフォーカスの理由か。キーボード (Tab、Backtab、Shortcut) と、プログラムが
// 戻したフォーカス (Other、Popup) ではスクロールし、ウィンドウがアクティブになって戻っただけ (ActiveWindow) と、
// マウスで押したウィジェット (Mouse) ではスクロールしない。この 2 つでスクロールすると、クリックが届く前に一覧が動き、
// 押した行と違う行にクリックが届くことがあるため (詳しくは dock/dock_widget.cpp のコメント)。
bool scroll_follows_focus(Qt::FocusReason reason);

} // namespace cordmux::dock::ui

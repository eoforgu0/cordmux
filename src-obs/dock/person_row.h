// SPDX-License-Identifier: Apache-2.0

// 参加者の一覧の 1 行。並びは [チェック + アバター + 名前 + タグ / 2 行目] [状態のアイコン] [状態のテキスト] [解像度]
// [画面のアイコン] [マイク] [スピーカー] [...]。行は user_id をキーにして作り、更新のたびに作り直さない。書き換えは
// PersonRowWidget::apply だけで行う。幅と省略の計算のうち、Qt に依存しない部分は name_check.h にある。

#pragma once

#include <QAbstractButton>
#include <QCheckBox>
#include <QFontMetrics>
#include <QPointer>
#include <QPushButton>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dock/name_check.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/dock_style.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/status_icons.h"
#include "dock/ui/theme_read.h"
#include "ledger/actions.h"
#include "ledger/dock_view.h"

class QHBoxLayout;
class QLabel;
class QSpacerItem;

namespace cordmux::dock {

namespace ui {
class ElideLabel;
class MarkLabel;
} // namespace ui

// 行の描画に使う値。PeopleList が持ち、全部の行に同じ値を渡す。
struct RowEnv {
	ui::DockMetrics metrics;
	ui::ThemeRead theme;
	ui::DockStyle style;
	bool muted_line2 = false;    // 2 行目と解像度に薄い色のテキスト (text-muted) を使ってよいか
	double disabled_alpha = 1.0; // 押せない行の名前のラベルの不透明度 (絵文字まで同じ薄さにするため)
	QColor enabled_text;         // 押せない行のラベル (名前と 2 行目) に使う、押せるときの本文の色 (不透明度と合わせて押せない色にする)
	int text_indent_px = 0;      // チェックボックスのインジケーターの左端から名前のテキストの左端まで (Vesktop のアカウントの行も同じ位置にそろえる)
	double status_keep_em = rows::kStatusTextKeepEm; // 名前を残すために状態のテキストを縮めるときの下限 (プレビューツールが変えて描く)
};

bool operator==(const RowEnv &a, const RowEnv &b);
inline bool operator!=(const RowEnv &a, const RowEnv &b)
{
	return !(a == b);
}

// 名前、タグ ([メイン]、[自動])、2 行目のラベルを持ち、位置を自分で決める。レイアウトに入れると、ラベルのテキストの幅が行の最小の幅になる
// ためである。チェックのウィジェット (RowCheck) が使う。
class NameBlock {
public:
	NameBlock(QWidget *owner);

	void set_name(const QString &full);
	void set_name_mode(Qt::TextElideMode mode);
	// 名前の右に置くタグ。名前に下限の幅を残せないときは、その行のタグを隠す (読み上げとツールチップには残らないが、[メイン] は
	// 上部の行で、[自動] は行のメニューで分かる)。
	void set_tags(bool main_tag, bool auto_tag);
	// 2 行目の部分 (並びは View のまま)。short_text はユーザー名の値だけの形 (なければ空)。value はテキストの中の値
	// (「声: alice」の alice。なければ空) で、省いたときに残す先頭 (「声:」のような語 + 値の最初の 1 字) を測るのに使う。
	struct Part {
		rows::PartKind kind = rows::PartKind::Other;
		QString full;
		QString short_text;
		QString value;
	};
	void set_line2(const std::vector<Part> &parts);
	void set_env(const RowEnv &env);
	void set_dimmed(bool dimmed);

	// 位置を決め直す (owner の resizeEvent から呼び出す)。left はテキストの左端、line1_h は 1 行目の高さ。
	void place(int left, int width, int line1_h);
	int height_for(int line1_h) const;
	bool has_line2() const { return !parts_.empty(); }

	QString full_name() const;
	QString shown_name() const;
	int name_width() const;
	// 2 行目のどれかを省いたか落としたか (落とした部分はツールチップに入れる)。
	bool line2_shortened() const { return line2_shortened_; }
	std::vector<QString> line2_full() const;
	// 見えているテキストの右端 (owner の座標)。押せる範囲の右端になる。
	int text_right() const;
	ui::ElideLabel *name_label() const { return name_; }
	std::vector<QLabel *> line2_labels() const { return labels_; }
	int line2_height() const;

private:
	void fit_line2(int width);
	void place_name_and_tags(int width);
	QLabel *make_tag(const char *key, const char *tip_key = nullptr);

	QWidget *owner_;
	ui::ElideLabel *name_ = nullptr;
	QLabel *main_tag_ = nullptr;
	QLabel *auto_tag_ = nullptr;
	bool want_main_tag_ = false;
	bool want_auto_tag_ = false;
	std::vector<Part> parts_;
	std::vector<QLabel *> labels_;
	RowEnv env_;
	bool dimmed_ = false;
	bool line2_shortened_ = false;
	int left_ = 0;
	int line1_h_ = 0;
};

// 行のチェック (テキストを持たない QCheckBox)。押せる範囲を、チェックボックスのインジケーターの左端から、見えている
// テキストの右端までにし、高さは行の全部にする (hitButton とフォーカスリングの FocusHitRect が同じ矩形)。名前の右の
// 空き、状態のアイコンとテキストを押しても何も起きないので、ドックにフォーカスを移すつもりで行の空いた所を押しても、
// 人を枠に載せない。
class RowCheck : public QCheckBox, public ui::FocusHitRect {
public:
	explicit RowCheck(QWidget *parent);

	NameBlock &names() { return names_; }
	const NameBlock &names() const { return names_; }
	void set_line1_height(int h);
	void set_env(const RowEnv &env);
	void set_press_guard(std::function<bool()> blocked) { press_blocked_ = std::move(blocked); }
	// 枠が埋まっている間のチェック。薄く描き、押しても状態を変えない (押すと「入れ替える相手」のメニューを開く)。
	void set_replaces(bool replaces);
	// アバター。image が空なら、color の円に letter を描く。shown が false なら場所も取らない。
	void set_avatar(const QPixmap &image, const QString &letter, const QColor &color);
	void set_avatar_shown(bool shown);
	bool avatar_shown() const { return avatar_shown_; }

	QRect indicator_rect() const;
	int text_left() const; // チェックボックスのインジケーターの左端から、アバターの左端まで
	int name_left() const; // チェックボックスのインジケーターの左端から、名前の左端まで
	QRect focus_hit_rect() const override;
	QRect qt_click_rect() const; // Qt の既定 (SE_CheckBoxClickRect)。テストとプレビューツールが比べる

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;
	std::function<void()> on_geometry_changed;

protected:
	void paintEvent(QPaintEvent *event) override;
	bool hitButton(const QPoint &pos) const override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void changeEvent(QEvent *event) override;
	void nextCheckState() override;

private:
	NameBlock names_;
	RowEnv env_;
	int line1_h_ = 0;
	bool replaces_ = false;
	bool avatar_shown_ = true;
	QPixmap avatar_image_;
	QString avatar_letter_;
	QColor avatar_color_;
	std::function<bool()> press_blocked_;
};

// 行の [...] (OBS のクラス btn-tool icon-dots-vert)。Enter でもメニューを開く。現れた直後と動いた直後のクリックは捨てる。
class DotsButton : public QPushButton {
public:
	explicit DotsButton(QWidget *parent);
	void set_env(const RowEnv &env);
	void set_press_guard(std::function<bool()> blocked) { press_blocked_ = std::move(blocked); }

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
	std::function<bool()> press_blocked_;
	bool fallback_icon_ = false; // cordmux のアイコン (3 つの点) をこのウィジェットが入れている
};

// 行のミュートのボタン (声はマイク、画面共有の音はスピーカー)。押した状態がミュート中で、押しても自分では状態を
// 変えない。表示の状態は set_muted だけが変え、押した結果は次の周期の View (RowAudio) で変わる。押した結果と
// 画面が食い違ったまま残らないようにするため。押せるときは背景を付けて、押せないアイコンと区別する。ミュートしていないときの
// 図は補足のテキストの色、ミュート中は本文の色に危険の色の斜線を重ねる。話している間は、成功の色の枠線と図にする (配信に
// 出ている声だけ。Pipeline::audio_levels の speaking)。現れた直後と動いた直後のクリックは捨てる。
class MuteButton : public QAbstractButton {
public:
	MuteButton(ui::AudioMark mark, QWidget *parent);
	void set_env(const RowEnv &env, int side_px);
	void set_muted(bool muted);
	// 話しているか。値が変わったときだけ、このボタンを塗り直す (1 秒に 10 回呼ばれる)。
	void set_speaking(bool speaking);
	bool speaking() const { return speaking_; }
	void set_press_guard(std::function<bool()> blocked) { press_blocked_ = std::move(blocked); }
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *event) override;
	void nextCheckState() override {}
	void keyPressEvent(QKeyEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
	ui::AudioMark mark_;
	RowEnv env_;
	int side_px_ = 24;
	bool speaking_ = false;
	std::function<bool()> press_blocked_;
};

// ミュートのボタンの一辺。[...] の高さにそろえ (行の高さを変えない)、図より小さくならないようにする。
int mute_button_side(const QSize &dots_hint, int icon_px);

// 参加者の一覧の 1 行のウィジェット。
//   - Vesktop のアカウントの行 (View の checkable が false) は、自分の枠のチェックを置き (self_frame_check)、[...] には
//     [自動で載せる] だけのメニューを置く (ミュートのボタンは置かない)。
//   - ミュートのボタン (声と画面共有の音) は、合成のソースがあるとき (RowAudio::applies) だけ表示し、ない間も場所を空ける。
//     [画面のアイコン][声][画面共有の音] の順で置く。画面共有の音のボタンは、その人が画面共有の音を持たない間も場所を空け、
//     画面のアイコンと声のボタンの位置をどの行でも動かさない。
//   - 画面のアイコンは、どの行でも同じ横の位置に置き、共有していない行も幅を確保する。中の四角で枠の位置を示す。
//   - 接続が切れた行 (View の off) は、薄くして押せなくする。
//   - 2 行目は部分ごとのラベルを横に並べ、部分の間は大きい余白にする。区切りの文字は描かない。狭いときの落とし方は
//     name_check.h の fit_line2。
//   - 行の状態のテキスト (refused_note も同じ所に表示する) と解像度は、行が使える幅で決め、全部の行に同じ結果を
//     適用する。名前と状態のテキストの全文が両方は入らないときは、状態のテキストを下限 (kStatusTextKeepEm) まで先に
//     縮めて、名前の全文を残す (name_check.h の split_name_status)。解像度は、名前と状態のテキストの全文を削らずに
//     入るときだけ表示する。状態は、幅にかかわらず短い形 (「映像途切れ」「映像待ち」など) を表示する
//     (name_check.h の status_short_key_for)。全文は、ツールチップの 1 行目とアクセシブル名に残る。
//   - 名前のラベルと、その右のまとまり (状態のアイコンから [...] まで) の間と、テキスト (状態のテキストと解像度) と
//     画面共有のアイコンの列の間は大きい余白にし、まとまりの中のほかの間は小さい余白にする (状態のアイコンは自分の
//     テキストの側に寄せる)。画面共有のアイコンと [...] の間は、[...] のフォーカスリングの外に背景が 1 px 以上残る
//     幅にする (name_check.h の dots_gap_px)。
//   - 状態のアイコンのツールチップと accessibleDescription には、状態のテキストの後に「枠に何が映っているか」の 1 文を
//     追加する。
//   - 現れた直後と動いた直後は、ダブルクリックの時間だけクリックを受け付けない。
class PersonRowWidget : public QWidget {
public:
	PersonRowWidget(const std::string &user_id, const RowEnv &env, QWidget *parent);

	const std::string &user_id() const { return user_id_; }
	void set_env(const RowEnv &env);

	// View を反映する。allow_shape_change が false (並びの保留の間) なら、行の高さを変える書き換え (2 行目の有無、
	// チェックの有無) は後に残し、ほかをその場で書き換える。
	void apply(const ledger::dockview::PersonRow &view, bool allow_shape_change);
	// 反映した View と比べて、行の高さかウィジェットの形が変わるか (並びの保留で後に回す変化か)。
	bool shape_differs(const ledger::dockview::PersonRow &view) const;
	const ledger::dockview::PersonRow &view() const { return view_; }
	bool shape_pending() const { return shape_pending_; }

	// 並びの保留の間に通話から抜けた人の行にする (押せなくする)。フォーカスは PeopleList が先に移す。
	void set_departed(bool departed);
	bool departed() const { return departed_; }

	// 狭いときに消す項目を適用する。resolution_allowed は全部の行で同じ値にする (1 つの行でも収まらなければ、
	// 全部の行で表示しない)。
	void set_width_rules(const rows::WidthRules &rules, bool resolution_allowed);
	bool resolution_shown() const;
	// 解像度を表示しても、名前の幅の下限 (4 em) と行の状態のテキストの全文を削らずに 1 行に収まるか。
	bool resolution_would_fit(const rows::WidthRules &rules) const;
	// 中のレイアウトを今すぐやり直す (幅で消す項目を決めた直後に、ウィジェットの幅を測るため)。
	void activate_layouts();

	// 名前の見分け。
	QString full_name() const;
	int name_width() const;
	void set_name_mode(Qt::TextElideMode mode);
	Qt::TextElideMode name_mode() const { return name_mode_; }
	QString shown_name() const;
	QFontMetrics name_font_metrics() const;
	// ツールチップ、アクセシブル名、accessibleDescription を、現在の省き方と押せる状態で決め直す。
	void refresh_accessibility();

	// クリックを捨てるガード。changed_at_ms は単調時計の ms。
	void mark_changed(std::int64_t now_ms) { changed_at_ms_ = now_ms; }
	bool press_blocked_now() const;

	// 話しているか (マイクのボタンだけを塗り直す)。
	void set_speaking(bool speaking);
	// アバターの画像 (空なら色の円と頭文字)。
	void set_avatar(const QPixmap &image);

	RowCheck *check() const { return check_; }
	DotsButton *dots() const { return dots_; }
	MuteButton *voice_mute() const { return voice_mute_; }
	MuteButton *screen_mute() const { return screen_mute_; }
	QWidget *focus_target() const; // 行で最初にフォーカスを受けるウィジェット (チェック。なければ [...])

	std::function<void()> on_check_clicked;
	std::function<void(QWidget *anchor, const QPoint &global_pos, bool keyboard)> on_menu_requested;
	// ミュートのボタンが押された。押した時点の View の値を反転した操作を渡す (ledger::ActionId::SetVoiceMute か
	// SetScreenAudioMute と、user_id と on)。
	std::function<void(ledger::ActionId, const ledger::ActionArgs &)> on_audio_action;
	std::function<void()> on_geometry_changed;

	// 単調時計の ms (クリックのガードと、動いた時刻の記録が同じ時計を使う)。
	static std::int64_t now_ms();

protected:
	void contextMenuEvent(QContextMenuEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void build_name_widget();
	void update_texts();
	void update_tail();
	bool audio_buttons_shown() const; // ミュートのボタンを置く行か (RowAudio::applies で、チェックのある行)
	void update_audio_buttons();
	void update_share_mark();
	void update_avatar_fallback();
	// チェックを押せるかを変える。押せなくするとき焦点がチェックにあれば、先に同じ人の [...] へ移す。
	void set_check_enabled(bool enabled);
	// 行の状態のテキストと、それに続ける 1 文 (枠に何が映っているか。画質の状態では理由の文) の行。状態がなければ空。
	// 状態のアイコンのツールチップと、行のチェックの accessibleDescription が同じものを使う。
	std::vector<QString> state_lines() const;
	// 名前と状態のテキストの幅の分け方 (name_check.h の split_name_status) と、共有の列の前の空きを、現在の行の幅と
	// ウィジェットの表示と非表示に合わせる。何度呼んでも同じ結果になる (値が変わらなければウィジェットに触らない)。
	void fit_tail();
	int name_left() const; // 行の左端から名前のテキストの左端まで
	// 名前より後ろのウィジェットの幅 (tail_layout_ と同じ数え方)。status_text_px は状態のテキストのラベルに与える幅。
	int tail_width(bool status_text, int status_text_px, bool resolution) const;
	int share_gap_extra() const;
	int dots_gap_extra() const; // 共有のアイコンと [...] の間のうち、レイアウトの spacing (小さい余白) に足す幅
	int line1_height() const;
	void relayout_heights();

	std::string user_id_;
	RowEnv env_;
	ledger::dockview::PersonRow view_;
	bool has_view_ = false;
	bool shape_pending_ = false;
	bool departed_ = false;
	rows::WidthRules rules_;
	bool resolution_hidden_by_fit_ = false;
	Qt::TextElideMode name_mode_ = Qt::ElideRight;
	std::int64_t changed_at_ms_ = 0;

	QHBoxLayout *layout_ = nullptr;
	RowCheck *check_ = nullptr;
	QWidget *tail_ = nullptr;
	QHBoxLayout *tail_layout_ = nullptr;
	ui::MarkLabel *status_mark_ = nullptr; // cordmuxRowStatus
	ui::ElideLabel *status_text_ = nullptr; // cordmuxRowStatusText
	// 状態のテキストのラベルに短い形を表示しているか (update_tail が決める)。全文は、ツールチップとアクセシブル名に入れる。
	bool status_shown_short_ = false;
	QLabel *resolution_ = nullptr;          // cordmuxRowResolution
	ui::MarkLabel *share_ = nullptr;        // cordmuxRowShare
	QSpacerItem *share_gap_ = nullptr;      // テキスト (状態のテキストと解像度) と共有の列の間を大きい余白にするための空き
	QSpacerItem *dots_gap_ = nullptr;       // スピーカーのボタンと [...] の間を広げるための空き
	QPixmap avatar_image_;
	MuteButton *voice_mute_ = nullptr;      // cordmuxRowVoiceMute
	MuteButton *screen_mute_ = nullptr;     // cordmuxRowScreenMute
	DotsButton *dots_ = nullptr;            // cordmuxRowMenuButton
};

} // namespace cordmux::dock

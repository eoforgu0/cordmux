// SPDX-License-Identifier: Apache-2.0

// 音声タブの中身。参加者ごとに 1 つのまとまり (AudioRowWidget) を縦に並べる。1 人は、名前と、声の行と、画面共有の音声の行からなる。
// 行は [声または画面の見出し] [つまみと、その下のメーター] [パーセント] [ミュートのボタン] の並びで、つまみの場所には、声が
// 届いていない人と共有していない人の文を同じ高さで置く。行の並びと状態は ledger/audio_tab.h が決めた AudioTabView で、ここは
// 描くだけである。行がないときは、その理由の 1 文を一覧の場所に置く。

#pragma once

#include <QPointer>
#include <QResizeEvent>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dock/dock_look.h"
#include "dock/person_row.h"
#include "dock/row_menu.h"
#include "dock/ui/level_meter.h"
#include "dock/ui/volume_slider.h"
#include "ledger/actions.h"
#include "ledger/dock_view.h"
#include "pipeline/audio_mixer.h"

class QLabel;
class QGridLayout;
class QVBoxLayout;

namespace cordmux::dock {

namespace ui {
class ElideLabel;
class WrapLabel;
} // namespace ui

// 押された操作を DockPoll に渡すコールバックの型 ActionCallback は row_menu.h にある。

// 行の描画に使う値。AudioTab が持ち、全部の行に同じ値を渡す。
struct AudioEnv {
	RowEnv row; // ミュートのボタンが読む値 (メトリクス、テーマ、スタイル)
	ui::MeterColors meter;
	int mute_side = 24; // ミュートのボタンの一辺
	QColor rule;        // 行の下の細い線
};

// 声の行か画面共有の音声の行。
enum class AudioKind { Voice, Screen };

// 音声タブの 1 人。
class AudioRowWidget : public QWidget {
public:
	AudioRowWidget(const std::string &user_id, const AudioEnv &env, QWidget *parent);

	const std::string &user_id() const { return user_id_; }
	void set_env(const AudioEnv &env);
	// View を反映する。行の高さは View によらず一定なので、並びの保留の間も呼んでよい。
	void apply(const ledger::dockview::AudioRowView &view);
	const ledger::dockview::AudioRowView &view() const { return view_; }

	// 並びの保留の間に参加者タブの一覧から消えた人の行にする (操作できなくする)。
	void set_departed(bool departed);
	bool departed() const { return departed_; }

	// メーターの値。track が nullptr (その組の音がない) なら 0 にする。つまみを置かない行と、薄くした行も 0 にする。
	void set_level(AudioKind kind, const pipeline::AudioTrackLevel *track);

	// 動いた直後の押し間違いを防ぐため、つまみとミュートのボタンへのクリックを、ダブルクリックの時間だけ捨てる。
	void arm_guards();

	ui::VolumeSlider *slider(AudioKind kind) const;
	MuteButton *mute(AudioKind kind) const;
	ui::LevelMeter *meter(AudioKind kind) const;
	QLabel *value_label(AudioKind kind) const;
	QLabel *kind_label(AudioKind kind) const; // 見出し (声、画面)
	ui::ElideLabel *note_label(AudioKind kind) const;
	ui::ElideLabel *name_label() const { return name_; }
	// この行の名前の全文の幅 (px。上限なし)。
	int wanted_name_px() const;
	// 全員の名前の全文の幅の最大を渡す (AudioTab が渡す)。名前の欄の幅は、この値と行の幅から、どの行も同じに決まる
	// (calc::name_column_px)。つまみの位置が行によらずそろう。
	void set_wanted_name_px(int px);
	// つまみの場所に文を置いているか (偽ならつまみとメーターを置いている)。
	bool showing_note(AudioKind kind) const;

	std::function<void(ledger::ActionId, const ledger::ActionArgs &)> on_action;
	// 行がフォントかスタイルの変化で測り直した後に呼ぶ (AudioTab が名前の欄の幅をそろえ直す)。
	std::function<void()> on_remeasured;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	// フォントかスタイルが変わったら (テーマの変更、polish がまだのまま作った行が後から polish された)、幅と高さを測り直す。
	void changeEvent(QEvent *event) override;
	// 見出しとパーセントのラベルのフォントかスタイルが変わったときも、測り直す。
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	struct Line {
		QLabel *kind = nullptr;
		QWidget *controls = nullptr;
		ui::VolumeSlider *slider = nullptr;
		ui::LevelMeter *meter = nullptr;
		ui::ElideLabel *note = nullptr;
		bool note_on = false;
		QLabel *value = nullptr;
		MuteButton *mute = nullptr;
	};

	Line &line(AudioKind kind) { return kind == AudioKind::Voice ? voice_ : screen_; }
	const Line &line(AudioKind kind) const { return kind == AudioKind::Voice ? voice_ : screen_; }
	void build_line(Line &line, AudioKind kind, int grid_row);
	void apply_line(AudioKind kind, const ledger::dockview::AudioLine &spec);
	void apply_texts();
	// 行の幅と高さを決める。最初に行を polish し、見出しとパーセントの幅は、それぞれのラベル自身のフォントと余白で測る。
	void relayout();
	// 測り直しをイベントループに積む (同じ周の要求は 1 回にまとめる)。relayout の中で起きた要求は捨てる。
	void request_relayout();
	// 行の幅と、全員の名前の全文の幅から、名前の欄の幅を決める。
	void apply_name_width();
	// フォーカスが inside の中にあれば、一覧そのものに移す (Qt は隠したウィジェットのフォーカスを次のウィジェットに移すので、
	// 続けて押したキーが別のつまみやボタンに働く)。
	void move_focus_off(QWidget *inside);

	std::string user_id_;
	AudioEnv env_;
	ledger::dockview::AudioRowView view_;
	bool has_view_ = false;
	bool departed_ = false;
	QGridLayout *grid_ = nullptr;
	int wanted_px_ = 0;
	int name_min_ = 0;
	int name_cap_ = 0;
	int slider_min_ = 0;
	int slider_comfort_ = 0;
	int others_w_ = 0;
	bool in_relayout_ = false;
	bool relayout_queued_ = false;
	ui::ElideLabel *name_ = nullptr;
	Line voice_;
	Line screen_;
};

// 音声タブの中身。
//   - 行は user_id をキーにして持ち続け、更新のたびに作り直さない。前の値と同じ行は何もしない。
//   - 位置が動く変化 (行の追加と削除、並べ替え、一覧と文の切り替え) は、並びの保留 (set_layout_hold(true)) の間はためておき、解けた
//     時点でまとめて反映する。つまみを動かしている間は、押している間の保留に入るので、行が動かない。行の中の値とテキスト、
//     ミュートの状態は、行の高さを変えないので並びの保留の間もその場で書き換える。
//   - 並びの保留の間に参加者から消えた人の行は残したまま、操作できなくする。フォーカスがその行にあれば、先に一覧そのものに移す。
//   - 幅は状態で変えない。最小の幅は minimum_row_width_px() で、ドックの最小幅に入る。
class AudioTab : public QWidget {
public:
	explicit AudioTab(QWidget *parent);

	// 行と文を書き換える。位置が動く変化は、並びの保留の間は後に回す。
	void update(const ledger::dockview::AudioTabView &view);
	// 音の大きさ (Pipeline::audio_levels の tracks)。値が変わったメーターだけを塗り直す。
	void set_levels(const std::vector<pipeline::AudioTrackLevel> &tracks);
	// メーターをすべて 0 にする (タブが見えなくなったとき)。
	void clear_levels();

	void set_look(const DockLook &look);
	void set_layout_hold(bool held);
	bool has_pending_layout() const;
	void set_action_callback(ActionCallback cb) { on_action_ = std::move(cb); }

	// 1 人の最小の幅。人がいなくても同じ値になる。
	int minimum_row_width_px() const;

	// プレビューツールとドックを操作するテストが読む関数。
	QWidget *list_widget() const { return list_; }
	AudioRowWidget *row_for(const std::string &user_id) const;
	std::vector<std::string> shown_user_ids() const { return shown_order_; }
	// 行がないときの文を表示しているか。
	bool notice_shown() const;
	QString notice_text() const;

private:
	void ensure_env();
	AudioEnv make_env();
	void apply_structure();
	void apply_in_place();
	bool structure_differs() const;
	AudioRowWidget *make_row(const std::string &user_id);
	void remove_row(const std::string &user_id);
	// 全員の名前の全文の幅の最大から、どの行も同じ名前の欄の推奨の幅にする。
	void fit_name_column();
	void fix_tab_order();
	void apply_notice();

	ActionCallback on_action_;
	DockLook look_;
	bool has_look_ = false;
	AudioEnv env_;
	bool env_ready_ = false;
	mutable int min_row_width_ = -1;

	QVBoxLayout *outer_ = nullptr;
	QWidget *list_ = nullptr; // cordmuxAudioList。行が消えるときにフォーカスを移す先
	QVBoxLayout *list_layout_ = nullptr;
	QWidget *notice_ = nullptr;
	ui::WrapLabel *notice_text_ = nullptr;

	ledger::dockview::AudioTabView latest_;
	bool has_latest_ = false;
	std::vector<std::string> shown_order_;
	bool notice_visible_ = false;
	std::map<std::string, AudioRowWidget *> rows_;
	bool held_ = false;
};

} // namespace cordmux::dock

// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールのウィンドウ。左にプレビュー (キャンバス全体を縮めた表示と、名前ラベルのあたりの等倍と
// 2 倍) と場面 (人数と人、名前ラベルの隅、話す、背景)、右に合成のソースの名前ラベルのつまみを並べる。つまみは
// config::Settings を変え、描画は tuner_renderer.h が製品と同じコンポーネントで行う。画面のスレッドだけから使う。

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include <QImage>
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QWidget>

#include <string>

#include "config/settings.h"
#include "speak_track.h"
#include "tuner_compositor.h"
#include "tuner_knobs.h"
#include "tuner_renderer.h"
#include "tuner_state.h"

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTimer;
class QVBoxLayout;

namespace cordmux::label_tuner {

class FitView;
class PixelView;
class WheelSafeComboBox;
class WheelSafeSlider;
class WheelSafeSpinBox;

class TunerWindow : public QWidget {
public:
	struct Options {
		QString save_path; // [保存] の書き先。空なら [保存] は押せない
		QString load_note; // 起動のときに読んだファイルについて、状態の行に出す文 (空なら出さない)
		// --shot。描き直しのスレッドを作らず (tick がその場で描く)、タイマーも動かさない。
		bool shot = false;
	};

	// clock は時刻 (ns。0 より大きく、前に返した値より小さくならない)。画面では実際の時刻、--shot では引数で
	// 決めた時刻を返す。
	TunerWindow(const Options &options, const config::Settings &settings, const ToolState &state,
		    std::function<std::uint64_t()> clock);
	~TunerWindow() override;

	// 1 回分の更新。時刻を読み、描き終えた画像を受け取り、変わっていれば重ね直して表示を更新する。画面では 16 ms ごとの
	// タイマーが呼び、--shot では呼び出し元が呼ぶ。
	void tick();

	// 席 seat (0 から 3) の [話している] (cycle が偽) か「2 秒話して 1 秒黙る」(cycle が真) を、時刻 at_ns に切り替える。
	// 画面の切り替えも合わせる。
	void set_speaking(int seat, bool cycle, bool on, std::uint64_t at_ns);

	// 画面共有していない参加者の行 row (0 から 7) の [話している] か「2 秒話して 1 秒黙る」を、時刻 at_ns に切り替える。
	void set_roster_speaking(int row, bool cycle, bool on, std::uint64_t at_ns);

	// 置いている画像がどれも、現在のつまみの値と場面で描いたもので、表示がそれを重ねた最後の画像か。等倍と 2 倍の
	// 表示を、名前ラベルが見えるようにスクロールし終えたか。
	bool all_current() const;

	// 直近の重ねにかかった時間 (ms。キャンバス全体と、等倍で見る範囲)。
	double full_compose_ms() const { return full_compose_ms_; }
	double crop_compose_ms() const { return crop_compose_ms_; }

	// いま表示しているキャンバス (背景と、名前ラベルと光を重ねたもの)。
	const QImage &canvas() const { return canvas_; }

	// --shot の説明の JSON。dpr はウィンドウを描いた画像の devicePixelRatio で、等倍と 2 倍の表示の位置を、その画像の
	// ピクセルで書く。
	QJsonObject describe(double dpr) const;

	// 見えている QLabel、QCheckBox、QPushButton、QComboBox のうち、sizeHint の幅が実際の幅より広い物 (文字が切れている)。
	// スクロールの外に隠れている物と、折り返す QLabel は除く。各要素は {class, text, width, hint}。
	QJsonArray clipped_widgets() const;

	// ---- つまみごとの見本 (--sweep) が使う。どれも利用者の操作と同じ経路 (ウィジェットの値を変える) で変える ----

	// 数のつまみ key を value にする。via_slider が真ならスライダーを、偽なら数の欄を動かす。key が数のつまみで
	// なければ false。
	bool set_knob_number(const std::string &key, double value, bool via_slider);
	// チェックボックスのつまみ key を on にする。
	bool set_knob_toggle(const std::string &key, bool on);
	// 色 (`#RRGGBB`) かフォントの名前 (空は製品の既定のフォント) のつまみ key を text にする。
	bool set_knob_text(const std::string &key, const QString &text);
	// 選択肢のつまみ key の選択肢 index を選ぶ。選べない選択肢 (配置に合わない並び) は選ばず false を返す。
	bool set_knob_choice(const std::string &key, int index);
	// 画面共有していない参加者の人数 (0 から 8)、最後の 1 人を声を聞けない人にするか、等倍と 2 倍で見る行 (0 から。
	// 範囲の外ならタイルに戻す)。
	void set_roster_people(int count);
	void set_roster_last_unheard(bool on);
	void set_view_roster(int row);
	// [既定に戻す] と [保存] を押す (ボタンの clicked を通す)。
	void reset_knobs();
	void press_save();
	// 人数を変える。
	void set_people(int count);
	// 等倍と 2 倍で見るタイルを変える (範囲の外なら最後のタイル)。
	void set_view_tile(int tile);
	// つまみの並びを、key のつまみが見えるようにスクロールする。
	void focus_knob(const std::string &key);

	// ---- 画面と同じ経路の確認 (--drive) が使う ----

	// tick のたびに呼ぶ関数に渡す値。seat_level は席ごとの明るさで、人数の外の席は seat_shown が偽。
	struct TickInfo {
		std::uint64_t now_ns = 0;
		std::array<double, sources::kCompositeCellCount> seat_level{};
		std::array<bool, sources::kCompositeCellCount> seat_shown{};
	};
	void set_tick_observer(std::function<void(const TickInfo &)> observer) { tick_observer_ = std::move(observer); }
	// 席 seat の [プッシュ・トゥ・トーク]、[話している]、[2 秒話して 1 秒黙る]、チェックボックスのつまみ key の
	// チェックボックス。
	// マウスの押下と離しを送るために使う。
	QPushButton *hold_button(int seat) const;
	QCheckBox *speak_box(int seat) const;
	QCheckBox *cycle_box(int seat) const;
	QCheckBox *knob_check(const std::string &key) const;
	// 背景のコンボボックスの項目 index を選ぶ (画像のファイルの項目は選ばない)。
	void set_background_choice(int index);
	// 全体を縮めた表示と、等倍と 2 倍の表示を描いた回数。
	int fit_paints() const;
	int pixel_paints() const;
	// 現在のつまみの値と場面を、描き直しのスレッドも置いている画像も使わずにその場で描き、表示しているキャンバスと
	// 違うピクセルの数を返す (0 なら、表示は新しく描いたものと同じ)。表示していなければ -1。
	long long fresh_mismatch() const;

private:
	struct KnobWidgets {
		const KnobDef *def = nullptr;
		WheelSafeSlider *slider = nullptr;
		WheelSafeSpinBox *spin = nullptr;
		QLabel *label = nullptr;
		QCheckBox *check = nullptr;
		WheelSafeComboBox *combo = nullptr;
		QLineEdit *hex = nullptr;
		QPushButton *swatch = nullptr;
	};

	struct SeatRow {
		QWidget *box = nullptr;
		WheelSafeComboBox *person = nullptr;
		QCheckBox *speak = nullptr;
		QPushButton *hold = nullptr;
		QCheckBox *cycle = nullptr;
		QLabel *info = nullptr;
	};

	// 画面共有していない参加者の行。席の行と同じコンポーネントで、隅の選択はない (位置は並びの設定が決める)。
	struct RosterRow {
		QWidget *box = nullptr;
		WheelSafeComboBox *person = nullptr;
		QCheckBox *speak = nullptr;
		QPushButton *hold = nullptr;
		QCheckBox *cycle = nullptr;
		QLabel *info = nullptr;
	};

	QWidget *build_scene_panel();
	QWidget *build_knob_panel();
	// row_layout は、1 行にまとめるチェックボックスの保存先 (そうでなければ nullptr)。
	void add_knob(const KnobDef &def, QWidget *parent, QVBoxLayout *layout, QHBoxLayout *row_layout);
	// つまみのヒントを、現在の値で付け直す。
	void refresh_hints();

	// つまみの値を画面に入れる (変更の通知は出さない)。
	void load_knob_widgets();
	// つまみか場面が変わった。描き直しを頼み、次の tick で重ね直す。
	void on_scene_changed();
	void set_color(KnobWidgets &knob, const QString &hex);

	decor_preview::SceneRequest make_request() const;
	sources::BorderTiming timing() const;
	void rebuild_background_if_needed();
	// キャンバス全体を重ねる仕事を頼む。
	void request_full_compose(const std::vector<decor_preview::SceneTileImages> &tiles,
				  const std::vector<decor_preview::SceneRosterImages> &roster);
	// 等倍と 2 倍の表示に見えている範囲だけを重ねる仕事を頼む。
	void request_crop_compose(const std::vector<decor_preview::SceneTileImages> &tiles,
				  const std::vector<decor_preview::SceneRosterImages> &roster, const QRect &region);
	// 等倍と 2 倍の表示に見えている、キャンバスの範囲。
	QRect visible_crop_region() const;
	// 重ね終えた画像を受け取って表示する。受け取ったら true。
	bool take_composed();
	// 等倍と 2 倍で見るタイルと、そのタイルの名前ラベルの位置。
	struct ViewTarget {
		bool valid = false;
		QRect extent;       // 見せる範囲 (名前ラベルと光と、名前ラベルの側の枠の隅に余白を足した範囲)
		QRect label;        // 名前ラベルの矩形 (光を含めない。置いていなければ空)
		QPoint cell_corner; // 名前ラベルの側の枠の隅のピクセル
		sources::DecorCorner corner = sources::DecorCorner::BottomLeft;
	};
	ViewTarget view_target() const;

	// 等倍と 2 倍の表示のスクロールを決める元。これが変わったときだけ、名前ラベルが見えるようにスクロールし直す
	// (変わらない間は、利用者が手で動かしたスクロールを変えない)。
	struct ScrollGoal {
		QRect extent;
		QRect label;
		QPoint cell_corner;
		sources::DecorCorner corner = sources::DecorCorner::BottomLeft;
		QSize one_port;
		QSize two_port;
		double dpr = 1.0;
		bool operator==(const ScrollGoal &o) const
		{
			return extent == o.extent && label == o.label && cell_corner == o.cell_corner &&
			       corner == o.corner && one_port == o.one_port && two_port == o.two_port && dpr == o.dpr;
		}
		bool operator!=(const ScrollGoal &o) const { return !(*this == o); }
	};
	// スクロールし直すのが済んでいなければ (スクロールエリアがまだ新しい大きさを知らなければ)、もう一度試す。
	void apply_scroll_goal();
	void rebuild_view_choices();
	void update_seat_rows();
	// 並びの向きの選択肢のうち、現在の配置に合わないものを押せなくする。
	void update_choice_states();
	void update_status();
	void open_picture();
	void save();
	void reset_to_defaults();

	Options options_;
	config::Settings settings_;
	ToolState state_;
	std::function<std::uint64_t()> clock_;
	TunerRenderer renderer_;
	// 席 (0 から 3) の後ろに、画面共有していない参加者の行 (4 から 11) が続く。
	std::array<SpeakTrack, sources::kCompositeCellCount + kRosterRows> tracks_;

	QImage picture_;
	QImage background_;
	std::vector<QRect> background_cells_;
	BackgroundMode background_mode_ = BackgroundMode::Mixed;
	int background_kind_ = 0;
	bool background_dirty_ = true;

	QImage canvas_;
	std::vector<double> levels_;      // タイルの光の明るさ
	std::vector<double> dims_;        // タイルの名前ラベルの色の倍率
	std::vector<double> roster_levels_; // 画面共有していない参加者の光の明るさ
	std::vector<double> roster_dims_;
	bool dirty_ = true;
	QRect crop_;              // 等倍と 2 倍で見せる範囲
	QRect crop_region_;       // そのうち、重ねるよう頼んだ範囲 (見えている所)
	QRect crop_region_shown_; // そのうち、重ね終えて表示している範囲
	ScrollGoal scroll_goal_;
	bool scroll_pending_ = false;
	std::uint64_t compose_serial_ = 0;
	std::uint64_t last_shown_serial_full_ = 0;
	std::uint64_t last_shown_serial_crop_ = 0;
	double full_compose_ms_ = 0.0;
	double crop_compose_ms_ = 0.0;
	// キャンバス全体を重ねる仕事と、等倍と 2 倍で見る範囲だけを重ねる仕事。範囲だけの方が軽いので、光が大きくても
	// 等倍の表示はなめらかに動く (全体を縮めた表示は、重ね終えた順に追いつく)。
	ComposeWorker full_worker_;
	ComposeWorker crop_worker_;
	QString note_;
	bool loading_ = false;

	FitView *fit_ = nullptr;
	PixelView *one_ = nullptr;
	PixelView *two_ = nullptr;
	QScrollArea *one_scroll_ = nullptr;
	QScrollArea *two_scroll_ = nullptr;
	WheelSafeComboBox *view_combo_ = nullptr;
	QSpinBox *people_spin_ = nullptr;
	QSpinBox *roster_spin_ = nullptr;
	QCheckBox *roster_unheard_ = nullptr;
	std::array<RosterRow, kRosterRows> roster_rows_;
	std::array<SeatRow, sources::kCompositeCellCount> rows_;
	WheelSafeComboBox *background_combo_ = nullptr;
	QLabel *picture_label_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *save_button_ = nullptr;
	QPushButton *reset_button_ = nullptr;
	std::vector<KnobWidgets> knobs_;
	QScrollArea *knob_scroll_ = nullptr;
	QScrollArea *scene_scroll_ = nullptr;
	std::string focus_key_; // focus_knob で見えるようにしたつまみ (--shot の JSON に位置を書く)
	QTimer *timer_ = nullptr;
	std::function<void(const TickInfo &)> tick_observer_;
};

} // namespace cordmux::label_tuner

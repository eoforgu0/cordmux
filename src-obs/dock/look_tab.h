// SPDX-License-Identifier: Apache-2.0

// 表示タブの中身。上から「枠」「名前ラベル」「話している人の強調」の 3 つの組を縦に並べる。配信の画面を見ながら値を動かすタブで、
// 変えた値は [適用] なしですぐ配信の画面に渡す。値を動かしている間は、渡す回数を ledger::kLookReflectIntervalMs ごとに
// 減らし、最後の値は必ず渡す。保存は渡す側 (DockPoll) が、変更の少し後にまとめる (dock/deferred_save.h)。
//
// 判断は持たない。入力欄を押せるかは ledger::look_control_enabled、外から届いた値で入力欄を書き換えるかは
// ledger::look_field_follows、渡す間隔は ledger::look_reflect_wait_ms が決める。ダミー映像の切り替えは ledger の View
// (LookTabView) の ViewButton で、押したら ActionId::ToggleDummy をコールバックに渡す。
//
// どの入力欄もホイールを受け取らず、スクロールに渡す (ui/no_wheel.h)。幅は状態で変えず、最小の幅でも中身が横にはみ出さない
// (コンボボックスは文字数で幅を決め、入りきらない項目は省略する。ラベルは折り返す)。

#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "config/settings.h"
#include "dock/dock_look.h"
#include "ledger/actions.h"
#include "ledger/dock_view.h"
#include "ledger/look_tab.h"
#include "sources/self_speaking_judge.h"

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QFormLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QTimer;

namespace cordmux::dock {

namespace ui {
class SliderSpin;
class WrapLabel;
} // namespace ui

class LookRule;
class AxisPair;

class LookTab : public QWidget {
public:
	explicit LookTab(QWidget *parent = nullptr);
	~LookTab() override;

	// 設定の値 (DockTab の行) を入力欄へ反映する。周期ごとに呼んでよい。利用者が操作している入力欄と、変えた値をまだ渡して
	// いない間は書き換えない。値が同じなら何もしない。
	void set_values(const config::Settings &settings);
	// ledger の View (ダミー映像の切り替えと、メインアカウントの有無)。
	void set_view(const ledger::dockview::LookTabView &view);
	// テーマから読んだ見た目。
	void set_look(const DockLook &look);
	// 配信の画面の大きさ (フォントサイズのツールチップに px を表示する。0 は分からないこと)。
	void set_canvas_size(int width, int height);

	// 変えた値を渡す先。引数の設定は、このタブが持つ現在の値で、DockTab の行だけを読む。
	void set_on_changed(std::function<void(const config::Settings &)> callback) { on_changed_ = std::move(callback); }
	// ダミー映像の切り替えを押したときに呼ぶ。
	void set_action_callback(std::function<void(ledger::ActionId, const ledger::ActionArgs &)> callback)
	{
		on_action_ = std::move(callback);
	}
	// OBS のソースの候補を集める関数 (自分の枠に映す映像と、自分の枠の音声ソース)。OBS を呼ぶので、タブを開いたときと
	// コンボボックスを開いたときにだけ呼ぶ。
	using Candidates = std::function<std::vector<sources::SelfSourceCandidate>()>;
	void set_source_providers(Candidates frame, Candidates speaking);
	// ソースの候補を集め直して、コンボボックスの選択肢を作り直す (タブを開いたとき)。コンボボックスを開いている間は何もしない。
	void refresh_sources();

	// 変えたまま渡していない値があれば、今渡す (OBS を閉じるとき、タブを離れるとき)。
	void flush();
	bool has_unreflected() const { return dirty_; }

	// 入力欄を、json のキーで探す (複数のキーを持つ入力欄は、「+」でつないだ形でも、キー 1 つでも見つかる)。テストと
	// プレビューツールが使う。
	QWidget *field(const QString &json_keys) const;
	// いま入力欄が持つ値 (DockTab の行だけが意味を持つ)。
	const config::Settings &edited() const { return edited_; }
	QCheckBox *dummy_check() const { return dummy_check_; }
	// 押せない入力欄か (テストが読む)。
	bool control_enabled(ledger::LookControl control) const;

private:
	// 設定の色の値 (#RRGGBB の文字列) を指すメンバー。
	using ColorMember = std::string config::Settings::*;

	struct Field {
		QWidget *widget = nullptr;
		std::function<void()> load;          // edited_ から入力欄へ (signal を出さない)
		std::function<void()> store;         // 入力欄から edited_ へ
		std::function<bool()> editing;       // 利用者が操作している最中か
	};
	// ほかの入力欄の値で押せるかが決まる入力欄 (ラベルと入力欄)。
	struct Dependent {
		ledger::LookControl control;
		std::vector<QWidget *> widgets;
	};

	void build();
	void register_field(QWidget *widget, const std::string &keys, std::function<void()> load, std::function<void()> store,
			    std::function<bool()> editing);
	QString field_in_group_name(const char *label_key, const char *group_key) const;
	void on_field_changed();
	void update_dependents();
	// [先頭に配置] を押せるかを決め直す (ledger::look_self_slot_first_enabled)。
	void update_self_first_enabled();
	void request_reflect();
	void reflect_now();
	void load_all();
	void fill_source_combo(QComboBox *combo, const std::vector<sources::SelfSourceCandidate> &choices,
			       const std::string &uuid, const std::string &name, bool is_frame);
	int fill_font_weights(int weight, bool pick_nearest);
	void update_font_size_tip();
	void update_color_swatch(QLabel *swatch, const std::string &hex);
	void choose_color(ColorMember member, QLabel *swatch, const QString &title);
	void apply_type_levels();

	DockLook look_;
	config::Settings edited_;
	ledger::dockview::LookTabView view_;
	bool loading_ = false;
	bool dirty_ = false;
	std::int64_t last_reflect_ms_ = 0;
	QTimer *reflect_timer_ = nullptr;
	int canvas_width_ = 0;
	int canvas_height_ = 0;
	std::function<void(const config::Settings &)> on_changed_;
	std::function<void(ledger::ActionId, const ledger::ActionArgs &)> on_action_;
	Candidates frame_provider_;
	Candidates speaking_provider_;
	std::vector<sources::SelfSourceCandidate> frame_choices_;
	std::vector<sources::SelfSourceCandidate> speaking_choices_;

	std::vector<Field> fields_;
	std::vector<Dependent> dependents_;
	// 大見出し (枠、名前ラベル、話している人の強調、ダミー映像) と中見出し (名前、輪郭、アバターなど)。中見出しの bool は、
	// 大見出しのすぐ後の中見出しか (上の余白を大見出しの下の余白だけにする)。
	std::vector<QLabel *> group_headings_;
	std::vector<std::pair<QLabel *, bool>> sub_headings_;
	// 大きい分類の間のはっきりした罫線。中くらいの分類の間には罫線を引かない (中見出しの太字と余白で分ける)。
	std::vector<LookRule *> strong_rules_;
	// 見出しのない、意味の違う部分の間の余白 (光り方と光る条件の間)。
	std::vector<QWidget *> section_gaps_;
	std::vector<std::pair<QLabel *, ColorMember>> swatches_;

	QFormLayout *form_ = nullptr;
	QComboBox *frame_combo_ = nullptr;
	// [先頭に配置]。押せるかはメインアカウントの有無 (view_) でも変わるので、ほかの入力欄の値で決まる入力欄 (dependents_) と
	// 分けて持つ (update_self_first_enabled)。
	QCheckBox *self_first_check_ = nullptr;
	QComboBox *speaking_combo_ = nullptr;
	QFontComboBox *font_family_ = nullptr;
	QComboBox *font_weight_ = nullptr;
	QComboBox *roster_direction_ = nullptr;
	// 画面共有していない参加者のオフセットの X と Y の欄の組と、それぞれの「X」と欄を並べるレイアウト。すき間はテーマの値で
	// 決める (apply_type_levels)。
	AxisPair *offset_pair_ = nullptr;
	std::vector<QHBoxLayout *> offset_axis_layouts_;
	ui::SliderSpin *font_size_ = nullptr;
	QCheckBox *dummy_check_ = nullptr;
	ui::WrapLabel *dummy_note_ = nullptr;
	QWidget *dummy_note_box_ = nullptr; // dummy_note_ の入れ物 (ui::note_box)
};

} // namespace cordmux::dock

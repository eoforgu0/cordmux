// SPDX-License-Identifier: Apache-2.0

// look のシナリオ。ドックの表示タブ (枠、名前ラベル、話している人の強調) を確認する。画面は開かない。
//   前半 (入力欄): 表示タブの入力欄だけを取り出して、入力欄の値を直接変える。
//     (a) 表の DockTab の行のすべてに入力欄がある。
//     (b) 入力欄を 1 つ変えると、変えたキーだけが変わる (手で書いた桁の多い値と選択肢にない太さを含む)。
//     (c) 数の入力欄の最小、最大、刻みの境、刻みに合わない値を、設定に書いて読み直せる。
//     (d) 配置を変えると並びが直り、合わない並びは選べない項目になる。
//     (e) 押せなくする条件。
//     (f) 自分の枠の音声ソースと、自分の枠に映す映像の選択肢。[先頭に配置] を押せる条件。
//     (g) 太さの選択肢。
//   後半 (ドック): SimPoll のドックの表示タブを、ポインターで操作する。
//     (1) 値を変えると、保存の前に、配信の画面へ渡す呼び出しが起きる。
//     (2) 保存は最後の変更の 1 秒後に 1 回にまとまる。動かしている間の渡す回数は間引かれ、最後の値は必ず渡す。
//     (3) ホイールで値が変わらず、ページがスクロールする (つまみ、数の入力欄、コンボボックス)。
//     (4) 設定ウィンドウの [適用] で、表示タブの値が古い値に戻らない。
//     (5) [変更] で変えたメインアカウントが、[適用] で戻らない。
//     (6) ダミー映像を切り替えると、1 行目が変わる。
//     (7) 表示タブで値を変えても、1 行目、メインとサブの行、タブのバー、下のバーの位置が動かない。
//     (8) 外から届いた値は、操作していない入力欄に反映し、操作している入力欄は書き換えない。
//     (9) OBS を閉じるとき (flush) に、渡していない値と待っている保存を行う。
//     (10) 色のダイアログは、ドックの操作だけを止める。
//     (17) [先頭に配置] を外すと、自分の枠の行の位置と配信の配置の位置が、同じだけ変わる。

#include "scenarios.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "config/settings.h"
#include "dock/accounts_bar.h"
#include "dock/bottom_bar.h"
#include "dock/dock_tabs.h"
#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/inline_confirm.h"
#include "dock/look_tab.h"
#include "dock/settings_fields.h"
#include "dock/settings_window.h"
#include "dock/status_tab.h"
#include "dock/ui/slider_spin.h"
#include "dock/ui/text_resolve.h"
#include "drive_env.h"
#include "fixtures.h"
#include "input.h"
#include "obs_dock.h"
#include "ledger/look_tab.h"
#include "ledger/observation.h"
#include "ledger/text_ids.h"
#include "query.h"
#include "scen_common.h"
#include "script.h"
#include "sim_poll.h"
#include "sources/self_speaking_judge.h"

namespace cordmux::dock_drive {

namespace {

using S = config::Settings;
namespace keys = ledger::text::keys;

const QString kGroup = QStringLiteral("look");

// 取り出した表示タブと、変えた値の記録。
struct Opened {
	std::unique_ptr<dock::LookTab> tab;
	std::shared_ptr<std::vector<S>> applied = std::make_shared<std::vector<S>>();

	QWidget *field(const std::string &key) const { return tab->field(QString::fromStdString(key)); }
	dock::ui::SliderSpin *number(const std::string &key) const { return dynamic_cast<dock::ui::SliderSpin *>(field(key)); }
};

// 音声ソースの候補 (名前の順)。
std::vector<sources::SelfSourceCandidate> self_source_choices()
{
	return {{"uuid-game", "ゲーム音声", true}, {"uuid-mic", "マイク", true}};
}

// 映像のソースの候補 (名前の順)。
std::vector<sources::SelfSourceCandidate> self_frame_choices()
{
	return {{"uuid-board", "キャプチャーボード", true}, {"uuid-window", "ゲーム画面", true}};
}

Opened open_tab(const S &current, std::vector<sources::SelfSourceCandidate> choices = self_source_choices(),
		std::vector<sources::SelfSourceCandidate> frame_choices = self_frame_choices(), bool has_main_account = false)
{
	Opened o;
	o.tab = std::make_unique<dock::LookTab>();
	o.tab->set_canvas_size(1920, 1080);
	o.tab->set_source_providers([frame_choices] { return frame_choices; }, [choices] { return choices; });
	ledger::dockview::LookTabView view;
	view.has_main_account = has_main_account;
	o.tab->set_view(view);
	auto applied = o.applied;
	o.tab->set_on_changed([applied](const S &s) { applied->push_back(s); });
	o.tab->refresh_sources();
	o.tab->set_values(current);
	return o;
}

nlohmann::json json_of(const S &s)
{
	return nlohmann::json::parse(config::settings_to_json(s));
}

// 設定を変える前の設定。設定のファイルを手で書いたときの、入力欄の桁より桁が多い値や、太さの選択肢にない値を含める
// (どれも設定として読める値)。
S hand_written_settings()
{
	S s;
	s.label_margin_pct = 3.25;
	s.label_outline_ratio = 0.125;
	s.label_avatar_ratio = 1.234;
	s.label_glow_blur_ratio = 0.333;
	s.label_glow_spread_ratio = 0.077;
	s.label_font_weight = 550;
	s.roster_gap_ratio = 0.35;
	s.label_dim_pct = 45;
	s.label_em_ratio_pct = 3.7;
	return s;
}

// 入力欄 1 つを、書き換わったと分かる別の値にする。変えられない種類 (色、フォント) では false を返す。
bool nudge(QWidget *w)
{
	if (qobject_cast<QPushButton *>(w) != nullptr || qobject_cast<QFontComboBox *>(w) != nullptr)
		return false;
	if (auto *check = qobject_cast<QCheckBox *>(w)) {
		check->setChecked(!check->isChecked());
		return true;
	}
	if (auto *number = dynamic_cast<dock::ui::SliderSpin *>(w)) {
		QDoubleSpinBox *spin = number->spin();
		const double up = spin->value() + spin->singleStep();
		spin->setValue(up <= spin->maximum() + 1e-9 ? up : spin->value() - spin->singleStep());
		return true;
	}
	if (auto *combo = qobject_cast<QComboBox *>(w)) {
		combo->setCurrentIndex((combo->currentIndex() + 1) % combo->count());
		return true;
	}
	return false;
}

// 数の入力欄の境の確認に使う、キーと範囲と刻み。範囲と刻みは設定の定数と同じ値を使う。
struct NumberField {
	const char *key;
	double lo;
	double hi;
	double step;
	std::function<double(const S &)> get;
};

std::vector<NumberField> number_fields()
{
	return {
		{"tile_gap_px", config::kTileGapMinPx, config::kTileGapMaxPx, 1,
		 [](const S &s) { return static_cast<double>(s.tile_gap_px); }},
		{"slot_background_opacity_pct", config::kSlotBackgroundOpacityMinPct, config::kSlotBackgroundOpacityMaxPct, 1,
		 [](const S &s) { return static_cast<double>(s.slot_background_opacity_pct); }},
		{"label_em_ratio_pct", config::kLabelEmRatioMinPct, config::kLabelEmRatioMaxPct, 0.1,
		 [](const S &s) { return s.label_em_ratio_pct; }},
		{"label_outline_ratio", config::kLabelOutlineRatioMin, config::kLabelOutlineRatioMax, 0.01,
		 [](const S &s) { return s.label_outline_ratio; }},
		{"label_avatar_ratio", config::kLabelAvatarRatioMin, config::kLabelAvatarRatioMax, 0.01,
		 [](const S &s) { return s.label_avatar_ratio; }},
		{"label_plate_opacity_pct", config::kLabelPlateOpacityMinPct, config::kLabelPlateOpacityMaxPct, 1,
		 [](const S &s) { return static_cast<double>(s.label_plate_opacity_pct); }},
		{"label_margin_pct", config::kLabelMarginMinPct, config::kLabelMarginMaxPct, 0.1,
		 [](const S &s) { return s.label_margin_pct; }},
		{"label_opacity_pct", config::kLabelOpacityMinPct, config::kLabelOpacityMaxPct, 1,
		 [](const S &s) { return static_cast<double>(s.label_opacity_pct); }},
		{"label_glow_spread_ratio", config::kLabelGlowSpreadRatioMin, config::kLabelGlowSpreadRatioMax, 0.01,
		 [](const S &s) { return s.label_glow_spread_ratio; }},
		{"label_glow_blur_ratio", config::kLabelGlowBlurRatioMin, config::kLabelGlowBlurRatioMax, 0.01,
		 [](const S &s) { return s.label_glow_blur_ratio; }},
		{"label_dim_pct", config::kLabelDimMinPct, config::kLabelDimMaxPct, config::kLabelDimStepPct,
		 [](const S &s) { return static_cast<double>(s.label_dim_pct); }},
		{"self_speaking_threshold_db", config::kSelfSpeakingThresholdMinDb, config::kSelfSpeakingThresholdMaxDb, 1,
		 [](const S &s) { return static_cast<double>(s.self_speaking_threshold_db); }},
		{"label_border_rise_ms", config::kLabelBorderTimingMinMs, config::kLabelBorderTimingMaxMs, 1,
		 [](const S &s) { return static_cast<double>(s.label_border_rise_ms); }},
		{"label_border_hold_ms", config::kLabelBorderTimingMinMs, config::kLabelBorderTimingMaxMs, 1,
		 [](const S &s) { return static_cast<double>(s.label_border_hold_ms); }},
		{"label_border_fade_ms", config::kLabelBorderTimingMinMs, config::kLabelBorderTimingMaxMs, 1,
		 [](const S &s) { return static_cast<double>(s.label_border_fade_ms); }},
		{"roster_gap_ratio", config::kRosterGapRatioMin, config::kRosterGapRatioMax, 0.05,
		 [](const S &s) { return s.roster_gap_ratio; }},
		{"roster_name_max_em", config::kRosterNameMaxEmMin, config::kRosterNameMaxEmMax, 0.5,
		 [](const S &s) { return s.roster_name_max_em; }},
		{"roster_offset_x_px", config::kRosterOffsetMinPx, config::kRosterOffsetMaxPx, 1,
		 [](const S &s) { return static_cast<double>(s.roster_offset_x_px); }},
		{"roster_offset_y_px", config::kRosterOffsetMinPx, config::kRosterOffsetMaxPx, 1,
		 [](const S &s) { return static_cast<double>(s.roster_offset_y_px); }},
	};
}

void set_number(QWidget *w, double value)
{
	if (auto *number = dynamic_cast<dock::ui::SliderSpin *>(w))
		number->spin()->setValue(value);
}

QStringList items_of(const QComboBox *c)
{
	QStringList out;
	for (int i = 0; i < c->count(); ++i)
		out << QStringLiteral("%1=%2").arg(c->itemText(i)).arg(c->itemData(i).toInt());
	return out;
}

// 入力欄の取り出し (前半)。
void run_look_fields(Results &results)
{
	auto q = [](const std::string &s) { return QString::fromStdString(s); };

	// このタブの入力欄を持つキー (表の DockTab の行)。
	std::vector<std::string> tab_keys;
	for (std::size_t i = 0; i < dock::settings_field_count(); ++i) {
		const dock::SettingsFieldRow &row = dock::settings_field_at(i);
		// 映像のバッファは状態タブの入力欄が持つ (scen_status.cpp)。
		const std::string key = row.json_key;
		if (row.owner == dock::SettingsOwner::DockTab && key != "display_buffer_ms")
			tab_keys.push_back(row.json_key);
	}

	// (a) 表のすべての行に入力欄がある。
	{
		Opened o = open_tab(S{});
		QStringList missing;
		for (const std::string &key : tab_keys) {
			if (o.field(key) == nullptr)
				missing << q(key);
		}
		results.expect_eq(kGroup, QStringLiteral("(a) 表の表示タブの行の数"), static_cast<long long>(tab_keys.size()), 52);
		results.expect_eq(kGroup, QStringLiteral("(a) 表の表示タブの行のすべてに、cordmuxLook_<キー> の入力欄がある"),
				  missing.size(), 0, missing.join(QLatin1Char(' ')));
	}

	// (b) 手で書いた値のファイルで、入力欄を 1 つだけ変えると、変えたキーのほかは書き換わらない。値を変えずに渡さない。
	{
		const S before = hand_written_settings();
		const nlohmann::json before_json = json_of(before);
		int tried = 0;
		QStringList wrong;
		for (const std::string &key : tab_keys) {
			Opened o = open_tab(before);
			QWidget *w = o.field(key);
			if (w == nullptr || !nudge(w))
				continue;
			++tried;
			o.tab->flush();
			if (o.applied->empty()) {
				wrong << q(key) + QStringLiteral("(渡していない)");
				continue;
			}
			const nlohmann::json after_json = json_of(o.applied->back());
			std::set<std::string> allowed{key};
			if (key == "roster_anchor")
				allowed.insert("roster_direction");
			// UUID と名前は 1 つのコンボボックスが書くので、どちらを指しても 2 つとも変わる。
			if (key == "self_speaking_source_uuid")
				allowed.insert("self_speaking_source_name");
			if (key == "self_speaking_source_name")
				allowed.insert("self_speaking_source_uuid");
			if (key == "self_frame_source_uuid")
				allowed.insert("self_frame_source_name");
			if (key == "self_frame_source_name")
				allowed.insert("self_frame_source_uuid");
			bool changed_self = false;
			for (auto it = before_json.begin(); it != before_json.end(); ++it) {
				if (it.value() == after_json.at(it.key()))
					continue;
				if (it.key() == key)
					changed_self = true;
				else if (allowed.count(it.key()) == 0)
					wrong << q(key) + QStringLiteral("->") + q(it.key());
			}
			if (!changed_self)
				wrong << q(key) + QStringLiteral("(変えたキーが変わらない)");
		}
		results.expect_eq(kGroup,
				  QStringLiteral("(b) 入力欄を 1 つ変えると、変えたキーだけが変わる (手で書いた桁の多い値と選択肢にない太さを含む)"),
				  wrong.size(), 0, QStringLiteral("%1 欄。%2").arg(tried).arg(wrong.join(QLatin1Char(' '))));

		Opened o = open_tab(before);
		o.tab->flush();
		results.expect_eq(kGroup, QStringLiteral("(b) 値を変えずに開いただけでは、渡さない"), static_cast<long long>(o.applied->size()), 0);
		results.expect_true(kGroup, QStringLiteral("(b) 値を変えない間、入力欄の値は開いたときの設定と同じ"),
				    json_of(o.tab->edited()) == before_json);
	}

	// (c) 数の入力欄の最小、最大、刻みの境、刻みに合わない値を設定のファイルの形に書いて、読み直せる。
	{
		QStringList bad;
		int checked = 0;
		for (const NumberField &f : number_fields()) {
			Opened o = open_tab(hand_written_settings());
			QWidget *w = o.field(f.key);
			if (w == nullptr) {
				bad << QString::fromLatin1(f.key) + QStringLiteral("(入力欄なし)");
				continue;
			}
			const double per_unit = std::round(1.0 / f.step);
			const std::vector<double> candidates{f.lo,
							     f.lo + f.step,
							     f.hi,
							     f.hi - f.step,
							     std::round((f.lo + f.hi) / 2 / f.step) * f.step,
							     f.lo + f.step * 0.4,
							     f.hi - f.step * 0.6,
							     f.lo + f.step * 2.5};
			for (const double v : candidates) {
				set_number(w, std::clamp(v, f.lo, f.hi));
				S parsed;
				std::string error;
				++checked;
				const bool ok = config::parse_settings(config::settings_to_json(o.tab->edited()), &parsed, &error);
				const double got = f.get(parsed);
				const double off_grid = std::fabs(got * per_unit - std::round(got * per_unit));
				if (!ok || got < f.lo - 1e-9 || got > f.hi + 1e-9 || off_grid > 1e-6)
					bad << QStringLiteral("%1=%2(%3)").arg(QString::fromLatin1(f.key)).arg(v).arg(q(error));
			}
		}
		results.expect_eq(kGroup,
				  QStringLiteral("(c) 数の入力欄の最小、最大、刻みの境、刻みに合わない値を設定に書いて、読み直せて、刻みと範囲に合う"),
				  bad.size(), 0, QStringLiteral("%1 通りを確認。%2").arg(checked).arg(bad.join(QLatin1Char(' '))));
	}

	// (c2) つまみと数の入力欄が同じ値になる。段数が多い入力欄 (時間) はつまみを置かない。
	{
		Opened o = open_tab(S{});
		dock::ui::SliderSpin *gap = o.number("tile_gap_px");
		dock::ui::SliderSpin *rise = o.number("label_border_rise_ms");
		results.expect_true(kGroup, QStringLiteral("(c2) 枠の間隔にはつまみがあり、光のフェードインにはつまみがない (段数が多い)"),
				    gap != nullptr && gap->slider() != nullptr && rise != nullptr && rise->slider() == nullptr);
		if (gap != nullptr && gap->slider() != nullptr) {
			gap->slider()->setValue(12);
			results.expect_true(kGroup, QStringLiteral("(c2) つまみを動かすと数の入力欄が同じ値になり、設定に書く"),
					    std::lround(gap->spin()->value()) == 12 && o.tab->edited().tile_gap_px == 12);
			gap->spin()->setValue(20);
			results.expect_true(kGroup, QStringLiteral("(c2) 数の入力欄を変えるとつまみが同じ値になる"),
					    gap->slider()->value() == 20 && o.tab->edited().tile_gap_px == 20);
		}
	}

	// (d) 配置を変えると、合う並びに直り、合わない並びは選べない項目になる。
	{
		Opened o = open_tab(S{});
		auto *anchor = qobject_cast<QComboBox *>(o.field("roster_anchor"));
		auto *direction = qobject_cast<QComboBox *>(o.field("roster_direction"));
		if (anchor == nullptr || direction == nullptr) {
			results.expect_true(kGroup, QStringLiteral("(d) 配置と並びの入力欄がある"), false);
		} else {
			auto *model = qobject_cast<QStandardItemModel *>(direction->model());
			auto enabled_items = [&] {
				std::vector<bool> out;
				for (int i = 0; i < direction->count(); ++i)
					out.push_back(model != nullptr && model->item(i) != nullptr && model->item(i)->isEnabled());
				return out;
			};
			const std::vector<bool> both{true, true};
			results.expect_true(kGroup, QStringLiteral("(d) 配置が右下のとき、並びは横並びと縦並びのどちらも選べる"), enabled_items() == both);
			anchor->setCurrentIndex(1); // 中央上
			results.expect_true(kGroup, QStringLiteral("(d) 配置を中央上にすると、並びは横並びに直り、縦並びは選べない"),
					    o.tab->edited().roster_direction == config::RosterDirection::Row &&
						    direction->currentData().toInt() == 0 && enabled_items() == std::vector<bool>({true, false}));
			anchor->setCurrentIndex(3); // 左中央
			results.expect_true(kGroup, QStringLiteral("(d) 配置を左中央にすると、並びは縦並びに直り、横並びは選べない"),
					    o.tab->edited().roster_direction == config::RosterDirection::Column &&
						    direction->currentData().toInt() == 1 && enabled_items() == std::vector<bool>({false, true}));
			anchor->setCurrentIndex(0); // 左上
			results.expect_true(kGroup, QStringLiteral("(d) 配置を左上にすると、並びはどちらも選べる (現在の並びは縦並びのまま)"),
					    o.tab->edited().roster_direction == config::RosterDirection::Column && enabled_items() == both);
		}
	}

	// (e) 押せなくする条件。
	{
		Opened o = open_tab(S{});
		auto en = [&](const char *key) {
			QWidget *w = o.field(key);
			return w != nullptr && w->isEnabled();
		};
		auto toggle = [&](const char *key, bool on) {
			if (auto *c = qobject_cast<QCheckBox *>(o.field(key)))
				c->setChecked(on);
		};
		const std::vector<const char *> name_only{"label_font_family", "label_font_weight", "label_em_ratio_pct",
							  "label_text_color", "label_show_outline", "label_outline_color", "label_outline_ratio"};
		const std::vector<const char *> glow_only{"label_glow_frame", "label_glow_avatar", "label_glow_name",
							  "label_border_color", "label_glow_spread_ratio", "label_glow_blur_ratio"};
		const std::vector<const char *> times{"label_border_rise_ms", "label_border_hold_ms", "label_border_fade_ms"};
		const std::vector<const char *> roster_rest{"roster_anchor",      "roster_offset_x_px", "roster_offset_y_px",
							    "roster_direction",   "roster_gap_ratio",   "roster_avatar_side",
							    "roster_show_bots"};
		auto all = [&](const std::vector<const char *> &list, bool want) {
			for (const char *k : list) {
				if (en(k) != want)
					return false;
			}
			return true;
		};
		results.expect_true(kGroup, QStringLiteral("(e) 既定: 名前、輪郭、アバターのサイズ、光、時間、配置の入力欄が押せる"),
				    all(name_only, true) && all(glow_only, true) && all(times, true) && all(roster_rest, true) &&
					    en("label_avatar_ratio") && en("label_dim_pct"));
		results.expect_true(kGroup, QStringLiteral("(e) 既定: 背景は表示なので、背景の [表示]、色、不透明度が押せる"),
				    en("label_show_plate") && en("label_plate_color") && en("label_plate_opacity_pct"));
		toggle("label_show_plate", false);
		results.expect_true(kGroup, QStringLiteral("(e) 背景の [表示] を切ると、色と不透明度が押せず、[表示] は押せる"),
				    en("label_show_plate") && !en("label_plate_color") && !en("label_plate_opacity_pct"));
		toggle("label_show_outline", false);
		results.expect_true(kGroup, QStringLiteral("(e) 輪郭の [表示] を切ると、輪郭の色とサイズが押せず、[表示] は押せる"),
				    en("label_show_outline") && !en("label_outline_color") && !en("label_outline_ratio") &&
					    en("label_text_color") && en("label_em_ratio_pct"));
		toggle("label_show_outline", true);
		results.expect_true(kGroup, QStringLiteral("(e) 輪郭の [表示] を入れ直すと、輪郭の色とサイズが押せる"),
				    en("label_outline_color") && en("label_outline_ratio"));
		toggle("label_show_name", false);
		results.expect_true(kGroup, QStringLiteral("(e) 名前の [表示] を切ると、名前と輪郭の残りが押せず、[表示] は押せる"),
				    all(name_only, false) && en("label_show_name") && en("label_show_avatar") && en("label_avatar_ratio"));
		toggle("label_show_avatar", false);
		results.expect_true(kGroup, QStringLiteral("(e) 名前とアバターの [表示] を切ると、アバターのサイズと背景のまとまり全体が押せない"),
				    !en("label_avatar_ratio") && !en("label_show_plate") && !en("label_plate_color"));
		toggle("label_show_avatar", true);
		toggle("label_show_plate", true);
		results.expect_true(kGroup, QStringLiteral("(e) アバターが表示で背景の [表示] を入れると、背景の色と不透明度が押せる"),
				    en("label_plate_color") && en("label_plate_opacity_pct"));
		toggle("label_show_name", true);
		// 枠の背景: 名前ラベルの表示には依らず、枠の背景の [表示] だけで色と不透明度が決まる。
		results.expect_true(kGroup, QStringLiteral("(e) 既定: 枠の背景は表示なので、[表示]、色、不透明度が押せる"),
				    en("slot_background_show") && en("slot_background_color") && en("slot_background_opacity_pct"));
		toggle("slot_background_show", false);
		results.expect_true(kGroup, QStringLiteral("(e) 既定から枠の背景の [表示] を切ると、色と不透明度が押せず、[表示] は押せる"),
				    en("slot_background_show") && !en("slot_background_color") && !en("slot_background_opacity_pct"));
		toggle("slot_background_show", true);
		results.expect_true(kGroup, QStringLiteral("(e) 枠の背景の [表示] を入れると、色と不透明度が押せる"),
				    en("slot_background_color") && en("slot_background_opacity_pct"));
		// 画面共有している参加者の [表示]: 外すと [1人のときは非表示]、[位置]、[アバターの位置] が押せず、[表示] と名前ラベルの
		// 見た目 (並びの名前ラベルにも使う) は押せる。
		results.expect_true(kGroup, QStringLiteral("(e) 既定: 画面共有している参加者の [表示]、[1人のときは非表示]、[位置]、[アバターの位置] が押せる"),
				    en("label_show_in_slot") && en("label_hide_when_single") && en("label_corner") && en("label_avatar_side"));
		toggle("label_show_in_slot", false);
		results.expect_true(kGroup, QStringLiteral("(e) 画面共有している参加者の [表示] を切ると、ほかの 3 つが押せず、[表示] と名前の見た目は押せる"),
				    en("label_show_in_slot") && !en("label_hide_when_single") && !en("label_corner") &&
					    !en("label_avatar_side") && all(name_only, true) && en("roster_show"));
		toggle("label_show_in_slot", true);
		results.expect_true(kGroup, QStringLiteral("(e) 画面共有している参加者の [表示] を入れ直すと、3 つが押せる"),
				    en("label_hide_when_single") && en("label_corner") && en("label_avatar_side"));
		toggle("label_show_plate", false);
		results.expect_true(kGroup, QStringLiteral("(e) 名前ラベルの背景を切っても、枠の背景の色と不透明度は押せる"),
				    en("slot_background_color") && en("slot_background_opacity_pct"));
		toggle("slot_background_show", false);
		results.expect_true(kGroup, QStringLiteral("(e) 枠の背景の [表示] を切ると、色と不透明度が押せず、[表示] は押せる"),
				    en("slot_background_show") && !en("slot_background_color") && !en("slot_background_opacity_pct"));
		toggle("label_show_plate", true);
		toggle("label_show_speaking", false);
		results.expect_true(kGroup, QStringLiteral("(e) [光] を切ると、対象、色、太さ、ぼかしが押せず、暗さと [光] は押せる"),
				    all(glow_only, false) && en("label_dim_pct") && en("label_show_speaking"));
		results.expect_true(kGroup, QStringLiteral("(e) [光] が切れで暗さが 0 でないとき、時間の 3 つは押せる"), all(times, true));
		set_number(o.field("label_dim_pct"), 0);
		results.expect_true(kGroup, QStringLiteral("(e) [光] が切れで暗さが 0 のとき、時間の 3 つが押せない"), all(times, false));
		toggle("label_show_speaking", true);
		results.expect_true(kGroup, QStringLiteral("(e) [光] を入れると、暗さが 0 でも時間の 3 つと光の入力欄が押せる"),
				    all(times, true) && all(glow_only, true));
		toggle("roster_show", false);
		results.expect_true(kGroup, QStringLiteral("(e) 画面共有していない参加者の [表示] を切ると、そのまとまりの残りが押せず、[表示] は押せる"),
				    all(roster_rest, false) && en("roster_show"));
	}

	// (f) 自分の枠の音声ソース。
	{
		const auto combo_of = [](const Opened &o) { return qobject_cast<QComboBox *>(o.field("self_speaking_source_uuid")); };
		const auto uuid_of = [](const QComboBox *c, int i) { return c->itemData(i, Qt::UserRole).toString(); };

		{
			Opened o = open_tab(S{});
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 指定なしのとき、先頭の (なし) を選んでいて、候補の数は (なし) と候補の和"),
					    combo != nullptr && combo->currentIndex() == 0 && combo->count() == 3 && uuid_of(combo, 0).isEmpty());
			QWidget *threshold = o.field("self_speaking_threshold_db");
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 指定なしのとき、しきい値が押せない"),
					    threshold != nullptr && !threshold->isEnabled());
			if (combo != nullptr)
				combo->setCurrentIndex(2);
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 候補を選ぶと、UUID と名前が設定に入り、しきい値が押せる"),
					    o.tab->edited().self_speaking_source_uuid == "uuid-mic" &&
						    o.tab->edited().self_speaking_source_name == "マイク" && threshold != nullptr &&
						    threshold->isEnabled());
			if (combo != nullptr)
				combo->setCurrentIndex(0);
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: (なし) に戻すと、UUID と名前が空になり、しきい値が押せない"),
					    o.tab->edited().self_speaking_source_uuid.empty() &&
						    o.tab->edited().self_speaking_source_name.empty() && threshold != nullptr &&
						    !threshold->isEnabled());
		}
		{
			S s;
			s.self_speaking_source_uuid = "uuid-game";
			s.self_speaking_source_name = "古い名前";
			Opened o = open_tab(s);
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 保存した UUID の候補があれば、名前が違っていてもその候補を選ぶ"),
					    combo != nullptr && combo->count() == 3 && combo->currentIndex() == 1 &&
						    o.tab->edited().self_speaking_source_name == "古い名前");
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 開いただけでは、値を渡さない"), o.applied->empty());
		}
		{
			S s;
			s.self_speaking_source_uuid = "uuid-old";
			s.self_speaking_source_name = "マイク";
			Opened o = open_tab(s);
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: UUID は候補になく名前が合う候補があれば、その候補を選び、見つからない項目を足さない"),
					    combo != nullptr && combo->count() == 3 && combo->currentIndex() == 2);
		}
		{
			S s;
			s.self_speaking_source_uuid = "uuid-gone";
			s.self_speaking_source_name = "消えたマイク";
			Opened o = open_tab(s);
			QComboBox *combo = combo_of(o);
			const bool shown = combo != nullptr && combo->count() == 4 && combo->currentIndex() == 3 &&
					   uuid_of(combo, 3) == QStringLiteral("uuid-gone") &&
					   combo->currentText().contains(QStringLiteral("消えたマイク")) &&
					   combo->currentText() != QStringLiteral("消えたマイク");
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 保存したソースが見つからないとき、その名前に見つからないことを添えた項目を選んだままにする"),
					    shown, combo != nullptr ? combo->currentText() : QString());
			QWidget *threshold = o.field("self_speaking_threshold_db");
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 見つからないソースを選んでいる間も、しきい値が押せる"),
					    threshold != nullptr && threshold->isEnabled());
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 見つからないソースのまま開いただけでは、値を渡さない"),
					    o.applied->empty());
		}
		{
			Opened o = open_tab(S{}, {});
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f) 音声ソース: 候補がないとき、(なし) だけの一覧になる"),
					    combo != nullptr && combo->count() == 1);
		}
	}

	// (f2) 自分の枠に映す映像。音声ソースの欄と別の欄で、UUID と名前を別のキーに書く。
	{
		const auto combo_of = [](const Opened &o) { return qobject_cast<QComboBox *>(o.field("self_frame_source_uuid")); };
		const auto uuid_of = [](const QComboBox *c, int i) { return c->itemData(i, Qt::UserRole).toString(); };
		{
			Opened o = open_tab(S{});
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 指定なしのとき、先頭の (なし) を選んでいて、候補の数は (なし) と候補の和"),
					    combo != nullptr && combo->currentIndex() == 0 && combo->count() == 3 && uuid_of(combo, 0).isEmpty());
			auto *audio = qobject_cast<QComboBox *>(o.field("self_speaking_source_uuid"));
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 音声ソースの欄とは別の欄で、候補も別"),
					    combo != nullptr && audio != nullptr && combo != audio &&
						    combo->itemText(1) == QStringLiteral("キャプチャーボード") &&
						    audio->itemText(1) == QStringLiteral("ゲーム音声"));
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 欄に、ツールチップがある"), combo != nullptr && !combo->toolTip().isEmpty());
			if (combo != nullptr)
				combo->setCurrentIndex(2);
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 候補を選ぶと、映すソースの UUID と名前だけが設定に入る"),
					    o.tab->edited().self_frame_source_uuid == "uuid-window" &&
						    o.tab->edited().self_frame_source_name == "ゲーム画面" &&
						    o.tab->edited().self_speaking_source_uuid.empty());
			o.tab->flush();
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 選んだ UUID と名前が渡る"),
					    !o.applied->empty() && o.applied->back().self_frame_source_uuid == "uuid-window" &&
						    o.applied->back().self_frame_source_name == "ゲーム画面");
			if (combo != nullptr)
				combo->setCurrentIndex(0);
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: (なし) に戻すと、UUID と名前が空になる"),
					    o.tab->edited().self_frame_source_uuid.empty() && o.tab->edited().self_frame_source_name.empty());
		}
		{
			S s;
			s.self_frame_source_uuid = "uuid-board";
			s.self_frame_source_name = "古い名前";
			Opened o = open_tab(s);
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 保存した UUID の候補があれば、名前が違っていてもその候補を選ぶ"),
					    combo != nullptr && combo->count() == 3 && combo->currentIndex() == 1 &&
						    o.tab->edited().self_frame_source_name == "古い名前");
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 開いただけでは、値を渡さない"), o.applied->empty());
		}
		{
			S s;
			s.self_frame_source_uuid = "uuid-old";
			s.self_frame_source_name = "ゲーム画面";
			Opened o = open_tab(s);
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: UUID は候補になく名前が合う候補があれば、その候補を選び、見つからない項目を足さない"),
					    combo != nullptr && combo->count() == 3 && combo->currentIndex() == 2);
		}
		{
			S s;
			s.self_frame_source_uuid = "uuid-gone";
			s.self_frame_source_name = "消えた画面";
			Opened o = open_tab(s);
			QComboBox *combo = combo_of(o);
			const bool shown = combo != nullptr && combo->count() == 4 && combo->currentIndex() == 3 &&
					   uuid_of(combo, 3) == QStringLiteral("uuid-gone") &&
					   combo->currentText().contains(QStringLiteral("消えた画面")) &&
					   combo->currentText() != QStringLiteral("消えた画面");
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 保存したソースが見つからないとき、その名前に見つからないことを添えた項目を選んだままにする"),
					    shown, combo != nullptr ? combo->currentText() : QString());
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 見つからないソースのまま開いただけでは、値を渡さない"),
					    o.applied->empty());
			set_number(o.field("tile_gap_px"), 9);
			o.tab->flush();
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 見つからないソースのまま別の欄を変えても、保存した UUID と名前を残す"),
					    !o.applied->empty() && o.applied->back().self_frame_source_uuid == "uuid-gone" &&
						    o.applied->back().self_frame_source_name == "消えた画面");
		}
		{
			Opened o = open_tab(S{}, self_source_choices(), {});
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f2) 映すソース: 候補がないとき、(なし) だけの一覧になる"),
					    combo != nullptr && combo->count() == 1);
		}
		// 先頭の選択肢の文字列: メインアカウントがなければ (なし)、あれば「Discordの画面共有」。メインアカウントが変わったらすぐ
		// 替わる。どちらも保存する値は空のまま。
		{
			const QString none_text = dock::ui::resolve_key(keys::kLookNoneOption);
			const QString discord_text = dock::ui::resolve_key(keys::kLookSelfFrameDiscordOption);
			Opened o = open_tab(S{});
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f2) 映す映像: メインアカウントがなければ、先頭の選択肢は (なし)"),
					    combo != nullptr && !discord_text.isEmpty() && discord_text != none_text && combo->itemText(0) == none_text);
			ledger::dockview::LookTabView view;
			view.has_main_account = true;
			o.tab->set_view(view);
			results.expect_true(kGroup, QStringLiteral("(f2) 映す映像: メインアカウントができるとすぐ、先頭の選択肢が「Discordの画面共有」になる (値は空のまま)"),
					    combo != nullptr && combo->itemText(0) == discord_text && uuid_of(combo, 0).isEmpty() &&
						    o.tab->edited().self_frame_source_uuid.empty() && o.tab->edited().self_frame_source_name.empty());
			view.has_main_account = false;
			o.tab->set_view(view);
			results.expect_true(kGroup, QStringLiteral("(f2) 映す映像: メインアカウントがなくなると、先頭の選択肢が (なし) に戻る"),
					    combo != nullptr && combo->itemText(0) == none_text);
			Opened with_main = open_tab(S{}, self_source_choices(), self_frame_choices(), true);
			QComboBox *main_combo = combo_of(with_main);
			results.expect_true(kGroup, QStringLiteral("(f2) 映す映像: メインアカウントがあれば、先頭の選択肢が「Discordの画面共有」で、それを選んでいる"),
					    main_combo != nullptr && main_combo->itemText(0) == discord_text && main_combo->currentIndex() == 0);
		}
		// シーンの候補: 文字列は「シーン: 名前」。ソースの後ろに並ぶ。選ぶと UUID と名前がそのまま設定に入る。
		{
			std::vector<sources::SelfSourceCandidate> frame = self_frame_choices();
			frame.push_back({"uuid-scene", "配信前", true, true});
			Opened o = open_tab(S{}, self_source_choices(), frame);
			QComboBox *combo = combo_of(o);
			results.expect_true(kGroup, QStringLiteral("(f2) 映す映像: シーンの候補の文字列は「シーン: 名前」で、ソースの後ろに並ぶ"),
					    combo != nullptr && combo->count() == static_cast<int>(frame.size()) + 1 &&
						    combo->itemText(combo->count() - 1) ==
							    dock::ui::resolve_key(keys::kLookSelfFrameSceneOption).arg(QStringLiteral("配信前")) &&
						    combo->itemText(1) == QStringLiteral("キャプチャーボード"));
			if (combo != nullptr)
				combo->setCurrentIndex(combo->count() - 1);
			results.expect_true(kGroup, QStringLiteral("(f2) 映す映像: シーンを選ぶと、シーンそのものの UUID と名前が設定に入る (「シーン: 」は付かない)"),
					    o.tab->edited().self_frame_source_uuid == "uuid-scene" && o.tab->edited().self_frame_source_name == "配信前");
		}
	}

	// (f3) [先頭に配置]。自分の枠に映す映像が「(なし)」の間 (メインアカウントがなく、OBS の映像を選んでいない) だけ押せない。
	{
		Opened o = open_tab(S{});
		auto *first = qobject_cast<QCheckBox *>(o.field("self_slot_first"));
		auto *frame = qobject_cast<QComboBox *>(o.field("self_frame_source_uuid"));
		results.expect_true(kGroup, QStringLiteral("(f3) [先頭に配置] がある。文字列は ini のとおりで、既定は付いている"),
				    first != nullptr && first->text() == dock::ui::resolve_key(keys::kLookSelfSlotFirstCheck) &&
					    first->isChecked());
		results.expect_true(kGroup, QStringLiteral("(f3) 映す映像が「(なし)」の間は押せない"),
				    first != nullptr && !first->isEnabled());
		ledger::dockview::LookTabView view;
		view.has_main_account = true;
		o.tab->set_view(view);
		results.expect_true(kGroup, QStringLiteral("(f3) メインアカウントができる (先頭の選択肢が「Discordの画面共有」になる) と、すぐ押せる"),
				    first != nullptr && first->isEnabled());
		view.has_main_account = false;
		o.tab->set_view(view);
		const bool back_disabled = first != nullptr && !first->isEnabled();
		if (frame != nullptr)
			frame->setCurrentIndex(1);
		results.expect_true(kGroup, QStringLiteral("(f3) メインアカウントがなくなると押せず、OBS のソースを選ぶと押せる"),
				    back_disabled && first != nullptr && first->isEnabled(),
				    QStringLiteral("映す映像 %1").arg(frame != nullptr ? frame->currentText() : QString()));
		if (first != nullptr)
			first->setChecked(false);
		o.tab->flush();
		results.expect_true(kGroup, QStringLiteral("(f3) 外すと、self_slot_first が false で渡る (選んだ映像はそのまま)"),
				    !o.applied->empty() && !o.applied->back().self_slot_first &&
					    o.applied->back().self_frame_source_uuid == "uuid-board");
	}

	// (g) 太さの選択肢。
	{
		const QString default_family = QStringLiteral("UD デジタル 教科書体 NK-B");
		const QStringList installed = QFontDatabase::families();
		if (installed.contains(default_family)) {
			S s;
			s.label_font_family = default_family.toStdString();
			s.label_font_weight = 700;
			Opened o = open_tab(s);
			auto *weight = qobject_cast<QComboBox *>(o.field("label_font_weight"));
			results.info(kGroup, QStringLiteral("既定のフォントの太さの選択肢 (名前=太さ)"),
				     weight != nullptr ? items_of(weight).join(QLatin1Char(' ')) : QString());
			results.expect_true(kGroup, QStringLiteral("(g) 既定 (フォント UD デジタル 教科書体 NK-B、太さ 700) で、名前の付いた項目 (数でない項目) が 700 で選ばれる"),
					    weight != nullptr && weight->currentData().toInt() == 700 &&
						    weight->currentText() != QStringLiteral("700") && !weight->currentText().isEmpty(),
					    weight != nullptr ? weight->currentText() : QString());
		} else {
			results.info(kGroup, QStringLiteral("既定のフォントが入っていない"), default_family);
		}
		{
			S s;
			s.label_font_weight = 550;
			Opened o = open_tab(s);
			auto *weight = qobject_cast<QComboBox *>(o.field("label_font_weight"));
			results.expect_true(kGroup, QStringLiteral("(g) 保存されている太さが選択肢にないとき、その数の項目を選び、設定の値は 550 のまま"),
					    weight != nullptr && weight->currentText() == QStringLiteral("550") &&
						    weight->currentData().toInt() == 550 && o.tab->edited().label_font_weight == 550);
		}
		{
			QString other;
			for (const QString &family : {QStringLiteral("Segoe UI"), QStringLiteral("Meiryo UI"), QStringLiteral("Arial")}) {
				if (installed.contains(family))
					other = family;
			}
			QString next;
			for (const QString &f : installed) {
				if (f != other && QFontDatabase::styles(f).size() > 1) {
					next = f;
					break;
				}
			}
			if (!other.isEmpty() && !next.isEmpty()) {
				S s;
				s.label_font_family = other.toStdString();
				s.label_font_weight = 550;
				Opened o = open_tab(s);
				auto *family = qobject_cast<QFontComboBox *>(o.field("label_font_family"));
				auto *weight = qobject_cast<QComboBox *>(o.field("label_font_weight"));
				if (family != nullptr && weight != nullptr) {
					family->setCurrentFont(QFont(next));
					const int chosen = weight->currentData().toInt();
					bool named = false;
					for (int i = 0; i < weight->count(); ++i)
						named = named || (weight->itemData(i).toInt() == chosen &&
								  weight->itemText(i) != QString::number(chosen));
					results.expect_true(kGroup, QStringLiteral("(g) フォントを変えると、太さは新しいフォントの名前の付いた項目から選ばれ、設定の値と同じになる"),
							    named && o.tab->edited().label_font_weight == chosen &&
								    o.tab->edited().label_font_family == next.toStdString(),
							    QStringLiteral("%1 -> %2: %3").arg(other, next, items_of(weight).join(QLatin1Char(' '))));
				}
			} else {
				results.info(kGroup, QStringLiteral("フォントを変える確認に使えるフォントが入っていない"), QString());
			}
		}
	}
}

QPoint center_of(QWidget *w)
{
	return w->mapToGlobal(w->rect().center());
}

QScrollArea *display_scroll(SimPoll &sim)
{
	return sim.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxDisplayScroll"));
}

// ホイールを 3 ノッチ回す。ポインターは w の中央に置く。値が変わらないことと、ページがスクロールすることを返す。
struct WheelResult {
	bool scrolled = false;
	bool under_pointer = false;
};

WheelResult wheel_over(SimPoll &sim, Input &pointer, QWidget *w)
{
	WheelResult r;
	QScrollArea *scroll = display_scroll(sim);
	if (scroll == nullptr || w == nullptr)
		return r;
	scroll->verticalScrollBar()->setValue(0);
	scroll->ensureWidgetVisible(w, 0, 0);
	const int before = scroll->verticalScrollBar()->value();
	pointer.move_to(center_of(w));
	pump(60);
	r.under_pointer = Input::widget_at(center_of(w)) != nullptr;
	for (int i = 0; i < 3; ++i) {
		pointer.wheel(-120);
		pump(40);
	}
	pump(200);
	r.scrolled = scroll->verticalScrollBar()->value() > before;
	return r;
}

// ドックの表示タブ (後半)。
void run_look_dock(const ScenarioOptions &options, Results &results)
{
	const ui_preview::Fixture fixture = ui_preview::fixture_by_id("display-default");
	SimPoll sim(fixture, options.style);
	sim.dock()->setGeometry(1300, 120, 420, 560);
	sim.settings().layout_hold_idle_ms = 600;
	sim.widget()->set_tab(config::DockTab::Display);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.widget()->set_look_source_providers([] { return self_frame_choices(); }, [] { return self_source_choices(); });
	sim.start();
	pump(1200);
	dock::LookTab *look = sim.widget()->look_tab();
	QScrollArea *scroll = display_scroll(sim);
	results.expect_true(kGroup, QStringLiteral("前提: 表示タブの中身があり、低いドックではスクロールする"),
			    look != nullptr && scroll != nullptr && scroll->verticalScrollBar()->maximum() > 0 && sim.widget()->tab() == config::DockTab::Display);
	if (look == nullptr || scroll == nullptr)
		return;
	auto number = [&](const char *key) { return dynamic_cast<dock::ui::SliderSpin *>(look->field(QString::fromLatin1(key))); };

	std::vector<std::string> reflected_gap;
	int reflect_calls = 0;
	sim.on_look_reflect = [&](const S &s) {
		++reflect_calls;
		reflected_gap.push_back(std::to_string(s.tile_gap_px));
	};

	// ---- (1) 値を変えると、保存の前に、配信の画面へ渡す呼び出しが起きる ----
	{
		dock::ui::SliderSpin *gap = number("tile_gap_px");
		const int reflects0 = sim.look_reflects();
		const int saves0 = sim.look_saves();
		gap->spin()->setValue(8);
		results.expect_true(kGroup, QStringLiteral("(1) 値を変えると、すぐに配信の画面へ渡す呼び出しが起きる"),
				    sim.look_reflects() == reflects0 + 1 && reflect_calls == 1 && sim.settings().tile_gap_px == 8,
				    QStringLiteral("呼び出し %1 回、設定の値 %2").arg(sim.look_reflects() - reflects0).arg(sim.settings().tile_gap_px));
		results.expect_true(kGroup, QStringLiteral("(1) その時点では、保存していない (保存の前に反映する)"),
				    sim.look_saves() == saves0 && sim.look_save_pending() && sim.saved_settings().tile_gap_px != 8);
		pump(400);
		results.expect_true(kGroup, QStringLiteral("(1) 0.4 秒たっても、まだ保存していない"), sim.look_saves() == saves0);

		// ---- (2) 保存は最後の変更の 1 秒後に 1 回にまとまる ----
		pump(1100);
		results.expect_true(kGroup, QStringLiteral("(2) 変更の 1 秒後を過ぎると、1 回だけ保存する"),
				    sim.look_saves() == saves0 + 1 && !sim.look_save_pending() && sim.saved_settings().tile_gap_px == 8,
				    QStringLiteral("保存 %1 回").arg(sim.look_saves() - saves0));
		// 渡す呼び出しは、保存より前に起きている。
		const std::vector<SimPoll::LookEvent> &events = sim.look_events();
		bool order_ok = false;
		for (std::size_t i = 0; i + 1 < events.size(); ++i)
			order_ok = order_ok || (events[i].kind == 'r' && events[i + 1].kind == 's');
		results.expect_true(kGroup, QStringLiteral("(2) 渡す呼び出しは、保存より前に起きた"), order_ok);
	}
	{
		// 0.3 秒おきに 3 回変える -> 最後の変更の 1 秒後に 1 回だけ保存する。
		dock::ui::SliderSpin *gap = number("tile_gap_px");
		const int saves0 = sim.look_saves();
		const int reflects0 = sim.look_reflects();
		gap->spin()->setValue(10);
		pump(300);
		gap->spin()->setValue(12);
		pump(300);
		gap->spin()->setValue(14);
		pump(700);
		results.expect_true(kGroup, QStringLiteral("(2) 動かしている間 (最後の変更から 0.7 秒) は保存しない"), sim.look_saves() == saves0);
		pump(600);
		results.expect_true(kGroup, QStringLiteral("(2) 3 回変えても、最後の変更の 1 秒後に 1 回だけ保存し、最後の値を保存する"),
				    sim.look_saves() == saves0 + 1 && sim.saved_settings().tile_gap_px == 14,
				    QStringLiteral("保存 %1 回、値 %2").arg(sim.look_saves() - saves0).arg(sim.saved_settings().tile_gap_px));
		results.expect_true(kGroup, QStringLiteral("(2) 3 回の変更を、3 回とも配信の画面へ渡した (0.3 秒おきは間引かない)"),
				    sim.look_reflects() - reflects0 == 3);

		// 続けて 20 回変える (間にイベントを処理しない) -> 渡す回数は間引かれ、最後の値は必ず渡す。
		const int reflects1 = sim.look_reflects();
		const int saves1 = sim.look_saves();
		for (int v = 20; v < 40; ++v)
			gap->spin()->setValue(v);
		pump(400);
		const int passed = sim.look_reflects() - reflects1;
		results.expect_true(kGroup, QStringLiteral("(2) 続けて 20 回変えると、渡す回数は間引かれる (最初の 1 回と、間隔の後の最後の 1 回)"),
				    passed >= 2 && passed <= 3, QStringLiteral("渡した回数 %1").arg(passed));
		results.expect_true(kGroup, QStringLiteral("(2) 間引いても、最後の値は渡している"), sim.settings().tile_gap_px == 39);
		pump(1300);
		results.expect_true(kGroup, QStringLiteral("(2) 20 回変えても、保存は 1 回で、最後の値を保存する"),
				    sim.look_saves() == saves1 + 1 && sim.saved_settings().tile_gap_px == 39);
	}

	// ---- (3) ホイールで値が変わらず、ページがスクロールする ----
	{
		struct Target {
			QString name;
			QWidget *widget;
			std::function<QString()> value;
		};
		dock::ui::SliderSpin *gap = number("tile_gap_px");
		auto *assign = qobject_cast<QComboBox *>(look->field(QStringLiteral("assign_mode")));
		auto *order = qobject_cast<QComboBox *>(look->field(QStringLiteral("slot_order")));
		const std::vector<Target> targets{
			{QStringLiteral("つまみ"), gap->slider(), [&] { return QString::number(gap->slider()->value()); }},
			{QStringLiteral("数の入力欄"), gap->spin(), [&] { return QString::number(gap->spin()->value()); }},
			{QStringLiteral("コンボボックス (載せる人の選び方)"), assign, [&] { return QString::number(assign->currentIndex()); }},
			{QStringLiteral("コンボボックス (並び順)"), order, [&] { return QString::number(order->currentIndex()); }},
		};
		const int reflects0 = sim.look_reflects();
		for (const Target &t : targets) {
			if (t.widget == nullptr) {
				results.expect_true(kGroup, QStringLiteral("(3) ") + t.name + QStringLiteral(": 入力欄がある"), false);
				continue;
			}
			const QString before = t.value();
			const WheelResult r = wheel_over(sim, input, t.widget);
			results.expect_true(kGroup, QStringLiteral("(3) ") + t.name + QStringLiteral(": ホイールで値が変わらない"),
					    r.under_pointer && t.value() == before, QStringLiteral("%1 -> %2").arg(before, t.value()));
			results.expect_true(kGroup, QStringLiteral("(3) ") + t.name + QStringLiteral(": ホイールでページがスクロールする"), r.scrolled);
			// フォーカスがあっても同じ。
			t.widget->setFocus(Qt::TabFocusReason);
			const WheelResult focused = wheel_over(sim, input, t.widget);
			results.expect_true(kGroup, QStringLiteral("(3) ") + t.name + QStringLiteral(": フォーカスがあっても、値が変わらずページがスクロールする"),
					    focused.scrolled && t.value() == before, QStringLiteral("%1 -> %2").arg(before, t.value()));
			t.widget->clearFocus();
		}
		results.expect_true(kGroup, QStringLiteral("(3) ホイールでは、配信の画面へ値を渡さない"), sim.look_reflects() == reflects0);
		scroll->verticalScrollBar()->setValue(0);
		pump(100);
	}

	// ---- (7) 位置が動かない (1 行目、メインとサブの行、タブのバー、下のバー) ----
	{
		const Frame base = frame_of(sim.widget());
		results.expect_true(kGroup, QStringLiteral("前提: 4 つの固定の部分の矩形を読めた"),
				    !base.first_line.isEmpty() && !base.accounts.isEmpty() && !base.tab_bar.isEmpty() && !base.bottom.isEmpty(),
				    frame_text(base));
		QStringList moved;
		auto check = [&](const QString &what) {
			pump(600);
			const Frame now = frame_of(sim.widget());
			if (!(now == base))
				moved << what + QStringLiteral(" ") + frame_text(now);
		};
		dock::ui::SliderSpin *gap = number("tile_gap_px");
		gap->spin()->setValue(3);
		check(QStringLiteral("枠の間隔"));
		for (const char *key : {"self_slot_first", "slot_background_show", "label_show_name", "label_show_outline", "label_show_avatar", "label_show_plate",
					"label_show_speaking", "roster_show", "label_show_in_slot", "label_hide_when_single"}) {
			if (auto *c = qobject_cast<QCheckBox *>(look->field(QString::fromLatin1(key)))) {
				c->setChecked(!c->isChecked());
				check(QString::fromLatin1(key));
				c->setChecked(!c->isChecked());
				check(QString::fromLatin1(key) + QStringLiteral(" (戻す)"));
			}
		}
		if (auto *order = qobject_cast<QComboBox *>(look->field(QStringLiteral("slot_order")))) {
			order->setCurrentIndex(1);
			check(QStringLiteral("並び順"));
		}
		if (auto *anchor = qobject_cast<QComboBox *>(look->field(QStringLiteral("roster_anchor")))) {
			anchor->setCurrentIndex(1);
			check(QStringLiteral("配置"));
		}
		if (auto *size = number("label_em_ratio_pct"))
			size->spin()->setValue(6.5);
		check(QStringLiteral("フォントサイズ"));
		results.expect_true(kGroup, QStringLiteral("(7) 表示タブで値を変えても、1 行目、メインとサブの行、タブのバー、下のバーの位置が動かない"),
				    moved.isEmpty(), moved.join(QStringLiteral(" / ")));
		// 元に戻す。
		gap->spin()->setValue(0);
		if (auto *order = qobject_cast<QComboBox *>(look->field(QStringLiteral("slot_order"))))
			order->setCurrentIndex(0);
		if (auto *anchor = qobject_cast<QComboBox *>(look->field(QStringLiteral("roster_anchor"))))
			anchor->setCurrentIndex(7);
		if (auto *size = number("label_em_ratio_pct"))
			size->spin()->setValue(4.5);
		look->flush();
		pump(300);
	}

	// ---- (6) ダミー映像を切り替えると、1 行目が変わる ----
	{
		QCheckBox *dummy = look->dummy_check();
		scroll->ensureWidgetVisible(dummy);
		pump(100);
		const QString dummy_text = dock::ui::resolve_key(keys::kDockSummaryPlaceholdersShortStatus);
		const Frame base = frame_of(sim.widget());
		results.expect_true(kGroup, QStringLiteral("(6) 前提: ダミー映像のチェックは切れていて、1 行目は「ダミー映像を表示中」でない"),
				    dummy != nullptr && !dummy->isChecked() && !first_line_full_text(sim.widget()).contains(dummy_text),
				    first_line_full_text(sim.widget()));
		input.move_to(dummy->mapToGlobal(QPoint(dummy->height() / 2, dummy->height() / 2)));
		input.click();
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(6) チェックを入れると、ダミー映像の切り替えを実行し、1 行目が「ダミー映像を表示中」になる"),
				    sim.obs().choices.dummy == ledger::FeatureSwitch::On &&
					    first_line_full_text(sim.widget()).contains(dummy_text) && dummy->isChecked(),
				    first_line_full_text(sim.widget()));
		results.expect_true(kGroup, QStringLiteral("(6) 1 行目の高さと位置は変わらない"), frame_of(sim.widget()) == base, frame_text(frame_of(sim.widget())));
		input.move_to(dummy->mapToGlobal(QPoint(dummy->height() / 2, dummy->height() / 2)));
		input.click();
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(6) チェックを外すと、1 行目が元に戻る"),
				    sim.obs().choices.dummy == ledger::FeatureSwitch::Off &&
					    !first_line_full_text(sim.widget()).contains(dummy_text) && !dummy->isChecked(),
				    first_line_full_text(sim.widget()));
		results.expect_true(kGroup, QStringLiteral("(6) ダミーの切り替えは、設定の値を書き換えない (表示タブの保存の対象でない)"),
				    sim.look_save_pending() == false);
	}

	// ---- (13) 配信中と OBS の録画中でも、ダミー映像の表示は確認せずにすぐ始める ----
	{
		auto *confirm = dynamic_cast<dock::InlineConfirm *>(
			sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxInlineConfirm")));
		QCheckBox *dummy = look->dummy_check();
		results.expect_true(kGroup, QStringLiteral("(13) 前提: 確認のウィジェットとダミー映像のチェックがあり、ダミーは切れている"),
				    confirm != nullptr && dummy != nullptr && !dummy->isChecked() &&
					    sim.obs().choices.dummy == ledger::FeatureSwitch::Off);
		if (confirm != nullptr && dummy != nullptr) {
			const auto click_check = [&]() {
				scroll->ensureWidgetVisible(dummy);
				pump(100);
				input.move_to(dummy->mapToGlobal(QPoint(dummy->height() / 2, dummy->height() / 2)));
				input.click();
				pump(900);
			};
			sim.obs().uninstall.obs_streaming = true;
			sim.obs().obs_outputs.streaming = true;
			sim.changed();
			pump(700);
			click_check();
			results.expect_true(kGroup, QStringLiteral("(13) 配信中にダミー映像のチェックを押すと、確認は出ず、すぐ始まる"),
					    !confirm->active() && dummy->isChecked() && sim.obs().choices.dummy == ledger::FeatureSwitch::On);
			click_check();
			results.expect_true(kGroup, QStringLiteral("(13) 表示をやめる向きも、確認なしで戻る"),
					    !confirm->active() && !dummy->isChecked() && sim.obs().choices.dummy == ledger::FeatureSwitch::Off);
			sim.obs().uninstall.obs_streaming = false;
			sim.obs().obs_outputs.streaming = false;
			sim.obs().uninstall.obs_recording = true;
			sim.obs().obs_outputs.recording = true;
			sim.changed();
			pump(700);
			click_check();
			results.expect_true(kGroup, QStringLiteral("(13) OBS の録画中も、確認なしで始まる"),
					    !confirm->active() && dummy->isChecked() && sim.obs().choices.dummy == ledger::FeatureSwitch::On);
			click_check();
			sim.obs().uninstall.obs_recording = false;
			sim.obs().obs_outputs.recording = false;
			sim.changed();
			pump(700);
		}
	}

	// ---- (12) クリックではフォーカスを取らない。クリックしたあとの Space と矢印のキーは値を変えない ----
	{
		results.expect_eq(kGroup, QStringLiteral("(12) 表示タブのチェック、ボタン、コンボボックスは、クリックでフォーカスを取らない"),
				  click_focus_controls(look).size(), 0, click_focus_controls(look).join(QLatin1Char(' ')));
		results.expect_eq(kGroup, QStringLiteral("(12) 状態タブのチェック、ボタン、コンボボックスも、クリックでフォーカスを取らない"),
				  click_focus_controls(sim.widget()->status_tab()).size(), 0,
				  click_focus_controls(sim.widget()->status_tab()).join(QLatin1Char(' ')));

		auto *show_name = qobject_cast<QCheckBox *>(look->field(QStringLiteral("label_show_name")));
		scroll->ensureWidgetVisible(show_name);
		pump(100);
		const bool before = show_name->isChecked();
		input.move_to(show_name->mapToGlobal(QPoint(show_name->height() / 2, show_name->height() / 2)));
		input.click();
		pump(200);
		const bool clicked = show_name->isChecked();
		results.expect_true(kGroup, QStringLiteral("(12) チェックをクリックすると値が変わる"), clicked != before);
		results.expect_true(kGroup, QStringLiteral("(12) クリックしてもチェックはフォーカスを取らない"),
				    QApplication::focusWidget() != show_name);
		input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
		pump(200);
		results.expect_true(kGroup, QStringLiteral("(12) 続けて Space を押しても、チェックの値は変わらない"), show_name->isChecked() == clicked);
		// 元に戻す。
		input.click();
		pump(200);

		auto *order = qobject_cast<QComboBox *>(look->field(QStringLiteral("slot_order")));
		scroll->ensureWidgetVisible(order);
		pump(100);
		const int index0 = order->currentIndex();
		input.move_to(center_of(order));
		input.click();
		pump(200);
		input.key(Qt::Key_Escape);
		pump(200);
		input.key(Qt::Key_Down);
		pump(200);
		results.expect_true(kGroup, QStringLiteral("(12) コンボボックスを開いて閉じたあと、矢印のキーを押しても値は変わらず、フォーカスも取らない"),
				    order->currentIndex() == index0 && QApplication::focusWidget() != order,
				    QStringLiteral("選択 %1 -> %2").arg(index0).arg(order->currentIndex()));
		results.expect_true(kGroup, QStringLiteral("(12) 前提: Tab のキーでは、コンボボックスにフォーカスが移れる"),
				    (order->focusPolicy() & Qt::TabFocus) != 0);
	}

	// ---- (8) 外から届いた値は、操作していない入力欄に反映し、操作している入力欄は書き換えない ----
	{
		dock::ui::SliderSpin *dim = number("label_dim_pct");
		sim.settings().label_dim_pct = 40;
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(8) 外から届いた値は、操作していない入力欄に反映する"),
				    std::lround(dim->spin()->value()) == 40 && look->edited().label_dim_pct == 40,
				    QString::number(dim->spin()->value()));
		dim->spin()->setFocus(Qt::TabFocusReason);
		pump(100);
		sim.settings().label_dim_pct = 70;
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(8) 操作している (フォーカスのある) 入力欄は、外から届いた値で書き換えない"),
				    std::lround(dim->spin()->value()) == 40, QString::number(dim->spin()->value()));
		dim->spin()->clearFocus();
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(8) 操作が終わると、次の周期で外の値に追従する"),
				    std::lround(dim->spin()->value()) == 70, QString::number(dim->spin()->value()));
		// つまみを押している間も書き換えない。
		sim.settings().label_dim_pct = 20;
		dim->slider()->setSliderDown(true);
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(8) つまみを押している間は、外から届いた値で書き換えない"),
				    std::lround(dim->spin()->value()) == 70, QString::number(dim->spin()->value()));
		dim->slider()->setSliderDown(false);
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(8) つまみを離すと、次の周期で外の値に追従する"),
				    std::lround(dim->spin()->value()) == 20, QString::number(dim->spin()->value()));
		// 変えた値をまだ渡していない間も書き換えない。
		dim->spin()->setValue(35);
		dim->spin()->setValue(45);
		sim.settings().label_dim_pct = 5;
		sim.tick();
		results.expect_true(kGroup, QStringLiteral("(8) 変えて渡していない値がある間は、外から届いた値で書き換えない"),
				    look->has_unreflected() && std::lround(dim->spin()->value()) == 45, QString::number(dim->spin()->value()));
		look->flush();
		pump(600);
		sim.settings().label_dim_pct = 0;
		pump(900);
	}

	// ---- (4) 設定ウィンドウの [適用] で、表示タブの値が古い値に戻らない ----
	{
		dock::ui::SliderSpin *gap = number("tile_gap_px");
		gap->spin()->setValue(0);
		look->flush();
		pump(200);
		// 設定ウィンドウを開く (この時点の設定をウィンドウの値の元にする)。
		dock::SettingsWindowInput in;
		in.current = sim.settings();
		in.effective_at_start = sim.settings();
		in.page = ledger::SettingsPage::Recording;
		in.about = sim.view().about;
		auto applied = std::make_shared<int>(0);
		in.on_apply = [&sim, applied](const S &s) {
			++*applied;
			// dock/dock_poll_settings.cpp の on_settings_changed と同じ関数。
			dock::apply_settings_page_fields(s, sim.settings());
			return true;
		};
		std::unique_ptr<dock::SettingsWindow> window(dock::make_settings_window(main_window(), in));
		// ウィンドウを開いた後に、ドックの表示タブで値を変える。
		gap->spin()->setValue(30);
		if (auto *name = qobject_cast<QCheckBox *>(look->field(QStringLiteral("label_show_name"))))
			name->setChecked(false);
		if (auto *order = qobject_cast<QComboBox *>(look->field(QStringLiteral("slot_order"))))
			order->setCurrentIndex(1);
		look->flush();
		pump(200);
		// ウィンドウの入力欄を 1 つ変えて [適用] を押す。
		auto *warn = window->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		auto *box = window->findChild<QDialogButtonBox *>();
		QPushButton *apply = box != nullptr ? box->button(QDialogButtonBox::Apply) : nullptr;
		const int warn_before = sim.settings().record_space_warn_minutes;
		if (warn != nullptr)
			warn->setValue(warn->value() + 1);
		if (apply != nullptr && apply->isEnabled())
			apply->click();
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(4) 設定ウィンドウの [適用] を押せ、ウィンドウの値が設定に入る"),
				    *applied == 1 && sim.settings().record_space_warn_minutes == warn_before + 1);
		results.expect_true(kGroup, QStringLiteral("(4) [適用] の後も、表示タブで変えた値 (枠の間隔、名前の表示、並び順) が古い値に戻らない"),
				    sim.settings().tile_gap_px == 30 && !sim.settings().label_show_name &&
					    sim.settings().slot_order == config::SlotOrder::Added,
				    QStringLiteral("間隔 %1、名前 %2、並び順 %3")
					    .arg(sim.settings().tile_gap_px)
					    .arg(sim.settings().label_show_name)
					    .arg(static_cast<int>(sim.settings().slot_order)));
		results.expect_true(kGroup, QStringLiteral("(4) 表示タブの入力欄も、その値のまま"),
				    std::lround(gap->spin()->value()) == 30 && look->edited().slot_order == config::SlotOrder::Added);
		window.reset();
		// 戻す。
		gap->spin()->setValue(0);
		if (auto *name = qobject_cast<QCheckBox *>(look->field(QStringLiteral("label_show_name"))))
			name->setChecked(true);
		if (auto *order = qobject_cast<QComboBox *>(look->field(QStringLiteral("slot_order"))))
			order->setCurrentIndex(0);
		look->flush();
		pump(300);
	}

	// ---- (5) [変更] で変えたメインアカウントが、[適用] で戻らない ----
	{
		const std::string before_id = sim.settings().broadcaster_user_id;
		std::string other;
		for (const ledger::PersonFact &p : sim.obs().people) {
			if (!p.user_id.empty() && p.user_id != before_id && !p.is_receiver) {
				other = p.user_id;
				break;
			}
		}
		results.expect_true(kGroup, QStringLiteral("前提: メインアカウントを替える相手が通話に参加している"), !other.empty());
		// 先に、メインアカウントを使わない構成にする ([変更] の「メインアカウントを使わない」と同じ操作)。
		ledger::ActionArgs none;
		sim.run_action(ledger::ActionId::SetMainAccount, none);
		pump(900);
		auto frame_combo = [&]() { return qobject_cast<QComboBox *>(look->field(QStringLiteral("self_frame_source_uuid"))); };
		results.expect_true(kGroup, QStringLiteral("(5) メインアカウントを使わない構成では、自分の枠に映す映像の先頭の選択肢が (なし)"),
				    sim.settings().broadcaster_user_id.empty() && frame_combo() != nullptr &&
					    frame_combo()->itemText(0) == dock::ui::resolve_key(keys::kLookNoneOption));
		dock::SettingsWindowInput in;
		in.current = sim.settings();
		in.effective_at_start = sim.settings();
		in.page = ledger::SettingsPage::Recording;
		in.about = sim.view().about;
		in.on_apply = [&sim](const S &s) {
			dock::apply_settings_page_fields(s, sim.settings());
			return true;
		};
		std::unique_ptr<dock::SettingsWindow> window(dock::make_settings_window(main_window(), in));
		// ウィンドウを開いた後に、[変更] と同じ操作 (ActionId::SetMainAccount) でメインアカウントを替える。
		ledger::ActionArgs args;
		args.user_id = other;
		sim.run_action(ledger::ActionId::SetMainAccount, args);
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(5) [変更] の操作で、メインアカウントが替わる"),
				    sim.settings().broadcaster_user_id == other, QString::fromStdString(sim.settings().broadcaster_user_id));
		auto *warn = window->findChild<QSpinBox *>(QStringLiteral("cordmuxField_record_space_warn_minutes"));
		auto *box = window->findChild<QDialogButtonBox *>();
		QPushButton *apply = box != nullptr ? box->button(QDialogButtonBox::Apply) : nullptr;
		if (warn != nullptr)
			warn->setValue(warn->value() + 1);
		if (apply != nullptr && apply->isEnabled())
			apply->click();
		pump(900);
		results.expect_true(kGroup, QStringLiteral("(5) 設定ウィンドウの [適用] の後も、メインアカウントが古い値に戻らない"),
				    sim.settings().broadcaster_user_id == other, QString::fromStdString(sim.settings().broadcaster_user_id));
		results.expect_true(kGroup, QStringLiteral("(5) 設定ウィンドウには、メインアカウントのユーザー ID の入力欄がない"),
				    window->findChild<QWidget *>(QStringLiteral("cordmuxField_broadcaster_user_id")) == nullptr);
		results.expect_true(kGroup, QStringLiteral("(5) メインアカウントができると、自分の枠に映す映像の先頭の選択肢が「Discordの画面共有」になる"),
				    frame_combo() != nullptr &&
					    frame_combo()->itemText(0) == dock::ui::resolve_key(keys::kLookSelfFrameDiscordOption));
		window.reset();
		ledger::ActionArgs back;
		back.user_id = before_id;
		sim.run_action(ledger::ActionId::SetMainAccount, back);
		pump(600);
	}

	// ---- (9) OBS を閉じるとき: 渡していない値と待っている保存を、待たずに行う ----
	{
		dock::ui::SliderSpin *gap = number("tile_gap_px");
		pump(1300);
		const int reflects0 = sim.look_reflects();
		const int saves0 = sim.look_saves();
		gap->spin()->setValue(6);
		gap->spin()->setValue(7); // 間隔の中なので、渡さずに待たせる
		results.expect_true(kGroup, QStringLiteral("(9) 前提: 渡していない値と、待っている保存がある"),
				    look->has_unreflected() && sim.look_save_pending() && sim.settings().tile_gap_px == 6);
		sim.flush_look();
		results.expect_true(kGroup, QStringLiteral("(9) flush で、最後の値を渡し、保存を待たずに行う"),
				    sim.settings().tile_gap_px == 7 && sim.look_saves() == saves0 + 1 && sim.saved_settings().tile_gap_px == 7 &&
					    !sim.look_save_pending() && sim.look_reflects() > reflects0,
				    QStringLiteral("値 %1、保存 %2 回").arg(sim.settings().tile_gap_px).arg(sim.look_saves() - saves0));
		gap->spin()->setValue(0);
		sim.flush_look();
	}

	// ---- (11) 最小の幅でも、表示タブの中身が横にはみ出さず、ドックの最小の幅を広げない ----
	{
		const int min_w = sim.widget()->computed_min_width();
		sim.dock()->resize(min_w, 700);
		pump(500);
		QWidget *content = scroll->widget();
		const int viewport_w = scroll->viewport()->width();
		results.expect_true(kGroup, QStringLiteral("(11) ドックを最小の幅にしても、ドックの最小の幅が変わらない"),
				    sim.widget()->computed_min_width() == min_w && sim.dock()->width() >= min_w,
				    QStringLiteral("最小の幅 %1 px、ドックの幅 %2 px").arg(min_w).arg(sim.dock()->width()));
		results.expect_true(kGroup, QStringLiteral("(11) 最小の幅で、表示タブの中身の最小の幅が、見える範囲の幅に入る (横にスクロールしない)"),
				    content->minimumSizeHint().width() <= viewport_w && scroll->horizontalScrollBar()->maximum() == 0 &&
					    content->width() <= viewport_w,
				    QStringLiteral("中身の最小の幅 %1 px、見える範囲 %2 px").arg(content->minimumSizeHint().width()).arg(viewport_w));
		sim.dock()->resize(420, 560);
		pump(300);
	}

	// ---- (10) 色のダイアログは、ドックの操作だけを止める ----
	{
		Script s;
		int reflects_before = 0;
		auto pick_of = [&]() { return qobject_cast<QPushButton *>(look->field(QStringLiteral("label_text_color"))); };
		auto dialog_of = [&]() -> QColorDialog * {
			for (QWidget *w : QApplication::topLevelWidgets()) {
				if (w->isVisible())
					if (auto *d = qobject_cast<QColorDialog *>(w))
						return d;
			}
			return nullptr;
		};
		s.then(200, [&] {
			scroll->ensureWidgetVisible(pick_of());
			pump(100);
			// クリックではフォーカスを取らないので、キーボードで操作する人と同じく、先に Tab でフォーカスを置く。
			pick_of()->setFocus(Qt::TabFocusReason);
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: 前提: [色を選択] が押せる"), pick_of() != nullptr && pick_of()->isEnabled());
		});
		s.then_blocking(200, [&] {
			input.move_to(center_of(pick_of()));
			input.click();
		});
		s.then(500, [&] {
			QColorDialog *d = dialog_of();
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: 表示される"), d != nullptr);
			if (d == nullptr)
				return;
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: モーダルでなく、止めるのはドックだけ (OwnerOnly)"),
					    d->windowModality() == Qt::NonModal && QApplication::activeModalWidget() == nullptr &&
						    d->property("messageBlocking").toString() == QLatin1String("ownerOnly"),
					    d->property("messageBlocking").toString());
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: タイトルは色の入力欄の名前"),
					    !d->windowTitle().isEmpty());
			// 色を選んでいる間も、入力欄の色が変わる。
			reflects_before = sim.look_reflects();
			d->setCurrentColor(QColor(QStringLiteral("#123456")));
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: 色を選んでいる間に、入力欄の色が変わる"),
					    look->edited().label_text_color == "#123456" && d->isVisible(), QString::fromStdString(look->edited().label_text_color));
		});
		s.then(900, [&] {
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: 色を選んでいる間に、配信の画面へ渡す呼び出しが起きる"),
					    sim.look_reflects() > reflects_before && sim.settings().label_text_color == "#123456");
			if (QColorDialog *d = dialog_of())
				d->reject();
		});
		s.then(300, [&] {
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: 閉じた後、フォーカスが [色を選択] へ戻る"),
					    QApplication::focusWidget() == pick_of(),
					    QApplication::focusWidget() != nullptr ? QApplication::focusWidget()->objectName() : QString());
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: やめたので、色は開く前に戻る"),
					    look->edited().label_text_color == S{}.label_text_color);
		});
		s.then(900, [&] {
			results.expect_true(kGroup, QStringLiteral("色のダイアログ: やめたことも、配信の画面に渡す (選んでいる間の色が残らない)"),
					    sim.settings().label_text_color == S{}.label_text_color);
		});
		const bool finished = s.run(20000);
		results.expect_true(kGroup, QStringLiteral("色のダイアログの手順が時間内に終わった"), finished);
	}

	// ---- (15) 数を打っている途中の値は配信に渡さない。打つ手が止まって 0.5 秒か、Enter で確定する ----
	{
		dock::ui::SliderSpin *gap = number("tile_gap_px");
		scroll->ensureWidgetVisible(gap);
		pump(100);
		gap->spin()->setValue(0);
		pump(1300);
		const auto type_over = [&](const QString &digits) {
			gap->spin()->selectAll();
			for (const QChar c : digits) {
				input.key(Qt::Key_0 + (c.unicode() - '0'), Qt::NoModifier, QString(c));
				pump(60);
			}
		};
		input.move_to(center_of(gap->spin()));
		input.click();
		pump(150);
		results.expect_true(kGroup, QStringLiteral("(15) 前提: 数の入力欄はクリックでフォーカスを取る"), gap->spin()->hasFocus());
		const int reflects0 = sim.look_reflects();
		type_over(QStringLiteral("12"));
		results.expect_true(kGroup, QStringLiteral("(15) 打っている途中は、値も配信への受け渡しも変わらない"),
				    sim.look_reflects() == reflects0 && sim.settings().tile_gap_px == 0 && gap->spin()->hasFocus(),
				    QStringLiteral("受け渡し %1 回、設定の値 %2").arg(sim.look_reflects() - reflects0).arg(sim.settings().tile_gap_px));
		pump(dock::ui::SliderSpin::kTypingSettleMs + 300);
		results.expect_true(kGroup, QStringLiteral("(15) 打つ手が止まって 0.5 秒たつと、打った値を確定して渡す"),
				    sim.settings().tile_gap_px == 12 && sim.look_reflects() > reflects0,
				    QStringLiteral("設定の値 %1").arg(sim.settings().tile_gap_px));
		type_over(QStringLiteral("5"));
		input.key(Qt::Key_Return);
		pump(200);
		results.expect_true(kGroup, QStringLiteral("(15) Enter で確定し、フォーカスを外す"),
				    sim.settings().tile_gap_px == 5 && !gap->spin()->hasFocus(), QStringLiteral("設定の値 %1").arg(sim.settings().tile_gap_px));
		gap->spin()->setValue(0);
		pump(1300);
	}

	// ---- (16) つまみはクリックでフォーカスを取り、キーで動き、ダブルクリックでその設定の既定値に戻る ----
	{
		dock::ui::SliderSpin *dim = number("label_dim_pct");
		dock::ui::SliderSpin *em = number("label_em_ratio_pct");
		QSlider *slider = dim != nullptr ? dim->slider() : nullptr;
		results.expect_true(kGroup, QStringLiteral("(16) 前提: 暗さと名前の大きさのつまみがあり、クリックと Tab でフォーカスを取る"),
				    slider != nullptr && em != nullptr && em->slider() != nullptr &&
					    slider->focusPolicy() == Qt::StrongFocus && em->slider()->focusPolicy() == Qt::StrongFocus);
		if (slider != nullptr && em != nullptr && em->slider() != nullptr) {
			const S defaults{};
			dim->spin()->setValue(60);
			em->spin()->setValue(6.0);
			pump(1300);
			scroll->ensureWidgetVisible(slider);
			pump(150);
			if (QWidget *focused = QApplication::focusWidget())
				focused->clearFocus();
			// ダブルクリック (1 回目のクリックでつまみが動いても、既定値に戻る)。
			input.move_to(center_of(slider));
			input.click();
			input.click();
			pump(200);
			results.expect_true(kGroup, QStringLiteral("(16) 暗さのつまみをダブルクリックすると、既定値 (30%) に戻り、フォーカスが来る"),
					    std::lround(dim->spin()->value()) == defaults.label_dim_pct && slider->hasFocus(),
					    QString::number(dim->spin()->value()));
			pump(1300);
			results.expect_true(kGroup, QStringLiteral("(16) 戻した値は、配信の画面へ渡して保存する"),
					    sim.settings().label_dim_pct == defaults.label_dim_pct &&
						    sim.saved_settings().label_dim_pct == defaults.label_dim_pct);
			// キー: 右で 1 段 (5%)、PageUp で大きい刻み (段数 18 の 4 分の 1 の 4 段 = 20%)、上で 1 段増え、下で 1 段減る、
			// Home と End で両端。
			input.key(Qt::Key_Right);
			const long after_right = std::lround(dim->spin()->value());
			input.key(Qt::Key_PageUp);
			const long after_page = std::lround(dim->spin()->value());
			input.key(Qt::Key_Up);
			const long after_up = std::lround(dim->spin()->value());
			input.key(Qt::Key_Down);
			const long after_up_down = std::lround(dim->spin()->value());
			input.key(Qt::Key_End);
			const long after_end = std::lround(dim->spin()->value());
			input.key(Qt::Key_Home);
			const long after_home = std::lround(dim->spin()->value());
			results.expect_true(kGroup, QStringLiteral("(16) 右の矢印で 5%、PageUp で 20%、上の矢印で 5% 増え、下の矢印で 5% 減り、End で 90%、Home で 0% に動く"),
					    after_right == 35 && after_page == 55 && after_up == 60 && after_up_down == 55 && after_end == 90 &&
						    after_home == 0,
					    QStringLiteral("%1 %2 %3 %4 %5 %6").arg(after_right).arg(after_page).arg(after_up).arg(after_up_down).arg(after_end).arg(after_home));
			pump(1300);
			results.expect_true(kGroup, QStringLiteral("(16) キーで動かした最後の値 (0%) を保存する"),
					    sim.saved_settings().label_dim_pct == 0, QString::number(sim.saved_settings().label_dim_pct));
			// 小数の設定 (名前の大きさ) も、その設定の既定値に戻る。
			scroll->ensureWidgetVisible(em->slider());
			pump(150);
			input.move_to(center_of(em->slider()));
			input.click();
			input.click();
			pump(1300);
			results.expect_true(kGroup, QStringLiteral("(16) 名前の大きさのつまみをダブルクリックすると、既定値 (2.5%) に戻る"),
					    std::fabs(em->spin()->value() - defaults.label_em_ratio_pct) < 1e-9 &&
						    sim.saved_settings().label_em_ratio_pct == defaults.label_em_ratio_pct,
					    QString::number(em->spin()->value()));
			dim->spin()->setValue(defaults.label_dim_pct);
			if (QWidget *focused = QApplication::focusWidget())
				focused->clearFocus();
			pump(1300);
		}
	}

	sim.stop();
	sim.dock()->hide();
}

} // namespace

// 行の枠の位置と、配信の配置の 4 等分の位置の番号 (0 左上、1 右上、2 左下、3 右下) を比べるための変換。1 つを全面に置くときは
// 全面。
ledger::dockview::SlotPosition stream_position_of(const sources::CompositeFocusTile &tile, std::size_t tile_count)
{
	namespace dv = ledger::dockview;
	if (tile_count == 1)
		return dv::SlotPosition::Full;
	const dv::SlotPosition by_cell[] = {dv::SlotPosition::TopLeft, dv::SlotPosition::TopRight, dv::SlotPosition::BottomLeft,
					    dv::SlotPosition::BottomRight};
	return tile.cell >= 0 && tile.cell < 4 ? by_cell[tile.cell] : dv::SlotPosition::None;
}

// 参加者タブの行が持つ枠の位置 (行がなければ None)。
ledger::dockview::SlotPosition row_position_of(const ledger::dockview::PeopleListView &list, const std::string &user_id)
{
	if (list.pinned.has_value() && list.pinned->user_id == user_id)
		return list.pinned->position;
	for (const auto *rows : {&list.sharing, &list.in_call}) {
		for (const ledger::dockview::PersonRow &row : *rows) {
			if (row.user_id == user_id)
				return row.position;
		}
	}
	return ledger::dockview::SlotPosition::None;
}

// (17) [先頭に配置] を外すと、自分の枠の行の位置と配信の配置の位置が、同じだけ変わる。行の位置は ledger の slot_positions、
// 配信の配置は sources の composite_focus_tiles (製品と同じ人と名前を Observation から入れる。ui_preview::focus_tiles_for) で
// 読む。どちらも同じ関数 (ledger/slot_cells.h の plan_slot_cells) で決めるので、同じ位置になる。
void run_look_self_first(const ScenarioOptions &options, Results &results)
{
	namespace dv = ledger::dockview;
	// 自分の枠を出している場面 (メインアカウントの Taro の行が自分の枠)。
	const ui_preview::Fixture fixture = ui_preview::fixture_by_id("people-self-frame-on");
	SimPoll sim(fixture, options.style);
	sim.dock()->setGeometry(1300, 120, 420, 560);
	sim.widget()->set_tab(config::DockTab::Display);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.start();
	pump(1200);
	dock::LookTab *look = sim.widget()->look_tab();
	auto *first = look != nullptr ? qobject_cast<QCheckBox *>(look->field(QStringLiteral("self_slot_first"))) : nullptr;
	std::string self_id;
	for (const ledger::PersonFact &p : sim.obs().people) {
		if (p.is_self_frame)
			self_id = p.user_id;
	}
	results.expect_true(kGroup, QStringLiteral("(17) 前提: 自分の枠を出していて、[先頭に配置] が付いていて押せる"),
			    first != nullptr && first->isChecked() && first->isEnabled() && sim.obs().choices.self_frame_shown &&
				    !self_id.empty());
	if (first == nullptr || self_id.empty())
		return;

	// 自分の枠の行の位置と配信の配置の位置。
	const auto self_positions = [&]() {
		const std::vector<sources::CompositeFocusTile> tiles = ui_preview::focus_tiles_for(sim.obs());
		dv::SlotPosition stream = dv::SlotPosition::None;
		for (const sources::CompositeFocusTile &tile : tiles) {
			if (tile.user_id == self_id)
				stream = stream_position_of(tile, tiles.size());
		}
		return std::make_pair(row_position_of(sim.view().people_list, self_id), stream);
	};
	const auto text_of = [](dv::SlotPosition p) { return QString::number(static_cast<int>(p)); };

	const auto on = self_positions();
	results.expect_true(kGroup, QStringLiteral("(17) 付いている間は、自分の枠の行も配信の配置も左上"),
			    on.first == dv::SlotPosition::TopLeft && on.second == dv::SlotPosition::TopLeft,
			    QStringLiteral("行 %1、配信 %2").arg(text_of(on.first), text_of(on.second)));

	first->setChecked(false);
	pump(600);
	const auto off = self_positions();
	results.expect_true(kGroup, QStringLiteral("(17) 外すと設定が false になり、自分の枠は名前順で左上以外 (名前の順で後ろ) に移る"),
			    !sim.settings().self_slot_first && off.first != dv::SlotPosition::TopLeft && off.first != dv::SlotPosition::None,
			    QStringLiteral("行 %1").arg(text_of(off.first)));
	results.expect_true(kGroup, QStringLiteral("(17) 外した後も、自分の枠の行の位置と配信の配置の位置は同じ"), off.first == off.second,
			    QStringLiteral("行 %1、配信 %2").arg(text_of(off.first), text_of(off.second)));
	// 配信に映る全員の行の位置と配信の位置が、すべて同じ。
	{
		QStringList differs;
		const std::vector<sources::CompositeFocusTile> tiles = ui_preview::focus_tiles_for(sim.obs());
		for (const sources::CompositeFocusTile &tile : tiles) {
			const dv::SlotPosition row = row_position_of(sim.view().people_list, tile.user_id);
			const dv::SlotPosition stream = stream_position_of(tile, tiles.size());
			if (row != stream)
				differs << QString::fromStdString(tile.user_id).right(3) +
						   QStringLiteral(" 行 %1 配信 %2").arg(text_of(row), text_of(stream));
		}
		results.expect_true(kGroup, QStringLiteral("(17) 外した後、配信に映る全員の行の位置が配信の配置と同じ"),
				    tiles.size() >= 2 && differs.isEmpty(), differs.join(QStringLiteral(" / ")));
	}

	first->setChecked(true);
	pump(600);
	const auto back = self_positions();
	results.expect_true(kGroup, QStringLiteral("(17) 付け直すと、行も配信の配置も左上に戻る"),
			    sim.settings().self_slot_first && back.first == dv::SlotPosition::TopLeft &&
				    back.second == dv::SlotPosition::TopLeft);
}

void run_look(const ScenarioOptions &options, Results &results)
{
	run_look_fields(results);
	run_look_dock(options, results);
	run_look_self_first(options, results);
}

} // namespace cordmux::dock_drive

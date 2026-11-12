// SPDX-License-Identifier: Apache-2.0

// tuner_knobs.h の実装。

#include "tuner_knobs.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace cordmux::label_tuner {

namespace {

KnobDef number(const char *key, const char *group, const char *title, double min, double max, int divisor,
	       const char *unit, double config::Settings::*field)
{
	KnobDef k;
	k.key = key;
	k.group = group;
	k.title = title;
	k.kind = KnobKind::Number;
	k.min = min;
	k.max = max;
	k.divisor = divisor;
	k.unit = unit;
	k.get_number = [field](const config::Settings &s) {
		return s.*field;
	};
	k.set_number = [field](config::Settings &s, double v) {
		s.*field = v;
	};
	return k;
}

KnobDef whole(const char *key, const char *group, const char *title, int min, int max, const char *unit,
	      int config::Settings::*field)
{
	KnobDef k;
	k.key = key;
	k.group = group;
	k.title = title;
	k.kind = KnobKind::Number;
	k.min = min;
	k.max = max;
	k.divisor = 1;
	k.unit = unit;
	k.get_number = [field](const config::Settings &s) {
		return static_cast<double>(s.*field);
	};
	k.set_number = [field](config::Settings &s, double v) {
		s.*field = static_cast<int>(std::lround(v));
	};
	return k;
}

KnobDef toggle(const char *key, const char *group, const char *title, bool config::Settings::*field)
{
	KnobDef k;
	k.key = key;
	k.group = group;
	k.title = title;
	k.kind = KnobKind::Toggle;
	k.get_toggle = [field](const config::Settings &s) {
		return s.*field;
	};
	k.set_toggle = [field](config::Settings &s, bool v) {
		s.*field = v;
	};
	return k;
}

KnobDef text(KnobKind kind, const char *key, const char *group, const char *title, std::string config::Settings::*field)
{
	KnobDef k;
	k.key = key;
	k.group = group;
	k.title = title;
	k.kind = kind;
	k.get_text = [field](const config::Settings &s) {
		return s.*field;
	};
	k.set_text = [field](config::Settings &s, const std::string &v) {
		s.*field = v;
	};
	return k;
}

// 刻みが 1 / divisor ではない整数のつまみ (5 刻みの暗さ)。step は刻みの段数 (divisor が 1 なら、値の刻み)。
KnobDef whole_step(const char *key, const char *group, const char *title, int min, int max, int step,
		   const char *unit, int config::Settings::*field)
{
	KnobDef k = whole(key, group, title, min, max, unit, field);
	k.step = step;
	return k;
}

// 決まった選択肢から 1 つ選ぶつまみ。get と set は options の添字を読み書きする。
KnobDef choice(const char *key, const char *group, const char *title, std::vector<const char *> options,
	       std::function<int(const config::Settings &)> get, std::function<void(config::Settings &, int)> set)
{
	KnobDef k;
	k.key = key;
	k.group = group;
	k.title = title;
	k.kind = KnobKind::Choice;
	k.options = std::move(options);
	k.get_choice = std::move(get);
	k.set_choice = std::move(set);
	return k;
}

// つまみにヒントを付ける。
KnobDef with_hint(KnobDef k, const char *hint)
{
	k.hint = hint;
	return k;
}

// チェックボックスを、名前 row の 1 行にまとめて並べる 1 つにする。
KnobDef in_row(KnobDef k, const char *row)
{
	k.row = row;
	return k;
}

constexpr const char *kRatioOfFontSize = "フォントサイズに対する倍率です。";

std::vector<KnobDef> make_knob_defs()
{
	using S = config::Settings;
	std::vector<KnobDef> knobs;

	const char *name_group = "名前";
	knobs.push_back(toggle("label_show_name", name_group, "表示", &S::label_show_name));
	knobs.push_back(text(KnobKind::Font, "label_font_family", name_group, "フォント", &S::label_font_family));
	knobs.push_back(with_hint(whole("label_font_weight", name_group, "太さ", config::kLabelFontWeightMin,
					config::kLabelFontWeightMax, "", &S::label_font_weight),
				  "400 は標準、700 は太字です。"));
	// フォントサイズの刻み (0.1) は設定が決めている (config/settings.h の clamp_settings と read_bounded_tenth_step)。
	{
		KnobDef size = number("label_em_ratio_pct", name_group, "フォントサイズ", config::kLabelEmRatioMinPct,
				      config::kLabelEmRatioMaxPct, 10, "%", &S::label_em_ratio_pct);
		size.get_hint = [](const S &s) {
			const long px = std::lround(kPreviewCanvasHeight * s.label_em_ratio_pct / 100.0);
			return "キャンバスの高さに対する割合です。" + std::to_string(kPreviewCanvasWidth) + "x" +
			       std::to_string(kPreviewCanvasHeight) + " では " + std::to_string(px) + " px です。";
		};
		knobs.push_back(size);
	}
	knobs.push_back(text(KnobKind::Color, "label_text_color", name_group, "色", &S::label_text_color));

	const char *outline_group = "輪郭";
	knobs.push_back(toggle("label_show_outline", outline_group, "表示", &S::label_show_outline));
	knobs.push_back(text(KnobKind::Color, "label_outline_color", outline_group, "色", &S::label_outline_color));
	knobs.push_back(with_hint(number("label_outline_ratio", outline_group, "サイズ", config::kLabelOutlineRatioMin,
					 config::kLabelOutlineRatioMax, 100, "倍", &S::label_outline_ratio),
				  kRatioOfFontSize));

	const char *avatar_group = "アバター";
	knobs.push_back(toggle("label_show_avatar", avatar_group, "表示", &S::label_show_avatar));
	knobs.push_back(with_hint(number("label_avatar_ratio", avatar_group, "サイズ", config::kLabelAvatarRatioMin,
					 config::kLabelAvatarRatioMax, 100, "倍", &S::label_avatar_ratio),
				  kRatioOfFontSize));
	knobs.push_back(choice(
		"label_avatar_shape", avatar_group, "形", {"丸", "角丸", "四角"},
		[](const S &s) { return static_cast<int>(s.label_avatar_shape); },
		[](S &s, int v) { s.label_avatar_shape = static_cast<config::AvatarShape>(v); }));

	const char *plate_group = "背景";
	knobs.push_back(toggle("label_show_plate", plate_group, "表示", &S::label_show_plate));
	knobs.push_back(text(KnobKind::Color, "label_plate_color", plate_group, "色", &S::label_plate_color));
	knobs.push_back(whole("label_plate_opacity_pct", plate_group, "不透明度", config::kLabelPlateOpacityMinPct,
			      config::kLabelPlateOpacityMaxPct, "%", &S::label_plate_opacity_pct));

	const char *whole_group = "名前ラベル全体";
	knobs.push_back(with_hint(number("label_margin_pct", whole_group, "余白", config::kLabelMarginMinPct,
					 config::kLabelMarginMaxPct, 10, "%", &S::label_margin_pct),
				  "キャンバスの高さに対する割合です。"));
	knobs.push_back(whole("label_opacity_pct", whole_group, "不透明度", config::kLabelOpacityMinPct,
			      config::kLabelOpacityMaxPct, "%", &S::label_opacity_pct));

	const char *speaking_group = "話している人の強調";
	const char *target_row = "対象";
	knobs.push_back(toggle("label_show_speaking", speaking_group, "光", &S::label_show_speaking));
	knobs.push_back(in_row(toggle("label_glow_frame", speaking_group, "枠", &S::label_glow_frame), target_row));
	knobs.push_back(
		in_row(toggle("label_glow_avatar", speaking_group, "アバター", &S::label_glow_avatar), target_row));
	knobs.push_back(in_row(toggle("label_glow_name", speaking_group, "名前", &S::label_glow_name), target_row));
	knobs.push_back(text(KnobKind::Color, "label_border_color", speaking_group, "色", &S::label_border_color));
	knobs.push_back(with_hint(number("label_glow_spread_ratio", speaking_group, "太さ",
					 config::kLabelGlowSpreadRatioMin, config::kLabelGlowSpreadRatioMax, 100, "倍",
					 &S::label_glow_spread_ratio),
				  kRatioOfFontSize));
	knobs.push_back(with_hint(number("label_glow_blur_ratio", speaking_group, "ぼかし",
					 config::kLabelGlowBlurRatioMin, config::kLabelGlowBlurRatioMax, 100, "倍",
					 &S::label_glow_blur_ratio),
				  kRatioOfFontSize));
	knobs.push_back(with_hint(whole_step("label_dim_pct", speaking_group, "話していない人の暗さ",
					     config::kLabelDimMinPct, config::kLabelDimMaxPct, config::kLabelDimStepPct,
					     "%", &S::label_dim_pct),
				  "話していない人の名前ラベルを暗くする度合いです。0 % で暗くしません。"));
	knobs.push_back(whole("label_border_rise_ms", speaking_group, "フェードイン", config::kLabelBorderTimingMinMs,
			      config::kLabelBorderTimingMaxMs, "ms", &S::label_border_rise_ms));
	knobs.push_back(with_hint(whole("label_border_hold_ms", speaking_group, "保持時間",
					config::kLabelBorderTimingMinMs, config::kLabelBorderTimingMaxMs, "ms",
					&S::label_border_hold_ms),
				  "話し終えてから、フェードアウトを始めるまでの時間です。"));
	knobs.push_back(whole("label_border_fade_ms", speaking_group, "フェードアウト", config::kLabelBorderTimingMinMs,
			      config::kLabelBorderTimingMaxMs, "ms", &S::label_border_fade_ms));

	// 画面共有している参加者の名前ラベル。並びはドックの表示タブと同じ ([表示]、[1 人のときは非表示]、位置、アバターの位置)。
	// 位置は全員一律で、各人の枠の同じ隅に置く。
	const char *shared_group = "画面共有している参加者";
	knobs.push_back(toggle("label_show_in_slot", shared_group, "表示", &S::label_show_in_slot));
	knobs.push_back(toggle("label_hide_when_single", shared_group, "1 人のときは非表示", &S::label_hide_when_single));
	knobs.push_back(with_hint(
		choice(
			"label_corner", shared_group, "名前ラベルの位置", {"左上", "右上", "左下", "右下"},
			[](const S &s) { return static_cast<int>(s.label_corner); },
			[](S &s, int v) { s.label_corner = static_cast<config::LabelCorner>(v); }),
		"それぞれの人の枠の、同じ隅に置きます。"));
	knobs.push_back(with_hint(
		choice(
			"label_avatar_side", shared_group, "アバターの位置", {"左", "右"},
			[](const S &s) { return static_cast<int>(s.label_avatar_side); },
			[](S &s, int v) { s.label_avatar_side = static_cast<config::AvatarSide>(v); }),
		"右にすると、アバターが右端、名前がその左に並びます。"));

	const char *roster_group = "画面共有していない参加者";
	knobs.push_back(toggle("roster_show", roster_group, "表示", &S::roster_show));
	// 配置は OBS の配置の 9 つから中央を除いた 8 つ。並びは、配置に合うものだけを選べる (中央上と中央下は横並び、
	// 左中央と右中央は縦並びだけ)。
	knobs.push_back(choice(
		"roster_anchor", roster_group, "配置",
		{"左上", "中央上", "右上", "左中央", "右中央", "左下", "中央下", "右下"},
		[](const S &s) { return static_cast<int>(s.roster_anchor); },
		[](S &s, int v) {
			s.roster_anchor = static_cast<config::RosterAnchor>(v);
			// 並びが新しい配置に合わなければ、合う並びにする。
			s.roster_direction = config::roster_direction_for(s.roster_anchor, s.roster_direction);
		}));
	{
		KnobDef direction = choice(
			"roster_direction", roster_group, "並び", {"横並び", "縦並び"},
			[](const S &s) { return static_cast<int>(s.roster_direction); },
			[](S &s, int v) {
				s.roster_direction =
					config::roster_direction_for(s.roster_anchor, static_cast<config::RosterDirection>(v));
			});
		direction.choice_allowed = [](const S &s, int v) {
			return config::roster_direction_for(s.roster_anchor, static_cast<config::RosterDirection>(v)) ==
			       static_cast<config::RosterDirection>(v);
		};
		knobs.push_back(direction);
	}
	{
		KnobDef gap = number("roster_gap_ratio", roster_group, "間隔", config::kRosterGapRatioMin,
				     config::kRosterGapRatioMax, 100, "倍", &S::roster_gap_ratio);
		gap.step = 5;
		knobs.push_back(with_hint(gap, kRatioOfFontSize));
	}
	{
		KnobDef width = number("roster_name_max_em", roster_group, "名前の最大幅", config::kRosterNameMaxEmMin,
				       config::kRosterNameMaxEmMax, 10, "倍", &S::roster_name_max_em);
		width.step = 5;
		knobs.push_back(with_hint(width, "フォントサイズに対する倍率です。これより長い名前は、省略して表示します。"));
	}
	knobs.push_back(with_hint(
		choice(
			"roster_avatar_side", roster_group, "アバターの位置", {"左", "右"},
			[](const S &s) { return static_cast<int>(s.roster_avatar_side); },
			[](S &s, int v) { s.roster_avatar_side = static_cast<config::AvatarSide>(v); }),
		"右にすると、アバターが右端、名前がその左に並び、縦並びの列は右揃えになります。"));
	return knobs;
}

} // namespace

std::string knob_hint(const KnobDef &knob, const config::Settings &settings)
{
	return knob.get_hint ? knob.get_hint(settings) : std::string(knob.hint);
}

const std::vector<KnobDef> &knob_defs()
{
	static const std::vector<KnobDef> knobs = make_knob_defs();
	return knobs;
}

double snap_to_step(const KnobDef &knob, double value)
{
	const double divisor = knob.divisor;
	const long long unit = std::max(1, knob.step);
	const long long low = std::llround(knob.min * divisor);
	const long long high = std::llround(knob.max * divisor);
	if (std::isnan(value))
		return static_cast<double>(low) / divisor;
	// 刻み (unit 段ごと) の近い段に丸めてから、範囲に収める。
	const long long rounded = std::llround(value * divisor / static_cast<double>(unit)) * unit;
	const long long step = std::clamp(rounded, low, high);
	return static_cast<double>(step) / divisor;
}

std::string knobs_to_json(const config::Settings &settings)
{
	const nlohmann::json all = nlohmann::json::parse(config::settings_to_json(settings));
	nlohmann::json out = nlohmann::json::object();
	for (const KnobDef &knob : knob_defs())
		out[knob.key] = all.at(knob.key);
	return out.dump(2) + "\n";
}

bool load_knobs(const std::string &json_text, config::Settings *out, std::string *error)
{
	return config::parse_settings(json_text, out, error);
}

bool merge_knobs_into(const std::string &existing_text, const config::Settings &settings, std::string *out,
		      std::string *error)
{
	nlohmann::json base = nlohmann::json::object();
	if (!existing_text.empty()) {
		base = nlohmann::json::parse(existing_text, nullptr, /*allow_exceptions=*/false);
		if (base.is_discarded() || !base.is_object()) {
			if (error)
				*error = "保存先にあるファイルが JSON のオブジェクトとして読めない";
			return false;
		}
	}
	const nlohmann::json values = nlohmann::json::parse(knobs_to_json(settings));
	for (auto it = values.begin(); it != values.end(); ++it)
		base[it.key()] = it.value();
	*out = base.dump(2) + "\n";
	return true;
}

bool knobs_within_range(const config::Settings &settings, std::string *error)
{
	config::Settings clamped = settings;
	config::clamp_settings(clamped);
	if (config::settings_to_json(clamped) != config::settings_to_json(settings)) {
		if (error)
			*error = "範囲の外の値がある (config::clamp_settings が値を変える)";
		return false;
	}
	for (const KnobDef &knob : knob_defs()) {
		if (knob.kind != KnobKind::Number)
			continue;
		const double value = knob.get_number(settings);
		if (snap_to_step(knob, value) != value) {
			if (error)
				*error = std::string(knob.key) + " の値が刻みに合わない";
			return false;
		}
	}
	return true;
}

} // namespace cordmux::label_tuner

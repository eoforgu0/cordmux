// SPDX-License-Identifier: Apache-2.0

// tuner_sweep.h の実装。

#include "tuner_sweep.h"

#include <QFontDatabase>
#include <QStringList>

#include <cstdio>
#include <string>
#include <vector>

#include "tuner_knobs.h"
#include "tuner_window.h"

namespace cordmux::label_tuner {

namespace {

constexpr std::uint64_t kMs = 1'000'000ULL;

// 話している人の席。見本と同じく右上の枠に並ぶ人 (席 4)。
constexpr int kSpeakingSeat = 3;
constexpr int kSpeakingTile = 1;

// 画面共有していない参加者で話し続ける行 (参加者 2)。
constexpr int kSpeakingRosterRow = 1;

// 1 段。apply がつまみを動かす。
struct Step {
	QString label;
	std::function<bool(TunerWindow &)> apply;
};

std::vector<Step> steps_of(const KnobDef &def)
{
	const config::Settings defaults;
	const std::string key = def.key;
	std::vector<Step> steps;
	switch (def.kind) {
	case KnobKind::Number: {
		// 下の端はスライダー、既定は数の欄、上の端はスライダーで動かす (どちらの経路も通す)。
		const double values[] = {def.min, def.get_number(defaults), def.max};
		const char *names[] = {"下の端", "既定", "上の端"};
		for (int i = 0; i < 3; ++i) {
			const double v = values[i];
			const bool via_slider = i != 1;
			steps.push_back({QStringLiteral("%1 %2").arg(QString::fromUtf8(names[i])).arg(v),
					 [key, v, via_slider](TunerWindow &w) {
						 return w.set_knob_number(key, v, via_slider);
					 }});
		}
		break;
	}
	case KnobKind::Toggle: {
		const bool fallback = def.get_toggle(defaults);
		for (const bool on : {false, true})
			steps.push_back(
				{QStringLiteral("%1%2").arg(on ? QStringLiteral("オン") : QStringLiteral("オフ"),
							    on == fallback ? QStringLiteral(" (既定)") : QString()),
				 [key, on](TunerWindow &w) {
					 return w.set_knob_toggle(key, on);
				 }});
		break;
	}
	case KnobKind::Color: {
		const QString fallback = QString::fromStdString(def.get_text(defaults)).toUpper();
		QStringList colors{QStringLiteral("#000000"), fallback, QStringLiteral("#FF00FF")};
		colors.removeDuplicates();
		for (const QString &color : colors)
			steps.push_back({color + (color == fallback ? QStringLiteral(" (既定)") : QString()),
					 [key, color](TunerWindow &w) {
						 return w.set_knob_text(key, color);
					 }});
		break;
	}
	case KnobKind::Choice: {
		// 選択肢を 1 つずつ。
		const int fallback = def.get_choice(defaults);
		for (std::size_t i = 0; i < def.options.size(); ++i) {
			const int index = static_cast<int>(i);
			steps.push_back({QStringLiteral("%1%2").arg(QString::fromUtf8(def.options[i]),
								    index == fallback ? QStringLiteral(" (既定)") : QString()),
					 [key, index](TunerWindow &w) {
						 return w.set_knob_choice(key, index);
					 }});
		}
		break;
	}
	case KnobKind::Font: {
		QStringList families{QString()};
		for (const char *family : {"Segoe UI", "Meiryo"})
			if (QFontDatabase::families().contains(QString::fromLatin1(family)))
				families.append(QString::fromLatin1(family));
		for (const QString &family : families)
			steps.push_back(
				{family.isEmpty() ? QStringLiteral("(既定)") : family, [key, family](TunerWindow &w) {
					 return w.set_knob_text(key, family);
				 }});
		break;
	}
	}
	return steps;
}

} // namespace

int run_knob_sweep(TunerWindow &window, std::uint64_t *virtual_now, const QString &dir,
		   const std::function<void()> &refresh, const ShotWriter &write_shot)
{
	const auto advance = [virtual_now](std::uint64_t ms) {
		*virtual_now += ms * kMs;
	};
	const auto speak = [&window, virtual_now](bool on) {
		window.set_speaking(kSpeakingSeat, false, on, *virtual_now);
	};

	// 右上の人が話し続けている (明るさが 1 の) 状態から始める。画面共有していない参加者の 2 人目 (参加者 2) も話し続ける
	// (画面共有していない参加者を出す見本で、話している人と話していない人が並ぶ)。
	speak(true);
	window.set_roster_speaking(kSpeakingRosterRow, false, true, *virtual_now);
	advance(10'000);
	int differing = 0;
	int index = 0;
	for (const KnobDef &def : knob_defs()) {
		++index;
		const std::string key = def.key;
		const bool rise = key == "label_border_rise_ms";
		const bool hold = key == "label_border_hold_ms";
		const bool fade = key == "label_border_fade_ms";
		const bool roster_knob = key.rfind("roster_", 0) == 0;
		const bool with_roster = roster_knob || key == "label_dim_pct" || key == "label_show_plate";

		// 前のつまみを既定に戻し、このつまみが見える場面にする (背景の色と不透明度は背景を出す)。画面共有していない
		// 参加者に関わるつまみには、5 人を出す。
		window.reset_knobs();
		window.set_people(key == "label_hide_when_single" ? 1 : 4);
		window.set_roster_people(with_roster ? 5 : 0);
		window.set_view_tile(kSpeakingTile);
		if (roster_knob)
			window.set_view_roster(0);
		if (key == "label_plate_color" || key == "label_plate_opacity_pct")
			window.set_knob_toggle("label_show_plate", true);

		const std::vector<Step> steps = steps_of(def);
		for (std::size_t i = 0; i < steps.size(); ++i) {
			if (!steps[i].apply(window)) {
				std::fprintf(stderr, "label_tuner: %s を %s にできません\n", key.c_str(),
					     steps[i].label.toUtf8().constData());
				return 2;
			}
			QJsonObject extra;
			if (rise || hold || fade) {
				// 黙って消え終わるのを待ってから話し始め、立ち上がりは 100 ms 後、保つと消えるは 1 秒話して止まった
				// 100 ms 後と 300 ms 後に書き出す (保つと消えるの既定は 200 ms)。
				advance(100);
				speak(false);
				advance(11'000);
				speak(true);
				std::uint64_t after = 100;
				if (!rise) {
					advance(1000);
					speak(false);
					after = hold ? 100 : 300;
				}
				advance(after);
				extra[QStringLiteral("scenario")] = rise   ? QStringLiteral("start+100")
								    : hold ? QStringLiteral("speak1000+stop+100")
									   : QStringLiteral("speak1000+stop+300");
			} else {
				advance(20);
			}
			window.focus_knob(key);
			refresh();
			const long long mismatch = window.fresh_mismatch();
			extra[QStringLiteral("knob")] = QString::fromLatin1(def.key);
			extra[QStringLiteral("step")] = steps[i].label;
			extra[QStringLiteral("fresh_mismatch")] = static_cast<double>(mismatch);
			const QString path = QStringLiteral("%1/%2_%3_%4.png")
						     .arg(dir)
						     .arg(index, 2, 10, QLatin1Char('0'))
						     .arg(QString::fromLatin1(def.key))
						     .arg(i);
			if (!write_shot(path, extra))
				return 2;
			if (mismatch != 0)
				++differing;
		}
		if (rise || hold || fade) {
			// 次のつまみは、また話し続けている状態で見る。
			window.reset_knobs();
			advance(100);
			speak(true);
			advance(10'000);
		}
	}

	// 画面共有していない参加者の配置 8 か所 x 並び 2 (選べる組だけ) を、8 人で並べた見本。横並びは入りきらなければ折り返す。
	// アバターの位置は左と右の両方を見せる (右のときの縦並びの列は右揃え)。既定 (右下、縦並び、アバター右) もこの中にある。
	static const char *const anchor_names[] = {"tl", "tc", "tr", "ml", "mr", "bl", "bc", "br"};
	static const char *const direction_names[] = {"row", "column"};
	for (const bool avatar_right : {false, true}) {
		for (int anchor = 0; anchor < 8; ++anchor) {
			for (int direction = 0; direction < 2; ++direction) {
				window.reset_knobs();
				window.set_people(4);
				window.set_roster_people(8);
				window.set_view_tile(kSpeakingTile);
				window.set_view_roster(0);
				if (!window.set_knob_choice("roster_avatar_side", avatar_right ? 1 : 0))
					return 2;
				if (!window.set_knob_choice("roster_anchor", anchor))
					return 2;
				// 配置に合わない並びは選べない。選べない組は見本にしない。
				if (!window.set_knob_choice("roster_direction", direction))
					continue;
				++index;
				advance(20);
				window.focus_knob("roster_direction");
				refresh();
				const long long mismatch = window.fresh_mismatch();
				QJsonObject extra;
				extra[QStringLiteral("knob")] = QStringLiteral("roster_layout");
				extra[QStringLiteral("step")] = QStringLiteral("%1 %2 アバター%3")
									.arg(QString::fromLatin1(anchor_names[anchor]),
									     QString::fromLatin1(direction_names[direction]),
									     avatar_right ? QStringLiteral("右") : QStringLiteral("左"));
				extra[QStringLiteral("fresh_mismatch")] = static_cast<double>(mismatch);
				const QString path = QStringLiteral("%1/%2_roster_layout_%3_%4_%5.png")
							     .arg(dir)
							     .arg(index, 2, 10, QLatin1Char('0'))
							     .arg(QString::fromLatin1(anchor_names[anchor]))
							     .arg(QString::fromLatin1(direction_names[direction]))
							     .arg(avatar_right ? QStringLiteral("right") : QStringLiteral("left"));
				if (!write_shot(path, extra))
					return 2;
				if (mismatch != 0)
					++differing;
			}
		}
	}
	return differing == 0 ? 0 : 1;
}

} // namespace cordmux::label_tuner

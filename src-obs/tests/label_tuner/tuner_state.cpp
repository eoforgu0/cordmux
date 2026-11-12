// SPDX-License-Identifier: Apache-2.0

// tuner_state.h の実装。

#include "tuner_state.h"

#include <QStringList>

#include "composite_scene.h"
#include "tuner_knobs.h"

namespace cordmux::label_tuner {

namespace sources = cordmux::sources;
namespace decor_preview = cordmux::decor_preview;

namespace {

bool parse_ms(const QString &text, int *out)
{
	bool ok = false;
	const int value = text.toInt(&ok);
	if (!ok || value < 0)
		return false;
	*out = value;
	return true;
}

// 「from-to」を「,」でつないだ話している区間。to を書かなければ話したまま。
bool parse_spans(const QString &text, std::vector<SpeakSpan> *out)
{
	out->clear();
	for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		const QStringList ends = part.split(QLatin1Char('-'));
		if (ends.size() != 2)
			return false;
		SpeakSpan span;
		if (!parse_ms(ends[0], &span.from_ms))
			return false;
		if (!ends[1].isEmpty() && (!parse_ms(ends[1], &span.to_ms) || span.to_ms < span.from_ms))
			return false;
		out->push_back(span);
	}
	return true;
}

} // namespace

ToolState default_tool_state()
{
	ToolState state;
	for (int i = 0; i < sources::kCompositeCellCount; ++i)
		state.seats[static_cast<std::size_t>(i)].person = i;
	// 画面共有していない参加者の初めの人は、画面共有の 4 人の次の人から順に並べる (見本の人の残りと、足した架空の人)。
	for (int i = 0; i < kRosterRows; ++i)
		state.roster_rows[static_cast<std::size_t>(i)].person = decor_preview::kSamplePeopleCount + i;
	return state;
}

QString corner_title(sources::DecorCorner corner)
{
	switch (corner) {
	case sources::DecorCorner::TopLeft:
		return QStringLiteral("左上");
	case sources::DecorCorner::TopRight:
		return QStringLiteral("右上");
	case sources::DecorCorner::BottomLeft:
		return QStringLiteral("左下");
	case sources::DecorCorner::BottomRight:
		return QStringLiteral("右下");
	}
	return QString();
}

bool parse_state(const QString &text, ToolState *state, QString *error)
{
	const std::vector<decor_preview::ScenePerson> &people = decor_preview::scene_people();
	for (const QString &item : text.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
		const int eq = item.indexOf(QLatin1Char('='));
		if (eq <= 0) {
			*error = QStringLiteral("--state の「%1」が「名前=値」の形ではありません").arg(item);
			return false;
		}
		QString name = item.left(eq).trimmed();
		const QString value = item.mid(eq + 1).trimmed();
		// 画面共有していない参加者の行の番号 (1 から 8) で終わる名前 (rperson、rspeak、rcycle) は、その行の値。
		int row = -1;
		for (const QString &prefix : {QStringLiteral("rperson"), QStringLiteral("rspeak"), QStringLiteral("rcycle")}) {
			if (name.size() == prefix.size() + 1 && name.startsWith(prefix) && name.back().isDigit()) {
				row = name.back().digitValue() - 1;
				name.chop(1);
				if (row < 0 || row >= kRosterRows) {
					*error = QStringLiteral("--state の行の番号は 1 から %1 です: %2").arg(kRosterRows).arg(item);
					return false;
				}
				break;
			}
		}
		// 席の番号 (1 から 4) で終わる名前は、その席の値。
		int seat = -1;
		if (row < 0 && !name.isEmpty() && name.back().isDigit()) {
			seat = name.back().digitValue() - 1;
			name.chop(1);
			if (seat < 0 || seat >= sources::kCompositeCellCount) {
				*error = QStringLiteral("--state の席の番号は 1 から 4 です: %1").arg(item);
				return false;
			}
		}
		bool ok = true;
		if (row >= 0) {
			RosterRowState &r = state->roster_rows[static_cast<std::size_t>(row)];
			if (name == QStringLiteral("rperson")) {
				ok = false;
				for (std::size_t i = 0; i < people.size(); ++i) {
					if (value == QString::fromLatin1(people[i].id)) {
						r.person = static_cast<int>(i);
						ok = true;
					}
				}
			} else if (name == QStringLiteral("rspeak")) {
				ok = parse_spans(value, &r.speak);
			} else {
				ok = parse_ms(value, &r.cycle_from_ms);
			}
		} else if (seat >= 0) {
			SeatState &s = state->seats[static_cast<std::size_t>(seat)];
			if (name == QStringLiteral("person")) {
				ok = false;
				for (std::size_t i = 0; i < people.size(); ++i) {
					if (value == QString::fromLatin1(people[i].id)) {
						s.person = static_cast<int>(i);
						ok = true;
					}
				}
			} else if (name == QStringLiteral("speak")) {
				ok = parse_spans(value, &s.speak);
			} else if (name == QStringLiteral("cycle")) {
				ok = parse_ms(value, &s.cycle_from_ms);
			} else {
				ok = false;
			}
		} else if (name == QStringLiteral("people")) {
			int count = 0;
			ok = parse_ms(value, &count) && count >= 1 && count <= sources::kCompositeCellCount;
			if (ok)
				state->people = count;
		} else if (name == QStringLiteral("roster")) {
			int count = 0;
			ok = parse_ms(value, &count) && count <= kRosterRows;
			if (ok)
				state->roster = count;
		} else if (name == QStringLiteral("runheard")) {
			ok = value == QStringLiteral("0") || value == QStringLiteral("1");
			if (ok)
				state->roster_last_unheard = value == QStringLiteral("1");
		} else if (name == QStringLiteral("rview")) {
			int index = 0;
			ok = parse_ms(value, &index) && index >= 1 && index <= kRosterRows;
			if (ok)
				state->view_roster = index - 1;
		} else if (name == QStringLiteral("bg")) {
			const QStringList kinds{QStringLiteral("game"), QStringLiteral("sky"),
						QStringLiteral("stripes"), QStringLiteral("snow")};
			if (value == QStringLiteral("mixed")) {
				state->background = BackgroundMode::Mixed;
			} else if (kinds.contains(value)) {
				state->background = BackgroundMode::Kind;
				state->background_kind = static_cast<int>(kinds.indexOf(value));
			} else if (value.startsWith(QStringLiteral("file:"))) {
				state->background = BackgroundMode::Picture;
				state->picture_path = value.mid(5);
			} else {
				ok = false;
			}
		} else if (name == QStringLiteral("view")) {
			int tile = 0;
			ok = parse_ms(value, &tile) && tile < sources::kCompositeCellCount;
			if (ok)
				state->view_tile = tile;
		} else if (name == QStringLiteral("focus")) {
			ok = false;
			for (const KnobDef &knob : knob_defs())
				ok = ok || value == QString::fromLatin1(knob.key);
			if (ok)
				state->focus_key = value;
		} else if (name == QStringLiteral("press")) {
			ok = value == QStringLiteral("save") || value == QStringLiteral("reset");
			if (ok)
				state->press = value;
		} else if (name == QStringLiteral("bench")) {
			ok = parse_ms(value, &state->bench_frames) && state->bench_frames <= 3600;
		} else if (name == QStringLiteral("at")) {
			ok = parse_ms(value, &state->at_ms);
		} else if (name == QStringLiteral("size")) {
			const QStringList wh = value.split(QLatin1Char('x'));
			ok = wh.size() == 2 && parse_ms(wh[0], &state->window_w) && parse_ms(wh[1], &state->window_h) &&
			     state->window_w >= 400 && state->window_h >= 300 && state->window_w <= 7680 &&
			     state->window_h <= 4320;
		} else {
			ok = false;
		}
		if (!ok) {
			*error =
				QStringLiteral(
					"--state の「%1」が読めません (書き方は src-obs/tests/label_tuner/README.md の「--state」)")
					.arg(item);
			return false;
		}
	}
	return true;
}

} // namespace cordmux::label_tuner

// SPDX-License-Identifier: Apache-2.0

// text_resolve.h の実装。

#include "dock/ui/text_resolve.h"

#include <obs-module.h>

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>

#include <cstddef>

#include "dock/ui/text_arg.h"

namespace cordmux::dock::ui {

namespace {

// %1、%2 ... の番号として数える上限。args の数より十分大きく、int の桁より十分小さければよい。
constexpr int kMaxTextRefIndex = 1000000;

// 時刻の書式 (地域の設定の短い形から秒を除き、画面の言語が日本語なら 24 時間制にする)。with_date が true なら、
// QLocale の短い日時の形にする (日付と時刻のつなぎ方も地域の設定のまま)。
QString clock_format(bool with_date)
{
	const QLocale system = QLocale::system();
	const QString raw = with_date ? system.dateTimeFormat(QLocale::ShortFormat)
				      : system.timeFormat(QLocale::ShortFormat);
	const bool force_24h = QLocale().language() == QLocale::Japanese;
	return QString::fromStdString(calc::short_clock_format(raw.toStdString(), force_24h));
}

} // namespace

QString format_time_us(std::int64_t us, bool with_date)
{
	const QDateTime dt = QDateTime::fromMSecsSinceEpoch(us / 1000).toLocalTime();
	return QLocale::system().toString(dt, clock_format(with_date));
}

QString format_arg(const std::string &arg)
{
	const calc::ArgPlan plan = calc::plan_arg(arg);
	switch (plan.kind) {
	case calc::ArgKind::Time:
		return format_time_us(plan.epoch_us, false);
	case calc::ArgKind::DateTime:
		return format_time_us(plan.epoch_us, true);
	case calc::ArgKind::Plain:
		break;
	}
	return QString::fromUtf8(plan.text.c_str());
}

QString resolve_key(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString resolve(const ledger::TextRef &ref)
{
	const QString text = resolve_key(ref.id.c_str());
	QString out;
	out.reserve(text.size());
	for (int i = 0; i < text.size(); ++i) {
		if (text.at(i) == QLatin1Char('%') && i + 1 < text.size() && text.at(i + 1).isDigit()) {
			int j = i + 1;
			int index = 0;
			while (j < text.size() && text.at(j).isDigit()) {
				// 桁の長い数字の並びで int があふれないようにする (あふれは未定義動作)。上限を超えたら
				// 数えるのをやめる (範囲の外に決まるので、そのままの文字列として表示される)。
				if (index <= kMaxTextRefIndex)
					index = index * 10 + text.at(j).digitValue();
				++j;
			}
			if (index >= 1 && static_cast<std::size_t>(index) <= ref.args.size()) {
				out += format_arg(ref.args[static_cast<std::size_t>(index) - 1]);
				i = j - 1;
				continue;
			}
		}
		out += text.at(i);
	}
	return out;
}

QString resolve_label(const ledger::Label &label)
{
	if (label.is_text())
		return resolve(*label.text);
	// Discord から来た文字列からも、画面に表示しない制御文字を除く (format_arg と同じ除き方)。
	return QString::fromUtf8(calc::detail::without_controls(label.literal, 0).c_str());
}

} // namespace cordmux::dock::ui

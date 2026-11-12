// SPDX-License-Identifier: Apache-2.0

// report.h の実装。

#include "report.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

namespace cordmux::dock_drive {

namespace {

QString verdict_name(Verdict v)
{
	switch (v) {
	case Verdict::Pass:
		return QStringLiteral("pass");
	case Verdict::Fail:
		return QStringLiteral("FAIL");
	case Verdict::Info:
		break;
	}
	return QStringLiteral("info");
}

} // namespace

void say(const QString &line)
{
	std::fputs(line.toUtf8().constData(), stdout);
	std::fputs("\n", stdout);
	std::fflush(stdout);
}

QString tsv_cell(QString text)
{
	text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
	text.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
	text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
	text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
	return text;
}

bool write_file(const QString &path, const QString &text)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(text.toUtf8());
	return true;
}

void Results::add(const QString &group, const QString &item, const QString &value, const QString &expect,
		  Verdict verdict, const QString &note)
{
	rows_.push_back({group, item, value, expect, verdict, note});
	say(QStringLiteral("[%1] %2 %3: %4 (expect %5)%6")
		    .arg(verdict_name(verdict), group, item, value, expect,
			 note.isEmpty() ? QString() : QStringLiteral(" -- ") + note));
}

void Results::expect_eq(const QString &group, const QString &item, long long value, long long expected,
			const QString &note)
{
	add(group, item, QString::number(value), QString::number(expected),
	    value == expected ? Verdict::Pass : Verdict::Fail, note);
}

void Results::expect_true(const QString &group, const QString &item, bool value, const QString &note)
{
	add(group, item, value ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"),
	    value ? Verdict::Pass : Verdict::Fail, note);
}

void Results::info(const QString &group, const QString &item, const QString &value, const QString &note)
{
	add(group, item, value, QStringLiteral("-"), Verdict::Info, note);
}

bool Results::any_fail() const
{
	return fail_count() > 0;
}

int Results::fail_count() const
{
	int n = 0;
	for (const ResultRow &r : rows_)
		n += r.verdict == Verdict::Fail ? 1 : 0;
	return n;
}

bool Results::write_tsv(const QString &path) const
{
	QString text = QStringLiteral("group\titem\tvalue\texpect\tverdict\tnote\n");
	for (const ResultRow &r : rows_) {
		text += QStringList{tsv_cell(r.group), tsv_cell(r.item), tsv_cell(r.value), tsv_cell(r.expect),
				    verdict_name(r.verdict), tsv_cell(r.note)}
				.join(QLatin1Char('\t')) +
			QLatin1Char('\n');
	}
	return write_file(path, text);
}

void Results::print() const
{
	int pass = 0;
	int fail = 0;
	int info = 0;
	for (const ResultRow &r : rows_) {
		if (r.verdict == Verdict::Pass)
			++pass;
		else if (r.verdict == Verdict::Fail)
			++fail;
		else
			++info;
	}
	say(QStringLiteral("results: pass=%1 FAIL=%2 info=%3").arg(pass).arg(fail).arg(info));
}

} // namespace cordmux::dock_drive

// SPDX-License-Identifier: Apache-2.0

// 結果の表 (1 つの確認に 1 行) と、その書き出し。列は group (シナリオ)、item (確認すること)、value (値)、expect (合格の
// 条件)、verdict (pass、FAIL、info)、note (補足)。info は合否を持たない記録 (数えるだけの値。例: メニューの項目の押せる
// 範囲) である。FAIL が 1 つでもあれば、終了コードは 1 になる。

#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace cordmux::dock_drive {

enum class Verdict { Pass, Fail, Info };

struct ResultRow {
	QString group;
	QString item;
	QString value;
	QString expect;
	Verdict verdict = Verdict::Info;
	QString note;
};

class Results {
public:
	// 1 行を加え、同じ内容を標準出力にも書く。
	void add(const QString &group, const QString &item, const QString &value, const QString &expect, Verdict verdict,
		 const QString &note = QString());
	// value が expected と等しければ pass、違えば FAIL の行を加える。
	void expect_eq(const QString &group, const QString &item, long long value, long long expected,
		       const QString &note = QString());
	void expect_true(const QString &group, const QString &item, bool value, const QString &note = QString());
	// 合否を持たない info の行を加える。
	void info(const QString &group, const QString &item, const QString &value, const QString &note = QString());

	bool any_fail() const;
	int fail_count() const;
	const std::vector<ResultRow> &rows() const { return rows_; }

	// path に TSV で書く。フォルダーがなければ作る。書けなければ false を返す。
	bool write_tsv(const QString &path) const;
	// pass、FAIL、info の件数を、標準出力に 1 行で書く。
	void print() const;

private:
	std::vector<ResultRow> rows_;
};

// 標準出力に 1 行を書き、すぐにフラッシュする。
void say(const QString &line);
// 文字列を TSV のフィールドにする。バックスラッシュ、タブ、改行を \\、\t、\n、\r の形にエスケープする。
QString tsv_cell(QString text);
// text を UTF-8 で path に書く (フォルダーがなければ作る)。書けなければ false を返す。
bool write_file(const QString &path, const QString &text);

} // namespace cordmux::dock_drive

// SPDX-License-Identifier: Apache-2.0

// 1 行で省くラベル (ElideLabel) と、折り返すラベル (CappedWrapLabel、make_wrap_label)。
// テキストを表示するラベルは、折り返す (高さは中身から決まる) か、1 行で省いて全文をツールチップとアクセシブル名に
// 入れるかの、どちらかにする。右が切れるだけのラベルは作らない。

#pragma once

#include <QFontMetrics>
#include <QLabel>
#include <QString>
#include <QStringList>

namespace cordmux::dock::ui {

// 1 行で省くラベル。
//   - 横のサイズポリシーを Ignored にする (OBS のソースの一覧の名前のラベルと同じ)。テキストの幅全体を最小の幅に
//     しないので、長いテキスト 1 つでドックの最小幅が広がらない。最小の幅 (名前なら 4 em) は、使う側が
//     setMinimumWidth で設定する。
//   - 末尾を省く (Qt::ElideRight) か、中央を省く (Qt::ElideMiddle。先頭が同じ名前の見分けと、フォルダーのパス) かを
//     選べる。
//   - 末尾を省くときは、数・時刻・人数・件数の語の途中で切らない (dock_ui_calc.h の safe_cut)。
//   - 長い順の候補 (平常の要約。ledger/dock_view.h の FirstLine::forms) を渡したときは、幅に入る最初の
//     候補を表示し、どれも入らないときだけ候補の末尾を省く。
//   - 省いたときは、全文をツールチップとアクセシブル名に入れる。使う側が設定したツールチップの行は、全文の後に残す。
//   - 省略記号は Qt が付けるものを使う。テキストは常にプレーンテキストとして表示する (Discord から来た名前を
//     HTML として解釈させないため)。
class ElideLabel : public QLabel {
public:
	explicit ElideLabel(QWidget *parent = nullptr);

	// 1 つのテキストを表示する (set_forms の候補は使わなくなる)。
	void set_full_text(const QString &text);
	// 長い順に並べた候補を表示する。幅に入る最初の候補を表示し、どれも入らなければ候補の末尾を省く。
	// Qt::ElideRight で数の語を守る (set_keep_numbers が true) ときは、守る語の途中で切らずに省ける候補を選んで
	// 省き (calc::elide_form_index)、ほかのときは最後の候補を省く。
	void set_forms(const QStringList &forms);

	void set_elide_mode(Qt::TextElideMode mode);
	// 末尾を省くときに、数の語、en の短縮形の語、ja の打ち消しの述語を途中で切らないか (既定は true。
	// calc::safe_cut)。
	void set_keep_numbers(bool keep);
	// 省いていないときのツールチップとアクセシブル名 (使う側が設定するもの)。省いたときは、ツールチップは全文を
	// 1 行目に置いてこのテキストの行を続け (このテキストの 1 行目が全文と同じなら、このテキストのまま)、
	// アクセシブル名は全文に替わる。
	void set_base_tool_tip(const QString &tip);
	void set_base_accessible_name(const QString &name);
	// ラベルが自分のツールチップを持つか (既定は true)。false なら、省いたときも全文をツールチップに入れず、
	// ツールチップを空にする (アクセシブル名はそのまま)。親のウィジェットがツールチップを 1 つにまとめて持つ所
	// (ドックの 1 行目は、見出しの全文と本文を 1 つのツールチップに持つ) で、ツールチップが 2 つにならない
	// ようにする。
	void set_own_tool_tip(bool on);

	// 推奨の幅 (sizeHint の幅) を px で指定する。負の値 (既定) なら、省かない全文の幅。複数のラベルの列の幅をそろえるときに、
	// 使う側が全員の全文の幅から決めた値を渡す。最小の幅は変えない。
	void set_preferred_width(int px);

	// 全文 (候補の並びなら、いちばん長い候補)。
	QString full_text() const;
	// 全文から何かを落としているか (短い候補を表示しているか、省いている)。
	bool is_elided() const { return elided_; }
	// 表示している候補の番号 (候補の並びでないときは 0)。
	int shown_form() const { return form_index_; }

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

	// 現在の幅で省き直す。幅もテキストも変わっていないときも省き直す。
	void refit();

protected:
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	int available_width() const;
	QString elide_one(const QString &text, int width) const;

	QString full_;
	QStringList forms_;
	bool use_forms_ = false;
	Qt::TextElideMode mode_ = Qt::ElideRight;
	bool keep_numbers_ = true;
	QString base_tip_;
	QString base_name_;
	bool own_tip_ = true;
	int preferred_w_ = -1;
	bool elided_ = false;
	int form_index_ = 0;
	bool fitting_ = false;
};

// 折り返すラベル (プレーンテキスト、横のサイズポリシーは Ignored、高さは中身から決まる)。スクロール領域の中の文
// (メッセージのレベル 2 のカードとメッセージのレベル 3 の行の見出しと本文、空の一覧の文など) に使う。
//
// 高さは、ラベルに最大の幅 (段落の 40 em など) があれば、その幅で測る。Qt のレイアウトは、ラベルに最大の幅が
// あっても、レイアウトの幅のままラベルの高さを問い合わせる (QWidgetItem::heightForWidth は幅を最大の幅で切らない)。
// ラベルは最大の幅で配置されるので、そのままでは広いドックで 1 行分の高さしか取られず、2 行目が切れる。
class CappedWrapLabel : public QLabel {
public:
	explicit CappedWrapLabel(QWidget *parent = nullptr);
	int heightForWidth(int width) const override;
};

// CappedWrapLabel を作ってテキストを設定する。
QLabel *make_wrap_label(const QString &text, QWidget *parent = nullptr);

// 末尾を省いたテキスト (数の語などの途中で切らない。calc::safe_cut)。使う側が自分で描くテキストに使う。
QString elide_keep_numbers(const QFontMetrics &fm, const QString &text, int width);

} // namespace cordmux::dock::ui

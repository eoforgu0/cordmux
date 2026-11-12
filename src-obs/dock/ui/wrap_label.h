// SPDX-License-Identifier: Apache-2.0

// 折り返すラベル (WrapLabel) と、折り返しと最後の行の規則の計算 (last_line_fits、fitting_wrap_width、wrap_lines)。
// テキストはプレーンテキストで表示し、語の切れ目で折り返す (QLabel::setWordWrap)。ラベルの幅がレイアウトで決まる所
// (設定ウィンドウと参加者の設定ウィンドウの補足) では、最後の行が短すぎる幅になったとき、テキストを折り返す
// 幅だけを右の余白で狭めて、前の行から文字を移す (狭めるのはラベルの幅の floor_ratio まで)。ラベルの幅そのものと、
// ほかのウィジェットの配置は変えない。テキストは呼び出し元が ini から読んで渡し、箇条書きの記号も呼び出し元が
// 渡す (このファイルは ini のキーを持たない)。

#pragma once

#include <QLabel>
#include <QMargins>
#include <QSize>
#include <QString>
#include <QStringList>

#include "dock/ui/dock_style.h"

class QFont;

namespace cordmux::dock::ui {

// テキストを幅 width で折り返したとき、最後の行が規則に合うか (1 行に入るなら合う)。規則は 2 つある (値は DockStyle)。
//   - 文字数: min_graphemes 文字 (書記素クラスターの数) より少なくしない。最後の行が句読点を含めて 2 文字以下に
//     なると読みにくいため。
//   - 長さ: 折り返す幅の min_ratio より短くしない。文字数の規則は満たすが、「さい。」や "folder." のように 1 語だけが
//     最後の行に残る形を避ける。min_ratio が 0 なら文字数だけを見る。
// 最後の行の前の文の切れ目でラベルを 2 つに分ける方法は使わない。ini の 1 つの段階落が画面で 2 つの段階落に見え、
// 段落の分け方が、幅とフォントサイズで変わるため。
bool last_line_fits(const QString &text, const QFont &font, int width, int min_graphemes, double min_ratio);

// ラベルの幅 width の中で、最後の行が規則に合う、いちばん広い折り返す幅 (width から width * floor_ratio まで 1 px
// ずつ調べる)。どれも合わなければ、文字数だけに合ういちばん広い幅、それもなければ width を返す。
int fitting_wrap_width(const QString &text, const QFont &font, int width, int min_graphemes, double min_ratio,
		       double floor_ratio);

// 折り返す位置の決め方 (set_breaks)。Qt の折り返し (既定) のほかに 2 つある。
//   - KeepNumbers (ドックのカードの見出しと段落): 数と単位の語 (calc::number_word_spans。「13:41」「30分」) を行で
//     分けない。角かっこで囲んだボタンの名前 (calc::button_name_spans。「[Vesktopに追加]」) は、1 行に入り、分けなくても行が
//     増えないときは分けない。
//     数字を含む括弧の組 (calc::number_bracket_spans。「(2人)」) は、組が 1 行に入り、分けなくても行が
//     増えないときだけ分けない。最後の行が規則に合わなければ、行の数を増やさない範囲で行の長さをそろえる (ドックの
//     幅は利用者が決めるので、カードの高さを変えないため)。1 つの語が幅に入らないときだけ、語の中で折り返す
//     (最小の幅は 1 文字。長い語でドックの最小の幅を広げないため)。
//   - PathSeparators (場所の行): 区切りの「\」「/」の直後で折り返し、1 つの区切りの間が幅に入らないときは、名前の中の
//     空白、次に「-」「_」「.」の直後で折り返し、どれも入らないときだけ文字の間で折り返す (数字の並びの途中では
//     折り返さない。段階の順と理由は calc::path_break)。省かない (全文が見える)。最後の行の規則は適用しない
//     (末尾の「cordmux」1 語の行は、パスの形のままにする)。
// 実装: ラベルが現在の幅で折り返す位置を決め、その位置に行の区切り (U+2028) を入れたテキストを QLabel に表示する。
// Qt の折り返す位置は語と文字の種類だけで変わり、折り返してよい位置を加える API がないため (ダイアログの主文の
// 折り返しと同じ文字を使う)。元のテキストは source_text() と動的なプロパティ sourceText に持ち、アクセシブル名にも元の
// テキストを入れる。選択してコピーできる (TextSelectableByMouse) ラベルでは、コピーする文字列から U+2028 を除く
// (Ctrl+C と、右クリックのメニューの [コピー])。選択したテキストをドラッグしてほかへドロップする操作は止める
// (ドロップした先で、パスに改行が入るため。選択した所を押すと、選択をやめる)。
// 次の方法は使わない。ゼロ幅スペース (U+200B) で折り返してよい位置を加える (コピーした文字列に混ざり、Windows の
// エクスプローラーのアドレスバーに貼ると開けない)。読み取り専用の QLineEdit にする (右が切れて、末尾のフォルダー名が
// 見えない)。フォーカスがない間だけ中央を省く入力欄にする (見えている文字列とコピーする文字列が違う)。QTextEdit を
// 読み取り専用で使う (OBS のテーマが入力欄の枠と背景を描くので、ラベルに見せるにはスタイルシートで打ち消すことに
// なる)。
enum class WrapBreaks {
	Qt,             // Qt の折り返しのまま (既定)
	KeepNumbers,    // 数と単位の語を分けず、行の数を増やさずに最後の行の長さをそろえる (ドックのカード)
	PathSeparators, // 区切りの直後で折り返す (場所の行。規則は calc::path_break)
};

// テキストを幅 width (テキストを置ける幅) で、mode の決め方で折り返した行 (つなぐと元のテキストに戻る)。
// WrapBreaks::Qt では Qt の折り返しのまま。KeepNumbers の、最後の行の長さをそろえる処理は含まない (行の決め方だけ)。
QStringList wrap_lines(const QString &text, const QFont &font, int width, WrapBreaks mode);

class WrapLabel : public QLabel {
public:
	explicit WrapLabel(const QString &text = QString(), QWidget *parent = nullptr);

	// テキストを替える (QLabel::setText は仮想関数ではないので、狭める幅と折り返す位置を決め直すこの関数を使う)。
	void set_text(const QString &text);
	// 折り返す位置を入れる前のテキスト (WrapBreaks::Qt なら text() と同じ)。
	QString source_text() const;

	// 最後の行の規則 (既定は DockStyle の既定の値)。
	void set_last_line_rule(int min_graphemes, double min_ratio, double floor_ratio);
	void set_last_line_rule(const DockStyle &style);
	// 最後の行の規則に合わせて、ラベルが自分で折り返す幅を狭めるか (既定は true)。幅を呼び出し元が決めるラベル
	// (ダイアログの列。ダイアログのすべてのラベルで同じ幅を選ぶ) では、呼び出し元が幅を決めるまで false にする。
	void set_auto_narrow(bool on);
	// 折り返す位置の決め方 (既定は WrapBreaks::Qt)。Qt 以外では右の余白で狭めず (狭める分は折り返す位置に
	// 含まれる)、アクセシブル名に元のテキストを入れる。
	void set_breaks(WrapBreaks mode);
	WrapBreaks breaks() const { return breaks_; }

	// 箇条書きの記号 (空なら記号なし)。gap_px は記号とテキストの間隔で、テキストは記号の幅 + gap_px だけ右から
	// 始まる。記号はラベルの左の余白に描き、テキストは記号の右にそろえて折り返す。記号は描くだけで、テキストには
	// 入れない (スクリーンリーダーはテキストだけを読む)。記号を別のラベルにすると、名前のないラベルを Windows の
	// ナレーターが記号の文字で読み上げるため。
	void set_bullet(const QString &mark, int gap_px);
	// テキストの左の余白 (箇条書きの記号と間隔。記号がなければ 0)。
	int text_indent() const { return indent_; }

	// ラベルの幅が w のときに、テキストを折り返す幅 (左の余白を除いた幅を、最後の行の規則で狭めたもの)。
	int wrap_width_for(int w) const;
	// ラベルの幅が w のときに表示する行 (折り返す位置をラベルが決める形のとき。WrapBreaks::Qt なら、元のテキスト
	// 1 つだけ)。
	QStringList lines_for(int w) const;

	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override;
	// 狭めるための右の余白は、サイズの計算に入れない (入れると、幅、余白、求める幅、幅と変わり続ける)。
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void changeEvent(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	void update_margins();
	// 枠 (テーマの余白と枠線) の幅。QFrame が内側の余白として持つ分。
	QMargins frame_margins() const;
	void refresh_breaks();
	bool managed() const { return breaks_ != WrapBreaks::Qt; }
	void copy_selection() const;

	QString source_;
	WrapBreaks breaks_ = WrapBreaks::Qt;
	QString mark_;
	int gap_px_ = 0;
	int indent_ = 0;
	QMargins frame_; // 枠の幅 (update_margins が読む)
	int min_graphemes_ = DockStyle().min_last_line_graphemes;
	double min_ratio_ = DockStyle().min_last_line_ratio;
	double floor_ratio_ = DockStyle().wrap_narrow_floor_ratio;
	bool auto_narrow_ = true;

	// wrap_width_for の結果のキャッシュ (heightForWidth は 1 回のレイアウトで何度も呼ばれ、そのたびにテキストを
	// 何十回もレイアウトするため)。
	mutable int cache_w_ = -1;
	mutable int cache_result_ = 0;
	mutable QString cache_text_;
	mutable QString cache_font_;
	mutable int cache_indent_ = -1;
	// lines_for の結果のキャッシュ (同じ理由)。
	mutable int lines_cache_w_ = -1;
	mutable QStringList lines_cache_;
	mutable QString lines_cache_text_;
	mutable QString lines_cache_font_;
	mutable int lines_cache_indent_ = -1;
};

} // namespace cordmux::dock::ui

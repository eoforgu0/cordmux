// SPDX-License-Identifier: Apache-2.0

// ドックの状態タブと、参加者タブの最初の設定のカードに表示するメッセージ。レベル 1、2、3 を同じカードで扱う。
// NoticeList はメッセージのウィジェットを Notice::key をキーにして持ち、変わった値だけを書き込む。並びは View の順
// (メッセージのレベルの順で、同じレベルは表示した順。並べるのは ledger の NoticeTracker)。カードの追加と削除、並べ替えは
// 位置が動く変化なので、並びの保留の間は行わず、中身のテキストだけを書き換える (update の allow_moves)。

#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <QColor>
#include <QFrame>
#include <QPointer>
#include <QWidget>

#include "dock/dock_look.h"
#include "ledger/dock_view.h"

class QHBoxLayout;
class QLabel;
class QPushButton;
class QResizeEvent;
class QVBoxLayout;

namespace cordmux::dock {

namespace ui {
class MarkLabel;
class WrapLabel;
} // namespace ui

using NoticeButtonCallback = std::function<void(const ledger::dockview::ViewButton &)>;
// 場所の行の [フォルダーを開く] と [ファイルの場所を開く] が押されたときに呼び出す (場所の UTF-8 のテキストと場所の
// 種類)。開き方 (消えた場所はどこを開くか、ファイルを選ぶか) は、製品では DockPoll が platform の関数で決める
// (dock_poll.cpp の open_location_of)。設定しなければ何もしない (プレビューツールとテストはエクスプローラーを開かない)。
using FolderOpener = std::function<void(const std::string &path_utf8, ledger::dockview::LocationKind kind)>;

// メッセージのテキストのまとまり (見出しの全文、影響と次の操作、準備の段階、場所の行)。カードと行が使う。
class NoticeBody : public QWidget {
public:
	explicit NoticeBody(QWidget *parent = nullptr);
	// body_muted は、影響と次の操作の段落と 2 行目を、薄い色のテキスト (OBS のクラス text-muted) にするか。薄い色のテキストが
	// パネルの上で読めるとき (muted_ok) だけ true にする。
	void set_look(const DockLook &look, bool body_muted);
	// heading_visible は見出しを表示するか。
	void set_notice(const ledger::dockview::Notice &notice, bool heading_visible);
	void set_heading_visible(bool visible);
	// accessibleDescription とツールチップに入れるテキスト (影響と次の操作と場所)。
	QString description() const;
	// 場所の行の [フォルダーを開く] の幅 (表示していなければ 0)。ドックの最小幅に含める。
	int open_button_width_px() const;
	// 表示している [フォルダーを開く] に、現れた直後と動いた直後のクリックを捨てるガードをかけ直す。
	void arm_guards();
	// フォーカスを受けるウィジェット (場所の行のラベルと [フォルダーを開く]) を、表示の順 (上から) に返す。隠れている
	// ものも含む (Tab の順を並べ直す側が使う。Qt は隠れたウィジェットを Tab で飛ばす)。
	std::vector<QWidget *> focus_order() const;
	// [フォルダーを開く] が押されたときの呼び出し先。
	void set_folder_opener(FolderOpener opener) { opener_ = std::move(opener); }

private:
	// 本文の段落の間に置く場所の行 (Notice::body_locations) のウィジェット。
	struct InlineLocation {
		ui::WrapLabel *label = nullptr;
		QPushButton *open = nullptr;
		std::string path;
		ledger::dockview::LocationKind kind = ledger::dockview::LocationKind::Folder;
		bool open_wanted = false;
		std::size_t after = 0;
	};

	void rebuild_paragraphs(int count);
	void rebuild_inline_locations();
	// 段落、段落の間の場所の行、まとまりの間の空きを、View の順に並べ直す。
	void arrange_flow();
	void paint();
	void open_path(const std::string &path, ledger::dockview::LocationKind kind) const;
	// 本文のウィジェットの表示と非表示を、View が持つかで決める。フォーカスを持ったウィジェットを隠すときは、先にこの
	// ウィジェットにフォーカスを移す。
	void apply_parts_visible();

	DockLook look_;
	bool body_muted_ = false;
	bool open_wanted_ = false; // 本文の後の場所の行に開くボタンを置くか
	ledger::dockview::LocationKind open_kind_ = ledger::dockview::LocationKind::Folder;
	ledger::dockview::Notice notice_;
	bool has_notice_ = false;
	QVBoxLayout *layout_ = nullptr;
	// 見出し、段落、準備の段階の行、2 行目は、数と単位の語を行で分けず、最後の行の長さをならして折り返すラベル
	// (ui::WrapBreaks::KeepNumbers)。場所の行は区切りの後で折り返し、省かずに選んでコピーできるラベル
	// (ui::WrapBreaks::PathSeparators)。テキストは ui::WrapLabel::source_text で読む (text() は折り返しの行の区切りを
	// 含む)。
	ui::WrapLabel *heading_ = nullptr;
	// 影響と次の操作の段落と、段落の間の場所の行のコンテナー。まとまりが始まる段落の前に、まとまりの間隔を空ける。
	QWidget *flow_ = nullptr;
	QVBoxLayout *flow_layout_ = nullptr;
	std::vector<ui::WrapLabel *> paragraphs_;
	std::vector<InlineLocation> inline_;
	std::vector<std::size_t> arranged_starts_;
	std::vector<std::size_t> arranged_afters_;
	std::vector<bool> arranged_opens_;
	int arranged_gap_ = -1;
	int arranged_after_button_ = -1;
	int arranged_row_pad_ = -1;
	FolderOpener opener_;
	QWidget *stages_ = nullptr;
	ui::WrapLabel *location_ = nullptr;
	// 場所の行の [フォルダーを開く] (LocationLine::open_folder が true のときだけ表示する)。場所の行のすぐ下に左寄せで置く。
	QPushButton *open_ = nullptr;
	std::string open_path_;
	ui::WrapLabel *line2_ = nullptr;
};

// 操作のボタンの並び (テキストの幅のまま左寄せ。幅が足りなければ折り返す)。押せない理由は、ツールチップと、ボタンの
// 下の 1 文で表示する。
class NoticeButtons : public QWidget {
public:
	explicit NoticeButtons(QWidget *parent = nullptr);
	void set_look(const DockLook &look);
	void set_buttons(const std::vector<ledger::dockview::ViewButton> &buttons);
	// 処理中の操作 (押した後、結果まで 1 秒を超えることがあるもの)。そのボタンは押せなくし、テキストを処理中の形に
	// する。済んだら、フォーカスが Qt の移した先のままならボタンに戻す。
	void set_busy(const std::set<ledger::ActionId> &busy);
	void set_on_press(NoticeButtonCallback cb) { on_press_ = std::move(cb); }
	// 全部のボタンに、現れた直後と動いた直後のクリックを捨てるガードをかけ直す。
	void arm_guards();
	// 最も広いボタンの幅 (ドックの最小幅に使う)。
	int widest_button_px() const;
	std::vector<QPushButton *> buttons() const { return buttons_; }

protected:
	// ツールチップを持たないボタンの上で、親のツールチップを表示しない。ボタンのツールチップは、そのボタンのことだけにする。
	bool event(QEvent *event) override;

private:
	DockLook look_;
	std::vector<ledger::dockview::ViewButton> views_;
	bool set_once_ = false;
	std::vector<QPushButton *> buttons_;
	QWidget *row_ = nullptr;
	// 押せない理由。カードの段落と同じ折り返し方 (KeepNumbers) のラベル。
	ui::WrapLabel *reason_ = nullptr;
	NoticeButtonCallback on_press_;
	std::set<ledger::ActionId> busy_;
	QPointer<QWidget> focus_moved_to_; // 処理中にして押せなくしたときに、Qt がフォーカスを移した先
	QPointer<QPushButton> focus_owner_; // そのときにフォーカスを持っていたボタン

	void apply_views();
};

// メッセージのカード。レベル 1、2、3 で同じ形にする。
//   - カードの中身は、メッセージのレベルのアイコン、見出し (太字)、影響と次の操作 (折り返す。薄い色)、準備の段階の「名前: 値」、
//     場所の行 (開ける場所なら、その下に [フォルダーを開く])、2 行目 (録画ファイルの作成の進み具合)、操作のボタン (0 から
//     2 個。テキストの幅のまま、文の下に左寄せ)。
//   - 背景はパネルの色 (ui::panel_fill)。左端に 3 px の線を引く。レベル 1 は危険の色、レベル 2 は注意の色、レベル 3 は
//     線なし。テーマがその色を持たないときは本文の色で引く。枠線と赤い背景は使わない (赤は 1 行目だけ)。
//   - アクセシブル名は見出し、accessibleDescription は影響と次の操作にする。
// 操作のボタンは、通常はテキストの列 (メッセージのレベルのアイコンの右) から始め、カードの中の幅がその列から最も広い
// ボタンまでに足りないときだけ、カードの内側の左端 (アイコンの列の下) から始める。ボタンのテキストは省かない。
class NoticeCard : public QFrame {
public:
	explicit NoticeCard(QWidget *parent = nullptr);
	void set_look(const DockLook &look);
	void set_notice(const ledger::dockview::Notice &notice, bool heading_visible);
	void set_heading_visible(bool visible);
	void set_on_press(NoticeButtonCallback cb);
	void set_folder_opener(const FolderOpener &opener);
	void set_busy(const std::set<ledger::ActionId> &busy);
	void arm_guards();
	int widest_button_px() const;
	const ledger::dockview::Notice &notice() const { return notice_; }
	// 最小の幅は、ボタンをアイコンの列の下から始める形で返す (ドックの最小幅はこの形で数える)。
	QSize minimumSizeHint() const override;
	// ボタンをアイコンの列の下から始めているか (プレビューツールが数える)。
	bool buttons_under_mark() const { return buttons_under_mark_; }

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void apply_frame();
	void apply_mark();
	QColor bar_color() const;
	// 現在の幅で、ボタンをテキストの列から始めるか、アイコンの列の下から始めるかを決め、置き直す。
	void place_buttons();

	DockLook look_;
	ledger::dockview::Notice notice_;
	bool has_notice_ = false;
	ui::MarkLabel *mark_ = nullptr;
	QHBoxLayout *row_ = nullptr;     // アイコンとテキストの列
	QVBoxLayout *column_ = nullptr;  // テキストの列 (本文と、通常の位置のボタン)
	NoticeBody *body_ = nullptr;
	NoticeButtons *buttons_ = nullptr;
	bool buttons_under_mark_ = false;
};

class NoticeList : public QWidget {
public:
	explicit NoticeList(QWidget *parent = nullptr);
	void set_look(const DockLook &look);
	// notices は View の順のまま渡す。allow_moves はカードの追加と削除、並べ替えをしてよいか (並びの保留の間は false)。
	// 追加と削除か並べ替えを、並びの保留のためにためておいたときは true を返す。
	bool update(const std::vector<ledger::dockview::Notice> &notices, bool allow_moves);
	void set_on_press(NoticeButtonCallback cb);
	void set_folder_opener(FolderOpener opener);
	void set_busy(const std::set<ledger::ActionId> &busy);
	bool empty() const { return order_.empty(); }
	int widest_button_px() const;
	std::vector<NoticeCard *> cards() const;

private:
	NoticeCard *make_card(const ledger::dockview::Notice &notice);

	DockLook look_;
	bool has_look_ = false;
	QVBoxLayout *layout_ = nullptr;
	std::map<std::string, QPointer<NoticeCard>> by_key_;
	std::vector<std::string> order_;
	NoticeButtonCallback on_press_;
	FolderOpener opener_;
	std::set<ledger::ActionId> busy_;
};

} // namespace cordmux::dock

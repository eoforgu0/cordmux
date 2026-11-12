// SPDX-License-Identifier: Apache-2.0

// 参加者の設定ウィンドウ。通話に参加している人 (「現在の参加者」) と過去の参加者を 1 人 1 行で並べ、参加者ごとの値
// (自動で載せる、cordmux での名前、使う声、表示) を変える。値を変えると、すぐに反映する。
// ボタンは [閉じる] だけで、OBS の [オーディオの詳細プロパティ] と同じ形にする。モーダルにしないので、開いたまま
// OBS を操作できる。テキストは ini から読み、[閉じる] だけは OBS の ini (obs_frontend_get_locale_string) から読む。

#pragma once

#include <QDialog>
#include <QPointer>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ledger/actions.h"
#include "ledger/dock_view.h"

class QAbstractButton;
class QGridLayout;
class QLabel;
class QScrollArea;

namespace cordmux::dock {

namespace ui {
class FocusRing;
} // namespace ui

// 押された操作を呼び出し元に渡すコールバック。row_menu.h ほかにも同じ別名があり、どれを include しても同じ型になる。
using ActionCallback = std::function<void(ledger::ActionId, const ledger::ActionArgs &)>;

// 1 つの QGridLayout に、列の見出しの行、「現在の参加者」の見出しとその人の行、「過去の参加者」の見出しとその人の行を
// 縦に並べる。列が 1 つのグリッドなので、2 つのまとまりの列の幅はいつもそろい、どちらもウィンドウの幅まで伸びる。
// どのウィジェットも普通の子のウィジェットなので、Tab で順にフォーカスが移り、スクリーンリーダーに名前と説明が届く。
// QTableWidget のセルにウィジェットを置く形 (setCellWidget) にすると、2 つの表の列の幅がそろわない。セルの中の
// ウィジェットに Tab とスクリーンリーダーが届くかも確認していない。
// 値を変えると、ドックの行のメニューと同じ ActionId を on_action に渡す。押せるか、何をするかは ledger::apply が決める。
class ParticipantsWindow : public QDialog {
public:
	// 列。参加者ごとの値 (自動で載せる、cordmux での名前、使う声、通話のみ時ラベル非表示) の前に名前を置き、最後に最終参加日時
	// (過去の参加者は最後に通話にいた日時、現在の参加者はこの通話に参加した日時) を置く。列の見出しを押すと、その列で並べ替える。
	enum Column { ColName = 0, ColAutoAdd, ColRename, ColVoice, ColRosterHide, ColLastSeen, ColumnCount };

	ParticipantsWindow(QWidget *parent, ActionCallback on_action);
	~ParticipantsWindow() override;

	ParticipantsWindow(const ParticipantsWindow &) = delete;
	ParticipantsWindow &operator=(const ParticipantsWindow &) = delete;

	// 行を user_id をキーにして書き換える。並びは rows の順ではなく、このウィンドウの並べ替え (set_sort。既定は最終参加日時の
	// 新しい順) で決める。「現在の参加者」と「過去の参加者」の 2 つの組は残し、それぞれの組の中で並べる。行のウィジェットは
	// 作り直さず、テキスト、チェック、選択だけを書き換える。並びが変わったときもグリッドの中の位置だけを入れ替えるので、
	// フォーカスと入力途中のテキストを失わない。消える行にフォーカスがあれば、フォーカスをスクロール領域に移す。
	// 利用者が操作している行 (行のウィジェットにフォーカスがある間と、使う声の一覧を開いている間) は動かさない。その間に
	// 並びが変わる値を書き換えたときは、その行を元の位置のままにし、フォーカスが行から外れたときに並べ直す。
	void update_rows(const std::vector<ledger::dockview::ParticipantRow> &rows);

	// 並べ替えの列と向き。見出しを押すと、その列で並べ替え、同じ見出しをもう一度押すと逆の順にする。選んだ並びは、OBS を
	// 終了するまで覚える (ウィンドウを閉じて開き直しても同じ。設定のファイルには保存しない)。
	void set_sort(Column column, bool ascending);
	Column sort_column() const;
	bool sort_ascending() const;
	// 覚えている並びを既定 (最終参加日時の新しい順) に戻す (テストとプレビューツールが、場面ごとに初期状態にそろえる)。
	static void reset_remembered_sort();

	// 見出しのボタン (テストとプレビューツールが押す)。
	QAbstractButton *column_header(Column column) const;

	// ウィンドウを閉じたときに 1 回呼び出す (どの閉じ方でも)。geometry は QWidget::saveGeometry の base64 で、
	// participants_window_geometry に保存する値。
	void set_on_closed(std::function<void(const std::string &geometry)> on_closed);
	// 前に閉じたときの大きさと位置に戻す。空か戻せなければ、中身から決めた大きさにする (位置は親ウィンドウの中央)。
	void restore_window_geometry(const std::string &geometry);

	// OBS のテーマ、[フォントサイズ]、[密度] が変わったときに呼び出す。見出しのフォント、列の間隔、フォーカスリングを
	// 適用し直す。
	void refresh_theme();

	// ウィンドウを開いたウィジェット。ウィンドウがフォーカスを持ったまま閉じたら、フォーカスをここに戻す
	// (設定ウィンドウと同じ)。
	void set_return_focus(QWidget *opener);

protected:
	void done(int result) override;

private:
	struct Row; // 1 人の行のウィジェット

	Row *make_row(const ledger::dockview::ParticipantRow &row_data);
	void fill_row(Row &row, const ledger::dockview::ParticipantRow &row_data, bool first);
	void place_rows();
	// 現在の並べ替えで並べ直す。force が偽で、利用者が行を操作している間は、現在の並びを保つ (pending_resort_)。
	void reorder(bool force);
	std::vector<std::string> sorted_ids(bool in_call) const;
	bool user_is_engaged() const;
	void on_header_clicked(Column column);
	void refresh_sort_headers();
	void apply_heading_type();
	void apply_spacing();
	void refresh_focus_ring();
	void polish_rows() const;
	QSize initial_size() const;
	QSize minimum_window_size() const;

	ActionCallback on_action_;
	std::function<void(const std::string &)> on_closed_;
	bool closed_reported_ = false;
	ui::FocusRing *ring_ = nullptr;  // フォーカスリング
	QPointer<QWidget> return_focus_; // 閉じた後にフォーカスを戻すウィジェット (set_return_focus)

	QScrollArea *scroll_ = nullptr;
	QWidget *content_ = nullptr;
	QGridLayout *grid_ = nullptr;
	std::vector<QAbstractButton *> column_headers_;
	QLabel *current_heading_ = nullptr;
	QLabel *current_empty_ = nullptr; // 通話に参加している人が 0 人のときの 1 文
	QLabel *past_heading_ = nullptr;
	QLabel *past_empty_ = nullptr;

	std::map<std::string, Row *> rows_;  // user_id -> 行
	std::vector<std::string> order_;     // 現在の並び
	std::vector<bool> order_in_call_;    // order_ の同じ位置の人が「現在の参加者」か
	Column sort_column_ = ColLastSeen;
	bool sort_ascending_ = false;
	bool pending_resort_ = false;        // 行を操作している間に並びが変わる値が来て、並べ直しを待っている
};

// ウィンドウを作り、開かずに返す。プレビューツールとドックを操作するテストが、開かずに中身を調べるために使う。
ParticipantsWindow *make_participants_window(QWidget *parent, const std::vector<ledger::dockview::ParticipantRow> &rows,
					     ActionCallback on_action);

} // namespace cordmux::dock

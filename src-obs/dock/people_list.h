// SPDX-License-Identifier: Apache-2.0

// 参加者タブの一覧。メインの行、「画面共有中」の組、「通話中」の組の順に、1 人 1 行のウィジェット (person_row.h の
// PersonRowWidget) を縦に並べる。一覧が空のときは、一覧の場所に、空の理由か条件の 1 文とボタン (EmptyList) を表示する。
// スクロールしない上の行 (枠の数) は PeopleHeader である。DockWidget が参加者タブのページに置く。

#pragma once

#include <QPointer>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dock/dock_look.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/dock_style.h"
#include "dock/ui/theme_read.h"
#include "dock/person_row.h"
#include "dock/row_menu.h"
#include "ledger/actions.h"
#include "ledger/dock_view.h"

class QCheckBox;
class QLabel;
class QMenu;
class QPushButton;
class QVBoxLayout;

namespace cordmux::dock {

class NoticeList;

namespace ui {
class ElideLabel;
class WrapLabel;
} // namespace ui

// 押された操作を DockPoll に渡すコールバックの型 ActionCallback は row_menu.h にある。押せるかは ledger::apply() が
// 決める。

// 参加者タブのスクロールしない上の行。枠の数を置く。
class PeopleHeader : public QWidget {
public:
	explicit PeopleHeader(QWidget *parent = nullptr);
	void set_look(const DockLook &look);
	void set_slots(const QString &text);

private:
	QLabel *slots_ = nullptr;
};

// 行は user_id をキーにして持ち続け、更新のたびに作り直さない。前の値と同じ行は何もしない。
// 位置が動く変化 (行の追加と削除、組の間の移動、並べ替え、行の高さの変化、空の一覧の表示と非表示) は、
// 並びの保留 (set_layout_hold(true)) の間はためておき、解けた時点でまとめて反映する。行の中のテキスト、アイコン、
// チェックの状態は、並びの保留の間もその場で書き換える (行の高さを変えない書き換えだけ)。
// 並びの保留の間に通話から抜けた人の行は残したまま、その行のチェックと [...] を押せなくする。フォーカスが
// その行にあれば、押せなくする前に一覧そのもの (cordmuxPeopleList) に移す。フォーカスのある行を消すときも、
// 先に一覧そのものに移す。
// 行の矩形がウィンドウの上で動いたら、その行はダブルクリックの時間だけクリックを受け付けない。ウィンドウの座標で
// 見るので、一覧の中の並びの変化に加えて、ドックの上端の高さが変わってスクロール領域ごと動いたときと、自動の
// スクロール (フォーカスに合わせたスクロール、中身が縮んだときの詰め) も対象になる。利用者が自分でスクロールした
// 動きだけは除く (スクロールバーの actionTriggered で見分ける)。
// 狭いときに消す項目 (アバター、行の状態のテキスト、解像度) は、行が使える幅で決め、全部の行に同じ結果を適用する。
// テキストは ini のキー (ledger の TextRef) を dock/ui/text_resolve.h の関数で読み、コードで連結しない。
class PeopleList : public QWidget {
public:
	explicit PeopleList(QWidget *parent);
	~PeopleList() override;

	// 行を user_id をキーにして書き換える。並びはメインの行、画面共有中、通話中の順。位置が動く変化 (行の追加と削除、組の間の
	// 移動) は、並びの保留の間はためておき、set_layout_hold(false) で反映する。テキスト、アイコン、チェックはその場で
	// 書き換える。reason は枠が埋まっている理由で、一覧のアクセシブルな説明に使う (画面の文としては置かない)。
	void update(const ledger::dockview::PeopleListView &list, const std::optional<ledger::dockview::ListReason> &reason,
		    const std::optional<ledger::dockview::EmptyList> &empty);
	// 話している人 (配信に出ている声で話している人の user_id)。マイクのボタンのうち、値が変わったものだけを塗り直す。
	void set_speaking(const std::vector<std::string> &user_ids);
	// 参加者ごとのアバターの画像 (PNG。user_id -> バイト列)。バイト列のポインターが変わった人の分だけデコードする。
	using AvatarBytes = std::shared_ptr<const std::vector<std::uint8_t>>;
	void set_avatars(const std::map<std::string, AvatarBytes> &avatars);
	// 一覧の場所に置く最初の設定のメッセージの並び (ledger::setup_notices_of。行がない間だけ中身を持つ)。
	NoticeList *setup_notices() const { return setup_; }
	void set_layout_hold(bool held);
	bool has_pending_layout() const;
	void set_action_callback(ActionCallback cb);                                  // チェック、行のメニューの項目
	void set_rename_callback(std::function<void(const std::string &user_id)> cb); // 「名前を変更...」が押されたとき
	void set_menu_open_callback(std::function<void(bool open)> cb);               // 行のメニューを開く前と閉じた後
	void set_metrics(const ui::DockMetrics &metrics);                             // テーマとフォントの大きさが変わったとき
	void set_available_width_em(double em);                                       // 狭いときに消す項目を決める幅
	// 名前を残すために状態のテキストを縮めるときの下限 (em。既定は rows::kStatusTextKeepEm)。プレビューツールが値を
	// 変えて描くために使う。
	void set_status_keep_em(double em);
	// 1 行の最小幅 (rows::min_row_width にミュートのボタンの幅を足した値)。ドックの最小幅を状態で変えないので、行の中身によらない。
	int minimum_row_width_px() const;
	// 行の状態のテキストを表示する境目 (px。rows::status_text_min_width)。プレビューツールが読む。
	int status_text_min_width_px() const;

	// テーマの色とアイコン (薄い色のテキストを使えるか、画面共有のアイコン、[...] のアイコン) と見た目の値。DockWidget が
	// テーマの値を読み込み直したときに、set_metrics と一緒に呼び出す。呼び出されるまでは、このウィジェットが自分で読んだ色で
	// 描く (画面共有のアイコンは cordmux の形)。
	void set_theme(const ui::ThemeRead &theme, const ui::DockStyle &style);

	// プレビューツールとドックを操作するテストが読む関数。
	QWidget *list_widget() const;                                   // 一覧そのもの (cordmuxPeopleList)
	PersonRowWidget *row_for(const std::string &user_id) const;     // 今表示している行 (なければ nullptr)
	std::vector<std::string> shown_user_ids() const;                // 今表示している行の並び
	// 行のメニューを作る (開かない)。プレビューツールが「menu:row」の画面を描くために使う。製品でメニューを開かない
	// 行 (Vesktop のアカウントの行、通話から抜けた人の行) と、表示していない人では nullptr。
	QMenu *make_menu_for(const std::string &user_id, QWidget *parent);
	// 幅で消す項目の判定と名前の見分けを、今すぐやり直す。通常は大きさが変わった後に、イベントループの次の周回で
	// 自分でやり直す。プレビューツールが画像を描画する前に呼び出す。
	void refit_now();

protected:
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;
	void showEvent(QShowEvent *event) override;

private:
	struct Pending {
		ledger::dockview::PeopleListView list;
		std::vector<ledger::dockview::PersonRow> rows; // 表示する行を、並びの順に並べたもの
		std::optional<ledger::dockview::ListReason> reason;
		std::optional<ledger::dockview::EmptyList> empty;
	};

	void ensure_env();
	RowEnv make_env();
	void apply_structure();
	void apply_in_place();
	void flatten_latest();
	std::vector<QWidget *> wanted_widgets() const;
	void apply_headings();
	void apply_empty(const std::optional<ledger::dockview::EmptyList> &empty);
	void update_empty_content(const ledger::dockview::EmptyList &empty);
	void update_empty_gap();
	PersonRowWidget *make_row(const std::string &user_id);
	void remove_row(const std::string &user_id);
	void move_focus_off(QWidget *row);
	void fix_tab_order();
	void schedule_refit();
	void refit();
	void elide_names();
	void update_list_accessibility();
	const ledger::dockview::PersonRow *latest_row(const std::string &user_id) const;
	void open_row_menu(const std::string &user_id, QWidget *anchor, const QPoint &global_pos, bool keyboard);
	void open_replace_menu(const std::string &user_id, QWidget *anchor);
	// 行のメニューと「入れ替える相手」のメニューを開く共通の処理。make_menu は、押された操作をメニューを閉じてから実行する
	// コールバックを受けて、メニューを作る。
	void run_menu(const std::string &user_id, const QPoint &at,
		      const std::function<QMenu *(const ActionCallback &, const std::function<void(const std::string &)> &)> &make_menu);
	QPixmap avatar_for(const std::string &user_id);
	void track_ancestors();
	void note_positions(bool mark_moved);
	void note_user_scroll();
	void end_user_scroll();
	double width_em() const;
	bool structure_differs() const;

	ActionCallback on_action_;
	std::function<void(const std::string &)> on_rename_;
	std::function<void(bool)> on_menu_open_;

	ui::DockMetrics metrics_;
	ui::ThemeRead theme_;
	ui::DockStyle style_;
	bool has_theme_ = false;
	bool has_metrics_ = false;
	bool env_ready_ = false;
	RowEnv env_;
	double available_em_ = 0.0; // 0 以下 = 自分の幅から計算する
	double status_keep_em_ = rows::kStatusTextKeepEm;
	mutable int min_row_width_ = -1;

	QVBoxLayout *outer_ = nullptr;
	QWidget *list_ = nullptr;                // cordmuxPeopleList
	QVBoxLayout *list_layout_ = nullptr;
	QLabel *sharing_heading_ = nullptr;      // cordmuxGroupSharing
	QLabel *in_call_heading_ = nullptr;      // cordmuxGroupInCall
	NoticeList *setup_ = nullptr;            // 最初の設定のメッセージ
	QWidget *empty_ = nullptr;               // cordmuxEmptyState
	ui::WrapLabel *empty_text_ = nullptr;    // cordmuxEmptyText
	QPushButton *empty_button_ = nullptr;
	std::optional<ledger::dockview::ViewButton> empty_action_;

	// 最後に受け取った値 (並びの保留の間にためておく変化の元) と、現在の画面の並びに反映している値。
	Pending latest_;
	bool has_latest_ = false;
	std::vector<std::string> shown_order_;
	bool empty_shown_ = false;
	std::map<std::string, AvatarBytes> avatars_;
	std::map<std::string, QPixmap> avatar_images_;
	std::map<std::string, PersonRowWidget *> rows_;
	bool held_ = false;
	bool refit_scheduled_ = false;
	bool refitting_ = false;

	// 開いている行のメニュー (同時に 1 つ)。
	QPointer<QMenu> open_menu_;
	std::string open_menu_user_;

	// 行の矩形 (ウィンドウの座標。押した点にいる人が替わる動きを全部見る) と、それを動かすことがある祖先のウィジェット
	// (ウィンドウの手前まで。スクロール領域の中身、viewport、スクロール領域そのものを含む)。
	std::map<std::string, QRect> positions_;
	std::vector<QPointer<QWidget>> tracked_ancestors_;
	// 利用者のスクロール (ホイール、スクロールバー、キー) で中身が動いている最中か。スクロールバーの actionTriggered で
	// true にし、値が変わり終えたとき (valueChanged) か、同じイベントループの周回が終わったときに false にする。自動の
	// スクロール (フォーカスに合わせたスクロール、中身が縮んだときの詰め) は actionTriggered を出さないので、ガードの
	// 対象になる。
	bool user_scrolling_ = false;
	std::vector<QMetaObject::Connection> scroll_connections_;
};

} // namespace cordmux::dock

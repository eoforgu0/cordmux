// SPDX-License-Identifier: Apache-2.0

// ドックのウィジェット。上から 1 行目、メインとサブの行、タブ、タブの中身、下のバーを持つ (構成は DockWidget のコメントにある)。
// 判断はせず、ledger の DockViewV2 (ledger::stabilize の後の値) を描くだけである。

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QPointer>
#include <QWidget>

#include "config/settings.h"
#include "dock/dock_look.h"
#include "ledger/actions.h"
#include "ledger/dock_view.h"
#include "ledger/inline_confirm.h"
#include "ledger/notice_tracker.h"
#include "ledger/view.h"
#include "pipeline/audio_mixer.h"
#include "sources/self_speaking_judge.h"

class QFrame;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace cordmux::dock {

class AccountsBar;
class AudioTab;
class BottomBar;
class DockPage;
class DockTabBar;
class FirstLineBar;
class InlineConfirm;
class LayoutHold;
class LookTab;
class NoticeList;
class StatusTab;
class PeopleHeader;
class PeopleList;

namespace ui {
class FocusRing;
} // namespace ui

// 操作が押されたときに呼ばれる。実際の処理は DockPoll が行う。
using ActionCallback = std::function<void(ledger::ActionId, const ledger::ActionArgs &)>;

// ドックのウィジェット。上から次のものを持つ。
//   1. 1 行目 (first_line.*): 押すと状態タブを開く。高さは一定で、メッセージのレベル 1 のときは 1 行目の背景を危険の色で塗る。
//   2. メインとサブの行 (accounts_bar.*)。
//   3. タブのバーとタブの中身 (dock_tabs.*)。[参加者] [音声] [表示] [状態] の 4 つで、スクロールするのはタブの中身だけである。
//   4. 下のバー (bottom_bar.*): 録画のボタンとドックのメニュー。タブの中身との間に区切り線を引く。
//
// 1 行目、メインとサブの行、タブ、下のバーの高さと位置は、メッセージや外部の出来事で動かさない。押し間違いは配信に直接出るためで
// ある。メインとサブの行の数が変わるのは、[変更] を押したときだけである。
//
// 判断はしない。render() は View を描くだけで、押せるかは View の disabled_reason の有無を見るだけである。押されたら、
// コールバックに ActionId と引数を渡す (押してよいかは ledger::apply() が判断し直す)。View を作るのも、
// ポーリングをするのも DockPoll である。
//
// 描き直しでウィジェットを作り直さない。前の View と比べ、変わった値だけを今あるウィジェットに書き込む。タブの中で位置が動く変化
// (カードの追加と削除、行の組の間の移動) は、並びの保留の間 (layout_hold.*) はためておき、終わったら反映する。
//
// 見た目はテーマから読む。テーマ、[フォントサイズ]、[密度] が変わったら、on_theme_changed() で読み直す
// (OBS_FRONTEND_EVENT_THEME_CHANGED は DockPoll::on_theme_changed から来る。ウィジェットの StyleChange でも読み直す)。
//
// 最小幅は中身から計算し、状態によらず固定する (下のバー、参加者の一覧の 1 行、音声タブの 1 人、メインとサブの行、タブ、1 行目のうち、
// いちばん広いもの)。
// 状態タブのメッセージは含めない。カードの文は折り返し、ボタンは折り返して並ぶ。
class DockWidget : public QWidget {
public:
	explicit DockWidget(QWidget *parent = nullptr);
	~DockWidget() override;

	// 操作が押されたときのコールバック (nullptr で外す)。
	void set_action_callback(ActionCallback callback);

	// 行のメニューの「名前を変更...」を押したときのコールバック。設定しなければ、ドックが「名前を変更」の
	// ダイアログを開き、入力されたテキストで SetNameOverride を呼ぶ (rename_dialog.*)。プレビューツールとドックを
	// 操作するテストが、ダイアログを開かずに受け取るために使う。
	void set_rename_callback(std::function<void(const std::string &user_id)> callback);

	// メッセージの場所の行の [フォルダーを開く] か [ファイルの場所を開く] を押したときのコールバック (製品では、
	// DockPoll が platform/ の関数で開く)。設定しなければ、押しても何もしない (プレビューツールとドックを操作する
	// テストは、エクスプローラーを開かない)。
	void set_open_folder_callback(
		std::function<void(const std::string &path_utf8, ledger::dockview::LocationKind kind)> callback);

	// View (ledger::stabilize の後の値) を描く。1 行目は ledger::settle_first_line の後の値にしておく。
	void render(const ledger::dockview::DockViewV2 &view);

	// 状態タブの名前 (ledger::FirstLineResult::status_tab_label)。タブの幅は変わらない。
	void set_status_tab_label(const ledger::TextRef &label);

	// 開いているタブ。set_tab はコールバックも呼ぶ。
	config::DockTab tab() const;
	void set_tab(config::DockTab tab);
	// 利用者か 1 行目の操作でタブが替わったときに呼ぶ (設定 dock_tab の保存に使う)。
	void set_on_tab_changed(std::function<void(config::DockTab)> callback) { on_tab_changed_ = std::move(callback); }
	// 状態タブを選んでいて、ドックが見えているか (ledger::FirstLineInput::status_tab_open)。
	bool status_tab_open() const;

	// 確認に使う値 (ledger::confirm_needed の入力のうち、View にないもの)。周期ごとに渡す。
	void set_confirm_inputs(bool streaming, bool confirm_while_streaming);

	// 並びの保留で、ポインターが止まってから反映するまでの時間 (ms。設定 layout_hold_idle_ms)。
	void set_layout_hold_idle_ms(int ms);

	// ---- 表示タブ (look_tab.h) ----
	// 設定の値を表示タブの入力欄へ反映する。周期ごとに渡してよい。操作している入力欄は書き換えない。
	void set_look_values(const config::Settings &settings);
	// 表示タブで値を変えたときに呼ぶ。引数の設定は表示タブが持つ現在の値で、DockTab の行だけを読む。値を動かしている間は
	// 間引いて呼ぶ。
	void set_look_changed_callback(std::function<void(const config::Settings &)> callback);
	// 表示タブのソースの候補を集める関数 (自分の枠に映す映像、自分の枠の音声ソース)。
	void set_look_source_providers(std::function<std::vector<sources::SelfSourceCandidate>()> frame,
				       std::function<std::vector<sources::SelfSourceCandidate>()> speaking);
	// 配信の画面の大きさ (フォントサイズのツールチップ)。
	void set_canvas_size(int width, int height);
	// 表示タブと状態タブで変えたまま渡していない値を、今渡す (OBS を閉じるとき)。
	void flush_look();
	LookTab *look_tab() const { return look_tab_; }

	// ---- 状態タブ (status_tab.h) ----
	// 映像のバッファを変えたときに呼ぶ。引数は入力欄の値 (ms)。値を動かしている間は間引いて呼ぶ。値は表示タブと同じ規則で
	// 外から届いた値に追従する (set_look_values が渡す)。
	void set_buffer_changed_callback(std::function<void(int)> callback);
	StatusTab *status_tab() const { return status_; }

	// 参加者ごとの声の大きさを読む周期を動かしてよいかが変わったときに呼ぶ。参加者タブと音声タブが見えている間だけ true で渡す
	// (ほかのタブが見えている間は読まない)。
	void set_on_levels_wanted(std::function<void(bool)> callback);
	bool levels_wanted() const;
	// 話している人 (Pipeline::audio_levels の speaking)。マイクのボタンのうち、値が変わったものだけを塗り直す。一覧を作り直さない
	// ので、1 秒に 10 回呼んでよい。
	void set_speaking(const std::vector<std::string> &user_ids);
	// 組ごとの音の大きさ (Pipeline::audio_levels の tracks)。音声タブのメーターのうち、値が変わったものだけを塗り直す。音声タブが
	// 見えていない間は何もしない。1 秒に 10 回呼んでよい。
	void set_audio_levels(const std::vector<pipeline::AudioTrackLevel> &tracks);
	// 参加者ごとのアバターの画像 (PNG のバイト列)。周期ごとに渡してよい。バイト列のポインターが変わった人だけデコードする。
	void set_avatars(const std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> &avatars);

	// 処理中の操作 (押した後、結果まで 1 秒を超えることがあるもの)。その操作のボタンを押せなくし、テキストを処理中の
	// 形にする。DockPoll が、DockPollInstallState から渡す。
	void set_busy_actions(const std::set<ledger::ActionId> &busy);

	// 状態の変化を、スクリーンリーダーの読み上げに渡す。assertive は、メッセージのレベル 1 が表示されたときに
	// true にする。フォーカスは動かさない。QAccessible の Alert のイベントは出さない。
	void announce(const QString &text, bool assertive);

	// OBS のテーマ、[フォントサイズ]、[密度] が変わったときに呼ぶ (OBS_FRONTEND_EVENT_THEME_CHANGED)。色、
	// アイコン、大きさを読み直して、描き直す。
	void on_theme_changed();

	// 見た目の設計の値 (DockStyle) を変える。プレビューツールが、別の値の組み合わせで描画するために使う。
	void set_style(const ui::DockStyle &style);

	// 計算した最小幅 (setMinimumWidth に設定した値)。
	int computed_min_width() const { return min_width_; }

	// 並びの保留中か (ドックを操作するテストとプレビューツールが見る)。
	bool layout_held() const;

	// スクロールの位置を変える (0 は先頭、1 は中ほど、2 は最後)。プレビューツールが描画するために使う。
	void set_scroll_position(int position);

	// 現在の大きさで、配置と省略を決め直す (プレビューツールが大きさを変えた後に呼ぶ。製品では resizeEvent が呼ぶ)。
	void refresh_layout();

protected:
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void read_look();
	void apply_look();
	void apply(bool allow_moves);
	void update_min_size();
	void on_hold_changed(bool held);
	void on_tab_index_changed(int index);
	void report_levels_wanted();
	void open_dock_menu();
	void open_main_account_menu(QWidget *anchor);
	void press(const ledger::dockview::ViewButton &button);
	void press_action(ledger::ActionId id, const ledger::ActionArgs &args);
	std::optional<ledger::ConfirmPlan> confirm_plan_for(ledger::ActionId id) const;
	void start_confirm(const ledger::ConfirmPlan &plan, ledger::ActionId id, const ledger::ActionArgs &args);
	bool confirm_still_valid() const;
	void rename(const std::string &user_id);
	QScrollArea *current_scroll() const;

	DockLook look_;
	bool look_ready_ = false;
	bool rereading_ = false;
	// on_theme_changed がスタイルを適用し直している途中か。途中に自分が起こした変化のイベントでは、読み直さない。
	bool refreshing_ = false;
	bool shown_once_ = false;

	ledger::dockview::DockViewV2 view_;
	bool has_view_ = false;
	bool pending_moves_ = false;

	QVBoxLayout *root_ = nullptr;
	FirstLineBar *first_line_ = nullptr;
	AccountsBar *accounts_ = nullptr;
	DockTabBar *tab_bar_ = nullptr;
	QStackedWidget *pages_ = nullptr;
	DockPage *people_page_ = nullptr;
	DockPage *audio_page_ = nullptr;
	DockPage *display_page_ = nullptr;
	DockPage *status_page_ = nullptr;
	PeopleHeader *people_header_ = nullptr;
	PeopleList *people_ = nullptr;
	QWidget *people_content_ = nullptr;
	QVBoxLayout *people_content_layout_ = nullptr;
	AudioTab *audio_ = nullptr;
	QWidget *audio_content_ = nullptr;
	QVBoxLayout *audio_content_layout_ = nullptr;
	LookTab *look_tab_ = nullptr;
	QWidget *look_content_ = nullptr;
	QVBoxLayout *look_content_layout_ = nullptr;
	QWidget *status_content_ = nullptr;
	QVBoxLayout *status_content_layout_ = nullptr;
	StatusTab *status_ = nullptr;
	QFrame *separator_ = nullptr; // タブの中身と下のバーの間の区切り線
	BottomBar *bottom_ = nullptr;
	InlineConfirm *confirm_ = nullptr;
	std::optional<ledger::ConfirmPlan> confirm_plan_;
	ledger::ActionId confirm_action_ = ledger::ActionId::ExportBundle;

	LayoutHold *hold_ = nullptr;
	// フォーカスに合わせてスクロールするイベントフィルター (dock_widget.cpp の FocusFollow)。
	QObject *focus_follow_ = nullptr;
	ui::FocusRing *ring_ = nullptr;

	int min_width_ = 0;
	int last_bottom_min_ = -1;
	bool streaming_ = false;
	bool confirm_while_streaming_ = true;
	bool levels_wanted_ = false;
	ActionCallback action_cb_;
	std::function<void(const std::string &)> rename_cb_;
	std::function<void(config::DockTab)> on_tab_changed_;
	std::function<void(bool)> on_levels_wanted_;
};

// 読み上げに渡す 1 つのメッセージ。
struct Announcement {
	QString text;
	bool assertive = false;
};

// メッセージの出入りの差分 (ledger::stabilize) と、その後の 1 行目から、読み上げに渡すメッセージを並べる。製品の DockPoll と
// ドックを操作するテスト (dock_drive の SimPoll) が、同じ決め方を使うための純粋関数である (DockWidget::announce で
// 渡す)。
//   - メッセージのレベル 1 が表示された: 見出しを、割り込み (assertive) で渡す。メッセージのレベル 2 かメッセージのレベル 3 が
//     表示された: 見出しを、割り込まずに渡す。人の行の状態の出入りは渡さない。
//   - メッセージのレベル 1 が消えた: 消えた見出しではなく、消えた後の 1 行目の全文 (平常の要約か、次に上に来たメッセージの
//     見出し) を、割り込まずに 1 回渡す (同じ差分でメッセージのレベル 1 がいくつ消えても 1 回)。同じ差分で表示された
//     メッセージの見出しと同じテキストなら渡さない (表示されたほうで、もう渡している)。消えた見出しをもう一度渡すと、
//     直ったことと、また起きたことを聞き分けられない。「解消しました」のような新しいテキストにすると、何が直ったかを
//     伝えるには、見出しごとにテキストが必要になる。
//   - 枠がすべて使用中になったか、空いた: 一覧が前から表示されていたときだけ、割り込まずに渡す (list_was_shown)。
std::vector<Announcement> announcements_for(const ledger::NoticeDiff &diff,
					    const ledger::dockview::FirstLine &first_line, bool list_was_shown);

} // namespace cordmux::dock

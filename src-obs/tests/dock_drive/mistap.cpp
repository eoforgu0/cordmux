// SPDX-License-Identifier: Apache-2.0

// mistap.h の実装。数えるものと数え方は、Mistap クラスのコメントにある。

#include "mistap.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QLayout>
#include <QtWidgets/private/qlayout_p.h>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleHints>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/notice_list.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/ui/text_resolve.h"
#include "input.h"
#include "obs_dock.h"
#include "query.h"
#include "sim_poll.h"

std::int64_t drive_log_lines(); // main.cpp (製品の diag::logf に渡された行の数)

// Qt が、最後にボタンを押すイベントを受けたウィジェットとして覚えているもの (qapplication.cpp。
// Qt6Widgets がエクスポートしている)。ボタンを押すときにこれが残っていると、Qt はイベントをポインターの下の
// ウィジェットではなく、このウィジェットへ送る (QApplicationPrivate::pickMouseReceiver)。(a) の原因を分けるために、
// 押す前の値を読むだけで、書き換えない。
extern Q_DECL_IMPORT QWidget *qt_button_down;

namespace cordmux::dock_drive {

namespace {

using ledger::ActionId;
using ledger::PersonFact;

const QChar kEllipsis(0x2026);

// テスト用の ID (18 桁。tests/ui_preview/fixtures.cpp の fake_id と同じ形。実在の ID は書かない)。
std::string fake_id(int n)
{
	std::string digits = std::to_string(n);
	return std::string(18 - digits.size() - 1, '0').insert(0, "1") + digits;
}

// テスト用の名前 (短い英字、空白を含むもの、絵文字を含むもの、長い日本語、区切りのない長い英字)。
const char *const kNames[] = {"aoi",       "Yuki Mori",  "みなと",     "Kaito ⭐",   "はるか🎵",
			      "ryo_dev",   "なつめ",     "Sora Kimura", "とても長い名前のテスト参加者さんです",
			      "LongNameWithoutSpaces_Example_2026",       "mio",        "Ren2",
			      "こはく",     "Nagi",       "ひかり✨",    "taiga",       "あおい",     "Aoi"};

enum class Dispatch { None, PointerRow, PointerMenu, PointerOther, Key };

struct Step {
	int delay_ms = 0;
	std::function<void()> fn;
};

struct PressCtx {
	std::int64_t t_ms = 0;
	QPoint pos;
	std::string under_user;
	QString under_part;
	// ポインターの下の行の左上が最後に動いてからの時間 (製品のガードと同じ見方。-1 は記録なし)
	std::int64_t moved_age_ms = -1;
	std::int64_t under_changed_age_ms = -1; // 押した位置の行が入れ替わってからの時間 (-1 は 500 ms 以内の入れ替わりなし)
	std::string under_before;               // 入れ替わる前にその位置にあった行 (空なら行がなかった)
	// 入れ替わりの原因を分けるための記録 (見つけた押し間違いの 1 行に書く):
	std::int64_t viewport_moved_age_ms = -1; // スクロール領域の表示範囲 (viewport) が画面の上で動いてからの時間
	std::int64_t content_moved_age_ms = -1;  // その行のスクロール領域の中の位置が変わってからの時間 (ガードが見る動き)
	// ボタンを押すイベントを実際に受けたウィジェットの行の人 (Qt が送った先。ポインターの下のウィジェットと違えば、
	// テストの入力の状態か Qt のマウスグラブが食い違っている) と、押す前のボタンの状態 (テストの Input と
	// Qt の両方。0 でなければ、前に押したボタンを離すイベントが届いていない)。
	std::string receiver_user;
	QString receiver_part;
	int buttons_before_input = 0;
	int buttons_before_qt = 0;
	// 押す前に Qt が覚えていた、ボタンを押すイベントを受けたウィジェット (qt_button_down) の行の人とウィジェット。
	// 空なら覚えていない (通常はこちら)。
	std::string button_down_user;
	QString button_down_part;
	bool button_down_set = false;
	// 押す前にアクティブだったウィンドウがドックか、押す前とイベントがウィジェットに届いた時点のスクロールの位置。
	// アクティブになったときに、フォーカスのあるウィジェットへスクロールすると、イベントが届く時点でポインターの
	// 下の人が替わる。
	bool dock_active_before = false;
	int scroll_before = 0;
	int scroll_at_delivery = 0;
	std::string under_at_delivery; // イベントがウィジェットに届いた時点でポインターの下にあった行の人
	bool held = false;
	// (b) の 1 回が、製品の不具合か、テストの測定の境界のずれかを分けるための記録。時刻はすべてこの
	// テストの時計の値。
	//   - change_lo_ms: 押した位置の入れ替わりが起こりえた最も早い時刻。入れ替わりを見つけた測定の
	//     1 つ前の測定の時刻と、その後に最初に届いた位置のイベントの時刻の、遅いほう。製品の行が変わった時刻
	//     (changed_at_ms_) は同じイベントの中で付くので、この時刻より前にはない。
	//   - change_seen_ms: 入れ替わりを見つけた測定の時刻 (under_changed_age_ms の元。遅い側)。
	//   - t_delivery_ms: イベントがウィジェットに届いた時刻 (製品のガードがクリックを捨てるかを答える時点)。
	//   - product_age_ms: 同じ時点の、製品の行が変わった時刻 (PersonRowWidget の changed_at_ms_。クリックを捨てる
	//     ガードが使う) からの経過。-1 は時刻がないか 65 秒より前、-2 は測っていない。
	std::int64_t change_lo_ms = -1;
	std::int64_t change_seen_ms = -1;
	std::int64_t t_delivery_ms = -1;
	std::string guard_user; // ガードを読んだ行の人 (イベントが届いたウィジェットの行。なければポインターの下の行)
	std::int64_t product_age_ms = -2;
	bool product_blocked = false; // 同じ時点で製品のガードがクリックを捨てると答えたか (現在のダブルクリックの時間で)
	int guard_row = -1;           // ガードの答えの記録 (guard_rows_) の行の番号。-1 は記録していない
};

// 製品の行が変わった時刻 (PersonRowWidget の changed_at_ms_) からの経過を、今この時点の製品の答え
// (PersonRowWidget::press_blocked_now) から求める。changed_at_ms_ は外から読めない。クリックを捨てるガードは
// 「経過 < ダブルクリックの時間」のときにクリックを捨てる (dock/name_check.cpp の press_blocked) ので、Qt の
// styleHints のダブルクリックの時間を変えながら、捨てるようになる最小の時間を二分探索で探す (最小の時間は
// 経過 + 1)。探し終えたら元の時間に戻す。ボタンを押すイベントを配る途中 (Qt のダブルクリックの判定は配る前に
// 済んでいる) で、ウィジェットの mousePressEvent の直前に呼ぶ。2^16 ms (65 秒) でも捨てなければ -1 (時刻がないか、
// それより前) を返す。次の方法はとらない。
//   - 製品に経過を返す関数を加える。テストのために製品の API を広げることになる。
//   - 製品が changed_at_ms_ を付ける処理 (dock/people_list.cpp の note_positions) をテストの中で再現して時刻を取る。
//     再現が製品とずれても気付けず、読みたいのは製品そのものの時刻である。
std::int64_t product_guard_age(const dock::PersonRowWidget *row)
{
	if (row == nullptr)
		return -1;
	QStyleHints *hints = QGuiApplication::styleHints();
	const int keep = hints->mouseDoubleClickInterval();
	const int cap = 1 << 16;
	std::int64_t age = -1;
	hints->setMouseDoubleClickInterval(cap);
	if (row->press_blocked_now()) {
		int lo = 1;
		int hi = cap; // hi では捨てる
		while (lo < hi) {
			const int mid = lo + (hi - lo) / 2;
			hints->setMouseDoubleClickInterval(mid);
			if (row->press_blocked_now())
				hi = mid;
			else
				lo = mid + 1;
		}
		age = lo - 1;
	}
	hints->setMouseDoubleClickInterval(keep);
	return age;
}

// 製品の経過と、テストで測った入れ替わりの範囲を照合するときの許容差 (ms)。2 つの時計 (このテストの
// QElapsedTimer と製品の steady_clock) はどちらも ms に切り捨てるので、同じ瞬間の差は 1 ms まで。両側の分で 2 にする。
constexpr std::int64_t kClockSlackMs = 2;

// (b) で見る時間 (500 ms)。Windows の既定のダブルクリックの時間で、dock_drive の --dblclick-ms の既定値と同じ。
// 不具合か境界のずれかは、現在のダブルクリックの時間ではなく、この値で判定する。--dblclick-ms 0 でガードを止めた
// セルフテストでも、入れ替わりから 500 ms 未満で動作したクリックを、不具合に数えるためである。
constexpr std::int64_t kGuardWindowMs = 500;

// 押した位置の行が (テストの測定で) 500 ms 以内に入れ替わっていて、クリックが動作したときの判定。空を返せば境界のずれ
// (記録だけ)、空でなければ製品の不具合 ((b) に数える) で、その原因を返す。
//   - 境界のずれ: クリックが届いた時点の、製品の行が変わった時刻 (changed_at_ms_) からの経過が 500 ms 以上で、
//     その時刻が、入れ替わりが起こりえた範囲の中 (最も早い時刻から許容差を引いた時刻以降) にある。製品は同じ
//     入れ替わりで時刻を付け、その時から時間がたっていた。テストの 500 ms は、入れ替わりを見つけた測定 (イベントの
//     後のイベントループの周回) と押す前の時刻で数えるので、イベントの処理が遅れると、製品の経過より短くなる
//     (495 ms になった例がある)。
//   - 不具合: 時刻がない (no-mark)、時刻からの経過が 500 ms より短いのに適用された (ran-within-window。ガードが
//     捨てなかった)、時刻が入れ替わりより前 (mark-before-change。製品がこの入れ替わりを見落とした)、測れなかった
//     (not-measured)。
// 製品の経過が 500 ms 未満のときだけを不具合にする方法はとらない。製品が入れ替わりを見落とすと、経過は古い時刻から
// 数えて長くなり、不具合が境界のずれに紛れるためである。
QString b_reason(const PressCtx &p, std::int64_t window_ms)
{
	if (p.product_age_ms == -2 || p.t_delivery_ms < 0)
		return QStringLiteral("not-measured");
	if (p.product_age_ms < 0)
		return QStringLiteral("no-mark");
	// 読んだ直後から、ウィジェットのガードが答えるまでに時計が 1 ms 進むことがあるので、1 ms 足りないだけなら通す
	// 答えとして扱う。
	if (p.product_age_ms + 1 < window_ms)
		return QStringLiteral("ran-within-window");
	const std::int64_t mark_t = p.t_delivery_ms - p.product_age_ms;
	if (mark_t + kClockSlackMs < p.change_lo_ms)
		return QStringLiteral("mark-before-change");
	return QString();
}

// mistap のシナリオ。テストデータ people-8 を元に、ポインターの移動、クリック、メニューの開閉、キーの入力と、
// 人の出入り、名前の変更、画面共有の開始と終了、メッセージの表示と消去を、時刻をランダムにずらして重ね、次の数を数える。
// どれも、すべてのシードで 0 が合格である。
//   (a) クリックが、押した瞬間にポインターの下にあった行の人以外に適用された回数。行のメニューの項目は、
//       メニューを開いた人以外に適用された回数。
//   (b) 押した位置の行が、押す前の 500 ms の間に入れ替わっていて (一覧の並び、行の高さ、上端の高さが変わった、
//       行が現れた、消えた)、そのクリックが動作した回数。利用者がホイールでスクロールした分は、行の動きに数えない。
//   (c) フォーカスのある人が通話から抜けた後のキーが、別の人に適用された回数。あわせて、フォーカスのある行の人以外に適用された
//       キーの数も数える。
//   (d) 1 行目の全文が View の 1 行目と違う状態か、表示されているテキストが View の 1 行目の候補 (長さの違う形) の
//       どれでもない状態が、500 ms を超えた回数。
//   (e) 並びの保留が解けている間に、行の並びかメッセージのカードの並びが View と違う状態が、500 ms を超えた回数。
// あわせて、状態を変えない区間に、OBS のログに書かれるはずの行 (製品の diag::logf の行と、メッセージの表示と消去の
// 1 つに 1 行) がないことを確認する。状態を変えない区間は 25 秒で、区間が終わるたびに 60 秒の間を空けて繰り返し、
// その間は Observation を書き換えず、状態を変えない入力だけを流す。行を数えるのは、区間の始まりか最後の書き換えから
// settle_ms_ (映像が止まったとみなすまでの時間、最初のフレームを待つ時間、メッセージを表示するまでと消すまでの待ち、
// 1 秒の和) がたった後だけである。
// 「動作した」は、操作を ledger::apply() が押せると答えたこと (製品の DockPoll が実行する操作) を指す。メッセージのボタン
// から、メッセージが指す人に適用された操作は、(a) と (c) に数えない。(b) の行の位置は、
// イベントの後と押す直前と 50 ms ごとに測る。500 ms 以内に入れ替わっていて動作したクリックは、製品の行が変わった
// 時刻からの経過 (product_guard_age) と照合し、b_reason で分ける。(d) と (e) は 50 ms ごとに測る。
class Mistap {
public:
	Mistap(const MistapOptions &opt)
		: opt_(opt), sim_(ui_preview::fixture_by_id("people-8"), opt.style), rng_(static_cast<unsigned>(opt.seed))
	{
	}

	void run(Results &results);

private:
	// ---- 時計 ----
	std::int64_t now() const { return clock_.elapsed(); }
	int rand_int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
	double rand01() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
	bool chance(double p) { return rand01() < p; }

	// ---- ウィジェット ----
	QRect dock_rect() const { return global_rect(sim_.dock()); }
	QRect viewport_rect() const
	{
		auto *scroll = sim_.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
		return scroll != nullptr ? global_rect(scroll->viewport()) : QRect();
	}
	std::vector<std::string> shown_users() const
	{
		dock::PeopleList *list = sim_.people();
		return list != nullptr ? list->shown_user_ids() : std::vector<std::string>();
	}
	QPoint random_point_in(const QRect &r)
	{
		const QRect in = r.adjusted(2, 2, -2, -2);
		if (in.width() <= 0 || in.height() <= 0)
			return r.center();
		return QPoint(rand_int(in.left(), in.right()), rand_int(in.top(), in.bottom()));
	}

	// ---- 入力と記録 ----
	void sample_positions();
	void schedule_sample();
	void do_press(Qt::MouseButton button);
	void do_release(Qt::MouseButton button);
	void do_key(int key, Qt::KeyboardModifiers mods, const QString &text);
	void do_context_key();
	void do_wheel(int delta);
	bool on_action(const ActionRecord &rec);
	void on_rename(const std::string &user_id);
	void watch_busy(ledger::ActionId id);
	void judge(const std::string &acted_user, ActionId id, bool ran, const QString &via);
	void incident(const QString &kind, const QString &detail);
	QString press_detail() const;
	struct Scope;
	friend struct Scope;

	// ---- 手順を作る ----
	void poll();
	void schedule_poll();
	void plan_gesture();
	void add(int delay, std::function<void()> fn) { gesture_.push_back({delay, std::move(fn)}); }
	void plan_aim(std::function<QRect()> target, int min_dwell = 0);
	void plan_click_target(std::function<QRect()> target);
	void plan_menu_handling(bool allow_items);
	void plan_hover();
	void plan_leave();
	void plan_wheel();
	void plan_keyboard(bool allow_activate);
	void plan_focus_leave_space();
	void plan_shift_then_click();
	void plan_pounce();
	void plan_leave_now();
	void plan_notice_button();
	void plan_dock_menu();
	void plan_right_click();

	// ---- Observation の書き換え ----
	void model_event();
	void join();
	void leave(const std::string &user_id);
	std::string random_present(bool slotted_only, bool sharing_only);
	void recount_roster();
	void mutated() { last_mutation_ms_ = now(); }

	// ---- (d) と (e) ----
	void check_lines();

	MistapOptions opt_;
	SimPoll sim_;
	Input in_;
	std::mt19937 rng_;
	QElapsedTimer clock_;

	std::deque<Step> gesture_;
	std::int64_t next_step_at_ = 0;
	std::int64_t next_gesture_at_ = 0;
	std::int64_t next_model_at_ = 0;
	int depth_ = 0;
	bool running_ = false;
	std::int64_t stuck_since_ = -1;

	bool quiet_ = false;
	std::int64_t quiet_start_ = 0;
	std::int64_t quiet_end_ = 0;
	std::int64_t next_quiet_at_ = 30000;
	bool quiet_measuring_ = false;
	std::int64_t quiet_measure_start_ = 0;
	std::int64_t quiet_base_lines_ = 0;
	std::int64_t settle_ms_ = 11000;
	std::int64_t last_mutation_ms_ = 0;

	Dispatch dispatch_ = Dispatch::None;
	PressCtx press_;
	std::string pending_menu_owner_;
	std::string menu_owner_;
	std::string key_focus_user_;
	std::string focus_departed_user_; // フォーカスのあった人が抜けた (Tab でフォーカスを動かすまで覚える)
	bool key_after_departure_ = false;
	bool key_focus_on_list_ = false; // キーを押したとき、フォーカスが一覧 (抜けた人の行から移された先) にあった
	// メニューの中で項目を選んだ (クリックか Enter)。行のメニューの項目はメニューが閉じてから実行される
	// (dock/people_list.cpp の open_row_menu の pending) ので、閉じた後に届いた操作を、メニューの項目の操作として
	// 数えるためのフラグ。メニューを開いた、いちばん外の入力が戻ったら下ろす。
	bool menu_choice_made_ = false;

	std::map<std::string, QPoint> row_pos_;           // 行の左上 (スクリーン座標 + 利用者のスクロール量)
	std::map<std::string, std::int64_t> moved_at_;    // 行の左上が最後に変わった時刻 (現れたときを含む)
	// 行の矩形の履歴 (同じ座標。直近 1 秒ぶん)。押した位置の行が直前に入れ替わったかを見る ((b) の数え方)。
	struct Snap {
		std::int64_t t = 0;
		// この並びへの変化が起こりえた最も早い時刻 (1 つ前の測定の時刻と、その後の最初の位置のイベントの
		// 時刻の、遅いほう)。
		std::int64_t lo = 0;
		std::vector<std::pair<std::string, QRect>> rows;
	};
	std::deque<Snap> history_;
	std::int64_t last_sample_t_ = 0;  // 最後に測った時刻
	std::int64_t first_event_t_ = -1; // 最後の測定の後で最初に届いた、位置のイベント (Move など) の時刻
	// 押した位置の行が 500 ms 以内に入れ替わっていたクリックごとの、製品のガードの答えの記録
	// (mistap-seed<N>-guard.tsv)。
	struct GuardRow {
		std::int64_t t_press = 0;
		std::int64_t t_delivery = 0;
		std::string under;
		std::string before;
		std::string guard_user;
		QString part;
		std::int64_t lo_age = -1;   // 届いた時点から、入れ替わりが起こりえた最も早い時刻までの経過
		std::int64_t seen_age = -1; // 同じく、入れ替わりを見つけた測定の時刻までの経過
		std::int64_t product_age = -2;
		bool product_blocked = false;
		bool held = false;
		QString result = QStringLiteral("none"); // ran、refused、none (none はガードが捨てたか、押せない所)
		QString verdict = QStringLiteral("-");   // b (不具合)、b-edge (境界のずれ)、- (動作していない)
	};
	std::vector<GuardRow> guard_rows_;
	std::int64_t guard_closed_ = 0, guard_open_ = 0; // 上のクリックのうち、届いた時点のガードが捨てた数と通した数
	std::int64_t b_edge_ = 0; // (b) の候補のうち、境界のずれと判定した回数 (記録だけ)
	QRect last_viewport_;
	std::int64_t viewport_moved_at_ = -1;
	std::map<std::string, QPoint> content_pos_;
	std::map<std::string, std::int64_t> content_moved_at_;
	std::string occupant(const std::vector<std::pair<std::string, QRect>> &rows, const QPoint &p) const
	{
		for (const auto &[uid, r] : rows) {
			if (r.contains(p))
				return uid;
		}
		return std::string();
	}
	int user_scroll_ = 0; // 利用者のホイールで動いたスクロールの量の合計
	bool sample_pending_ = false;

	// 数えた値
	std::int64_t a_ = 0, a_menu_ = 0, b_ = 0, c_ = 0, c_any_ = 0, d_ = 0, e_ = 0;
	std::int64_t w30_lines_ = 0, quiet_windows_ = 0, quiet_measured_ms_ = 0;
	std::int64_t presses_ = 0, row_presses_ = 0, guarded_window_presses_ = 0, effective_ = 0, keys_ = 0;
	std::int64_t menus_opened_ = 0, model_events_ = 0, hold_episodes_ = 0, unattributed_ = 0;
	std::int64_t notice_person_actions_ = 0;
	std::int64_t row_moved_presses_ = 0, b_row_ = 0;
	bool adding_to_vesktop_ = false;
	std::int64_t stale_buttons_ = 0; // 押す前にボタンが押されたままだったクリック (0 であること)
	std::int64_t stale_button_down_ = 0; // 押す前に qt_button_down が残っていたクリック (記録だけ)
	std::int64_t menu_owner_differs_ = 0; // 行のメニューの人が、開いたつもりの人と違った回数 (記録だけ)
	std::int64_t busy_measured_ = 0, busy_slow_ = 0, busy_missed_ = 0, busy_max_ms_ = 0;
	std::int64_t d_since_ = -1, e_since_ = -1;
	bool was_held_ = false;
	bool was_dock_visible_ = true;
	std::int64_t other_menus_ = 0; // cordmux のものでないメニューが開いた回数 (記録だけ)
	std::int64_t dock_hidden_checks_ = 0; // ドックが表示されていなかった確認の回数 (0 であること)
	std::map<QString, std::int64_t> event_counts_;
	QStringList incidents_;
	QStringList action_log_;
	QStringList gesture_log_;
};

// ---- 位置の測定 (b) ----

void Mistap::sample_positions()
{
	sample_pending_ = false;
	dock::PeopleList *list = sim_.people();
	if (list == nullptr)
		return;
	const std::int64_t t = now();
	std::set<std::string> seen;
	Snap snap;
	snap.t = t;
	snap.lo = first_event_t_ >= 0 ? std::max(first_event_t_, last_sample_t_) : last_sample_t_;
	last_sample_t_ = t;
	first_event_t_ = -1;
	for (const std::string &uid : list->shown_user_ids()) {
		dock::PersonRowWidget *row = list->row_for(uid);
		if (row == nullptr || !row->isVisible())
			continue;
		QRect r = global_rect(row);
		r.translate(0, user_scroll_); // 利用者がスクロールした分は動きに数えない
		// スクロール領域の表示範囲の外の部分は押せないので除く (同じ座標で)。
		const QRect vp = viewport_rect().translated(0, user_scroll_);
		snap.rows.push_back({uid, r.intersected(vp)});
		seen.insert(uid);
		const auto it = row_pos_.find(uid);
		if (it == row_pos_.end() || it->second != r.topLeft()) {
			row_pos_[uid] = r.topLeft();
			moved_at_[uid] = t;
		}
	}
	if (history_.empty() || history_.back().rows != snap.rows)
		history_.push_back(std::move(snap));
	// 原因の記録: viewport の画面の上の位置と、行のスクロール領域の中での位置 (製品の dock/people_list.cpp の
	// note_positions と同じ基準)。
	const QRect vp_now = viewport_rect();
	if (vp_now != last_viewport_) {
		if (last_viewport_.isValid())
			viewport_moved_at_ = t;
		last_viewport_ = vp_now;
	}
	auto *scroll = sim_.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
	QWidget *content = scroll != nullptr ? scroll->widget() : nullptr;
	if (content != nullptr) {
		for (const std::string &uid : list->shown_user_ids()) {
			dock::PersonRowWidget *row = list->row_for(uid);
			if (row == nullptr || !row->isVisible())
				continue;
			const QPoint cp = row->mapTo(content, QPoint(0, 0));
			const auto it = content_pos_.find(uid);
			if (it == content_pos_.end() || it->second != cp) {
				content_pos_[uid] = cp;
				content_moved_at_[uid] = t;
			}
		}
	}
	// 直近 1 秒ぶんと、その手前の 1 つ (比べる元) を残す。
	while (history_.size() > 2 && history_[1].t < t - 1000)
		history_.pop_front();
	for (auto it = row_pos_.begin(); it != row_pos_.end();) {
		if (seen.count(it->first) == 0)
			it = row_pos_.erase(it);
		else
			++it;
	}
}

void Mistap::schedule_sample()
{
	if (sample_pending_)
		return;
	sample_pending_ = true;
	// 受け手にドックを渡す (この Mistap と一緒に破棄される)。受け手がないと、シードを終えて Mistap を破棄した後に
	// 残った呼び出しが届き、破棄したものを読む (次の場面の途中で落ちる)。このファイルの単発のタイマーは、どれも同じ形にする。
	QTimer::singleShot(0, sim_.dock(), [this] { sample_positions(); });
}

// ---- 入力 ----

// 入れ子の入力 (メニューの中のクリック) の間は、外の入力の記録を退けておき、戻ったら元に戻す。メニューが閉じた後に届く
// 操作は、外の入力 (メニューを開いたクリック) の中で実行されるので、外の記録で数える。いちばん外の入力では、
// ボタンを押したときの記録を、離す手順まで残す (クリックは、押す手順と離す手順に分かれている)。
struct Mistap::Scope {
	Mistap *m;
	bool nested;
	Dispatch prev_dispatch;
	PressCtx prev_press;
	std::string prev_key_user;
	bool prev_after;
	bool prev_on_list;
	explicit Scope(Mistap *self)
		: m(self), nested(self->depth_ > 0), prev_dispatch(self->dispatch_), prev_press(self->press_),
		  prev_key_user(self->key_focus_user_), prev_after(self->key_after_departure_),
		  prev_on_list(self->key_focus_on_list_)
	{
		++m->depth_;
	}
	~Scope()
	{
		--m->depth_;
		if (nested) {
			m->dispatch_ = prev_dispatch;
			m->press_ = prev_press;
			m->key_focus_user_ = prev_key_user;
			m->key_after_departure_ = prev_after;
			m->key_focus_on_list_ = prev_on_list;
		} else {
			m->dispatch_ = Dispatch::None;
			m->menu_choice_made_ = false;
		}
	}
};

void Mistap::do_press(Qt::MouseButton button)
{
	sample_positions();
	QWidget *under = Input::widget_at(in_.pos());
	const bool in_menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()) != nullptr;
	PressCtx p;
	p.t_ms = now();
	p.pos = in_.pos();
	p.under_user = in_menu ? std::string() : user_of(under);
	p.under_part = in_menu ? QStringLiteral("menu") : part_of(under);
	p.held = sim_.widget()->layout_held();
	if (!p.under_user.empty()) {
		const auto it = moved_at_.find(p.under_user);
		p.moved_age_ms = it == moved_at_.end() ? -1 : p.t_ms - it->second;
		++row_presses_;
		// 押した位置の行が、押す前の 500 ms の間に入れ替わったか (一覧の並びや高さが変わった、
		// 行が現れた、消えた)。履歴の各時点で同じ位置にあった行と、今ある行を比べる。入れ替わった時刻は、
		// 違う行があった最後の時点の、次の時点とする。
		const QPoint pc = p.pos + QPoint(0, user_scroll_);
		const std::string now_occupant = occupant(history_.empty() ? std::vector<std::pair<std::string, QRect>>() : history_.back().rows, pc);
		for (std::size_t i = history_.size(); i-- > 0;) {
			const std::string then = occupant(history_[i].rows, pc);
			if (then != now_occupant) {
				const std::int64_t changed_at = i + 1 < history_.size() ? history_[i + 1].t : p.t_ms;
				if (p.t_ms - changed_at < 500) {
					p.under_changed_age_ms = p.t_ms - changed_at;
					p.under_before = then;
					p.change_seen_ms = changed_at;
					p.change_lo_ms = i + 1 < history_.size() ? history_[i + 1].lo : p.t_ms;
				}
				break;
			}
		}
		if (p.under_changed_age_ms >= 0)
			++guarded_window_presses_;
		p.viewport_moved_age_ms = viewport_moved_at_ >= 0 ? p.t_ms - viewport_moved_at_ : -1;
		const auto ct = content_moved_at_.find(p.under_user);
		p.content_moved_age_ms = ct == content_moved_at_.end() ? -1 : p.t_ms - ct->second;
		if (p.moved_age_ms >= 0 && p.moved_age_ms < 500)
			++row_moved_presses_;
	}
	press_ = p;
	++presses_;
	// 枠が埋まっている間は、チェックを押しても入れ替える相手のメニューが開く。
	if (!in_menu && (p.under_part == QLatin1String("dots") || p.under_part == QLatin1String("check") || button == Qt::RightButton))
		pending_menu_owner_ = p.under_user;
	press_.buttons_before_input = static_cast<int>(in_.buttons());
	press_.buttons_before_qt = static_cast<int>(QGuiApplication::mouseButtons());
	if (press_.buttons_before_input != 0 || press_.buttons_before_qt != 0)
		++stale_buttons_;
	if (qt_button_down != nullptr) {
		press_.button_down_set = true;
		press_.button_down_user = user_of(qt_button_down);
		press_.button_down_part = part_of(qt_button_down);
		++stale_button_down_;
	}
	Scope scope(this);
	dispatch_ = in_menu ? Dispatch::PointerMenu : (p.under_user.empty() ? Dispatch::PointerOther : Dispatch::PointerRow);
	// Qt がボタンを押すイベントを送った最初のウィジェットを記録する (アプリ全体のイベントフィルター。
	// クリック 1 つの間だけ)。
	struct ReceiverSpy : QObject {
		QWidget *first = nullptr;
		Mistap *self = nullptr;
		bool eventFilter(QObject *o, QEvent *e) override
		{
			if (first == nullptr && e->type() == QEvent::MouseButtonPress && o->isWidgetType() &&
			    !static_cast<QWidget *>(o)->isWindow()) {
				first = static_cast<QWidget *>(o);
				self->press_.scroll_at_delivery = scroll_value(self->sim_.widget());
				self->press_.under_at_delivery = user_of(Input::widget_at(self->in_.pos()));
				// ウィジェットの mousePressEvent (製品のガード) の直前に、製品のガードの答えと、
				// 行が変わった時刻 (changed_at_ms_) からの経過を読む。
				if (self->dispatch_ == Dispatch::PointerRow) {
					self->press_.t_delivery_ms = self->now();
					std::string u = user_of(first);
					if (u.empty())
						u = self->press_.under_user;
					dock::PeopleList *list = self->sim_.people();
					const dock::PersonRowWidget *row = list != nullptr ? list->row_for(u) : nullptr;
					self->press_.guard_user = u;
					self->press_.product_blocked = row != nullptr && row->press_blocked_now();
					self->press_.product_age_ms = product_guard_age(row);
				}
			}
			return false;
		}
	} spy;
	spy.self = this;
	press_.dock_active_before = QGuiApplication::focusWindow() != nullptr &&
				    QGuiApplication::focusWindow() == sim_.dock()->windowHandle();
	press_.scroll_before = scroll_value(sim_.widget());
	qApp->installEventFilter(&spy);
	in_.press(button);
	qApp->removeEventFilter(&spy);
	if (spy.first != nullptr) {
		press_.receiver_user = user_of(spy.first);
		press_.receiver_part = part_of(spy.first);
	}
	// 押した位置の行が (テストの測定で) 500 ms 以内に入れ替わっていたクリックは、製品のガードの答えを
	// 1 行ずつ残す。
	if (dispatch_ == Dispatch::PointerRow && press_.under_changed_age_ms >= 0) {
		GuardRow g;
		g.t_press = press_.t_ms;
		g.t_delivery = press_.t_delivery_ms;
		g.under = press_.under_user;
		g.before = press_.under_before;
		g.guard_user = press_.guard_user;
		g.part = press_.under_part;
		if (press_.t_delivery_ms >= 0) {
			g.lo_age = press_.t_delivery_ms - press_.change_lo_ms;
			g.seen_age = press_.t_delivery_ms - press_.change_seen_ms;
			if (press_.product_blocked)
				++guard_closed_;
			else
				++guard_open_;
		}
		g.product_age = press_.product_age_ms;
		g.product_blocked = press_.product_blocked;
		g.held = press_.held;
		press_.guard_row = static_cast<int>(guard_rows_.size());
		guard_rows_.push_back(std::move(g));
	}
}

void Mistap::do_release(Qt::MouseButton button)
{
	const bool in_menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()) != nullptr;
	Scope scope(this);
	dispatch_ = press_.under_part == QLatin1String("menu")
			    ? Dispatch::PointerMenu
			    : (press_.under_user.empty() ? Dispatch::PointerOther : Dispatch::PointerRow);
	if (in_menu && press_.under_part == QLatin1String("menu"))
		menu_choice_made_ = true;
	in_.release(button); // メニュー (exec) が開くと、閉じるまでここから戻らない
}

void Mistap::do_key(int key, Qt::KeyboardModifiers mods, const QString &text)
{
	const bool in_menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()) != nullptr;
	if (!in_menu) {
		QWidget *f = QApplication::focusWidget();
		key_focus_user_ = user_of(f);
		key_focus_on_list_ = f != nullptr && (f->objectName() == QLatin1String("cordmuxPeopleList") ||
						       f->objectName() == QLatin1String("cordmuxScroll"));
		key_after_departure_ = !focus_departed_user_.empty();
		pending_menu_owner_ = key_focus_user_;
	}
	++keys_;
	Scope scope(this);
	dispatch_ = in_menu ? Dispatch::PointerMenu : Dispatch::Key;
	if (in_menu && (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space))
		menu_choice_made_ = true;
	in_.key(key, mods, text);
}

void Mistap::do_context_key()
{
	Scope scope(this);
	key_focus_user_ = user_of(QApplication::focusWidget());
	key_focus_on_list_ = false;
	key_after_departure_ = !focus_departed_user_.empty();
	pending_menu_owner_ = key_focus_user_;
	++keys_;
	dispatch_ = Dispatch::Key;
	in_.context_menu_key(chance(0.5) ? Qt::ShiftModifier : Qt::NoModifier);
}

void Mistap::do_wheel(int delta)
{
	const int before = scroll_value(sim_.widget());
	in_.wheel(delta);
	const int after = scroll_value(sim_.widget());
	user_scroll_ += after - before;
}

void Mistap::incident(const QString &kind, const QString &detail)
{
	incidents_ << QStringLiteral("%1\t%2\t%3").arg(now()).arg(kind, tsv_cell(detail));
	say(QStringLiteral("  [seed %1] %2 at %3 ms: %4").arg(opt_.seed).arg(kind).arg(now()).arg(detail));
}

QString Mistap::press_detail() const
{
	// 最後のクリックの原因を調べるための記録 (ポインターの下、Qt が送った先、押す前に Qt が覚えていたウィジェット、
	// どの経路か)。
	return QStringLiteral("press_under=%1/%2 receiver=%3/%4 button_down=%5/%6 key_focus=%7 dispatch=%8 "
			      "dock_active_before=%9 scroll=%10->%11 under_at_delivery=%12")
		.arg(QString::fromStdString(press_.under_user).right(3), press_.under_part,
		     QString::fromStdString(press_.receiver_user).right(3), press_.receiver_part,
		     press_.button_down_set ? QString::fromStdString(press_.button_down_user).right(3) : QStringLiteral("-"),
		     press_.button_down_part, QString::fromStdString(key_focus_user_).right(3))
		.arg(static_cast<int>(dispatch_))
		.arg(press_.dock_active_before ? 1 : 0)
		.arg(press_.scroll_before)
		.arg(press_.scroll_at_delivery)
		.arg(QString::fromStdString(press_.under_at_delivery).right(3));
}

void Mistap::judge(const std::string &acted_user, ActionId id, bool ran, const QString &via)
{
	const QString acted = QString::fromStdString(acted_user).right(3);
	action_log_ << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7")
			       .arg(now())
			       .arg(static_cast<int>(id))
			       .arg(acted, via, ran ? QStringLiteral("ran") : QStringLiteral("refused"),
				    QString::fromStdString(press_.under_user).right(3))
			       .arg(press_.moved_age_ms);
	const bool row_press_guarded = !menu_choice_made_ && dispatch_ == Dispatch::PointerRow && press_.guard_row >= 0 &&
				       press_.guard_row < static_cast<int>(guard_rows_.size());
	if (row_press_guarded)
		guard_rows_[static_cast<size_t>(press_.guard_row)].result = ran ? QStringLiteral("ran") : QStringLiteral("refused");
	if (!ran)
		return;
	++effective_;
	// [変更] のメニューの項目は、選んだ人をメインアカウントにする操作で、行の人に適用する操作ではない。
	if (id == ActionId::SetMainAccount)
		return;
	if (menu_choice_made_) {
		// 行のメニューの項目は、メニューを開いた人に適用されること。クリックを捨てるガードはメニューを開いた
		// クリックに適用されるものなので、(b) は数えない。
		if (acted_user != menu_owner_) {
			++a_;
			++a_menu_;
			incident(QStringLiteral("a-menu"), QStringLiteral("action=%1 acted=%2 menu_owner=%3 %4")
								  .arg(static_cast<int>(id))
								  .arg(acted, QString::fromStdString(menu_owner_).right(3), press_detail()));
		}
		return;
	}
	switch (dispatch_) {
	case Dispatch::PointerRow:
		if (acted_user != press_.under_user) {
			++a_;
			incident(QStringLiteral("a"),
				 QStringLiteral("action=%1 acted=%2 under=%3 part=%4 via=%5 receiver=%6/%7 buttons_before input=%8 qt=%9 %10")
					 .arg(static_cast<int>(id))
					 .arg(acted, QString::fromStdString(press_.under_user).right(3), press_.under_part, via,
					      QString::fromStdString(press_.receiver_user).right(3), press_.receiver_part)
					 .arg(press_.buttons_before_input)
					 .arg(press_.buttons_before_qt)
					 .arg(press_detail()));
		}
		if (press_.under_changed_age_ms >= 0) {
			// 製品の行が変わった時刻と照合して、不具合 ((b) に数える) と境界のずれ (記録だけ) に分ける。
			const QString why = b_reason(press_, kGuardWindowMs);
			const QString detail =
				QStringLiteral("action=%1 user=%2 under_changed_age=%3ms before=%4 row_moved_age=%5ms "
					       "content_moved_age=%6ms viewport_moved_age=%7ms held=%8")
					.arg(static_cast<int>(id))
					.arg(acted)
					.arg(press_.under_changed_age_ms)
					.arg(QString::fromStdString(press_.under_before).right(3))
					.arg(press_.moved_age_ms)
					.arg(press_.content_moved_age_ms)
					.arg(press_.viewport_moved_age_ms)
					.arg(press_.held ? 1 : 0) +
				QStringLiteral(" delivery_after_press=%1ms change_window_at_delivery=%2..%3ms guard_user=%4 "
					       "product_age=%5ms product_blocked=%6")
					.arg(press_.t_delivery_ms >= 0 ? press_.t_delivery_ms - press_.t_ms : -1)
					.arg(press_.t_delivery_ms >= 0 ? press_.t_delivery_ms - press_.change_seen_ms : -1)
					.arg(press_.t_delivery_ms >= 0 ? press_.t_delivery_ms - press_.change_lo_ms : -1)
					.arg(QString::fromStdString(press_.guard_user).right(3))
					.arg(press_.product_age_ms)
					.arg(press_.product_blocked ? 1 : 0);
			if (why.isEmpty()) {
				++b_edge_;
				incident(QStringLiteral("b-edge"), detail);
			} else {
				++b_;
				incident(QStringLiteral("b"), detail + QStringLiteral(" why=") + why);
			}
			if (row_press_guarded)
				guard_rows_[static_cast<size_t>(press_.guard_row)].verdict = why.isEmpty() ? QStringLiteral("b-edge")
														: QStringLiteral("b:") + why;
		}
		if (press_.moved_age_ms >= 0 && press_.moved_age_ms < 500) {
			// 製品のガードと同じ見方 (その行の左上が 500 ms 以内に動いたか現れた) で、
			// 動作したクリック。記録だけ。
			++b_row_;
		}
		break;
	case Dispatch::PointerMenu:
		if (acted_user != menu_owner_) {
			++a_;
			++a_menu_;
			incident(QStringLiteral("a-menu"), QStringLiteral("action=%1 acted=%2 menu_owner=%3 %4")
								  .arg(static_cast<int>(id))
								  .arg(acted, QString::fromStdString(menu_owner_).right(3), press_detail()));
		}
		break;
	case Dispatch::Key:
		if (key_focus_user_.empty() && !key_focus_on_list_) {
			// フォーカスが行にも一覧にもない (メッセージのボタン)。メッセージのボタンの操作は、
			// メッセージが指す人に適用される。
			++notice_person_actions_;
			break;
		}
		if (acted_user != key_focus_user_) {
			++c_any_;
			if (key_after_departure_)
				++c_;
			incident(QStringLiteral("c"), QStringLiteral("action=%1 acted=%2 focus=%3 after_departure=%4")
							     .arg(static_cast<int>(id))
							     .arg(acted, QString::fromStdString(key_focus_user_).right(3))
							     .arg(key_after_departure_ ? 1 : 0));
		}
		break;
	case Dispatch::PointerOther:
		// 行の外 (メッセージのボタン) を押して、人の操作が動作した。メッセージのボタンはメッセージが指す人に適用されるので、
		// (a) には数えない (押した所に行がない)。数だけを記録する。
		++notice_person_actions_;
		break;
	case Dispatch::None:
		++unattributed_;
		incident(QStringLiteral("unattributed"), QStringLiteral("action=%1 acted=%2").arg(static_cast<int>(id)).arg(acted));
		break;
	}
}

bool Mistap::on_action(const ActionRecord &rec)
{
	if (rec.ran && rec.kind == ledger::IntentKind::WriteVencordDir) {
		// 時間のかかる操作 (DockPoll の install_state_ の adding_to_vesktop と同じ)。押したら処理中にし、
		// 1.5 秒後に追加できて終わる。押してから処理中の見た目になるまでの時間を、押したボタンを
		// 2 ms ごとに見て測る。
		adding_to_vesktop_ = true;
		sim_.set_busy({rec.id});
		watch_busy(rec.id);
		QTimer::singleShot(1500, sim_.dock(), [this] {
			sim_.set_busy({});
			sim_.obs().install.dir = ledger::VencordDirState::Matches;
			sim_.obs().install.process = ledger::VesktopProcess::Running;
			sim_.obs().ops.vesktop_add_failure.reset();
			sim_.changed();
			adding_to_vesktop_ = false;
		});
		mutated();
		return true;
	}
	if (rec.args.user_id.empty())
		return false; // 人に関係しない操作 (録画など) は押し間違いに数えない
	judge(rec.args.user_id, rec.id, rec.ran, QStringLiteral("action"));
	if (rec.ran)
		mutated();
	return false;
}

void Mistap::watch_busy(ledger::ActionId id)
{
	(void)id;
	// 押したボタン: 押した瞬間にポインターの下にあった QPushButton (キーなら、フォーカスのあるウィジェット)。
	QPointer<QPushButton> button = qobject_cast<QPushButton *>(
		dispatch_ == Dispatch::Key ? QApplication::focusWidget() : Input::widget_at(press_.pos));
	if (button.isNull())
		return;
	const QString text = button->text();
	// 起点は操作が動作した時点 (クリックはボタンを離した時点で動作する。押してから離すまでの時間は数えない)。
	const std::int64_t start = now();
	auto *poll = new QTimer();
	poll->setInterval(2);
	QObject::connect(poll, &QTimer::timeout, [this, poll, button, text, start] {
		const std::int64_t t = now();
		if (button.isNull() || t - start > 2000) {
			++busy_missed_;
			poll->deleteLater();
			return;
		}
		if (!button->isEnabled() || button->text() != text) {
			const std::int64_t d = t - start;
			busy_max_ms_ = std::max(busy_max_ms_, d);
			++busy_measured_;
			if (d > 100) {
				++busy_slow_;
				incident(QStringLiteral("busy-slow"), QStringLiteral("busy look after %1 ms").arg(d));
			}
			poll->deleteLater();
		}
	});
	// 現在の時点で処理中の見た目になっていれば 0 (同じイベントループの周回の中で描き終えている)。
	if (!button->isEnabled() || button->text() != text) {
		busy_max_ms_ = std::max<std::int64_t>(busy_max_ms_, now() - start);
		++busy_measured_;
		delete poll;
		return;
	}
	poll->start();
}

void Mistap::on_rename(const std::string &user_id)
{
	// 「名前を変更...」はダイアログを表示しない (SimPoll::on_rename)。押した人が誰かだけを数える。
	judge(user_id, ActionId::SetNameOverride, true, QStringLiteral("rename"));
}

// ---- (d) と (e) ----

void Mistap::check_lines()
{
	const std::int64_t t = now();
	// (d) 1 行目: 全文が View の 1 行目と同じで、表示されているテキストがその候補のどれか (か、
	// 最後の候補を省略したもの) であること。
	const ledger::dockview::DockViewV2 &view = sim_.view();
	auto *first = dynamic_cast<dock::FirstLineBar *>(
		sim_.widget()->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine")));
	bool line_ok = true;
	if (first != nullptr) {
		const QString expected = first_line_expected(view);
		line_ok = first->full_text() == expected;
		const QString shown = first->shown_text();
		if (line_ok && !shown.isEmpty()) {
			bool belongs = false;
			for (const ledger::TextRef &ref : view.first_line.forms) {
				const QString form = dock::ui::resolve(ref);
				if (shown == form)
					belongs = true;
				else if (shown.endsWith(kEllipsis) && form.startsWith(shown.left(shown.size() - 1)))
					belongs = true;
			}
			line_ok = belongs;
		}
	}
	if (line_ok) {
		d_since_ = -1;
	} else if (d_since_ < 0) {
		d_since_ = t;
	} else if (t - d_since_ > 500) {
		++d_;
		incident(QStringLiteral("d"), QStringLiteral("shown=%1 full=%2 expected=%3")
						      .arg(first != nullptr ? first->shown_text() : QString(),
							   first != nullptr ? first->full_text() : QString(), first_line_expected(view)));
		d_since_ = -1;
	}

	// (e) 並びの保留が解けている間に、並び (行の順とメッセージのキーの順) が View と違ったままか。ドックが
	// 表示されていない間は数えない (Qt は表示されていないウィジェットを並べ直さず、表示されていないものの並びは
	// 利用者に見えない)。表示されているかの変化は記録する。
	const bool dock_visible = sim_.widget()->isVisible();
	if (dock_visible != was_dock_visible_) {
		was_dock_visible_ = dock_visible;
		incident(QStringLiteral("dock-visible"), QStringLiteral("visible=%1 floating=%2 last_gesture=%3")
							     .arg(dock_visible ? 1 : 0)
							     .arg(sim_.dock()->isFloating() ? 1 : 0)
							     .arg(gesture_log_.isEmpty() ? QString() : gesture_log_.last()));
	}
	if (!dock_visible) {
		++dock_hidden_checks_;
		e_since_ = -1;
		return;
	}
	const bool held = sim_.widget()->layout_held();
	if (held && !was_held_)
		++hold_episodes_;
	was_held_ = held;
	if (held) {
		e_since_ = -1;
		return;
	}
	std::vector<std::string> expected_people;
	for (const ledger::dockview::PersonRow &row : listed_rows(view))
		expected_people.push_back(row.user_id);
	// メッセージのカードは状態タブにあるので、開いている間だけ比べる。
	const bool status_open = sim_.widget()->tab() == config::DockTab::Status;
	const bool same = shown_users() == expected_people &&
			  (!status_open || shown_notice_keys(sim_.widget()) == expected_notice_keys(view));
	if (same) {
		e_since_ = -1;
	} else if (e_since_ < 0) {
		e_since_ = t;
	} else if (t - e_since_ > 500) {
		++e_;
		// 原因を分けるための記録: カードごとのキー、種類、上端の位置、親と、View の 1 行目、
		// メッセージのレベル、表示した時刻。
		QStringList cards;
		for (QWidget *c : sim_.widget()->findChildren<QWidget *>()) {
			auto *card = dynamic_cast<dock::NoticeCard *>(c);
			if (card == nullptr || !card->isVisibleTo(sim_.widget()))
				continue;
			// レイアウトの中の位置 (親の layout の何番目か) と、親の中の矩形。位置の番号が正しく、
			// 矩形だけが古ければ、layout が並べ直していない (LayoutRequest が処理されていない)。
			QLayout *lay = card->parentWidget() != nullptr ? card->parentWidget()->layout() : nullptr;
			const QLayoutPrivate *lay_d = lay != nullptr ? static_cast<const QLayoutPrivate *>(QObjectPrivate::get(lay)) : nullptr;
			cards << QStringLiteral("%1/%2@%3<%4 idx=%5 geo=%6,%7,%8x%9 vis=%10 act=%11")
					 .arg(QString::fromStdString(card->notice().key).left(24))
					 .arg(static_cast<int>(card->notice().tier))
					 .arg(card->mapTo(sim_.widget(), QPoint(0, 0)).y())
					 .arg(card->parentWidget() != nullptr ? QString::fromLatin1(card->parentWidget()->metaObject()->className()) : QString())
					 .arg(lay != nullptr ? lay->indexOf(card) : -2)
					 .arg(card->geometry().x())
					 .arg(card->geometry().y())
					 .arg(card->geometry().width())
					 .arg(card->geometry().height())
					 .arg(card->isVisible() ? 1 : 0)
					 .arg(lay_d != nullptr ? static_cast<int>(lay_d->activated) : -1);
		}
		QStringList tiers;
		for (const ledger::dockview::Notice &n : view.notices)
			tiers << QStringLiteral("%1:%2:%3").arg(QString::fromStdString(n.key).left(24)).arg(static_cast<int>(n.tier)).arg(n.appeared_at_us / 1000);
		incident(QStringLiteral("e"), QStringLiteral("people_shown=%1 expected=%2 notices_shown=%3 expected=%4 cards=[%5] view=[%6] "
							     "first_line=%7/%8")
						      .arg(shown_users().size())
						      .arg(expected_people.size())
						      .arg(shown_notice_keys(sim_.widget()).join(QLatin1Char(',')),
							   expected_notice_keys(view).join(QLatin1Char(',')), cards.join(QLatin1Char(' ')),
							   tiers.join(QLatin1Char(' ')))
						      .arg(static_cast<int>(view.first_line.kind))
						      .arg(QString::fromStdString(view.first_line.notice_key)));
		e_since_ = -1;
	}
}

// ---- Observation の書き換え ----

void Mistap::recount_roster()
{
	int n = 0;
	for (const PersonFact &p : sim_.obs().people)
		n += p.in_roster ? 1 : 0;
	sim_.obs().call.roster_count = n;
}

std::string Mistap::random_present(bool slotted_only, bool sharing_only)
{
	std::vector<std::string> ids;
	for (const PersonFact &p : sim_.obs().people) {
		if (!p.in_roster || p.is_receiver)
			continue;
		if (slotted_only && !p.slot.has_value())
			continue;
		if (sharing_only && !p.sharing)
			continue;
		ids.push_back(p.user_id);
	}
	if (ids.empty())
		return std::string();
	return ids[static_cast<std::size_t>(rand_int(0, static_cast<int>(ids.size()) - 1))];
}

void Mistap::join()
{
	std::set<std::string> present;
	for (const PersonFact &p : sim_.obs().people)
		present.insert(p.user_id);
	if (present.size() >= 12)
		return;
	for (int tries = 0; tries < 20; ++tries) {
		const int n = rand_int(20, 45);
		const std::string id = fake_id(n);
		if (present.count(id) != 0)
			continue;
		PersonFact p;
		p.user_id = id;
		const char *name = kNames[rand_int(0, static_cast<int>(std::size(kNames)) - 1)];
		p.display_name = std::string(name);
		p.discord_name = std::string(name);
		p.in_roster = true;
		p.sharing_known = true;
		if (chance(0.4)) {
			p.sharing = true;
			p.sharing_since_us = sim_.now_us();
		}
		sim_.obs().people.push_back(p);
		recount_roster();
		++event_counts_[QStringLiteral("join")];
		return;
	}
}

void Mistap::leave(const std::string &user_id)
{
	auto &people = sim_.obs().people;
	for (auto it = people.begin(); it != people.end(); ++it) {
		if (it->user_id != user_id)
			continue;
		if (user_of(QApplication::focusWidget()) == user_id)
			focus_departed_user_ = user_id;
		if (it->slot.has_value()) {
			// 枠に載っている人は、通話から抜けても一覧に残る (dock/build_observation.cpp の
			// people_of は、枠に載っている人を一覧に入れる)。
			it->in_roster = false;
			it->sharing = false;
		} else {
			people.erase(it);
		}
		recount_roster();
		++event_counts_[QStringLiteral("leave")];
		return;
	}
}

void Mistap::model_event()
{
	ledger::Observation &obs = sim_.obs();
	const int roll = rand_int(0, 99);
	++model_events_;
	if (roll < 18) {
		join();
	} else if (roll < 36) {
		int present = 0;
		for (const PersonFact &p : obs.people)
			present += p.in_roster && !p.is_receiver ? 1 : 0;
		if (present > 2)
			leave(random_present(false, false));
	} else if (roll < 48) {
		const std::string id = random_present(false, false);
		for (PersonFact &p : obs.people) {
			if (p.user_id != id)
				continue;
			const std::string name = kNames[rand_int(0, static_cast<int>(std::size(kNames)) - 1)];
			p.display_name = name;
			p.discord_name = name;
		}
		++event_counts_[QStringLiteral("rename")];
	} else if (roll < 58) {
		const std::string id = random_present(false, false);
		for (PersonFact &p : obs.people) {
			if (p.user_id != id)
				continue;
			p.sharing = !p.sharing;
			p.sharing_since_us = p.sharing ? std::optional<std::int64_t>(sim_.now_us()) : std::nullopt;
		}
		++event_counts_[QStringLiteral("share")];
	} else if (roll < 68) {
		const std::string id = random_present(true, true);
		if (!id.empty())
			sim_.set_frames_stopped(id, !sim_.frames_stopped(id)); // AllVideoStoppedHint の表示と消去
		++event_counts_[QStringLiteral("frames")];
	} else if (roll < 73) {
		obs.seen.discarded_with_marks = obs.seen.discarded_with_marks > 0 ? 0 : 2;
		++event_counts_[QStringLiteral("notice-L3")];
	} else if (roll < 78) {
		obs.versions.discord_side = obs.versions.discord_side == "0.0.9" ? "0.1.0+dev" : "0.0.9";
		++event_counts_[QStringLiteral("notice-L2-version")];
	} else if (roll < 83) {
		obs.choices.discord_audio_source = obs.choices.discord_audio_source == ledger::DiscordAudioSource::Found
							   ? ledger::DiscordAudioSource::NotFound
							   : ledger::DiscordAudioSource::Found;
		++event_counts_[QStringLiteral("notice-L2-audio")];
	} else if (roll < 88) {
		// Discord の内部への依存箇所 (どれもメッセージのレベル 2)。
		const bool pc_hook = chance(0.5);
		const char *name = pc_hook ? "pc-hook" : "profile";
		auto &contacts = obs.contacts;
		const auto it = std::find_if(contacts.begin(), contacts.end(),
					     [&](const ledger::ContactFact &c) { return c.id == name; });
		if (it == contacts.end())
			contacts.push_back({name, pc_hook ? ledger::ContactState::Missing : ledger::ContactState::Lost});
		else
			contacts.erase(it);
		++event_counts_[QStringLiteral("notice-L2-contact")];
	} else if (roll < 93) {
		obs.recording.stopped_by_space_exhausted = !obs.recording.stopped_by_space_exhausted;
		++event_counts_[QStringLiteral("notice-L1-space")];
	} else if (roll < 96 && !adding_to_vesktop_) {
		// [Vesktopに追加] の失敗 (メッセージのレベル 2 の VesktopAddFailed)。このメッセージは、時間のかかる操作のボタン
		// (cordmux.dock.add_to_vesktop.button) を持つ。
		// [Vesktopに追加] は Vesktop の起動中は押せない (ledger/actions.cpp の vesktop_busy_reason) ので、メッセージを出す間は
		// Vesktop を終了した状態にし、時間のかかる操作のボタンを押せるようにする (消すときに起動している状態へ戻す)。
		const bool failed = obs.ops.vesktop_add_failure.has_value();
		obs.install.dir = failed ? ledger::VencordDirState::Matches : ledger::VencordDirState::NotConfigured;
		obs.install.process = failed ? ledger::VesktopProcess::Running : ledger::VesktopProcess::NotRunning;
		if (failed)
			obs.ops.vesktop_add_failure.reset();
		else
			obs.ops.vesktop_add_failure = ledger::VesktopAddFailure::StateWriteFailed;
		++event_counts_[QStringLiteral("notice-L2-add-failed")];
	}
	sim_.changed();
	mutated();
}

// ---- 手順を作る ----

void Mistap::plan_aim(std::function<QRect()> target, int min_dwell)
{
	// 現在の位置から目標の中の 1 点へ、3 から 8 回に分けて動かす (人の手の動きを粗くまねる)。目標の位置は動かすたびに
	// 測り直す (目標が動けば追う)。
	const int steps = rand_int(3, 8);
	auto goal = std::make_shared<QPoint>();
	auto from = std::make_shared<QPoint>();
	add(rand_int(10, 40), [this, target, goal, from] {
		// 目標は、ドックの中身 (フローティングのウィンドウのタイトルバーを除く) の表示されている所に限る。
		// 人は表示されている所しか押さない。タイトルバーを押すと、OBS のメインウィンドウのドックの一覧の
		// メニュー (QMainWindow::createPopupMenu) が開き、その項目でドックを隠してしまう。
		const QRect content = global_rect(sim_.widget());
		const QRect r = target().intersected(content);
		*goal = r.isValid() && !r.isEmpty() ? random_point_in(r) : content.center();
		*from = in_.pos().x() < 0 ? QPoint(goal->x() - 200, goal->y() - 100) : in_.pos();
	});
	for (int i = 1; i <= steps; ++i) {
		add(rand_int(15, 60), [this, goal, from, i, steps] {
			const double f = static_cast<double>(i) / steps;
			in_.move_to(QPoint(from->x() + static_cast<int>((goal->x() - from->x()) * f),
					   from->y() + static_cast<int>((goal->y() - from->y()) * f)));
		});
	}
	// 止まってから押すまでの時間 (速いクリックを多めに混ぜ、クリックを捨てるガードの 500 ms の内と外の両方を試す)。
	const double r = rand01();
	int dwell = r < 0.35 ? rand_int(0, 150) : r < 0.75 ? rand_int(150, 600) : rand_int(600, 1500);
	dwell = std::max(dwell, min_dwell);
	add(dwell, [] {});
}

void Mistap::plan_click_target(std::function<QRect()> target)
{
	plan_aim(std::move(target));
	add(0, [this] { do_press(Qt::LeftButton); });
	add(rand_int(40, 140), [this] { do_release(Qt::LeftButton); });
}

void Mistap::plan_menu_handling(bool allow_items)
{
	// メニューが開いていれば、Esc を押すか、項目を 1 つ押す (サブメニューがあれば、開いてからその中を押す)。
	add(rand_int(250, 1200), [this, allow_items] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (menu == nullptr)
			return;
		// cordmux のメニューでなければ (コンテキストメニューのイベントを cordmux のウィジェットが受けずに
		// OBS のメインウィンドウまで伝わり、メインウィンドウのドックの一覧のメニュー
		// (QMainWindow::createPopupMenu) が開いた)、項目を選ばずに閉じる。選ぶとドックを隠してしまい、
		// その後の数が、表示されていないドックでの数になる。開いた回数と開いた所は記録する。
		if (!menu->objectName().startsWith(QLatin1String("cordmux"))) {
			++other_menus_;
			QWidget *f = QApplication::focusWidget();
			incident(QStringLiteral("other-menu"),
				 QStringLiteral("menu=%1/%2 items=%3 focus=%4/%5 %6")
					 .arg(QString::fromLatin1(menu->metaObject()->className()), menu->objectName())
					 .arg(menu->actions().size())
					 .arg(f != nullptr ? f->objectName() : QString(),
					      f != nullptr ? QString::fromLatin1(f->metaObject()->className()) : QString(), press_detail()));
			do_key(Qt::Key_Escape, Qt::NoModifier, QString());
			return;
		}
		++menus_opened_;
		menu_owner_ = pending_menu_owner_;
		// 製品が開いた行のメニューの人 (dock/row_menu.cpp の userId) と、テストが開いたつもりの人を比べる
		// (記録だけ。違えば、項目を押したときに (a) に数える。原因を分けるために、開いたクリックの記録を残す)。
		if (menu->objectName() == QLatin1String("cordmuxRowMenu")) {
			const std::string product_owner = menu->property("userId").toString().toStdString();
			if (product_owner != menu_owner_) {
				++menu_owner_differs_;
				incident(QStringLiteral("menu-owner"), QStringLiteral("product=%1 harness=%2 %3")
									       .arg(QString::fromStdString(product_owner).right(3),
										    QString::fromStdString(menu_owner_).right(3), press_detail()));
			}
		}
		std::vector<QAction *> items;
		for (QAction *a : menu->actions()) {
			if (!a->isSeparator() && a->isVisible() && a->isEnabled())
				items.push_back(a);
		}
		if (!allow_items || items.empty() || chance(0.55)) {
			do_key(Qt::Key_Escape, Qt::NoModifier, QString());
			return;
		}
		QAction *a = items[static_cast<std::size_t>(rand_int(0, static_cast<int>(items.size()) - 1))];
		const QRect r = menu->actionGeometry(a);
		in_.move_to(menu->mapToGlobal(r.center()));
		if (a->menu() != nullptr) {
			// サブメニュー (使う声): 開くのを待ってから中の項目を押す。開かなければ、Esc ですべて閉じる。
			QTimer::singleShot(rand_int(400, 700), sim_.dock(), [this] {
				auto *sub = qobject_cast<QMenu *>(QApplication::activePopupWidget());
				if (sub == nullptr)
					return;
				std::vector<QAction *> subs;
				for (QAction *s : sub->actions()) {
					if (!s->isSeparator() && s->isVisible() && s->isEnabled())
						subs.push_back(s);
				}
				if (subs.empty()) {
					do_key(Qt::Key_Escape, Qt::NoModifier, QString());
					return;
				}
				QAction *s = subs[static_cast<std::size_t>(rand_int(0, static_cast<int>(subs.size()) - 1))];
				in_.move_to(sub->mapToGlobal(sub->actionGeometry(s).center()));
				do_press(Qt::LeftButton);
				do_release(Qt::LeftButton);
			});
			return;
		}
		do_press(Qt::LeftButton);
		do_release(Qt::LeftButton);
	});
	// 閉じていないメニューがあれば Esc を押す (サブメニューを開いたまま押せなかったときなど)。
	add(900, [this] {
		for (int i = 0; i < 3 && QApplication::activePopupWidget() != nullptr; ++i)
			do_key(Qt::Key_Escape, Qt::NoModifier, QString());
	});
}

void Mistap::plan_right_click()
{
	// 参加者の行の右クリック。狙う行が見えていない (状態タブなどを開いていて、参加者の一覧が隠れている) か、見えている部分が
	// ドックの中にないときは、ポインターを動かすだけで押さない。plan_aim は狙いがないとドックの中央へ動かし、行の矩形は隠れて
	// いても求まるので、押すと参加者の行ではない物 (状態タブの数の入力欄の、Qt の標準のメニューなど) を押すことになる。この
	// テストの狙いは参加者の行の押し間違いなので、そこでは押さない (Qt の標準のメニューは入れ子のイベントループで開くので、
	// 右ボタンを離す入力がメニューの中に届き、押す前にボタンが押されたままの食い違いを数えたことがある)。
	auto aimed = std::make_shared<bool>(false);
	plan_aim([this, aimed] {
		*aimed = false;
		const std::vector<std::string> users = shown_users();
		if (users.empty())
			return QRect();
		dock::PeopleList *list = sim_.people();
		const std::string &u = users[static_cast<std::size_t>(rand_int(0, static_cast<int>(users.size()) - 1))];
		dock::PersonRowWidget *row = list != nullptr ? list->row_for(u) : nullptr;
		if (row == nullptr || !row->isVisible())
			return QRect();
		const QRect r = global_rect(row).intersected(viewport_rect());
		*aimed = !r.intersected(global_rect(sim_.widget())).isEmpty();
		return r;
	});
	add(0, [this, aimed] {
		if (*aimed)
			do_press(Qt::RightButton);
	});
	add(rand_int(40, 120), [this, aimed] {
		if (*aimed)
			do_release(Qt::RightButton);
	});
	plan_menu_handling(!quiet_);
}

void Mistap::plan_hover()
{
	const int n = rand_int(3, 12);
	for (int i = 0; i < n; ++i)
		add(rand_int(30, 250), [this] { in_.move_to(random_point_in(dock_rect())); });
}

void Mistap::plan_leave()
{
	add(rand_int(20, 200), [this] {
		const QRect d = dock_rect();
		in_.move_to(QPoint(d.right() + rand_int(30, 400), d.top() + rand_int(0, d.height())));
	});
}

void Mistap::plan_wheel()
{
	plan_aim([this] { return viewport_rect(); });
	const int n = rand_int(1, 3);
	const int dir = chance(0.5) ? 120 : -120;
	for (int i = 0; i < n; ++i)
		add(rand_int(40, 200), [this, dir] { do_wheel(dir); });
}

void Mistap::plan_keyboard(bool allow_activate)
{
	// フォーカスが一覧になければ、最初の行のチェックに置いてから始める (Tab で外から入る操作は、
	// keyboard のシナリオで確認する)。
	add(rand_int(50, 300), [this] {
		focus_departed_user_.clear();
		if (user_of(QApplication::focusWidget()).empty()) {
			const std::vector<std::string> users = shown_users();
			dock::PeopleList *list = sim_.people();
			if (!users.empty() && list != nullptr) {
				dock::PersonRowWidget *row = list->row_for(users.front());
				if (row != nullptr && row->focus_target() != nullptr)
					row->focus_target()->setFocus(Qt::TabFocusReason);
			}
		}
	});
	const int tabs = rand_int(1, 4);
	for (int i = 0; i < tabs; ++i) {
		add(rand_int(80, 400), [this] {
			focus_departed_user_.clear();
			do_key(Qt::Key_Tab, chance(0.3) ? Qt::ShiftModifier : Qt::NoModifier, QString());
		});
	}
	if (!allow_activate)
		return;
	const double r = rand01();
	if (r < 0.6) {
		add(rand_int(100, 900), [this] { do_key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")); });
	} else if (r < 0.8) {
		add(rand_int(100, 900), [this] { do_context_key(); });
		plan_menu_handling(true);
	}
}

void Mistap::plan_focus_leave_space()
{
	// フォーカスのある人を通話から抜けさせ、少し後に Space を押す ((c) を起こしやすくする)。
	plan_keyboard(false);
	add(rand_int(100, 500), [this] {
		const std::string u = user_of(QApplication::focusWidget());
		if (u.empty())
			return;
		for (const PersonFact &p : sim_.obs().people) {
			if (p.user_id == u && !p.is_receiver) {
				leave(u);
				sim_.changed();
				mutated();
				break;
			}
		}
	});
	add(rand_int(0, 1200), [this] { do_key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")); });
}

void Mistap::plan_shift_then_click()
{
	// 一覧を動かす変化 (人の出入り、名前の変更、メッセージ) の直後に、行へ素早く動かして押す ((a) と
	// (b) を起こしやすくする)。
	add(rand_int(20, 200), [this] { model_event(); });
	plan_click_target([this] {
		const std::vector<std::string> users = shown_users();
		if (users.empty())
			return QRect();
		const std::string &u = users[static_cast<std::size_t>(rand_int(0, static_cast<int>(users.size()) - 1))];
		return check_hit_rect_global(sim_.people(), u);
	});
}

void Mistap::plan_pounce()
{
	// 一覧が動いた直後に押す ((b) と (a) を起こす主な手順)。ポインターをスクロール領域の外 (下のバーの上か、
	// ドックの外) に置いて並びの保留を解き、人の出入りか名前の変更で一覧を動かし、行が動いたのを見たらすぐに
	// (0 から 400 ms で) 行のチェックへ動かして押す。並びの保留の間は一覧が動かないので、動くのはポインターが
	// 外にある間だけである。クリックを捨てるガードが働いていれば、動いてから 500 ms 以内のクリックは動作しない。
	auto started = std::make_shared<std::int64_t>(0);
	add(rand_int(20, 150), [this] {
		auto *bar = sim_.widget()->findChild<QWidget *>(QStringLiteral("cordmuxBottomBar"));
		if (bar != nullptr && chance(0.5))
			in_.move_to(bar->mapToGlobal(QPoint(bar->width() / 2, bar->height() - 2)));
		else
			plan_leave_now();
	});
	add(rand_int(50, 200), [this, started] {
		*started = now();
		const int roll = rand_int(0, 2);
		if (roll == 0)
			join();
		else if (roll == 1)
			leave(random_present(false, false));
		else {
			const std::string id = random_present(false, false);
			for (PersonFact &p : sim_.obs().people) {
				if (p.user_id == id) {
					const std::string name = kNames[rand_int(0, static_cast<int>(std::size(kNames)) - 1)];
					p.display_name = name;
					p.discord_name = name;
				}
			}
		}
		++event_counts_[QStringLiteral("pounce")];
		sim_.changed();
		mutated();
	});
	// 行が動くのを待つ (次の周期まで。最長 1.5 秒)。
	auto wait_step = std::make_shared<std::function<void()>>();
	*wait_step = [this, started, wait_step] {
		sample_positions();
		bool moved = false;
		for (const auto &[uid, t] : moved_at_) {
			(void)uid;
			moved = moved || t >= *started;
		}
		if (!moved && now() - *started < 1500)
			gesture_.push_front({20, *wait_step});
	};
	add(20, *wait_step);
	// すぐに動かして押す (1 回で動かし、止まってから 0 から 300 ms で押す)。
	auto goal = std::make_shared<QPoint>();
	add(rand_int(0, 100), [this, goal] {
		const std::vector<std::string> users = shown_users();
		if (users.empty())
			return;
		const std::string &u = users[static_cast<std::size_t>(rand_int(0, static_cast<int>(users.size()) - 1))];
		const QRect r = check_hit_rect_global(sim_.people(), u);
		*goal = r.isValid() ? random_point_in(r) : QPoint();
		if (!goal->isNull())
			in_.move_to(*goal);
	});
	add(rand_int(0, 300), [this, goal] {
		if (goal->isNull())
			return;
		do_press(Qt::LeftButton);
	});
	add(rand_int(40, 120), [this, goal] {
		if (goal->isNull())
			return;
		do_release(Qt::LeftButton);
	});
}

void Mistap::plan_leave_now()
{
	const QRect d = dock_rect();
	in_.move_to(QPoint(d.left() - rand_int(30, 400), d.top() + rand_int(0, d.height())));
}

void Mistap::plan_notice_button()
{
	plan_click_target([this] {
		std::vector<QPushButton *> buttons;
		for (QPushButton *b : sim_.widget()->findChildren<QPushButton *>()) {
			const QString name = b->objectName();
			if (b->isVisible() && b->isEnabled() && !name.startsWith(QLatin1String("cordmuxRow")) &&
			    name != QLatin1String("cordmuxRecordButton") && name != QLatin1String("cordmuxDockMenuButton"))
				buttons.push_back(b);
		}
		if (buttons.empty())
			return QRect();
		return global_rect(buttons[static_cast<std::size_t>(rand_int(0, static_cast<int>(buttons.size()) - 1))]);
	});
}

void Mistap::plan_dock_menu()
{
	plan_click_target([this] {
		auto *b = sim_.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"));
		return b != nullptr && b->isVisible() ? global_rect(b) : QRect();
	});
	// ドックのメニューは Esc で閉じるだけにする (ウィンドウを開く項目と機能を切る項目は、windows と
	// menus のシナリオで確認する)。
	plan_menu_handling(false);
}

void Mistap::plan_gesture()
{
	gesture_log_ << QStringLiteral("%1	depth=%2 popup=%3 modal=%4 focus=%5")
				.arg(now())
				.arg(depth_)
				.arg(QApplication::activePopupWidget() != nullptr ? 1 : 0)
				.arg(QApplication::activeModalWidget() != nullptr ? 1 : 0)
				.arg(QApplication::focusWidget() != nullptr ? QApplication::focusWidget()->objectName() : QString());
	if (QApplication::activePopupWidget() != nullptr) {
		add(100, [this] { do_key(Qt::Key_Escape, Qt::NoModifier, QString()); });
		return;
	}
	const int roll = rand_int(0, 99);
	gesture_log_.last() += QStringLiteral(" roll=%1 quiet=%2").arg(roll).arg(quiet_ ? 1 : 0);
	if (quiet_) {
		// 状態を変えない区間: 状態を変えない入力だけを流す。
		if (roll < 30)
			plan_hover();
		else if (roll < 45)
			plan_leave();
		else if (roll < 65)
			plan_wheel();
		else if (roll < 85)
			plan_keyboard(false);
		else if (roll < 95)
			plan_right_click();
		else
			plan_dock_menu();
		return;
	}
	// 重み (100 のうち): 行のチェックを押す 25、[...] 8、右クリック 7、ポインターを動かすだけ 8、外へ出る
	// 5、ホイール 6、キーボード 12、フォーカスのある人が抜けて Space 4、変化の直後に押す 3、動いた直後に押す
	// 14、メッセージのボタン 5、ドックのメニュー 3。
	const auto random_check = [this] {
		const std::vector<std::string> users = shown_users();
		if (users.empty())
			return QRect();
		const std::string &u = users[static_cast<std::size_t>(rand_int(0, static_cast<int>(users.size()) - 1))];
		return check_hit_rect_global(sim_.people(), u).intersected(viewport_rect());
	};
	if (roll < 25) {
		plan_click_target(random_check);
	} else if (roll < 33) {
		plan_click_target([this] {
			const std::vector<std::string> users = shown_users();
			if (users.empty())
				return QRect();
			const std::string &u = users[static_cast<std::size_t>(rand_int(0, static_cast<int>(users.size()) - 1))];
			return dots_rect_global(sim_.people(), u).intersected(viewport_rect());
		});
		plan_menu_handling(true);
	} else if (roll < 40) {
		plan_right_click();
	} else if (roll < 48) {
		plan_hover();
	} else if (roll < 53) {
		plan_leave();
	} else if (roll < 59) {
		plan_wheel();
	} else if (roll < 71) {
		plan_keyboard(true);
	} else if (roll < 75) {
		plan_focus_leave_space();
	} else if (roll < 78) {
		plan_shift_then_click();
	} else if (roll < 92) {
		plan_pounce();
	} else if (roll < 97) {
		plan_notice_button();
	} else {
		plan_dock_menu();
	}
}

void Mistap::schedule_poll()
{
	// 1 回ごとに、新しい単発のタイマーで次を呼ぶ。繰り返しの QTimer は、自分の処理の中 (メニューの
	// exec の入れ子のイベントループ) では次の timeout を出さない (Qt のタイマーは、自分の処理の最中に重ねて
	// 発火しない) ので、手順がメニューを開くと止まってしまう。次の 1 回を先に予約してから手順を実行すれば、
	// 入れ子のイベントループの中で次の手順が動く (script.h と同じ考え方)。
	if (!running_)
		return;
	QTimer::singleShot(10, sim_.dock(), [this] { poll(); });
}

void Mistap::poll()
{
	if (!running_)
		return;
	schedule_poll();
	const std::int64_t t = now();
	// 状態を変えない区間の始まりと終わり。
	if (!quiet_ && t >= next_quiet_at_) {
		quiet_ = true;
		quiet_start_ = t;
		quiet_end_ = t + 25000;
		quiet_measuring_ = false;
	}
	if (quiet_) {
		if (!quiet_measuring_ && t >= std::max(quiet_start_, last_mutation_ms_) + settle_ms_) {
			quiet_measuring_ = true;
			quiet_base_lines_ = drive_log_lines() + sim_.notice_log_lines();
			quiet_measure_start_ = t;
		}
		if (t >= quiet_end_) {
			if (quiet_measuring_) {
				const std::int64_t lines = drive_log_lines() + sim_.notice_log_lines() - quiet_base_lines_;
				w30_lines_ += lines;
				++quiet_windows_;
				quiet_measured_ms_ += t - quiet_measure_start_;
				if (lines != 0)
					incident(QStringLiteral("quiet-lines"), QStringLiteral("lines=%1 in quiet window").arg(lines));
			}
			quiet_ = false;
			next_quiet_at_ = t + 60000;
		}
	}
	// Observation の書き換え。
	if (!quiet_ && t >= next_model_at_) {
		model_event();
		const double mean = 1800.0;
		const int gap = static_cast<int>(std::exponential_distribution<double>(1.0 / mean)(rng_));
		next_model_at_ = t + std::clamp(gap, 100, 6000);
	}
	// 入力の手順。先に次を進めてから実行する。手順がメニューを開くと、次の手順は入れ子のイベントループの中で動く
	// (script.h と同じ)。
	if (!gesture_.empty() && t >= next_step_at_) {
		Step step = std::move(gesture_.front());
		gesture_.pop_front();
		next_step_at_ = t + (gesture_.empty() ? 0 : gesture_.front().delay_ms);
		if (gesture_.empty())
			next_gesture_at_ = t + rand_int(50, 700);
		step.fn();
		return;
	}
	if (gesture_.empty() && t >= next_gesture_at_ && depth_ == 0) {
		plan_gesture();
		next_step_at_ = t + (gesture_.empty() ? 0 : gesture_.front().delay_ms);
		stuck_since_ = -1;
	} else if (gesture_.empty() && depth_ > 0) {
		// 手順がなくなったのに、メニューかダイアログが開いたままのとき (キーボードの Space でドックのメニューの
		// [...] を押したときなど、手順が閉じ方を持たないもの)。1.5 秒待っても閉じなければ、利用者と同じく
		// Esc で閉じる。
		if (stuck_since_ < 0) {
			stuck_since_ = t;
		} else if (t - stuck_since_ > 1500) {
			stuck_since_ = -1;
			if (QApplication::activePopupWidget() != nullptr)
				do_key(Qt::Key_Escape, Qt::NoModifier, QString());
			else if (QWidget *modal = QApplication::activeModalWidget())
				modal->close();
		}
	}
}

void Mistap::run(Results &results)
{
	const QString group = QStringLiteral("mistap seed %1").arg(opt_.seed);
	settle_ms_ = sim_.obs().video_stall_threshold_us / 1000 + sim_.settings().first_frame_wait_ms +
		     sim_.settings().notice_show_delay_ms + sim_.settings().notice_clear_delay_ms + 1000;
	sim_.on_action = [this](const ActionRecord &rec) { return on_action(rec); };
	sim_.on_rename = [this](const std::string &u) { on_rename(u); };
	// 1920 x 1080 の画面の右寄りに、OBS の既定のドックの幅に近い 420 x 760 でフローティングにする。
	sim_.dock()->setGeometry(1300, 120, 420, 760);
	sim_.dock()->show();
	clock_.start();
	sim_.start();

	// 位置の測定: ドックのウィンドウの中のウィジェットが動いたら、イベントループの周回の最後に測る。
	struct Watcher : QObject {
		Mistap *self;
		QWidget *window;
		bool eventFilter(QObject *o, QEvent *e) override
		{
			switch (e->type()) {
			case QEvent::Move:
			case QEvent::Resize:
			case QEvent::Show:
			case QEvent::Hide:
				if (o->isWidgetType() && static_cast<QWidget *>(o)->window() == window) {
					// 測定の後に最初に届いた位置のイベントの時刻 (入れ替わりが起こりえた最も早い
					// 時刻)。アプリ全体のイベントフィルターは、ウィジェットのイベントフィルター
					// (製品の dock/people_list.cpp の eventFilter が、行が変わった時刻を付ける所)
					// より先に呼ばれる。
					if (self->first_event_t_ < 0)
						self->first_event_t_ = self->now();
					self->schedule_sample();
				}
				break;
			default:
				break;
			}
			return false;
		}
	};
	Watcher watcher;
	watcher.self = this;
	watcher.window = sim_.dock();
	qApp->installEventFilter(&watcher);

	QTimer sampler;
	sampler.setInterval(50);
	QObject::connect(&sampler, &QTimer::timeout, [this] {
		sample_positions();
		check_lines();
	});
	QEventLoop loop;
	QTimer end;
	end.setSingleShot(true);
	QObject::connect(&end, &QTimer::timeout, &loop, [&] {
		// 開いているメニューがあれば閉じてから終える。
		running_ = false;
		for (int i = 0; i < 4 && QApplication::activePopupWidget() != nullptr; ++i)
			in_.key(Qt::Key_Escape);
		loop.quit();
	});
	next_model_at_ = 1500;
	next_gesture_at_ = 500;
	running_ = true;
	schedule_poll();
	sampler.start();
	end.start(static_cast<int>(opt_.minutes * 60000.0));
	loop.exec();
	sampler.stop();
	qApp->removeEventFilter(&watcher);
	sim_.stop();
	sim_.dock()->hide();

	results.expect_eq(group, QStringLiteral("(a) 押した瞬間にポインターの下にあった行の人以外に適用された回数"), a_, 0,
			  QStringLiteral("うちメニューの項目 %1").arg(a_menu_));
	results.expect_eq(group, QStringLiteral("(b) 位置が変わってから 500 ms 以内のクリックが動作した回数"), b_, 0,
			  QStringLiteral("行のクリック %1 回のうち、押した位置の行が 500 ms 以内に入れ替わっていた %2 回。"
					 "その行の左上が 500 ms 以内に動いていたクリック %3 回のうち、動作した %4 回 (製品のガードの見方。記録だけ)")
				  .arg(row_presses_)
				  .arg(guarded_window_presses_)
				  .arg(row_moved_presses_)
				  .arg(b_row_));
	results.info(group, QStringLiteral("(b) 押した位置の行が 500 ms 以内に入れ替わっていたクリックの、届いた時点の製品のガードの答え (捨てる / 通す) と、"
					   "動作したクリックのうち境界のずれと判定した回数 (製品の行が変わった時刻が同じ入れ替わりにあり、経過が 500 ms 以上)"),
		     QStringLiteral("%1 / %2、境界のずれ %3").arg(guard_closed_).arg(guard_open_).arg(b_edge_),
		     QStringLiteral("クリックごとの記録は mistap-seed%1-guard.tsv。不具合の原因は incidents の why").arg(opt_.seed));
	results.expect_eq(group, QStringLiteral("(c) フォーカスのある人が抜けた後の Space が、別の人を載せ替えた回数"), c_, 0,
			  QStringLiteral("フォーカスのある人以外に適用されたキーの数 %1").arg(c_any_));
	results.expect_eq(group, QStringLiteral("(c') フォーカスのある人以外に適用されたキーの数 (すべて)"), c_any_, 0);
	results.expect_eq(group, QStringLiteral("(d) 1 行目が View と違う状態が 500 ms を超えた回数"), d_, 0);
	results.expect_eq(group, QStringLiteral("(e) 並びの保留が解けている間に、並びが View と違う状態が 500 ms を超えた回数"), e_, 0,
			  QStringLiteral("並びの保留の回数 %1").arg(hold_episodes_));
	results.expect_eq(group, QStringLiteral("状態を変えない区間に OBS のログに書かれた行の数"), w30_lines_, 0,
			  QStringLiteral("区間 %1、測った長さ %2 s (製品の diag::logf の行と、メッセージの表示と消去の行)")
				  .arg(quiet_windows_)
				  .arg(quiet_measured_ms_ / 1000));
	results.expect_eq(group, QStringLiteral("どのクリックとも結び付けられなかった操作"), unattributed_, 0);
	results.expect_eq(group, QStringLiteral("押す前にボタンが押されたままだったクリック (テストの入力の食い違い)"), stale_buttons_, 0);
	results.expect_eq(group, QStringLiteral("ドックが表示されていなかった確認の回数 (テストの入力がドックを隠した)"), dock_hidden_checks_, 0);
	results.info(group, QStringLiteral("原因を分けるための記録: 押す前に Qt がウィジェットを覚えていたクリック / 行のメニューの人が、開いたつもりの人と違った回数"),
		     QStringLiteral("%1 / %2").arg(stale_button_down_).arg(menu_owner_differs_));
	results.info(group, QStringLiteral("cordmux のものでないメニュー (OBS のメインウィンドウのドックの一覧) が開いた回数 (項目は選ばずに閉じる)"),
		     QString::number(other_menus_));
	results.expect_eq(group, QStringLiteral("押してから処理中の見た目になるまで 100 ms を超えたクリック"), busy_slow_ + busy_missed_, 0,
			  QStringLiteral("[Vesktopに追加] のボタンのクリック %1 回、最大 %2 ms (処理中のフラグは、テストが DockPoll と同じタイミングで立てる)")
				  .arg(busy_measured_)
				  .arg(busy_max_ms_));
	QStringList ev;
	for (const auto &[k, v] : event_counts_)
		ev << QStringLiteral("%1=%2").arg(k).arg(v);
	results.info(group, QStringLiteral("実行した量"),
		     QStringLiteral("クリック %1、行のクリック %2、動作した操作 %3 (うちメッセージのボタンからメッセージが指す人へ %8)、キー %4、メニュー %5、"
				    "Observation の書き換え %6、周期 %7")
			     .arg(presses_)
			     .arg(row_presses_)
			     .arg(effective_)
			     .arg(keys_)
			     .arg(menus_opened_)
			     .arg(model_events_)
			     .arg(sim_.ticks())
			     .arg(notice_person_actions_),
		     ev.join(QLatin1Char(' ')));
	const QString base = opt_.out_dir + QStringLiteral("/mistap-seed%1").arg(opt_.seed);
	write_file(base + QStringLiteral("-incidents.tsv"), QStringLiteral("t_ms\tkind\tdetail\n") + incidents_.join(QLatin1Char('\n')) +
							       QLatin1Char('\n'));
	write_file(base + QStringLiteral("-gestures.tsv"), gesture_log_.join(QLatin1Char('\n')) + QLatin1Char('\n'));
	// 製品のガードの答えの記録。経過はすべて、クリックがウィジェットに届いた時点から見た ms。
	QStringList guard;
	guard << QStringLiteral("t_press\tt_delivery\tunder\tbefore\tguard_user\tpart\tchange_seen_age\tchange_lo_age\tproduct_age\t"
				"product_blocked\theld\tresult\tverdict");
	for (const GuardRow &g : guard_rows_) {
		guard << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11\t%12\t%13")
				 .arg(g.t_press)
				 .arg(g.t_delivery)
				 .arg(QString::fromStdString(g.under).right(3), QString::fromStdString(g.before).right(3),
				      QString::fromStdString(g.guard_user).right(3), g.part)
				 .arg(g.seen_age)
				 .arg(g.lo_age)
				 .arg(g.product_age)
				 .arg(g.product_blocked ? 1 : 0)
				 .arg(g.held ? 1 : 0)
				 .arg(g.result, g.verdict);
	}
	write_file(base + QStringLiteral("-guard.tsv"), guard.join(QLatin1Char('\n')) + QLatin1Char('\n'));
	write_file(base + QStringLiteral("-actions.tsv"),
		   QStringLiteral("t_ms\taction\tacted\tvia\tresult\tunder\tmoved_age_ms\n") + action_log_.join(QLatin1Char('\n')) +
			   QLatin1Char('\n'));
}

} // namespace

void run_mistap(const MistapOptions &options, Results &results)
{
	Mistap m(options);
	m.run(results);
}

} // namespace cordmux::dock_drive

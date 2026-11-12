// SPDX-License-Identifier: Apache-2.0

// keyboard のシナリオ。キーボードだけでドックを操作できることを確認する。Tab でフォーカスが移る順とアクセシブル名、
// フォーカスリングの見え方とスクロール領域の端での切れ方、Space、キーボードで開く行のメニュー、マウスで押した後の
// フォーカスリング、押せなくなったチェックからのフォーカスの移り先、キーボードで開いたウィンドウを閉じた後の
// フォーカスの戻り先を、ブロックごとに (1) から (7) の番号で確認する。

#include "scenarios.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMetaEnum>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>
#include <QWindow>
#include <QGuiApplication>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dock/dock_widget.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/settings_window.h"
#include "dock/ui/focus_ring.h"
#include "drive_env.h"
#include "input.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "script.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

const QString kGroup = QStringLiteral("keyboard");

// 結果の表に書くウィジェットの名前 (空白を含まない 1 語)。行のウィジェットは、人の ID の末尾 2 文字と part_of にする。
QString name_of(const QWidget *w)
{
	if (w == nullptr)
		return QStringLiteral("(none)");
	const std::string u = user_of(w);
	const QString part = part_of(w);
	if (!u.empty())
		return QString::fromStdString(u).right(2) + QLatin1Char(':') + part;
	return w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName();
}

QScrollArea *scroll_of(dock::DockWidget *widget)
{
	return widget->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
}

// タブストップのアクセシブル名と役割。スクリーンリーダーがフォーカスのあるウィジェットで読み上げる名前
// (Qt の QAccessibleInterface の Name。UIA のブリッジが Windows のスクリーンリーダーに渡す文字列) を、
// ウィジェットの accessibleName ではなく QAccessibleInterface から読む。テキストを持つウィジェット
// (ボタンが表示しているテキスト) は accessibleName が空でも名前があり、名前を付け忘れたテキストのないウィジェット
// (名前のない cordmuxScroll など) は QAccessibleInterface からも空が返る (Qt 6.8.3 の qaccessiblewidget.cpp の
// QAccessibleWidget::text)。フォーカスがない (nullptr) か、QAccessibleInterface がないウィジェットは、
// 読み上げるものがないので空として数える。
// accessibleName を見る方法はとらない。テキストを持つボタンが空と判定され、スクリーンリーダーが実際に読み上げる名前と
// 合わないためである。
struct A11yStop {
	QString name; // アクセシブル名 (QAccessibleInterface の Name)
	QString role; // 役割 (記録だけ。例: スクロール領域は Client)
};

A11yStop a11y_stop_of(QWidget *w)
{
	A11yStop out;
	QAccessibleInterface *iface = w != nullptr ? QAccessible::queryAccessibleInterface(w) : nullptr;
	if (iface == nullptr) {
		out.role = w != nullptr ? QStringLiteral("(インターフェイスなし)") : QStringLiteral("(フォーカスなし)");
		return out;
	}
	out.name = iface->text(QAccessible::Name);
	const char *key = QMetaEnum::fromType<QAccessible::Role>().valueToKey(iface->role());
	out.role = key != nullptr ? QString::fromLatin1(key) : QString::number(static_cast<int>(iface->role()));
	return out;
}

// アクセシブル名が読み上げられるか (空白だけの名前は何も読み上げられないので、空と同じに数える)。
bool a11y_name_says_something(const QString &name)
{
	return !name.trimmed().isEmpty();
}

// アクセシブル名の読み方のセルフテスト。名前のないスクロール領域を作り、上の読み方が空と読むことを確認する。
// スクロール領域の accessibleName を一時的に空にして読み、元に戻してもう一度読む。空にしたスクロール領域に名前があると
// 読んだか、戻したスクロール領域の名前が前と違えば、その内容を返す。合っていれば空を返す。
// テストのためだけのウィジェットを作って読む方法はとらない。製品と同じウィジェットの種類 (QScrollArea) と、
// 製品のウィジェットのツリーの中での読み方を通らないためである。
QString a11y_name_selfcheck(QScrollArea *box)
{
	if (box == nullptr)
		return QStringLiteral("スクロール領域がない");
	const QString kept = box->accessibleName();
	const QString before = a11y_stop_of(box).name;
	box->setAccessibleName(QString());
	const A11yStop cleared = a11y_stop_of(box);
	box->setAccessibleName(kept);
	const QString after = a11y_stop_of(box).name;
	QStringList problems;
	if (a11y_name_says_something(cleared.name))
		problems << QStringLiteral("名前を消したスクロール領域を「%1」と読んだ (役割 %2)").arg(cleared.name, cleared.role);
	if (after != before)
		problems << QStringLiteral("戻したスクロール領域の名前が「%1」で、前の「%2」と違う").arg(after, before);
	return problems.join(QStringLiteral(" / "));
}

// ウィジェット w が、スクロール領域の表示範囲にすべて入っているか (スクロール領域の中のウィジェットだけを見る。
// 外のウィジェットには真を返す)。
bool fully_in_viewport(dock::DockWidget *widget, QWidget *w)
{
	QScrollArea *scroll = scroll_of(widget);
	if (scroll == nullptr || w == nullptr || !scroll->viewport()->isAncestorOf(w))
		return true;
	// 行のウィジェットは、行の全体が入っているかを見る (フォーカスに合わせたスクロールの確認)。
	QWidget *target = w;
	for (QWidget *p = w; p != nullptr && p != scroll; p = p->parentWidget()) {
		if (p->objectName() == QLatin1String("cordmuxRow")) {
			target = p;
			break;
		}
	}
	const QRect r(target->mapTo(scroll->viewport(), QPoint(0, 0)), target->size());
	return scroll->viewport()->rect().contains(r);
}

// Tab の順の中の、スクロール領域 (cordmuxScroll) の位置を確認する。製品はスクロール領域をタブストップのまま残す。
// スクロール領域の中が、押せるウィジェットのないカードとテキストだけになることがあり、そのときも、キーボードを使う人が
// スクロール領域にフォーカスを置いてスクロールできるようにするためである。確認すること: 1 周 (cycle。最後に先頭へ戻った
// 分を除いた並び) の中にスクロール領域がちょうど 1 回あり、スクロール領域の中のタブストップがすべて、
// スクロール領域に続いてひと続きに来る (Tab の向きでは直後から、Shift+Tab の向きでは直前まで)。つまりスクロール領域は、
// 中へ入る前の 1 回 (Tab) で、中から出た後の 1 回 (Shift+Tab) になる。テストデータによって中のタブストップの数が
// 変わっても、同じ形で確認する。合っていれば空を、違っていればその内容を返す。
// スクロール領域が Tab の順の先頭にあることを確認する方法はとらない。フォーカスのない状態から
// Tab で移る先はテストデータによって変わり、1 行目に押せるウィジェットがあるときは先頭にならないためである。
// 順の全体を固定の値と比べる方法もとらない。テストデータごとに書き直すことになり、スクロール領域の位置と関係のない行の
// 中の順まで確認してしまうためである。
QString box_position_problem(const std::vector<QWidget *> &cycle, QScrollArea *box, bool forward)
{
	if (box == nullptr || box->widget() == nullptr)
		return QStringLiteral("スクロール領域がない");
	std::vector<size_t> at;
	size_t inside = 0;
	for (size_t i = 0; i < cycle.size(); ++i) {
		if (cycle[i] == box)
			at.push_back(i);
		else if (cycle[i] != nullptr && box->widget()->isAncestorOf(cycle[i]))
			++inside;
	}
	if (at.size() != 1)
		return QStringLiteral("スクロール領域が 1 周に %1 回").arg(at.size());
	const size_t n = cycle.size();
	for (size_t k = 1; k <= inside; ++k) {
		const size_t idx = forward ? (at[0] + k) % n : (at[0] + n - k) % n;
		QWidget *w = cycle[idx];
		if (w == nullptr || !box->widget()->isAncestorOf(w))
			return QStringLiteral("スクロール領域から %1 回目がスクロール領域の外の %2 (中のタブストップ %3)").arg(k).arg(name_of(w)).arg(inside);
	}
	return QString();
}

// スクロール領域の前後のタブストップ (name_of の名前)。
QString box_neighbors(const std::vector<QWidget *> &cycle, QScrollArea *box)
{
	const auto it = std::find(cycle.begin(), cycle.end(), static_cast<QWidget *>(box));
	if (it == cycle.end() || cycle.empty())
		return QStringLiteral("(スクロール領域なし)");
	const size_t i = static_cast<size_t>(it - cycle.begin());
	const size_t n = cycle.size();
	return QStringLiteral("前 %1 / 後 %2").arg(name_of(cycle[(i + n - 1) % n]), name_of(cycle[(i + 1) % n]));
}

struct RingStats {
	int measured = 0;
	int not_visible = 0;
	int not_drawn = 0;
	int not_enclosing = 0;
	int too_wide = 0;
	int low_contrast = 0;
	int missing_side = 0;
	double min_p05 = 1e9;
	QStringList bad;
};

// フォーカスリングの辺ごとの集計を 1 語にする (左、上、右、下の順に、描かれた位置の数/辺の長さ)。
QString sides_text(const int side_len[4], const int side_cover[4])
{
	return QStringLiteral("L%1/%2 T%3/%4 R%5/%6 B%7/%8")
		.arg(side_cover[0])
		.arg(side_len[0])
		.arg(side_cover[1])
		.arg(side_len[1])
		.arg(side_cover[2])
		.arg(side_len[2])
		.arg(side_cover[3])
		.arg(side_len[3]);
}

// 辺の集計のセルフテスト。押せる範囲を 1 px 空けて 2 px で囲むフォーカスリングのピクセルを作り、
// ウィンドウの外のピクセルを除いて集計する。4 辺ともウィンドウの中なら 4、押せる範囲がウィンドウの幅いっぱい
// (cordmuxScroll のように、左右の辺がウィンドウの外) なら 2 になれば合っている。合わなければその内容を返す。
QString ring_sides_selfcheck()
{
	const QRect window(0, 0, 420, 380);
	const auto count = [&](const QRect &hit) {
		const QRect ring = hit.adjusted(-3, -3, 3, 3);
		std::vector<QPoint> pixels;
		for (int y = ring.top(); y <= ring.bottom(); ++y) {
			for (int x = ring.left(); x <= ring.right(); ++x) {
				const bool on = x <= ring.left() + 1 || x >= ring.right() - 1 || y <= ring.top() + 1 || y >= ring.bottom() - 1;
				if (on && window.contains(x, y))
					pixels.emplace_back(x, y);
			}
		}
		int len[4];
		int cover[4];
		count_ring_sides(pixels, hit, ring, len, cover);
		return std::make_pair(ring_sides_drawn(len, cover), sides_text(len, cover));
	};
	const auto whole = count(QRect(20, 40, 100, 30));
	const auto cut = count(QRect(0, 60, 420, 200));
	if (whole.first == 4 && cut.first == 2)
		return QString();
	return QStringLiteral("4 辺のリング %1 (%2)、左右が切れたリング %3 (%4)").arg(whole.first).arg(whole.second).arg(cut.first).arg(cut.second);
}

void check_ring(QWidget *top, QWidget *focused, RingStats &st, const QString &out_png)
{
	if (focused == nullptr)
		return;
	const RingMeasure m = measure_ring(top, focused);
	++st.measured;
	const QString who = name_of(focused);
	if (!m.ring_visible) {
		++st.not_visible;
		st.bad << who + QStringLiteral(":not-visible");
		return;
	}
	if (!m.drawn) {
		++st.not_drawn;
		st.bad << who + QStringLiteral(":not-drawn");
		return;
	}
	// フォーカスリングは、押せる範囲を 1 px 空けて 2 px の線で囲む。押せる範囲がスクロール領域の外に出ている所は
	// 切られるので、表示されている所で比べる。リングのピクセルの外接矩形は押せる範囲を含み、各辺で 3 px
	// (丸めの 1 px を加える) より外へ出ない。
	const QRect visible_hit = m.hit.intersected(m.bbox.adjusted(-3, -3, 3, 3));
	if (!m.bbox.adjusted(1, 1, -1, -1).contains(visible_hit.adjusted(1, 1, -1, -1))) {
		++st.not_enclosing;
		st.bad << who + QStringLiteral(":bbox(%1,%2 %3x%4)-hit(%5,%6 %7x%8)")
					.arg(m.bbox.x())
					.arg(m.bbox.y())
					.arg(m.bbox.width())
					.arg(m.bbox.height())
					.arg(m.hit.x())
					.arg(m.hit.y())
					.arg(m.hit.width())
					.arg(m.hit.height());
	}
	// フォーカスリングの 4 辺が、押せる範囲の辺の全長に描かれていること。上の比較はリングのピクセルの外接矩形で
	// 見るので、ドックの外で切れた辺は比較から漏れる。そのため、辺ごとに描かれた位置を数える。
	if (ring_sides_drawn(m.side_len, m.side_cover) != 4) {
		++st.missing_side;
		st.bad << who + QStringLiteral(":sides(%1)").arg(sides_text(m.side_len, m.side_cover));
	}
	if (m.bbox.left() < m.hit.left() - 4 || m.bbox.right() > m.hit.right() + 4 || m.bbox.top() < m.hit.top() - 4 ||
	    m.bbox.bottom() > m.hit.bottom() + 4) {
		++st.too_wide;
		st.bad << who + QStringLiteral(":wider-than-hit");
	}
	st.min_p05 = std::min(st.min_p05, m.contrast_p05);
	if (m.contrast_p05 < 3.0) {
		++st.low_contrast;
		st.bad << who + QStringLiteral(":contrast %1").arg(m.contrast_p05, 0, 'f', 2);
	}
	if (!out_png.isEmpty()) {
		QDir().mkpath(QFileInfo(out_png).absolutePath());
		m.with_ring.save(out_png, "PNG");
	}
}

} // namespace

void run_keyboard(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
	// 人の行に音量とミュートのコンポーネントを置かない形 (合成のソースがない) で、Tab の順を確認する。
	sim.obs().choices.composite_present = false;
	// 8 人がスクロール領域に収まらない高さ (フォーカスに合わせたスクロールを確認する)。
	sim.dock()->setGeometry(1300, 120, 420, 380);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.start();
	Script s;
	const QString png_dir = options.out_dir + QStringLiteral("/keyboard");

	std::vector<QWidget *> chain;
	QStringList chain_names;
	std::vector<A11yStop> chain_a11y; // タブストップに移った時点のアクセシブル名 (chain と同じ並び)
	int not_in_view = 0;
	QStringList not_in_view_names;
	RingStats rings;
	std::set<QString> shot_kinds;

	// (1) フォーカスがない状態から Tab を押し続け、フォーカスが移る順を記録する (1 周するまで。上限は 60 回)。
	s.then(600, [&] {
		if (QWidget *f = QApplication::focusWidget())
			f->clearFocus();
		input.move_to(QPoint(1800, 900)); // ポインターはドックの外
	});
	for (int i = 0; i < 60; ++i) {
		s.then(120, [&, i] {
			(void)i;
			if (!chain.empty() && chain.size() > 1 && chain.front() == chain.back())
				return; // 1 周した
			input.key(Qt::Key_Tab);
		});
		s.then(80, [&] {
			if (!chain.empty() && chain.size() > 1 && chain.front() == chain.back())
				return;
			QWidget *f = QApplication::focusWidget();
			chain.push_back(f);
			chain_names << name_of(f);
			// アクセシブル名は、フォーカスが移った時点で読む (スクリーンリーダーが読み上げるのはその
			// 時点で、後の手順で名前が変わるウィジェットもある)。
			chain_a11y.push_back(a11y_stop_of(f));
			if (!fully_in_viewport(sim.widget(), f)) {
				++not_in_view;
				not_in_view_names << name_of(f);
			}
			// フォーカスリングをすべてのウィジェットで測り、ウィジェットの種類ごとに最初の
			// 1 枚を画像に書き出す。
			const QString kind = part_of(f).isEmpty() ? (f != nullptr ? f->objectName() : QString()) : part_of(f);
			QString png;
			if (!kind.isEmpty() && shot_kinds.insert(kind).second)
				png = png_dir + QStringLiteral("/ring-") + kind + QStringLiteral(".png");
			check_ring(sim.dock(), f, rings, png);
		});
	}
	s.then(100, [&] {
		// (1) の判定: 1 周したか。行のチェック (押せるもの) と [...] のすべてが、行の並びの順に、
		// チェック、[...] の順で来たか。
		const bool cycled = chain.size() > 1 && chain.front() == chain.back();
		results.expect_true(kGroup, QStringLiteral("Tab でフォーカスが移る順が 1 周する (ドックの中で閉じる)"), cycled,
				    QStringLiteral("%1 回").arg(chain.size() - (cycled ? 1 : 0)));
		results.info(kGroup, QStringLiteral("Tab でフォーカスが移る順"), chain_names.join(QLatin1Char(' ')));
		std::vector<std::pair<std::string, QString>> row_stops;
		for (QWidget *w : chain) {
			const std::string u = user_of(w);
			if (!u.empty())
				row_stops.push_back({u, part_of(w)});
		}
		std::vector<std::pair<std::string, QString>> expected;
		for (const ledger::dockview::PersonRow &row : listed_rows(sim.view())) {
			// サブアカウントがない構成の Vesktop のアカウントの行も、自分の枠のチェックと [...] を持つ (person_row.cpp の
			// shows_menu)。
			const bool has_check = row.checkable || row.self_frame_check;
			const bool has_dots = row.checkable || row.menu.auto_add_only;
			// 枠が埋まっている間のチェックも、入れ替える相手のメニューを開くので押せる。
			if (has_check && (row.check_enabled || row.check_replaces))
				expected.push_back({row.user_id, QStringLiteral("check")});
			if (has_dots)
				expected.push_back({row.user_id, QStringLiteral("dots")});
		}
		if (cycled && !row_stops.empty() && row_stops.size() > expected.size())
			row_stops.resize(expected.size()); // 1 周目の分だけ
		results.expect_true(kGroup, QStringLiteral("行のウィジェットがすべて、上の順に来る (押せるチェック -> [...])"),
				    row_stops == expected,
				    QStringLiteral("来た %1 / 期待 %2").arg(row_stops.size()).arg(expected.size()));
		const auto reached = [&](const char *name) {
			return std::any_of(chain.begin(), chain.end(),
					   [&](QWidget *w) { return w != nullptr && w->objectName() == QLatin1String(name); });
		};
		results.expect_true(kGroup, QStringLiteral("録画のボタンに Tab で届く"), reached("cordmuxRecordButton"));
		results.expect_true(kGroup, QStringLiteral("ドックのメニューの [...] に Tab で届く"), reached("cordmuxDockMenuButton"));
		{
			// スクロール領域の位置 (Tab の向き)。1 周しなければ不合格にする。
			std::vector<QWidget *> cycle(chain.begin(), chain.end());
			if (cycled)
				cycle.pop_back();
			const QString problem = cycled ? box_position_problem(cycle, scroll_of(sim.widget()), true)
						       : QStringLiteral("1 周しない");
			results.expect_true(kGroup,
					    QStringLiteral("スクロール領域が、Tab の 1 周に 1 回だけタブストップとして来て、スクロール領域の中のタブストップが"
							   "すべて、その直後に続く"),
					    problem.isEmpty(),
					    problem.isEmpty() ? box_neighbors(cycle, scroll_of(sim.widget())) : problem);
		}
		{
			// Tab の 1 周のタブストップごとに 1 行の結果を書く。タブストップにアクセシブル名がないと、
			// ナレーターを使う人には、移った先で役割しか読み上げられない (名前を付けていない
			// cordmuxScroll がそうなる)。1 周 (最後に先頭へ戻った分を除く) のすべてを見る。
			// 1 周しなければ、移った先のすべてを見る (1 周しないことは上の結果で不合格になる)。
			// 結果の項目は順の番号と name_of の名前で、note の列に読んだ名前と役割を書く。
			// アクセシブル名のないタブストップの数を 1 行で数える方法はとらない。どのウィジェットに名前が
			// ないかが項目に出ず、前の結果と比べたときにタブストップの順の変化も見えないためである。
			// 名前の文字列そのものを期待する値と比べる方法もとらない。文言は ini で決まるので、
			// 文言を修正するたびにテストを書き直すことになり、名前があるかを確認するというこの結果の目的を
			// 超えるためである。
			const size_t stops = cycled ? chain.size() - 1 : chain.size();
			for (size_t i = 0; i < stops && i < chain_a11y.size(); ++i) {
				const A11yStop &a = chain_a11y[i];
				results.expect_true(kGroup,
						    QStringLiteral("Tab のタブストップ %1 (%2) にアクセシブル名がある")
							    .arg(static_cast<int>(i + 1), 2, 10, QLatin1Char('0'))
							    .arg(chain_names.value(static_cast<int>(i))),
						    a11y_name_says_something(a.name), QStringLiteral("名前「%1」、役割 %2").arg(a.name, a.role));
			}
			const QString problem = a11y_name_selfcheck(scroll_of(sim.widget()));
			results.expect_true(kGroup,
					    QStringLiteral("アクセシブル名の読み方のセルフテスト (名前を消したスクロール領域を空と読み、戻すと元の名前を読む)"),
					    problem.isEmpty(), problem);
		}
		results.expect_eq(kGroup, QStringLiteral("フォーカスのあるウィジェットがスクロール領域の表示範囲の外に残った回数 (フォーカスに合わせたスクロール)"), not_in_view, 0,
				  not_in_view_names.join(QLatin1Char(' ')));
		results.expect_eq(kGroup, QStringLiteral("キーボードでフォーカスを移したときにフォーカスリングが表示されなかったウィジェット"), rings.not_visible + rings.not_drawn, 0,
				  QStringLiteral("測った %1").arg(rings.measured));
		results.expect_eq(kGroup, QStringLiteral("フォーカスリングが押せる範囲を囲まなかったウィジェット"), rings.not_enclosing, 0);
		results.expect_eq(kGroup, QStringLiteral("フォーカスリングの 4 辺のどれかが、押せる範囲の辺の全長に描かれなかったウィジェット (ドックの外で切れた辺)"),
				  rings.missing_side, 0, QStringLiteral("測った %1").arg(rings.measured));
		{
			const QString problem = ring_sides_selfcheck();
			results.expect_true(kGroup, QStringLiteral("辺の集計のセルフテスト (4 辺のリングを 4、左右がウィンドウの外で切れたリングを 2 と数える)"),
					    problem.isEmpty(), problem);
		}
		results.expect_eq(kGroup, QStringLiteral("フォーカスリングが押せる範囲より広い (行の外周を囲む) ウィジェット"), rings.too_wide, 0);
		results.expect_eq(kGroup, QStringLiteral("フォーカスリングと背景のコントラスト比が 3:1 に届かないウィジェット"), rings.low_contrast, 0,
				  QStringLiteral("最小の 5 パーセンタイル %1").arg(rings.min_p05, 0, 'f', 2));
		if (!rings.bad.isEmpty())
			results.info(kGroup, QStringLiteral("フォーカスリングの不合格の内訳"), rings.bad.join(QLatin1Char(' ')));
	});

	// (7) スクロール領域の中で Tab と Shift+Tab でフォーカスが移ったとき、移った先のフォーカスリングが表示範囲の
	// 端で切れないこと。設定ウィンドウのページのシナリオ (scen_windows.cpp の pages の (8)) と同じ方法で確認する。
	// タブストップの順で隣り合う (a, b) について、b がスクロール領域の中にあれば、a にフォーカスを置き、
	// スクロールして b を表示範囲の上か下の端から 0、1、2 px の位置に置き (マウスのホイールでスクロールした後の
	// 状態。ウィジェットはすべて表示されている)、同じキーで b へ移る。移った b を追うフォーカスリングの
	// ウィジェットの矩形が、表示範囲に縦に収まっていれば合格。順は、Tab が (1) で記録した順、
	// Shift+Tab がフォーカスのない状態から Shift+Tab で 1 周した順 (Tab の逆順と同じとは限らない)。
	// 合否にするのはリングの縦だけにする。リングが横に切れていること (縦にスクロールしても直らない) と、
	// 行の全体が表示されていないこと ((1) の判定。表示されている必要があるのはウィジェットの全体で、
	// 行の全体ではない) は記録だけにする。リングの線の太さはここに書かず、リングのウィジェットの geometry を見る。
	// スクロールしてもその位置に置けない組み合わせと、表示範囲より高いウィジェットは、飛ばして数を記録する。
	// (1) の後、(2) の前に実行する ((2) 以降の手順は、名前を変え、行を作り直す)。確認する間だけドックを低くする
	// (下のコメント)。
	struct EdgeAcc {
		std::vector<QPointer<QWidget>> seq_tab;
		std::vector<QPointer<QWidget>> seq_backtab;
		QStringList miss_tab;
		QStringList miss_backtab;
		QStringList unset;           // a へのフォーカスかスクロールを設定できなかった (手順の不具合)
		std::set<QString> side;      // 記録だけ: リングが横に切れたウィジェット
		std::set<QString> row_cut;   // 記録だけ: 移った先のウィジェットの行が、表示範囲から縦にはみ出た位置
		std::set<int> pad;           // 記録だけ: 押せる範囲の上の辺から、リングの外側の上の辺までの px
		int checks_tab = 0;
		int checks_backtab = 0;
		int skipped = 0;
		int range = 0;               // 確認するときの高さでのスクロールの範囲 (px)
		int height_before = 0;       // 低くする前のドックの高さ (終わりに戻す)
		bool backtab_cycled = false;
	};
	auto edge = std::make_shared<EdgeAcc>();
	// ウィジェット f を追っているフォーカスリング (ドックのルートのウィジェットの子に
	// 1 つある。dock/ui/focus_ring.h)。
	auto ring_on = [&](QWidget *f) -> dock::ui::FocusRing * {
		if (f == nullptr)
			return nullptr;
		for (QWidget *w : sim.dock()->findChildren<QWidget *>(QStringLiteral("cordmuxFocusRing"))) {
			auto *r = dynamic_cast<dock::ui::FocusRing *>(w);
			if (r != nullptr && r->target() == f)
				return r;
		}
		return nullptr;
	};
	// 表示範囲 (スクロール領域の viewport) を、ウィジェット base の座標で返す。
	auto view_in = [&](QWidget *base) -> QRect {
		QScrollArea *sa = scroll_of(sim.widget());
		QWidget *vp = sa->viewport();
		return QRect(base->mapFromGlobal(vp->mapToGlobal(QPoint(0, 0))), vp->size());
	};
	// スクロール領域の中のウィジェット f のフォーカスリングが、表示範囲に縦に収まっているか。収まっていれば空を、
	// 外れていればその内容を返す。リングが表示範囲より高ければ、一部が表示されていればよい。横に切れていれば
	// 記録に加える。
	auto ring_cut = [&, edge](QWidget *f) -> QString {
		QScrollArea *sa = scroll_of(sim.widget());
		if (sa == nullptr || sa->widget() == nullptr || f == nullptr || !sa->widget()->isAncestorOf(f))
			return QString();
		dock::ui::FocusRing *r = ring_on(f);
		if (r == nullptr || !r->isVisible() || r->parentWidget() == nullptr)
			return QStringLiteral("リングが表示されていない");
		QWidget *base = r->parentWidget();
		const QRect view = view_in(base);
		const QRect ring = r->geometry();
		const QRect hit = dock::ui::focus_hit_rect_of(f);
		edge->pad.insert(base->mapFromGlobal(f->mapToGlobal(hit.topLeft())).y() - ring.top());
		if (ring.left() < view.left() || ring.right() > view.right())
			edge->side.insert(name_of(f) + QStringLiteral(" (リング x %1-%2 / 表示範囲 x 0-%3)")
							  .arg(ring.left() - view.left())
							  .arg(ring.right() - view.left())
							  .arg(view.width() - 1));
		const bool in = ring.height() > view.height() ? (ring.bottom() >= view.top() && ring.top() <= view.bottom())
							      : (ring.top() >= view.top() && ring.bottom() <= view.bottom());
		if (in)
			return QString();
		return QStringLiteral("リング %1-%2 / 表示範囲 0-%3")
			.arg(ring.top() - view.top())
			.arg(ring.bottom() - view.top())
			.arg(view.height() - 1);
	};
	auto scroll_text = [&]() -> QString {
		QScrollArea *sa = scroll_of(sim.widget());
		return sa != nullptr ? QStringLiteral("スクロール %1 / %2").arg(sa->verticalScrollBar()->value()).arg(sa->verticalScrollBar()->maximum())
				     : QStringLiteral("(スクロール領域なし)");
	};
	// 1 回の確認: ウィジェット b を表示範囲の端 (top なら上、そうでなければ下) から d px の位置に置き、
	// a にフォーカスを置いてから key で b へ移る。b がスクロール領域の外 (1 行目や下のバー) なら確認しない。
	auto edge_trial = [&, edge](int key, QWidget *a, QWidget *b, bool top, int d, QStringList &miss, int &checks) {
		QScrollArea *sa = scroll_of(sim.widget());
		if (sa == nullptr || sa->widget() == nullptr || b == nullptr || !sa->widget()->isAncestorOf(b))
			return;
		if (a == nullptr || !a->isVisible() || !b->isVisible()) {
			++edge->skipped;
			return;
		}
		QScrollBar *bar = sa->verticalScrollBar();
		const int view_h = sa->viewport()->height();
		const QRect r(b->mapTo(sa->widget(), QPoint(0, 0)), b->size());
		const int value = top ? r.top() - d : r.bottom() + 1 + d - view_h;
		if (r.height() > view_h || value < bar->minimum() || value > bar->maximum()) {
			++edge->skipped;
			return;
		}
		const QString where = QStringLiteral("%1 を%2の端から %3 px に置き %4 から")
					      .arg(name_of(b), top ? QStringLiteral("上") : QStringLiteral("下"))
					      .arg(d)
					      .arg(name_of(a));
		a->setFocus(Qt::TabFocusReason);
		pump(20);
		bar->setValue(value);
		pump(30);
		if (QApplication::focusWidget() != a || bar->value() != value) {
			edge->unset << where + QStringLiteral(" (フォーカス %1、%2)").arg(name_of(QApplication::focusWidget()), scroll_text());
			return;
		}
		input.key(key);
		pump(60);
		++checks;
		QWidget *f = QApplication::focusWidget();
		if (f != b) {
			miss << where + QStringLiteral(": 移った先のウィジェットが違う %1").arg(name_of(f));
			return;
		}
		if (!fully_in_viewport(sim.widget(), f))
			edge->row_cut.insert(name_of(f) + QStringLiteral(" (%1の端から %2 px)").arg(top ? QStringLiteral("上") : QStringLiteral("下")).arg(d));
		const QString cut = ring_cut(f);
		if (!cut.isEmpty())
			miss << where + QStringLiteral(": %1、%2").arg(cut, scroll_text());
	};
	// Tab の順は (1) で記録した順。Shift+Tab は、フォーカスのない状態から 1 周させる (上限は 60 回)。
	// (1) の高さ (380 px) では、スクロールの範囲が数 px しかなく (既定の組み合わせでは 0 から 3 px)、
	// 端に置けるウィジェットがほとんどない。確認する間だけドックを低くしてスクロールの範囲を作り、
	// 終わったら戻す (タブストップの順は高さで変わらない)。
	s.then(200, [&, edge] {
		for (QWidget *w : chain)
			edge->seq_tab.emplace_back(w);
		edge->height_before = sim.dock()->height();
		sim.dock()->resize(sim.dock()->width(), 240);
		pump(100);
		if (QScrollArea *sa = scroll_of(sim.widget()))
			edge->range = sa->verticalScrollBar()->maximum();
		if (QWidget *f = QApplication::focusWidget())
			f->clearFocus();
		pump(30);
		for (int k = 0; k < 60; ++k) {
			input.key(Qt::Key_Backtab);
			pump(30);
			QWidget *f = QApplication::focusWidget();
			edge->seq_backtab.emplace_back(f);
			if (edge->seq_backtab.size() > 1 && edge->seq_backtab.front().data() == f) {
				edge->backtab_cycled = true;
				break;
			}
		}
	});
	s.then(200, [&, edge] {
		const struct {
			int key;
			std::vector<QPointer<QWidget>> *seq;
			QStringList *miss;
			int *checks;
		} ways[] = {{Qt::Key_Tab, &edge->seq_tab, &edge->miss_tab, &edge->checks_tab},
			    {Qt::Key_Backtab, &edge->seq_backtab, &edge->miss_backtab, &edge->checks_backtab}};
		for (const auto &way : ways) {
			for (size_t i = 0; i + 1 < way.seq->size(); ++i) {
				for (const bool top : {true, false}) {
					for (int d = 0; d <= 2; ++d)
						edge_trial(way.key, (*way.seq)[i].data(), (*way.seq)[i + 1].data(), top, d, *way.miss, *way.checks);
				}
			}
		}
	});
	s.then(100, [&, edge] {
		auto miss_text = [](const QStringList &l) { return l.isEmpty() ? QString() : l.join(QStringLiteral(" | ")); };
		results.expect_true(kGroup, QStringLiteral("(7) 前提: ウィジェットを表示範囲の端に置いて、Tab と Shift+Tab で確認できたときがある"),
				    edge->checks_tab > 0 && edge->checks_backtab > 0 && edge->unset.isEmpty() && edge->backtab_cycled,
				    QStringLiteral("スクロールの範囲 %8 px で Tab %1 回、Shift+Tab %2 回 (Shift+Tab の順 %3 回%4)、スクロールしても置けずに飛ばした %5 回、手順を設定できなかった %6 回%7")
					    .arg(edge->checks_tab)
					    .arg(edge->checks_backtab)
					    .arg(edge->seq_backtab.size())
					    .arg(edge->backtab_cycled ? QString() : QStringLiteral("、1 周しない"))
					    .arg(edge->skipped)
					    .arg(edge->unset.size())
					    .arg(edge->unset.isEmpty() ? QString() : QStringLiteral("。") + miss_text(edge->unset))
					    .arg(edge->range));
		results.expect_eq(kGroup,
				  QStringLiteral("(7) スクロールしてウィジェットを表示範囲の上か下の端から 0-2 px に置き、1 つ前のタブストップから Tab で移る: "
						 "フォーカスリングが表示範囲に縦に収まる (外れた回数)"),
				  edge->miss_tab.size(), 0, QStringLiteral("%1 回のうち。%2").arg(edge->checks_tab).arg(miss_text(edge->miss_tab)));
		results.expect_eq(kGroup,
				  QStringLiteral("(7) 同じく Shift+Tab で移る: フォーカスリングが表示範囲に縦に収まる (外れた回数)"),
				  edge->miss_backtab.size(), 0,
				  QStringLiteral("%1 回のうち。%2").arg(edge->checks_backtab).arg(miss_text(edge->miss_backtab)));
		QStringList pads;
		for (int p : edge->pad)
			pads << QString::number(p);
		results.info(kGroup, QStringLiteral("(7) リングの外側の辺と押せる範囲の上の辺の間 (px。リングの線の太さと空きの和)"), QString::number(edge->pad.size()),
			     pads.join(QStringLiteral("、")));
		const QStringList sides(edge->side.begin(), edge->side.end());
		results.info(kGroup, QStringLiteral("(7) リングが横に切れたウィジェット (縦にスクロールしても直らない。合否にしない)"),
			     QString::number(sides.size()), sides.join(QStringLiteral(" | ")));
		const QStringList rows_cut(edge->row_cut.begin(), edge->row_cut.end());
		results.info(kGroup, QStringLiteral("(7) 移った先のウィジェットの行が、表示範囲から縦にはみ出た位置 (表示されている必要があるのはウィジェットの全体まで。合否にしない)"),
			     QString::number(rows_cut.size()), rows_cut.join(QStringLiteral(" | ")));
		// スクロール領域の位置 (Shift+Tab の向き。フォーカスのない状態から 1 周した順)。
		{
			std::vector<QWidget *> cycle;
			for (const QPointer<QWidget> &w : edge->seq_backtab)
				cycle.push_back(w.data());
			if (edge->backtab_cycled)
				cycle.pop_back();
			const QString problem = edge->backtab_cycled ? box_position_problem(cycle, scroll_of(sim.widget()), false)
								     : QStringLiteral("1 周しない");
			results.expect_true(kGroup,
					    QStringLiteral("スクロール領域が、Shift+Tab の 1 周に 1 回だけタブストップとして来て、スクロール領域の中のタブストップが"
							   "すべて、その直前に続く (中から出た次の 1 回)"),
					    problem.isEmpty(),
					    problem.isEmpty() ? box_neighbors(cycle, scroll_of(sim.widget())) : problem);
		}
		// スクロール領域をタブストップに残す理由そのものを確認する。スクロール領域にフォーカスがあるとき、
		// キーでスクロールできる (QAbstractScrollArea::keyPressEvent の PageDown、PageUp、下矢印、
		// 上矢印)。スクロールできなければ、タブストップに残す前提が成り立っていない
		// (製品の不具合)。スクロールの範囲があるこの高さ (240 px) で、値 0 から PageDown、PageUp、
		// 下矢印、上矢印の順に押し、毎回その向きに動き、フォーカスがスクロール領域から移らないこと。
		{
			QScrollArea *sa = scroll_of(sim.widget());
			QStringList bad;
			QStringList steps;
			if (sa == nullptr || sa->verticalScrollBar()->maximum() <= 0) {
				bad << QStringLiteral("スクロールの範囲がない");
			} else {
				QScrollBar *bar = sa->verticalScrollBar();
				bar->setValue(0);
				sa->setFocus(Qt::TabFocusReason);
				pump(30);
				const struct {
					int key;
					int dir; // 1 = 下へ、-1 = 上へ
					const char *name;
				} keys_in_order[] = {{Qt::Key_PageDown, 1, "PageDown"},
						     {Qt::Key_PageUp, -1, "PageUp"},
						     {Qt::Key_Down, 1, "Down"},
						     {Qt::Key_Up, -1, "Up"}};
				for (const auto &k : keys_in_order) {
					const int before = bar->value();
					input.key(k.key);
					pump(40);
					const int after = bar->value();
					steps << QStringLiteral("%1 %2->%3").arg(QString::fromLatin1(k.name)).arg(before).arg(after);
					if ((after - before) * k.dir <= 0)
						bad << QString::fromLatin1(k.name) + QStringLiteral(" で動かない");
					if (QApplication::focusWidget() != sa)
						bad << QString::fromLatin1(k.name) + QStringLiteral(" の後のフォーカス ") + name_of(QApplication::focusWidget());
				}
				bar->setValue(0);
			}
			results.expect_true(kGroup,
					    QStringLiteral("スクロール領域にフォーカスがあるとき、PageDown、PageUp、下矢印、上矢印でスクロールでき、フォーカスはスクロール領域に残る"),
					    bad.isEmpty(),
					    (bad.isEmpty() ? QString() : bad.join(QStringLiteral("、")) + QStringLiteral("。")) +
						    steps.join(QStringLiteral("、")) +
						    QStringLiteral(" (範囲 %1 px)").arg(sa != nullptr ? sa->verticalScrollBar()->maximum() : 0));
		}
		sim.dock()->resize(sim.dock()->width(), edge->height_before);
		pump(100);
	});

	// (2) Space: 押せるチェックごとに 2 回押す (載せるか外すかをして、元に戻す)。フォーカスのある人の
	// 操作だけが届くこと。
	struct SpaceStats {
		int tried = 0;
		int wrong = 0;
		int missing = 0;
		QStringList bad;
	};
	auto space = std::make_shared<SpaceStats>();
	s.then(200, [&, space] {
		dock::PeopleList *list = sim.people();
		if (list == nullptr)
			return;
		for (const std::string &u : list->shown_user_ids()) {
			dock::PersonRowWidget *row = list->row_for(u);
			// 枠が埋まっている間のチェックは、Space で入れ替える相手のメニューを開く (framework のシナリオが確認する)。
			if (row == nullptr || row->check() == nullptr || !row->check()->isEnabled() || row->view().check_replaces)
				continue;
			for (int k = 0; k < 2; ++k) {
				row->check()->setFocus(Qt::TabFocusReason);
				const std::size_t before = sim.actions().size();
				input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
				pump(80); // 0 ms の周期を実行して View を書き換える
				++space->tried;
				if (sim.actions().size() != before + 1) {
					++space->missing;
					space->bad << QString::fromStdString(u).right(2) + QStringLiteral(":none");
				} else if (sim.actions().back().args.user_id != u) {
					++space->wrong;
					space->bad << QString::fromStdString(u).right(2) + QStringLiteral(":other");
				}
				row = list->row_for(u);
				if (row == nullptr || row->check() == nullptr || !row->check()->isEnabled() || row->view().check_replaces)
					break;
			}
		}
		results.expect_eq(kGroup, QStringLiteral("Space がフォーカスのある人以外に適用された回数"), space->wrong, 0,
				  QStringLiteral("押した %1 回").arg(space->tried));
		results.expect_eq(kGroup, QStringLiteral("Space で何も起きなかった回数 (押せるチェック)"), space->missing, 0,
				  space->bad.join(QLatin1Char(' ')));
	});

	// (3) Menu キー (キーボードからのコンテキストメニュー) と [...] の Enter で行のメニューが開き、
	// Esc で閉じた後にフォーカスが戻ること。
	struct MenuStats {
		int opened = 0;
		int not_opened = 0;
		int wrong_place = 0;
		int focus_lost = 0;
		int ring_missing_after = 0;
		QStringList bad;
	};
	auto menus = std::make_shared<MenuStats>();
	auto menu_targets = std::make_shared<std::vector<std::pair<std::string, int>>>(); // (人, 0 = チェックで Menu キー / 1 = [...] で Enter)
	s.then(200, [&, menu_targets] {
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (!row.checkable)
				continue;
			if (row.check_enabled)
				menu_targets->push_back({row.user_id, 0});
			menu_targets->push_back({row.user_id, 1});
		}
	});
	auto current_focus = std::make_shared<QPointer<QWidget>>();
	auto current_user = std::make_shared<std::string>();
	for (int i = 0; i < 20; ++i) {
		s.then_blocking(150, [&, i, menu_targets, current_focus, current_user] {
			if (i >= static_cast<int>(menu_targets->size()))
				return;
			const auto &[u, how] = (*menu_targets)[static_cast<std::size_t>(i)];
			*current_user = u;
			dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(u) : nullptr;
			if (row == nullptr)
				return;
			QWidget *w = how == 0 ? static_cast<QWidget *>(row->check()) : static_cast<QWidget *>(row->dots());
			if (w == nullptr || !w->isEnabled())
				return;
			w->setFocus(Qt::TabFocusReason);
			*current_focus = w;
			if (how == 0)
				input.context_menu_key();
			else
				input.key(Qt::Key_Return); // メニューが exec で開くと、閉じるまでここから戻らない
		});
		s.then(250, [&, i, menu_targets, menus, current_user] {
			if (i >= static_cast<int>(menu_targets->size()))
				return;
			auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
			if (menu == nullptr) {
				++menus->not_opened;
				menus->bad << QString::fromStdString(*current_user).right(2) + QStringLiteral(":not-opened");
				return;
			}
			++menus->opened;
			// キーボードで開いたメニューは、その人の [...] の下に表示される (dock/people_list.cpp の
			// open_row_menu)。
			dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(*current_user) : nullptr;
			if (row != nullptr && row->dots() != nullptr) {
				const QPoint anchor = row->dots()->mapToGlobal(QPoint(0, row->dots()->height()));
				const QPoint at = menu->geometry().topLeft();
				if (std::abs(at.x() - anchor.x()) > 2 && std::abs(at.y() - anchor.y()) > 2) {
					++menus->wrong_place;
					menus->bad << QString::fromStdString(*current_user).right(2) + QStringLiteral(":place");
				}
			}
			input.key(Qt::Key_Escape);
		});
		s.then(150, [&, i, menu_targets, menus, current_focus, current_user] {
			if (i >= static_cast<int>(menu_targets->size()))
				return;
			if (QApplication::focusWidget() != current_focus->data()) {
				++menus->focus_lost;
				menus->bad << QString::fromStdString(*current_user).right(2) + QStringLiteral(":focus->") +
						      name_of(QApplication::focusWidget());
			}
			const RingMeasure m = measure_ring(sim.dock(), QApplication::focusWidget());
			if (!m.ring_visible)
				++menus->ring_missing_after;
		});
	}
	s.then(100, [&, menus, menu_targets] {
		results.expect_eq(kGroup, QStringLiteral("行のメニューがキーボードで開かなかった回数"), menus->not_opened, 0,
				  QStringLiteral("試した %1 (Menu キーと [...] の Enter)").arg(std::min<std::size_t>(menu_targets->size(), 20)));
		results.expect_eq(kGroup, QStringLiteral("キーボードで開いたメニューがその人の [...] の下に出なかった回数"), menus->wrong_place, 0);
		results.expect_eq(kGroup, QStringLiteral("メニューを閉じた後に、フォーカスが開く前のウィジェットへ戻らなかった回数"),
				  menus->focus_lost, 0, menus->bad.join(QLatin1Char(' ')));
		results.expect_eq(kGroup, QStringLiteral("メニューを閉じた後にフォーカスリングが消えた回数 (最後の入力はキーボード)"),
				  menus->ring_missing_after, 0);
	});

	// (4) マウスで押した後はフォーカスリングを表示しない。その後 Tab を押すと表示する。
	s.then(200, [&] {
		// 押す位置は枠の数のラベル (フォーカスを受けないウィジェット)。ボタンを押すと操作が実行されたり
		// メニューが開いたりするので避ける。
		auto *slots_label = sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxSlotsUsed"));
		if (slots_label != nullptr) {
			input.move_to(slots_label->mapToGlobal(slots_label->rect().center()));
			input.click();
		}
	});
	s.then(150, [&] {
		bool any_ring_visible = false;
		for (QWidget *w : sim.dock()->findChildren<QWidget *>(QStringLiteral("cordmuxFocusRing")))
			any_ring_visible = any_ring_visible || w->isVisible();
		results.expect_true(kGroup, QStringLiteral("マウスで押した後はフォーカスリングを表示しない"), !any_ring_visible,
				    QStringLiteral("フォーカス %1").arg(name_of(QApplication::focusWidget())));
		input.key(Qt::Key_Tab);
	});
	s.then(150, [&] {
		const RingMeasure m = measure_ring(sim.dock(), QApplication::focusWidget());
		results.expect_true(kGroup, QStringLiteral("その後 Tab を押すとフォーカスリングを表示する"), m.ring_visible,
				    name_of(QApplication::focusWidget()));
	});

	// (5) フォーカスのあるチェックが、空いている枠がなくなって押せなくなったとき、フォーカスが同じ人の
	// [...] へ移ること。Tab の順でチェックの次になるミュートのボタンがある行で確認する (合成のソースがある形)。
	auto r5_user = std::make_shared<std::string>();
	auto r5_freed = std::make_shared<std::string>();
	s.then(200, [&, r5_user, r5_freed] {
		// 載っている 1 人を外して空きを 1 つ作り、載っていない人のチェックを押せるようにする。
		ledger::Observation &obs = sim.obs();
		obs.choices.composite_present = true;
		for (ledger::PersonFact &p : obs.people) {
			if (p.slot.has_value() && r5_freed->empty()) {
				*r5_freed = p.user_id;
				p.slot.reset();
			}
		}
		sim.changed();
		sim.tick();
		pump(100);
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && !row.checked && row.check_enabled && row.user_id != *r5_freed && r5_user->empty())
				*r5_user = row.user_id;
		}
		dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(*r5_user) : nullptr;
		if (row != nullptr && row->check() != nullptr) {
			row->check()->setFocus(Qt::TabFocusReason);
			// 修飾キーはリングの切り替えに数えない。最後の入力はキーボードのまま (直前の Tab)。
			input.key(Qt::Key_Shift);
		}
	});
	s.then(200, [&, r5_user, r5_freed] {
		// 空きを外から埋める (外した人をもう一度載せる)。フォーカスのある人のチェックは、入れ替える相手のメニューを開くチェックになる。
		ledger::Observation &obs = sim.obs();
		for (ledger::PersonFact &p : obs.people) {
			if (p.user_id == *r5_freed)
				p.slot = 1;
		}
		// 外した人の枠の番号は 1 とは限らないので、空いている番号を探して入れる。
		std::set<int> used;
		for (const ledger::PersonFact &p : obs.people) {
			if (p.slot.has_value() && p.user_id != *r5_freed)
				used.insert(*p.slot);
		}
		for (int slot = 1; slot <= 4; ++slot) {
			if (used.count(slot) == 0) {
				for (ledger::PersonFact &p : obs.people) {
					if (p.user_id == *r5_freed)
						p.slot = slot;
				}
				break;
			}
		}
		sim.changed();
		sim.tick();
	});
	s.then(200, [&, r5_user] {
		dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(*r5_user) : nullptr;
		QWidget *f = QApplication::focusWidget();
		results.expect_true(kGroup, QStringLiteral("(5) 枠が埋まっても、フォーカスのあるチェックは入れ替えのために押せるままで、フォーカスは動かない"),
				    row != nullptr && row->check() != nullptr && row->check()->isEnabled() && row->view().check_replaces &&
					    f == row->check(),
				    QStringLiteral("フォーカス %1").arg(name_of(f)));
	});

	// (6) キーボードでウィンドウを開いて閉じた後、フォーカスが、ウィンドウを開いたウィジェットに戻ること。
	//   (6a) ドックのメニューの「設定」で開く設定ウィンドウを、Esc で閉じる。
	//        製品は、モーダルでない設定ウィンドウを、メインウィンドウを親ウィンドウにして前面に出す。
	//        ここでは dock/dock_poll_settings.cpp の DockPollSettingsPart::open_settings と同じ処理を行う。
	//   (6b) 行のメニューの「名前を変更...」で開く製品のダイアログ (dock/rename_dialog.h) に名前を入れ、Enter で
	//        閉じる。
	QPointer<QWidget> settings_window;
	sim.on_action = [&](const ActionRecord &rec) {
		if (rec.kind == ledger::IntentKind::OpenSettings && rec.ran) {
			dock::SettingsWindowInput in;
			in.current = sim.settings();
			in.effective_at_start = sim.settings();
			in.page = rec.args.page;
			in.about = sim.view().about;
			auto *w = dock::make_settings_window(main_window(), in);
			w->setAttribute(Qt::WA_DeleteOnClose, true);
			w->move(80, 60);
			w->show();
			w->raise();
			Input::activate_window(w);
			settings_window = w;
			return true;
		}
		return false;
	};
	auto dock_menu_button = [&] { return sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton")); };
	s.then_blocking(300, [&] {
		QPushButton *b = dock_menu_button();
		if (b == nullptr)
			return;
		input.activate(sim.dock());
		b->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")); // ドックのメニュー (exec)
	});
	s.then(300, [&] {
		// 最初の項目 (設定) を Enter で選ぶ。
		input.key(Qt::Key_Down);
		input.key(Qt::Key_Return);
	});
	s.then(400, [&] {
		results.expect_true(kGroup, QStringLiteral("(6a) キーボードで、ドックのメニューから設定ウィンドウが開く"),
				    !settings_window.isNull() && settings_window->isVisible());
		input.key(Qt::Key_Escape);
	});
	s.then(300, [&] {
		QPushButton *b = dock_menu_button();
		results.expect_true(kGroup, QStringLiteral("(6a) 設定ウィンドウを Esc で閉じた後、フォーカスがドックの [...] に戻る"),
				    (settings_window.isNull() || !settings_window->isVisible()) && QApplication::focusWidget() == b,
				    QStringLiteral("フォーカス %1 (フォーカスのあるウィンドウ %2)")
					    .arg(name_of(QApplication::focusWidget()),
						 QGuiApplication::focusWindow() != nullptr ? QGuiApplication::focusWindow()->objectName()
											   : QStringLiteral("(none)")));
		// (6b) の前に、「名前を変更...」で製品のダイアログを表示するようにする (テストの既定では表示しない)。
		sim.use_product_rename_dialog();
	});
	auto rename_user = std::make_shared<std::string>();
	auto rename_opener = std::make_shared<QPointer<QWidget>>();
	s.then_blocking(300, [&, rename_user, rename_opener] {
		input.activate(sim.dock());
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && row.check_enabled && rename_user->empty())
				*rename_user = row.user_id;
		}
		dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(*rename_user) : nullptr;
		if (row == nullptr || row->check() == nullptr)
			return;
		row->check()->setFocus(Qt::TabFocusReason);
		*rename_opener = row->check();
		input.context_menu_key(); // 行のメニュー (exec)
	});
	s.then_blocking(300, [&] {
		// 「名前を変更...」を下の矢印キーで選び、Enter で押す。ダイアログが開く (閉じるまで戻らない)。上には
		// [自動で枠に載せる] と [全画面で表示] があり、押せない項目を矢印キーが飛ばすかはスタイルで決まるので、
		// 選ばれた項目を見ながら進める。
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		for (int i = 0; i < 6; ++i) {
			input.key(Qt::Key_Down);
			if (menu != nullptr && menu->activeAction() != nullptr &&
			    menu->activeAction()->objectName() == QLatin1String("cordmuxRowRename"))
				break;
		}
		input.key(Qt::Key_Return);
	});
	s.then(400, [&] {
		// ダイアログは、操作を始めた範囲 (ドック) だけを止める (アプリケーション全体のモーダルではない) ので、
		// トップレベルのウィジェットの一覧からも探す。
		QWidget *modal = QApplication::activeModalWidget();
		for (QWidget *w : QApplication::topLevelWidgets()) {
			if (modal == nullptr && w->isVisible() && w->objectName() == QLatin1String("cordmuxRenameDialog"))
				modal = w;
		}
		results.expect_true(kGroup, QStringLiteral("(6b) キーボードで「名前を変更」のダイアログが開く"), modal != nullptr,
				    modal != nullptr ? modal->objectName() : QString());
		results.expect_true(kGroup, QStringLiteral("(6b) 開いたダイアログがアクティブで、キーがそのダイアログへ届く"),
				    modal != nullptr && QApplication::activeWindow() == modal,
				    QApplication::activeWindow() != nullptr ? QApplication::activeWindow()->objectName() : QStringLiteral("(none)"));
		for (const QChar ch : QStringLiteral("drive")) {
			input.key(Qt::Key_A + (ch.unicode() - 'a'), Qt::NoModifier, QString(ch));
		}
		input.key(Qt::Key_Return);
	});
	s.then(400, [&, rename_user, rename_opener] {
		bool renamed = false;
		for (const ActionRecord &a : sim.actions()) {
			if (a.id == ledger::ActionId::SetNameOverride && a.args.user_id == *rename_user && a.args.name == "drive")
				renamed = true;
		}
		results.expect_true(kGroup, QStringLiteral("(6b) 入れた名前がその人の SetNameOverride になる"), renamed);
		results.expect_true(kGroup, QStringLiteral("(6b) ダイアログを閉じた後、フォーカスがメニューを開いたウィジェットに戻る"),
				    QApplication::focusWidget() == rename_opener->data(),
				    QStringLiteral("%1 / タブ %2").arg(name_of(QApplication::focusWidget())).arg(static_cast<int>(sim.widget()->tab())));
	});

	const bool finished = s.run(240000);
	results.expect_true(kGroup, QStringLiteral("手順が時間内に終わった"), finished);
	sim.stop();
	sim.dock()->hide();
}

} // namespace cordmux::dock_drive

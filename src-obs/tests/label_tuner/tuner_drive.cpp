// SPDX-License-Identifier: Apache-2.0

// tuner_drive.h の実装。

#include "tuner_drive.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sources/composite_label_plan.h"
#include "sources/label_border.h"
#include "tuner_window.h"

namespace cordmux::label_tuner {

namespace {

namespace sources = cordmux::sources;

// [プッシュ・トゥ・トーク] を押すのは人 4 (見本の右上の枠の人)、「2 秒話して 1 秒黙る」を入れるのは人 2。ほかの手順では
// この 2 人を話させない (明るさの期待値を、押下と離しの時刻だけから求めるため)。
constexpr int kHoldSeat = 3;
constexpr int kCycleSeat = 1;

// 手順を入れ始める前に、初めの描き直しを待つ時間 (ms)。
constexpr int kStartDelayMs = 1000;

// 全体の上限 (ms)。これを過ぎたら確認を打ち切り、終了コード 1 で終える。
constexpr int kWatchdogMs = 60000;

constexpr std::uint64_t kSecondNs = 1'000'000'000ULL;

std::int64_t steady_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

// 操作の時刻。操作の関数の中で読んだ時計の値は、操作の前 (lo) と後 (hi) の間にある。
struct Moment {
	bool known = false;
	std::uint64_t lo = 0;
	std::uint64_t hi = 0;
};

// 利用者がボタンを押すのと同じく、マウスの押下と離しのイベントをウィジェットの真ん中に送る。
void send_mouse(QWidget *widget, QEvent::Type type)
{
	const QPointF at = QRectF(widget->rect()).center();
	const Qt::MouseButtons buttons = type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton;
	QMouseEvent event(type, at, widget->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
	QCoreApplication::sendEvent(widget, &event);
}

// 始まり (true) と止まり (false) の時刻の並びから、時刻 t の明るさを、合成のソースと同じ関数で求める。
double level_from(const std::vector<std::pair<std::uint64_t, bool>> &events, std::uint64_t t,
		  const sources::BorderTiming &timing)
{
	sources::SpeakingTimes times;
	for (const auto &[at, start] : events) {
		if (at > t)
			break;
		times = start ? sources::merge_speaking(times, at, 0, timing)
			      : sources::merge_speaking(times, 0, at, timing);
	}
	return sources::composite_speaking_level(times, t, timing, true, false, 1.0f);
}

// 「2 秒話して 1 秒黙る」を start に入れ、stop (0 なら切らない) に切ったときの始まりと止まり。
std::vector<std::pair<std::uint64_t, bool>> cycle_events(std::uint64_t start, std::uint64_t stop, std::uint64_t until)
{
	std::vector<std::pair<std::uint64_t, bool>> events;
	for (std::uint64_t begin = start; begin <= until; begin += 3 * kSecondNs) {
		if (stop != 0 && begin >= stop)
			break;
		events.emplace_back(begin, true);
		const std::uint64_t end = begin + 2 * kSecondNs;
		if (stop != 0 && end >= stop) {
			events.emplace_back(stop, false);
			break;
		}
		events.emplace_back(end, false);
	}
	return events;
}

struct TickRecord {
	std::int64_t steady = 0;
	TunerWindow::TickInfo info;
	int step = 0; // この tick の前に入れた手順の数
};

class Drive {
public:
	Drive(TunerWindow &window, QString dir, std::function<std::uint64_t()> clock,
	      std::function<bool(const QString &, const QJsonObject &)> write_shot)
		: window_(window),
		  dir_(std::move(dir)),
		  clock_(std::move(clock)),
		  write_shot_(std::move(write_shot))
	{
		build_steps();
	}

	int run()
	{
		started_ = steady_ns();
		window_.set_tick_observer([this](const TunerWindow::TickInfo &info) {
			ticks_.push_back(TickRecord{steady_ns(), info, next_step_});
			max_full_compose_ms_ = std::max(max_full_compose_ms_, window_.full_compose_ms());
			max_crop_compose_ms_ = std::max(max_crop_compose_ms_, window_.crop_compose_ms());
		});
		QTimer::singleShot(kWatchdogMs, &window_, [this]() {
			std::fprintf(stderr, "label_tuner: --drive が %d ms で終わりませんでした\n", kWatchdogMs);
			window_.set_tick_observer(nullptr);
			QCoreApplication::exit(1);
		});
		QTimer::singleShot(kStartDelayMs, &window_, [this]() { run_next(); });
		const int code = QCoreApplication::exec();
		window_.set_tick_observer(nullptr);
		return code;
	}

private:
	struct Step {
		int delay_ms = 0;
		QString label;
		std::function<void()> act;
	};

	void add(int delay_ms, const QString &label, std::function<void()> act)
	{
		steps_.push_back(Step{delay_ms, label, std::move(act)});
	}

	Moment timed(const std::function<void()> &act)
	{
		Moment m;
		m.lo = clock_();
		act();
		m.hi = clock_();
		m.known = true;
		return m;
	}

	void build_steps()
	{
		TunerWindow &w = window_;
		// フォントサイズのスライダーを 30 ms ごとに動かし続ける (描き直しの仕事が途中で入れ替わる)。
		for (int k = 0; k < 30; ++k) {
			const double v = 2.5 + 0.2 * k;
			add(30, QStringLiteral("フォントサイズ %1 (スライダー)").arg(v),
			    [&w, v]() { w.set_knob_number("label_em_ratio_pct", v, true); });
		}
		add(30, QStringLiteral("フォントサイズ 2.2 (数の欄)"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 2.2, false); });
		// 重い値に動かした直後に軽い値へ戻す。
		add(40, QStringLiteral("フォントサイズ 8.5"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 8.5, true); });
		add(40, QStringLiteral("ぼかし 3.0"),
		    [&w]() { w.set_knob_number("label_glow_blur_ratio", 3.0, false); });
		add(40, QStringLiteral("太さ 1.0"),
		    [&w]() { w.set_knob_number("label_glow_spread_ratio", 1.0, true); });
		add(40, QStringLiteral("対象の [枠] を入れる (押す)"), [&w]() { click(w.knob_check("label_glow_frame")); });
		add(60, QStringLiteral("フォントサイズ 2.2"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 2.2, false); });
		add(40, QStringLiteral("ぼかし 0.1"),
		    [&w]() { w.set_knob_number("label_glow_blur_ratio", 0.1, true); });
		add(40, QStringLiteral("太さ 0.1"),
		    [&w]() { w.set_knob_number("label_glow_spread_ratio", 0.1, false); });
		add(40, QStringLiteral("対象の [枠] を切る (押す)"), [&w]() { click(w.knob_check("label_glow_frame")); });
		// 重い値のまま描き終えさせ、人 1 を話させて、重い光を重ねる仕事を続けて走らせる (全体を重ねるのに 1 枚
		// 0.1 秒ほどかかる間も、画面のスレッドが止まらないか)。
		add(40, QStringLiteral("重い値: フォントサイズ 8.5"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 8.5, false); });
		add(40, QStringLiteral("重い値: ぼかし 3.0"),
		    [&w]() { w.set_knob_number("label_glow_blur_ratio", 3.0, true); });
		add(40, QStringLiteral("重い値: 太さ 1.0"),
		    [&w]() { w.set_knob_number("label_glow_spread_ratio", 1.0, false); });
		add(40, QStringLiteral("重い値: 対象の [枠] を入れる (押す)"),
		    [&w]() { click(w.knob_check("label_glow_frame")); });
		add(1200, QStringLiteral("人 1 の [話している] を入れる (押す)"), [&w]() { click(w.speak_box(0)); });
		add(700, QStringLiteral("人 1 の [話している] を切る (押す)"), [&w]() { click(w.speak_box(0)); });
		add(700, QStringLiteral("軽い値: フォントサイズ 2.2"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 2.2, true); });
		add(40, QStringLiteral("軽い値: ぼかし 0.1"),
		    [&w]() { w.set_knob_number("label_glow_blur_ratio", 0.1, false); });
		add(40, QStringLiteral("軽い値: 太さ 0.1"),
		    [&w]() { w.set_knob_number("label_glow_spread_ratio", 0.1, true); });
		add(40, QStringLiteral("軽い値: 対象の [枠] を切る (押す)"),
		    [&w]() { click(w.knob_check("label_glow_frame")); });
		// 見るタイル、背景、背景の表示、人数。
		for (const int tile : {0, 1, 2, 3, 1})
			add(50, QStringLiteral("見るタイル %1").arg(tile), [&w, tile]() { w.set_view_tile(tile); });
		for (const int choice : {1, 2, 3, 4, 0})
			add(60, QStringLiteral("背景 %1").arg(choice),
			    [&w, choice]() { w.set_background_choice(choice); });
		add(40, QStringLiteral("背景の [表示] を入れる (押す)"), [&w]() { click(w.knob_check("label_show_plate")); });
		add(200, QStringLiteral("背景の [表示] を切る (押す)"), [&w]() { click(w.knob_check("label_show_plate")); });
		for (const int people : {2, 1, 3, 4})
			add(80, QStringLiteral("人数 %1").arg(people), [&w, people]() { w.set_people(people); });
		// 話す。人 4 は押している間だけ話し、人 2 は「2 秒話して 1 秒黙る」を入れて 3.6 秒後に切る。その間にも
		// つまみを動かす。
		add(300, QStringLiteral("人 4 の [プッシュ・トゥ・トーク] を押す"), [this, &w]() {
			hold_press_ = timed([&w]() { send_mouse(w.hold_button(kHoldSeat), QEvent::MouseButtonPress); });
		});
		add(700, QStringLiteral("人 4 の [プッシュ・トゥ・トーク] を離す"), [this, &w]() {
			hold_release_ =
				timed([&w]() { send_mouse(w.hold_button(kHoldSeat), QEvent::MouseButtonRelease); });
		});
		add(200, QStringLiteral("人 2 の [2 秒話して 1 秒黙る] を入れる (押す)"),
		    [this, &w]() { cycle_on_ = timed([&w]() { click(w.cycle_box(kCycleSeat)); }); });
		add(500, QStringLiteral("フォントサイズ 3.0 (話している間)"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 3.0, false); });
		add(500, QStringLiteral("フォントサイズ 2.2 (話している間)"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 2.2, true); });
		add(2600, QStringLiteral("人 2 の [2 秒話して 1 秒黙る] を切る (押す)"),
		    [this, &w]() { cycle_off_ = timed([&w]() { click(w.cycle_box(kCycleSeat)); }); });
		// 終わりの状態。既定と違う値にしておき、その場で描いた画像と比べる。
		add(600, QStringLiteral("フォントサイズ 3.0"),
		    [&w]() { w.set_knob_number("label_em_ratio_pct", 3.0, false); });
		add(40, QStringLiteral("背景の [表示] を入れる (押す)"), [&w]() { click(w.knob_check("label_show_plate")); });
		add(40, QStringLiteral("人数 3"), [&w]() { w.set_people(3); });
		add(40, QStringLiteral("背景 2"), [&w]() { w.set_background_choice(2); });
		add(40, QStringLiteral("見るタイル 1"), [&w]() { w.set_view_tile(1); });
	}

	static void click(QAbstractButton *button)
	{
		send_mouse(button, QEvent::MouseButtonPress);
		send_mouse(button, QEvent::MouseButtonRelease);
	}

	void run_next()
	{
		if (next_step_ >= static_cast<int>(steps_.size())) {
			last_step_at_ = steady_ns();
			wait_settle();
			return;
		}
		const Step &step = steps_[static_cast<std::size_t>(next_step_)];
		step_times_.push_back(static_cast<double>(steady_ns() - started_) / 1e6);
		step.act();
		++next_step_;
		const int delay = next_step_ < static_cast<int>(steps_.size())
					  ? steps_[static_cast<std::size_t>(next_step_)].delay_ms
					  : 0;
		QTimer::singleShot(delay, &window_, [this]() { run_next(); });
	}

	void wait_settle()
	{
		const double waited = static_cast<double>(steady_ns() - last_step_at_) / 1e6;
		if (window_.all_current()) {
			settle_ms_ = waited;
			finish(true);
			return;
		}
		if (waited > kDriveSettleLimitMs) {
			settle_ms_ = waited;
			finish(false);
			return;
		}
		QTimer::singleShot(20, &window_, [this]() { wait_settle(); });
	}

	void finish(bool settled)
	{
		window_.set_tick_observer(nullptr);
		const long long mismatch = settled ? window_.fresh_mismatch() : -1;

		// 画面のスレッドの tick の間隔。手順を入れ始めてから描き終えるまで。
		std::vector<double> gaps;
		double worst = 0.0;
		int worst_step = 0;
		for (std::size_t i = 1; i < ticks_.size(); ++i) {
			const double gap = static_cast<double>(ticks_[i].steady - ticks_[i - 1].steady) / 1e6;
			gaps.push_back(gap);
			if (gap > worst) {
				worst = gap;
				worst_step = ticks_[i].step;
			}
		}
		std::vector<double> sorted = gaps;
		std::sort(sorted.begin(), sorted.end());
		const double p99 = sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
		const double run_ms = static_cast<double>(steady_ns() - started_) / 1e6;

		// 明るさ。操作の前と後の時計の値の組み合わせで期待値の幅を求め、表示に使った明るさがその幅に入るかを見る。
		sources::BorderTiming timing; // 手順は時間のつまみを動かさないので、製品の既定値
		const auto check = [&](int seat,
				       const std::function<std::vector<std::pair<std::uint64_t, bool>>(int)> &events,
				       int combos, int *checked, int *wrong, double *widest) {
			for (const TickRecord &record : ticks_) {
				if (!record.info.seat_shown[static_cast<std::size_t>(seat)])
					continue;
				double low = 2.0;
				double high = -1.0;
				for (int c = 0; c < combos; ++c) {
					const double v = level_from(events(c), record.info.now_ns, timing);
					low = std::min(low, v);
					high = std::max(high, v);
				}
				const double shown = record.info.seat_level[static_cast<std::size_t>(seat)];
				++*checked;
				*widest = std::max(*widest, high - low);
				if (shown < low - 1e-9 || shown > high + 1e-9)
					++*wrong;
			}
		};
		int hold_checked = 0;
		int hold_wrong = 0;
		double hold_widest = 0.0;
		check(
			kHoldSeat,
			[this](int c) {
				std::vector<std::pair<std::uint64_t, bool>> events;
				if (hold_press_.known)
					events.emplace_back((c & 1) ? hold_press_.hi : hold_press_.lo, true);
				if (hold_release_.known)
					events.emplace_back((c & 2) ? hold_release_.hi : hold_release_.lo, false);
				return events;
			},
			4, &hold_checked, &hold_wrong, &hold_widest);
		int cycle_checked = 0;
		int cycle_wrong = 0;
		double cycle_widest = 0.0;
		const std::uint64_t until = clock_();
		check(
			kCycleSeat,
			[this, until](int c) {
				if (!cycle_on_.known)
					return std::vector<std::pair<std::uint64_t, bool>>();
				const std::uint64_t on = (c & 1) ? cycle_on_.hi : cycle_on_.lo;
				const std::uint64_t off = !cycle_off_.known ? 0
									    : ((c & 2) ? cycle_off_.hi : cycle_off_.lo);
				return cycle_events(on, off, until);
			},
			4, &cycle_checked, &cycle_wrong, &cycle_widest);

		const bool ok = settled && mismatch == 0 && hold_wrong == 0 && cycle_wrong == 0 && hold_press_.known &&
				hold_release_.known && cycle_on_.known && cycle_off_.known && hold_checked > 0 &&
				cycle_checked > 0;

		QJsonObject drive;
		drive[QStringLiteral("ok")] = ok;
		drive[QStringLiteral("steps")] = static_cast<int>(steps_.size());
		QJsonArray log;
		for (std::size_t i = 0; i < steps_.size() && i < step_times_.size(); ++i)
			log.append(QStringLiteral("%1 ms: %2").arg(step_times_[i], 0, 'f', 0).arg(steps_[i].label));
		drive[QStringLiteral("step_log")] = log;
		drive[QStringLiteral("settled")] = settled;
		drive[QStringLiteral("settle_ms")] = settle_ms_;
		drive[QStringLiteral("fresh_mismatch")] = static_cast<double>(mismatch);
		drive[QStringLiteral("run_ms")] = run_ms;
		drive[QStringLiteral("ticks")] = static_cast<int>(ticks_.size());
		drive[QStringLiteral("tick_gap_max_ms")] = worst;
		drive[QStringLiteral("tick_gap_max_after_step")] =
			worst_step > 0 && worst_step <= static_cast<int>(steps_.size())
				? steps_[static_cast<std::size_t>(worst_step - 1)].label
				: QStringLiteral("(手順の前)");
		drive[QStringLiteral("tick_gap_p99_ms")] = p99;
		drive[QStringLiteral("full_compose_max_ms")] = max_full_compose_ms_;
		drive[QStringLiteral("crop_compose_max_ms")] = max_crop_compose_ms_;
		drive[QStringLiteral("fit_paints")] = window_.fit_paints();
		drive[QStringLiteral("pixel_paints")] = window_.pixel_paints();
		drive[QStringLiteral("hold_levels_checked")] = hold_checked;
		drive[QStringLiteral("hold_levels_wrong")] = hold_wrong;
		drive[QStringLiteral("hold_level_bound_width_max")] = hold_widest;
		drive[QStringLiteral("cycle_levels_checked")] = cycle_checked;
		drive[QStringLiteral("cycle_levels_wrong")] = cycle_wrong;
		drive[QStringLiteral("cycle_level_bound_width_max")] = cycle_widest;
		QJsonObject extra;
		extra[QStringLiteral("drive")] = drive;
		const bool written = !settled || write_shot_(dir_ + QStringLiteral("/drive.png"), extra);
		std::printf(
			"label_tuner: --drive %s (描き終えるまで %.0f ms、違う画素 %lld、tick %zu 回、いちばん長い間 %.1f ms、"
			"明るさ %d + %d 点で外れ %d + %d)\n",
			ok ? "OK" : "NG", settle_ms_, mismatch, ticks_.size(), worst, hold_checked, cycle_checked,
			hold_wrong, cycle_wrong);
		QCoreApplication::exit(ok && written ? 0 : 1);
	}

	TunerWindow &window_;
	QString dir_;
	std::function<std::uint64_t()> clock_;
	std::function<bool(const QString &, const QJsonObject &)> write_shot_;
	std::vector<Step> steps_;
	std::vector<double> step_times_;
	std::vector<TickRecord> ticks_;
	int next_step_ = 0;
	std::int64_t started_ = 0;
	std::int64_t last_step_at_ = 0;
	double settle_ms_ = 0.0;
	double max_full_compose_ms_ = 0.0;
	double max_crop_compose_ms_ = 0.0;
	Moment hold_press_;
	Moment hold_release_;
	Moment cycle_on_;
	Moment cycle_off_;
};

} // namespace

int run_drive(TunerWindow &window, const QString &dir, const std::function<std::uint64_t()> &clock,
	      const std::function<bool(const QString &path, const QJsonObject &extra)> &write_shot)
{
	Drive drive(window, dir, clock, write_shot);
	return drive.run();
}

} // namespace cordmux::label_tuner

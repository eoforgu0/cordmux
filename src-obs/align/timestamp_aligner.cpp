// SPDX-License-Identifier: Apache-2.0

// align/timestamp_aligner.h の実装。値の選び方の理由は、ヘッダーの AlignerConfig のコメントにある。

#include "align/timestamp_aligner.h"

#include <algorithm>
#include <cmath>

namespace cordmux::align {

namespace {

int64_t abs_i64(int64_t v)
{
	return v < 0 ? -v : v;
}

} // namespace

void TimestampAligner::reset()
{
	stats_ = AlignerStats{};
	initialized_ = false;
	out_us_ = 0;
	last_sender_us_ = 0;
	residual_us_ = 0.0;
	period_us_ = 0.0;
	seed_us_ = 0.0;
	err_ema_us_ = 0.0;
	err_ema_n_ = 0;
	last_slew_us_ = 0.0;
	carry_us_ = 0;
	w_ = sx_ = sy_ = sxx_ = sxy_ = 0.0;
	mean_sum_us_ = 0.0;
	mean_count_ = 0;
	recent_diffs_.clear();
	discard_pending();
}

// 回帰と差分の平均を捨てて、与えた初期値から推定をやり直す (再ロックと再整列のとき)。
//
// 再整列でも捨てるのは、再整列は、送信側の時計が途中で不連続に変わったと判断したときに起きるからである。
// 変わる前と後の点は、傾きは同じでも切片の違う 2 本の線の上にあり、まとめて当てはめると傾きがゆがむ。
// 捨てずにいると、時計が 300 ms 戻る列で推定周期が 2% 低くなり、そのぶんグリッドが遅れて、
// 360 フレーム後にもう一度再整列した。
void TimestampAligner::restart_estimator(double seed_us)
{
	w_ = sx_ = sy_ = sxx_ = sxy_ = 0.0;
	mean_sum_us_ = 0.0;
	mean_count_ = 0;
	seed_us_ = seed_us > 0.0 ? seed_us : 0.0;
	recent_diffs_.clear();
}

// 指数重み付き最小二乗に 1 フレームぶんを反映する。
//   shift_frames: 回帰の原点を動かすフレーム数 (グリッドを進める k とは別の量。align のコメントを参照)
//   diff_us:      送信側のタイムスタンプの進み
//   add_point:    このフレームを点として足すか (不連続のフレームは足さない)
void TimestampAligner::update_estimator(double shift_frames, double diff_us, bool add_point)
{
	// (1) 原点を現在のフレームへ移す。x を -shift_frames、y を -diff_us だけ動かす。
	//     傾きは平行移動で変わらないので、原点を移すのは数値の桁を抑えるためである。
	{
		const double kd = shift_frames;
		const double dd = diff_us;
		sxx_ = sxx_ - 2.0 * kd * sx_ + kd * kd * w_;
		sxy_ = sxy_ - dd * sx_ - kd * sy_ + kd * dd * w_;
		sx_ = sx_ - kd * w_;
		sy_ = sy_ - dd * w_;
	}
	// (2) 古い点を忘れる。period_tau_frames はフレーム数の時定数なので、不連続で shift_frames フレーム
	//     進んだときは、そのフレーム数ぶん忘れる (lambda の累乗)。1 回しか掛けないと、大きな不連続の直後だけ
	//     古い点の重みが下がりきらず、時定数の意味がずれる。
	{
		const double tau = cfg_.period_tau_frames > 1.0 ? cfg_.period_tau_frames : 1.0;
		double lambda = 1.0 - 1.0 / tau;
		if (shift_frames > 1.0)
			lambda = std::pow(lambda, shift_frames);
		w_ *= lambda;
		sx_ *= lambda;
		sy_ *= lambda;
		sxx_ *= lambda;
		sxy_ *= lambda;
	}
	// (3) 現在のフレーム (x = 0, y = 0) を足す。不連続のフレームは、落ちたフレームの数が分からないので
	//     推定に使わない (align の「回帰の原点を動かすフレーム数」のコメント)。
	if (add_point) {
		w_ += 1.0;
		mean_sum_us_ += diff_us;
		mean_count_++;
	}
}

// 保留の間に保存したフレームを推定に反映して、保留を終える。
//   as_jump = true  (不連続と確定した、または時計が戻った): ほかの不連続と同じに扱う。原点を
//                   diff / 推定周期 だけ動かし、点は足さない (推定に影響しない)。
//   as_jump = false (ジッターだった): 疑ったフレーム n だけを普通の点として入れる。後続の確認のフレームは、
//                   ジッターの後半 (早く届いたフレーム) で差分が短いので入れない。
void TimestampAligner::flush_pending(bool as_jump)
{
	for (size_t i = 0; i < deferred_diffs_.size(); i++) {
		const double d = deferred_diffs_[i];
		double shift = 1.0;
		if (as_jump && period_us_ > 0.0)
			shift = d / period_us_;
		update_estimator(shift, d, !as_jump && i == 0);
	}
	discard_pending();
}

// 保留を捨てる (保存した点も捨てる)。再ロックと再整列のときに使う。
// どちらも restart_estimator で推定をやり直すので、保存した点は別の線の上にあり、入れられない。
void TimestampAligner::discard_pending()
{
	deferred_diffs_.clear();
	jump_pending_ = false;
	jump_pending_residual_us_ = 0.0;
	jump_pending_left_ = 0;
}

// 現在の推定周期。重みが足りないうちは、初期値か差分の平均を使う。
double TimestampAligner::period_estimate() const
{
	if (w_ >= cfg_.min_regression_weight) {
		const double denom = w_ * sxx_ - sx_ * sx_;
		if (denom > 1e-9) {
			const double slope = (w_ * sxy_ - sx_ * sy_) / denom;
			if (slope > 0.0)
				return slope;
		}
	}
	if (seed_us_ > 0.0)
		return seed_us_;
	if (mean_count_ > 0)
		return mean_sum_us_ / double(mean_count_);
	return 0.0;
}

// 直近 relock_frames フレームの差分から、送信側の周期が持続的に変わったかを判定する。変わっていれば、
// 差分の中央値を *median_out に入れて true を返す。
//
// 判定は差分の平均で行い、同じ向きにずれた差分が過半数であることも条件にする。理由は
// AlignerConfig::relock_frames のコメントにある。立ち上がりで推定周期がまだ粗い場合も、8 点の回帰の
// 傾きのずれは最悪 1.7 ms なので、ジッターの寄与 (最悪 1.25 ms) と合わせて 2.95 ms で、60 fps の 20%
// (3.3 ms) に届かない。
// 上向きの平均は外れ値を relock_trim 個除いてから求め、下向きの平均は全部の差分で求める。理由は
// AlignerConfig::relock_trim のコメントにある。
bool TimestampAligner::check_relock(double *median_out) const
{
	const size_t need = size_t(cfg_.relock_frames > 0 ? cfg_.relock_frames : 1);
	if (recent_diffs_.size() < need || period_us_ <= 0.0)
		return false;

	const double lo = period_us_ * (1.0 - cfg_.relock_deviation);
	const double hi = period_us_ * (1.0 + cfg_.relock_deviation);
	size_t above = 0;
	size_t below = 0;
	for (int64_t d : recent_diffs_) {
		const double dd = double(d);
		if (dd > hi)
			above++;
		else if (dd < lo)
			below++;
	}

	std::vector<int64_t> sorted(recent_diffs_);
	std::sort(sorted.begin(), sorted.end());
	const size_t n = sorted.size();

	// 除く外れ値の個数。差分の数の半分までに抑える (全部除くと判定できない)。
	size_t trim = size_t(cfg_.relock_trim > 0 ? cfg_.relock_trim : 0);
	if (trim > n / 2)
		trim = n / 2;
	double sum_low = 0.0; // 大きいほうから trim 個を除いた和 (上向きの判定に使う)
	double sum_all = 0.0; // 全部の差分の和 (下向きの判定に使う)
	for (size_t i = 0; i < n; i++) {
		sum_all += double(sorted[i]);
		if (i < n - trim)
			sum_low += double(sorted[i]);
	}
	const size_t majority = n / 2 + 1; // 8 フレームなら 5 フレーム
	const bool up = sum_low / double(n - trim) > hi && above >= majority;
	const bool down = sum_all / double(n) < lo && below >= majority;
	if (!up && !down)
		return false;

	const size_t mid = n / 2;
	const double median = (n % 2 == 0) ? (double(sorted[mid - 1]) + double(sorted[mid])) / 2.0
	                                   : double(sorted[mid]);
	if (median <= 0.0)
		return false;
	*median_out = median;
	return true;
}

int64_t TimestampAligner::align(int64_t sender_ts_us)
{
	stats_.frames++;

	// 最初のフレームは、送信側のタイムスタンプをそのままグリッドの原点にする。
	if (!initialized_) {
		initialized_ = true;
		out_us_ = sender_ts_us;
		last_sender_us_ = sender_ts_us;
		residual_us_ = 0.0;
		last_slew_us_ = 0.0;
		seed_us_ = cfg_.initial_period_hint_us > 0.0 ? cfg_.initial_period_hint_us : 0.0;
		// 回帰の最初の点 (x = 0, y = 0) を置く。
		w_ = 1.0;
		sx_ = sy_ = sxx_ = sxy_ = 0.0;
		period_us_ = period_estimate();
		stats_.period_us = period_us_;
		stats_.last_offset_us = 0;
		stats_.last_step_frames = 0;
		stats_.carry_us = 0;
		stats_.last_slew_us = 0.0;
		return out_us_;
	}

	const int64_t diff = sender_ts_us - last_sender_us_;
	last_sender_us_ = sender_ts_us;

	// 周期がまだ分からなければ、最初の差分を初期値にする。
	if (period_us_ <= 0.0 && diff > 0) {
		seed_us_ = double(diff);
		period_us_ = seed_us_;
	}

	// 再ロックの判定を先に行う。後の不連続と再整列の判定に、置き直した周期を使うためである
	// (30 fps から 60 fps のようにフレームレートが上がる向きは、不連続の判定では拾えない)。
	if (diff > 0) {
		recent_diffs_.push_back(diff);
		if (recent_diffs_.size() > size_t(cfg_.relock_frames > 0 ? cfg_.relock_frames : 1))
			recent_diffs_.erase(recent_diffs_.begin());
	} else {
		recent_diffs_.clear();
	}

	bool relocked = false;
	double relock_median = 0.0;
	if (check_relock(&relock_median)) {
		relocked = true;
		stats_.relock_count++;
		restart_estimator(relock_median);
		discard_pending(); // 周期が変わったので、保留中の残差も保存した点も使えない
		period_us_ = relock_median;
	}

	// 現在のフレームの差分が推定周期から大きくずれているか (再ロックしそうな状態)。
	// この間は再整列しない。疑わしいのは送信側の時計ではなく推定周期のほうで、誤差は再ロックか回帰が
	// 片付ける。
	// 再整列を止めるのはこのフレームだけである (毎フレーム測り直す)。送信側が実際に変わったのなら差分の
	// 平均がずれて再ロックし、変わっていないなら次のフレームでずれが 20% 未満に戻る (60 fps でジッターが
	// +-5 ms なら、差分 1 つが 20% を超えるのは 44%)。
	bool relock_pending = false;
	if (!relocked && diff > 0 && period_us_ > 0.0) {
		const double dev = std::fabs(double(diff) - period_us_) / period_us_;
		relock_pending = dev >= cfg_.relock_deviation;
	}

	// 不連続の判定: グリッドが 1 フレーム進んだときの予想位置から、送信側がどれだけ先にあるかで測る。
	// 差分ではなく残差で測る理由は、AlignerConfig::jump_ratio のコメントにある。残差が余白を超えても、
	// その場では確定せず、次のフレームで確認する (AlignerConfig::jump_confirm_frames)。確認している間の
	// フレームはグリッドを 1 フレームだけ進めて返すので、出力の遅延は増えない。
	bool is_jump = false;        // グリッドを k フレーム進め、このフレームを推定に入れない
	bool defer_this_frame = false; // このフレームの推定への反映を、次のフレームまで保留する
	bool mix_this_frame = true;  // 普通のフレームとして点を足すか
	int64_t k = 1;
	const double margin = (cfg_.jump_ratio - 1.0) * period_us_;
	// 平滑化した誤差のうち、判定で差し引くのは +-W まで (AlignerConfig::jump_excuse_max_us)。
	// W の外は、グリッドの位置そのもので判定する。
	//   - 出力が W より先に出ている (err_ema < -W): 残差が W を超えたぶんだけ小さくなり、送信側の長い間隔を
	//     ドロップと読まない。送信側がその間隔で追いつき、その間隔は普通の点として推定に入る。
	//   - 送信側が W より先に出ている (err_ema > +W): W を超えたぶんが残差に残り、不連続としてグリッドを
	//     進める。
	// W <= 0 なら全部を差し引く。
	double ema_excuse_us = err_ema_us_;
	if (cfg_.jump_excuse_max_us > 0.0) {
		const double w = cfg_.jump_excuse_max_us;
		if (ema_excuse_us > w || ema_excuse_us < -w) {
			ema_excuse_us = ema_excuse_us > 0.0 ? w : -w;
			stats_.jump_excuse_capped_frames++;
		}
	}
	// 大きな値でも桁を落とさないよう、まず int64 の引き算で小さな数にしてから double にする。
	const double residual_from_grid =
	        double(sender_ts_us - out_us_) - period_us_ - residual_us_ - ema_excuse_us;

	if (diff <= 0) {
		// 送信側のタイムスタンプが進んでいない。推定には入れず、グリッドは 1 フレーム進める。
		is_jump = true;
		stats_.backward_count++;
		// 時計が戻っても、保留中のフレーム n が不連続だったかどうかは分からない。保存した点は不連続と
		// 同じに扱い (推定に影響させない)、保留を終える。
		if (jump_pending_)
			flush_pending(true);
	} else if (jump_pending_) {
		// 確認のフレーム。グリッドは +1 周期だけ進めた位置にあるので、そこからの残差を測る。
		// 周期が分からないときに保留になることはない (下の分岐が period_us_ > 0 を条件にしている) が、
		// 0 で割らないよう条件に入れてある。0 なら確認できないので、ジッターとして扱う。
		stats_.last_grid_residual_us = residual_from_grid;
		const double tol = cfg_.jump_confirm_tolerance_periods * period_us_;
		const bool persists =
		        period_us_ > 0.0 && residual_from_grid > margin &&
		        std::fabs(residual_from_grid - jump_pending_residual_us_) <= tol;
		if (!persists) {
			// ジッターだった (遅れて届いたフレームの次が早く届いた)。グリッドは動かさない。
			stats_.jump_wobble_count++;
			flush_pending(false);
			mix_this_frame = false; // ジッターの後半は差分が短いので、この点は入れない
		} else if (jump_pending_left_ > 1) {
			jump_pending_left_--;
			defer_this_frame = true;
		} else {
			// 不連続と確定した。グリッドを余分に進め、このフレームをそこに置く。
			//
			// 余分に進める量は、2 つの残差 (疑ったフレーム n の残差 r と、このフレームの残差) の
			// 小さいほうから決める。どちらの残差にも、それを測ったフレームの遅延がそのまま乗っている。
			// 送信側のタイムスタンプは単調増加なので、遅延は正の向きにしか動作しない (遅れて届いたフレームの
			// 次が早く届いても、元の位置に戻っただけである)。小さいほうを使えば、遅延のぶんが落ちる。
			// 片方だけを見ると、ジッターと実際のドロップが隣り合ったときに、グリッドを 1 周期余分に進める。
			// たとえば、このフレームの残差だけを見ると、ドロップの次のフレームが遅れて届いたときに
			// 1.87 周期になり、30 分の列で 1 回 k = 3 になって出力が 1 周期先に出た。それを補正で戻す
			// 300 フレームの間に、重複 1 件と余分な欠落 1 件が出た。
			const double gap_us = std::min(residual_from_grid, jump_pending_residual_us_);
			int64_t extra = int64_t(std::llround(gap_us / period_us_));
			if (extra < 1)
				extra = 1;
			k = 1 + extra;
			is_jump = true;
			stats_.jump_count++;
			stats_.jump_frames += extra;
			flush_pending(true);
		}
	} else if (period_us_ > 0.0 && stats_.frames > cfg_.jump_warmup_frames) {
		stats_.last_grid_residual_us = residual_from_grid;
		if (residual_from_grid > margin) {
			const bool confirmable =
			        cfg_.jump_confirm_frames > 0 &&
			        residual_from_grid <= cfg_.jump_confirm_max_periods * period_us_;
			// 整定の度合い (jump_settle_ratio) は、その場で確定するときにだけ課す。理由は
			// AlignerConfig::jump_settle_ratio のコメントにある。
			const double ema_lag = cfg_.error_tau_frames * std::fabs(last_slew_us_);
			const bool settled = ema_lag <= cfg_.jump_settle_ratio * margin;
			if (confirmable) {
				// 疑いだけを持ち、グリッドは 1 フレームだけ進める (ジッターとして扱う)。
				jump_pending_ = true;
				jump_pending_residual_us_ = residual_from_grid;
				jump_pending_left_ = cfg_.jump_confirm_frames;
				defer_this_frame = true;
				stats_.jump_pending_count++;
			} else if (settled) {
				// ジッターでは作れない大きさなので、その場で確定する
				// (AlignerConfig::jump_confirm_max_periods)。
				k = 1 + int64_t(std::llround(residual_from_grid / period_us_));
				if (k < 2)
					k = 2;
				is_jump = true;
				stats_.jump_count++;
				stats_.jump_frames += k - 1;
			}
		}
		// 送信側が予想位置よりずっと後ろにある (residual_from_grid < -margin) ときは、不連続に数えない。
		// それは送信側ではなくグリッドの問題だからである。差分が正なら、送信側のタイムスタンプは送信側の
		// 間隔のとおりに進んでおり、周期の推定に入れてよい。
		// 入れないと、推定周期が長すぎる状態 (グリッドが送信側より先に出る) では毎フレームが除かれ、
		// 回帰の重みが min_regression_weight に届かずに、周期が初期値のまま変わらなくなる (60 fps から
		// 75 fps への変化で、再ロックの初期値が 3.7% 高かったとき、1000 フレーム以上変わらず、誤差は
		// -95 ms になり、補正は上限に張り付き、再整列も carry の残りに隠れて起きなかった)。
		// 送信側の時計が戻る場合は diff <= 0 の分岐が拾う。1 周期に満たない戻りは、600 点の回帰の中の
		// 1 点にしかならないので害がない。
	}

	// 回帰の原点を動かすフレーム数。グリッドを進める k とは別の量である。
	//
	// k はグリッドを何フレーム進めるかで、不連続と判定したときは整数に丸めてある (2 以上)。一方、回帰の原点は、
	// 送信側が実際に何フレームぶん進んだかで動かす必要がある。ここを k にすると、送信側のフレームレートが
	// 再ロックのしきい値 (20%) 未満だけ持続的に変わったときに、「数フレームに 1 回 k = 2 で不連続と誤判定し、
	// そのぶん x を 1 余分に進める」状態が、それ自身を支える不動点になる。推定周期が変化前の値に張り付いた
	// まま不連続が出続ける (60 fps から 50 fps に変えた 3000 フレームで、不連続が 601 回、推定周期は
	// 16660 us のまま。5 フレームに 1 回 k = 2 で進めると、見かけの周期が 5 x 20000 / 6 = 16667 us になり、
	// 推定値と釣り合ってしまう)。
	//
	// 不連続のフレームでは diff / 推定周期 (丸めない) を使う。この比で動かすと、動かす量の傾きが必ず
	// 推定周期そのものになるので、点を足さないことと合わせて、不連続のフレームは推定に影響しない。
	// ドロップによる不連続 (diff がほぼ k 周期) では、k を使ったときとほぼ同じ値になる。
	if (defer_this_frame) {
		// 推定には何も反映しない (原点も動かさない)。確定かジッターかが決まったときに、flush_pending が
		// まとめて反映する。保留の間は推定周期が動かないので、確認のフレームの予想位置も、疑ったフレームと
		// 同じグリッドの上で測れる。
		deferred_diffs_.push_back(double(diff));
	} else {
		double shift_frames = double(k);
		if (is_jump && period_us_ > 0.0)
			shift_frames = double(diff) / period_us_;
		update_estimator(shift_frames, double(diff), !is_jump && mix_this_frame);
	}

	period_us_ = period_estimate();
	if (period_us_ <= 0.0)
		period_us_ = cfg_.initial_period_hint_us > 0.0 ? cfg_.initial_period_hint_us : 1.0;

	// グリッドを k フレーム進める。端数は持ち越す (切り捨てると 60 fps で 1 時間に 100 ms たまる)。
	double want_us = period_us_ * double(k) + residual_us_;
	int64_t grid_step = int64_t(std::llround(want_us));
	int64_t out_pred = out_us_ + grid_step;

	// 平滑化前の誤差。
	const double e_raw = double(sender_ts_us - out_pred);
	const double e_abs = std::fabs(e_raw);
	stats_.err_abs_sum_us += e_abs;
	if (e_abs > stats_.err_abs_max_us)
		stats_.err_abs_max_us = e_abs;

	// 誤差の平滑化 (指数移動平均)。立ち上がりは 1/n で速く追う。
	//
	// 前のフレームで補正したぶんは、自分で動かしたと分かっている変化なので、測った値を混ぜる前に平均に入れる。
	// 普通の指数移動平均にすると、補正している間ずっと「時定数 x 補正量」(既定で 120 x 500 us = 60 ms) の
	// 遅延が出る。再ロックの 230 フレーム後に誤差がもう 0 なのに平均はまだ -47 ms を示し、補正が上限に
	// 張り付いたまま反対側へ 47 ms 行き過ぎて、振動した。分かっている変化を先に入れておけば、平均は
	// ジッターをならすだけになり、遅延が出ない。
	err_ema_n_++;
	{
		const double predicted = err_ema_us_ - last_slew_us_;
		const double tau = cfg_.error_tau_frames > 1.0 ? cfg_.error_tau_frames : 1.0;
		double beta = 1.0 / tau;
		const double warm = 1.0 / double(err_ema_n_);
		if (warm > beta)
			beta = warm;
		err_ema_us_ = predicted + (e_raw - predicted) * beta;
	}

	bool snapped_forward = false;
	if (relocked) {
		// 再ロックの直後は、周期が違っていた間にたまった誤差が残っている。これは分かっている誤差なので
		// carry として持ち、再整列とは数えずに補正で戻す。
		carry_us_ = int64_t(std::llround(e_raw));
		err_ema_us_ = e_raw;
		err_ema_n_ = 1;
	} else if (!relock_pending) {
		const double threshold = std::max(cfg_.realign_periods * period_us_, cfg_.realign_min_us);
		if (std::fabs(e_raw - double(carry_us_)) > threshold) {
			stats_.realign_count++;
			// 送信側の時計が不連続に変わったと判断した。変わる前の点は別の線の上にあるので、推定を
			// やり直す (周期そのものは変わっていないはずなので、初期値に残す)。
			restart_estimator(period_us_);
			// 再整列がこの差を引き受けたので、保留中の不連続は確認しなくてよい。
			discard_pending();
			if (e_raw > 0.0) {
				// 送信側が先にある。出力を送信側のタイムスタンプに合わせる
				// (前に進むので、単調性は保たれる)。
				snapped_forward = true;
				carry_us_ = 0;
				err_ema_us_ = 0.0;
				err_ema_n_ = 0;
			} else {
				// 出力が先にある。タイムスタンプは戻せないので、差を carry として補正で戻す。
				// ここで送信側に合わせると出力が逆行し、毎フレーム再整列し続ける。
				carry_us_ = int64_t(std::llround(e_raw));
				err_ema_us_ = e_raw;
				err_ema_n_ = 1;
			}
		}
	}

	// 補正: 平滑化した誤差のうち不感帯を超えたぶんを時定数で割り、上限で抑えて足す。
	// 上限は max(max_slew_us_per_frame, max_slew_ratio x 推定周期) (AlignerConfig::max_slew_ratio のコメント)。
	double slew_us = 0.0;
	if (!snapped_forward) {
		const double mag = std::fabs(err_ema_us_);
		if (mag > cfg_.slew_deadband_us) {
			const double tau = cfg_.slew_tau_frames > 1.0 ? cfg_.slew_tau_frames : 1.0;
			slew_us = (mag - cfg_.slew_deadband_us) / tau;
			double cap = cfg_.max_slew_us_per_frame;
			if (cfg_.max_slew_ratio > 0.0 && cfg_.max_slew_ratio * period_us_ > cap)
				cap = cfg_.max_slew_ratio * period_us_;
			if (slew_us >= cap) {
				slew_us = cap;
				stats_.slew_capped_frames++;
			}
			if (err_ema_us_ < 0.0)
				slew_us = -slew_us;
		}
	}

	int64_t out_new = 0;
	if (snapped_forward) {
		out_new = sender_ts_us;
		residual_us_ = 0.0;
		slew_us = 0.0;
	} else {
		const double want_total = want_us + slew_us;
		const int64_t total_step = int64_t(std::llround(want_total));
		residual_us_ = want_total - double(total_step);
		out_new = out_us_ + total_step;
	}

	// 狭義単調増加を必ず守る。
	if (out_new <= out_us_) {
		out_new = out_us_ + cfg_.min_output_step_us;
		residual_us_ = 0.0;
		stats_.monotonic_fix_count++;
	}

	// carry (再ロックや再整列で分かっている誤差の残り) を、現在の誤差に合わせて縮める。
	// 補正した量を引いて減らす形にすると、周期のわずかなずれのぶんだけ実際の誤差と食い違い続ける
	// (1000 フレーム後に、実際の誤差 0.3 ms に対して carry が 13 ms 残った)。平滑化した誤差を上限にして
	// 縮めれば、必ず実際の誤差に追いつき、増えることはない。
	if (carry_us_ != 0) {
		if (std::fabs(slew_us) <= cfg_.settled_slew_us) {
			carry_us_ = 0; // 補正がもう動作していないので、整定した
		} else {
			const int64_t now = int64_t(std::llround(std::fabs(err_ema_us_)));
			int64_t mag = abs_i64(carry_us_);
			if (now < mag)
				mag = now;
			carry_us_ = carry_us_ < 0 ? -mag : mag;
		}
	}

	out_us_ = out_new;
	last_slew_us_ = slew_us;

	stats_.period_us = period_us_;
	stats_.slew_abs_sum_us += std::fabs(slew_us);
	stats_.slew_sum_us += slew_us;
	stats_.last_offset_us = out_us_ - sender_ts_us;
	stats_.last_step_frames = k;
	stats_.carry_us = carry_us_;
	stats_.last_slew_us = slew_us;
	stats_.last_err_us = e_raw;
	stats_.err_ema_us = err_ema_us_;
	return out_us_;
}

} // namespace cordmux::align

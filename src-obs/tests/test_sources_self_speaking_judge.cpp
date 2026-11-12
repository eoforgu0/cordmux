// SPDX-License-Identifier: Apache-2.0

// sources/self_speaking_judge.h の単体テスト。音声ソースのパケットから話している・話していないを決める判定
// (しきい値、続けるしきい値、100 ms の切れ目、配信に声が出ていない間、250 ms 届かない間)、3 つの状態から作る
// 光と暗さ、ソースの探し方 (UUID、名前、どちらもない)、peak の分布の集計を確認する。

#include "doctest.h"

#include "sources/composite_label_plan.h"
#include "sources/self_speaking_judge.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace cordmux::sources;

namespace {

constexpr std::int64_t kPacketNs = 10'000'000; // 10 ms
constexpr std::uint64_t kBaseNs = 5'000'000'000ULL;

// パケット index (kBaseNs から 10 ms ずつ) の peak が peak_db のとき、判定に渡して変化を返す。到着の時刻はパケットの時刻と同じ。
SelfSpeakingJudge::Result step(SelfSpeakingJudge &judge, int index, double peak_db, bool audible = true)
{
	SelfSpeakingPacket packet;
	packet.start_ns = kBaseNs + static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(kPacketNs);
	packet.length_ns = kPacketNs;
	packet.peak_db = peak_db;
	packet.audible = audible;
	return judge.observe(packet, packet.start_ns);
}

constexpr int gap_packets()
{
	return static_cast<int>(kSelfSpeakingGapNs / kPacketNs);
}

} // namespace

TEST_CASE("peak を dBFS にする: 満度が 0 dB、半分が約 -6 dB、0 と NaN が -96 dB")
{
	CHECK(peak_to_db(1.0) == doctest::Approx(0.0));
	CHECK(peak_to_db(0.5) == doctest::Approx(-6.0206).epsilon(0.001));
	CHECK(peak_to_db(0.0) == -96.0);
	CHECK(peak_to_db(-0.3) == -96.0);
	CHECK(peak_to_db(std::nan("")) == -96.0);
	CHECK(peak_to_db(1e-9) == -96.0);
}

TEST_CASE("plane_peak は絶対値の最大を返し、空のチャンネルは 0 にする")
{
	const float samples[] = {0.1f, -0.7f, 0.3f, 0.2f};
	CHECK(plane_peak(samples, 4) == doctest::Approx(0.7f));
	CHECK(plane_peak(samples, 0) == 0.0f);
	CHECK(plane_peak(nullptr, 4) == 0.0f);
}

TEST_CASE("しきい値以上のパケットで話し始め、その時刻はパケットの開始になる")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-26);
	const auto quiet = step(judge, 0, -27.0);
	CHECK(quiet.changes.empty());
	CHECK_FALSE(judge.talking());
	const auto loud = step(judge, 1, -26.0);
	REQUIRE(loud.changes.size() == 1);
	CHECK(loud.changes[0].start_ns == kBaseNs + static_cast<std::uint64_t>(kPacketNs));
	CHECK(loud.changes[0].stop_ns == 0);
	CHECK(judge.talking());
}

TEST_CASE("話し始めた後は、しきい値の 6 dB 下までは話し続け、それより下は音が下回った扱いになる")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-26);
	step(judge, 0, -20.0);
	// -32 dB は続ける線ちょうど: 続く。切れ目の 100 ms をまたいでも話し終わりにならない。
	for (int i = 1; i <= gap_packets() + 5; ++i)
		CHECK(step(judge, i, -32.0).changes.empty());
	CHECK(judge.talking());
	// -33 dB は続ける線の下: 100 ms たつと話し終わり。
	bool stopped = false;
	for (int i = gap_packets() + 6; i < gap_packets() + 6 + gap_packets() + 2; ++i) {
		const auto r = step(judge, i, -33.0);
		if (!r.changes.empty())
			stopped = true;
	}
	CHECK(stopped);
	CHECK_FALSE(judge.talking());
}

TEST_CASE("続けるしきい値の帯の音では、話していない状態から話し始めない")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-26);
	for (int i = 0; i < 20; ++i)
		CHECK(step(judge, i, -30.0).changes.empty());
	CHECK_FALSE(judge.talking());
}

TEST_CASE("しきい値を下回って 100 ms たつと話し終わり、時刻は最後に大きかったパケットの終わり")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-26);
	step(judge, 0, -10.0);
	step(judge, 1, -10.0); // 最後に大きかったパケット。終わりは kBaseNs + 20 ms
	for (int i = 2; i < 2 + gap_packets(); ++i)
		CHECK(step(judge, i, -80.0).changes.empty()); // 100 ms 未満の無音
	const auto r = step(judge, 2 + gap_packets(), -80.0);
	REQUIRE(r.changes.size() == 1);
	CHECK(r.changes[0].start_ns == 0);
	CHECK(r.changes[0].stop_ns == kBaseNs + 2 * static_cast<std::uint64_t>(kPacketNs));
	CHECK_FALSE(judge.talking());
}

TEST_CASE("100 ms 未満の息継ぎでは話し終わりにならず、話し始めも重ならない")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-26);
	REQUIRE(step(judge, 0, -10.0).changes.size() == 1);
	for (int i = 1; i < 1 + gap_packets() - 1; ++i)
		CHECK(step(judge, i, -80.0).changes.empty());
	CHECK(step(judge, gap_packets(), -10.0).changes.empty());
	CHECK(judge.talking());
}

TEST_CASE("しきい値を変えると、次のパケットから新しいしきい値で判定する")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-20);
	CHECK(step(judge, 0, -25.0).changes.empty());
	judge.set_threshold_db(-30);
	CHECK(step(judge, 1, -25.0).changes.size() == 1);
}

TEST_CASE("範囲の端のしきい値: 0 dB では満度でだけ話し始め、-96 dB では無音でも話し始める")
{
	SelfSpeakingJudge top;
	top.set_threshold_db(0);
	CHECK(step(top, 0, -0.5).changes.empty());
	CHECK(step(top, 1, 0.0).changes.size() == 1);

	SelfSpeakingJudge bottom;
	bottom.set_threshold_db(-96);
	CHECK(step(bottom, 0, -96.0).changes.size() == 1);
}

TEST_CASE("配信に声が出ていないパケットは大きくても話していないことになり、100 ms で話し終わる")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-26);
	REQUIRE(step(judge, 0, -10.0).changes.size() == 1);
	// ミュート (または、フェーダーが 0、ソースが配信に出ていない) になった。大きな音のままでも話していない。
	bool stopped = false;
	for (int i = 1; i <= gap_packets() + 1; ++i) {
		if (!step(judge, i, -10.0, /*audible=*/false).changes.empty())
			stopped = true;
	}
	CHECK(stopped);
	CHECK_FALSE(judge.talking());
	// 声が出ている状態に戻ると、すぐに話し始める。
	CHECK(step(judge, gap_packets() + 2, -10.0, true).changes.size() == 1);
}

TEST_CASE("配信に声が出ている判定: ミュート、フェーダー 0 以下と NaN、配信に出ていないソースのどれでも偽")
{
	CHECK(self_source_audible(false, 1.0f, true));
	CHECK(self_source_audible(false, 0.3f, true));
	CHECK_FALSE(self_source_audible(true, 1.0f, true));
	CHECK_FALSE(self_source_audible(false, 0.0f, true));
	CHECK_FALSE(self_source_audible(false, -1.0f, true));
	CHECK_FALSE(self_source_audible(false, std::nanf(""), true));
	CHECK_FALSE(self_source_audible(false, 1.0f, false));
}

TEST_CASE("パケットが 250 ms より空くと前の状態を捨て、restarted を返す")
{
	SelfSpeakingJudge judge;
	judge.set_threshold_db(-26);
	REQUIRE(step(judge, 0, -10.0).changes.size() == 1);
	// 250 ms ちょうどまでは続きとみなす。
	SelfSpeakingPacket packet;
	packet.start_ns = kBaseNs + 250'000'000ULL;
	packet.length_ns = kPacketNs;
	packet.peak_db = -10.0;
	const auto on_edge = judge.observe(packet, packet.start_ns);
	CHECK_FALSE(on_edge.restarted);
	// 250 ms を超えて空いたら、前の話し始めを引き継がず、新しく話し始める。
	packet.start_ns = kBaseNs + 250'000'000ULL + 251'000'000ULL;
	const auto later = judge.observe(packet, packet.start_ns);
	CHECK(later.restarted);
	REQUIRE(later.changes.size() == 1);
	CHECK(later.changes[0].start_ns == packet.start_ns);
}

TEST_CASE("時刻 0 のパケットは時刻 1 ナノ秒にして、話し始めの時刻が 0 (話していない) にならない")
{
	SelfSpeakingJudge judge;
	SelfSpeakingPacket packet;
	packet.start_ns = 0;
	packet.length_ns = kPacketNs;
	packet.peak_db = -10.0;
	const auto r = judge.observe(packet, 1'000);
	REQUIRE(r.changes.size() == 1);
	CHECK(r.changes[0].start_ns == 1);
}

TEST_CASE("音声ソースの状態: 指定なしは NotJudged、見つからないか一度もパケットが来ないと Undeterminable")
{
	const std::uint64_t now = 10'000'000'000ULL;
	CHECK(source_voice_at(false, true, now, now) == SourceVoice::NotJudged);
	CHECK(source_voice_at(false, false, 0, now) == SourceVoice::NotJudged);
	CHECK(source_voice_at(true, false, now, now) == SourceVoice::Undeterminable);
	CHECK(source_voice_at(true, true, 0, now) == SourceVoice::Undeterminable);
	CHECK(source_voice_at(true, true, now, now) == SourceVoice::Determinable);
}

TEST_CASE("音声ソースの状態: 最後のパケットから 250 ms までは Determinable、超えると Undeterminable")
{
	const std::uint64_t last = 10'000'000'000ULL;
	CHECK(source_voice_at(true, true, last, last + 250'000'000ULL) == SourceVoice::Determinable);
	CHECK(source_voice_at(true, true, last, last + 250'000'001ULL) == SourceVoice::Undeterminable);
	// 読む時刻がパケットの時刻より前でも、届いているとみなす。
	CHECK(source_voice_at(true, true, last, last - 5'000'000ULL) == SourceVoice::Determinable);
}

TEST_CASE("3 つの状態の声を聞けるか: 見つかっている間は真、判定できない間は偽、指定なしは Discord の値")
{
	CHECK(source_voice_hears(SourceVoice::Determinable, false));
	CHECK(source_voice_hears(SourceVoice::Determinable, true));
	CHECK_FALSE(source_voice_hears(SourceVoice::Undeterminable, true));
	CHECK_FALSE(source_voice_hears(SourceVoice::Undeterminable, false));
	CHECK(source_voice_hears(SourceVoice::NotJudged, true));
	CHECK_FALSE(source_voice_hears(SourceVoice::NotJudged, false));
}

TEST_CASE("3 つの状態の発言の時刻: ソースで決める間はソースの時刻、判定できない間は空、指定なしは Discord の時刻")
{
	const SpeakingTimes discord{100, 0};
	const SpeakingTimes source{200, 0};
	CHECK(source_voice_speaking(SourceVoice::Determinable, discord, source).start_ns == 200);
	CHECK(source_voice_speaking(SourceVoice::Undeterminable, discord, source).start_ns == 0);
	CHECK(source_voice_speaking(SourceVoice::Undeterminable, discord, source).stop_ns == 0);
	CHECK(source_voice_speaking(SourceVoice::NotJudged, discord, source).start_ns == 100);
}

TEST_CASE(
	"3 つの状態から光と暗さ: 話している人は光って暗くならず、話していない人は光らず暗くなり、判定できない人は光らず暗くならない")
{
	BorderTiming timing;
	timing.rise_ms = 100;
	timing.hold_ms = 500;
	timing.fade_ms = 500;
	const std::uint64_t t0 = 20'000'000'000ULL;
	const std::uint64_t later = t0 + 5'000'000'000ULL;
	const SpeakingTimes talking{t0, 0};
	const SpeakingTimes ended{t0, t0 + 1'000'000'000ULL};
	const SpeakingTimes none{};
	const int dim_pct = 40;

	const auto lit = [&](SourceVoice voice, const SpeakingTimes &source, std::uint64_t now) {
		const SpeakingTimes used = source_voice_speaking(voice, SpeakingTimes{}, source);
		return composite_speaking_level(used, now, timing, true, false, 1.0f);
	};
	const auto dim = [&](SourceVoice voice, const SpeakingTimes &source, std::uint64_t now) {
		const SpeakingTimes used = source_voice_speaking(voice, SpeakingTimes{}, source);
		return composite_label_dim_at(used, now, timing, dim_pct, source_voice_hears(voice, false), false,
					      1.0f);
	};

	// 話している: 光る、暗くしない。
	CHECK(lit(SourceVoice::Determinable, talking, later) == doctest::Approx(1.0));
	CHECK(dim(SourceVoice::Determinable, talking, later) == doctest::Approx(1.0));
	// 話していない (話し終えてしばらくたった、または一度も話していない): 光らない、暗くなる。
	CHECK(lit(SourceVoice::Determinable, ended, later) == doctest::Approx(0.0));
	CHECK(dim(SourceVoice::Determinable, ended, later) == doctest::Approx(0.6));
	CHECK(dim(SourceVoice::Determinable, none, later) == doctest::Approx(0.6));
	// 判定できない: 話している最中に見失っても、光らず、暗くもしない。
	CHECK(lit(SourceVoice::Undeterminable, talking, later) == doctest::Approx(0.0));
	CHECK(dim(SourceVoice::Undeterminable, talking, later) == doctest::Approx(1.0));
	CHECK(dim(SourceVoice::Undeterminable, none, later) == doctest::Approx(1.0));
}

TEST_CASE("ソースの探し方: UUID が合う候補を先に選び、UUID が合わなければ名前が合う候補を選ぶ")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "マイク", true},
		{"u-2", "ゲーム音声", true},
		{"u-3", "マイク", true},
	};
	CHECK(find_self_source(all, "u-3", "マイク") == 2);     // UUID が優先 (同じ名前が 2 つあっても UUID で選ぶ)
	CHECK(find_self_source(all, "u-2", "別の名前") == 1);   // 名前を変えられていても UUID で見つかる
	CHECK(find_self_source(all, "u-9", "ゲーム音声") == 1); // UUID が変わっていても名前で見つかる
	CHECK(find_self_source(all, "u-9", "マイク") == 0);     // 同じ名前が複数あれば最初
	CHECK(find_self_source(all, "", "ゲーム音声") == 1);    // UUID が空なら名前だけで探す
}

TEST_CASE("ソースの探し方: UUID も名前も合わない、どちらも空、候補にしない種類だけが合うときは見つからない")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "マイク", true},
		{"u-2", "合成", false}, // cordmux の種類など、候補にしない
	};
	CHECK(find_self_source(all, "u-9", "存在しない") == -1);
	CHECK(find_self_source(all, "", "") == -1);
	CHECK(find_self_source(all, "u-2", "") == -1);
	CHECK(find_self_source(all, "", "合成") == -1);
	CHECK(find_self_source({}, "u-1", "マイク") == -1);
}

TEST_CASE("ソースの探し方: OBS で削除したソースは見つからず、同じ名前で作り直したソースが見つかる")
{
	const std::vector<SelfSourceCandidate> all = {
		{"u-1", "マイク", true, false, true}, // 削除した (参照が残る間は列挙に出る)
		{"u-4", "マイク", true, false, false},
	};
	CHECK(find_self_source(all, "u-1", "") == -1);
	CHECK(find_self_source(all, "u-1", "マイク") == 1);
	CHECK(find_self_source(all, "", "マイク") == 1);
}

TEST_CASE("cordmux の種類の ID は音声ソースの候補にしない")
{
	CHECK(is_cordmux_source_id("cordmux_composite"));
	CHECK(is_cordmux_source_id("cordmux_screen_source"));
	CHECK(is_cordmux_source_id("cordmux_voice_source"));
	CHECK(is_cordmux_source_id("cordmux_tile"));
	CHECK_FALSE(is_cordmux_source_id("wasapi_input_capture"));
	CHECK_FALSE(is_cordmux_source_id(""));
	CHECK_FALSE(is_cordmux_source_id("cordmux"));
}

TEST_CASE("しきい値の設定は -96 から 0 に収める")
{
	CHECK(clamp_self_speaking_threshold_db(-200) == -96);
	CHECK(clamp_self_speaking_threshold_db(-96) == -96);
	CHECK(clamp_self_speaking_threshold_db(-26) == -26);
	CHECK(clamp_self_speaking_threshold_db(0) == 0);
	CHECK(clamp_self_speaking_threshold_db(30) == 0);
}

TEST_CASE("peak の分布: 中央値、95 パーセンタイル、最大、話していると判定した割合を求め、取り出すと 0 に戻る")
{
	PeakHistogram hist;
	for (int i = 0; i < 90; ++i)
		hist.add(-60.4, false); // 切り捨てて -61 の位置
	for (int i = 0; i < 10; ++i)
		hist.add(-12.0, true);
	const PeakHistogram::Summary s = hist.take();
	CHECK(s.packets == 100);
	CHECK(s.p50_db == -61);
	CHECK(s.p95_db == -12);
	CHECK(s.max_db == -12);
	CHECK(s.talking_ratio == doctest::Approx(0.10));
	const PeakHistogram::Summary empty = hist.take();
	CHECK(empty.packets == 0);
	CHECK(empty.talking_ratio == 0.0);
}

TEST_CASE("peak の分布: 範囲の外の値は端に収めて数える")
{
	PeakHistogram hist;
	hist.add(-200.0, false);
	hist.add(5.0, false);
	const PeakHistogram::Summary s = hist.take();
	CHECK(s.packets == 2);
	CHECK(s.max_db == 0);
	CHECK(s.p50_db == -96);
}

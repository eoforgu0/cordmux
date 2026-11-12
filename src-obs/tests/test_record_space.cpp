// SPDX-License-Identifier: Apache-2.0

// record/record_space.h の単体テスト。録画中の消費、録画できる残り時間、録画ファイルの作成の途中で必要になる空き容量の見積もりを
// 確認する。

#include "doctest.h"

#include "record/record_space.h"

using cordmux::record::bytes_per_second;
using cordmux::record::bytes_per_second_of;
using cordmux::record::estimate_finalize_need_bytes;
using cordmux::record::FinalizeBranch;
using cordmux::record::FinalizeSpaceParams;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::MediaKind;
using cordmux::record::remaining_seconds;
using cordmux::record::session_span_us;
using cordmux::record::space_low;
using cordmux::record::StreamKind;

namespace {

// 10 分のセッションで、画面共有の映像 1 つ (600 MB) と音声 1 つ (230 MB) を書いた録画ログ。
LedgerDocument ten_minute_doc()
{
	LedgerDocument doc;
	doc.session.started_at_steady_us = 1'000'000;
	doc.session.ended_at_steady_us = 601'000'000; // 600 秒

	LedgerSegmentRow video;
	video.user_id = "1111";
	video.kind = StreamKind::Screen;
	video.media = MediaKind::Video;
	video.file = "segments/1111_screen_video_000001.mov";
	video.width = 1280;
	video.height = 720;
	video.fps_num = 60;
	video.fps_den = 1;
	video.timescale = 60000;
	video.duration_ticks = 600 * 60000;
	video.sample_count = 600 * 60;
	video.bytes = 600'000'000;
	doc.segments.push_back(video);

	LedgerSegmentRow audio;
	audio.user_id = "1111";
	audio.kind = StreamKind::Screen;
	audio.media = MediaKind::Audio;
	audio.file = "segments/1111_screen_audio_000001.mov";
	audio.timescale = 48000;
	audio.duration_ticks = 600 * 48000;
	audio.bytes = 230'400'000; // 600 秒 x 48000 x 2 チャンネル x 4 バイト
	doc.segments.push_back(audio);
	return doc;
}

} // namespace

TEST_CASE("録画中の消費を、録画ログのバイト数と経過時間から求める")
{
	const LedgerDocument doc = ten_minute_doc();
	CHECK(bytes_per_second(doc, 600'000'000) == (600'000'000 + 230'400'000) / 600);
	CHECK(bytes_per_second(doc, 0) == 0);
	CHECK(bytes_per_second(LedgerDocument{}, 600'000'000) == 0);
}

TEST_CASE("録画中の消費は、書いたバイト数を直接渡しても求められる")
{
	// 録画ログに行が載るのはセグメントを閉じたときだけなので、まだ 1 つも閉じていないセッションでは、録画ログから
	// 数える関数が 0 を返す。開いたままのセグメントの分を持っている呼び出し元は、それを加えた値を
	// bytes_per_second_of に渡す (渡さないと、切れ目のないセッションで空き容量の監視が 1 度も働かない)。
	CHECK(bytes_per_second(LedgerDocument{}, 10'000'000) == 0);
	CHECK(bytes_per_second_of(50'000'000, 10'000'000) == 5'000'000);

	// 経過時間もバイト数も、0 以下なら 0 を返す (分からない値を大きな数にしない)。
	CHECK(bytes_per_second_of(50'000'000, 0) == 0);
	CHECK(bytes_per_second_of(0, 10'000'000) == 0);
	CHECK(bytes_per_second_of(-1, 10'000'000) == 0);

	// 録画ログから数える関数は、同じ計算をその合計で呼ぶだけである。
	const LedgerDocument doc = ten_minute_doc();
	CHECK(bytes_per_second(doc, 600'000'000) == bytes_per_second_of(600'000'000 + 230'400'000, 600'000'000));
}

TEST_CASE("消費が分からなければ、残り時間は値を持たない")
{
	CHECK_FALSE(remaining_seconds(1'000'000'000, 0).has_value());
	CHECK_FALSE(remaining_seconds(0, -5).has_value());

	const auto remaining = remaining_seconds(1'000'000'000, 1'000'000);
	REQUIRE(remaining.has_value());
	CHECK(*remaining == 1000);

	// 空き容量が 0 でも、分かっている 0 秒は値を持つ (0 と、分からないことを区別する)。
	const auto none_left = remaining_seconds(0, 1'000'000);
	REQUIRE(none_left.has_value());
	CHECK(*none_left == 0);
}

TEST_CASE("残り時間がしきい値 (既定の 30 分) を下回ったときだけ、少ないと判定する")
{
	CHECK(space_low(std::optional<std::int64_t>(29 * 60), 30));
	CHECK_FALSE(space_low(std::optional<std::int64_t>(30 * 60), 30)); // 境: ちょうどのときは少ないとしない
	CHECK_FALSE(space_low(std::optional<std::int64_t>(31 * 60), 30));
	// 読めていないときは、少ないとしない。
	CHECK_FALSE(space_low(std::nullopt, 30));
}

TEST_CASE("セッションの長さは録画ログの session の行から求め、なければ映像のセグメントの長さを合計する")
{
	CHECK(session_span_us(ten_minute_doc()) == 600'000'000);

	LedgerDocument no_session = ten_minute_doc();
	no_session.session.started_at_steady_us = 0;
	no_session.session.ended_at_steady_us = 0;
	CHECK(session_span_us(no_session) == 600'000'000);

	CHECK(session_span_us(LedgerDocument{}) == 0);
}

TEST_CASE("Remux で録画ファイルを作成するときに必要な空き容量は、セグメントファイルとほぼ同じ大きさになる")
{
	const LedgerDocument doc = ten_minute_doc();
	FinalizeSpaceParams params;
	const std::uint64_t pcm = 600ull * 48000ull * 2ull * 4ull;

	const std::uint64_t need = estimate_finalize_need_bytes(doc, {FinalizeBranch::Remux}, params);
	CHECK(need == 600'000'000ull + pcm * 2);
}

TEST_CASE("ProRes の空き容量の見積もりは、それぞれの人の解像度で求める")
{
	const LedgerDocument one = ten_minute_doc();
	LedgerDocument two = one;
	// 別の人が、より長い時間、4 倍のピクセル数で共有している。1111 の見積もりは変わらず、2222 の見積もりは大きくなる。
	LedgerSegmentRow other = two.segments.front();
	other.user_id = "2222";
	other.file = "segments/2222_screen_video_000001.mov";
	other.width = 2560;
	other.height = 1440;
	other.duration_ticks = 6000 * 60000;
	two.segments.push_back(other);

	FinalizeSpaceParams params;
	const std::uint64_t pcm = 600ull * 48000ull * 2ull * 4ull;
	const std::uint64_t first = estimate_finalize_need_bytes(one, {FinalizeBranch::ProRes}, params);
	const std::uint64_t both =
		estimate_finalize_need_bytes(two, {FinalizeBranch::ProRes, FinalizeBranch::ProRes}, params);
	const std::uint64_t first_video = first - pcm * 2;
	CHECK(both == first + pcm * 2 + first_video * 4);
}

TEST_CASE("ProRes で録画ファイルを作成するときに必要な空き容量は、決まった倍率ではなく解像度と fps と時間から求める")
{
	const LedgerDocument doc = ten_minute_doc();
	FinalizeSpaceParams params;

	const std::uint64_t remux = estimate_finalize_need_bytes(doc, {FinalizeBranch::Remux}, params);
	const std::uint64_t prores = estimate_finalize_need_bytes(doc, {FinalizeBranch::ProRes}, params);
	CHECK(prores > remux * 4);

	// 同じ映像でも、1 ピクセルあたりのビット数を変えれば見積もりが変わる (実際の録画で測って置き換える値)。
	FinalizeSpaceParams heavier = params;
	heavier.prores_bits_per_pixel = params.prores_bits_per_pixel * 2.0;
	CHECK(estimate_finalize_need_bytes(doc, {FinalizeBranch::ProRes}, heavier) > prores);
}

TEST_CASE("録画ファイルの PCM の形が小さければ、見積もりも小さくなる")
{
	const LedgerDocument doc = ten_minute_doc();
	FinalizeSpaceParams f32;
	FinalizeSpaceParams i24;
	i24.pcm_bytes_per_sample = 3;
	CHECK(estimate_finalize_need_bytes(doc, {FinalizeBranch::Remux}, i24) <
	      estimate_finalize_need_bytes(doc, {FinalizeBranch::Remux}, f32));
}

TEST_CASE("作成方法の並びが食い違う呼び出しでは、すべてを Remux として見積もる")
{
	const LedgerDocument doc = ten_minute_doc();
	const std::uint64_t remux = estimate_finalize_need_bytes(doc, {FinalizeBranch::Remux});
	CHECK(estimate_finalize_need_bytes(doc, {}) == remux);
	CHECK(estimate_finalize_need_bytes(doc, {FinalizeBranch::ProRes, FinalizeBranch::ProRes}) == remux);
	CHECK(estimate_finalize_need_bytes(LedgerDocument{}, {}) == 0);
}

TEST_CASE("声だけの人の見積もりには、映像の分を加えない")
{
	LedgerDocument doc;
	doc.session.started_at_steady_us = 0;
	doc.session.ended_at_steady_us = 60'000'000;
	LedgerSegmentRow voice;
	voice.user_id = "2222";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/2222_voice_000001.mov";
	voice.timescale = 48000;
	voice.duration_ticks = 60 * 48000;
	voice.bytes = 23'040'000;
	doc.segments.push_back(voice);

	FinalizeSpaceParams params;
	const std::uint64_t pcm = 60ull * 48000ull * 2ull * 4ull;
	CHECK(estimate_finalize_need_bytes(doc, {FinalizeBranch::ProRes}, params) == pcm * 2);
}

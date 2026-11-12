// SPDX-License-Identifier: Apache-2.0

// align/av_offset.h (AvOffsetTranscriber) の単体テスト。

#include "doctest.h"

#include "align/av_offset.h"
#include "align/timestamp_aligner.h"
#include "fixtures.h"

#include <cstdint>
#include <vector>

using cordmux::align::AvOffsetConfig;
using cordmux::align::AvOffsetTranscriber;
using cordmux::align::AlignerConfig;
using cordmux::align::TimestampAligner;

TEST_CASE("映像に付いたオフセットが音声のタイムスタンプにも同じだけ足される")
{
	AvOffsetTranscriber av;
	av.note_video(1000000, 1012345); // 映像に +12345 us のオフセットが付いた
	CHECK(av.has_video());
	CHECK(av.video_offset_us() == 12345);
	CHECK(av.map_audio(2000000) == 2012345);
	CHECK(av.map_audio(2010000) == 2022345);
}

TEST_CASE("最初の映像が来る前の音声のタイムスタンプはそのまま返す")
{
	AvOffsetTranscriber av;
	CHECK_FALSE(av.has_video());
	CHECK(av.map_audio(500000) == 500000);
	CHECK(av.map_audio(520000) == 520000);
	CHECK(av.stats().passthrough_audio_frames == 2);

	av.note_video(1000000, 1008000);
	CHECK(av.map_audio(1000000) == 1008000);
	CHECK(av.stats().passthrough_audio_frames == 2);
}

TEST_CASE("constant_offset_us は映像が来た後の音声にだけ足され、後から変えられる")
{
	AvOffsetConfig cfg;
	cfg.constant_offset_us = 25000; // 音声を 25 ms 遅らせる
	AvOffsetTranscriber av(cfg);
	CHECK(av.constant_offset_us() == 25000);

	// 映像が来る前は定数も足さない。
	CHECK(av.map_audio(100000) == 100000);

	av.note_video(1000000, 1005000);
	CHECK(av.map_audio(1000000) == 1000000 + 5000 + 25000);

	// 後から変えられる。
	av.set_constant_offset_us(-8000);
	CHECK(av.map_audio(1000000) == 1000000 + 5000 - 8000);

	// 既定の 0 では何も足さない。
	AvOffsetTranscriber plain;
	plain.note_video(1000000, 1005000);
	CHECK(plain.map_audio(1000000) == 1005000);
}

TEST_CASE("再整列でオフセットが変わると音声のオフセットも同じだけ変わる")
{
	AvOffsetTranscriber av;
	av.note_video(1000000, 1005000);
	CHECK(av.map_audio(1000000) == 1005000);

	// 再整列で出力が送信側のタイムスタンプに合わせられ、オフセットが 5000 から 0 に変わった。
	av.note_video(1100000, 1100000);
	CHECK(av.video_offset_us() == 0);
	CHECK(av.map_audio(1100000) == 1100000);

	// 出力が送信側より先に出る向きでも、同じだけ変わる。
	av.note_video(1200000, 1230000);
	CHECK(av.map_audio(1200000) == 1230000);
	CHECK(av.stats().offset_changes == 3);
}

TEST_CASE("既定では音声のタイムスタンプの戻りを抑えず、enforce_monotonic_audio を true にすると戻らない")
{
	// 既定の false では、オフセットの変化をそのまま足すので、音声のタイムスタンプが戻ることがある。
	AvOffsetTranscriber pure;
	pure.note_video(1000000, 1050000);
	CHECK(pure.map_audio(1000000) == 1050000);
	pure.note_video(1010000, 1010000); // オフセットが 50 ms 減った
	CHECK(pure.map_audio(1010000) == 1010000);
	CHECK(pure.stats().monotonic_fix_count == 0);

	// true にすると戻らない。
	AvOffsetConfig cfg;
	cfg.enforce_monotonic_audio = true;
	AvOffsetTranscriber guarded(cfg);
	guarded.note_video(1000000, 1050000);
	CHECK(guarded.map_audio(1000000) == 1050000);
	guarded.note_video(1010000, 1010000);
	CHECK(guarded.map_audio(1010000) == 1050001);
	CHECK(guarded.stats().monotonic_fix_count == 1);
}

TEST_CASE("reset で、AvOffsetTranscriber を作った直後の状態に戻る")
{
	AvOffsetTranscriber av;
	av.note_video(1000000, 1005000);
	CHECK(av.map_audio(1000000) == 1005000);
	av.reset();
	CHECK_FALSE(av.has_video());
	CHECK(av.map_audio(1000000) == 1000000);
	CHECK(av.stats().video_frames == 0);
}

TEST_CASE("TimestampAligner の出力を渡すと、音声のオフセットが映像のオフセットと一致する")
{
	// pipeline/ と同じ使い方をする。映像の 1 フレームごとに align の結果を note_video に渡し、
	// 音声は map_audio に通す。
	const cordmux::tests::Fixture f = cordmux::tests::fixture_b(1800);
	TimestampAligner aligner;
	AvOffsetTranscriber av;

	int64_t audio_ts = f.ts_us.front();
	int64_t last_audio_out = 0;
	bool have_audio = false;
	int64_t checked = 0;

	for (size_t n = 0; n < f.ts_us.size(); n++) {
		const int64_t out = aligner.align(f.ts_us[n]);
		av.note_video(f.ts_us[n], out);

		// 音声は映像の周期とは関係なく、10 ms ごとに届くものとする。
		while (audio_ts <= f.ts_us[n]) {
			const int64_t a = av.map_audio(audio_ts);
			// 音声に足したオフセットが、そのときの映像のオフセットと一致する。
			CHECK(a - audio_ts == out - f.ts_us[n]);
			if (have_audio)
				REQUIRE(a > last_audio_out); // ジッターだけの列では音声も単調に増える
			last_audio_out = a;
			have_audio = true;
			audio_ts += 10000;
			checked++;
		}
	}
	CHECK(checked > 1000);
	CHECK(av.stats().video_frames == int64_t(f.ts_us.size()));
}

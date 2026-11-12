// SPDX-License-Identifier: Apache-2.0

// 参加者ごとの声と画面の音を、別々の利得で混ぜて、時刻の途切れない 1 本の音にするコンポーネント (AudioMixer)。合成のソース
// 1 つから全員の音を出すために使う。OBS は 1 つのソースに時刻を重ねて渡された音を、後に渡した音で前の音を
// 消す形で置く (libobs の obs-source.c の source_output_audio_place は、時刻の位置に deque_place で上書きし、
// その後ろを deque_pop_back で切り捨てる)。参加者ごとの音をそのまま同じソースに渡すと互いに消し合うので、cordmux
// が自分で混ぜて渡す。時刻はすべて呼び出し元が渡し、ここでは時計を読まないので、単体テストで固定の時刻を与えて
// 動作を確認できる。OBS への受け渡し (obs_source_output_audio) は呼び出し元にある。
// libobs にも asio にも依存しない。ロックは持たず、呼び出し元のロックの中で使う。
//
// 時刻の領域は 1 つで、呼び出し元が OBS の時計 (os_gettime_ns / 1000) に変換した値 (マイクロ秒) である。add の
// at_us (チャンクを置く時刻) も、take の now_us (今) も、出す区間の開始の時刻も、この領域にある。画面の音に
// 映像と同じ D を足すことも、声に届き方の揺れを吸う幅 J と声の遅延 (voice_audio_delay_ms) を足すことも、
// 呼び出し元が at_us に入れて渡す。このコンポーネントは、組ごとに置く位置を渡された at_us に追従させる
// (align/audio_drift.h) だけで、D も J も知らない。
//
// 利得は出すときに掛ける。置くときに掛けると、画面の音では D の分だけ後に出す音まで古い利得で掛かっていて、
// ミキサーの操作が D だけ遅れて聞こえる。出すときなら、操作は次の区間から聞こえる。
//
// 音の振れ幅を -1 から 1 に切りそろえるのは、ソースの音量 (ミキサーのフェーダー) を掛けた後の最後の段階である
// (libobs の media-io/audio-io.c の clamp_audio_output)。ここで切りそろえると、利用者がミキサーで音量を下げても
// 切りそろえた後の割れた音が残るので、切りそろえない。和が 1 を超えた音はそのまま出す。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "align/audio_drift.h"
#include "pipeline/audio_meter.h"

namespace cordmux::pipeline {

// 音のサンプルレートとチャンネル数 (pipeline.cpp の kAudioSampleRate と同じ。48 kHz のステレオ、float のインターリーブ)。
constexpr std::int64_t kMixerSampleRate = 48000;
constexpr std::size_t kMixerChannels = 2;

// OBS が 1 回に混ぜる長さ (マイクロ秒)。libobs の AUDIO_OUTPUT_FRAMES = 1024 フレームを 48 kHz で割った約 21.33 ms
// を切り上げた値である。OBS の音の処理 (obs-audio.c の audio_callback) は、この長さの区間ごとに、ソースの溜まりから
// 1024 フレーム分を取って混ぜる。次の 3 か所が、溜まりが足りないときの振る舞いを決める。
//   - media-io/audio-io.c の audio_thread: 区間 [s, s + この長さ] は、実時間が区間の終わりを過ぎてから混ぜる。
//   - obs-source.c の process_audio_source_tick (5297-5308 行あたり): 溜まりが 1024 フレームに満たないと、溜まりの
//     先頭の時刻 (audio_ts) がどこにあっても、そのソースを保留にして混ぜない。
//   - obs-audio.c の discard_audio (245-256 行あたり): 区間の終わりまでの音が溜まっていないと、溜まりの音を捨てずに
//     audio_ts を区間の終わりにする。溜まりの先頭の音に区間の終わりの時刻が付き直り、以後の音は続きに積まれる
//     (push_back) ので、その分 (最大でこの長さ) だけソースの音が遅れたまま戻らない。その場合は無音になり、ログにも何も
//     出ない。区間の終わりまでは溜まっているが 1024 フレームに満たないときは、混ぜずに捨てる (その分の音が欠ける)。
constexpr std::int64_t kObsMixSpanUs = 21334;

// 先渡しの幅の既定 (マイクロ秒)。区間の終わりが now_us + この値以内になったら出す。
// 守るべき条件は 2 つある (どちらも kObsMixSpanUs の説明の振る舞いから)。
//   (1) 定常: 今より先に渡してある長さ (出し終えた位置の時刻 - 今) が、どの瞬間も 0 以上。OBS は区間の終わりを
//       過ぎてから混ぜるので、そのとき区間の終わりまでの音が溜まっていれば足りる。破ると、合成のソースの音が
//       1 回分 (約 21.33 ms) 遅れたまま戻らず、続けて起きると積もる。
//   (2) 起点の直後: 最初の音の時刻は OBS の区間の途中に来るので、OBS は最初の音の時刻から 1 回分が溜まるまで
//       混ぜない。最初の take は起点から (この値 - 区間の長さ) より先まで出すので、この値は 区間の長さ + 1 回分 以上に
//       する (下の static_assert)。破ると、最初の区間の音が欠けるか、その分だけ遅れたまま戻らない。
// 先に渡してある長さは take の直前に最も短く、この値から「区間の終わりで出す丸め (最大 10 ms) + 取り出しの間隔 10 ms +
// 取り出しの起きる遅れ」を引いた長さまで減る。55 ms は、取り出しが 35 ms 遅れても (1) を守る値である ((2) は 31.33 ms
// で足りる)。長くすると、声と画面の音を置く時刻の下限 (J。composite_audio_plan.h) が同じだけ延び、声がその分遅れて
// 鳴る。短くするかは、Stats mixer の mixer_min_ahead_ms (取り出しの起きる遅れの実測) を見て決める。
// 先に渡してもタイムスタンプはその時刻のままなので、(1) と (2) を守っていれば、音が出る時刻は遅れない。
constexpr std::int64_t kMixerLookaheadUs = 55000;
static_assert(kMixerLookaheadUs - 480 * 1000000 / kMixerSampleRate >= kObsMixSpanUs,
	      "最初の take で、既定の区間 (480 フレーム) の丸めの後も OBS の 1 回分より先まで出す");

// 入力の種類。声と画面の音は、同じ人でも別々に利得を持つ。
enum class MixerKind { Voice, ScreenAudio };

// 入力の単位 (人と種類の組)。
struct MixerKey {
	std::string user_id;
	MixerKind kind = MixerKind::Voice;
};

inline bool operator<(const MixerKey &a, const MixerKey &b)
{
	if (a.user_id != b.user_id)
		return a.user_id < b.user_id;
	return static_cast<int>(a.kind) < static_cast<int>(b.kind);
}

inline bool operator==(const MixerKey &a, const MixerKey &b)
{
	return a.user_id == b.user_id && a.kind == b.kind;
}

// 組 1 つの音の大きさ (AudioMixer::levels)。値は線形の振れ幅で、dBFS にするには peak_to_dbfs を使う。
struct AudioTrackLevel {
	MixerKey key;
	// 利得とミュートを掛ける前の値。受け取った音の大きさで、ミュート中や音量 0 の間も動く。
	float peak_before = 0.0f;
	// 掛けた後の値。配信に出ている音の大きさ。
	float peak_after = 0.0f;
	// ミュートか音量 0 か。真の間は peak_after が 0 になるので、メーターは peak_before を灰色で出す。
	bool silenced = false;
};

// 組ごとに保持する長さ (フレーム数) を、時刻の先の最大から計算する。出し終えた位置より先に置かれうる最も遠い音は、
// 画面の音なら D (上限 1000 ms)、声なら声の遅延 (settings の voice_audio_delay_ms。上限 2000 ms) に、届き方の揺れを
// 吸う幅 J と先渡しの幅を足した位置にあり、チャンク 1 つ分の長さがその先に伸びる。固定の値にせず、呼び出し元の
// 設定の上限から計算する。
//   max_delay_us  D と声の遅延の大きいほうの上限 (マイクロ秒)
//   jitter_us     揺れを吸う幅 J
//   lookahead_us  先渡しの幅 (AudioMixerConfig::lookahead_us と同じ値)
//   chunk_frames  チャンク 1 つのフレーム数の上限 (送信側は 10 ms = 480)
// 負の引数は 0 として扱う。
inline std::int64_t audio_mixer_retention_frames(std::int64_t max_delay_us, std::int64_t jitter_us,
						 std::int64_t lookahead_us, std::int64_t chunk_frames)
{
	const std::int64_t span_us = std::max<std::int64_t>(max_delay_us, 0) + std::max<std::int64_t>(jitter_us, 0) +
				     std::max<std::int64_t>(lookahead_us, 0);
	return (span_us * kMixerSampleRate + 999999) / 1000000 + std::max<std::int64_t>(chunk_frames, 0);
}

// AudioMixer の設定。既定の値の理由は各項目に書く。
struct AudioMixerConfig {
	// 出す区間の長さ (フレーム)。480 は 10 ms で、送信側のチャンクと同じ長さである。区間の開始の時刻は
	// 起点からのサンプル数で計算するので、10 ms の倍数でなくても丸めの誤差は積もらない。
	std::int64_t block_frames = 480;

	// 先渡しの幅 (マイクロ秒)。区間の終わりが now_us + この値以内になったら出す。既定の値と内訳、守るべき条件は
	// kMixerLookaheadUs に書いた。区間の開始のタイムスタンプは now_us から now_us + この値 - 区間の長さ の間になり、
	// 取り出す側の呼び出しの間隔の揺れがあっても、タイムスタンプは今より古くならない。OBS はタイムスタンプが混ぜている区間より
	// 古いと全体の音の待ち時間を足し (obs-audio.c の add_audio_buffering)、OBS を閉じるまで戻さないので、古くしない。
	// 制約: 区間の終わり近くの音は、その時刻のほぼこの値だけ前に出す。声を置く at_us は届いた時刻 + J で、
	// 声のチャンクの先頭が区間のどこに来るかは決まっていないので、J はこの値以上にする。短いと、チャンクの
	// 先頭が届く前にその区間を出してしまう (遅れて届いた分として捨てる)。
	std::int64_t lookahead_us = kMixerLookaheadUs;

	// 組ごとに、置く位置を at_us に追従させる決め方 (align/audio_drift.h)。前のチャンクの続きと at_us の差は、
	// 置き直しのしきい値の内なら速さの補正でゆっくり戻し、超えたら at_us に置き直す。録画と同じ決め方にする。
	align::AudioDriftConfig drift{};

	// 声の組の置き直しのしきい値 (マイクロ秒)。声の組では drift.resync_us の代わりにこの値を使う。声の at_us は届いた時刻
	// から作るので、送信側の JavaScript が止まった後にまとめて届くと、続きの位置と at_us が離れる。続きより前へ置き直すと、
	// 置いたのにまだ出していない声を上書きして失い、後ろへ置き直すと、その分の無音が入る。置き直さずに速さで戻すと、声の
	// 遅れがしばらく増えるが、声は失わない。声には対になる映像がないので、画面の音 (音と映像のずれに気付く 45 ms) より
	// 広くしてよい。70 ms は、OBS がソースの音の時刻の揺れを続きとして扱う幅 (obs-source.c の TS_SMOOTHING_THRESHOLD) と
	// 同じ値で、失う声と遅れの増え方の釣り合いを、OBS のふつうのソースと同じにする。設定 (voice_audio_resync_ms) で変えられる。
	std::int64_t voice_resync_us = 70000;

	// 取り出しが遅れたときに、遅れた区間を出す限度 (マイクロ秒)。区間の開始の時刻が now_us からこの値より
	// 古くなる区間は出さずに飛ばす。古いタイムスタンプの音をまとめて渡すと OBS が待ち時間を足すので、音を一部
	// 失っても足させない側に倒す。既定の 20 ms は OBS が 1 回に混ぜる長さ (kObsMixSpanUs) より約 1 ms 短い。
	// 先渡しの幅を変えても動かさない: この限度が守るのは、区間の開始のタイムスタンプが OBS の混ぜている区間より古くならない
	// ことで、OBS が混ぜている区間の始まりは now_us から約 1 回分 (21.33 ms) 以内の過去にあり、先渡しの幅には
	// 依らないからである。OBS が混ぜている区間の始まりがどこまで過去かは測っていないので、実機で
	// 「adding ... milliseconds of audio buffering」が出たら小さくする。
	std::int64_t late_limit_us = 20000;

	// 組ごとに保持するフレーム数 (出し終えた位置からの先の最大)。これを超えて先に置かれる音は捨てる。
	// 既定は D と声の遅延の上限 2000 ms (settings の kVoiceAudioDelayMaxMs。D の上限 1000 ms より大きい)、
	// 揺れを吸う幅 200 ms (この計算にだけ使う値で、声を置く時刻の J (kVoiceMixJitterUs、65 ms) より大きく取る)、
	// 先渡しの幅 kMixerLookaheadUs、チャンク 480 フレームから計算する。メモリは、1 組につき
	// フレーム数 x 2 チャンネル x 4 バイトで、既定は 108720 フレーム = 約 0.87 MB。声 30 人と
	// 画面の音 4 つの 34 組が全部音を出していても約 30 MB である。音を出していない組は確保しない。
	std::int64_t max_ahead_frames = audio_mixer_retention_frames(2000000, 200000, kMixerLookaheadUs, 480);

	// 声の組に「聞こえる音があった」と数える、区間の中の音の振れ幅の下限 (利得とミュートを掛けた後の、チャンネルの
	// 絶対値の最大。set_activity_before_gain を真にした組は、掛ける前の値)。-60 dBFS で、話している人の枠の光を出す
	// 条件に使う (MixedBlock::audible_voices)。
	// Discord は話していない間は音を送らず、送られない間は組に音がない (出し終えた位置より先に音がない) ので、
	// 無音の間はそもそも対象にならない。この値が除くのは、ミュートや利得 0 への直線の移りの末尾と、
	// 浮動小数点数のごく小さな値である。話し声のふつうの振れ幅は、これよりずっと大きいと見込んでいるが、小さく
	// 話したときの振れ幅は測っていない (テスト用の送信側や実際の通話で、小さく話した声の振れ幅を測れば、決め直せる)。
	// 利用者が音量を下げて聞こえなくなった人を光らせるかは、このしきい値と利得で決まる (利得を掛けた後の値で比べる)。
	// 掛ける前の値で比べる組 (自分の声) は、ミュートや利得 0 でも、受け取った声があれば数える。
	float audible_peak = 0.001f;
};

// 1 つの組の統計 (AudioMixer::take_stats)。
struct AudioMixerKeyStats {
	MixerKey key;
	std::uint64_t placed_frames = 0; // 置いたフレーム数
	std::uint64_t late_frames = 0;   // 出し終えた位置より前に掛かっていて、捨てたフレーム数
	std::uint64_t ahead_frames = 0;  // 保持の上限より先で、捨てたフレーム数
	std::uint64_t replaced = 0;      // 置き直しのしきい値を超えたか、出し終えた位置より遅れて、at_us に置き直した回数
	// 周期の終わりの、推定した時計どうしの速さの差 (ppm。align::AudioDriftStats::drift_ppm)。
	double drift_ppm = 0.0;
	// 周期の間の、at_us と続きの位置の差の絶対値の最大 (us。置き直したチャンクは含まない)。
	double max_error_us = 0.0;
};

// 統計の 1 周期分。
struct AudioMixerStats {
	std::vector<AudioMixerKeyStats> keys; // 周期の間に何かを数えた組だけ
	std::uint64_t blocks = 0;             // 出した区間の数
	// 区間の開始の時刻が take の now_us より前で出した区間の数。取り出しが遅れて、先渡しの幅を使い切った分である。
	std::uint64_t catch_up_blocks = 0;
	std::uint64_t skips = 0;          // 遅れが限度を超えて、区間を飛ばした回数
	std::uint64_t skipped_frames = 0; // 飛ばした長さ (フレーム)
	// 周期の間の、take のはじめ (区間を出す前) の、今より先に渡してある長さの最小 (マイクロ秒。出し終えた位置の時刻 -
	// now_us)。先に渡してある長さは take の直前に最も短いので、ここで測る。起点ができた後の最初の take は測らない。
	// 周期の間に 1 回も測らなかったときは値を持たない。
	std::optional<std::int64_t> min_ahead_us;
	// 周期の間に、先に渡してある長さが 0 を下回っていた (先に渡した分を使い切っていた) take の回数。その間に OBS が
	// 区間を混ぜていれば、溜まりの先頭の時刻を付け直して、音が遅れたまま戻らない (kMixerLookaheadUs の (1))。
	// 測らなかった take は数えない。
	std::uint64_t short_ahead_takes = 0;
};

// 混ぜて出した区間 1 つ。
struct MixedBlock {
	std::int64_t start_us = 0;  // 区間の開始の時刻 (obs_source_audio の timestamp にする)
	std::size_t frames = 0;     // フレーム数
	std::vector<float> samples; // frames x 2 チャンネルのインターリーブ。切りそろえていない
	// この区間に聞こえる音 (AudioMixerConfig::audible_peak を超える音) があった声の組の user_id。画面の音の組と、
	// remove した後に 0 へ移している最中の組は含まない。並びは user_id の昇順。時刻は start_us (音が鳴る時刻)。
	// 振れ幅は利得とミュートを掛けた後の値で見る。set_activity_before_gain を真にした組は、掛ける前の値で見る。
	std::vector<std::string> audible_voices;
};

// 起点の決め方。最初の add か take の now_us を起点にして、無音の区間から出し始める。
// 理由: (1) 最初の音の位置 (at_us) から始めると、画面の音が D を足した遠い位置から先に届いたとき、後から届く
// 声の at_us が起点より前になり、声が全部「遅れて届いた分」として捨てられる。(2) now_us より前の時刻から始めると、
// 最初の区間からタイムスタンプが古くなり、OBS が待ち時間を足す。now_us から始めれば、タイムスタンプは今以後で、どの組の音も
// その位置より後に置ける。先頭の無音は、時刻の途切れない 1 本にするために出す。出し続けるのをやめるのは、
// 呼び出し元が reset するときである。
//
// 区間の位置は起点からのサンプル数で数える。時刻 (マイクロ秒) とサンプルの位置の変換は、そのつど整数で
// 四捨五入するので、チャンクごとの丸めの誤差が区間の開始の時刻に積もらない。
class AudioMixer {
public:
	AudioMixer() : AudioMixer(AudioMixerConfig{}) {}

	explicit AudioMixer(const AudioMixerConfig &config) : config_(sanitize(config)) {}

	// チャンクを置く。
	//   now_us  今 (起点が未定ならこの値が起点になる)
	//   key     人と種類の組
	//   at_us   チャンクの先頭を置く時刻
	//   samples frames x 2 チャンネルのインターリーブ (48 kHz の float)
	// 置く位置は組ごとの align::AudioDriftCorrector が決める。前のチャンクの続きに置き、続きの位置と at_us の差は
	// 速さの補正 (ごくわずかなサンプルレートの変換) で戻す。差が置き直しのしきい値を超えたとき、または続きに置くと
	// 出し終えた位置より前になるときは、at_us に置き直して数える。置き直しで前に戻ったときは、その組のそこから後ろを
	// 上書きする (OBS が同じ時刻に置き直した音の後ろを切り捨てるのと同じ)。出し終えた位置より前の部分は捨てて数える。
	// 保持の上限より先に掛かる場合は、チャンク全体を捨てて数える。
	void add(std::int64_t now_us, const MixerKey &key, std::int64_t at_us, const float *samples, std::size_t frames)
	{
		ensure_origin(now_us);
		Track &track = track_for(key);
		if (frames == 0 || samples == nullptr)
			return;

		const std::uint64_t resyncs_before = track.corrector.stats().resyncs;
		const align::AudioDriftStep step =
			track.corrector.push(exact_pos_from_us(at_us), samples, frames, &converted_, out_pos_);
		if (track.corrector.stats().resyncs != resyncs_before)
			++track.stats.replaced;
		if (!step.resynced)
			track.stats.max_error_us = std::max(track.stats.max_error_us, step.error_us < 0 ? -step.error_us
												 : step.error_us);
		track.stats.drift_ppm = track.corrector.stats().drift_ppm;
		samples = converted_.data();
		const std::int64_t pos = step.start_pos;
		const std::int64_t n = static_cast<std::int64_t>(step.frames);
		if (n == 0)
			return;

		const bool went_back = pos < track.valid_end;
		std::int64_t first = pos;
		std::int64_t skip_front = 0;
		if (first < out_pos_) {
			skip_front = std::min(out_pos_ - first, n);
			track.stats.late_frames += static_cast<std::uint64_t>(skip_front);
			first += skip_front;
		}
		const std::int64_t count = n - skip_front;
		if (count <= 0) {
			// 全部が遅れて届いた分。前に戻る置き直しなら、そこから後ろは上書きされてなくなる。
			if (went_back)
				track.valid_end = std::max(pos + n, out_pos_);
			return;
		}
		if (first + count > out_pos_ + config_.max_ahead_frames) {
			track.stats.ahead_frames += static_cast<std::uint64_t>(count);
			return;
		}

		if (track.ring.empty())
			track.ring.assign(static_cast<std::size_t>(config_.max_ahead_frames) * kMixerChannels, 0.0f);

		const std::int64_t data_end = std::max(track.valid_end, out_pos_);
		if (first > data_end)
			fill_zero(track, data_end, first);
		const std::int64_t cap = config_.max_ahead_frames;
		for (std::int64_t i = 0; i < count; ++i) {
			const std::size_t ring_index = static_cast<std::size_t>((first + i) % cap) * kMixerChannels;
			const std::size_t src = static_cast<std::size_t>(skip_front + i) * kMixerChannels;
			for (std::size_t c = 0; c < kMixerChannels; ++c)
				track.ring[ring_index + c] = samples[src + c];
		}
		// 置いた音の後ろは、前に戻ったときだけでなく続きに置いたときも、ここで終わる (続きの後ろには何もない)。
		track.valid_end = first + count;
		track.stats.placed_frames += static_cast<std::uint64_t>(count);
	}

	// 次の add を必ず at_us に置き直す (D を変えたとき、ストリームが始め直したとき)。置き直しは数えない。
	// 推定した時計どうしの速さの差は残す。
	void resync(const MixerKey &key) { track_for(key).corrector.reset(); }

	// 組の利得 (線形)。出す区間の中で、前の値から新しい値へ直線で移る。組がなければ作る。
	void set_gain(const MixerKey &key, float gain)
	{
		Track &track = track_for(key);
		track.gain = (gain >= 0.0f) ? gain : 0.0f; // 負の値と NaN は 0
		settle_if_silent(track);
	}

	// 組のミュート。利得 0 と同じ扱いで、利得の値は変えない (解除すると元の利得へ直線で戻る)。
	void set_mute(const MixerKey &key, bool muted)
	{
		Track &track = track_for(key);
		track.muted = muted;
		settle_if_silent(track);
	}

	// 組の「聞こえる音があった」(MixedBlock::audible_voices) を、利得とミュートを掛ける前の音で判定するか。既定は偽
	// (掛けた後の音)。真にした組は、ミュートしていても、受け取った音があれば話していると数える (自分の声。
	// pipeline/mixer_roster.h の MixerLevels::activity_before_gain)。出す音には関係しない。remove すると既定に戻る。
	void set_activity_before_gain(const MixerKey &key, bool before_gain)
	{
		track_for(key).activity_before_gain = before_gain;
	}

	// 組ごとの音の大きさ (利得を掛ける前と後のピーク)。区間を出すたびに更新した保持値のコピーで、読んでも値は変わらない
	// (音が上がるときはすぐ、下がるときは audio_meter.h の速さで動く)。並びは MixerKey の順。組の数は話した人の数に比例するが、30 人でも
	// 34 組なので、呼び出し元のロックの中でコピーしても短い。take していない間 (合成のソースがない間) は、reset が 0 に戻した値のまま。
	std::vector<AudioTrackLevel> levels() const
	{
		std::vector<AudioTrackLevel> out;
		out.reserve(tracks_.size());
		for (const auto &entry : tracks_) {
			const Track &track = entry.second;
			out.push_back(AudioTrackLevel{entry.first, track.meter_before.value(), track.meter_after.value(),
						      !(track.target() > 0.0f)});
		}
		return out;
	}

	// 組を外す (人が抜けた、画面共有をやめた)。次の区間で利得を 0 へ移してから捨てる。外した後に同じ組へ add
	// すると、利得 1・ミュートなしの新しい組になる。
	void remove(const MixerKey &key)
	{
		const auto it = tracks_.find(key);
		if (it == tracks_.end())
			return;
		retire_stats(key, it->second.stats);
		it->second.stats = AudioMixerKeyStats{};
		if (it->second.valid_end > out_pos_) {
			it->second.muted = true; // 次の区間で 0 へ移す
			fading_.push_back(std::move(it->second));
		}
		tracks_.erase(it);
	}

	// now_us の時点で出す時刻になった区間を、時刻の途切れない順に返す。区間の終わりが now_us + 先渡しの幅
	// 以内になったものが対象で、音のない区間も無音として返す。取り出しが遅れて区間がたまっていたら続けて全部
	// 返す。ただし開始の時刻が now_us から late_limit_us より古くなる区間は返さずに飛ばして数える。
	// 飛ばすときは起点を動かさず、区間の格子の上で区間の数だけ先へ進むので、
	// 飛ばした後も区間の開始の時刻は同じ格子の上にあり、丸めの誤差が積もらない。
	std::vector<MixedBlock> take(std::int64_t now_us)
	{
		ensure_origin(now_us);
		std::vector<MixedBlock> out;

		// 今より先に渡してある長さは、区間を出す前に測る (出した後や飛ばした後だと、不足が見えなくなる)。
		// 起点ができた直後の最初の take は、出し終えた位置が起点のままで先に渡した音がないので測らない。
		if (ahead_measurable_) {
			const std::int64_t ahead_us = us_from_pos(out_pos_) - now_us;
			if (!stats_.min_ahead_us || ahead_us < *stats_.min_ahead_us)
				stats_.min_ahead_us = ahead_us;
			if (ahead_us < 0)
				++stats_.short_ahead_takes;
		}
		ahead_measurable_ = true;

		const std::int64_t stale_before = pos_from_us(now_us - config_.late_limit_us);
		if (out_pos_ < stale_before) {
			const std::int64_t gap = stale_before - out_pos_;
			const std::int64_t blocks = (gap + config_.block_frames - 1) / config_.block_frames;
			const std::int64_t frames = blocks * config_.block_frames;
			out_pos_ += frames;
			++stats_.skips;
			stats_.skipped_frames += static_cast<std::uint64_t>(frames);
		}

		while (us_from_pos(out_pos_ + config_.block_frames) <= now_us + config_.lookahead_us) {
			MixedBlock block;
			block.start_us = us_from_pos(out_pos_);
			block.frames = static_cast<std::size_t>(config_.block_frames);
			mix_block(&block);
			if (block.start_us < now_us)
				++stats_.catch_up_blocks;
			++stats_.blocks;
			out_pos_ += config_.block_frames;
			out.push_back(std::move(block));
		}
		return out;
	}

	// 起点を忘れ、保持している音と、置き直しの基準を捨てる (pipeline の停止と再開)。組と利得とミュートは残る。
	// 統計は残す。
	void reset()
	{
		origin_us_.reset();
		out_pos_ = 0;
		ahead_measurable_ = false;
		fading_.clear();
		for (auto &entry : tracks_) {
			Track &track = entry.second;
			track.valid_end = 0;
			track.corrector.reset();
			track.applied_gain = track.target();
			track.meter_before.clear();
			track.meter_after.clear();
		}
	}

	// 統計の周期ごとに呼ぶ。数を取り出して 0 に戻す。
	AudioMixerStats take_stats()
	{
		AudioMixerStats result = stats_;
		stats_ = AudioMixerStats{};
		result.keys = std::move(retired_);
		retired_.clear();
		for (auto &entry : tracks_) {
			AudioMixerKeyStats &s = entry.second.stats;
			if (s.placed_frames || s.late_frames || s.ahead_frames || s.replaced) {
				s.key = entry.first;
				result.keys.push_back(s);
				const double drift_ppm = s.drift_ppm;
				s = AudioMixerKeyStats{};
				s.drift_ppm = drift_ppm;
			}
		}
		return result;
	}

	// 起点 (最初の add か take の now_us)。未定なら値を持たない。
	std::optional<std::int64_t> origin_us() const { return origin_us_; }

	// 次に出す区間の開始の時刻。起点が未定なら値を持たない。
	std::optional<std::int64_t> next_block_start_us() const
	{
		if (!origin_us_)
			return std::nullopt;
		return us_from_pos(out_pos_);
	}

	const AudioMixerConfig &config() const { return config_; }
	std::size_t track_count() const { return tracks_.size(); }

private:
	struct Track {
		explicit Track(const align::AudioDriftConfig &drift) : corrector(drift, kMixerChannels) {}

		float gain = 1.0f;
		bool muted = false;
		float applied_gain = 1.0f; // 直前の区間の終わりで掛けていた利得
		bool activity_before_gain = false; // 聞こえる音の判定を、利得を掛ける前の音で行うか
		std::vector<float> ring; // 保持する音。位置 % 上限 の場所に置く。音が来るまで空
		std::int64_t valid_end = 0; // 置いた音の終わりの位置。これ以後は無音
		align::AudioDriftCorrector corrector; // 置く位置を at_us に追従させる
		AudioMixerKeyStats stats;
		PeakHold meter_before; // 利得を掛ける前のピークの保持値 (levels)
		PeakHold meter_after;  // 掛けた後のピークの保持値

		float target() const { return muted ? 0.0f : gain; }
	};

	// 出す位置より先に音がない組は、利得を変えても聞こえる音がないので、移さずにすぐ目標の値にする
	// (音が来る前に利得を決めたとき、最初の音が不要な立ち上がりを持たないため)。
	void settle_if_silent(Track &track) const
	{
		if (track.valid_end <= out_pos_)
			track.applied_gain = track.target();
	}

	static AudioMixerConfig sanitize(AudioMixerConfig c)
	{
		if (c.block_frames <= 0)
			c.block_frames = 1;
		if (c.lookahead_us < 0)
			c.lookahead_us = 0;
		if (c.late_limit_us < 0)
			c.late_limit_us = 0;
		// 追従の位置はこのミキサーのサンプルの番号で数えるので、レートはミキサーのものにそろえる。
		c.drift.sample_rate = static_cast<int>(kMixerSampleRate);
		if (c.voice_resync_us < 1)
			c.voice_resync_us = 1;
		if (c.max_ahead_frames < c.block_frames)
			c.max_ahead_frames = c.block_frames;
		if (!(c.audible_peak >= 0.0f))
			c.audible_peak = 0.0f; // 負の値と NaN は 0
		return c;
	}

	static std::int64_t floor_div(std::int64_t a, std::int64_t b)
	{
		std::int64_t q = a / b;
		if ((a % b != 0) && ((a < 0) != (b < 0)))
			--q;
		return q;
	}

	void ensure_origin(std::int64_t now_us)
	{
		if (!origin_us_) {
			origin_us_ = now_us;
			out_pos_ = 0;
			ahead_measurable_ = false;
		}
	}

	// 時刻と、起点からのサンプルの位置の変換。そのつど四捨五入し、誤差を持ち越さない。
	std::int64_t pos_from_us(std::int64_t t_us) const
	{
		return floor_div((t_us - *origin_us_) * kMixerSampleRate + 500000, 1000000);
	}

	std::int64_t us_from_pos(std::int64_t pos) const
	{
		return *origin_us_ + floor_div(pos * 1000000 + kMixerSampleRate / 2, kMixerSampleRate);
	}

	// 丸めない位置 (小数)。追従の目標に使う。整数に丸めると、時刻のマイクロ秒の端数が 0.5 サンプルの単位の段になって、
	// 追従がその段を追いかける。
	double exact_pos_from_us(std::int64_t t_us) const
	{
		return double(t_us - *origin_us_) * double(kMixerSampleRate) / 1e6;
	}

	Track &track_for(const MixerKey &key)
	{
		auto it = tracks_.find(key);
		if (it == tracks_.end()) {
			align::AudioDriftConfig drift = config_.drift;
			if (key.kind == MixerKind::Voice)
				drift.resync_us = config_.voice_resync_us;
			it = tracks_.emplace(key, Track(drift)).first;
		}
		return it->second;
	}

	void retire_stats(const MixerKey &key, const AudioMixerKeyStats &s)
	{
		if (s.placed_frames || s.late_frames || s.ahead_frames || s.replaced) {
			AudioMixerKeyStats copy = s;
			copy.key = key;
			retired_.push_back(copy);
		}
	}

	void fill_zero(Track &track, std::int64_t from, std::int64_t to)
	{
		const std::int64_t cap = config_.max_ahead_frames;
		for (std::int64_t p = from; p < to; ++p) {
			const std::size_t ring_index = static_cast<std::size_t>(p % cap) * kMixerChannels;
			for (std::size_t c = 0; c < kMixerChannels; ++c)
				track.ring[ring_index + c] = 0.0f;
		}
	}

	// 1 つの組の音を、前の区間の終わりの利得から現在の目標の利得へ直線で移しながら、区間に足す。足した音の
	// 振れ幅 (利得を掛けた後のチャンネルの絶対値の最大) を返す。raw_peak には、利得を掛ける前の振れ幅を入れる。
	float mix_track(Track &track, float target, MixedBlock *block, float *raw_peak = nullptr) const
	{
		float peak = 0.0f;
		float raw = 0.0f;
		const std::int64_t n = config_.block_frames;
		const std::int64_t cap = config_.max_ahead_frames;
		const float g0 = track.applied_gain;
		const float step = (target - g0) / static_cast<float>(n);
		const std::int64_t have = std::min(std::max<std::int64_t>(track.valid_end - out_pos_, 0), n);
		for (std::int64_t i = 0; i < have; ++i) {
			const float g = (g0 == target) ? target : g0 + step * static_cast<float>(i + 1);
			const std::size_t ring_index = static_cast<std::size_t>((out_pos_ + i) % cap) * kMixerChannels;
			for (std::size_t c = 0; c < kMixerChannels; ++c) {
				const float sample = track.ring[ring_index + c];
				const float value = g * sample;
				block->samples[static_cast<std::size_t>(i) * kMixerChannels + c] += value;
				peak = std::max(peak, value < 0.0f ? -value : value);
				raw = std::max(raw, sample < 0.0f ? -sample : sample);
			}
		}
		track.applied_gain = target;
		if (raw_peak != nullptr)
			*raw_peak = raw;
		return peak;
	}

	void mix_block(MixedBlock *block)
	{
		block->samples.assign(static_cast<std::size_t>(config_.block_frames) * kMixerChannels, 0.0f);
		const float decay = meter_decay_per_block(config_.block_frames, kMixerSampleRate);
		// tracks_ は MixerKey の順 (user_id の昇順) なので、audible_voices も昇順になる。
		for (auto &entry : tracks_) {
			float raw_peak = 0.0f;
			const float peak = mix_track(entry.second, entry.second.target(), block, &raw_peak);
			entry.second.meter_before.update(raw_peak, decay);
			entry.second.meter_after.update(peak, decay);
			const float judged = entry.second.activity_before_gain ? raw_peak : peak;
			if (entry.first.kind == MixerKind::Voice && judged > config_.audible_peak)
				block->audible_voices.push_back(entry.first.user_id);
		}
		for (Track &track : fading_)
			mix_track(track, 0.0f, block);
		fading_.clear();
	}

	AudioMixerConfig config_;
	std::optional<std::int64_t> origin_us_;
	std::int64_t out_pos_ = 0; // 次に出す区間の先頭の位置 (起点からのサンプル数)
	bool ahead_measurable_ = false; // 起点の後で take を一度呼んだか (先に渡してある長さを測れるか)
	std::map<MixerKey, Track> tracks_;
	std::vector<Track> fading_; // remove した後、次の区間で 0 へ移すもの
	std::vector<AudioMixerKeyStats> retired_;
	AudioMixerStats stats_;
	std::vector<float> converted_; // add で変換したサンプルの置き場 (呼び出しをまたいで使い回す)
};

} // namespace cordmux::pipeline

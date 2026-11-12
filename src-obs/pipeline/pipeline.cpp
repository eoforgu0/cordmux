// SPDX-License-Identifier: Apache-2.0

// pipeline.h の Pipeline の実装。受信のスレッド (ws::Listener の io_thread) から届いた映像は、ストリームごとの
// デコードのスレッドでデコードし、表示の緩衝の待ち行列を通して合成のソースの子に渡す。音声は受信のスレッドから
// 混ぜた音のコンポーネント (AudioMixer) に置き、取り出すスレッドが合成のソースから出す。ほかに、統計をログに書くスレッドと、
// 合成のソースの有無を調べる監視のスレッドを持つ。
// スレッドとロックの順は、下の Pipeline::Impl の mu の宣言にある。

#include "pipeline/pipeline.h"

#include "align/av_offset.h"
#include "align/timestamp_aligner.h"
#include "decode/h264_decoder.h"
#include "diag/diag_log.h"
#include "install/uninstall.h"
#include "pipeline/arrival_stats.h"
#include "pipeline/audio_mixer.h"
#include "pipeline/av_offset_plan.h"
#include "pipeline/bounded_call.h"
#include "pipeline/composite_audio_plan.h"
#include "pipeline/frame_delivery_plan.h"
#include "pipeline/frame_pacer.h"
#include "pipeline/hw_fallback_note.h"
#include "pipeline/latency_stats.h"
#include "pipeline/mixer_roster.h"
#include "pipeline/queue_admission.h"
#include "pipeline/receive_scope.h"
#include "pipeline/sender_clock.h"
#include "pipeline/slot_release.h"
#include "pipeline/stream_router.h"
#include "pipeline/voice_activity.h"
#include "platform/platform.h"
#include "record/record_location.h"
#include "record/record_search_dirs.h"
#include "record/record_writer.h"
#include "record/recorder.h"
#include "sources/composite_source.h"
#include "sources/decor_registry.h"
#include "sources/dummy_feed.h"
#include "sources/self_speaking_source.h"
#include "sources/screen_end.h"
#include "sources/screen_feed.h"
#include "ws/wire.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cordmux::pipeline {

namespace {

// キーフレームを待っている間に繰り返す要求を間引く間隔。目安の値で、測って決めたものではない。キーフレームを待ち
// 始めたときの最初の要求は間引かない (Impl::request_keyframe_now の宣言)。
constexpr std::int64_t kKeyframeRequestMinIntervalUs = 1'000'000; // 1 秒

// 段階ごとの計数をログに書く間隔。目安の値で、測って決めたものではない。
constexpr std::chrono::seconds kStatsLogInterval{10};

// 音声のメッセージの形 (docs/wire_format.md の「音声のペイロード」。32 ビット浮動小数点のインターリーブ、48 kHz、2 ch、
// 1 つのメッセージに 480 サンプル)。受信の関数 (handle_audio_frame) と、混ぜた区間を合成のソースに渡す所の両方で使う。
constexpr std::uint32_t kAudioSampleRate = 48000;
constexpr std::size_t kAudioChannels = 2;

// 混ぜた音のコンポーネント (AudioMixer) の形は、受け取る音声の形と同じでなければならない。
static_assert(kAudioSampleRate == kMixerSampleRate, "AudioMixer's sample rate must match the audio messages");
static_assert(kAudioChannels == kMixerChannels, "AudioMixer's channel count must match the audio messages");

// 混ぜた音を取り出して合成のソースに渡す間隔。AudioMixer の区間 (480 フレーム) と同じ 10 ms で、区間が溜まらない
// 間隔である。
constexpr std::chrono::milliseconds kMixerTakeInterval{10};

// 取り出すスレッドの起きる時刻がこれより遅れたら、遅れを取り戻さずに今から数え直す (長い停止の後に、溜まった分を
// 続けて取り出さない。溜まった区間は AudioMixer が飛ばす)。目安の値で、測って決めたものではない。
constexpr std::chrono::milliseconds kMixerLagResetLimit{50};

// 表示の緩衝 (FramePacer) の待ち行列の大きさを決める値。どれも目安の値で、測って決めたものではない。
//   - フレームの間隔は 60 fps (約 16.7 ms) とみなす。送信側が送るフレームレートの上限である。これより遅い
//     ストリームは、数の上限に余裕ができるだけである。
//   - 余裕の 6 フレームは、取り出すスレッドが 100 ms ほど遅れても、定常状態では捨てない大きさである。
//   - バイト数の上限は、解像度が大きいストリームで D が長いときに、メモリーを使い切らないための歯止めである。
//     1080p の NV12 は 1 フレーム約 3.1 MB で、D が上限の 1000 ms のときの上限は (1000 / 16.7 = 60) + 6 = 66 フレーム、
//     約 205 MB になる。4 ストリームが同時にこの大きさで約 820 MB である。256 MiB を超える解像度
//     (1440p60 の D = 1000 ms など) では上限が先にかかり、古いフレームを捨てる。捨てた数は統計の行の
//     paced_dropped に出る。
constexpr std::int64_t kPacerFrameIntervalUs = 16667;
constexpr std::size_t kPacerHeadroomFrames = 6;
constexpr std::size_t kPacerMaxBytes = 256ull * 1024 * 1024;

// 待ち行列に、現在の表示の緩衝 D と、それに合わせた上限を設定する。
template <typename Pacer> void configure_frame_pacer(Pacer &pacer, std::int64_t delay_us)
{
	pacer.configure(delay_us, frame_pacer_capacity(delay_us, kPacerFrameIntervalUs, kPacerHeadroomFrames),
			kPacerMaxBytes);
}

// 録画の保存先の候補が使えるかを調べるときの、空き容量の問い合わせのタイムアウト (ミリ秒)。目安の値で、測って
// 決めたものではない。ローカルのディスクは 1 ms 未満、動いている共有フォルダーでも数十 ms で返る一方、到達できない
// ネットワークの場所は、OS の側でタイムアウトするまで数十秒返らない。保存先はプラグインの読み込み中 (Pipeline の
// コンストラクター) にも決めるので、タイムアウトがないと OBS の起動がその間止まる。長くすると、到達できない場所で
// UI のスレッドと OBS の起動が長く止まる。短くすると、応答が遅いだけの場所を「分からない」と扱うことが増える。
constexpr int kRecordBaseDirProbeTimeoutMs = 3000;

// 保存先の候補への問い合わせの答え。
enum class RecordDirProbeAnswer {
	Found,    // 空き容量が読めた
	NotFound, // 絶対パスでない、または空き容量が読めないと答えた (ドライブごとない場所)
	Unknown,  // タイムアウトまでに答えなかった、または前の問い合わせがまだ返っていない
};

// タイムアウト付きの問い合わせと、返っていない問い合わせがあるかのフラグ。UI のスレッドだけが使う (BoundedCall の
// 約束と同じ)。
struct RecordDirProbe {
	BoundedCall call;
	// 問い合わせのスレッドが答えを出したら false にする。待つのをやめた後もスレッドが書くので、shared_ptr で持つ。
	std::shared_ptr<std::atomic<bool>> in_flight = std::make_shared<std::atomic<bool>>(false);
};

// 保存先の候補として使えるかを調べる。見るのは、絶対パスであることと、その場所の空き容量がタイムアウトまでに読める
// ことの 2 つ。
//   - 絶対パス: record/ の RecordPlatformOps は base_dir を絶対パスとして受け取る約束で、相対パスを渡すと OBS の
//     プロセスの作業フォルダーの下に書く。このチェックは問い合わせを起こさないので先に見る。
//   - 空き容量が読めること: platform::free_space_bytes は、まだないフォルダーなら親のフォルダーをたどるので、
//     読めないのは、取り外した外付けのドライブのように、ドライブごとない場所だけである。読めない場所を保存先にすると、
//     録画を始めた瞬間に 1 バイトも書けずに終わる。確認するためだけにファイルを作ることはしない。実際に書けるかは録画の
//     セッションを始めるときに分かり、そのとき画面に表示する。
//   - タイムアウトまでに返らなかった答えは Unknown にして、NotFound と区別する。ビデオフォルダーに切り替えるのは、
//     OBS の録画の保存先が空か見つからないときだけで、保存先が決まっているのに書けないときは切り替えない。スリープ
//     している外付けのドライブや、応答の遅いネットワークの場所は存在する場所なので、その場所のままにし、書けるか
//     どうかは録画のセッションを始めるとき (ensure_session_open) に任せる (書けなければ StartFailed にして、その場所を
//     表示する)。
//   - 前の問い合わせが返っていない間は、新しい問い合わせを起こさずに Unknown を返す。起こすと、返らない場所で録画を
//     開始するたびに UI のスレッドがタイムアウトの時間だけ止まり、待つのをやめたスレッドが 1 つずつたまる。
RecordDirProbeAnswer probe_record_base_dir(const std::string &candidate, RecordDirProbe *probe)
{
	if (candidate.empty() || !platform::is_absolute_path(candidate))
		return RecordDirProbeAnswer::NotFound;
	if (probe->in_flight->load())
		return RecordDirProbeAnswer::Unknown;
	probe->in_flight->store(true);
	std::shared_ptr<std::atomic<bool>> in_flight = probe->in_flight;
	const std::optional<bool> answer = probe->call.ask(
		[candidate, in_flight]() {
			// 例外を投げても in_flight を false にする (しないと、以後の問い合わせが全部 Unknown になる)。
			// 例外は「ない」と同じ答えにする (BoundedCall と同じ)。
			bool found = false;
			try {
				found = platform::free_space_bytes(candidate).has_value();
			} catch (...) {
				found = false;
			}
			in_flight->store(false);
			return found;
		},
		kRecordBaseDirProbeTimeoutMs);
	if (!answer.has_value()) {
		diag::logf(LOG_WARNING,
			   "The recording folder didn't respond within %d ms; it's kept and checked again when "
			   "cordmux recording starts",
			   kRecordBaseDirProbeTimeoutMs);
		return RecordDirProbeAnswer::Unknown;
	}
	return *answer ? RecordDirProbeAnswer::Found : RecordDirProbeAnswer::NotFound;
}

// cordmux の録画の保存先を、現在の設定で決める。決め方は record/record_location.h の choose_record_base_dir にあり、
// ここは値を集めるだけである。
//   - 利用者が選んだ場所 (settings の record_dir) は、使えなくてもほかの場所に切り替えない (録画のセッションを
//     始めるときに StartFailed になり、画面に表示する)。
//   - OBS の録画の保存先は obs_frontend_get_current_record_output_path で読む。OBS が録画に使う場所と同じ値になる (OBS
//     31.1.1 の OBSBasic::GetCurrentOutputPath は、出力モードが詳細で種類が FFmpeg なら FFFilePath、ほかは
//     RecFilePath、基本なら FilePath を返す。
//     https://github.com/obsproject/obs-studio/blob/31.1.1/frontend/widgets/OBSBasic_OutputHandler.cpp)。設定のキーを
//     自分で読み分けると、OBS が種類の値を増やしたときに食い違う。
//   - OBS の保存先が空か見つからないときだけ、Windows のビデオフォルダーにする。
// この関数は Pipeline のコンストラクター (obs_module_load の中) から呼んでよい。OBS 31.1.1 の OBSBasic::OBSInit は、
// プロファイルとシーンコレクションを読んでから obs_load_all_modules2 を呼び、モジュールは初期化の間にプロファイルの
// 情報を読めると書いている (https://github.com/obsproject/obs-studio/blob/31.1.1/frontend/widgets/OBSBasic.cpp)。
// obs_frontend_get_current_record_output_path は test/lint/forbidden_libobs_names.txt の一覧に入っていない。UI の
// スレッドから呼ぶ (OBS の設定を読むため。問い合わせにはタイムアウトがある)。タイムアウトまでに答えなかった OBS の
// 保存先は、その場所のまま返す (probe_record_base_dir)。unknown を渡すと、OBS の保存先が答えなかったかを書く
// (コンストラクターで、作成待ちを探す処理に渡すかを決めるため)。
record::RecordDirChoice resolve_record_dir_choice(const std::string &record_dir_setting, RecordDirProbe *probe,
						  bool *unknown = nullptr)
{
	std::string from_obs;
	if (record_dir_setting.empty()) {
		char *path = obs_frontend_get_current_record_output_path();
		if (path != nullptr) {
			from_obs = path;
			bfree(path);
		}
	}
	bool answered_unknown = false;
	const record::RecordDirChoice choice = record::choose_record_base_dir(
		record_dir_setting, from_obs, record_dir_setting.empty() ? platform::videos_dir() : std::string(),
		[](const std::string &path) { return platform::is_absolute_path(path); },
		[probe, &answered_unknown](const std::string &path) {
			const RecordDirProbeAnswer answer = probe_record_base_dir(path, probe);
			if (answer == RecordDirProbeAnswer::Unknown)
				answered_unknown = true;
			return answer != RecordDirProbeAnswer::NotFound;
		});
	if (unknown != nullptr)
		*unknown = answered_unknown && choice.source == record::RecordDirSource::ObsRecordPath;
	if (record_dir_setting.empty() && !from_obs.empty() && choice.source != record::RecordDirSource::ObsRecordPath)
		diag::logf(LOG_WARNING, "OBS recording path not found; cordmux recordings are saved in the Videos folder");
	return choice;
}

std::int64_t steady_now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

// OBS の時計 (os_gettime_ns) のマイクロ秒。OBS の描く刻み (obs_get_video_frame_time) と同じ時計で、表示の緩衝
// (FramePacer) の積む・取り出す・待つはすべてこの時計で行う。steady_now_us とは原点が同じとは限らないので、
// 2 つの値を引き算しない。
std::int64_t obs_now_us()
{
	return std::int64_t(os_gettime_ns() / 1000ULL);
}

// OBS の描く刻みの格子 (FramePacer::set_tick_grid)。OBS の映像が動いていなければ (間隔か刻みの時刻が 0) 間隔を 0
// にして、格子なしの今までの渡し方に戻す。
//
// 根拠は libobs の obs-video.c と obs.c (OBS 31.1.1)。描画のスレッドは繰り返しの先頭の tick_sources で video_time を
// 使ってソースを進め、末尾の video_sleep で video_time に間隔を足した時刻まで待つ (遅れたときは間隔の整数倍だけ
// 進める。格子は崩れない)。非同期のソースが表示するフレームは、この tick_sources の中の async_tick (obs-source.c)
// が、そのとき届いている最新の 1 枚から選ぶ。そのため、描く刻みは video_time + 間隔の整数倍で、渡したフレームは
// 渡した時刻以後の最初の刻みで選ばれる。video_time は obs_get_video_frame_time、間隔は obs_get_frame_interval_ns で読め、どちらも
// os_gettime_ns の時計の ns である。video_time は描画のスレッドだけが書く 64 ビットの整列した値で、別のスレッドから
// 読むと、1 つ前か現在の刻みのどちらかが読める (どちらも格子の上で、ずれは間隔の整数倍だけである)。x64 では整列した
// 64 ビットの読みは分割されない。
struct ObsTickGrid {
	std::int64_t phase_us = 0;
	std::int64_t interval_us = 0;
};

ObsTickGrid read_obs_tick_grid()
{
	const std::uint64_t interval_ns = obs_get_frame_interval_ns();
	const std::uint64_t time_ns = obs_get_video_frame_time();
	if (interval_ns == 0 || time_ns == 0)
		return {};
	// 間隔は四捨五入する (60 fps は 16666.67 us なので 16667)。1 刻みあたり最大 0.5 us の誤差が、読み直した刻みから
	// 離れた分 (D の間の刻みの数) だけ溜まるが、D の上限 1 秒でも 60 刻みで 30 us で、半刻み (約 8 ms) に比べて小さい。
	return {std::int64_t(time_ns / 1000ULL), std::int64_t((interval_ns + 500ULL) / 1000ULL)};
}

// システム時刻のエポックからのマイクロ秒。画面に時刻として表示する値 (「10:32:04」) と、ledger::Observation の時刻の
// 基準はこちらである。経過時間を測るのは steady_now_us で、この 2 つは混ぜない。
std::int64_t wall_now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

// OBS に渡す音声 1 つ (obs_source_audio) を作る (混ぜた区間を合成のソースに渡す形)。samples は呼び出しの間だけ
// 有効ならよい (obs_source_output_audio がコピーする)。
obs_source_audio make_audio_frame(const std::uint8_t *samples, std::uint32_t frame_count, std::int64_t out_ts_us)
{
	obs_source_audio audio{};
	audio.data[0] = samples;
	audio.frames = frame_count;
	audio.speakers = SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_FLOAT;
	audio.samples_per_sec = kAudioSampleRate;
	audio.timestamp = std::uint64_t(out_ts_us) * 1000; // us -> ns
	return audio;
}

// ---- 合成のソースの有無 ----

// 合成のソースの有無を調べ直す間隔。利用者が OBS の [+] で合成のソースを追加したことと、手で削除したことには
// イベントがないので、media の値と、メインアカウントの声の経路 (AssignPolicy::route_main_account_voice) を決め直す
// ための監視である。1 秒は目安の値で、測って決めたものではない。送信側のゲートが開くまでの遅延がこの長さになり、
// 合成のソースを追加してから 1 秒で開けば、導入の手順の途中で待たせない。1 回に調べるのは sources/ が覚えている値
// だけで、軽い。
constexpr std::chrono::milliseconds kLayoutPresenceInterval{1000};

// 合成のソースの有無を調べる (sources/composite_source.h の composite_present。ソースを作らない)。mu を持たずに呼ぶ
// (監視のスレッドと、OBS の UI のスレッドから呼ぶ)。
LayoutPresence read_layout_presence()
{
	LayoutPresence presence;
	presence.composite = sources::composite_present();
	return presence;
}

const char *on_off(bool on)
{
	return on ? "on" : "off";
}

// ---- 自分の枠を使っているか ----

// 自分の枠の人。メインアカウント、メインアカウントが空なら Vesktop のアカウント (ドックの self_frame_user_id と同じ決め方)。
// 自分の枠を使い始めてよいかを数えるとき (stream_router.h の accepted_self_frame_in_use) に、この人の枠を数えない。
std::string self_frame_owner_of(const std::string &main_account_user_id, const FactsSnapshot &facts)
{
	if (!main_account_user_id.empty())
		return main_account_user_id;
	return facts.self_user_id.value_or(std::string());
}

// 自分の枠を使っているかが変わったときのログ。画面の枠に載せられる人数も書く。
void log_self_frame_in_use(bool in_use)
{
	diag::logf(LOG_INFO, "Self frame %s: up to %d people in screen slots", in_use ? "in use" : "not in use",
		   config::screen_slot_capacity(in_use));
}

} // namespace

struct Pipeline::Impl {
	Impl(ws::Listener &listener_, config::AllocationTable &table_, const config::Settings &settings_)
		: listener(listener_), table(table_), settings(settings_)
	{
		// 画面の音と映像のずれの補正は、符号で足す先が変わる。正なら画面の音のタイムスタンプに足して遅らせ
		// (av_offset の constant_offset_us)、負なら絶対値 (audio_early_us) だけ画面の音を早く置く (映像は
		// 遅れない。add_mixed_audio_locked のコメント)。分け方は pipeline/av_offset_plan.h の純粋関数にある。
		const AvOffsetPlan plan = plan_screen_audio_offset(settings.screen_audio_offset_ms);
		av_cfg.constant_offset_us = plan.audio_constant_offset_us;
		audio_early_us = plan.audio_early_us;

		// 表示の緩衝は、ドックが作られない起動でも settings.json の値どおりに動くように、起動のときの値をここで
		// コピーする。動いている間の変更は set_display_buffer_ms が受ける。
		display_buffer_us.store(std::int64_t(settings.display_buffer_ms > 0 ? settings.display_buffer_ms : 0) * 1000);

		// 声を遅らせる時間 (混ぜた音で声を置く時刻に足す。composite_audio_plan.h の voice_audio_at_us)。
		voice_delay_us = std::int64_t(settings.voice_audio_delay_ms) * 1000;
		// 混ぜた音の組ごとに置く位置を時刻に追従させる決め方は、録画と同じ設定の値で決める (align/audio_drift.h)。
		AudioMixerConfig mixer_config;
		mixer_config.drift = align::make_audio_drift_config(
			settings.audio_resync_ms, settings.audio_drift_max_ppm, settings.audio_drift_time_constant_ms);
		// 声の組だけは、置き直しのしきい値を別に持つ (audio_mixer.h の AudioMixerConfig::voice_resync_us)。
		mixer_config.voice_resync_us = std::int64_t(settings.voice_audio_resync_ms) * 1000;
		mixer = AudioMixer(mixer_config);
		link_lost_hold_ns = std::uint64_t(settings.link_lost_hold_ms > 0 ? settings.link_lost_hold_ms : 0) * 1000000ULL;
		reconnect_layout_hold_us =
			std::int64_t(settings.reconnect_layout_hold_ms > 0 ? settings.reconnect_layout_hold_ms : 0) * 1000;

		// ログを入れるリングバッファーの行数は、settings の値。出来事の行 (既定 5000) と統計の行 (既定 200) を別々に
		// 持つ (行数の決め方は config/settings.h の kEventLogDefaultLines)。
		// 受信のやり直しを頼むまでの時間 (pipeline/receive_restart.h)。0 なら頼まない。
		receive_restart_wait_us = std::int64_t(settings.receive_restart_wait_ms > 0 ? settings.receive_restart_wait_ms : 0) * 1000;

		const auto lines_of = [](int n) { return std::size_t(n < 0 ? 0 : n); };
		js_log = LogRing(lines_of(settings.js_event_log_lines));
		js_stats_log = LogRing(lines_of(settings.js_log_ring_lines));
		plugin_log = LogRing(lines_of(settings.plugin_event_log_lines));
		plugin_stats_log = LogRing(lines_of(settings.plugin_log_ring_lines));

		// 誰を枠に入れるかの決め方。ドックもコピーを渡すが、ドックが作られない起動 (ドックを追加できなかった
		// 場合) でも settings.json の値どおりに動くように、起動のときの値はここでコピーする。「自動で載せる」を
		// 付けた人は seen_people.json にあり、読むのはドックなので、ここでは空のまま始める (Pipeline は
		// ファイルを読まない)。
		assign_policy.automatic = settings.assign_mode == config::AssignMode::Auto;
		// 受け取る範囲も同じ理由でここでコピーする (枠に載せた人だけの設定が、ドックを作らない起動でも動作するように)。
		receive_scope = settings.receive_scope;
		// メインアカウントの userId も同じ理由でここでコピーする。自分の声の経路 (合成のソースがあるときだけ作る) と、
		// ミキサーでの自分の声の既定のミュートが、ドックを作らない起動でも動作するようにする (stream_router.h の
		// AssignPolicy、mixer_roster.h の MixerLevels)。
		assign_policy.main_account_user_id = settings.broadcaster_user_id;
		// 自分の枠に映す映像を OBS から選んでいる間は、メインアカウントの画面を枠に載せない (同じ理由でここでコピーする)。
		assign_policy.self_frame_selected = config::self_frame_video_selected(settings);
		// 自分の枠を使っているか (画面の枠に新しく載せられる人数が 1 つ減る) も同じ理由でここでコピーする。シーンの読み込みが
		// 終わる前に Vesktop から facts が届くと、そこで画面共有している人を枠に載せる (admit_sharers) ので、そのときに
		// もう分かっている必要がある。
		assign_policy.self_frame_in_use =
			config::self_frame_in_use(assign_policy.self_frame_selected, settings.self_frame_on);
		mixer_levels.set_self_voice_user(settings.broadcaster_user_id);
		mixer_levels.set_self_voice_muted(settings.self_voice_muted);

		// cordmux の有効と無効、画質の上限も、起動のときの値はここでコピーする (上の assign_policy と同じ
		// 理由)。動いている間の変更は set_feature_enabled と set_limits が受ける。
		feature_enabled = settings.feature_enabled;
		limits = limits_from_settings(settings.limit_max_long_edge, settings.limit_fps_divisor,
					      settings.encoder_preference, settings.encode_priority);

		// 録画 (Recorder) はコンストラクターで作る。作成待ち (pending) の録画のセッションを数えるのは
		// Recorder のコンストラクターの 1 回だけで (record/recorder.h)、ドックはその数を最初のポーリングから
		// 読むためである。録画が OFF の間に RecordPlatformOps の関数を 1 つも呼ばないのは、Recorder の側の
		// 約束である。
		ffmpeg_child_log = std::make_shared<std::string>();
		// 保存先はタイムアウトを付けて問い合わせる (probe_record_base_dir。タイムアウトした問い合わせの
		// スレッドは base_dir_probe が保持し、stop() で待つ)。ここで決めるのは、表示と、作成待ちを探すためで
		// ある (録画のセッションの保存先は、録画を始めるときに決め直す。set_recording)。
		record_dir_setting = settings.record_dir;
		record_values.output_pcm = settings.record_output_pcm;
		// 録画ファイルの名前の語と書式。どの録画のセッションと、どの録画ファイルの作成の再開よりも前に、ここで埋める
		// (pipeline.h の record_name_words)。
		const RecordNameWords name_words = record_name_words([](const char *key) { return obs_module_text(key); });
		record_values.screen_kind_word = name_words.screen_kind_word;
		record_values.voice_kind_word = name_words.voice_kind_word;
		record_values.output_name_format = name_words.output_name_format;
		bool obs_dir_unknown = false;
		const record::RecordDirChoice choice =
			resolve_record_dir_choice(record_dir_setting, &base_dir_probe, &obs_dir_unknown);
		record_root_now = choice.base_dir;
		// タイムアウトまでに答えなかった OBS の保存先は表示にだけ使い、作成待ちを探す処理 (Recorder の
		// コンストラクター) には渡さない。その処理は保存先の下を同じスレッドでたどるので、到達できない場所では
		// OBS の起動がその間止まる。
		record_base_dir = obs_dir_unknown ? std::string() : choice.base_dir;
		const std::string &base_dir = record_base_dir;
		// 録画ファイルの作成で起動する子プロセス (ffmpeg.exe) の出力を書くファイル (失敗の種類を分けるのに使う)。
		// record/record_finalize.cpp が子プロセスを起動するたびに、録画の中間ファイルを入れるフォルダーにある
		// ログに切り替えるので、ここで決めるのは最初のファイルだけである。
		if (!base_dir.empty())
			*ffmpeg_child_log = record::join_path(record_root_now, "ffmpeg-last.log");
		recorder = std::make_unique<record::Recorder>(
			settings, record::make_real_record_platform_ops(ffmpeg_child_log), base_dir);
		recorder->set_session_values(record_values);
		// 録画ファイルの作成の済んでいない録画のセッションが残っている保存先を、ファイル (record::kPendingDirsFileName) に
		// 覚えておく (record/record_search_dirs.h)。その場所に届くかは、Recorder が場所ごとに別のスレッドで
		// 問い合わせ、OBS の起動は待たない。問い合わせの中身は保存先の問い合わせと同じ (絶対パスで、空き容量が
		// 読める)。ファイルに書く関数は、アンインストールの最中かをロックの下で見る
		// (install::write_unless_uninstalling)。アンインストールが設定のフォルダーを空にした後に、この
		// ファイルを作り直さないためである。
		char *pending_dirs_path = obs_module_config_path(record::kPendingDirsFileName);
		if (pending_dirs_path != nullptr) {
			// ポータブル版の OBS では相対パスになるので、ほかの設定のファイルと同じく絶対パスにする (platform::absolute_path)。
			const std::string store_path = platform::absolute_path(pending_dirs_path);
			bfree(pending_dirs_path);
			recorder->remember_pending_dirs(
				record::pending_dirs_file_store(store_path,
								[](const std::function<bool()> &write) {
									return install::write_unless_uninstalling(write);
								}),
				[](const std::string &dir) {
					if (dir.empty() || !platform::is_absolute_path(dir))
						return false;
					return platform::free_space_bytes(dir).has_value();
				});
		}
	}

	ws::Listener &listener;
	config::AllocationTable &table; // 状態の所有者はこの表で、ここでは参照を借りるだけ
	config::Settings settings;

	// 画面の音と映像のずれの補正。正の分は av_cfg の constant_offset_us に、負の分の絶対値は audio_early_us に
	// 入る (分け方は av_offset_plan.h)。av_cfg は ScreenStream を作るたびに渡すので、作り直したストリーム
	// (needs_screen_stream_recreate) にも同じ補正が動作する。
	align::AvOffsetConfig av_cfg;
	std::int64_t audio_early_us = 0;

	// 声を、届いてからこの時間だけ遅らせて置く (混ぜた音の時刻に足す)。
	std::int64_t voice_delay_us = 0;

	// 画面共有の映像の表示の緩衝 D (マイクロ秒。settings の display_buffer_ms)。D が 0 でも、フレームごとの待ち行列
	// (pacers) に積み、送信側のタイムスタンプ + D の後の OBS の描く刻みに表示されるよう、その半刻み前に合成のソースの
	// 子へ渡す (渡し方は pipeline/frame_delivery_plan.h)。書き換えるのは pacer_mu を持った間だけで、読むのは持たなくて
	// よい。
	std::atomic<std::int64_t> display_buffer_us{0};
	// snapshot() が参加者ごとの遅れの列を作るか (set_arrival_watch)。
	std::atomic<bool> arrival_watch{false};

	// Vesktop の送信側のログ (on_log) の 1 行を、リングバッファーに入れる前に通すチェックの関数。既定は全部を捨てる
	// (pipeline.h の set_js_log_sink)。plugin-main.cpp が set_js_log_sink() で差し替える。
	Pipeline::JsLogGuard js_log_sink = [](std::string_view, Pipeline::JsLineKind) { return false; };

	// Pipeline::stop() の最初に true にする。stop() が戻った後も、listener の io_thread が configure を
	// 届けることがある (Pipeline::stop() のコメント) ので、その間に ensure_screen_stream が新しいデコードの
	// スレッドを起動し、join されないまま残る (リソースのリーク) のを防ぐ。
	std::atomic<bool> stopping{false};

	// Impl の状態の大部分を守るロック。router、table (借りた参照)、screen_streams (ストリームごとのデコードの
	// スレッド一式)、counters などを、この 1 つでまとめて守る。ロックを細かく分けるのは測ってから考える。
	// デコードそのもの (decode::H264Decoder::push、重い処理) だけは mu の外で行う。
	//
	// ロックの順。上から下へ取り、逆の向きには取らない。2 つを同時に持つのは、mu を持ったまま下のどれかを
	// 取るときだけである。
	//   mu
	//     pacer_mu: 画面の映像の表示の緩衝の待ち行列。mu を持ったまま取ってよい
//     mixer_mu: 混ぜた音の AudioMixer と、その組の名簿と、参加者ごとの利得の表。mu を持ったまま取ってよい。
//       取り出すスレッド (mixer_loop) は mixer_mu だけを持ち、その中で mu を取らない。AudioMixer から取り出した
//       区間は mixer_mu を放してから合成のソースに渡す (sources::output_composite_audio)
	//     record::Recorder の record_mu: 録画のキュー。mu を持ったまま取ってよい (Recorder の側は mu を取らない)
	//     plugin-main.cpp のログの出力先 (guard_and_log) の mutex: mu を持ったまま取ってよい
	//     plugin_log_mu: cordmux.dll 自身のログのリングバッファー。mu を持ったまま取ってよい。ログの出力先が、
	//       自分の mutex を放してから push_plugin_log の中で取る
	//     sources/ の、画面の枠の表示の遅れの測定のロック (take_screen_shown_latency。ほかのロックを取らない)。mu を
	//       持ったまま取ってよい
	//   stats_control_mu、presence_control_mu、layout_sink_mu、record_place_mu、speaking_mu は、ほかのロックと同時には
	//   持たない。
	// diag/diag_log.h の関数は mu の中から呼んでよい。
	// snapshot() は、plugin_log_mu と record_mu と Listener の中のロックを、mu を取る前に 1 つずつ取って放す。
	// ws::Listener の request_keyframe、send_recording、send_media、send_limits は asio::post するだけなので、どの
	// ロックを持ったまま呼んでもよい。
	std::mutex mu;
	StreamRouter router;
	config::StreamStageCounters counters;

	// 直近の facts。mu の中でだけ読み書きし、snapshot() が mu を取ってコピーする (下の js_log と last_frames も
	// 同じ)。
	FactsSnapshot facts;
	// 録画の保存先 (cordmux/<録画のセッション> の親。空なら書ける場所がない)。コンストラクターのときの値で、
	// Recorder のコンストラクターに渡した場所 (作成待ちを探す場所) である。録画のセッションの保存先は、録画を
	// 始めるときに決め直す。
	std::string record_base_dir;
	// ---- 録画の保存先と、録画のセッションごとの値 (record_place_mu で守る) ----
	// ほかのロックを取らないロック。platform/os_probe.h のスレッドも record_base_dir() で読む。
	mutable std::mutex record_place_mu;
	// 現在の決め方で決めた、録画のセッションのフォルダーを並べる場所 (<保存先>/cordmux)。
	std::string record_root_now;
	// settings の record_dir のコピー (update_record_settings で替わる。UI のスレッドだけが触る)。
	std::string record_dir_setting;
	// 次に始める録画のセッションの値 (保存先は始めるときに埋める)。
	record::RecordSessionValues record_values;
	// 保存先の候補への空き容量の問い合わせ (タイムアウト付き。probe_record_base_dir)。タイムアウトした問い合わせの
	// スレッドを保持するので、stop() まで残しておく。
	RecordDirProbe base_dir_probe;
	// cordmux.dll のバージョン。値を決めるのは plugin-main.cpp (buildspec.json の値) で、ここは
	// 録画ログに書くために受け取るだけである。
	std::string plugin_version;
	LogRing js_log;       // 送信側の出来事の行 (診断情報の discord.log)
	LogRing js_stats_log; // 送信側の統計の行 (discord-stats.log)
	std::map<std::uint32_t, StreamFrameClock> last_frames;

	// profiles (参加者ごとに届いたアバターを含む) と、ストリームごとの映像の形。どちらも mu の中でだけ読み書きし、
	// snapshot() がコピーする。接続が切れたら捨てる (handle_disconnected。前の通話の名前と解像度を画面に表示し
	// 続けると、事実と違う表示になる。facts と同じ扱い)。
	ProfilesSnapshot profiles;
	std::map<std::uint32_t, StreamFormat> stream_formats;

	// 受信のやり直し (pipeline/receive_restart.h)。どれも mu の中でだけ読み書きする。
	//   linked: Vesktop と接続している (handle_connected から handle_disconnected まで)。
	//   receive_restart_state: 頼みの状態 (presence_loop が 1 秒ごとに plan_receive_restart で進める)。
	//   stream_check_outcomes: ストリームごとの、送信側の最後の答え (同じ答えが続く間は、ログに 1 回だけ書く)。
	//   receive_restarts: 参加者ごとの、受信をやり直した回数 (PipelineSnapshot::receive_restarts)。
	std::int64_t receive_restart_wait_us = 0;
	bool linked = false;
	ReceiveRestartState receive_restart_state;
	// デコードの経路が黙って止まったときの作り直しの状態 (plan_decode_restart)。mu の中でだけ読み書きする。
	DecodeRestartState decode_restart_state;
	std::map<std::uint32_t, std::string> stream_check_outcomes;
	std::map<std::string, std::uint64_t> receive_restarts;

	// Vesktop との接続が切れたときに映っていた枠 (pipeline.h の LinkLostLayoutHold)。mu の中でだけ読み書きする。
	// handle_disconnected が書き、handle_connected が接続し直した時刻を入れ、snapshot() が配置を保つ枠を読む。
	LinkLostLayoutHold link_lost_layout;
	// 接続し直してから、映像が戻らない枠の配置を保つ長さ (settings の reconnect_layout_hold_ms。動いている間に変えられる)。
	std::atomic<std::int64_t> reconnect_layout_hold_us{10000000};

	// ---- 配信の映像に直接渡す値 (ドックのポーリングを通さない) ----
	//
	// 接続が切れてから画面の枠を空にするまで、最後のフレームを残す長さ (settings の link_lost_hold_ms。動いている
	// 間に変えられる)。
	std::atomic<std::uint64_t> link_lost_hold_ns{sources::kDefaultScreenHoldNs};
	// 画面共有の終わりで枠を空にした後に呼ぶ、並べ直しのコールバック (set_screen_layout_changed_sink)。ほかの
	// ロックを取らないロックで守り、呼ぶときもロックを持ったまま呼ぶ (外した後に呼ばない。notify_layout_changed)。
	std::mutex layout_sink_mu;
	std::function<void()> layout_changed_sink;

	// 並べ直しのコールバックを呼ぶ (mu を持たずに呼ぶ)。
	void notify_layout_changed();

	// ダミーを表示していた時間帯。mu の中でだけ触る。接続が切れても捨てない (ログのリングバッファーと同じく、
	// 診断情報に残すためのものである)。
	std::vector<DummyInterval> dummy_intervals;

	// ---- 録画 ----
	//
	// 録画の ON と OFF の値は、このフラグだけが持つ (settings には持たない)。Recorder の中にも同じ値があるが、
	// そちらはコピーで、決めるのはここである (ドックもこの値を PipelineSnapshot::recording.enabled で見る)。
	// atomic なのは、mu の外で読み書きする所があるため。stop() が false にするのと、snapshot() がコピーするのは
	// mu の外である (set_recording と poll_recording_self_stop は、値が変わったかの判断のために mu の中で
	// 読み書きする)。受信の関数は、このフラグが false の間、メッセージを 1 バイトも録画にコピーしない。
	// Recorder::on_video はコピーしてから中の判定を見るので、フラグを見ずに渡すと、録画していない間もコピーの
	// 負荷がかかる。
	std::atomic<bool> recording_on{false};
	std::shared_ptr<std::string> ffmpeg_child_log;
	std::unique_ptr<record::Recorder> recorder;

	// ドックが決めた、配信に出すその人の名前のコピー (pipeline.h の set_display_names)。mu の中でだけ触る。接続が
	// 切れても捨てない。切れた後に録画ファイルを作成すると名前を引けなくなるためで、名前を録画ログに書いておくのも同じ理由である
	// (profiles は切れたら捨てる)。
	std::map<std::string, std::string> display_names;

	// cordmux.dll 自身のログのリングバッファー。mu ではなく専用のロックで守る。ログの出力先 (plugin-main.cpp の
	// LogSink) は mu を持ったまま呼ばれることがあり、同じロックにすると自分自身を待つ (デッドロックになる)。mu の
	// 後に plugin_log_mu を取る向きだけを許す (上の mu の宣言)。
	mutable std::mutex plugin_log_mu;
	LogRing plugin_log;       // 出来事の行 (診断情報の plugin.log)
	LogRing plugin_stats_log; // 統計の行 (plugin-stats.log)

	// OBS のシーンコレクションの読み込みが終わったか (pipeline.h の on_frontend_finished_loading)。true になる前に
	// 届いた configure は、pending_configure に最新の 1 通だけ覚えて、true になったときに適用する。どちらも mu の
	// 中でだけ読み書きする (フラグを読むのと pending_configure に書くのが同じ mu の中で起きるので、true になる
	// 直前に届いた configure が、どちらの処理からも適用されないことは起きない)。
	bool loading_finished = false;
	std::optional<ws::ConfigureMessage> pending_configure;

	// 誰を枠に入れるかの決め方 (pipeline.h の set_assign_policy) と、最後に適用した configure。適用し直す処理
	// (set_assign_policy、assign_slot、move_self_frame など) がこの 1 通をもう一度適用する。configure はいま生きている
	// ストリームをすべて含むので、最新の 1 通だけ持てば足りる。接続が切れたら捨てる。
	AssignPolicy assign_policy;
	std::optional<ws::ConfigureMessage> last_configure;
	// last_configure が替わるたびに 1 つ増える番号 (古い configure を適用し直さないため。handle_configure の宣言)。
	std::uint64_t configure_generation = 0;
	// 最後に configure を適用したときに、合成のソースがあったか。メインアカウントの声の経路は合成のソースの有無で
	// 決まる (AssignPolicy::route_main_account_voice) ので、有無が変わったら適用し直す (refresh_layout_presence)。
	bool configure_composite = false;

	// ---- 送信側に送る値と、合成のソースの有無。どれも mu の中でだけ読み書きする ----
	//
	// cordmux の有効と無効 (元の値は settings.json の feature_enabled。コンストラクターでコピーし、その後は
	// set_feature_enabled が書く)。
	bool feature_enabled = true;
	// 送信側に送る画質の上限 (limits_from_settings の結果。その後は set_limits が書く)。
	ws::LimitsMessage limits;
	// 最後に Listener に渡した値 (Listener も覚えて、次の hello_ok に載せる)。値を持たないのは、まだ一度も
	// 渡していないとき (start() より前)。値が変わったときだけ送るために比べる。
	std::optional<bool> media_sent;
	std::optional<ws::LimitsMessage> limits_sent;
	// 視聴の範囲に入れた時刻 (PipelineSnapshot::watching_since_us)。
	std::map<std::string, std::int64_t> watching_since_us;
	// 受け取る範囲 (元の値は settings.json の receive_scope。set_receive_scope が書く)。Slots のときは、枠に載っている
	// 人だけを送信側に視聴させ、録画の入力にも枠に載っている人の画面だけを通す (pipeline/receive_scope.h)。
	config::ReceiveScope receive_scope = config::ReceiveScope::Slots;
	// 最後に送信側へ送った、視聴する人の範囲と、メインアカウントの userId。値を持たないのは、まだ一度も送っていない
	// とき。値が変わったときだけ送るために比べる (接続した直後は、変わっていなくても送り直す)。
	std::optional<ws::ReceiveScopeMessage> watch_sent;
	std::optional<std::string> main_account_sent;
	// 現在のシーンコレクションの合成のソースの有無 (refresh_layout_presence が調べる)。presence は、最後に調べた値を
	// 切り替えの最中も持ち続ける (media の値を決めるのに使う。切り替えのたびに、送信側にゲートを閉じてまた開かせる
	// ことをしない)。presence_current は、切り替えのイベントから調べ直すまでの間だけ false にする (持っている値が
	// 切り替える前のシーンコレクションのものだと分かるようにする。調べ直した最初の値は、変わったかにかかわらずログに
	// 書く)。
	std::optional<LayoutPresence> presence;
	bool presence_current = false;
	// 合成のソースの有無を調べた値の世代。切り替えのイベント (CHANGING と CHANGED) のたびに増やす。mu の外で
	// 調べている間に切り替わったときの古い値を、新しいシーンコレクションの値として使わないため。
	std::uint64_t presence_epoch = 0;

	// OBS_FRONTEND_EVENT_EXIT を受けたか (pipeline.h の on_frontend_exit)。true になったら調べ直さない。mu の外
	// (監視の入口) でも読むので atomic にした。
	std::atomic<bool> frontend_exiting{false};

	// 監視のスレッド (kLayoutPresenceInterval ごと)。stats_thread と同じ形で、専用の mutex と condition_variable で
	// 待ち、stop() で起こして join する。
	std::thread presence_thread;
	std::mutex presence_control_mu;
	std::condition_variable presence_control_cv;
	bool presence_stop_requested = false;

	// safe_call が捕まえた例外の総数 (ログと診断情報に出すだけで、判定には使わない)。
	std::atomic<std::uint64_t> exceptions_caught{0};

	// 段階ごとの計数を kStatsLogInterval ごとにログに書くスレッドの制御。mu とは別の mutex にする (stats_loop は
	// counters を読む短い間だけ mu を取り、それ以外はこの専用の mutex と condition_variable で待つ。stop() は間隔の
	// 分だけ待たずに、すぐにこのスレッドを起こして join できる)。
	std::thread stats_thread;
	std::mutex stats_control_mu;
	std::condition_variable stats_control_cv;
	bool stats_stop_requested = false;

	struct QueuedAu {
		std::int64_t timestamp_us = 0;
		std::uint8_t flags = 0;
		std::vector<std::uint8_t> data;
	};

	// 画面の枠 (kind=Screen) のストリームだけが持つ、ストリームごとのデコードのスレッド一式。screen_streams への
	// 出し入れは mu の中でだけ行う。フィールドの読み書きも mu の中で行う (デコードそのもの (decoder.push) だけは
	// mu の外)。
	struct ScreenStream {
		// acfg は、画面の音と映像のずれの補正のうち、画面の音のタイムスタンプに足す定数を持つ。
		ScreenStream(std::uint32_t id, const decode::DecoderConfig &dcfg, const align::AvOffsetConfig &acfg)
			: stream_id(id), decoder(dcfg), av_offset(acfg)
		{
		}

		std::uint32_t stream_id;
		decode::H264Decoder decoder; // コピーもムーブもできない (h264_decoder.h)。コンストラクターで直接作る。
		align::TimestampAligner aligner;
		align::AvOffsetTranscriber av_offset;

		std::thread worker;
		std::condition_variable queue_cv;
		std::deque<QueuedAu> queue;
		std::size_t queued_bytes = 0; // queue_admission.h のバイト数の上限の判定に使う
		bool stop_requested = false;
		// 上限を超えて捨てた後、次のキーフレームが来るまで、新しく届いたアクセスユニットも捨て続ける。
		bool waiting_for_keyframe = false;
		// この ScreenStream で、デコードできたフレームの最初の 1 枚を、まだ処理していないか。最初の 1 枚で名前ラベルに
		// 描く人を結び付ける (フレームのたびに呼ばないためのフラグ)。ScreenStream は、枠に載っている人が替わったかも
		// しれないときに作り直す (needs_screen_stream_recreate) ので、最初の 1 枚で足りる。作り直さずに経路の枠だけを
		// 付け替えるとき (枠から外した人を載せ直す。Pipeline::assign_slot) は、rearm_decor_owner_locked で立て直す。
		bool decor_owner_pending = true;

		// デコードの経路が黙って止まったかを見るための時刻 (steady の us。pipeline/receive_restart.h の
		// plan_decode_restart)。first_undecoded_arrival_us は、最後のデコードの出力より後に届いた最初のフレームの到着
		// 時刻 (0 は、出力の後にまだ届いていない。出力が出たら 0 に戻す)、last_decoded_us はデコード済みのフレームが
		// 最後に出た時刻 (0 は 1 枚も出ていない)。どちらも mu の下で書き、読む。新しい ScreenStream は 0 から始まるので、
		// 経路を作り直した後は、作った後の最初の到着から数える。
		std::int64_t first_undecoded_arrival_us = 0;
		std::int64_t last_decoded_us = 0;

		// 受信のスレッド (上限を超えたとき) とデコードのスレッド (デコーダーがキーフレームを求めたとき) の
		// 両方から書くので atomic にした (mu を取らずに読み書きするため)。
		std::atomic<std::int64_t> last_keyframe_request_us{0};

		// 受信側の OBS の時計 - 送信側のタイムスタンプ (OBS の時計に換算した値) の統計 (latency_stats.h)。
		// mu を持って足し、mu を持って取り出す。
		VideoLatency latency;
		// 最後に OBS に渡した枠の番号 (mu の下で書き、読む)。OBS が表示に選んだフレームの遅れは枠ごとに
		// sources/ が持つので、統計の行を書くときにどの枠から取り出すかを、これで引く。0 は渡していない。
		int latency_slot = 0;

		// デコーダーがハードウェアデコードを使えずソフトウェアデコードに切り替えたか。デコードのスレッドが
		// 初めて見たときに 1 度だけログに書く (note はそのスレッドだけが触る)。診断情報の数え上げが mu の下で
		// 読むので、flag は atomic にした。
		HwFallbackNote hw_fallback_note;
		std::atomic<bool> hw_fallback{false};
	};

	std::map<std::uint32_t, std::unique_ptr<ScreenStream>> screen_streams;

	// ---- 表示の緩衝 (画面共有の映像を、送信側のタイムスタンプ + D の時刻に渡す) ----
	//
	// 目的は、届き方の揺れを意図した大きさ D で吸うことである。OBS の非同期ソースの緩衝は、最初のフレームと実際の
	// 時間の進みから OBS が決めた深さになり (libobs の ready_async_frame)、実際の通話では約 80 ms (66 から 111 ms で
	// 動いた) だった。cordmux が渡す時刻を決め、画面のソースは OBS の緩衝なし (async_unbuffered) で、渡された最新の
	// フレームをそのまま表示させる。YUV の変換とアップロードは今までどおり OBS が持つ。
	//
	// 時計。フレームのタイムスタンプ (整列した値) は送信側の単調な時計の上にあり、接続ごとに決めた差 (sender_offset_us。
	// pipeline/sender_clock.h) を足すと OBS の時計 (obs_now_us。os_gettime_ns) の時刻になる。積む・取り出す・待つは
	// すべて OBS の時計で行う。OBS の描く刻みと同じ時計にそろえるためである。システム時刻はこの経路に入れない
	// (NTP の合わせ直しで飛んでも、渡す時刻は動かない)。差が決まらない間の扱いと、送信側の時刻が今より先にある
	// ときの扱いは frame_pacer.h にある。steady_clock と os_gettime_ns の原点が同じとは仮定しない。
	//
	// 刻み。フレームごとの渡す時刻は、送信側のタイムスタンプ + D の後の最初の OBS の描く刻みの半刻み前にする
	// (frame_pacer.h の「刻みの格子」。read_obs_tick_grid が根拠を書いている)。送信側の間隔と OBS の刻みの間隔が同じ
	// 60 fps のとき、タイムスタンプ + D の時刻のまま渡すと、位相が近い枠で 1 つの刻みに 2 フレームと次の刻みに 0 フレームが
	// 繰り返され、フレームが飛んで同じフレームが 2 回出る。格子は取り出すスレッドが回るたびに読み直す (OBS の映像の設定が
	// 替わっても、積んであるフレームに適用される)。
	//
	// 画面の音は、下の「混ぜた音」のとおり、AudioMixer が映像と同じ D を足した時刻に置く。
	//
	// 待ち行列に積むのはデコードのスレッド (push_decoded_frame)、取り出して OBS に渡すのは専用のスレッド
	// (frame_pacer_loop) で、全ストリームで 1 本を共有する。スレッドを持つ理由と共有にした理由は frame_pacer_loop の
	// 宣言にある。
	//
	// ロックの順。mu の後に pacer_mu を取る向きだけを許す (stats_loop が mu の中で pacer_mu を取る)。frame_pacer_loop
	// は pacer_mu の下で取り出し、それを放してから mu を取って OBS に渡す (両方を同時には持たない)。
	struct PacedFrame {
		std::uint32_t stream_id = 0;
		Route route; // 積んだときの経路。取り出すときに変わっていないかを見る
		std::int64_t out_us = 0; // OBS に渡すタイムスタンプ (整列した値)
		decode::DecodedFrame frame;
	};
	struct PacerSlot {
		FramePacer<PacedFrame> pacer;
		// frame_pacer_loop が取り出して、OBS に渡し終えていないフレームの数。0 でない間は、待ち行列を捨てても
		// この行を pacers から取り除かない (discard_pacer と discard_all_pacers)。
		int in_flight = 0;
		// 積むときの届くまでの遅れ (age) の分布 (arrival_stats.h)。待ち行列と同じ寿命で、pacer_mu の下で足し、
		// 取り出す。
		ArrivalDelayStats arrival;
	};
	std::thread pacer_thread;
	std::mutex pacer_mu;
	std::condition_variable pacer_cv;
	bool pacer_stop_requested = false;
	std::map<std::uint32_t, PacerSlot> pacers;

	// ---- 混ぜた音 (AudioMixer が全員の音を混ぜて、合成のソースから 1 本で出す) ----
	//
	// 受信の関数 (handle_audio_frame) が画面の音と声を AudioMixer に置く。取り出すスレッド (mixer_loop。
	// frame_pacer_loop とは別) が 10 ms ごとに混ぜた区間を取り出し、合成のソースの obs_source_output_audio に渡す。
	// 時刻はすべて OBS の時計 (obs_now_us) の上にあり、画面の音は映像と同じ D を足した時刻に、声は届いた時刻 + J +
	// 声の遅延に置く (composite_audio_plan.h)。合成のソースがない間とダミー表示の間は、音を置かずに捨てる
	// (add_mixed_audio_locked)。
	//
	// ロックの順。mu の後に mixer_mu を取る向きだけを許す。取り出すスレッドは mixer_mu だけを持ち、その中で
	// mu を取らない。混ぜた区間を obs_source_output_audio に渡すときは、mixer_mu を放してから渡す。libobs の
	// obs_source_output_audio は、ソースの filter_mutex の下で音のフィルタ (利用者が付けたものを含む) を動かし、
	// その中で audio_mutex と audio_buf_mutex を取る (libobs の obs-source.c)。cordmux のロックには戻らないが、
	// フィルタが遅いと長く待つので、受信の関数が mu を持ったまま mixer_mu を待つことがないように、持ったまま
	// 呼ばない。渡す相手の合成のソースは、sources::output_composite_audio が参照を取って渡すので、消されている最中の
	// ソースには渡らない。
	//
	// mixer_roster は AudioMixer に今ある組を、ストリームごとに覚える。組の出入りで AudioMixer の remove と、
	// 新しく入った組への利得の当て直しをするのは、いつも mixer_mu の下である。
	std::thread mixer_thread;
	std::mutex mixer_mu;
	std::condition_variable mixer_cv;
	bool mixer_stop_requested = false;
	AudioMixer mixer;
	MixerRoster mixer_roster;
	MixerLevels mixer_levels;
	// 混ぜた区間ごとの「聞こえる声」から、参加者ごとの話し始めと話し終わりを作る。触るのは取り出すスレッド (mixer_loop) だけ
	// で、スレッドを起動する前と join した後には、そのスレッドの外から reset する。ロックは要らない。
	VoiceActivity voice_activity;
	// voice_activity の話している人 (talking_users) のコピー。mixer_loop が書き、audio_levels() が読む。ほかのロックを
	// 取らないロックで守る。
	mutable std::mutex speaking_mu;
	std::vector<std::string> speaking_users;
	// speaking_users を voice_activity が持つ話している人の一覧にそろえる。mixer_mu を放してから、取り出すスレッドの中で呼ぶ。
	void publish_speaking_users();
	// 音のチャンクを float に詰め直す保存先 (受信のスレッドが mu の下でだけ使う)。payload は float の境界に
	// 並んでいるとは限らないので、memcpy で移す。
	std::vector<float> audio_scratch;

	// 音声のストリームごとの、受信側の OBS の時計 - 送信側のタイムスタンプ (OBS の時計に換算した値) の統計。mu を持って
	// 足し、mu を持って取り出す。統計の周期で、その周期に 1 つも足されなかったストリームは消す (経路のないストリームの
	// 表を増やさない)。
	std::map<std::uint32_t, AudioLatency> audio_latency;

	// ---- 送信側の時計と OBS の時計の差 (pipeline/sender_clock.h) ----
	//
	// 送信側のタイムスタンプ (timestamp_us) は送信側の単調な時計の上にあり、OBS の時計 (obs_now_us) との差は接続の間は
	// 変わらない。差は hello と pong が運ぶ送信側の時計の読みから決め (handle_sender_clock)、映像を渡す時刻、画面の音を
	// 置く時刻、遅れの統計のすべてが同じ差で換算する。sender_clock は io_thread だけが触る (handle_connected と
	// handle_sender_clock)。デコードのスレッドも換算するので、決まった差を sender_offset_us に写す
	// (kSenderOffsetUnknown は差がまだ決まっていないことを示す)。
	static constexpr std::int64_t kSenderOffsetUnknown = INT64_MIN;
	SenderClockOffset sender_clock;
	std::atomic<std::int64_t> sender_offset_us{kSenderOffsetUnknown};
	void handle_sender_clock(std::int64_t sender_us, std::int64_t received_us);
	// 送信側のタイムスタンプを OBS の時計に換算する。差が決まっていなければ obs_now (今届いたものとして扱う)。
	std::int64_t sender_to_obs_now(std::int64_t sender_us, std::int64_t obs_now) const
	{
		const std::int64_t offset = sender_offset_us.load(std::memory_order_relaxed);
		return sender_to_obs_us(sender_us,
					offset == kSenderOffsetUnknown ? std::nullopt : std::optional<std::int64_t>(offset),
					obs_now);
	}

	// ---- ws::Listener の on_* から呼ばれる (すべて asio の io_thread から。ws/listener.cpp で確認した) ----
	void handle_video_frame(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len);
	void handle_audio_frame(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len);
	// expect_generation を渡すのは、適用し直す呼び出し (set_assign_policy、assign_slot、move_self_frame など) だけである。渡した
	// 世代が現在の configure_generation と違えば、その間に新しい configure が適用されたということなので、古い
	// 1 通は適用せずに戻る (適用すると、送信側がもう閉じたストリームの経路とデコードのスレッドを作り直してしまう)。
	// 渡さない呼び出し (ws/ から届いた 1 通と、保留していた 1 通) が新しい configure で、last_configure と世代を
	// 進めるのはそちらだけである。
	void handle_configure(const ws::ConfigureMessage &msg,
			      std::optional<std::uint64_t> expect_generation = std::nullopt);
	void handle_log(const ws::LogMessage &msg);
	void handle_facts(const ws::FactsMessage &msg);
	void handle_profiles(const ws::ProfilesMessage &msg);
	void handle_avatar(const ws::AvatarMessage &msg);
	void handle_connected();
	void handle_disconnected();

	// コールバックを例外から守る。ws::Listener の on_* は asio の io_context::run() の中 (io_thread の
	// 関数そのもの) で呼ばれ、ws/listener.cpp には try/catch がないので、on_* から漏れた例外は
	// std::terminate になり、OBS ごとクラッシュする。ここには例外を投げうる処理がある (au.data.assign の
	// std::bad_alloc、std::thread の作成の std::system_error など)。例外を捕まえてログに書き、その 1 件だけを捨てる
	// (クラッシュするより、1 件を失うほうがよい)。Pipeline::start() が listener に登録するコールバックのほか、
	// 監視のスレッドや、configure を適用し直す処理などでも使う。
	template <typename Fn> void safe_call(const char *where, Fn &&fn)
	{
		try {
			std::forward<Fn>(fn)();
		} catch (const std::exception &e) {
			exceptions_caught++;
			diag::logf(LOG_WARNING, "Error while handling %s (skipped): %s", where, e.what());
		} catch (...) {
			exceptions_caught++;
			diag::logf(LOG_WARNING, "Unknown error while handling %s (skipped)", where);
		}
	}

	// ---- 補助の関数 (呼び出す前に持っているべきロックを、それぞれのコメントに書く) ----

	// mu を持っていること。stream_id から ScreenStream を探す (なければ nullptr)。
	ScreenStream *find_screen_stream_locked(std::uint32_t stream_id);

	// mu を持っていること。user_id の画面の経路を持つ ScreenStream の decor_owner_pending を立て直す (次のデコードできた
	// フレームで、名前ラベルに描く人を結び付け直す)。
	void rearm_decor_owner_locked(const std::string &user_id);

	// mu を持っていないこと (中で mu を取る)。新しい画面のストリームのデコードのスレッドを起動する (すでにあれば
	// 何もしない)。
	void ensure_screen_stream(std::uint32_t stream_id);

	// mu を持っていないこと。画面のストリームのデコードのスレッドを止めて join し、削除する。
	void remove_screen_stream(std::uint32_t stream_id);

	// デコードのスレッドの本体。
	void screen_worker_main(ScreenStream *s);

	// mu を持っていないこと (中で mu を取る)。デコードできたフレーム 1 枚を待ち行列に積み、frame_pacer_loop が合成の
	// ソースの子に渡す。渡す先がなければ積まずに捨てて数える。frame の中身は積むときに移す。
	void push_decoded_frame(ScreenStream &s, decode::DecodedFrame &frame);

	// mu を持っていないこと (中で mu を取る)。フレーム 1 枚を合成のソースの子に、OBS の緩衝なしで渡して数える。
	void hand_frame_to_obs(std::uint32_t stream_id, int slot, const decode::DecodedFrame &frame, std::int64_t out_us);

	// 待ち行列から取り出して OBS に渡す専用のスレッド (全ストリームで 1 本)。取り出す時刻まで待つ。
	//   - デコードのスレッドに相乗りしない: デコードは 1 フレームに数 ms から十数 ms かかり (ソフトウェアデコード)、
	//     その間は時刻が来ても渡せないので、渡す時刻が揺れる。
	//   - OBS の描画の tick のコールバックに相乗りしない: 渡す時刻が描画の周期 (60 fps で 16.7 ms) に丸まり、
	//     60 fps の共有と 60 fps の描画の位相しだいでフレームの間隔が乱れる。
	//   - 待ちは condition_variable::wait_for で行う。OBS は libobs の DllMain (util/platform-windows.c) で
	//     timeBeginPeriod(1) を呼ぶので、OBS の中の待ちは約 1 ms 刻みで起きる (cordmux からは Windows の API を
	//     呼ばない。呼ぶのは platform/ の中だけ)。待ち時間は切り上がることがあり、渡す時刻は最大で約 1 ms 遅れる。
	//     早く起きたときは、時刻を見直してもう一度待つ。
	//   - 全ストリームで共有する: 待っている間は CPU を使わず、渡す処理は 1 フレームに数百 us のコピーだけで、4 ストリーム
	//     でも時刻が重なることは少ない。ストリームごとに 1 本持つとスレッドが増えるだけである。
	void frame_pacer_loop();

	// mu を持っていないこと。取り出したフレームを、経路と枠を見直してから OBS に渡す。待っている間に枠がほかの人に
	// 渡ったり、切れたり、合成のソースがなくなったりしていれば、渡さずに数える。
	void release_paced_frame(PacedFrame &paced);

	// 表示の緩衝を替えたとき (set_display_buffer_ms)、積んである全ストリームの待ち行列に D と上限を反映する。
	// pacer_mu を持っていること。
	void configure_pacers_locked();

	// どちらも mu を持っていないこと (中で mu と pacer_mu を取る)。ストリームの待ち行列を捨てて、捨てたフレームを
	// 数える。discard_all_pacers は取り出し中のフレームがないものだけを取り除く。
	void discard_pacer(std::uint32_t stream_id);
	void discard_all_pacers();

	// どちらも mu を持っていてもいなくてもよい (listener.request_keyframe は asio::post するだけで、すぐに戻る。
	// ws/listener.cpp で確認した)。上限を超えてキーフレームを待ち始めたときの要求は、落とすと次のキーフレームが
	// 来るまで映像が止まったままになるので、request_keyframe_now で必ず 1 通送る。待っている間に繰り返す要求
	// (待っている間に届いて捨てる P フレームごとの要求と、デコーダーがキーフレームを求めたとき) だけを
	// request_keyframe_throttled で間引く。
	void request_keyframe_now(ScreenStream &s);
	void request_keyframe_throttled(ScreenStream &s);

	// ---- 録画 (mu を持ったまま呼ぶ。mu の後に record_mu を取る) ----
	//
	// 現在の stream_formats と表示名を Recorder に渡し直す (録画は経路ではなく stream_formats を見る。枠に
	// 載っていない人も録画するため)。configure が来たとき、表示名が変わったとき、profiles が届いたときの 3 か所から
	// 呼ぶ。
	void notify_recorder_formats_locked();
	// 録画ログの session の行に書く値。バージョン、SPS と PPS を付けた回数、ダミーの時間帯の 3 つをまとめて渡す。
	// 録画が OFF の間も渡す (record/recorder.h。コピーを置き換えるだけで、RecordPlatformOps の関数を 1 つも
	// 呼ばない)。
	void notify_recorder_session_facts_locked();
	// その人の、配信に出す名前。
	std::string display_name_for_locked(const std::string &user_id) const;
	// 録画を ON にした瞬間に、すべての画面のストリームにキーフレームを求める (録画のセグメントはキーフレームから
	// 始まるため。枠に載っていない人も録画するので、そのストリームにも求める)。
	void request_keyframes_for_recording_locked();

	// mu を持っていること。音のチャンク 1 つ (画面の音か声) を AudioMixer に置く (中で mixer_mu を取る)。out_ts_us は
	// 画面の音なら av_offset の map_audio を通した送信側のタイムスタンプ。置く先がなければ捨てて数える。置く先がないのは、
	// 合成のソースがないとき、ダミー表示の間 (composite_audio_plan.h の mixed_audio_admitted)、組が決まらない枠の
	// ときである。
	void add_mixed_audio_locked(std::uint32_t stream_id, const Route &route, std::int64_t sender_ts_us,
				    std::int64_t out_ts_us, const std::uint8_t *payload, std::uint32_t frame_count);

	// mu を持っていること。現在の経路と割り当て表から、AudioMixer の組の名簿を作り直す (中で mixer_mu を取る)。経路や
	// 割り当てが変わった所 (configure、枠の付け替え、切断、停止) で呼ぶ。枠を空けられた人、通話から消えた人、
	// 枠から外した人の組を remove して、前の人の残りの音を鳴らし続けない。
	void sync_mixer_roster_locked();

	// mu を持っていること。経路と割り当てが変わった所 (sync_mixer_roster_locked と同じ所) で呼ぶ。ミキサーの組を
	// 決め直し、画面の枠に載っているストリームが変わった枠をログに書く。
	void on_routes_changed_locked();

	// mu を持っていること。画面の枠ごとに、載っているストリームが前に書いたときと変わっていれば 1 行書く
	// (「Screen slot N carries stream M」、空いたら「Screen slot N carries no stream」)。試し運転 (test/tools/obs-run) が、
	// 合成のソースの配置の行 (Composite layout) の枠の番号を人に対応させるのに使う。ソースの名前から人を引けないためである。
	// 人の ID は書かず、ワイヤーの stream_id で書く。
	void log_screen_slot_streams_locked();
	// 最後に書いた、画面の枠の番号 -> stream_id。mu の下で読み書きする。
	std::map<int, std::uint32_t> logged_screen_slot_streams;

	// mixer_mu を持っていること。名簿の変化を AudioMixer に反映する。外した組は remove し、入った組には表の利得と
	// ミュートを当てる。
	void apply_mixer_changes_locked(const MixerRoster::Change &change);
	// mixer_mu を持っていること。表の利得とミュートと、話しているかの判定の仕方を、1 つの組に当てる。
	void apply_mixer_level_locked(const MixerKey &key);
	// mixer_mu を持っていること。自分の声の人 (メインアカウント) を表に入れ、前の人と現在の人の声の組がミキサーに
	// あれば当て直す (既定のミュートと判定の仕方が変わるため)。
	void set_self_voice_user_locked(const std::string &user_id);

	// mu を持っていること。configure に画面のストリームがある人と facts の sharing の人 (mixer_roster.h の
	// screen_sharers_of) から、画面共有を新しく始めた人を見分け、その人の画面の音をミュートにする (ScreenShareTracker)。
	// configure を適用する前に呼ぶ (組が名簿に入るときには、ミュートが表に入っている)。configure が届いていないとき
	// (facts の側から呼ぶとき) は nullptr を渡す。handle_configure と handle_facts の両方が呼ぶ。
	void note_screen_shares_locked(const ws::ConfigureMessage *configure);
	// 画面共有の始まりを見分ける。mu の下で読み書きする。
	ScreenShareTracker screen_shares;

	// 混ぜた音を取り出して合成のソースに渡す専用のスレッド。
	void mixer_loop();

	// 段階ごとの計数を定期的にログに書くループ (専用のスレッド。stats_control_mu と stats_control_cv で待つ)。
	void stats_loop();

	// ---- media と limits、合成のソースの有無 ----

	// mu を持っていること。現在の値から media の値を決める (判断は pipeline.h の media_inputs_of と decide_media)。
	bool media_value_locked() const;
	// mu を持っていること。値が変わったときだけ Listener に渡し、ログに 1 行書く。why はログに書く場面の名前
	// (英語)。
	void refresh_media_locked(const char *why);
	void refresh_limits_locked();
	// mu を持っていること。接続した直後に、値が変わっていなくても両方を送り直す。
	void resend_media_and_limits_locked();
	// mu を持っていること。視聴する人の範囲 (pipeline/receive_scope.h の watch_scope_of) を、変わったときだけ送信側に
	// 送り、ログに 1 行書く。why はログに書く場面の名前 (英語)。
	void refresh_watch_scope_locked(const char *why);
	// mu を持っていること。視聴の範囲 (next) に入った時刻 (watching_since_us) を、範囲の差分から更新する。入った人にその
	// 時刻を付け、外れた人を消し、Everyone なら全員を消す。restart が真なら、残る人もそのときの時刻にする (接続した直後)。
	void note_watch_since_locked(const ws::ReceiveScopeMessage &next, bool restart);
	// mu を持っていること。メインアカウントの userId を、変わったときだけ送信側に送る。
	void refresh_main_account_locked();
	// mu を持っていること。接続した直後に、値が変わっていなくても視聴する人の範囲とメインアカウントを送り直す。
	void resend_scope_and_main_locked();
	// mu を持っていること。受け取る範囲が Slots のとき、画面共有している人 (facts の sharing) のうち枠に載せてよい人を、
	// 映像が届く前に載せる (stream_router.h の admit_sharers)。載せたら視聴する人の範囲も決め直す。
	void admit_sharers_from_facts_locked();
	// mu を持っていること。facts の sharing と roster で、画面共有をやめた人と通話から抜けた人の枠を空ける
	// (pipeline/slot_release.h)。画面の枠の経路は枠 0 を指させ (release_slot と同じ)、外した人の一覧には入れない
	// (やめて始め直した人は、自動なら載る)。声の枠は、声のストリームが直前の configure に載っている間は空けず、
	// 空けるときは経路を触らない (ストリームが生きていないので経路もない)。include_screen が偽のときは、声の枠だけを
	// 見る (handle_configure。画面の枠を configure で空けると、configure が facts より先に届いたとき、画面共有を
	// 始めたばかりの人を外してしまう)。空けた画面共有の枠の番号を返す。呼び出し元が mu を放してから、映像を空にして
	// 並べ直す。
	std::set<int> release_departed_locked(bool include_screen);
	// mu を持っていること。この stream_id の映像と音声を録画の入力に通してよいか (pipeline/receive_scope.h の
	// record_stream_allowed)。configure に載っていない stream_id は、通す (今までと同じ)。
	bool record_allowed_locked(std::uint32_t stream_id) const;
	// mu を持っていること。調べた合成のソースの有無を保存する。
	void apply_layout_presence_locked(const LayoutPresence &read);
	// mu を持っていないこと。合成のソースの有無を調べ直して保存し、media を決め直す。有無が、最後に configure を
	// 適用したときと変わっていれば、configure を適用し直す。シーンコレクションの読み込みの前、切り替えの最中、
	// アンインストールの最中、停止の最中は何もしない。
	void refresh_layout_presence(const char *why);
	// 監視のスレッドの本体。
	void presence_loop();

	// 受信のやり直しを頼むかを決め (plan_receive_restart)、頼むストリームに stream_check を送り、そのストリームのデコードの
	// 経路を作り直す。presence_loop から 1 秒ごとに呼ぶ。mu を持たずに呼ぶ。
	void check_receive_restart();
	// 1 つのストリームのデコードの経路 (H264Decoder、TimestampAligner、AvOffsetTranscriber、待ち行列) を作り直し、
	// キーフレームを求める。フレームが届いているのにデコード済みのフレームが出ないとき (plan_decode_restart) だけに呼ぶ。
	// 合成のソースの子と枠の割り当てには触らない (sources/screen_end.h の ReceiveRestarted)。mu を持たずに呼ぶ。
	void rebuild_screen_decode(std::uint32_t stream_id);
	// 送信側の stream_check_result (io_thread から)。
	void handle_stream_check_result(const ws::StreamCheckResultMessage &msg);
};

Pipeline::Impl::ScreenStream *Pipeline::Impl::find_screen_stream_locked(std::uint32_t stream_id)
{
	const auto it = screen_streams.find(stream_id);
	return it == screen_streams.end() ? nullptr : it->second.get();
}

void Pipeline::Impl::rearm_decor_owner_locked(const std::string &user_id)
{
	for (auto &[stream_id, s] : screen_streams) {
		const std::optional<Route> route = router.route(stream_id);
		if (route && route->kind == config::SlotKind::Screen && route->user_id == user_id)
			s->decor_owner_pending = true;
	}
}

// ---------------------------------------------------------------------------
// configure
// ---------------------------------------------------------------------------

void Pipeline::Impl::handle_configure(const ws::ConfigureMessage &msg, std::optional<std::uint64_t> expect_generation)
{
	std::vector<RouteChange> changes;
	std::set<int> live_screen_after;
	// 送信側のストリームの追加と終了 (画面と声。出来事の行。configure は届くたびに数えない)。
	std::optional<std::pair<MemberChange, MemberChange>> stream_list_change;
	{
		std::lock_guard<std::mutex> lock(mu);

		// シーンコレクションの読み込みが終わるまでは適用せずに、最新の 1 通だけ覚えて待つ (pipeline.h の
		// on_frontend_finished_loading)。
		if (!loading_finished) {
			pending_configure = msg;
			diag::logf(LOG_INFO, "Holding the stream list from Vesktop until scenes finish loading");
			return;
		}

		if (expect_generation.has_value()) {
			// 適用し直す呼び出し。ロックを放していた間に新しい configure が適用されていたら、古い 1 通は
			// 適用しない (宣言のコメント)。
			if (*expect_generation != configure_generation)
				return;
		} else {
			// 新しい 1 通。適用し直すために覚えておく (Impl の last_configure)。保留する処理より後に置く。
			// まだ適用していない configure を「最後に適用した 1 通」として覚えると、シーンコレクションの
			// 読み込みが終わる前に適用し直してしまう。
			last_configure = msg;
			++configure_generation;
		}

		// メインアカウントの声の経路は、合成のソースがあるときだけ作る (stream_router.h の AssignPolicy)。有無が
		// 変わったら、refresh_layout_presence が適用し直す。
		AssignPolicy policy = assign_policy;
		policy.route_main_account_voice = sources::composite_present();
		configure_composite = policy.route_main_account_voice;
		// 画面共有を新しく始めた人の画面の音をミュートにする。経路を付けて組が名簿に入る前に、表へ入れておく。
		note_screen_shares_locked(&msg);
		changes = router.apply_configure(msg, table, policy);
		// 適用した後に画面の経路を持つ枠 (画面共有の終わりを見分けるために、下の changes の後始末の後で使う)。
		live_screen_after = live_screen_slots(router);
		// 経路と割り当てが変わったので、ミキサーの組も決め直す (枠を空けられた人と消えた人の組を外す)。
		on_routes_changed_locked();
		// 声のストリームが消えた人が、facts の roster にもいなければ、声の枠を空ける (facts が先に届いて、その時点では
		// ストリームが生きていたために残した枠)。画面の枠は空けない (release_departed_locked の宣言)。
		release_departed_locked(false);

		// ストリームごとの映像の形 (width、height、fps) を、まとめて入れ替える。configure はいま生きている
		// ストリームをすべて含むので、前の configure の行は残さない。0 のフィールドはそのまま持つ (送信側は、
		// 最初のエンコードの段階を決めるまでと、段階が unavailable の間は省く。0 を「解像度が 0」として画面に
		// 表示しないのは、表示する側の役目)。
		{
			std::vector<std::uint32_t> screens_before, voices_before, screens_after, voices_after;
			for (const auto &[stream_id, format] : stream_formats)
				(format.screen ? screens_before : voices_before).push_back(stream_id);
			for (const ws::StreamConfig &stream : msg.streams)
				(stream.kind == ws::StreamKind::Screen ? screens_after : voices_after).push_back(stream.stream_id);
			const MemberChange screens = member_change(screens_before, screens_after);
			const MemberChange voices = member_change(voices_before, voices_after);
			if (screens.changed() || voices.changed())
				stream_list_change = std::make_pair(screens, voices);
		}
		stream_formats.clear();
		for (const ws::StreamConfig &stream : msg.streams) {
			StreamFormat format;
			format.stream_id = stream.stream_id;
			format.user_id = stream.user_id;
			format.screen = stream.kind == ws::StreamKind::Screen;
			// int に収まらない値は「分からない」(0) として扱う。画面に表示する数なので、丸めた値を表示
			// すると読み違える (丸めずに捨てるのは facts_state.h の count と同じ)。
			format.width = stream.width <= 0x7FFFFFFFu ? static_cast<int>(stream.width) : 0;
			format.height = stream.height <= 0x7FFFFFFFu ? static_cast<int>(stream.height) : 0;
			format.fps = stream.fps;
			format.audio = stream.audio;
			stream_formats[stream.stream_id] = std::move(format);
		}

		// 録画が見る表はこれである (router.route() ではない)。経路の付け替えより前に渡す。録画の状態は
		// stream_id と user_id に結び付け、枠にも ScreenStream にもデコードのスレッドにも結び付けないので、
		// 経路の結果を待つ理由がない。
		notify_recorder_formats_locked();
		// 自動の割り当てなどで枠に載っている人が変わったかもしれないので、視聴する人の範囲を決め直す。
		refresh_watch_scope_locked("configure");
	}
	if (stream_list_change) {
		const MemberChange &s = stream_list_change->first;
		const MemberChange &v = stream_list_change->second;
		diag::logf(LOG_INFO, "Streams from Vesktop changed: screen=%zu added=%zu ended=%zu voice=%zu added=%zu ended=%zu",
			   s.now, s.added, s.removed, v.now, v.added, v.removed);
	}

	for (const auto &change : changes) {
		// この stream_id がもう画面の枠ではなくなったとき (configure から消えた。または、ワイヤー形式の
		// 使い方では起きないはずだが、同じ stream_id が声の枠に種類を変えた) と、画面の枠のまま別の人か別の
		// 枠に替わったとき (needs_screen_stream_recreate。stream_router.h) は、先にデコードのスレッドを止めて
		// 片付ける (後者は作り直すため)。片付けを済ませてから、下で新しい経路のデコードのスレッドを用意する。
		const bool left_screen = change.before && change.before->kind == config::SlotKind::Screen &&
					  (!change.after || change.after->kind != config::SlotKind::Screen);
		const bool recreate = needs_screen_stream_recreate(change);
		if (left_screen || recreate)
			remove_screen_stream(change.stream_id); // 中でデコードのスレッドを join する

		// 合成のソースがない間も、画面の経路にはデコードのスレッドを用意する。枠から外した人の経路 (枠 0) と同じく、
		// フレームは届いて数えられるが OBS には渡らない形で、後から合成のソースを追加したときに、次の configure を
		// 待たずに映る。その代わり、合成のソースがないシーンコレクションで録画だけを使い、画面の枠に載る人がいる
		// 間は、渡す先のないデコードが動く。
		if (change.after && change.after->kind == config::SlotKind::Screen)
			ensure_screen_stream(change.stream_id);
	}

	// 画面共有が終わった枠をすぐ空にし、同じ処理で並べ直す (画面共有の終わりが分かったときと、通話から
	// 抜けたときは、最後のフレームを残さない)。上の remove_screen_stream がデコードのスレッドを join した後に
	// 空にする (止まりかけのスレッドが、空にした後にフレームを出さないように)。mu を持たずに呼ぶ。名前ラベルに
	// 描く人も外す (空の枠に、前の人の名前とアバターを結び付けたままにしない。次の人の最初のフレームで、描く人が
	// 決まる)。configure からストリームが消えたのは、画面共有をやめたか通話から抜けたかで、どちらも同じに扱う
	// (sources/screen_end.h)。
	const sources::ScreenEndAction share_end = sources::screen_end_action(sources::ScreenEndEvent::ShareEnded);
	const std::set<int> ended = screen_slots_share_ended(changes, live_screen_after);
	for (const int slot : ended) {
		if (share_end.clear_now)
			sources::clear_screen_now(slot);
		if (share_end.clear_owner)
			sources::set_decor_owner(slot, std::string());
	}
	if (!ended.empty() && share_end.relayout)
		notify_layout_changed();

	// configure から消えた stream_id の計数を取り除く (config/stage_counters.h。stream_id ごとの表を無制限に
	// 増やさない)。上の remove_screen_stream の join がすべて終わってから行う。デコードのスレッドは止まる直前まで
	// counters.stream(...) を触るので、先に取り除くと、止まりかけのスレッドがその項目を作り直してしまう。
	{
		std::lock_guard<std::mutex> lock(mu);
		const auto live_ids = router.route_ids();
		counters.retain(live_ids);
		// ストリームごとの最後のフレームの時刻も、同じ一覧で取り除く (理由は上と同じ)。取り除き方は
		// pipeline.h の純粋関数 retain_frame_clocks にある (単体テストから使える)。
		retain_frame_clocks(last_frames, live_ids);
		// 受信のやり直しの答えも、同じ一覧で取り除く (stream_id ごとの表を無制限に増やさない)。
		for (auto it = stream_check_outcomes.begin(); it != stream_check_outcomes.end();) {
			if (live_ids.count(it->first) == 0)
				it = stream_check_outcomes.erase(it);
			else
				++it;
		}
	}
}

// ---------------------------------------------------------------------------
// log、facts、接続と切断
// ---------------------------------------------------------------------------

void Pipeline::Impl::handle_log(const ws::LogMessage &msg)
{
	// トークンとポート番号のチェックを先に通し、通った行だけをリングバッファーに入れる。出来事の行 (lines と
	// alerts) は js_log (診断情報の discord.log)、統計の行 (stats) は js_stats_log (discord-stats.log) に入れる。
	// チェックの判定と、OBS のログに書くかどうかは plugin-main.cpp が持つ (pipeline.h の set_js_log_sink。Pipeline は
	// トークンもポート番号も知らない)。チェックの関数は mu の外で呼ぶ (その関数が何をするかはここからは分からないので、
	// ほかの受信の関数を待たせない)。lines を alerts より先に積む。送信側は alerts の行を書いたときにすぐ送るので、
	// 1 通の中では、lines の行が alerts の行より前に書かれている (src-vencord/core/logbook.ts)。
	std::vector<const std::string *> events;
	std::vector<const std::string *> stats;
	events.reserve(msg.lines.size() + msg.alerts.size());
	stats.reserve(msg.stats.size());
	for (const auto &line : msg.lines) {
		if (js_log_sink(line, Pipeline::JsLineKind::Event))
			events.push_back(&line);
	}
	for (const auto &line : msg.alerts) {
		if (js_log_sink(line, Pipeline::JsLineKind::Alert))
			events.push_back(&line);
	}
	for (const auto &line : msg.stats) {
		if (js_log_sink(line, Pipeline::JsLineKind::Stats))
			stats.push_back(&line);
	}
	if (events.empty() && stats.empty())
		return;
	std::lock_guard<std::mutex> lock(mu);
	for (const std::string *line : events)
		js_log.push(*line);
	for (const std::string *line : stats)
		js_stats_log.push(*line);
}

void Pipeline::Impl::handle_facts(const ws::FactsMessage &msg)
{
	// 直近の facts をそのまま保持する (docs/wire_format.md の「facts」の形。読むのは pipeline/facts_state.h の
	// 純粋関数で、ここは受け取って保存するだけ)。
	FactsSnapshot parsed = parse_facts_payload(msg.facts);
	parsed.received = true;
	parsed.received_at_us = wall_now_us();

	// ログに書く値は、保存する前にまとめておく (ログは mu を放してから書く)。書くのは、届いたことと数だけである
	// (userId も依存箇所の id も書かない。ログに人を特定する値を残さない)。roster と stages はキーごと
	// 省かれることがあるので、省かれていることを "-" で書く (0 と区別する)。
	const std::string roster_text = parsed.roster ? std::to_string(parsed.roster->user_ids.size()) : std::string("-");
	const std::string stages_text = parsed.stages ? std::to_string(parsed.stages->size()) : std::string("-");
	// Vesktop のアカウント (self_user_id) は、届いたかどうかだけを書く (userId は書かない)。「使う声」のメニューに
	// Vesktop のアカウントが表示されたときに、送信側が送っていないのか、受信側が使っていないのかを、ログで
	// 見分けられるようにする。
	const char *self_text = parsed.self_user_id ? "yes" : "-";
	const std::size_t sharing_count = parsed.sharing.user_ids.size();
	const std::size_t warning_count = parsed.warnings.size();
	const int unreadable_keys = parsed.unreadable_keys;
	std::set<int> released_screen_slots;
	// 画面共有の枠を空けたときに、直前の configure を適用し直すための 1 通 (下の説明)。
	std::optional<ws::ConfigureMessage> replay;
	std::uint64_t replay_generation = 0;
	// 通話の参加者の出入りと、画面共有の始まりと終わり (出来事の行。下の Facts の行は統計の行)。
	std::optional<MemberChange> roster_change;
	std::optional<MemberChange> sharing_change;
	{
		std::lock_guard<std::mutex> lock(mu);
		// 前の facts と比べて、変わったときだけ出来事の行を書く。読めていない一覧 (roster のキーがない、sharing が
		// 壊れている) は比べない (分からないことを、出入りとして書かない)。
		if (parsed.roster) {
			const MemberChange c =
				member_change(facts.roster ? facts.roster->user_ids : std::vector<std::string>{}, parsed.roster->user_ids);
			if (c.changed())
				roster_change = c;
		}
		if (parsed.sharing_valid) {
			const MemberChange c = member_change(facts.sharing_valid ? facts.sharing.user_ids : std::vector<std::string>{},
							     parsed.sharing.user_ids);
			if (c.changed())
				sharing_change = c;
		}
		// 画面共有していると分かった時刻を前の facts から引き継ぐ (画面共有しているのに映像が届かない、という
		// メッセージに使う)。
		carry_sharing_since(facts, parsed, parsed.received_at_us);
		// 画面共有をやめた人は、チェックを外した人の一覧から消す (stream_router.h の exclude_user)。sharing の
		// キーがない facts では、誰がやめたか分からないので消さない。
		if (parsed.sharing_valid)
			router.forget_stopped_sharing(parsed.sharing.user_ids);
		facts = std::move(parsed);
		// 画面共有を始めた人の画面の音をミュートにする (configure より先に facts が届いたときの始まり。
		// 受け取る範囲が「枠に載せた人だけ」のときは、視聴を始めるまで configure に画面のストリームが載らない)。
		note_screen_shares_locked(last_configure ? &*last_configure : nullptr);
		// SPS と PPS を付けた回数は受信側からは見えないので、録画ログに残すにはここで渡すしかない。
		notify_recorder_session_facts_locked();
		// 画面共有をやめた人と、通話から抜けた人の枠を、すぐ空ける。空けた枠に、下で別の人が載れる。
		released_screen_slots = release_departed_locked(true);
		// 受け取る範囲が Slots のときは、映像が届かない人も、画面共有を始めたら枠に載せる。
		admit_sharers_from_facts_locked();
		// 枠を空けた後は、直前の configure を適用し直す。configure が facts より先に届くと、5 人目は枠が埋まっていて
		// 経路を持てず、その後に枠が空いても、次の configure まで映らない (枠から外す操作の assign_slot と同じ理由)。
		if (!released_screen_slots.empty() && loading_finished) {
			replay = last_configure;
			replay_generation = configure_generation;
		}
	}
	// 空けた画面共有の枠の映像をすぐ空にし、同じ処理で並べ直す (枠から外す操作と同じ。名前ラベルに描く人も外す)。
	// mu を持たずに呼ぶ。
	const sources::ScreenEndAction share_end = sources::screen_end_action(sources::ScreenEndEvent::ShareEnded);
	const auto end_released_slots = [this, &share_end](const std::set<int> &slots) {
		for (const int slot : slots) {
			if (share_end.clear_now)
				sources::clear_screen_now(slot);
			if (share_end.clear_owner)
				sources::set_decor_owner(slot, std::string());
		}
		if (!slots.empty() && share_end.relayout)
			notify_layout_changed();
	};
	end_released_slots(released_screen_slots);
	if (replay) {
		safe_call("the stream list after slots were released", [&] { handle_configure(*replay, replay_generation); });
		// facts より先に届いた configure には、画面共有をやめた人のストリームがまだ載っていることがある。自動のときは
		// 適用し直しがその人を空いた枠にまた入れるので、同じ facts でもう一度空ける (次の configure でストリームが
		// 消えても、枠を空けるのは facts だけなので、空けないと誰も空けない)。
		std::set<int> released_again;
		{
			std::lock_guard<std::mutex> lock(mu);
			released_again = release_departed_locked(true);
		}
		end_released_slots(released_again);
	}

	if (roster_change)
		diag::logf(LOG_INFO, "People in the call changed: count=%zu joined=%zu left=%zu", roster_change->now,
			   roster_change->added, roster_change->removed);
	if (sharing_change)
		diag::logf(LOG_INFO, "People sharing their screen changed: count=%zu started=%zu stopped=%zu",
			   sharing_change->now, sharing_change->added, sharing_change->removed);
	diag::logf(diag::LineKind::Stats, LOG_DEBUG,
	           "Facts: roster=%s sharing=%zu warnings=%zu stages=%s self=%s unreadable_keys=%d",
	           roster_text.c_str(), sharing_count, warning_count, stages_text.c_str(), self_text,
	           unreadable_keys);
}

void Pipeline::Impl::handle_profiles(const ws::ProfilesMessage &msg)
{
	// profiles は Pipeline が保持する (名前ラベルに渡すのはドックのポーリングの役目)。人の表は 1 通で全体を置き換える
	// (1 通は参加者全員の分なので、前のメッセージの分を残すと、通話から抜けた人の名前が残る)。アバターは引き継ぎ、
	// もういない人とバージョンが変わった人の分だけを捨てる (pipeline/profiles_state.h の next_profiles_snapshot)。
	const std::int64_t now_us = wall_now_us();
	std::size_t people = 0;
	int decode_failures = 0;
	{
		std::lock_guard<std::mutex> lock(mu);
		profiles = next_profiles_snapshot(profiles, msg, now_us);
		people = profiles.people.size();
		decode_failures = profiles.avatar_decode_failures;
		// 表示名が読めた時点で録画に渡し直す (名前が読めた時点で、録画ログに人の行をもう一度書く。profiles は
		// configure より後に届くことがある)。
		notify_recorder_formats_locked();
	}

	// ログに書くのは数だけである (userId も表示名も書かない。handle_facts と同じく、ログに人を特定する値を
	// 残さない)。
	diag::logf(diag::LineKind::Stats, LOG_DEBUG, "Profiles: people=%zu avatar_decode_failures=%d", people, decode_failures);
}

void Pipeline::Impl::handle_avatar(const ws::AvatarMessage &msg)
{
	// base64 のデコードは mu の外で行う (フレームの受信の関数を待たせない)。表に入れるのは mu の中で、名前ラベルに
	// 渡すのはドックのポーリングの役目である (profiles と同じ)。
	const DecodedAvatar decoded = decode_avatar(msg);
	bool stored = false;
	std::size_t with_avatar = 0;
	int decode_failures = 0;
	{
		std::lock_guard<std::mutex> lock(mu);
		stored = apply_decoded_avatar(&profiles, decoded);
		with_avatar = profiles.avatars.size();
		decode_failures = profiles.avatar_decode_failures;
	}
	// 数だけを書く (userId は書かない)。
	diag::logf(diag::LineKind::Stats, LOG_DEBUG, "Avatar: stored=%s people_with_avatar=%zu avatar_decode_failures=%d",
	           stored ? "yes" : "no", with_avatar, decode_failures);
}

void Pipeline::Impl::notify_layout_changed()
{
	// ロックを持ったまま呼ぶ。外す側 (set_screen_layout_changed_sink(nullptr)) が戻った後に、コールバック (ドックが
	// 持つもの) を呼ばないため。コールバックは UI のスレッドに処理を渡すだけで、ほかのロックを取らない
	// (pipeline.h の宣言)。
	std::lock_guard<std::mutex> lock(layout_sink_mu);
	if (layout_changed_sink)
		layout_changed_sink();
}

void Pipeline::Impl::handle_sender_clock(std::int64_t sender_us, std::int64_t received_us)
{
	// ws/listener.h の on_sender_clock から呼ばれる。received_us は、Listener がメッセージを読む前に読んだ OBS の時計
	// (Pipeline::start が receive_clock_us に obs_now_us を渡す)。
	const std::optional<std::int64_t> before = sender_clock.offset_us();
	if (!sender_clock.note(sender_us, received_us))
		return;
	const std::int64_t offset = *sender_clock.offset_us();
	sender_offset_us.store(offset, std::memory_order_relaxed);
	// 差は接続の初めに数回だけ下がる (pipeline/sender_clock.h)。下がった量は、前の値に含まれていた届くまでの遅れである。
	diag::logf(diag::LineKind::Stats, LOG_DEBUG, "Sender clock offset set (samples=%llu lowered_us=%lld)",
		   static_cast<unsigned long long>(sender_clock.samples()),
		   static_cast<long long>(before ? *before - offset : 0));
}

void Pipeline::Impl::handle_connected()
{
	// ws/listener.h の on_connected から呼ばれる。
	//
	// 送信側の時計と OBS の時計の差は接続ごとに決め直す (pipeline/sender_clock.h)。hello の時計の読みは、この後に届く
	// (ws/listener.h の on_sender_clock)。
	sender_clock.reset();
	sender_offset_us.store(kSenderOffsetUnknown, std::memory_order_relaxed);
	//
	// 接続した直後に録画の状態を送り直す。送信側は接続が切れても録画の状態を OFF に戻さないので、再接続の直後のこの
	// 1 通が、両側をそろえ直す唯一の機会である (食い違うのは切れている間だけになる)。接続していなければ、ログも
	// 残さずに捨てられる (ws/listener.cpp の send_recording)。
	listener.send_recording(recording_on.load());

	// media と limits も接続の直後に送り直す。初期値は hello_ok にも載っている (Listener が最後に渡された値を
	// 覚えている) が、別に 1 通ずつ送る (docs/wire_format.md の「メッセージの種類」。on_connected は hello_ok を
	// 作る前に呼ばれ、ここで頼んだ送信は hello_ok の後に送られる)。
	{
		std::lock_guard<std::mutex> lock(mu);
		linked = true;
		resend_media_and_limits_locked();
		// メインアカウントと視聴する人の範囲も同じ。
		resend_scope_and_main_locked();
		// 切れたときに映っていた枠は、ここから reconnect_layout_hold_ms の間だけ配置を保つ (pipeline.h の
		// LinkLostLayoutHold)。
		if (!link_lost_layout.owners.empty() && !link_lost_layout.reconnected_at_us.has_value())
			link_lost_layout.reconnected_at_us = steady_now_us();
	}

	// 録画の側にも伝える (録画ログのセグメントの区切りは、接続をまたぐかどうかで決まる)。
	if (recorder)
		recorder->on_connected();
}

void Pipeline::Impl::handle_disconnected()
{
	// ws/listener.h の on_disconnected から呼ばれる。古い経路 (StreamRouter) と、古い画面のデコードのスレッドを、
	// ここですべて片付ける (次の接続は、同じ stream_id を別の意味で使うことがあるため)。
	//
	// 切れた理由は Listener が持つので、mu の外で読んで渡す (同じ値を 2 か所で持たない。listener.stats() は mu を
	// 持たずに呼ぶ。ロックの順は Impl の mu の宣言)。
	const ws::DisconnectReason reason = listener.stats().last_disconnect_reason;
	std::map<std::uint32_t, std::unique_ptr<ScreenStream>> victims;
	// 切れる前に画面の経路を持っていた枠 (最後のフレームを残してから空にする枠)。
	std::set<int> held_slots;
	{
		std::lock_guard<std::mutex> lock(mu);
		if (stopping.load())
			return; // Pipeline::stop() が後始末している最中か後 (2 回後始末しない)
		// 受信のやり直しの状態も捨てる (次の接続は、同じ stream_id を別の意味で使うことがある)。
		linked = false;
		receive_restart_state = ReceiveRestartState{};
		decode_restart_state = DecodeRestartState{};
		stream_check_outcomes.clear();

		// 切れたら録画のセグメントを閉じる。stopping のチェックより後に置く。Pipeline::stop() の側が
		// Recorder::stop() で閉じているので、ここで 2 回伝えない (stop() の側でもセグメントを閉じるのは、この
		// 早い戻りがあるためである)。
		if (recorder)
			recorder->on_disconnected(reason);
		for (auto &[id, s] : screen_streams) {
			(void)id;
			s->stop_requested = true;
			s->queue_cv.notify_all();
		}
		victims = std::move(screen_streams);
		screen_streams.clear();
		held_slots = live_screen_slots(router);
		// 映っていた枠の配置を保つ (pipeline.h の LinkLostLayoutHold)。接続し直した後にまだ保っている枠 (前の途切れから
		// 映像が戻る前に、また切れた) も引き継ぐ。
		{
			std::map<int, std::string> owners =
				link_lost_layout_owners(link_lost_layout, steady_now_us(), reconnect_layout_hold_us.load());
			for (const auto &[slot, user_id] : live_screen_owners(router))
				owners[slot] = user_id;
			link_lost_layout.owners = std::move(owners);
			link_lost_layout.lost_at_us = steady_now_us();
			link_lost_layout.reconnected_at_us.reset();
		}
		router.clear();
		// 共有している人の記憶は残し、切れた時刻を覚える (つなぎ直した直後の configure を、共有の続きとして扱う)。
		screen_shares.on_link_lost(steady_now_us());
		on_routes_changed_locked();
		// 保留していた configure も捨てる (次の接続の configure が来るまで、適用するものがない)。
		// 適用し直すための「最後に適用した 1 通」も同じ理由で捨てる (切れた後に適用し直すと、もういない人の枠と
		// デコードのスレッドを作り直すことになる)。
		pending_configure.reset();
		last_configure.reset();
		++configure_generation; // 途中の適用し直しが、この後で適用されないようにする

		// 直近の facts と、ストリームごとの最後のフレームの時刻も捨てる。切れた後も前の通話の人数や映っている
		// 状態を画面に表示し続けると、事実と違う表示になる。ログのリングバッファー (js_log) は捨てない。
		// 接続していない間に何があったかを、診断情報に残すためである。
		facts = FactsSnapshot{};
		last_frames.clear();
		// 表示名とアバター、ストリームごとの映像の形も、同じ理由で捨てる。送信側は再接続の直後に、
		// 変わっていなくても必ず送り直すので、持ち越す必要がない (docs/wire_format.md の「profiles」と「avatar」)。
		profiles = ProfilesSnapshot{};
		stream_formats.clear();
		// 空になった表を録画にも渡す。Recorder は切れたときにセグメントを閉じるだけでストリームの表は残し、
		// 録画が OFF の間に渡された表を「ON にした時点の表」として使う。ここで渡し直さないと、切れている間に
		// 録画を ON にしたセッションが、前の接続の stream_id で始まる (再接続すると、同じ番号が別の
		// 人になることがある。stream_router.h の needs_screen_stream_recreate)。
		notify_recorder_formats_locked();
	}
	for (auto &[id, s] : victims) {
		(void)id;
		if (s->worker.joinable())
			s->worker.join();
	}
	// 表示の緩衝の待ち行列に残ったフレームも捨てる (次の接続で同じ stream_id が別の人に割り当てられると、切れる前の
	// 映像が遅れて出てしまうため)。
	discard_all_pacers();
	// デコードのスレッドを join し終えてから、stream_id ごとの計数を取り除く (handle_configure の最後と同じ理由)。
	// 合計には取り除いた分も残る (config/stage_counters.h の retain)。
	{
		std::lock_guard<std::mutex> lock(mu);
		counters.retain({});
	}
	// 接続が切れたときは、最後のフレームを残してから枠を空にし、並べ直さない (割り当て表は捨てないので、再接続して
	// 同じ人が画面共有し直せば、同じ枠に映り直す)。デコードのスレッドを join した後に予約する (止まりかけの
	// スレッドのフレームが予約を取り消さないように)。mu を持たずに呼ぶ。
	const sources::ScreenEndAction link_lost = sources::screen_end_action(sources::ScreenEndEvent::LinkLost);
	const std::uint64_t hold_ns = link_lost_hold_ns.load();
	for (const int slot : held_slots) {
		if (link_lost.hold_then_clear)
			sources::hold_then_clear_screen(slot, hold_ns);
		else if (link_lost.clear_now)
			sources::clear_screen_now(slot);
		if (link_lost.clear_owner)
			sources::set_decor_owner(slot, std::string());
	}
	diag::logf(LOG_INFO, "Disconnected from Vesktop; cleared screen share decoding and routes");
}

// ---------------------------------------------------------------------------
// 画面: デコードのスレッドの作成と削除
// ---------------------------------------------------------------------------

void Pipeline::Impl::ensure_screen_stream(std::uint32_t stream_id)
{
	{
		std::lock_guard<std::mutex> lock(mu);
		if (stopping.load())
			return; // 停止の最中と後に、新しいデコードのスレッドを起動しない (stopping の宣言)。
		if (screen_streams.find(stream_id) != screen_streams.end())
			return;
	}

	// settings.decoder を AVHWDeviceType に変換するのは pipeline の役目 (config/settings.h)。
	decode::DecoderConfig dcfg;
	dcfg.hw_type =
		(settings.decoder == config::DecoderChoice::D3D11VA) ? AV_HWDEVICE_TYPE_D3D11VA : AV_HWDEVICE_TYPE_NONE;

	// ScreenStream の作成 (H264Decoder の確保を含む) と、デコードのスレッドの起動は mu の外で行う (確保や
	// std::thread の作成を mu を持ったまま行うと、時間がかかったときや失敗したときに、その間ほかのすべての
	// ストリームの処理を止めてしまう。ここで漏れた例外は、呼び出し元 (handle_configure) を包む safe_call が
	// 捕まえる)。av_cfg は、画面の音のタイムスタンプに足す定数を持つ。作り直すたびに渡すので、同じ
	// stream_id がほかの枠に替わっても、補正は動作したままになる。
	auto s = std::make_unique<ScreenStream>(stream_id, dcfg, av_cfg);
	ScreenStream *raw = s.get();
	raw->worker = std::thread([this, raw] { screen_worker_main(raw); });

	bool discard = false;
	{
		std::lock_guard<std::mutex> lock(mu);
		// mu を放していた間に stop() が呼ばれたか、同じ stream_id のものがもうできていたら (通常は起きないが
		// 念のため)、いま作ったものは使わずに捨てる。
		// 経路が画面の枠でなくなっていれば (mu を放していた間に configure が経路を消した)、作ったものは使わない。
		// 受信のやり直し (rebuild_screen_decode) は presence_loop のスレッドから呼ぶので、configure の処理と前後しても、
		// 経路のないストリームのデコードのスレッドを残さないためである。
		const auto route = router.route(stream_id);
		if (stopping.load() || screen_streams.find(stream_id) != screen_streams.end() || !route ||
		    route->kind != config::SlotKind::Screen) {
			discard = true;
		} else {
			screen_streams.emplace(stream_id, std::move(s));
		}
	}
	if (discard) {
		{
			std::lock_guard<std::mutex> lock(mu);
			raw->stop_requested = true;
		}
		raw->queue_cv.notify_all();
		if (raw->worker.joinable())
			raw->worker.join();
	}
}

void Pipeline::Impl::remove_screen_stream(std::uint32_t stream_id)
{
	std::unique_ptr<ScreenStream> victim;
	{
		std::lock_guard<std::mutex> lock(mu);
		const auto it = screen_streams.find(stream_id);
		if (it == screen_streams.end())
			return;
		victim = std::move(it->second);
		screen_streams.erase(it);
	}
	// mu を放してから join する (join の間に、デコードのスレッドが push_decoded_frame の中で mu を
	// 取ろうとすることがあるため。mu を持ったまま join するとデッドロックになる)。
	{
		std::lock_guard<std::mutex> qlock(mu);
		victim->stop_requested = true;
	}
	victim->queue_cv.notify_all();
	if (victim->worker.joinable())
		victim->worker.join();
	// デコードのスレッドが止まった後に、表示の緩衝の待ち行列に残ったフレームを捨てる (同じ stream_id が別の人に
	// 替わったとき、前の人のフレームを渡さない。渡す側の経路の見直しと二重に守る)。
	discard_pacer(stream_id);
}

// ---------------------------------------------------------------------------
// 画面: デコードのスレッドの本体
// ---------------------------------------------------------------------------

void Pipeline::Impl::screen_worker_main(ScreenStream *s)
{
	for (;;) {
		QueuedAu au;
		{
			std::unique_lock<std::mutex> lock(mu);
			s->queue_cv.wait(lock, [s] { return s->stop_requested || !s->queue.empty(); });
			if (s->stop_requested) {
				// 止めると決まったら、キューに残っている分はデコードせずに捨てて数え、すぐに抜ける
				// (OBS を閉じる最中やシーンコレクションを切り替える最中に、壊れかけたシーンに映像を
				// 渡さないため。全部デコードしてから抜けると、その間 obs_module_unload も待たされる)。
				counters.stream(s->stream_id, config::MediaKind::Video)
					.add_dropped(std::uint64_t(s->queue.size()));
				s->queue.clear();
				s->queued_bytes = 0;
				return;
			}
			au = std::move(s->queue.front());
			s->queue.pop_front();
			s->queued_bytes -= au.data.size();
		}

		// デコードは mu の外で行う (重い処理をロックの中に入れない。Impl の mu の宣言)。
		std::vector<decode::DecodedFrame> frames;
		const bool need_keyframe = s->decoder.push(au.data.data(), au.data.size(), au.timestamp_us, &frames);
		if (need_keyframe)
			request_keyframe_throttled(*s); // 繰り返す要求なので間引く (宣言のコメント)
		// 切り替えはデコーダーを作るとき (最初の push) に決まる。設定の [デコーダー] が動作しないことを、画面には出さず
		// ログと診断情報に残す。人の名前と userId は書かない。
		if (s->hw_fallback_note.observe(s->decoder.stats().hw_fallback)) {
			s->hw_fallback.store(true);
			diag::logf(LOG_INFO, "Screen share stream %u: hardware decoding is unavailable; using software decoding",
				   static_cast<unsigned>(s->stream_id));
		}

		// デコードし終えた時刻。遅れの統計 (VideoLatency::decoded) の基準で、mu を待つ時間は入れない。
		const std::int64_t decoded_obs_us = obs_now_us();
		{
			std::lock_guard<std::mutex> lock(mu);
			counters.stream(s->stream_id, config::MediaKind::Video).add_decoded(std::uint64_t(frames.size()));
			if (!frames.empty()) {
				// plan_decode_restart が読む。出力が出たので、出力のないまま届いている時間を数え直す。
				s->last_decoded_us = steady_now_us();
				s->first_undecoded_arrival_us = 0;
			}
			for (const auto &frame : frames) {
				if (frame.timestamp_us != 0)
					s->latency.decoded.add(decoded_obs_us -
							       sender_to_obs_now(frame.timestamp_us, decoded_obs_us));
			}
			if (s->stop_requested) {
				// デコードしている間に止まった。デコードできたフレームも OBS に渡さずに捨てる (上と同じ
				// 理由)。
				counters.stream(s->stream_id, config::MediaKind::Video)
					.add_dropped(std::uint64_t(frames.size()));
				return;
			}
		}
		for (auto &frame : frames)
			push_decoded_frame(*s, frame);
	}
}

void Pipeline::Impl::request_keyframe_now(ScreenStream &s)
{
	s.last_keyframe_request_us.store(steady_now_us()); // 次の繰り返す要求は、ここから間引く
	listener.request_keyframe(s.stream_id); // どのスレッドから呼んでもよい (ws/listener.cpp で確認した)
}

void Pipeline::Impl::request_keyframe_throttled(ScreenStream &s)
{
	const std::int64_t now_us = steady_now_us();
	const std::int64_t prev = s.last_keyframe_request_us.load();
	if (now_us - prev < kKeyframeRequestMinIntervalUs)
		return;
	s.last_keyframe_request_us.store(now_us);
	listener.request_keyframe(s.stream_id);
}

// ---------------------------------------------------------------------------
// 録画に渡すもの
// ---------------------------------------------------------------------------

std::string Pipeline::Impl::display_name_for_locked(const std::string &user_id) const
{
	if (user_id.empty())
		return {};
	// ドックが決めた名前 (利用者が付けた名前の上書きを含む) を先に使う。
	const auto it = display_names.find(user_id);
	if (it != display_names.end() && !it->second.empty())
		return it->second;
	// ドックからまだ受け取っていない人は profiles の表示名を使う。ドックは周期 (既定 200 ms) ごとにしか渡さず、ドックが
	// 作られない起動もある。上書きのない人では両者は同じ値になり、上書きのある人でも、userId のままより近い名前が
	// 残る。録画ログは名前が読めた時点で人の行をもう一度書くので、後から正しい名前に変わることも妨げない。
	const std::optional<std::string> from_profiles = profiles.display_name_of(user_id);
	return from_profiles.has_value() ? *from_profiles : std::string();
}

void Pipeline::Impl::notify_recorder_formats_locked()
{
	if (!recorder)
		return;
	std::vector<StreamFormat> formats;
	formats.reserve(stream_formats.size());
	std::map<std::string, std::string> names;
	for (const auto &[stream_id, format] : stream_formats) {
		// 受け取る範囲が Slots のとき、枠に載っていない人の画面は録画の対象にしない。
		if (!record_allowed_locked(stream_id))
			continue;
		formats.push_back(format);
		std::string name = display_name_for_locked(format.user_id);
		if (!name.empty())
			names[format.user_id] = std::move(name);
	}
	// 録画が OFF の間も渡す (record/recorder.h。コピーを置き換えるだけで、RecordPlatformOps の関数を 1 つも
	// 呼ばない。OFF の間に渡さないと、ON にした瞬間にストリームの表が空のまま始まる)。
	recorder->on_configure(formats, names);
}

void Pipeline::Impl::notify_recorder_session_facts_locked()
{
	if (!recorder)
		return;
	record::SessionFacts session_facts;
	session_facts.plugin_version = plugin_version;
	// 値がなければ 0 のままにする (古い送信側はキーごと省く。docs/wire_format.md の「facts」)。
	session_facts.sps_pps_prepended = facts.sps_pps_prepended.value_or(0);
	session_facts.dummy_intervals = dummy_intervals;
	recorder->on_session_facts(std::move(session_facts));
}

void Pipeline::Impl::request_keyframes_for_recording_locked()
{
	// 見る表は stream_formats。経路のないストリームにも求める。
	for (const auto &[stream_id, format] : stream_formats) {
		if (!format.screen || !record_allowed_locked(stream_id))
			continue;
		listener.request_keyframe(stream_id); // asio::post するだけ (mu を持ったまま呼べる)
		// デコードのスレッドを持つストリームでは、繰り返す要求を間引く起点も進めておく (直後に同じ
		// ストリームにもう 1 通送らないため)。
		if (ScreenStream *s = find_screen_stream_locked(stream_id))
			s->last_keyframe_request_us.store(steady_now_us());
	}
}

// ---------------------------------------------------------------------------
// 画面: デコードできたフレームを OBS に渡す
// ---------------------------------------------------------------------------

void Pipeline::Impl::push_decoded_frame(ScreenStream &s, decode::DecodedFrame &frame)
{
	std::optional<Route> route;
	bool allowed = false;
	bool bound_decor_owner = false;
	std::int64_t out_us = 0;
	// 整列したタイムスタンプ。表示の緩衝の時刻の基準になる。
	std::int64_t aligned_us = 0;
	{
		// s.av_offset は受信のスレッド (handle_audio_frame の map_audio) からも触るので、note_video もここで
		// mu を持ったまま呼ぶ (mu の外で呼ぶと、mu を持っている map_audio とデータ競合になる。TimestampAligner
		// (s.aligner) はこのデコードのスレッドしか触らないが、同じロックの中にまとめておく)。
		std::lock_guard<std::mutex> lock(mu);
		out_us = s.aligner.align(frame.timestamp_us);
		aligned_us = out_us;
		s.av_offset.note_video(frame.timestamp_us, out_us);

		// 経路は stream_id だけで引く。映像は画面のストリームからしか来ないので、kind が Screen でなければ、
		// この ScreenStream は片付けられる途中である。
		route = router.route(s.stream_id);
		if (route && route->kind != config::SlotKind::Screen)
			route.reset();
		if (route) {
			if (s.decor_owner_pending && route->slot >= 1) {
				// デコードできたフレーム (キーフレームを待ってからデコードした、実際の映像) が来た最初の 1 回だけ、
				// 名前ラベルに描く人を結び付ける (ScreenStream の decor_owner_pending)。枠から外している間 (枠 0) は
				// 結び付ける先がないので、フラグを残して、枠に載ったときのフレームで結び付ける。
				s.decor_owner_pending = false;
				bound_decor_owner = true;
			}
			// 映像を出してよいかは、映像の判定で見る (枠の有無と凍結。
			// StreamRouter::video_allowed)。
			allowed = router.video_allowed(s.stream_id, table);
		}
		if (!route || !allowed)
			counters.stream(s.stream_id, config::MediaKind::Video).add_dropped(1);
	}

	// 枠に載っている人を名前ラベルに渡す。デコードできたフレームが来た最初の 1 回 (上の decor_owner_pending) に、この
	// フレームを OBS に渡す前に書くので、載っている人が替わった最初のフレームから、前の人の名前は出ない (新しい人の
	// 名前とアバターがまだなければ、名前ラベルを描かない)。映像を出してよいかにかかわらず書く (出さない間に書かないと、
	// 出し始めたときに名前ラベルに描く人がいないまま残る)。mu を放してから呼ぶ。
	if (bound_decor_owner && route)
		sources::set_decor_owner(route->slot, route->user_id);

	if (!route || !allowed)
		return;

	// 渡す先は合成のソースの子だけである (frame_delivery_plan.h)。合成のソースがなければ、待ち行列に積まずに
	// 捨てて数える。
	if (!frame_deliverable(sources::composite_present(), route->slot)) {
		std::lock_guard<std::mutex> lock(mu);
		counters.stream(s.stream_id, config::MediaKind::Video).add_dropped(1);
		return;
	}

	// OBS に渡さず待ち行列に積む (frame_pacer_loop が、送信側のタイムスタンプ + D の後の刻みに渡す)。D が 0 でも
	// 通す。積んだ後に合成のソースがなくなったときは、release_paced_frame が渡さずに捨てて数える。D の読み出しと
	// 積む処理は、set_display_buffer_ms の書き換えと食い違わないように、どちらも pacer_mu の中で行う。
	//
	// 時刻の基準は整列したタイムスタンプ (aligned_us) である。画面の音と映像のずれの補正は画面の音の位置を決める
	// 調整であって、映像を遅らせるものではない (add_mixed_audio_locked のコメント)。
	const std::int64_t push_obs_us = obs_now_us();
	const std::int64_t age_us = push_obs_us - sender_to_obs_now(aligned_us, push_obs_us);
	std::size_t dropped_on_push = 0;
	{
		std::lock_guard<std::mutex> pacer_lock(pacer_mu);
		const std::int64_t delay_us = display_buffer_us.load();
		auto it = pacers.find(s.stream_id);
		if (it == pacers.end()) {
			it = pacers.try_emplace(s.stream_id).first;
			configure_frame_pacer(it->second.pacer, delay_us);
		}
		PacedFrame paced;
		paced.stream_id = s.stream_id;
		paced.route = *route;
		paced.out_us = out_us;
		const std::size_t bytes = frame.plane[0].size() + frame.plane[1].size() + frame.plane[2].size();
		paced.frame = std::move(frame);
		it->second.arrival.note(push_obs_us / 1000000, age_us, delay_us);
		dropped_on_push = it->second.pacer.push(push_obs_us, age_us, bytes, std::move(paced));
	}
	pacer_cv.notify_one();
	if (dropped_on_push > 0) {
		std::lock_guard<std::mutex> lock(mu);
		counters.stream(s.stream_id, config::MediaKind::Video).add_dropped(std::uint64_t(dropped_on_push));
	}
}

void Pipeline::Impl::hand_frame_to_obs(std::uint32_t stream_id, int slot, const decode::DecodedFrame &frame,
				       std::int64_t out_us)
{
	obs_source_frame2 out{};
	out.width = std::uint32_t(frame.width);
	out.height = std::uint32_t(frame.height);
	out.timestamp = std::uint64_t(out_us) * 1000; // us -> ns (obs_source_frame2 は ns)
	out.format = (frame.format == decode::PixelFormat::I420) ? VIDEO_FORMAT_I420 : VIDEO_FORMAT_NV12;
	// 色の範囲は DecodedFrame.full_range から決める (decode/h264_decoder.h)。
	out.range = frame.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	out.flip = false;
	out.flags = 0;
	// 色空間は VIDEO_CS_DEFAULT にする。送信側が送る映像の実際の色空間は確認していない (送信側のエンコーダが出す
	// H.264 の色の情報を確認すれば決まる)。
	out.trc = VIDEO_TRC_DEFAULT;
	video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, out.range, out.format, out.color_matrix,
						out.color_range_min, out.color_range_max);

	const int planes = (out.format == VIDEO_FORMAT_I420) ? 3 : 2;
	for (int p = 0; p < planes; ++p) {
		out.data[p] = const_cast<std::uint8_t *>(frame.plane[p].data());
		out.linesize[p] = std::uint32_t(frame.linesize[p]);
	}

	// OBS に渡す直前の時刻。遅れの統計 (VideoLatency::handed) の基準で、渡す処理にかかった時間と mu を待つ時間は
	// 入れない。
	const std::int64_t handoff_obs_us = obs_now_us();
	const std::int64_t sent_obs_us = sender_to_obs_now(frame.timestamp_us, handoff_obs_us);
	// OBS が表示に選んだフレームの遅れから、送信側のタイムスタンプ (OBS の時計に換算した値) に戻すための補正 (整列した分と
	// 時計の差)。渡す前に設定する (渡したフレームが表示に選ばれたときには、設定されていなければならない)。
	sources::set_screen_shown_correction_us(slot, out_us - sent_obs_us);
	// 表示の時刻は待ち行列が決めたので、渡し先の OBS の緩衝は使わない (frame_delivery_plan.h)。
	sources::push_screen_video(slot, &out, /*unbuffered=*/true);

	std::lock_guard<std::mutex> lock(mu);
	// 渡している間に片付けられたストリームには、遅れの統計を足さない。
	if (ScreenStream *stream = find_screen_stream_locked(stream_id)) {
		stream->latency_slot = slot;
		if (frame.timestamp_us != 0)
			stream->latency.handed.add(handoff_obs_us - sent_obs_us);
	}
	counters.stream(stream_id, config::MediaKind::Video).add_handed_to_obs(1);
}

// ---------------------------------------------------------------------------
// 画面: 表示の緩衝 (待ち行列から、送信側のタイムスタンプ + D の時刻に渡す)
// ---------------------------------------------------------------------------

void Pipeline::Impl::frame_pacer_loop()
{
	for (;;) {
		std::vector<PacedFrame> ready;
		std::vector<std::pair<std::uint32_t, std::size_t>> skipped;
		{
			std::unique_lock<std::mutex> lock(pacer_mu);
			if (pacer_stop_requested)
				return;

			const std::int64_t now_us = obs_now_us();
			// 刻みの格子は回るたびに読み直す (軽い)。映像の設定が替わっても、待つ時刻と取り出しに適用される。
			const ObsTickGrid grid = read_obs_tick_grid();
			std::optional<std::int64_t> next_us;
			for (auto &[stream_id, slot] : pacers) {
				slot.pacer.set_tick_grid(grid.phase_us, grid.interval_us);
				// 時刻が来たフレームのうち最も新しい 1 つだけを取り出す。それより古いフレームは、待ち行列が
				// 捨てる (frame_pacer.h)。
				PacedTake<PacedFrame> taken = slot.pacer.take(now_us);
				if (taken.skipped > 0)
					skipped.emplace_back(stream_id, taken.skipped);
				if (taken.item) {
					++slot.in_flight;
					ready.push_back(std::move(*taken.item));
				}
				const std::optional<std::int64_t> release_us = slot.pacer.next_release_us();
				if (release_us && (!next_us || *release_us < *next_us))
					next_us = release_us;
			}

			if (ready.empty() && skipped.empty()) {
				// 渡すものがない。次の時刻まで待つ。もっと早く渡すフレームが積まれたら notify_one で起こされ、
				// この計算をやり直す。待つ間はロックを放す。
				if (!next_us)
					pacer_cv.wait(lock);
				else if (*next_us > now_us)
					pacer_cv.wait_for(lock, std::chrono::microseconds(*next_us - now_us));
				continue;
			}
		}

		// pacer_mu を放してから mu を取る (ロックの順は Impl の mu の宣言)。
		if (!skipped.empty()) {
			std::lock_guard<std::mutex> lock(mu);
			for (const auto &[stream_id, count] : skipped)
				counters.stream(stream_id, config::MediaKind::Video).add_dropped(std::uint64_t(count));
		}
		for (PacedFrame &paced : ready) {
			release_paced_frame(paced);
			std::lock_guard<std::mutex> lock(pacer_mu);
			const auto it = pacers.find(paced.stream_id);
			if (it != pacers.end() && it->second.in_flight > 0)
				--it->second.in_flight;
		}
	}
}

void Pipeline::Impl::release_paced_frame(PacedFrame &paced)
{
	int slot = 0;
	{
		std::lock_guard<std::mutex> lock(mu);
		const auto route = router.route(paced.stream_id);
		// 経路が積んだときと同じで、映像を出してよい状態のときだけ渡す。待っている間に枠がほかの人に渡った
		// (前に載っていた人の映像を、新しく載った人の枠に出さない)、接続が切れた、停止が始まった、合成のソースが
		// なくなった、のどれでも渡さずに数える。
		const bool deliver = !stopping.load() && route && *route == paced.route &&
				     router.video_allowed(paced.stream_id, table) &&
				     frame_deliverable(sources::composite_present(), paced.route.slot);
		if (!deliver) {
			counters.stream(paced.stream_id, config::MediaKind::Video).add_dropped(1);
			return;
		}
		slot = paced.route.slot;
	}
	hand_frame_to_obs(paced.stream_id, slot, paced.frame, paced.out_us);
}

void Pipeline::Impl::configure_pacers_locked()
{
	const std::int64_t delay_us = display_buffer_us.load();
	for (auto &[stream_id, slot] : pacers) {
		(void)stream_id;
		configure_frame_pacer(slot.pacer, delay_us);
	}
}

void Pipeline::Impl::discard_pacer(std::uint32_t stream_id)
{
	std::size_t dropped = 0;
	{
		std::lock_guard<std::mutex> lock(pacer_mu);
		const auto it = pacers.find(stream_id);
		if (it == pacers.end())
			return;
		dropped = it->second.pacer.discard_all();
		// 取り出し中のフレームがあれば、frame_pacer_loop が渡すか捨てるまで残す (経路が変わっていれば
		// release_paced_frame が捨てる)。
		if (it->second.in_flight == 0)
			pacers.erase(it);
	}
	if (dropped > 0) {
		std::lock_guard<std::mutex> lock(mu);
		counters.stream(stream_id, config::MediaKind::Video).add_dropped(std::uint64_t(dropped));
	}
}

void Pipeline::Impl::discard_all_pacers()
{
	std::vector<std::pair<std::uint32_t, std::size_t>> dropped;
	{
		std::lock_guard<std::mutex> lock(pacer_mu);
		for (auto it = pacers.begin(); it != pacers.end();) {
			const std::size_t count = it->second.pacer.discard_all();
			if (count > 0)
				dropped.emplace_back(it->first, count);
			if (it->second.in_flight == 0)
				it = pacers.erase(it);
			else
				++it;
		}
	}
	if (!dropped.empty()) {
		std::lock_guard<std::mutex> lock(mu);
		for (const auto &[stream_id, count] : dropped)
			counters.stream(stream_id, config::MediaKind::Video).add_dropped(std::uint64_t(count));
	}
}

// ---------------------------------------------------------------------------
// 映像と音声の受信の関数 (asio の io_thread から呼ばれる)
// ---------------------------------------------------------------------------

void Pipeline::Impl::handle_video_frame(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len)
{
	// payload はこの呼び出しの間だけ有効 (ws/listener.cpp の on_message が同期的に呼ぶ)。キューに積む前に
	// コピーする。
	// 受け取った時刻。遅れの統計 (VideoLatency::received) の基準で、mu を待つ時間は入れない。
	const std::int64_t recv_obs_us = obs_now_us();
	std::lock_guard<std::mutex> lock(mu);

	// ---- 録画に渡す ----
	//
	// 経路のチェックと last_frames の更新より前に置く。理由は 2 つある。(1) 録画専用のフラグ (kFlagRecordingOnly)
	// が付いたメッセージ (黒埋め用のフレーム) を、表示の処理に 1 枚も入れない (画面共有をやめた瞬間に、黒が 1 秒
	// だけ配信に出るのを防ぐ)。(2) フラグを見ずに last_frames を更新すると、画面共有が終わったストリームが
	// 動いているように見え、キーフレームを待つ状態が黒いフレームで解ける。また、経路のないストリームのメッセージも
	// 録画は受け取る (録画が見る表は枠ではなく stream_formats で、「枠に載っていないのにフレームは届いている」の
	// 判断に使う値もここを通る)。
	const bool recording_now = recording_on.load();
	if (header.is_recording_only()) {
		// 黒埋め用のフレーム。デコードも表示もしない。数えるのは録画の側の material_frames_received である。
		if (recording_now && recorder && record_allowed_locked(header.stream_id)) {
			recorder->on_video(header, payload, len, steady_now_us(), wall_now_us());
		} else {
			// 録画が OFF なのに黒埋め用のフレームが届いたのは、送信側との食い違いである。数えずに捨てると
			// 気付けないので数える。表示の段階の計数には入れられない (表示には 1 枚も渡さない) ので、
			// 「経路なし」の received と dropped にそれぞれ 1 を足す。
			counters.no_route(config::MediaKind::Video).add_received(1);
			counters.no_route(config::MediaKind::Video).add_dropped(1);
		}
		return;
	}
	if (recording_now && recorder && record_allowed_locked(header.stream_id))
		recorder->on_video(header, payload, len, steady_now_us(), wall_now_us());

	// 経路の有無を先に見る (config/stage_counters.h。stream_id ごとの表を無制限に増やさない)。configure より前に
	// 届いたメッセージ、シーンコレクションの読み込みが終わる前に届いたメッセージ (configure を保留している間)、
	// 壊れた送信側が示した stream_id は、どれもここで「経路なし」の 1 つの計数に入れる。
	const auto route = router.route(header.stream_id);
	if (!route || route->kind != config::SlotKind::Screen) {
		counters.no_route(config::MediaKind::Video).add_received(1);
		counters.no_route(config::MediaKind::Video).add_dropped(1);
		return;
	}

	counters.stream(header.stream_id, config::MediaKind::Video).add_received(1);

	// ストリームごとの最後のフレームの時刻 (ドックが、映像が届いていない人を表示するときに読む)。届いた時点の時刻を
	// 残す。デコードや出力の判定 (枠がない、キューの上限、渡す先がない) で捨てたフレームも、届いては
	// いる。
	// この表示は、接続とハートビートが動いているのにフレームが届かない状態を指すので、届いたことをそのまま持つ。
	{
		StreamFrameClock &clock = last_frames[header.stream_id];
		clock.stream_id = header.stream_id;
		clock.user_id = route->user_id;
		clock.kind = route->kind;
		clock.slot = route->slot;
		clock.last_video_frame_steady_us = steady_now_us();
		clock.last_video_frame_wall_us = wall_now_us();
	}

	ScreenStream *s = find_screen_stream_locked(header.stream_id);
	if (!s) {
		// 経路はあるがデコードのスレッドがまだない (handle_configure が mu を放してから、
		// ensure_screen_stream が終わるまでの間)。このストリームの dropped に数える。
		counters.stream(header.stream_id, config::MediaKind::Video).add_dropped(1);
		return;
	}
	if (header.timestamp_us != 0)
		s->latency.received.add(recv_obs_us - sender_to_obs_now(std::int64_t(header.timestamp_us), recv_obs_us));
	// 出力がないまま届き始めた時刻 (plan_decode_restart)。キーフレームを待って捨てるフレームも、届いたものとして数える。
	if (s->first_undecoded_arrival_us == 0)
		s->first_undecoded_arrival_us = steady_now_us();

	const bool is_key = header.is_keyframe();
	const QueueAdmissionInput admission_in{s->queue.size(), s->queued_bytes, len, is_key, s->waiting_for_keyframe};
	switch (decide_queue_admission(admission_in)) {
	case QueueAdmission::DropWaitingForKeyframe:
		counters.stream(header.stream_id, config::MediaKind::Video).add_dropped(1);
		// キーフレームを待っている間に繰り返す要求なので、間引く (最初の要求が届かなかったときに、ずっと
		// 待ち続けないための要求である)。
		request_keyframe_throttled(*s);
		return;
	case QueueAdmission::OverflowClear:
		// 上限を超えたら、たまっていた分も含めて全部捨て、次のキーフレームまで捨て続ける。デコードが
		// 追い付くのを待つより、次のキーフレームから出し直すほうが早く追い付く。バイト数の上限も同じ扱い
		// (queue_admission.h)。
		counters.stream(header.stream_id, config::MediaKind::Video).add_dropped(std::uint64_t(s->queue.size()) + 1);
		s->queue.clear();
		s->queued_bytes = 0;
		s->waiting_for_keyframe = true;
		// キーフレームを待ち始めたところなので、間引かずに必ず 1 通送る (この 1 通を間引くと、キーフレームが
		// 来ないまま映像が止まる)。
		request_keyframe_now(*s);
		return;
	case QueueAdmission::Admit:
		s->waiting_for_keyframe = false; // キーフレームを待っていたなら、待つのをやめる
		break;
	}

	Impl::QueuedAu au;
	au.timestamp_us = std::int64_t(header.timestamp_us);
	au.flags = header.flags;
	au.data.assign(payload, payload + len);
	s->queued_bytes += au.data.size();
	s->queue.push_back(std::move(au));
	s->queue_cv.notify_one();
}

void Pipeline::Impl::handle_audio_frame(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len)
{
	// 音声のメッセージの形は kAudioSampleRate と kAudioChannels (docs/wire_format.md の「音声のペイロード」)。
	const std::size_t frame_bytes = sizeof(float) * kAudioChannels;

	// 受け取った時刻。遅れの統計 (AudioLatency::received) の基準で、mu を待つ時間は入れない。
	const std::int64_t recv_obs_us = obs_now_us();
	std::lock_guard<std::mutex> lock(mu);

	// ---- 録画に渡す ----
	//
	// 経路の有無のチェック (この少し下の if (!route)) より前に置く。録画は configure に載っているすべての
	// ストリームを対象にし、枠に載っているかは条件にしないので、経路のチェックの後に置くと、枠に載っていない人の
	// 音声が 1 サンプルも録画されない (映像の側も同じ理由で経路のチェックより前に置く)。長さのチェックだけは、
	// ここで自分で行う (壊れたメッセージを録画に渡さない)。
	//
	// 表示のための 2 つの補正 (screen_audio_offset_ms と voice_audio_delay_ms) はかけない。補正は libobs で
	// 再生するときのずれに合わせた値で、録画は libobs を通らない。渡すのは送信側のタイムスタンプそのままである。
	const bool recording_now = recording_on.load();
	const bool audio_length_ok = len != 0 && len % frame_bytes == 0;
	if (header.is_recording_only()) {
		// 録画専用のフラグが付いた音声が届いたら、録画に渡して、録画の側が捨てて数える (黒埋め用に送るのは
		// 映像だけで、音声は録画の側が 0 で埋める)。数えるのは録画の側の material_audio_discarded である。
		if (recording_now && recorder && audio_length_ok && record_allowed_locked(header.stream_id)) {
			// 長さのチェックはここでも通す (壊れたメッセージを録画に渡さない)。この音声は録画の側が捨てて
			// 数えるが、渡す形はそろえる。
			recorder->on_audio(header, payload, len, steady_now_us(), wall_now_us());
		} else {
			// 渡さなかった音声も数える (数えずに捨てない)。録画が OFF なのに届いたものは送信側との
			// 食い違いで、長さが形に合わないものは壊れたメッセージである。どちらも、映像の側と同じく
			// 「経路なし」の計数に入れる。
			counters.no_route(config::MediaKind::Audio).add_received(1);
			counters.no_route(config::MediaKind::Audio).add_dropped(1);
		}
		return;
	}
	if (recording_now && recorder && audio_length_ok && record_allowed_locked(header.stream_id))
		recorder->on_audio(header, payload, len, steady_now_us(), wall_now_us());

	// 音声のメッセージは、画面の音か声かをワイヤー形式では示さない (ws/wire.h のヘッダーは type と
	// stream_id だけ)。stream_id の経路の kind で決める (画面の音と声は AudioMixer の別の組に置く。
	// composite_audio_plan.h の mixer_key_for。画面共有の音を声の組に置かないのは、この kind による)。経路がなければ
	// 「経路なし」の 1 つの計数に入れる (handle_video_frame と同じ扱い)。
	const auto route = router.route(header.stream_id);
	if (!route) {
		counters.no_route(config::MediaKind::Audio).add_received(1);
		counters.no_route(config::MediaKind::Audio).add_dropped(1);
		return;
	}

	counters.stream(header.stream_id, config::MediaKind::Audio).add_received(1);

	if (!audio_length_ok) {
		// 壊れたメッセージ (ws/wire.h は type と flags しか見ないので、payload の長さは、上の録画に渡す所と、
		// ここで確認する)。
		counters.stream(header.stream_id, config::MediaKind::Audio).add_dropped(1);
		return;
	}
	const std::uint32_t frame_count = std::uint32_t(len / frame_bytes);
	if (header.timestamp_us != 0)
		audio_latency[header.stream_id].received.add(
			recv_obs_us - sender_to_obs_now(std::int64_t(header.timestamp_us), recv_obs_us));

	std::int64_t out_ts_us = std::int64_t(header.timestamp_us);

	if (route->kind == config::SlotKind::Screen) {
		ScreenStream *s = find_screen_stream_locked(header.stream_id);
		if (s)
			out_ts_us = s->av_offset.map_audio(out_ts_us);
	}
	// 枠がない経路 (枠から外した人の経路は枠 0 を指す) と、切り替えの最中の凍結は、output_allowed が両方を見る。
	const bool allowed = router.output_allowed(header.stream_id, table);

	if (!allowed) {
		counters.stream(header.stream_id, config::MediaKind::Audio).add_dropped(1);
		return;
	}

	// 画面の音も声も AudioMixer に置く。置く先がなければ (合成のソースがない、ダミー表示の間)、中で捨てて数える。
	add_mixed_audio_locked(header.stream_id, *route, std::int64_t(header.timestamp_us), out_ts_us, payload,
			       frame_count);
}

// ---------------------------------------------------------------------------
// 混ぜた音
// ---------------------------------------------------------------------------

void Pipeline::Impl::apply_mixer_changes_locked(const MixerRoster::Change &change)
{
	for (const MixerKey &key : change.removed)
		mixer.remove(key);
	// AudioMixer は remove すると利得も捨てる。組が名簿に入るたびに、表の値を当て直す。
	for (const MixerKey &key : change.added)
		apply_mixer_level_locked(key);
}

void Pipeline::Impl::apply_mixer_level_locked(const MixerKey &key)
{
	apply_mixer_level(mixer, mixer_levels, key);
}

void Pipeline::Impl::set_self_voice_user_locked(const std::string &user_id)
{
	const std::string before = mixer_levels.self_voice_user();
	if (!mixer_levels.set_self_voice_user(user_id))
		return;
	for (const std::string &id : {before, user_id}) {
		const MixerKey key{id, MixerKind::Voice};
		if (!id.empty() && mixer_roster.contains(key))
			apply_mixer_level_locked(key);
	}
}

void Pipeline::Impl::on_routes_changed_locked()
{
	sync_mixer_roster_locked();
	log_screen_slot_streams_locked();
}

void Pipeline::Impl::log_screen_slot_streams_locked()
{
	std::map<int, std::uint32_t> now;
	for (const std::uint32_t stream_id : router.route_ids()) {
		const auto route = router.route(stream_id);
		if (route && route->kind == config::SlotKind::Screen && route->slot >= 1)
			now[route->slot] = stream_id;
	}
	for (int slot = 1; slot <= config::kScreenSlotCount; ++slot) {
		const auto before = logged_screen_slot_streams.find(slot);
		const auto after = now.find(slot);
		const bool had = before != logged_screen_slot_streams.end();
		const bool has = after != now.end();
		if (has && (!had || before->second != after->second))
			diag::logf(LOG_INFO, "Screen slot %d carries stream %u", slot, static_cast<unsigned>(after->second));
		else if (!has && had)
			diag::logf(LOG_INFO, "Screen slot %d carries no stream", slot);
	}
	logged_screen_slot_streams = std::move(now);
}

void Pipeline::Impl::note_screen_shares_locked(const ws::ConfigureMessage *configure)
{
	std::set<std::string> in_configure;
	if (configure != nullptr) {
		for (const ws::StreamConfig &stream : configure->streams) {
			if (stream.kind == ws::StreamKind::Screen && !stream.user_id.empty())
				in_configure.insert(stream.user_id);
		}
	}
	const std::set<std::string> sharing = screen_sharers_of(in_configure, facts.sharing_valid, facts.sharing.user_ids);
	const std::vector<std::string> started = screen_shares.update(sharing, steady_now_us());
	if (started.empty())
		return;
	{
		std::lock_guard<std::mutex> mixer_lock(mixer_mu);
		for (const std::string &user_id : started) {
			mixer_levels.mute_for_share_start(user_id);
			const MixerKey key{user_id, MixerKind::ScreenAudio};
			if (mixer_roster.contains(key))
				apply_mixer_level_locked(key);
		}
	}
	diag::logf(LOG_INFO, "Screen share started; its audio starts muted (people: %zu)", started.size());
}

void Pipeline::Impl::sync_mixer_roster_locked()
{
	const std::map<std::uint32_t, MixerKey> desired = desired_mixer_keys(router, table);
	std::lock_guard<std::mutex> mixer_lock(mixer_mu);
	apply_mixer_changes_locked(mixer_roster.sync(desired));
}

void Pipeline::Impl::add_mixed_audio_locked(std::uint32_t stream_id, const Route &route, std::int64_t sender_ts_us,
					    std::int64_t out_ts_us, const std::uint8_t *payload,
					    std::uint32_t frame_count)
{
	// 合成のソースがない間は音の出口がなく、ダミー表示の間は実際の音を配信に出さない (composite_audio_plan.h)。
	if (!mixed_audio_admitted(sources::composite_present(), sources::dummy_mode_active())) {
		counters.stream(stream_id, config::MediaKind::Audio).add_dropped(1);
		return;
	}
	const config::SlotAssignment *assignment = table.assignment(route.kind, route.slot);
	const std::optional<MixerKey> key = mixer_key_for(route, assignment ? assignment->owner_user_id : std::string());
	if (!key) {
		counters.stream(stream_id, config::MediaKind::Audio).add_dropped(1);
		return;
	}

	// タイムスタンプのない音は、届いた時刻に着いたものとして置く。
	const std::int64_t obs_now = obs_now_us();
	const std::int64_t mapped_obs_us = sender_ts_us != 0 ? sender_to_obs_now(out_ts_us, obs_now) : obs_now;

	audio_scratch.resize(std::size_t(frame_count) * kAudioChannels);
	std::memcpy(audio_scratch.data(), payload, audio_scratch.size() * sizeof(float));

	{
		// D の読み出しと置くことを同じ mixer_mu の中で行う (set_display_buffer_ms の resync と食い違わない)。
		std::lock_guard<std::mutex> mixer_lock(mixer_mu);
		apply_mixer_changes_locked(mixer_roster.set_stream(stream_id, key));
		const std::int64_t at_us =
			route.kind == config::SlotKind::Screen
				? screen_audio_at_us(mapped_obs_us, obs_now, display_buffer_us.load(),
						     read_obs_tick_grid().interval_us, audio_early_us)
				: voice_audio_at_us(obs_now, voice_delay_us);
		mixer.add(obs_now, *key, at_us, audio_scratch.data(), frame_count);
	}

	counters.stream(stream_id, config::MediaKind::Audio).add_handed_to_obs(1);
	if (sender_ts_us != 0) {
		const std::int64_t handed_obs_us = obs_now_us();
		audio_latency[stream_id].handed.add(handed_obs_us - sender_to_obs_now(sender_ts_us, handed_obs_us));
	}
}

void Pipeline::Impl::publish_speaking_users()
{
	std::vector<std::string> users = voice_activity.talking_users();
	std::lock_guard<std::mutex> lock(speaking_mu);
	if (users != speaking_users)
		speaking_users = std::move(users);
}

void Pipeline::Impl::mixer_loop()
{
	bool composite_before = false;
	auto next = std::chrono::steady_clock::now();
	for (;;) {
		std::vector<MixedBlock> blocks;
		bool composite_changed = false;
		{
			std::unique_lock<std::mutex> lock(mixer_mu);
			next += kMixerTakeInterval;
			const auto now = std::chrono::steady_clock::now();
			if (now - next > kMixerLagResetLimit)
				next = now;
			if (mixer_cv.wait_until(lock, next, [this] { return mixer_stop_requested; }))
				return;

			// 合成のソースが現れた・なくなった切り替わりで、AudioMixer の起点と保持している音を捨てる。ないときは
			// 取り出さない (起点があると、無音の区間を出し続ける)。
			const bool composite = sources::composite_present();
			if (composite != composite_before) {
				mixer.reset();
				composite_before = composite;
				composite_changed = true;
			}
			if (composite)
				blocks = mixer.take(obs_now_us());
		}
		// 合成のソースが現れた・なくなったときは、前の発言を捨てる (光を残さない)。mixer_mu を放してから、表のロックを
		// 取る。
		if (composite_changed) {
			voice_activity.reset();
			sources::clear_voice_speaking();
			publish_speaking_users();
		}
		if (blocks.empty())
			continue;

		// 区間ごとに、聞こえる声のあった人の話し始めと話し終わりの時刻を、合成のソースが読む表に書く。時刻は区間の
		// 開始 (音が鳴る時刻) なので、光と声がそろう。mixer_mu を放してから書く (Impl の mixer_mu の宣言)。
		const std::int64_t block_us = std::int64_t(blocks.front().frames) * 1000000 / kMixerSampleRate;
		for (const MixedBlock &block : blocks) {
			for (const VoiceActivityUpdate &update :
			     voice_activity.observe(block.start_us, block_us, block.audible_voices))
				sources::set_voice_speaking(update.user_id, update.start_ns, update.stop_ns);
		}
		publish_speaking_users();

		// mixer_mu を放してから渡す (Impl の mixer_mu の宣言)。
		std::vector<obs_source_audio> audios;
		audios.reserve(blocks.size());
		for (const MixedBlock &block : blocks)
			audios.push_back(make_audio_frame(reinterpret_cast<const std::uint8_t *>(block.samples.data()),
							  std::uint32_t(block.frames), block.start_us));
		sources::output_composite_audio(audios.data(), audios.size());
	}
}

// ---------------------------------------------------------------------------
// 段階ごとの計数を定期的にログに書く
// ---------------------------------------------------------------------------

void Pipeline::Impl::stats_loop()
{
	config::StageCounters prev_video;
	config::StageCounters prev_audio;
	std::int64_t prev_us = steady_now_us();

	std::unique_lock<std::mutex> control_lock(stats_control_mu);
	for (;;) {
		const bool woke_to_stop =
			stats_control_cv.wait_for(control_lock, kStatsLogInterval, [this] { return stats_stop_requested; });
		if (woke_to_stop)
			return;
		control_lock.unlock();

		config::StageCounters cur_video;
		config::StageCounters cur_audio;
		config::StageCounters no_route_video;
		config::StageCounters no_route_audio;
		{
			std::lock_guard<std::mutex> lock(mu);
			cur_video = counters.total_snapshot(config::MediaKind::Video);
			cur_audio = counters.total_snapshot(config::MediaKind::Audio);
			no_route_video = counters.no_route_snapshot(config::MediaKind::Video);
			no_route_audio = counters.no_route_snapshot(config::MediaKind::Audio);
		}

		const std::int64_t now_us = steady_now_us();
		const auto rv = config::rate_between(prev_video, cur_video, now_us - prev_us);
		const auto ra = config::rate_between(prev_audio, cur_audio, now_us - prev_us);
		// 判定には使わず、原因を調べるためだけの値。実機テストのツール (test/tools/obs-run/run-window.mjs の
		// collect-plugin-log) が、[cordmux] で始まるこの行を集める。キーの名前と並び順は変えない (行を読む
		// 正規表現が、別の値を拾うことがある)。末尾の no_route_dropped は、経路がなくて捨てたメッセージの
		// 数である (「経路なし」は 1 つの計数にまとめていて、stream_id ごとの内訳に出ないので、ここに書く)。
		diag::logf(diag::LineKind::Stats, LOG_DEBUG,
		           "Stats video received_hz=%.1f decoded_hz=%.1f handed_to_obs_hz=%.1f dropped_hz=%.1f "
		           "received=%llu decoded=%llu handed_to_obs=%llu dropped=%llu no_route_dropped=%llu",
		           rv.received_hz, rv.decoded_hz, rv.handed_to_obs_hz, rv.dropped_hz,
		           static_cast<unsigned long long>(cur_video.received), static_cast<unsigned long long>(cur_video.decoded),
		           static_cast<unsigned long long>(cur_video.handed_to_obs),
		           static_cast<unsigned long long>(cur_video.dropped),
		           static_cast<unsigned long long>(no_route_video.dropped));
		diag::logf(diag::LineKind::Stats, LOG_DEBUG,
		           "Stats audio received_hz=%.1f handed_to_obs_hz=%.1f dropped_hz=%.1f "
		           "received=%llu handed_to_obs=%llu dropped=%llu no_route_dropped=%llu",
		           ra.received_hz, ra.handed_to_obs_hz, ra.dropped_hz, static_cast<unsigned long long>(cur_audio.received),
		           static_cast<unsigned long long>(cur_audio.handed_to_obs),
		           static_cast<unsigned long long>(cur_audio.dropped),
		           static_cast<unsigned long long>(no_route_audio.dropped));
		// ストリームごとの整列 (TimestampAligner) と、音声と映像のずらし量 (AvOffsetTranscriber)。原因を
		// 調べるためだけの値で、判定には使わない (OBS の出力のタイムスタンプと、渡したタイムスタンプの差を、
		// 実機テストのログで読む)。realign=、slew_capped=、excuse_capped= は、整列のやり直しが起きたかと、
		// 調整の量が上限で止まっていたかを、ログから読むためのものである。キーの名前は、ほかのキー (relock= や
		// jump= など) で終わらない形にしてある (jump=(\d+) のような正規表現が、新しいキーの値を拾わないように)。
		{
			std::lock_guard<std::mutex> lock(mu);
			for (auto &kv : screen_streams) {
				auto &st = *kv.second;
				const auto &as = st.aligner.stats();
				const auto &vs = st.av_offset.stats();
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats stream=%u align period_us=%.1f frames=%lld realign=%lld relock=%lld jump=%lld "
				           "wobble=%lld backward=%lld mono_fix=%lld slew_capped=%lld excuse_capped=%lld "
				           "offset_us=%lld err_abs_max_us=%.0f av video_offset_us=%lld "
				           "offset_changes=%lld audio=%lld passthrough=%lld",
				           static_cast<unsigned>(kv.first), as.period_us, static_cast<long long>(as.frames),
				           static_cast<long long>(as.realign_count),
				           static_cast<long long>(as.relock_count), static_cast<long long>(as.jump_count),
				           static_cast<long long>(as.jump_wobble_count), static_cast<long long>(as.backward_count),
				           static_cast<long long>(as.monotonic_fix_count),
				           static_cast<long long>(as.slew_capped_frames),
				           static_cast<long long>(as.jump_excuse_capped_frames),
				           static_cast<long long>(as.last_offset_us),
				           as.err_abs_max_us, static_cast<long long>(vs.video_offset_us),
				           static_cast<long long>(vs.offset_changes), static_cast<long long>(vs.audio_frames),
				           static_cast<long long>(vs.passthrough_audio_frames));
			}
		}

		// ストリームごとの遅れ (受信側の OBS の時計 - 送信側のタイムスタンプを OBS の時計に換算した値)。原因を
		// 調べるためだけの値で、判定には使わない。
		// recv_ は WebSocket で受け取った時、decoded_ はデコードし終えた時、handed_ は OBS に渡す直前で、
		// 平均 (avg_ms)、95 パーセンタイルの近似 (p95_ms)、その周期に足した数 (n) を書く。p95 は 1 ms 刻みの上端
		// なので、実際の値より最大 1 ms 大きい。音声の handed_ は、AudioMixer に置いた時である。映像の
		// shown_ は、OBS が表示に選んだフレーム (替わったときだけ数える) の遅れで、基準は handed_ と同じ送信側のタイムスタンプである
		// (screen_feed.h の表示の遅れの測定)。
		// 1 つも足されなかったストリームの行は書かない。
		{
			std::lock_guard<std::mutex> lock(mu);
			for (auto &kv : screen_streams) {
				auto &lat = kv.second->latency;
				const LatencySummary r = lat.received.take();
				const LatencySummary d = lat.decoded.take();
				const LatencySummary h = lat.handed.take();
				// 枠の表示の遅れは、そのストリームが最後に渡した枠から取り出す (sources/ が持つロックは
				// 他のロックを取らないので、mu の中で取ってよい)。
				const LatencySummary sh = sources::take_screen_shown_latency(kv.second->latency_slot);
				if (r.count + d.count + h.count + sh.count == 0)
					continue;
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats stream=%u video_latency recv_avg_ms=%.1f recv_p95_ms=%.1f recv_n=%llu "
				           "decoded_avg_ms=%.1f decoded_p95_ms=%.1f decoded_n=%llu "
				           "handed_avg_ms=%.1f handed_p95_ms=%.1f handed_n=%llu "
				           "shown_avg_ms=%.1f shown_p95_ms=%.1f shown_n=%llu",
				           static_cast<unsigned>(kv.first), r.avg_ms, r.p95_ms,
				           static_cast<unsigned long long>(r.count), d.avg_ms, d.p95_ms,
				           static_cast<unsigned long long>(d.count), h.avg_ms, h.p95_ms,
				           static_cast<unsigned long long>(h.count), sh.avg_ms, sh.p95_ms,
				           static_cast<unsigned long long>(sh.count));
			}
			// 表示の緩衝 (frame_pacer.h) の、その周期の数。paced_tick_us は渡す時刻を合わせている OBS の描く刻みの
			// 間隔 (0 なら刻みに合わせず、タイムスタンプ + D の時刻に渡している)。paced_on_time は時刻まで待ってから
			// 渡した数、paced_late は届いた時点でもう時刻を過ぎていてすぐ渡した数 (D が受信までの遅れより短いと
			// 増える)、paced_dropped は渡さずに捨てた数 (同時に時刻が来た古いフレーム、上限超過、破棄)、
			// paced_queue_max は待ち行列の長さの最大である。キーの名前は、ほかの行のキー (dropped= など) で終わらない
			// 形にしてある。合成のソースがない (または D が 0 の) とき、その周期に何も起きなかったストリームの行は書かない。
			// 合成のソースがない間は待ち行列に積まない (frame_delivery_plan.h)。
			// D そのものの動作は、上の video_latency の shown_ (OBS が表示に選んだフレームの遅れ) で確認する。
			const bool composite_now = sources::composite_present();
			for (auto &kv : screen_streams) {
				FramePacerStats paced;
				std::int64_t delay_us = 0;
				std::int64_t tick_us = 0;
				{
					std::lock_guard<std::mutex> pacer_lock(pacer_mu);
					delay_us = display_buffer_us.load();
					const auto it = pacers.find(kv.first);
					if (it != pacers.end()) {
						paced = it->second.pacer.take_stats();
						tick_us = it->second.pacer.tick_interval_us();
					}
				}
				if (!(composite_now && delay_us > 0) && paced.on_time + paced.late + paced.dropped == 0)
					continue;
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats stream=%u video_pacer paced_delay_ms=%lld paced_tick_us=%lld paced_on_time=%llu "
				           "paced_late=%llu paced_dropped=%llu paced_queue_max=%zu",
				           static_cast<unsigned>(kv.first), static_cast<long long>(delay_us / 1000),
				           static_cast<long long>(tick_us),
				           static_cast<unsigned long long>(paced.on_time), static_cast<unsigned long long>(paced.late),
				           static_cast<unsigned long long>(paced.dropped), paced.max_queued);
			}
			// ストリームごとの、届くまでの遅れ (表示の緩衝の待ち行列に積むときの、OBS の時計 - 換算した送信側の
			// タイムスタンプ) の分布 (arrival_stats.h)。arrival_p50_ms、arrival_p95_ms、arrival_p99_ms、
			// arrival_max_ms は直近 arrival_window_s 秒 (最大 60) の分布で、arrival_n はそのフレームの数、arrival_window_late_pct はその期間で
			// 現在の D (paced_delay_ms) にしていたら間に合わなかったフレームの率 (%) である。arrival_late と
			// arrival_late_runs はこの周期の数で、間に合わなかったフレームの数と、続けて間に合わなかったフレームを 1 つと
			// 数えたまとまりの数 (見た目のカクつきの回数) である。arrival_late は paced_late と同じ数になる。合成のソースが
			// ないとき (待ち行列を通らないとき) は、行を書かない。
			const std::int64_t arrival_now_s = obs_now_us() / 1000000;
			for (auto &kv : screen_streams) {
				ArrivalSummary summary;
				ArrivalPeriod period;
				double late_pct = 0.0;
				{
					std::lock_guard<std::mutex> pacer_lock(pacer_mu);
					const auto it = pacers.find(kv.first);
					if (it == pacers.end())
						continue;
					summary = it->second.arrival.summary(arrival_now_s);
					period = it->second.arrival.take_period();
					late_pct = 100.0 * it->second.arrival.late_fraction_at(arrival_now_s,
											       display_buffer_us.load() / 1000);
				}
				if (summary.frames == 0 && period.frames == 0)
					continue;
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats stream=%u video_arrival arrival_window_s=%d arrival_n=%llu arrival_p50_ms=%.0f "
				           "arrival_p95_ms=%.0f arrival_p99_ms=%.0f arrival_max_ms=%.0f "
				           "arrival_window_late_pct=%.2f arrival_late=%llu arrival_late_runs=%llu",
				           static_cast<unsigned>(kv.first), summary.window_seconds,
				           static_cast<unsigned long long>(summary.frames), summary.p50_ms, summary.p95_ms,
				           summary.p99_ms, summary.max_ms, late_pct, static_cast<unsigned long long>(period.late),
				           static_cast<unsigned long long>(period.runs));
			}
			for (auto it = audio_latency.begin(); it != audio_latency.end();) {
				const LatencySummary r = it->second.received.take();
				const LatencySummary h = it->second.handed.take();
				if (r.count + h.count == 0) {
					it = audio_latency.erase(it);
					continue;
				}
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats stream=%u audio_latency recv_avg_ms=%.1f recv_p95_ms=%.1f recv_n=%llu "
				           "handed_avg_ms=%.1f handed_p95_ms=%.1f handed_n=%llu",
				           static_cast<unsigned>(it->first), r.avg_ms, r.p95_ms,
				           static_cast<unsigned long long>(r.count), h.avg_ms, h.p95_ms,
				           static_cast<unsigned long long>(h.count));
				++it;
			}
		}

		// 混ぜた音 (AudioMixer) のこの周期の数。mixer_blocks は出した区間の数 (10 ms ごと)、mixer_catch_up は取り出しが
		// 遅れて、区間の開始の時刻が今より前で出した数、mixer_skips と mixer_skipped_frames は遅れが限度を超えて
		// 飛ばした回数と長さ、mixer_tracks は今ある組の数である。組ごとの行 (mixer_track) は、周期の間に何かを
		// 数えた組だけで、人の ID は書かず、枠の番号で書く (枠にいない組は slot=0)。late_frames は出し終えた位置より
		// 遅れて届いて捨てた長さ、ahead_frames は先すぎて捨てた長さ、replaced は置き直した回数 (話し始めるたびに数えるので
		// 多くても不具合ではない)、placed_frames は置いた長さで、どれもフレーム数である。drift_ppm は推定した
		// 時計どうしの速さの差、max_error_ms は at_us と続きの位置の差の周期の最大である (align/audio_drift.h)。
		// 合成のソースがなく、何も数えなかった周期は書かない。mixer_min_ahead_ms は、取り出しのはじめに測った、今より先に渡してある長さの
		// 周期の最小 (ミリ秒、小数 1 桁。取り出しの起きる遅れの分だけ先渡しの幅より短くなる)、mixer_short_ahead は、
		// それが 0 を下回っていた (先に渡した分を使い切っていた) 取り出しの回数である。1 回でもあると、OBS が溜まりの
		// 先頭の時刻を付け直して、合成のソースの音が遅れたまま戻らないことがある (audio_mixer.h の kMixerLookaheadUs)。
		// 周期の間に 1 回も測らなかったとき (起点ができた直後など) は mixer_min_ahead_ms=none と書く。
		{
			AudioMixerStats mixer_stats;
			std::size_t mixer_tracks = 0;
			{
				std::lock_guard<std::mutex> mixer_lock(mixer_mu);
				mixer_stats = mixer.take_stats();
				mixer_tracks = mixer.track_count();
			}
			if (mixer_stats.blocks != 0 || !mixer_stats.keys.empty()) {
				char min_ahead[32] = "none";
				if (mixer_stats.min_ahead_us)
					std::snprintf(min_ahead, sizeof(min_ahead), "%.1f",
					              double(*mixer_stats.min_ahead_us) / 1000.0);
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats mixer mixer_blocks=%llu mixer_catch_up=%llu mixer_skips=%llu "
				           "mixer_skipped_frames=%llu mixer_tracks=%zu mixer_min_ahead_ms=%s "
				           "mixer_short_ahead=%llu",
				           static_cast<unsigned long long>(mixer_stats.blocks),
				           static_cast<unsigned long long>(mixer_stats.catch_up_blocks),
				           static_cast<unsigned long long>(mixer_stats.skips),
				           static_cast<unsigned long long>(mixer_stats.skipped_frames), mixer_tracks, min_ahead,
				           static_cast<unsigned long long>(mixer_stats.short_ahead_takes));
				std::lock_guard<std::mutex> lock(mu);
				for (const AudioMixerKeyStats &track : mixer_stats.keys) {
					const bool voice = track.key.kind == MixerKind::Voice;
					const int slot = table.slot_of_owner(
						voice ? config::SlotKind::Voice : config::SlotKind::Screen, track.key.user_id);
					diag::logf(diag::LineKind::Stats, LOG_DEBUG,
					           "Stats mixer_track %s slot=%d placed_frames=%llu late_frames=%llu "
					           "ahead_frames=%llu replaced=%llu drift_ppm=%.1f max_error_ms=%.2f",
					           voice ? "voice" : "screen_audio", slot,
					           static_cast<unsigned long long>(track.placed_frames),
					           static_cast<unsigned long long>(track.late_frames),
					           static_cast<unsigned long long>(track.ahead_frames),
					           static_cast<unsigned long long>(track.replaced), track.drift_ppm,
					           track.max_error_us / 1000.0);
				}
			}
		}

		// 合成のソースの名前ラベルと光の描き直し (描画のスレッドの外の専用のスレッドで行う) のこの周期の数。
		// decor_submitted は渡した仕事、decor_painted は描き終えた仕事、decor_dropped は描かずに捨てた仕事と、受け取らずに
		// 捨てた画像、decor_paint_max_ms は 1 回の描画 (QPainter) にかかった時間の最大である。テクスチャは描き直しの
		// スレッドがグラフィックスのコンテキストに入って作る (大きな画像は帯に分け、入るのは 1 フレームに 1 回まで)。
		// decor_upload_max_ms はコンテキストに 1 回 (帯 1 つ) 入っていた時間の最大 (描画のスレッドを 1 フレームに
		// 待たせうる時間の上限) である。decor_uploads は描画のスレッドが受け取って
		// 置き換えた仕事の数、decor_swap_max_ms はその 1 回にかかった時間の最大。何も数えなかった周期は書かない。
		if (sources::composite_present()) {
			const sources::CompositeDecorPaintStats decor = sources::take_composite_decor_paint_stats();
			if (decor.paint.submitted != 0 || decor.paint.finished != 0 || decor.paint.dropped != 0 || decor.uploads != 0) {
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats composite_decor decor_submitted=%llu decor_painted=%llu decor_dropped=%llu "
				           "decor_paint_max_ms=%.1f decor_uploads=%llu decor_upload_max_ms=%.2f decor_swap_max_ms=%.2f",
				           static_cast<unsigned long long>(decor.paint.submitted),
				           static_cast<unsigned long long>(decor.paint.finished),
				           static_cast<unsigned long long>(decor.paint.dropped),
				           double(decor.paint.max_paint_ns) / 1e6, static_cast<unsigned long long>(decor.uploads),
				           double(decor.max_upload_ns) / 1e6, double(decor.max_swap_ns) / 1e6);
			}
		}

		// 自分の枠の音声ソースの音から話している・話していないを決めているとき、しきい値を決め直す材料にする。
		// packets はこの周期に受け取ったパケットの数、self_peak_p50_db と self_peak_p95_db と self_peak_max_db はパケットの
		// peak (dBFS。1 dB 刻み) の分布、self_talking_pct は話していると判定したパケットの割合、self_threshold_db は
		// 話し始めのしきい値である。パケットを受け取らなかった周期は書かない。
		{
			const sources::SelfSpeakingStats self = sources::take_self_speaking_stats();
			if (self.summary.packets != 0) {
				diag::logf(diag::LineKind::Stats, LOG_DEBUG,
				           "Stats self_speaking_source packets=%llu self_peak_p50_db=%d self_peak_p95_db=%d "
				           "self_peak_max_db=%d self_talking_pct=%.0f self_threshold_db=%d",
				           static_cast<unsigned long long>(self.summary.packets), self.summary.p50_db,
				           self.summary.p95_db, self.summary.max_db, self.summary.talking_ratio * 100.0,
				           self.threshold_db);
			}
		}

		prev_video = cur_video;
		prev_audio = cur_audio;
		prev_us = now_us;
		control_lock.lock();
	}
}

// ---------------------------------------------------------------------------
// media と limits、合成のソースの有無
// ---------------------------------------------------------------------------

bool Pipeline::Impl::media_value_locked() const
{
	// 切り替えの最中は、最後に調べた値を使う (presence の宣言。調べ直すまで、送信側のゲートを閉じない)。
	return decide_media(media_inputs_of(feature_enabled, loading_finished, presence, recording_on.load()));
}

void Pipeline::Impl::refresh_media_locked(const char *why)
{
	const bool media = media_value_locked();
	if (media_sent.has_value() && *media_sent == media)
		return;
	media_sent = media;
	// 接続していなくても渡す。Listener は値を覚えて、次の hello_ok の初期値に載せる (ws/listener.h の send_media)。
	// 送るタイミングを決めるのはこちらである。
	listener.send_media(media);
	diag::logf(LOG_INFO,
		   "Video and voice from Vesktop turned %s (%s: feature=%s sources=%s recording=%s)",
		   on_off(media), why, on_off(feature_enabled), on_off(presence.has_value() && any_cordmux_source(*presence)),
		   on_off(recording_on.load()));
}

void Pipeline::Impl::refresh_limits_locked()
{
	if (limits_sent.has_value() && *limits_sent == limits)
		return;
	limits_sent = limits;
	listener.send_limits(limits);
	// 0 は、省いた (制限なし) ことを表す (limits_from_settings)。
	diag::logf(LOG_INFO, "Quality limits sent to Vesktop (max_long_edge=%u fps_divisor=%d encoder=%s latency=%s)",
		   static_cast<unsigned>(limits.max_long_edge.value_or(0)), limits.fps_divisor.value_or(1),
		   limits.encoder_preference.value_or("hardware").c_str(),
		   limits.latency_mode.value_or("realtime").c_str());
}

void Pipeline::Impl::resend_media_and_limits_locked()
{
	// 接続した直後。値が変わっていなくても送る。送信側は接続が切れると値を既定に戻す
	// (src-vencord/core/gate.ts) ので、接続のたびにそろえ直す。ログには書かない (値が変わったときの 1 行で足りる。
	// 再接続のたびに同じ行を書かない)。
	const bool media = media_value_locked();
	media_sent = media;
	listener.send_media(media);
	limits_sent = limits;
	listener.send_limits(limits);
}

void Pipeline::Impl::note_watch_since_locked(const ws::ReceiveScopeMessage &next, bool restart)
{
	// Everyone の間は持たない。Slots では、範囲に入った人にこの時刻を付け、外れた人を消す。
	if (!next.slots_only || restart)
		watching_since_us.clear();
	if (!next.slots_only)
		return;
	const std::int64_t now_us = wall_now_us();
	std::map<std::string, std::int64_t> updated;
	for (const std::string &user_id : next.user_ids) {
		const auto found = watching_since_us.find(user_id);
		updated[user_id] = found != watching_since_us.end() ? found->second : now_us;
	}
	watching_since_us = std::move(updated);
}

void Pipeline::Impl::refresh_watch_scope_locked(const char *why)
{
	const ws::ReceiveScopeMessage next =
		watch_scope_of(receive_scope, table, feature_enabled, assign_policy.focus_user_id);
	if (watch_sent.has_value() && *watch_sent == next)
		return;
	watch_sent = next;
	note_watch_since_locked(next, false);
	listener.send_receive_scope(next);
	diag::logf(LOG_INFO, "Receive scope sent to Vesktop (%s: scope=%s slots=%zu)", why,
		   next.slots_only ? "slots" : "everyone", next.user_ids.size());
}

void Pipeline::Impl::refresh_main_account_locked()
{
	const std::string &next = assign_policy.main_account_user_id;
	if (main_account_sent.has_value() && *main_account_sent == next)
		return;
	main_account_sent = next;
	listener.send_main_account(next);
}

void Pipeline::Impl::resend_scope_and_main_locked()
{
	// 接続した直後。値が変わっていなくても送る。送信側は、接続が切れても値を戻さないが、再起動した送信側は何も
	// 覚えていない。ログには書かない (値が変わったときの 1 行で足りる)。
	const ws::ReceiveScopeMessage scope =
		watch_scope_of(receive_scope, table, feature_enabled, assign_policy.focus_user_id);
	watch_sent = scope;
	// 送信側は再起動していることがあり、このときから視聴を始める。全員を今から数え直す。
	note_watch_since_locked(scope, true);
	listener.send_receive_scope(scope);
	main_account_sent = assign_policy.main_account_user_id;
	listener.send_main_account(assign_policy.main_account_user_id);
}

std::set<int> Pipeline::Impl::release_departed_locked(bool include_screen)
{
	std::set<int> screen_slots;
	std::vector<SlotToRelease> departed;
	if (include_screen)
		departed = screen_slots_to_release(table, facts);
	// 声のストリームが生きている人 (直前の configure に載っている人)。
	std::set<std::string> live_voice_users;
	if (last_configure) {
		for (const ws::StreamConfig &stream : last_configure->streams) {
			if (stream.kind == ws::StreamKind::Voice)
				live_voice_users.insert(stream.user_id);
		}
	}
	const std::vector<SlotToRelease> voices =
		voice_slots_to_release(table, facts, assign_policy.voice_links, live_voice_users);
	departed.insert(departed.end(), voices.begin(), voices.end());
	if (departed.empty())
		return screen_slots;
	for (const SlotToRelease &r : departed) {
		table.release(r.kind, r.slot);
		// 画面の枠の経路は消さず、枠 0 を指させる (release_slot の説明のとおり、フレームは届いて数えられるが OBS には
		// 渡らない)。声の枠は、ストリームが生きていない (空ける条件) ので、経路を触らない。
		if (r.kind == config::SlotKind::Screen) {
			router.retarget_user(r.owner, r.kind, 0);
			screen_slots.insert(r.slot);
		}
	}
	// 枠に載っている人が減ったので、ミキサーの組と、視聴する人の範囲と、録画の対象を決め直す。
	on_routes_changed_locked();
	refresh_watch_scope_locked("slot released");
	if (receive_scope == config::ReceiveScope::Slots)
		notify_recorder_formats_locked();
	diag::logf(LOG_INFO, "Released slots of people who stopped sharing or left the call: %zu", departed.size());
	return screen_slots;
}

void Pipeline::Impl::admit_sharers_from_facts_locked()
{
	if (receive_scope != config::ReceiveScope::Slots || !facts.sharing_valid)
		return;
	const std::vector<std::string> admitted = router.admit_sharers(facts.sharing.user_ids, table, assign_policy);
	if (!admitted.empty()) {
		on_routes_changed_locked();
		notify_recorder_formats_locked();
		diag::logf(LOG_INFO, "Screen slots filled from the sharing list: %zu", admitted.size());
	}
	refresh_watch_scope_locked("sharing");
}

bool Pipeline::Impl::record_allowed_locked(std::uint32_t stream_id) const
{
	if (receive_scope != config::ReceiveScope::Slots)
		return true;
	const auto it = stream_formats.find(stream_id);
	if (it == stream_formats.end())
		return true;
	return record_stream_allowed(receive_scope, table, it->second.screen, it->second.user_id,
				     assign_policy.focus_user_id);
}

void Pipeline::Impl::apply_layout_presence_locked(const LayoutPresence &read)
{
	// 有無が変わったときだけログに 1 行書く (1 秒ごとの監視で同じ行を書かない)。
	const bool changed = !presence_current || !presence || presence->composite != read.composite;
	presence = read;
	presence_current = true;
	if (changed)
		diag::logf(LOG_INFO, "The cordmux source is %s", read.composite ? "present" : "absent");
}

void Pipeline::Impl::refresh_layout_presence(const char *why)
{
	// アンインストールの最中は調べない (アンインストールは待ち受けを閉じた後にソースを削除するので、送る
	// 相手がもういない)。停止の最中と、OBS を閉じ始めた後 (on_frontend_exit) も調べない。
	if (stopping.load() || frontend_exiting.load() || install::is_uninstalling())
		return;
	std::uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(mu);
		if (!loading_finished || table.is_frozen())
			return;
		epoch = presence_epoch;
	}
	// mu の外で調べる (フレームの受信の関数を待たせない)。
	const LayoutPresence read = read_layout_presence();

	std::optional<ws::ConfigureMessage> replay;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(mu);
		// 調べている間に切り替えが始まったか終わったときは、その値を使わない (presence_epoch の宣言)。
		if (stopping.load() || frontend_exiting.load() || !loading_finished || table.is_frozen() ||
		    epoch != presence_epoch)
			return;
		apply_layout_presence_locked(read);
		refresh_media_locked(why);
		// 合成のソースの有無が、最後に configure を適用したときと変わった。メインアカウントの声の経路を作るかが
		// 変わるので、最後の configure を適用し直す (AssignPolicy::route_main_account_voice)。メインアカウントが
		// 決まっていなければ、経路は変わらないので適用し直さない。
		if (read.composite != configure_composite && !assign_policy.main_account_user_id.empty() &&
		    last_configure) {
			configure_composite = read.composite;
			replay = last_configure;
			generation = configure_generation;
		}
	}
	if (replay) {
		diag::logf(LOG_INFO, "Reapplying the stream list from Vesktop because the cordmux source %s",
		           read.composite ? "was added" : "was removed");
		handle_configure(*replay, generation);
	}
}

void Pipeline::Impl::presence_loop()
{
	std::unique_lock<std::mutex> control_lock(presence_control_mu);
	for (;;) {
		const bool woke_to_stop = presence_control_cv.wait_for(control_lock, kLayoutPresenceInterval,
									[this] { return presence_stop_requested; });
		if (woke_to_stop)
			return;
		control_lock.unlock();
		safe_call("the layout check", [&] { refresh_layout_presence("periodic check"); });
		safe_call("the video stall check", [&] { check_receive_restart(); });
		control_lock.lock();
	}
}

// ---------------------------------------------------------------------------
// 受信のやり直し (pipeline/receive_restart.h)
// ---------------------------------------------------------------------------

namespace {

// 送信側から届いた文字列 (stream_check_result の outcome と detail) を、ログの行に書ける形にする。英数字と記号の一部
// だけを残し、長さを抑える (送信側の不具合で、ログの行が壊れたり長くなったりしないため)。
std::string printable_short(const std::string &text, std::size_t max_len)
{
	std::string out;
	for (const char c : text) {
		if (out.size() >= max_len)
			break;
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
				c == '=' || c == '+' || c == '-' || c == '.' || c == ',' || c == ':' || c == ' ';
		out.push_back(ok ? c : '?');
	}
	return out;
}

} // namespace

void Pipeline::Impl::check_receive_restart()
{
	struct Ask {
		std::uint32_t stream_id = 0;
		int slot = 0;
		double stopped_s = 0.0;
		int check_number = 0;
	};
	struct Back {
		int slot = 0;
		double stopped_s = 0.0;
		double since_first_s = 0.0;
		int checks = 0;
	};
	struct Redo {
		std::uint32_t stream_id = 0;
		int slot = 0;
		double stalled_s = 0.0;
		int check_number = 0;
	};
	struct DecodeBack {
		std::uint32_t stream_id = 0;
		int slot = 0;
		double stopped_s = 0.0;
		double since_first_s = 0.0;
		int checks = 0;
	};
	std::vector<Ask> asks;
	std::vector<Back> backs;
	std::vector<Redo> redos;
	std::vector<DecodeBack> decode_backs;
	{
		std::lock_guard<std::mutex> lock(mu);
		if (stopping.load())
			return;
		ReceiveRestartInput in;
		in.now_us = steady_now_us();
		in.wait_us = receive_restart_wait_us;
		in.connected = linked;
		in.feature_enabled = feature_enabled;
		in.dummy = sources::dummy_mode_active();
		std::map<std::uint32_t, int> slot_of_stream;
		for (const auto &[stream_id, format] : stream_formats) {
			if (!format.screen)
				continue;
			ReceiveRestartStream s;
			s.stream_id = stream_id;
			s.user_id = format.user_id;
			const auto route = router.route(stream_id);
			// 全画面だけで映す人の経路 (kFocusOnlySlot) は枠に載せていないので含めない (枠 1 から 4 だけを見る)。
			s.in_slot = route && route->kind == config::SlotKind::Screen && route->slot >= 1 &&
				    route->slot != config::kFocusOnlySlot && router.video_allowed(stream_id, table);
			if (route)
				slot_of_stream[stream_id] = route->slot;
			const auto clock = last_frames.find(stream_id);
			if (clock != last_frames.end() && clock->second.last_video_frame_steady_us != 0)
				s.last_frame_us = clock->second.last_video_frame_steady_us;
			in.streams.push_back(std::move(s));
		}
		const ReceiveRestartPlan plan = plan_receive_restart(receive_restart_state, in);
		receive_restart_state = plan.next;
		for (const ReceiveRestartRequest &req : plan.requests) {
			for (const std::uint32_t stream_id : req.stream_ids) {
				Ask a;
				a.stream_id = stream_id;
				a.slot = slot_of_stream.count(stream_id) != 0 ? slot_of_stream[stream_id] : 0;
				a.stopped_s = double(req.stopped_for_us) / 1e6;
				a.check_number = req.check_number;
				asks.push_back(a);
				// 新しく止まった (1 回目の頼み) なら、前の答えを忘れる (答えを、止まるたびに 1 回はログに書く)。
				if (req.check_number == 1)
					stream_check_outcomes.erase(stream_id);
			}
		}
		for (const ReceiveRestartRecovery &r : plan.recoveries) {
			Back b;
			b.slot = table.slot_of_owner(config::SlotKind::Screen, r.user_id);
			b.stopped_s = double(r.stopped_for_us) / 1e6;
			b.since_first_s = double(r.since_first_check_us) / 1e6;
			b.checks = r.checks;
			backs.push_back(b);
		}

		// デコードの経路が黙って止まったか (受信側だけで閉じる判定。pipeline/receive_restart.h の plan_decode_restart)。
		DecodeRestartInput din;
		din.now_us = in.now_us;
		din.wait_us = in.wait_us;
		din.connected = in.connected;
		din.feature_enabled = in.feature_enabled;
		din.dummy = in.dummy;
		for (const ReceiveRestartStream &rs : in.streams) {
			const auto stream = screen_streams.find(rs.stream_id);
			if (stream == screen_streams.end())
				continue; // デコードの経路がない (経路がない、作っている途中)
			DecodeRestartStream ds;
			ds.stream_id = rs.stream_id;
			ds.in_slot = rs.in_slot;
			ds.last_arrival_us = rs.last_frame_us;
			if (stream->second->first_undecoded_arrival_us != 0)
				ds.first_undecoded_arrival_us = stream->second->first_undecoded_arrival_us;
			if (stream->second->last_decoded_us != 0)
				ds.last_decoded_us = stream->second->last_decoded_us;
			din.streams.push_back(ds);
		}
		const DecodeRestartPlan dplan = plan_decode_restart(decode_restart_state, din);
		decode_restart_state = dplan.next;
		for (const DecodeRestartRequest &req : dplan.requests) {
			Redo r;
			r.stream_id = req.stream_id;
			r.slot = slot_of_stream.count(req.stream_id) != 0 ? slot_of_stream[req.stream_id] : 0;
			r.stalled_s = double(req.stalled_for_us) / 1e6;
			r.check_number = req.check_number;
			redos.push_back(r);
			// 状態タブの履歴に「受信をやり直しました」を出す (送信側が始め直したときと同じ数え方)。
			const auto format = stream_formats.find(req.stream_id);
			if (format != stream_formats.end() && !format->second.user_id.empty())
				++receive_restarts[format->second.user_id];
		}
		for (const DecodeRestartRecovery &r : dplan.recoveries) {
			DecodeBack b;
			b.stream_id = r.stream_id;
			b.slot = slot_of_stream.count(r.stream_id) != 0 ? slot_of_stream[r.stream_id] : 0;
			b.stopped_s = double(r.stopped_for_us) / 1e6;
			b.since_first_s = double(r.since_first_check_us) / 1e6;
			b.checks = r.checks;
			decode_backs.push_back(b);
		}
	}

	// ログには人の名前と userId を書かず、枠の番号とストリームの番号だけを書く (ほかのログの行と同じ)。止まった後の 1 回目
	// だけを LOG_WARNING にし、頼み直しは LOG_INFO にする。静止した画面の共有では、届かない間 30 秒ごとに頼み直すので、
	// 全部を LOG_WARNING にすると、利用者の OBS のログが警告で埋まる。
	for (const Ask &a : asks) {
		diag::logf(a.check_number <= 1 ? LOG_WARNING : LOG_INFO,
			   "Asked Vesktop to restart receiving the screen share in slot %d: stream=%u no_video_s=%.1f request=%d",
			   a.slot, static_cast<unsigned>(a.stream_id), a.stopped_s, a.check_number);
		// デコードの経路は作り直さない (フレームが届いていないので、デコーダーには何も来ていない。
		// pipeline/receive_restart.h の先頭のコメント)。
		listener.send_stream_check(a.stream_id);
	}
	for (const Back &b : backs) {
		diag::logf(LOG_INFO,
			   "Video is arriving again in slot %d after restarting receiving: stopped_s=%.1f "
			   "after_first_request_s=%.1f requests=%d",
			   b.slot, b.stopped_s, b.since_first_s, b.checks);
	}
	// 頼み直しの 1 回目だけを LOG_WARNING にする理由は、上の頼みの行と同じである。
	for (const Redo &r : redos) {
		diag::logf(r.check_number <= 1 ? LOG_WARNING : LOG_INFO,
			   "Restarted decoding the screen share in slot %d: stream=%u frames_arriving_without_output_s=%.1f "
			   "restart=%d",
			   r.slot, static_cast<unsigned>(r.stream_id), r.stalled_s, r.check_number);
		rebuild_screen_decode(r.stream_id);
	}
	for (const DecodeBack &b : decode_backs) {
		diag::logf(LOG_INFO,
			   "Decoded video is back in slot %d after restarting decoding: stream=%u stopped_s=%.1f "
			   "after_first_restart_s=%.1f restarts=%d",
			   b.slot, static_cast<unsigned>(b.stream_id), b.stopped_s, b.since_first_s, b.checks);
	}
}

void Pipeline::Impl::rebuild_screen_decode(std::uint32_t stream_id)
{
	// 作り直すのはデコードの経路だけで、枠の映像 (合成のソースの子) は空けない。子の大きさと最後のフレームを保つので、
	// 新しいキーフレームが届くまで、枠には止まった映像と名前ラベルが残る (sources/screen_end.h の ReceiveRestarted)。
	// 経路と枠の割り当ても変えない。
	const sources::ScreenEndAction action = sources::screen_end_action(sources::ScreenEndEvent::ReceiveRestarted);
	int slot = 0;
	{
		std::lock_guard<std::mutex> lock(mu);
		const auto route = router.route(stream_id);
		if (!route || route->kind != config::SlotKind::Screen)
			return; // もう画面の経路がない (configure が消した)。作り直すものがない。
		slot = route->slot;
	}
	remove_screen_stream(stream_id); // 中でデコードのスレッドを join し、表示の緩衝の待ち行列を捨てる
	ensure_screen_stream(stream_id); // 経路がなくなっていれば作らない
	if (slot >= 1 && action.clear_now)
		sources::clear_screen_now(slot);
	if (slot >= 1 && action.clear_owner)
		sources::set_decor_owner(slot, std::string());
	if (action.relayout)
		notify_layout_changed();
	// 新しいデコーダーはキーフレームから始めるので、待たずに 1 通求める (次の一定の間隔のキーフレームを待たない)。
	listener.request_keyframe(stream_id);
}

void Pipeline::Impl::handle_stream_check_result(const ws::StreamCheckResultMessage &msg)
{
	bool write = false;
	int slot = 0;
	{
		std::lock_guard<std::mutex> lock(mu);
		std::string &last = stream_check_outcomes[msg.stream_id];
		write = last != msg.outcome;
		last = msg.outcome;
		const auto route = router.route(msg.stream_id);
		slot = route ? route->slot : 0;
		// 送信側が実際に始め直した (同じトラックで始め直した、または新しい接続の映像に切り替えた) 回数を、参加者ごとに数える。
		const auto format = stream_formats.find(msg.stream_id);
		if ((msg.outcome == "restarted" || msg.outcome == "superseded") && format != stream_formats.end() &&
		    !format->second.user_id.empty())
			++receive_restarts[format->second.user_id];
	}
	// 同じストリームで同じ答えが続く間 (静止した画面の共有で、30 秒ごとに not_receiving が返る間など) は書かない。
	if (!write)
		return;
	const std::string outcome = printable_short(msg.outcome, 32);
	const std::string detail = printable_short(msg.detail, 160);
	diag::logf(LOG_INFO, "Vesktop answered the restart request for slot %d: stream=%u outcome=%s %s", slot,
		   static_cast<unsigned>(msg.stream_id), outcome.c_str(), detail.c_str());
}

// ---------------------------------------------------------------------------
// Pipeline (公開 API)
// ---------------------------------------------------------------------------

Pipeline::Pipeline(ws::Listener &listener, config::AllocationTable &table, const config::Settings &settings)
	: impl_(std::make_unique<Impl>(listener, table, settings))
{
}

Pipeline::~Pipeline()
{
	stop();
}

void Pipeline::set_js_log_sink(JsLogGuard sink)
{
	impl_->js_log_sink = sink ? std::move(sink) : JsLogGuard([](std::string_view, JsLineKind) { return false; });
}

void Pipeline::push_plugin_log(std::string_view line, diag::LineKind kind)
{
	// チェックはここにはない (plugin-main.cpp のログの出力先が、渡す前に通している。pipeline.h の宣言)。
	// 呼び出し元は、その出力先だけである (diag/diag_log.h)。専用のロックを一瞬だけ取る。
	//
	// 行の先頭にローカル時刻「HH:MM:SS.mmm: 」を付ける (形と理由は log_ring.h の log_clock_prefix)。時計を読むのは
	// ロックの外 (ロックを持つ時間を延ばさない)。時刻はチェックの後に付く (チェックが見るのは元の行である。付ける
	// 時刻は数字と記号だけでトークンにならず、5 桁以上続く数字を含まないので、ポート番号と取り違えない)。
	const auto now = std::chrono::system_clock::now();
	const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
	std::tm local{};
	const bool have_local = localtime_s(&local, &now_t) == 0;
	const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	std::string stamped = have_local ? log_clock_prefix(local.tm_hour, local.tm_min, local.tm_sec, ms)
					 : std::string("--:--:--.---: ");
	stamped.append(line.data(), line.size());
	std::lock_guard<std::mutex> lock(impl_->plugin_log_mu);
	if (kind == diag::LineKind::Stats)
		impl_->plugin_stats_log.push(std::move(stamped));
	else
		impl_->plugin_log.push(std::move(stamped));
}

void Pipeline::note_dummy_mode(bool on)
{
	// 表示が実際に切り替わったときだけ、時間帯を開くか閉じる。
	bool changed = false;
	std::size_t count = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		changed = note_dummy_interval(&impl_->dummy_intervals, on, wall_now_us());
		count = impl_->dummy_intervals.size();
		// 録画ログの session の行に書く (録画した映像がダミーだったかを、後から区別できるようにする)。
		if (changed)
			impl_->notify_recorder_session_facts_locked();
	}
	if (!changed)
		return;

	// ON と OFF のたびに、cordmux.dll のログにも 1 行書く (時間帯の一覧とログの両方に残す。ログしかないときでも、
	// ダミーだったかを言えるようにするため)。mu の外で書く (handle_facts と同じく、ほかの受信の関数を待たせない)。
	//
	// リングバッファーに直接積まない。ここもほかの行と同じく diag/ の関数を通す。直接積むと、リングバッファーに書く
	// 所が 2 つになり、トークンのチェックを通ったかどうかが、呼び出し元の書き方だけで決まってしまう。
	diag::logf(LOG_DEBUG, "Placeholder interval %zu %s", count, on ? "started" : "ended");
}

void Pipeline::start()
{
	impl_->listener.on_video_frame = [this](const ws::FrameHeader &h, const std::uint8_t *p, std::size_t l) {
		impl_->safe_call("a video frame", [&] { impl_->handle_video_frame(h, p, l); });
	};
	impl_->listener.on_audio_frame = [this](const ws::FrameHeader &h, const std::uint8_t *p, std::size_t l) {
		impl_->safe_call("an audio frame", [&] { impl_->handle_audio_frame(h, p, l); });
	};
	impl_->listener.on_configure = [this](const ws::ConfigureMessage &m) {
		impl_->safe_call("the stream list from Vesktop", [&] { impl_->handle_configure(m); });
	};
	impl_->listener.on_log = [this](const ws::LogMessage &m) {
		impl_->safe_call("a log message from Vesktop", [&] { impl_->handle_log(m); });
	};
	impl_->listener.on_facts = [this](const ws::FactsMessage &m) {
		impl_->safe_call("facts from Vesktop", [&] { impl_->handle_facts(m); });
	};
	impl_->listener.on_profiles = [this](const ws::ProfilesMessage &m) {
		impl_->safe_call("profiles from Vesktop", [&] { impl_->handle_profiles(m); });
	};
	impl_->listener.on_avatar = [this](const ws::AvatarMessage &m) {
		impl_->safe_call("an avatar from Vesktop", [&] { impl_->handle_avatar(m); });
	};
	impl_->listener.on_stream_check_result = [this](const ws::StreamCheckResultMessage &m) {
		impl_->safe_call("a restart answer from Vesktop", [&] { impl_->handle_stream_check_result(m); });
	};
	impl_->listener.on_connected = [this] { impl_->safe_call("the connection to Vesktop",
								 [&] { impl_->handle_connected(); }); };
	impl_->listener.on_disconnected = [this] {
		impl_->safe_call("the disconnection from Vesktop", [&] { impl_->handle_disconnected(); });
	};
	impl_->listener.receive_clock_us = [] { return obs_now_us(); };
	impl_->listener.on_sender_clock = [this](std::int64_t sender_us, std::int64_t received_us) {
		impl_->safe_call("a clock reading from Vesktop", [&] { impl_->handle_sender_clock(sender_us, received_us); });
	};

	impl_->stats_stop_requested = false;
	impl_->stats_thread = std::thread([this] { impl_->stats_loop(); });

	// media と limits の初期値を、ここで 1 回渡す。plugin-main.cpp は start() の後に try_open() するので、渡した
	// 値は Listener が覚えて、最初の hello_ok に載せる (送信は io_thread が動き出してから順に行う)。
	// シーンコレクションの読み込みの前なので、cordmux が有効でも media は false になる (decide_media)。
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->refresh_media_locked("startup");
		impl_->refresh_limits_locked();
	}

	// 合成のソースの有無の監視 (イベントのない変化でも、media の値が変わったことを見つける)。
	{
		std::lock_guard<std::mutex> lock(impl_->presence_control_mu);
		impl_->presence_stop_requested = false;
	}
	impl_->presence_thread = std::thread([this] { impl_->presence_loop(); });

	// 表示の緩衝の待ち行列から渡すスレッド。緩衝が 0 の間も起動しておく (動いている間に set_display_buffer_ms で
	// 変わる。待ち行列が空の間は、積まれるまで寝ているだけで何も使わない)。
	{
		std::lock_guard<std::mutex> lock(impl_->pacer_mu);
		impl_->pacer_stop_requested = false;
	}
	impl_->pacer_thread = std::thread([this] { impl_->frame_pacer_loop(); });

	// 混ぜた音を取り出して合成のソースに渡すスレッド。合成のソースがない間は、10 ms ごとに起きて有無を見るだけで何も
	// しない。起点を忘れてから始める (停止の後の再開で、前の起点の続きから出さない)。
	{
		std::lock_guard<std::mutex> lock(impl_->mixer_mu);
		impl_->mixer_stop_requested = false;
		impl_->mixer.reset();
		impl_->voice_activity.reset();
	}
	sources::clear_voice_speaking();
	impl_->publish_speaking_users();
	impl_->mixer_thread = std::thread([this] { impl_->mixer_loop(); });
}

// obs_module_unload は Pipeline::stop() の後に Listener::stop() を呼ぶ (plugin-main.cpp) ので、stop() が戻った直後も
// listener の io_thread が動いていて、on_video_frame などが呼ばれることがある。stop() は listener.on_* を触らない
// (nullptr に戻さない)。stop() の後に届いたコールバックは、mu で守った内部の状態 (screen_streams が空で、router に
// 経路がない。stopping も true) を見て、渡す先のないメッセージとして dropped に数えるだけの普通の処理を通るので、
// クラッシュもデータ競合も起きない (handle_video_frame、handle_audio_frame、handle_configure のどれも、この状態では
// 新しいスレッドを作らずに終わる)。Pipeline のデストラクターより前に listener.stop() が終わっていれば
// (plugin-main.cpp)、その後はコールバックは呼ばれない。
void Pipeline::stop()
{
	// stopping を最初に立てる (これ以降、ensure_screen_stream は新しいデコードのスレッドを起動しない)。
	// listener.stop() はまだ呼ばれていないことがあるので、この後も on_video_frame などが呼ばれることがあるが、上の
	// コメントのとおり安全に終わる。
	impl_->stopping.store(true);

	// デコードのスレッドの後始末より前に、録画を止めて join する。受信の関数はこのフラグ (recording_on) を見て
	// 録画に渡すので、ここで false にすれば、以後のメッセージは録画に行かない。待つ上限は settings の
	// record_finalize_wait_on_exit_s (既定 30 秒)。ここで明示的に呼ぶ。デストラクターに任せると stop(0) になり、
	// 録画ファイルの作成がすぐに打ち切られて pending になる (上限が動作しない)。上限で打ち切ったときに子プロセスを止め、
	// 作りかけの録画ファイルを削除し、録画ログを pending にするのは、Recorder の側の役目である。
	impl_->recording_on.store(false);
	if (impl_->recorder) {
		const int wait_s = impl_->settings.record_finalize_wait_on_exit_s;
		impl_->recorder->stop(wait_s > 0 ? wait_s * 1000 : 0);
	}

	// 保存先の候補への問い合わせがタイムアウトしていたら、ここで待つ (bounded_call.h。待つのをやめたスレッドを、
	// プラグインがアンロードされた後まで動かしたままにしない)。普通は保持しているスレッドがないので、何も待たない。
	impl_->base_dir_probe.call.join_pending();

	// 段階ごとの計数を書く定期のスレッドを、screen_streams の後始末より前に止めて join する (後始末の途中に
	// stats_loop が counters と mu を触ることがないように)。
	{
		std::lock_guard<std::mutex> lock(impl_->stats_control_mu);
		impl_->stats_stop_requested = true;
	}
	impl_->stats_control_cv.notify_all();
	if (impl_->stats_thread.joinable())
		impl_->stats_thread.join();

	// 合成のソースの有無の監視も、同じ理由で先に止めて join する (mu を取るため)。
	{
		std::lock_guard<std::mutex> lock(impl_->presence_control_mu);
		impl_->presence_stop_requested = true;
	}
	impl_->presence_control_cv.notify_all();
	if (impl_->presence_thread.joinable())
		impl_->presence_thread.join();

	// 表示の緩衝の待ち行列から渡すスレッドも、同じ理由で止めて join する (渡すときに mu を取る)。
	{
		std::lock_guard<std::mutex> lock(impl_->pacer_mu);
		impl_->pacer_stop_requested = true;
	}
	impl_->pacer_cv.notify_all();
	if (impl_->pacer_thread.joinable())
		impl_->pacer_thread.join();
	// 混ぜた音を取り出すスレッドも止めて join する (合成のソースに渡している最中なら、終わるのを待つ)。止めた後に
	// 起点と保持している音を捨てる。組の名簿は、下の router.clear() の後で空にする。
	{
		std::lock_guard<std::mutex> lock(impl_->mixer_mu);
		impl_->mixer_stop_requested = true;
	}
	impl_->mixer_cv.notify_all();
	if (impl_->mixer_thread.joinable())
		impl_->mixer_thread.join();
	{
		std::lock_guard<std::mutex> lock(impl_->mixer_mu);
		impl_->mixer.reset();
		impl_->voice_activity.reset();
	}
	sources::clear_voice_speaking();
	impl_->publish_speaking_users();

	std::map<std::uint32_t, std::unique_ptr<Impl::ScreenStream>> victims;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		for (auto &[id, s] : impl_->screen_streams) {
			(void)id;
			s->stop_requested = true;
			s->queue_cv.notify_all();
		}
		victims = std::move(impl_->screen_streams);
		impl_->screen_streams.clear();
	}
	for (auto &[id, s] : victims) {
		(void)id;
		if (s->worker.joinable())
			s->worker.join();
	}
	// 表示の緩衝の待ち行列に残ったフレームは、渡すスレッドを止めてあるので OBS に渡さずに捨てて数える。デコードの
	// スレッドが止まった後に捨てるので、この後に積まれるフレームはない。
	impl_->discard_all_pacers();

	std::lock_guard<std::mutex> lock(impl_->mu);
	impl_->router.clear();
	impl_->on_routes_changed_locked();
	impl_->pending_configure.reset();
	impl_->last_configure.reset();
	++impl_->configure_generation;
}

PipelineStats Pipeline::stats() const
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	PipelineStats out;
	out.totals_video = impl_->counters.total_snapshot(config::MediaKind::Video);
	out.totals_audio = impl_->counters.total_snapshot(config::MediaKind::Audio);
	out.no_route_video = impl_->counters.no_route_snapshot(config::MediaKind::Video);
	out.no_route_audio = impl_->counters.no_route_snapshot(config::MediaKind::Audio);
	out.per_stream_video = impl_->counters.per_stream_snapshot(config::MediaKind::Video);
	out.per_stream_audio = impl_->counters.per_stream_snapshot(config::MediaKind::Audio);
	return out;
}

PipelineSnapshot Pipeline::snapshot() const
{
	// listener.stats() は mu の外で読む (2 つのロックを同時に持たない。Impl の mu の宣言)。Listener の stats() が
	// 取るのは、送信側のバージョンだけを守る、ほかのロックを取らないロックを一瞬だけなので、ここで
	// 待たされることはない (ws/listener.cpp で確認した)。
	PipelineSnapshot out;
	out.listener = impl_->listener.stats();
	out.exceptions_caught = impl_->exceptions_caught.load();
	out.settings = impl_->settings; // コンストラクターでコピーした値 (その後は変わらない)

	// 録画の状態と、録画に渡したメッセージの表。mu の外で読む (listener.stats() と同じく、2 つのロックを同時に
	// 持たない。中で取るのは record_mu だけで、そこから mu は取らない)。
	if (impl_->recorder) {
		out.recording = impl_->recorder->snapshot();
		out.record_only_frames = impl_->recorder->record_only_frames();
	}
	// 録画の ON と OFF の値は Pipeline のメモリが持つので、Recorder のコピーではなく、こちらを入れ直す (Recorder の
	// 側は停止の最中に自分で false にするので、一瞬だけ食い違うことがある)。
	out.recording.enabled = impl_->recording_on.load();
	{
		// 最後に決めた保存先 (決めるのは Pipeline だけ)。
		std::lock_guard<std::mutex> lock(impl_->record_place_mu);
		out.recording.resolved_base_dir = impl_->record_root_now;
	}

	{
		// cordmux.dll 自身のログのリングバッファーは、専用のロックで守っている (Impl::plugin_log の宣言)。mu を
		// 取る前にコピーする (2 つのロックを同時に持たない。listener.stats() と同じ扱い)。
		std::lock_guard<std::mutex> log_lock(impl_->plugin_log_mu);
		out.plugin_log = impl_->plugin_log.snapshot();
		out.plugin_log_dropped = impl_->plugin_log.dropped();
		out.plugin_stats_log = impl_->plugin_stats_log.snapshot();
		out.plugin_stats_log_dropped = impl_->plugin_stats_log.dropped();
	}

	std::lock_guard<std::mutex> lock(impl_->mu);
	out.taken_at_wall_us = wall_now_us();
	out.taken_at_steady_us = steady_now_us();

	// 割り当て表を読む (書かない)。画面 1 から 4、声 1 から 8 の順に、割り当てのある枠だけを並べる。並べ方は
	// pipeline.h の純粋関数 collect_slot_snapshots にある (単体テストから使える)。
	out.slots = collect_slot_snapshots(impl_->table);
	mark_share_present(out.slots, live_screen_slots(impl_->router));
	mark_voice_present(out.slots, live_voice_slots(impl_->router));
	// 映像を空けたかは、切れた時刻から link_lost_hold_ms がたったかで見る (sources::hold_then_clear_screen と同じ長さ)。
	const std::int64_t frame_hold_us = static_cast<std::int64_t>(impl_->link_lost_hold_ns.load() / 1000);
	mark_link_lost_hold(out.slots,
			    link_lost_layout_owners(impl_->link_lost_layout, out.taken_at_steady_us,
						    impl_->reconnect_layout_hold_us.load()),
			    out.taken_at_steady_us - impl_->link_lost_layout.lost_at_us >= frame_hold_us);
	out.frozen = impl_->table.is_frozen();

	out.facts = impl_->facts;
	out.watching_since_us = impl_->watching_since_us;
	// 送信側が返した、録画に関わる 2 つの値。値を持たないのは、まだ届いていないとき (facts が 1 通も来ていないか、
	// そのキーを送らない古い送信側。pipeline/facts_state.h)。
	out.sender_recording = impl_->facts.recording;
	out.sender_sps_pps_prepended = impl_->facts.sps_pps_prepended;
	// 送信側に渡した値 (pipeline.h の PipelineSnapshot)。
	out.media_sent = impl_->media_sent;
	out.limits_sent = impl_->limits_sent;
	out.profiles = impl_->profiles;
	out.stream_formats = impl_->stream_formats;
	if (impl_->arrival_watch.load()) {
		// 参加者ごとの届くまでの遅れ。画面のストリームを stream_formats の user_id で人に結ぶ。ロックの順は mu の後に
		// pacer_mu。時刻は note と同じ時計で読み、期限が切れたフレームを外してからコピーする。
		const std::int64_t now_s = obs_now_us() / 1000000;
		std::lock_guard<std::mutex> pacer_lock(impl_->pacer_mu);
		for (const auto &[stream_id, format] : impl_->stream_formats) {
			if (!format.screen || format.user_id.empty())
				continue;
			const auto it = impl_->pacers.find(stream_id);
			if (it == impl_->pacers.end())
				continue;
			ArrivalWindow window = it->second.arrival.window(now_s);
			if (!window.delays_ms.empty())
				out.arrival_by_user[format.user_id].push_back(std::move(window));
		}
	}
	{
		// 参加者ごとの音量とミュート。ロックの順は mu の後に mixer_mu。画面共有の音を持つ人は、configure に音声のある画面の
		// ストリームがあるかで決める。
		std::set<std::string> with_screen_audio;
		for (const auto &[stream_id, format] : impl_->stream_formats) {
			(void)stream_id;
			if (format.screen && format.audio && !format.user_id.empty())
				with_screen_audio.insert(format.user_id);
		}
		std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
		std::set<std::string> ids = impl_->mixer_levels.user_ids();
		ids.insert(with_screen_audio.begin(), with_screen_audio.end());
		for (const std::string &user_id : ids) {
			const MixerLevel voice = impl_->mixer_levels.get(MixerKey{user_id, MixerKind::Voice});
			const MixerLevel screen = impl_->mixer_levels.get(MixerKey{user_id, MixerKind::ScreenAudio});
			PersonAudioSnapshot person;
			person.voice_muted = voice.muted;
			person.voice_volume_pct = voice.volume_pct;
			person.screen_muted = screen.muted;
			person.screen_volume_pct = screen.volume_pct;
			person.has_screen_audio = with_screen_audio.count(user_id) != 0;
			out.person_audio.emplace(user_id, person);
		}
	}
	out.last_frames = impl_->last_frames;
	out.js_log = impl_->js_log.snapshot();
	out.js_log_dropped = impl_->js_log.dropped();
	out.js_stats_log = impl_->js_stats_log.snapshot();
	out.js_stats_log_dropped = impl_->js_stats_log.dropped();
	out.receive_restarts = impl_->receive_restarts;
	out.dummy_intervals = impl_->dummy_intervals;
	for (const auto &[stream_id, stream] : impl_->screen_streams) {
		(void)stream_id;
		if (stream->hw_fallback.load())
			out.hw_fallback_streams++;
	}

	out.stats.totals_video = impl_->counters.total_snapshot(config::MediaKind::Video);
	out.stats.totals_audio = impl_->counters.total_snapshot(config::MediaKind::Audio);
	out.stats.no_route_video = impl_->counters.no_route_snapshot(config::MediaKind::Video);
	out.stats.no_route_audio = impl_->counters.no_route_snapshot(config::MediaKind::Audio);
	out.stats.per_stream_video = impl_->counters.per_stream_snapshot(config::MediaKind::Video);
	out.stats.per_stream_audio = impl_->counters.per_stream_snapshot(config::MediaKind::Audio);
	return out;
}

AudioLevels Pipeline::audio_levels() const
{
	AudioLevels out;
	{
		std::lock_guard<std::mutex> lock(impl_->mixer_mu);
		out.tracks = impl_->mixer.levels();
	}
	std::vector<std::string> talking;
	{
		std::lock_guard<std::mutex> lock(impl_->speaking_mu);
		talking = impl_->speaking_users;
	}
	// 配信に出ていない声 (ミュートか音量 0。組がない場合も含む) の人は、話していても入れない。
	for (std::string &user_id : talking) {
		for (const AudioTrackLevel &track : out.tracks) {
			if (track.key.kind == MixerKind::Voice && track.key.user_id == user_id) {
				if (!track.silenced)
					out.speaking.push_back(std::move(user_id));
				break;
			}
		}
	}
	return out;
}

Pipeline::SlotChange Pipeline::assign_slot(config::SlotKind kind, const std::string &user_id)
{
	SlotChange out;
	if (user_id.empty())
		return out; // 相手が分からない (押せるかどうかは ledger::apply が先に見ている)

	std::optional<ws::ConfigureMessage> replay;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);

		// 表と経路を書き換える。画面の枠に新しく載せるのは、載せられる人数 (自分の枠を使っている間は 3 人) までである
		// (pipeline.h の宣言)。
		out = assign_slot_in(impl_->table, impl_->router, impl_->assign_policy, kind, user_id);
		if (out.outcome == SlotChange::Outcome::Insufficient)
			return out;
		// 付け替えた画面のストリームは、次のデコードできたフレームで、名前ラベルに描く人を結び付け直す。枠から外したとき
		// (DockPoll::after_slot_release) に描く人を外しているので、デコードのスレッドを作り直さない付け替えでは、
		// 結び付け直さないと、載せ直した人の名前ラベルが出ない。
		if (kind == config::SlotKind::Screen && out.retargeted > 0)
			impl_->rearm_decor_owner_locked(user_id);
		impl_->on_routes_changed_locked();
		// 枠に載った人が増えたので、視聴する人の範囲と録画の対象を決め直す (受け取る範囲が Slots のとき)。
		impl_->refresh_watch_scope_locked("slot assigned");
		if (impl_->receive_scope == config::ReceiveScope::Slots)
			impl_->notify_recorder_formats_locked();

		// 付け替えた経路が 1 つもなかったときは、直前の configure を適用し直す。手動では、枠に入れていない人の
		// 映像に経路を作らない (stream_router.h の AssignPolicy) ので、枠に載せた時点ではこの人の経路がまだ
		// ない。付け替える経路がないまま戻ると、次の configure が届くまで、その人の映像が 1 枚も出ない
		// (configure は参加者が変わるまで来ない)。自動でも同じで、枠が足りずに Insufficient になった人は経路を
		// 持たないので、誰かを外して空けた枠に入れても、次の configure まで映らない。
		if (impl_->loading_finished && out.retargeted == 0) {
			replay = impl_->last_configure;
			generation = impl_->configure_generation;
		}
	}

	if (replay)
		impl_->safe_call("the stream list after a slot change",
				 [&] { impl_->handle_configure(*replay, generation); });
	return out;
}

bool Pipeline::set_assign_policy(config::AssignMode mode, std::set<std::string> always_on,
				 std::map<std::string, std::string> voice_links, std::string main_account_user_id,
				 bool self_frame_selected, bool self_frame_in_use)
{
	std::optional<ws::ConfigureMessage> replay;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		const bool automatic = mode == config::AssignMode::Auto;
		// 自分の枠を使い始めることは、割り当て表と同じこのロックの中で受け付けるか断る (pipeline.h の宣言)。断ったら、
		// 使っていないことにして、ほかの値と同じく比べる (ほかに変わった値がなければ何もしない)。
		self_frame_in_use =
			accepted_self_frame_in_use(impl_->table, impl_->assign_policy.self_frame_in_use, self_frame_in_use,
						   self_frame_owner_of(main_account_user_id, impl_->facts));
		// 値が変わっていなければ何もしない。ドックはポーリングのたびにコピーを渡すので、ここで比べないと、
		// ドックの周期ごとに configure を適用し直すことになる。使う声も比べる (比べないと、変えた瞬間に
		// 適用し直さず、次の configure まで声が変わらない)。
		if (impl_->assign_policy.automatic == automatic && impl_->assign_policy.always_on == always_on &&
		    impl_->assign_policy.voice_links == voice_links &&
		    impl_->assign_policy.main_account_user_id == main_account_user_id &&
		    impl_->assign_policy.self_frame_selected == self_frame_selected &&
		    impl_->assign_policy.self_frame_in_use == self_frame_in_use)
			return self_frame_in_use;
		if (impl_->assign_policy.self_frame_in_use != self_frame_in_use)
			log_self_frame_in_use(self_frame_in_use);
		// 自動から手動に戻しても、いま画面の枠に載っている人は外さない (手動の割り当ては、枠に載っている人の
		// 経路を保つ。stream_router.h の AssignPolicy の (1))。
		// 選び方を切り替えたら、外した人の一覧を捨てる (stream_router.h の exclude_user)。
		if (impl_->assign_policy.automatic != automatic)
			impl_->router.clear_excluded();
		impl_->assign_policy.automatic = automatic;
		impl_->assign_policy.always_on = std::move(always_on);
		impl_->assign_policy.voice_links = std::move(voice_links);
		impl_->assign_policy.main_account_user_id = std::move(main_account_user_id);
		impl_->assign_policy.self_frame_selected = self_frame_selected;
		impl_->assign_policy.self_frame_in_use = self_frame_in_use;
		impl_->refresh_main_account_locked();
		// 決め方が変わったので、受け取る範囲が Slots のときは、すでに画面共有している人を載せ直す。
		impl_->admit_sharers_from_facts_locked();
		{
			// ミキサーでの自分の声の既定のミュートと、話しているかの判定の仕方 (mixer_roster.h の MixerLevels)。
			// ロックの順は mu の後に mixer_mu。
			std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
			impl_->set_self_voice_user_locked(impl_->assign_policy.main_account_user_id);
		}
		// シーンコレクションの読み込みが終わる前は適用し直さない (保留した configure は、
		// on_frontend_finished_loading が新しい決め方で適用する)。
		if (impl_->loading_finished) {
			replay = impl_->last_configure;
			generation = impl_->configure_generation;
		}
	}

	if (replay)
		impl_->safe_call("the stream list after Slot Assignment changed",
				 [&] { impl_->handle_configure(*replay, generation); });
	return self_frame_in_use;
}

Pipeline::SelfFrameMove Pipeline::move_self_frame(const std::string &user_id, bool to_self_frame,
						  std::optional<std::int64_t> after_order)
{
	SelfFrameMove out;
	std::optional<ws::ConfigureMessage> replay;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		const bool in_use_before = impl_->assign_policy.self_frame_in_use;
		out = move_self_frame_in(impl_->table, impl_->router, impl_->assign_policy,
					 self_frame_owner_of(impl_->assign_policy.main_account_user_id, impl_->facts), user_id,
					 to_self_frame, after_order);
		if (impl_->assign_policy.self_frame_in_use != in_use_before)
			log_self_frame_in_use(impl_->assign_policy.self_frame_in_use);
		// 載せ直した人の名前ラベルを結び付け直す (assign_slot と同じ)。
		if (!to_self_frame && out.change.retargeted > 0)
			impl_->rearm_decor_owner_locked(user_id);
		// 枠の付け外しの後と、決め方が変わった後に行うこと (assign_slot、release_slot、set_assign_policy と同じ)。
		impl_->on_routes_changed_locked();
		impl_->admit_sharers_from_facts_locked();
		impl_->refresh_watch_scope_locked(to_self_frame ? "slot released" : "slot assigned");
		if (impl_->receive_scope == config::ReceiveScope::Slots)
			impl_->notify_recorder_formats_locked();
		// 自分の枠に映す映像を替えたので、直前の configure を適用し直す (メインアカウントの画面の経路を作るか消す。
		// set_assign_policy と同じく、シーンコレクションの読み込みが終わる前は適用し直さない)。
		if (impl_->loading_finished) {
			replay = impl_->last_configure;
			generation = impl_->configure_generation;
		}
	}

	if (replay)
		impl_->safe_call("the stream list after the self frame video changed",
				 [&] { impl_->handle_configure(*replay, generation); });
	return out;
}

void Pipeline::set_focus_target(const std::string &user_id)
{
	std::optional<ws::ConfigureMessage> replay;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		// 値が変わっていなければ何もしない (ドックは配置を決め直すたびに渡す)。
		if (impl_->assign_policy.focus_user_id == user_id)
			return;
		impl_->assign_policy.focus_user_id = user_id;
		// ログには人の ID を書かない。
		diag::logf(LOG_INFO, "%s", user_id.empty() ? "Full screen target cleared" : "Full screen target set");
		// 枠に載せていない人なら、ここから受信を始める (やめたら受信もやめる)。録画の対象も同じ。
		impl_->refresh_watch_scope_locked("full screen");
		if (impl_->receive_scope == config::ReceiveScope::Slots)
			impl_->notify_recorder_formats_locked();
		// すでに受信している人なら、直前の configure を適用し直して、すぐに経路を作る (やめたら経路を消す)。
		if (impl_->loading_finished) {
			replay = impl_->last_configure;
			generation = impl_->configure_generation;
		}
	}
	if (replay)
		impl_->safe_call("the stream list after the full screen target changed",
				 [&] { impl_->handle_configure(*replay, generation); });
}

void Pipeline::set_receive_scope(config::ReceiveScope scope)
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	// 値が変わっていなければ何もしない (ドックは周期 (既定 200 ms) ごとにコピーを渡す)。
	if (impl_->receive_scope == scope)
		return;
	impl_->receive_scope = scope;
	diag::logf(LOG_INFO, "Receive scope changed to %s", scope == config::ReceiveScope::Slots ? "slots" : "everyone");
	// 画面共有している人を先に載せてから範囲を送る (Slots のときだけ載せる)。載せる人がいなければ、そこで範囲は
	// 決め直されないので、ここでも決め直す。
	impl_->admit_sharers_from_facts_locked();
	impl_->refresh_watch_scope_locked("setting");
	// 録画の対象も変わる。
	impl_->notify_recorder_formats_locked();
}

void Pipeline::request_move_to_main_call()
{
	// 利用者が [移動] を押したときだけ呼ぶ。移動先は送らず、送信側が押された時点のメインアカウントの通話を読み直す。
	diag::logf(LOG_INFO, "Move to the main account's call requested");
	impl_->listener.send_move_to_main_call();
}

void Pipeline::request_leave_call()
{
	diag::logf(LOG_INFO, "Leave the call requested");
	impl_->listener.send_leave_call();
}

void Pipeline::set_recording(bool on)
{
	// 録画の ON と OFF の値は、ここのメモリだけが持つ (settings には持たない)。値が変わらない呼び出し (ドックの
	// ドックの周期ごとのコピー) は、保存先を決めずに戻る (下の mu の中でも同じ確認をする。ここで先に見るのは、保存先の
	// 問い合わせをドックの周期ごとに起こさないため)。
	if (impl_->recording_on.load() == on)
		return;
	if (on && impl_->recorder) {
		// 停止の処理中と、書き込みのスレッドを止めている最中の ON は受け付けない (判定は
		// Recorder::set_enabled と同じ、record_types.h の recording_start_blocked)。
		if (record::recording_start_blocked(impl_->recorder->snapshot()))
			return;
		// 録画のセッションの保存先は、この時点で決める。セッションの値 (保存先、PCM の形、ファイルの名前の
		// 語) を録画の側にコピーしてから始める。
		const record::RecordDirChoice choice =
			resolve_record_dir_choice(impl_->record_dir_setting, &impl_->base_dir_probe);
		record::RecordSessionValues values;
		{
			std::lock_guard<std::mutex> lock(impl_->record_place_mu);
			impl_->record_root_now = choice.base_dir;
			impl_->record_values.base_dir = choice.base_dir;
			values = impl_->record_values;
		}
		impl_->recorder->set_session_values(values);
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		// 値が変わっていなければ何もしない。ドックは周期 (既定 200 ms) ごとにコピーを渡すので、ここで比べないと、
		// ドックの周期ごとに録画を開き直すことになる。
		if (impl_->recording_on.load() == on)
			return;
		// 停止の最中に ON に戻さない (受信の関数はこのフラグを見て録画に渡すので、ここで true にすると、
		// 止めたはずの録画にメッセージが流れ込む)。
		if (on && impl_->stopping.load())
			return;
		impl_->recording_on.store(on);

		if (on) {
			// 録画のセグメントは最初のキーフレームから始まるので、ON にした瞬間に、すべての画面の
			// ストリームにキーフレームを求める (枠に載っていない人も録画の対象である)。
			impl_->request_keyframes_for_recording_locked();
		}
		// 値が変わったときに送信側に伝える。送信の失敗は数えるだけ (ws/listener.h の send_failures) なので、
		// 届いたかどうかは、送信側が facts で返す値と比べて画面に表示する。
		impl_->listener.send_recording(on);
		// 録画の ON と OFF は media の値に関わる (合成のソースがあるか、録画が ON なら true)。合成のソースがない
		// シーンコレクションで録画だけを使う人は、ここで ON にしたときにゲートが開き、OFF にしたときに閉じる。
		impl_->refresh_media_locked(on ? "recording on" : "recording off");
	}

	// Recorder は mu の外で動かす。ON は書き込みのスレッドを起動し、OFF は録画ファイルの作成の処理を積む。どちらも
	// record_mu しか取らないので、mu を持ったまま呼んでもロックの順は崩れないが、スレッドを起動する間、すべての
	// ストリームの受信の関数を止めないようにする。OFF は待たない (停止の処理中であることを画面に表示するため、
	// 書き込みのスレッドの join は録画の側の別のスレッドが行う)。
	if (impl_->recorder) {
		if (on)
			impl_->recorder->set_enabled(true);
		else
			impl_->recorder->request_disable();
	}

	diag::logf(LOG_INFO, "Turned cordmux recording %s", on ? "on" : "off");
}

void Pipeline::poll_recording_self_stop()
{
	// 録画の側が自分で止めるのは、空き容量がなくなったときだけである。ここで見つけないと、(1) 送信側が黒埋め用の
	// フレームを送り続け (送信側に OFF を伝えないので)、(2) 書き込みのスレッドが止まらず、(3) 次に
	// [cordmuxの録画を開始] を押しても、set_recording が「変わっていない」と見て何も起きない (pipeline.h の宣言)。
	if (!impl_->recorder || impl_->stopping.load())
		return;
	// フラグがもう false のときは、録画の状態も読まない (止めた後は、この処理を 1 回通れば済む)。
	if (!impl_->recording_on.load())
		return;
	const record::RecordingSnapshot snapshot = impl_->recorder->snapshot();
	// 保存先に書けずに始められなかったときも、同じ処理で後始末する (ON を OFF に戻す)。
	const bool start_failed = snapshot.phase == record::RecordPhase::StartFailed;
	if (!snapshot.self_stopped_reason.has_value() && !start_failed)
		return;

	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (!impl_->recording_on.load())
			return;
		impl_->recording_on.store(false);
		// 値が変わったので送信側に伝える (set_recording と同じ関数。届いたかどうかは、送信側が facts で返す
		// 値と比べる)。
		impl_->listener.send_recording(false);
		// 録画の状態は media の値に関わるので、決め直す (set_recording と同じ)。
		impl_->refresh_media_locked("recording stopped by itself");
	}

	// Recorder は mu の外で動かす (set_recording の後半と同じ扱い)。このセッションの録画ログはもう閉じているので、
	// ここでするのは書き込みのスレッドを止めることだけである (待たない。止めるのは録画の側の別のスレッド)。
	impl_->recorder->request_disable();

	if (start_failed)
		diag::logf(LOG_ERROR, "Couldn't start cordmux recording: the save folder can't be written to");
	else
		diag::logf(LOG_ERROR, "The cordmux recording stopped: the save folder's drive is full");
}

std::string Pipeline::record_base_dir() const
{
	std::lock_guard<std::mutex> lock(impl_->record_place_mu);
	return impl_->record_root_now;
}

std::string Pipeline::preview_record_base_dir(const std::string &record_dir_setting)
{
	const record::RecordDirChoice choice = resolve_record_dir_choice(record_dir_setting, &impl_->base_dir_probe);
	return choice.base_dir;
}

void Pipeline::set_finalize_stall_s(int seconds)
{
	if (impl_->recorder)
		impl_->recorder->set_finalize_stall_s(seconds);
}

void Pipeline::set_link_lost_hold_ms(int ms)
{
	impl_->link_lost_hold_ns = std::uint64_t(ms > 0 ? ms : 0) * 1000000ULL;
}

void Pipeline::set_reconnect_layout_hold_ms(int ms)
{
	impl_->reconnect_layout_hold_us = std::int64_t(ms > 0 ? ms : 0) * 1000;
}

void Pipeline::set_display_buffer_ms(int ms)
{
	// settings の範囲に収める。範囲の外の値で、待ち行列が際限なく伸びたり、時刻の計算があふれたりしない。
	const int clamped = std::min(std::max(ms, config::kDisplayBufferMinMs), config::kDisplayBufferMaxMs);
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(impl_->pacer_mu);
		const std::int64_t delay_us = std::int64_t(clamped) * 1000;
		if (impl_->display_buffer_us.load() != delay_us) {
			impl_->display_buffer_us.store(delay_us);
			// 積んであるフレームの時刻も新しい D で決め直す。0 にすると、残りは次の取り出しですぐ渡る
			// (最も新しい 1 つだけ。古いフレームは捨てて数える)。
			impl_->configure_pacers_locked();
			changed = true;
		}
	}
	if (changed) {
		impl_->pacer_cv.notify_all();
		// 画面の音の置く位置は D で決まる。続きに置く許容幅 (audio_resync_ms) が D の変更を飲み込むので、全員の画面の音を
		// 置き直させる。pacer_mu を放してから取る (D の読み出しと置くことは、どちらも mixer_mu の中で行う)。
		{
			std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
			for (const MixerKey &key : impl_->mixer_roster.keys(MixerKind::ScreenAudio))
				impl_->mixer.resync(key);
		}
		diag::logf(LOG_INFO, "Display buffer changed: %d ms", clamped);
	}
}

void Pipeline::set_arrival_watch(bool on)
{
	impl_->arrival_watch.store(on);
}

void Pipeline::set_person_audio_volume(const std::string &user_id, MixerKind kind, int volume_pct)
{
	const MixerKey key{user_id, kind};
	std::lock_guard<std::mutex> lock(impl_->mixer_mu);
	impl_->mixer_levels.set_volume_pct(key, volume_pct);
	// 組がミキサーにない間は表に覚えるだけにする (組を作らない)。入ったときに当てる。
	if (impl_->mixer_roster.contains(key))
		impl_->mixer.set_gain(key, impl_->mixer_levels.get(key).gain());
}

void Pipeline::set_person_audio_mute(const std::string &user_id, MixerKind kind, bool muted)
{
	const MixerKey key{user_id, kind};
	std::lock_guard<std::mutex> lock(impl_->mixer_mu);
	impl_->mixer_levels.set_mute(key, muted);
	if (impl_->mixer_roster.contains(key))
		impl_->mixer.set_mute(key, impl_->mixer_levels.get(key).muted);
}

void Pipeline::set_person_volumes(const std::map<std::string, PersonVolumes> &volumes)
{
	std::lock_guard<std::mutex> lock(impl_->mixer_mu);
	// 値を持っている人と、渡された人の全員に当てる。渡されなかった人は 100% に戻る。
	std::set<std::string> ids = impl_->mixer_levels.user_ids();
	for (const auto &entry : volumes)
		ids.insert(entry.first);
	for (const std::string &user_id : ids) {
		const auto found = volumes.find(user_id);
		const PersonVolumes wanted = found != volumes.end() ? found->second : PersonVolumes{};
		for (const MixerKind kind : {MixerKind::Voice, MixerKind::ScreenAudio}) {
			const MixerKey key{user_id, kind};
			impl_->mixer_levels.set_volume_pct(key, kind == MixerKind::Voice ? wanted.voice_pct
											 : wanted.screen_pct);
			if (impl_->mixer_roster.contains(key))
				impl_->mixer.set_gain(key, impl_->mixer_levels.get(key).gain());
		}
	}
}

MixerLevel Pipeline::person_audio_level(const std::string &user_id, MixerKind kind) const
{
	std::lock_guard<std::mutex> lock(impl_->mixer_mu);
	return impl_->mixer_levels.get(MixerKey{user_id, kind});
}

void Pipeline::set_screen_layout_changed_sink(std::function<void()> sink)
{
	std::lock_guard<std::mutex> lock(impl_->layout_sink_mu);
	impl_->layout_changed_sink = std::move(sink);
}

void Pipeline::set_record_kind_words(const std::string &screen, const std::string &voice)
{
	record::RecordSessionValues values;
	{
		std::lock_guard<std::mutex> lock(impl_->record_place_mu);
		impl_->record_values.screen_kind_word = screen;
		impl_->record_values.voice_kind_word = voice;
		values = impl_->record_values;
	}
	if (impl_->recorder)
		impl_->recorder->set_session_values(values);
}

void Pipeline::set_record_file_name_format(const std::string &format)
{
	record::RecordSessionValues values;
	{
		std::lock_guard<std::mutex> lock(impl_->record_place_mu);
		impl_->record_values.output_name_format = format;
		values = impl_->record_values;
	}
	if (impl_->recorder)
		impl_->recorder->set_session_values(values);
}

void Pipeline::update_record_settings(const config::Settings &settings)
{
	impl_->record_dir_setting = settings.record_dir;
	const record::RecordDirChoice choice = resolve_record_dir_choice(settings.record_dir, &impl_->base_dir_probe);
	record::RecordSessionValues values;
	{
		std::lock_guard<std::mutex> lock(impl_->record_place_mu);
		impl_->record_values.output_pcm = settings.record_output_pcm;
		impl_->record_values.base_dir = choice.base_dir;
		impl_->record_root_now = choice.base_dir;
		values = impl_->record_values;
	}
	if (impl_->recorder)
		impl_->recorder->set_session_values(values);
}

void Pipeline::set_plugin_version(std::string version)
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	if (impl_->plugin_version == version)
		return;
	impl_->plugin_version = std::move(version);
	// 録画ログの session の行に書く。
	impl_->notify_recorder_session_facts_locked();
}

void Pipeline::set_display_names(std::map<std::string, std::string> names)
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	// 値が変わっていなければ何もしない (set_assign_policy と同じ。ドックは周期 (既定 200 ms) ごとにコピーを渡すので、
	// 比べないと、録画ログに人の行をドックの周期ごとに追加することになる)。
	if (impl_->display_names == names)
		return;
	impl_->display_names = std::move(names);
	// 名前が変わったので録画に渡し直す (読めた時点で録画ログに追記する)。
	impl_->notify_recorder_formats_locked();
}

void Pipeline::resume_finalize()
{
	// mu を取らない。録画の側のロックだけで済む処理で、すべてのストリームの受信の関数を止める理由がない
	// (set_recording の後半と同じ扱い)。
	if (impl_->stopping.load())
		return; // 停止の最中に、新しい録画ファイルの作成を始めない
	if (impl_->recorder)
		impl_->recorder->resume_pending();
	diag::logf(LOG_INFO, "Resumed finalizing recordings from an earlier session");
}

void Pipeline::set_feature_enabled(bool on)
{
	// 値が変わっていなければ何もしない (pipeline.h の宣言)。
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->feature_enabled == on)
			return;
		impl_->feature_enabled = on;
		// 無効にしたら media を false にして送る (送信側はすべてのストリームを止める)。待ち受けは開いたまま。
		impl_->refresh_media_locked(on ? "feature on" : "feature off");
		// 無効の間は誰も視聴しない (視聴の範囲を空にして送る)。有効に戻したら、受け取る範囲どおりに送り直す。
		impl_->refresh_watch_scope_locked(on ? "feature on" : "feature off");
	}
	diag::logf(LOG_DEBUG, "Feature flag %s", on_off(on));
	// 録画中なら止めて録画ファイルの作成を始める。set_recording は同じ値なら何もしないので、呼び出し元 (ドック) が先に
	// 止めていても 2 回は止めない。mu の外で呼ぶ (set_recording が自分で mu を取る)。
	if (!on)
		set_recording(false);
}

void Pipeline::set_limits(int max_long_edge, int fps_divisor, config::EncoderPreference encoder_preference,
			  config::EncodePriority encode_priority)
{
	// 値が変わっていなければ何もしない (pipeline.h の宣言)。
	const ws::LimitsMessage next =
		limits_from_settings(max_long_edge, fps_divisor, encoder_preference, encode_priority);
	std::lock_guard<std::mutex> lock(impl_->mu);
	if (impl_->limits == next)
		return;
	impl_->limits = next;
	impl_->refresh_limits_locked();
}

void Pipeline::on_listener_stopped()
{
	// Listener::stop() は on_disconnected を呼ばない (pipeline.h の宣言) ので、切れたときと同じ後始末をここで行う。
	// io_thread は join した後なので、受信の関数と重ならない。
	impl_->safe_call("the disconnection after the connection was closed", [&] { impl_->handle_disconnected(); });
}

Pipeline::SlotChange Pipeline::release_slot(config::SlotKind kind, const std::string &user_id)
{
	std::lock_guard<std::mutex> lock(impl_->mu);

	// 表を書き換え、外した人を覚え、経路を枠 0 へ付け替える (release_slot_in。枠 0 の経路の扱いはその説明)。
	const SlotChange out = release_slot_in(impl_->table, impl_->router, kind, user_id);
	if (out.outcome != SlotChange::Outcome::Released)
		return out;
	// 枠から外した人の組を外す (残りの音を鳴らし続けない)。
	impl_->on_routes_changed_locked();
	// 枠に載っている人が減ったので、視聴する人の範囲と録画の対象を決め直す (受け取る範囲が Slots のとき)。
	impl_->refresh_watch_scope_locked("slot released");
	if (impl_->receive_scope == config::ReceiveScope::Slots)
		impl_->notify_recorder_formats_locked();
	return out;
}

Pipeline::SlotChange Pipeline::replace_slot(config::SlotKind kind, const std::string &out_user_id,
					    const std::string &in_user_id)
{
	SlotChange none;
	none.outcome = SlotChange::Outcome::NotAssigned;
	// 入れ替えるのは画面の枠だけ (声の枠は画面の枠の選び方に合わせない)。
	if (kind != config::SlotKind::Screen)
		return none;

	SlotChange out;
	std::optional<ws::ConfigureMessage> replay;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		int out_retargeted = 0;
		int in_retargeted = 0;
		out = replace_slot_in(impl_->table, impl_->router, out_user_id, in_user_id, &out_retargeted,
				      &in_retargeted);
		if (out.outcome != SlotChange::Outcome::Replaced)
			return out;
		// 外した人の組と、載せた人の経路を一度に反映し、視聴する人の範囲と録画の対象も決め直す
		// (release_slot と assign_slot が行うことと同じ)。
		impl_->on_routes_changed_locked();
		impl_->refresh_watch_scope_locked("slot replaced");
		if (impl_->receive_scope == config::ReceiveScope::Slots)
			impl_->notify_recorder_formats_locked();
		// 載せた人の経路がまだなかったときは、直前の configure を適用し直す (assign_slot と同じ)。
		if (impl_->loading_finished && in_retargeted == 0) {
			replay = impl_->last_configure;
			generation = impl_->configure_generation;
		}
	}

	if (replay)
		impl_->safe_call("the stream list after a slot change",
				 [&] { impl_->handle_configure(*replay, generation); });
	return out;
}

void Pipeline::on_frontend_finished_loading()
{
	// 読み込みが終わったシーンコレクションの、合成のソースの有無を先に調べる (mu の外)。アンインストールの最中は
	// 調べない (refresh_layout_presence と同じ)。
	const bool read_now = !install::is_uninstalling();
	const LayoutPresence read = read_now ? read_layout_presence() : LayoutPresence{};

	// フラグを立てるのと、保留した configure を取り出すのを、同じ mu の中で行う (この後に届く configure は、
	// handle_configure がそのまま適用する)。調べた値の保存と media の決め直しも、同じ mu の中で行う (フラグが立った
	// 瞬間から、フレームを渡す処理が同じ値を見る)。
	std::optional<ws::ConfigureMessage> pending;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->loading_finished = true;
		if (read_now && !impl_->table.is_frozen())
			impl_->apply_layout_presence_locked(read);
		impl_->refresh_media_locked("scene collection loaded");
		pending = std::move(impl_->pending_configure);
		impl_->pending_configure.reset();
	}
	if (!pending)
		return;
	diag::logf(LOG_INFO, "Scenes finished loading; applying the held stream list from Vesktop");
	impl_->safe_call("the held stream list", [&] { impl_->handle_configure(*pending); });
}

void Pipeline::on_scene_collection_changing()
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	// 切り替えの最中は、映像も音声も渡さない (凍結。stream_router.h の output_allowed と video_allowed)。これから壊す
	// シーンコレクションの合成のソースに渡さないためである。割り当ては変えない。
	impl_->table.begin_scene_collection_change();

	// 調べてあった合成のソースの有無は、切り替える前のシーンコレクションの値なので、現在の値ではないことにする。
	// media の値を決めるためには持ち続ける (Impl::presence の宣言)。調べている途中の監視の結果も使わない
	// (presence_epoch)。
	++impl_->presence_epoch;
	impl_->presence_current = false;
}

void Pipeline::on_scene_collection_changed()
{
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->table.apply_scene_collection_changed();
		++impl_->presence_epoch;
	}
	// 切り替えた先の合成のソースの有無を調べ直し、media を決め直す (有無が変わっていれば、configure も適用し直す)。
	impl_->safe_call("the layout check after the scene collection changed",
			 [&] { impl_->refresh_layout_presence("scene collection changed"); });
}

void Pipeline::on_scene_collection_cleanup()
{
	// 調べてあった値を「現在の値ではない」にする (pipeline.h の宣言)。調べている途中の監視の結果も使わない。
	// media の値を決めるためには持ち続ける (切り替えのたびに、送信側にゲートを閉じてまた開かせない。
	// Impl::presence の宣言)。
	std::lock_guard<std::mutex> lock(impl_->mu);
	++impl_->presence_epoch;
	impl_->presence_current = false;
}

void Pipeline::on_frontend_exit()
{
	// フラグを先に立て (監視の入口がすぐ戻る)、調べてあった値を「現在の値ではない」にする。
	impl_->frontend_exiting.store(true);
	on_scene_collection_cleanup();
}

} // namespace cordmux::pipeline

// SPDX-License-Identifier: Apache-2.0

// recorder.h の実装。on_* の関数は ws の io_thread から呼ばれ、セグメントと録画ログは書き込み専用のスレッドが書く。
// ほかに、録画ファイルの作成のスレッド、保存先にアクセスできるかを問い合わせるスレッド、request_disable が書き込むスレッドを
// join するためのスレッドがある。
// ロックの順は count_mu -> pending_dirs_mu、count_mu -> mu、count_mu -> finalize_mu である。mu と finalize_mu は
// 重ねて持たず、pending_dirs_mu と probe_mu を持ったまま、ほかのロックを取らない。

#include "record/recorder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

#include "platform/platform.h"
#include "record/record_args.h"
#include "record/record_audio.h"
#include "record/record_avcc.h"
#include "record/record_branch.h"
#include "record/record_clock.h"
#include "record/record_finalize.h"
#include "record/record_ledger.h"
#include "record/record_search_dirs.h"
#include "record/record_space.h"
#include "record/record_writer.h"

namespace cordmux::record {

// ---------------------------------------------------------------------------
// 製品で使う RecordPlatformOps
// ---------------------------------------------------------------------------

RecordPlatformOps make_real_record_platform_ops(std::shared_ptr<std::string> child_log_path)
{
	RecordPlatformOps ops;
	ops.make_directories = [](const std::string &path) { return platform::make_directories(path); };
	ops.make_new_directory = [](const std::string &path, bool *already_exists) {
		return platform::make_new_directory(path, already_exists);
	};
	ops.open_file = [](const std::string &path) -> FileHandle { return platform::open_file(path); };
	ops.append = [](FileHandle file, const void *data, std::size_t len) {
		return platform::append(file, data, len);
	};
	ops.close = [](FileHandle file) { platform::close(file); };
	ops.free_space_bytes = [](const std::string &path) { return platform::free_space_bytes(path); };
	// start_child だけは、子プロセスの出力先 (child_log_path の現在の値) を加えて platform::start_child を呼ぶ。
	ops.start_child = [child_log_path](const std::string &program,
					   const std::vector<std::string> &args) -> ChildHandle {
		const std::string log = child_log_path ? *child_log_path : std::string();
		return platform::start_child(program, args, log);
	};
	ops.wait_child = [](ChildHandle child, int timeout_ms) {
		return platform::wait_child(child, timeout_ms);
	};
	ops.kill_child = [](ChildHandle child) { platform::kill_child(child); };
	ops.now_steady_us = []() { return platform::now_steady_us(); };
	ops.now_wall_us = []() { return platform::now_wall_us(); };
	ops.ffmpeg_path = []() { return platform::ffmpeg_path(); };
	ops.utc_offset_sec = []() { return platform::utc_offset_sec_now(); };
	// セッションが使用中であることを示すロックファイル。中断したセッションの判定に使う。
	ops.hold_lock = [](const std::string &path, bool *held_elsewhere) -> FileHandle {
		return platform::hold_lock_file(path, held_elsewhere);
	};
	ops.lock_held = [](const std::string &path) { return platform::lock_file_held(path); };
	// 保存先にあるセッションの一覧を読む。関数が空のときと同じく record_writer.h の list_subdirectories を呼ぶ。
	// 一覧を読む途中の失敗は実際のフォルダーでは起こしにくいので、テストが差し替えて起こせるように関数として持つ。
	ops.list_subdirectories = [](const std::string &path, ListOutcome *outcome) {
		return list_subdirectories(path, outcome);
	};
	return ops;
}

namespace {

// resume_pending がセッションのロックファイルを持とうとする回数と間隔。ほかのプロセスの集計がロックファイルを
// 確認する間 (開いてすぐ閉じる) だけ持てないことがあるので、その一瞬を越えるだけ短く再試行する。
// ほかの OBS が録画中か作成中で持っているセッションは 5 回とも持てないので、キューに積まない。
constexpr int kLockAttempts = 5;
constexpr int kLockRetryMs = 20;

// 書き込むスレッドのキューに積むもの。on_* の関数は、受け取ったデータをコピーしてここに積むだけである。
enum class EventKind { Video, Audio, Configure, Connected, Disconnected, Enable, Disable, Stop };

struct Event {
	EventKind kind = EventKind::Video;
	ws::FrameHeader header;
	std::vector<std::uint8_t> payload;
	std::int64_t steady_us = 0;
	std::int64_t wall_us = 0;
	std::vector<pipeline::StreamFormat> formats;
	std::map<std::string, std::string> names;
	StopReason stop_reason = StopReason::TurnedOff;
	// Enable のときだけ使う。ON にした時点のセッションの値。
	RecordSessionValues values;
};

struct TrackKey {
	std::uint32_t stream_id = 0;
	TrackKind track = TrackKind::ScreenVideo;
};

bool operator<(const TrackKey &a, const TrackKey &b)
{
	if (a.stream_id != b.stream_id)
		return a.stream_id < b.stream_id;
	return static_cast<int>(a.track) < static_cast<int>(b.track);
}

// 1 つのトラック (stream_id と種別の組み合わせ) の状態。SegmentWriter はコピーできないので、tracks は
// unique_ptr で持つ。
struct TrackState {
	std::string user_id;
	TrackKind track = TrackKind::ScreenVideo;
	StreamFormatMemo format;      // configure が示す現在の解像度とフレームレート
	StreamFormatMemo open_format; // 開いているセグメントの解像度とフレームレート
	bool present = true;
	std::string encoder_instance_id;
	int instance_seq = 0;

	SegmentWriter writer;
	bool open = false;
	LedgerSegmentRow row;
	ClockState clock;
	AudioCursor cursor;
	std::vector<std::uint8_t> avcc;
	std::string avcc_hex;
	std::int64_t segment_origin_us = 0;
	bool dropping_until_keyframe = false;

	// 黒埋め用のフレームを書くファイル (エンコーダのインスタンスごとに 1 つ)。
	SegmentWriter material;
	bool material_open = false;
	LedgerBlackRow material_row;
	std::int64_t material_origin_us = 0;
	std::set<std::string> material_done;
	// 通常のフレーム (黒埋め用でないフレーム) が 1 つでも届いたエンコーダのインスタンス。黒埋め用のフレームが
	// 作られたタイミングの推定に使う (guess_material_origin)。
	std::set<std::string> instances_with_real_frame;
};

// 録画を ON にしてから、ON を受けて送信側が作った黒埋め用のフレーム (MaterialOrigin::RecordingTurnedOn) が
// 届くまでの時間の上限。ドックは、録画の状態が送信側に届いていないと表示するまでに 5 秒待つ
// (ledger/observation.h の kDefaultSenderRecordingGraceUs)。ON が送信側に届くまでにそれだけの時間を見込むので、
// ON を受けて作られたフレームも同じ時間まで待つ。これより後に届いたフレームは、ON と結び付けない。
constexpr std::int64_t kMaterialAfterEnableUs = 5'000'000;

// 有理数のフレームレートから、フレームの間隔 (マイクロ秒) を求める。
std::int64_t frame_period_us_of(int fps_num, int fps_den)
{
	if (fps_num <= 0 || fps_den <= 0)
		return 0;
	return (static_cast<std::int64_t>(fps_den) * 1000000 + fps_num / 2) / fps_num;
}

// pipeline::StreamFormat の fps は倍精度なので、録画ログに書くために有理数に戻す (60000/1001 を 59.94 と
// 書かない)。整数に十分近ければ `n/1`、`n*1000/1001` に十分近ければ `n000/1001` にし、どちらでもなければ
// 分母が 1000 の有理数にする。許容の幅 0.005 は、29.97 と 30 (差は約 0.03) を取り違えない幅として選んだ。
void fps_to_rational(double fps, int *num, int *den)
{
	*num = 0;
	*den = 0;
	if (!(fps > 0.0))
		return;
	constexpr double kTolerance = 0.005;

	const int whole = static_cast<int>(fps + 0.5);
	if (whole > 0 && fps > whole - kTolerance && fps < whole + kTolerance) {
		*num = whole;
		*den = 1;
		return;
	}
	const int ntsc = static_cast<int>(fps * 1001.0 / 1000.0 + 0.5);
	if (ntsc > 0) {
		const double exact = static_cast<double>(ntsc) * 1000.0 / 1001.0;
		if (fps > exact - kTolerance && fps < exact + kTolerance) {
			*num = ntsc * 1000;
			*den = 1001;
			return;
		}
	}
	*num = static_cast<int>(fps * 1000.0 + 0.5);
	*den = 1000;
}

} // namespace

// ---------------------------------------------------------------------------
// Recorder::Impl
// ---------------------------------------------------------------------------

struct Recorder::Impl {
	config::Settings settings;
	RecordPlatformOps ops;
	std::string base_dir;
	RecordingGateFn gate = &recording_gate;

	// ---- 入力と書き込むスレッドの間。Pipeline のロックを持った入力が、この mu を取る ----
	mutable std::mutex mu;
	std::condition_variable cv;
	std::deque<Event> queue;
	std::map<std::uint32_t, bool> overflow; // キューが溢れてデータを捨てたストリーム
	bool quit = false;
	bool stopping = false;

	// 録画が ON か。書き込むスレッドと入力の両方がロックを持たずに読むので、スナップショットの `snap.enabled` とは
	// 別に atomic で持つ (`snap` は `mu` の下でだけ読み書きする)。
	std::atomic<bool> enabled_flag{false};
	// 接続しているか (セグメントを分ける条件に使う)。録画が OFF の間も更新する。録画を ON にする時点では、もう
	// 接続しているのが普通である。
	std::atomic<bool> connected_flag{false};

	// 最新の `configure` のコピー。録画が OFF の間も受け取って持つ。`configure` は接続の直後に 1 度届くだけで、
	// 録画を ON にするのはその後なので、OFF の間に捨てると、ストリームの表が空のまま始まる。キューには積まずに
	// 1 つを置き換えるだけなので、OFF の間に増えるものはない。
	// ON にした時点で、このコピーを Enable のイベントに載せる。キューに載せずに後から読むと、その後に届いた
	// `configure` を先に適用してしまい、セグメントの切れ目が消える。
	std::vector<pipeline::StreamFormat> pending_formats;
	std::map<std::string, std::string> pending_names;

	// 録画ログに書く値。pending_formats と同じく、OFF の間も受け取って 1 つを置き換えるだけで、
	// RecordPlatformOps の関数は呼ばない。プラグインのバージョンとダミーを表示していた時間帯は、セッションの始めと
	// 終わりに session の行に書く (後の行が勝つ)。SPS と PPS を付けた回数は、セグメントを閉じるときにセグメントの
	// 行に書く。
	SessionFacts pending_facts;

	// 次に始めるセッションの値 (set_session_values。mu の下)。pending_formats と同じく、ON にした時点で Enable の
	// イベントに載せる。後から読むと、その後に変えた値を前のセッションに適用してしまう。
	RecordSessionValues pending_values;
	// set_session_values が 1 度でも呼ばれたか (mu の下)。呼ばれていれば、その `base_dir` が空でも、構築のときの
	// 場所は使わない (空は解決できなかったことを示し、このセッションは始めない)。構築のときの場所を使うのは、
	// セッションの値を 1 度も渡さない呼び出し元 (テスト) だけである。
	bool session_values_given = false;
	// 書き込むスレッドが動いているか (mu の下)。`writer_thread` そのものをほかのスレッドから読むと、止める処理の
	// スレッドの join とデータ競合になるので、writer_running() はこちらを返す。
	bool writer_alive = false;
	// 録画ファイルの作成の進み具合が止まったとみなす秒数。画面に表示するための判定なので、セッションにはコピーせず、実行中の
	// 録画ファイルの作成にも使う。
	std::atomic<int> finalize_stall_s{kDefaultFinalizeStallS};
	// 作成待ちを探す保存先 (セッションのフォルダーの親フォルダー。mu の下)。構築のときの場所、この起動で
	// セッションを始めた場所、一覧 (record_search_dirs.h) のうちアクセスできた保存先が入る。現在の保存先は、一覧とは
	// 別に必ず探す。
	std::vector<std::string> pending_search_dirs;
	// 録画中に接続が切れた時刻 (mu の下)。値なしなら切れていない。
	// RecordingSnapshot::gaps に数えるのは、録画中に Vesktop との接続が切れていた期間だけである。1 人の画面共有が
	// 途中で止まった期間は黒で埋まる正常な状態で、キューが溢れたときはセグメントを分けるだけで録画は続くので、
	// どちらも数えない。
	std::optional<std::int64_t> gap_started_wall_us;

	// ---- 画面に表示するスナップショット (mu の下で読み書きする) ----
	RecordingSnapshot snap;
	std::map<std::uint32_t, pipeline::RecordOnlyFrameClock> record_only;

	// 止める処理のスレッド (request_disable が起動する)。書き込むスレッドを join するのは、このスレッドか、
	// このスレッドを join した後の呼び出し元だけで、同じスレッドを 2 か所で join しない。
	// stopper_thread に触るのは、request_disable・set_enabled・stop を呼ぶ 1 つのスレッドだけである。
	std::thread stopper_thread;

	// ---- 書き込むスレッドだけが読み書きするもの ----
	std::thread writer_thread;
	SessionState session;
	// このセッションのロックファイルのハンドル。セッションを閉じたら、録画ログのハンドルと一緒に録画ファイルの作成のジョブに
	// 渡す。
	FileHandle session_lock = kInvalidFile;
	LedgerDocument doc;
	std::map<TrackKey, std::unique_ptr<TrackState>> tracks;
	// 音声のチャンクを float に移す置き場と、書く位置に合わせて変換したサンプルの置き場 (handle_audio が使い回す)。
	std::vector<float> audio_in;
	std::vector<float> audio_out;
	std::map<std::string, std::string> display_names;
	std::map<std::string, std::size_t> person_index;
	// セグメントの通し番号。ストリームごとではなくセッションの中で通しにするので、録画ログを読まなくても、
	// ファイル名の並びで書いた順が分かる。
	std::int64_t segment_seq = 0;
	std::int64_t closed_bytes = 0;
	std::int64_t last_space_check_us = 0;
	// 書き込むスレッドから見て、今録画中か。入力が読む `enabled_flag` と分けるのは、キューに順番があるためである。
	// Enable と Disable もデータと同じキューに並ぶので、その間に積まれたデータは録画中に届いたものとして書く。
	// `enabled_flag` を書き込むスレッドが読むと、OFF にした時点で、まだ書いていないデータがすべて失われる。
	// この値が真になるのは Enable を処理したときだけなので、録画が OFF の間にセグメントのファイルを作ることはない。
	bool writer_enabled = false;
	// 現在のセッションの値 (Enable のイベントが運んだコピー)。
	RecordSessionValues active_values;

	// ---- 録画ファイルの作成 ----
	struct FinalizeJob {
		LedgerDocument doc;
		std::string session_dir;
		// 録画ログとセグメントのフォルダー。
		std::string materials_dir;
		// セッションを始めた時点の値。再開したセッションでは、再開した時点の値。
		RecordSessionValues values;
		// 開いている録画ログのハンドル (セッションを閉じた直後の録画ファイルの作成で使う)。
		FileHandle ledger = kInvalidFile;
		// 前のセッションの録画ログのパス (再開で使う)。ハンドルがないので、追記のために開き直す。
		std::string ledger_path;
		bool repair = false;
		// 前のセッションの録画ログが改行で終わっていない (最後の行を書いている途中で OBS か PC が止まった)。
		// 追記する前に改行を 1 つ入れる。入れないと、最初に追記する行が途中で切れた行の続きになり、両方とも
		// 読めなくなる。
		bool ledger_needs_newline = false;
		// このセッションのロックファイルのハンドル。録画ファイルの作成を終えて録画ログの最後の行を書き、録画ログを閉じた
		// 後に、このハンドルも閉じる。kInvalidFile ならロックファイルを持てなかった (ほかのプロセスは
		// 録画ログだけで判定する)。
		FileHandle lock = kInvalidFile;
	};
	std::thread finalize_thread;
	mutable std::mutex finalize_mu; // pending_session_dirs (const) も finalize_inflight をコピーするために取る
	std::condition_variable finalize_cv;
	std::deque<FinalizeJob> finalize_queue;
	bool finalize_quit = false;
	std::atomic<bool> finalize_cancel{false};
	std::atomic<bool> finalize_busy{false};
	// キューに積んだか作成中のセッションのフォルダー (finalize_mu の下)。作成待ちの集計から除き、
	// resume_pending を続けて呼んでも、同じセッションを 2 度積まない。
	std::multiset<std::string> finalize_inflight;
	// 作成待ちの集計 (recount_pending) を 1 つずつ実行するためのロック。
	std::mutex count_mu;
	// 録画ファイルの作成の済んでいないセッションが残る保存先の一覧 (record_search_dirs.h)。pending_dirs_mu の下で読み書きする。
	// count_mu と別の小さなロックにするのは、書き込むスレッドがセッションを始めるときに、ネットワークの場所を読む
	// 集計を待たないようにするためである。待つとセッションの始まりが止まり、キューが溢れることがある。
	// ロックの順は count_mu -> pending_dirs_mu で、pending_dirs_mu を持ったまま count_mu・mu・finalize_mu を
	// 取らない。
	// `pending_dirs_store.save` が空の間は一覧を使わない (remember_pending_dirs を呼ばない呼び出し元。テストの
	// 多く)。
	std::mutex pending_dirs_mu;
	PendingDirsStore pending_dirs_store;
	// セッションの始めに一覧に追加したが、まだファイルに書けていない保存先 (ファイルを読めなかったか、
	// 書けなかった)。
	// 次に一覧を更新するたびに追加し直し、ファイルに載ったら外す。その保存先を後で探して、録画ファイルの作成の済んでいない
	// セッションがないと分かったときも外す。集計の結果による追加と削除は、次の集計がもう一度求めるので持たない。
	std::vector<std::string> unsaved_session_adds;
	// セッションの始めに一覧に追加した回数と、保存先ごとの、最後に追加したときのその回数。集計がフォルダーを
	// 読み始めた後にセッションを始めた保存先を、その集計では一覧から外さないために使う (読み始めた後に始めた
	// セッションは、その集計の結果に載っていないことがある)。
	std::uint64_t pending_dir_adds = 0;
	std::vector<std::pair<std::string, std::uint64_t>> pending_dir_added_at;
	// 一覧の保存先にアクセスできるかを問い合わせるスレッド。起動するのも join するのも UI のスレッド
	// (remember_pending_dirs と stop) で、probe_mu の下で持つ。`probe_quit` を true にした後に答えた保存先は
	// 追加しない (止める最中に数え直さない)。
	std::mutex probe_mu;
	std::vector<std::thread> probe_threads;
	std::atomic<bool> probe_quit{false};
	// 起動のときにアクセスできなかった保存先を、問い合わせ直すまで待つのに使う。待つのは probe_mu の下で、
	// probe_quit を true にする側は probe_mu を取ってから変えて通知する (signal_probe_quit)。通知を取りこぼすと、
	// stop が待ち時間の分だけ止まる。
	std::condition_variable probe_cv;
	std::atomic<int> reprobe_delay_ms{kPendingDirReprobeDelayMs};
	// 録画中のセッションのフォルダー (mu の下。空なら録画していない)。自分が録画中のセッションは、作成待ちに
	// 数えない。ロックファイルを持てなかったセッション (ロックファイルの関数がない、作れない保存先) でも、自分が
	// 録画中のセッションを中断したセッションと判定しない。セッションを閉じて録画ファイルの作成のキューに積んだ後に空に戻す
	// (そこから先は finalize_inflight が除く)。
	std::string recording_session_dir;
	std::shared_ptr<std::string> child_log = std::make_shared<std::string>();

	// -----------------------------------------------------------------------
	// 入力 (コピーしてキューに積むだけ)
	// -----------------------------------------------------------------------

	bool enabled() const { return enabled_flag.load(); }

	// このセッションの保存先 (Enable が運んだ値)。書き込むスレッドだけが呼ぶ。
	// 空か相対パスなら空を返す。空は解決できなかったことを示し、構築のときの場所には移さない。相対パスは OBS の
	// 作業フォルダーの下に解決されてしまう。どちらも ensure_session_open が空の保存先として断り、このセッションは
	// StartFailed になる。呼び出し元 (Pipeline) も相対パスを確認するが、ここは解決済みの絶対パスを前提に書き始める
	// 最後の場所なので、ここでも確認する。
	std::string session_base_dir() const
	{
		const std::string &dir = active_values.base_dir;
		if (dir.empty() || !platform::is_absolute_path(dir))
			return std::string();
		return dir;
	}

	// セッションのフォルダー名に使う、UTC との時差 (RecordPlatformOps::utc_offset_sec がなければ 0)。
	int utc_offset() const { return ops.utc_offset_sec ? ops.utc_offset_sec() : 0; }

	std::int64_t wall_now() const { return ops.now_wall_us ? ops.now_wall_us() : 0; }
	std::int64_t steady_now() const { return ops.now_steady_us ? ops.now_steady_us() : 0; }

	// 段階を変える (mu を持って呼ぶ)。同じ段階なら時刻を変えない。ドックは段階が変わってから 1 秒間、録画のボタンに
	// 前の文言を表示する (ledger/rules_dock.cpp の kRecordLabelHoldUs) ので、時刻を更新するとその 1 秒が延びる。
	void set_phase_locked(RecordPhase phase)
	{
		if (snap.phase == phase)
			return;
		snap.phase = phase;
		snap.phase_since_wall_us = wall_now();
		snap.phase_since_steady_us = steady_now();
	}

	// 段階が `from` のときだけ `to` に変える (mu を持って呼ぶ)。書き込むスレッドが段階を変えるのは、この関数を
	// 通すときだけである。変えてよいのは Starting から Recording か StartFailed へと、Recording から Idle (自分で
	// 止まったとき) へだけで、利用者の OFF が設定した Stopping は上書きしない。Stopping を終わらせるのは、書き込む
	// スレッドを join した側 (fold_writer_and_settle) である。書き込むスレッドが無条件に段階を設定すると、書き込む
	// スレッドが処理するより先に OFF が届いたときに Stopping が Recording に上書きされ、join の後も Idle に
	// 戻らない。
	bool set_phase_locked_if(RecordPhase from, RecordPhase to)
	{
		if (snap.phase != from)
			return false;
		set_phase_locked(to);
		return true;
	}

	// 接続が切れていた期間を閉じて、gaps に 1 件追加する (mu を持って呼ぶ)。
	void close_gap_locked()
	{
		if (!gap_started_wall_us)
			return;
		RecordGapSpan span;
		span.start_wall_us = *gap_started_wall_us;
		const std::int64_t now = wall_now();
		span.duration_us = now > span.start_wall_us ? now - span.start_wall_us : 0;
		snap.gaps.push_back(span);
		gap_started_wall_us.reset();
	}

	// 探す保存先を追加する (mu を持って呼ぶ)。同じ保存先は 1 つにまとめる。
	void add_search_dir_locked(const std::string &dir)
	{
		if (dir.empty())
			return;
		if (std::find(pending_search_dirs.begin(), pending_search_dirs.end(), dir) == pending_search_dirs.end())
			pending_search_dirs.push_back(dir);
	}

	// 探す保存先を追加して数え直す (一覧の保存先が問い合わせに答えたときの問い合わせのスレッドから呼ぶ)。その
	// 集計で `dir` にアクセスできたかを返す。`dir` のフォルダーを読めなかったときだけ false で、一覧を使って
	// いなくて分からないときは true。
	bool add_search_dir_and_recount(const std::string &dir)
	{
		{
			std::lock_guard<std::mutex> lock(mu);
			add_search_dir_locked(dir);
		}
		std::vector<SearchDirResult> walked;
		recount_pending(nullptr, &walked);
		return reached_in(walked, dir);
	}

	// 集計の結果で `dir` にアクセスできたか。結果に載っていなければ true を返す (分からないので、
	// 問い合わせ直さない)。
	static bool reached_in(const std::vector<SearchDirResult> &walked, const std::string &dir)
	{
		for (const SearchDirResult &result : walked) {
			if (!result.reachable && platform::same_dir_path(result.base_dir, dir))
				return false;
		}
		return true;
	}

	// 一覧の保存先に 1 回問い合わせ、アクセスできたら探す保存先に追加して数え直す (問い合わせのスレッドの中で
	// 呼ぶ)。
	// アクセスできたかを返す。止める要求の後は問い合わせず、追加もしない。
	bool probe_and_recount(const std::function<bool(const std::string &)> &reachable, const std::string &dir)
	{
		if (probe_quit.load() || !reachable(dir) || probe_quit.load())
			return false;
		return add_search_dir_and_recount(dir);
	}

	// 問い合わせ直す前に待つ (問い合わせのスレッドの中で呼ぶ)。止める要求が来たらすぐ戻り、false を返す。
	bool wait_before_reprobe(int delay_ms)
	{
		std::unique_lock<std::mutex> lock(probe_mu);
		probe_cv.wait_for(lock, std::chrono::milliseconds(delay_ms), [this]() { return probe_quit.load(); });
		return !probe_quit.load();
	}

	// 問い合わせのスレッドに止める要求を出す (Recorder::stop と join_probes)。probe_mu の下で probe_quit を
	// true にしてから通知する。ロックの外で変えると、待つ側が false を見た後、待ちに入る前に通知されたときに通知を
	// 取りこぼし、もう一度問い合わせるまでの待ち時間の分だけ、stop (OBS の終了のときに UI のスレッドで呼ぶ) が
	// 止まる。
	void signal_probe_quit()
	{
		{
			std::lock_guard<std::mutex> lock(probe_mu);
			probe_quit = true;
		}
		probe_cv.notify_all();
	}

	// 保存先の一覧 (record_search_dirs.h) を更新し、変わったらファイルに書く。pending_dirs_mu を持って呼ぶ。現在の
	// ファイルの中身に `change` と、セッションの始めに追加してまだ書けていない保存先を適用する。ファイルを
	// 読めなければ書かない。前に読んだコピーで書くと、ほかの OBS が追加した保存先や、読めなかった中身を消す。
	// ファイルを書くのは一覧が変わったときだけなので、録画を使ったことがなく、録画ファイルの作成の済んでいないセッションもない
	// 利用者には、ファイルを作らない。セッションがすべて完成すると、ファイルは空の一覧で残る (空の一覧とファイルが
	// ないことは同じに扱うので、削除はしない)。録画が OFF の間も、集計で更新した一覧を書く。保存先を変えるのは
	// 録画が OFF の間なので、OFF の間に書かないと、前の保存先を一覧に残せない。
	// 2 つの OBS がほぼ同時に読み直して書くと、片方の変更が失われることがある。片方が保存先 A を読み終えた直後に、
	// もう片方が A でセッションを始めて一覧に追加すると、先に読んだ結果が A を外すこともある。防ぐにはプロセスの
	// 間のロックか世代の番号が必要だが、2 つの OBS が同じ時期に録画ファイルの作成の済んでいないセッションを作ったときにしか
	// 起きない。
	void change_pending_dirs_locked(const std::function<std::vector<std::string>(const std::vector<std::string> &)> &change)
	{
		if (!pending_dirs_store.save || !pending_dirs_store.load)
			return;
		const std::optional<std::vector<std::string>> on_disk = pending_dirs_store.load();
		if (!on_disk)
			return;
		std::vector<std::string> next = change(*on_disk);
		for (const std::string &dir : unsaved_session_adds)
			next = add_pending_dir(std::move(next), dir);
		if (next == *on_disk || pending_dirs_store.save(next))
			unsaved_session_adds.clear(); // ファイルに載った
	}

	// セッションを始めた保存先を一覧に追加する。書き込むスレッドから、mu も count_mu も持たずに呼ぶ。追加するのは、
	// 開けたセッションの保存先だけである。
	void remember_session_base_dir(const std::string &dir)
	{
		if (dir.empty())
			return;
		std::lock_guard<std::mutex> lock(pending_dirs_mu);
		if (!pending_dirs_store.save)
			return;
		++pending_dir_adds;
		bool found = false;
		for (auto &entry : pending_dir_added_at) {
			if (platform::same_dir_path(entry.first, dir)) {
				entry.second = pending_dir_adds;
				found = true;
			}
		}
		if (!found)
			pending_dir_added_at.emplace_back(dir, pending_dir_adds);
		unsaved_session_adds = add_pending_dir(std::move(unsaved_session_adds), dir);
		change_pending_dirs_locked([](const std::vector<std::string> &base) { return base; });
	}

	// 問い合わせのスレッドをすべて join する (Recorder::stop)。detach すると、プラグインがアンロードされた後に
	// スレッドが戻り、アンロード済みのコードを実行する (pipeline/bounded_call.h と同じ理由)。応答しない
	// 問い合わせがあると、OBS の終了は OS が諦めるまで待つことがある。
	void join_probes()
	{
		signal_probe_quit();
		std::vector<std::thread> threads;
		{
			std::lock_guard<std::mutex> lock(probe_mu);
			threads.swap(probe_threads);
		}
		for (std::thread &thread : threads) {
			if (thread.joinable())
				thread.join();
		}
	}

	void push(Event &&event)
	{
		std::lock_guard<std::mutex> lock(mu);
		if (stopping)
			return;
		queue.push_back(std::move(event));
		cv.notify_one();
	}

	// 映像か音声のデータをキューに積む。キューが溢れたら捨てて数え、そのストリームにフラグを立てる。書き込む
	// スレッドはそのストリームのセグメントを閉じ、次のキーフレームまで捨てる。録画は止めない (止めるのは空き容量が
	// 尽きたときだけ)。上限はデータの数で数える。バイト数で数えると、同じ設定でも解像度によって積めるフレームの数が
	// 変わる。
	void push_frame(Event &&event)
	{
		std::lock_guard<std::mutex> lock(mu);
		if (stopping || !gate(enabled_flag.load()))
			return;
		const int limit = settings.record_queue_limit > 0 ? settings.record_queue_limit : 1;
		if (static_cast<int>(queue.size()) >= limit) {
			++snap.frames_dropped_overflow;
			overflow[event.header.stream_id] = true;
			return;
		}
		queue.push_back(std::move(event));
		cv.notify_one();
	}

	bool take_overflow(std::uint32_t stream_id)
	{
		std::lock_guard<std::mutex> lock(mu);
		auto it = overflow.find(stream_id);
		if (it == overflow.end() || !it->second)
			return false;
		it->second = false;
		return true;
	}

	// -----------------------------------------------------------------------
	// 録画ログ
	// -----------------------------------------------------------------------

	// `gate` には、録画が ON かではなく、このセッションの録画ログが開いているかを渡す。録画を OFF にした直後に
	// セグメントを閉じて最後の行を書くのは、そのセッションの続きだからである。録画ログが開くのは
	// ensure_session_open が同じ `gate` を通ったときだけなので、録画が OFF の間に新しいセッションのファイルを
	// 作ることはない。
	void append_line(const std::string &line)
	{
		append_ledger_line(session, ops, line, session.open, gate);
	}

	// session の行を書くたびに、その時点のプラグインのバージョンと、ダミーを表示していた時間帯を入れる。時間帯は
	// セッションの途中で増えるので、最後に書いた session の行の値が、そのセッションの値になる。
	void take_session_facts()
	{
		SessionFacts facts;
		{
			std::lock_guard<std::mutex> lock(mu);
			facts = pending_facts;
		}
		doc.session.plugin_version = facts.plugin_version;
		doc.session.dummy_intervals = facts.dummy_intervals;
	}

	void write_session_row()
	{
		if (!session.open)
			return;
		take_session_facts();
		append_line(to_jsonl(doc.session));
	}

	// セグメントを開いた時点の行 (`truncated` を真、close_reason を not_closed にする)。閉じたときの行が、同じ
	// `file` をキーに後から勝つ。閉じる前に OBS か PC が止まったセッションでは、この行が、閉じていないセグメントが
	// あることを示す唯一の情報になる。位置を決める時刻、avcC、解像度は開いた時点でそろっており、長さは録画ファイルの作成が
	// セグメントのファイルから読み直す。録画ログにないファイルを録画ファイルの作成のときに segments/ から探す方法では、位置を
	// 決める時刻 (送信側のタイムスタンプと受信側の単調な時計) がファイルにないので、置く位置を決められない。
	void append_open_row(const LedgerSegmentRow &row)
	{
		LedgerSegmentRow open_row = row;
		open_row.truncated = true;
		open_row.close_reason = CloseReason::NotClosed;
		append_line(to_jsonl(open_row));
	}

	void note_person(const std::string &user_id, TrackKind track)
	{
		auto name_it = display_names.find(user_id);
		const std::string name = name_it != display_names.end() ? name_it->second : std::string();

		auto index_it = person_index.find(user_id);
		if (index_it == person_index.end()) {
			LedgerPersonRow row;
			row.user_id = user_id;
			row.display_name = name;
			row.kinds.push_back(track);
			person_index[user_id] = doc.people.size();
			doc.people.push_back(row);
			append_line(to_jsonl(doc.people.back()));
			return;
		}

		LedgerPersonRow &row = doc.people[index_it->second];
		const bool has_kind = std::find(row.kinds.begin(), row.kinds.end(), track) != row.kinds.end();
		const bool name_changed = row.display_name != name && !name.empty();
		if (!has_kind)
			row.kinds.push_back(track);
		if (name_changed)
			row.display_name = name;
		if (!has_kind || name_changed)
			append_line(to_jsonl(row)); // 同じ人の行は、後の行が勝つ
	}

	// 録画を終了する時点の名前を、録っていた全員の人の行に書く。録画ファイルの名前は、録画ログの人の行
	// (後の行が勝つ) の名前から作るので、セグメントを開いた後の改名もここで反映される。名前が変わっていない人と、
	// 名前が分からない人 (通話から抜けた人など) には書かない。終了した後の改名は届かないので、録画の名前は変わらない。
	void note_final_names()
	{
		for (LedgerPersonRow &row : doc.people) {
			auto name_it = display_names.find(row.user_id);
			if (name_it == display_names.end() || name_it->second.empty() || name_it->second == row.display_name)
				continue;
			row.display_name = name_it->second;
			append_line(to_jsonl(row));
		}
	}

	// -----------------------------------------------------------------------
	// セグメント
	// -----------------------------------------------------------------------

	TrackState *find_track(std::uint32_t stream_id, TrackKind track)
	{
		TrackKey key;
		key.stream_id = stream_id;
		key.track = track;
		auto it = tracks.find(key);
		return it != tracks.end() ? it->second.get() : nullptr;
	}

	TrackState &ensure_track(std::uint32_t stream_id, TrackKind track)
	{
		TrackKey key;
		key.stream_id = stream_id;
		key.track = track;
		auto it = tracks.find(key);
		if (it == tracks.end()) {
			it = tracks.emplace(key, std::unique_ptr<TrackState>(new TrackState())).first;
			it->second->track = track;
		}
		return *it->second;
	}

	std::string next_instance_id(TrackState &state, std::uint32_t stream_id)
	{
		++state.instance_seq;
		return std::to_string(stream_id) + "-" + std::to_string(state.instance_seq);
	}

	void close_segment(TrackState &state, CloseReason reason, bool truncated)
	{
		if (!state.open)
			return;
		SegmentWriteStats stats;
		const bool ok = state.writer.close(&stats);
		state.row.sample_count = stats.sample_count;
		state.row.duration_ticks = stats.duration_ticks;
		state.row.bytes = stats.bytes;
		state.row.truncated = truncated || !ok;
		state.row.close_reason = reason;
		// 送信側が IDR の前に SPS と PPS を付けた回数。送信側が送るのはプラグインの起動からの累計だけで、
		// ストリームが消えても減らない (docs/wire_format.md の「facts」)。受信側はセグメントごとの数を知る
		// 手段がないので、セグメントを閉じた時点の累計をそのまま残す。セグメントごとの数は、前の行との差で
		// 求める。
		{
			std::lock_guard<std::mutex> lock(mu);
			state.row.sps_pps_prepended_count = static_cast<std::int64_t>(pending_facts.sps_pps_prepended);
		}
		state.row.pts_periodic_backward =
			backward_is_periodic(state.row.pts_reanchor, state.clock.frame_period_us);
		doc.segments.push_back(state.row);
		append_line(to_jsonl(state.row));

		closed_bytes += stats.bytes;
		state.open = false;
		state.clock = ClockState();
		state.cursor = AudioCursor();
		state.row = LedgerSegmentRow();

		std::lock_guard<std::mutex> lock(mu);
		++snap.segment_count;
		if (snap.open_segment_count > 0)
			--snap.open_segment_count;
		// 開いているセグメントの分も入れる。閉じたセグメントだけでは、切れ目のないセッションで最後までほぼ 0 の
		// ままになる。
		snap.bytes_written = written_bytes_so_far();
	}

	void close_material(TrackState &state)
	{
		if (!state.material_open)
			return;
		SegmentWriteStats stats;
		state.material.close(&stats);
		state.material_row.frame_count = stats.sample_count;
		state.material_row.duration_ticks = stats.duration_ticks;
		doc.blacks.push_back(state.material_row);
		append_line(to_jsonl(state.material_row));
		closed_bytes += stats.bytes;
		state.material_open = false;
		state.material_row = LedgerBlackRow();
	}

	void close_all(CloseReason reason, bool truncated)
	{
		for (auto &entry : tracks) {
			close_material(*entry.second);
			close_segment(*entry.second, reason, truncated);
		}
	}

	// -----------------------------------------------------------------------
	// 映像
	// -----------------------------------------------------------------------

	void handle_video(Event &event)
	{
		// 録画専用のフラグが付いたフレームは黒埋め用のフレームで、デコードも表示もせず、録画だけが受け取る
		// (docs/wire_format.md の「flags」)。
		if (event.header.is_recording_only()) {
			handle_material(event);
			return;
		}

		TrackState *found = find_track(event.header.stream_id, TrackKind::ScreenVideo);
		if (found == nullptr)
			return; // configure に含まれないストリームは録画しない
		TrackState &state = *found;
		touch_record_only(event, state);

		// 黒埋め用のフレームを書いていたなら、通常のフレームが届いた時点でそのファイルを閉じる (黒埋め用は
		// 1 秒分)。
		close_material(state);

		const bool keyframe = event.header.is_keyframe();
		const std::int64_t pts_us = static_cast<std::int64_t>(event.header.timestamp_us);

		std::string incoming_hex;
		std::vector<std::uint8_t> incoming_avcc;
		if (keyframe) {
			const std::optional<std::vector<std::uint8_t>> built =
				build_avcc_from_annex_b(event.payload.data(), event.payload.size());
			if (built) {
				incoming_avcc = *built;
				incoming_hex = to_hex(incoming_avcc);
			}
		}

		ClockDecision decision;
		if (state.open) {
			ClockThresholds thresholds;
			thresholds.push_max_frames = settings.record_clock_push_max_frames;
			thresholds.reanchor_max_ms = settings.record_clock_reanchor_max_ms;
			decision = classify_pts(state.clock, pts_us, thresholds);
		}

		SegmentCutInput cut;
		cut.connected = connected_flag.load();
		cut.present_in_configure = state.present;
		cut.format = state.format;
		cut.clock = decision.step;
		cut.recording_enabled = writer_enabled;
		cut.queue_overflowed = take_overflow(event.header.stream_id);
		if (cut.queue_overflowed)
			state.dropping_until_keyframe = true;

		if (state.open) {
			std::optional<CloseReason> reason = segment_cut_reason(state.open_format, cut);
			// avcC が変わったらセグメントを分ける。mov の extradata は 1 つのファイルの中で
			// 差し替えられない。avcC が変わるのはエンコーダが作り直されたとき (解像度、fps、ストリームの
			// 張り直し) なので、理由は format_changed にする。
			if (!reason && !incoming_hex.empty() && incoming_hex != state.avcc_hex)
				reason = CloseReason::FormatChanged;
			if (reason) {
				close_segment(state, *reason, false);
				if (*reason == CloseReason::FormatChanged || *reason == CloseReason::ClockJump)
					state.dropping_until_keyframe = true;
			}
		}

		if (!state.open) {
			// 最初のキーフレームが届くまで、セグメントのファイルを開かない。mov に書く avcC は、
			// キーフレームの SPS と PPS から作る。
			if (!keyframe || incoming_avcc.empty())
				return;
			if (!open_video_segment(state, event, incoming_avcc, incoming_hex, pts_us))
				return;
			// セグメントの最初のフレームは原点そのものなので、ずれと届いた値も置き直す
			// (advance がこの 3 つを状態にコピーする)。
			decision.step = ClockStep::Forward;
			decision.out_pts_us = pts_us;
			decision.in_pts_us = pts_us;
			decision.offset_us = 0;
		} else if (state.dropping_until_keyframe && !keyframe) {
			return;
		}
		state.dropping_until_keyframe = false;

		const std::vector<std::uint8_t> sample =
			annex_b_to_avcc(event.payload.data(), event.payload.size());
		if (sample.empty())
			return;
		// 書き込みに失敗したセグメントは WriteFailed で閉じる。キューが溢れたときと同じ理由で閉じると、
		// 録画ログを読む側がキューの溢れと区別できない。
		if (!state.writer.write_video(sample.data(), sample.size(), decision.out_pts_us - state.segment_origin_us,
					      keyframe)) {
			close_segment(state, CloseReason::WriteFailed, true);
			return;
		}

		if (decision.step == ClockStep::Pushed)
			++state.row.pts_pushed_count;
		if (decision.step == ClockStep::Reanchored) {
			PtsReanchor reanchor;
			reanchor.delta_us = decision.backward_us;
			reanchor.at_pts_us = pts_us;
			reanchor.at_steady_us = event.steady_us;
			state.row.pts_reanchor.push_back(reanchor);
		}
		state.row.last_pts_us = decision.out_pts_us;
		advance(state.clock, decision);
		bump_written();
	}

	bool open_video_segment(TrackState &state, const Event &event, const std::vector<std::uint8_t> &avcc,
				const std::string &avcc_hex, std::int64_t pts_us)
	{
		if (!ensure_session_open(session, ops, session_base_dir(), writer_enabled, utc_offset(), gate))
			return false;
		// avcC が変わったのはエンコーダが作り直されたためなので、エンコーダのインスタンスの id も新しくする。
		if (avcc_hex != state.avcc_hex || state.encoder_instance_id.empty())
			state.encoder_instance_id = next_instance_id(state, event.header.stream_id);

		const std::string file = segment_file_name(state.user_id, state.track, ++segment_seq);
		SegmentVideoSpec spec;
		spec.avcc = avcc;
		spec.width = state.format.width;
		spec.height = state.format.height;
		spec.fps_num = state.format.fps_num;
		spec.fps_den = state.format.fps_den;
		// 解像度か fps が分からない (0) 間は開かない。0 のまま開くとフレームの間隔が 0 になり、1 フレームの
		// 間隔より小さい後戻りを押し出せずに、置き直しとして扱ってしまう。分かるまでフレームを捨て、
		// configure が値を運んだ後の次のキーフレームから始める。
		if (spec.width <= 0 || spec.height <= 0 || spec.fps_num <= 0 || spec.fps_den <= 0)
			return false;
		if (!state.writer.open_video(join_path(session.materials_dir, file), spec))
			return false;

		state.avcc = avcc;
		state.avcc_hex = avcc_hex;
		state.open_format = state.format;
		state.open = true;
		state.segment_origin_us = pts_us;
		// このエンコーダのインスタンスから、通常のフレームが届いた (guess_material_origin で使う)。セグメントを
		// 開くのは通常のフレームのキーフレームだけなので、ここが、そのインスタンスから届いた最初の通常の
		// フレームである。
		state.instances_with_real_frame.insert(state.encoder_instance_id);

		state.row = LedgerSegmentRow();
		state.row.user_id = state.user_id;
		state.row.kind = stream_kind_of(state.track);
		state.row.stream_id = event.header.stream_id;
		state.row.encoder_instance_id = state.encoder_instance_id;
		state.row.file = file;
		state.row.media = MediaKind::Video;
		state.row.avcc_hex = avcc_hex;
		state.row.width = spec.width;
		state.row.height = spec.height;
		state.row.fps_num = spec.fps_num;
		state.row.fps_den = spec.fps_den;
		state.row.timescale = state.writer.timescale();
		state.row.first_pts_us = pts_us;
		state.row.first_pts_steady_us = event.steady_us;
		state.row.first_pts_wall_us = event.wall_us;
		state.row.starts_with_idr = true;

		state.clock = ClockState();
		state.clock.frame_period_us = frame_period_us_of(spec.fps_num, spec.fps_den);

		note_person(state.user_id, state.track);
		append_open_row(state.row);
		std::lock_guard<std::mutex> lock(mu);
		++snap.open_segment_count;
		snap.session_dir = session.session_dir;
		return true;
	}

	// 黒埋め用のフレームが、どのタイミングで作られたものかを推定する (録画ログの black の行にある `origin`)。
	// ワイヤー形式はタイミングを運ばない (ヘッダーにあるのは録画専用のフラグだけ) ので、届いた順と時刻から
	// 推定する。
	// 次の順に判定する。
	//   (1) 録画を ON にしてから kMaterialAfterEnableUs 以内なら RecordingTurnedOn (ON を受けて、別のインスタンスで
	//       作られたもの)。これを最初に見るのは、このフレームが、もう通常のフレームを送っているインスタンスと同じ
	//       avcC で届くことがあり (送信側は保存しておいた設定を使う)、そのときは (2) と区別できないためである。
	//   (2) そのインスタンスから通常のフレームがもう届いていれば EncoderTeardown (エンコーダを閉じるときに
	//       送るもの)。
	//   (3) どちらでもなければ EncoderCreated (エンコーダを作った直後、最初の通常のフレームより前に送るもの)。
	// 外れることがある。ON の直後に画面共有をやめた人の EncoderTeardown と、ON の直後に作り直されたエンコーダの
	// EncoderCreated は、どちらも (1) になる。録画ログを読む側は、この値を推定として扱う。
	MaterialOrigin guess_material_origin(const TrackState &state, const std::string &instance,
					     std::int64_t arrived_steady_us) const
	{
		if (session.open && session.started_at_steady_us > 0 &&
		    arrived_steady_us - session.started_at_steady_us <= kMaterialAfterEnableUs)
			return MaterialOrigin::RecordingTurnedOn;
		if (state.instances_with_real_frame.count(instance) != 0)
			return MaterialOrigin::EncoderTeardown;
		return MaterialOrigin::EncoderCreated;
	}

	// 黒埋め用のフレームを書く。録画専用のフラグが付いたフレームは、録画だけが受け取る。
	void handle_material(Event &event)
	{
		TrackState *found = find_track(event.header.stream_id, TrackKind::ScreenVideo);
		if (found == nullptr)
			return;
		TrackState &state = *found;
		{
			std::lock_guard<std::mutex> lock(mu);
			++snap.material_frames_received;
		}

		if (!state.material_open) {
			if (!event.header.is_keyframe())
				return;
			const std::optional<std::vector<std::uint8_t>> avcc =
				build_avcc_from_annex_b(event.payload.data(), event.payload.size());
			if (!avcc)
				return;
			const std::string hex = to_hex(*avcc);
			if (!ensure_session_open(session, ops, session_base_dir(), writer_enabled, utc_offset(), gate))
				return;
			// 黒埋め用のフレームは、エンコーダのインスタンスごとに 1 回だけ書く (先に届いたものだけを
			// 使う)。
			std::string instance = state.encoder_instance_id;
			if (instance.empty() || hex != state.avcc_hex)
				instance = next_instance_id(state, event.header.stream_id);
			if (state.material_done.count(instance) != 0)
				return;

			const std::string file = black_file_name(state.user_id, instance);
			SegmentVideoSpec spec;
			spec.avcc = *avcc;
			spec.width = state.format.width;
			spec.height = state.format.height;
			spec.fps_num = state.format.fps_num;
			spec.fps_den = state.format.fps_den;
			// 解像度は、このフレームの SPS から取る。EncoderCreated のフレームは新しい解像度の
			// `configure` より先に届くことがあり、そのときは保存している解像度が前のままである。
			if (const std::optional<SpsDimensions> dims =
				    sps_dimensions_from_annex_b(event.payload.data(), event.payload.size())) {
				spec.width = dims->width;
				spec.height = dims->height;
			}
			// 解像度か fps が分からない間は開かない (open_video_segment と同じ理由)。黒埋め用のフレームは
			// `configure` より先に届くことがある (EncoderCreated) ので、ここで戻ったインスタンスは
			// 黒埋め用のフレームを持てない。
			if (spec.width <= 0 || spec.height <= 0 || spec.fps_num <= 0 || spec.fps_den <= 0)
				return;
			if (!state.material.open_video(join_path(session.materials_dir, file), spec))
				return;

			state.material_open = true;
			state.material_done.insert(instance);
			state.material_row = LedgerBlackRow();
			state.material_row.user_id = state.user_id;
			state.material_row.encoder_instance_id = instance;
			state.material_row.file = file;
			state.material_row.avcc_hex = hex;
			state.material_row.width = spec.width;
			state.material_row.height = spec.height;
			state.material_row.fps_num = spec.fps_num;
			state.material_row.fps_den = spec.fps_den;
			state.material_row.origin = guess_material_origin(state, instance, event.steady_us);
			state.material_origin_us = static_cast<std::int64_t>(event.header.timestamp_us);
		}

		const std::vector<std::uint8_t> sample =
			annex_b_to_avcc(event.payload.data(), event.payload.size());
		if (sample.empty())
			return;
		const std::int64_t pts_us =
			static_cast<std::int64_t>(event.header.timestamp_us) - state.material_origin_us;
		state.material.write_video(sample.data(), sample.size(), pts_us > 0 ? pts_us : 0,
					   event.header.is_keyframe());
	}

	// -----------------------------------------------------------------------
	// 音声
	// -----------------------------------------------------------------------

	void handle_audio(Event &event)
	{
		// 録画専用のフラグは映像のフレームにだけ付けるので、音声に付いて届いたものは捨てて数える。
		if (event.header.is_recording_only()) {
			std::lock_guard<std::mutex> lock(mu);
			++snap.material_audio_discarded;
			return;
		}

		TrackState *found = find_track(event.header.stream_id, TrackKind::ScreenAudio);
		if (found == nullptr)
			found = find_track(event.header.stream_id, TrackKind::Voice);
		if (found == nullptr)
			return;
		TrackState &state = *found;
		touch_record_only(event, state);

		const int channels = 2; // 2 ch に固定 (docs/wire_format.md の「音声のペイロード」)
		const std::int64_t samples = static_cast<std::int64_t>(event.payload.size() / (channels * 4));
		if (samples <= 0)
			return;
		const std::int64_t pts_us = static_cast<std::int64_t>(event.header.timestamp_us);
		// payload は float の境界に並んでいるとは限らないので、memcpy で移す。
		audio_in.resize(static_cast<std::size_t>(samples) * channels);
		std::memcpy(audio_in.data(), event.payload.data(), audio_in.size() * sizeof(float));

		SegmentCutInput cut;
		cut.connected = connected_flag.load();
		cut.present_in_configure = state.present;
		cut.format = state.format;
		cut.clock = ClockStep::Forward;
		cut.recording_enabled = writer_enabled;
		cut.queue_overflowed = take_overflow(event.header.stream_id);
		if (state.open) {
			const std::optional<CloseReason> reason = segment_cut_reason(state.open_format, cut);
			if (reason)
				close_segment(state, *reason, false);
		}

		if (!state.open && !open_audio_segment(state, event, pts_us))
			return;

		AudioPlacement placement = place_audio(state.cursor, pts_us, state.segment_origin_us, audio_in.data(),
						       static_cast<std::size_t>(samples), &audio_out);
		if (placement.backward) {
			// 書いたサンプルより前に置き直す (タイムスタンプがしきい値を超えて戻った)。書いたサンプルは消せないので、
			// 映像の大きな後戻りと同じ理由でセグメントを閉じ、このチャンクから新しいセグメントを始める。セグメントの
			// 位置は録画ファイルの作成が先頭のタイムスタンプで決めるので、重なった分は後のセグメントで上書きされる。
			close_segment(state, CloseReason::ClockJump, false);
			if (!open_audio_segment(state, event, pts_us))
				return;
			placement = place_audio(state.cursor, pts_us, state.segment_origin_us, audio_in.data(),
						static_cast<std::size_t>(samples), &audio_out);
		}
		if (placement.silence_samples > 0 && !state.writer.write_audio_silence(placement.silence_samples)) {
			close_segment(state, CloseReason::WriteFailed, true);
			return;
		}
		if (placement.frames > 0 &&
		    !state.writer.write_audio(audio_out.data(), audio_out.size() * sizeof(float),
					      static_cast<std::int64_t>(placement.frames))) {
			close_segment(state, CloseReason::WriteFailed, true);
			return;
		}
		if (placement.resynced) {
			PtsReanchor reanchor;
			reanchor.delta_us = placement.drift_us >= 0 ? placement.drift_us : -placement.drift_us;
			reanchor.at_pts_us = pts_us;
			reanchor.at_steady_us = event.steady_us;
			state.row.pts_reanchor.push_back(reanchor);
		}
		state.row.last_pts_us = audio_cursor_end_us(state.cursor, state.segment_origin_us);
		bump_written();
	}

	bool open_audio_segment(TrackState &state, const Event &event, std::int64_t pts_us)
	{
		if (!ensure_session_open(session, ops, session_base_dir(), writer_enabled, utc_offset(), gate))
			return false;
		const std::string file = segment_file_name(state.user_id, state.track, ++segment_seq);
		SegmentAudioSpec spec;
		spec.sample_rate = 48000; // 48 kHz に固定 (docs/wire_format.md の「音声のペイロード」)
		spec.channels = 2;
		if (!state.writer.open_audio(join_path(session.materials_dir, file), spec))
			return false;

		state.open = true;
		state.open_format = state.format;
		state.segment_origin_us = pts_us;
		// 書く位置の追従は、配信の AudioMixer と同じ設定の値で決める (align/audio_drift.h)。
		align::AudioDriftConfig drift = align::make_audio_drift_config(
			settings.audio_resync_ms, settings.audio_drift_max_ppm, settings.audio_drift_time_constant_ms);
		drift.sample_rate = spec.sample_rate;
		state.cursor = AudioCursor(drift);

		state.row = LedgerSegmentRow();
		state.row.user_id = state.user_id;
		state.row.kind = stream_kind_of(state.track);
		state.row.stream_id = event.header.stream_id;
		state.row.encoder_instance_id = state.encoder_instance_id;
		state.row.file = file;
		state.row.media = MediaKind::Audio;
		state.row.timescale = spec.sample_rate;
		state.row.first_pts_us = pts_us;
		state.row.first_pts_steady_us = event.steady_us;
		state.row.first_pts_wall_us = event.wall_us;

		note_person(state.user_id, state.track);
		append_open_row(state.row);
		std::lock_guard<std::mutex> lock(mu);
		++snap.open_segment_count;
		snap.session_dir = session.session_dir;
		return true;
	}

	// -----------------------------------------------------------------------
	// configure / 接続
	// -----------------------------------------------------------------------

	void handle_configure(const std::vector<pipeline::StreamFormat> &formats,
			      const std::map<std::string, std::string> &names)
	{
		display_names = names;
		std::set<std::uint32_t> live;
		for (const pipeline::StreamFormat &format : formats) {
			live.insert(format.stream_id);
			int fps_num = 0;
			int fps_den = 0;
			fps_to_rational(format.fps, &fps_num, &fps_den);
			StreamFormatMemo memo;
			memo.width = format.width;
			memo.height = format.height;
			memo.fps_num = fps_num;
			memo.fps_den = fps_den;

			if (format.screen) {
				for (const TrackKind track : {TrackKind::ScreenVideo, TrackKind::ScreenAudio}) {
					TrackState &state = ensure_track(format.stream_id, track);
					state.user_id = format.user_id;
					state.present = true;
					state.format = track == TrackKind::ScreenVideo ? memo : StreamFormatMemo();
				}
			} else {
				TrackState &state = ensure_track(format.stream_id, TrackKind::Voice);
				state.user_id = format.user_id;
				state.present = true;
				state.format = StreamFormatMemo();
			}
		}

		// configure から消えたストリームのセグメントは閉じる。
		for (auto &entry : tracks) {
			if (live.count(entry.first.stream_id) != 0)
				continue;
			entry.second->present = false;
			close_material(*entry.second);
			close_segment(*entry.second, CloseReason::StreamGone, false);
		}

		// 最後の到着時刻も、configure から消えたストリームの分を消す。ドックはこの時刻で、録画のためだけに受け取って
		// いる人を数える (ledger/rules.cpp の recording_only_person_count)。枠から外した人の画面は、外した時点で
		// 録画をやめるので、時刻を残すと、映像が止まったとみなすしきい値の間だけ、その人を誤って数える。
		std::lock_guard<std::mutex> lock(mu);
		for (auto it = record_only.begin(); it != record_only.end();)
			it = live.count(it->first) == 0 ? record_only.erase(it) : std::next(it);
	}

	void handle_disconnected()
	{
		close_all(CloseReason::Disconnected, false);
	}

	// -----------------------------------------------------------------------
	// 空き容量の監視
	// -----------------------------------------------------------------------

	void check_space(bool force)
	{
		if (!gate(writer_enabled) || !session.open || !ops.free_space_bytes || !ops.now_steady_us)
			return;
		const std::int64_t now = ops.now_steady_us();
		const std::int64_t interval_us =
			static_cast<std::int64_t>(settings.record_space_check_interval_s > 0
							  ? settings.record_space_check_interval_s
							  : 1) *
			1000000;
		if (!force && last_space_check_us != 0 && now - last_space_check_us < interval_us)
			return;
		last_space_check_us = now;

		const std::optional<std::uint64_t> free = ops.free_space_bytes(session.session_dir);
		const std::int64_t elapsed = now - session.started_at_steady_us;
		// 開いているセグメントの分も数える。録画ログの行はセグメントを閉じたときにしか載らないので、切れ目の
		// ないセッションでは `bytes_per_second(doc, ...)` がずっと 0 を返し、監視が 1 度も働かない。
		const std::int64_t rate = bytes_per_second_of(written_bytes_so_far(), elapsed);
		std::optional<std::int64_t> left;
		if (free)
			left = remaining_seconds(*free, rate);

		std::vector<FinalizeBranch> branches;
		for (const OutputKey &key : finalize_outputs(doc))
			branches.push_back(decide_branch(collect_branch_input(doc, key.user_id, key.kind)));
		const std::uint64_t need = estimate_finalize_need_bytes(doc, branches);

		{
			std::lock_guard<std::mutex> lock(mu);
			snap.free_space_bytes = free;
			snap.remaining_seconds = left;
			snap.space_checked_at_steady_us = now;
			snap.finalize_need_bytes = need;
			// 画面に表示する書いたバイト数もここで更新する (close_segment と同じ値)。セグメントが長く
			// 閉じないセッションでも、監視の間隔ごとに新しくなる。
			snap.bytes_written = written_bytes_so_far();
		}

		// 現在の書き込みの速さで残り 1 秒も持たなくなったら、録画を止める。0 バイトまで待つと、最後の書き込みが
		// 途中で切れる。空き容量を読めなかったときは止めない。
		// 録画ファイルの作成に要る量 (上の need) を残しては止めない。残して止めると、録画できる長さが毎回その分 (生の
		// PCM は 1 時間で十数 GB) 短くなり、空きがあるのに配信の途中で録画が止まる。空き容量が尽きて止まった
		// セッションはその場では完成せず、録画ログが Failed になるが、セグメントのファイルは残るので、空きを
		// 作ってから resume_pending で作成できる。画面の文言 (cordmux.dock.space_low.text と
		// cordmux.dock.record_stopped_space.text) も、止めた時点までの録画ファイルを作成するとは約束しない。
		// 自分で止めたセッションであることを、スナップショットの self_stopped_reason に残す。
		if (left && *left <= 0)
			finish_session(StopReason::SpaceExhausted, true);
	}

	// このセッションで今までに書いたバイト数 (閉じたセグメントと、開いているセグメントと黒埋め用のフレームの
	// ファイル)。
	std::int64_t written_bytes_so_far() const
	{
		std::int64_t total = closed_bytes;
		for (const auto &entry : tracks) {
			if (entry.second->open)
				total += entry.second->writer.stats().bytes;
			if (entry.second->material_open)
				total += entry.second->material.stats().bytes;
		}
		return total;
	}

	// -----------------------------------------------------------------------
	// セッションの開始と終了
	// -----------------------------------------------------------------------

	// ON をキューに積んだ後に OFF が届いていても、セッションは開く。キューの順が録画中に届いたデータを決める
	// (writer_enabled のコメント) ので、ON と OFF の間に積まれたデータは、書き込むスレッドが OFF の後に取り出しても
	// 録画中に届いたものである。取り出した時点の段階とフラグを見て開かないようにすると、空のセッションの
	// フォルダーは残らないが、ON の直後に OFF にしたセッションのデータがすべて失われる。段階だけは OFF の側が
	// 決めるので、下では Starting のときだけ変える。
	void start_session()
	{
		writer_enabled = true;
		if (session.open)
			return;
		if (!ensure_session_open(session, ops, session_base_dir(), writer_enabled, utc_offset(), gate)) {
			// 保存先にフォルダーを作れないか、録画ログを開けなかった。保存先に書けるかは、ON にしたときに
			// 実際に開けたかで決める。書けるかを確認する確実な方法は実際に書くことで、その最初の 1 回が
			// セッションの開始なので、別に試し書きはしない。
			// 失敗を画面に表示する。ここで何も表示せずに戻ると、ボタンだけが録画中の表示になり、何も
			// 録画されないままメッセージも出ない。このセッションは始めず、別の場所にも移さずに、録画を OFF に
			// 戻す。
			// 入力がデータをコピーし続けないように、ON/OFF のフラグも false にする。後始末 (送信側への
			// 通知と書き込むスレッドの join) は、自分で止まったセッションと同じく、スナップショットを読む
			// Pipeline::poll_recording_self_stop が行う。
			writer_enabled = false;
			std::lock_guard<std::mutex> lock(mu);
			// 書けなかった場所は、開いている間に OFF が届いていても残す。利用者が次に ON にする前に
			// メッセージられる。
			snap.save_dir_writable = false;
			snap.unwritable_dir = session.failed_root;
			// フラグと段階を戻すのは、この ON がまだ有効なときだけである。開いている間に OFF が
			// 届いていたら、フラグはもう false で、段階 (Stopping) は書き込むスレッドを join した側が
			// Idle にする。この場合に StartFailed にすると、利用者が止めた後に、もう望んでいない操作の
			// 失敗を伝えることになる。
			if (snap.phase == RecordPhase::Starting) {
				enabled_flag = false;
				snap.enabled = false;
				set_phase_locked(RecordPhase::StartFailed);
			}
			return;
		}
		// 録画ログに最初の行を書く前に、ロックファイルを持つ。録画ログが空の間はどのプロセスの
		// 集計もそのセッションを飛ばすので、行が見えるときにはロックファイルが先にある。持てなくても録画は
		// 止めない。この起動の中では recording_session_dir で自分のセッションを除くが、ほかの OBS からは
		// 中断したセッションに見えることがあり、その OBS で再開すると、録画中の録画ログに録画ファイルの作成の行が混じる。
		take_session_lock();
		doc = LedgerDocument();
		doc.session.schema = kLedgerSchema;
		doc.session.session_id = session.session_id;
		doc.session.started_at_wall_us = session.started_at_wall_us;
		doc.session.started_at_steady_us = session.started_at_steady_us;
		doc.session.stopped_reason = StopReason::StillRunning;
		doc.session.finalize_state = FinalizeState::NotStarted;
		doc.session.ffmpeg_version = record_ffmpeg_version();
		// person_index は doc.people の添字を持つので、doc と一緒に空にする。残すと、前のセッションにもいた人の
		// セグメントを開いたときに、空の doc.people を古い添字で読む。
		person_index.clear();
		segment_seq = 0;
		closed_bytes = 0;
		last_space_check_us = 0;
		write_session_row();

		const std::string base_dir_now = session_base_dir();
		{
			std::lock_guard<std::mutex> lock(mu);
			snap.save_dir_writable = true; // 実際に開けた
			snap.unwritable_dir.clear();
			recording_session_dir = session.session_dir; // 自分が録画中のセッションは数えない
			// Recording にするのは、開いている間に OFF が届いていないときだけである。届いていたら段階は
			// Stopping のままで、続く OFF がこのセッションを閉じる (セッションは実際に開いたので、下の
			// スナップショットは更新する)。
			set_phase_locked_if(RecordPhase::Starting, RecordPhase::Recording);
			// 接続が切れていた期間はセッションごとに数え、次のセッションを始めたら空にする。
			snap.gaps.clear();
			gap_started_wall_us.reset();
			// このセッションを始めた保存先も、作成待ちを探す保存先に入れる。この起動の中で保存先を
			// 変えても、打ち切られたこのセッションを resume_pending で見つけられる。
			add_search_dir_locked(base_dir_now);
			// 前のセッションを自分で止めた理由は、新しいセッションが始まったところで消す。それまでは、
			// 画面に止まった理由を表示し続ける。
			snap.self_stopped_reason.reset();
			snap.started_at_wall_us = session.started_at_wall_us;
			snap.started_at_steady_us = session.started_at_steady_us;
			snap.session_dir = session.session_dir;
			snap.segment_count = 0;
			snap.open_segment_count = 0;
			snap.bytes_written = 0;
			// 前のセッションの録画ファイルの作成がまだ続いているときは、その進み具合を消さない (消すと、次の進み具合が
			// 届くまで録画ファイルの作成の行が消える)。終わった結果だけを新しいセッションの始まりで消す。
			if (snap.finalize.state != FinalizeState::Running)
				snap.finalize = FinalizeProgress();
		}
		// このセッションの保存先を一覧に追加する。mu を放した後に、count_mu を取らずに呼ぶ (ロックの順は
		// count_mu -> mu で、集計も呼ばない)。recording_session_dir を設定した後に追加するので、この後に
		// 始まった集計は自分のセッションとして扱い、前に始まった集計は、追加した後の保存先として
		// 一覧から外さない。
		remember_session_base_dir(base_dir_now);
	}

	// セグメントを閉じて録画ログに最後の行を書き、録画ファイルの作成のジョブを積む。`self_stopped` が真なら、自分で止めた
	// セッションとしてスナップショットに理由を残す (後始末は、スナップショットを定期的に読む側が行う)。
	void finish_session(StopReason reason, bool self_stopped = false)
	{
		writer_enabled = false;
		if (!session.open)
			return;
		close_all(CloseReason::RecordingToggled, false);
		note_final_names();
		doc.session.ended_at_steady_us = ops.now_steady_us ? ops.now_steady_us() : 0;
		doc.session.stopped_reason = reason;
		doc.session.finalize_state = FinalizeState::Running;
		write_session_row();

		FinalizeJob job;
		job.doc = doc;
		job.session_dir = session.session_dir;
		job.materials_dir = session.materials_dir;
		job.values = active_values; // セッションを始めた時点の値
		job.ledger = session.ledger;
		job.repair = false;
		// 録画ログとロックファイルのハンドルは、ここでは閉じずに録画ファイルの作成のジョブに渡す。録画ファイルの作成は書き込む
		// スレッドとは別のスレッドで実行する。書き込むスレッドで録画ファイルを作成すると、OFF の直後に ON にしたセッションの
		// データを、録画ファイルの作成が終わるまで 1 つも書けない (ProRes への再エンコードは、実測で実時間の最大
		// 4.2 倍かかる)。次のセッションは新しい録画ログを開く。
		session.ledger = kInvalidFile;
		session.open = false;
		job.lock = session_lock;
		session_lock = kInvalidFile;

		{
			std::lock_guard<std::mutex> lock(mu);
			// ON/OFF のフラグを false にするのは、自分で止まったセッションのときだけである。利用者の OFF
			// (Disable) と OBS の終了 (Stop) は、積む側が積む前に false にしている。ここで重ねて
			// false にすると、書き込むスレッドが古い OFF を処理する前に新しい ON が true にしたフラグを
			// false に戻してしまい、何も録画されないセッションになる。OFF のイベントに世代の番号を
			// 持たせる方法もあるが、false にする必要があるのは自分で止まったセッションだけなので、番号は
			// 持たない。
			if (self_stopped) {
				enabled_flag = false;
				snap.enabled = false;
				snap.self_stopped_reason = reason;
				// 自分で止まったセッションは Idle にし、self_stopped_reason を残す。利用者が OFF にした
				// セッションは、書き込むスレッドを join し終わるまで Stopping のままにする (join した
				// 側が変える)。空き容量が尽きたのと同時に利用者が OFF にしていたら
				// Stopping のままにする (理由は事実なので残す)。
				set_phase_locked_if(RecordPhase::Recording, RecordPhase::Idle);
			}
			// 接続が切れたままセッションが終わったら、終わりまでを切れていた期間として閉じる。
			close_gap_locked();
			snap.finalize = FinalizeProgress();
			snap.finalize.state = FinalizeState::Running;
		}
		const std::string closed_dir = job.session_dir;
		enqueue_finalize(std::move(job));
		// キューに積んだので、ここからは finalize_inflight が自分のセッションを集計から除く。
		std::lock_guard<std::mutex> lock(mu);
		if (recording_session_dir == closed_dir)
			recording_session_dir.clear();
	}

	// このセッションのロックファイルを持つ。書き込むスレッドだけが呼ぶ。新しく作ったセッションのフォルダーの
	// 中なので、ほかに持つプロセスはいない。持てなければ、ロックファイルなしで続ける。
	void take_session_lock()
	{
		release_lock(session_lock);
		if (!ops.hold_lock || session.materials_dir.empty())
			return;
		bool held_elsewhere = false;
		session_lock = ops.hold_lock(join_path(session.materials_dir, kSessionLockFileName), &held_elsewhere);
	}

	// ロックファイルを閉じる (OS がファイルも削除する)。削除を許さない保存先ではファイルが残るが、残ったファイルは
	// だれも持っていないと判定するので、集計は変わらない。
	void release_lock(FileHandle &lock)
	{
		if (lock != kInvalidFile && ops.close)
			ops.close(lock);
		lock = kInvalidFile;
	}

	// -----------------------------------------------------------------------
	// 録画ファイルの作成のスレッド
	// -----------------------------------------------------------------------

	// 書き込むスレッド (セッションを閉じたとき) と UI のスレッド (resume_pending) の両方から呼ばれる。
	// 録画ファイルの作成のスレッドを起動するのも finalize_mu の下で行う。ロックの外で joinable を見てから代入すると、録画ファイルの作成の
	// スレッドがまだないときに 2 つのスレッドが同時に起動し、動いている std::thread に代入して
	// std::terminate になる。
	// ロックの下で起動しても、新しいスレッドはロックが放されるまでキューを読まないだけなので、待ちは生まれない。
	// 再開のジョブ (録画ログのハンドルを持たない) は、同じセッションのフォルダーがキューにあるか作成中なら、
	// 積まずに false を返す。セッションを閉じた直後のジョブはハンドルを持つので、必ず積む (断るとハンドルが
	// 閉じられずに残る)。
	bool enqueue_finalize(FinalizeJob &&job)
	{
		{
			std::lock_guard<std::mutex> lock(finalize_mu);
			if (job.ledger == kInvalidFile && finalize_inflight.count(job.session_dir) > 0)
				return false;
			finalize_inflight.insert(job.session_dir);
			finalize_queue.push_back(std::move(job));
			if (!finalize_thread.joinable())
				finalize_thread = std::thread([this]() { finalize_loop(); });
		}
		finalize_cv.notify_one();
		return true;
	}

	void finalize_loop()
	{
		for (;;) {
			FinalizeJob job;
			{
				std::unique_lock<std::mutex> lock(finalize_mu);
				finalize_cv.wait(lock, [this]() { return !finalize_queue.empty() || finalize_quit; });
				if (finalize_queue.empty())
					return;
				job = std::move(finalize_queue.front());
				finalize_queue.pop_front();
			}
			finalize_busy = true;
			run_job(job);
			finalize_busy = false;
		}
	}

	void run_job(FinalizeJob &job)
	{
		// 前のセッションの録画ログに追記するときだけ、切り詰めずに開く (record_writer.h の
		// RawFile::open_append)。
		// RecordPlatformOps::open_file は、ファイルが既にあれば切り詰める。
		RawFile reopened;
		if (job.ledger == kInvalidFile && !job.ledger_path.empty()) {
			reopened.open_append(job.ledger_path);
			// 途中で切れた最後の行の続きに書かない (FinalizeJob::ledger_needs_newline)。
			if (job.ledger_needs_newline && reopened.is_open())
				reopened.append("\n", 1);
		}

		FinalizeRun run;
		run.ops = ops;
		run.options.ffmpeg_path = ops.ffmpeg_path ? ops.ffmpeg_path() : std::string();
		run.options.session_dir = job.session_dir;
		run.options.materials_dir = job.materials_dir;
		// セッションを始めた時点の値で録画ファイルを作成する。録画ファイルの作成の時点の settings を読むと、作成を待っている前の
		// セッションに、後から変えた値が及ぶ。
		run.options.output_pcm = job.values.output_pcm;
		run.options.output_name_format = job.values.output_name_format;
		run.options.screen_kind_word = job.values.screen_kind_word;
		run.options.voice_kind_word = job.values.voice_kind_word;
		run.options.finalize_stall_s = finalize_stall_s.load();
		run.options.repair_truncated = job.repair;
		run.doc = &job.doc;
		run.append_line = [this, &job, &reopened](const std::string &line) {
			std::string text = line;
			if (text.empty() || text.back() != '\n')
				text.push_back('\n');
			if (job.ledger != kInvalidFile && ops.append)
				ops.append(job.ledger, text.data(), text.size());
			else if (reopened.is_open())
				reopened.append(text.data(), text.size());
		};
		run.on_progress = [this](const FinalizeProgress &progress) {
			std::lock_guard<std::mutex> lock(mu);
			snap.finalize = progress;
		};
		run.should_stop = [this]() { return finalize_cancel.load(); };
		run.set_child_log = [this](const std::string &path) { *child_log = path; };

		const FinalizeProgress progress = run_finalize(run);

		job.doc.session.finalize_state = progress.state;
		{
			std::string text = to_jsonl(job.doc.session);
			text.push_back('\n');
			if (job.ledger != kInvalidFile && ops.append)
				ops.append(job.ledger, text.data(), text.size());
			else if (reopened.is_open())
				reopened.append(text.data(), text.size());
		}
		if (job.ledger != kInvalidFile && ops.close)
			ops.close(job.ledger);
		job.ledger = kInvalidFile;
		reopened.close();
		// ロックファイルを閉じるのは、録画ログの最後の行を書いて閉じた後にする。ほかのプロセスの集計は
		// ロックファイルを確認してから録画ログを読み直すので、ロックファイルが消えたときには、録画ログはもう
		// Done・Pending・Failed のどれかを示している。
		release_lock(job.lock);

		// 録画ファイルを作成し終えたセッションを作成中から外してから数え直す。失敗したセッションは録画ログが
		// Failed なので、ここで作成待ちに戻る。
		{
			std::lock_guard<std::mutex> lock(finalize_mu);
			const auto it = finalize_inflight.find(job.session_dir);
			if (it != finalize_inflight.end())
				finalize_inflight.erase(it);
		}
		// 作成待ちの数は、毎回フォルダーから数え直す。終わった分を引き忘れると、画面が再開のボタンを
		// 表示し続ける。
		// 録画ファイルの作成の結果も、同じロックの中でスナップショットに入れる。別々に入れると、失敗のメッセージと、再開の
		// ボタンを押せるかが、別々の周期で画面に反映される。
		recount_pending(&progress);
	}

	// -----------------------------------------------------------------------
	// 書き込むスレッド
	// -----------------------------------------------------------------------

	void touch_record_only(const Event &event, const TrackState &state)
	{
		pipeline::RecordOnlyFrameClock clock;
		clock.stream_id = event.header.stream_id;
		clock.user_id = state.user_id;
		clock.screen = state.track != TrackKind::Voice;
		clock.last_frame_steady_us = event.steady_us;
		clock.last_frame_wall_us = event.wall_us;
		std::lock_guard<std::mutex> lock(mu);
		record_only[event.header.stream_id] = clock;
	}

	void bump_written()
	{
		std::lock_guard<std::mutex> lock(mu);
		++snap.frames_written;
	}

	void writer_loop()
	{
		for (;;) {
			Event event;
			{
				std::unique_lock<std::mutex> lock(mu);
				cv.wait_for(lock, std::chrono::milliseconds(250),
					    [this]() { return !queue.empty() || quit; });
				if (queue.empty()) {
					if (quit)
						break;
					lock.unlock();
					check_space(false);
					continue;
				}
				event = std::move(queue.front());
				queue.pop_front();
			}

			switch (event.kind) {
			case EventKind::Enable:
				// OFF の間に届いていた `configure` を、データより先にここで適用する。
				if (!event.formats.empty())
					handle_configure(event.formats, event.names);
				// このセッションの値 (保存先、PCM の形、名前の語) は、ON にした時点のコピーを使う。
				if (!session.open)
					active_values = event.values;
				start_session();
				break;
			case EventKind::Disable:
				finish_session(StopReason::TurnedOff);
				break;
			case EventKind::Stop:
				finish_session(event.stop_reason);
				break;
			case EventKind::Video:
				handle_video(event);
				break;
			case EventKind::Audio:
				handle_audio(event);
				break;
			case EventKind::Configure:
				// データと同じキューで適用する。順番を崩すと、先に積まれたデータが新しい解像度の
				// セグメントに書かれ、解像度が変わった所の切れ目が消える。
				handle_configure(event.formats, event.names);
				break;
			case EventKind::Connected:
				break;
			case EventKind::Disconnected:
				handle_disconnected();
				break;
			}
			check_space(false);
		}
	}

	// `writer_thread` を読み書きするのは、UI のスレッド (start_writer と、set_enabled(false) と stop の join) と、
	// 止める処理のスレッド (join) だけである。UI のスレッドは止める処理のスレッドを join してから触るので、両者が
	// 同時に触ることはない。ほかのスレッドには、mu の下の `writer_alive` で答える。
	void start_writer()
	{
		if (writer_thread.joinable())
			return;
		{
			std::lock_guard<std::mutex> lock(mu);
			quit = false;
			writer_alive = true;
		}
		writer_thread = std::thread([this]() { writer_loop(); });
	}

	void join_writer()
	{
		if (!writer_thread.joinable())
			return;
		{
			std::lock_guard<std::mutex> lock(mu);
			quit = true;
		}
		cv.notify_all();
		writer_thread.join();
		std::lock_guard<std::mutex> lock(mu);
		writer_alive = false;
	}

	// -----------------------------------------------------------------------
	// 前のセッションの作成待ち
	// -----------------------------------------------------------------------

	// 前のセッションを作成待ちとして数える理由。
	enum class SessionClaim {
		// 数えない (完成した、録画ログの `schema` に対応していない、ほかのプロセスが使用中、中断したが
		// 読めるものがない)
		None,
		Pending,     // 録画ファイルの作成を打ち切った (OBS の終了で、待つ上限に達した)
		Failed,      // 録画ファイルの作成に失敗した。セグメントのファイルは残っているので、もう一度作成できる
		Interrupted, // 録画か録画ファイルの作成の途中で OBS か PC が止まった (録画ログが NotStarted か Running で終わる)
	};

	// 録画ログの値だけで決めた理由 (ロックファイルは見ない)。
	static SessionClaim ledger_claim(const LedgerDocument &doc)
	{
		if (!ledger_schema_supported(doc.session.schema))
			return SessionClaim::None;
		switch (doc.session.finalize_state) {
		case FinalizeState::Pending:
			return SessionClaim::Pending;
		case FinalizeState::Failed:
			return SessionClaim::Failed;
		case FinalizeState::NotStarted:
		case FinalizeState::Running:
			return SessionClaim::Interrupted;
		case FinalizeState::Done:
			return SessionClaim::None;
		}
		return SessionClaim::None;
	}

	static std::string session_lock_path(const SessionLayout &layout)
	{
		return join_path(layout.materials_dir, kSessionLockFileName);
	}

	// 録画ログがないと分かったか。OS が録画ログはないと答え、しかもセッションのフォルダーがフォルダーとして
	// 見えたときだけ true を返す。ネットワークの場所が切れると、どちらもないと答えることがあるので、フォルダーも
	// 確認する。
	static bool ledger_known_absent(const std::string &ledger_path, const std::string &session_dir)
	{
		std::error_code ec;
		const std::filesystem::file_status ledger = std::filesystem::status(std::filesystem::u8path(ledger_path), ec);
		if (ledger.type() != std::filesystem::file_type::not_found)
			return false;
		std::error_code dir_ec;
		const std::filesystem::file_status dir =
			std::filesystem::status(std::filesystem::u8path(session_dir), dir_ec);
		return !dir_ec && dir.type() == std::filesystem::file_type::directory;
	}

	// 中断したセッションに読めるサンプルがあるかを返す。閉じたセグメント (長さを示す) が 1 つでもあれば true を
	// 返す。
	// 閉じていないセグメントと書けずに閉じたセグメントは、ファイルを先頭から読み、読めるサンプルが 1 つでもあれば
	// true を返す。録画ファイルの作成が修復するときと同じ判定 (record_writer.h の copy_readable_part) で、数えるだけで何も
	// 書かない。
	// 読めるサンプルが 1 つもない中断したセッション (録画を始めた直後に止まった) は数えず、メッセージも表示しない。
	// セッションのフォルダーにも録画ログにも何も書かず、削除もしない (集計は読むだけにする)。数えるときに録画ログへ
	// 録画ファイルの作成済みの行を追加して閉じると、録画が OFF の間に保存先へ書くことになり、ほかの OBS が同じセッションを見て
	// いる間に書くことにもなる。
	static bool has_readable_material(const SessionLayout &layout, const LedgerDocument &doc)
	{
		for (const LedgerSegmentRow &row : doc.segments) {
			if (!row.truncated && row.sample_count > 0)
				return true;
		}
		for (const LedgerSegmentRow &row : doc.segments) {
			if (row.truncated && !row.file.empty() &&
			    copy_readable_part(join_path(layout.materials_dir, row.file), std::string(), 1).samples > 0)
				return true;
		}
		return false;
	}

	// このセッションを作成待ちとして数える理由を返す。`first` は先に読んだ録画ログ。`check_lock` が真なら、
	// ロックファイルを確認してから録画ログを読み直して決める。ロックファイルを持つプロセスは録画ログの最後の行を
	// 書いてからロックファイルを閉じるので、ロックファイルがないのを見た後に読んだ録画ログは、そのプロセスが
	// 終えた直後でも、終えた後の値である。逆にそのプロセスがセッションを始めた直後なら、録画ログに最初の行が
	// 見えるときにはロックファイルがもうある (start_session)。
	// `held_elsewhere` を渡したら、ほかのプロセスがロックファイルを持っていたので数えなかったときに true を入れる
	// (その保存先には、録画ファイルの作成の済んでいないセッションがある)。`unreadable` を渡したら、ロックファイルを確認した後に
	// 録画ログを読み直せなかったときに true を入れる (数えないが、済んだか分からないので、一覧の上では録画ファイルの作成の
	// 済んでいないセッションとして扱う)。
	SessionClaim claim_of(const SessionLayout &layout, const LedgerDocument &first, bool check_lock,
			      bool *held_elsewhere = nullptr, bool *unreadable = nullptr) const
	{
		if (held_elsewhere != nullptr)
			*held_elsewhere = false;
		if (unreadable != nullptr)
			*unreadable = false;
		SessionClaim claim = ledger_claim(first);
		if (claim == SessionClaim::None)
			return claim;
		LedgerDocument reread;
		const LedgerDocument *now = &first;
		if (check_lock) {
			if (ops.lock_held && ops.lock_held(session_lock_path(layout))) {
				if (held_elsewhere != nullptr)
					*held_elsewhere = true;
				return SessionClaim::None; // ほかのプロセス (ほかの OBS) が録画中か作成中
			}
			std::string text;
			if (!read_whole_file(join_path(layout.materials_dir, kLedgerFileName), &text) || text.empty()) {
				// 先に読んだときは中身があった録画ログを、読み直せない (ネットワークが一瞬切れたなど)。
				// 録画ログを書くプロセスは、録画ログを空にしない。
				if (unreadable != nullptr)
					*unreadable = true;
				return SessionClaim::None;
			}
			reread = parse_ledger(text);
			now = &reread;
			claim = ledger_claim(reread);
			if (claim == SessionClaim::None)
				return claim;
		}
		if (claim == SessionClaim::Interrupted && !has_readable_material(layout, *now))
			return SessionClaim::None;
		return claim;
	}

	// 作成待ちのセッションを探す。探す保存先は pending_search_dirs のすべてである。
	// 回のフォルダーごとに、中間ファイルのフォルダーの中の録画ログを見る。
	// 数えるのは、録画ログが Pending か Failed のセッションと、中断したセッション (録画ログが NotStarted か
	// Running で終わり、だれもロックファイルを持たない) である。読めるサンプルが 1 つもない中断したセッションは
	// 数えない。
	// キューに積んだか作成中のセッションと、自分が録画中のセッションも数えない。ロックファイルを今ほかの
	// プロセスが持つセッションは、録画ログの値に関係なく数えない (ほかの OBS が録画中か作成中)。
	// mu も finalize_mu も持たずに呼ぶ (中でそれぞれを短く取ってコピーする。2 つを重ねて持たない)。
	// `newest_failed` を渡したら、数えたセッションのうち、録画ログが Failed で最も新しいセッションのフォルダーを
	// 入れる (フォルダーの名前、つまり始めた日時で比べる。なければ空)。RecordingSnapshot::failed_session_dir に
	// 入れ、失敗のメッセージを録画ファイルの作成が成功するまで残すのに使う。録画ファイルの作成の結果 (snap.finalize) はこの起動で最後に作成した
	// セッションのものなので、それだけでは次のセッションを始めるか OBS を起動し直すと、失敗のメッセージが残らない。
	// `results` を渡したら、探した保存先ごとに、アクセスできたかと、録画ファイルの作成の済んでいないセッションがあるかを入れる
	// (record_search_dirs.h の一覧の更新に使う)。アクセスできたかは、`cordmux` フォルダーの中を
	// 読めたか、`cordmux` がないと分かったうえでドライブが空き容量を返したかで決める。読めなかった保存先は、
	// アクセスできなかったとして一覧から外さない。
	std::vector<SessionLayout> pending_session_dirs(std::string *newest_failed = nullptr,
							std::vector<SearchDirResult> *results = nullptr) const
	{
		if (newest_failed != nullptr)
			newest_failed->clear();
		if (results != nullptr)
			results->clear();
		std::string newest_failed_name;
		std::vector<std::string> bases;
		std::string recording_dir;
		{
			std::lock_guard<std::mutex> lock(mu);
			bases = pending_search_dirs;
			recording_dir = recording_session_dir;
		}
		std::multiset<std::string> inflight;
		{
			std::lock_guard<std::mutex> lock(finalize_mu);
			inflight = finalize_inflight;
		}
		std::vector<SessionLayout> found;
		for (const std::string &base : bases) {
			if (base.empty())
				continue;
			const std::string root = base;
			ListOutcome listed = ListOutcome::Listed;
			const std::vector<std::string> names =
				ops.list_subdirectories ? ops.list_subdirectories(root, &listed) : list_subdirectories(root, &listed);
			SearchDirResult result;
			result.base_dir = base;
			result.reachable = listed == ListOutcome::Listed ||
					   (listed == ListOutcome::Missing && results != nullptr &&
					    platform::free_space_bytes(base).has_value());
			for (const std::string &name : names) {
				// 保存先は D:\ のような録画以外のフォルダーも入る場所でありうるので、録画ごとの
				// フォルダーの名前の形でないものは、録画ログを読もうとしない。
				if (!is_session_dir_name(name))
					continue;
				const std::string dir = join_path(root, name);
				const SessionLayout layout = session_layout_of(dir);
				std::string text;
				const std::string ledger_path = join_path(layout.materials_dir, kLedgerFileName);
				if (!read_whole_file(ledger_path, &text)) {
					// 録画ログがあるのに読めない (ほかのソフトが開いている、
					// ネットワークが一瞬切れた) セッションは、済んだか分からないので、
					// 一覧の上では録画ファイルの作成の済んでいないセッションとして扱う。
					// 読み飛ばすのは、ないと分かったときだけである (ledger_known_absent)。
					// 存在の確認が失敗したときと、セッションのフォルダーも見えない
					// ときは、読めないセッションとして扱う。
					if (results != nullptr && !ledger_known_absent(ledger_path, layout.session_dir))
						result.holds_unfinished = true;
					continue;
				}
				if (text.empty())
					continue;
				// 自分のセッション (録画中か、録画ファイルの作成のキューにある) はここで除く
				// (一覧の上では、録画ファイルの作成の済んでいないセッションとして扱う)。
				if (inflight.count(layout.session_dir) != 0 || layout.session_dir == recording_dir)
					result.holds_unfinished = true;
				if (inflight.count(layout.session_dir) == 0 && layout.session_dir != recording_dir) {
					bool held_elsewhere = false;
					bool reread_failed = false;
					const SessionClaim claim =
						claim_of(layout, parse_ledger(text), true, &held_elsewhere, &reread_failed);
					if (held_elsewhere)
						result.holds_unfinished = true; // ほかの OBS が使用中
					if (reread_failed)
						result.holds_unfinished = true; // 録画ログを読み直せない
					if (claim != SessionClaim::None) {
						result.holds_unfinished = true;
						found.push_back(layout);
						if (newest_failed != nullptr && claim == SessionClaim::Failed &&
						    (newest_failed->empty() || name > newest_failed_name)) {
							*newest_failed = layout.session_dir;
							newest_failed_name = name;
						}
					}
				}
			}
			if (results != nullptr)
				results->push_back(result);
		}
		return found;
	}

	// 作成待ちの数をスナップショットに入れ直す。スナップショットの作成待ちの数を書くのはここだけで、探してから
	// スナップショットに入れるまでを count_mu の下で 1 つの処理にする。分けると、UI のスレッド (resume_pending) が
	// 作成中として除いた古い数 (0) を、録画ファイルの作成のスレッドが失敗したセッションを戻した新しい数 (1) の後に書き、
	// 失敗を伝えても、再開のボタンが押せないまま残る。
	// mu も finalize_mu も持たずに呼ぶ (ロックの順は count_mu -> mu と count_mu -> finalize_mu)。`finished` を
	// 渡したら (録画ファイルの作成のスレッドが 1 つのセッションの録画ファイルを作成し終えたとき)、その結果も同じロックの中で
	// スナップショットに入れる。
	// `walked` を渡したら、探した保存先ごとの結果 (アクセスできたか) を入れる (一覧を使っていなければ空)。
	// 問い合わせ直すかの判定に使う。
	void recount_pending(const FinalizeProgress *finished = nullptr, std::vector<SearchDirResult> *walked = nullptr)
	{
		std::lock_guard<std::mutex> count_lock(count_mu);
		bool remembering = false;
		std::uint64_t adds_before = 0;
		{
			std::lock_guard<std::mutex> dirs_lock(pending_dirs_mu);
			remembering = static_cast<bool>(pending_dirs_store.save);
			adds_before = pending_dir_adds;
		}
		std::string newest_failed;
		std::vector<SearchDirResult> results;
		const std::vector<SessionLayout> pending =
			pending_session_dirs(&newest_failed, remembering ? &results : nullptr);
		if (walked != nullptr)
			*walked = results;
		if (remembering) {
			// 録画ファイルの作成の済んでいないセッションが残る保存先の一覧を更新する。変わったときだけ書き、
			// 書けなければ次の集計でもう一度書く。ロックはフォルダーを読み終えてから取る。フォルダーを読む
			// 間、書き込むスレッドがセッションを始めるのを待たせないためである。
			std::lock_guard<std::mutex> dirs_lock(pending_dirs_mu);
			// フォルダーを読み始めた後にセッションを始めた保存先は、この集計では探さなかったものとして、
			// 一覧から外さない。
			std::vector<SearchDirResult> settled;
			for (const SearchDirResult &result : results) {
				bool added_since = false;
				for (const auto &entry : pending_dir_added_at) {
					if (entry.second > adds_before && platform::same_dir_path(entry.first, result.base_dir))
						added_since = true;
				}
				if (!added_since)
					settled.push_back(result);
			}
			// セッションを始めた後に読んで、録画ファイルの作成の済んでいないセッションがないと分かった保存先は、まだ
			// 書けていない追加から外す。
			for (const SearchDirResult &result : settled) {
				if (!result.reachable || result.holds_unfinished)
					continue;
				unsaved_session_adds.erase(std::remove_if(unsaved_session_adds.begin(), unsaved_session_adds.end(),
									  [&result](const std::string &dir) {
										  return platform::same_dir_path(dir, result.base_dir);
									  }),
							   unsaved_session_adds.end());
			}
			change_pending_dirs_locked([&settled](const std::vector<std::string> &base) {
				return update_pending_dirs(base, settled);
			});
		}
		std::lock_guard<std::mutex> lock(mu);
		if (finished != nullptr)
			snap.finalize = *finished;
		snap.pending_present = !pending.empty();
		snap.pending_session_count = static_cast<int>(pending.size());
		snap.failed_session_dir = std::move(newest_failed);
	}

	// 書き込むスレッドを join して、段階を止まった状態にする (set_enabled(false) と request_disable の共通の後半)。
	// join した後、ON/OFF のフラグが false で、段階が Starting・Recording・Stopping のどれかなら Idle にする。
	// Stopping を終わらせるのはここだけである。Starting と Recording は、OFF で段階が Stopping になった後には
	// 残らないはずだが、念のため Idle にする。始められなかったセッション (StartFailed) と、自分で止まった
	// セッション (Idle と理由) は、画面に表示するのでそのまま残す。
	// 止めている最中のフラグ (`writer_shutting_down`) は、join しなかったとき (テストで差し替えた `gate` が、
	// OFF でも録画中と判定するとき) も、ここで必ず false にする。false にし忘れると、その後の ON を
	// 受け付けなくなる。
	void fold_writer_and_settle()
	{
		if (!gate(false))
			join_writer();
		std::lock_guard<std::mutex> lock(mu);
		const RecordPhase phase = snap.phase;
		if (!enabled_flag.load() &&
		    (phase == RecordPhase::Starting || phase == RecordPhase::Recording || phase == RecordPhase::Stopping))
			set_phase_locked(RecordPhase::Idle);
		snap.writer_shutting_down = false;
	}
};

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------

Recorder::Recorder(const config::Settings &settings, RecordPlatformOps ops, std::string base_dir,
		   RecordingGateFn gate)
	: impl_(new Impl())
{
	impl_->settings = settings;
	impl_->ops = std::move(ops);
	impl_->base_dir = std::move(base_dir);
	impl_->gate = gate != nullptr ? gate : &recording_gate;
	impl_->pending_values.output_pcm = settings.record_output_pcm;
	impl_->add_search_dir_locked(impl_->base_dir); // ほかのスレッドはまだないので、mu は取らない
	// 構築のときに 1 度数える。フォルダーとファイルを読むだけで、作成も書き込みもしない。探す保存先を
	// 追加したときと、録画ファイルの作成の後にも数え直す。
	impl_->recount_pending();
}

Recorder::~Recorder()
{
	stop(0);
}

void Recorder::set_session_values(RecordSessionValues values)
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	impl_->pending_values = std::move(values);
	impl_->session_values_given = true;
}

void Recorder::set_finalize_stall_s(int seconds)
{
	impl_->finalize_stall_s = seconds > 0 ? seconds : kDefaultFinalizeStallS;
}

void Recorder::remember_pending_dirs(PendingDirsStore store, std::function<bool(const std::string &)> reachable)
{
	std::optional<std::vector<std::string>> loaded;
	if (store.load)
		loaded = store.load();
	const std::vector<std::string> remembered = loaded ? *loaded : std::vector<std::string>();
	{
		std::lock_guard<std::mutex> lock(impl_->pending_dirs_mu);
		impl_->pending_dirs_store = std::move(store);
	}
	// 既に探している保存先 (現在の保存先) の集計は、問い合わせのスレッドを起動する前に、このスレッドで
	// 済ませる。起動した後に数えると、すぐ答えた保存先の集計 (遅いネットワークの場所を読む) を、ここで
	// 待つことがある。
	// この集計が、一覧にあって既に探している保存先への 1 回目の問い合わせになる (アクセスできなければ、後でもう一度
	// 問い合わせるだけにする)。
	std::vector<SearchDirResult> walked;
	impl_->recount_pending(nullptr, &walked);
	std::vector<std::string> already;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		already = impl_->pending_search_dirs;
	}
	// まだ探していない一覧の保存先ごとに、別のスレッドでアクセスできるかを問い合わせ、答えた保存先から追加して
	// 数え直す (現在の保存先はもう探している)。ここでは待たない。アクセスできなかった保存先は、同じ
	// スレッドが待ってから 1 度だけ問い合わせ直す。集計は定期的には実行しないので、問い合わせ直さないと、起動の
	// ときだけ一時的にアクセスできなかった保存先 (ログインの直後でまだ接続していないネットワークの場所など) の
	// セッションを、一覧にだけある保存先ではその起動の間、既に探している保存先では次の集計まで数えない。
	// 何度も問い合わせ直すと、応答しない保存先のたびにスレッドが残るので、1 度だけにする。
	if (reachable && !impl_->probe_quit.load()) {
		Impl *impl = impl_.get();
		const int delay_ms = impl_->reprobe_delay_ms.load();
		// スレッドの起動と probe_threads への追加は probe_mu の下で 1 つにし、追加する領域は起動する前に
		// 確保する。
		// push_back が確保に失敗すると、probe_threads に入らなかった join できる std::thread が破棄され、
		// std::terminate で OBS が終了する。ロックの中で止める要求も再確認する。stop が probe_threads を
		// 取り出した後に起動したスレッドは、だれにも join されない。
		std::lock_guard<std::mutex> lock(impl_->probe_mu);
		impl_->probe_threads.reserve(impl_->probe_threads.size() + static_cast<std::size_t>(kMaxPendingDirProbes));
		int started = 0;
		for (const std::string &dir : remembered) {
			bool searched = false;
			for (const std::string &existing : already)
				searched = searched || platform::same_dir_path(existing, dir);
			// 既に探していて、上の集計でアクセスできた保存先には、もう問い合わせない。
			if (searched && Impl::reached_in(walked, dir))
				continue;
			if (started >= kMaxPendingDirProbes || impl_->probe_quit.load())
				break; // 残りはこの起動では探さず、一覧には残す (アクセスできない保存先と同じ扱い)
			// スレッドの中身をすべて try で囲む。スレッドの中で投げられた例外を受ける所がないと、
			// std::terminate で OBS が終了する。問い合わせだけでなく、その後の集計 (保存先のフォルダーを
			// 読む、一覧のファイルを読み書きする) も投げることがある。投げたら、アクセスできなかった
			// 保存先と同じに扱う (一覧には残す)。集計の途中で投げたときも、一覧はまだ書かれていない。
			// 投げうる関数を 1 つずつ囲むと、後から追加した関数が囲まれずに残るので、スレッドの全体を囲む。
			// このスレッドは何も返さないので、ここで受ければ外に漏れることはない。
			// 1 回目でアクセスできなければ (false を返した、投げた、フォルダーを読めなかった)、
			// 待ってからもう 1 度だけ問い合わせて終える。既に探している保存先の 1 回目は、上の集計で
			// 済んでいる。2 回目の問い合わせも応答しない保存先 (切れたままのネットワークの場所) では、
			// その間スレッドが残り、その間に OBS を閉じると、問い合わせが戻るまで待つ。
			const bool probe_first = !searched;
			const auto body = [impl, reachable, dir, probe_first, delay_ms]() {
				try {
					if (probe_first && impl->probe_and_recount(reachable, dir))
						return;
				} catch (...) {
				}
				try {
					if (impl->wait_before_reprobe(delay_ms))
						impl->probe_and_recount(reachable, dir);
				} catch (...) {
				}
			};
			std::thread probe;
			try {
				probe = std::thread(body);
			} catch (const std::system_error &) {
				// スレッドを起動できない (OS の資源が足りない)。残りはこの起動では探さず、一覧に残す。
				break;
			}
			++started;
			impl_->probe_threads.push_back(std::move(probe));
		}
	}
}

void Recorder::set_pending_dir_reprobe_delay_ms(int ms)
{
	impl_->reprobe_delay_ms = ms > 0 ? ms : kPendingDirReprobeDelayMs;
}

void Recorder::on_video(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len,
			std::int64_t arrived_steady_us, std::int64_t arrived_wall_us)
{
	if (payload == nullptr || len == 0)
		return;
	// `gate` はコピーの前に確認する。録画が OFF の間は、データを 1 バイトもコピーしない。呼び出し元 (Pipeline) にも
	// 同じ確認があるが、それは念のための重複で、保証はこちらが持つ。
	if (!impl_->gate(impl_->enabled_flag.load()))
		return;
	Event event;
	event.kind = EventKind::Video;
	event.header = header;
	event.payload.assign(payload, payload + len);
	event.steady_us = arrived_steady_us;
	event.wall_us = arrived_wall_us;
	impl_->push_frame(std::move(event));
}

void Recorder::on_audio(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len,
			std::int64_t arrived_steady_us, std::int64_t arrived_wall_us)
{
	if (payload == nullptr || len == 0)
		return;
	// 映像と同じ理由で、`gate` はコピーの前に確認する。
	if (!impl_->gate(impl_->enabled_flag.load()))
		return;
	Event event;
	event.kind = EventKind::Audio;
	event.header = header;
	event.payload.assign(payload, payload + len);
	event.steady_us = arrived_steady_us;
	event.wall_us = arrived_wall_us;
	impl_->push_frame(std::move(event));
}

void Recorder::on_configure(const std::vector<pipeline::StreamFormat> &formats,
			    const std::map<std::string, std::string> &display_names)
{
	// `gate` を通さない。最新の 1 つを置き換えるだけで、RecordPlatformOps の関数を呼ばず、キューにも積まない。
	// 録画を ON にするのは `configure` が届いた後なので、OFF の間に捨てると、ストリームの表が空のまま始まる。
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->pending_formats = formats;
		impl_->pending_names = display_names;
	}
	if (!impl_->gate(impl_->enabled_flag.load()))
		return;
	Event event;
	event.kind = EventKind::Configure;
	event.formats = formats;
	event.names = display_names;
	impl_->push(std::move(event));
}

void Recorder::on_session_facts(SessionFacts facts)
{
	// `gate` を通さない。on_configure と同じく最新の 1 つを置き換えるだけで、RecordPlatformOps の関数を呼ばず、
	// キューにも積まない。録画を ON にするのはこの後なので、OFF の間に捨てると、録画ログの session の行が空のまま
	// 始まる。
	std::lock_guard<std::mutex> lock(impl_->mu);
	impl_->pending_facts = std::move(facts);
}

void Recorder::on_connected()
{
	impl_->connected_flag = true;
	{
		// 録画中に接続が切れていた期間を閉じる。スナップショットのフィールドを書くだけで、RecordPlatformOps の
		// 関数は呼ばない。
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->close_gap_locked();
	}
	if (!impl_->gate(impl_->enabled_flag.load()))
		return;
	Event event;
	event.kind = EventKind::Connected;
	impl_->push(std::move(event));
}

void Recorder::on_disconnected(ws::DisconnectReason reason)
{
	(void)reason; // 切れた理由は、録画ログのセグメントの close_reason では区別しない
	impl_->connected_flag = false;
	{
		// 録画中に切れたら、切れた時刻を記録する。録画していない間の切断は数えない。
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->enabled_flag.load() && impl_->snap.phase == RecordPhase::Recording &&
		    !impl_->gap_started_wall_us)
			impl_->gap_started_wall_us = impl_->wall_now();
	}
	if (!impl_->gate(impl_->enabled_flag.load()))
		return;
	Event event;
	event.kind = EventKind::Disconnected;
	impl_->push(std::move(event));
}

void Recorder::set_enabled(bool on)
{
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->stopping)
			return;
		// 止めている最中の ON は受け付けない (ドックも、停止の処理中はボタンを押せなくする)。書き込むスレッドを
		// join するスレッドが、まだ動いているためである。段階が Stopping でなくても (自分で止まった
		// セッションと、始められなかったセッションのスレッドを止めている最中) 受け付けない。受け付けると、UI の
		// スレッドがそのスレッドの join (つまり書き込むスレッドの join) を待つ。判定は、呼び出し元
		// (Pipeline::set_recording) と同じ recording_start_blocked で行う。受け付けて、join し終えた側が新しい
		// セッションを始める方法では、書き込むスレッドを起動する所が 2 つになり、止める途中の書き込むスレッドの
		// キューにデータが積まれ続けて、止まらないことがある。
		// 拒んでも画面は止まった表示のままなので、事実と食い違わない (もう一度押せば始まる)。
		if (on && recording_start_blocked(impl_->snap))
			return;
		changed = impl_->enabled_flag.load() != on;
		if (changed) {
			impl_->enabled_flag = on;
			impl_->snap.enabled = on;
			impl_->set_phase_locked(on ? RecordPhase::Starting : RecordPhase::Stopping);
		}
	}
	if (on) {
		// 状態が変わらないときは何もしない。
		if (!changed)
			return;
		// 前に止めたときのスレッドを join する。止めている最中のフラグが false であることを上で確認したので、
		// そのスレッドは書き込むスレッドの join を済ませており、あとは戻るだけである (書き込むスレッドを待つ
		// join ではない)。
		if (impl_->stopper_thread.joinable())
			impl_->stopper_thread.join();
		impl_->start_writer();
		Event event;
		event.kind = EventKind::Enable;
		{
			// ON にした時点のストリームの表とセッションの値を載せる (pending_formats のコメント)。
			std::lock_guard<std::mutex> lock(impl_->mu);
			event.formats = impl_->pending_formats;
			event.names = impl_->pending_names;
			event.values = impl_->pending_values;
			// セッションの値を 1 度も渡されていない呼び出し元 (テスト) だけ、構築のときの場所を使う。
			if (!impl_->session_values_given)
				event.values.base_dir = impl_->base_dir;
		}
		impl_->push(std::move(event));
		return;
	}
	// このセッションがまだ続いていたときだけ Disable を積み、録画ログの最後の行と録画ファイルの作成のジョブを作る。
	if (changed) {
		Event event;
		event.kind = EventKind::Disable;
		impl_->push(std::move(event));
	}
	// request_disable が止めている最中なら、そのスレッドの join が済むのを待つ (同じスレッドを 2 か所で
	// join しない)。
	if (impl_->stopper_thread.joinable())
		impl_->stopper_thread.join();
	// 止めたら、書き込むスレッドを join する。OFF の間はスレッドを残さない。キューは空になるまで処理してから
	// 止まるので、今積んだ Disable (録画ログの最後の行と録画ファイルの作成のジョブ) はこの中で必ず終わる。録画ファイルの作成そのものは別の
	// スレッドが持つので、ここでは待たない。
	// `changed` が false でも join する。自分で止めたセッション (空き容量が尽きた) では、書き込むスレッド自身がもう
	// ON/OFF のフラグを false にしているので、ここでは変わっていないように見える。そこで戻ると、OFF なのに
	// スレッドが残る。既に join していれば join_writer は何もしない。
	// join するかも `gate` で決める。録画中かを決める関数を 1 つにしておかないと、`gate` を差し替えて録画が
	// OFF でも動く形にしたテストが、スレッドの寿命について何も確認できなくなる。
	impl_->fold_writer_and_settle();
}

void Recorder::request_disable()
{
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->stopping)
			return;
		// 既に止めている最中なら、そのスレッドに任せる (同じスレッドを 2 か所で join せず、UI のスレッドで前の
		// スレッドの join を待たない)。
		if (impl_->snap.phase == RecordPhase::Stopping || impl_->snap.writer_shutting_down)
			return;
		changed = impl_->enabled_flag.load();
		if (changed) {
			impl_->enabled_flag = false;
			impl_->snap.enabled = false;
			impl_->set_phase_locked(RecordPhase::Stopping);
		}
		// 書き込むスレッドを止め終わるまで true にしておくフラグ。false にするのは
		// fold_writer_and_settle だけである。
		impl_->snap.writer_shutting_down = true;
	}
	if (changed) {
		Event event;
		event.kind = EventKind::Disable;
		impl_->push(std::move(event));
	}
	// 前に止めたときのスレッドは、止めている最中のフラグが false だった (上で確認した) ので、もう戻るだけである。
	if (impl_->stopper_thread.joinable())
		impl_->stopper_thread.join();
	// 書き込むスレッドの join は別のスレッドで行い、UI のスレッドでは待たない。状態が変わっていなくても join する
	// (自分で止まったセッションと始められなかったセッション。set_enabled(false) と同じ)。
	Impl *impl = impl_.get();
	impl_->stopper_thread = std::thread([impl]() { impl->fold_writer_and_settle(); });
}

bool Recorder::writer_running() const
{
	// どのスレッドから呼んでもよい。std::thread を読むと、止める処理のスレッドの join とデータ競合になるので、mu の
	// 下のフラグで答える。
	std::lock_guard<std::mutex> lock(impl_->mu);
	return impl_->writer_alive;
}

RecordingSnapshot Recorder::snapshot() const
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	RecordingSnapshot out = impl_->snap;
	// 録画ファイルの作成の進み具合が止まったかは、読む時点で判定し直す。録画ファイルの作成の側は、数が進まない間は判定する機会がないことが
	// ある (音声のセグメントの読み出しが止まったときなど) ので、書いた時点の値だけでは止まったことにならない。
	if (out.finalize.state == FinalizeState::Running && out.finalize.last_progress_steady_us > 0 &&
	    impl_->ops.now_steady_us) {
		const std::int64_t stall_us = static_cast<std::int64_t>(impl_->finalize_stall_s.load()) * 1000000;
		if (impl_->ops.now_steady_us() - out.finalize.last_progress_steady_us >= stall_us)
			out.finalize.stalled = true;
	}
	return out;
}

std::map<std::uint32_t, pipeline::RecordOnlyFrameClock> Recorder::record_only_frames() const
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	return impl_->record_only;
}

void Recorder::stop(int finalize_wait_ms)
{
	// 一覧の保存先の問い合わせが後から答えても、もう追加しない (止める最中に数え直さない)。問い合わせのスレッドは
	// 最後に join する。先に join すると、応答しない保存先の問い合わせが戻るまで、録画ファイルの作成を待ち始められない。もう
	// 一度問い合わせるまでの待ちも、ここで終わらせる。
	impl_->signal_probe_quit();
	// 止める処理のスレッドの終わりを先に待つ。書き込むスレッドの join を、そのスレッドとここの 2 か所で行わない
	// ためである。
	if (impl_->stopper_thread.joinable())
		impl_->stopper_thread.join();
	bool was_enabled = false;
	bool already_stopped = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		already_stopped =
			impl_->stopping && !impl_->writer_thread.joinable() && !impl_->finalize_thread.joinable();
	}
	if (already_stopped) {
		impl_->join_probes();
		return;
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		was_enabled = impl_->enabled_flag.load();
		impl_->enabled_flag = false;
		impl_->snap.enabled = false;
	}

	if (was_enabled && impl_->writer_thread.joinable()) {
		Event event;
		event.kind = EventKind::Stop;
		event.stop_reason = StopReason::ObsExit;
		{
			std::lock_guard<std::mutex> lock(impl_->mu);
			impl_->queue.push_back(std::move(event));
			impl_->cv.notify_one();
		}
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->stopping = true;
	}
	// 書き込むスレッドを止めて join する。Pipeline は、デコードのスレッドを片付ける前にこの stop を呼ぶ。
	impl_->join_writer();

	// 録画ファイルの作成を上限まで待つ。録画ファイルの作成のスレッドがあるかも、起動する側 (enqueue_finalize) と同じ finalize_mu の下で
	// 読む。
	// 書き込むスレッドはもう join したので、ここから先に録画ファイルの作成のスレッドを起動するものはいない。
	bool finalize_started = false;
	{
		std::lock_guard<std::mutex> lock(impl_->finalize_mu);
		finalize_started = impl_->finalize_thread.joinable();
	}
	if (finalize_started) {
		const std::int64_t deadline_us =
			(impl_->ops.now_steady_us ? impl_->ops.now_steady_us() : 0) +
			static_cast<std::int64_t>(finalize_wait_ms > 0 ? finalize_wait_ms : 0) * 1000;
		for (;;) {
			bool idle = false;
			{
				std::lock_guard<std::mutex> lock(impl_->finalize_mu);
				idle = impl_->finalize_queue.empty() && !impl_->finalize_busy.load();
			}
			if (idle)
				break;
			const std::int64_t now = impl_->ops.now_steady_us ? impl_->ops.now_steady_us() : 0;
			if (finalize_wait_ms <= 0 || now >= deadline_us) {
				impl_->finalize_cancel = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		{
			std::lock_guard<std::mutex> lock(impl_->finalize_mu);
			impl_->finalize_quit = true;
		}
		impl_->finalize_cv.notify_all();
		impl_->finalize_thread.join();
	}

	close_session(impl_->session, impl_->ops);
	// セッションを閉じずに書き込むスレッドが終わっていれば、ロックファイルもここで閉じる。書き込むスレッドは
	// join 済みなので、そのハンドルに触るのはもうこのスレッドだけである。
	impl_->release_lock(impl_->session_lock);
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->snap.phase != RecordPhase::StartFailed)
			impl_->set_phase_locked(RecordPhase::Idle);
		impl_->recording_session_dir.clear();
	}
	impl_->join_probes();
}

void Recorder::resume_pending()
{
	{
		// 止めている最中は再開しない。下で finalize_cancel を false に戻すので、打ち切りの要求を
		// 取り消してしまう。
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->stopping)
			return;
	}
	// 作成待ちのセッションを探す。呼ばれるたびに試し、前に失敗したセッションも、同じ失敗が続くセッションも、
	// 試す回数に上限を設けない。上限で打ち切ると、セグメントのファイルが残っているセッションの録画ファイルを作成する手段が
	// なくなる。
	// 失敗すれば録画ログが Failed に戻り、また作成待ちとして数える (失敗のメッセージが残る)。
	// 1 回の再開で、そのセッションの録画ファイルをすべて作り直す (前に作れた録画ファイルも上書きする)。録画ログの
	// Matched の行を信じて前に作れた録画ファイルを飛ばすと、その後に移動か削除されたファイルを作り直せない。
	const std::vector<SessionLayout> dirs = impl_->pending_session_dirs();
	RecordSessionValues values;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		// 再開するセッションは、始めた時点の値を持っていない (録画ログにない) ので、再開した時点の値で
		// 録画ファイルを作成する。
		values = impl_->pending_values;
	}
	if (dirs.empty()) {
		impl_->recount_pending();
		return;
	}

	impl_->finalize_cancel = false;
	for (const SessionLayout &layout : dirs) {
		// 先にロックファイルを持ち、ほかの OBS が同じセッションを同時に作成しないようにする。ほかのプロセスが
		// 持っていれば積まない。持てない理由がほかにあれば (ロックファイルを作れない保存先)、
		// ロックファイルなしで、録画ログだけで決める。持てないときは、どの場合も短く数回だけ再試行する。ほかの
		// プロセスの集計がロックファイルを確認する一瞬だけ開いていることがあり (共有違反)、持っていたプロセスが
		// 閉じた直後は、削除中のファイルを開けない (アクセス拒否) ことがある。
		// 再試行しても共有違反以外の理由で持てないセッション (読み取り専用にされた古いロックファイルなど) は、
		// 2 つの OBS が同時に再開すると、同じセッションを 2 度録画ファイルを作成することがある。そこで止めると、再開しても
		// 何も起きないセッションが残り続ける (ロックファイルを手で削除するしかない) ので、まれな重なりは
		// 受け入れて先に進む。
		FileHandle lock = kInvalidFile;
		bool held_elsewhere = false;
		if (impl_->ops.hold_lock) {
			for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
				held_elsewhere = false;
				lock = impl_->ops.hold_lock(Impl::session_lock_path(layout), &held_elsewhere);
				if (lock != kInvalidFile)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(kLockRetryMs));
			}
		}
		if (lock == kInvalidFile && held_elsewhere)
			continue;
		// ロックファイルを持った後に録画ログを読み直し、まだ作成待ちかを確認する。数えた後にほかの OBS が
		// 録画ファイルを作成し終えたセッションを、2 度作成しないためである。
		const std::string ledger_path = join_path(layout.materials_dir, kLedgerFileName);
		std::string text;
		LedgerDocument parsed;
		Impl::SessionClaim claim = Impl::SessionClaim::None;
		if (read_whole_file(ledger_path, &text) && !text.empty()) {
			parsed = parse_ledger(text);
			claim = impl_->claim_of(layout, parsed, false);
		}
		// 数えた後にこのプロセスがそのセッションで録画を始めていないかも、積む直前に確認する。ロックファイルを
		// 持てない保存先では、ロックファイルがこの重なりを防がないためである。
		{
			std::lock_guard<std::mutex> guard(impl_->mu);
			if (layout.session_dir == impl_->recording_session_dir)
				claim = Impl::SessionClaim::None;
		}
		if (claim == Impl::SessionClaim::None) {
			impl_->release_lock(lock);
			continue;
		}
		Impl::FinalizeJob job;
		job.doc = std::move(parsed);
		// 止めた理由が記録されないまま終わったセッションは、StopReason::Interrupted として録画ファイルを作成する (録画ログの
		// 最後の session の行に書く)。録画ファイルの作成の途中で止まったセッション (Running) は止めた理由を持っているので、
		// 変えない。
		if (claim == Impl::SessionClaim::Interrupted && job.doc.session.stopped_reason == StopReason::StillRunning)
			job.doc.session.stopped_reason = StopReason::Interrupted;
		job.ledger_needs_newline = text.back() != '\n';
		job.lock = lock;
		job.session_dir = layout.session_dir;
		job.materials_dir = layout.materials_dir;
		job.values = values;
		job.ledger = kInvalidFile;
		job.ledger_path = ledger_path; // 追記のために開き直す
		job.repair = true; // 途中で切れたセグメントの修復は、再開のときだけ行う
		// 同じセッションが既にキューにあるか作成中なら、積まれない。そのときはロックファイルを閉じる。
		if (!impl_->enqueue_finalize(std::move(job)))
			impl_->release_lock(lock);
	}
	// 積んだセッションは作成中なので、集計から外れる。数え直して、作成している間は作成待ちのメッセージと再開の
	// ボタンを表示しない。押しても積むものがないボタンは表示しない。
	impl_->recount_pending();
}

} // namespace cordmux::record

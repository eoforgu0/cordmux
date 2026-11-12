// SPDX-License-Identifier: Apache-2.0

// 録画のコードが共有する型と定数。純粋関数のヘッダー (record_clock.h、record_segment.h、record_args.h など) は、
// ここの型だけを引数と戻り値に使う。
// libobs、Qt、avformat に依存しない。読み込むのは C++17 の標準ライブラリと pipeline/dummy_interval.h だけである。
// Windows の名前 (test/lint/windows_names.txt) を書かない。子プロセスの起動、空き容量の取得、フォルダーの作成、
// ファイルへの書き込みは、RecordPlatformOps の関数として注入する。
// パスを含め、文字列はすべて UTF-8 で持つ。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pipeline/dummy_interval.h"

namespace cordmux::record {

// ---------------------------------------------------------------------------
// 種類
// ---------------------------------------------------------------------------

// セグメントファイル 1 つが持つ中身。画面共有の映像と音声は別のファイルに書く。同じファイルに入れると、片方だけが
// 途切れたときにもセグメントを分けることになるためである。
// StreamKind と MediaKind の組で表すと、声の映像という存在しない組を作れてしまうので、存在する 3 つの値の列挙にする。
enum class TrackKind {
	ScreenVideo, // 画面共有の映像 (H.264)
	ScreenAudio, // 画面共有の音声 (PCM)
	Voice,       // 声 (PCM)
};

// 録画ログの segment の行の `kind` と `media`。TrackKind から決まるが、録画ログのフィールドと同じ形の型で持つ。
enum class StreamKind { Screen, Voice };
enum class MediaKind { Video, Audio };

inline StreamKind stream_kind_of(TrackKind track)
{
	return track == TrackKind::Voice ? StreamKind::Voice : StreamKind::Screen;
}

inline MediaKind media_kind_of(TrackKind track)
{
	return track == TrackKind::ScreenVideo ? MediaKind::Video : MediaKind::Audio;
}

// ---------------------------------------------------------------------------
// 状態と理由
// ---------------------------------------------------------------------------

// セグメントを閉じた理由。ストリームを表示するソースや枠の番号が変わっただけでは閉じない (枠を並べ替えるたびに
// 録画が分かれないようにするため)。
// Unknown は持たない。セグメントは、閉じる理由が分かっているときにだけ閉じるためである。
enum class CloseReason {
	Disconnected,     // 接続が切れた
	StreamGone,       // configure からその stream が消えた
	FormatChanged,    // 解像度か fps が変わった
	ClockJump,        // タイムスタンプが 1 秒以上戻ったか、前へ大きく進んだ
	RecordingToggled, // 録画を ON にした、または OFF にした
	QueueOverflow,    // 録画のキューがあふれた
	WriteFailed,      // ファイルに書き込めなかった (録画ログでキューのあふれと区別するために分ける)
	// まだ閉じていないセグメント。セグメントを開いたときに録画ログへ書く行が、この値を持つ。
	// 開いたときに書くのは、録画の途中で OBS か PC が止まっても、開いていたセグメントを録画ログから見つけて
	// 作成できるようにするためである。
	// 閉じたときに書く行が同じ `file` の行を後から上書きするので、正常に閉じたセッションの録画ログには残らない。
	// 残るのは閉じる前に止まったセグメントだけで、その行は `truncated` も真である。
	NotClosed,
};

// 録画ファイルの作成の進み具合。
enum class FinalizeState {
	NotStarted, // まだ録画を止めていないか、止めたが録画ファイルの作成を始めていない
	Running,    // 子プロセスが動いている
	Done,       // すべてのファイルを書き終えた
	Pending,    // OBS の終了時に待つ時間の上限で打ち切った。次の起動の後に [作成を再開] で再開する
	Failed,     // 失敗した。セグメントファイルは 1 バイトも削除しない
};

// 作成方法。Unknown を持たない。まだ決まっていないときは std::optional<FinalizeBranch> で表す。
enum class FinalizeBranch {
	Remux,  // 再エンコードしない。セグメントと黒埋め用のフレームを並べて多重化し直す
	ProRes, // その人のその種類だけ、ProRes 422 HQ へ 1 回再エンコードする
};

// 黒埋め用のフレームを作ったタイミング。3 つのうち、最初に届いたものだけを使う。
enum class MaterialOrigin {
	EncoderCreated,    // エンコーダを作った直後 (録画がすでに ON のとき)
	RecordingTurnedOn, // 録画を ON にしたとき (別のエンコーダのインスタンスで作る)
	EncoderTeardown,   // エンコーダを破棄するとき (ほかの 2 つで作れなかったときのため)
};

// 録画を止めた理由 (録画ログの session の行の `stopped_reason`)。
enum class StopReason {
	StillRunning,     // まだ止まっていない (書き込みの途中で読んだ録画ログ)
	TurnedOff,        // 利用者が OFF にした
	ObsExit,          // OBS が終了した
	SpaceExhausted,   // 空き容量がなくなった (録画のコードが自分で録画を止めるのはこのときだけ)
	// 止めた行がないまま終わったセッションを、後から作成したとき。録画ログが NotStarted のまま残っていた
	// セッションで、録画の途中で OBS か PC が止まったときにこうなる。止まった時刻は分からない
	// (`ended_at_steady_us` は 0 のまま)。
	Interrupted,
};

// 作成した後のチェックの結果。終了コードが 0 のまま録画ファイルが短くなることがあるので、終了コードと長さの両方を
// 見る。
enum class FinalizeCheck {
	NotChecked,       // まだチェックしていない
	Matched,          // 録画ログと録画ファイルの長さとフレーム数が一致した
	DurationMismatch, // 終了コードは 0 だが、長さが録画ログと違う (途中で切れたセグメントがあるときに起きる)
	ProcessFailed,    // 終了コードが 0 でない
	SpaceExhausted,   // 書き込みの途中で空き容量がなくなった
};

// セグメントの間の欠落 1 つを、録画ファイルの作成でどう埋めたか (録画ログの finalize の行の `gaps`)。
struct GapFill {
	std::int64_t start_us = 0;    // セッションの原点からの位置
	std::int64_t duration_us = 0; // 欠落の長さ
	// 映像の欠落: 黒埋め用のフレームのファイルを repeat_count 回並べ、残りの端数を remainder_ticks の長さだけ使う。
	// 音声の欠落: 0 のサンプルを duration_us の長さだけ書く (repeat_count と remainder_ticks は 0 のまま)。
	int repeat_count = 0;
	std::int64_t remainder_ticks = 0;
	// 黒埋め用のフレームを作ったエンコーダのインスタンスの ID。空は黒埋め用のフレームがなかったことを示し、
	// その人の映像は ProRes で録画ファイルを作成する (黒を lavfi で作るため)。
	std::string black_encoder_instance_id;
};

// ---------------------------------------------------------------------------
// 時計
// ---------------------------------------------------------------------------

// タイムスタンプが戻ったときの扱い (record_clock.h の classify_pts の結果)。しきい値 1 つで分けずに 3 段階に
// 分ける理由は、classify_pts のコメントにある。
enum class ClockStep {
	Forward,     // 通常どおり進んだ
	Pushed,      // 1 フレームの間隔より小さく戻った。前の値 + 1 フレームの間隔へずらした
	Reanchored,  // 1 フレームの間隔以上 1 秒未満戻った。ずらして、原点の置き直しとして数えた
	Jumped,      // 1 秒以上戻ったか、前へ大きく進んだ。セグメントを閉じる (CloseReason::ClockJump)
};

// 原点の置き直し 1 件 (録画ログの segment の行の `pts_reanchor`)。量と時刻の両方を持つのは、B フレームが疑われる
// 後戻り (フレームの間隔の整数倍で規則的に繰り返す) と、送信側の通常の置き直し (1 度だけで続かない) を後から
// 見分けるためである。
struct PtsReanchor {
	std::int64_t delta_us = 0;     // 戻った量 (正の値で持つ)
	std::int64_t at_pts_us = 0;    // 戻ったときの送信側のタイムスタンプ
	std::int64_t at_steady_us = 0; // そのときの受信側の steady_clock の時刻
};

// ---------------------------------------------------------------------------
// 録画ログ (JSONL) の 5 種類の行
// ---------------------------------------------------------------------------

// 録画ログの形式のバージョン。録画ファイルの作成を後の起動で再開するとき、別のバージョンが書いた録画ログを見分けるために持つ。
// 1 から始めるのは、`schema` のキーがない行 (0 として読む) と区別するためである。
inline constexpr int kLedgerSchema = 1;

struct LedgerSessionRow {
	int schema = kLedgerSchema;
	std::string session_id; // セッションのフォルダーの名前と同じ値 (開始の日時)
	std::int64_t started_at_wall_us = 0;
	std::int64_t started_at_steady_us = 0;
	std::int64_t ended_at_steady_us = 0; // 0 はまだ止めていないことを示す
	StopReason stopped_reason = StopReason::StillRunning;
	FinalizeState finalize_state = FinalizeState::NotStarted;
	std::string plugin_version; // cordmux.dll のバージョン
	std::string ffmpeg_version; // 同梱の FFmpeg が示したバージョン
	// ダミーを表示していた時間帯。ダミーのフレームは録画の入力を通らないので録画には入らないが、コードの構造が
	// 変わっても、ダミーを表示していた時間帯を後から見分けられるように残す。
	std::vector<pipeline::DummyInterval> dummy_intervals;
};

struct LedgerPersonRow {
	std::string user_id;
	// セグメントを開いたときにコピーした表示名。接続が切れると読めなくなるので、録画ログに残す。
	// 空はまだ読めていなかったことを示す (読めたときに、表示名を入れた行を追加する)。
	std::string display_name;
	std::vector<TrackKind> kinds; // その人について実際にセグメントを開いた種類
};

struct LedgerSegmentRow {
	std::string user_id;
	StreamKind kind = StreamKind::Screen;
	// 再接続すると同じ番号が別の人に割り当てられるので、セグメントを見分けるキーにしない。原因を調べるために
	// 値だけを残す。
	std::uint32_t stream_id = 0;
	// ワイヤー形式にも configure にもエンコーダのインスタンスを示すフィールドがないので、受信側が、ストリームを
	// 受け取り始めたときと avcC が変わったときに新しい ID を割り当てる。
	// 整数でなく文字列にするのは、録画ログを読むツールがセッションをまたいで比べるときに、桁に意味のない値として
	// 扱えるようにするためである (user_id と同じ)。
	std::string encoder_instance_id; // 受信側が割り当てる ID
	std::string file;                // 中間ファイルのフォルダーからの相対パス (segments/...)
	MediaKind media = MediaKind::Video;
	std::string avcc_hex; // 録画のコードが組み立てた avcC のバイト列そのもの (音声は空)
	int width = 0;
	int height = 0;
	// fps は有理数で持つ。60000/1001 を 59.94 と書くと、録画ファイルの作成で長さを整数で比べられなくなる。
	int fps_num = 0;
	int fps_den = 0;
	int timescale = 0;             // 映像は 60000。音声はサンプリングレート
	std::int64_t first_pts_us = 0; // 送信側のタイムスタンプ
	std::int64_t first_pts_steady_us = 0; // 受信側の steady_clock の時刻 (再接続をまたぐ位置に使う)
	std::int64_t first_pts_wall_us = 0;   // 受信側のシステム時刻 (後から原因を調べるために持つ)
	std::int64_t last_pts_us = 0;
	std::int64_t sample_count = 0;   // 映像はフレーム数、音声はサンプル数
	std::int64_t duration_ticks = 0; // timescale の単位で数えた長さ
	std::int64_t bytes = 0;          // 実際に書いたバイト数 (空き容量の見積もりが読む)
	bool starts_with_idr = false;    // 映像は必ず真
	std::int64_t pts_pushed_count = 0;       // ClockStep::Pushed になった回数
	std::vector<PtsReanchor> pts_reanchor;   // ClockStep::Reanchored になった 1 件ずつ
	bool pts_periodic_backward = false;      // 後戻りが規則的に繰り返したか (B フレームの疑い)
	// 送信側が IDR の前に SPS と PPS を付けた回数の、このセグメントを閉じた時点の累計。送信側は
	// プラグインの起動からの累計だけを送るので、セグメントごとの数は前の行との差で求める。
	std::int64_t sps_pps_prepended_count = 0;
	std::string au_hash;             // セグメントごとの framemd5 (受け取った映像と同じかを確認するため)
	bool truncated = false;          // 書き込みの途中で止まったセグメント (録画ファイルの作成の前に読める部分を書き直す)
	CloseReason close_reason = CloseReason::RecordingToggled;
	// 途中で切れたセグメントを読める所まで書き直したとき、読めずに捨てた末尾のバイト数。書き直したセグメントの
	// 行だけが値を持つ (0 は、捨てたものがないか、書き直していないことを示す)。
	std::int64_t dropped_tail_bytes = 0;
};

struct LedgerBlackRow {
	std::string user_id;
	std::string encoder_instance_id;
	std::string file;     // 中間ファイルのフォルダーからの相対パス (black/...)
	std::string avcc_hex; // 作成方法を決めるとき、この値をセグメントの avcc_hex と比べる
	int width = 0;
	int height = 0;
	int fps_num = 0;
	int fps_den = 0;
	std::int64_t frame_count = 0;
	std::int64_t duration_ticks = 0;
	MaterialOrigin origin = MaterialOrigin::EncoderCreated;
	// MaterialOrigin::EncoderCreated のとき、エンコーダを作ってから最初の実際のフレームが出るまでに加わった遅延。
	// 遅延を測るツールは test/tools/webcodecs-check にある。
	std::int64_t first_frame_delay_us = 0;
};

struct LedgerFinalizeRow {
	std::string user_id;
	// 録画ファイルの作成の行は、作る録画ファイル 1 つにつき 1 行である。
	// 画面共有の録画ファイル (「alice 画面共有 2000-01-01 00-00-00.mov」の形) は画面共有の映像と音声を 1 つに
	// まとめるので、種類は TrackKind (3 つの値) ではなく StreamKind (2 つの値) で持つ。セグメントファイルは
	// TrackKind ごとに分かれるので、セグメントの行とは単位が違う。
	StreamKind kind = StreamKind::Screen;
	FinalizeBranch branch = FinalizeBranch::Remux;
	std::string output_file; // セッションのフォルダーからの相対パス (録画ファイルの名前)
	// FFmpeg に渡した引数。build_finalize_plan が録画ログから作ったものを、そのまま残す。
	std::vector<std::string> args;
	int exit_code = 0;
	std::int64_t output_duration_ticks = 0;
	int output_timescale = 0;
	FinalizeCheck check = FinalizeCheck::NotChecked;
	std::vector<GapFill> gaps; // 欠落をどう埋めたか
};

// ---------------------------------------------------------------------------
// ドックに渡す状態
// ---------------------------------------------------------------------------

// 録画ファイルの作成の進み具合が止まったとみなすまでの既定の秒数。目安の値で、測って決めたものではない。ProRes で録画ファイルを作成する
// 1 つのファイルの中で、進み具合の値が止まる最も長い時間の 2 倍を既定にする考えだが、その時間はまだ測っていない。
// 設定の `record_finalize_stall_s` の既定と同じ値にする (test_config_settings.cpp が同じかを確認する)。
inline constexpr int kDefaultFinalizeStallS = 120;

// 録画ファイルの作成の進み具合。何が進んでいるかが分かるように、ファイルの数で表す。1 つのファイルの中の割合と、止まっているか
// どうかは、実際に進んだ量からだけ求める。
struct FinalizeProgress {
	FinalizeState state = FinalizeState::NotStarted;
	int files_total = 0;
	int files_done = 0;
	int files_failed = 0;
	// 今処理しているファイルの作成方法。まだ決まっていなければ値を持たない。
	std::optional<FinalizeBranch> branch;
	// ProRes で録画ファイルを作成する人が 1 人でもいるか (録画ファイルの作成に要る空き容量の見積もりが大きくなる)。
	bool any_reencode = false;
	// どれかのファイルで、同梱の FFmpeg を起動できなかった (実行ファイルの場所が分からないか、子プロセスを作れない)。
	// ドックは、録画ファイルの作成に失敗したメッセージの次の操作を、空き容量の確認ではなく FFmpeg のファイルの置き直しにする。
	bool ffmpeg_unavailable = false;

	// 今作成しているファイルの番号 (1 から数える。0 は、作成しているファイルがないことを示す)。
	int current_index = 0;
	// 現在のファイルの処理が済んだ割合 (千分率、0 から 1000)。分からない間は値を持たない (推測した割合を
	// 表示しないため)。音声を組み立てている間は、書いた量を全体の量で割った値。FFmpeg が動いている間は、FFmpeg が
	// 報告した out_time_us を録画ログの長さで割った値。
	std::optional<int> current_permille;
	// 最後にどれかの値 (ファイルの数か割合) が進んだ時刻。システム時刻は画面の「何分間進んでいません」の表示に、
	// steady_clock の時刻は止まったかどうかの判定に使う。判定は録画のコードが行う。0 はまだ進んでいないことを示す。
	std::int64_t last_progress_wall_us = 0;
	std::int64_t last_progress_steady_us = 0;
	// どの値も `record_finalize_stall_s` 秒の間進んでいない。セグメントファイルは消えないので、真になっても
	// メッセージのレベルは上げない。
	bool stalled = false;
};

// 録画ログに書く事実のうち、録画のコードからは観測できないもの。plugin_version と dummy_intervals は session の
// 行に、sps_pps_prepended はセグメントの行 (sps_pps_prepended_count) に書く。
// 所有者は Pipeline で、Recorder は渡された値を書くだけである (同じ事実を 2 か所で数えないため)。
// 録画が OFF の間に渡してもよい。保存したコピーを置き換えるだけで、RecordPlatformOps の関数を 1 つも呼ばない
// (on_configure と同じ扱い)。
struct SessionFacts {
	// cordmux.dll のバージョン (録画ログの session の行の plugin_version にそのまま入る)。
	std::string plugin_version;
	// 送信側が facts で返した、IDR に SPS と PPS がなかったときにそれを前に付けた回数。プラグインを起動してからの
	// 累計で、セグメントごとの数ではない。
	std::uint64_t sps_pps_prepended = 0;
	// ダミーを表示していた時間帯。
	std::vector<pipeline::DummyInterval> dummy_intervals;
};

// 録画の段階。段階とほかのフィールド (enabled、save_dir_writable、self_stopped_reason) が食い違ったら、
// 画面の側は段階を正しいものとして扱う。
enum class RecordPhase {
	Idle,        // 止まっている (一度も始めていないか、止め終わった)
	Starting,    // ON にしたが、保存先にフォルダーと録画ログをまだ作れていない
	Recording,   // 保存先に書けた (ドックの録画のボタンを押された見た目にしてよいのはこの段階だけ)
	Stopping,    // OFF にしたが、書き込むスレッドがまだ止まり終わっていない
	StartFailed, // 保存先に書けず、このセッションを始めなかった (別の場所へは移さない)
};

// 録画が途切れた期間 1 つ (ドックの cordmux.dock.record_gap.headline に表示する)。今は、録画中に Vesktop との
// 接続が切れていた期間を数える。再接続したときか、切れたままセッションが終わったときに 1 件になる。
struct RecordGapSpan {
	std::int64_t start_wall_us = 0; // 途切れ始めたシステム時刻
	std::int64_t duration_us = 0;   // 途切れた長さ
};

// ドックが周期ごとに読む状態のうち、録画の部分。値のコピーだけを持つ (PipelineSnapshot と同じ)。
struct RecordingSnapshot {
	// 今録画が ON か。OBS を起動し直したときに録画が始まらないように、設定には保存せず、メモリにだけ持つ。
	bool enabled = false;

	// 段階と、その段階に入った時刻。時刻は、1 秒未満で終わる操作でボタンのテキストを変えないための判定に使う。
	RecordPhase phase = RecordPhase::Idle;
	std::int64_t phase_since_wall_us = 0;
	std::int64_t phase_since_steady_us = 0;

	// 録画を止める処理を受け持つスレッドが、まだ書き込むスレッドを停止している最中か。
	// 利用者が OFF にしたセッションでは、段階が Stopping の間ずっと真になる。録画のコードが自分で止めた
	// セッションと、始められなかったセッションでは、段階 (Idle か StartFailed) を変えずに、このフィールドだけが
	// 真になる (停止は一瞬で、画面に表示する状態ではないため)。
	// この間は ON を受け付けない (recording_start_blocked)。受け付けると、UI のスレッドが書き込むスレッドの join を
	// 待つか、停止の途中の書き込むスレッドに新しいセッションを渡すことになる。
	bool writer_shutting_down = false;

	// 現在のセッションか、最後のセッションで途切れた期間 (次のセッションを始めたら空にする)。
	std::vector<RecordGapSpan> gaps;

	// 最後に開始を試して書けなかった場所 (セッションのフォルダーを作るフォルダー)。空は、書けなかった場所が
	// ないことを示す。次に始められたセッションで空に戻す。
	std::string unwritable_dir;

	// 最後に解決した保存先 (セッションのフォルダーを並べるフォルダー。UTF-8 の絶対パス)。
	// 値を入れるのは Pipeline である (保存先を解決するのが Pipeline だけのため)。Recorder のコピーでは空のまま。
	std::string resolved_base_dir;

	// 現在のセッションの開始時刻 (0 は一度も ON にしていないことを示す)。
	std::int64_t started_at_wall_us = 0;
	std::int64_t started_at_steady_us = 0;

	// 保存先の中の、このセッションのフォルダー (UTF-8 の絶対パス。空はまだ作っていないことを示す)。
	std::string session_dir;

	// 保存先に実際に書けたか。値を持たないのは、まだ確認していないことを示す (書けなかったことと区別する)。
	// 録画を ON にしてフォルダーを作り、録画ログを開けたセッションで真、そこで失敗したセッションで偽になる。
	// 偽のときは、ドックが保存先に書けなかったことを伝える (表示の規則の NoticeRuleId::RecordDirUnwritable)。
	// 録画のボタンを押したのに、理由が分からないまま録画が始まらない、という状態を避けるためである。
	std::optional<bool> save_dir_writable;

	// 録画のコードが自分で止めたセッションの理由。値を持たないのは、自分では止めていないことを示す。
	// 今入るのは StopReason::SpaceExhausted だけである (自分で止めるのは空き容量がなくなったときだけ)。利用者が OFF
	// にしたときと OBS の終了は、止めた側がすでに知っているので、ここには入らない。
	//
	// このフィールドだけが、止めたことを外に伝える方法である。止める判断は書き込むスレッドの中で起きるので、
	// そのスレッドは自分を join できず、送信側へ録画の OFF を送る手段も持たない (Recorder は Listener を知らない)。
	// 定期的にコピーを読む側がこれを見て、録画のフラグを false にし、送信側へメッセージ、set_enabled(false) を呼ぶ。
	//
	// 次に ON にするまで残す。止まった理由を画面に表示し続けるためで、次のセッションが始まったときに消える。
	std::optional<StopReason> self_stopped_reason;

	// セグメントの数と書いたバイト数。
	std::int64_t segment_count = 0;      // 閉じたセグメントの数
	std::int64_t open_segment_count = 0; // 今開いているセグメントの数
	std::int64_t bytes_written = 0;      // 録画ログとセグメントと黒埋め用のフレームを合わせた、実際のバイト数

	// 空き容量と、録画できる残り時間の見積もり。値を持たないのは、まだ見ていないことを示す。
	std::optional<std::uint64_t> free_space_bytes;
	std::optional<std::int64_t> remaining_seconds;
	std::int64_t space_checked_at_steady_us = 0;

	// 録画ファイルの作成の途中で必要になる空き容量の最大値の見積もり。足りない量は、表示の規則 (ledger/) が求める。
	std::optional<std::uint64_t> finalize_need_bytes;

	// 録画ファイルの作成の進み具合と作成方法。
	FinalizeProgress finalize;

	// 作成待ちのセッションがあるか ([作成を再開] で再開する。ActionId::ResumeFinalize)。
	// 数えるのは、録画ファイルの作成が途中で終わったセッション (Pending)、作成に失敗したセッション (Failed。
	// セグメントファイルは残っている)、録画か録画ファイルの作成の途中で OBS か PC が止まったセッションである。キューに入れた
	// セッションと、作成している最中のセッションは数えない (作成している間に [作成を再開] を押しても、同じ
	// セッションを 2 度キューに入れないため)。
	bool pending_present = false;
	int pending_session_count = 0;
	// 上で数えたセッションのうち、録画ログが Failed で最も新しいセッションのフォルダー (フォルダーの名前、つまり
	// 始めた日時で比べる)。なければ空。キューに入れたセッションと、作成している最中のセッションは入れない。
	// finalize はこの起動で最後に作成したセッションのことしか示さないので、次のセッションを始めるか OBS を
	// 起動し直すと、失敗が finalize から見えなくなる。ドック (表示の規則の RecordFinalizeFailed) は、この
	// フィールドで失敗が残っていることを知り、場所の行にこのフォルダーを表示する。失敗のメッセージを、録画ファイルの作成に成功する
	// まで残すためである。
	std::string failed_session_dir;

	// 原因を調べるための数 (判定には使わない)。
	std::int64_t frames_written = 0;        // 録画に書いた映像のフレームと音声のチャンクの数
	std::int64_t frames_dropped_overflow = 0; // キューがあふれて捨てたフレームとチャンクの数
	std::int64_t material_frames_received = 0; // ws::kFlagRecordingOnly 付きで届いたフレーム (黒埋め用のフレーム)
	std::int64_t material_audio_discarded = 0; // ws::kFlagRecordingOnly 付きで届いた音声のチャンク (捨てて数える)
};

// 今、録画の ON を受け付けないか。Stopping の間と、書き込むスレッドを停止している最中は受け付けない。
// Recorder::set_enabled(true) と、フラグを立てる前に確認する呼び出し元 (Pipeline::set_recording(true)) が、
// この同じ関数を使う。片方だけが拒むと、呼び出し元は ON と思い、録画は OFF のままになって食い違う。
inline bool recording_start_blocked(const RecordingSnapshot &snapshot)
{
	return snapshot.phase == RecordPhase::Stopping || snapshot.writer_shutting_down;
}

// ---------------------------------------------------------------------------
// OS の機能 (RecordPlatformOps)
// ---------------------------------------------------------------------------

// 開いたファイル。0 は開けなかったことを示す。
// record/ から std::ofstream を直接使わない。Windows の narrow な API (char のパスを取る std::ofstream や fopen) は
// パスをアクティブコードページの文字列として読むので、ASCII でない保存先を開けないためである。
using FileHandle = std::uint64_t;
inline constexpr FileHandle kInvalidFile = 0;

// 起動した子プロセス。0 は起動できなかったことを示す。
using ChildHandle = std::uint64_t;
inline constexpr ChildHandle kInvalidChild = 0;

// 子プロセスの終了を待った結果。
enum class ChildWait {
	Exited,  // 終了した (exit_code に値が入る)
	Timeout, // 上限まで待っても終了しなかった
	Failed,  // 待てなかった (ハンドルが壊れているなど)
};

struct ChildWaitResult {
	ChildWait outcome = ChildWait::Failed;
	int exit_code = 0;
};

// フォルダーの一覧を読めたか。record_search_dirs.h が、作成待ちのセッションを探せたかを判断するのに使う
// (record_writer.h の list_subdirectories と、RecordPlatformOps::list_subdirectories が返す)。
enum class ListOutcome {
	Listed,  // 読めた (空でもよい)
	Missing, // その場所がない (OS がないと答えた)
	Failed,  // 読めなかった (権限、ネットワークの切断、ドライブの取り外しなど。途中で読めなくなったときも)
};

// 録画のコードが使う OS の機能 (ws::PlatformOps と同じ形)。すべて関数オブジェクトとして注入する。record/ の中に
// Windows の名前を書かないためと、テストでテスト用の関数に差し替えて、録画が OFF の間は 1 度も呼ばれないことを
// 確認するためである。
//
// どの関数も例外を投げない。失敗は戻り値 (false、0、nullopt) で返す。
struct RecordPlatformOps {
	// path までのフォルダーを作る (途中のフォルダーも作る。すでにあれば true)。
	std::function<bool(const std::string &path)> make_directories;
	// path のフォルダーを 1 階層だけ作る (親はすでにあること)。すでにあれば作らずに false を返し、`*already_exists`
	// を真にする。セッションのフォルダーの名前を、ほかの OBS や前のセッションが使っている名前と
	// 取り違えないためである (record_segment.h の ensure_session_open)。ほかの理由で作れなかったときは false で、
	// `*already_exists` は偽になる。空の関数のときは、すでにあるかを確認せずに make_directories で作る (テスト用の
	// セットのため。製品では必ず設定する)。
	std::function<bool(const std::string &path, bool *already_exists)> make_new_directory;
	// 書き込み用に開く (UTF-8 のパス)。すでにあれば中身を捨てる。開けなければ kInvalidFile。
	std::function<FileHandle(const std::string &path)> open_file;
	// 開いたファイルの末尾に書く。書けたバイト数が len に満たなければ false。
	std::function<bool(FileHandle file, const void *data, std::size_t len)> append;
	// 閉じる。kInvalidFile を渡してもよい (何もしない)。
	std::function<void(FileHandle file)> close;
	// path があるボリュームの空き容量 (バイト)。読めなければ nullopt を返す。読めなかったことを 0 で返すと、
	// 空き容量がなくなったと読まれるためである。
	std::function<std::optional<std::uint64_t>(const std::string &path)> free_space_bytes;
	// 子プロセスを起動する。program は実行ファイルの絶対パス、args は引数 (program 自身を含まない)。
	// 起動できなければ kInvalidChild。
	std::function<ChildHandle(const std::string &program, const std::vector<std::string> &args)> start_child;
	// 終了するまで待つ (timeout_ms が 0 以下なら、待たずに現在の状態だけを見る)。
	std::function<ChildWaitResult(ChildHandle child, int timeout_ms)> wait_child;
	// 子プロセスを止める。止めずに残して戻る手段は持たない。残すと、OBS を終了した後も FFmpeg が動き続ける。
	std::function<void(ChildHandle child)> kill_child;
	// 受信側の steady_clock の時刻 (マイクロ秒)。再接続をまたぐ欠落の長さに使う。
	std::function<std::int64_t()> now_steady_us;
	// 受信側のシステム時刻 (エポックからのマイクロ秒)。セッションのフォルダーの名前と録画ログに使う。
	std::function<std::int64_t()> now_wall_us;
	// 同梱の FFmpeg の実行ファイルの絶対パス (UTF-8)。見つからなければ空。実行ファイルの名前は platform/ の中にだけ
	// 書く。
	std::function<std::string()> ffmpeg_path;
	// 現在の時差 (秒。現地の時刻 - UTC)。セッションのフォルダーの名前を、OBS の録画の既定と同じく現地の時刻で
	// 付けるために使う。空の関数のときは 0 (UTC) として扱う。
	std::function<int()> utc_offset_sec;
	// セッションが使用中であることを示すマーカー。作成待ちを集計するとき、ほかの OBS が録画中か作成中の
	// セッションを、途中で止まったセッションと取り違えないために使う。path のファイルを、ほかのどのプロセスとも
	// 共有せずに開いて持つ (なければ作る)。ハンドルを close に渡すか、持っているプロセスが終了すると
	// (クラッシュしたときも)、OS がハンドルを閉じてファイルも消える。持てなければ kInvalidFile を返し、ほかの
	// プロセスが持っているためなら `*held_elsewhere` を真にする。削除を許さない保存先では、閉じたときに消す指定を
	// 外して開き直す (閉じてもファイルが残るが、残ったマーカーは lock_held が偽と読む)。空の関数は「持てなかった、
	// 誰も持っていない」と同じに扱う (マーカーを使わずに、録画ログだけで決める)。
	std::function<FileHandle(const std::string &path, bool *held_elsewhere)> hold_lock;
	// path のマーカーを今だれかが持っているか。作らず、消さず、書かない (読むだけの集計から呼ぶ)。
	// ファイルがないとき、持っているプロセスがいない古いマーカー (電源が落ちたセッションの残り) のとき、確認でき
	// なかったときは偽。
	std::function<bool(const std::string &path)> lock_held;
	// path の直下のフォルダーの名前を並べ、読めたかを `*outcome` に入れる (作成待ちの集計が、保存先の `cordmux`
	// の中のセッションを探す)。読めなければ、途中で読めなくなったときも、空を返して Failed にする (途中までの一覧を
	// 返さない)。空の関数のときは record_writer.h の list_subdirectories で読む (製品のセットも同じものを
	// 設定する)。関数として注入するのは、一覧の途中で読めなくなる形 (ネットワークの切断、ドライブの取り外し) を
	// テストで起こしにくいので、テストが差し替えられるようにするためである。
	std::function<std::vector<std::string>(const std::string &path, ListOutcome *outcome)> list_subdirectories;
};

// ---------------------------------------------------------------------------
// 保存先の中の並び
// ---------------------------------------------------------------------------

// `<保存先>/<セッションのフォルダー>/` の中の並び (保存先は、利用者が選んだ場所か、選んでいないときの既定の場所):
//   - セッションのフォルダーの名前は、OBS の録画の既定のファイル名と同じ形 (「2000-01-01 00-00-00」)。
//   - セッションのフォルダーの直下には、録画ファイル (参加者ごとに画面共有と声の 2 つ) だけを置く。
//   - 録画の中間ファイル (録画ログ、セグメント、黒埋め用のフレーム、録画ファイルの作成のログと一時ファイル) は、直下の 1 つの
//     フォルダー (kMaterialsDirName) にまとめる。録画ログの `file` はそのフォルダーからの相対パス (`segments/...`)
//     である。
// 利用者が保存先を選んでいないときだけ、OBS の録画の保存先 (読めなければビデオフォルダー) の下に作る既定のフォルダーの名前。
// OBS の録画ファイルも日時の名前なので、直接置くと混ざるため、既定では 1 つ下げる。
inline constexpr const char *kRecordRootDirName = "cordmux";
inline constexpr const char *kLedgerFileName = "ledger.jsonl";
inline constexpr const char *kSegmentsDirName = "segments";
inline constexpr const char *kBlackDirName = "black";
// セッションが使用中であることを示すマーカーのファイル (中間ファイルのフォルダーの中)。録画を始めてから、その
// セッションの録画ファイルの作成が終わるまでだけあり、持っているプロセスが手放すと (クラッシュしたときも) OS が消す。
inline constexpr const char *kSessionLockFileName = "in_use.lock";

// 中間ファイルのフォルダーの名前。OBS の言語を切り替えても同じ名前になるように、英字の 1 語にする。source は OBS の
// ソースと紛れ、work と temp は消してよいフォルダーに見える (消すとセグメントファイルを失う) ので使わない。
inline constexpr const char *kMaterialsDirName = "data";

// 録画ファイルの名前の書式。%1 は人の名前、%2 は種類の語、%3 はセッションの日時 (セッションのフォルダーの名前と同じ)。
// 書式と語は、呼び出し元が ini から読んで渡す (cordmux.source.file_name.label、cordmux.source.file_screen_share.label、
// cordmux.source.file_voice.label)。record/ は libobs を呼び出さないためである。渡されなかったときの既定は、
// en-US.ini と同じ文字列にする。
inline constexpr const char *kDefaultOutputNameFormat = "%1 %2 %3";
inline constexpr const char *kDefaultScreenKindWord = "Screen Share";
inline constexpr const char *kDefaultVoiceKindWord = "Voice";

} // namespace cordmux::record

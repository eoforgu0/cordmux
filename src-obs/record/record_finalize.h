// SPDX-License-Identifier: Apache-2.0

// 録画のセッションの録画ファイルの作成。録画ログから作った引数で同梱の FFmpeg を録画ファイル 1 つにつき 1 回起動し、終了コードを
// 確認し、録画ファイルの長さを録画ログと比べて、結果を録画ログに書く。エントリポイントは run_finalize で、Recorder
// (recorder.cpp) が作成のスレッドから呼ぶ。流れは docs/architecture.md の「録画ファイルの作成」にある。
// このヘッダーは libavformat のヘッダーを読み込まず、libobs と Qt にも依存しない。Windows に固有の名前は
// platform/ の外に書かないので、FFmpeg の実行ファイルの場所は RecordPlatformOps::ffmpeg_path から受け取る。

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config/settings.h"
#include "record/record_args.h"
#include "record/record_types.h"

namespace cordmux::record {

// FFmpeg の子プロセスの終了を待つポーリングの間隔 (ミリ秒)。打ち切りの要求 (FinalizeRun::should_stop) は
// ポーリングの合間に確認する。終了まで待ち続けると打ち切れず、間隔を 0 にすると CPU のコアを 1 つ使い続ける。
inline constexpr int kFinalizePollMs = 200;

// 録画ファイルの作成 1 回の入力と、結果を受け取るコールバック。
struct FinalizeRun {
	RecordPlatformOps ops;
	FinalizeOptions options; // ffmpeg_path / session_dir / output_pcm / repair_truncated
	// 録画ファイルを作成する録画ログ。run_finalize はコピーを取らないので、呼び出し元は戻るまで生かしておく。
	const LedgerDocument *doc = nullptr;

	// 録画ログに 1 行追加する (finalize の行と、最後の session の行)。
	std::function<void(const std::string &line)> append_line;
	// 進み具合が変わるたびに呼ぶ。ドックの進み具合の表示に使う。
	std::function<void(const FinalizeProgress &)> on_progress;
	// true を返したら打ち切る。Recorder は、OBS の終了で録画ファイルの作成を待つ上限に達したときに true を返す。
	std::function<bool()> should_stop;
	// 子プロセスを起動する直前に、その出力を書き込むファイルのパスを渡す。失敗の種類を分ける手がかりは FFmpeg の
	// メッセージにしかないが、RecordPlatformOps::start_child には出力先の引数がないので、この関数で渡す。
	std::function<void(const std::string &path)> set_child_log;
};

// 録画ファイルを 1 つずつ作成し、最後の進み具合を返す。戻り値の `state` は Done・Pending・Failed のどれかで、1 つでも
// 失敗すれば Failed、打ち切れば Pending になる。ある録画ファイルの作成に失敗しても、次の録画ファイルへ進む。
// 1 人の失敗で全員の録画ファイルの作成を止めると、作成できたはずの録画ファイルまで作られないためである。
// 録画ファイルは並列に作成しない。並列にすると、一時ファイルに要る空き容量が録画ファイルの数だけ重なる。また、
// ProRes への再エンコードは実測で実時間の最大 4.2 倍かかるので、CPU を取り合うと全体が遅くなる。
// セグメントのファイルは、成功しても失敗しても削除しない。打ち切ったときは、作りかけの録画ファイルを削除する。
FinalizeProgress run_finalize(const FinalizeRun &run);

// 録画ファイルの作成に使う一時ファイル (生の PCM) を作る。全長分を 0 で埋めたファイルを作り、各セグメントの mov から
// libavformat で読み出した PCM を FinalizePlan::audio_writes の位置に書き込む。子プロセスは起動しない。
// 書き込む範囲が重なったときは、後の書き込みが勝つ。音声が早く届いて原点を置き直したセッションでは、範囲が
// 重なることがある。
// `session_dir` には録画の中間ファイルのフォルダーを渡す。録画ログの `file` は、このフォルダーからの相対パスである。
// 録画ファイルの PCM の形 (output_pcm) が `in24` のときは、書き込みながら f32 を 24 ビットに丸める。セグメントの
// ファイルは f32 のまま変えない。
// ファイルを作れないか、最後に閉じられなければ false を返す。読めないセグメントの範囲は 0 のまま残して続ける。
bool build_finalize_pcm(const FinalizePlan &plan, const std::string &session_dir,
			config::RecordPcmFormat output_pcm);

// 上と同じ。書き込むたびに、書いた量と書く予定の全体の量 (バイト) を on_written に渡す。全体の量は、書く前に
// セグメントのサンプルの数から分かる。録画ファイルの作成の進み具合の表示に使う。
bool build_finalize_pcm(const FinalizePlan &plan, const std::string &session_dir,
			config::RecordPcmFormat output_pcm,
			const std::function<void(std::int64_t written_bytes, std::int64_t total_bytes)> &on_written);

// FFmpeg の `-progress` の出力 (`key=value` の行の並び) から、最後の `out_time_us` の値を読む。改行で終わって
// いない最後の行と、値が 64 ビットの整数として読めない行 (`N/A` など) は数えない。例外は投げない。
// どの行も読めなければ値なしを返す。
std::optional<std::int64_t> parse_progress_out_time_us(const std::string &text);

// 途中で切れたセグメント (録画ログの `truncated`) を、読めるサンプルだけのファイルに修復する前処理。
// run.options.repair_truncated が真のとき (resume_pending で再開したとき) だけ修復し、偽なら何もしない。
// `working` は録画ファイルの作成に使う録画ログのコピーで、修復したセグメントの行を読めた長さに修正し、`file` を修復した
// ファイル (`_repair.mov`) に、`truncated` を偽にする。読めるサンプルが 1 つもないセグメントの行は `working` から外す。
// 修正した行と外した行は、元の `file` と `truncated` のまま run.append_line で録画ログにも追加する (同じ `file` の行は
// 後の行が勝つ)。同じセグメントの行が 2 つあっても、修復は 1 度だけにする。
struct RepairOutcome {
	std::vector<std::string> repaired;  // 修復したファイルの名前 (data/ からの相対パス)
	// 読めるサンプルがあるのに修復したファイルを書けなかったセグメントと、読み出しそのものに失敗したセグメント
	// (元の名前)。これらを使う録画ファイルの作成は失敗にする。
	std::vector<std::string> unwritten;
	bool stopped = false;               // 打ち切りの要求で止めた (セッションは Pending になる)
};
RepairOutcome repair_truncated_segments(const FinalizeRun &run, LedgerDocument &working);

} // namespace cordmux::record

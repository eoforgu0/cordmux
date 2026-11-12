// SPDX-License-Identifier: Apache-2.0

// 録画の I/O の部分 (Recorder)。受信したフレームと音声をセグメントのファイルに書いて録画ログに追記し、録画を
// 止めたら録画ファイルの作成 (record_finalize.h) を別のスレッドで実行する。作成待ちのセッションを数え、再開もする。
// on_* の関数は受け取ったデータをコピーしてキューに積むだけで、ファイルに書くのは書き込み専用のスレッド 1 つである。
// 録画が OFF の間は、受け取ったデータをコピーせず、セグメントも録画ログも書かない。
// このヘッダーは libavformat のヘッダーを読み込まず、libobs と Qt にも依存しない。pipeline/pipeline.h は値を持つ
// 構造体を定義するだけで、libobs を読み込まない。Windows に固有の名前は書かない。

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config/settings.h"
#include "pipeline/pipeline.h"
#include "record/record_search_dirs.h"
#include "record/record_segment.h"
#include "record/record_types.h"
#include "ws/listener.h"
#include "ws/wire.h"

namespace cordmux::record {

// 製品で使う RecordPlatformOps を作る。ほとんどの関数は platform/ の関数をそのまま呼ぶ。platform/ の関数を
// RecordPlatformOps に入れるのはここだけである。
// `child_log_path` は、次に起動する子プロセスの出力を書き込むファイルのパスで、start_child が起動するときの値を
// 読む。失敗の種類を分ける手がかりは FFmpeg のメッセージにしかないが、RecordPlatformOps::start_child には出力先の
// 引数がないので、パスをここから渡す。録画ファイルの作成は FinalizeRun::set_child_log でこの値を書き換える。
RecordPlatformOps make_real_record_platform_ops(std::shared_ptr<std::string> child_log_path);

// セッションごとの値。録画を ON にした時点の値をセッションにコピーし、そのセッションの録画ファイルの作成にもその値を使う。
// 録画ファイルの作成を待っている前のセッションに、後から変えた値は及ばない。ただし、録画ファイルの作成を再開するセッションは録画ログに
// この値がないので、再開した時点の値で録画ファイルを作成する (resume_pending)。
struct RecordSessionValues {
	// 保存先 (セッションのフォルダーを作る場所)。解決済みの UTF-8 の絶対パスを渡す。
	// 空か相対パスなら解決できなかったとみなし、このセッションは始めずに StartFailed にする。どの場所に
	// 書けなかったかは言えないので、`unwritable_dir` は空になる。構築のときに渡された場所は使わない。それを
	// 使うのは、set_session_values を 1 度も呼ばない呼び出し元 (テスト) だけである。
	// ここに書けなくても、別の場所には移さずに StartFailed にする。伝えずに別の場所へ書くと、利用者は設定した
	// 場所を探して、録画を見失う。
	std::string base_dir;
	config::RecordPcmFormat output_pcm = config::RecordPcmFormat::Float32;
	// 録画ファイルの名前の書式と、種類を表す語。空なら record_types.h の既定を使う。
	std::string output_name_format;
	std::string screen_kind_word;
	std::string voice_kind_word;
};

// 録画のセッションを管理する。on_* の関数は受け取ったデータをコピーしてキューに積むだけで、ファイルに書くのは
// Recorder が内部に持つ書き込み専用のスレッド 1 つである。録画ファイルの作成は別のスレッド 1 つが、キューに積んだ順に実行する。
// 段階 (RecordingSnapshot::phase) は、ON にすると Starting、書き込むスレッドがセッションのフォルダーと録画ログを
// 開けたら Recording、開けなければ StartFailed、OFF にすると Stopping、書き込むスレッドが終わると Idle になる。
// 空き容量が尽きたときは書き込むスレッドが自分で録画を止め、RecordingSnapshot::self_stopped_reason に理由を残す。
// 始められなかったとき (StartFailed) も、書き込むスレッドが自分で録画を OFF に戻す。どちらも、スナップショットを
// 定期的に読む呼び出し元 (Pipeline::poll_recording_self_stop) が request_disable を呼んで後始末をする。書き込む
// スレッドは自分を join できず、送信側に録画の OFF を送る手段も持たない (Recorder は ws::Listener を
// 持たない) ためである。
class Recorder {
public:
	// `base_dir` は解決済みの絶対パスを渡す。保存先の既定値は platform/ の中で決める。`gate` はテストだけが
	// 差し替える。常に true を返す関数に差し替えると、OFF のままでも RecordPlatformOps が呼ばれることを、テストが
	// 確認する (record_segment.h の recording_gate)。製品は既定の recording_gate のまま呼ぶ。
	// 構築のときに、`base_dir` の作成待ちのセッションを 1 度数える。数えるためにフォルダーとファイルを読むが、
	// ファイルとフォルダーの作成、書き込み、削除、子プロセスの起動はしない。
	Recorder(const config::Settings &settings, RecordPlatformOps ops, std::string base_dir,
		 RecordingGateFn gate = &recording_gate);
	~Recorder();
	Recorder(const Recorder &) = delete;
	Recorder &operator=(const Recorder &) = delete;

	// ---- 入力 (ws の io_thread から、Pipeline のロックを持ったまま呼ばれる) ----
	// 受け取ったデータをコピーしてキューに積むだけで、ファイルには書かない。Pipeline のロックを持ったまま呼ばれる
	// ので、ここで書き込みを待つと、すべてのストリームの入力と snapshot() が止まる。ロックの順は Pipeline のロック
	// から Recorder のロックへの 1 方向である。
	// 録画が OFF の間は、on_video と on_audio はデータをコピーせずに戻る。キューが settings の
	// record_queue_limit に達したら、そのデータを捨てて数え、そのストリームのセグメントを閉じる (映像は、次の
	// キーフレームから新しいセグメントに書く)。
	void on_video(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len,
		      std::int64_t arrived_steady_us, std::int64_t arrived_wall_us);
	void on_audio(const ws::FrameHeader &header, const std::uint8_t *payload, std::size_t len,
		      std::int64_t arrived_steady_us, std::int64_t arrived_wall_us);
	// 録画するストリームは `formats` (configure の stream_formats) で決め、router.route() の経路は見ない。録画は
	// OBS のソースを通らないので、枠に載っているかを条件にしない。録画が OFF の間も呼んでよい (最新の 1 つを
	// 保存するだけ)。
	void on_configure(const std::vector<pipeline::StreamFormat> &formats,
			  const std::map<std::string, std::string> &display_names);
	// 接続の状態を受け取る。録画中に接続が切れていた期間は RecordingSnapshot::gaps に入り、切れたときに開いていた
	// セグメントは閉じる。
	void on_connected();
	void on_disconnected(ws::DisconnectReason reason);
	// 録画ログに書く値 (プラグインのバージョン、ダミーを表示していた時間帯、送信側が IDR の前に SPS と PPS を
	// 付けた回数) を受け取る。録画が OFF の間も呼んでよい (最新の 1 つを保存するだけ)。プラグインのバージョンと
	// ダミーの時間帯は、セッションの始めと終わりに、その時点の値を session の行に書く (後の行が勝つ)。SPS と PPS を
	// 付けた回数は、セグメントを閉じるときに、その時点の累計をセグメントの行に書く。
	void on_session_facts(SessionFacts facts);

	// ---- 操作 ----
	// 録画を ON か OFF にする。状態が変わらないときは何もしないので、呼び出し元は同じ値を続けて渡してよい。
	// ただし false は、状態が変わらなくても書き込むスレッドを join する。自分で止まったセッション
	// (snapshot().self_stopped_reason が値を持つ) では ON/OFF のフラグが既に false になっており、join するのは
	// 呼び出し元だからである。false から戻った後は、書き込むスレッドは残っていない。false は join を待つので、
	// UI のスレッドからは request_disable を呼ぶ。
	// true は、recording_start_blocked(snapshot()) が真の間 (止める処理の最中、書き込むスレッドを止めている最中) は
	// 何もせずに戻る。呼び出し元は、ON にする前に同じ関数で確認する。
	void set_enabled(bool on);
	// 録画の OFF を、書き込むスレッドの終了を待たずに要求する。UI のスレッドからはこちらを呼ぶ。
	// 段階を Stopping にして戻り、書き込むスレッドの join は、止める処理のための別のスレッド 1 つが行う。join が
	// 済んだら段階を Idle にする。Stopping の間の ON は受け付けない。既に止めている最中か、書き込むスレッドを
	// 止めている最中なら何もしない (前のスレッドの join を待たない)。
	void request_disable();
	// 次に始めるセッションの値を設定する。録画中のセッションには及ばない。
	void set_session_values(RecordSessionValues values);
	// 録画ファイルの作成の進み具合が止まったとみなすまでの秒数を設定する (0 以下なら既定の kDefaultFinalizeStallS)。
	// 実行中の録画ファイルの作成の判定にもすぐ反映する。この判定は画面に表示するためのもので、セッションごとの値ではない。
	void set_finalize_stall_s(int seconds);
	// 録画ファイルの作成の済んでいないセッションが残る保存先の一覧 (record_search_dirs.h) を使い始める。
	// `store.load` で前の起動までの一覧を読み、まだ探していない保存先ごとに別のスレッドで `reachable` を呼んで
	// アクセスできるかを問い合わせ、true を返した保存先を探す保存先に追加して数え直す。この関数は問い合わせを
	// 待たずに戻り、スレッドは stop が join する。1 つのスレッドで順に問い合わせると、応答しない保存先
	// (スリープ中の外付けのドライブ、切れたネットワークの場所) の後ろの保存先が、その起動の間ずっと探されない。
	// 起動のときにアクセスできなかった保存先は、その保存先のスレッドが待ってから 1 度だけ問い合わせ直し、
	// アクセスできれば数え直す。既に探している保存先 (構築のときの保存先、セッションを始めた
	// 保存先など) は、この関数で行う集計でフォルダーを読めなかったときだけスレッドを起動し、待ってから 1 度だけ
	// 問い合わせる。待ちは stop ですぐ終わる。
	// 以後は、集計のたびとセッションを始めるたびに一覧を更新し、変わったら `store.save` で書く (false を返したら、
	// 次の集計でもう一度書く)。
	// `reachable` はどのスレッドから呼ばれてもよい形にする。応答しない保存先で戻らないと、OBS の終了
	// (stop) がその分待つ。スレッドの中で投げられた例外 (`reachable`、その後の集計、`store` の関数) は外に出さず、
	// その保存先はアクセスできなかった保存先と同じく一覧に残す。
	void remember_pending_dirs(PendingDirsStore store, std::function<bool(const std::string &)> reachable);
	// 上の問い合わせ直しまでの待ち時間 (ミリ秒) を設定する。0 以下なら既定の kPendingDirReprobeDelayMs に戻す。
	// remember_pending_dirs より前に呼ぶ。起動したスレッドは、起動したときの値で待つ。
	void set_pending_dir_reprobe_delay_ms(int ms);
	// 書き込むスレッドが動いているかを返す。OFF にした後にスレッドが残らないことを、テストが確認するのに使う。
	// どのスレッドから呼んでもよい (std::thread を直接読まず、ロックの下のフラグで答える)。request_disable の
	// 直後は、スレッドが終わるまで true のことがある。
	bool writer_running() const;
	// 画面に表示する状態のコピーを返す。どのスレッドから呼んでもよい。
	RecordingSnapshot snapshot() const;
	// OBS の終了のときに呼ぶ。書き込むスレッドを止めて join し、録画ファイルの作成を `finalize_wait_ms` まで待つ。上限で打ち
	// 切ったら子プロセスを止め、作りかけの録画ファイルを削除し、録画ログを `pending` にする。打ち切ったセッションは、
	// 次の起動で作成待ちとして数える。保存先を問い合わせているスレッドもここで join する。何度呼んでもよい
	// (デストラクターも stop(0) を呼ぶ)。
	void stop(int finalize_wait_ms);
	// 作成待ちのセッションを探し、録画ファイルの作成のキューに積む。ActionId::ResumeFinalize の操作で呼ばれる。
	// 作成待ちのセッションは、録画ログが Pending か Failed のセッションと、中断したセッション (録画ログが
	// NotStarted か Running で終わり、だれもロックファイルを持たず、読めるサンプルがある) である。キューに積んだ
	// セッションと作成中のセッションは除く。呼ばれるたびに試し、試す回数に上限を設けない。失敗すれば録画ログが
	// Failed に戻り、また作成待ちとして数える。起動のときには自動で作成しない。録画ファイルの作成は CPU とディスクを使い、
	// OBS を起動した直後は配信中のことがあるので、いつ録画ファイルを作成するかは利用者が決める。
	// 積む前にセッションのロックファイル (data/in_use.lock) を持ち、ほかの OBS が同じセッションを同時に作成しない
	// ようにする。ほかのプロセスが持っているセッションは積まない。
	// **残る形**: 印を持てない保存先 (印を作れない・分け合わない開き方を受け付けない) の場合は、ほかの OBS からは
	// 帳簿だけで決まる (この起動の中では自分の回として外す)。
	void resume_pending();

	// 録画の入力を通ったメッセージの、ストリームごとの最後の到着時刻を返す。枠への経路の有無に関係なく
	// 記録するので、Pipeline の last_frames とは別の表である。録画のためだけにストリームを
	// 受け取り続けていることを、ドックに表示するのに使う。
	std::map<std::uint32_t, pipeline::RecordOnlyFrameClock> record_only_frames() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace cordmux::record

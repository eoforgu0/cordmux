// SPDX-License-Identifier: Apache-2.0

// DockPoll に渡す設定 (DockPollConfig)。Qt を含まない。Qt を含む dock_poll.h と分けてあるのは、DockPollConfig から
// アンインストールの入力を作る純粋関数 (dock/install_flow.h の uninstall_context_of) を、Qt を使わない単体テストで
// 確認するためである。uninstall_context_of がフィールドを渡し忘れると、アンインストールが毎回途中で止まる。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "bundle/bundle_files.h"
#include "config/settings.h"
#include "ledger/observation.h"
#include "platform/os_probe.h"

namespace cordmux::dock {

// 起動時の書き直しを、もう一度試した結果 (DockPollConfig::retry_startup_rewrite)。pipeline::StartupRewriteOutcome の
// うち、DockPoll が知る必要のある違いだけにまとめる (このヘッダーが pipeline/ に依存しないため)。
enum class StartupRewriteRetry {
	Wrote,   // 書き直した (設定ウィンドウの「起動時の書き直し」を「書き直しました」にする)
	Waiting, // Vesktop が起動していたか、起動しているかを調べられなかった。次に起動していないと分かったら、また試す
	Done,    // 書き直す必要がなかったか、書き直せなかった。もう試さない (理由はログに書いてある)
};

// Vesktop が起動していて接続していない間に、Vesktop が終了したかを DockPoll が自分で調べる間隔 (マイクロ秒)。
// [Vesktopに追加] は Vesktop の起動中は押せず、状態タブの「Vesktop」の組 ([Vesktopから削除]) は Vesktop の起動中は
// 表示しない。利用者は Vesktop を終了してから押す。OsProbe の間隔 (接続していない間は 30 秒) を待つと、終了した後も
// 最大 30 秒、押せないまま (組が現れないまま) になる。接続している間に終了したときは、接続が切れた時点で OsProbe に
// 問い合わせ直させるので、ここでは接続していない間だけ調べる。調べるのはプロセスの一覧だけ
// (platform::vesktop_running_now) で、終了を見たら OsProbe にすぐ問い合わせ直させる。終了してからボタンを押せるように
// なるまでを、利用者が待たされたと感じない長さにする値で、測って決めたものではない。
inline constexpr std::int64_t kVesktopQuitCheckIntervalUs = 1'000'000;

// Vesktop の終了を自分で調べる時刻か (kVesktopQuitCheckIntervalUs)。次のすべてが成り立つときだけ調べる。
//   vesktop_running   OsProbe の結果 (かドックが自分で見た結果) で、Vesktop が起動している
//   link_connected    が偽 (接続している間に終了したときは、接続が切れた時点で OsProbe に問い合わせ直させる)
//   writes_stopped    が偽 (アンインストールを始めた後は、Vesktop の設定を書く操作がない)
//   waits_for_quit    Vesktop の終了で押せるようになる操作があるか、起動時の書き直しが Vesktop の終了を待っている。
//                     呼び出し元は、cordmux が有効で、Vesktop の設定を読めている (VencordDirState が Unknown でない) か、
//                     起動時の書き直しを待っているときに真にする。cordmux を無効にしている間と、Vesktop の設定を
//                     読めていない間は、[Vesktopに追加] のカードも [Vesktopから削除] の組も出ないので、毎秒調べない
// 前に調べた時刻から間隔が過ぎたときと、システム時刻が前に戻ったときに true を返す。
inline bool vesktop_quit_check_due(bool vesktop_running, bool link_connected, bool writes_stopped, bool waits_for_quit,
				   std::int64_t now_us, std::int64_t last_check_us)
{
	if (!vesktop_running || link_connected || writes_stopped || !waits_for_quit)
		return false;
	return now_us - last_check_us >= kVesktopQuitCheckIntervalUs || now_us < last_check_us;
}

// DockPollConfig::maintain_listener を呼ぶ間隔 (ミリ秒)。待ち受けていない間は、別の OBS が終了したか、ポートが
// 空いたかを確認するために try_open() を呼び直す。待ち受けている間は、session.json が消されたり書き換えられたり
// していないかを確認する。どちらも急がなくてよく、ファイルとポートへの問い合わせを毎回のポーリングで行わない
// ための値で、測って決めたものではない。
inline constexpr std::int64_t kMaintainListenerIntervalUs = 5'000'000;

// 合成のソースへの枠の並べ方の入力と、自分の枠を出すかの判断を進める間隔 (ミリ秒)。ドックの表示を更新する間隔
// (settings の dock_poll_ms。50 ms から 60 秒) とは別に、固定にする。新しく共有を始めた人の枠が配信の絵に映るまでの
// 遅れを、表示の間隔に引きずられないようにするためで、配信の絵の反映として遅れが気にならない短さ (1 秒に 4 回) と、
// 毎回の問い合わせが重くならない長さの間で決めた。測って決めたものではない。
inline constexpr int kLayoutStepIntervalMs = 250;

// 並べ方の入力を進める間隔。ドックの表示を更新する間隔 (dock_poll_ms) によらず、いつも kLayoutStepIntervalMs を返す。
inline constexpr int layout_step_interval_ms([[maybe_unused]] int dock_poll_ms)
{
	return kLayoutStepIntervalMs;
}

// maintain_listener を呼ぶ時刻か。前に呼んだ時刻から間隔が過ぎたときと、システム時刻が前に戻ったときに true を
// 返す (戻ったままだと、時刻が追いつくまで呼ばれなくなる)。
inline bool maintain_listener_due(std::int64_t now_us, std::int64_t last_us)
{
	return now_us < last_us || now_us - last_us >= kMaintainListenerIntervalUs;
}

// Vesktop との接続が、前のポーリングで「あり」、今回「なし」になったか。このときドックは OsProbe にすぐ 1 回
// 問い合わせさせる。Vesktop の通知領域の [Repair Vencord] は、同梱の Vencord のフォルダーを上書きしてから Vesktop を
// 起動し直すので、接続が切れた直後の問い合わせで上書きに気付けば、30 秒の間隔を待たずにメッセージを出せる。
inline bool link_just_lost(bool was_connected, bool is_connected)
{
	return was_connected && !is_connected;
}

// ポーリングの間隔と、押された操作を実行するのに必要な値。どれも plugin-main.cpp が設定する。
struct DockPollConfig {
	// ドックが状態を読む間隔 (ミリ秒。settings の dock_poll_ms)。
	int poll_ms = 200;

	// フレームが届かない時間がこの長さを超えると、映像が止まったと判断する (settings の video_stall_ms)。
	std::int64_t video_stall_threshold_us = ledger::kDefaultVideoStallUs;

	// Vesktop の設定ファイル (state.json) に書き込んでよいか (settings の allow_state_json_write)。
	// settings の既定値は true だが、このフィールドの既定値は false にしてある。plugin-main.cpp が
	// 設定し忘れたときに、Vesktop の設定を書き換えないためである。
	bool allow_state_json_write = false;

	// 画面に表示する cordmux.dll のバージョン (ws::ListenerConfig::server_version と同じ文字列)。
	std::string plugin_version;

	// [Vesktopに追加] で、Vesktop の vencordDir に書き込むフォルダー (配布用の zip の data/vencord)。
	std::string bundled_vencord_dir;

	// cordmux.dll があるフォルダー。vencordDir のバックアップ (vencord_dir_backup.json) に保存する。
	std::string dll_dir;

	// vencordDir のバックアップ (vencord_dir_backup.json) のパス。
	std::string backup_json_path;

	// state.json とバックアップのファイルを読むときの、ファイルの大きさの上限。OsProbe のワーカーと同じ値
	// (platform/os_probe.h の kDefaultMaxFileBytes) を使う。別の値にすると、2 つの値の間の大きさの state.json で、
	// ドックの表示では読めているのに、ボタンを押したときだけ読めない、という食い違いが起きる。
	std::size_t max_json_bytes = platform::kDefaultMaxFileBytes;

	// 起動時に plugin-main.cpp が settings.json から読んだ設定。DockPoll はこれをコピーして持ち
	// (DockPoll::settings_)、利用者が画面で変えた値をそのコピーに書く。Pipeline が作られたときにコピーした設定は
	// 起動時の値のままなので、画面に表示する値と合成のソースに渡す値は、DockPoll のコピーから取る。
	config::Settings settings;

	// 変えた設定を書き込む settings.json のパス (cordmux の設定フォルダーの中)。空なら書き込まない。
	std::string settings_json_path;

	// settings.json はあったが、読めなかった (中身の形式が違った)。true のときは settings.json に一切書き込まない。
	// このとき settings に入っているのは既定値なので、書き込むと、利用者が書いた値がすべて既定値に置き換わる。
	// seen_people.json を読めなかったときも同じ扱いにする。ファイルがないときは false である。初めての起動では
	// ファイルがないのが普通で、書き込んでよい。
	bool settings_file_unreadable = false;

	// これまでに見た人の一覧 (seen_people.json) のパス (cordmux の設定フォルダーの中)。空なら読み書きしない。
	std::string seen_people_json_path;

	// 診断情報をエクスポートするときに、接続のトークンを消す関数 (redact) と、zip に入れる中身にトークンが
	// 残っていないかを確認する関数 (reveals_secret)。トークンを持っているのは plugin-main.cpp だけなので、
	// DockPoll には関数だけを渡す。どちらも空なら、トークンを消すことも確認することもしない。
	bundle::RedactFn redact;
	bundle::RevealFn reveals_secret;

	// data/vencord-src のパス (platform::OsProbeConfig::bundled_vencord_src_dir と同じ値)。ここにある
	// cordmux-build.txt (install/build_mark.h) から同梱の Vencord のバージョンを読み、[再インストール] の
	// コピー元にする。空なら cordmux-build.txt を読めないものとして扱い、バージョンは "-" と表示し、
	// [再インストール] では vencord-src をコピーできなかったものとして表示する。
	std::string bundled_vencord_src_dir;

	// この起動で、obs_module_load が Vesktop の vencordDir を書き直したか。ドックは、設定ウィンドウの
	// 「バージョン情報」のページに表示するだけである (後で retry_startup_rewrite が書き直したときも true にする)。
	bool rewrote_vencord_dir_at_startup = false;

	// 起動時の書き直しを、Vesktop の終了を待ってもう一度試す関数。obs_module_load が書き直そうとしたときに、Vesktop が
	// 起動していた (か、起動しているかを調べられなかった) ときだけ、plugin-main.cpp が入れる。空なら待っていない。
	// DockPoll は、OsProbe の新しい結果で Vesktop が起動していないと分かるたびに呼び、Waiting でない結果が返ったら、
	// もう呼ばない (書き直すのは 1 回だけ)。引数は、呼ぶ時点で cordmux が有効か。関数は、書く直前にも Vesktop が
	// 起動していないことを確認する (pipeline/pipeline.h の run_startup_vencord_dir_rewrite)。
	std::function<StartupRewriteRetry(bool feature_enabled)> retry_startup_rewrite;

	// アンインストールの最初の手順で、接続の待ち受けを止める関数 (install::UninstallContext::stop_listening に
	// 渡す)。Listener を持っているのは plugin-main.cpp なので、関数で受け取る。
	std::function<void()> stop_listening;

	// 待ち受けの世話をする関数。kMaintainListenerIntervalUs ごとに、UI のスレッドから呼ぶ。待ち受けていなければ
	// 開き直し、待ち受けていれば session.json を確認する。トークンを持っているのは plugin-main.cpp だけなので、
	// 関数で受け取る。空なら何もしない。
	std::function<void()> maintain_listener;

	// OBS を再起動しなくても反映される設定 (cordmux の有効と無効、画質の上限) が変わったときに、変わった後の設定を
	// 渡して呼ぶ。呼び出された側は Pipeline に渡し、Pipeline が送信側に media と limits を送り直す
	// (docs/wire_format.md の「メッセージの種類」)。起動時の値は plugin-main.cpp が settings から渡すので、
	// 変わったときにだけ呼ぶ。
	std::function<void(const config::Settings &)> apply_runtime_settings;
};

} // namespace cordmux::dock

// SPDX-License-Identifier: Apache-2.0

// 127.0.0.1 で送信側からの WebSocket の接続を待ち受ける Listener。同時に受け付ける接続は 1 つで、最初のメッセージ
// (hello) のトークンが一致した接続からだけメッセージを受け取り、コールバックに渡す。コールバックはどれも io_thread
// (asio のハンドラーを動かす 1 つのスレッド) から呼ぶ。ワイヤー形式は docs/wire_format.md にある。
// libobs に依存せず、asio と websocketpp で動く。トークンのファイルの読み書きとプロセスの確認は、PlatformOps で
// 受け取った platform/ の関数で行う。

#pragma once

#include "platform/platform.h"
#include "ws/control_messages.h"
#include "ws/log_sink.h"
#include "ws/wire.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cordmux::ws {

// ws/ が使う platform/ の関数。単体テストが実際の session.json を読み書きせずに済むように、関数オブジェクトで
// 受け取る (テスト用の実装は tests/ws_test_helpers.h にある)。
struct PlatformOps {
	std::function<cordmux::platform::ProcessIdentity()> current_process_identity;
	std::function<bool(const cordmux::platform::SessionInfo &)> write_session_file;
	// 読めたら out に書いて true を返す。ないか壊れていれば false を返し、out は変えない。
	std::function<bool(cordmux::platform::SessionInfo &out)> read_session_file;
	std::function<bool(uint32_t pid, int64_t started_at)> is_process_alive;
	// session.json を削除する。もうなければ true を返す。
	std::function<bool()> delete_session_file;
};

// cordmux.dll が使う実装。platform/platform.h の関数をそのまま呼ぶ。
PlatformOps make_real_platform_ops();

struct ListenerConfig {
	uint16_t base_port = 4470;    // 待ち受けを試す最初のポート番号
	int port_attempts = 16;       // base_port から 1 ずつ増やして試すポートの数
	// ping の間隔と、pong が続けて返らなかったら切断する回数。どちらも目安の値で、測って決めたものではない。
	int ping_interval_ms = 5000;
	int max_missed_pongs = 2;
	// 接続してから hello が届くまで待つ時間。過ぎたら切断する。同時に受け付ける接続は 1 つだけなので、切断
	// しないと、hello を送らない接続が残り続けて、Vesktop が接続できなくなる。目安の値で、測って決めたものではない。
	int hello_timeout_ms = 5000;
	// 1 つのメッセージの長さの上限 (バイト)。目安の値で、測って決めたものではない。
	size_t max_message_bytes = 64ull * 1024 * 1024;
	// Origin ヘッダーがない接続は許す。Origin ヘッダーがあれば、この一覧のどれかと一致するときだけ許す (一覧が空なら、
	// Origin ヘッダーのある接続をすべて拒む)。Vesktop 1.6.7 の src/main/mainWindow.ts の loadUrl は、Discord の
	// ページを既定では https://discord.com から読み込み、設定によって canary か ptb から読み込むので、この 3 つにする。
	// これはソースを読んで決めた一覧で、実際の Vesktop が送る Origin ヘッダーの値 (送らないかどうかも含めて) は確認
	// していない。実際の Vesktop から接続したときのハンドシェイクの Origin ヘッダーを読めば決まる。
	std::vector<std::string> allowed_origins = {
		"https://discord.com",
		"https://canary.discord.com",
		"https://ptb.discord.com",
	};
	LogSink log = null_log_sink; // ログの出力先。Listener はトークンとポート番号をログに書かない
	// hello_ok で送信側に伝える cordmux.dll のバージョン (表示専用の値)。CORDMUX_VERSION_STRING は、ws/ws.cmake が
	// コンパイルの定義として渡す値である (値の形は ws/ws.cmake のコメント)。定義がないビルド (buildspec.json を
	// 読まない単体テストのビルドなど) では空にし、決まった文字列を埋め込まない。
#if defined(CORDMUX_VERSION_STRING)
	std::string server_version = CORDMUX_VERSION_STRING;
#else
	std::string server_version;
#endif
};

enum class OpenOutcome {
	Opened,             // 待ち受けを始めた
	BlockedByOtherWriter, // session.json を書いた別のプロセス (別の OBS) が動いているので、待ち受けなかった
	// base_port から port_attempts 個のポートがどれも使用中だった。ポートは開けたが接続の受け付けを始められ
	// なかったときもこの値になる (ドックの表示は同じで、原因は OpenResult::problem で分かる)。
	PortsExhausted,
	// この Listener は一度 Opened を返したので、stop() の後でも、もう一度は開けない (try_open() のコメント)。
	// 開き直すときは新しい Listener を作る。
	AlreadyOpenedBefore,
};

// try_open() の中で起きた、outcome だけでは分からない問題。呼び出し元 (plugin-main.cpp) はこの値を見て、OBS の
// ログに LOG_WARNING の行を 1 行書く。ws::LogSink はログレベルを持たず LOG_INFO でしか書けないので、
// ws/listener.cpp ではこの 2 つをログに書かない。OpenSummary::problem にもコピーし、ドックの表示に使う
// (dock/build_observation.cpp が ledger::ListenerFacts にコピーする)。SessionFileNotSaved のとき outcome は
// Opened のままなので、outcome だけで表示を決めると、Vesktop が接続できないのに待ち受け中と表示してしまう。
enum class OpenProblem {
	None,
	// Opened だが、接続のための情報のファイル (ポート番号とトークン。session.json) を保存できなかった。待ち受けは
	// 続けるが、Vesktop はファイルを読めないので接続できない。
	SessionFileNotSaved,
	// ポートは開けたが、接続の受け付けを始められなかった (outcome は PortsExhausted で、待ち受けていない)。
	AcceptFailed,
};

struct OpenResult {
	OpenProblem problem = OpenProblem::None;
	OpenOutcome outcome = OpenOutcome::PortsExhausted;
	uint16_t port = 0;             // Opened のとき
	std::string secret_hex;        // Opened のとき
	uint32_t other_writer_pid = 0; // BlockedByOtherWriter のとき
	int64_t other_writer_started_at = 0; // BlockedByOtherWriter のとき
	// 待ち受けを試したポート番号の範囲 (Opened と PortsExhausted のときだけ入る)。待ち受けを始められないときの
	// メッセージ (ledger::PrimaryRuleId::ListenPortsExhausted) が、この範囲を表示する。ポートを開こうとしなかった
	// とき (BlockedByOtherWriter と AlreadyOpenedBefore) は 0 のままにし、試していない範囲を表示させない。
	uint16_t port_first = 0;
	uint16_t port_last = 0;
};

// try_open() の結果のうち、ドックの表示とログに使ってよい部分のコピー。トークン (secret_hex) は持たない
// (トークンをログにも画面にも出さないため)。
struct OpenSummary {
	bool attempted = false; // try_open() を 1 度でも呼んだか (まだ試していないことを、結果と区別するため)
	OpenOutcome outcome = OpenOutcome::PortsExhausted;
	uint16_t port = 0;
	uint32_t other_writer_pid = 0;
	int64_t other_writer_started_at = 0;
	uint16_t port_first = 0;
	uint16_t port_last = 0;
	// try_open() の中で起きた問題 (OpenResult::problem のコピー)。開こうとしなかったときと、問題がなかったときは
	// None。outcome と同じく、待ち受けを始めたときに起きたことなので、stop() の後も残る。
	OpenProblem problem = OpenProblem::None;
};

// check_session_file() の結果。呼び出し元 (plugin-main.cpp) がログの行を決めるのに使う。
enum class SessionCheck {
	NotApplicable,     // 待ち受けていないか、Vesktop が接続しているので、確認しなかった
	Unchanged,         // 自分が書いた中身のままだった
	Rewritten,         // ないか違う中身だったので、書き直した
	LeftToOtherWriter, // 動いている別のプロセスが書いた中身だったので、書き直さなかった
	WriteFailed,       // 書き直せなかった。保存できない状態になったときの 1 回だけ返す
	WriteStillFailing, // 書き直せなかった。保存できない状態が続いている
};

// 接続が切れた理由。ドックが、接続が切れたことを表示するときに使う。
//
// 名前は ledger/observation.h の DisconnectReason と 1 対 1 に対応する。LocalClose だけは ledger の側に対応する値が
// ない。こちらから閉じたとき (cordmux を無効にしたときと OBS の終了) は、表示の規則が切断ではなく、cordmux を
// 無効にしている状態 (ledger::PrimaryRuleId::FeatureOff) を表示するので、切れた理由としては表示しない。
enum class DisconnectReason {
	Unknown,           // 理由が分からない (分からないことを断定しない)
	SecretMismatch,    // トークンが一致しなかった (HelloNgReason::BadSecret)
	WireMismatch,      // ワイヤー形式のバージョンが違った (HelloNgReason::BadWire)
	MalformedHello,    // 最初のメッセージが hello でないか、壊れていた (HelloNgReason::Malformed)
	HelloTimeout,      // 接続したまま hello を送ってこなかった
	HeartbeatTimeout,  // ハートビートの pong が続けて返らなかった
	PeerClosed,        // 相手が切断した
	LocalClose,        // こちらから閉じた (Listener::stop)
};

struct ListenerStats {
	uint64_t rejected_clients = 0;       // 2 つ目以降の接続を拒否した回数
	uint64_t dropped_invalid_frames = 0; // decode_frame_header が読めずに捨てたバイナリメッセージの数
	uint64_t dropped_unknown_control = 0; // type が分からないか形が壊れていて捨てた制御用の JSON の数
	uint64_t hello_secret_mismatches = 0; // hello のトークンが一致しなかった回数
	// Origin か Host が一致せずに拒んだ回数。切れた理由 (DisconnectReason) には入れない。拒んだ接続は on_validate で
	// 拒否され、1 度も接続しないので、接続した後に切れたことにはならない。
	uint64_t handshake_rejections = 0;
	uint64_t heartbeat_timeouts = 0;      // pong が返らずに切断した回数
	uint64_t hello_timeouts = 0;          // hello を送らない接続を切断した回数
	// server.send() がエラーを返した回数。数えるだけで、判断には使わない。この値を確認する単体テストはない。
	uint64_t send_failures = 0;
	bool client_connected = false;        // hello が通った接続がいまあるか
	// いま待ち受けているか (is_listening() と同じ値)。stop() の後は、open.outcome が Opened のままでも false に
	// なる。ドックは、この値で ledger::ListenerState の Listening と Closed を分ける。
	bool listening = false;

	// ---- ドックと診断情報が読む値 ----

	// try_open() の結果のコピー (トークンを含まない)。
	OpenSummary open;

	// 時刻はすべて、システム時刻のエポックからのマイクロ秒 (0 はまだないことを示す)。ドックが切れた時刻を表示する
	// (cordmux.dock.disconnected.headline) ので、steady_clock ではなくシステム時刻で持つ。ledger::Observation の
	// 時刻と同じ単位である。
	int64_t connection_opened_at_us = 0; // 最後に接続を受け付けた時刻 (hello の前)
	int64_t hello_ok_at_us = 0;          // 最後に hello が通った時刻
	int64_t connection_closed_at_us = 0; // 最後に接続が切れた時刻
	DisconnectReason last_disconnect_reason = DisconnectReason::Unknown;
	uint64_t connections_accepted = 0;   // 受け付けた接続の数 (hello が通らなかった接続も含む)

	// ハートビートの状態。生きているかの判定は heartbeat_alive() で行う。
	uint64_t pings_sent = 0;
	uint64_t pongs_received = 0;
	int unanswered_pings = 0;  // 現在の接続で、pong が返っていない ping の数
	int64_t last_pong_at_us = 0;

	// いま接続している送信側が hello で送ったバージョン (ledger::Versions::discord_side にコピーする)。切断すると
	// 空に戻る。表示にだけ使い、値を信用しない (HelloMessage::version と同じ)。長さも文字も送信側が送ったままな
	// ので、表示する側で収まる形にすること。
	std::string peer_version;
	// いま接続している送信側が、hello の任意のキー vesktop で送った Vesktop のバージョン
	// (ledger::Versions::vesktop にコピーする)。扱いは peer_version と同じである。空なら分からない (送信側が
	// 読めなかったか、接続していない)。
	std::string peer_vesktop_version;
};

// ハートビートが生きているか。pong が返っていない ping の数が max_missed_pongs に達していなければ true を返す。
// max_missed_pongs には、呼び出し元が ListenerConfig::max_missed_pongs を渡す。Listener を持たないドックも同じ
// 判定を使えるように、引数にしている。接続がなければ false を返す。接続していないことはハートビートとは別の
// 状態なので、呼び出し元は先に client_connected を見ること。
inline bool heartbeat_alive(const ListenerStats &stats, int max_missed_pongs)
{
	return stats.client_connected && stats.unanswered_pings < max_missed_pongs;
}

// 127.0.0.1 で WebSocket の接続を待ち受ける。同時に受け付ける接続は 1 つだけである。
class Listener {
public:
	Listener(ListenerConfig cfg, PlatformOps ops);
	~Listener();

	Listener(const Listener &) = delete;
	Listener &operator=(const Listener &) = delete;

	// base_port から順に port_attempts 個のポートで、127.0.0.1 での待ち受けを試す。session.json を書いた別の
	// プロセス (別の OBS) が動いていれば、ポートを開かずに BlockedByOtherWriter を返す。ポートを開けたらトークンを
	// 作り、接続の受け付けと io_thread を始めてから、ポート番号とトークンを session.json に書く。
	// 一度 Opened を返したインスタンスでもう一度呼ぶと、AlreadyOpenedBefore を返す。stop() は io_context を止めて
	// io_thread を join し、このクラスはそれを動かし直さないためである。開き直すときは新しい Listener を作る。
	OpenResult try_open();

	// try_open() をもう一度呼べば、開ける可能性があるか。BlockedByOtherWriter か PortsExhausted が返った後で、まだ
	// 待ち受けていないときだけ true を返す (AlreadyOpenedBefore になるのは、一度 Opened を返した後だけである)。
	// 別の OBS が終了したことや、ポートが空いたことは、呼び直さないと分からない。どのスレッドから呼んでもよい。
	bool can_retry_open() const;

	// 待ち受けていて、Vesktop が接続していないときに、session.json が try_open() で書いた中身 (ポート番号、トークン、
	// pid、起動の時刻) のままかを確認し、ないか違うか壊れていれば書き直す。Vesktop は繋ぎ直すたびに session.json を
	// 読み直すので、消されたり書き換えられたりしたままだと、OBS を再起動するまで繋がらない。
	// 中身が違っていても、書いたのが動いている別のプロセスなら書き直さない (2 つの OBS が書き合いを続けないため)。
	// そのときログに 1 行書く (この Listener で 1 回だけ)。書けなかったときは stats().open.problem を
	// SessionFileNotSaved にし、次に書けたら None に戻す。呼ぶのは、定期的に見回る 1 つのスレッドとする。
	SessionCheck check_session_file();

	// session.json の pid と起動の時刻が自分と同じなら、削除して true を返す。ないときと別のプロセスの物のときは
	// 何もせず false を返す。呼ぶのは stop() の後 (OBS の終了のとき)。OBS が落ちたときは削除されずに残るが、次の
	// 起動の try_open() が書き直す。
	bool remove_session_file_if_mine();

	// 待ち受けをやめ、io_context を止めて io_thread を join する。何度呼んでもよい。on_disconnected は呼ばないので、
	// 接続が切れたときの後始末は呼び出し元が行う (pipeline/pipeline.h の on_listener_stopped)。この Listener で
	// もう一度待ち受けることはできない (try_open() のコメント)。
	void stop();

	bool is_listening() const;
	uint16_t port() const;
	ListenerStats stats() const;

	// 接続していれば keyframe_request を送り、接続していなければ何もしない。どのスレッドから呼んでもよい
	// (asio::post で io_thread に渡す)。
	void request_keyframe(uint32_t stream_id);

	// 接続していれば stream_check を送り、接続していなければ捨てる (request_keyframe と同じ。覚えない)。映像が届かない
	// ストリームの確認を頼むのは pipeline/ が決める。どのスレッドから呼んでもよい (asio::post で io_thread に渡す)。
	void send_stream_check(uint32_t stream_id);

	// 接続していれば recording を送り、接続していなければ捨てる (request_keyframe と同じ)。呼ぶのは、録画の
	// 状態が変わったときと、接続した直後で、タイミングは pipeline/ が決める。制御用の JSON の形は ws/ だけが
	// 持つので、JSON はこの関数の中で作る。どのスレッドから呼んでもよい (asio::post で io_thread に渡す)。
	void send_recording(bool on);

	// 接続していれば media と limits を送る。扱いは send_recording と同じだが、最後に渡された値を覚えておき、
	// 次に hello が通ったときの hello_ok に初期値として入れる (HelloOkMessage の任意のキー)。1 度も呼ばれて
	// いなければ hello_ok にキーを入れず、送信側は読み出しを許された状態で動く。呼ぶタイミング (値が変わったとき、
	// 接続した直後) は pipeline/ が決める。範囲の外の limits (limits_in_range が false) は、覚えも送りもせず、
	// ログに 1 行書く。
	void send_media(bool on);
	void send_limits(const LimitsMessage &limits);

	// 接続していれば main_account、receive_scope、move_to_main_call、leave_call を送り、接続していなければ捨てる
	// (send_recording と同じで、覚えない。接続した直後に pipeline/ が現在の値を送り直す)。move_to_main_call と
	// leave_call は、利用者が押したときだけ呼ぶ。接続していなかったときに送り直さない (押したのに、あとから
	// 通話が動くことがないようにするため)。どのスレッドから呼んでもよい (asio::post で io_thread に渡す)。
	void send_main_account(const std::string &user_id);
	void send_receive_scope(const ReceiveScopeMessage &scope);
	void send_move_to_main_call();
	void send_leave_call();

	// 受け取ったメッセージを渡すコールバック。どれも io_thread から呼ぶ。nullptr でもよい。
	std::function<void(const FrameHeader &, const uint8_t *payload, size_t len)> on_video_frame;
	std::function<void(const FrameHeader &, const uint8_t *payload, size_t len)> on_audio_frame;
	std::function<void(const ConfigureMessage &)> on_configure;
	std::function<void(const LogMessage &)> on_log;
	std::function<void(const FactsMessage &)> on_facts;
	// 参加者ごとの表示名、ユーザー名、アバターのバージョン、Bot の印。nullptr のままでも、profiles を dropped_unknown_control には
	// 数えない。数えると、送信側とのバージョンの食い違いを見つけるための値が、通話の間ずっと増え続ける。
	std::function<void(const ProfilesMessage &)> on_profiles;
	// 1 人のアバターの画像。nullptr のままでも dropped_unknown_control には数えない (on_profiles と同じ理由)。
	std::function<void(const AvatarMessage &)> on_avatar;
	// stream_check の結果。nullptr のままでも dropped_unknown_control には数えない (on_profiles と同じ理由)。
	std::function<void(const StreamCheckResultMessage &)> on_stream_check_result;

	// hello が通って接続したときと、その接続が切れたとき (websocketpp の close か fail、ハートビートでの切断) に
	// 呼ぶ。hello が通る前の切断と、stop() では呼ばない。どちらも io_thread から呼ぶ。nullptr でもよい。
	//
	// Pipeline は on_disconnected で、StreamRouter の経路と画面のデコードのスレッドを片付ける。これがないと、
	// Vesktop か OBS の片方を再起動して接続し直すたびに、古い経路とスレッドが残る。
	std::function<void()> on_connected;
	std::function<void()> on_disconnected;

	// hello と pong が運んだ送信側の時計の読み (clock_us。送信側がメッセージを送る前に読んだ値) と、そのメッセージを
	// 受け取った時刻 (received_us。receive_clock_us の時計) を渡す。受け取った時刻は、メッセージを読む前 (JSON を
	// 解析する前) に読んだ値で、hello の分を on_connected の後に呼んでも、on_connected の処理の時間は入らない。
	// キーがないメッセージでは呼ばない。io_thread から呼ぶ。nullptr でもよい。
	std::function<void(std::int64_t sender_clock_us, std::int64_t received_us)> on_sender_clock;

	// テキストのメッセージを受け取った時刻を読む時計 (us)。on_sender_clock の received_us に使う。Pipeline は OBS の時計
	// (os_gettime_ns / 1000) を渡す (このフォルダーは libobs に依存しないので、時計は呼び出し元が決める)。nullptr なら
	// received_us は 0 になる。io_thread から呼ぶ。
	std::function<std::int64_t()> receive_clock_us;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace cordmux::ws

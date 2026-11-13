// SPDX-License-Identifier: Apache-2.0

// ws/listener.h の実装。WebSocket は websocketpp と standalone asio で扱い、外から届くバイト列を読むコードを自分で
// 書かない。使うバージョンは ws/ws_deps.cmake にある。
// websocketpp のログ (access log と error log。ハンドシェイクの Host ヘッダーなどを含む) は止め、ログはすべて
// Impl::log() から ListenerConfig::log に渡す。トークンとポート番号を書かないことを、1 か所で守るためである。

// asio と websocketpp は外部のライブラリなので、そのヘッダーが /W4 で出す警告は、include の間だけ表示しない
// (`#pragma warning(push, 0)` で警告レベルを 0 にし、`pop` で元に戻す)。これで frame.hpp、md5.hpp、sha1.hpp の行の
// 警告は消える。
//
// 1 つだけ消えない警告がある。websocketpp 0.8.2 の hybi00 (廃止された古い WebSocket の形式) の処理が std::fill を
// char のバッファーに int で呼ぶので、websocketpp の行ではなく、MSVC の <xutility> の _Fill_memset の行に C4244 が
// 出る。<xutility> はほかの標準ヘッダーを通して、この `#pragma` より前に読み込まれるので、ここで囲んでも消えない。
// そのため ws/ws_deps.cmake の cordmux_add_ws_deps が、ws/listener.cpp にだけ /wd4244 を付けている。この指定で、
// ws/listener.cpp のほかの行の C4244 も表示されなくなる。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <asio.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "ws/listener.h"

#include "ws/secret.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace cordmux::ws {

namespace {

using server_t = websocketpp::server<websocketpp::config::asio>;
using message_ptr = server_t::message_ptr;

std::string to_lower(std::string s)
{
	for (char &c : s)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// websocketpp の connection_hdl は shared_ptr<void> を指す weak_ptr で、operator== を持たない。そのため
// owner_before で両方向に比べる。
bool same_connection(websocketpp::connection_hdl a, websocketpp::connection_hdl b)
{
	return !a.owner_before(b) && !b.owner_before(a);
}

// 現在のシステム時刻のエポックからのマイクロ秒。接続した時刻と切れた時刻はドックに表示するので、steady_clock
// ではなくシステム時刻で持つ (ledger::Observation の時刻と同じ単位)。
int64_t wall_now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

} // namespace

PlatformOps make_real_platform_ops()
{
	PlatformOps ops;
	ops.current_process_identity = &cordmux::platform::current_process_identity;
	ops.write_session_file = &cordmux::platform::write_session_file;
	ops.read_session_file = &cordmux::platform::read_session_file;
	ops.is_process_alive = &cordmux::platform::is_process_alive;
	ops.delete_session_file = &cordmux::platform::delete_session_file;
	return ops;
}

struct Listener::Impl {
	explicit Impl(Listener *owner_, ListenerConfig cfg_, PlatformOps ops_)
		: owner(owner_), cfg(std::move(cfg_)), ops(std::move(ops_))
	{
	}

	Listener *owner;
	ListenerConfig cfg;
	PlatformOps ops;

	server_t server;
	// asio の io_context::run() は、待っている処理が 1 つもないとすぐに戻る。いまは start_accept() の後にスレッドを
	// 起動するので戻らないが、順番が変わると、run() が先に戻って、その後の accept も ping も処理されなくなる。
	// work_guard を持って、stop() まで run() が戻らないようにする。
	std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard;
	std::thread io_thread;

	std::atomic<bool> listening{false};
	uint16_t bound_port = 0;
	std::string secret_hex;
	// try_open() が一度でも Opened を返したか。true なら、try_open() は AlreadyOpenedBefore を返す
	// (理由は ws/listener.h の try_open() のコメント)。
	bool ever_opened = false;

	// try_open() が session.json に書いた中身と、check_session_file() の状態。try_open() と check_session_file() と
	// remove_session_file_if_mine() が触るので、session_mu で守る。ほかのロックを取らないロックで、io_thread は
	// 取らない。
	std::mutex session_mu;
	std::optional<cordmux::platform::SessionInfo> written_session;
	bool other_writer_logged = false;

	std::atomic<uint64_t> rejected_clients{0};
	std::atomic<uint64_t> dropped_invalid_frames{0};
	std::atomic<uint64_t> dropped_unknown_control{0};
	std::atomic<uint64_t> hello_secret_mismatches{0};
	std::atomic<uint64_t> handshake_rejections{0};
	std::atomic<uint64_t> heartbeat_timeouts{0};
	std::atomic<uint64_t> hello_timeouts{0};
	std::atomic<uint64_t> send_failures{0};
	std::atomic<bool> client_connected{false};

	// ドックと診断情報が読む値。書くのは io_thread、try_open()、stop() で、stats() がほかのスレッドから読むので、
	// 上の数と同じく std::atomic で持つ。
	std::atomic<bool> open_attempted{false};
	std::atomic<OpenOutcome> open_outcome{OpenOutcome::PortsExhausted};
	std::atomic<OpenProblem> open_problem{OpenProblem::None};
	std::atomic<uint16_t> open_port{0};
	std::atomic<uint32_t> open_other_writer_pid{0};
	std::atomic<int64_t> open_other_writer_started_at{0};
	std::atomic<uint16_t> open_port_first{0};
	std::atomic<uint16_t> open_port_last{0};
	std::atomic<int64_t> connection_opened_at_us{0};
	std::atomic<int64_t> hello_ok_at_us{0};
	std::atomic<int64_t> connection_closed_at_us{0};
	std::atomic<DisconnectReason> last_disconnect_reason{DisconnectReason::Unknown};
	std::atomic<uint64_t> connections_accepted{0};
	std::atomic<uint64_t> pings_sent{0};
	std::atomic<uint64_t> pongs_received{0};
	std::atomic<int64_t> last_pong_at_us{0};

	// 送信側が hello で送ったバージョンと、任意のキー vesktop で送った Vesktop のバージョン (送らなければ空)。
	// 文字列は std::atomic にできないので、この 2 つのためのロックを持つ。ほかのロックを取らないロックで、取るのは
	// io_thread (hello が通ったときと切断したとき)、stop()、stats() だけである。
	std::mutex peer_mu;
	std::string peer_version;
	std::string peer_vesktop_version;

	// ここから下は io_thread (server.run()) の中だけで読み書きする。asio のハンドラーは 1 つのスレッドで順に動く
	// ので、ロックは使わない。stop() は io_thread を join した後に書く。unanswered_pings だけは stats() からも読む
	// ので std::atomic にしている (書くのは io_thread と、join した後の stop() だけである)。
	bool has_connection = false;
	websocketpp::connection_hdl active_hdl;
	enum class ConnState { AwaitingHello, Connected } conn_state = ConnState::AwaitingHello;
	// send_media と send_limits に最後に渡された値。hello_ok の任意のキー (初期値) に入れる。send_media と
	// send_limits は asio::post で io_thread に渡してから書くので、読み書きするのは io_thread だけである。1 度も
	// 渡されていなければ値を持たず、hello_ok のキーも省く。そのとき送信側は、読み出しを許された状態で動く。
	std::optional<bool> latest_media;
	std::optional<LimitsMessage> latest_limits;
	std::atomic<int> unanswered_pings{0};
	// こちらから接続を閉じるときに、閉じる直前に入れる理由。reset_connection_state が last_disconnect_reason に
	// 移して空に戻す。空のまま切れたら、相手が切断したものとする。
	std::optional<DisconnectReason> pending_disconnect_reason;
	uint64_t next_ping_seq = 1;
	std::shared_ptr<asio::steady_timer> ping_timer;
	// hello を送らない接続を切断するタイマー。on_validate が接続を受け付けたときに始め、hello が通ったときか、
	// 切断したときにキャンセルする。
	std::shared_ptr<asio::steady_timer> hello_timer;
	// 現在の接続で、送信の失敗をログに書いたか (send_text)。接続を受け付けるたびに false に戻す。
	bool send_failure_logged = false;

	void log(const std::string &line)
	{
		if (cfg.log)
			cfg.log(line);
	}

	// ---- ハンドシェイクのチェック ----
	//
	// Origin と Host を確認する。接続の認証はトークンで行い、この 2 つのチェックだけには頼らない。

	bool origin_allowed(const std::string &origin) const
	{
		if (origin.empty())
			return true; // Origin がないクライアントも許す (理由は下のコメント)
		for (const auto &allowed : cfg.allowed_origins) {
			if (origin == allowed)
				return true;
		}
		return false;
	}
	// Origin を送らないのはブラウザーの外のプログラムで、そのプログラムは Origin を自由に書けるので、拒んでも防げる
	// 相手は増えない。ブラウザーのページからの接続は Origin を偽れないので、一覧と比べて拒める。

	// Host が 127.0.0.1:<ポート番号> のときだけ許す。DNS リバインディングで、ほかの名前から接続されるのを防ぐ。
	bool host_allowed(const std::string &host) const
	{
		const std::string expected = "127.0.0.1:" + std::to_string(bound_port);
		return to_lower(host) == to_lower(expected);
	}

	bool on_validate(websocketpp::connection_hdl hdl)
	{
		server_t::connection_ptr con = server.get_con_from_hdl(hdl);

		if (has_connection) {
			// 同時に受け付ける接続は 1 つだけ。2 つ目は拒否して数える。
			con->set_status(websocketpp::http::status_code::service_unavailable);
			rejected_clients++;
			log("Refused a connection: Vesktop is already connected");
			return false;
		}

		const std::string host = con->get_request_header("Host");
		if (!host_allowed(host)) {
			con->set_status(websocketpp::http::status_code::forbidden);
			handshake_rejections++;
			log("Refused a connection: the Host header didn't match");
			return false;
		}

		const std::string origin = con->get_origin();
		if (!origin_allowed(origin)) {
			con->set_status(websocketpp::http::status_code::forbidden);
			handshake_rejections++;
			log("Refused a connection: the Origin isn't allowed");
			return false;
		}

		// has_connection を true にするのは、on_open ではなくここである。asio は接続ごとのハンドシェイクを非同期に
		// 進めるので、この接続の validate が true を返してから on_open が呼ばれるまでの間に、2 つ目の接続の validate が
		// 先に動くことがある。
		has_connection = true;
		active_hdl = hdl;
		conn_state = ConnState::AwaitingHello;
		unanswered_pings.store(0);
		next_ping_seq = 1;
		pending_disconnect_reason.reset();
		send_failure_logged = false;
		// 接続を受け付けた時刻は、hello が通る前のこの時点で記録する。トークンが一致せずに切れた接続にも、接続した
		// 時刻を残すためである (dock/build_observation.cpp の link_of)。
		connection_opened_at_us.store(wall_now_us());
		connections_accepted++;
		schedule_hello_timeout();
		return true;
	}

	void on_open(websocketpp::connection_hdl /*hdl*/)
	{
		log("Connection opened; waiting for Vesktop to identify itself");
	}

	void reset_connection_state()
	{
		// owner->on_disconnected を呼ぶのは、hello が通った後の切断だけである (ws/listener.h の on_disconnected)。hello の
		// 前の拒否とタイムアウトは、接続が切れたことにならないので呼ばない。
		const bool was_connected = client_connected;
		has_connection = false;
		active_hdl = websocketpp::connection_hdl();
		conn_state = ConnState::AwaitingHello;
		client_connected = false;

		// 切れた時刻と理由を残す。こちらから閉じたときは、閉じる直前に pending_disconnect_reason を入れてある。
		// 入っていなければ、相手が切断したものとする。
		connection_closed_at_us.store(wall_now_us());
		last_disconnect_reason.store(pending_disconnect_reason.value_or(DisconnectReason::PeerClosed));
		pending_disconnect_reason.reset();
		unanswered_pings.store(0);
		{
			// 切れた後は、接続していない相手のバージョンを表示し続けないように空にする。
			std::lock_guard<std::mutex> peer_lock(peer_mu);
			peer_version.clear();
			peer_vesktop_version.clear();
		}
		if (ping_timer) {
			asio::error_code ec;
			ping_timer->cancel(ec);
			ping_timer.reset();
		}
		if (hello_timer) {
			asio::error_code ec;
			hello_timer->cancel(ec);
			hello_timer.reset();
		}
		if (was_connected && owner->on_disconnected)
			owner->on_disconnected();
	}

	void on_close(websocketpp::connection_hdl hdl)
	{
		// websocketpp は、validate で拒んだ接続 (has_connection を true にしていない接続) でも fail と close を呼ぶ。
		// いま受け付けている接続と一致するときだけ後始末する。
		if (!has_connection || !same_connection(hdl, active_hdl))
			return;
		log("Disconnected from Vesktop");
		reset_connection_state();
	}

	void on_fail(websocketpp::connection_hdl hdl)
	{
		if (!has_connection || !same_connection(hdl, active_hdl))
			return;
		log("Connection from Vesktop failed during the handshake");
		// validate を通った後のハンドシェイクの失敗である (Origin か Host で拒んだ接続は、has_connection が false
		// なので上で戻る)。どちらの側の原因で失敗したかは分からないので、Unknown にする。
		if (!pending_disconnect_reason)
			pending_disconnect_reason = DisconnectReason::Unknown;
		reset_connection_state();
	}

	void send_text(const std::string &text)
	{
		websocketpp::lib::error_code ec;
		server.send(active_hdl, text, websocketpp::frame::opcode::text, ec);
		if (ec) {
			send_failures++;
			// 壊れた接続では送るたびに失敗することがあるので、ログに書くのは 1 つの接続で最初の 1 回だけにする。回数は
			// stats() の send_failures で分かる。
			if (!send_failure_logged) {
				send_failure_logged = true;
				log("Couldn't send a message to Vesktop");
			}
		}
	}

	void close_connection(websocketpp::close::status::value code, const std::string &reason)
	{
		websocketpp::lib::error_code ec;
		server.close(active_hdl, code, reason, ec);
	}

	void schedule_ping()
	{
		ping_timer = std::make_shared<asio::steady_timer>(server.get_io_service());
		ping_timer->expires_after(std::chrono::milliseconds(cfg.ping_interval_ms));
		std::weak_ptr<asio::steady_timer> weak_timer = ping_timer;
		ping_timer->async_wait([this, weak_timer](const asio::error_code &ec) {
			if (ec)
				return; // キャンセルされた (接続が切れたか、止めた)
			if (weak_timer.lock() != ping_timer)
				return; // 前の接続のタイマーが遅れて発火した
			on_ping_tick();
		});
	}

	void on_ping_tick()
	{
		if (!has_connection || conn_state != ConnState::Connected)
			return;

		// ping_interval_ms ごとに ping を送り、pong が max_missed_pongs 回続けて返らなければ切断する。
		if (unanswered_pings.load() >= cfg.max_missed_pongs) {
			heartbeat_timeouts++;
			log("Disconnected from Vesktop: it stopped responding");
			pending_disconnect_reason = DisconnectReason::HeartbeatTimeout;
			close_connection(websocketpp::close::status::normal, "heartbeat timeout");
			reset_connection_state();
			return;
		}

		PingMessage m;
		m.seq = next_ping_seq++;
		send_text(build_ping(m));
		pings_sent++;
		unanswered_pings++;
		schedule_ping();
	}

	// on_validate が接続を受け付けた時点 (has_connection を true にしたとき) に始める。hello が通って Connected に
	// 移るか、切断か拒否で reset_connection_state が呼ばれると、キャンセルされる。
	void schedule_hello_timeout()
	{
		hello_timer = std::make_shared<asio::steady_timer>(server.get_io_service());
		hello_timer->expires_after(std::chrono::milliseconds(cfg.hello_timeout_ms));
		std::weak_ptr<asio::steady_timer> weak_timer = hello_timer;
		hello_timer->async_wait([this, weak_timer](const asio::error_code &ec) {
			if (ec)
				return; // キャンセルされた (hello が届いたか、切れた)
			if (weak_timer.lock() != hello_timer)
				return; // 前の接続のタイマーが遅れて発火した (ping と同じ)
			on_hello_timeout();
		});
	}

	void on_hello_timeout()
	{
		if (!has_connection || conn_state != ConnState::AwaitingHello)
			return;

		hello_timeouts++;
		log("Disconnected: Vesktop didn't identify itself in time");
		pending_disconnect_reason = DisconnectReason::HelloTimeout;
		close_connection(websocketpp::close::status::policy_violation, "hello timeout");
		reset_connection_state();
	}

	// ---- 最初のメッセージ (hello) ----

	void handle_hello_phase(websocketpp::frame::opcode::value opcode, const std::string &payload,
				std::int64_t received_us)
	{
		// 切断する前に理由を入れておく (reset_connection_state が last_disconnect_reason に移す)。理由は HelloNgReason と
		// 1 対 1 に対応し、ドックが接続の切れた理由の表示に使う。
		if (opcode != websocketpp::frame::opcode::text) {
			log("Disconnected: the first message wasn't text");
			pending_disconnect_reason = DisconnectReason::MalformedHello;
			close_connection(websocketpp::close::status::policy_violation, "expected hello");
			return;
		}

		const auto json = try_parse_json(payload);
		const auto type = json ? peek_type(*json) : std::nullopt;
		if (!json || !type || *type != control_type::kHello) {
			log("Disconnected: the first message wasn't a hello");
			pending_disconnect_reason = DisconnectReason::MalformedHello;
			close_connection(websocketpp::close::status::policy_violation, "expected hello");
			return;
		}

		const auto hello = parse_hello(*json);
		if (!hello) {
			log("Disconnected: the hello message was malformed");
			pending_disconnect_reason = DisconnectReason::MalformedHello;
			send_text(build_hello_ng(HelloNgMessage{HelloNgReason::Malformed}));
			close_connection(websocketpp::close::status::policy_violation, "malformed hello");
			return;
		}

		if (hello->wire != kWireVersion) {
			log("Disconnected: Vesktop uses a different protocol version");
			pending_disconnect_reason = DisconnectReason::WireMismatch;
			send_text(build_hello_ng(HelloNgMessage{HelloNgReason::BadWire}));
			close_connection(websocketpp::close::status::policy_violation, "bad wire version");
			return;
		}

		if (!constant_time_equals(hello->secret, secret_hex)) {
			hello_secret_mismatches++;
			log("Disconnected: the connection secret didn't match");
			pending_disconnect_reason = DisconnectReason::SecretMismatch;
			send_text(build_hello_ng(HelloNgMessage{HelloNgReason::BadSecret}));
			close_connection(websocketpp::close::status::policy_violation, "bad secret");
			return;
		}

		conn_state = ConnState::Connected;
		client_connected = true;
		hello_ok_at_us.store(wall_now_us());
		{
			// 送信側のバージョンを、画面に表示するために覚える (トークンは覚えない)。
			std::lock_guard<std::mutex> peer_lock(peer_mu);
			peer_version = hello->version;
			// 送らなかったとき (キーがないか、文字列でないとき) は空にし、分からないことを示す。
			peer_vesktop_version = hello->vesktop.value_or(std::string());
		}
		if (owner->on_connected)
			owner->on_connected();
		// 時計の種類は、hello の時計の読みより先に渡す (受信側は、種類で読みの扱いを決めるため)。
		if (owner->on_sender_clock_base)
			owner->on_sender_clock_base(hello->clock_base);
		if (hello->clock_us && owner->on_sender_clock)
			owner->on_sender_clock(*hello->clock_us, received_us);
		if (hello_timer) {
			asio::error_code ec;
			hello_timer->cancel(ec);
			hello_timer.reset();
		}
		HelloOkMessage ok;
		ok.wire = kWireVersion;
		// cfg.server_version の既定値は CORDMUX_VERSION_STRING である (ListenerConfig のコメント)。
		ok.version = cfg.server_version;
		// 最後に渡された media と limits を、初期値として入れる。
		ok.media = latest_media;
		ok.limits = latest_limits;
		send_text(build_hello_ok(ok));
		log("Connected to Vesktop");
		schedule_ping();
	}

	// ---- 接続した後の制御用の JSON ----

	void handle_control_message(const nlohmann::json &j, const std::string &type, std::int64_t received_us)
	{
		if (type == control_type::kPong) {
			if (const auto pong = parse_pong(j)) {
				if (pong->clock_us && owner->on_sender_clock)
					owner->on_sender_clock(*pong->clock_us, received_us);
				unanswered_pings.store(0);
				pongs_received++;
				last_pong_at_us.store(wall_now_us());
			} else {
				dropped_unknown_control++;
			}
			return;
		}
		if (type == control_type::kConfigure) {
			const auto m = parse_configure(j);
			if (m) {
				if (owner->on_configure)
					owner->on_configure(*m);
			} else {
				// 形が壊れているか、同じ stream_id が 2 つ以上ある。configure 全体を捨てて数え、ログに 1 行書く。どの値が
				// 正しいか決められないので何も渡さず、捨てたことはログに残す。ログには stream_id もメッセージの中身も
				// 書かない。
				dropped_unknown_control++;
				log("Ignored a stream list from Vesktop: it was malformed");
			}
			return;
		}
		if (type == control_type::kLog) {
			const auto m = parse_log(j);
			if (m) {
				if (owner->on_log)
					owner->on_log(*m);
			} else {
				dropped_unknown_control++;
			}
			return;
		}
		if (type == control_type::kFacts) {
			const auto m = parse_facts(j);
			if (m) {
				if (owner->on_facts)
					owner->on_facts(*m);
			} else {
				dropped_unknown_control++;
			}
			return;
		}
		if (type == control_type::kProfiles) {
			// facts と違い、ここで要素ごとに形を確認する。1 つでも壊れていれば全体を捨てる (configure と同じ)。
			// 表示名も user_id もログに書かず、ログに人を特定できる値を残さない。
			const auto m = parse_profiles(j);
			if (m) {
				if (owner->on_profiles)
					owner->on_profiles(*m);
			} else {
				dropped_unknown_control++;
				log("Ignored profiles from Vesktop: they were malformed");
			}
			return;
		}
		if (type == control_type::kAvatar) {
			// 形が違うか、画像が上限より大きければ、そのメッセージを捨てて数える。参加者ごとに届くので、ログには
			// 書かない (送信側が壊れていると、ログがこの行で埋まる)。user_id もログに書かない。
			const auto m = parse_avatar(j);
			if (m) {
				if (owner->on_avatar)
					owner->on_avatar(*m);
			} else {
				dropped_unknown_control++;
			}
			return;
		}
		if (type == control_type::kStreamCheckResult) {
			// 形が違えば捨てて数える。届く回数は受信側が頼んだ回数だけなので、ログに書く行は pipeline/ が決める。
			const auto m = parse_stream_check_result(j);
			if (m) {
				if (owner->on_stream_check_result)
					owner->on_stream_check_result(*m);
			} else {
				dropped_unknown_control++;
			}
			return;
		}
		// 2 回目の hello と知らない type は受け付けない (捨てて数え、切断はしない)。
		dropped_unknown_control++;
	}

	void handle_binary_frame(const std::string &payload)
	{
		const auto *bytes = reinterpret_cast<const uint8_t *>(payload.data());
		const auto header = decode_frame_header(bytes, payload.size());
		if (!header) {
			dropped_invalid_frames++;
			return;
		}
		const uint8_t *body = frame_payload(bytes);
		const size_t body_len = frame_payload_size(payload.size());
		if (header->type == FrameType::Video) {
			if (owner->on_video_frame)
				owner->on_video_frame(*header, body, body_len);
		} else {
			if (owner->on_audio_frame)
				owner->on_audio_frame(*header, body, body_len);
		}
	}

	void on_message(websocketpp::connection_hdl /*hdl*/, message_ptr msg)
	{
		const auto opcode = msg->get_opcode();
		const std::string &payload = msg->get_payload();
		// 受け取った時刻は、メッセージを読む前に読む (hello と pong の clock_us と組にして、送信側の時計との差を決めるのに
		// 使うので、解析とコールバックの処理の時間を入れない)。
		const std::int64_t received_us =
			opcode == websocketpp::frame::opcode::text && owner->receive_clock_us ? owner->receive_clock_us() : 0;

		if (conn_state == ConnState::AwaitingHello) {
			handle_hello_phase(opcode, payload, received_us);
			return;
		}

		if (opcode == websocketpp::frame::opcode::binary) {
			handle_binary_frame(payload);
			return;
		}
		if (opcode == websocketpp::frame::opcode::text) {
			const auto json = try_parse_json(payload);
			const auto type = json ? peek_type(*json) : std::nullopt;
			if (!json || !type) {
				dropped_unknown_control++;
				return;
			}
			handle_control_message(*json, *type, received_us);
			return;
		}
		// ほかの opcode (continuation など) はここには来ない (websocketpp は、分割されたメッセージを 1 つに
		// まとめてから message_handler を呼ぶ)。
	}
};

Listener::Listener(ListenerConfig cfg, PlatformOps ops) : impl_(std::make_unique<Impl>(this, std::move(cfg), std::move(ops)))
{
	impl_->server.clear_access_channels(websocketpp::log::alevel::all);
	impl_->server.clear_error_channels(websocketpp::log::elevel::all);
	impl_->server.init_asio();
	// set_reuse_addr(true) にはしない。Windows の SO_REUSEADDR は、使用中のポートにも bind できる意味になるので、
	// 使用中なら次のポートを試す try_open() の処理が働かなくなる。Windows では、TIME_WAIT のポートでも SO_REUSEADDR
	// なしで listen の bind ができるので、テストを続けて実行しても困らない。
	impl_->server.set_max_message_size(impl_->cfg.max_message_bytes);

	impl_->server.set_validate_handler([this](websocketpp::connection_hdl hdl) { return impl_->on_validate(hdl); });
	impl_->server.set_open_handler([this](websocketpp::connection_hdl hdl) { impl_->on_open(hdl); });
	impl_->server.set_close_handler([this](websocketpp::connection_hdl hdl) { impl_->on_close(hdl); });
	impl_->server.set_fail_handler([this](websocketpp::connection_hdl hdl) { impl_->on_fail(hdl); });
	impl_->server.set_message_handler(
		[this](websocketpp::connection_hdl hdl, server_t::message_ptr msg) { impl_->on_message(hdl, msg); });
}

Listener::~Listener()
{
	stop();
}

OpenResult Listener::try_open()
{
	OpenResult result;

	// 結果は ListenerStats::open にもコピーする。ドックが、別の OBS が使っていることや、待ち受けを始められないことを
	// 表示するのに使う。トークンはコピーしない (OpenSummary のコメント)。
	const auto remember = [this](const OpenResult &r) {
		impl_->open_outcome.store(r.outcome);
		impl_->open_problem.store(r.problem);
		impl_->open_port.store(r.port);
		impl_->open_other_writer_pid.store(r.other_writer_pid);
		impl_->open_other_writer_started_at.store(r.other_writer_started_at);
		impl_->open_port_first.store(r.port_first);
		impl_->open_port_last.store(r.port_last);
		impl_->open_attempted.store(true);
	};

	if (impl_->ever_opened) {
		result.outcome = OpenOutcome::AlreadyOpenedBefore;
		// ログには書かない。呼び出し元 (plugin-main.cpp) が outcome を見て LOG_WARNING で 1 行書く (同じことを 2 行
		// 書かない)。
		remember(result);
		return result;
	}

	cordmux::platform::SessionInfo existing;
	if (impl_->ops.read_session_file && impl_->ops.read_session_file(existing)) {
		const bool alive = impl_->ops.is_process_alive && impl_->ops.is_process_alive(existing.writer_pid,
		                                                                              existing.writer_started_at);
		if (alive) {
			const auto me = impl_->ops.current_process_identity ? impl_->ops.current_process_identity()
			                                                     : cordmux::platform::ProcessIdentity{};
			const bool is_myself =
				existing.writer_pid == me.pid && existing.writer_started_at == me.started_at;
			if (!is_myself) {
				result.outcome = OpenOutcome::BlockedByOtherWriter;
				result.other_writer_pid = existing.writer_pid;
				result.other_writer_started_at = existing.writer_started_at;
				// ログには書かない (AlreadyOpenedBefore と同じく、plugin-main.cpp が書く)。
				remember(result);
				return result;
			}
		}
	}

	// 試すポート番号の範囲 (待ち受けを始められないときのメッセージに表示する)。port_attempts が 0 以下なら 1 つも
	// 試さないので、範囲は 0 のままにする。
	if (impl_->cfg.port_attempts > 0) {
		result.port_first = impl_->cfg.base_port;
		result.port_last = uint16_t(impl_->cfg.base_port + impl_->cfg.port_attempts - 1);
	}

	uint16_t chosen_port = 0;
	for (int i = 0; i < impl_->cfg.port_attempts; i++) {
		const uint16_t candidate = uint16_t(impl_->cfg.base_port + i);
		asio::error_code ec;
		impl_->server.listen(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), candidate), ec);
		if (!ec) {
			chosen_port = candidate;
			break;
		}
	}

	if (chosen_port == 0) {
		result.outcome = OpenOutcome::PortsExhausted;
		// ログには書かない (AlreadyOpenedBefore と同じく、plugin-main.cpp が書く)。
		remember(result);
		return result;
	}

	impl_->bound_port = chosen_port;
	impl_->secret_hex = generate_secret_hex();

	asio::error_code accept_ec;
	impl_->server.start_accept(accept_ec);
	if (accept_ec) {
		// ログには書かない。呼び出し元 (plugin-main.cpp) が problem を見て、LOG_WARNING で原因を 1 行書く
		// (ws/listener.h の OpenProblem)。
		// 開いたポートを閉じ、ポート番号とトークンも持たない状態に戻す。待ち受けていないのに port() がポート番号を
		// 返さないようにするためである。listen() は済んでいるので、閉じないとポートを持ったままになり、同じ
		// インスタンスで try_open() をもう一度呼んだときに、自分が開いたままのポートに塞がれる。ever_opened は false の
		// ままにする。
		{
			websocketpp::lib::error_code close_ec;
			impl_->server.stop_listening(close_ec);
		}
		impl_->bound_port = 0;
		impl_->secret_hex.clear();
		result.outcome = OpenOutcome::PortsExhausted;
		result.problem = OpenProblem::AcceptFailed;
		remember(result);
		return result;
	}

	impl_->work_guard.emplace(asio::make_work_guard(impl_->server.get_io_service()));
	impl_->io_thread = std::thread([this] { impl_->server.run(); });
	impl_->listening = true;

	cordmux::platform::SessionInfo info;
	info.port = chosen_port;
	info.secret_hex = impl_->secret_hex;
	if (impl_->ops.current_process_identity) {
		const auto id = impl_->ops.current_process_identity();
		info.writer_pid = id.pid;
		info.writer_started_at = id.started_at;
	}
	// 書けなくても待ち受けは続ける (outcome は Opened)。ログには書かない。呼び出し元が problem を見て、
	// "Waiting for Vesktop to connect" の代わりに LOG_WARNING で 1 行書く (AcceptFailed と同じ)。problem は
	// remember で stats().open.problem にもコピーされ、ドックは待ち受け中とは別の状態として表示する。
	{
		std::lock_guard<std::mutex> lock(impl_->session_mu);
		impl_->written_session = info;
		impl_->other_writer_logged = false;
		if (impl_->ops.write_session_file && !impl_->ops.write_session_file(info))
			result.problem = OpenProblem::SessionFileNotSaved;
	}

	impl_->ever_opened = true;
	result.outcome = OpenOutcome::Opened;
	result.port = chosen_port;
	result.secret_hex = impl_->secret_hex;
	remember(result); // トークンはコピーされない (remember は OpenSummary のフィールドにしか書かない)
	return result;
}

bool Listener::can_retry_open() const
{
	if (impl_->listening || !impl_->open_attempted.load())
		return false;
	const OpenOutcome outcome = impl_->open_outcome.load();
	return outcome == OpenOutcome::BlockedByOtherWriter || outcome == OpenOutcome::PortsExhausted;
}

SessionCheck Listener::check_session_file()
{
	if (!impl_->listening || impl_->client_connected)
		return SessionCheck::NotApplicable;

	std::lock_guard<std::mutex> lock(impl_->session_mu);
	if (!impl_->written_session)
		return SessionCheck::NotApplicable;
	const cordmux::platform::SessionInfo &expected = *impl_->written_session;

	cordmux::platform::SessionInfo found;
	const bool readable = impl_->ops.read_session_file && impl_->ops.read_session_file(found);
	if (readable) {
		const bool same = found.port == expected.port && found.secret_hex == expected.secret_hex &&
				  found.writer_pid == expected.writer_pid &&
				  found.writer_started_at == expected.writer_started_at;
		if (same) {
			// 前に書けなかった状態が、誰かが同じ中身を置いたことで解消している場合も、元に戻す。
			if (impl_->open_problem.load() == OpenProblem::SessionFileNotSaved)
				impl_->open_problem.store(OpenProblem::None);
			return SessionCheck::Unchanged;
		}
		const bool other_alive = impl_->ops.is_process_alive &&
					 impl_->ops.is_process_alive(found.writer_pid, found.writer_started_at);
		const bool is_myself = found.writer_pid == expected.writer_pid &&
				       found.writer_started_at == expected.writer_started_at;
		if (other_alive && !is_myself) {
			if (!impl_->other_writer_logged) {
				impl_->other_writer_logged = true;
				impl_->log("Left the connection details file as is: another running process wrote it");
			}
			return SessionCheck::LeftToOtherWriter;
		}
	}

	if (impl_->ops.write_session_file && impl_->ops.write_session_file(expected)) {
		impl_->open_problem.store(OpenProblem::None);
		return SessionCheck::Rewritten;
	}
	const bool was_failing = impl_->open_problem.exchange(OpenProblem::SessionFileNotSaved) ==
				 OpenProblem::SessionFileNotSaved;
	return was_failing ? SessionCheck::WriteStillFailing : SessionCheck::WriteFailed;
}

bool Listener::remove_session_file_if_mine()
{
	std::lock_guard<std::mutex> lock(impl_->session_mu);
	if (!impl_->ops.read_session_file || !impl_->ops.current_process_identity || !impl_->ops.delete_session_file)
		return false;
	cordmux::platform::SessionInfo found;
	if (!impl_->ops.read_session_file(found))
		return false;
	const auto me = impl_->ops.current_process_identity();
	if (found.writer_pid != me.pid || found.writer_started_at != me.started_at)
		return false;
	return impl_->ops.delete_session_file();
}

void Listener::stop()
{
	if (!impl_->listening.exchange(false))
		return;

	// 待ち受けのソケット (acceptor) を閉じる。閉じないと、この Listener が破棄されるまでポートを持ったままになり、
	// 新しい Listener が同じポートで待ち受けられない。
	{
		websocketpp::lib::error_code ec;
		impl_->server.stop_listening(ec);
	}

	impl_->work_guard.reset();
	impl_->server.get_io_service().stop();
	if (impl_->io_thread.joinable())
		impl_->io_thread.join();

	// こちらから閉じたときも、切れた時刻と理由を残す。io_thread は join したので、ここから先はほかに書き込む
	// スレッドがない。on_disconnected は呼ばない。止めるときの後始末は呼び出し元が行う (ws/listener.h の stop())。
	if (impl_->has_connection) {
		impl_->has_connection = false;
		impl_->client_connected = false;
		impl_->connection_closed_at_us.store(wall_now_us());
		impl_->last_disconnect_reason.store(DisconnectReason::LocalClose);
		impl_->unanswered_pings.store(0);
		std::lock_guard<std::mutex> peer_lock(impl_->peer_mu);
		impl_->peer_version.clear();
		impl_->peer_vesktop_version.clear();
	}
}

bool Listener::is_listening() const
{
	return impl_->listening;
}

uint16_t Listener::port() const
{
	return impl_->bound_port;
}

ListenerStats Listener::stats() const
{
	// フィールドは 1 つずつ読むので、全体が同じ瞬間の値である保証はない (切れた時刻が新しくなった直後に、理由だけ
	// 1 つ前の値を読むことがある)。ドックは 500 ms ごとに読み直すので困らない。ロックを追加して io_thread を
	// 待たせるより、読む処理を軽いままにする。
	ListenerStats s;
	s.listening = impl_->listening;
	s.rejected_clients = impl_->rejected_clients;
	s.dropped_invalid_frames = impl_->dropped_invalid_frames;
	s.dropped_unknown_control = impl_->dropped_unknown_control;
	s.hello_secret_mismatches = impl_->hello_secret_mismatches;
	s.handshake_rejections = impl_->handshake_rejections;
	s.heartbeat_timeouts = impl_->heartbeat_timeouts;
	s.hello_timeouts = impl_->hello_timeouts;
	s.send_failures = impl_->send_failures;
	s.client_connected = impl_->client_connected;

	s.open.attempted = impl_->open_attempted.load();
	s.open.outcome = impl_->open_outcome.load();
	s.open.problem = impl_->open_problem.load();
	s.open.port = impl_->open_port.load();
	s.open.other_writer_pid = impl_->open_other_writer_pid.load();
	s.open.other_writer_started_at = impl_->open_other_writer_started_at.load();
	s.open.port_first = impl_->open_port_first.load();
	s.open.port_last = impl_->open_port_last.load();

	s.connection_opened_at_us = impl_->connection_opened_at_us.load();
	s.hello_ok_at_us = impl_->hello_ok_at_us.load();
	s.connection_closed_at_us = impl_->connection_closed_at_us.load();
	s.last_disconnect_reason = impl_->last_disconnect_reason.load();
	s.connections_accepted = impl_->connections_accepted;

	s.pings_sent = impl_->pings_sent;
	s.pongs_received = impl_->pongs_received;
	s.unanswered_pings = impl_->unanswered_pings.load();
	s.last_pong_at_us = impl_->last_pong_at_us.load();
	{
		std::lock_guard<std::mutex> peer_lock(impl_->peer_mu);
		s.peer_version = impl_->peer_version;
		s.peer_vesktop_version = impl_->peer_vesktop_version;
	}
	return s;
}

void Listener::request_keyframe(uint32_t stream_id)
{
	asio::post(impl_->server.get_io_service(), [this, stream_id] {
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected)
			return;
		KeyframeRequestMessage m;
		m.stream_id = stream_id;
		impl_->send_text(build_keyframe_request(m));
	});
}

void Listener::send_stream_check(uint32_t stream_id)
{
	asio::post(impl_->server.get_io_service(), [this, stream_id] {
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected)
			return;
		StreamCheckMessage m;
		m.stream_id = stream_id;
		impl_->send_text(build_stream_check(m));
	});
}

void Listener::send_recording(bool on)
{
	// request_keyframe と同じ形。接続していなければ捨てる。接続し直した直後に pipeline/ が現在の値を送り直すので、
	// ここで覚えておく必要がない。
	asio::post(impl_->server.get_io_service(), [this, on] {
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected)
			return;
		RecordingMessage m;
		m.on = on;
		impl_->send_text(build_recording(m));
	});
}

void Listener::send_media(bool on)
{
	// request_keyframe と同じ形。値は接続していなくても覚える。次の hello_ok に初期値として入れるためである
	// (ws/listener.h の send_media)。送るのは接続しているときだけで、接続していなければ捨てる (recording と同じ)。
	asio::post(impl_->server.get_io_service(), [this, on] {
		impl_->latest_media = on;
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected)
			return;
		MediaMessage m;
		m.on = on;
		impl_->send_text(build_media(m));
	});
}

void Listener::send_limits(const LimitsMessage &limits)
{
	// send_media と同じ形。範囲の外の値は、覚えも送りもしない (送信側が捨てて数える値を送らない)。呼び出し元の
	// 誤りなので、ログに残す。ログも io_thread の中で書く (Impl に触るのは io_thread だけにする)。
	asio::post(impl_->server.get_io_service(), [this, limits] {
		if (!limits_in_range(limits)) {
			impl_->log("Quality limits not sent to Vesktop: a value is out of range");
			return;
		}
		impl_->latest_limits = limits;
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected)
			return;
		impl_->send_text(build_limits(limits));
	});
}

void Listener::send_main_account(const std::string &user_id)
{
	asio::post(impl_->server.get_io_service(), [this, user_id] {
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected)
			return;
		MainAccountMessage m;
		m.user_id = user_id;
		impl_->send_text(build_main_account(m));
	});
}

void Listener::send_receive_scope(const ReceiveScopeMessage &scope)
{
	asio::post(impl_->server.get_io_service(), [this, scope] {
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected)
			return;
		impl_->send_text(build_receive_scope(scope));
	});
}

void Listener::send_move_to_main_call()
{
	asio::post(impl_->server.get_io_service(), [this] {
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected) {
			impl_->log("Move not sent to Vesktop: not connected");
			return;
		}
		impl_->send_text(build_move_to_main_call());
	});
}

void Listener::send_leave_call()
{
	asio::post(impl_->server.get_io_service(), [this] {
		if (!impl_->has_connection || impl_->conn_state != Impl::ConnState::Connected) {
			impl_->log("Leave not sent to Vesktop: not connected");
			return;
		}
		impl_->send_text(build_leave_call());
	});
}

} // namespace cordmux::ws

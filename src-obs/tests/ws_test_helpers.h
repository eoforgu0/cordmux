// SPDX-License-Identifier: Apache-2.0

// tests/test_ws_listener.cpp が使うヘルパー。待ち合わせ、ログの捕捉、テスト用の PlatformOps、WebSocket のクライアント
// (生の TCP で送る RawWsClient と、websocketpp の TestClient) を持つ。テストだけが使う。

#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <algorithm>
#include <asio.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "ws/listener.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cordmux::tests::ws {

// ---- 待ち合わせ ----

// pred が true になるまで、最大 timeout_ms の間、短い間隔で確認する。true になれば true を返す。
// Listener の io_thread とテストのスレッドは別なので、stats() やフラグをすぐに読むと、まだ変わっていないことがある。
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 2000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (pred())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

// 1 つの TEST_CASE が使うポートの数。テストは base_port から 16 個 (ListenerConfig::port_attempts の既定) を使い、
// 余裕を見て 32 個ずつ割り当てる。
inline constexpr uint16_t kTestPortBlock = 32;

// base から kTestPortBlock 個のポートを、今 127.0.0.1 で待ち受けられるか。1 つずつ bind して listen し、すぐに閉じる
// (接続を受け付けていない待ち受けのポートは、閉じると TIME_WAIT を残さない)。
inline bool test_port_block_free(uint16_t base)
{
	asio::io_context io;
	for (uint16_t i = 0; i < kTestPortBlock; ++i) {
		asio::ip::tcp::acceptor acc(io);
		asio::error_code ec;
		acc.open(asio::ip::tcp::v4(), ec);
		if (!ec)
			acc.bind(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), uint16_t(base + i)), ec);
		if (!ec)
			acc.listen(asio::socket_base::max_listen_connections, ec);
		if (ec)
			return false;
	}
	return true;
}

// TEST_CASE ごとに違う base_port を返す (前のテストのポートが TIME_WAIT で残っていても影響しないように
// する)。製品の既定のポート (4470) は使わない。
// 返す範囲は、今待ち受けられることを確認してから返す。確認しないと、Windows が除外したポートの範囲
// (netsh int ipv4 show excludedportrange protocol=tcp で見える範囲。Hyper-V などが予約し、再起動で変わる) に当たった
// テストが、bind の WSAEACCES で失敗する。待ち受けられない範囲は飛ばして次の範囲を使う。20000 より小さい範囲と、
// 65535 を超える範囲は使わない (数が一周したら、20000 より小さい間は飛ばす)。
inline uint16_t next_test_port_base()
{
	static std::atomic<uint16_t> next{48000};
	for (int tries = 0; tries < 4096; ++tries) {
		const uint16_t base = next.fetch_add(kTestPortBlock);
		if (base < 20000 || base > 65535 - kTestPortBlock)
			continue;
		if (test_port_block_free(base))
			return base;
	}
	return next.fetch_add(kTestPortBlock); // 見つからなければ、確認せずに返す (テストがその場で失敗して分かる)。
}

// ---- ログの捕捉 (ログにトークンとポート番号が含まれないことを確認するテストに使う) ----

class CapturingLog {
public:
	cordmux::ws::LogSink sink()
	{
		return [this](std::string_view line) {
			std::lock_guard<std::mutex> lk(mutex_);
			text_ += std::string(line);
			text_ += '\n';
		};
	}

	std::string text() const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		return text_;
	}

private:
	mutable std::mutex mutex_;
	std::string text_;
};

// ---- テスト用の PlatformOps (実際の %LOCALAPPDATA% には書かない) ----

inline cordmux::ws::PlatformOps make_fake_platform_ops()
{
	cordmux::ws::PlatformOps ops;
	ops.current_process_identity = [] { return cordmux::platform::ProcessIdentity{4242, 1000}; };
	ops.write_session_file = [](const cordmux::platform::SessionInfo &) { return true; };
	ops.read_session_file = [](cordmux::platform::SessionInfo &) { return false; }; // session.json はない
	ops.is_process_alive = [](uint32_t, int64_t) { return false; };
	return ops;
}

// session.json を書いた別の OBS が動いている状態にする PlatformOps (BlockedByOtherWriter のテスト用)。
inline cordmux::ws::PlatformOps make_fake_platform_ops_blocked(uint32_t other_pid, int64_t other_started_at)
{
	cordmux::ws::PlatformOps ops;
	ops.current_process_identity = [] { return cordmux::platform::ProcessIdentity{4242, 1000}; };
	ops.write_session_file = [](const cordmux::platform::SessionInfo &) { return true; };
	ops.read_session_file = [other_pid, other_started_at](cordmux::platform::SessionInfo &out) {
		out.port = 9999;
		out.secret_hex = "not-used";
		out.writer_pid = other_pid;
		out.writer_started_at = other_started_at;
		return true;
	};
	ops.is_process_alive = [](uint32_t, int64_t) { return true; }; // 動いている
	return ops;
}

// メモリの上の session.json。中身の有無、書き込みの失敗、動いているプロセスを、テストが変えられる。ops() が返す
// PlatformOps はこのオブジェクトを参照するので、PlatformOps を使う Listener より長く生かすこと。
struct FakeSessionFile {
	cordmux::platform::ProcessIdentity me{4242, 1000};
	std::optional<cordmux::platform::SessionInfo> content;
	bool write_fails = false;
	int writes = 0;
	std::vector<std::pair<uint32_t, int64_t>> alive; // 動いているプロセス (自分は含めなくてよい)

	cordmux::ws::PlatformOps ops()
	{
		cordmux::ws::PlatformOps o;
		o.current_process_identity = [this] { return me; };
		o.write_session_file = [this](const cordmux::platform::SessionInfo &info) {
			if (write_fails)
				return false;
			content = info;
			++writes;
			return true;
		};
		o.read_session_file = [this](cordmux::platform::SessionInfo &out) {
			if (!content)
				return false;
			out = *content;
			return true;
		};
		o.is_process_alive = [this](uint32_t pid, int64_t started_at) {
			return std::find(alive.begin(), alive.end(), std::make_pair(pid, started_at)) != alive.end();
		};
		o.delete_session_file = [this] {
			content.reset();
			return true;
		};
		return o;
	}
};

// ---- 生の TCP で送るクライアント (ハンドシェイク、opcode、長さ、分割が不正なメッセージのテスト用) ----

struct HandshakeResult {
	bool is_101 = false;
	int status_code = 0;
	std::string raw_response;
};

class RawWsClient {
public:
	RawWsClient() : socket_(io_) {}

	bool connect_to(uint16_t port)
	{
		asio::error_code ec;
		socket_.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
		return !ec;
	}

	// host_header と extra_headers を指定でき、不正なハンドシェイクのテストに使う。
	HandshakeResult handshake(const std::string &host_header, const std::string &extra_headers = "")
	{
		std::ostringstream req;
		req << "GET / HTTP/1.1\r\n"
		    << "Host: " << host_header << "\r\n"
		    << "Upgrade: websocket\r\n"
		    << "Connection: Upgrade\r\n"
		    << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		    << "Sec-WebSocket-Version: 13\r\n"
		    << extra_headers << "\r\n";
		return send_raw_handshake(req.str());
	}

	HandshakeResult send_raw_handshake(const std::string &raw_request)
	{
		HandshakeResult r;
		asio::error_code ec;
		asio::write(socket_, asio::buffer(raw_request), ec);
		if (ec)
			return r;

		std::string response;
		char buf[4096];
		for (;;) {
			const size_t n = socket_.read_some(asio::buffer(buf), ec);
			if (ec || n == 0)
				break;
			response.append(buf, n);
			if (response.find("\r\n\r\n") != std::string::npos)
				break;
			if (response.size() > 8192)
				break;
		}
		r.raw_response = response;
		if (response.size() > 12 && response.rfind("HTTP/1.1 ", 0) == 0) {
			r.status_code = std::atoi(response.substr(9, 3).c_str());
			r.is_101 = (r.status_code == 101);
		}
		return r;
	}

	// RFC 6455 のとおりにマスクした、1 つの WebSocket のフレームを送る。
	void send_frame(uint8_t opcode, const std::string &payload, bool fin = true)
	{
		send_raw(build_frame(opcode, payload, fin));
	}

	void send_raw(const std::vector<uint8_t> &bytes)
	{
		asio::error_code ec;
		asio::write(socket_, asio::buffer(bytes), ec);
	}

	static std::vector<uint8_t> build_frame(uint8_t opcode, const std::string &payload, bool fin = true)
	{
		std::vector<uint8_t> frame;
		frame.push_back(uint8_t((fin ? 0x80 : 0x00) | (opcode & 0x0F)));
		const uint8_t mask_bit = 0x80;
		if (payload.size() <= 125) {
			frame.push_back(uint8_t(mask_bit | payload.size()));
		} else if (payload.size() <= 0xFFFFu) {
			frame.push_back(uint8_t(mask_bit | 126));
			frame.push_back(uint8_t((payload.size() >> 8) & 0xFF));
			frame.push_back(uint8_t(payload.size() & 0xFF));
		} else {
			frame.push_back(uint8_t(mask_bit | 127));
			for (int i = 7; i >= 0; i--)
				frame.push_back(uint8_t((uint64_t(payload.size()) >> (8 * i)) & 0xFF));
		}
		const uint8_t mask_key[4] = {0x11, 0x22, 0x33, 0x44};
		frame.insert(frame.end(), mask_key, mask_key + 4);
		const size_t header_len = frame.size();
		frame.resize(header_len + payload.size());
		for (size_t i = 0; i < payload.size(); i++)
			frame[header_len + i] = uint8_t(uint8_t(payload[i]) ^ mask_key[i % 4]);
		return frame;
	}

	// 1 回の読み込みを、最大 timeout_ms の間待つ (asio の非同期の読み込みと run_for でタイムアウトにする。sleep を
	// 繰り返して待たない)。閉じられたか、何も届かなければ 0 を返す。
	size_t try_read(std::vector<uint8_t> &out, int timeout_ms)
	{
		out.assign(4096, 0);
		size_t got = 0;
		bool handled = false;
		asio::error_code got_ec;
		socket_.async_read_some(asio::buffer(out), [&](const asio::error_code &ec, size_t n) {
			handled = true;
			got_ec = ec;
			got = n;
		});
		io_.restart();
		io_.run_for(std::chrono::milliseconds(timeout_ms));
		if (!handled) {
			asio::error_code cancel_ec;
			socket_.cancel(cancel_ec);
			io_.restart();
			io_.run_for(std::chrono::milliseconds(50)); // キャンセルの完了ハンドラーを実行させる
		}
		if (!handled || got_ec || got == 0) {
			out.clear();
			return 0;
		}
		out.resize(got);
		return got;
	}

	bool is_open() const { return socket_.is_open(); }

private:
	asio::io_context io_;
	asio::ip::tcp::socket socket_;
};

// ---- websocketpp のクライアント (正しいハンドシェイクをするクライアントが必要なテスト用) ----

class TestClient {
public:
	using client_t = websocketpp::client<websocketpp::config::asio_client>;

	TestClient()
	{
		client_.clear_access_channels(websocketpp::log::alevel::all);
		client_.clear_error_channels(websocketpp::log::elevel::all);
		client_.init_asio();
		// asio の io_context::run() は、待っている処理が 1 つもないとすぐに戻る。この後、テストのスレッドの connect() が
		// 非同期の処理を積むが、それより先にこのスレッドの run() が戻ると、積んだ処理を誰も実行せず、接続できたり
		// できなかったりする。work_guard を持って、デストラクターで stop() するまで run() が戻らないようにする。
		work_guard_.emplace(asio::make_work_guard(client_.get_io_service()));

		client_.set_open_handler([this](websocketpp::connection_hdl) {
			std::lock_guard<std::mutex> lk(mutex_);
			opened_ = true;
			cv_.notify_all();
		});
		client_.set_close_handler([this](websocketpp::connection_hdl) {
			std::lock_guard<std::mutex> lk(mutex_);
			closed_ = true;
			cv_.notify_all();
		});
		client_.set_fail_handler([this](websocketpp::connection_hdl) {
			std::lock_guard<std::mutex> lk(mutex_);
			failed_ = true;
			closed_ = true;
			cv_.notify_all();
		});
		client_.set_message_handler([this](websocketpp::connection_hdl, client_t::message_ptr msg) {
			std::lock_guard<std::mutex> lk(mutex_);
			received_.push_back(msg->get_payload());
			cv_.notify_all();
		});

		io_thread_ = std::thread([this] { client_.run(); });
	}

	~TestClient()
	{
		work_guard_.reset();
		client_.get_io_service().stop();
		if (io_thread_.joinable())
			io_thread_.join();
	}

	TestClient(const TestClient &) = delete;
	TestClient &operator=(const TestClient &) = delete;

	// origin が空なら Origin ヘッダーを付けない (Listener は Origin ヘッダーのない接続を許す)。
	bool connect(uint16_t port, const std::string &origin = "")
	{
		websocketpp::lib::error_code ec;
		auto con = client_.get_connection("ws://127.0.0.1:" + std::to_string(port), ec);
		if (ec)
			return false;
		if (!origin.empty())
			con->append_header("Origin", origin);
		hdl_ = con->get_handle();
		client_.connect(con);
		return true;
	}

	void send(const std::string &text)
	{
		websocketpp::lib::error_code ec;
		client_.send(hdl_, text, websocketpp::frame::opcode::text, ec);
	}

	void send_binary(const uint8_t *data, size_t len)
	{
		websocketpp::lib::error_code ec;
		client_.send(hdl_, data, len, websocketpp::frame::opcode::binary, ec);
	}

	bool wait_opened(int timeout_ms = 2000)
	{
		std::unique_lock<std::mutex> lk(mutex_);
		return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return opened_ || failed_; }) &&
		       opened_;
	}

	bool wait_closed(int timeout_ms = 2000)
	{
		std::unique_lock<std::mutex> lk(mutex_);
		return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return closed_; });
	}

	bool wait_message(int timeout_ms = 2000)
	{
		std::unique_lock<std::mutex> lk(mutex_);
		const size_t before = received_.size();
		return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
		                     [this, before] { return received_.size() > before; });
	}

	std::vector<std::string> received() const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		return received_;
	}

	bool failed() const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		return failed_;
	}

private:
	client_t client_;
	std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
	std::thread io_thread_;
	websocketpp::connection_hdl hdl_;

	mutable std::mutex mutex_;
	std::condition_variable cv_;
	bool opened_ = false;
	bool closed_ = false;
	bool failed_ = false;
	std::vector<std::string> received_;
};

} // namespace cordmux::tests::ws

// SPDX-License-Identifier: Apache-2.0

// ws/listener.h の単体テスト。127.0.0.1 の空いているポートで実際に待ち受け、生の TCP のクライアント (RawWsClient)
// と websocketpp のクライアント (TestClient) から、不正なハンドシェイクや不正なメッセージも含めて送る。ログに
// トークンとポート番号が含まれないことも確認する。ヘルパーは tests/ws_test_helpers.h にある。

#include "doctest.h"

#include "ws/listener.h"
#include "ws/log_sink.h"

#include "ws_test_helpers.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace cordmux::ws;
using namespace cordmux::tests::ws;

namespace {

// テスト用の ListenerConfig。ping の間隔を短くし、メッセージの長さの上限を小さくして、上限を超えるメッセージを
// 小さいデータで作れるようにする。
ListenerConfig fast_test_config(uint16_t base_port, LogSink log = null_log_sink)
{
	ListenerConfig cfg;
	cfg.base_port = base_port;
	cfg.port_attempts = 16;
	cfg.ping_interval_ms = 30;
	cfg.max_missed_pongs = 2;
	cfg.max_message_bytes = 4096;
	cfg.log = std::move(log);
	return cfg;
}

std::string hello_json(const std::string &secret, int wire = kWireVersion,
		       const std::optional<std::string> &vesktop = std::nullopt,
		       std::optional<std::int64_t> clock_us = std::nullopt,
		       const std::optional<std::string> &clock_base = std::nullopt)
{
	nlohmann::json j;
	j["type"] = control_type::kHello;
	j["wire"] = wire;
	j["version"] = "test-client";
	j["secret"] = secret;
	// 任意のキー。渡されたときだけ書く (送信側の buildHello と同じ形)。
	if (vesktop.has_value())
		j["vesktop"] = *vesktop;
	if (clock_us.has_value())
		j["clock_us"] = *clock_us;
	if (clock_base.has_value())
		j["clock_base"] = *clock_base;
	return j.dump();
}

} // namespace

TEST_CASE("try_open は 127.0.0.1 でポートを 1 つ開き、64 文字のトークンを作る")
{
	Listener listener(fast_test_config(next_test_port_base()), make_fake_platform_ops());
	const auto result = listener.try_open();
	CHECK(result.outcome == OpenOutcome::Opened);
	CHECK(result.port >= 48000);
	CHECK(result.secret_hex.size() == 64);
	CHECK(listener.is_listening());
	CHECK(listener.port() == result.port);
	listener.stop();
}

TEST_CASE("try_open は、base_port が使用中なら次のポートで待ち受ける")
{
	const uint16_t base = next_test_port_base();

	// base を先に acceptor で使用中にしておく。Listener は set_reuse_addr(true) にしないので、Windows でもこのポートは
	// 開けない (ws/listener.cpp のコメント)。
	asio::io_context io;
	asio::ip::tcp::acceptor blocker(io);
	blocker.open(asio::ip::tcp::v4());
	blocker.bind(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), base));
	blocker.listen();

	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto result = listener.try_open();
	REQUIRE(result.outcome == OpenOutcome::Opened);
	CHECK(result.port == base + 1); // base は使用中なので、次のポートで待ち受ける

	listener.stop();
}

TEST_CASE("session.json を書いた別のプロセスが動いていれば、try_open はポートを開かずに BlockedByOtherWriter を返す")
{
	Listener listener(fast_test_config(next_test_port_base()), make_fake_platform_ops_blocked(999, 12345));
	const auto result = listener.try_open();
	CHECK(result.outcome == OpenOutcome::BlockedByOtherWriter);
	CHECK(result.other_writer_pid == 999);
	CHECK(result.other_writer_started_at == 12345);
	CHECK_FALSE(listener.is_listening());
}

TEST_CASE("トークンが一致する hello には hello_ok を返し、接続した状態になる")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());

	const auto msgs = client.received();
	REQUIRE(msgs.size() == 1);
	const auto j = try_parse_json(msgs[0]);
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kHelloOk));

	CHECK(wait_until([&] { return listener.stats().client_connected; }));
	listener.stop();
}

TEST_CASE("on_connected は hello が通ったときに 1 回、on_disconnected はその接続が切れたときに 1 回だけ呼ばれる")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	std::atomic<int> connected_count{0};
	std::atomic<int> disconnected_count{0};
	listener.on_connected = [&] { connected_count++; };
	listener.on_disconnected = [&] { disconnected_count++; };
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	CHECK(wait_until([&] { return listener.stats().client_connected; }));
	CHECK(wait_until([&] { return connected_count.load() == 1; }));
	CHECK(disconnected_count.load() == 0);

	// pong を返さずに、ハートビートで切断させる。hello が通った後の切断なので、on_disconnected が呼ばれる。
	CHECK(wait_until([&] { return listener.stats().heartbeat_timeouts == 1; }));
	// 接続中の印は切断が済んでから消える (数は切断を始める前に増える。listener.cpp の reset_connection_state)。
	CHECK(wait_until([&] { return !listener.stats().client_connected; }));
	CHECK(wait_until([&] { return disconnected_count.load() == 1; }));
	CHECK(connected_count.load() == 1); // 2 回目は呼ばれていない

	listener.stop();
}

TEST_CASE("hello が通らないまま切れた接続では、on_connected も on_disconnected も呼ばれない")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	std::atomic<int> connected_count{0};
	std::atomic<int> disconnected_count{0};
	listener.on_connected = [&] { connected_count++; };
	listener.on_disconnected = [&] { disconnected_count++; };
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json("0000000000000000000000000000000000000000000000000000000000dead"));
	REQUIRE(client.wait_message()); // hello_ng
	CHECK(client.wait_closed());
	CHECK(wait_until([&] { return listener.stats().hello_secret_mismatches == 1; }));

	CHECK(connected_count.load() == 0);
	CHECK(disconnected_count.load() == 0); // hello が通っていない接続の切断では呼ばれない

	listener.stop();
}

TEST_CASE("トークンが一致しない hello には hello_ng (bad_secret) を返して切断し、回数を数える")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json("0000000000000000000000000000000000000000000000000000000000dead"));
	REQUIRE(client.wait_message());

	const auto j = try_parse_json(client.received().front());
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kHelloNg));
	CHECK(j->at("reason").get<std::string>() == "bad_secret");

	CHECK(client.wait_closed());
	CHECK(wait_until([&] { return listener.stats().hello_secret_mismatches == 1; }));
	CHECK_FALSE(listener.stats().client_connected);
	listener.stop();
}

TEST_CASE("secret がない hello には hello_ng (malformed) を返して切断する")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());

	nlohmann::json j;
	j["type"] = control_type::kHello;
	j["wire"] = kWireVersion;
	// secret フィールドがない。
	client.send(j.dump());

	REQUIRE(client.wait_message());
	const auto reply = try_parse_json(client.received().front());
	REQUIRE(reply.has_value());
	CHECK(peek_type(*reply) == std::string(control_type::kHelloNg));
	CHECK(reply->at("reason").get<std::string>() == "malformed");
	CHECK(client.wait_closed());
	listener.stop();
}

TEST_CASE("最初のメッセージが hello でなければ切断する")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(build_ping({1})); // hello ではない

	CHECK(client.wait_closed());
	CHECK_FALSE(listener.stats().client_connected);
	listener.stop();
}

TEST_CASE("2 つ目の接続は拒否し、1 つ目の接続はそのまま続く")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient first;
	REQUIRE(first.connect(open.port));
	REQUIRE(first.wait_opened());
	first.send(hello_json(open.secret_hex));
	REQUIRE(first.wait_message());
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	TestClient second;
	REQUIRE(second.connect(open.port));
	CHECK(second.wait_closed()); // validate で拒否されるので、open にならずに fail か close で終わる

	CHECK(wait_until([&] { return listener.stats().rejected_clients == 1; }));

	// 1 つ目の接続が続いていることを、2 つ目を拒否した後も ping が 1 つ目に届くことで確認する。2 つ目を拒否した
	// ときに has_connection か active_hdl を消してしまうと、この ping は届かない。
	CHECK(first.wait_message()); // 2 つ目のメッセージは最初の ping (ping_interval_ms=30 なので数十 ms で届く)
	CHECK(listener.stats().client_connected);

	listener.stop();
}

TEST_CASE("pong を返さないクライアントは、pong が 2 回続けて返らなかった時点で切断される")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message()); // hello_ok
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	// pong を返さない。ping_interval_ms=30、max_missed_pongs=2 なので、200 ms あれば切断される。
	CHECK(client.wait_closed(2000));
	CHECK(wait_until([&] { return listener.stats().heartbeat_timeouts == 1; }));
	// 接続中の印は切断が済んでから消える (数は切断を始める前に増える。listener.cpp の reset_connection_state)。
	CHECK(wait_until([&] { return !listener.stats().client_connected; }));
	listener.stop();
}

TEST_CASE("ping に pong を返し続けるクライアントは切断されない")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message()); // hello_ok

	// 300 ms の間 (30 ms の ping_interval_ms の 10 回分)、届いた ping にすべて pong を返す。
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
	size_t seen = 1; // hello_ok の 1 つは、既に received に入っている
	while (std::chrono::steady_clock::now() < deadline) {
		if (client.wait_message(50)) {
			const auto msgs = client.received();
			for (; seen < msgs.size(); seen++) {
				const auto j = try_parse_json(msgs[seen]);
				if (j && peek_type(*j) == std::string(control_type::kPing)) {
					const auto ping = j->at("seq").get<uint64_t>();
					client.send(build_pong({ping}));
				}
			}
		}
	}

	CHECK(listener.stats().heartbeat_timeouts == 0);
	CHECK(listener.stats().client_connected);
	listener.stop();
}

TEST_CASE("hello を送らない接続は hello_timeout_ms で切断され、次のクライアントが接続できる")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.hello_timeout_ms = 100; // ping_interval_ms と同じく、テストでは短くする
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient silent;
	REQUIRE(silent.connect(open.port));
	REQUIRE(silent.wait_opened());
	// hello を送らない。

	CHECK(silent.wait_closed(2000));
	CHECK(wait_until([&] { return listener.stats().hello_timeouts == 1; }));

	// 前の接続が切断されたので、次の正しいクライアントは接続できる。
	TestClient good;
	REQUIRE(good.connect(open.port));
	REQUIRE(good.wait_opened());
	good.send(hello_json(open.secret_hex));
	REQUIRE(good.wait_message());
	CHECK(wait_until([&] { return listener.stats().client_connected; }));

	listener.stop();
}

TEST_CASE("hello_timeout_ms が過ぎる前に hello を送れば、切断されない")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.hello_timeout_ms = 200;
	// ハートビートはこのテストで確認しないので、ping_interval_ms を、下で待つ時間より十分に長い既定値に戻す。
	// fast_test_config の 30 ms のままだと、このクライアントは pong を返さないのでハートビートで切断され、
	// hello_timeout_ms を確認できない。
	cfg.ping_interval_ms = 5000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message()); // hello_ok

	// hello_timeout_ms の 2 倍以上待っても、hello が通っていれば切断されない。
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	CHECK(listener.stats().hello_timeouts == 0);
	CHECK(listener.stats().client_connected);

	listener.stop();
}

TEST_CASE("stop() の後に同じインスタンスで try_open() を呼ぶと AlreadyOpenedBefore を返し、別のインスタンスなら同じポートを開ける")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	listener.stop();

	const auto reopen = listener.try_open();
	CHECK(reopen.outcome == OpenOutcome::AlreadyOpenedBefore);
	CHECK(reopen.port == 0);
	CHECK_FALSE(listener.is_listening());

	// stop() が acceptor を閉じるので、同じポートを別のインスタンスで開けることも確認する。
	Listener fresh(fast_test_config(base), make_fake_platform_ops());
	const auto reopened = fresh.try_open();
	CHECK(reopened.outcome == OpenOutcome::Opened);
	CHECK(reopened.port == base);
	fresh.stop();
}

TEST_CASE("一覧にない Origin の接続は 403 で拒否し、ログにトークンとポート番号を書かない")
{
	const uint16_t base = next_test_port_base();
	CapturingLog log;
	Listener listener(fast_test_config(base, log.sink()), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient bad;
	REQUIRE(bad.connect(open.port, "https://evil.example.com"));
	CHECK(bad.wait_closed());
	CHECK(wait_until([&] { return listener.stats().handshake_rejections == 1; }));
	CHECK_FALSE(text_reveals_secret_or_port(log.text(), open.secret_hex, open.port));

	// 一覧にある Origin なら接続できる (拒否した理由が Origin であることを確認する)。
	TestClient good;
	REQUIRE(good.connect(open.port, "https://discord.com"));
	CHECK(good.wait_opened());

	listener.stop();
}

TEST_CASE("Host が違う接続は 403 で拒否し、ログにトークンとポート番号を書かない")
{
	const uint16_t base = next_test_port_base();
	CapturingLog log;
	Listener listener(fast_test_config(base, log.sink()), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	RawWsClient raw;
	REQUIRE(raw.connect_to(open.port));
	const auto res = raw.handshake("evil.example.com:1234");
	CHECK_FALSE(res.is_101);
	CHECK(res.status_code == 403);

	CHECK(wait_until([&] { return listener.stats().handshake_rejections == 1; }));
	CHECK_FALSE(text_reveals_secret_or_port(log.text(), open.secret_hex, open.port));
	listener.stop();
}

TEST_CASE("Upgrade ヘッダーがないハンドシェイクは WebSocket の接続にならず、その後も正しいクライアントは接続できる")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	RawWsClient raw;
	REQUIRE(raw.connect_to(open.port));
	std::ostringstream req;
	req << "GET / HTTP/1.1\r\nHost: 127.0.0.1:" << open.port << "\r\n\r\n"; // Upgrade がない
	const auto res = raw.send_raw_handshake(req.str());
	CHECK_FALSE(res.is_101);

	// 不正なハンドシェイクの後でも、正しいクライアントは接続できる (サーバーが止まっていない)。
	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	listener.stop();
}

TEST_CASE("予約されている opcode を送った接続には close を返し、サーバーは動き続ける")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	RawWsClient raw;
	REQUIRE(raw.connect_to(open.port));
	const auto hs = raw.handshake("127.0.0.1:" + std::to_string(open.port));
	REQUIRE(hs.is_101);

	raw.send_frame(0x3, "x"); // 0x3 は予約されている (制御用でない) opcode

	std::vector<uint8_t> buf;
	// websocketpp は、プロトコルエラーとして close のフレーム (opcode 0x8) を返してから切断する。
	const size_t n = raw.try_read(buf, 1000);
	REQUIRE(n >= 2);
	CHECK((buf[0] & 0x0F) == 0x8); // 先頭のバイトの下位 4 ビット (opcode) が close

	// サーバーが動いていることを、別の正しいクライアントで確認する。
	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	listener.stop();
}

TEST_CASE("最初のメッセージとして長さ 0 のテキストを送られても、サーバーは動き続ける")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	RawWsClient raw;
	REQUIRE(raw.connect_to(open.port));
	const auto hs = raw.handshake("127.0.0.1:" + std::to_string(open.port));
	REQUIRE(hs.is_101);

	raw.send_frame(0x1, ""); // opcode は text、長さは 0

	std::vector<uint8_t> buf;
	raw.try_read(buf, 500); // close が届いても届かなくても、次でサーバーが動いていることを確認する

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	listener.stop();
}

TEST_CASE("2 つのフレームに分割して送った hello も、1 つのメッセージとして受け付ける")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	RawWsClient raw;
	REQUIRE(raw.connect_to(open.port));
	const auto hs = raw.handshake("127.0.0.1:" + std::to_string(open.port));
	REQUIRE(hs.is_101);

	const std::string text = hello_json(open.secret_hex);
	REQUIRE(text.size() > 4);
	const std::string part1 = text.substr(0, text.size() / 2);
	const std::string part2 = text.substr(text.size() / 2);

	raw.send_frame(0x1, part1, /*fin=*/false); // text, FIN=0
	raw.send_frame(0x0, part2, /*fin=*/true);  // continuation, FIN=1

	std::vector<uint8_t> buf;
	REQUIRE(raw.try_read(buf, 1000) > 0);
	const std::string reply(buf.begin(), buf.end());
	// サーバーからクライアントへのフレームはマスクしない (RFC 6455)。先頭のバイトは FIN (1) と opcode (text = 1) で
	// 0x81 になる。
	REQUIRE(buf.size() > 2);
	CHECK(buf[0] == 0x81);
	CHECK(reply.find(control_type::kHelloOk) != std::string::npos);

	CHECK(wait_until([&] { return listener.stats().client_connected; }));
	listener.stop();
}

TEST_CASE("max_message_bytes を超えるメッセージを送られても、サーバーは動き続ける")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops()); // fast_test_config の max_message_bytes は 4096
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	RawWsClient raw;
	REQUIRE(raw.connect_to(open.port));
	const auto hs = raw.handshake("127.0.0.1:" + std::to_string(open.port));
	REQUIRE(hs.is_101);

	const std::string huge(8192, 'a'); // 4096 を超える
	raw.send_frame(0x1, huge);

	std::vector<uint8_t> buf;
	raw.try_read(buf, 500);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	listener.stop();
}

TEST_CASE("正しい映像のメッセージは on_video_frame に渡し、不正なバイナリメッセージは捨てて数える")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());

	std::vector<FrameHeader> got_video;
	std::mutex got_mutex;
	listener.on_video_frame = [&](const FrameHeader &h, const uint8_t *, size_t) {
		std::lock_guard<std::mutex> lk(got_mutex);
		got_video.push_back(h);
	};

	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());

	FrameHeader h;
	h.type = FrameType::Video;
	h.stream_id = 3;
	h.timestamp_us = 555;
	h.flags = kFlagKeyframe;
	const uint8_t payload[] = {9, 9, 9};
	const auto good = encode_frame(h, payload, sizeof(payload));
	client.send_binary(good.data(), good.size());

	CHECK(wait_until([&] {
		std::lock_guard<std::mutex> lk(got_mutex);
		return got_video.size() == 1;
	}));

	// type が不正 (0) のメッセージを送る。捨てられるので、on_video_frame は呼ばれない。
	std::vector<uint8_t> bad = good;
	bad[0] = 0;
	client.send_binary(bad.data(), bad.size());

	CHECK(wait_until([&] { return listener.stats().dropped_invalid_frames == 1; }));
	{
		std::lock_guard<std::mutex> lk(got_mutex);
		CHECK(got_video.size() == 1);
	}
	listener.stop();
}

TEST_CASE("不正な接続を混ぜた一連の接続のログ全体に、トークンもポート番号も含まれない")
{
	const uint16_t base = next_test_port_base();
	CapturingLog log;
	Listener listener(fast_test_config(base, log.sink()), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	// 正しいクライアント、トークンが違うクライアント、拒否される 2 つ目のクライアント、不正なハンドシェイクを混ぜる。
	{
		TestClient bad;
		REQUIRE(bad.connect(open.port));
		REQUIRE(bad.wait_opened());
		bad.send(hello_json("0000000000000000000000000000000000000000000000000000000000dead"));
		bad.wait_message();
	}
	{
		RawWsClient raw;
		REQUIRE(raw.connect_to(open.port));
		raw.handshake("wrong-host:1");
	}

	TestClient good;
	REQUIRE(good.connect(open.port));
	REQUIRE(good.wait_opened());
	good.send(hello_json(open.secret_hex));
	REQUIRE(good.wait_message());
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	TestClient second;
	REQUIRE(second.connect(open.port));
	second.wait_closed();

	CHECK(wait_until([&] { return listener.stats().rejected_clients >= 1; }));

	const std::string all_text = log.text();
	CHECK_FALSE(all_text.empty()); // ログが空なら確認したことにならないので、空でないことも確認する
	CHECK_FALSE(text_reveals_secret_or_port(all_text, open.secret_hex, open.port));

	listener.stop();
}

// ---------------------------------------------------------------------------
// ログにトークンとポート番号が含まれないことを、切断や失敗の種類ごとに 1 つの TEST_CASE で確認する。
// ---------------------------------------------------------------------------

TEST_CASE("ワイヤー形式のバージョンが違う hello で切断したときも、ログにトークンとポート番号が含まれない")
{
	const uint16_t base = next_test_port_base();
	CapturingLog log;
	Listener listener(fast_test_config(base, log.sink()), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex, kWireVersion + 1)); // バージョンだけを変える
	REQUIRE(client.wait_message());

	const auto j = try_parse_json(client.received().front());
	REQUIRE(j.has_value());
	CHECK(peek_type(*j) == std::string(control_type::kHelloNg));
	CHECK(j->at("reason").get<std::string>() == "bad_wire");
	CHECK(client.wait_closed());

	CHECK_FALSE(text_reveals_secret_or_port(log.text(), open.secret_hex, open.port));
	listener.stop();
}

TEST_CASE("ハートビートで切断したときも、ログにトークンとポート番号が含まれない")
{
	const uint16_t base = next_test_port_base();
	CapturingLog log;
	Listener listener(fast_test_config(base, log.sink()), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message()); // hello_ok
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	// pong を返さない。fast_test_config は ping_interval_ms=30、max_missed_pongs=2 なので、数百 ms で切断される。
	CHECK(client.wait_closed(2000));
	CHECK(wait_until([&] { return listener.stats().heartbeat_timeouts == 1; }));
	CHECK_FALSE(text_reveals_secret_or_port(log.text(), open.secret_hex, open.port));

	listener.stop();
}

TEST_CASE("ポートがすべて使用中のときも、試したポートの番号をログに書かない")
{
	const uint16_t base = next_test_port_base();

	// fast_test_config の port_attempts (16) 個のポートを、すべて先に acceptor で使用中にする。Listener は
	// set_reuse_addr(true) にしないので、Windows でもこれらのポートは開けない。
	asio::io_context blocker_io;
	std::vector<std::unique_ptr<asio::ip::tcp::acceptor>> blockers;
	for (int i = 0; i < 16; i++) {
		auto acc = std::make_unique<asio::ip::tcp::acceptor>(blocker_io);
		acc->open(asio::ip::tcp::v4());
		acc->bind(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), uint16_t(base + i)));
		acc->listen();
		blockers.push_back(std::move(acc));
	}

	CapturingLog log;
	Listener listener(fast_test_config(base, log.sink()), make_fake_platform_ops());
	const auto result = listener.try_open();
	REQUIRE(result.outcome == OpenOutcome::PortsExhausted);
	CHECK_FALSE(listener.is_listening());

	// PortsExhausted のときは port が 0 で、secret_hex も空 (トークンを作っていない) なので、その値で
	// text_reveals_secret_or_port を呼んでも何も調べない。ログに書いてはいけないのは試したポートの番号なので、
	// base から base+15 を 1 つずつ確認する。
	const std::string text = log.text();
	for (int i = 0; i < 16; i++)
		CHECK_FALSE(text_reveals_secret_or_port(text, "", uint16_t(base + i)));
}

TEST_CASE("session.json を書けなかったときも、ログにトークンとポート番号が含まれない")
{
	const uint16_t base = next_test_port_base();
	CapturingLog log;

	// write_session_file だけが false を返す PlatformOps (このテストだけで使うので、tests/ws_test_helpers.h には置かない)。
	PlatformOps ops = make_fake_platform_ops();
	ops.write_session_file = [](const cordmux::platform::SessionInfo &) { return false; };

	Listener listener(fast_test_config(base, log.sink()), ops);
	const auto open = listener.try_open();
	// 書けなくても待ち受けは続ける (outcome は Opened のまま)。失敗は problem で呼び出し元に伝え、ws/listener.cpp は
	// ログに書かない (呼び出し元が LOG_WARNING で 1 行書く)。
	REQUIRE(open.outcome == OpenOutcome::Opened);
	CHECK(open.problem == OpenProblem::SessionFileNotSaved);
	CHECK(listener.is_listening());
	CHECK(log.text().find("connection details file") == std::string::npos);
	// ドックが読む stats().open にも、同じ problem がコピーされる (outcome だけでは待ち受け中と区別できない)。
	CHECK(listener.stats().open.outcome == OpenOutcome::Opened);
	CHECK(listener.stats().open.problem == OpenProblem::SessionFileNotSaved);

	CHECK_FALSE(text_reveals_secret_or_port(log.text(), open.secret_hex, open.port));
	listener.stop();
	// 待ち受けを始めたときに起きたことなので、stop() の後も残る (outcome と同じ)。
	CHECK(listener.stats().open.problem == OpenProblem::SessionFileNotSaved);
}

TEST_CASE("問題なく開けたときと、開かなかったとき (AlreadyOpenedBefore) は、problem が None になる")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	CHECK(listener.stats().open.problem == OpenProblem::None); // 開こうとする前
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	CHECK(open.problem == OpenProblem::None);
	CHECK(listener.stats().open.problem == OpenProblem::None);
	const auto reopen = listener.try_open();
	CHECK(reopen.outcome == OpenOutcome::AlreadyOpenedBefore);
	CHECK(reopen.problem == OpenProblem::None);
	CHECK(listener.stats().open.problem == OpenProblem::None);
	listener.stop();
}

// OpenProblem::AcceptFailed (ws/listener.cpp で start_accept がエラーを返す場合) は、公開の関数からは起こせないので
// テストがない。start_accept は、listen() が成功した直後の acceptor に async_accept を積むだけで、テスト用の
// PlatformOps で失敗させることも、asio の endpoint を壊すこともできない。problem を設定する行と、呼び出し元が
// ログに書く行は、コードを読んで確認した。この場合に開いたポートを閉じて port() を 0 に戻す処理も、同じ理由で
// テストできない。websocketpp 0.8.2 の websocketpp/transport/asio/endpoint.hpp の stop_listening は、endpoint が
// LISTENING のときに acceptor を閉じて READY に戻す。listen() の後は LISTENING なので、この処理で閉じられる。

// 送信に失敗したとき (ws/listener.cpp の send_text のエラーの分岐。send_failures を数え、1 つの接続で最初の 1 回だけ
// "Couldn't send a message to Vesktop" をログに書く) は、公開の関数から確実には起こせないので、テストがない。
// 次の 2 つを試したが、どちらでも送信の失敗は起きなかった。
// (1) hello_ng を送って閉じた直後 (conn_state が AwaitingHello のまま戻らない間) に、2 つ目の hello を送る。
//     websocketpp は、閉じている途中の接続では message_handler を呼ばないとみられ、1 度も起きなかった。
// (2) SO_LINGER (1, 0) で接続を強制的に閉じた (RST) 直後に、request_keyframe を 500 ms の間続けて呼ぶ。5 回試して、
//     どれも切断の検出 (on_fail か on_close で client_connected が false になる) のほうが先に起きた。
// send_text を呼ぶ所は、どれも has_connection と conn_state == Connected を確認した後にあり、公開の関数からその
// 条件を破れない。ログの行が決まった文字列だけで、トークンとポート番号を含まないことは、ws/listener.cpp を読んで
// 確認した。

// ---------------------------------------------------------------------------
// ドックが読む値 (OpenResult のコピー、接続の時刻と理由、ハートビートの状態)
// ---------------------------------------------------------------------------

TEST_CASE("待ち受けを開けた結果 (ポート番号と試した範囲) を、stats().open にコピーする")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());

	CHECK_FALSE(listener.stats().open.attempted); // 開こうとする前は、まだ結果がない

	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	const auto stats = listener.stats();
	CHECK(stats.open.attempted);
	CHECK(stats.open.outcome == OpenOutcome::Opened);
	CHECK(stats.open.port == open.port);
	CHECK(stats.open.port_first == base);
	CHECK(stats.open.port_last == uint16_t(base + 15)); // port_attempts は 16
	CHECK(stats.open.other_writer_pid == 0);

	listener.stop();
}

TEST_CASE("別の OBS が動いていた結果 (pid と起動の時刻) を、stats().open にコピーする")
{
	Listener listener(fast_test_config(next_test_port_base()), make_fake_platform_ops_blocked(999, 12345));
	const auto result = listener.try_open();
	REQUIRE(result.outcome == OpenOutcome::BlockedByOtherWriter);

	const auto stats = listener.stats();
	CHECK(stats.open.attempted);
	CHECK(stats.open.outcome == OpenOutcome::BlockedByOtherWriter);
	CHECK(stats.open.other_writer_pid == 999);
	CHECK(stats.open.other_writer_started_at == 12345);
	// ポートを 1 つも試していないので、試した範囲は 0 のままにする。
	CHECK(stats.open.port_first == 0);
	CHECK(stats.open.port_last == 0);
}

TEST_CASE("ポートがすべて使用中のときは、試したポート番号の範囲を stats().open にコピーする")
{
	const uint16_t base = next_test_port_base();

	// 16 個のポートをすべて使用中にする (ログに試したポートの番号を書かないことのテストと同じ)。
	asio::io_context blocker_io;
	std::vector<std::unique_ptr<asio::ip::tcp::acceptor>> blockers;
	for (int i = 0; i < 16; i++) {
		auto acc = std::make_unique<asio::ip::tcp::acceptor>(blocker_io);
		acc->open(asio::ip::tcp::v4());
		acc->bind(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), uint16_t(base + i)));
		acc->listen();
		blockers.push_back(std::move(acc));
	}

	Listener listener(fast_test_config(base), make_fake_platform_ops());
	REQUIRE(listener.try_open().outcome == OpenOutcome::PortsExhausted);

	const auto stats = listener.stats();
	CHECK(stats.open.outcome == OpenOutcome::PortsExhausted);
	CHECK(stats.open.port == 0);
	CHECK(stats.open.port_first == base);
	CHECK(stats.open.port_last == uint16_t(base + 15));
}

TEST_CASE("接続を受け付けた時刻と、hello が通った時刻を残す")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 5000; // ハートビートで切断される前に確認したいので、長くする
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	CHECK(listener.stats().connection_opened_at_us == 0); // まだ接続がない

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	const auto stats = listener.stats();
	CHECK(stats.connections_accepted == 1);
	CHECK(stats.connection_opened_at_us > 0);
	CHECK(stats.hello_ok_at_us >= stats.connection_opened_at_us);
	CHECK(stats.connection_closed_at_us == 0); // まだ切れていない
	CHECK(stats.last_disconnect_reason == DisconnectReason::Unknown);

	listener.stop();
}

TEST_CASE("トークンが一致せずに切れた接続にも、受け付けた時刻と切れた理由を残す")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json("0000000000000000000000000000000000000000000000000000000000dead"));
	REQUIRE(client.wait_message()); // hello_ng
	CHECK(client.wait_closed());
	REQUIRE(wait_until([&] { return listener.stats().connection_closed_at_us > 0; }));

	const auto stats = listener.stats();
	// hello が通っていなくても、接続は受け付けている。ドックは、接続した時刻と切れた理由を使って、トークンが
	// 一致しなかったことを表示する。
	CHECK(stats.connection_opened_at_us > 0);
	CHECK(stats.hello_ok_at_us == 0);
	CHECK(stats.connection_closed_at_us >= stats.connection_opened_at_us);
	CHECK(stats.last_disconnect_reason == DisconnectReason::SecretMismatch);
	CHECK(stats.hello_secret_mismatches == 1);

	listener.stop();
}

TEST_CASE("ワイヤー形式のバージョンが違って切断したときの理由は WireMismatch になる")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex, kWireVersion + 1));
	REQUIRE(client.wait_message()); // バージョンが合わないことを伝える hello_ng (bad_wire)
	REQUIRE(wait_until([&] { return listener.stats().connection_closed_at_us > 0; }));

	CHECK(listener.stats().last_disconnect_reason == DisconnectReason::WireMismatch);
	listener.stop();
}

TEST_CASE("最初のメッセージが hello でないときの理由は MalformedHello になる")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(R"({"type":"pong","seq":1})"); // hello ではない
	REQUIRE(wait_until([&] { return listener.stats().connection_closed_at_us > 0; }));

	CHECK(listener.stats().last_disconnect_reason == DisconnectReason::MalformedHello);
	listener.stop();
}

TEST_CASE("hello を送らない接続を切断したときの理由は HelloTimeout になる")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.hello_timeout_ms = 100;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient silent;
	REQUIRE(silent.connect(open.port));
	REQUIRE(silent.wait_opened());
	// 数は切断を始める前に増え、理由は切断が済んでから入る (listener.cpp の reset_connection_state)。理由が入るまで待つ。
	REQUIRE(wait_until([&] {
		const auto s = listener.stats();
		return s.hello_timeouts == 1 && s.last_disconnect_reason != DisconnectReason::Unknown;
	}));

	const auto stats = listener.stats();
	CHECK(stats.last_disconnect_reason == DisconnectReason::HelloTimeout);
	CHECK(stats.connection_closed_at_us > 0);
	listener.stop();
}

TEST_CASE("pong が返らずに切断したときの理由は HeartbeatTimeout になる")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	// pong を返さないので、2 回続けて返らなかった時点で切断される (fast_test_config の ping の間隔は 30 ms)。
	// 数は切断を始める前に増え、理由は切断が済んでから入る (listener.cpp の reset_connection_state)。理由が入るまで待つ。
	REQUIRE(wait_until([&] {
		const auto s = listener.stats();
		return s.heartbeat_timeouts == 1 && s.last_disconnect_reason != DisconnectReason::Unknown;
	}));

	const auto stats = listener.stats();
	CHECK(stats.last_disconnect_reason == DisconnectReason::HeartbeatTimeout);
	CHECK(stats.pings_sent >= 2);
	CHECK(stats.pongs_received == 0);
	// 2 は fast_test_config の max_missed_pongs (製品の既定と同じ値)。
	CHECK_FALSE(heartbeat_alive(stats, 2)); // 接続がないので false
	listener.stop();
}

TEST_CASE("hello と pong の clock_us を、読む前に読んだ受け取りの時刻と組にして、on_connected の後に on_sender_clock で渡す")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	std::mutex mu;
	std::vector<std::string> calls;
	listener.on_connected = [&] {
		std::lock_guard<std::mutex> lock(mu);
		calls.push_back("connected");
	};
	std::atomic<std::int64_t> clock_reads{0};
	listener.receive_clock_us = [&] { return 1000 + clock_reads.fetch_add(1); };
	listener.on_sender_clock = [&](std::int64_t sender_us, std::int64_t received_us) {
		std::lock_guard<std::mutex> lock(mu);
		calls.push_back("clock " + std::to_string(sender_us) + " at " + std::to_string(received_us));
	};
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex, kWireVersion, std::nullopt, 1'700'000'000'000'000));
	REQUIRE(client.wait_message());
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));
	client.send(R"({"type":"pong","seq":1,"clock_us":1700000000005000})");
	client.send(R"({"type":"pong","seq":2})"); // clock_us のない pong では呼ばない
	REQUIRE(wait_until([&] { return listener.stats().pongs_received >= 2; }));
	listener.stop();

	// 受け取った時刻は、テキストのメッセージを読む前に 1 回ずつ読んだ時計の値 (hello が 1000、1 つ目の pong が 1001)。
	std::lock_guard<std::mutex> lock(mu);
	CHECK(calls == std::vector<std::string>{"connected", "clock 1700000000000000 at 1000",
						"clock 1700000000005000 at 1001"});
}

TEST_CASE("hello の clock_base を、on_connected の後、hello の on_sender_clock の前に on_sender_clock_base で渡す")
{
	for (const auto &[base_key, expected] :
	     std::vector<std::pair<std::optional<std::string>, std::string>>{{std::string("qpc"), "base qpc"},
									     {std::nullopt, "base unspecified"},
									     {std::string("other"), "base unspecified"}}) {
		const uint16_t base = next_test_port_base();
		Listener listener(fast_test_config(base), make_fake_platform_ops());
		std::mutex mu;
		std::vector<std::string> calls;
		listener.on_connected = [&] {
			std::lock_guard<std::mutex> lock(mu);
			calls.push_back("connected");
		};
		listener.on_sender_clock_base = [&](SenderClockBase b) {
			std::lock_guard<std::mutex> lock(mu);
			calls.push_back(b == SenderClockBase::Qpc ? "base qpc" : "base unspecified");
		};
		listener.receive_clock_us = [] { return std::int64_t{1000}; };
		listener.on_sender_clock = [&](std::int64_t sender_us, std::int64_t) {
			std::lock_guard<std::mutex> lock(mu);
			calls.push_back("clock " + std::to_string(sender_us));
		};
		const auto open = listener.try_open();
		REQUIRE(open.outcome == OpenOutcome::Opened);

		TestClient client;
		REQUIRE(client.connect(open.port));
		REQUIRE(client.wait_opened());
		client.send(hello_json(open.secret_hex, kWireVersion, std::nullopt, 42, base_key));
		REQUIRE(client.wait_message());
		REQUIRE(wait_until([&] { return listener.stats().client_connected; }));
		listener.stop();

		std::lock_guard<std::mutex> lock(mu);
		CHECK(calls == std::vector<std::string>{"connected", expected, "clock 42"});
	}
}

TEST_CASE("pong を返すクライアントとの接続では、heartbeat_alive が true を返す")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	// ping が届くたびに pong を返す。
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
	size_t answered = 0;
	while (std::chrono::steady_clock::now() < deadline) {
		const auto msgs = client.received();
		while (answered < msgs.size()) {
			const auto j = try_parse_json(msgs[answered]);
			if (j && peek_type(*j) == std::string(control_type::kPing))
				client.send(R"({"type":"pong","seq":1})");
			answered++;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	const auto stats = listener.stats();
	CHECK(stats.client_connected);
	CHECK(stats.pongs_received >= 1);
	CHECK(stats.last_pong_at_us > 0);
	CHECK(heartbeat_alive(stats, 2)); // 2 は fast_test_config の max_missed_pongs
	CHECK(stats.heartbeat_timeouts == 0);
	listener.stop();
}

TEST_CASE("相手が切断したときの理由は PeerClosed になる")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 5000; // ハートビートで切断されるより先に、クライアントから閉じる
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	RawWsClient raw;
	REQUIRE(raw.connect_to(open.port));
	const auto hs = raw.handshake("127.0.0.1:" + std::to_string(open.port));
	REQUIRE(hs.is_101);
	raw.send_frame(0x1, hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	raw.send_frame(0x8, std::string("\x03\xe8", 2)); // close のフレーム (1000 = normal)
	REQUIRE(wait_until([&] { return !listener.stats().client_connected; }));

	const auto stats = listener.stats();
	CHECK(stats.last_disconnect_reason == DisconnectReason::PeerClosed);
	CHECK(stats.connection_closed_at_us >= stats.hello_ok_at_us);
	listener.stop();
}

TEST_CASE("stop() で閉じたときの理由は LocalClose になる")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 5000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(client.wait_message());
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	listener.stop();

	const auto stats = listener.stats();
	CHECK_FALSE(stats.client_connected);
	CHECK(stats.last_disconnect_reason == DisconnectReason::LocalClose);
	CHECK(stats.connection_closed_at_us > 0);
}

TEST_CASE("送信側が hello で送ったバージョンを stats() に入れ、stop() の後は空に戻す")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 5000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	CHECK(listener.stats().peer_version.empty()); // まだ接続がない

	{
		TestClient client;
		REQUIRE(client.connect(open.port));
		REQUIRE(client.wait_opened());
		client.send(hello_json(open.secret_hex)); // version は "test-client"
		REQUIRE(client.wait_message());
		REQUIRE(wait_until([&] { return listener.stats().client_connected; }));
		CHECK(listener.stats().peer_version == "test-client");
		// hello に vesktop のキーがなければ、Vesktop のバージョンは空のまま (分からないことを示す)。
		CHECK(listener.stats().peer_vesktop_version.empty());
	}

	listener.stop();
	// stop() の後は、接続している送信側のバージョンではないので空にする。
	CHECK(listener.stats().peer_version.empty());
	CHECK(listener.stats().peer_vesktop_version.empty());
}

TEST_CASE("hello の任意のキー vesktop で送られた Vesktop のバージョンを stats() に入れ、切断すると空に戻す")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 5000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	CHECK(listener.stats().peer_vesktop_version.empty()); // まだ接続がない

	// client_connected だけを待ってからバージョンを確認しない。ws/listener.cpp は、hello が通ったときに client_connected を
	// true にしてから 2 つのバージョンを書き、切断したときも client_connected を false にしてからバージョンを消す。
	// client_connected だけを待つと、その間に読んで結果が変わることがある。2 つのバージョンは同じロックの中で一緒に
	// 書き換わるので、peer_version が変わるのを待てば、vesktop を確実に確認できる。
	const auto connected_as_test_client = [&] {
		const ListenerStats s = listener.stats();
		return s.client_connected && s.peer_version == "test-client";
	};

	{
		RawWsClient raw;
		REQUIRE(raw.connect_to(open.port));
		REQUIRE(raw.handshake("127.0.0.1:" + std::to_string(open.port)).is_101);
		raw.send_frame(0x1, hello_json(open.secret_hex, kWireVersion, std::string("1.6.7")));
		REQUIRE(wait_until(connected_as_test_client));
		CHECK(listener.stats().peer_vesktop_version == "1.6.7");

		// 相手が切断したら空に戻る (接続していない相手のバージョンを表示し続けない)。
		raw.send_frame(0x8, std::string("\x03\xe8", 2)); // close のフレーム (1000 = normal)
		REQUIRE(wait_until([&] {
			const ListenerStats s = listener.stats();
			return !s.client_connected && s.peer_version.empty();
		}));
		CHECK(listener.stats().peer_vesktop_version.empty());
	}

	{
		// 次の送信側が vesktop のキーを送らなければ、前の送信側のバージョンを引き継がない。前のバージョンは切断で
		// 消えているので、新しい hello を読み終えるまで待ってから確認する。待たずに読むと、hello を読む前の空の値を
		// 読んで、確認したことにならない。
		RawWsClient raw;
		REQUIRE(raw.connect_to(open.port));
		REQUIRE(raw.handshake("127.0.0.1:" + std::to_string(open.port)).is_101);
		raw.send_frame(0x1, hello_json(open.secret_hex));
		REQUIRE(wait_until(connected_as_test_client));
		CHECK(listener.stats().peer_vesktop_version.empty());
	}

	listener.stop();
	CHECK(listener.stats().peer_vesktop_version.empty());
}

TEST_CASE("stats().listening は待ち受けている間だけ true で、stop() の後も stats().open の結果は残る")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	CHECK_FALSE(listener.stats().listening);

	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	CHECK(listener.stats().listening);

	listener.stop();
	const auto stats = listener.stats();
	CHECK_FALSE(stats.listening);
	// 開けた結果は残る (ドックは、開けたが今は閉じていることを区別できる)。
	CHECK(stats.open.outcome == OpenOutcome::Opened);
	CHECK(stats.open.port == open.port);
}

TEST_CASE("heartbeat_alive は、接続がないときと、pong が返らない ping が max_missed_pongs に達したときに false を返す")
{
	ListenerStats stats;
	CHECK_FALSE(heartbeat_alive(stats, 2)); // 接続がない

	stats.client_connected = true;
	stats.unanswered_pings = 0;
	CHECK(heartbeat_alive(stats, 2));
	stats.unanswered_pings = 1;
	CHECK(heartbeat_alive(stats, 2)); // 1 回はまだ待つ
	stats.unanswered_pings = 2;
	CHECK_FALSE(heartbeat_alive(stats, 2)); // 2 回続けて返事がない
}

TEST_CASE("正しい形の profiles は on_profiles に渡し、dropped_unknown_control には数えない")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	std::atomic<int> received{0};
	std::vector<std::string> ids;
	std::mutex ids_mu;
	listener.on_profiles = [&](const ProfilesMessage &m) {
		{
			std::lock_guard<std::mutex> lock(ids_mu);
			for (const auto &entry : m.profiles)
				ids.push_back(entry.user_id);
		}
		received++;
	};

	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	client.send(R"({"type":"profiles","profiles":[{"user_id":"100","display_name":"taro"},{"user_id":"200"}]})");
	CHECK(wait_until([&] { return received.load() == 1; }));
	{
		std::lock_guard<std::mutex> lock(ids_mu);
		REQUIRE(ids.size() == 2);
		CHECK(ids[0] == "100");
		CHECK(ids[1] == "200");
	}
	// バージョンの食い違いを見つけるための値を増やさない。profiles は通話の間ずっと届くので、数えると
	// dropped_unknown_control が役に立たなくなる。
	CHECK(listener.stats().dropped_unknown_control == 0);

	listener.stop();
}

TEST_CASE("壊れた要素が 1 つでもある profiles は、全体を捨てて数え、接続は切らない")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	std::atomic<int> received{0};
	listener.on_profiles = [&](const ProfilesMessage &) { received++; };

	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	// 2 つ目の要素の display_name が文字列でない。
	client.send(R"({"type":"profiles","profiles":[{"user_id":"100"},{"user_id":"200","display_name":7}]})");
	CHECK(wait_until([&] { return listener.stats().dropped_unknown_control == 1; }));
	CHECK(received.load() == 0);
	// 捨てても接続は切らない (制御用の JSON が 1 つ壊れていただけで、接続を止めない)。
	CHECK(listener.stats().client_connected);

	listener.stop();
}

TEST_CASE("on_profiles が nullptr でも、profiles を dropped_unknown_control には数えない")
{
	const uint16_t base = next_test_port_base();
	Listener listener(fast_test_config(base), make_fake_platform_ops());
	// on_profiles は nullptr のまま。

	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	client.send(R"({"type":"profiles","profiles":[{"user_id":"100"}]})");
	// 続けて pong を送り、pongs_received が増えたことで、その前の profiles を処理し終えたことを確認する。
	client.send(R"({"type":"pong","seq":1})");
	CHECK(wait_until([&] { return listener.stats().pongs_received == 1; }));
	CHECK(listener.stats().dropped_unknown_control == 0);

	listener.stop();
}

// ---- media と limits ----

namespace {

// 届いたテキストのうち、type が一致する最後のメッセージ (なければ nullopt)。ping が定期的に混ざるので、位置では
// なく type で探す。
std::optional<nlohmann::json> last_of_type(const std::vector<std::string> &texts, const char *type)
{
	std::optional<nlohmann::json> found;
	for (const std::string &text : texts) {
		const auto j = try_parse_json(text);
		if (j && peek_type(*j) == std::string(type))
			found = *j;
	}
	return found;
}

std::size_t count_of_type(const std::vector<std::string> &texts, const char *type)
{
	std::size_t n = 0;
	for (const std::string &text : texts) {
		const auto j = try_parse_json(text);
		if (j && peek_type(*j) == std::string(type))
			++n;
	}
	return n;
}

} // namespace

TEST_CASE("接続する前に send_media と send_limits に渡した値は、hello_ok の初期値に入る")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 60000; // ハートビートは確認しない (pong を返さないクライアントが切断されないようにする)
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	listener.send_media(false);
	LimitsMessage limits;
	limits.max_long_edge = 1280;
	limits.fps_divisor = 2;
	listener.send_limits(limits);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return last_of_type(client.received(), control_type::kHelloOk).has_value(); }));

	const auto hello_ok = parse_hello_ok(*last_of_type(client.received(), control_type::kHelloOk));
	REQUIRE(hello_ok.has_value());
	REQUIRE(hello_ok->media.has_value());
	CHECK_FALSE(*hello_ok->media);
	REQUIRE(hello_ok->limits.has_value());
	CHECK(*hello_ok->limits == limits);
	// 接続する前に渡した値は、その時点では送らない (接続していなければ捨てる)。
	CHECK(count_of_type(client.received(), control_type::kMedia) == 0);
	CHECK(count_of_type(client.received(), control_type::kLimits) == 0);
	listener.stop();
}

TEST_CASE("send_media と send_limits を 1 度も呼んでいなければ、hello_ok に media と limits のキーを書かない")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 60000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return last_of_type(client.received(), control_type::kHelloOk).has_value(); }));
	const auto j = last_of_type(client.received(), control_type::kHelloOk);
	CHECK(j->find("media") == j->end());
	CHECK(j->find("limits") == j->end());
	listener.stop();
}

TEST_CASE("接続している間は send_media と send_limits の値が 1 つずつ届き、範囲の外の limits は送らない")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 60000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	listener.send_media(true);
	REQUIRE(wait_until([&] { return count_of_type(client.received(), control_type::kMedia) == 1; }));
	const auto media = parse_media(*last_of_type(client.received(), control_type::kMedia));
	REQUIRE(media.has_value());
	CHECK(media->on);

	LimitsMessage bad;
	bad.max_long_edge = 0; // 0 は範囲の外 (制限しないときはキーを省く)
	listener.send_limits(bad);
	LimitsMessage good;
	good.fps_divisor = 2;
	listener.send_limits(good);
	REQUIRE(wait_until([&] { return count_of_type(client.received(), control_type::kLimits) >= 1; }));
	// 範囲の外の値は送られていない。届いた limits は後の 1 つだけである。
	CHECK(count_of_type(client.received(), control_type::kLimits) == 1);
	const auto limits = parse_limits(*last_of_type(client.received(), control_type::kLimits));
	REQUIRE(limits.has_value());
	CHECK(*limits == good);
	listener.stop();
}

TEST_CASE("エンコードの設定のキーも limits に入って届く")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 60000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	LimitsMessage sent;
	sent.encoder_preference = "software";
	sent.latency_mode = "quality";
	listener.send_limits(sent);
	REQUIRE(wait_until([&] { return count_of_type(client.received(), control_type::kLimits) == 1; }));
	const auto limits = parse_limits(*last_of_type(client.received(), control_type::kLimits));
	REQUIRE(limits.has_value());
	CHECK(*limits == sent);
	CHECK(limits->encoder_preference == std::optional<std::string>("software"));
	CHECK(limits->latency_mode == std::optional<std::string>("quality"));
	listener.stop();
}

// ---- 開き直しと session.json のポーリング ----

TEST_CASE("別のプロセスが書いた session.json を書いたプロセスが終了した後に try_open し直すと、開けて stats().open も Opened になる")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	file.content = cordmux::platform::SessionInfo{9999, "other-secret", 7000, 500};
	file.alive.push_back({7000, 500});

	Listener listener(fast_test_config(base), file.ops());
	CHECK_FALSE(listener.can_retry_open()); // まだ試していない
	REQUIRE(listener.try_open().outcome == OpenOutcome::BlockedByOtherWriter);
	CHECK(listener.can_retry_open());
	CHECK(listener.stats().open.outcome == OpenOutcome::BlockedByOtherWriter);

	// 書いたプロセスが動いている間は、何度呼んでも開かない。
	REQUIRE(listener.try_open().outcome == OpenOutcome::BlockedByOtherWriter);
	CHECK_FALSE(listener.is_listening());

	file.alive.clear();
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	CHECK(open.secret_hex.size() == 64);
	CHECK(listener.is_listening());
	CHECK_FALSE(listener.can_retry_open());
	CHECK(listener.stats().open.outcome == OpenOutcome::Opened);
	CHECK(listener.stats().open.other_writer_pid == 0);
	REQUIRE(file.content.has_value());
	CHECK(file.content->writer_pid == file.me.pid);
	CHECK(file.content->secret_hex == open.secret_hex);

	// 一度開いた後は、stop() の後も開き直せない (AlreadyOpenedBefore になる)ので、呼び直す対象にしない。
	listener.stop();
	CHECK_FALSE(listener.can_retry_open());
}

TEST_CASE("別の OBS が動いている間は、何度試しても session.json を書き換えず、待ち受けも始めない")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	const cordmux::platform::SessionInfo other{9999, "other-secret", 7000, 500};
	file.content = other;
	file.alive.push_back({7000, 500});

	Listener listener(fast_test_config(base), file.ops());
	for (int i = 0; i < 3; i++)
		REQUIRE(listener.try_open().outcome == OpenOutcome::BlockedByOtherWriter);

	CHECK(file.writes == 0);
	REQUIRE(file.content.has_value());
	CHECK(file.content->port == other.port);
	CHECK(file.content->secret_hex == other.secret_hex);
	CHECK(file.content->writer_pid == other.writer_pid);
	CHECK(file.content->writer_started_at == other.writer_started_at);
	CHECK_FALSE(listener.is_listening());
	CHECK_FALSE(listener.stats().listening);
	CHECK(listener.stats().open.other_writer_pid == 7000);
	// 塞がれた Listener は、自分が書いたものがないので、別の OBS の session.json を見回しても書き直さない。
	CHECK(listener.check_session_file() == SessionCheck::NotApplicable);
	CHECK(file.writes == 0);
}

TEST_CASE("OBS が落ちて終わった書き手の session.json が残っていても、最初の try_open で新しいトークンを書いて開く")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	file.content = cordmux::platform::SessionInfo{base, "stale-secret", 7000, 500};
	// alive は空 = 書いた OBS はもう動いていない。

	Listener listener(fast_test_config(base), file.ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	CHECK(file.writes == 1);
	REQUIRE(file.content.has_value());
	CHECK(file.content->secret_hex == open.secret_hex);
	CHECK(file.content->secret_hex != "stale-secret");
	CHECK(file.content->writer_pid == file.me.pid);
	CHECK(file.content->writer_started_at == file.me.started_at);
	CHECK(file.content->port == open.port);
	listener.stop();
}

TEST_CASE("ポートがすべて使用中だった後に、ポートが空けば、try_open し直して開ける")
{
	const uint16_t base = next_test_port_base();
	asio::io_context blocker_io;
	std::vector<std::unique_ptr<asio::ip::tcp::acceptor>> blockers;
	for (int i = 0; i < 16; i++) {
		auto acc = std::make_unique<asio::ip::tcp::acceptor>(blocker_io);
		acc->open(asio::ip::tcp::v4());
		acc->bind(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), uint16_t(base + i)));
		acc->listen();
		blockers.push_back(std::move(acc));
	}

	Listener listener(fast_test_config(base), make_fake_platform_ops());
	REQUIRE(listener.try_open().outcome == OpenOutcome::PortsExhausted);
	CHECK(listener.can_retry_open());

	blockers.clear();
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	CHECK(listener.stats().open.outcome == OpenOutcome::Opened);
	CHECK(listener.stats().listening);
	listener.stop();
}

TEST_CASE("check_session_file は、同じ中身なら書かず、消えていれば書き直し、違う中身で書いたプロセスが終了していれば書き直す")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	Listener listener(fast_test_config(base), file.ops());
	CHECK(listener.check_session_file() == SessionCheck::NotApplicable); // 待ち受ける前
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	REQUIRE(file.writes == 1);

	CHECK(listener.check_session_file() == SessionCheck::Unchanged);
	CHECK(file.writes == 1);

	file.content.reset();
	CHECK(listener.check_session_file() == SessionCheck::Rewritten);
	CHECK(file.writes == 2);
	REQUIRE(file.content.has_value());
	CHECK(file.content->port == open.port);
	CHECK(file.content->secret_hex == open.secret_hex);

	// 書き換えた人は動いていない (alive が空)。
	file.content = cordmux::platform::SessionInfo{1234, "stale", 7000, 500};
	CHECK(listener.check_session_file() == SessionCheck::Rewritten);
	CHECK(file.writes == 3);
	CHECK(file.content->secret_hex == open.secret_hex);
	CHECK(listener.check_session_file() == SessionCheck::Unchanged);
	listener.stop();
}

TEST_CASE("check_session_file は、動いている別のプロセスが書いた中身を書き直さず、ログに 1 回だけ書く")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	CapturingLog log;
	Listener listener(fast_test_config(base, log.sink()), file.ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	file.content = cordmux::platform::SessionInfo{1234, "other-secret", 7000, 500};
	file.alive.push_back({7000, 500});
	const int writes_before = file.writes;
	CHECK(listener.check_session_file() == SessionCheck::LeftToOtherWriter);
	CHECK(listener.check_session_file() == SessionCheck::LeftToOtherWriter);
	CHECK(file.writes == writes_before);
	CHECK(file.content->secret_hex == "other-secret");

	const std::string text = log.text();
	const std::string needle = "another running process";
	const size_t first = text.find(needle);
	REQUIRE(first != std::string::npos);
	CHECK(text.find(needle, first + 1) == std::string::npos);
	CHECK_FALSE(text_reveals_secret_or_port(text, open.secret_hex, open.port));
	listener.stop();
}

TEST_CASE("check_session_file は、書けなければ problem を SessionFileNotSaved にし、次に書けたら None に戻す")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	Listener listener(fast_test_config(base), file.ops());
	REQUIRE(listener.try_open().outcome == OpenOutcome::Opened);
	CHECK(listener.stats().open.problem == OpenProblem::None);

	file.content.reset();
	file.write_fails = true;
	CHECK(listener.check_session_file() == SessionCheck::WriteFailed);
	CHECK(listener.stats().open.problem == OpenProblem::SessionFileNotSaved);
	// 失敗が続いている間は WriteFailed を繰り返さない (呼び出し元がログの行を 1 回だけ書けるように)。
	CHECK(listener.check_session_file() == SessionCheck::WriteStillFailing);
	CHECK(listener.stats().open.problem == OpenProblem::SessionFileNotSaved);

	file.write_fails = false;
	CHECK(listener.check_session_file() == SessionCheck::Rewritten);
	CHECK(listener.stats().open.problem == OpenProblem::None);
	listener.stop();
}

TEST_CASE("try_open で session.json を書けなかった後も、check_session_file が書き直して problem を None に戻す")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	file.write_fails = true;
	Listener listener(fast_test_config(base), file.ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);
	REQUIRE(open.problem == OpenProblem::SessionFileNotSaved);

	file.write_fails = false;
	CHECK(listener.check_session_file() == SessionCheck::Rewritten);
	CHECK(listener.stats().open.problem == OpenProblem::None);
	REQUIRE(file.content.has_value());
	CHECK(file.content->secret_hex == open.secret_hex);
	listener.stop();
}

TEST_CASE("check_session_file は、Vesktop が接続している間と、stop() の後は確認しない")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	Listener listener(fast_test_config(base), file.ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	file.content.reset();
	CHECK(listener.check_session_file() == SessionCheck::NotApplicable);
	CHECK_FALSE(file.content.has_value());

	listener.stop();
	CHECK(listener.check_session_file() == SessionCheck::NotApplicable);
}

TEST_CASE("remove_session_file_if_mine は、自分が書いた session.json だけを削除する")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	Listener listener(fast_test_config(base), file.ops());
	REQUIRE(listener.try_open().outcome == OpenOutcome::Opened);
	listener.stop();

	// 別の OBS が書いたファイル (pid か起動の時刻が違う) は削除しない。
	const cordmux::platform::SessionInfo mine = *file.content;
	auto other_pid = mine;
	other_pid.writer_pid = mine.writer_pid + 1;
	file.content = other_pid;
	CHECK_FALSE(listener.remove_session_file_if_mine());
	CHECK(file.content.has_value());

	auto other_start = mine;
	other_start.writer_started_at = mine.writer_started_at + 1;
	file.content = other_start;
	CHECK_FALSE(listener.remove_session_file_if_mine());
	CHECK(file.content.has_value());

	// ないときは何もしない。
	file.content.reset();
	CHECK_FALSE(listener.remove_session_file_if_mine());

	file.content = mine;
	CHECK(listener.remove_session_file_if_mine());
	CHECK_FALSE(file.content.has_value());
}

TEST_CASE("待ち受けなかった Listener は、別の OBS の session.json を削除しない")
{
	const uint16_t base = next_test_port_base();
	FakeSessionFile file;
	file.content = cordmux::platform::SessionInfo{9999, "other-secret", 7000, 500};
	file.alive.push_back({7000, 500});
	Listener listener(fast_test_config(base), file.ops());
	REQUIRE(listener.try_open().outcome == OpenOutcome::BlockedByOtherWriter);
	listener.stop();
	CHECK_FALSE(listener.remove_session_file_if_mine());
	CHECK(file.content.has_value());
}

TEST_CASE("接続している間は main_account、receive_scope、move_to_main_call、leave_call が届く")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 60000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));

	listener.send_main_account("42");
	REQUIRE(wait_until([&] { return count_of_type(client.received(), control_type::kMainAccount) == 1; }));
	const auto main_account = parse_main_account(*last_of_type(client.received(), control_type::kMainAccount));
	REQUIRE(main_account.has_value());
	CHECK(main_account->user_id == "42");

	ReceiveScopeMessage scope;
	scope.slots_only = true;
	scope.user_ids = {"1", "2"};
	listener.send_receive_scope(scope);
	REQUIRE(wait_until([&] { return count_of_type(client.received(), control_type::kReceiveScope) == 1; }));
	const auto received = parse_receive_scope(*last_of_type(client.received(), control_type::kReceiveScope));
	REQUIRE(received.has_value());
	CHECK(*received == scope);

	listener.send_move_to_main_call();
	listener.send_leave_call();
	REQUIRE(wait_until([&] {
		return count_of_type(client.received(), control_type::kMoveToMainCall) == 1 &&
		       count_of_type(client.received(), control_type::kLeaveCall) == 1;
	}));
	listener.stop();
}

TEST_CASE("接続していない間に呼んだ main_account、receive_scope、move_to_main_call、leave_call は捨て、あとから送らない")
{
	const uint16_t base = next_test_port_base();
	ListenerConfig cfg = fast_test_config(base);
	cfg.ping_interval_ms = 60000;
	Listener listener(cfg, make_fake_platform_ops());
	const auto open = listener.try_open();
	REQUIRE(open.outcome == OpenOutcome::Opened);

	listener.send_main_account("42");
	listener.send_receive_scope(ReceiveScopeMessage{});
	listener.send_move_to_main_call();
	listener.send_leave_call();

	TestClient client;
	REQUIRE(client.connect(open.port));
	REQUIRE(client.wait_opened());
	client.send(hello_json(open.secret_hex));
	REQUIRE(wait_until([&] { return listener.stats().client_connected; }));
	// 接続した後に、届かなかったことを確認する (hello_ok が届くまで待ってから、少し待つ)。
	REQUIRE(wait_until([&] { return count_of_type(client.received(), control_type::kHelloOk) == 1; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(count_of_type(client.received(), control_type::kMainAccount) == 0);
	CHECK(count_of_type(client.received(), control_type::kReceiveScope) == 0);
	CHECK(count_of_type(client.received(), control_type::kMoveToMainCall) == 0);
	CHECK(count_of_type(client.received(), control_type::kLeaveCall) == 0);
	listener.stop();
}

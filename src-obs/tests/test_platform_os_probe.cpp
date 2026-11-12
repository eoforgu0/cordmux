// SPDX-License-Identifier: Apache-2.0

// platform/os_probe.h の単体テスト。実際の Vesktop にも利用者のプロファイルにも触らないように、OsProbeOps の関数は
// すべてテスト用のものを使う。問い合わせの中身は probe_once (スレッドを持たない) で確認し、OsProbe では、スレッドの
// 起動と停止と、問い合わせの間隔だけを確認する。

#include "doctest.h"

#include "platform/bundle_check.h"
#include "platform/os_probe.h"
#include "platform/sha256.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>

using cordmux::platform::BundleState;
using cordmux::platform::join_path;
using cordmux::platform::kVencordBundleFileCount;
using cordmux::platform::kVencordBundleFiles;
using cordmux::platform::OsProbe;
using cordmux::platform::OsProbeConfig;
using cordmux::platform::OsProbeOps;
using cordmux::platform::OsProbeResult;
using cordmux::platform::probe_once;
using cordmux::platform::sha256_hex;
using cordmux::platform::VesktopProcessInfo;

namespace {

constexpr const char *kBundleDir = "C:/plugins/cordmux/data/vencord";
constexpr const char *kSourceDir = "C:/plugins/cordmux/data/vencord-src";
constexpr const char *kStatePath = "C:/Users/alice/AppData/Roaming/Vesktop/state.json";
constexpr const char *kBackupPath = "C:/obs-config/cordmux/vencord_dir_backup.json";

// テスト用のファイルの一覧 (場所 -> 中身)。ここにない場所は読めない。
struct FakeWorld {
	std::map<std::string, std::string> files;
	bool vesktop_running = false;
	std::int64_t vesktop_started_at_us = 0;
	bool state_path_known = true;
	std::int64_t now_us = 948'684'800'000'000LL;
	std::shared_ptr<std::atomic<int>> probe_count = std::make_shared<std::atomic<int>>(0);

	void put_bundle(const std::string &dir, const std::string &body)
	{
		for (std::size_t i = 0; i < kVencordBundleFileCount; ++i)
			files[join_path(dir, kVencordBundleFiles[i])] = body + kVencordBundleFiles[i];
	}
};

OsProbeConfig fake_config()
{
	OsProbeConfig cfg;
	cfg.bundled_vencord_dir = kBundleDir;
	cfg.bundled_vencord_src_dir = kSourceDir;
	cfg.backup_json_path = kBackupPath;
	return cfg;
}

OsProbeOps fake_ops(const std::shared_ptr<FakeWorld> &world)
{
	OsProbeOps ops;
	ops.find_vesktop_process = [world](VesktopProcessInfo &out) {
		if (!world->vesktop_running)
			return false;
		out.pid = 1234;
		out.started_at_us = world->vesktop_started_at_us;
		return true;
	};
	ops.vesktop_state_json_path = [world](std::string &out) {
		if (!world->state_path_known)
			return false;
		out = kStatePath;
		return true;
	};
	ops.read_file_bytes = [world](const std::string &path, std::string &out, std::size_t max_bytes) {
		const auto it = world->files.find(path);
		if (it == world->files.end())
			return false;
		if (it->second.size() > max_bytes)
			return false;
		out = it->second;
		return true;
	};
	ops.now_us = [world] {
		(*world->probe_count)++;
		return world->now_us;
	};
	return ops;
}

// 4 つのファイルがそろい、vencordDir が同梱のフォルダーを指し、バックアップもある、通常の状態。
std::shared_ptr<FakeWorld> healthy_world()
{
	auto world = std::make_shared<FakeWorld>();
	world->vesktop_running = true;
	world->vesktop_started_at_us = 947'684'800'000'000LL;
	world->put_bundle(kBundleDir, "bundle-");
	world->put_bundle(kSourceDir, "bundle-"); // 中身はコピー元と同じ
	world->files[kStatePath] = std::string(R"({"vencordDir": ")") + kBundleDir + R"("})";
	world->files[kBackupPath] = std::string(R"({"written": ")") + kBundleDir +
				     R"(", "written_at": 946684800000000})";
	return world;
}

} // namespace

TEST_CASE("probe_once は、Vesktop が動いていて、vencordDir が同梱のフォルダーを指し、4 つのファイルが一致する状態を読む")
{
	const auto world = healthy_world();
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));

	CHECK(result.probed);
	CHECK(result.vesktop_process);
	CHECK(result.vesktop_started_at_us == 947'684'800'000'000LL);
	CHECK(result.state_json_read);
	REQUIRE(result.vencord_dir.has_value());
	CHECK(*result.vencord_dir == kBundleDir);
	CHECK(result.points_at_bundle);
	CHECK(result.points_at_last_written);
	CHECK(result.bundle_hash.state == BundleState::Matches);
	REQUIRE(result.state_json_written_at_us.has_value());
	CHECK(*result.state_json_written_at_us == 946'684'800'000'000LL);
	CHECK(result.probed_at_us == world->now_us);
}

TEST_CASE("Vesktop が動いていなければ、probe_once の vesktop_process は false のままで、ファイルの照合は行う")
{
	const auto world = healthy_world();
	world->vesktop_running = false;
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK_FALSE(result.vesktop_process);
	CHECK(result.vesktop_started_at_us == 0);
	CHECK(result.bundle_hash.state == BundleState::Matches); // ファイルの照合は、プロセスの有無と関係なく行う
}

TEST_CASE("起動の時刻が読めない Vesktop は、時刻を 0 にして、動いているものとする")
{
	const auto world = healthy_world();
	world->vesktop_started_at_us = 0;
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK(result.vesktop_process);
	CHECK(result.vesktop_started_at_us == 0);
}

TEST_CASE("上書きされたファイルがあれば、probe_once の照合は Mismatch になり、そのファイルの名前が入る")
{
	const auto world = healthy_world();
	world->files[join_path(kBundleDir, kVencordBundleFiles[1])] = "force update で上書きされた中身";

	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK(result.bundle_hash.state == BundleState::Mismatch);
	REQUIRE(result.bundle_hash.differing.size() == 1);
	CHECK(result.bundle_hash.differing[0] == std::string(kVencordBundleFiles[1]));
}

TEST_CASE("コピー元が読めなければ、probe_once の照合は Unknown になる (違うとは言わない)")
{
	const auto world = healthy_world();
	world->files.erase(join_path(kSourceDir, kVencordBundleFiles[0]));
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK(result.bundle_hash.state == BundleState::Unknown);
}

TEST_CASE("vencordDir が別の場所を指していても、probe_once は 4 つのファイルを照合する")
{
	// 表示の規則は、別の場所を指していること (ledger::PrimaryRuleId::VencordDirElsewhere) と、中身が違うこと
	// (VencordOverwritten) を別の行として持つ。Vencord の更新が上書きするのは data/vencord なので、vencordDir が
	// どこを指していても同じように照合する。
	const auto world = healthy_world();
	world->files[kStatePath] = R"({"vencordDir": "C:/somewhere/else/vencord"})";

	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	REQUIRE(result.vencord_dir.has_value());
	CHECK(*result.vencord_dir == "C:/somewhere/else/vencord");
	CHECK_FALSE(result.points_at_bundle);
	CHECK_FALSE(result.points_at_last_written);
	CHECK(result.bundle_hash.state == BundleState::Matches);
}

TEST_CASE("区切りと大文字小文字が違うだけの vencordDir は、同梱のフォルダーと同じ場所とみなす")
{
	const auto world = healthy_world();
	world->files[kStatePath] = R"({"vencordDir": "c:\\plugins\\CORDMUX\\data\\vencord\\"})";
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK(result.points_at_bundle);
	CHECK(result.points_at_last_written);
}

TEST_CASE("state.json が読めなければ、probe_once の state_json_read は false になる")
{
	const auto world = healthy_world();
	world->files.erase(kStatePath);
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK_FALSE(result.state_json_read);
	CHECK_FALSE(result.vencord_dir.has_value());
	CHECK_FALSE(result.points_at_bundle);
}

TEST_CASE("state.json が JSON として壊れているか形が違えば、probe_once は読めた扱いにしない")
{
	// 読めた扱いにすると vencordDir がないのと見分けがつかず、ドックが [Vesktopに追加] を出してしまう。
	const char *const broken[] = {R"({"vencordDir": "C:\x")", "", "[]", R"({"vencordDir": 5})"};
	for (const char *text : broken) {
		CAPTURE(text);
		const auto world = healthy_world();
		world->files[kStatePath] = text;
		const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
		CHECK_FALSE(result.state_json_read);
		CHECK_FALSE(result.vencord_dir.has_value());
		CHECK_FALSE(result.points_at_bundle);
	}
}

TEST_CASE("vencordDir のキーがない state.json は、probe_once が読めた扱いにする")
{
	const auto world = healthy_world();
	world->files[kStatePath] = R"({"other": 1})";
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK(result.state_json_read);
	CHECK_FALSE(result.vencord_dir.has_value());
}

TEST_CASE("state.json の場所が分からなければ、probe_once は state.json を読まない")
{
	const auto world = healthy_world();
	world->state_path_known = false;
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK_FALSE(result.state_json_read);
}

TEST_CASE("バックアップがなければ、probe_once は Vesktop の設定を書いた時刻を持たない")
{
	const auto world = healthy_world();
	world->files.erase(kBackupPath);
	const OsProbeResult result = probe_once(fake_config(), fake_ops(world));
	CHECK_FALSE(result.state_json_written_at_us.has_value());
	CHECK_FALSE(result.points_at_last_written); // 最後に書いた場所が分からない
	CHECK(result.points_at_bundle);             // 同梱のフォルダーを指していることは分かる
}

TEST_CASE("max_file_bytes より大きいファイルは、probe_once が読まない")
{
	const auto world = healthy_world();
	world->files[kStatePath] = std::string(R"({"vencordDir": ")") + kBundleDir + R"("})";
	OsProbeConfig cfg = fake_config();
	cfg.max_file_bytes = 4; // どのファイルも読めない大きさ

	const OsProbeResult result = probe_once(cfg, fake_ops(world));
	CHECK_FALSE(result.state_json_read);
	CHECK(result.bundle_hash.state == BundleState::Unknown); // コピー元も読めない
}

TEST_CASE("OsProbeOps の関数が 1 つもなくても、probe_once はクラッシュしない")
{
	const OsProbeResult result = probe_once(OsProbeConfig{}, OsProbeOps{});
	CHECK(result.probed);
	CHECK_FALSE(result.vesktop_process);
	CHECK_FALSE(result.state_json_read);
	CHECK(result.bundle_hash.state == BundleState::Unknown);
	CHECK(result.probed_at_us == 0);
}

TEST_CASE("OsProbe は start() の後に結果を持ち、stop() の後は問い合わせない")
{
	const auto world = healthy_world();
	OsProbeConfig cfg = fake_config();
	cfg.disconnected_interval_ms = 1; // テストでは間隔を短くする (既定は 30 秒)
	cfg.connected_interval_ms = 1;

	OsProbe probe(cfg, fake_ops(world));
	CHECK_FALSE(probe.latest().probed); // start() の前は、まだ問い合わせていない

	probe.start();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!probe.latest().probed && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));

	const OsProbeResult result = probe.latest();
	REQUIRE(result.probed);
	CHECK(result.vesktop_process);
	CHECK(result.bundle_hash.state == BundleState::Matches);

	probe.set_link_connected(true);  // 間隔を変えても止まらない
	probe.set_link_connected(false);

	probe.stop();
	const int after_stop = world->probe_count->load();
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK(world->probe_count->load() == after_stop); // stop() の後は 1 回も問い合わせない

	probe.stop(); // 2 回呼んでもよい
}

TEST_CASE("OsProbe は、start() の前に stop() を呼んでもよく、stop() の後に start() で起動し直せる")
{
	const auto world = healthy_world();
	OsProbeConfig cfg = fake_config();
	cfg.disconnected_interval_ms = 1;
	cfg.connected_interval_ms = 1;

	OsProbe probe(cfg, fake_ops(world));
	probe.stop(); // start() の前の stop()

	probe.start();
	probe.start(); // 2 回呼んでもスレッドは 1 つ
	probe.stop();

	const int after_first = world->probe_count->load();
	CHECK(after_first >= 1);

	probe.start();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() <= after_first && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	CHECK(world->probe_count->load() > after_first);
	probe.stop();
}

TEST_CASE("接続している間の長い間隔で待っていても、切断を伝えるとすぐに次の問い合わせに進む")
{
	const auto world = healthy_world();
	OsProbeConfig cfg = fake_config();
	cfg.connected_interval_ms = 60000; // 接続している間は長い (既定は 5 分)
	cfg.disconnected_interval_ms = 1;  // 接続していない間は短い (既定は 30 秒)

	OsProbe probe(cfg, fake_ops(world));
	probe.set_link_connected(true);
	probe.start();

	// 最初の 1 回の後、接続している間は 60 秒後まで問い合わせない。
	const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() < 1 && std::chrono::steady_clock::now() < first_deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	REQUIRE(world->probe_count->load() == 1);

	// 切断を伝えると、短い間隔で決め直して、次の問い合わせが来る。
	probe.set_link_connected(false);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() < 2 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	CHECK(world->probe_count->load() >= 2);

	probe.stop();
}

TEST_CASE("request_probe_now() を呼ぶと、OsProbe は待ち時間を捨てて、すぐに次の問い合わせに進む")
{
	const auto world = healthy_world();
	OsProbeConfig cfg = fake_config();
	// どちらの間隔も長いので、request_probe_now() を呼ばなければ次の問い合わせは来ない (既定は 30 秒と 5 分)。
	cfg.connected_interval_ms = 60000;
	cfg.disconnected_interval_ms = 60000;

	OsProbe probe(cfg, fake_ops(world));
	probe.start();

	const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() < 1 && std::chrono::steady_clock::now() < first_deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	REQUIRE(world->probe_count->load() == 1);

	// 待っている間は次の問い合わせが来ない (これを確認しないと、下の 2 回目が間隔によるものかどうか分からない)。
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	REQUIRE(world->probe_count->load() == 1);

	// state.json を書き換えたドックが、ここで request_probe_now() を呼ぶ。
	probe.request_probe_now();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() < 2 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	CHECK(world->probe_count->load() >= 2);

	// 1 回呼んだら、問い合わせも 1 回だけ (フラグは使ったら false に戻し、その後はまた間隔どおりに待つ)。
	const int after_request = world->probe_count->load();
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK(world->probe_count->load() == after_request);

	probe.stop();
}

TEST_CASE("stop() の後に request_probe_now() を呼んでも、OsProbe は問い合わせない")
{
	const auto world = healthy_world();
	OsProbeConfig cfg = fake_config();
	cfg.connected_interval_ms = 60000;
	cfg.disconnected_interval_ms = 60000;

	OsProbe probe(cfg, fake_ops(world));
	probe.start();
	const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() < 1 && std::chrono::steady_clock::now() < first_deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	probe.stop();

	const int after_stop = world->probe_count->load();
	probe.request_probe_now(); // 止まっているときに呼んでもクラッシュせず、動き出さない
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK(world->probe_count->load() == after_stop);

	// 起動し直したとき、止まっている間に呼ばれた request_probe_now() で、問い合わせが 2 回にならない。
	probe.start();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() <= after_stop && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	const int after_restart = world->probe_count->load();
	CHECK(after_restart == after_stop + 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK(world->probe_count->load() == after_restart);

	probe.stop();
}

TEST_CASE("問い合わせの最中に request_probe_now() を何回呼んでも、その後の問い合わせは 1 回だけ足される")
{
	const auto world = healthy_world();
	OsProbeConfig cfg = fake_config();
	cfg.connected_interval_ms = 60000;
	cfg.disconnected_interval_ms = 60000;

	// 最初の問い合わせを、release が true になるまで止める。
	const auto entered = std::make_shared<std::atomic<int>>(0);
	const auto release = std::make_shared<std::atomic<bool>>(false);
	OsProbeOps ops = fake_ops(world);
	const auto inner = ops.find_vesktop_process;
	ops.find_vesktop_process = [inner, entered, release](VesktopProcessInfo &out) {
		if (entered->fetch_add(1) == 0) {
			const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!release->load() && std::chrono::steady_clock::now() < limit)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return inner(out);
	};

	OsProbe probe(cfg, ops);
	probe.start();
	const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (entered->load() < 1 && std::chrono::steady_clock::now() < enter_deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	REQUIRE(entered->load() == 1);

	probe.request_probe_now();
	probe.request_probe_now();
	probe.request_probe_now();
	release->store(true);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (world->probe_count->load() < 2 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	REQUIRE(world->probe_count->load() == 2);

	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK(world->probe_count->load() == 2);

	probe.stop();
}

// ---------------------------------------------------------------------------
// 空き容量
// ---------------------------------------------------------------------------

TEST_CASE("空き容量を調べる場所が空なら、probe_once は free_space_bytes を呼ばない")
{
	const auto world = healthy_world();
	OsProbeOps ops = fake_ops(world);
	int calls = 0;
	ops.free_space_bytes = [&calls](const std::string &) {
		calls++;
		return std::optional<std::uint64_t>(123);
	};

	OsProbeConfig cfg = fake_config(); // free_space_probe_path は空のまま
	const OsProbeResult result = probe_once(cfg, ops);

	CHECK(calls == 0);
	CHECK_FALSE(result.free_space_bytes.has_value());
}

TEST_CASE("空き容量を調べる場所があれば、probe_once はその場所の空き容量を結果に入れる")
{
	const auto world = healthy_world();
	OsProbeOps ops = fake_ops(world);
	std::string asked;
	ops.free_space_bytes = [&asked](const std::string &path) {
		asked = path;
		return std::optional<std::uint64_t>(123);
	};

	OsProbeConfig cfg = fake_config();
	cfg.free_space_probe_path = "D:/videos";
	const OsProbeResult result = probe_once(cfg, ops);

	CHECK(asked == "D:/videos");
	REQUIRE(result.free_space_bytes.has_value());
	CHECK(*result.free_space_bytes == 123u);
}

TEST_CASE("空き容量が読めないときと、free_space_bytes がないときは、値を持たない (0 にしない)")
{
	const auto world = healthy_world();
	OsProbeConfig cfg = fake_config();
	cfg.free_space_probe_path = "D:/videos";

	// 読めなかったとき (0 を、空き容量が尽きたと読ませない)。
	OsProbeOps unreadable = fake_ops(world);
	unreadable.free_space_bytes = [](const std::string &) { return std::optional<std::uint64_t>(); };
	CHECK_FALSE(probe_once(cfg, unreadable).free_space_bytes.has_value());

	// free_space_bytes がないとき (ほかのフィールドは入る)。
	const OsProbeResult without = probe_once(cfg, fake_ops(world));
	CHECK_FALSE(without.free_space_bytes.has_value());
	CHECK(without.vesktop_process);
}

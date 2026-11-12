// SPDX-License-Identifier: Apache-2.0

// OS への問い合わせ (Vesktop が動いているか、state.json の vencordDir、同梱の Vencord のファイルの照合、録画の保存先の
// 空き容量) を、1 つのスレッドで一定の間隔で行い、最後の結果を持つワーカー (OsProbe)。ファイルを読む処理で UI の
// スレッドを止めないため、ドックは latest() で結果を読むだけにする。結果はドックが ledger::Observation の Install に
// コピーする (dock/build_observation.cpp の install_of)。
// OS にアクセスする関数は OsProbeOps で受け取るので、このファイルに Windows の名前はなく、単体テストは実際の Vesktop
// にも利用者のプロファイルにも触らずに確認できる。

#pragma once

#include "platform/bundle_check.h"
#include "platform/platform.h"
#include "platform/sha256.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cordmux::platform {

// 1 回の問い合わせの結果。読むのはドック (周期ごと) である。
struct OsProbeResult {
	// 1 度でも問い合わせたか。false の間はどの値も分からないのであって、ないのではない (起動した直後の当然の状態を、
	// 異常として表示しないため)。
	bool probed = false;
	std::int64_t probed_at_us = 0; // 最後に問い合わせ終えた時刻 (エポックからのマイクロ秒)

	// Vesktop のプロセス。
	bool vesktop_process = false;
	std::int64_t vesktop_started_at_us = 0; // 0 は分からないことを示す

	// Vesktop の設定 (state.json)。
	bool state_json_read = false;              // state.json を読めたか (JSON として壊れていれば、読めていない)
	std::optional<std::string> vencord_dir;    // state.json の vencordDir (キーがなければ値を持たない)
	bool points_at_bundle = false;             // それが同梱の Vencord のフォルダー (data/vencord) か
	bool points_at_last_written = false;       // それが、cordmux が最後に書いた場所か
	BundleComparison bundle_hash;              // コピー元 (data/vencord-src) と data/vencord の 4 つのファイルの照合

	// バックアップ (vencord_dir_backup.json) から読んだ、cordmux が最後に Vesktop の設定を書いた時刻。表示の規則は、
	// これと vesktop_started_at_us を比べて、Vesktop の再起動が必要かを決める。
	std::optional<std::int64_t> state_json_written_at_us;

	// 録画の保存先の空き容量。値がないのは、まだ調べていないときである (0 を空き容量が尽きたと読まないこと)。
	// 調べる場所 (OsProbeConfig の free_space_probe_path_fn か free_space_probe_path) が空の間は、値を持たない。
	std::optional<std::uint64_t> free_space_bytes;
};

// 1 つのファイル全体を読むときの大きさの上限 (バイト)。state.json を読むコードは、どれもこの値を使う (このワーカーも、
// Vesktop の設定を書き換えるときのドックも)。値が違うと、その間の大きさの state.json で、ドックの表示では読めている
// のに、操作したときだけ読めない、という食い違いが起きる。4 つのファイルでいちばん大きい
// vencordDesktopRenderer.js でも数 MiB なので、それより桁違いに大きいファイルは読まずに、読めなかったものとする。
// 64 MiB という値は目安で、実際のファイルより十分に大きく、間違って巨大なファイルを読み込まない大きさにしてある。
inline constexpr std::size_t kDefaultMaxFileBytes = 64u * 1024u * 1024u;

// 問い合わせる場所と間隔。間隔は目安の値で、測って決めたものではないので、settings から変えられるようにしてある
// (plugin-main.cpp が、probe_interval_ms_from_seconds で settings の値を入れる)。
struct OsProbeConfig {
	// 同梱の Vencord のフォルダー (data/vencord) と、そのコピー元 (data/vencord-src)。どちらも cordmux のデータの
	// フォルダーの下にあるので、値を入れるのは呼び出し元 (plugin-main.cpp) である。
	std::string bundled_vencord_dir;
	std::string bundled_vencord_src_dir;
	// バックアップ (obs_module_config_path の下の vencord_dir_backup.json)。空なら読まない。
	std::string backup_json_path;

	// 空き容量を調べる場所。空なら調べない (既定は空で、録画の保存先が決まっているときだけ呼び出し元が入れる)。
	//
	// 空き容量の取得も OS への問い合わせなので、UI のスレッドではなく、このワーカーで行う。間隔はこのワーカーの間隔
	// (disconnected_interval_ms と connected_interval_ms) である。録画している間の空き容量の監視は、録画の側が自分の
	// スレッドで、settings の record_space_check_interval_s の間隔で行う。ここは、録画していない間も画面に空き容量を
	// 表示するためのものである。この値は OsProbe を作るときに決まる。
	std::string free_space_probe_path;
	// 空き容量を調べる場所を、調べるたびに返す関数。録画の保存先は、録画を始めるときと設定を変えたときに決め直すので、
	// OsProbe を作ったときの値のままでは古くなる。関数があれば free_space_probe_path より先に使い、空を返したらその場合は
	// 調べない。このワーカーのスレッドから呼ぶので、返す値はほかのスレッドから読める形にしておくこと
	// (plugin-main.cpp は Pipeline::record_base_dir を渡す)。
	std::function<std::string()> free_space_probe_path_fn;

	int disconnected_interval_ms = 30000; // Vesktop と接続していない間の間隔
	// 接続している間の間隔。接続している間も調べるのは、配信中に Vencord の更新でファイルが上書きされることがあるため
	// である。
	int connected_interval_ms = 300000;

	// 1 つのファイルを読むときの大きさの上限 (kDefaultMaxFileBytes)。
	std::size_t max_file_bytes = kDefaultMaxFileBytes;
};

// settings の秒 (config/settings.h の vencord_check_disconnected_s と vencord_check_connected_s) を、OsProbeConfig の間隔の
// ミリ秒にする。呼ぶのは plugin-main.cpp で、このヘッダーは config/ を読み込まない (platform/ は config/ に依存しない)。
// 0 以下なら fallback_ms を返す (settings は 1 以上しか受け入れないが、呼び出し元の誤りで間隔が 0 になり、休みなく
// 問い合わせることにならないようにする)。int に収まらない秒は、収まる最大の秒にする。
inline int probe_interval_ms_from_seconds(int seconds, int fallback_ms)
{
	if (seconds <= 0)
		return fallback_ms;
	// max を括弧で囲むのは、max のマクロを定義するヘッダー (windows.h など) の後に読み込まれても、コンパイルできるように
	// するため。
	constexpr int kMaxSeconds = (std::numeric_limits<int>::max)() / 1000;
	return (seconds > kMaxSeconds ? kMaxSeconds : seconds) * 1000;
}

// OsProbe が OS にアクセスする関数。テストでは差し替える。
struct OsProbeOps {
	std::function<bool(VesktopProcessInfo &)> find_vesktop_process;
	std::function<bool(std::string &)> vesktop_state_json_path;
	std::function<bool(const std::string &, std::string &, std::size_t)> read_file_bytes;
	// 現在の時刻 (エポックからのマイクロ秒)。テストが時刻を決められるように、関数で受け取る。
	std::function<std::int64_t()> now_us;
	// 空き容量。空でもよい (空なら空き容量を調べない)。
	std::function<std::optional<std::uint64_t>(const std::string &)> free_space_bytes;
};

// cordmux.dll が使う実装。platform/platform.h の関数をそのまま呼ぶ。
inline OsProbeOps make_real_os_probe_ops()
{
	OsProbeOps ops;
	ops.find_vesktop_process = [](VesktopProcessInfo &out) { return cordmux::platform::find_vesktop_process(out); };
	ops.vesktop_state_json_path = [](std::string &out) {
		return cordmux::platform::vesktop_state_json_path(out);
	};
	ops.read_file_bytes = [](const std::string &path, std::string &out, std::size_t max_bytes) {
		return cordmux::platform::read_file_bytes(path, out, max_bytes);
	};
	ops.now_us = [] {
		return std::int64_t(std::chrono::duration_cast<std::chrono::microseconds>(
					    std::chrono::system_clock::now().time_since_epoch())
					    .count());
	};
	ops.free_space_bytes = [](const std::string &path) { return cordmux::platform::free_space_bytes(path); };
	return ops;
}

// 1 回の問い合わせ。スレッドを持たないので、テストはこの関数を直接呼べる。
inline OsProbeResult probe_once(const OsProbeConfig &cfg, const OsProbeOps &ops)
{
	OsProbeResult out;
	out.probed = true;

	// Vesktop のプロセス。
	if (ops.find_vesktop_process) {
		VesktopProcessInfo info;
		if (ops.find_vesktop_process(info)) {
			out.vesktop_process = true;
			out.vesktop_started_at_us = info.started_at_us;
		}
	}

	const auto read_file = [&ops, &cfg](const std::string &path, std::string &text) {
		return ops.read_file_bytes && ops.read_file_bytes(path, text, cfg.max_file_bytes);
	};

	// state.json の vencordDir。
	std::string state_path;
	if (ops.vesktop_state_json_path && ops.vesktop_state_json_path(state_path)) {
		std::string text;
		// 読めても JSON として壊れていれば、読めていない扱いにする。「vencordDir がない」と同じにすると、
		// ドックが [Vesktopに追加] を出すが、書く側は壊れた state.json を書き換えない。
		if (read_file(state_path, text) && state_json_usable(text)) {
			out.state_json_read = true;
			out.vencord_dir = parse_vencord_dir(text);
		}
	}

	// バックアップ。読めた分だけ使う。
	std::optional<std::string> last_written;
	if (!cfg.backup_json_path.empty()) {
		std::string text;
		if (read_file(cfg.backup_json_path, text)) {
			if (const auto backup = parse_vencord_dir_backup(text)) {
				out.state_json_written_at_us = backup->written_at_us;
				last_written = backup->written;
			}
		}
	}

	if (out.vencord_dir) {
		out.points_at_bundle = !cfg.bundled_vencord_dir.empty() &&
					same_dir_path(*out.vencord_dir, cfg.bundled_vencord_dir);
		out.points_at_last_written = last_written.has_value() && same_dir_path(*out.vencord_dir, *last_written);
	}

	// 4 つのファイルのハッシュの照合。state.json がどこを指していても行う。Vencord の更新 (Force Update) が上書きする
	// のは data/vencord のファイルで、vencordDir が別の場所を指していても、data/vencord が上書きされていることは
	// 変わらないためである。
	const auto digests_of = [&](const std::string &dir) {
		std::vector<FileDigest> list;
		list.reserve(kVencordBundleFileCount);
		for (std::size_t i = 0; i < kVencordBundleFileCount; ++i) {
			FileDigest digest;
			digest.name = kVencordBundleFiles[i];
			if (!dir.empty()) {
				std::string text;
				if (read_file(join_path(dir, digest.name), text)) {
					digest.found = true;
					digest.sha256_hex = sha256_hex(text);
				}
			}
			list.push_back(std::move(digest));
		}
		return list;
	};
	out.bundle_hash = compare_bundle(digests_of(cfg.bundled_vencord_src_dir), digests_of(cfg.bundled_vencord_dir));

	// 録画の保存先の空き容量。場所が決まっていないときは調べない。
	const std::string space_path = cfg.free_space_probe_path_fn ? cfg.free_space_probe_path_fn()
								    : cfg.free_space_probe_path;
	if (!space_path.empty() && ops.free_space_bytes)
		out.free_space_bytes = ops.free_space_bytes(space_path);

	if (ops.now_us)
		out.probed_at_us = ops.now_us();
	return out;
}

// 1 つのスレッドで probe_once を一定の間隔で実行し、最後の結果を持つ。
//
// ロックは 2 つで、同時には持たない。result_mu_ は結果を守り、control_mu_ は止めるフラグと、接続しているかの
// フラグと、待ち合わせを守る。問い合わせ (ファイルの読み込み) は、どちらのロックも持たずに行う。UI のスレッドを
// 止めないためのワーカーなので、latest() を待たせるのは、結果をコピーする一瞬だけにする。
class OsProbe {
public:
	OsProbe(OsProbeConfig cfg, OsProbeOps ops) : cfg_(std::move(cfg)), ops_(std::move(ops)) {}

	~OsProbe() { stop(); }

	OsProbe(const OsProbe &) = delete;
	OsProbe &operator=(const OsProbe &) = delete;

	// スレッドを 1 つ起動する。2 回呼んでも、スレッドは 1 つのままである。起動したときの 1 回目の問い合わせは、
	// スレッドの中で行う (start() を呼んだ OBS の読み込みのスレッドを、ファイルの読み込みで待たせないため)。
	void start()
	{
		if (thread_.joinable())
			return;
		{
			std::lock_guard<std::mutex> lock(control_mu_);
			stop_requested_ = false;
			// 止まっている間に呼ばれた request_probe_now() は、起動した後の 1 回目の問い合わせで済ませる。
			probe_now_requested_ = false;
		}
		thread_ = std::thread([this] { loop(); });
	}

	// スレッドを止めて join する。何度呼んでもよい (stop() の後に start() で起動し直せる)。
	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(control_mu_);
			stop_requested_ = true;
		}
		control_cv_.notify_all();
		if (thread_.joinable())
			thread_.join();
	}

	// Vesktop と接続しているかで、問い合わせの間隔が変わる。ドックが、ポーリングのたびに Pipeline::snapshot() の接続の
	// 状態を渡す。値が変わったらすぐにスレッドを起こすので、接続している間の 5 分の間隔の途中で切れても、その場で
	// 30 秒の間隔に切り替わる。
	void set_link_connected(bool connected)
	{
		{
			std::lock_guard<std::mutex> lock(control_mu_);
			if (link_connected_ == connected)
				return;
			link_connected_ = connected;
		}
		control_cv_.notify_all();
	}

	// すぐにもう 1 回問い合わせる。待っている途中なら、残りの待ち時間を捨てて次の問い合わせに進む (問い合わせの最中
	// なら、それが終わった直後にもう 1 回問い合わせる)。
	//
	// ドックが Vesktop の設定を書き換えたときと、同梱の Vencord のファイルをコピーし直したとき
	// (dock/dock_poll_install.cpp)、Vesktop との接続が切れたとき (dock/dock_poll.cpp の tick) に呼ぶ。操作の結果は
	// 次の問い合わせで初めて分かるので、30 秒か 5 分の間隔を待つと、操作した利用者には何も起きなかったように見える。
	// 間隔は変えず、cordmux が OS の状態を変えたときと、接続が切れたときだけ、この関数でワーカーに 1 回問い合わせ
	// させる。
	void request_probe_now()
	{
		{
			std::lock_guard<std::mutex> lock(control_mu_);
			probe_now_requested_ = true;
		}
		control_cv_.notify_all();
	}

	// 現在の結果のコピー (まだ 1 度も問い合わせていなければ probed は false)。どのスレッドから呼んでもよい。
	OsProbeResult latest() const
	{
		std::lock_guard<std::mutex> lock(result_mu_);
		return latest_;
	}

private:
	void store(OsProbeResult result)
	{
		std::lock_guard<std::mutex> lock(result_mu_);
		latest_ = std::move(result);
	}

	// 次の問い合わせまで待つ。止めることになったら false を返す。待っている間に接続の状態が変わったら、待ち始めて
	// からの時間で決め直す (5 分の間隔で待っている途中で切れたとき、30 秒を過ぎていれば、すぐに次に進む)。
	//
	// request_probe_now() が呼ばれていたら、待たずに次に進む。問い合わせの最中に呼ばれたときも、ここで次に進む
	// (その問い合わせは、呼ばれる前の状態を見ているので、もう 1 回必要である)。
	bool wait_for_next()
	{
		const auto started = std::chrono::steady_clock::now();
		std::unique_lock<std::mutex> lock(control_mu_);
		for (;;) {
			if (stop_requested_)
				return false;
			if (probe_now_requested_) {
				probe_now_requested_ = false;
				return true;
			}
			const int interval_ms =
				link_connected_ ? cfg_.connected_interval_ms : cfg_.disconnected_interval_ms;
			const auto interval = std::chrono::milliseconds(interval_ms < 0 ? 0 : interval_ms);
			const auto elapsed = std::chrono::steady_clock::now() - started;
			if (elapsed >= interval)
				return true;
			control_cv_.wait_for(lock, interval - elapsed);
		}
	}

	void loop()
	{
		for (;;) {
			store(probe_once(cfg_, ops_));
			if (!wait_for_next())
				return;
		}
	}

	OsProbeConfig cfg_;
	OsProbeOps ops_;

	mutable std::mutex result_mu_;
	OsProbeResult latest_;

	std::mutex control_mu_;
	std::condition_variable control_cv_;
	bool stop_requested_ = false;
	bool link_connected_ = false;
	bool probe_now_requested_ = false;

	std::thread thread_;
};

} // namespace cordmux::platform

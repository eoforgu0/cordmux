// SPDX-License-Identifier: Apache-2.0

// 診断情報の manifest.json を作る関数。中身は、バージョンの一覧、Discord の内部への依存箇所の
// 結果、ダミーを表示していた時間帯、cordmux.dll だけで分かる値である。キーも値も英語で書く。manifest.json は画面に
// 表示するものではなく、問題を調べる人とツールが読むデータなので、ini の文字列を使わない (settings.json と同じ)。
// 読めなかった値はキーを省かず、分からないことが分かる形で書く (状態は "unknown"、時刻は 0)。
// libobs にも Qt にも依存しない。

#pragma once

// nlohmann/json は外部のライブラリなので、そのヘッダーが出す警告は表示しない。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "pipeline/dummy_interval.h"

namespace cordmux::bundle {

// 動いている部分のバージョン 1 つ。値が空なら分からないことを示す (空のまま書き、読む人が、読めていないと分かる
// ようにする)。
struct ManifestVersion {
	std::string name;
	std::string value;
};

// Discord の内部への依存箇所の 1 つの結果。依存箇所の一覧は src-vencord/ledger.ts にだけあり、C++ には持たないので、
// ここに入るのは、送信側が使えないと伝えてきた id と、そう伝えられた後に使えるようになった id だけである。
struct ManifestContact {
	std::string id;
	std::string state;                // "unavailable" (いま使えない) か "ok" (使えるようになった)
	std::int64_t last_ok_at_us = 0;   // 最後に使えた時刻 (0 は、OBS を起動してから 1 度も使えていないことを示す)
};

// cordmux.dll だけで分かる値。送信側から何も届かなくても入るので、Vesktop と接続できないときの診断に使える。
struct ManifestSelfFacts {
	bool listener_attempted = false;      // 待ち受けを開こうとしたか
	std::string listener_outcome;         // "opened"、"blocked_by_other_writer"、"ports_exhausted" など
	// 待ち受けを始めたときに起きた問題 ("none"、"session_file_not_saved"、"accept_failed"。ws::OpenProblem の名前)。
	// session_file_not_saved のとき、outcome は "opened" で listening も true のままなので、このフィールドがないと
	// manifest.json からは待ち受け中としか読めず、Vesktop が接続できないことが zip のどこにも残らないことがある (OBS の
	// ログは zip に入らず、plugin.log はリングバッファーで、起動した直後の行から先に押し出される)。
	std::string listener_open_problem;
	int listening_port = 0;               // 開いたポート番号 (診断情報では伏せない。RevealFn のコメント)
	bool listening = false;

	std::int64_t connection_opened_at_us = 0; // 最後に接続を受け付けた時刻
	std::int64_t hello_ok_at_us = 0;          // 最後に hello が通った時刻 (トークンが一致した時刻)
	std::int64_t connection_closed_at_us = 0; // 最後に接続が切れた時刻
	std::string last_disconnect_reason;       // ws::DisconnectReason の名前
	std::uint64_t connections_accepted = 0;
	std::uint64_t secret_mismatches = 0;      // トークンが一致せずに切断した回数
	std::uint64_t handshake_rejections = 0;
	std::uint64_t heartbeat_timeouts = 0;
	std::uint64_t hello_timeouts = 0;
	std::uint64_t rejected_clients = 0;       // 2 つ目以降の接続を断った回数

	std::int64_t state_json_written_at_us = 0; // cordmux が Vesktop の設定 (state.json) を書いた時刻
	std::string bundle_hash_state;             // 同梱の Vencord の 4 つのファイルの照合 (matches、mismatch、unknown)
	std::int64_t bundle_hash_checked_at_us = 0;
	std::vector<std::string> bundle_hash_missing;
	std::vector<std::string> bundle_hash_differing;

	bool vesktop_running = false;
	std::int64_t vesktop_started_at_us = 0;

	// 上限を超えて捨てた行の数 (0 でなければ、ログが途中から始まる)。plugin_log と discord_log は出来事の行
	// (plugin.log と discord.log)、*_stats_log は統計の行 (plugin-stats.log と discord-stats.log) の数である。
	std::uint64_t plugin_log_dropped = 0;
	std::uint64_t plugin_stats_log_dropped = 0;
	std::uint64_t discord_log_dropped = 0;
	std::uint64_t discord_stats_log_dropped = 0;
	std::uint64_t exceptions_caught = 0;
	int facts_unreadable_keys = 0;        // 送信側と受信側のバージョンの食い違いを疑う手がかり
	int avatar_decode_failures = 0;
	int hw_fallback_streams = 0;          // ソフトウェアデコードに切り替えた画面のストリームの数 (設定のデコーダーが動作していない数)
};

// manifest.json に書く値。
struct ManifestInput {
	std::int64_t taken_at_us = 0; // 書き出した時刻 (エポックからのマイクロ秒)
	int utc_offset_sec = 0;       // そのときの時差 (秒。読む人が現地の時刻に修正できるように)

	std::vector<ManifestVersion> versions;

	std::vector<ManifestContact> contacts;
	std::vector<pipeline::DummyInterval> dummy_intervals;
	ManifestSelfFacts facts;
};

// input を 1 つの JSON の文字列にする。時計もファイルも読まないので、同じ入力には必ず同じ文字列を返す。
inline std::string build_manifest_json(const ManifestInput &input)
{
	nlohmann::json root = nlohmann::json::object();
	root["schema"] = "cordmux-materials/1";
	root["taken_at_us"] = input.taken_at_us;
	root["utc_offset_sec"] = input.utc_offset_sec;

	nlohmann::json versions = nlohmann::json::object();
	for (const ManifestVersion &version : input.versions)
		versions[version.name] = version.value;
	root["versions"] = std::move(versions);

	nlohmann::json contacts = nlohmann::json::array();
	for (const ManifestContact &contact : input.contacts) {
		nlohmann::json row = nlohmann::json::object();
		row["id"] = contact.id;
		row["state"] = contact.state;
		row["last_ok_at_us"] = contact.last_ok_at_us;
		contacts.push_back(std::move(row));
	}
	root["contacts"] = std::move(contacts);

	nlohmann::json intervals = nlohmann::json::array();
	for (const pipeline::DummyInterval &interval : input.dummy_intervals) {
		nlohmann::json row = nlohmann::json::object();
		row["started_at_us"] = interval.started_at_us;
		// まだ続いている時間帯は終わりの時刻を持たない (0 を、1970 年に終わったと読ませない)。
		if (interval.open())
			row["open"] = true;
		else
			row["ended_at_us"] = interval.ended_at_us;
		intervals.push_back(std::move(row));
	}
	root["dummy_intervals"] = std::move(intervals);

	const ManifestSelfFacts &facts = input.facts;
	nlohmann::json self = nlohmann::json::object();
	self["listener_attempted"] = facts.listener_attempted;
	self["listener_outcome"] = facts.listener_outcome;
	self["listener_open_problem"] = facts.listener_open_problem;
	self["listening"] = facts.listening;
	self["listening_port"] = facts.listening_port;
	self["connection_opened_at_us"] = facts.connection_opened_at_us;
	self["hello_ok_at_us"] = facts.hello_ok_at_us;
	self["connection_closed_at_us"] = facts.connection_closed_at_us;
	self["last_disconnect_reason"] = facts.last_disconnect_reason;
	self["connections_accepted"] = facts.connections_accepted;
	self["secret_mismatches"] = facts.secret_mismatches;
	self["handshake_rejections"] = facts.handshake_rejections;
	self["heartbeat_timeouts"] = facts.heartbeat_timeouts;
	self["hello_timeouts"] = facts.hello_timeouts;
	self["rejected_clients"] = facts.rejected_clients;
	self["state_json_written_at_us"] = facts.state_json_written_at_us;
	self["bundle_hash_state"] = facts.bundle_hash_state;
	self["bundle_hash_checked_at_us"] = facts.bundle_hash_checked_at_us;
	self["bundle_hash_missing"] = facts.bundle_hash_missing;
	self["bundle_hash_differing"] = facts.bundle_hash_differing;
	self["vesktop_running"] = facts.vesktop_running;
	self["vesktop_started_at_us"] = facts.vesktop_started_at_us;
	self["plugin_log_dropped"] = facts.plugin_log_dropped;
	self["plugin_stats_log_dropped"] = facts.plugin_stats_log_dropped;
	self["discord_log_dropped"] = facts.discord_log_dropped;
	self["discord_stats_log_dropped"] = facts.discord_stats_log_dropped;
	self["exceptions_caught"] = facts.exceptions_caught;
	self["facts_unreadable_keys"] = facts.facts_unreadable_keys;
	self["avatar_decode_failures"] = facts.avatar_decode_failures;
	self["hw_fallback_streams"] = facts.hw_fallback_streams;
	root["cordmux_only_facts"] = std::move(self);

	return root.dump(2) + "\n";
}

} // namespace cordmux::bundle

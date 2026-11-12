// SPDX-License-Identifier: Apache-2.0

// 状態タブの判断。前の周期の状態を持つ部分 (映像途切れの累計、履歴、最後の録画) を、前の状態と現在の状態を比べる純粋関数にする。
// 時計は読まず、時刻は呼ぶ側が渡す。状態は DockPoll が値で持つ (ledger/notice_tracker.h の NoticeTrackerState と同じ形)。
// libobs にも Qt にも依存しない。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ledger/dock_view.h"
#include "ledger/observation.h"
#include "ledger/view.h"

namespace cordmux::ledger {

// ---- 映像途切れの累計 ----

// 参加者の行に「映像が届いていません」(映像途切れ) が出ている人。出ているかは、待ちを通した後の行 (PersonRow::status) を
// 見る。行が使う判定 (rules_dock.cpp の row_status_of) とは別の判定を作らない。自分の枠の映像が見つからない行も
// RowState::NoVideo だが、テキストが違う (映像途切れではない) ので数えない。
struct StalledPerson {
	std::string user_id;
	Label name;
};

// 待ちを通した後の View の行のうち、映像途切れが出ている人。
std::vector<StalledPerson> stalled_people(const dockview::DockViewV2 &stable);

// 1 人の累計。途切れが「出ていない -> 出ている」に変わるたびに count が 1 増え、出ている間の時間が total_us に足される。
struct StallEntry {
	std::string user_id;
	Label name;
	std::uint64_t count = 0;
	std::int64_t total_us = 0;            // 終わった途切れの時間の合計
	std::optional<std::int64_t> since_us; // 続いている途切れの始まり
	// リセットの時点で途切れていた。その途切れが終わるまで、数えも足しもしない (リセットの直後に「1 回」と出さない)。
	bool ignoring = false;
};

struct StallTally {
	std::vector<StallEntry> entries;
};

inline bool operator==(const StallEntry &a, const StallEntry &b)
{
	return a.user_id == b.user_id && a.name == b.name && a.count == b.count && a.total_us == b.total_us &&
	       a.since_us == b.since_us && a.ignoring == b.ignoring;
}

inline bool operator==(const StallTally &a, const StallTally &b)
{
	return a.entries == b.entries;
}

// 累計をゼロに戻す。途切れが続いている人は ignoring にして残し、ほかの人の行は消す。
StallTally reset_stall_tally(const StallTally &prev);

// 累計を 1 周期進める。stalled はこの周期に映像途切れが出ている人。reset が真なら、先に reset_stall_tally() で戻してから
// 進める (配信の開始)。累計が 0 で途切れも続いていない人の行は消す。
StallTally step_stall_tally(const StallTally &prev, const std::vector<StalledPerson> &stalled, std::int64_t now_us,
			    bool reset);

// 表に出す合計 (us)。続いている途切れは now_us までを含める。
std::int64_t stall_total_us(const StallEntry &entry, std::int64_t now_us);

// ---- 履歴 ----

// 履歴の出来事を見つけるために比べる、ある周期の状態。
struct HistoryNotice {
	std::string key;
	TextRef headline;
};

struct HistorySlot {
	std::string user_id;
	Label name;
};

// 受信をやり直した回数 (PersonFact::receive_restarts) が 1 以上の人。
struct HistoryRestart {
	std::string user_id;
	Label name;
	std::uint64_t count = 0;
};

struct HistorySnapshot {
	std::vector<HistoryNotice> notices; // 待ちを通した後のメッセージ (全レベル)
	LinkState link = LinkState::Unknown;
	bool recording = false;
	std::vector<HistorySlot> slots; // 画面共有の枠に載っている人
	std::vector<HistoryRestart> restarts; // 受信をやり直したことのある人 (一覧の行がある人だけ)
};

struct HistoryEvent {
	TextRef text;
	std::optional<TextRef> subject; // 表示する側が解決して text の %1 に入れる (dockview::StatusHistoryLine)
};

HistorySnapshot history_snapshot_of(const dockview::DockViewV2 &stable, const Observation &obs);

// 前の周期 (prev。最初の周期は持たない) と現在の周期 (cur) を比べて、出来事を古い順に返す。
//   - メッセージの発生と解消 (全レベル。待ちを通した後なので、途切れの出入りのような短い変化は入らない)。最初の周期は、
//     そこにあるメッセージを発生として返す。
//   - Vesktop との接続と切断。
//   - 枠に載せた人と外した人。割り当ての変化だけを返す (利用者の操作と自動で載せる動きのほか、画面共有をやめた人や
//     通話を抜けた人の枠が空いたときも、外した人として返す)。
//   - 録画の開始と終了。
//   - 映像が届かなくなって、受信をやり直した人 (受信をやり直した回数が前の周期より増えた人)。
// 最初の周期は、メッセージ以外を返さない (起動した時点の状態は、出来事ではない)。
std::vector<HistoryEvent> history_events(const std::optional<HistorySnapshot> &prev, const HistorySnapshot &cur);

// ---- 最後の録画 ----

// この起動の中だけ覚える録画。ファイルを読み直さない。
struct RecordingMemory {
	bool active = false;
	std::int64_t start_us = 0;
	std::string folder; // 録画中のセッションのフォルダー (録画中は周期ごとに更新する)
	bool has_last = false;
	std::int64_t last_start_us = 0;
	std::int64_t last_duration_us = 0;
	std::string last_folder;
};

inline bool operator==(const RecordingMemory &a, const RecordingMemory &b)
{
	return a.active == b.active && a.start_us == b.start_us && a.folder == b.folder && a.has_last == b.has_last &&
	       a.last_start_us == b.last_start_us && a.last_duration_us == b.last_duration_us &&
	       a.last_folder == b.last_folder;
}

RecordingMemory step_recording_memory(const RecordingMemory &prev, const Observation &obs, std::int64_t now_us);

// 録画の節の View。時刻の引数は when_arg_of() で作る。
dockview::StatusRecordingView recording_section_of(const RecordingMemory &memory, std::int64_t now_us,
						     int utc_offset_sec);

// 今日の時刻なら時刻だけ、今日でない日の時刻なら日付も付けた時刻の引数 (dockview::time_arg と date_time_arg)。
std::string when_arg_of(std::int64_t epoch_us, std::int64_t now_us, int utc_offset_sec);

// ---- 状態タブ全体 ----

// 表の直近 1 分の列 (遅延、最大、行のツールチップ) を書き換える間隔。周期ごとに書き換えると数字がちらついて読めないので、OBS の
// 統計ウィンドウと同じ 2 秒にする。途切れの回数と合計は出来事の数なので、周期ごとに書き換える。
constexpr std::int64_t kStatusReceiveRefreshUs = 2'000'000;

// [映像のバッファ] の値を配信の処理へ渡すまでに、入力が止まっている時間 (ms)。渡すたびに映像と画面共有の音声が一時的に止まったり
// 飛んだりするので、つまみを動かしている間は渡さず、表の計算だけを先に行う。
constexpr int kStatusBufferIdleMs = 300;

struct StatusState {
	StallTally tally;
	std::optional<HistorySnapshot> prev;
	std::vector<dockview::StatusHistoryLine> history; // 新しいものが先頭
	RecordingMemory recording;
	bool streaming = false; // 前の周期の配信中
	// 表に出している直近 1 分の列と、最後に書き換えた時刻。
	std::optional<std::int64_t> receive_shown_at_us;
	std::map<std::string, std::vector<pipeline::ArrivalWindow>> receive_shown;
};

struct StatusInput {
	std::int64_t now_us = 0;
	int utc_offset_sec = 0;
	bool streaming = false; // OBS が配信中か
};

struct StatusResult {
	StatusState next;
	dockview::StatusTabView view;
};

// 待ちを通した後の View (stable。status_tab には decide_dock() の値が入っている) と Observation から、状態タブの View と
// 次の状態を決める。配信が始まった周期 (前の周期は配信中でなく、現在の周期が配信中) に、累計をゼロに戻す。
StatusResult settle_status(const StatusState &prev, const dockview::DockViewV2 &stable, const Observation &obs,
			   const StatusInput &input);

// [途切れの累計をリセット] を押したとき。
StatusState reset_status(const StatusState &prev);

} // namespace cordmux::ledger

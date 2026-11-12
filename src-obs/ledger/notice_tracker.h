// SPDX-License-Identifier: Apache-2.0

// メッセージを表示するまでの待ちと消すまでの待ちを通し、表示した順に並べる (stabilize)。
// decide_dock() (ledger/rules.h) は前の周期の状態を持たないので、「2 秒続いたら表示する」「1 秒消えていたら消す」
// 「表示した順に並べる」を決められない。そこで、前の状態と現在の時刻を引数に取る純粋関数をここに置き、状態は
// ドック (dock/dock_poll.h の DockPoll) が持つ。時計は読まない。libobs にも Qt にも依存しない。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ledger/dock_view.h"

namespace cordmux::ledger {

// 表示するまでの待ちと消すまでの待ち (マイクロ秒)。設定の notice_show_delay_ms と notice_clear_delay_ms から来る。
struct NoticeTrackerConfig {
	std::int64_t show_delay_us = 2'000'000;
	std::int64_t clear_delay_us = 1'000'000;
};

// 表示した、または消した 1 件 (メッセージ 1 つか、人の行の状態 1 つ)。ドックはこれを OBS のログの 1 行と、
// スクリーンリーダーへの通知に使う。
struct NoticeEvent {
	std::string key;         // Notice::key か "row:<userId>:<RowState の名前>"
	std::string rule_name;   // 行の状態は "RowStatus.<RowState の名前>"
	// ログに書く対象。枠に載っていれば "slot <n>"、ほかは空。ログはほかの人に渡されることがあるので、
	// userId と名前は書かない。
	std::string log_subject;
	dockview::Tier tier = dockview::Tier::L3;
	dockview::LogLevel log_level = dockview::LogLevel::Info;
	TextRef headline;
	bool is_row = false; // 人の行の状態 (スクリーンリーダーへは通知せず、ログにだけ書く)
	std::int64_t at_us = 0;
};

inline bool operator==(const NoticeEvent &a, const NoticeEvent &b)
{
	return a.key == b.key && a.rule_name == b.rule_name && a.log_subject == b.log_subject && a.tier == b.tier &&
	       a.log_level == b.log_level && a.headline == b.headline && a.is_row == b.is_row && a.at_us == b.at_us;
}

inline bool operator!=(const NoticeEvent &a, const NoticeEvent &b)
{
	return !(a == b);
}

struct NoticeDiff {
	std::vector<NoticeEvent> appeared;
	std::vector<NoticeEvent> disappeared;
	std::optional<bool> list_full_changed_to; // 枠がすべて使用中になった (true)、空いた (false)
};

inline bool operator==(const NoticeDiff &a, const NoticeDiff &b)
{
	return a.appeared == b.appeared && a.disappeared == b.disappeared &&
	       a.list_full_changed_to == b.list_full_changed_to;
}

inline bool operator!=(const NoticeDiff &a, const NoticeDiff &b)
{
	return !(a == b);
}

// メッセージか人の行の状態 1 つについて、周期をまたいで覚えておく値。
struct TrackedItem {
	std::string key;                      // Notice::key か "row:<userId>:<RowState の名前>"
	std::int64_t raw_since_us = 0;        // decide_dock() の結果に続けて含まれ始めた時刻 (0 = 今は含まれていない)
	std::int64_t raw_absent_since_us = 0; // decide_dock() の結果から続けて消え始めた時刻 (0 = 今は含まれている)
	bool shown = false;                   // 今、画面に表示している
	std::int64_t appeared_at_us = 0;      // 画面に表示した時刻 (並べる順に使う)

	// 消すまでの待ちの間、decide_dock() の結果から消えた後も同じテキストを表示し続けるためのコピー。
	// 伝えなら last_notice、人の行の状態なら last_row_status に入る。
	std::optional<dockview::Notice> last_notice;
	std::optional<dockview::RowStatus> last_row_status;
	std::string last_log_subject; // 人の行の状態のログの対象 (消したときにも同じ対象を書く)
};

inline bool operator==(const TrackedItem &a, const TrackedItem &b)
{
	return a.key == b.key && a.raw_since_us == b.raw_since_us && a.raw_absent_since_us == b.raw_absent_since_us &&
	       a.shown == b.shown && a.appeared_at_us == b.appeared_at_us && a.last_notice == b.last_notice &&
	       a.last_row_status == b.last_row_status && a.last_log_subject == b.last_log_subject;
}

inline bool operator!=(const TrackedItem &a, const TrackedItem &b)
{
	return !(a == b);
}

// DockPoll が値で持つ状態。既定値は、何も表示していない状態である。
struct NoticeTrackerState {
	std::vector<TrackedItem> items;
	// 前の周期の「枠がすべて使用中」。持たない = まだ 1 周期も見ていない。最初の周期は変わったとみなさない。
	// 起動した時点で枠が埋まっていても、利用者の目の前で埋まったのではないので、「空いている枠がありません」を
	// スクリーンリーダーに通知しない。
	std::optional<bool> list_full;
};

inline bool operator==(const NoticeTrackerState &a, const NoticeTrackerState &b)
{
	return a.items == b.items && a.list_full == b.list_full;
}

inline bool operator!=(const NoticeTrackerState &a, const NoticeTrackerState &b)
{
	return !(a == b);
}

struct StabilizeResult {
	dockview::DockViewV2 view; // 待ちを通し、表示した順に並べた View
	NoticeTrackerState next;
	NoticeDiff diff;
};

// decide_dock() の結果 raw に待ちを通し、表示した順に並べる。prev は前の周期の next (最初の周期は既定値)。
// now_us はシステム時刻のマイクロ秒。
//   - Settle::Debounced のメッセージと人の行の状態は、raw に show_delay_us 続けて含まれてから表示し、raw から
//     clear_delay_us 続けて消えてから消す。
//   - Settle::ClearOnly は表示する前に待たない (規則のしきい値そのものが待ちになっている。映像が止まったとみなす
//     しきい値など)。消すときだけ clear_delay_us 待つ。
//   - Settle::Immediate は待たない。
//   - 表示した時刻 (appeared_at_us) は前の状態から引き継ぎ、同じメッセージのレベルの中を古い順に並べる (新しいものが下)。
//   - 1 行目は、待ちを通した後のメッセージから first_line_of() で決め直す。メッセージがなければ raw.fallback_line。
//   - diff には、この周期に表示したものと消したもの、枠がすべて使用中になったか空いたかが入る。
StabilizeResult stabilize(const NoticeTrackerState &prev, const dockview::DockViewV2 &raw, std::int64_t now_us,
			  const NoticeTrackerConfig &cfg);

// ---- 1 行目の中身と既読 ----

// 既読にしたレベル 2 のメッセージ 1 件。absent_since_us を持つ間は、メッセージが消えている。
struct ReadNotice {
	std::string key;
	std::optional<std::int64_t> absent_since_us;
	int absent_stream = 0; // 消えたときの配信の番号 (NoticeReadState::stream_count)
	int stage = 0;         // 読んだときの段階 (dockview::Notice::read_stage)
};

inline bool operator==(const ReadNotice &a, const ReadNotice &b)
{
	return a.key == b.key && a.absent_since_us == b.absent_since_us && a.absent_stream == b.absent_stream &&
	       a.stage == b.stage;
}

inline bool operator!=(const ReadNotice &a, const ReadNotice &b)
{
	return !(a == b);
}

// DockPoll が値で持つ状態。保存しない (OBS を起動するたびに、すべて未読から始める)。
struct NoticeReadState {
	std::vector<ReadNotice> read;
	bool streaming = false;
	int stream_count = 0; // 配信を始めるたびに増やす番号
};

inline bool operator==(const NoticeReadState &a, const NoticeReadState &b)
{
	return a.read == b.read && a.streaming == b.streaming && a.stream_count == b.stream_count;
}

inline bool operator!=(const NoticeReadState &a, const NoticeReadState &b)
{
	return !(a == b);
}

struct FirstLineInput {
	std::int64_t now_us = 0;
	bool streaming = false; // 配信中か
	// 状態タブを開いているか。真の周期に表示しているレベル 2 を既読にする。開いている間に新しく出たレベル 2 は、同じ画面の
	// 一覧で読めるので、1 行目で繰り返さない。
	bool status_tab_open = false;
	// 配信の外で、消えてからこの時間がたって出たメッセージを未読に戻す。
	std::int64_t read_forget_us = 60'000'000;
};

struct FirstLineResult {
	dockview::FirstLine first_line;
	int status_count = 0;      // 状態タブの件数 (レベル 1 と 2 の合計)
	TextRef status_tab_label;  // 状態タブの名前。件数が 0 のときは件数を付けない
	NoticeReadState next;
};

// 1 行目の中身を決める。view は stabilize() を通した View である。優先順は、レベル 1 > 無効 > 未読のレベル 2 > 全画面で
// 表示中 > ダミー映像を表示中 > 状態の要約。無効と、全画面から下の順は DockViewV2::fallback_line と read_line が持つ。
// 既読の規則:
//   - 状態タブを開くと、その時点のレベル 2 は既読になり、1 行目から消えて件数だけに残る。レベル 1 は既読にならない。
//     ただし、Notice::read_on_status_tab が真のレベル 1 (録画の停止と録画の開始の失敗) は、レベル 2 と同じに既読になる。
//   - 既読のメッセージが消えて、別の配信で再び出たら未読に戻す。同じ配信の中で再び出たときは既読のままにする (回線の弱い
//     人の出入りのたびに 1 行目が変わらないようにする)。
//   - 配信の外では、消えて read_forget_us 以上たってから出たら未読に戻す。
//   - 消えていない間は、配信をまたいでも既読のままにする。
FirstLineResult settle_first_line(const NoticeReadState &prev, const dockview::DockViewV2 &view,
				  const FirstLineInput &input);

// 人の行の状態の英語の名前。NoticeEvent::rule_name の "RowStatus.<名前>" の後ろに入り、ログに書く。
const char *row_state_log_name(dockview::RowState state);

// メッセージの並びと、メッセージがないときの 1 行目 (fallback) から、ドックの 1 行目を決める。
// 1 行目に表示するのはメッセージのレベル 1 と 2 の見出しだけである。メッセージのレベル 1 があればその中で並びの最も前の
// もの、なければメッセージのレベル 2 の中で並びの最も前のものを選ぶ。メッセージのレベル 1 が 2 件以上なら、残りの件数
// (cordmux.dock.more_notices.status) を添える。stabilize() と decide_dock() (待ちを通す前の 1 行目) が使う。
// メッセージのレベル 3 は 1 行目に表示しない。メッセージのレベル 3 は警告ではないので、表示すると「作成しています...」などの
// 行に平常の要約が押しのけられ、問題のない日の 1 行目が読めなくなる。
dockview::FirstLine first_line_of(const std::vector<dockview::Notice> &notices, const dockview::FirstLine &fallback);

// メッセージの並びの中で、プライマリのボタン (ViewButton::primary) を 1 つだけにする。ドックにはメッセージが何枚も並ぶので、
// 次に押すボタンを 1 つだけ目立たせる。メッセージのレベル 1、2 の順に、並びの前にあるメッセージから順に見て、最初の押せる
// ボタン 1 つの primary を true にし、ほかのボタンは false にする。メッセージのレベル 3 は何かをしてもらう行ではないので、
// そのボタンはプライマリにしない。押せないボタンもプライマリにしない。stabilize() の最後と decide_dock() の最後の
// 2 か所で呼ぶ (アンインストールの結果のダイアログは、stabilize() を通さない decide_dock() の View を使う)。
// メッセージを作る関数ごとにプライマリを決めないのは、ドックにメッセージが何枚並ぶかを、それぞれの関数が知らないためである。
void mark_primary_button(std::vector<dockview::Notice> *notices);

} // namespace cordmux::ledger

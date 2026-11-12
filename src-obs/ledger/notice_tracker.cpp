// SPDX-License-Identifier: Apache-2.0

// notice_tracker.h の実装。追うものは、メッセージ (キーは Notice::key) と人の行の状態 (キーは row_key()) の 2 種類である。

#include "ledger/notice_tracker.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ledger/plural.h"
#include "ledger/text_ids.h"

namespace cordmux::ledger {

namespace {

using dockview::DockViewV2;
using dockview::FirstLine;
using dockview::FirstLineKind;
using dockview::Notice;
using dockview::PersonRow;
using dockview::RowStatus;
using dockview::Settle;
using dockview::Tier;

int tier_rank(Tier tier)
{
	switch (tier) {
	case Tier::L1:
		return 0;
	case Tier::L2:
		return 1;
	case Tier::L3:
		break;
	}
	return 2;
}

// 人の行の状態のキー "row:<userId>:<RowState の名前>" を返す。状態ごとに別のものとして追うので、
// 「映像を待っています...」から「映像が届いていません」に変わったら、前の状態が消えて新しい状態を表示したことになる
// (ログの行も 2 つ書く)。キーを "row:<userId>" の 1 つにして同じものとして追うと、消すまでの待ちの間に前の状態の
// テキストを表示し続けるか、新しい状態の表示するまでの待ちを飛ばすかのどちらかになり、ログにも状態の変わり目が
// 残らない。
std::string row_key(const std::string &user_id, dockview::RowState state)
{
	return "row:" + user_id + ":" + row_state_log_name(state);
}

std::string row_prefix(const std::string &user_id)
{
	return "row:" + user_id + ":";
}

const TrackedItem *find_item(const NoticeTrackerState &state, const std::string &key)
{
	for (const TrackedItem &item : state.items) {
		if (item.key == key)
			return &item;
	}
	return nullptr;
}

Settle settle_of(const TrackedItem &item)
{
	if (item.last_notice.has_value())
		return item.last_notice->settle;
	if (item.last_row_status.has_value())
		return item.last_row_status->settle;
	return Settle::Immediate;
}

// decide_dock() の結果に含まれているものが、表示するまでの待ちを満たしたか。
bool ready_to_show(Settle settle, std::int64_t raw_since_us, std::int64_t now_us, const NoticeTrackerConfig &cfg)
{
	if (settle != Settle::Debounced)
		return true; // Immediate は待たない。ClearOnly は規則のしきい値そのものが表示するまでの待ちになっている
	return now_us - raw_since_us >= cfg.show_delay_us;
}

// decide_dock() の結果から消えたものが、消すまでの待ちを満たしたか。
bool ready_to_clear(Settle settle, std::int64_t absent_since_us, std::int64_t now_us, const NoticeTrackerConfig &cfg)
{
	if (settle == Settle::Immediate)
		return true;
	return now_us - absent_since_us >= cfg.clear_delay_us;
}

NoticeEvent event_of_notice(const Notice &n, std::int64_t at_us)
{
	NoticeEvent e;
	e.key = n.key;
	e.rule_name = n.rule_name;
	e.log_subject = n.log_subject;
	e.tier = n.tier;
	e.log_level = n.log_level;
	e.headline = n.headline;
	e.is_row = false;
	e.at_us = at_us;
	return e;
}

NoticeEvent event_of_row(const std::string &key, const RowStatus &status, const std::string &log_subject,
			 std::int64_t at_us)
{
	NoticeEvent e;
	e.key = key;
	e.rule_name = std::string("RowStatus.") + row_state_log_name(status.state);
	e.log_subject = log_subject;
	e.tier = Tier::L3;
	e.log_level = dockview::LogLevel::Info;
	e.headline = status.text;
	e.is_row = true;
	e.at_us = at_us;
	return e;
}

// 状態タブを開いたら既読にするメッセージか。メッセージのレベル 2 と、既読にしてよいと印を付けたメッセージのレベル 1。
bool readable_on_status_tab(const dockview::Notice &n)
{
	return n.tier == Tier::L2 || (n.tier == Tier::L1 && n.read_on_status_tab);
}

std::string slot_subject(const PersonRow &row)
{
	return row.slot.has_value() ? "slot " + std::to_string(*row.slot) : std::string();
}

} // namespace

const char *row_state_log_name(dockview::RowState state)
{
	switch (state) {
	case dockview::RowState::NotSharing:
		return "NotSharing";
	case dockview::RowState::Waiting:
		return "Waiting";
	case dockview::RowState::NoVideo:
		return "NoVideo";
	}
	return "Unknown";
}

dockview::FirstLine first_line_of(const std::vector<dockview::Notice> &notices, const dockview::FirstLine &fallback)
{
	// 並びがメッセージのレベルの順でなくても、メッセージのレベルで選ぶ。stabilize() はメッセージのレベルの順に並べてから
	// 渡すが、decide_dock() は規則の表の順のまま渡す (primary の表の行が notices の表の行より先に並ぶ)。
	const dockview::Notice *first_l1 = nullptr;
	const dockview::Notice *first_l2 = nullptr;
	int l1_count = 0;
	for (const dockview::Notice &n : notices) {
		if (n.tier == dockview::Tier::L1) {
			++l1_count;
			if (first_l1 == nullptr)
				first_l1 = &n;
		} else if (n.tier == dockview::Tier::L2 && first_l2 == nullptr) {
			first_l2 = &n;
		}
	}
	// 優先順は、レベル 1 > 無効 > 未読のレベル 2 > 全画面 > ダミー > 要約。無効の行はレベル 2 の見出しより優先する
	// (「cordmuxは無効です」を幅によらず必ず出す)。全画面とダミーと要約は fallback に入っているので、メッセージがなければ
	// fallback を返す。
	if (first_l1 == nullptr && fallback.disabled)
		return fallback;
	const dockview::Notice *top = first_l1 != nullptr ? first_l1 : first_l2;
	if (top == nullptr)
		return fallback;
	dockview::FirstLine line;
	line.kind = dockview::FirstLineKind::Notice;
	line.tier = top->tier;
	line.forms = {top->headline};
	if (top->headline_short.has_value())
		line.forms.push_back(*top->headline_short);
	line.notice_key = top->key;
	// メッセージのレベル 1 が 2 件以上なら、残りの件数を添える。メッセージのレベル 2 の見出しを選んだときは、
	// メッセージのレベル 1 がない。
	if (top->tier == dockview::Tier::L1 && l1_count > 1) {
		TextRef more;
		more.id = text::keys::kDockMoreNoticesStatus;
		more.args = {std::to_string(l1_count - 1)};
		line.more_count = more;
	}
	return line;
}

void mark_primary_button(std::vector<dockview::Notice> *notices)
{
	for (dockview::Notice &n : *notices) {
		for (dockview::ViewButton &b : n.buttons)
			b.primary = false;
	}
	for (const dockview::Tier tier : {dockview::Tier::L1, dockview::Tier::L2}) {
		for (dockview::Notice &n : *notices) {
			if (n.tier != tier)
				continue;
			for (dockview::ViewButton &b : n.buttons) {
				if (!b.disabled_reason.has_value()) {
					b.primary = true;
					return;
				}
			}
		}
	}
}

StabilizeResult stabilize(const NoticeTrackerState &prev, const dockview::DockViewV2 &raw, std::int64_t now_us,
			  const NoticeTrackerConfig &cfg)
{
	StabilizeResult result;
	result.view = raw;
	NoticeTrackerState &next = result.next;

	// ---- メッセージ ----
	std::set<std::string> raw_notice_keys;
	std::vector<std::pair<dockview::Notice, std::size_t>> shown; // 表示しているメッセージと、raw の中の位置
	for (std::size_t i = 0; i < raw.notices.size(); ++i) {
		const dockview::Notice &n = raw.notices[i];
		if (!raw_notice_keys.insert(n.key).second)
			continue; // 同じキーが 2 つあれば先の 1 つだけを使う (規則の誤りで画面をちらつかせない)
		const TrackedItem *before = find_item(prev, n.key);
		TrackedItem item;
		item.key = n.key;
		item.raw_since_us = before != nullptr && before->raw_since_us != 0 ? before->raw_since_us : now_us;
		item.raw_absent_since_us = 0;
		item.shown = before != nullptr && before->shown;
		item.appeared_at_us = before != nullptr ? before->appeared_at_us : 0;
		item.last_notice = n;
		if (!item.shown && ready_to_show(n.settle, item.raw_since_us, now_us, cfg)) {
			item.shown = true;
			item.appeared_at_us = now_us;
			result.diff.appeared.push_back(event_of_notice(n, now_us));
		}
		if (item.shown) {
			dockview::Notice out = n;
			out.appeared_at_us = item.appeared_at_us;
			item.last_notice = out;
			shown.emplace_back(std::move(out), i);
		}
		next.items.push_back(std::move(item));
	}
	// 前の周期に表示していて、現在の decide_dock() の結果にないメッセージ (消すまでの待ち)。
	for (const TrackedItem &before : prev.items) {
		if (!before.last_notice.has_value() || raw_notice_keys.count(before.key) != 0)
			continue;
		if (!before.shown)
			continue; // 表示する前に消えたものは何も残さない
		TrackedItem item = before;
		item.raw_since_us = 0;
		if (item.raw_absent_since_us == 0)
			item.raw_absent_since_us = now_us;
		if (ready_to_clear(settle_of(item), item.raw_absent_since_us, now_us, cfg)) {
			result.diff.disappeared.push_back(event_of_notice(*before.last_notice, now_us));
			continue;
		}
		shown.emplace_back(*item.last_notice, raw.notices.size() + next.items.size());
		next.items.push_back(std::move(item));
	}
	// メッセージのレベルの順に並べ、同じメッセージのレベルの中は表示した順にする (新しいものが下)。同じ時刻に表示したものは
	// decide_dock() の結果の並び (規則の表の順) にする。
	std::stable_sort(shown.begin(), shown.end(), [](const auto &a, const auto &b) {
		const int ra = tier_rank(a.first.tier);
		const int rb = tier_rank(b.first.tier);
		if (ra != rb)
			return ra < rb;
		if (a.first.appeared_at_us != b.first.appeared_at_us)
			return a.first.appeared_at_us < b.first.appeared_at_us;
		return a.second < b.second;
	});
	result.view.notices.clear();
	for (auto &entry : shown)
		result.view.notices.push_back(std::move(entry.first));
	// プライマリのボタンは、今表示しているメッセージの並びで決め直す (待ちの間のメッセージは画面にないため)。
	mark_primary_button(&result.view.notices);

	// ---- 人の行の状態 ----
	std::set<std::string> raw_row_keys;
	std::set<std::string> rows_present;
	for (dockview::PersonRow &row : result.view.people) {
		rows_present.insert(row.user_id);
		const std::string prefix = row_prefix(row.user_id);
		std::optional<dockview::RowStatus> current_shown;
		if (row.status.has_value()) {
			const dockview::RowStatus status = *row.status;
			const std::string key = row_key(row.user_id, status.state);
			raw_row_keys.insert(key);
			const TrackedItem *before = find_item(prev, key);
			TrackedItem item;
			item.key = key;
			item.raw_since_us = before != nullptr && before->raw_since_us != 0 ? before->raw_since_us : now_us;
			item.shown = before != nullptr && before->shown;
			item.appeared_at_us = before != nullptr ? before->appeared_at_us : 0;
			item.last_row_status = status;
			item.last_log_subject = slot_subject(row);
			if (!item.shown && ready_to_show(status.settle, item.raw_since_us, now_us, cfg)) {
				item.shown = true;
				item.appeared_at_us = now_us;
				result.diff.appeared.push_back(event_of_row(key, status, item.last_log_subject, now_us));
			}
			if (item.shown)
				current_shown = status;
			next.items.push_back(std::move(item));
		}
		// 消すまでの待ちの間の状態 (decide_dock() の結果から消えた、同じ人の前の状態)。現在の状態を表示していれば
		// 現在の状態を表示し (現在の事実を先に伝える)、表示していなければ、こちらを表示し続ける。
		std::optional<dockview::RowStatus> lingering;
		std::int64_t lingering_at = -1;
		for (const TrackedItem &before : prev.items) {
			if (!before.last_row_status.has_value() || before.key.compare(0, prefix.size(), prefix) != 0)
				continue;
			if (raw_row_keys.count(before.key) != 0 || !before.shown)
				continue;
			TrackedItem item = before;
			item.raw_since_us = 0;
			if (item.raw_absent_since_us == 0)
				item.raw_absent_since_us = now_us;
			if (ready_to_clear(settle_of(item), item.raw_absent_since_us, now_us, cfg)) {
				result.diff.disappeared.push_back(
					event_of_row(before.key, *before.last_row_status, before.last_log_subject, now_us));
				continue;
			}
			if (item.appeared_at_us > lingering_at) {
				lingering = item.last_row_status;
				lingering_at = item.appeared_at_us;
			}
			next.items.push_back(std::move(item));
		}
		row.status = current_shown.has_value() ? current_shown : lingering;
	}
	// 行ごといなくなった人の状態は、待たずに消す (状態を表示する行がない)。
	for (const TrackedItem &before : prev.items) {
		if (!before.last_row_status.has_value() || !before.shown)
			continue;
		const std::string rest = before.key.substr(4); // "row:" の後ろ
		const std::size_t colon = rest.rfind(':');
		const std::string user_id = colon == std::string::npos ? rest : rest.substr(0, colon);
		if (rows_present.count(user_id) != 0)
			continue;
		result.diff.disappeared.push_back(
			event_of_row(before.key, *before.last_row_status, before.last_log_subject, now_us));
	}

	// ---- 枠がすべて使用中 ----
	// アンインストールを始めた後は一覧を表示しない (EmptyKind::Uninstalled) ので、枠の空き具合の変わり目も
	// 通知せず、前の値を保つ。アンインストールの途中で枠を削除すると空きができるが、表示していない一覧について
	// 「空きました」と伝えないためである。decide_dock() で list_full を false に固定しないのは、枠が埋まって
	// いたときに、アンインストールを始めた瞬間に「空いた」と 1 回通知してしまうためである。
	const bool list_hidden = raw.empty.has_value() && raw.empty->kind == dockview::EmptyKind::Uninstalled;
	if (list_hidden) {
		next.list_full = prev.list_full;
	} else {
		next.list_full = raw.list_full;
		if (prev.list_full.has_value() && *prev.list_full != raw.list_full)
			result.diff.list_full_changed_to = raw.list_full;
	}

	// ---- 参加者タブの行にも、待ちを通した後の行の状態を入れる ----
	const auto settled_status_of = [&result](dockview::PersonRow &row) {
		if (row.off)
			return;
		for (const dockview::PersonRow &settled : result.view.people) {
			if (settled.user_id == row.user_id) {
				row.status = settled.status;
				return;
			}
		}
	};
	if (result.view.people_list.pinned.has_value())
		settled_status_of(*result.view.people_list.pinned);
	for (dockview::PersonRow &row : result.view.people_list.sharing)
		settled_status_of(row);
	for (dockview::PersonRow &row : result.view.people_list.in_call)
		settled_status_of(row);

	// ---- 1 行目は、待ちを通した後のメッセージから決め直す ----
	result.view.first_line = first_line_of(result.view.notices, raw.fallback_line);
	return result;
}

FirstLineResult settle_first_line(const NoticeReadState &prev, const dockview::DockViewV2 &view,
				  const FirstLineInput &input)
{
	FirstLineResult result;
	NoticeReadState &next = result.next;
	next.streaming = input.streaming;
	next.stream_count = prev.stream_count + (input.streaming && !prev.streaming ? 1 : 0);

	std::set<std::string> shown;
	for (const dockview::Notice &n : view.notices) {
		shown.insert(n.key);
		if (n.tier == Tier::L1 || n.tier == Tier::L2)
			++result.status_count;
	}

	std::set<std::string> read_keys;
	for (const ReadNotice &before : prev.read) {
		ReadNotice item = before;
		if (item.absent_since_us.has_value()) {
			if (item.absent_stream != next.stream_count)
				continue; // 消えた後に別の配信が始まった
			if (!input.streaming && input.now_us - *item.absent_since_us >= input.read_forget_us)
				continue; // 配信の外で、消えてから時間がたった
		}
		if (shown.count(item.key) != 0) {
			item.absent_since_us.reset();
		} else if (!item.absent_since_us.has_value()) {
			item.absent_since_us = input.now_us;
			item.absent_stream = next.stream_count;
		}
		read_keys.insert(item.key);
		next.read.push_back(std::move(item));
	}

	// 読んだときより段階が進んだメッセージは、未読に戻す (同じキーのまま、読んだ記録を捨てる)。
	for (const dockview::Notice &n : view.notices) {
		if (!readable_on_status_tab(n))
			continue;
		const auto at = std::find_if(next.read.begin(), next.read.end(), [&n](const ReadNotice &item) {
			return item.key == n.key && n.read_stage > item.stage;
		});
		if (at != next.read.end()) {
			read_keys.erase(n.key);
			next.read.erase(at);
		}
	}

	std::vector<dockview::Notice> unread;
	bool any_read_shown = false;
	for (const dockview::Notice &n : view.notices) {
		if (!readable_on_status_tab(n)) {
			unread.push_back(n);
			continue;
		}
		// 無効の間は既読にしない (有効に戻したときに、未読のレベル 2 を 1 行目に出すため)。
		if (input.status_tab_open && !view.fallback_line.disabled && read_keys.insert(n.key).second) {
			ReadNotice item;
			item.key = n.key;
			item.stage = n.read_stage;
			next.read.push_back(std::move(item));
		}
		if (read_keys.count(n.key) != 0)
			any_read_shown = true;
		else
			unread.push_back(n);
	}

	result.first_line = first_line_of(unread, any_read_shown ? view.read_line : view.fallback_line);
	if (result.status_count > 0) {
		result.status_tab_label.id = text::keys::kDockTabStatusCountLabel;
		result.status_tab_label.args = {std::to_string(result.status_count)};
	} else {
		result.status_tab_label.id = text::keys::kDockTabStatusLabel;
	}
	return result;
}

} // namespace cordmux::ledger

// SPDX-License-Identifier: Apache-2.0

// 参加者の一覧の行の幅と省略の計算 (name_check.h) を実装する。

#include "dock/name_check.h"

#include <algorithm>
#include <numeric>

#include "ledger/text_ids.h"

namespace cordmux::dock::rows {

namespace {

bool starts_with(const std::u16string &text, const std::u16string &prefix)
{
	return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin());
}

int part_width(const PartIn &in, const PartOut &out)
{
	return out.use_short && in.short_px >= 0 ? in.short_px : in.full_px;
}

// 表示する部分の幅の合計 (部分の間の空きを含む)。
int total_width(const std::vector<PartIn> &parts, const std::vector<PartOut> &outs, int gap_px)
{
	int total = 0;
	int count = 0;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (!outs[i].shown)
			continue;
		total += part_width(parts[i], outs[i]);
		++count;
	}
	if (count > 1)
		total += gap_px * (count - 1);
	return total;
}

} // namespace

std::vector<bool> middle_elide_rows(const std::vector<NameShown> &rows)
{
	std::vector<bool> middle(rows.size(), false);
	for (std::size_t i = 0; i < rows.size(); ++i) {
		if (!rows[i].elided)
			continue;
		for (std::size_t j = 0; j < rows.size(); ++j) {
			if (j == i)
				continue;
			// ほかの人の名前の先頭に一致する (見えているテキストが同じになる場合を含む)。
			if (starts_with(rows[j].full, rows[i].visible_prefix)) {
				middle[i] = true;
				break;
			}
		}
	}
	return middle;
}

PartKind part_kind_of(const std::string &key)
{
	namespace k = ledger::text::keys;
	if (key == k::kDockRowVoiceStatus || key == k::kDockRowVoiceAbsentStatus)
		return PartKind::Voice;
	if (key == k::kDockRowDiscordNameStatus)
		return PartKind::DiscordName;
	if (key == k::kDockRowUsernameStatus)
		return PartKind::Username;
	return PartKind::Other;
}

std::vector<PartOut> fit_line2(const std::vector<PartIn> &parts, int available_px, int gap_px)
{
	std::vector<PartOut> outs(parts.size());
	for (std::size_t i = 0; i < parts.size(); ++i)
		outs[i].width_px = parts[i].full_px;
	if (parts.empty() || total_width(parts, outs, gap_px) <= available_px)
		return outs;

	// 2. ほかの部分があるときは、Discord での名前の部分を落とす。
	const bool has_other = std::any_of(parts.begin(), parts.end(),
					   [](const PartIn &p) { return p.kind != PartKind::DiscordName; });
	if (has_other) {
		for (std::size_t i = 0; i < parts.size(); ++i) {
			if (parts[i].kind == PartKind::DiscordName) {
				outs[i].shown = false;
				outs[i].width_px = 0;
			}
		}
		if (total_width(parts, outs, gap_px) <= available_px)
			return outs;
	}

	// 3. ユーザー名を値だけの短い形にする。
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (outs[i].shown && parts[i].kind == PartKind::Username && parts[i].short_px >= 0) {
			outs[i].use_short = true;
			outs[i].width_px = parts[i].short_px;
		}
	}
	if (total_width(parts, outs, gap_px) <= available_px)
		return outs;

	// 4. 残った部分で幅を分ける。等しい水位で分け、テキストの短い部分は全文のまま残し、余りを長い部分で等しく分ける
	//    (幅の比で分けると、短い「声: bob」まで省かれる)。全部の部分の head_px が入る幅なら、どの部分も head_px を
	//    下回らせない (水位だけで分けると、少し長い「声: carol」が head_px より狭くなり、「声: ...」だけが残る)。
	//    声を常に全文にすると、声の名前が長いときにユーザー名の先頭と末尾が消え、同じ名前の 2 人を見分けられなくなる。
	//    head_px を字数で決めないのは、文字の幅がフォントと言語で違い、測った px でないと「声:」だけが残る形を防げない
	//    ためである。
	std::vector<std::size_t> shown;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (outs[i].shown)
			shown.push_back(i);
	}
	if (shown.empty())
		return outs;
	int remaining = available_px - gap_px * static_cast<int>(shown.size() - 1);
	if (remaining < 0)
		remaining = 0;
	int floors_sum = 0;
	int max_natural = 0;
	for (std::size_t i : shown) {
		const int natural = part_width(parts[i], outs[i]);
		floors_sum += std::min(natural, std::max(0, parts[i].head_px));
		max_natural = std::max(max_natural, natural);
	}
	const bool keep_heads = floors_sum <= remaining;
	if (!keep_heads && shown.size() > 1) {
		// head_px も全部は入らない幅では、ユーザー名があれば、1 つだけを残してほかの部分を丸ごと落とす。
		// 「声: ...」と「a...1」のように、誰の声かも誰の行かも読めない形を作らない。声があれば声を残す。声は利用者が
		// 自分で選んだ設定で、行の使う声の表示は、どの声を使っているかを知る手掛かりがほかにない。ユーザー名は、
		// 1 行目に名前があるので、同じ名前の人を見分けるための補いである。声がなければ、ユーザー名を残す。
		const auto find_kind = [&](PartKind kind) {
			return std::find_if(shown.begin(), shown.end(), [&](std::size_t i) { return parts[i].kind == kind; });
		};
		const auto voice = find_kind(PartKind::Voice);
		const auto user = find_kind(PartKind::Username);
		if (user != shown.end()) {
			const std::size_t keep = voice != shown.end() ? *voice : *user;
			for (std::size_t i : shown) {
				if (i == keep)
					continue;
				outs[i].shown = false;
				outs[i].width_px = 0;
				outs[i].elide = ElideMode::None;
			}
			const int natural = part_width(parts[keep], outs[keep]);
			const int room = std::max(0, available_px);
			outs[keep].width_px = std::min(natural, room);
			outs[keep].elide = outs[keep].width_px < natural
						   ? (parts[keep].kind == PartKind::Username ? ElideMode::Middle : ElideMode::End)
						   : ElideMode::None;
			return outs;
		}
	}
	auto width_at = [&](std::size_t i, int level) {
		const int natural = part_width(parts[i], outs[i]);
		const int floor = keep_heads ? std::min(natural, std::max(0, parts[i].head_px)) : 0;
		return std::max(floor, std::min(natural, level));
	};
	auto sum_at = [&](int level) {
		int sum = 0;
		for (std::size_t i : shown)
			sum += width_at(i, level);
		return sum;
	};
	// 合計が remaining を超えない最も高い水位 (水位 0 の合計は head_px の合計か 0 で、どちらも remaining 以下)。
	int lo = 0;
	int hi = max_natural;
	while (lo < hi) {
		const int mid = lo + (hi - lo + 1) / 2;
		if (sum_at(mid) <= remaining)
			lo = mid;
		else
			hi = mid - 1;
	}
	int spare = remaining - sum_at(lo);
	for (std::size_t i : shown)
		outs[i].width_px = width_at(i, lo);
	// 割り切れない余りの px は、まだ全文に届かない部分に、後ろから 1 px ずつ与える。
	for (auto it = shown.rbegin(); it != shown.rend() && spare > 0; ++it) {
		if (outs[*it].width_px < part_width(parts[*it], outs[*it])) {
			++outs[*it].width_px;
			--spare;
		}
	}
	for (std::size_t i : shown) {
		if (outs[i].width_px < part_width(parts[i], outs[i]))
			outs[i].elide = parts[i].kind == PartKind::Username ? ElideMode::Middle : ElideMode::End;
	}
	return outs;
}

WidthRules width_rules(int row_px, int status_text_min_px, int resolution_min_px, int avatar_min_px)
{
	WidthRules rules;
	rules.status_text = row_px >= status_text_min_px;
	rules.resolution = row_px >= resolution_min_px;
	rules.avatar = row_px >= avatar_min_px;
	return rules;
}

int name_floor_with_avatar(int name_floor_px)
{
	return name_floor_px - name_floor_px / 4;
}

int avatar_min_width(int status_text_min_px, int avatar_px, int gap_px, int name_floor_px)
{
	return status_text_min_px + avatar_px + gap_px - (name_floor_px - name_floor_with_avatar(name_floor_px));
}

int status_text_min_width(int min_row_px, int status_keep_px, int large_px)
{
	return min_row_px + std::max(0, status_keep_px) + std::max(0, large_px);
}

NameStatusSplit split_name_status(int text_px, int name_natural_px, int status_natural_px, int name_floor_px,
				  int status_keep_px)
{
	NameStatusSplit split;
	const int room = std::max(0, text_px);
	if (status_natural_px <= 0) {
		split.name_px = room;
		return split;
	}
	if (name_natural_px + status_natural_px <= room) {
		split.status_px = status_natural_px;
		split.name_px = room - status_natural_px;
		return split;
	}
	// 入らないときは、状態のテキストを下限まで先に縮める。名前は全文の幅を上限、name_floor_px を下限にする。
	const int keep = std::min(status_natural_px, std::max(0, status_keep_px));
	const int name = std::min(std::max(0, name_natural_px), std::max(name_floor_px, room - keep));
	split.name_px = name;
	split.status_px = std::max(0, room - name);
	return split;
}

const char *status_short_key_for(const std::string &status_key)
{
	namespace k = ledger::text::keys;
	if (status_key == k::kDockRowWaitingStatus)
		return k::kDockRowWaitingShortStatus;
	if (status_key == k::kDockRowFpsReducedStatus)
		return k::kDockRowFpsReducedShortStatus;
	if (status_key == k::kDockRowResolutionReducedStatus)
		return k::kDockRowResolutionReducedShortStatus;
	if (status_key == k::kDockRowBothReducedStatus)
		return k::kDockRowBothReducedShortStatus;
	if (status_key == k::kDockRowQualityReducedStatus)
		return k::kDockRowQualityReducedShortStatus;
	if (status_key == k::kDockRowNotReceivingStatus)
		return k::kDockRowNotReceivingShortStatus;
	if (status_key == k::kDockRowNotSharingStatus)
		return k::kDockRowNotSharingShortStatus;
	if (status_key == k::kDockRowCantShowStatus)
		return k::kDockRowCantShowShortStatus;
	if (status_key == k::kDockRowSelfVideoMissingStatus)
		return k::kDockRowSelfVideoMissingShortStatus;
	return nullptr;
}

bool press_blocked(std::int64_t changed_at_ms, std::int64_t now_ms, int interval_ms)
{
	if (changed_at_ms <= 0 || interval_ms <= 0)
		return false;
	return now_ms - changed_at_ms < interval_ms;
}

bool dots_circle_clear(int check_right, int dots_left, int dots_width)
{
	// 円の左端は [...] の中心から 12 px 左。チェックの範囲 [.., check_right) と交わらないには、円の左端が
	// check_right 以上であればよい。
	const double circle_left = dots_left + dots_width / 2.0 - 12.0;
	return circle_left >= static_cast<double>(check_right);
}

int min_row_width(int indicator_and_gap_px, int name_floor_px, int mark_px, int dots_px, int small_px, int large_px,
		  int dots_gap_px)
{
	return indicator_and_gap_px + name_floor_px + large_px + mark_px + small_px + mark_px + dots_gap_px + dots_px;
}

int audio_buttons_px(int button_px, int small_px)
{
	return 2 * (button_px + small_px);
}

namespace {

// フォーカスリング (空き + 線) と、その外に残す背景の 1 px の合計。
int ring_clear_px(int ring_px, int ring_gap_px)
{
	return std::max(0, ring_px) + std::max(0, ring_gap_px) + 1;
}

} // namespace

int dots_gap_px(int large_px, int ring_px, int ring_gap_px)
{
	return std::max(large_px, ring_clear_px(ring_px, ring_gap_px));
}

int row_gap_px(int small_px, int ring_px, int ring_gap_px)
{
	return std::max(small_px, ring_clear_px(ring_px, ring_gap_px));
}

} // namespace cordmux::dock::rows

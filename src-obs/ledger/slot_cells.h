// SPDX-License-Identifier: Apache-2.0

// 枠に載っている人をキャンバスの 4 等分のどの位置に置くかを決める純粋関数。配信の配置 (sources/composite_layout.h) と
// ドックの行の枠の位置 (ledger/rules_dock.cpp) が同じこの関数を呼ぶので、行に表示する位置と配信の位置は必ず一致する。
// 位置の番号は 0 が左上、1 が右上、2 が左下、3 が右下である。libobs にも Qt にも依存しない。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config/allocation.h"
#include "ledger/natural_order.h"

namespace cordmux::ledger {

// 4 等分の位置の数 (左上、右上、左下、右下)。
inline constexpr int kSlotCellCount = 4;

// 合わせて 1 つのときに全面に置くか。設定の項目はない。
inline constexpr bool kFullWhenSingle = true;

// 映っている画面共有 1 つ分。
struct SlotCellShare {
	int slot = 0;                 // 枠の番号 (1 から 4)
	std::string name;             // 表示名
	std::string user_id;          // 同じ名前のときの並びの決め手
	std::int64_t added_order = 0; // 載せた順の番号 (config::SlotAssignment::added_order)
	bool is_main = false;         // メインアカウントの画面共有か (名前順と載せた順で、先頭に置く)
};

// 自分の枠の置き方 (自分の枠を出しているか、設定の self_slot_first、ほかの人と同じ並び順で置くときの決め手)。
struct SlotCellSelf {
	bool shown = false; // 自分の枠を出しているか
	// 並び順によらず先頭 (左上) に置くか (設定の self_slot_first)。メインアカウントの画面共有 (SlotCellShare::is_main) を
	// 先頭に置くかも、この値で決める。偽なら、自分の枠もメインアカウントの画面共有も、ほかの人と同じ並び順で置く。ダミー映像の
	// 間は、呼び出し元が設定によらず真を渡す (dock/dock_poll_layout.cpp。ダミーの枠の名前と番号は仮のもので、比べる意味がない)。
	bool first = true;
	std::string name;    // 名前順で比べる名前 (自分の枠の名前ラベルに出す名前)
	std::string user_id; // 同じ名前のときの決め手
	// 載せた順で比べる印。自分の枠を載せた時に、すでに載っていた人の added_order の最大 (0 は、誰よりも先に載せた)。
	// 自分の枠は、この番号までの人の後で、それより後に載せた人の前に置く (自分の枠は割り当て表の外にあるので、
	// added_order を持たない)。
	std::int64_t added_mark = 0;
};

// 位置に置くもの 1 つ。
struct SlotCell {
	bool self = false; // 自分の枠か
	int share = -1;    // 入力の shares の添字。自分の枠は -1
	int cell = 0;      // 位置の番号。全面のときは 0
};

struct SlotCellPlan {
	bool full = false;           // 全面に置く (cells は 1 つ)
	std::vector<SlotCell> cells; // 位置の番号の順
	std::vector<int> dropped;    // 置く場所が足りずに描かない画面共有 (入力の shares の添字)
	bool self_dropped = false;   // 置く場所が足りずに、自分の枠を描かない (先頭に置かないときだけ起こる)
};

// 表示名の順の比べ方。表示名の自然順で、同じ名前なら user_id、それも同じなら枠の番号。実行のたびに並びが変わらない
// ようにする。
inline bool slot_name_less(const std::string &a_name, const std::string &a_user, int a_slot, const std::string &b_name,
			   const std::string &b_user, int b_slot)
{
	if (a_name != b_name)
		return natural_less(a_name, b_name);
	if (a_user != b_user)
		return a_user < b_user;
	return a_slot < b_slot;
}

// 位置を決める。規則:
//   - 自分の枠を先頭に置くとき (self.first。既定): 自分の枠は、どの並び順でも左上。メインアカウントの画面共有 (is_main) は、
//     並び順によらず先頭 (自分の枠があればその次) に置く。参加者の一覧のメインアカウントの行が先頭に固定されるのと、順序を
//     そろえる。
//   - 先頭に置かないとき: 自分の枠とメインアカウントの画面共有も、ほかの人と同じ並び順で置く。名前順は、自分の枠を
//     self.name で比べる。載せた順は、自分の枠を self.added_mark の番号の人の後、次の番号の人の前に置く。
//   - 表示名の順と載せた順は、その順に、残りの位置へ左上から詰める。置ける数を超えた後ろの人は dropped に入れる (先頭に
//     置かないときは、自分の枠が後ろになって落ちることがある。そのときは self_dropped を真にする)。
//   - 合わせて 1 つなら、どの並び順でも全面 (full_when_single が偽なら、その人の位置)。
inline SlotCellPlan plan_slot_cells(const std::vector<SlotCellShare> &shares, const SlotCellSelf &self, config::SlotOrder order,
				    bool full_when_single)
{
	SlotCellPlan plan;
	// 並べるもの。shares の添字で、自分の枠は kSelfEntry。
	constexpr int kSelfEntry = -1;
	std::vector<int> entries;
	entries.reserve(shares.size() + 1);
	for (std::size_t i = 0; i < shares.size(); ++i)
		entries.push_back(static_cast<int>(i));
	const bool self_in_order = self.shown && !self.first;
	if (self_in_order)
		entries.push_back(kSelfEntry);

	// 名前順の比べ方の値 (自分の枠は self の名前と、枠の番号 0)。
	struct NameKey {
		const std::string *name;
		const std::string *user_id;
		int slot;
	};
	const auto name_key = [&shares, &self](int entry) {
		if (entry == kSelfEntry)
			return NameKey{&self.name, &self.user_id, 0};
		const SlotCellShare &x = shares[static_cast<std::size_t>(entry)];
		return NameKey{&x.name, &x.user_id, x.slot};
	};
	const auto by_name = [&name_key](int a, int b) {
		const NameKey x = name_key(a);
		const NameKey y = name_key(b);
		return slot_name_less(*x.name, *x.user_id, x.slot, *y.name, *y.user_id, y.slot);
	};
	// 載せた順の比べ方の値。画面共有は added_order の 2 倍、自分の枠は added_mark の 2 倍に 1 を足した値にして、自分の枠を
	// added_mark の番号の人の後、次の番号の人の前に置く (同じ値にならない)。
	const auto added_key = [&shares, &self](int entry) {
		if (entry == kSelfEntry)
			return self.added_mark * 2 + 1;
		return shares[static_cast<std::size_t>(entry)].added_order * 2;
	};

	if (self.shown && self.first) {
		SlotCell cell;
		cell.self = true;
		plan.cells.push_back(cell);
	}

	if (order == config::SlotOrder::Added) {
		std::sort(entries.begin(), entries.end(), [&added_key, &by_name](int a, int b) {
			const std::int64_t x = added_key(a);
			const std::int64_t y = added_key(b);
			return x != y ? x < y : by_name(a, b);
		});
	} else {
		std::sort(entries.begin(), entries.end(), by_name);
	}
	if (self.first) {
		std::stable_partition(entries.begin(), entries.end(), [&shares](int entry) {
			return entry != kSelfEntry && shares[static_cast<std::size_t>(entry)].is_main;
		});
	}
	int next_cell = static_cast<int>(plan.cells.size());
	for (const int entry : entries) {
		if (next_cell >= kSlotCellCount) {
			if (entry == kSelfEntry)
				plan.self_dropped = true;
			else
				plan.dropped.push_back(entry);
			continue;
		}
		SlotCell placed;
		placed.self = entry == kSelfEntry;
		placed.share = entry;
		placed.cell = next_cell++;
		plan.cells.push_back(placed);
	}

	if (plan.cells.size() == 1 && full_when_single) {
		plan.full = true;
		plan.cells[0].cell = 0;
	}
	return plan;
}

// 自分の枠を先頭に置くとき (設定の self_slot_first の既定) の形。self_shown は自分の枠を出しているか。
inline SlotCellPlan plan_slot_cells(const std::vector<SlotCellShare> &shares, bool self_shown, config::SlotOrder order,
				    bool full_when_single)
{
	SlotCellSelf self;
	self.shown = self_shown;
	return plan_slot_cells(shares, self, order, full_when_single);
}

} // namespace cordmux::ledger

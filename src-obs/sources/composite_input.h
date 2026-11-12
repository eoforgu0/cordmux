// SPDX-License-Identifier: Apache-2.0

// 合成のソースに渡す配置の入力 (見えている画面共有の枠、自分の枠を出しているか、すき間、画面共有していない参加者、
// 全画面で映す人) を作る純粋関数と、全画面にできる人を決める純粋関数。ドックの周期 (dock/dock_poll_layout.cpp) が作って
// sources/composite_source.h の set_composite_input に渡し、描画のスレッドが読む。キャンバスの大きさだけは描画のときに
// OBS から読むので、ここには含めない。libobs にも Qt にも依存しない。

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sources/composite_layout.h"
#include "sources/composite_roster.h"

namespace cordmux::sources {

// 配置の入力。plan_composite_layout に渡す CompositeLayoutRequest から、キャンバスの大きさを除いたものと、話していない人を
// 暗くするかの判断に使う値と、画面共有していない参加者。
struct CompositeInput {
	bool self_shown = false;
	std::vector<CompositeShare> shares;
	bool full_when_single = true;
	int gap_px = 0;
	// 並び順 (設定の slot_order。CompositeLayoutRequest の同じ名前のフィールド)。
	config::SlotOrder slot_order = config::SlotOrder::Name;
	// 自分の枠を先頭に置くか (設定の self_slot_first) と、載せた順の印 (CompositeLayoutRequest の同じ名前のフィールド)。
	bool self_first = true;
	std::int64_t self_added_mark = 0;
	// cordmux が自分の枠の人の声を聞けるか (CompositeShare::hears_voice と同じ意味)。ドックは、自分の声の経路があるとき
	// (サブアカウントの構成で、メインアカウントの声を受け取っているとき) だけ真にする。メインアカウントだけの構成では
	// Discord が本人の声を返さないので偽 (光らせず、暗くもしない)。既定は偽。設定で自分の枠の音声ソースを選んでいる間は、
	// この値によらず、音声ソースの状態で決める (見つかっている間は真、判定できない間は偽。decor_registry.h の
	// CompositeDecorView::source_voice)。
	bool self_hears_voice = false;
	// 名前ラベルを出す人 (通話の参加者のうち、Bot、受け取りに使うアカウント、「通話のみ時ラベル非表示」を付けた人などを除いた人。
	// dock/build_observation.h の composite_roster_people)。描く側が、名前ラベル付きで描くタイルがない人を、画面共有して
	// いない参加者の並びに描く (composite_label_plan.h の composite_roster_shows)。並びは問わない (描く側が
	// composite_roster_order の順に並べる)。並びの名前ラベルに描く名前とアバターは、user_id で引く表 (decor_registry.h の
	// set_roster_person_decor) から読む。
	std::vector<CompositeRosterPerson> roster;
	// 自分の枠の人の user_id と、配信に出している名前 (CompositeLayoutRequest::self_user_id)。
	std::string self_user_id;
	std::string self_name;
	// 全画面で映す人の user_id (CompositeLayoutRequest::focus_user_id。空 = 全画面でない)。
	std::string focus_user_id;
	// 全画面で映す人を全画面でないときの配置で描かないときの、その人の映像を持つ子の番号
	// (CompositeLayoutRequest::focus_extra_slot。0 = ない)。子に映像があるかは、描く側が描く直前に見る。
	int focus_extra_slot = 0;
};

// 枠に載っている人の情報 (表示名と user_id と、cordmux がその人の声を聞けるか。声の枠に上限がなく、話した人は
// 全員が声の経路を持つので、ドックは常に真を入れる)。
struct SlotPerson {
	int slot = 0;
	std::string name;
	std::string user_id;
	bool hears_voice = true;
	std::int64_t added_order = 0; // 載せた順の番号 (CompositeShare::added_order)
	bool is_main = false;         // メインアカウントの画面共有か (CompositeShare::is_main)
};

// 見えている画面共有の枠 visible_slots と、枠ごとの人の情報から、配置の入力を作る。
//   - 枠の番号が 1 から kCompositeCellCount の外のものと、同じ番号の 2 つ目以降は読み飛ばす。
//   - 人の情報がない枠は、表示名も user_id も空にして入れる (並びは枠の番号が決める)。声を聞けるかは真にする。
//   - 並べ替えはここではしない (plan_composite_layout が並び順のとおりに並べる)。
inline CompositeInput make_composite_input(const std::vector<int> &visible_slots, const std::vector<SlotPerson> &people,
					   bool self_shown, bool full_when_single, int gap_px)
{
	CompositeInput input;
	input.self_shown = self_shown;
	input.full_when_single = full_when_single;
	input.gap_px = gap_px;
	for (const int slot : visible_slots) {
		if (slot < 1 || slot > kCompositeCellCount)
			continue;
		const bool seen = std::any_of(input.shares.begin(), input.shares.end(),
					      [slot](const CompositeShare &share) { return share.slot == slot; });
		if (seen)
			continue;
		CompositeShare share;
		share.slot = slot;
		for (const SlotPerson &person : people) {
			if (person.slot == slot) {
				share.name = person.name;
				share.user_id = person.user_id;
				share.hears_voice = person.hears_voice;
				share.added_order = person.added_order;
				share.is_main = person.is_main;
				break;
			}
		}
		input.shares.push_back(std::move(share));
	}
	return input;
}

// ダミー映像の間の配置の入力。画面共有の枠の 4 つすべてに、ダミーの人 (dummy_people。枠ごとの名前と、全画面で映す人を
// 見分ける user_id) を置き、自分の枠は描かない。自分の枠の条件 (自分の行のチェック、自分の枠に映す映像、自分の画面共有、
// [先頭に配置]) には依らない。ダミー映像は、通話がないときに画面共有の枠と名前ラベルの見た目を確かめるためのもので、自分の
// 枠の映像は、選んだソースそのものを見れば確かめられるためである。4 つのダミーは、自分の枠がないときと同じ配置で描く。
// 自分の枠のチェックと割り当ては変えないので、ダミーをやめると、自分の枠は前のとおりに描かれる。
inline CompositeInput dummy_composite_input(const std::vector<SlotPerson> &dummy_people, bool full_when_single, int gap_px)
{
	std::vector<int> all_slots;
	for (int slot = 1; slot <= kCompositeCellCount; ++slot)
		all_slots.push_back(slot);
	return make_composite_input(all_slots, dummy_people, false, full_when_single, gap_px);
}

// 配置の入力とキャンバスの大きさから、plan_composite_layout に渡す値を作る。focus_extra_ready は偽のまま返す (子に映像が
// あるかは、描く側が描く直前に入れる)。
inline CompositeLayoutRequest make_layout_request(const CompositeInput &input, int canvas_w, int canvas_h)
{
	CompositeLayoutRequest request;
	request.canvas_w = canvas_w;
	request.canvas_h = canvas_h;
	request.self_shown = input.self_shown;
	request.shares = input.shares;
	request.full_when_single = input.full_when_single;
	request.gap_px = input.gap_px;
	request.slot_order = input.slot_order;
	request.self_first = input.self_first;
	request.self_added_mark = input.self_added_mark;
	request.focus_user_id = input.focus_user_id;
	request.self_user_id = input.self_user_id;
	request.self_name = input.self_name;
	request.focus_extra_slot = input.focus_extra_slot;
	return request;
}

// ---- 全画面 (一時的に 1 人だけをキャンバスの全面に描く) ----

// 全画面にできるタイル 1 つ分。全画面でないときの配置で、今描いているタイルである。
struct CompositeFocusTile {
	std::string user_id; // 自分の枠は CompositeInput::self_user_id
	std::string name;    // 配信に出している名前 (自分の枠は CompositeInput::self_name)
	int cell = 0;        // 4 等分の位置 (0 左上、1 右上、2 左下、3 右下。1 つを全面に置くときは 0)
};

// 全画面にできるタイルを、位置の順に返す。位置は全画面でないときの配置 (plan_composite_layout と同じ
// ledger/slot_cells.h の plan_slot_cells) で決め、input.focus_user_id には依らない。描かない人 (dropped) は入れない。
// user_id が空のタイル (人の情報がない枠と、自分の枠の人が分からない自分の枠) は、人を指せないので入れない。
inline std::vector<CompositeFocusTile> composite_focus_tiles(const CompositeInput &input)
{
	std::vector<CompositeFocusTile> out;
	const ledger::SlotCellPlan plan = ledger::plan_slot_cells(
		composite_detail::cell_shares_of(input.shares),
		composite_detail::cell_self_of(input.self_shown, input.self_first, input.self_name, input.self_user_id,
					       input.self_added_mark),
		input.slot_order, input.full_when_single);
	for (const ledger::SlotCell &cell : plan.cells) {
		CompositeFocusTile tile;
		if (cell.self) {
			tile.user_id = input.self_user_id;
			tile.name = input.self_name;
		} else {
			const CompositeShare &share = input.shares[static_cast<std::size_t>(cell.share)];
			tile.user_id = share.user_id;
			tile.name = share.name;
		}
		tile.cell = cell.cell;
		if (!tile.user_id.empty())
			out.push_back(std::move(tile));
	}
	return out;
}

// 全画面でないときの配置で描いていないが、全画面にできる人 (画面共有している人。枠に載せていない人と、描けずに
// dropped になった人)。全画面にすると、その人の映像を受信していなければ受信を始め、映像が届いたら全画面にする
// (CompositeLayoutRequest::focus_extra_slot)。
struct CompositeFocusOther {
	std::string user_id;
	std::string name; // 配信に出している名前
};

// 全画面でないときの配置で描いていない、画面共有している人を、sharing の順に返す。tiles にいる人、excluded の人
// (Vesktop のアカウント。自分の画面共有は受信できない。自分の枠で出す人)、user_id が空の人、同じ人の 2 回目は入れない。
// name_of は、配信に出している名前を返す関数。
template <typename NameOf>
std::vector<CompositeFocusOther> composite_focus_others(const std::vector<std::string> &sharing,
							const std::vector<CompositeFocusTile> &tiles,
							const std::vector<std::string> &excluded, NameOf name_of)
{
	std::vector<CompositeFocusOther> out;
	for (const std::string &user_id : sharing) {
		if (user_id.empty())
			continue;
		const auto same = [&user_id](const auto &item) { return item.user_id == user_id; };
		if (std::any_of(tiles.begin(), tiles.end(), same) || std::any_of(out.begin(), out.end(), same) ||
		    std::find(excluded.begin(), excluded.end(), user_id) != excluded.end())
			continue;
		out.push_back(CompositeFocusOther{user_id, name_of(user_id)});
	}
	return out;
}

// 配置を決め直したときの、全画面で映す人。全画面を続けるときは focus をそのまま返し、やめるときは空を返す。やめるのは
// cordmux が無効のときと、その人のタイルを描かなくなったとき (画面共有をやめた、通話を抜けた、枠から外れた、入れ替えで
// 別の人になった、自分の枠を消した、Vesktop との接続が切れて枠が空いた。どれも tiles からその人が消える) である。
// 枠に載せていない人を全画面にしているとき (呼び出し元が覚えている) は others も渡し、その人が others にいる間 (画面共有を
// 続けている間) は全画面を続ける。枠に載っている人を全画面にしていて、その人を枠から外したときは others を渡さず、全画面を
// やめる (外したのは、配信から消すためである)。
inline std::string focus_after_layout(const std::string &focus, const std::vector<CompositeFocusTile> &tiles,
				      bool feature_on, const std::vector<CompositeFocusOther> &others = {})
{
	if (focus.empty() || !feature_on)
		return std::string();
	for (const CompositeFocusTile &tile : tiles) {
		if (tile.user_id == focus)
			return focus;
	}
	for (const CompositeFocusOther &other : others) {
		if (other.user_id == focus)
			return focus;
	}
	return std::string();
}

// ホットキーの「<位置>の枠を全画面で表示」を押したときの操作。act が偽なら何もしない (その位置に誰も描いていない)。
// act が真なら、user_id の人を全画面にする (user_id が空なら、全画面を元に戻す)。その位置の人が既に全画面なら戻すので、
// 同じキーで全画面と元の配置を切り替えられる。tiles は全画面でないときの配置 (composite_focus_tiles) である。
struct FocusHotkeyTarget {
	bool act = false;
	std::string user_id;
};

inline FocusHotkeyTarget focus_hotkey_target(const std::vector<CompositeFocusTile> &tiles, int cell,
					     const std::string &focus)
{
	FocusHotkeyTarget out;
	for (const CompositeFocusTile &tile : tiles) {
		if (tile.cell != cell)
			continue;
		out.act = true;
		if (tile.user_id != focus)
			out.user_id = tile.user_id;
		return out;
	}
	return out;
}

// タイルごとの、描く人の user_id。tiles は input から作った配置のタイルで、戻り値は同じ並び。自分の枠は input.self_user_id、
// 画面共有の枠は同じ枠の番号の shares の user_id、全画面だけで映す人の子 (kFocusOnlySlot) は input.focus_user_id。見つからない
// 枠は空にする。名前ラベル付きで描いている枠がある人を、画面共有していない参加者の並びに描かないために使う
// (composite_label_plan.h の composite_labeled_tile_users)。
inline std::vector<std::string> composite_tile_user_ids(const CompositeInput &input,
							const std::vector<CompositeTile> &tiles)
{
	std::vector<std::string> out;
	out.reserve(tiles.size());
	for (const CompositeTile &tile : tiles) {
		if (tile.self) {
			out.push_back(input.self_user_id);
			continue;
		}
		std::string user_id = tile.slot == config::kFocusOnlySlot ? input.focus_user_id : std::string();
		for (const CompositeShare &share : input.shares) {
			if (share.slot == tile.slot) {
				user_id = share.user_id;
				break;
			}
		}
		out.push_back(std::move(user_id));
	}
	return out;
}

// タイルごとの、cordmux がその人の声を聞けるか。tiles は input から作った配置のタイルで、戻り値は同じ並び。自分の枠は
// input.self_hears_voice、画面共有の枠は同じ枠の番号の shares の値、全画面だけで映す人の子 (kFocusOnlySlot) は真。見つからない
// 枠は偽にする (声を聞けるか分からない人を暗くすると、話していても暗く見えることがある。配置は input.shares から作るので、
// 通常は見つかる)。
inline std::vector<bool> composite_tile_hears_voice(const CompositeInput &input, const std::vector<CompositeTile> &tiles)
{
	std::vector<bool> out;
	out.reserve(tiles.size());
	for (const CompositeTile &tile : tiles) {
		if (tile.self) {
			out.push_back(input.self_hears_voice);
			continue;
		}
		// 全画面だけで映す人の子は shares にない。声は枠に載せていない人も受け取るので、載っている人と同じく聞けるものとする。
		bool hears = tile.slot == config::kFocusOnlySlot;
		for (const CompositeShare &share : input.shares) {
			if (share.slot == tile.slot) {
				hears = share.hears_voice;
				break;
			}
		}
		out.push_back(hears);
	}
	return out;
}

} // namespace cordmux::sources

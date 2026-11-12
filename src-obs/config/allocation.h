// SPDX-License-Identifier: Apache-2.0

// 枠の割り当て表。画面共有の枠 (4 つ) と声の枠 (上限なし) に誰を載せているかを、cordmux の中でここだけが持つ。
// 表は保存しない。OBS を起動し直すと空から始まる。音量とミュートは参加者ごとの値で、この表は持たない
// (pipeline/mixer_roster.h の MixerLevels)。libobs にも Qt にも依存せず、C++17 の標準ライブラリだけを使う。
// テスト (test_config_*.cpp) が tests/CMakeLists.txt を変えずにインクルードできるように、ヘッダーだけで実装する。

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cordmux::config {

// 枠の種類。
enum class SlotKind {
	Screen, // 画面共有の枠 1 から 4 (映像と、画面共有の音声)
	Voice,  // 声の枠 1 から (マイクの声だけ。数に上限はなく、足りなければ増える)
};

constexpr int kScreenSlotCount = 4;

// 枠に載せていない人を全画面で映すときに、その人の映像を渡す合成のソースの子の番号。割り当て表の枠ではない (割り当て表は
// 画面の枠 1 から kScreenSlotCount だけを持つ)。経路の slot にこの番号を入れ、映像は出すが、画面共有の音は出さない
// (pipeline/stream_router.h の AssignPolicy::focus_user_id)。
constexpr int kFocusOnlySlot = kScreenSlotCount + 1;
// 合成のソースが持つ画面の子の数 (画面の枠の数と、kFocusOnlySlot の 1 つ)。
constexpr int kScreenTileCount = kScreenSlotCount + 1;

// 枠に載っている人を配信のどの位置に置くかの決め方 (設定の slot_order)。
//   Name   表示名の順に左上から詰める。人が抜けると後ろが詰まる
//   Added  載せた順 (SlotAssignment::added_order) に左上から詰める。人が抜けると後ろが詰まる
// 位置を決めるのは ledger/slot_cells.h の plan_slot_cells である。
enum class SlotOrder { Name, Added };

// 枠の番号は 1 から始まる (利用者に見える枠の番号とそろえる)。

// 枠 1 つの割り当て。
// owner_user_id: 枠に載っている人。画面共有の枠では、映像と画面共有の音声を出す人。声の枠では、ミキサーの行の名前と、
//                枠を持つ基準になる人。画面共有をやめるか通話から抜けると、呼び出し元 (pipeline) が枠を空ける。
// voice_user_id: 実際に声を出す人。既定値は owner_user_id と同じで、使う声を置き換えたときだけ、声の枠で別の
//                userId になる。画面共有の枠は声を運ばないので、常に owner_user_id と同じに保つ約束である
//                (呼び出し元が守る。ここでは確認せず、渡された値をそのまま持つ)。
// added_order: この人を枠に載せた順の番号 (大きいほど後)。手動ならチェックを付けた順、自動なら画面共有を始めた順に
//       なる。try_assign と、別の人に書き直す rewrite が新しい番号を付ける。既に載っている人の番号は変えない。
//       配信の位置の決め方が Added のときにだけ読む。
struct SlotAssignment {
	std::string owner_user_id;
	std::string voice_user_id;
	std::int64_t added_order = 0;
};

enum class AssignOutcome {
	AlreadyOwned, // 既にこの人の枠がある (枠の番号は変えない)
	Assigned,     // 空き枠に、新しく割り当てた
	Insufficient, // 割り当てられる枠がない
};

struct AssignResult {
	AssignOutcome outcome = AssignOutcome::Insufficient;
	int slot = 0; // Insufficient のときは 0
};

// 枠の割り当て表。画面共有の枠 (4 つ) と声の枠 (上限なし) を 1 つの表として持つ。呼び出しは、この中で閉じた
// 決定的な状態の変化で、libobs も Qt もファイルも触らない。誰が通話に参加しているかは、この表は知らない
// (枠を空ける判断は呼び出し元の pipeline/slot_release.h がする)。
class AllocationTable {
public:
	AllocationTable() = default;

	bool has_assignment(SlotKind kind, int slot) const;
	const SlotAssignment *assignment(SlotKind kind, int slot) const;

	// 音声を出してよいか (画面共有の枠では画面共有の音声、声の枠では声)。割り当てがなければ false。名前に反して、
	// 映像は判定しない (映像は is_video_allowed で判定する)。
	bool is_output_allowed(SlotKind kind, int slot) const;

	// 映像を出してよいか (画面共有の枠のフレームを OBS に渡してよいか、自動配置で見えている枠として数えてよいか)。
	// 割り当てがある画面共有の枠だけ true を返す。凍結 (is_frozen) はここでは見ない (is_output_allowed と同じ。凍結を
	// 見るのは pipeline/stream_router.h の役目)。
	//
	// 声の枠 (SlotKind::Voice) では常に false を返す。声の枠は映像を運ばないので、映像の判定は画面共有の枠に
	// だけ置く。種類によらず、割り当てがあれば true を返す形にすると、声の枠に映像を出してよいと答える関数になり、
	// 呼び出し元が種類を取り違えても、誤りに気付けないまま true が返る。
	bool is_video_allowed(SlotKind kind, int slot) const;

	// 画面共有の枠では、voice_user_id を常に owner_user_id と同じに保つ約束である (画面共有の枠は声を運ばない
	// ので、2 つの userId が分かれるのは使う声を置き換えたときだけで、それは声の枠でしか起きない)。この関数は、
	// その約束が守られているかを確認するだけで、破られていても修正しない (try_assign、rewrite、set_voice_source は、
	// この約束を自分では守らせない。呼び出し元の誤りをテストで見つけるための確認)。割り当てのない枠は崩れる
	// ことがないので true。声の枠は owner と voice が違ってよいので、常に true。
	bool is_consistent(SlotKind kind, int slot) const;

	// 画面共有の枠 1 から kScreenSlotCount のすべてで、is_consistent が true か。
	bool all_screen_slots_consistent() const;

	// owner_user_id の人を kind の枠に割り当てる。既にこの人の枠があればその枠 (番号を変えない)、なければ空き枠の
	// うち番号の小さい枠に載せる。画面共有の枠に空きがなければ Insufficient を返す (ほかの人の枠は取り上げない。枠を
	// 持っているのは画面共有をしている人だけで、空けるのは、その人が共有をやめるか通話から抜けたときの
	// pipeline/slot_release.h と、利用者の操作だけである)。声の枠は上限がなく、空きがなければ末尾に増やすので、
	// Insufficient にならない。
	AssignResult try_assign(SlotKind kind, const std::string &owner_user_id);

	// 枠の番号の最大値 (画面共有の枠は常に kScreenSlotCount、声の枠は今使っている最も大きい番号)。割り当てのある枠を
	// 全部たどるときの上限に使う。
	int slot_extent(SlotKind kind) const;

	// いちばん小さい空き枠の番号 (画面共有の枠で、なければ 0。声の枠は、空きがなければ末尾の次の番号)。空き枠が
	// あるかだけを知りたい呼び出し元のための関数である (載せる人の選び方が「手動」のとき、「自動で載せる」にした人を
	// 入れてよいのは空き枠があるときだけ。pipeline/stream_router.h)。
	int first_free_slot(SlotKind kind) const;

	// 載せた順の番号を設定する (SlotAssignment::added_order)。枠の入れ替え (pipeline の replace_slot_in) が、
	// 出る人の番号を入る人に引き継ぐために呼ぶ。これから付ける番号は、設定した値より大きくする。割り当てのない枠には
	// 何もしない。
	void set_added_order(SlotKind kind, int slot, std::int64_t order);

	// 使う声だけを置き換える (owner は変えないので、表示名とミキサーの行は、載っている人のまま)。
	void set_voice_source(SlotKind kind, int slot, const std::string &voice_user_id);

	// 枠を別の人で書き直す (枠の入れ替え。pipeline の replace_slot_in)。割り当てのない枠にも、新しい割り当てとして
	// 使える。
	void rewrite(SlotKind kind, int slot, const std::string &owner_user_id, const std::string &voice_user_id);

	// シーンコレクションの切り替えの始まり。OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING (ソースが破棄される前) で
	// 呼び出す。apply_scene_collection_changed を呼ぶまで凍結する (is_frozen)。割り当ては変えない。受信は止めない
	// (呼び出し元が続ける)。
	void begin_scene_collection_change();

	// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED (新しいソースの create と load の後) で呼び出す。凍結を解く。
	void apply_scene_collection_changed();

	// 枠の割り当てを消す。この枠は空きになり、try_assign が誰にでも割り当てられる。has_assignment、
	// is_video_allowed、is_output_allowed はどれも false になる。呼ぶのは、ドックの参加者の行のチェックを外す操作 (pipeline の
	// release_slot) と、載っている人が画面共有をやめたか通話から抜けたとき (pipeline/slot_release.h)。載せる人の
	// 選び方を自動から手動に切り替えても、載っている人の枠は空けない (自動の間もドックはその人の行にチェックを付けて
	// 表示しているので、空けると、設定を変えた瞬間に配信から人が消える)。声の枠は、末尾の空きの枠を表から削る。
	void release(SlotKind kind, int slot);

	// この人が今持っている枠の番号 (なければ 0)。利用者は枠の番号ではなく人を指定して外すので (ドックの行 1 つが
	// 1 人)、番号を引く関数をここに置く。
	int slot_of_owner(SlotKind kind, const std::string &owner_user_id) const;

	// begin_scene_collection_change を呼んでから apply_scene_collection_changed を呼ぶまでの間か。この間は OBS の
	// ソースが破棄されているので、pipeline/stream_router.h はフレームと音声を OBS に渡さず、pipeline/ は
	// 合成のソースの有無を調べない。config/ の状態はそのまま残り、is_video_allowed、is_output_allowed、
	// assignment はこの間も通常どおり読める。
	bool is_frozen() const { return frozen_; }

private:
	struct SlotEntry {
		bool assigned = false;
		SlotAssignment data;
	};

	std::array<SlotEntry, kScreenSlotCount> screen_slots_{};
	// 声の枠。番号 n の枠は voice_slots_[n - 1]。足りなければ増え、末尾の空きは release が削る。
	std::vector<SlotEntry> voice_slots_;

	bool frozen_ = false;
	std::int64_t next_added_order_ = 1;

	SlotEntry *entry(SlotKind kind, int slot);
	const SlotEntry *entry(SlotKind kind, int slot) const;
};

// ---------------------------------------------------------------------------
// 実装 (ヘッダーだけで実装する)。
// ---------------------------------------------------------------------------

inline AllocationTable::SlotEntry *AllocationTable::entry(SlotKind kind, int slot)
{
	if (kind == SlotKind::Screen) {
		if (slot < 1 || slot > kScreenSlotCount)
			return nullptr;
		return &screen_slots_[static_cast<size_t>(slot - 1)];
	}
	if (slot < 1 || slot > static_cast<int>(voice_slots_.size()))
		return nullptr;
	return &voice_slots_[static_cast<size_t>(slot - 1)];
}

inline const AllocationTable::SlotEntry *AllocationTable::entry(SlotKind kind, int slot) const
{
	return const_cast<AllocationTable *>(this)->entry(kind, slot);
}

inline bool AllocationTable::has_assignment(SlotKind kind, int slot) const
{
	const SlotEntry *e = entry(kind, slot);
	return e != nullptr && e->assigned;
}

inline const SlotAssignment *AllocationTable::assignment(SlotKind kind, int slot) const
{
	const SlotEntry *e = entry(kind, slot);
	if (!e || !e->assigned)
		return nullptr;
	return &e->data;
}

inline bool AllocationTable::is_output_allowed(SlotKind kind, int slot) const
{
	const SlotEntry *e = entry(kind, slot);
	return e != nullptr && e->assigned;
}

inline bool AllocationTable::is_video_allowed(SlotKind kind, int slot) const
{
	if (kind != SlotKind::Screen)
		return false; // 声の枠は映像を運ばない (宣言のコメント)。
	const SlotEntry *e = entry(kind, slot);
	return e != nullptr && e->assigned;
}

inline bool AllocationTable::is_consistent(SlotKind kind, int slot) const
{
	if (kind != SlotKind::Screen)
		return true; // 声の枠には owner == voice の約束がない (使う声を置き換えられる)。
	const SlotEntry *e = entry(kind, slot);
	if (!e || !e->assigned)
		return true; // 割り当てがなければ崩れることがない。
	return e->data.voice_user_id == e->data.owner_user_id;
}

inline bool AllocationTable::all_screen_slots_consistent() const
{
	for (int slot = 1; slot <= kScreenSlotCount; ++slot) {
		if (!is_consistent(SlotKind::Screen, slot))
			return false;
	}
	return true;
}

inline AssignResult AllocationTable::try_assign(SlotKind kind, const std::string &owner_user_id)
{
	const int extent = slot_extent(kind);

	// 1. 既にこの人の枠があれば、そのまま返す (番号を変えない)。
	for (int slot = 1; slot <= extent; ++slot) {
		const SlotEntry *e = entry(kind, slot);
		if (e->assigned && e->data.owner_user_id == owner_user_id)
			return AssignResult{AssignOutcome::AlreadyOwned, slot};
	}

	// 2. 空き枠のうち番号の小さい枠に載せる。
	for (int slot = 1; slot <= extent; ++slot) {
		SlotEntry *e = entry(kind, slot);
		if (!e->assigned) {
			e->assigned = true;
			e->data = SlotAssignment{owner_user_id, owner_user_id, next_added_order_++};
			return AssignResult{AssignOutcome::Assigned, slot};
		}
	}

	// 3. 声の枠は上限がないので、末尾に増やす。画面共有の枠は、空きがなければ載せない。
	if (kind == SlotKind::Voice) {
		SlotEntry added;
		added.assigned = true;
		added.data = SlotAssignment{owner_user_id, owner_user_id, next_added_order_++};
		voice_slots_.push_back(std::move(added));
		return AssignResult{AssignOutcome::Assigned, static_cast<int>(voice_slots_.size())};
	}
	return AssignResult{AssignOutcome::Insufficient, 0};
}

inline int AllocationTable::slot_extent(SlotKind kind) const
{
	return kind == SlotKind::Screen ? kScreenSlotCount : static_cast<int>(voice_slots_.size());
}

inline int AllocationTable::first_free_slot(SlotKind kind) const
{
	const int extent = slot_extent(kind);
	for (int slot = 1; slot <= extent; ++slot) {
		const SlotEntry *e = entry(kind, slot);
		if (e != nullptr && !e->assigned)
			return slot;
	}
	return kind == SlotKind::Voice ? extent + 1 : 0;
}

inline void AllocationTable::set_added_order(SlotKind kind, int slot, std::int64_t order)
{
	SlotEntry *e = entry(kind, slot);
	if (!e || !e->assigned)
		return;
	e->data.added_order = order;
	next_added_order_ = std::max(next_added_order_, order + 1);
}

inline void AllocationTable::set_voice_source(SlotKind kind, int slot, const std::string &voice_user_id)
{
	SlotEntry *e = entry(kind, slot);
	if (e && e->assigned)
		e->data.voice_user_id = voice_user_id;
}

inline void AllocationTable::rewrite(SlotKind kind, int slot, const std::string &owner_user_id,
				      const std::string &voice_user_id)
{
	// 声の枠で、まだない番号に書くときは、そこまで増やす。
	if (kind == SlotKind::Voice && slot >= 1 && slot > static_cast<int>(voice_slots_.size()))
		voice_slots_.resize(static_cast<size_t>(slot));
	SlotEntry *e = entry(kind, slot);
	if (!e)
		return;
	if (!e->assigned || e->data.owner_user_id != owner_user_id)
		e->data.added_order = next_added_order_++;
	e->assigned = true;
	e->data.owner_user_id = owner_user_id;
	e->data.voice_user_id = voice_user_id;
}

inline void AllocationTable::begin_scene_collection_change()
{
	frozen_ = true;
}

inline void AllocationTable::apply_scene_collection_changed()
{
	frozen_ = false;
}

inline void AllocationTable::release(SlotKind kind, int slot)
{
	SlotEntry *e = entry(kind, slot);
	if (!e)
		return;
	e->assigned = false;
	e->data = SlotAssignment{};
	// 声の枠は、末尾の空きを削る (枠の数が、使った番号の最大のまま残らないように)。
	while (!voice_slots_.empty() && !voice_slots_.back().assigned)
		voice_slots_.pop_back();
}

inline int AllocationTable::slot_of_owner(SlotKind kind, const std::string &owner_user_id) const
{
	if (owner_user_id.empty())
		return 0;
	for (int slot = 1; slot <= slot_extent(kind); ++slot) {
		const SlotEntry *e = entry(kind, slot);
		if (e && e->assigned && e->data.owner_user_id == owner_user_id)
			return slot;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// 使う声の置き換えの組
// ---------------------------------------------------------------------------

// 「載っている人 -> 使う声」の組のうち、声の経路に使ってよいものだけを返す (純粋関数)。
//
// 組は 1 対 1 にし、連鎖を作らない。
//   - 1 人の使う声は 1 人まで (組の形そのもの。載っている人をキーにした表)。
//   - 1 人を 2 人の使う声にしない (同じ人を指す組が 2 つ以上あれば、どれも採らない)。
//   - 使う声に指定された人の声を、さらに置き換えない (使う声の人にも使う声が指定されている組を採らない)。
//   - 誰かの使う声になっている人に、使う声を付けない (載っている人が誰かの使う声である組を採らない)。
//   - 空の値と、本人を指す組は、置き換えていないのと同じに扱う (空の上書きを上書きなしと扱うのと同じ約束。
//     ledger/observation.h の override_present)。
// 経路は「使う声の人の声 -> 載っている人の声の枠」と逆に引く (pipeline/stream_router.h) ので、逆に引いたときに
// 1 つに決まらないと、その声をどの枠に入れるかが決まらない。連鎖を許すと、「A の枠に B の声、B の枠に C の声」で
// B の声がどこにも出ず、どの声がどの枠から出ているかを、1 段の名前で表示できなくなる (画面は使う声の人の名前を
// 常に表示する)。
//
// ここで落とすのは、手で書き換えた一覧のファイル (seen_people.json) の組だけである。ドックの操作は
// ledger/actions.cpp が同じ約束で拒むので、画面からはこの形を作れない。落とした組の数をログに書くのは、
// 呼び出し元の役目である (dock/dock_poll.cpp)。衝突した組は全部落とす。
// どちらかを勝たせる決め方 (userId の順、先に書いた行) は、利用者の意図と関係がない。先に見つけた組を勝たせると、
// 表の順で結果が変わり、ファイルの行を並べ替えただけで、声が別の枠に移る。
//
// この関数を 3 か所が使う。声の経路 (stream_router.h)、参加者の一覧の「声: %1」(ledger/rules.cpp)、名前ラベルの
// 話している人の光 (dock/build_observation.cpp) である。別々に作ると、画面が「声: B」と表示しているのに、経路は載って
// いる人の声を流すことがある。
inline std::map<std::string, std::string> usable_voice_links(const std::map<std::string, std::string> &links)
{
	std::map<std::string, std::string> marks;
	for (const auto &[owner, source] : links) {
		if (!owner.empty() && !source.empty() && owner != source)
			marks.emplace(owner, source);
	}
	std::map<std::string, int> source_count;
	for (const auto &[owner, source] : marks) {
		(void)owner;
		++source_count[source];
	}

	std::map<std::string, std::string> out;
	for (const auto &[owner, source] : marks) {
		if (source_count[source] != 1)
			continue; // 1 人を 2 人の使う声にしている
		if (marks.count(source) != 0)
			continue; // 使う声の人にも使う声が指定されている (連鎖)
		if (source_count.count(owner) != 0)
			continue; // 載っている人が誰かの使う声になっている (連鎖)
		out.emplace(owner, source);
	}
	return out;
}

} // namespace cordmux::config

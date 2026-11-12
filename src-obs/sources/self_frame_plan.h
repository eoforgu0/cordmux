// SPDX-License-Identifier: Apache-2.0

// 自分の枠に映す OBS のソース (設定の self_frame_source_uuid と self_frame_source_name) の、候補にしてよい種類の判断と、
// 探した結果の純粋関数。探し方は自分の枠の音声ソースと同じ (sources/self_speaking_judge.h の find_self_source): UUID で
// 探し、見つからなければ名前で探す。あわせて、自分の枠を出すかの状態 (ドックの付け外しと、自分の Discord の共有の始まりと
// 終わり) の次の状態を決める純粋関数も持つ。libobs にも Qt にも依存しない。

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sources/self_speaking_judge.h"

namespace cordmux::sources {

// 自分の枠に映すソースの候補にしてよい種類か。映像を持つソースで、cordmux の種類ではない。cordmux の種類を選ぶと、
// 合成のソースが自分自身を描く循環ができる。
inline bool self_frame_source_eligible(bool has_video, std::string_view type_id)
{
	return has_video && !is_cordmux_source_id(type_id);
}

// OBS のシーンの中身。入れ子のシーンとグループは children に展開した木にする (展開の深さに上限があり、超えた所は
// truncated にする)。type_id は OBS のソースの種類の ID。
struct SceneNode {
	std::string type_id;
	bool truncated = false;
	std::vector<SceneNode> children;
};

// シーンの中に cordmux の種類のソースがあるか (入れ子のシーンとグループの中も見る)。あるシーンを自分の枠に映すと、
// 合成のソースが自分自身を描く循環ができる。展開を打ち切った所は、中が分からないので「ある」と扱う。
inline bool scene_contains_cordmux(const SceneNode &node)
{
	if (node.truncated)
		return true;
	for (const SceneNode &child : node.children) {
		if (is_cordmux_source_id(child.type_id) || scene_contains_cordmux(child))
			return true;
	}
	return false;
}

// 自分の枠に映すシーンの候補にしてよいか。グループではなく、cordmux の種類のソースを含まないシーン。is_group は、
// 呼び出し側が OBS の obs_source_is_group() で調べた値を渡す (ソースの種類の ID の文字列を比べない)。
inline bool self_frame_scene_eligible(bool is_group, const SceneNode &contents)
{
	return !is_group && !scene_contains_cordmux(contents);
}

// 設定の UUID と名前から候補を探した結果。index は見つけた候補の添字 (なければ -1)。rewrite が真なら、見つけたソースの
// UUID と名前が設定の値と違うので、設定を uuid と name に書き直す (名前で見つけたとき、利用者がソースの名前を変えたとき)。
struct SelfFrameLookup {
	int index = -1;
	bool by_uuid = false;
	bool rewrite = false;
	std::string uuid;
	std::string name;
};

inline SelfFrameLookup lookup_self_frame(const std::vector<SelfSourceCandidate> &all, const std::string &uuid,
					 const std::string &name)
{
	SelfFrameLookup result;
	result.index = find_self_source(all, uuid, name);
	if (result.index < 0)
		return result;
	const SelfSourceCandidate &chosen = all[static_cast<std::size_t>(result.index)];
	result.by_uuid = !uuid.empty() && chosen.uuid == uuid;
	result.rewrite = chosen.uuid != uuid || chosen.name != name;
	result.uuid = chosen.uuid;
	result.name = chosen.name;
	return result;
}

// ---- 自分の枠を出すかの状態 ----

// 自分の Discord の共有。Unknown は分からない (送信側が未接続、facts の sharing を読めない、自分の人が決まらない)。
// 分からない間は「共有していない」と扱わず、今までの動きに戻す。
enum class SelfShare { Unknown, Off, On };

// 状態が動くきっかけ。Poll は周期の処理 (自分の共有の変化を見る)。DockOn と DockOff は、ドックで自分の行のチェックを
// 付けた・外した。Refused は、付けたチェックを Pipeline が断った (自分の枠を使い始めることを Pipeline に渡したときに、
// Discord の人がもう画面共有の枠を 4 つとも埋めていた。pipeline/stream_router.h の accepted_self_frame_in_use)。
enum class SelfFrameEvent { Poll, DockOn, DockOff, Refused };

// 自分の枠を出すかの状態。on は出す (ドックのチェックの状態そのもの)。excluded は、ドックで外した (ほかの人の
// StreamRouter の excluded_ と同じで、自分の共有をやめて始め直すまで自動では付けない)。prev は直前に分かっていた
// 自分の共有の値 (Unknown は未取得)。on だけが settings.json に残る (config::Settings::self_frame_on)。excluded と prev は
// 残さない。
struct SelfFrameState {
	bool on = false;
	bool excluded = false;
	SelfShare prev = SelfShare::Unknown;
};

struct SelfFrameInput {
	SelfFrameState state;
	SelfShare share = SelfShare::Unknown; // 現在の自分の共有
	SelfFrameEvent event = SelfFrameEvent::Poll;
	bool selected = false;    // 自分の枠に映す映像として、OBS のソースかシーンを選んでいるか (設定)
	bool bound = false;       // 選んだ映像が結び付いているか
	bool automatic = false;   // 載せる人の選び方が「自動」か (設定の assign_mode)
	bool always_on = false;   // 自分の行の [自動で載せる] が付いているか
	int discord_slotted = 0;  // Discord の人で埋まっている画面共有の枠の数
	int slot_count = 4;       // 画面共有の枠の数
};

struct SelfFrameResult {
	SelfFrameState state;
	bool shown = false;    // 自分の枠を描くか (bound && on)
	bool persist = false;  // settings.json の self_frame_on を state.on に書き換える (on が変わったとき)
	bool deferred = false; // 付けるはずだったが、枠が埋まっていたので見送った
};

// 次の状態を決める。ほかの人の行と同じ動き (pipeline/stream_router.h の AssignPolicy) にする。
//   ドックで付けた    on を真、excluded を偽にする。
//   ドックで外した    on を偽、excluded を真にする。
//   共有の始まり      prev が On でなく、今が On。載せる人の選び方が自動か、[自動で載せる] が付いていて、excluded が
//                     偽のとき、on を真にする。
//   共有の間ずっと    今が On。選び方が自動で、excluded が偽なら、on を真にする (枠が空いたら載る)。
//   共有をやめた間    今が Off。excluded を偽にする (外した人は、やめて始め直すまで付けない)。on は変えない。
//   分からない        状態も prev も変えない。
//   Pipeline が断った on を偽にする。excluded は変えない (付けるはずの枠が埋まっていて見送ったときと同じ。自動なら、
//                     枠が空けば付く)。
// 付ける向きは、Discord の人が枠を埋めているときは押しのけず、見送る (deferred)。映像を OBS から選んでいない間
// (selected が偽) は、on を変えない (prev と excluded は進める)。共有の始まりと終わりは Poll のときだけ見る。ドックの
// 出来事と Pipeline が断ったことは prev を変えない (同じ瞬間の始まりを取りこぼさない)。
inline SelfFrameResult plan_self_frame(const SelfFrameInput &in)
{
	SelfFrameResult out;
	out.state = in.state;
	switch (in.event) {
	case SelfFrameEvent::DockOn:
		out.state.on = true;
		out.state.excluded = false;
		break;
	case SelfFrameEvent::DockOff:
		out.state.on = false;
		out.state.excluded = true;
		break;
	case SelfFrameEvent::Refused:
		out.state.on = false;
		break;
	case SelfFrameEvent::Poll:
		if (in.share == SelfShare::Unknown)
			break;
		if (in.share == SelfShare::Off)
			out.state.excluded = false;
		if (in.selected && in.share == SelfShare::On && !in.state.on && !out.state.excluded) {
			const bool started = in.state.prev != SelfShare::On;
			if (in.automatic || (in.always_on && started)) {
				if (in.discord_slotted >= in.slot_count)
					out.deferred = true;
				else
					out.state.on = true;
			}
		}
		out.state.prev = in.share;
		break;
	}
	out.persist = out.state.on != in.state.on;
	out.shown = in.bound && out.state.on;
	return out;
}

// ---- 自分の枠を載せた順の印 ----
//
// 並び順が「載せた順」で自分の枠を先頭に置かないときの、自分の枠の位置の決め手 (ledger/slot_cells.h の
// SlotCellSelf::added_mark)。割り当て表の載せた順の番号 (config::SlotAssignment::added_order) は載せるたびに大きくなるので、
// 自分の枠が映り始めた時に載っていた人の番号の最大を覚えれば、その人たちの後で、それより後に載せた人の前になる。
struct SelfOrderMark {
	bool decided = false; // 決めたか
	std::int64_t mark = 0; // 決めた印 (0 は、誰よりも先に載せた)
};

// 次の印。on はメインアカウントの行のチェック (自分の枠のチェック) が付いているか、shown は自分の枠が実際に映っているか、
// max_order は今の画面の枠の載せた順の番号の最大 (誰もいなければ 0)。
//   - チェックが外れていれば、決めていない状態に戻す (次に付けた時に決め直す)。
//   - チェックが付いていて、まだ決めていなくて、映っていれば、max_order に決める。映っていない間 (映像が「Discordの画面共有」の
//     間、選んだソースが見つからない間) は決めない (利用者に見えない時の並びで位置を決めない)。
//   - 決めた後は、チェックが付いている間は変えない (一時的に映らなくなっても位置が動かない)。
// OBS を起動したときは割り当て表が空なので、起動したときからチェックが付いていて、ほかの人より先に映れば 0 になる。
inline SelfOrderMark next_self_order_mark(const SelfOrderMark &now, bool on, bool shown, std::int64_t max_order)
{
	if (!on)
		return SelfOrderMark{};
	if (now.decided || !shown)
		return now;
	SelfOrderMark out;
	out.decided = true;
	out.mark = std::max<std::int64_t>(0, max_order);
	return out;
}

// メインアカウントの画面共有の枠 (載せた順の番号 share_order) から自分の枠にチェックを引き継ぐときの印。自分の枠を、
// メインアカウントの枠がいた位置 (share_order - 1 の人の後、share_order + 1 の人の前) に置く。映像の種類を替えても、
// 並びの位置を変えないためである。
inline SelfOrderMark self_order_mark_from_share(std::int64_t share_order)
{
	SelfOrderMark out;
	out.decided = true;
	out.mark = std::max<std::int64_t>(0, share_order - 1);
	return out;
}

} // namespace cordmux::sources

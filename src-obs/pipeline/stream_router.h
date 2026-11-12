// SPDX-License-Identifier: Apache-2.0

// 送信側のストリーム (stream_id) を、割り当て表 (config/allocation.h の AllocationTable) の枠に結び付ける経路。
// configure の streams を AllocationTable::try_assign に通して stream_id -> Route を作り、前の configure との
// 違いを RouteChange で返す。経路のキーは stream_id だけである。送信側は stream_id を画面と声をまたいだ
// 通し番号で振る (src-vencord/index.ts の nextStreamId) ので、音声のメッセージのヘッダーに種類がなくても、
// 経路の kind で画面の音か声かが決まる。画面共有の音を声の枠に渡さないのは、この kind による。
// libobs に依存しない。

#pragma once

#include "config/allocation.h"
#include "ws/control_messages.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cordmux::pipeline {

// stream_id の行き先。user_id は configure がその stream_id に付けた値をそのまま持つ。画面の枠なら枠に載っている人、
// 声の枠ならその声を送っている人である。使う声を置き換えた声の枠に載っている人は、AllocationTable::assignment() で
// 引く。
// kind は configure の kind をそのまま持ち、映像と音声を画面と声のどちらに渡すかをこれだけで決める。
struct Route {
	config::SlotKind kind = config::SlotKind::Screen;
	int slot = 0;
	std::string user_id;
};

inline bool operator==(const Route &a, const Route &b)
{
	return a.kind == b.kind && a.slot == b.slot && a.user_id == b.user_id;
}
inline bool operator!=(const Route &a, const Route &b)
{
	return !(a == b);
}

// 1 つの stream_id の経路が変わったこと。before と after のどちらかが値を持たないことがある (新しく追加された、
// configure から外れた)。reason は静的な文字列で、トークンもポート番号も含まないので、そのままログに書いてよい。
struct RouteChange {
	uint32_t stream_id = 0;
	std::optional<Route> before;
	std::optional<Route> after;
	const char *reason = "";
};

// 誰を枠に入れるかの決め方 (settings の assign_mode と、seen_people.json の always_on と voice_source_user_id)。
//
// automatic と always_on が動作するのは画面の枠だけである。声の枠は、決め方に関係なく configure に載った人を全員
// 自動で入れる (上限はない。メインアカウントの声だけは、下の main_account_user_id の説明のとおり)。声の枠を画面の枠の
// 選び方に合わせると、載せていない人の声が配信から消え、利用者がまだ誰も選んでいない起動の直後には、通話の声が 1 つも
// 出ない。
//
// automatic == true (自動): 画面共有をしている人を、始めた順に、載せられる人数 (下の self_frame_in_use) まで try_assign に通す。ただし、利用者がドックでチェックを外した人は、その人が画面共有をやめて始め直すまで通さない。
// 通さないと、外した人が次の configure (誰かが通話に参加する、共有を始める、など) で空いた枠に戻り、誤って配信に出た
// 人を外す操作が動作しない。外した人は StreamRouter が持つ (exclude_user、include_user、clear_excluded)。
// 外した人の扱いは手動のときの (2) にも同じように動作する。
//
// automatic == false (手動、既定): 画面の枠に入れるのは次の 2 つだけである。
//   (1) すでに自分の枠がある人。ドックで枠に載せた人はここに入り、枠から外した人は入らないので、外した人は
//       次の configure が来ても枠に戻らない。枠を持つのは画面共有をしている人だけで、共有をやめるか通話から抜けた人の
//       枠は pipeline/slot_release.h が空けるので、空けた枠に戻らない人は、押し直す。
//   (2) always_on の人 (「自動で載せる」を付けた人) が画面共有を始めたときで、載せられる人数 (下の self_frame_in_use) に
//       達していないとき。ただし、
//       利用者がドックでチェックを外した人 (StreamRouter の exclude_user) は、画面共有をやめて始め直すまで入れない
//       (接続が切れると screen_sharers_ を忘れるので、一覧がないと、繋ぎ直した後の最初の configure で外した人が
//       「始めた」に見えて戻る)。「始めた」は、
//       前の configure にその人の画面のストリームがなく、今回はあることで見る。configure が来るたびに入れると、
//       その人を枠から外しても次の configure で戻ってしまう。載せられる人数に達していれば入れない (ほかの人の枠を
//       取り上げない)。
//
// self_frame_in_use (自分の枠を使っている間。config::self_frame_in_use) は、画面の枠に新しく載せられる人数を 1 つ減らす
// (config::screen_slot_capacity。4 つが 3 つになる)。自動でも「自動で載せる」でも、admit_sharers でも同じである。合成のソースは
// キャンバスを 4 等分して描くので、自分の枠と Discord の 4 人で 5 つになると、1 つを描けない。手動で載せる操作
// (Pipeline::assign_slot) も同じ上限で断る (ドックも同じ上限で押せなくする。ledger/observation.h の self_frame_limit_reached)
// ので、経路によって人数が食い違わない。上限に達して
// 載せられない人は、手動で載せていない人と同じく経路を持たない (全画面で映す人なら focus_only の経路を作る)。すでに枠を
// 持つ人は、上限を超えていても外さない (Discord の人で 4 つ埋まっている間は、自分の枠を使い始めることを
// Pipeline::set_assign_policy が断るので、超えない。accepted_self_frame_in_use)。自分の枠を使うのをやめて上限が上がったときは、
// 呼び出し元 (Pipeline::set_assign_policy) が直前の
// configure を適用し直すので、待っていた人が空いた枠に載る。「自動で載せる」の人は、ほかの理由で載らなかったときと同じく、
// 画面共有を始め直すまで載らない (始めたときにだけ載せる規則のため)。
//
// voice_links (枠に載っている人 O -> 使う声に選んだ人 S) は声の枠だけに適用され、手動でも自動でも同じに適用する。
//   (1) S の声は O の声の枠に入れる (try_assign(Voice, O))。その枠の voice_user_id を S にする。枠と表示名と
//       ミキサーの音量は O のままにする。
//   (2) O 自身の声には経路を作らない (reason は "voice_replaced")。S が通話に参加していなくても同じである。
//       使う声は O の声を S の声に置き換える機能で、2 人の声を混ぜる機能ではない。
//   (3) 置き換えをやめたら O の枠の voice_user_id を O に戻し、S の声は S 自身の枠に戻る。S が前から持っていた
//       声の枠は、置き換えている間も割り当てを残したまま無音になる。
//   (4) O が通話から抜けても、S の声が届いている間は、O の声の枠を空けない (pipeline/slot_release.h)。空けると、
//       S の声を運んでいる枠が、次の configure で別の番号に付け直される。
// 組は config::usable_voice_links で 1 対 1 に絞ってから適用する (手で書き換えたファイルの重なりと連鎖を
// 経路に持ち込まない)。O が通話に参加しているときだけ置き換えると、O が再接続するたびに声がミキサーの
// 別の行に移り、音量が変わるので、O がいないときも置き換える。
//
// main_account_user_id (settings の broadcaster_user_id) の人の声 (自分の声) の扱いは、route_main_account_voice で
// 変わる。サブアカウントの構成では、メインアカウントの声もサブアカウントの Vesktop に届く。
//   - route_main_account_voice が真 (合成のソースがあるとき): ほかの人と同じく声の枠に入れ、経路を作る (声の枠を
//     1 つ使う)。合成のソースは声を cordmux のミキサーで混ぜ、自分の声は既定でミュートにする (pipeline/mixer_roster.h の
//     MixerLevels)。利用者の声は OBS がマイクから直接取っていることが多く、鳴らすと Discord を通った分だけ遅れて
//     二重に配信に出るためである。ミュートでも、受け取った声で自分の枠の光と暗さを決められる。
//   - 偽 (合成のソースがないとき): 声の枠の経路を作らない (reason は "main_account_voice")。メインアカウントが
//     自分の声で持っていた声の枠も空ける。使う声に選ばれていても入れない (声が出どころの人で見る)。
// メインアカウントだけの構成では、自分の声は Vesktop に返ってこないので、欄が空でも、Vesktop のアカウント自身の
// ID が入っていても、何も変わらない。cordmux の録画は経路と関係なく configure のすべてのストリームを受け取るので、
// どちらでも録画には残る。
//
// self_frame_selected が真 (自分の枠に映す映像として OBS のソースかシーンを選んでいる間) で main_account_user_id が空でないとき、
// メインアカウントの画面のストリームは画面の枠に入れず、経路も作らない (reason は "self_frame_selected")。自動でも手動でも同じで、
// 自分の枠を外している間と、選んだ映像が見つからない間も同じである (設定の値で決める)。メインアカウントがすでに持っている画面の枠は空ける。偽に戻ったら
// ほかの人と同じに扱う (手動なら、利用者が載せ直すまで載らない。自動なら画面共有していれば載る)。声と録画は変わらない。
//
// receiver_user_id (Vesktop のアカウント。受け取りに使うアカウントで、facts の self_user_id) の画面は、どの構成でも、どの
// 決め方でも画面の枠に入れず、経路も作らない (reason は "receiver")。送信側は自分の画面共有を視聴しない (WebRTC は自分が
// 送った映像を返さない) ので、受信側に映像が届かず、枠に載せても描けないまま、載せられる人数を 1 つ使ってしまう。facts の
// sharing にはこの人も入る (自分の枠を自動で付けるかの判断と、ドックの「画面共有中」の組分けが使う) ので、admit_sharers でも
// 除く。この人がすでに持っている画面の枠は空ける。手動で載せる操作 (pipeline.h の assign_slot_in) も断る (ドックはこの人の
// 行のチェックを押せなくしている。ledger/rules_dock.cpp の PersonRow::checkable)。メインアカウントだけの構成では自分の枠の人と
// 同じ人で、サブアカウントの構成ではサブアカウントである。
struct AssignPolicy {
	bool automatic = false;          // 既定は手動
	std::set<std::string> always_on; // 「自動で載せる」を付けた人。手動のときだけ見る
	// 使う声。枠に載っている人の userId -> 使う声に選んだ人の userId。元の値は seen_people.json の
	// voice_source_user_id で、ここはドックが渡すコピーである。
	std::map<std::string, std::string> voice_links;
	// メインアカウントの userId。空なら、メインアカウントだけの構成である。
	std::string main_account_user_id;
	// メインアカウントの声にも経路を作るか (上の main_account_user_id の説明)。呼び出し元 (pipeline.cpp) が、合成の
	// ソースがあるときに真にする。
	bool route_main_account_voice = false;
	// 自分の枠に映す映像として OBS のソースかシーンを選んでいるか (上の説明)。呼び出し元 (ドック) が渡す。
	bool self_frame_selected = false;
	// Vesktop のアカウントの userId (上の説明。空 = まだ分からない)。呼び出し元 (pipeline.cpp) が、facts の self_user_id から
	// 入れる。
	std::string receiver_user_id;
	// 自分の枠を使っているか (上の説明。config::self_frame_in_use)。真の間は、画面の枠に新しく載せられる人数が 1 つ減る
	// (screen_slot_room)。呼び出し元 (ドック) が、自分の行のチェックと self_frame_selected から決めて渡す。
	bool self_frame_in_use = false;
	// 全画面で映す人 (ドックが渡す。空 = 全画面でない)。この人が画面の枠を持たないとき (枠に載せていない、枠が足りない) は、
	// その人の画面のストリームに、割り当て表に触らずに config::kFocusOnlySlot への経路を作る (reason は "focus_only")。
	// 映像はその子に渡り、画面共有の音は出さない (output_allowed が偽)。自分の枠で出す人 (self_frame_selected の
	// メインアカウント) には作らない。
	std::string focus_user_id;
};

// 画面の枠に、まだ枠を持たない人を新しく載せられるか。載せられる人数は config::screen_slot_capacity で決まり、自分の枠を
// 使っている間 (policy.self_frame_in_use) は 1 つ少ない。数えるのは割り当てのある画面の枠すべてである (メインアカウントの
// 画面共有の枠も数える。描く物の数は、自分の枠と、割り当てのある画面の枠の数で決まるため)。
inline bool screen_slot_room(const config::AllocationTable &table, const AssignPolicy &policy)
{
	return table.assigned_count(config::SlotKind::Screen) < config::screen_slot_capacity(policy.self_frame_in_use);
}

// 自分の枠を使っていることにしてよいか (AssignPolicy::self_frame_in_use に入れる値)。使い始める (in_use_now が偽で、requested が
// 真) ときは、Discord の人が画面の枠をもう 4 つとも埋めていれば断り、偽を返す。自分の枠を足すと 5 つになり、1 つを描けない
// ためである。すでに枠に載っている人は外さない (押しのけない。ドックの自分の枠のチェックと同じ規則。sources/self_frame_plan.h)。
// 数えるとき、自分の枠の人 (self_owner。メインアカウント、メインアカウントが空なら Vesktop のアカウント。ドックの
// self_frame_user_id と同じ) の枠は数えない (自分の枠に映す映像を OBS から選んでいる間、その人の画面の枠は apply_configure が
// 空ける)。やめる向きと、使い続ける向きは断らない。
//
// ドックが自分の枠を付ける判断は、最大 1 周期前のスナップショットの人数で行うので、その後に自動で載せる経路が 4 人目を載せて
// いることがある。この判断を割り当て表と同じロックの中で行い (Pipeline::set_assign_policy)、断ったらドックがチェックを外す。
inline bool accepted_self_frame_in_use(const config::AllocationTable &table, bool in_use_now, bool requested,
				       const std::string &self_owner)
{
	if (!requested)
		return false;
	if (in_use_now)
		return true;
	int discord_slotted = 0;
	for (int slot = 1; slot <= table.slot_extent(config::SlotKind::Screen); ++slot) {
		const config::SlotAssignment *a = table.assignment(config::SlotKind::Screen, slot);
		if (a != nullptr && a->owner_user_id != self_owner)
			++discord_slotted;
	}
	return discord_slotted <= config::screen_slot_capacity(true);
}

// 同じ stream_id が、画面のまま別の人か別の枠に替わったか。true なら pipeline.cpp はデコードのスレッド一式
// (H264Decoder、TimestampAligner、AvOffsetTranscriber) を作り直す。使い回すと H264Decoder がキーフレームを
// 見たという状態 (saw_idr_) を持ち続け、新しい人の映像を、キーフレームより前の P フレームからデコードする。
// それは前の人のフレームを参照した壊れたフレームで、そのまま OBS に渡ってしまう。
// 送信側は起動のたびに通し番号を 1 から振り直す (src-vencord/index.ts の nextStreamId) ので、配信中に
// Vesktop だけを再起動すると、同じ stream_id が別の画面共有に使われる。
// 種類が変わった、または画面でなくなった変化はここでは扱わない (呼び出し元が remove_screen_stream で片付ける)。
inline bool needs_screen_stream_recreate(const RouteChange &change)
{
	if (!change.before || !change.after)
		return false;
	if (change.before->kind != config::SlotKind::Screen || change.after->kind != config::SlotKind::Screen)
		return false;
	return change.before->user_id != change.after->user_id || change.before->slot != change.after->slot;
}

class StreamRouter {
public:
	StreamRouter() = default;

	// configure の streams を割り当て表に通して経路を更新し、変わった経路の一覧を返す。
	// - msg.streams の stream_id は重複しない前提である (同じ stream_id が 2 回以上ある configure は、
	//   ws/control_messages.h の parse_configure が全体を捨てるので、ここまで届かない)。
	// - 前の configure にあって streams にない stream_id は、経路を消す。割り当て表の割り当ては変えない
	//   (枠を空けるのは、facts の sharing と roster で見る pipeline/slot_release.h である。繋ぎ直した直後の
	//   configure には画面のストリームがなく、ここで見ると、共有を続けている人の枠まで空けてしまう)。
	// - 画面のストリームのうち枠に入れてよい人だけを try_assign に通す (AssignPolicy。手動のときは載せた人と
	//   「自動で載せる」の人、自動のときは外していない人。新しく載せるのは、載せられる人数 (screen_slot_room) まで)。
	//   枠に入れない人には経路を作らない。呼び出し元は経路の有無でデコードのスレッドを作るので、表示しない
	//   映像をデコードし続けない。
	// - policy.voice_links があれば、使う声に選んだ人の声を、枠に載っている人の声の枠に入れ、載っている人自身の
	//   声の経路を消す (reason は "voice_replaced")。声の枠の voice_user_id を組のとおりにそろえる
	//   (AssignPolicy の (1) から (4))。
	std::vector<RouteChange> apply_configure(const ws::ConfigureMessage &msg, config::AllocationTable &table,
						  const AssignPolicy &policy);

	// 受け取る範囲が「枠に載せた人だけ」のときに、画面共有している人 (facts の sharing) のうち、枠に載せてよい人を、
	// 映像が届く前に載せる。この範囲では、枠に載っていない人の映像を送信側が視聴しないので、configure に画面の
	// ストリームが載らず、apply_configure の自動の割り当てが動かない。そのため、画面共有している人を facts から見て、
	// apply_configure と同じ規則 (AssignPolicy) で try_assign に通す。載せた人の映像は、この後に送信側が視聴を始めて
	// からの configure で経路になる。
	// - 自動: 外していない人を、sharing_user_ids の順に、載せられる人数 (screen_slot_room) まで try_assign に通す。
	// - 手動: 「自動で載せる」の人が画面共有を始めたときだけ、載せられる人数に達していなければ載せる。「始めた」は、前に呼ばれたときの
	//   sharing_user_ids と、前の configure の画面のストリームの両方にいないことで見る (最初の呼び出しは、全員が
	//   「始めた」になる)。
	// - 外した人 (exclude_user) と、自分の枠で出す人 (policy.self_frame_selected のメインアカウント) と、Vesktop のアカウント
	//   (policy.receiver_user_id。sharing_user_ids に入るが、映像が届かない) は載せない。
	// - すでに枠を持つ人は何もしない。
	// 戻り値は、新しく載せた人。
	std::vector<std::string> admit_sharers(const std::vector<std::string> &sharing_user_ids,
					       config::AllocationTable &table, const AssignPolicy &policy);

	// 現在の経路。なければ値を持たない。映像も音声もこれで引く (種類は Route::kind)。
	std::optional<Route> route(uint32_t stream_id) const;

	// この stream_id の音声 (画面の音と声) を出してよいか。AllocationTable::is_output_allowed (割り当てのない
	// 枠で false) と、シーンコレクションの切り替えの最中の凍結 (AllocationTable::is_frozen) の両方を見る。経路が
	// なければ false。
	bool output_allowed(uint32_t stream_id, const config::AllocationTable &table) const;

	// この stream_id の映像 (画面のフレーム) を出してよいか。AllocationTable::is_video_allowed (割り当てのない
	// 枠と声の枠で false) と凍結を見る。経路がなければ false。
	// 凍結はこちらでも見る。切り替えの最中は OBS のソースが壊されている途中で、映像も音声も渡す先がない。
	bool video_allowed(uint32_t stream_id, const config::AllocationTable &table) const;

	// いま経路を持つ stream_id の一覧 (呼び出し元が、stream_id ごとの表から古い行を取り除くのに使う)。
	std::set<uint32_t> route_ids() const;

	// 利用者がドックで枠の割り当てを変えたときに、その人の経路が指す枠だけを付け替える (割り当て表には
	// 触らない。表を書くのは呼び出し元)。付け替えた経路の数を返す。
	//
	// 経路は stream_id -> 枠を持ち、映像も音声も渡す瞬間に route(stream_id) を引く。A を枠から外して B に渡した後も
	// A の経路が古い枠の番号を指していると、その枠は B のものなので is_video_allowed と is_output_allowed が
	// true を返し、A の映像と音声が B の枠に出る。次の configure で直るが、それまで別の人の映像が出続ける。
	//
	// slot = 0 は「枠から外した」を表す (枠 0 には割り当てがないので、video_allowed と output_allowed が false を
	// 返し、出力が止まる)。経路そのものは消さない。消すと、届いたフレームが「経路なし」に数えられ、もう一度枠に
	// 入れたときに付け替える相手が分からなくなる。
	//
	// 種類ごとに付け替える。画面の枠から外しても声の枠はそのまま残る (声の枠は画面の枠の選び方に合わせない)。
	// デコードのスレッドは作り直さない (needs_screen_stream_recreate を通さない)。作り直すのは別の人の
	// フレームを参照しないためで、ここでは同じ人の映像がほかの枠に移るだけなので、デコーダーの状態はそのまま
	// 正しい。
	int retarget_user(const std::string &user_id, config::SlotKind kind, int slot);

	// 経路をすべて忘れる (割り当て表には触らない)。送信側との接続が切れたときに Pipeline が呼ぶ。次の接続は
	// 同じ stream_id を別の意味で使うことがあるので、古い経路を残さない。外した人の一覧は忘れない (下の
	// exclude_user)。
	void clear();

	// 利用者がドックで画面の枠のチェックを外した人を覚える (手動でも自動でも。呼び出し元は割り当て表の
	// release と同じロックの下で呼ぶ)。apply_configure は、覚えている人を画面共有をやめて始め直すまで
	// 画面の枠に入れず、経路も作らない (reason は "not_chosen")。手動で自分の枠がある人 (利用者が載せた人) の
	// 経路は、この一覧と関係なく今までどおりである。
	//
	// 覚えた人を忘れるのは次の 3 つだけである。
	//   (a) forget_stopped_sharing (facts の sharing に、その人がいないとき。画面共有をやめた)。
	//   (b) include_user (利用者がまたチェックを付けた)。
	//   (c) clear_excluded (選び方を切り替えた)。
	// 接続が切れたことでは忘れない。忘れると、繋ぎ直した後の最初の configure で、外した人が全員戻る。
	//
	// (a) を configure ではなく facts で見るのは、送信側が切断で画面のストリームをすべて止め、繋ぎ直した後も
	// ユーザーを特定し直してから作り直すので、繋ぎ直した直後の configure には画面のストリームが 1 つも
	// なく、configure を見ると、共有を続けている人まで「やめた」ことになるためである。facts の sharing は
	// エンコーダーのストリームとは別に、Discord 側の画面共有を表し、繋ぎ直しても変わらない。
	//
	// 一覧を StreamRouter が持つのは、Pipeline が持つ StreamRouter が 1 回の apply_configure より長く生きて、
	// AssignPolicy (apply_configure が const で受ける) に入れると facts から消せないからである。保存はしない。
	// OBS を起動し直すと一覧は空になり、外した人が画面共有を続けていれば、最初の configure で空き枠に載る
	// (起動をまたいで覚えるほどの価値はないという割り切りである)。
	void exclude_user(const std::string &user_id);
	void include_user(const std::string &user_id);
	void clear_excluded();
	// facts の sharing (Discord 側で今画面共有している人) にいない人を、外した人の一覧から消す。呼び出し元は
	// sharing_valid が true の facts のときだけ呼ぶ (sharing のキーがない facts では、誰がやめたか分からない)。
	void forget_stopped_sharing(const std::vector<std::string> &sharing_user_ids);
	bool is_excluded(const std::string &user_id) const;

private:
	static config::SlotKind to_slot_kind(ws::StreamKind k);

	// キーは stream_id だけ (ファイルの先頭のコメント)。
	std::map<uint32_t, Route> routes_;

	// 直前の configure に画面のストリームがあった人 (always_on の人が画面共有を始めたかを見るためだけに持つ)。
	// 経路や割り当てとは別に持つ。手動のときは枠に入れない人の経路を作らないので、経路の有無では「始めた」を
	// 見分けられない。clear() で忘れる (再接続の後の最初の configure では、どの人も「始めた」として扱う)。
	std::set<std::string> screen_sharers_;

	// 前の admit_sharers() に渡された、画面共有している人 (「始めた」を見るためだけに持つ。clear() で忘れる)。
	std::set<std::string> admitted_sharing_prev_;

	// 利用者が外した人 (exclude_user)。clear() では忘れない。
	std::set<std::string> excluded_;
};

// ---------------------------------------------------------------------------
// 実装。pipeline.cpp は libobs に依存して単体テストに入らないので、テストがインクルードだけで使えるように
// ヘッダーに置く。
// ---------------------------------------------------------------------------

inline config::SlotKind StreamRouter::to_slot_kind(ws::StreamKind k)
{
	return k == ws::StreamKind::Screen ? config::SlotKind::Screen : config::SlotKind::Voice;
}

inline std::vector<RouteChange> StreamRouter::apply_configure(const ws::ConfigureMessage &msg,
								config::AllocationTable &table,
								const AssignPolicy &policy)
{
	std::vector<RouteChange> changes;

	// 自分の枠に映す映像を OBS から選んでいる間は、メインアカウントの画面の枠を空ける (自分の枠と同じ人が 2 枚出ないように)。
	// 経路は下のループで消える (configure にそのストリームがなければ、すでに消えている)。
	const bool skip_main_screen = policy.self_frame_selected && !policy.main_account_user_id.empty();
	if (skip_main_screen) {
		const int main_slot = table.slot_of_owner(config::SlotKind::Screen, policy.main_account_user_id);
		if (main_slot != 0)
			table.release(config::SlotKind::Screen, main_slot);
	}
	// Vesktop のアカウントの画面は描けないので、持っている画面の枠を空ける (AssignPolicy の receiver_user_id)。
	if (!policy.receiver_user_id.empty()) {
		const int receiver_slot = table.slot_of_owner(config::SlotKind::Screen, policy.receiver_user_id);
		if (receiver_slot != 0)
			table.release(config::SlotKind::Screen, receiver_slot);
	}

	std::map<uint32_t, Route> new_routes;
	// この呼び出しの中で、経路が消えたともう返した stream_id。最後の「configure から消えた」の確認で、同じ
	// stream_id を 2 回返さないために持つ (枠に入れなかったストリームは configure に載ったまま経路だけが
	// なくなるので、新しい経路の表にもない)。1 つの stream_id に RouteChange を 2 つ返すと、呼び出し元が
	// 同じストリームを 2 回片付ける。
	std::set<uint32_t> dropped_ids;

	// always_on の人が画面共有を始めたかを見るための、この呼び出しで画面共有している人。決め方に関係なく
	// 毎回作り直して最後に保存する。手動のときだけ更新すると、自動から手動に戻した直後に全員が
	// 「いま始めた」に見える。
	std::set<std::string> screen_sharers_now;
	for (const auto &stream : msg.streams) {
		if (to_slot_kind(stream.kind) == config::SlotKind::Screen && !stream.user_id.empty())
			screen_sharers_now.insert(stream.user_id);
	}

	// 使う声 (AssignPolicy の voice_links の (1) から (4))。以下のコメントの O と S は、AssignPolicy のコメントと
	// 同じく、枠に載っている人と使う声に選んだ人を指す。組を 1 対 1 に絞ってから、S から O を引く表を作る。
	const std::map<std::string, std::string> voice_links = config::usable_voice_links(policy.voice_links);
	std::map<std::string, std::string> owner_of_source;
	for (const auto &[owner, source] : voice_links)
		owner_of_source.emplace(source, owner);

	for (const auto &stream : msg.streams) {
		const config::SlotKind kind = to_slot_kind(stream.kind);

		// メインアカウントの声に経路を作らないとき (AssignPolicy の main_account_user_id と route_main_account_voice) は、
		// 前の経路があれば消す。経路を作るときは、ほかの人の声と同じ道を通る。
		if (kind == config::SlotKind::Voice && !policy.route_main_account_voice &&
		    !policy.main_account_user_id.empty() && stream.user_id == policy.main_account_user_id) {
			const auto prev = routes_.find(stream.stream_id);
			if (prev != routes_.end()) {
				changes.push_back(RouteChange{stream.stream_id, prev->second, std::nullopt, "main_account_voice"});
				dropped_ids.insert(stream.stream_id);
			}
			// メインアカウントが自分の声で持っていた声の枠は空ける。使う声でほかの人の声を運んでいる枠は残す。
			const int own_slot = table.slot_of_owner(kind, stream.user_id);
			const config::SlotAssignment *own = own_slot != 0 ? table.assignment(kind, own_slot) : nullptr;
			if (own != nullptr && own->voice_user_id == stream.user_id && voice_links.count(stream.user_id) == 0)
				table.release(kind, own_slot);
			continue;
		}

		// voice_links の (2): O 自身の声には経路を作らない。前の経路があれば消す
		// (Insufficient と同じ形で、枠が足りないのではなく置き換えたためなので reason だけを変える)。
		if (kind == config::SlotKind::Voice && voice_links.count(stream.user_id) != 0) {
			const auto prev = routes_.find(stream.stream_id);
			if (prev != routes_.end()) {
				changes.push_back(RouteChange{stream.stream_id, prev->second, std::nullopt, "voice_replaced"});
				dropped_ids.insert(stream.stream_id);
			}
			continue;
		}

		// 全画面だけで映す人の経路 (AssignPolicy::focus_user_id)。枠を持たない人の画面のストリームを、割り当て表に触らずに
		// kFocusOnlySlot へ結ぶ。
		const auto route_focus_only = [&]() {
			const Route focus_route{kind, config::kFocusOnlySlot, stream.user_id};
			const auto prev = routes_.find(stream.stream_id);
			if (prev == routes_.end())
				changes.push_back(RouteChange{stream.stream_id, std::nullopt, focus_route, "focus_only"});
			else if (prev->second != focus_route)
				changes.push_back(RouteChange{stream.stream_id, prev->second, focus_route, "changed"});
			new_routes[stream.stream_id] = focus_route;
		};
		const bool focus_only = kind == config::SlotKind::Screen && !policy.focus_user_id.empty() &&
					stream.user_id == policy.focus_user_id &&
					table.slot_of_owner(kind, stream.user_id) == 0;

		// 画面の枠に入れてよい人かを先に見る (AssignPolicy)。声の枠はここを通らない。
		if (kind == config::SlotKind::Screen) {
			bool admit = true;
			if (!policy.receiver_user_id.empty() && stream.user_id == policy.receiver_user_id) {
				// Vesktop のアカウントの画面。送信側は視聴しないので普通は届かないが、届いても画面の枠に入れず、全画面だけで
				// 映す経路も作らない。前の経路があれば消す。
				const auto prev = routes_.find(stream.stream_id);
				if (prev != routes_.end()) {
					changes.push_back(RouteChange{stream.stream_id, prev->second, std::nullopt, "receiver"});
					dropped_ids.insert(stream.stream_id);
				}
				continue;
			}
			if (skip_main_screen && stream.user_id == policy.main_account_user_id) {
				// 自分の枠で出す人の画面は、画面の枠に入れない。経路を作らず、前の経路があれば消す。
				const auto prev = routes_.find(stream.stream_id);
				if (prev != routes_.end()) {
					changes.push_back(
						RouteChange{stream.stream_id, prev->second, std::nullopt, "self_frame_selected"});
					dropped_ids.insert(stream.stream_id);
				}
				continue;
			}
			if (policy.automatic) {
				admit = excluded_.count(stream.user_id) == 0;
			} else {
				const bool owns_slot = table.slot_of_owner(kind, stream.user_id) != 0;
				const bool always_on_started = policy.always_on.count(stream.user_id) != 0 &&
							       excluded_.count(stream.user_id) == 0 &&
							       screen_sharers_.count(stream.user_id) == 0 &&
							       screen_slot_room(table, policy);
				admit = owns_slot || always_on_started;
			}
			if (!admit && focus_only) {
				route_focus_only();
				continue;
			}
			if (!admit) {
				// 枠に入れない人。経路を作らないので、このストリームの映像はデコードもされない
				// (呼び出し元は経路の有無でデコードのスレッドを作る)。前の経路があれば消す
				// (枠が足りないのではなく、利用者が選んでいない、または外したためなので reason だけを
				// 変える)。
				const auto prev = routes_.find(stream.stream_id);
				if (prev != routes_.end()) {
					changes.push_back(
						RouteChange{stream.stream_id, prev->second, std::nullopt, "not_chosen"});
					dropped_ids.insert(stream.stream_id);
				}
				continue;
			}
		}

		// voice_links の (1): S の声は O の声の枠に入れる。枠を探す相手が O になるだけで、経路の user_id は
		// configure がこのストリームに付けた人 (S) のままである (Route)。
		std::string slot_owner = stream.user_id;
		if (kind == config::SlotKind::Voice) {
			const auto linked = owner_of_source.find(stream.user_id);
			if (linked != owner_of_source.end())
				slot_owner = linked->second;
		}
		// 画面の枠を持たない人は、載せられる人数 (screen_slot_room。自分の枠を使っている間は 3 人) に達していれば、空いている
		// 枠があっても載せず、枠が足りないときと同じに扱う (AssignPolicy の self_frame_in_use)。
		const bool over_capacity = kind == config::SlotKind::Screen && table.slot_of_owner(kind, slot_owner) == 0 &&
					   !screen_slot_room(table, policy);
		const auto result = over_capacity ? config::AssignResult{} : table.try_assign(kind, slot_owner);

		if (result.outcome == config::AssignOutcome::Insufficient && focus_only) {
			// 枠が足りずに載らない人を全画面で映すとき。
			route_focus_only();
			continue;
		}
		if (result.outcome == config::AssignOutcome::Insufficient) {
			// 割り当てられなかった。この stream_id は経路を持てないので、前の経路があれば消す。前の経路も
			// ない (初めて見る stream_id がそのまま Insufficient) ときは、経路がないまま変わらないので
			// RouteChange を返さない (呼び出し元には route() が値を持たないことで分かる)。
			const auto prev = routes_.find(stream.stream_id);
			if (prev != routes_.end()) {
				changes.push_back(RouteChange{stream.stream_id, prev->second, std::nullopt, "insufficient"});
				dropped_ids.insert(stream.stream_id);
			}
			continue;
		}

		const Route new_route{kind, result.slot, stream.user_id};

		// voice_links の (1) と (3): 声の枠の voice_user_id を、このストリームの人にする (置き換えていなければ
		// 枠に載っている人本人で、置き換えていれば使う声に選んだ人)。
		if (kind == config::SlotKind::Voice)
			table.set_voice_source(config::SlotKind::Voice, result.slot, stream.user_id);

		const auto prev = routes_.find(stream.stream_id);
		if (prev == routes_.end()) {
			const char *reason =
				(result.outcome == config::AssignOutcome::AlreadyOwned) ? "already_owned" : "assigned";
			changes.push_back(RouteChange{stream.stream_id, std::nullopt, new_route, reason});
		} else if (prev->second != new_route) {
			changes.push_back(RouteChange{stream.stream_id, prev->second, new_route, "changed"});
		}

		new_routes[stream.stream_id] = new_route;
	}

	// configure からなくなった stream_id は経路を消す。割り当ては残すので、割り当て表には触らない。
	for (const auto &[stream_id, old_route] : routes_) {
		if (new_routes.find(stream_id) == new_routes.end() && dropped_ids.count(stream_id) == 0)
			changes.push_back(RouteChange{stream_id, old_route, std::nullopt, "removed"});
	}

	routes_ = std::move(new_routes);
	screen_sharers_ = std::move(screen_sharers_now);

	// voice_links の (3): 声の枠の voice_user_id を、現在の組のとおりにそろえる。S が通話に参加していなくても
	// 組のとおりにする。その枠で使うのは S の声で、O の声は (2) で流していないので、表に O の userId を残すと、
	// 表のコピー (PipelineSnapshot の voice_user_id) を読む側に事実と違う値を渡す。組をやめた枠は O に戻す。
	// 経路の付け替えは上のループで済んでいる (S のストリームは自分の枠への経路に変わり、O のストリームは経路を
	// 取り戻す)。
	for (int slot = 1; slot <= table.slot_extent(config::SlotKind::Voice); ++slot) {
		const config::SlotAssignment *a = table.assignment(config::SlotKind::Voice, slot);
		if (a == nullptr)
			continue;
		const auto linked = voice_links.find(a->owner_user_id);
		const std::string &desired = linked != voice_links.end() ? linked->second : a->owner_user_id;
		if (a->voice_user_id != desired)
			table.set_voice_source(config::SlotKind::Voice, slot, desired);
	}

	return changes;
}

inline std::optional<Route> StreamRouter::route(uint32_t stream_id) const
{
	const auto it = routes_.find(stream_id);
	if (it == routes_.end())
		return std::nullopt;
	return it->second;
}

inline bool StreamRouter::output_allowed(uint32_t stream_id, const config::AllocationTable &table) const
{
	const auto it = routes_.find(stream_id);
	if (it == routes_.end())
		return false;
	// シーンコレクションの切り替えの最中 (凍結) は出さない (OBS のソースが
	// 壊されている途中のため)。
	if (table.is_frozen())
		return false;
	return table.is_output_allowed(it->second.kind, it->second.slot);
}

inline bool StreamRouter::video_allowed(uint32_t stream_id, const config::AllocationTable &table) const
{
	const auto it = routes_.find(stream_id);
	if (it == routes_.end())
		return false;
	// 凍結の扱いは output_allowed と同じ。
	if (table.is_frozen())
		return false;
	// 全画面だけで映す人の経路は、割り当て表の枠を持たないが、映像は出す (AssignPolicy::focus_user_id)。
	if (it->second.kind == config::SlotKind::Screen && it->second.slot == config::kFocusOnlySlot)
		return true;
	return table.is_video_allowed(it->second.kind, it->second.slot);
}

inline std::set<uint32_t> StreamRouter::route_ids() const
{
	std::set<uint32_t> ids;
	for (const auto &[stream_id, route] : routes_) {
		(void)route;
		ids.insert(stream_id);
	}
	return ids;
}

inline int StreamRouter::retarget_user(const std::string &user_id, config::SlotKind kind, int slot)
{
	if (user_id.empty())
		return 0;
	int changed = 0;
	for (auto &[stream_id, route] : routes_) {
		(void)stream_id;
		if (route.kind != kind || route.user_id != user_id || route.slot == slot)
			continue;
		route.slot = slot;
		++changed;
	}
	return changed;
}

inline void StreamRouter::clear()
{
	routes_.clear();
	screen_sharers_.clear();
	admitted_sharing_prev_.clear();
}

inline std::vector<std::string> StreamRouter::admit_sharers(const std::vector<std::string> &sharing_user_ids,
							    config::AllocationTable &table,
							    const AssignPolicy &policy)
{
	std::vector<std::string> admitted;
	const config::SlotKind kind = config::SlotKind::Screen;
	const bool skip_main = policy.self_frame_selected && !policy.main_account_user_id.empty();
	for (const std::string &user_id : sharing_user_ids) {
		if (user_id.empty() || (skip_main && user_id == policy.main_account_user_id))
			continue;
		// Vesktop のアカウントの画面共有は届かないので載せない (AssignPolicy の receiver_user_id)。
		if (!policy.receiver_user_id.empty() && user_id == policy.receiver_user_id)
			continue;
		if (table.slot_of_owner(kind, user_id) != 0 || excluded_.count(user_id) != 0)
			continue;
		if (!policy.automatic) {
			const bool started = admitted_sharing_prev_.count(user_id) == 0 && screen_sharers_.count(user_id) == 0;
			if (policy.always_on.count(user_id) == 0 || !started)
				continue;
		}
		// 載せられる人数 (自分の枠を使っている間は 3 人) に達していれば載せない (apply_configure と同じ規則)。
		if (!screen_slot_room(table, policy))
			continue;
		const auto result = table.try_assign(kind, user_id);
		if (result.outcome != config::AssignOutcome::Assigned)
			continue;
		admitted.push_back(user_id);
	}
	admitted_sharing_prev_ = std::set<std::string>(sharing_user_ids.begin(), sharing_user_ids.end());
	return admitted;
}

inline void StreamRouter::exclude_user(const std::string &user_id)
{
	if (!user_id.empty())
		excluded_.insert(user_id);
}

inline void StreamRouter::include_user(const std::string &user_id)
{
	excluded_.erase(user_id);
}

inline void StreamRouter::clear_excluded()
{
	excluded_.clear();
}

inline void StreamRouter::forget_stopped_sharing(const std::vector<std::string> &sharing_user_ids)
{
	const std::set<std::string> sharing(sharing_user_ids.begin(), sharing_user_ids.end());
	for (auto it = excluded_.begin(); it != excluded_.end();) {
		if (sharing.count(*it) == 0)
			it = excluded_.erase(it);
		else
			++it;
	}
}

inline bool StreamRouter::is_excluded(const std::string &user_id) const
{
	return excluded_.count(user_id) != 0;
}

} // namespace cordmux::pipeline

// SPDX-License-Identifier: Apache-2.0

// 受信側の処理の流れ。ws::Listener から届いた映像をデコードしてタイムスタンプを合わせ、音声と一緒に合成のソース
// (sources/composite_source.h) に渡す。録画 (record::Recorder) への受け渡し、送信側に送る media と limits の決定、
// ドックが読むスナップショットも受け持つ。主なエントリポイントは class Pipeline。この下の inline の関数は libobs に
// 依存しない純粋関数で、単体テストから使う。libobs に依存するのは Pipeline の実装 (pipeline.cpp) である。ws::Listener と割り当て表 (config::AllocationTable) の所有者は plugin-main.cpp で、Pipeline は
// 参照を借りるだけである。スレッドとロックの順は pipeline.cpp の Pipeline::Impl の mu の宣言にある。

#pragma once

#include "config/allocation.h"
#include "config/settings.h"
#include "config/stage_counters.h"
#include "diag/diag_log.h"
#include "install/backup_first_write.h"
#include "install/vencord_dir.h"
#include "ledger/text_ids.h"
#include "pipeline/arrival_window.h"
#include "pipeline/audio_mixer.h"
#include "pipeline/dummy_interval.h"
#include "pipeline/facts_state.h"
#include "pipeline/log_ring.h"
#include "pipeline/mixer_roster.h"
#include "pipeline/profiles_state.h"
#include "pipeline/receive_restart.h"
#include "pipeline/stream_router.h"
#include "record/record_types.h"
#include "ws/listener.h"
#include "ws/log_sink.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordmux::pipeline {

// stats() が返す計数。合否の判定には使わない (ログと診断情報で、どの段階でフレームを捨てたかを調べるための値)。映像と
// 音声を分けて持つ。画面のストリームは同じ stream_id で映像と音声の両方を送るので、1 つの計数に混ぜると、画面の
// ストリームについて受信とデコードのフレームレートを読めない (config/stage_counters.h の MediaKind)。totals_* は経路の
// ないメッセージの分 (no_route_*) も含む合計で、per_stream_* は経路のある stream_id だけの内訳である
// (config/stage_counters.h の StreamStageCounters は、stream_id ごとの表を無制限に増やさない)。
struct PipelineStats {
	config::StageCounters totals_video;
	config::StageCounters totals_audio;
	config::StageCounters no_route_video;
	config::StageCounters no_route_audio;
	std::map<std::uint32_t, config::StageCounters> per_stream_video;
	std::map<std::uint32_t, config::StageCounters> per_stream_audio;
};

// ---------------------------------------------------------------------------
// snapshot() が返すスナップショット
//
// ドックは周期 (既定 200 ms) ごとにこれを 1 つ取り、自分の設定と platform/os_probe.h の結果と合わせて ledger::Observation を作る。
// ドックは AllocationTable にも Listener の内部にも直接触らず、これだけを読む。値のコピーだけを持つ (参照もポインターも
// 持たない) ので、受け取った後に Pipeline が何をしていても読み続けられる。
// ---------------------------------------------------------------------------

// 割り当て表の 1 行のコピー (ドックに config::AllocationTable への参照を持たせないため)。
struct SlotSnapshot {
	config::SlotKind kind = config::SlotKind::Screen;
	int slot = 0;
	std::string owner_user_id;
	std::string voice_user_id; // 使う声を置き換えた声の枠でだけ owner と異なる
	// 音声 (画面の音と声) を出してよいか (AllocationTable::is_output_allowed。割り当てがなければ false)。
	bool output_allowed = false;
	// 画面の枠の映像を出してよいか (AllocationTable::is_video_allowed。割り当てがないか声の枠なら false)。映像を数える所と、
	// 枠に載っている人の名前を使う所 (自動配置で「見えている」枠、名前ラベル、OBS のソース名) はこちらを見る。
	// 声の枠ではいつも false (声の枠は映像を運ばない)。output_allowed の意味を映像に
	// 変えて音声のフィールドを別に追加すると、現在の読む側 (ドックの音声の扱いとテスト) が、意味の変わった
	// フィールドを気付かずに読み続ける。映像の判定を別のフィールドにすれば、映像に移す所が名前で分かる。
	bool video_allowed = false;
	// 載せた順の番号 (SlotAssignment::added_order)。配信の位置の決め方が「載せた順」のときに読む。
	std::int64_t added_order = 0;
	// 画面の枠: 枠に載っている人の画面共有のストリームが、いま経路を持っているか。枠に載せた直後や、繋ぎ直した
	// 直後は、映像がまだ届かず経路がない。output_allowed だけでは、そのような空の枠を見えている枠に数えてしまう。
	// 自動配置はこのフィールドでも絞る。声の枠では使わない (いつも false)。
	bool share_present = false;
	// 声の枠: 声のストリームが、いまこの枠への経路を持っているか (mark_voice_present)。割り当て表は、通話の参加者が分からない間
	// 枠の割り当てを残すので、割り当てだけでは、cordmux がいまその人の声を受け取っているかが分からない。合成のソースに渡す、自分の枠の人の声を聞けるか (sources/composite_input.h の self_hears_voice) を
	// 決めるのに使う。画面の枠では使わない (いつも false)。
	bool voice_present = false;
	// 画面の枠: Vesktop との接続が切れたときに画面の経路を持っていて、いまも同じ人が載っている枠 (mark_link_lost_hold)。
	// 接続が切れると経路は捨てるので share_present は偽になるが、配置は並べ直さずに保つ (docs/spec.md の「枠と配置」)。
	// 声の枠では使わない (いつも false)。
	bool held_after_link_lost = false;
	// held_after_link_lost の枠で、最後のフレームを残す長さ (link_lost_hold_ms) がたち、映像を空けた後か。配置は保つが、
	// その人は映っていないので、全画面を続けない (docs/spec.md の全画面の「接続が切れて枠が空いた」)。
	bool held_frame_cleared = false;
};

// ストリーム (stream_id) ごとの、最後にフレームが届いた時刻。ドックが、映像が届いていない人を表示するとき (人の行の
// cordmux.dock.row_not_receiving.status と、メッセージの cordmux.dock.no_video.headline) に読む。
// steady_clock とシステム時刻の両方を持つ。経過時間は steady_clock で測り (システム時刻を合わせ直しても値が
// 不連続にならない)、画面に時刻として表示するのはシステム時刻である。user_id と kind と slot は、そのフレームが
// 届いた時点の経路である (枠を空けて別の人が載ることがあるので、届いた時点の値を残す)。
struct StreamFrameClock {
	std::uint32_t stream_id = 0;
	std::string user_id;
	config::SlotKind kind = config::SlotKind::Screen;
	int slot = 0;
	std::int64_t last_video_frame_steady_us = 0;
	std::int64_t last_video_frame_wall_us = 0;
};

// 録画に渡した映像と音声の、ストリーム (stream_id) ごとの最後の時刻。
//
// last_frames とは別の表である。last_frames は pipeline.cpp の経路がある所でしか書かず、経路のないストリームの行は
// retain_frame_clocks が取り除く。手動が既定なので、枠に載っていない人には経路がなく、
// 「枠に載っていないのにフレームは届いている」を表す場所がほかにない。last_frames に経路のないストリームの行も
// 残すと、StreamFrameClock は届いた時点の経路を持つという前提が崩れ、その経路を読む「映像が届いていない」の判定にも
// 影響するので、表を分ける。
//
// 経路を持たないので kind と slot は持たない。steady_clock とシステム時刻の両方を持つ理由は StreamFrameClock と同じ。
struct RecordOnlyFrameClock {
	std::uint32_t stream_id = 0;
	std::string user_id;
	bool screen = false; // 画面のストリームか (声のストリームは録画でも解像度を持たない)
	std::int64_t last_frame_steady_us = 0;
	std::int64_t last_frame_wall_us = 0;
};

// ストリーム (stream_id) ごとの、いま届いている映像の形 (configure の width、height、fps)。
//
// 0 は「まだ分からない」を表す (送信側は、最初のエンコードの段階を決めるまでと、段階が unavailable の間は、3 つとも
// キーごと省く)。画面に「0x0 @ 0」と表示しないように、表示する側 (dock/build_observation.cpp) が 0 を除く。
struct StreamFormat {
	std::uint32_t stream_id = 0;
	std::string user_id;
	bool screen = false; // 画面のストリームか (声のストリームは解像度を持たない)
	int width = 0;
	int height = 0;
	double fps = 0.0;
	bool audio = false; // 音声があるか (configure の audio。画面のストリームなら、画面共有の音を持つ)
};

// 参加者ごとの音量とミュートのコピー (snapshot() の中身)。合成のソースが出す混ぜた音に掛かる値で、ミュートは覚えない
// (画面共有の音は共有が始まるたびにミュートから始まり、自分の声のミュートだけが settings.json に残る)。
struct PersonAudioSnapshot {
	bool voice_muted = false;
	int voice_volume_pct = config::kPersonVolumeDefaultPct;
	bool screen_muted = false;
	int screen_volume_pct = config::kPersonVolumeDefaultPct;
	// 画面共有の音を持っているか (configure に、この人の音声のある画面のストリームがある)。
	bool has_screen_audio = false;
};

// 前の一覧と新しい一覧 (userId か stream_id) の、新しい一覧の数と、増えた数と、減った数。出来事の行に、通話の参加者の出入り、画面共有の
// 始まりと終わり、送信側のストリームの追加と終了を書くために使う (Facts と configure の行は、届くたびに書く統計の行なので、
// 通話の全体を残す出来事の行には入らない)。ログには数だけを書き、userId は書かない。
struct MemberChange {
	std::size_t now = 0;
	std::size_t added = 0;
	std::size_t removed = 0;
	bool changed() const { return added != 0 || removed != 0; }
};

template <typename T> MemberChange member_change(const std::vector<T> &before, const std::vector<T> &after)
{
	const std::set<T> b(before.begin(), before.end());
	const std::set<T> a(after.begin(), after.end());
	MemberChange c;
	c.now = a.size();
	for (const T &x : a)
		c.added += b.count(x) == 0 ? 1 : 0;
	for (const T &x : b)
		c.removed += a.count(x) == 0 ? 1 : 0;
	return c;
}

// live_ids (いま経路を持つ stream_id) にないストリームの行を取り除く (config/stage_counters.h の retain と同じく、
// stream_id ごとの表を無制限に増やさない)。取り除き忘れた行があると、その人の行に「映像が届いていません」
// (cordmux.dock.row_not_receiving.status) が表示され続ける。pipeline.cpp は libobs に依存して単体テストに入らないので、
// ここに純粋関数で置く。
inline void retain_frame_clocks(std::map<std::uint32_t, StreamFrameClock> &clocks,
				const std::set<std::uint32_t> &live_ids)
{
	for (auto it = clocks.begin(); it != clocks.end();)
		it = (live_ids.count(it->first) == 0) ? clocks.erase(it) : std::next(it);
}

// 割り当て表から、割り当てのある枠だけを、画面 1 から 4、声 1 からの順に並べて返す (snapshot() の中身)。表は
// 読むだけで書かない。呼び出し元は表を守るロックを持っていること (snapshot() は Impl::mu の中で呼ぶ)。並び順は、
// ドックが人の一覧を作る順でもある (dock/build_observation.cpp は、先に来た画面の枠の番号をその人の枠とする) ので、
// 純粋関数にして単体テストで確認する。
inline std::vector<SlotSnapshot> collect_slot_snapshots(const config::AllocationTable &table)
{
	std::vector<SlotSnapshot> out;
	const config::SlotKind kinds[] = {config::SlotKind::Screen, config::SlotKind::Voice};
	for (const config::SlotKind kind : kinds) {
		for (int slot = 1; slot <= table.slot_extent(kind); ++slot) {
			const config::SlotAssignment *assignment = table.assignment(kind, slot);
			if (!assignment)
				continue;
			SlotSnapshot row;
			row.kind = kind;
			row.slot = slot;
			row.owner_user_id = assignment->owner_user_id;
			row.voice_user_id = assignment->voice_user_id;
			row.added_order = assignment->added_order;
			row.output_allowed = table.is_output_allowed(kind, slot);
			row.video_allowed = table.is_video_allowed(kind, slot);
			out.push_back(std::move(row));
		}
	}
	return out;
}

// いま画面の経路を持っている枠の番号 (枠 0、つまり枠から外した人の経路は数えない)。呼び出し元は経路を守るロック
// (Impl::mu) を持っていること。
inline std::set<int> live_screen_slots(const StreamRouter &router)
{
	std::set<int> out;
	for (const std::uint32_t id : router.route_ids()) {
		const std::optional<Route> route = router.route(id);
		if (route && route->kind == config::SlotKind::Screen && route->slot >= 1)
			out.insert(route->slot);
	}
	return out;
}

// 画面の枠の行に share_present を入れる (live は live_screen_slots の結果)。
inline void mark_share_present(std::vector<SlotSnapshot> &slots, const std::set<int> &live)
{
	for (SlotSnapshot &row : slots)
		row.share_present = row.kind == config::SlotKind::Screen && live.count(row.slot) != 0;
}

// いま画面の経路を持っている枠と、その経路の人 (枠の番号 -> user_id。枠 0 は数えない)。呼び出し元は経路を守るロック
// (Impl::mu) を持っていること。
inline std::map<int, std::string> live_screen_owners(const StreamRouter &router)
{
	std::map<int, std::string> out;
	for (const std::uint32_t id : router.route_ids()) {
		const std::optional<Route> route = router.route(id);
		if (route && route->kind == config::SlotKind::Screen && route->slot >= 1)
			out[route->slot] = route->user_id;
	}
	return out;
}

// Vesktop との接続が切れたときに、配置を保つ枠。
//
// 接続が切れると、経路と画面のデコードを捨てる (次の接続は同じ stream_id を別の人に使うことがある)。経路がないと
// share_present が偽になり、ドックの次の周期で合成のソースが並べ直してしまう。接続の途切れからは同じ人が戻ることが多く、
// 並べ直すと戻ったときにもう一度配置が変わるので、切れたときに映っていた枠は、配置の上では見えている枠のままにする
// (sources/screen_end.h の LinkLost。映像は link_lost_hold_ms だけ最後のフレームを残してから空ける)。
//
// 保つのは、切れている間と、接続し直してから window_us の間 (Pipeline は settings の reconnect_layout_hold_ms を渡す。
// 既定は 10 秒) である。接続し直した直後は、画面のストリームが届くまでに間があるので、その間に並べ直すと、届いたときに
// また並べ直すことになる。その間に届かない人は、届いていない枠として扱う (並べない)。
struct LinkLostLayoutHold {
	// 切れたときに画面の経路を持っていた枠と、その人 (枠の番号 -> user_id)。
	std::map<int, std::string> owners;
	// 切れた時刻 (steady_clock の us)。最後のフレームを残す長さを、ここから数える。
	std::int64_t lost_at_us = 0;
	// 切れた後に接続し直した時刻 (steady_clock の us)。切れている間は値を持たない。
	std::optional<std::int64_t> reconnected_at_us;
};

// いま配置を保つ枠 (枠の番号 -> 切れたときの人)。
inline std::map<int, std::string> link_lost_layout_owners(const LinkLostLayoutHold &hold, std::int64_t now_us,
							  std::int64_t window_us)
{
	if (hold.reconnected_at_us.has_value() && now_us - *hold.reconnected_at_us >= window_us)
		return {};
	return hold.owners;
}

// 画面の枠の行に held_after_link_lost と held_frame_cleared を入れる (held は link_lost_layout_owners の結果)。
// 切れたときと同じ人が載っている枠だけを保つ (枠が空いた後に別の人が載ったら、その人の映像が届くまでは並べない)。
// frame_cleared は、最後のフレームを残す長さがたったか (呼び出し元が切れた時刻と link_lost_hold_ms から決める)。
inline void mark_link_lost_hold(std::vector<SlotSnapshot> &slots, const std::map<int, std::string> &held,
				bool frame_cleared)
{
	for (SlotSnapshot &row : slots) {
		const auto it = held.find(row.slot);
		row.held_after_link_lost =
			row.kind == config::SlotKind::Screen && it != held.end() && it->second == row.owner_user_id;
		row.held_frame_cleared = row.held_after_link_lost && frame_cleared;
	}
}

// 配置は保っているが、映像を空けた後の人 (映像が戻って経路を持った人は除く)。全画面にできる人から除く。
inline std::set<std::string> link_lost_emptied_owners(const std::vector<SlotSnapshot> &slots)
{
	std::set<std::string> out;
	for (const SlotSnapshot &row : slots) {
		if (row.held_frame_cleared && !row.share_present)
			out.insert(row.owner_user_id);
	}
	return out;
}

// 配置を保っている枠に、最後のフレームを残している人がいるか (映像が戻っておらず、まだ空けていない)。この間、合成の
// ソースの名前ラベルと参加者の並びも、切れる前の見た目を保つ (dock/dock_poll_layout.cpp)。
inline bool link_lost_frame_held(const std::vector<SlotSnapshot> &slots)
{
	for (const SlotSnapshot &row : slots) {
		if (row.held_after_link_lost && !row.held_frame_cleared && !row.share_present)
			return true;
	}
	return false;
}

// いま声の経路を持っている声の枠の番号 (live_screen_slots と同じ約束)。
inline std::set<int> live_voice_slots(const StreamRouter &router)
{
	std::set<int> out;
	for (const std::uint32_t id : router.route_ids()) {
		const std::optional<Route> route = router.route(id);
		if (route && route->kind == config::SlotKind::Voice && route->slot >= 1)
			out.insert(route->slot);
	}
	return out;
}

// 声の枠の行に voice_present を入れる (live は live_voice_slots の結果)。
inline void mark_voice_present(std::vector<SlotSnapshot> &slots, const std::set<int> &live)
{
	for (SlotSnapshot &row : slots)
		row.voice_present = row.kind == config::SlotKind::Voice && live.count(row.slot) != 0;
}

// 1 つの configure を適用した後で、画面共有が終わった画面の枠を返す。適用の前は画面の経路があり、適用の後はその枠への
// 画面の経路が 1 つもない枠である。configure からストリームが消えた (画面共有をやめた、通話から抜けた) か、枠に
// 入れなくなった。同じ configure の中で同じ枠に別のストリームが入った枠 (同じ人が別の stream_id で画面共有し直した、
// 別の人が載った) は返さない (枠は空にならず、次のストリームの最初のフレームで載っている人が替わる)。経路の
// 付け替えを 1 つずつ見ると、画面共有し直したときに一度枠を空けてしまう。
inline std::set<int> screen_slots_share_ended(const std::vector<RouteChange> &changes, const std::set<int> &live_after)
{
	std::set<int> out;
	for (const RouteChange &change : changes) {
		if (!change.before || change.before->kind != config::SlotKind::Screen || change.before->slot < 1)
			continue;
		if (live_after.count(change.before->slot) == 0)
			out.insert(change.before->slot);
	}
	return out;
}

// 自動配置が「見えている」と数える画面の枠 (映像を出してよく、載っている人の画面共有のストリームがいまある枠)。昇順。
// 枠に載せた直後で映像がまだ届かない人の枠は、空の枠なので数えない (数えると、空の枠の場所を空けたまま並べる)。
// 「出してよい」は映像の判定 (video_allowed) で見る。Vesktop との接続が切れたときに映っていた枠も数える
// (held_after_link_lost。並べ直さない)。
inline std::vector<int> visible_screen_slots(const std::vector<SlotSnapshot> &slots)
{
	std::vector<int> out;
	for (const SlotSnapshot &row : slots) {
		if (row.kind == config::SlotKind::Screen && row.video_allowed &&
		    (row.share_present || row.held_after_link_lost))
			out.push_back(row.slot);
	}
	return out;
}

// 録画ファイルの名前に使う語と書式。ini の 3 つの文字列 (画面共有の語、声の語、名前の書式) をそのまま持つ。
struct RecordNameWords {
	std::string screen_kind_word;
	std::string voice_kind_word;
	std::string output_name_format;
};

// 録画ファイルの名前に使う語と書式を、ini の文字列で埋める。text はキーから文字列を引く関数で、製品は Pipeline の
// コンストラクター (obs_module_load の中) で obs_module_text を渡す (OBS は obs_module_load より前に
// obs_module_set_locale を呼ぶので、ini は読み込み済み)。コンストラクターで埋めるので、どの録画の開始と
// [作成を再開] (cordmux.dock.resume_finalizing.button) よりも前に入り、ドックが作られない起動でも
// record/record_types.h の英語の既定 ("Screen Share"、"Voice") にならない。文字列が引けなければ (null) 空のまま返し、
// そのときだけ録画の側の既定になる。
inline RecordNameWords record_name_words(const std::function<const char *(const char *key)> &text)
{
	RecordNameWords words;
	if (!text)
		return words;
	const auto pick = [&text](const char *key) {
		const char *value = text(key);
		return value != nullptr ? std::string(value) : std::string();
	};
	words.screen_kind_word = pick(ledger::text::keys::kSourceFileScreenShareLabel);
	words.voice_kind_word = pick(ledger::text::keys::kSourceFileVoiceLabel);
	words.output_name_format = pick(ledger::text::keys::kSourceFileNameLabel);
	return words;
}

// ---------------------------------------------------------------------------
// 起動と、送信側に送る値の判断 (純粋関数)
//
// pipeline.cpp と plugin-main.cpp は libobs に依存して単体テストに入らないので、利用者に見える結果に直接つながる
// 判断を、ここに純粋関数で置いて tests/test_pipeline_wiring.cpp で確認する。送信側に何を送るか (media と
// limits)、起動時に Vesktop の設定を書くか、settings.json を書いてよいか、の 3 つである。起動時の
// 書き直し (run_startup_vencord_dir_rewrite) は Pipeline の仕事ではないが、呼ぶのは plugin-main.cpp だけで、
// 単体テストから使えるファイルがほかにないのでここに置く。
// ---------------------------------------------------------------------------

// 現在のシーンコレクションに合成のソース (sources/composite_source.h) があるか。読むのは pipeline.cpp
// (sources::composite_present)。
struct LayoutPresence {
	bool composite = false;
};

// 送信側に映像と声を読み出させる cordmux のソースがあるか。音も映像もいつも合成のソースから出すので、合成のソースの有無で決まる。
inline bool any_cordmux_source(const LayoutPresence &presence)
{
	return presence.composite;
}

// decide_media に渡す値。
struct MediaInputs {
	bool feature_enabled = true;          // settings.feature_enabled (cordmux を有効にしているか)
	bool loading_finished = false;        // シーンコレクションの読み込みが終わったか
	bool cordmux_sources_present = false; // any_cordmux_source の値 (読み込みの前は意味を持たない)
	bool recording_on = false;            // cordmux の録画をしているか (Pipeline のメモリにある値)
};

// media の値。送信側に、映像と声を読み出してエンコードしてよいかを伝える値で、決めるのは受信側のここだけである。
// cordmux が有効で、現在のシーンコレクションに合成のソースがあるか、録画をしているときに true。
// シーンコレクションを読み終える前は、ソースがないものとして扱う。obs_module_load の時点ではまだどの
// シーンコレクションも読んでおらず、configure も読み込みが終わるまで保留するので、この間に送信側が読み出しと
// エンコードをしても渡す先がない。読み終えた直後に値を決め直して送るので、先に Vesktop が接続していても、読み終えた
// 時点で送信側が読み出しを始める。
inline bool decide_media(const MediaInputs &in)
{
	if (!in.feature_enabled)
		return false;
	if (in.recording_on)
		return true;
	return in.loading_finished && in.cordmux_sources_present;
}

// Pipeline が持つ値から decide_media に渡す値を作る (pipeline.cpp の media_value_locked)。presence は最後に調べた
// 合成のソースの有無で、値を持たないのは、まだ一度も調べていないとき。調べる前は、読み込みが終わっていても
// 終わっていないものとして扱う。シーンコレクションの切り替えの最中は、切り替える前に調べた値をそのまま渡す
// (切り替えのたびに、送信側にゲートを閉じてまた開かせない)。
inline MediaInputs media_inputs_of(bool feature_enabled, bool loading_finished,
				   const std::optional<LayoutPresence> &presence, bool recording_on)
{
	MediaInputs in;
	in.feature_enabled = feature_enabled;
	in.loading_finished = loading_finished && presence.has_value();
	in.cordmux_sources_present = presence.has_value() && any_cordmux_source(*presence);
	in.recording_on = recording_on;
	return in;
}

// settings の画質の上限 (config/settings.h の limit_max_long_edge と limit_fps_divisor) とエンコードの設定
// (encoder_preference と encode_priority) を、制御用の JSON の limits の形にする (docs/wire_format.md の「メッセージの
// 種類」)。0 (制限なし) なら max_long_edge を省く。fps_divisor は 2 のときだけ載せ、1 (そのまま) は省く。ワイヤー
// 形式では省いたときと 1 は同じ意味だが、既定の設定で送る値が送信側の既定 (何も届いていないときの {}) と同じ形に
// なり、facts で返る値とそのまま比べられる。settings の側で範囲を確認してある (0 か 16 から 7680、1 か 2) が、範囲の
// 外の値が来ても、ワイヤー形式の範囲の外の値 (Listener::send_limits が捨てる値) は作らない。エンコードの設定も同じで、
// 既定値 (ハードウェア、速さ) のときはキーを省き、ソフトウェアのときだけ encoder_preference を "software"、
// 画質のときだけ latency_mode を "quality" にして載せる。
inline ws::LimitsMessage
limits_from_settings(int max_long_edge, int fps_divisor,
		     config::EncoderPreference encoder_preference = config::EncoderPreference::Hardware,
		     config::EncodePriority encode_priority = config::EncodePriority::Speed)
{
	ws::LimitsMessage out;
	if (max_long_edge > 0)
		out.max_long_edge = static_cast<std::uint32_t>(max_long_edge);
	if (fps_divisor == 2)
		out.fps_divisor = 2;
	if (encoder_preference == config::EncoderPreference::Software)
		out.encoder_preference = "software";
	if (encode_priority == config::EncodePriority::Quality)
		out.latency_mode = "quality";
	return out;
}

// settings.json を読んだ結果。
enum class SettingsFileState {
	Absent,     // ない (初めての起動では普通のこと。既定値で始め、ドックは書いてよい)
	Read,       // 読めた (中身の形はこの後 config::parse_settings が確認する)
	Unreadable, // あるのに読めない (開けない、大きすぎる。ドックは 1 バイトも書かない)
};

// platform::read_file_bytes は「ない」と「開けない」を同じ false で返すので、読めなかったときだけ、フォルダーの一覧
// (platform::list_directory) にその名前があるかで分ける。
//   read_ok       read_file_bytes が読めたか
//   read_text     読めた中身 (read_ok が false なら見ない)
//   folder_listed フォルダーの一覧を取れたか (フォルダーごとなければ false)
//   entry_listed  一覧にその名前があったか (フォルダーでも「ある」とする)
// 一覧を取れなかったら「ない」とする。フォルダーがないのは初めての起動の普通の形で、ドックはフォルダーを作ってから書く
// (dock/dock_bind.h の module_config_file_path)。一覧も取れないフォルダーには書くのも失敗するので、
// 既定値で上書きするおそれはない。読めた中身が空か空白だけなら「ない」とする。このファイルには利用者の指定が 1 つも
// 入っていない。「読めない」にすると、以後の起動で毎回ドックが設定を書かず (dock/dock_poll_config.h の
// settings_file_unreadable)、利用者が手でファイルを消すまで設定が保存されない。既定値で上書きしても失うものがないので、
// 初めての起動と同じに扱う。
inline bool settings_text_is_blank(const std::string &text)
{
	for (const char c : text) {
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			return false;
	}
	return true;
}

inline SettingsFileState classify_settings_file(bool read_ok, const std::string &read_text, bool folder_listed,
						bool entry_listed)
{
	if (read_ok)
		return settings_text_is_blank(read_text) ? SettingsFileState::Absent : SettingsFileState::Read;
	if (folder_listed && entry_listed)
		return SettingsFileState::Unreadable;
	return SettingsFileState::Absent;
}

// Vesktop の設定 (state.json) を書いてよいか。settings.json があるのに読めなかったときは false にする。そのとき
// settings の値は既定値 (allow_state_json_write は true) なので、そのまま使うと、実機テストの実行スクリプトが初期設定の
// 書き込み (test/tools/obs-run/seed-config.py の --real-profile) で書いた false を無視して、実際の state.json を
// 書いてしまう。config::parse_settings はキーが 1 つでも範囲の外ならファイル全体を拒むので、製品とテストのツールで
// 範囲が食い違ったときにも起きる。分からないときは書かない。plugin-main.cpp はこの値を settings に入れ直し、起動時の
// 書き直しと、ドックが Vesktop の設定を書く操作 ([Vesktopに追加] やアンインストールなど) がその値を使う。
inline bool effective_allow_state_json_write(bool setting_value, bool settings_file_unreadable)
{
	return setting_value && !settings_file_unreadable;
}

// ---- 起動時の書き直し (plugin-main.cpp の obs_module_load から呼ぶ) ----
//
// 書くかどうかは install::plan_write(..., WriteScene::Startup) が決める (起動時のための別の判断の関数は作らない)。
// ここが持つのは、その前と後の処理 (書き直しをしない 3 つの条件、読む順、書く順、失敗の扱い) だけである。OS を扱う
// 関数は引数で渡す (テストはテスト用のファイルの表を渡す)。
struct StartupRewriteInputs {
	bool uninstalling = false;           // アンインストールの最中か (install::is_uninstalling())
	bool feature_enabled = true;         // cordmux が有効か (settings.feature_enabled)
	bool allow_state_json_write = false; // effective_allow_state_json_write を通した後の settings の値
	std::string target_dir;              // 同梱した Vencord のフォルダー (data/vencord。ドックと同じ作り方)
	std::string backup_json_path;        // バックアップのファイル (vencord_dir_backup.json) のパス
	std::string dll_dir;                 // 現在の cordmux.dll のフォルダー (バックアップの dll_dir。ドックと同じ値)
	std::size_t max_json_bytes = 0;      // state.json とバックアップを読む上限 (platform/os_probe.h と同じ値)
	std::int64_t now_us = 0;             // 現在の時刻 (エポックからのマイクロ秒)
};

struct StartupRewriteOps {
	std::function<bool(std::string &)> vesktop_state_json_path;
	std::function<bool(const std::string &, std::string &, std::size_t)> read_file_bytes;
	std::function<bool(const std::string &, const std::string &)> write_file_atomic;
	// Vesktop が今起動しているか (実際の関数は platform::vesktop_running_now)。調べられなければ nullopt。書く直前に
	// 1 回だけ呼ぶ。空なら調べられないとみなし、書かない (起動していると分かったときと同じく、後で書き直す)。
	std::function<std::optional<bool>()> vesktop_running;
};

enum class StartupRewriteOutcome {
	SkippedUninstalling, // アンインストールの最中 (起動の時点では起きないが、書き直しをしない条件として先に見る)
	SkippedFeatureOff,   // cordmux が無効 (起動時の書き直しもしない)
	NoTarget,            // 書きたい場所か、バックアップの場所が分からない
	StateJsonUnread,     // Vesktop の設定の場所が分からないか読めない (Vesktop を一度も起動していないときを含む)
	AlreadyOurs,         // もう同梱の場所を指している
	NotThisScene,        // Refuse(SceneNotAllowed): バックアップがないか、元の値に戻っている。普通の起動
	Refused,             // それ以外の Refuse (利用者かほかのツールが変えた、JSON として読めない、など)
	BlockedBySetting,    // 書く計算は通ったが allow_state_json_write が false
	// 書く計算は通ったが、書く直前に Vesktop が起動していた (VesktopRunning) か、起動しているかを調べられなかった
	// (VesktopUnknown)。何も書いていない。ドックが、Vesktop が起動していないと分かったときに、もう一度この関数を呼ぶ
	// (dock/dock_poll_install.cpp の retry_startup_rewrite。plugin-main.cpp の起動時の書き直しのコメント)。
	VesktopRunning,
	VesktopUnknown,
	BackupWriteFailed,   // バックアップを書けなかった (state.json には触っていない)
	StateWriteFailed,    // バックアップは書けたが state.json を書けなかった (バックアップは元に戻そうとする)
	Wrote,               // 書き直した (診断情報の ledger::Install::rewrote_at_startup に入る)
};

struct StartupRewriteResult {
	StartupRewriteOutcome outcome = StartupRewriteOutcome::NoTarget;
	install::RefuseReason refuse_reason = install::RefuseReason::BrokenJson; // Refused のときだけ意味を持つ
	bool backup_restored = false; // StateWriteFailed のときだけ: バックアップを書く前の中身に戻せたか
};

// 起動時の書き直しを 1 回行う。書くのは plan_write が Write を返し、設定が書き込みを許し、書く直前に Vesktop が
// 起動していないと分かったときだけである。利用者が操作していないのに書く場合 (バックアップがない、元の値に戻っている) は、
// plan_write が Refuse(SceneNotAllowed) を返すのでここまで来ない。Vesktop が起動していたときは VesktopRunning、
// 起動しているかを調べられなかったときは VesktopUnknown を返し、何も書かない。呼び出し元は、Vesktop が起動して
// いないと分かったときに、同じ入力でもう一度呼ぶ (startup_rewrite_waits_for_vesktop)。
//
// 書く順と戻し方は install::write_backup_then_state にあり、[Vesktopに追加] (dock/dock_poll_install.cpp の
// write_vencord_dir) と同じである。バックアップを先に書き、書けたときだけ state.json を書く (元の値を持つファイルが
// 1 つも残らない状態を作らない)。state.json を書けなかったら、バックアップを書く前の中身に戻そうとする。
// 起動時の書き直しは利用者の操作ではないので、バックアップだけが進んだまま残ると、次の起動で「利用者か
// ほかのツールが変えた」に見える。ドックはメッセージを表示し、ダイアログは、cordmux 自身の古いフォルダーなのに
// 「いまVesktopは別のVencordを使っています。」(cordmux.dialog.vesktop_switch_current.text) と表示する。戻せば、次の起動で同じ書き直しをもう一度試す。
inline StartupRewriteResult run_startup_vencord_dir_rewrite(const StartupRewriteInputs &in,
							    const StartupRewriteOps &ops)
{
	StartupRewriteResult out;
	if (in.uninstalling) {
		out.outcome = StartupRewriteOutcome::SkippedUninstalling;
		return out;
	}
	if (!in.feature_enabled) {
		out.outcome = StartupRewriteOutcome::SkippedFeatureOff;
		return out;
	}
	if (in.target_dir.empty() || in.backup_json_path.empty()) {
		out.outcome = StartupRewriteOutcome::NoTarget;
		return out;
	}

	// バックアップ。読めないか形が違うなら、なかったものとして渡す (plan_write の約束のとおり。バックアップが
	// なければ、起動時は書かない)。
	std::string backup_text;
	std::optional<install::Backup> backup;
	if (ops.read_file_bytes && ops.read_file_bytes(in.backup_json_path, backup_text, in.max_json_bytes)) {
		install::Backup parsed;
		if (install::backup_from_json(backup_text, &parsed))
			backup = parsed;
	}

	// 書き直す state.json は、バックアップに残した、前に書いた場所を先に使う。現在の環境変数から決めた場所が違っていても、
	// 前に書いた場所を書き直す (別の場所の state.json に、書いていない cordmux の値を足さない)。
	std::string state_path;
	std::string state_text;
	if (backup.has_value() && !backup->state_path.empty())
		state_path = backup->state_path;
	else if (!ops.vesktop_state_json_path || !ops.vesktop_state_json_path(state_path))
		state_path.clear();
	if (state_path.empty() || !ops.read_file_bytes ||
	    !ops.read_file_bytes(state_path, state_text, in.max_json_bytes)) {
		out.outcome = StartupRewriteOutcome::StateJsonUnread;
		return out;
	}

	const install::WritePlan plan = install::plan_write(state_text, in.target_dir, backup, in.now_us, in.dll_dir,
							     install::WriteScene::Startup);
	switch (plan.action) {
	case install::WriteAction::AlreadyOurs:
		out.outcome = StartupRewriteOutcome::AlreadyOurs;
		return out;
	case install::WriteAction::Refuse:
		if (plan.refuse_reason == install::RefuseReason::SceneNotAllowed) {
			out.outcome = StartupRewriteOutcome::NotThisScene;
		} else {
			out.outcome = StartupRewriteOutcome::Refused;
			out.refuse_reason = plan.refuse_reason;
		}
		return out;
	case install::WriteAction::Write:
		break;
	}

	if (!in.allow_state_json_write) {
		out.outcome = StartupRewriteOutcome::BlockedBySetting;
		return out;
	}
	if (!plan.new_backup.has_value()) {
		out.outcome = StartupRewriteOutcome::BackupWriteFailed;
		return out;
	}
	// 書く直前に、Vesktop が起動していないことを確認する。起動中の Vesktop は、起動したときに読んだ state.json の値を
	// メモリに持ち、ウィンドウを動かすたびにメモリの値の全体で state.json を書き直すので、起動中に書いた値は知らされずに
	// 消える (ledger/actions.cpp の vesktop_busy_reason)。起動しているか調べられないときも書かない。
	const std::optional<bool> running = ops.vesktop_running ? ops.vesktop_running() : std::nullopt;
	if (!running.has_value()) {
		out.outcome = StartupRewriteOutcome::VesktopUnknown;
		return out;
	}
	if (*running) {
		out.outcome = StartupRewriteOutcome::VesktopRunning;
		return out;
	}
	// 起動時に書くのはバックアップがあるときだけなので、書く前の中身は読める (delete_file と file_known_absent は
	// 不要である)。
	install::BackupFirstOps write_ops;
	write_ops.read_file_bytes = ops.read_file_bytes;
	write_ops.write_file_atomic = ops.write_file_atomic;
	// 書く場所をバックアップに残す (元に戻すときに使う)。
	install::Backup new_backup = *plan.new_backup;
	new_backup.state_path = state_path;
	const install::BackupFirstResult written = install::write_backup_then_state(
		write_ops, in.max_json_bytes, in.backup_json_path, install::backup_to_json(new_backup), state_path,
		plan.new_text);
	if (written.outcome == install::BackupFirstOutcome::BackupWriteFailed) {
		out.outcome = StartupRewriteOutcome::BackupWriteFailed;
		return out;
	}
	if (written.outcome == install::BackupFirstOutcome::StateWriteFailed) {
		out.outcome = StartupRewriteOutcome::StateWriteFailed;
		out.backup_restored = written.backup_restored;
		return out;
	}
	out.outcome = StartupRewriteOutcome::Wrote;
	return out;
}

// Vesktop が起動していないと分かったときに、書き直しをもう一度試す結果か (VesktopRunning と VesktopUnknown)。
inline bool startup_rewrite_waits_for_vesktop(StartupRewriteOutcome outcome)
{
	return outcome == StartupRewriteOutcome::VesktopRunning || outcome == StartupRewriteOutcome::VesktopUnknown;
}

// ログの 1 行に書く語句 (英語。列挙子の名前ではなく、何が起きたかが分かる語句にする)。
inline const char *startup_rewrite_outcome_name(StartupRewriteOutcome outcome)
{
	switch (outcome) {
	case StartupRewriteOutcome::SkippedUninstalling:
		return "skipped, cordmux is being uninstalled";
	case StartupRewriteOutcome::SkippedFeatureOff:
		return "skipped, cordmux is disabled";
	case StartupRewriteOutcome::NoTarget:
		return "skipped, the bundled Vencord folder or the backup location is unknown";
	case StartupRewriteOutcome::StateJsonUnread:
		return "skipped, Vesktop's settings file couldn't be found or read";
	case StartupRewriteOutcome::AlreadyOurs:
		return "already set to the Vencord bundled with cordmux";
	case StartupRewriteOutcome::NotThisScene:
		return "nothing to update";
	case StartupRewriteOutcome::Refused:
		return "not changed";
	case StartupRewriteOutcome::BlockedBySetting:
		return "not changed, writing Vesktop's settings is turned off in settings.json";
	case StartupRewriteOutcome::VesktopRunning:
		return "waiting, Vesktop is running (updated after Vesktop quits)";
	case StartupRewriteOutcome::VesktopUnknown:
		return "waiting, couldn't check whether Vesktop is running";
	case StartupRewriteOutcome::BackupWriteFailed:
		return "the backup of the previous setting couldn't be saved";
	case StartupRewriteOutcome::StateWriteFailed:
		return "Vesktop's settings file couldn't be written";
	case StartupRewriteOutcome::Wrote:
		return "updated to the Vencord bundled with cordmux";
	}
	return "unknown";
}

// Refused の理由の語句 (ログの 1 行に書く)。
inline const char *refuse_reason_name(install::RefuseReason reason)
{
	switch (reason) {
	case install::RefuseReason::BrokenJson:
		return "Vesktop's settings file isn't valid JSON";
	case install::RefuseReason::NotObject:
		return "Vesktop's settings file has an unexpected format";
	case install::RefuseReason::VencordDirNotString:
		return "Vesktop's Vencord location has an unexpected value";
	case install::RefuseReason::ForeignChange:
		return "Vesktop's Vencord location was changed outside cordmux";
	case install::RefuseReason::SceneNotAllowed:
		return "this case isn't changed automatically";
	}
	return "unknown";
}

// Pipeline::audio_levels() の結果。値は線形の振れ幅で、dBFS にするには peak_to_dbfs、メーターの位置にするには
// meter_position (pipeline/audio_meter.h)。
struct AudioLevels {
	// ミキサーに今ある組 (声と画面の音。人が抜けたり共有をやめたりすると消える)。user_id の昇順で、同じ人は声、画面の
	// 音の順。合成のソースがない間は、すべて 0。
	std::vector<AudioTrackLevel> tracks;
	// 配信に出ている声が聞こえている人の user_id (昇順)。名前ラベルの光と同じ判定 (VoiceActivity。100 ms までの息継ぎ
	// では切れない) から、ミュートか音量 0 の声を除いたもの。自分の声 (メインアカウント) は、ミュート中でもラベルは光る
	// が、配信に出ていないのでここには入らない。
	std::vector<std::string> speaking;
};

struct PipelineSnapshot {
	// スナップショットを取った時刻 (どちらも snapshot() の中で読む。ドックが、いつの値かを測れるように両方持つ)。
	std::int64_t taken_at_wall_us = 0;
	std::int64_t taken_at_steady_us = 0;

	// 割り当てのある枠だけを並べる (画面 1 から 4、声 1 から 8 の順)。
	std::vector<SlotSnapshot> slots;
	// シーンコレクションの切り替えの最中か (config::AllocationTable::is_frozen)。
	bool frozen = false;

	// 直近の facts (docs/wire_format.md の「facts」の形。received が false なら 1 通も届いていない)。
	FactsSnapshot facts;

	// 直近の profiles と、参加者ごとに届いたアバター (docs/wire_format.md の「profiles」と「avatar」の形)。表示名と
	// アバターのコピーはここだけで、ドックはここから人の一覧の表示名、OBS のソース名、名前ラベルの 3 か所に渡す。
	// アバターの画像は共有して持つので、コピーしても画像は複製しない (pipeline/profiles_state.h)。
	ProfilesSnapshot profiles;

	// ストリームごとの映像の形。configure が来るたびに全体が入れ替わる。
	std::map<std::uint32_t, StreamFormat> stream_formats;

	// 参加者ごとの音量とミュート。値を持っている人 (音量を変えた人、画面共有の音を持つ人、メインアカウント) だけが入る。
	// ない人は既定の値 (ミュートなし、100%) である。
	std::map<std::string, PersonAudioSnapshot> person_audio;

	// 参加者ごと (user_id) の、直近 1 分の画面共有の映像の届くまでの遅れ (ストリームごとの列。1 人が画面を 2 本以上共有して
	// いれば 2 つ以上)。画面のストリームで、直近 1 分にフレームが届いたものだけが入る。ストリームが終わると消える。
	// set_arrival_watch(true) の間だけ作る (見えていない状態タブのために、列をコピーしない。列の記録は常に行う)。
	std::map<std::string, std::vector<ArrivalWindow>> arrival_by_user;

	// 視聴の範囲に入れた人と、入れた時刻 (wall_now_us の時計。facts の sharing_since_us と同じ)。受け取る範囲が「枠に載せた
	// 人だけ」のときだけ持つ。範囲が変わったとき (refresh_watch_scope_locked) と、接続した直後に送り直すとき
	// (resend_scope_and_main_locked) に、入った人にそのときの時刻を付け、外れた人を消す。「最初のフレームが届かない」の待ち始めに
	// 使う (ledger/rules.cpp の watch_wait_from)。
	std::map<std::string, std::int64_t> watching_since_us;

	// 段階ごとの計数。stats() と同じ中身。
	PipelineStats stats;

	// ストリームごとの、最後にフレームが届いた時刻。
	std::map<std::uint32_t, StreamFrameClock> last_frames;

	// 参加者ごとの、受信をやり直した回数。送信側が実際に始め直した回数 (stream_check_result の outcome が restarted か
	// superseded だった回数) と、受信側がデコードの経路を作り直した回数 (pipeline/receive_restart.h の plan_decode_restart) の
	// 合計である。OBS を起動してから増えるだけで、減らない。ドックの [状態] タブの履歴が、前の周期と比べて増えた人の行を
	// 書く (ledger/status_tab.h の HistoryRestart)。
	std::map<std::string, std::uint64_t> receive_restarts;

	// 録画に渡した映像と音声の、ストリームごとの最後の時刻。「cordmuxの録画のためだけに受け取っています」
	// (cordmux.dock.recording_only.headline。ledger/rules.cpp の RecordingOnly) を表示するかの判断に使う。
	std::map<std::uint32_t, RecordOnlyFrameClock> record_only_frames;

	// 録画の状態 (record/record_types.h)。
	record::RecordingSnapshot recording;

	// 送信側が facts で返した録画の状態 (キーは recording)。値を持たないのは、まだ届いていないとき (facts が 1 通も
	// 来ていないか、このキーを送らない古い送信側)。受信側の recording.enabled と比べ、食い違えば画面に表示する
	// (cordmux.dock.record_not_reached.headline)。届いていない間を OFF とみなすのは送信側の約束で、受信側はここに
	// 「分からない」をそのまま持つ。
	std::optional<bool> sender_recording;

	// 送信側が facts で返した、SPS と PPS をキーフレームの前に付けた回数。値を持たない理由は sender_recording と
	// 同じ。受信側からは見えない値なので、録画ログに残すにはここを通すしかない。
	std::optional<std::uint64_t> sender_sps_pps_prepended;

	// ---- 送信側に渡した値 ----
	//
	// 受信側が最後に送信側に渡した media と limits の値 (Listener::send_media と send_limits に渡した値。
	// 接続していない間も Listener が覚えて、次の hello_ok に載せる)。値を持たないのは、まだ一度も決めていないとき
	// (start() より前)。送信側が facts で返す値と比べるのはドックの役目で、ここは受信側が決めた値のコピーだけを
	// 持つ。
	std::optional<bool> media_sent;
	std::optional<ws::LimitsMessage> limits_sent;

	// 待ち受けと接続の状態 (OpenResult のコピー、接続の時刻と理由、ハートビート。ws/listener.h)。Pipeline は接続の
	// 時刻を自分では持たない。同じ値を 2 か所で数えないように、持つのは Listener だけにする。
	ws::ListenerStats listener;

	// Pipeline のコンストラクターに渡された設定のコピー。動いている間に set_feature_enabled、set_limits、
	// update_record_settings などで変えた値は入らない。
	config::Settings settings;

	// Vesktop の送信側から届いたログの行のリングバッファー (古い順)。出来事の行 (log の lines と alerts) は js_log
	// (診断情報の discord.log)、統計の行 (log の stats) は js_stats_log (discord-stats.log) に入る。積む前に、トークンと
	// ポート番号のチェック (set_js_log_sink で渡された関数) を通している。*_dropped は上限を超えて捨てた行の総数。
	std::vector<std::string> js_log;
	std::uint64_t js_log_dropped = 0;
	std::vector<std::string> js_stats_log;
	std::uint64_t js_stats_log_dropped = 0;

	// cordmux.dll 自身のログの行のリングバッファー (古い順)。出来事の行は plugin_log (診断情報の plugin.log)、統計の
	// 行 (diag::LineKind::Stats) は plugin_stats_log (plugin-stats.log) に入る。入る行は、plugin-main.cpp のログの出力先が
	// トークンとポート番号のチェックを通したものである (Pipeline::push_plugin_log)。cordmux.dll の中で書いた行はすべて
	// どちらかに入る。ws/ 自身の行も、diag/diag_log.h を通った行も、同じ出力先を通る。
	std::vector<std::string> plugin_log;
	std::uint64_t plugin_log_dropped = 0;
	std::vector<std::string> plugin_stats_log;
	std::uint64_t plugin_stats_log_dropped = 0;

	// ダミーを表示していた時間帯 (古い順。最後の時間帯が開いていれば、いまダミーを表示している)。診断情報に入る。
	std::vector<DummyInterval> dummy_intervals;

	// ハードウェアデコードを使えずソフトウェアデコードに切り替えた画面のストリームの数 (今あるストリームだけ。設定の
	// [デコーダー] の値が動作していない数)。診断情報に入る。
	int hw_fallback_streams = 0;

	// safe_call が捕まえた例外の総数 (原因を調べるためだけの値で、判定には使わない)。
	std::uint64_t exceptions_caught = 0;
};

class Pipeline {
public:
	Pipeline(ws::Listener &listener, config::AllocationTable &table, const config::Settings &settings);
	~Pipeline();

	Pipeline(const Pipeline &) = delete;
	Pipeline &operator=(const Pipeline &) = delete;

	// 送信側のログの行の種類 (docs/wire_format.md の「メッセージの種類」の log の扱い)。Event は log の lines、Alert は alerts (異常に
	// 関わる出来事の行)、Stats は stats (数秒ごとの統計の行) で届いた行である。
	enum class JsLineKind {
		Event,
		Alert,
		Stats,
	};

	// Vesktop の送信側のログ (on_log) の 1 行を、リングバッファー (js_log か js_stats_log) に入れる前に通す
	// チェックの関数を差し替える。関数は行と種類を受け取り、リングバッファーに入れてよいかを返す (OBS のログに書くかと、
	// どのレベルで書くかも、その関数が種類で決める)。start() より前に呼ぶこと。既定は全部を捨てる (この関数を呼び忘れた
	// ときに、トークンを含む行をチェックなしで残さないため)。Pipeline はトークンもポート番号も知らないので、チェックの
	// 判定は持たない。plugin-main.cpp が、Listener の LogSink と同じチェックの関数を渡す。
	using JsLogGuard = std::function<bool(std::string_view line, JsLineKind kind)>;
	void set_js_log_sink(JsLogGuard sink);

	// cordmux.dll 自身のログの行を 1 行、リングバッファーに積む。呼ぶのは plugin-main.cpp のログの出力先だけで、
	// 渡すのはトークンとポート番号のチェック (ws::text_reveals_secret_or_port) を通った後の行である (ここに
	// チェックはない。Pipeline はトークンを知らない)。
	//
	// Pipeline 自身もここを直接呼ばない。書きたい行は diag/diag_log.h の関数に渡す (リングバッファーに積む所を
	// 1 つにし、チェックを通ったかどうかが呼び出し元の書き方で変わらないようにする)。
	//
	// どのスレッドから呼んでもよい。中で、ほかのロックを取らない専用のロックを一瞬だけ取り、Impl::mu は取らない
	// (ログの出力先は Impl::mu を持ったまま呼ばれることがあるため。ロックの順は pipeline.cpp の Pipeline::Impl の
	// mu の宣言)。
	// kind が Stats の行は plugin_stats_log に、ほかは plugin_log に積む。
	void push_plugin_log(std::string_view line, diag::LineKind kind);

	// ダミーの表示の切り替えを 1 回書き留める。ON と OFF が実際に変わったときだけ時間帯を開くか閉じ、そのときだけ
	// ログに 1 行書く。呼ぶのはドックのポーリング (dock/dock_poll.cpp) で、sources::set_dummy_mode と対で呼ぶこと。
	// どのスレッドから呼んでもよい。
	void note_dummy_mode(bool on);

	// listener の on_video_frame、on_audio_frame、on_configure、on_log、on_facts、on_profiles、on_avatar、
	// on_connected、on_disconnected に自分の関数を登録し、統計、監視、表示の緩衝、混ぜた音のスレッドを起動する。
	// 話している人は合成のソースで混ぜた声から決める (mixer_loop)。1 回だけ呼ぶ (obs_module_load から)。
	void start();

	// すべてのスレッド (デコード、統計、監視、表示の緩衝、混ぜた音) を止めて join し、録画を止める。2 回呼んでも
	// よい。listener 自体は止めない (止めるのは呼び出し元の plugin-main.cpp で、obs_module_unload は Pipeline::stop()
	// の後に Listener::stop() を呼ぶ)。listener.on_* は触らない (nullptr に戻さない) ので、stop() が戻った後も listener の
	// io_thread が動いていれば on_video_frame などが呼ばれることがある。そのときは内部の状態
	// (screen_streams など) が空なので、届いたメッセージは dropped に数えられて捨てられる (pipeline.cpp の
	// Pipeline::stop() のコメント)。
	void stop();

	// 段階ごとの計数のコピー。どのスレッドから呼んでもよい (中で Impl::mu を取る)。
	PipelineStats stats() const;

	// ドックがポーリングのたびに取るスナップショット (上の PipelineSnapshot)。どのスレッドから呼んでもよい。中で
	// Impl::mu を取って値をコピーするだけで、libobs にも Qt にも触らず、キューも止めない (コピーしている間だけ、
	// 受信の関数が待つ)。ws::Listener::stats() は Impl::mu の外で読む (2 つのロックを同時に持たない。ロックの順は
	// pipeline.cpp の Pipeline::Impl の mu の宣言)。
	PipelineSnapshot snapshot() const;

	// 参加者ごとの音の大きさと、話している人 (ドックの音声タブのメーターと、参加者タブのマイクの光)。ドックが 1 秒に 10 回
	// 呼んでもよい軽さで、PipelineSnapshot は作らない。どのスレッドから呼んでもよい。中で mixer_mu を一瞬だけ取って
	// 組ごとの保持値 (最大 16 組) をコピーし、Impl::mu は取らない。混ぜるスレッドが持つ mixer_mu の間も、区間ごとの
	// 最大を足すだけで、メーターを見ていない間も負荷は変わらない。
	AudioLevels audio_levels() const;

	// ---- 割り当て表を書き換える関数 (ドックの参加者の一覧で、枠に載せる、枠から外す) ----
	//
	// 表の所有者は config::AllocationTable のままで、Pipeline はその表と StreamRouter の経路を同じ Impl::mu の下で
	// 一緒に変えるために、この関数を持つ。2 つを別々に変えると、その間に届いたフレームが古い経路のまま出る (別の
	// 人の枠に出る。stream_router.h の retarget_user)。ドックは AllocationTable にも Listener の内部にも
	// 直接触らない。どのスレッドから呼んでもよい (ドックの UI のスレッドから呼ぶ)。
	struct SlotChange {
		enum class Outcome {
			Assigned,     // 空いている枠に入れた
			AlreadyOwned, // もう枠に載っていた (番号は変えない)
			Insufficient, // 入れられる枠がなかった
			Released,     // 枠から外した
			NotAssigned,  // 外す枠がなかった (もともと入っていない)
			Replaced,     // 枠の人を別の人に替えた (replace_slot)
		};
		Outcome outcome = Outcome::Insufficient;
		int slot = 0;      // 入れた枠か外した枠の番号 (なければ 0)
		int retargeted = 0; // 付け替えた経路の数 (stream_router.h の retarget_user)
	};

	// 画面の枠に新しく載せる人は、載せられる人数 (config::screen_slot_capacity。自分の枠を使っている間は 3 人) に達して
	// いれば、空いている枠があっても載せず、Insufficient を返す (自動で載せる経路と同じ上限。assign_slot_in)。ドックも同じ上限で
	// 押せなくしている (ledger/observation.h の self_frame_limit_reached) が、その判断は最大 1 周期前の Observation で行うので、
	// 同じ周期に自動で載せる経路 (選び方が「自動」のときの apply_configure と admit_sharers、[自動で枠に載せる] の人) が
	// 載せた人は、ここで割り当て表と同じロックの中で数える。選び方が「自動」のときも、手動のチェックは押せる。
	// Vesktop のアカウント (stream_router.h の AssignPolicy::receiver_user_id) は、画面が届かないので画面の枠に載せず、
	// Insufficient を返す (ドックはこの人の行のチェックを押せなくしている)。
	SlotChange assign_slot(config::SlotKind kind, const std::string &user_id);
	SlotChange release_slot(config::SlotKind kind, const std::string &user_id);

	// 自分の枠に映す映像を「Discordの画面共有」と OBS のソースかシーンの間で替えたときに、メインアカウント (user_id) を配信に
	// 出す場所を、画面共有の枠と自分の枠の間で移す (dock/dock_poll.cpp の carry_self_frame_check)。割り当て表と、
	// set_assign_policy の self_frame_selected と self_frame_in_use を、1 回のロックの中で一緒に変える。別々の呼び出しにすると、
	// その間に自動で載せる経路が、空いた枠に待っていた人を載せる (自分の枠と合わせて 5 つになるか、メインアカウントを載せ直せ
	// なくなる)。
	//   to_self_frame が真 (OBS のソースかシーンに替えた): user_id の画面の枠を空け (release_slot と同じ)、自分の枠に映す映像を
	//     OBS から選んでいることにし、自分の枠を使い始める (set_assign_policy と同じく、Discord の人が枠を 4 つとも埋めていれば
	//     断る。空けた枠の分で使うので、普通は断らない)。
	//   偽 (「Discordの画面共有」に替えた): 自分の枠を使うのをやめてから、user_id を画面の枠に載せる (assign_slot と同じ上限。
	//     自分の枠の分の枠が空くので、普通は載る)。after_order があれば、載せた順を、番号 after_order の人の後、次の番号の人の
	//     前にする (place_added_order_after。自分の枠の並びの位置を引き継ぐ)。もう載っていた人 (AlreadyOwned) の番号は変えない。
	// どちらも、変えた後に直前の configure を適用し直す (set_assign_policy と同じ)。どのスレッドから呼んでもよい (ドックの UI の
	// スレッドから呼ぶ)。
	struct SelfFrameMove {
		SlotChange change;             // 枠を空けたか載せたかの結果 (release_slot と assign_slot と同じ)
		bool self_frame_in_use = false; // 受け付けた、自分の枠を使っているか (set_assign_policy の戻り値と同じ)
	};
	SelfFrameMove move_self_frame(const std::string &user_id, bool to_self_frame,
				      std::optional<std::int64_t> after_order = std::nullopt);

	// 枠に載っている out_user_id を外し、同じ枠に in_user_id を載せる (ドックの [入れ替える])。割り当て表と経路を 1 回の
	// ロックの中で書き換えるので、人が抜けて詰まった並びが配信に出ることはない。
	// 結果は Replaced (slot は替えた枠の番号)、out_user_id が枠にいなければ NotAssigned、in_user_id がもう枠にいる、または同じ人どうしなら AlreadyOwned (どちらも何も変えない)。
	// 枠から外す操作 (release_slot) と載せる操作 (assign_slot) が起こすことは、すべて同じ結果になる
	// (replace_slot_in の宣言)。
	SlotChange replace_slot(config::SlotKind kind, const std::string &out_user_id, const std::string &in_user_id);

	// ---- 誰を枠に入れるかの決め方 ----
	//
	// 元の値は設定のファイルにある (mode は settings.json、always_on と voice_links は seen_people.json)。ここが
	// 持つのはそのコピーで、渡すのはドックだけである (dock/dock_poll.cpp が、起動のときと、設定を変えたとき、
	// 「自動で載せる」や使う声を変えたときに渡し直す)。適用の仕方は pipeline/stream_router.h の AssignPolicy。
	//
	// 中身が変わったら、直前の configure をその場で適用し直す。自動に切り替えた瞬間に画面共有している人が上限まで
	// 載るのも、「自動で載せる」を付けた人がもう画面共有しているときに載るのも、使う声を変えた瞬間に声が
	// 置き換わるのも、次の configure を待っていては起きない (configure は参加者が変わるまで来ない)。適用し直しは、
	// configure が届いたときと同じ処理を通る。
	//
	// 自動から手動に戻したときは、いま画面の枠に載っている人を誰も外さない (手動で適用し直しても、枠に載っている
	// 人の経路は残る)。
	//
	// 使う声もこの関数で渡す。割り当て表を直接書く関数 (set_voice_source) をドックから呼ぶと、経路 (stream_id ->
	// 枠) が古いまま残り、使う声に選んだ人の声が、次の configure までその人自身の枠から出続ける (割り当て表と経路を
	// 同じロックの下で一緒に変えるという、assign_slot と release_slot の約束にも反する)。
	//
	// main_account_user_id (メインアカウント。settings の broadcaster_user_id) は、自分の声の人である。自分の声には、
	// 合成のソースがあるときだけ声の枠の経路を作り (stream_router.h の AssignPolicy)、ミキサーでの既定をミュートにし、
	// 話しているかを利得とミュートを掛ける前の音で判定する (mixer_roster.h の MixerLevels)。合成のソースの有無が
	// 変わったときも、直前の configure を適用し直す (pipeline.cpp の refresh_layout_presence)。起動のときの値は、
	// ドックを作らない起動でも動作するように、コンストラクターが settings からコピーする。
	//
	// self_frame_selected (自分の枠に映す映像として OBS のソースかシーンを選んでいるか) が真の間は、メインアカウントの画面を
	// 画面の枠に入れない (stream_router.h の AssignPolicy)。この値が変わったときも、直前の configure を適用し直す。
	//
	// self_frame_in_use (自分の枠を使っているか。config::self_frame_in_use) が真の間は、画面の枠に新しく載せられる人数が
	// 1 つ減る (stream_router.h の AssignPolicy)。偽に変わったときは、直前の configure を適用し直し、画面共有している人を
	// 載せ直すので、自動では待っていた人が空いた枠に載る。ドックは、自分の行のチェックが変わるたびに渡し直す。起動のときの
	// 値は、main_account_user_id と同じく、コンストラクターが settings からコピーする (シーンの読み込みが終わる前に届く facts で
	// 枠に載せる人を決めるときに、もう分かっている)。
	// 偽から真に変える (自分の枠を使い始める) ときは、割り当て表と同じロックの中で、Discord の人が画面の枠をもう 4 つとも
	// 埋めていれば断り、偽のままにする (stream_router.h の accepted_self_frame_in_use)。ドックが付ける判断はスナップショット
	// (最大 1 周期前) の人数で行うので、その後に自動で載せる経路が 4 人目を載せていると、自分の枠と合わせて 5 つになるためである。
	// ほかの値は、断ったときも渡されたとおりにする。戻り値は、受け付けた self_frame_in_use (渡した値が偽なら偽、断ったら偽)。
	// ドックは、真を渡して偽が返ったら、自分の行のチェックを外す (dock/dock_poll.cpp の refuse_self_frame)。
	//
	// どのスレッドから呼んでもよい (ドックの UI のスレッドから呼ぶ)。
	bool set_assign_policy(config::AssignMode mode, std::set<std::string> always_on,
			       std::map<std::string, std::string> voice_links, std::string main_account_user_id,
			       bool self_frame_selected = false, bool self_frame_in_use = false);

	// 全画面で映す人 (空 = 全画面でない。stream_router.h の AssignPolicy::focus_user_id)。画面の枠を持たない人なら、
	// 受け取る範囲が Slots でも視聴の範囲に入れて受信を始め、その人の画面のストリームを config::kFocusOnlySlot の子へ渡す。
	// 受け取っていれば、直前の configure を適用し直してすぐに経路を作る。空にすると、経路を消し、視聴の範囲からも外す。
	// 画面の枠を持つ人を渡しても、経路と範囲は変わらない。どのスレッドから呼んでもよい (ドックの UI のスレッドから呼ぶ)。
	void set_focus_target(const std::string &user_id);

	// ---- 受け取る範囲 (設定の receive_scope) ----
	//
	// Slots のとき、送信側に枠に載っている人だけを視聴させ (制御用の JSON の receive_scope)、録画の入力にも枠に載っている
	// 人の画面だけを通す (pipeline/receive_scope.h)。誰が枠に載っているかが変わるたびに (枠の付け外し、自動の割り当て、
	// 配置の読み込み)、視聴する人の範囲を決め直して、変わったときだけ送る。Slots のとき、画面共有を始めた人は、映像が届く
	// 前に枠に載せる (stream_router.h の admit_sharers。自動の割り当ては映像が届かないと動かないため)。値が変わって
	// いなければ何もしない (ドックは周期 (既定 200 ms) ごとにコピーを渡す)。どのスレッドから呼んでもよい。起動のときの値は、
	// コンストラクターが settings からコピーする。
	void set_receive_scope(config::ReceiveScope scope);

	// ---- 通話の操作 ([移動] と [退出]) ----
	//
	// 利用者が押したときだけ呼ぶ。制御用の JSON の move_to_main_call と leave_call を送信側に送るだけで、移動先は送らない
	// (送信側が、押された時点のメインアカウントの通話を読み直す)。接続していなければ、何もせずログに書く。送信側が
	// 実行できなかったときの理由は、送信側のログ (discord.log) に書かれる。どのスレッドから呼んでもよい。
	void request_move_to_main_call();
	void request_leave_call();

	// ---- 録画の ON と OFF ----
	//
	// 録画するときは毎回利用者が ON にする (ON を覚えない) ので、settings には持たず、値は Pipeline の
	// メモリだけにある。起動のたびに OFF から始まる。
	//
	// 動いている間に切り替える関数が必要なので、set_assign_policy と同じ形で持つ。ドックはポーリングのたびに
	// コピーを渡すので、値が変わっていなければ何もしない (そうしないと、ドックの周期ごとに録画を開き直す)。どの
	// スレッドから呼んでもよい (ドックの UI のスレッドから呼ぶ)。
	//
	// 値が変わったときは、制御用の JSON の recording を送信側に送る (接続した直後に送るのは handle_connected)。
	//
	// 呼び出し元は 1 つのスレッドに限る (いまはドックのポーリングだけ)。フラグを変えるのは Impl::mu の中だが、
	// Recorder を動かすのはその外なので、2 つのスレッドから続けて呼ぶと、ON と OFF が入れ替わった順で録画に
	// 届くことがある。
	//
	// ON にするときは、その時点で保存先を決め直し、録画のセッションの値 (保存先、PCM の形、ファイルの名前の語) を
	// 録画の側に渡してから始める。OFF は待たない (段階を Stopping にして戻り、書き込みのスレッドを join するのは
	// 録画の側の別のスレッド)。停止の処理中 (snapshot().recording.phase == Stopping) の ON は受け付けない。
	void set_recording(bool on);

	// ---- 録画の側が自分で止めたときの後始末 ----
	//
	// 保存先のドライブの空き容量がなくなったときだけ、録画の側が自分で止める。止める判断は録画の書き込みの
	// スレッドの中で起きるので、そのスレッドは自分を join できず、送信側に伝える関数も持たない (record/ は ws/ に
	// 依存しない)。その場合を見つけてフラグを false にするのがこの関数である。false にするまで送信側は録画のための
	// 黒埋め用のフレームを送り続け、利用者が [cordmuxの録画を開始] を押しても、set_recording が「変わっていない」と
	// 見て何も起きない。
	//
	// ポーリングの最初、スナップショットを取る前に呼ぶ (snapshot() は録画の ON と OFF に Pipeline のフラグを
	// 入れ直すので、先に false にしないと、ボタンがそのポーリングの間だけ [cordmuxの録画を終了] のまま残る)。
	// set_recording と同じスレッドから呼ぶ。何も起きていないときは、録画の状態のコピーを 1 つ取るだけで戻る。
	//
	// 保存先に書けずに始められなかったとき (phase == StartFailed) も、同じ処理で後始末する (ON を OFF に戻す)。
	void poll_recording_self_stop();

	// ---- 録画ファイルに書く表示名 ----
	//
	// 表示名は、ドックが dock/build_observation.cpp の effective_display_name で決めた 1 つを受け取り、録画の
	// セグメントを開いた時点で録画ログに書く。その名前は利用者が付けた名前の上書きを含むので、seen_people.json を
	// 読むドックにしか作れない (Pipeline はファイルを読まない)。名前を決める場所を 2 つにしないように、ドックが
	// 決めた値をそのまま受け取る。
	//
	// 受け取っていない人は profiles の表示名を使う (理由は pipeline.cpp の display_name_for_locked)。
	//
	// set_assign_policy と同じく、値が変わっていなければ何もしない (ドックは周期 (既定 200 ms) ごとにコピーを渡す)。どの
	// スレッドから呼んでもよい。
	void set_display_names(std::map<std::string, std::string> names);

	// cordmux.dll のバージョン。録画ログの session の行に書くためだけに受け取る。値を決めるのは plugin-main.cpp の
	// 1 か所で、Pipeline はコピーを持つだけである。start() より前に呼ぶ。どのスレッドから呼んでもよい。
	void set_plugin_version(std::string version);

	// 録画の保存先。現在の決め方で決めた、録画のセッションのフォルダーを並べる場所 (<保存先>/cordmux。空なら書ける
	// 場所がない)。決め直すのは、コンストラクター、update_record_settings、録画の開始
	// (set_recording(true)) のとき。録画していない間に空き容量を表示する側
	// (platform::OsProbeConfig::free_space_probe_path) と、アンインストールの結果で録画が残っている場所を表示する
	// 処理 (install/uninstall.h の UninstallReport::record_dir) が読む。
	// snapshot().recording.resolved_base_dir と同じ値。どのスレッドから呼んでもよい。
	std::string record_base_dir() const;

	// 設定ウィンドウで編集中の保存先の値から、いま録画を始めたら書く場所 (<保存先>/cordmux) を決めて返す
	// (設定ウィンドウが、録画のページに保存先の例と場所を表示するのに使う)。書けるかどうかは見ない。OBS の録画の
	// 保存先を読むので、UI のスレッドから呼ぶ (問い合わせにはタイムアウトがある)。
	std::string preview_record_base_dir(const std::string &record_dir_setting);

	// 録画ファイルの作成の進み具合が止まったとみなすまでの秒数 (既定は record::kDefaultFinalizeStallS。
	// dock/dock_poll_layout.cpp が settings の record_finalize_stall_s を渡す)。どのスレッドから呼んでもよい。
	void set_finalize_stall_s(int seconds);

	// 録画ファイルの名前に使う種類の語 (ini の「画面共有」と「声」)。空なら既定の語。次に始める録画のセッションから
	// 動作する (セッションを始めるときに、セッションの値にコピーする)。どのスレッドから呼んでもよい。製品は
	// コンストラクターで ini の文字列を入れる (record_name_words) ので、いまこの関数と下の
	// set_record_file_name_format を呼ぶ所はない。
	void set_record_kind_words(const std::string &screen, const std::string &voice);

	// 録画ファイルの名前の書式 (%1 は名前、%2 は種類の語、%3 は録画のセッションの日時。空なら既定の書式)。次に
	// 始める録画のセッションから反映される。どのスレッドから呼んでもよい。
	void set_record_file_name_format(const std::string &format);

	// 録画の設定 (record_dir と record_output_pcm) のコピーを替える。次に始める録画のセッションから反映され、待っている
	// 前のセッションの録画ファイルの作成には影響しない。保存先の表示 (record_base_dir) も決め直す。OBS の録画の保存先を
	// 読むので、UI のスレッドから呼ぶ。
	void update_record_settings(const config::Settings &settings);

	// 作成待ち (pending) の録画のセッションを、続きから録画ファイルを作成する (ledger::ActionId::ResumeFinalize の行き先)。
	// 待たずに戻る。録画ファイルの作成は録画の側の別のスレッドが順に行い、進み具合は snapshot() の recording.finalize に入る。
	// 待っているものがなければ何もしない。どのスレッドから呼んでもよい。
	void resume_finalize();

	// ---- 画面の枠を空にするタイミング ----
	//
	// 送信側との接続かハートビートが切れてから、画面の枠を空にするまで最後のフレームを残す長さ (settings の
	// link_lost_hold_ms。OBS を再起動しなくても動作する)。値は settings の範囲で確認したものとしてそのまま使い、負の
	// 値は 0 にする。どのスレッドから呼んでもよい。
	void set_link_lost_hold_ms(int ms);

	// 接続し直してから、映像が戻らない枠の配置を保つ長さ (settings の reconnect_layout_hold_ms。LinkLostLayoutHold)。
	// 扱いは set_link_lost_hold_ms と同じ。
	void set_reconnect_layout_hold_ms(int ms);

	// 画面共有の映像の表示の緩衝 D (settings の display_buffer_ms。OBS を再起動しなくても動作する)。画面共有の映像は
	// 合成のソース (sources/composite_source.h) の子にだけ渡す。フレームを待ち行列に積み、送信側の
	// タイムスタンプ + D の後の OBS の描く刻みに OBS へ渡し、渡し先の子の OBS の緩衝を切る (D が 0 でも同じ)。
	// 範囲 (0 から 1000) の外の値は端に収める。値が変わっていなければ何もしない。D を縮めたときは、積んであるフレームの
	// 時刻も新しい D で決め直し、時刻が来たフレームのうち最も新しい 1 つだけを渡し、それより古いものは捨てる。どの
	// スレッドから呼んでもよい。
	void set_display_buffer_ms(int ms);

	// snapshot() の arrival_by_user を作るか。状態タブが見えている間だけ true にする。既定は false。列の記録そのものは、
	// この設定によらず常に行う (タブを開いた瞬間から、直近 1 分の全部が出る)。
	// どのスレッドから呼んでもよい。
	void set_arrival_watch(bool on);

	// 参加者ごと (user_id と種類。声と画面の音は別) の音量 (%。0 から 200、既定 100) とミュート (既定はなし)。合成のソースが
	// 出す混ぜた音に掛かる。ドックの参加者ごとの音量とミュートが呼ぶ口である。値は組がミキサーにない間も覚え、組が入ったとき
	// に当てる。自分の声 (set_assign_policy の main_account_user_id の声) のミュートは、参加者ごとではなく 1 つで、既定は
	// ミュート (settings の self_voice_muted。起動のときの値はコンストラクターが受け取る。呼び出し元が変えた値を
	// 保存する)。画面共有の音は、共有が始まるたびにミュートから始まる (configure で、画面のストリームのある人が新しく
	// 現れたとき。mixer_roster.h の ScreenShareTracker)。範囲の外の音量は端に収める。どのスレッドから呼んでもよい
	// (mu を取らない)。
	//
	// 枠の割り当て (try_assign) はこの表を読まない。ミュートと音量は人に付き、user_id で覚えているので、枠を別の人に
	// 割り当て直しても、前の人の値は失われず、次の人にも引き継がれない。
	void set_person_audio_volume(const std::string &user_id, MixerKind kind, int volume_pct);
	void set_person_audio_mute(const std::string &user_id, MixerKind kind, bool muted);
	MixerLevel person_audio_level(const std::string &user_id, MixerKind kind) const;

	// 覚えている音量 (声と画面共有の音の %) を、全員分まとめて入れ替える。載っていない人の音量は 100% に戻す。元の値は
	// seen_people.json にあり、起動のときと、覚えている行を捨てたときに使う。参加者ごとの変更は set_person_audio_volume で
	// 行う。1 回のロックで済ませるための口である。
	struct PersonVolumes {
		int voice_pct = config::kPersonVolumeDefaultPct;
		int screen_pct = config::kPersonVolumeDefaultPct;
	};
	void set_person_volumes(const std::map<std::string, PersonVolumes> &volumes);

	// 画面共有の終わりが分かって画面の枠をすぐ空にした後に呼ぶコールバック (自動配置で並べ直すきっかけ)。受信の
	// スレッドから呼ばれる (枠に載せた後の適用し直しでは、UI のスレッドからも呼ばれる)。コールバックは Qt の
	// ウィジェットにも cordmux のロックにも触らず、UI のスレッドに処理を渡すだけにすること
	// (dock/dock_poll_layout.cpp)。Pipeline の Impl::mu を持たずに、このコールバックだけを守るロックを持ったまま
	// 呼ぶ。そのため、空の関数を渡して外すと、戻った時点で呼び出しの途中のものも終わっている (外した後に
	// 呼ばない)。コールバックはほかのロックを取らないこと。どのスレッドから設定してもよい。
	void set_screen_layout_changed_sink(std::function<void()> sink);

	// ---- cordmux の有効と無効、画質の上限 ----
	//
	// 元の値は settings.json にある (feature_enabled と limit_*)。起動時の値はコンストラクターで settings から
	// コピーし、動いている間の変更は、ドックが dock::DockPollConfig::apply_runtime_settings を通して渡す
	// (plugin-main.cpp が登録する)。どちらも値が変わっていなければ何もしない (呼び出し元が何度コピーを渡しても、
	// 送信側に同じ値を送り直さない)。
	//
	// set_feature_enabled(false) は media を false にして送り、録画中なら set_recording(false) で止めて録画ファイルの作成を
	// 始める。ドックも同じ順で止めるが、呼び出し元が変わっても無効の間に録画が残らないように、ここでも止める。
	// 待ち受けは開いたままにする (Listener は開き直せない)。set_recording と同じスレッドから呼ぶ。
	void set_feature_enabled(bool on);
	// settings の形 (0 は制限なし、fps は 1 か 2) で受け取り、limits_from_settings で制御用の JSON の形にする。どの
	// スレッドから呼んでもよい。
	void set_limits(int max_long_edge, int fps_divisor, config::EncoderPreference encoder_preference,
			config::EncodePriority encode_priority);

	// こちらから待ち受けを閉じた後に呼ぶ (アンインストールの最初の手順)。ws::Listener::stop() は on_disconnected を
	// 呼ばない (ws/listener.h) ので、切れたときと同じ後始末 (経路、デコードのスレッド、直近の facts を捨て、
	// 録画に切れたことを伝える) をここで行う。Listener::stop() が io_thread を join した後に、同じスレッド (OBS の
	// UI のスレッド) から呼ぶこと (io_thread からの受信の関数と重ならない)。
	void on_listener_stopped();

	// OBS_FRONTEND_EVENT_FINISHED_LOADING で呼ぶ。これより前に届いた configure は最新の 1 通だけ覚えて保留してあり、
	// ここで適用する。適用する前に、現在のシーンコレクションの合成のソースの有無を調べ、media を決め直して送る。
	//
	// シーンコレクションを読み終えるまでは、渡す先の合成のソースがあるかが分からないので、読み込みが終わるまで
	// configure を適用しない (OBS と Vesktop のどちらを先に起動しても動くようにするため)。最新の 1 通だけを覚えるのは、
	// configure がいま生きているストリームをすべて含む形だからである (古いものから順に適用する意味がない)。
	void on_frontend_finished_loading();

	// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING と CHANGED から呼ぶ。切り替えの前後に AllocationTable を
	// 書き換える処理を、Pipeline が持つ状態 (StreamRouter、ストリームごとのデコードのスレッド、合成のソースの有無) と
	// 同じ Impl::mu の下で行うので、Pipeline の関数として持つ。
	//
	// CHANGING: AllocationTable::begin_scene_collection_change を呼んで凍結する (切り替えの最中は、映像も音声も
	//           渡さない。stream_router.h の output_allowed と video_allowed)。調べてあった合成のソースの有無は、
	//           切り替えた後のシーンコレクションには当てはまらないので、現在の値ではないことにする。
	// CHANGED:  AllocationTable::apply_scene_collection_changed で凍結を解き、合成のソースの有無を調べ直して media を
	//           決め直す。
	void on_scene_collection_changing();
	void on_scene_collection_changed();

	// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP から呼ぶ (切り替えのときも、終了のときも通る)。調べてあった
	// 合成のソースの有無を「現在の値ではない」にし、調べている途中の監視の結果も使わない。切り替えのときは
	// CHANGING がもう同じことをしている。終了のときはこれが最初のイベントである (OBS 31.1.1 の
	// OBSBasic::closeEvent は、ClearSceneData の後に EXIT を送る。
	// https://github.com/obsproject/obs-studio/blob/31.1.1/frontend/widgets/OBSBasic.cpp)。
	void on_scene_collection_cleanup();

	// OBS_FRONTEND_EVENT_EXIT から呼ぶ。OBS はこの直前にシーンコレクションを片付けていて (obs_module_unload より
	// 前)、Pipeline::stop() はその後に呼ばれる。ここから後は合成のソースの有無を調べ直さない (監視のスレッドも何も
	// しない。ドックがポーリングを止めるのと同じタイミング)。2 回呼んでもよい。
	void on_frontend_exit();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// 画面の枠 slot の人の載せた順の番号 (added_order) を after + 1 にし、ほかの枠の人のうち after より大きい番号の人を 1 つずつ
// 後ろへずらす。ほかの人どうしの順は変わらず、slot の人は after の人の後、次の番号の人の前になる。番号は数の大小だけで比べる
// ので、ずらしても配信の位置の決め方 (ledger/slot_cells.h) の答えは、slot の人の位置のほかは変わらない。呼び出し元は表を守る
// ロックを持っていること。
inline void place_added_order_after(config::AllocationTable &table, int slot, std::int64_t after)
{
	constexpr config::SlotKind kind = config::SlotKind::Screen;
	for (int other = 1; other <= config::kScreenSlotCount; ++other) {
		if (other == slot)
			continue;
		const config::SlotAssignment *a = table.assignment(kind, other);
		if (a != nullptr && a->added_order > after)
			table.set_added_order(kind, other, a->added_order + 1);
	}
	if (table.assignment(kind, slot) != nullptr)
		table.set_added_order(kind, slot, after + 1);
}

// replace_slot の、割り当て表と経路を書き換える部分。呼び出し元は表と経路を守るロック (Impl::mu) を持っていること。
// release_slot の画面の枠の処理 (表の release、exclude_user、外した人の経路を枠 0 へ) と assign_slot の処理
// (include_user、載せる人の経路を枠へ) を、同じ枠の番号で続けて行う。新しい人の added_order は
// rewrite が新しい番号にする。声の枠は画面の枠の選び方に合わせないので、触らない。out_retargeted と in_retargeted に、
// 付け替えた経路の数を返す (in_retargeted が 0 なら、呼び出し元が直前の configure を適用し直す)。
inline Pipeline::SlotChange replace_slot_in(config::AllocationTable &table, StreamRouter &router,
					    const std::string &out_user_id, const std::string &in_user_id,
					    int *out_retargeted, int *in_retargeted)
{
	using Outcome = Pipeline::SlotChange::Outcome;
	constexpr config::SlotKind kind = config::SlotKind::Screen;
	Pipeline::SlotChange change;
	change.outcome = Outcome::NotAssigned;
	if (out_user_id.empty() || in_user_id.empty())
		return change;
	const int slot = table.slot_of_owner(kind, out_user_id);
	if (slot == 0)
		return change;
	if (out_user_id == in_user_id || table.slot_of_owner(kind, in_user_id) != 0) {
		change.outcome = Outcome::AlreadyOwned;
		change.slot = slot;
		return change;
	}

	// 載せた順は、出る人の番号を入る人が引き継ぐ。rewrite は新しい番号を付けるので、入れ替えた枠が最後に動いてしまい、
	// 載せた順の並びで、ほかの人の位置まで変わる。
	const std::int64_t order = table.assignment(kind, slot)->added_order;
	table.rewrite(kind, slot, in_user_id, in_user_id);
	table.set_added_order(kind, slot, order);
	router.exclude_user(out_user_id);
	router.include_user(in_user_id);
	const int out_n = router.retarget_user(out_user_id, kind, 0);
	const int in_n = router.retarget_user(in_user_id, kind, slot);
	if (out_retargeted != nullptr)
		*out_retargeted = out_n;
	if (in_retargeted != nullptr)
		*in_retargeted = in_n;
	change.outcome = Outcome::Replaced;
	change.slot = slot;
	change.retargeted = in_n;
	return change;
}

// assign_slot の、割り当て表と経路を書き換える部分。呼び出し元は表と経路を守るロック (Impl::mu) を持っていること。
// 画面の枠に新しく載せる人は、載せられる人数 (stream_router.h の screen_slot_room。policy.self_frame_in_use が真なら 3 人) に
// 達していれば、空いている枠があっても載せず、Insufficient を返す (自動で載せる経路と同じ上限)。載せたら (もう載っていた
// ときも)、外した人の一覧から消し (StreamRouter::include_user)、その人の経路を枠へ付け替える。after_order があれば、新しく
// 載せた人の載せた順を、番号 after_order の人の後にする (place_added_order_after)。retargeted が 0 なら、呼び出し元が直前の
// configure を適用し直す。
inline Pipeline::SlotChange assign_slot_in(config::AllocationTable &table, StreamRouter &router, const AssignPolicy &policy,
					   config::SlotKind kind, const std::string &user_id,
					   std::optional<std::int64_t> after_order = std::nullopt)
{
	using Outcome = Pipeline::SlotChange::Outcome;
	Pipeline::SlotChange out;
	if (user_id.empty())
		return out;
	// Vesktop のアカウントの画面は届かないので、画面の枠に載せない (自動で載せる経路と同じ規則。stream_router.h の
	// AssignPolicy の receiver_user_id)。ドックはこの人の行のチェックを押せなくしているので、普通はここに来ない。
	if (kind == config::SlotKind::Screen && !policy.receiver_user_id.empty() && user_id == policy.receiver_user_id) {
		out.outcome = Outcome::Insufficient;
		return out;
	}
	const bool over_capacity = kind == config::SlotKind::Screen && table.slot_of_owner(kind, user_id) == 0 &&
				   !screen_slot_room(table, policy);
	const config::AssignResult result = over_capacity ? config::AssignResult{} : table.try_assign(kind, user_id);
	switch (result.outcome) {
	case config::AssignOutcome::AlreadyOwned:
		out.outcome = Outcome::AlreadyOwned;
		break;
	case config::AssignOutcome::Assigned:
		out.outcome = Outcome::Assigned;
		if (kind == config::SlotKind::Screen && after_order.has_value())
			place_added_order_after(table, result.slot, *after_order);
		break;
	case config::AssignOutcome::Insufficient:
		out.outcome = Outcome::Insufficient;
		return out;
	}
	out.slot = result.slot;
	// 外した人をまたチェックを付けたときは、外した人の一覧から消す (stream_router.h の exclude_user)。
	if (kind == config::SlotKind::Screen)
		router.include_user(user_id);
	// 経路も同じロックの下で付け替える (表と経路が食い違う間を作らない)。
	out.retargeted = router.retarget_user(user_id, kind, out.slot);
	return out;
}

// release_slot の、割り当て表と経路を書き換える部分。呼び出し元は Impl::mu を持っていること。外した人は、画面共有をやめて
// 始め直すまで、次の configure でも枠に戻さない (StreamRouter::exclude_user)。外した人の経路は消さず、枠の番号 0 を指させる
// (音声は output_allowed、映像は video_allowed が止める。stream_router.h の retarget_user)。
//
// 枠 0 の経路 (経路はあるが、どの枠でもない) は try_assign では作れない組み合わせなので、通る所を 1 つずつ確認してある。
//   - AllocationTable::entry() は範囲の外で nullptr を返し、is_output_allowed と is_video_allowed は false
//     (config/allocation.h)。
//   - push_decoded_frame の待ち行列に積む処理は、allowed の判定と frame_deliverable (枠の番号が 1 以上) の後にある。
//   - handle_audio_frame も allowed を見てから AudioMixer に置く。置く組は mixer_key_for が枠の外では決めない
//     (composite_audio_plan.h)。
// つまり枠 0 の経路では、フレームは届いて数えられるが、OBS には 1 枚も渡らない。
inline Pipeline::SlotChange release_slot_in(config::AllocationTable &table, StreamRouter &router, config::SlotKind kind,
					    const std::string &user_id)
{
	Pipeline::SlotChange out;
	out.outcome = Pipeline::SlotChange::Outcome::NotAssigned;
	if (user_id.empty())
		return out;
	const int slot = table.slot_of_owner(kind, user_id);
	if (slot == 0)
		return out;
	table.release(kind, slot);
	if (kind == config::SlotKind::Screen)
		router.exclude_user(user_id);
	out.outcome = Pipeline::SlotChange::Outcome::Released;
	out.slot = slot;
	out.retargeted = router.retarget_user(user_id, kind, 0);
	return out;
}

// move_self_frame の、割り当て表と経路と決め方 (policy の self_frame_selected と self_frame_in_use) を書き換える部分。呼び出し元は
// Impl::mu を持っていること。self_owner は自分の枠の人 (stream_router.h の accepted_self_frame_in_use に渡す)。
//   to_self_frame が真: user_id の画面の枠を空けてから (release_slot_in)、自分の枠を使い始める (accepted_self_frame_in_use)。
//   偽: 自分の枠を使うのをやめてから、user_id を載せる (assign_slot_in)。上限は自分の枠を使っていないときの 4 人で見る。
// 返す self_frame_in_use は、書き換えた後の policy.self_frame_in_use である。
inline Pipeline::SelfFrameMove move_self_frame_in(config::AllocationTable &table, StreamRouter &router, AssignPolicy &policy,
						  const std::string &self_owner, const std::string &user_id,
						  bool to_self_frame, std::optional<std::int64_t> after_order = std::nullopt)
{
	Pipeline::SelfFrameMove out;
	if (to_self_frame) {
		out.change = release_slot_in(table, router, config::SlotKind::Screen, user_id);
		policy.self_frame_selected = true;
		policy.self_frame_in_use =
			accepted_self_frame_in_use(table, policy.self_frame_in_use, true, self_owner);
	} else {
		policy.self_frame_selected = false;
		policy.self_frame_in_use = false;
		out.change = assign_slot_in(table, router, policy, config::SlotKind::Screen, user_id, after_order);
	}
	out.self_frame_in_use = policy.self_frame_in_use;
	return out;
}

} // namespace cordmux::pipeline

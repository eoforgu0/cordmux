// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、OBS の中の合成のソースと名前ラベルを扱うもの (配置の入力、名前ラベルに渡す値) の状態
// (DockPollLayoutState) と関数 (DockPollLayoutPart) を宣言する。実装は dock_poll_layout.cpp にある。
// 名前ラベルの見た目の値は、起動時と設定を変えたときに渡し、参加者ごとの値 (名前、アバター、表示する隅など) は
// ポーリングのたびに渡す。画面共有の枠に載っている人と発言のタイミングはポーリングを通さず、Pipeline が受信と
// デコードのスレッドから直接渡す。
// dock_poll.h を include しない (dock_poll.h がこのヘッダーを include する)。

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <QObject>

#include "sources/composite_input.h"

namespace cordmux::pipeline {
struct PipelineSnapshot;
}

namespace cordmux::dock {

class DockPoll;

// ほかのスレッド (Pipeline の受信スレッド) から、UI スレッドに処理を頼むためのブリッジ。頼む側は mu
// (ほかのロックを取らないロック) だけを取り、宛先 (context) があれば、Qt の QueuedConnection の呼び出しを 1 つ積む。
// cordmux のほかのロックは取らない。DockPoll が破棄される前に宛先を外すので、外した後に呼び出しが積まれることはない。
// Pipeline は shared_ptr でブリッジを持ち続けてよい。
struct DockPollLayoutBridge {
	std::mutex mu;
	QObject *context = nullptr; // UI スレッドの宛先 (mu で守る)。nullptr なら外してある
	DockPoll *owner = nullptr;  // 積んだ呼び出しの中で使う。宛先がある間は有効
	// 並べ直しの要求 (画面共有が終わったときに Pipeline が true にする)。true の間は 2 つ目の呼び出しを積まない。
	std::atomic<bool> relayout_requested{false};
};

// 枠と名前ラベルの処理の状態。DockPoll がメンバー layout_state_ として持つ。
struct DockPollLayoutState {
	DockPollLayoutState();
	~DockPollLayoutState(); // ブリッジの宛先を外す。detach が呼ばれなかったときのため
	DockPollLayoutState(const DockPollLayoutState &) = delete;
	DockPollLayoutState &operator=(const DockPollLayoutState &) = delete;

	std::shared_ptr<DockPollLayoutBridge> bridge;
	// UI スレッドの宛先。DockPoll は QObject ではないので、宛先にする QObject を持つ。attach で作り、
	// detach で破棄する。破棄すると、まだ実行されていない呼び出しを Qt が捨てる。
	std::unique_ptr<QObject> context;
	bool attached = false;

	// 名前ラベルの値を渡した人と、渡したアバターの画像。次のポーリングで、いなくなった人の値を削除するのと、画像が
	// まだ分からない間に前の画像を使い続ける (dock/build_observation.h の decor_avatar) のに使う。
	std::set<std::string> decor_people;
	std::map<std::string, std::vector<std::uint8_t>> decor_avatars;
	// 合成のソースの、画面共有していない参加者の並びに、名前とアバターを渡した人と画像 (decor_people と同じ使い方)。
	std::set<std::string> roster_decor_people;
	std::map<std::string, std::vector<std::uint8_t>> roster_avatars;
	// 最後に渡した参加者の並び。Vesktop との接続が切れて最後のフレームを残している間は、並びも切れる前のままにする
	// (pipeline/pipeline.h の link_lost_frame_held)。
	std::vector<sources::CompositeRosterPerson> roster;
	// 全画面にできるタイル (全画面でないときの配置で、今描いているタイル)。合成のソースに配置の入力を渡すたびに決め直し
	// (sources/composite_input.h の composite_focus_tiles)、合成のソースがない間は空にする。ホットキーの位置から人を選ぶのと、
	// Observation の全画面にできる人と全画面の名前に使う。
	std::vector<sources::CompositeFocusTile> focus_tiles;
	// 全画面でないときの配置に描いていないが、全画面にできる人 (画面共有している人。sources/composite_input.h の
	// composite_focus_others)。focus_tiles と同じときに決め直す。
	std::vector<sources::CompositeFocusOther> focus_others;
};

// 枠と名前ラベルの処理。static の関数だけを持ち、第 1 引数に DockPoll を取る。DockPoll はこの型を friend にしている。
struct DockPollLayoutPart {
	// DockPoll::start の最後から呼ぶ。OBS を再起動しなくても反映される設定 (push_runtime_values) を渡し、
	// Pipeline からの並べ直しの要求を受け付ける。何度呼んでもよい。
	static void attach(DockPoll &self);
	// DockPoll::stop の最初から呼ぶ。Pipeline からの要求を外す。attach の前に
	// 呼んでもよい。
	static void detach(DockPoll &self);
	// 設定ウィンドウの [OK] か [適用] で設定が変わった後に呼ぶ。push_runtime_values の値を渡し直す。
	static void on_settings_changed(DockPoll &self);

	// ---- dock_poll_layout.cpp の中だけで使う ----

	// OBS を再起動しなくても反映される設定を、sources と Pipeline に渡す (attach と on_settings_changed から呼ぶ)。
	static void push_runtime_values(DockPoll &self);
	// ブリッジが積んだ呼び出しの中身。UI スレッドで、並べ直しを行う。
	static void run_requested_relayout(DockPoll &self);
	// 自分の枠に映すソースを探して合成のソースに結び付け、設定の uuid と名前が見つけたソースと違えば書き直す (ポーリングの
	// たびに、自分の枠が表示されているかを調べる前に呼ぶ)。
	static void sync_self_frame_source(DockPoll &self, const pipeline::PipelineSnapshot &snapshot);
	// 参加者ごとの名前ラベルの値と、自分の枠に載せる人を sources に渡す (ポーリングのたびに呼ぶ)。
	static void distribute_person_decor(DockPoll &self, const pipeline::PipelineSnapshot &snapshot);
	// 合成のソースに渡す配置の入力 (見えている画面共有の枠、声を聞けるか、画面共有していない参加者の並び) を作って
	// 渡し、並ぶ人の名前とアバターを合成のソースの表に渡す。合成のソースがあるときに呼ぶ。
	static void push_composite_input(DockPoll &self, const pipeline::PipelineSnapshot &snapshot,
					 bool self_frame_shown);
	// 全画面にできるタイルを tiles に、配置に描いていない全画面にできる人を others に替え、全画面で表示している人を全画面に
	// できなくなったか、cordmux が無効なら、全画面をやめる (sources/composite_input.h の focus_after_layout)。やめたときは
	// ログに 1 行書く。枠に載せていない人を全画面にしている間は、その人を Pipeline に渡して受信させる (set_focus_target)。
	static void settle_focus(DockPoll &self, std::vector<sources::CompositeFocusTile> tiles,
				 std::vector<sources::CompositeFocusOther> others);
};

} // namespace cordmux::dock

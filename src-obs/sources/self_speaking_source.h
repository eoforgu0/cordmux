// SPDX-License-Identifier: Apache-2.0

// 自分の枠の人の話している・話していないを決める OBS の音声ソース (設定の self_speaking_source_uuid と
// self_speaking_source_name) の探し方と、音の読み方。選んだソースに obs_source_add_audio_capture_callback で
// コールバックを付け、フィルタの後の音のパケットごとに peak を求めて、sources/self_speaking_judge.h の判定に渡す。
// 判定の結果は decor_registry.h の set_source_speaking などで表に書き、読む側が user_id で選ぶ。
// 付け外しと探し直しはドックの周期のスレッド (UI のスレッド) だけで行う。コールバックは、ソースの音声のスレッドから、
// 軽い計算とこのファイルの状態のロック、表のロックだけで動く。

#pragma once

#include <string>
#include <vector>

#include "sources/self_speaking_judge.h"

namespace cordmux::sources {

// 設定の値。
struct SelfSpeakingSetting {
	std::string uuid;                                           // 空なら名前だけで探す
	std::string name;                                           // 表示と、UUID で見つからないときの探し直しに使う
	int threshold_db = config::kDefaultSelfSpeakingThresholdDb; // 話し始めのしきい値 (dBFS)
};

// sync_self_speaking の結果。rewrite が真なら、ソースを見つけた結果が設定の値と違うので、uuid と name を設定に書き直す
// (名前で見つけたときと、利用者がソースの名前を変えたとき)。
struct SelfSpeakingSync {
	bool rewrite = false;
	std::string uuid;
	std::string name;
};

// 設定に合わせて、コールバックの付け外しとソースの探し直しを行う。ドックの周期ごとに呼ぶ。
//   setting   設定の値。uuid と name が両方とも空なら指定なしで、コールバックを外す
//   user_id   自分の枠の人の user_id (空なら、指定があっても Discord から受け取った声で決める)
// 指定があって見つからない間は、呼ぶたびに探し直す。UI のスレッドから呼ぶこと。
SelfSpeakingSync sync_self_speaking(const SelfSpeakingSetting &setting, const std::string &user_id);

// コールバックを外し、ソースの参照を捨てる。シーンコレクションの切り替え (OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
// と、OBS の終了、プラグインの終了のときに呼ぶ。何度呼んでもよい。次の sync_self_speaking が探し直す。UI のスレッドから呼ぶこと。
void release_self_speaking();

// 設定ウィンドウの候補にするソース (音声を持つ入力のソースで、cordmux の種類を除く)。名前の順。UI のスレッドから呼ぶこと。
std::vector<SelfSourceCandidate> list_self_speaking_candidates();

// 前に読んでからのパケットの peak の分布と、判定に使ったしきい値。パケットがなければ summary.packets が 0。どのスレッド
// から呼んでもよい (ログのスレッドが 10 秒ごとに読む)。
struct SelfSpeakingStats {
	PeakHistogram::Summary summary;
	int threshold_db = config::kDefaultSelfSpeakingThresholdDb;
};
SelfSpeakingStats take_self_speaking_stats();

} // namespace cordmux::sources

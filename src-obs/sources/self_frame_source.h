// SPDX-License-Identifier: Apache-2.0

// 自分の枠に映す OBS のソース (設定の self_frame_source_uuid と self_frame_source_name) の探し方。UUID で探し、
// 見つからなければ名前で探して、見つけたソースを合成のソースに渡す (composite_bind_self_frame)。探し直しと結び付けは
// ドックの周期のスレッド (UI のスレッド) だけで行う。自分の枠の音声ソース (sources/self_speaking_source.h) と同じ作り。

#pragma once

#include <string>
#include <vector>

#include "sources/self_speaking_judge.h"

namespace cordmux::sources {

// 設定の値。
struct SelfFrameSetting {
	std::string uuid; // 空なら名前だけで探す
	std::string name; // 表示と、UUID で見つからないときの探し直しに使う
};

// sync_self_frame の結果。rewrite が真なら、ソースを見つけた結果が設定の値と違うので、uuid と name を設定に書き直す
// (名前で見つけたときと、利用者がソースの名前を変えたとき)。
struct SelfFrameSync {
	bool rewrite = false;
	std::string uuid;
	std::string name;
};

// 設定に合わせて、ソースの結び付けと探し直しを行う。ドックの周期ごとに呼ぶ。uuid と name が両方とも空なら指定なしで、
// 結び付けを外す。指定があって見つからない間は、呼ぶたびに探し直す。UI のスレッドから呼ぶこと。
SelfFrameSync sync_self_frame(const SelfFrameSetting &setting);

// 結び付けを外し、探した状態を捨てる。シーンコレクションの切り替え (OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) と、
// OBS の終了、プラグインの終了のときに呼ぶ。何度呼んでもよい。次の sync_self_frame が探し直す。UI のスレッドから呼ぶこと。
void release_self_frame();

// 設定ウィンドウの候補にするソース (映像を持つ入力のソースで、cordmux の種類を除く)。名前の順。UI のスレッドから呼ぶこと。
std::vector<SelfSourceCandidate> list_self_frame_candidates();

} // namespace cordmux::sources

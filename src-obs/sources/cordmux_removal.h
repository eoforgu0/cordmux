// SPDX-License-Identifier: Apache-2.0

// アンインストールのうち、現在のシーンコレクションの中の cordmux のもの (ソース) を
// 探して削除し、残りを数える処理。ほかのシーンコレクションを順に開くのは install/uninstall.h の walk_collections で、
// ここでは 1 つのシーンコレクションの中だけを扱う。削除するものの一覧と探し方は install/removal_table.h にある。
// UI のスレッドから呼び出す (シーンの削除は、OBS のシーンの一覧に同期で届く。OBS 31.1.1 の
// frontend/widgets/OBSBasic_SceneItems.cpp の SourceRemoved は、同じスレッドからなら直接呼ばれる)。

#pragma once

#include <vector>

#include "install/uninstall.h"

namespace cordmux::sources {

// 現在のシーンコレクションから items の種類を並びの順に削除し、削除した後に残っている数を種類ごとに返す (items と
// 同じ並びで、同じ数)。
//
// 種類ごとの探し方 (install/removal_table.h の Locate と同じ):
//   CordmuxSources (SourceTypeId) -- 合成のソース (cordmux) の種類の ID のソース。名前では探さない (利用者が OBS の [+] から追加したものも削除する)。合成のソースの子と、子の
//       フィルタは非公開で保存されないので、探さない。
// 知らない種類が来たら、削除せずに「1 つ残っている」と数える (一覧に種類を追加したのに、削除の処理を追加し
// 忘れたことに気付けるようにするため)。
//
// 削除の方法 (OBS 31.1.1 の libobs で確認した):
//   - ソースは obs_source_remove で削除する (OBS 自身がソースを削除するときと同じ関数。保存のときは、
//     removed のフラグが付いたものを書かない。libobs/obs.c の obs_save_sources_filtered)。
//   - その前に、それを指すシーンアイテムを、すべてのシーンとグループから obs_sceneitem_remove で外す。
//     obs_source_remove はシーンアイテムを描画の周期で遅れて外す (libobs/obs-scene.c の
//     update_transforms_and_prune_sources) が、シーンの保存はシーンアイテムをすべて書く (同じファイルの
//     scene_save)。そのため、すぐにシーンコレクションを切り替えると、切り替える前の保存に、削除したソースを
//     指すシーンアイテムが残る。
std::vector<install::RemovalCount> remove_cordmux_objects(const std::vector<install::RemovalItem> &items);

} // namespace cordmux::sources

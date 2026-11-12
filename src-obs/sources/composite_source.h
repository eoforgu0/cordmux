// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux。利用者がシーンに置くのはこの 1 つのソースで、その中に、キャンバスの 4 等分の決まった位置
// (sources/composite_layout.h) で、参加者ごとの画面共有の映像と自分の枠の映像を描く。
//
// 画面共有の映像は、枠 (1 から kScreenSlotCount) ごとに 1 つずつある非公開の子のソース (cordmux_tile。非同期の映像だけで、
// 音はない) に渡し、合成のソースが video_render で子を描く。枠に載せていない人を全画面で映すときは、その人の映像を
// もう 1 つの子 (kFocusOnlySlot) に渡し、全画面のときだけ描く。子は OBS の「ソースを追加」の一覧に出さない。子を持つのは
// このファイルのモジュールの状態 (合成のソースの外) で、最初の合成のソースを作るときに作り、最後の合成のソースを
// 削除するとき (と obs_module_unload) に解放する。合成のソースが 1 つもないシーンコレクションでは、子はなく、映像は
// 渡さない (screen_feed.h)。
//
// 自分の枠の映像は、利用者がドックの [表示] タブで選んだ既存の OBS のソース (設定の self_frame_source_uuid と
// self_frame_source_name。sources/self_frame_source.h が探して composite_bind_self_frame で渡す) で、モジュールに 1 つ持つ。
// すべての合成のソースが同じソースを描く。合成のソースは、自分でソースを選ぶプロパティを持たない (プロパティは設定ウィンドウを開くボタンだけ)。
//
// 表示の遅れの測定 (screen_feed.h の note_screen_frame_shown) は、子に付けた非公開のフィルタ (cordmux_tile_probe) が
// 受ける。OBS が表示に選んだフレームは、フィルタの filter_video でしか取り出せない (obs_source_get_frame を描画の前に
// 呼ぶと、描画が受け取るフレームを奪って映像が止まり、描画の後に呼ぶと何も返らない)。
//
// ロック。次の順に取り、逆の順に取る処理を作らない。
//   composite_source.cpp の lifecycle ロック (子と tick のコールバックの作成と解放) -> module のロック (g_mu)
//   dummy_feed.cpp の状態のロック -> module のロック (渡すスレッドの知らせのロックは、持ったまま状態のロックを取らない)
// module のロックは、子の表、配置の入力、合成のソースの一覧だけを守る短いロックで、持ったまま呼ぶ OBS の関数は
// obs_source_get_ref だけである (子の作成と解放、フレームの出力、描画は、ロックを放してから行う)。描画のスレッドが
// 取るのはこのロックだけで、lifecycle ロックは取らない。子への出力は、module のロックを
// 放してから FrameSink の出力のロックを取る。
//
// 名前ラベルと話している人の光 (sources/composite_decor.h) を描くとき、描画のスレッドは module のロックを放した後に、
// 次のロックを 1 つずつ順に取る。どれも持ったままほかの cordmux のロックを取らない (3 つ目だけは例外で、内部のロックを
// 持ったまま取る)。
//   - decor_registry.cpp の表のロック (read_composite_decor と read_composite_roster)。持ったままコピーするだけで、OBS の
//     関数は呼ばない
//   - CompositeDecor の内部のロック (グラフィックスのコンテキストの内側で取る。destroy も同じ順)
//   - 描き直しの受け渡しのロック (composite_paint_handoff.h の mu_)。葉のロックで、持ったまま何も取らず、描く関数
//     (QPainter) も呼ばない。10 秒ごとの Stats の行を書くスレッドは、module のロックで一覧をコピーして放した後に、
//     内部のロックと受け渡しのロックを順に取る
//   - 描き直しのスレッドは、受け渡しのロックと、OBS のグラフィックスのコンテキスト (テクスチャを作るとき) を取る。
//     どちらかを持ったままもう一方を取らない。描画のスレッドはコンテキストの中で受け渡しのロックを取るので、逆の順を
//     作らないためである
//   - 合成のソースの削除と obs_module_unload は、描き直しのスレッドを止めて待ってから (どのロックも持たず、
//     obs_enter_graphics の外で) テクスチャを破棄する。コンテキストの中で待つと、描き直しのスレッドがコンテキストを
//     待ったまま止まらない
// 自分の枠の映像を選び直す composite_bind_self_frame (UI のスレッドから呼ぶ) は、自分の枠のロック (g_self_frame_mu) を
// 取り、その下で module のロックを短く取って合成のソースの一覧をコピーし、放してから、合成のソースごとの life_mu を 1 つずつ
// 取る。順は 自分の枠のロック -> life_mu で、module のロックは持ったまま life_mu を取らない。composite_destroy も同じ順に
// 取る。選んだソースの TargetRef のロックは葉で、描画のスレッドが取る。どのロックを持っていても、このロックから先へは取らない。
// 選び直した差分の強い参照は、ロックを放してから解放する (ソースの破棄が合成のソースの破棄を呼ぶことがあるため)。
// OBS のミキサーの行のミュートと音量 (obs_source_muted、obs_source_get_volume) は、どのロックも持たずに読む。
// 発言の時刻を表に書くスレッド (pipeline.cpp の mixer_loop) は、sources::set_voice_speaking で表のロックだけを取る。

#pragma once

#include <cstddef>
#include <cstdint>

#include <obs-module.h>

#include "sources/composite_decor.h"
#include "sources/composite_input.h"

namespace cordmux::sources {

// obs_register_source に渡す値を作って返す (sources.cpp の register_source_types から呼び出す)。
obs_source_info make_composite_source_info();
obs_source_info make_tile_source_info();
obs_source_info make_tile_probe_filter_info();

// 合成のソースのプロパティの [cordmuxの設定を開く] で呼ぶ関数を登録する (nullptr で外す)。設定ウィンドウを作るのはドックなので、
// plugin-main.cpp がドックを追加できたときに登録し、ドックを止める前に外す。
void set_composite_open_settings(void (*open)());

// 合成のソースが現在のシーンコレクションにあるか。映像を渡す先と、表示の緩衝の使い方、フレームを渡す先があるかの判断が
// これで決まる。どのスレッドから呼んでもよい (ロックを取らない)。
bool composite_present();

// 配置の入力を渡す (ドックの周期から。dock/dock_poll_layout.cpp)。描画のスレッドが次の描画から使う。合成のソースが
// なくても呼んでよい (値を覚えるだけ)。
void set_composite_input(const CompositeInput &input);

// 自分の枠の映像を描くソースを uuid のソースに替える (空なら外す)。見つからなければ外す。すべての合成のソースが、替えた
// ソースを描く。アクティブか表示中の合成のソースには、替えたことを伝える。伝えるときに libobs が循環と判断したら (替えた
// ソースが合成のソースを含むとき)、結び付けずに外して false を返す。それ以外は true。UI のスレッドから呼ぶこと。どの
// ロックも持たずに呼ぶ。
bool composite_bind_self_frame(const char *uuid);

// 自分の枠の映像を描くソースの強い参照を返す (選んでいないか、選んだソースが OBS から消えたときは nullptr)。呼び出し元が
// obs_source_release で解放する。
obs_source_t *composite_self_frame_ref();

// 自分の枠が配置に数えられるか。合成のソースがあり、自分の枠の映像を描くソースが今あるとき true。
bool self_frame_shown();

// 枠 slot の子に映像を渡す。合成のソースがないとき (子がないとき)、枠の番号が範囲の外のときは何もしない。
// unbuffered の約束は screen_feed.h の push_screen_video と同じ。
void output_tile_video(int slot, const obs_source_frame2 *frame, bool unbuffered);

// 枠 slot の子をすぐ空ける、または hold_ns だけ待ってから空ける予約を置く。子がなければ何もしない。
void clear_tile_now(int slot);
void hold_then_clear_tile(int slot, std::uint64_t hold_ns);

// 混ぜた音を合成のソースから出す (obs_source_output_audio)。audios は count 個で、時刻の順に出す。合成のソースが
// なければ捨てる。合成のソースが複数あるときは、先に作られた 1 つにだけ出す (全部に出すと、同じ音が重なって鳴る)。
// 出す相手の参照は、module のロックと合成のソースの life_mu の下で取り、ロックを放してから出す。destroy と重なれば
// 参照が取れず、出さない。呼び出し元は、自分のロックを持たずに呼ぶこと。OBS はこの呼び出しの中で、ソースの
// フィルタ (利用者が付けた音のフィルタを含む) を filter_mutex の下で動かす。
void output_composite_audio(const obs_source_audio *audios, std::size_t count);

// 名前ラベルと光の描き直しのこの周期の値を、合成のソース全部について足して読み、0 に戻す (10 秒ごとの Stats の行用)。
// どのスレッドから呼んでもよい。合成のソースがなければ全部 0。
CompositeDecorPaintStats take_composite_decor_paint_stats();

// 子を解放する。最後の合成のソースを削除するときに内部で呼ぶ。obs_module_unload からも呼ぶ (何度呼んでもよい)。
// obs_module_unload から呼ぶと、残っている合成のソースの名前ラベルと光のテクスチャ、明るさを掛ける効果も破棄する。
void release_composite_children();

} // namespace cordmux::sources

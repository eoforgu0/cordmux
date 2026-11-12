// SPDX-License-Identifier: Apache-2.0

// 画面共有の映像を合成のソースの子 (composite_source.h) に渡す関数と、枠を空ける関数、表示の遅れの測定。
// 誰がどの枠にいるか (割り当て表) は config/ が持ち、このファイルは映像を渡す側の入口だけを持つ。枠を空ける
// タイミングは sources/screen_end.h で決める。

#pragma once

#include <cstdint>

#include <obs-module.h>

#include "pipeline/latency_stats.h"

namespace cordmux::sources {

// 接続が途切れたときに、最後のフレームを表示し続ける時間の既定値 (設定の link_lost_hold_ms の既定値 2000 と
// 同じ。値は hold_then_clear_screen の引数で届く)。2 秒は目安の値で、測って決めたものではない。
inline constexpr std::uint64_t kDefaultScreenHoldNs = 2'000'000'000ULL;

// デコードしたフレームを渡すたびに呼び出す。映像を出力し、保ってから空ける予約 (hold_then_clear_screen) が
// あれば取り消す (同じ枠に表示し直す)。frame は呼び出し元が所有したまま (obs_source_output_video2 が中で
// コピーする)。
//
// ダミー表示が ON の間は、この関数の先頭でフレームを捨てる。捨てる場所は、この関数の先頭の 1 か所だけである。
//
// 渡し先は、合成のソース (composite_source.h) が現在のシーンコレクションにあれば枠の子である。なければ何もしない。
//
// unbuffered が true なら、渡し先を OBS の緩衝なし (async_unbuffered) にして渡す。OBS は渡されたフレームの
// うち最新の 1 つをそのまま表示し、タイムスタンプで表示の時刻を決めない (表示の時刻は呼び出し元が決める)。false なら
// OBS の緩衝あり (既定) で、タイムスタンプを見て表示の時刻に合わせる。前の呼び出しと違う値で呼ぶと、そのフレームを
// 渡す直前に、OBS の非同期の映像の状態を空にして、新しい方式で最初のフレームから始め直す。
void push_screen_video(int slot, const obs_source_frame2 *frame, bool unbuffered = false);

namespace detail {

// ダミーのフレームだけを渡す内側の関数。push_screen_video から、先頭でフレームを捨てる処理を除いたもの。
// 実際のフレームには使わない (ダミー表示の間も、実際のフレームが表示されてしまう)。
void output_screen_video(int slot, const obs_source_frame2 *frame, bool unbuffered = false);

} // namespace detail

// 枠 slot に映像を渡す先があるか (合成のソースがあるとき真)。ダミー表示のフレームを渡すかの判断に使う。
bool screen_frame_target_exists(int slot);

// 表示の遅れの測定。OBS が表示に選んだ映像のフレーム (非同期の映像のソースが、渡されたフレームのうち、タイムスタンプを見て
// 表示に使うと決めたフレーム) のタイムスタンプと、そのときの OBS の時計の差を、枠ごとに足し上げて、統計の
// 周期ごとに取り出す。表示に選ばれたフレームは、合成のソースの子に付けたフィルタ (composite_source.cpp) の
// filter_video が受け取る。どの関数もほかのロックを取らない 1 つのロックだけを取り、OBS の関数は時計 (os_gettime_ns) の
// ほかは呼ばない (どのスレッドから呼び出してもよい)。

// 枠 slot に最後に渡したフレームの補正 (渡したタイムスタンプ - 送信側のタイムスタンプを OBS の時計に換算した値、us)。
// push_screen_video の直前に呼び出す。
// 表示されるフレームは渡したフレームより数フレーム前なので、この値は表示されるフレームの補正と、整列の補正の変化の分だけ違う (整列の
// 補正は 1 フレームに最大 500 us 動く)。呼び出されていない枠のフレームは数えない。
void set_screen_shown_correction_us(int slot, std::int64_t correction_us);

// 枠 slot のソースで OBS が表示に選んだフレーム (タイムスタンプ ns) を伝える。composite_source.cpp のフィルタの
// filter_video が呼ぶ。補正が設定されていない枠、範囲の外の slot では数えない。
void note_screen_frame_shown(int slot, std::uint64_t frame_timestamp_ns);

// 枠 slot の表示の遅れの統計を取り出して、空に戻す。範囲の外の slot は count が 0 の結果を返す。
pipeline::LatencySummary take_screen_shown_latency(int slot);

// 枠をすぐ空ける (透明にする)。画面共有の終了が分かったとき、通話から抜けたとき、ドックで枠から外したとき、
// ダミー表示を止めたときに呼び出す。保ってから空ける予約があれば取り消す。合成のソースがなければ何もしない。
// 取るのは合成のソースのモジュールのロックだけである。Pipeline の mu を持ったまま呼び出さない。
void clear_screen_now(int slot);

// hold_ns だけ待ってから枠を空ける予約を置く (Vesktop との接続が切れたときか、ハートビートが続けて返らなかった
// とき)。並べ直しはしない (呼び出し元が決める)。期限より前に push_screen_video でフレームが来たら、予約を
// 取り消す。既に空いている枠では何もしない。
void hold_then_clear_screen(int slot, std::uint64_t hold_ns);

} // namespace cordmux::sources

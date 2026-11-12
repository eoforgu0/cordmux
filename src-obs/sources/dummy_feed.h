// SPDX-License-Identifier: Apache-2.0

// ダミー表示の切り替えと、色のタイルのフレームを枠に渡す処理。ダミー表示は、通話がなくても見た目を確認するための
// 表示である。ダミーの間は、実際の画面共有の映像を push_screen_video の先頭で捨て、画面共有の音と声を
// pipeline/ の add_mixed_audio_locked で捨てて、音声を 1 つも出さない。ミュートと音量は利用者が参加者ごとに決める値
// なので、ダミー表示では変えない (変えて止めると、利用者が止めていた声が鳴り出すことがある)。
// 割り当て表 (config/) には触らない。誰がどの枠にいるかは通話の事実で、ダミーは表示だけを差し替える。

#pragma once

#include <atomic>
#include <string>

namespace cordmux::sources {

namespace detail {

// ダミー表示が ON か。push_screen_video の先頭と、pipeline/ の add_mixed_audio_locked で 1 回ずつ読む。フレームと
// 音声のチャンクごとに通る処理なので、ロックを取らずに読めるように atomic で持つ。
extern std::atomic<bool> g_dummy_active;

} // namespace detail

// g_dummy_active を読むだけの関数。映像と音声を捨てる所とドックの表示が、同じ 1 つの値を見る。
inline bool dummy_mode_active()
{
	return detail::g_dummy_active.load(std::memory_order_relaxed);
}

// ダミーの枠の、名前ラベルに描く人の ID (名前ラベルが情報を引く人)。Discord の userId は数字だけ
// なので、この形の ID とは重ならない。ダミー表示の間、名前ラベルは画面共有の枠の人を、この ID として
// 読む (decor_registry.h の set_decor_owner)。
inline std::string dummy_person_id(int slot)
{
	return "dummy:" + std::to_string(slot);
}

// ダミー表示の間の、枠 slot のダミーのタイルを全画面で映すときに見分ける user_id (sources/composite_input.h の
// CompositeShare::user_id)。その枠に載っている人 (owner_user_id) がいればその人にし、いなければダミーの人の ID にする。
// ダミーに切り替えても、全画面で映している人の枠のダミーを全面に描き続け、ダミーを止めると、ダミーの人の全画面は、その人を
// 描かなくなるのでやめるためである。名前ラベルが情報を引く人 (dummy_person_id) とは別の値である。
inline std::string dummy_tile_user_id(int slot, const std::string &owner_user_id)
{
	return owner_user_id.empty() ? dummy_person_id(slot) : owner_user_id;
}

// ダミー表示を開始する、または止める。
//
// ON: 実際の映像と音声を捨て始め、OBS の tick のコールバック (obs_add_tick_callback) から、
//     色のタイルのフレームを渡し始める。ダミーの人の名前ラベルの情報 (「ダミー %1」) を名前ラベルの表に入れる。
//     枠ごとの背景の画像 (番号) は、渡すスレッドが最初のフレームの前に描画する。ソースは作らず、既にある合成のソースの子にだけ
//     フレームを渡す (なければ何も表示されない)。利用者が cordmux を置いていないシーンコレクションに、
//     置いていない物の結果を表示しないためである。
// OFF: 捨てるのをやめ、フレームを渡すのもやめて、すべての画面共有の枠をすぐ空ける (screen_feed.h の
//      clear_screen_now)。フレームが来ないだけでは枠を空けないので、止めた側が空ける。ダミーの人の情報も
//      消す。実際のフレームが来た枠は、次のフレームから表示し直す。
//
// フレームは OBS のキャンバスの大きさ (obs_get_video_info の base_width と base_height) の NV12 で、60 fps
// (dummy_tiles.h の kDummyFps。OBS のフレームレートが低ければ、描画の刻みごとに 1 つ) で作る。普通の映像が滑らかに動き、
// 潰れずに映るかを確かめるための表示なので、大きさとフレームレートは設定にしない。キャンバスの大きさが変わったら、
// 渡すスレッドが番号の入った背景を新しい大きさで描画し直し、次のフレームから新しい大きさにする。
// 渡し方は、本物の画面共有の映像 (pipeline/ の hand_frame_to_obs) にそろえる。形式は同じ 1 画素 1.5 バイトの YUV 4:2:0 で、
// 渡すのは専用のスレッド (本物の映像は pipeline/ のスレッド) である。OBS の描画のスレッドの tick のコールバックは、渡す時機を
// 決めて知らせるだけにする。そのため、ダミーの 4 つの枠の負荷は、キャンバスと同じ大きさの本物の画面共有 4 つと同じになる。
//
// 負荷 (描画と変換の時間のほかは計算した量。どれも OBS の中では測っていない):
//   - cordmux が書き換えるのは、フレームごとに動く線の列だけである (dummy_tiles.h の move_dummy_line。1080p で 1 枠 1
//     フレームあたり約 36 KB、4 つの枠の 60 fps で毎秒約 9 MB)。NV12 への変換は、背景を描いたときの 1 回だけである。背景の
//     描画と変換は、ON にしたときとキャンバスの大きさが変わったときに、渡すスレッドで行う。4 つの枠の描画と変換にかかった
//     時間は、1080p で約 80 ms、1440p で約 145 ms、2160p で約 330 ms (Release のビルドで、OBS の外で 7 回測った中央値。
//     最初の 1 回はフォントを読み込むので、1080p で約 0.45 秒)。その間はフレームを渡さず、枠は前の表示のまま止まる。UI の
//     スレッドで行わないのは、どの大きさでも 1 回あたり 50 ms を超え、その間ドックと OBS の画面が止まるためである。
//   - libobs は obs_source_output_video2 でフレームの全体を自分の領域へコピーし (渡すスレッドの中)、描画のときに同じ量を
//     GPU へ送る (描画のスレッド。本物の映像と同じ)。1 枠のフレームは 1080p で 3.1 MB、1440p で 5.5 MB、2160p で 12.4 MB
//     なので、4 つの枠の 60 fps では、コピーと送る量がそれぞれ毎秒約 0.75 GB、1.3 GB、3.0 GB になる。
//   - メモリーは、4 つの枠のフレーム (1080p で約 12 MB) と、背景を描き直す間だけ、同じ大きさの NV12 の画像をもう 1 組と、
//     描いている 1 枠分の BGRA の画像 (1080p で 8.3 MB) を持つ。
//
// どのスレッドから呼び出してもよい (普通は UI のスレッドから呼び出す)。2 回続けて呼び出してもよい (同じ値なら何もしない)。
// OFF にすると、渡すスレッドを止めて終わるのを待ってから戻る (背景を描画している途中なら、描画している 1 つの枠の分だけ
// 待つ。2160p で約 80 ms)。
// ON のまま cordmux.dll をアンロードしないこと (OBS の終了の前に OFF にする。dock/dock_poll.cpp)。
void set_dummy_mode(bool on);

// 動く線の 1 往復の時間 (ミリ秒。設定ファイルの dummy_cycle_ms。既定値は 4000)。2000 より短い値は 2000 にする
// (同じ場所が明暗で何度も入れ替わらないように、1 往復を 2 秒以上にする)。次のフレームから反映される。
void set_dummy_cycle_ms(int ms);

// ダミーの名前ラベルの名前の書式 (「ダミー %1」の形。%1 に枠の番号が入る)。空のとき (既定値) は、ini の
// cordmux.source.placeholder.label から作る名前 (sources/scene_names.h の dummy_display_name) を使う。ON の間に
// 呼び出しても、次から反映される。
void set_dummy_label_format(const std::string &format_with_percent1);

} // namespace cordmux::sources

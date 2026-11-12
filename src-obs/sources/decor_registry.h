// SPDX-License-Identifier: Apache-2.0

// 人の情報、名前ラベルに描く人、発言、見た目の値の表 (cordmux 全体で 1 つ)。合成のソース (cordmux) の
// 名前ラベルと光と暗さが、描くたびにここから値をコピーして読む。書くのは配信の経路とドックの
// 周期で、OBS の設定 (obs_data) には何も書かないので、人の情報、名前ラベルに描く人、発言、見た目の値は OBS に保存
// されない (アバターがシーンコレクションに残らない)。外部と通信せず、アバターは送信側から届いた画像を使う。
// このファイルのロックは、どれもほかのロックを取らないロックである。

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sources/decor_paint.h"
#include "sources/decor_params.h"
#include "sources/label_border.h"
#include "sources/scene_names.h"
#include "sources/self_speaking_judge.h"

namespace cordmux::sources {

// ---- 見た目の値 (cordmux 全体で 1 つ) ----

// 見た目の値の型 (DecorLabelParams) と、設定からその値を作る純粋関数は sources/decor_params.h にある (libobs に
// 依存しない)。

// 名前ラベルの見た目の値を差し替える (次に描画するフレームから反映される。値が前と同じなら何もしない)。
// どのスレッドから呼び出してもよい。
void set_decor_label_params(const DecorLabelParams &params);

// ---- 人の情報と、名前ラベルに描く人 ----

// 1 人分の情報。名前ラベルの情報は枠ではなく人に付け、枠には名前ラベルに描く人 (set_decor_owner) を持たせる。
struct PersonDecor {
	std::string name;                              // 描画する名前 (読めなければ空。userId を入れない)
	std::vector<std::uint8_t> avatar_png;          // 空なら描画しない
	DecorCorner corner = DecorCorner::BottomLeft;  // 名前ラベルを置く角 (decor_paint.h の DecorCorner)
	std::vector<std::string> other_visible_names;  // 同じシーンに見えているほかの人の名前の全文
};

// 人の情報を入れる、または消す。前と同じ情報なら何もしない (描画し直さない)。アバターの PNG の読み込みは、
// 表のロックの外で、PNG が変わったときだけ行う。どのスレッドから呼び出してもよい。
void set_person_decor(const std::string &user_id, const PersonDecor &decor);
void forget_person_decor(const std::string &user_id);

// 枠の名前ラベルに描く人。slot は 1 から 4 が画面共有の枠、kSelfSlot (0) が自分の枠、kFocusOnlySlot (5) が全画面だけで
// 映す人の子。user_id が空なら描く人は
// いない (名前ラベルを描画しない)。名前ラベルはこの人の情報だけを描画するので、人が替わった最初のフレームから、
// 前の人の名前は表示されない (新しい人の情報がまだなければ、名前ラベルを描画しない)。OBS の関数を呼ばない
// (デコードのスレッドから呼び出せる)。ダミー表示の間は、画面共有の枠の人をダミーの人 (dummy_feed.h の
// dummy_person_id) として読む (ここで設定した値は消さずに残り、ダミー表示を止めると元に戻る)。
void set_decor_owner(int slot, const std::string &user_id);

// ---- 合成のソースが読む値 ----
//
// 合成のソース (composite_source.h) は、この表から、枠の描く人と人の情報を
// 読む。次の 5 つは、それぞれ別の値である。
//   - 合成のソースの名前ラベルの見た目の値 (set_composite_label_look)。
//   - 枠の背景の値 (set_composite_background)。
//   - 声の区間から求めた発言 (set_voice_speaking)。合成のソースで話している人を示す光は、ミキサーが声を OBS へ出した
//     区間から求める。
//   - 画面共有していない参加者の並びの設定 (set_composite_roster_params)。
//   - 画面共有していない参加者の人の情報 (set_roster_person_decor)。枠の人の情報 (set_person_decor) とは別の表に持つ。
// 話していない人の暗さ (DecorLabelParams::dim_pct) は set_decor_label_params で入るが、画像には描かず置くときに掛けるので、
// 変わっても表の version を進めない (名前ラベルを描き直さない)。

// 合成のソースの名前ラベルとグローの見た目の値を差し替える (値が前と同じなら何もしない)。どのスレッドから
// 呼び出してもよい。
void set_composite_label_look(const CompositeLabelLook &look);

// 枠の背景 (映像を描かない所の色) の値を差し替える。version は持たず、描画のたびにコピーされる (read_composite_background)。
// どのスレッドから呼び出してもよい。
void set_composite_background(const CompositeBackground &background);

// 枠の背景の値のコピーを返す。取るのは表のロックだけで、OBS の関数は呼ばない。
CompositeBackground read_composite_background();

// 画面共有していない参加者の並びの設定を差し替える。描画のたびにコピーされ、version は持たない (並びの名前ラベルの画像に
// 動作するのはアバターの位置だけで、それは描き直す鍵に入る)。どのスレッドから呼び出してもよい。
void set_composite_roster_params(const CompositeRosterParams &params);

// 画面共有していない参加者 1 人分の情報。
struct RosterPersonDecor {
	std::string name;                     // 描画する名前 (読めなければ空。userId を入れない)
	std::vector<std::uint8_t> avatar_png; // 空なら描画しない
};

// 画面共有していない参加者の人の情報を入れる、または消す。user_id で引く表で、並べる人の一覧 (composite_input.h の
// CompositeInput::roster) とは別に持つ。前と同じ情報なら何もしない (描き直さない)。変わったら、その人の version が進む。
// アバターの PNG の読み込みは、表のロックの外で、PNG が変わったときだけ行う。どのスレッドから呼び出してもよい。
void set_roster_person_decor(const std::string &user_id, const RosterPersonDecor &decor);
void forget_roster_person_decor(const std::string &user_id);

// 人の声が OBS へ出た時刻 (os_gettime_ns の時計) を渡す。start_ns が 0 でなければ話し始め、stop_ns が 0 でなければ話し
// 終わり (止まりだけを渡すときは start_ns を 0 にしてよい。話し始めの時刻を引き継ぐ)。時刻は声が鳴る時刻なので、遅延を
// 足さずに渡す。光は描画するたびにこの時刻から明るさを求める (label_border.h)。どのスレッドから呼び出してもよい
// (取るのは、ほかのロックを取らないロック 1 つ)。
void set_voice_speaking(const std::string &user_id, std::uint64_t start_ns, std::uint64_t stop_ns);

// set_voice_speaking で渡した発言をすべて捨てる (合成のソースが現れた、なくなった、停止した、のとき)。
void clear_voice_speaking();

// ---- 音声ソースで決める発言 ----
//
// 設定で OBS の音声ソースを選んだときは、自分の枠の人 (その人が画面共有の枠に載っているときはその枠も) の発言を、
// set_voice_speaking の時刻ではなく、音声ソースの音から求めた時刻で決める (sources/self_speaking_source.h)。読む側
// (read_composite_decor と read_composite_roster) が user_id で選ぶので、Discord から受け取った声の経路とミュートの
// 表は変えない。どの関数も、取るのは表のロック 1 つだけで、OBS の関数は呼ばない。どのスレッドから呼び出してもよい。

// 音声ソースで発言を決める人の user_id。空なら指定なし (読む側は Discord から受け取った声の時刻を使う)。
void set_source_voice_user(const std::string &user_id);

// 指定した音声ソースを見つけて、コールバックを付けているか。偽の間は、判定できない状態になる。
void set_source_voice_found(bool found);

// 音声ソースのパケットを受け取った時刻 (os_gettime_ns)。250 ms 届かないと判定できない状態になる。
void note_source_voice_packet(std::uint64_t now_ns);

// 音声ソースの音から求めた発言の時刻を渡す。意味は set_voice_speaking と同じ。
void set_source_speaking(std::uint64_t start_ns, std::uint64_t stop_ns);

// 音声ソースから求めた発言を捨てる (ソースを付け直した、見失った、パケットが途切れた、のとき)。
void clear_source_speaking();

// 合成のソースが表から読む値。read_composite_decor が作る。
struct CompositeDecorView {
	// 人の情報、枠の描く人、見た目の値のどれかが変わるたびに進む表の version と、合成のソースの見た目の値の
	// version。
	std::uint64_t version = 0;
	std::uint64_t look_version = 0;
	// 添字は枠の番号 (0 が自分の枠、1 から 4 が画面共有の枠、5 が全画面だけで映す人の子)。名前ラベルに描く人。ダミー表示の間の画面共有の枠は
	// ダミーの人になる。
	std::array<std::string, kScreenTileCount + 1> owners;
	// 枠の描く人の発言。描く人がいないか、発言がなければ start_ns が 0。音声ソースで決める人は source_voice の値に
	// 従う (set_source_speaking の時刻か、判定できない間は空)。それ以外の人は set_voice_speaking の時刻。
	std::array<SpeakingTimes, kScreenTileCount + 1> speaking;
	// 枠の描く人の発言を音声ソースで決めているか。Determinable の人は、cordmux がその人の声を聞けるものとして扱い、
	// Undeterminable の人は聞けないものとして扱う (source_voice_hears)。
	std::array<SourceVoice, kScreenTileCount + 1> source_voice{};
	// 話していない人の暗さ (%。DecorLabelParams::dim_pct)。version によらず、いつもコピーする。
	int dim_pct = 0;
	// 下の値は、表から人の情報と見た目の値をコピーしたとき (呼び出し元が前にコピーしたものと違うとき) だけ入る。
	bool material_valid = false;
	std::array<bool, kScreenTileCount + 1> person_present{};
	std::array<LabelContent, kScreenTileCount + 1> person;
	DecorLabelParams params;
	CompositeLabelLook look;
};

// 表から合成のソースが要る値をコピーする。取るのは表のロックだけで、OBS の関数は呼ばない。known_* は前にコピー
// したときの version と描く人で、どれも変わっていなければ material_valid が偽になり、人の情報と見た目の値は
// コピーしない。
CompositeDecorView read_composite_decor(std::uint64_t known_version, std::uint64_t known_look_version,
					const std::array<std::string, kScreenTileCount + 1> &known_owners);

// 画面共有していない参加者 1 人分の、表から読んだ値。present が偽なら、表にその人の情報がない (名前ラベルを描かない)。
// copied が真のときだけ name と avatar が入る (version が呼び出し元の知っているものと違うとき)。
struct CompositeRosterMaterial {
	bool present = false;
	std::uint64_t version = 0;
	bool copied = false;
	std::string name;
	QImage avatar;
};

// 合成のソースが表から読む、画面共有していない参加者の値。read_composite_roster が作る。
struct CompositeRosterView {
	CompositeRosterParams params;                // 並びの設定 (set_composite_roster_params)
	std::vector<SpeakingTimes> speaking;         // 参加者ごとの発言 (CompositeDecorView::speaking と同じ決め方)。user_ids と同じ並び
	std::vector<SourceVoice> source_voice;       // 参加者ごとに音声ソースで決めているか。user_ids と同じ並び
	std::vector<CompositeRosterMaterial> people; // user_ids と同じ並び
};

// 表から、画面共有していない参加者の値をコピーする。取るのは表のロックだけで、OBS の関数は呼ばない。known_versions[i] は
// user_ids[i] の人について呼び出し元が前にコピーした version (まだなければ 0。足りない分は 0 として読む) で、同じなら
// 名前とアバターはコピーしない。並びの設定と発言はいつもコピーする。
CompositeRosterView read_composite_roster(const std::vector<std::string> &user_ids,
					  const std::vector<std::uint64_t> &known_versions);

} // namespace cordmux::sources

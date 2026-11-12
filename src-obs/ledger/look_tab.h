// SPDX-License-Identifier: Apache-2.0

// 表示タブの判断。入力欄を押せるか、外から届いた値で入力欄を書き換えるか、変えた値をいつ配信画面と settings.json に渡すかを
// 決める純粋関数である。libobs にも Qt にも依存しない。

#pragma once

#include <cstdint>

#include "config/settings.h"

namespace cordmux::ledger {

// 入力欄を変えてから、変えた値を配信画面に渡すまでの最短の間隔 (ms)。スライダーやスピンボックスを動かしている間は、
// この間隔ごとに最後の値だけを渡す。名前ラベルの値を渡すたびに、合成のソースが名前ラベルを描き直す。枠の並べ方は
// 250 ms ごとに配信画面へ渡しているが、表示タブの値を渡した直後に並べ直しも行うので、動かしている値が配信画面に出る遅れは、
// この間隔で決まる。
inline constexpr int kLookReflectIntervalMs = 100;

// 入力欄を最後に変えてから、settings.json に書くまでの時間 (ms)。変えるたびに数え直す。動かしている間に書き続けないため。
inline constexpr int kLookSaveDelayMs = 1000;

// 最後に渡した時刻 (last_reflect_ms) から interval_ms がたつまでの残り (ms)。0 なら今渡してよい。時刻が前に戻ったとき
// (now_ms < last_reflect_ms) も 0 を返す (戻ったままだと、時刻が追いつくまで渡せなくなる)。
int look_reflect_wait_ms(std::int64_t last_reflect_ms, std::int64_t now_ms, int interval_ms);

// 外から届いた値で、入力欄を書き換えるか。利用者が操作している最中 (editing) と、変えた値をまだ渡していない間
// (edit_pending) は書き換えない (書き換えると、動かしている入力欄が古い値に戻る)。値が同じなら書き換えない。
bool look_field_follows(bool editing, bool edit_pending, bool differs);

// ほかの入力欄の値で、押せるかが決まる入力欄。名前ラベルの値が配信画面に反映される条件と同じにする。
enum class LookControl {
	NameFont,        // フォント (名前を表示するときだけ)
	NameWeight,      // 太さ (同上)
	NameSize,        // フォントサイズ (同上)
	NameColor,       // 名前の色 (同上)
	OutlineShow,     // 輪郭を表示するか (輪郭は名前の文字だけに付くので、名前を表示するときだけ)
	OutlineColor,    // 輪郭の色 (輪郭を表示し、名前を表示するときだけ)
	OutlineSize,     // 輪郭のサイズ (同上)
	AvatarSize,      // アバターのサイズ (アバターを表示するときだけ)
	AvatarShape,     // アバターの形 (同上)
	PlateShow,       // 背景を表示するか (名前かアバターを表示するときだけ。どちらも出さなければ背景は描かない)
	PlateColor,      // 背景の色 (背景を表示し、名前かアバターを表示するときだけ)
	PlateOpacity,    // 背景の不透明度 (同上)
	SlotBackgroundColor,   // 枠の背景の色 (枠の背景を表示するときだけ)
	SlotBackgroundOpacity, // 枠の背景の不透明度 (同上)
	GlowTargets,     // 光らせる対象 (光を表示するときだけ)
	GlowColor,       // 光の色 (同上)
	GlowThickness,   // 光の太さ (同上)
	GlowBlur,        // 光のぼかし (同上)
	TimingFadeIn,    // フェードイン (光を表示するか、話していない人を暗くするときだけ。時間の 3 つは両方に適用される)
	TimingHold,      // 保持時間 (同上)
	TimingFadeOut,   // フェードアウト (同上)
	SelfThreshold,   // 自分の枠の音声ソースのしきい値 (音声ソースを選んだときだけ)
	SlotLabelHideWhenSingle, // 画面共有している参加者の [1人のときは非表示] (名前ラベルを枠に描くときだけ)
	SlotLabelCorner,         // 同上の [位置]
	SlotLabelAvatarSide,     // 同上の [アバターの位置]
	RosterAnchor,    // 画面共有していない参加者の配置 (並びを表示するときだけ)
	RosterOffset,    // 同上のオフセット (横と縦の 2 つの欄)
	RosterDirection, // 同上の並び
	RosterGap,       // 同上の間隔
	RosterNameWidth, // 同上の名前の最大幅 (名前を表示するときだけ)
	RosterAvatarSide, // 同上のアバターの位置
	RosterShowBots,  // 同上の Bot を表示
};

// 入力欄 control を押せるか。押せない入力欄も隠さず、ラベルと一緒に押せない見た目にする。
bool look_control_enabled(LookControl control, const config::Settings &settings);

// [先頭に配置] (設定の self_slot_first) を押せるか。自分の枠に映す映像が「(なし)」のとき (OBS のソースもシーンも選んでおらず、
// メインアカウントもないとき) は、先頭に置くものがないので押せない。メインアカウントの有無は、ドックの View の値
// (dockview::LookTabView::has_main_account) を渡す。表示タブが持つ設定の写しのメインアカウントは、設定ウィンドウで変えても
// 表示タブの値が変わらない間は書き換わらないため。
bool look_self_slot_first_enabled(const config::Settings &settings, bool has_main_account);

// 画面共有していない参加者の配置 anchor で、並び direction を選べるか (中央上と中央下は横並びだけ、左中央と右中央は縦並び
// だけ)。選べない並びは、選べない項目にする。
bool look_roster_direction_allowed(config::RosterAnchor anchor, config::RosterDirection direction);

// [自分の枠に映す映像] の先頭の選択肢の文字列の ini のキー。メインアカウントがあれば、サブアカウントの Discord の画面共有を
// そのまま使うので「Discordの画面共有」、なければ「(なし)」。保存する値は同じ (UUID と名前が空)。
const char *look_self_frame_first_option_key(bool has_main_account);

} // namespace cordmux::ledger

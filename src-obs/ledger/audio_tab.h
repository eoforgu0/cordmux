// SPDX-License-Identifier: Apache-2.0

// 音声タブの表示の判断。参加者タブの一覧 (PeopleListView) から、音声タブの行の並びと、各行の状態、行がないときの 1 文を決める
// (audio_tab_of)。メーターが出す値と色の使い分けも決める (meter_reading)。libobs にも Qt にも依存しない。

#pragma once

#include <optional>

#include "ledger/dock_view.h"

namespace cordmux::ledger {

// 音声タブを決める。
//   - 行は参加者タブの一覧と同じ人で、メインアカウントが先頭、ほかは表示名の自然順 (組分けしない)。参加者タブの
//     pinned が Vesktop のアカウント (メインアカウントだけの構成) のときは、その人の声も画面共有も受信側に届かず、
//     操作しても配信の音に反映されないので、行にしない。
//   - 合成のソースがない間 (composite_present が偽) は、値が配信の音に反映されないので、行を 1 つも持たず、notice に
//     ソースを足すと使えることを伝える 1 文を入れる。
//   - 合成のソースがあって行がないときは、notice に行が並ぶ場所であることを伝える 1 文を入れる。
//   - 声の行は、まだ声が届いていない人 (MicState::NoVoice) の control を偽にして note に 1 文を入れる。画面共有の音声の行は、
//     音声を持っていない人の control を偽にして、共有していない人と、共有しているが音声がない人で note を分ける。
//     画面共有の音声のミュートのボタンは、音声を持たない人の行に置かず、場所だけ空ける。
//   - Vesktop との接続が切れた行 (PersonRow::off) は、行を残して off にし、ミュートのボタンを押せなくする。
dockview::AudioTabView audio_tab_of(const dockview::PeopleListView &list, bool composite_present);

// メーター 1 本に出す値。
struct MeterReading {
	// 0 (-48 dB) から 1 (0 dB) の位置。
	float position = 0.0f;
	// 灰色で出すか。
	bool gray = false;
};

// 組の音の大きさ (pipeline/audio_mixer.h の AudioTrackLevel の値。線形の振れ幅) とつまみの音量 (%) から、メーターの値を決める。
// メーターは、いつも音量を掛けた後の大きさを出す (つまみを動かすと、ミュート中でも動く)。
//   - 音を出している間 (silenced が偽) は、配信に出ている音の大きさ peak_after を色付きで出す。
//   - ミュート中か音量 0 の間 (silenced が真) は、掛けた後が 0 になるので、掛ける前の値 peak_before に音量の利得
//     (config::gain_of_person_volume) を掛けた値、つまりミュートを外したときに出る大きさを灰色で出す。ミュート中に話している
//     人を見て、外すか決められるようにするためである。音量 0 は利得 0 なので、メーターは振れない。
MeterReading meter_reading(float peak_before, float peak_after, bool silenced, int volume_pct);

} // namespace cordmux::ledger

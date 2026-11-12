# 変更履歴 (Changelog)

## 1.0.1

### ja

#### 変更点

- 参加者が多いときに、声と画面共有の音声の処理で使うCPUを減らしました。
- ダミー映像を表示している間は、自分の枠を表示せず、4つの枠すべてにダミー映像を表示するようにしました。

#### 不具合の修正

- 自分の枠を使っている間に、[枠に載せる人の選び方] が「自動」のときや、[自動で枠に載せる] を付けた人が画面共有を始めたとき、それとほぼ同時にチェックを付けたときに、4人目が枠に載り、1人が描かれないことがある問題を修正しました。
- サブアカウントを使わない構成で、[枠に載せる人の選び方] が「自動」のときや、自分の行の [自動で枠に載せる] を付けているときに、自分がDiscordで画面共有すると、表示されない枠を1つ使い、ほかの参加者を1人少なくしか枠に載せられない問題を修正しました。

### en

#### Changes

- Reduced the CPU usage of processing voices and screen share audio when many people are in the call
- Changed placeholders to fill all four slots without showing the self slot

#### Bug Fixes

- Fixed an issue where a fourth person was added to a slot while the self slot was in use, when [Slot Assignment] was set to "Automatic", a person with [Add Automatically] started sharing, or a check box was selected at about the same time, so one slot wasn't drawn
- Fixed an issue where, without an alt account, your own Discord screen share took a slot that wasn't drawn when [Slot Assignment] was set to "Automatic" or [Add Automatically] was selected in your row, so one fewer participant could be added to slots

## 1.0.0

### ja

#### 新機能

- 最初のリリースです。

### en

#### New Features

- Released the first version

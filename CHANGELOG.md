# 変更履歴 (Changelog)

## 1.0.4

### ja

#### 新機能

- フィルタ「cordmux: 1人のときだけ表示」を追加しました。付けたソースは、cordmuxのソースが1人の映像だけを配信画面全体に映している間だけ表示されます。たとえば、過去の配信を全画面で映している間だけ「振り返り中」と表示できます。
- サブアカウントで使うとき、メインアカウントが同じ通話にいる間は、Vesktopで画面共有の音声が鳴らないようにしました。cordmuxが受信する音声には影響しません。Vesktopで聞きたいときは、画面共有を右クリックして音量を動かします。Discordの音量の設定は変えません。

### en

#### New Features

- Added the "cordmux: Show Only When One Person" filter. A source with this filter is shown only while the cordmux source shows one person's video across the whole stream, such as to show "Looking back" only while a past stream is shown full screen
- Stopped screen share audio from playing in Vesktop while your main account is in the same call when you use an alt account. This doesn't affect the audio that cordmux receives. To hear a screen share in Vesktop, right-click it and change its volume. Discord's volume settings aren't changed

## 1.0.3

### ja

#### 変更点

- 画面共有を受け取り始めた直後などに、映像が音より早く表示されるずれを小さくしました。

#### 不具合の修正

- 4Kなどの大きな画面共有を受け取り始めた直後などに、映像がほとんど表示されなくなることがある問題を修正しました。

### en

#### Changes

- Reduced how far video can be shown ahead of audio, such as right after a screen share starts to be received

#### Bug Fixes

- Fixed an issue where video from a large screen share such as 4K could almost stop being shown, such as right after it started to be received

## 1.0.2

### ja

#### 変更点

- 画面共有の映像と音のずれを小さくしました。映像と音の時刻を、届いた時刻からの見積もりではなく、Discordが再生に使う時刻に合わせます。

#### 不具合の修正

- 枠に載せられずに「空いている枠がありません。」と出た後、しばらくして別の理由で枠が埋まったときに、前に押した人の行にまた同じ文が出ることがある問題を修正しました。

### en

#### Changes

- Reduced the offset between screen share video and audio by timing them with the times Discord uses for playback instead of estimating from arrival times

#### Bug Fixes

- Fixed an issue where, after "No free slots." was shown because a person couldn't be added to a slot, the same text could appear again in that person's row later when the slots filled up for another reason

## 1.0.1

### ja

#### 変更点

- 参加者が多いときに、声と画面共有の音声の処理で使うCPUを減らしました。
- ダミー映像を表示している間は、自分の枠を表示せず、4つの枠すべてにダミー映像を表示するようにしました。

#### 不具合の修正

- 画面共有の音が、映像より0.15秒ほど遅れたまま配信と録画に入り続けることがある問題を修正しました。
- 自分の枠を使っている間に、[枠に載せる人の選び方] が「自動」のときや、[自動で枠に載せる] を付けた人が画面共有を始めたとき、それとほぼ同時にチェックを付けたときに、4人目が枠に載り、1人が描かれないことがある問題を修正しました。
- サブアカウントを使わない構成で、[枠に載せる人の選び方] が「自動」のときや、自分の行の [自動で枠に載せる] を付けているときに、自分がDiscordで画面共有すると、表示されない枠を1つ使い、ほかの参加者を1人少なくしか枠に載せられない問題を修正しました。

### en

#### Changes

- Reduced the CPU usage of processing voices and screen share audio when many people are in the call
- Changed placeholders to fill all four slots without showing the self slot

#### Bug Fixes

- Fixed an issue where screen share audio could stay about 0.15 seconds behind the video in the stream and recordings
- Fixed an issue where a fourth person was added to a slot while the self slot was in use, when [Slot Assignment] was set to "Automatic", a person with [Add Automatically] started sharing, or a check box was selected at about the same time, so one slot wasn't drawn
- Fixed an issue where, without an alt account, your own Discord screen share took a slot that wasn't drawn when [Slot Assignment] was set to "Automatic" or [Add Automatically] was selected in your row, so one fewer participant could be added to slots

## 1.0.0

### ja

#### 新機能

- 最初のリリースです。

### en

#### New Features

- Released the first version

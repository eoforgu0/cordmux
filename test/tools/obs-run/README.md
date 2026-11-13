# test/tools/obs-run

実機の OBS を起動して、cordmux の受信側 (cordmux.dll) を確認するテストツールの一式である。実行スクリプト (`run-window.mjs`) が、テスト用の OBS とテスト用の送信側 (`test/tools/harness`) を起動して実行計画を進め、録画やスクリーンショットを判定ツールで読む。実際の Vesktop と Discord の通話を使う実行もある。

テスト用の OBS はポータブル版で、リポジトリの `_work` の下に置く。利用者の OBS と Vesktop の設定には触れない作りにしてあるが、実際の通話を使う実行 (`call-*`) だけは、実際の Vesktop の `state.json` を書き換える。

## 実行の前提

- Windows 11、PowerShell 5.1、Node 22 以降 (obs-websocket とトークンなしの接続のテストが組み込みの `WebSocket` を使う)、Python 3 (判定ツールは numpy を使う)、ffmpeg と ffprobe (PATH にあるか、`src-obs/.deps` の OBS の依存に同梱のもの)、`nvidia-smi`。
- テスト用の OBS はポータブル版 33.0.1 で、`_work/obs-33.0.1` に置く。`--obs` はリポジトリの `_work` の下、`--out` は `_work` か `build_x64` の下だけを受け付ける。外を使うときは `--allow-outside-work` を付ける。この確認は `--dry-run` でも、開始前の確認より前に実行し、外なら終了コード 1 で止まる。
- `--plugin` には、cordmux をインストールしたフォルダー (`cordmux.dll` と `data` が直下にある。`cmake --install` の出力の `<prefix>/cordmux`) を渡す。cordmux.dll と、同梱の ffmpeg などを、テスト用の OBS の `plugins\cordmux\` の直下に、データを `plugins\cordmux\data\` にコピーする (OBS 33 の置き方)。
- `call-*` と、`--dock-install ours` または `missing` を使うステップは、`_work/vencord-dist` に cordmux 入りの Vencord のビルド成果 (`vencordDesktopMain.js` `vencordDesktopPreload.js` `vencordDesktopRenderer.js` `vencordDesktopRenderer.css` の 4 ファイルと、`package.json`) を置いておく。実行スクリプトは Vencord をビルドしない。`src-vencord` を Vencord の `src/userplugins/cordmux` に置いて `pnpm build --standalone --disable-updater` を実行し、生成された `dist/` の中身をコピーする。`python test/tools/vencord/check_dist.py _work/vencord-dist` で 5 ファイルがそろっていることを確認できる。4 ファイルがそろっていないと、`seed-config.py` は原本を用意できず、`ours` と `missing` が同じ表示になる。
- テスト用の OBS は `--portable --multi --disable-shutdown-check --disable-missing-files-check --disable-updater --verbose` で起動する。cordmux のログは DEBUG なので、`--verbose` がないとログに出ない。obs-websocket は `127.0.0.1:4460` で、認証はない。

### 開始前の確認

実行の始めに、次のどれかに当たると何もせずに終了コード 2 で止まる。

- Program Files の OBS (利用者の OBS)、場所が分からない `obs64.exe`、Vesktop が動いている。ただし `--call-window` を付けた実行 (`call-*`) は、Vesktop の稼働を許す。
- 環境変数 `CORDMUX_PROTECTED_PROCESSES` にテキストファイルのパスを入れると、そのファイルに書いたプロセスが動いているときも止まる。1 行に 1 つのプロセス名 (例 `foo.exe`) を書く。空行と `#` で始まる行は読まない。大文字と小文字は区別しない。`--call-window` の実行でも止まる。指定したのにファイルが読めないときも、守るはずのものを守らずに始めないよう、止まる。
- `nvidia-smi` のエンコーダの使用率が 0 より大きい。使用率を読めないときも止まる。読めなくても進めるときだけ `--allow-unknown-encoder` を付ける。`--call-window` の実行は、エンコーダを使っているプロセスが `discord.exe` `vesktop.exe` `chrome.exe` `firefox.exe` だけなら止まらない。

### テスト用の OBS と環境変数の差し替え

`call-*` 以外のステップは、子プロセス (OBS、後片付け、判定ツール) に環境変数を差し替えて渡す。

| 変数 | 差し替え先 |
|---|---|
| `APPDATA` | `_work/appdata` |
| `VENCORD_USER_DATA_DIR` | `_work/appdata/Vesktop` |
| `LOCALAPPDATA` | `_work/localappdata` |
| `CORDMUX_HANDSHAKE_DIR` | `_work/localappdata/cordmux` |

cordmux.dll は、環境変数 `APPDATA` を読まない。Vesktop の設定の場所を Vesktop と同じ決め方 (`VENCORD_USER_DATA_DIR` があればそこ、なければ RoamingAppData の既知のフォルダーの下の `Vesktop`) で決めて `state.json` を読み、`%LOCALAPPDATA%\cordmux\session.json` にトークンを書く。差し替えると、利用者の実際のファイルを読み書きしない。`VENCORD_USER_DATA_DIR` が `_work` の下を指していなければ、`run-window.mjs` は OBS を起動する前に止まる (`vesktopDirSwapProblem`)。`--dry-run` の `env` に、製品が読む `productStateJson` と `vesktopDirUnderWork` が出る。`seed-config.py` は `--appdata` の下にテスト用の Vesktop の `state.json` を作る。

`call-*` は、実際の Vesktop の送信側が実際の `%LOCALAPPDATA%\cordmux\session.json` を読むので、差し替えない。そのため `--real-profile` を必須にしてある。逆に `call-*` 以外のステップは `--real-profile` と `--call-window` を受け付けない。

## 実行の方法

```
node test/tools/obs-run/run-window.mjs --obs _work/obs-33.0.1 --out _work/out/<名前> \
  --plugin <cordmux をインストールしたフォルダー> --step <ステップ> [--tag <名前>] [--dry-run]
```

### ドライラン

`--dry-run` は、開始前の確認 (`tasklist` と `nvidia-smi` を読むだけ) を実行し、実行計画を JSON で標準出力に書いて終わる。OBS もテスト用の送信側も起動せず、`--out` にも何も書かない。JSON には次の項目がある。

- `args`: 解釈した引数 (既定の値を含む)
- `guard` と `blocked`: 開始前の確認の結果
- `steps`: 実行するステップの一覧。`stepCount` は数
- `plannedMinutes` と `deadlineEstimate`: 実行計画が見込む時間と、期限の見積もり。`deadlineShortfallMinutes` は見込みが期限に足りない分
- `humanSteps`: 人の操作を待つステップ
- `env`: 子プロセスに渡す環境変数
- `seedPreview`: `seed-config.py --dry-run` が書くはずの内容
- `filesManifest`: テスト用の OBS にコピーするファイル

実機を使う実行の前に、同じ引数に `--dry-run` を付けて確認する。

### 主な引数

| 引数 | 意味 |
|---|---|
| `--obs` `--out` `--plugin` | 上の前提のとおり。`--step install-roundtrip` は `--obs` と `--plugin` を受け付けない (展開先は `_work/rt-<tag>/obs`、cordmux は `--zip` から置く) |
| `--step` | 下のステップの一覧のどれか。必須 |
| `--tag` | 結果のファイル名に使う名前。`install-roundtrip` は必須 |
| `--dry-run` | 上の「ドライラン」 |
| `--n-screen` `--n-voice` `--seconds` | テスト用の送信側の画面の数 (既定 4)、声の数 (既定 8)、秒数 (既定 180) |
| `--warmup 0\|1` | エンコーダのウォームアップ (既定 1)。起動直後のフレームの欠落を避ける |
| `--hw prefer-hardware\|prefer-software` | テスト用の送信側のエンコーダの選び方 (既定 `prefer-hardware`) |
| `--deadline-min` | 実行全体の期限 (分。既定 120)。超えると残りの手順を打ち切り、終了コード 3 を返す。`call-1` の実際の実行で `--deadline-min` を付けないときは、見積もりと 180 のうち大きいほう |
| `--stream` | `through` と `integration` で配信も始める (配信を受ける ffmpeg を起動する) |
| `--dock-scenario` | `dock-view` の状態。下の一覧 |
| `--dock-install elsewhere\|missing\|ours\|absent` | 初期設定で、ドックが見る Vencord のインストール状態を作る (既定 `elsewhere`) |
| `--assign-mode manual\|auto` | cordmux の設定の枠への載せ方 (既定 `manual`) |
| `--decoder software\|hardware` | cordmux の設定のデコードの方式 (既定 `software`) |
| `--display-buffer-ms` | cordmux の設定の表示の緩衝 (0 から 1000)。渡さなければ製品の既定 |
| `--obs-language` `--obs-theme` `--obs-font-scale` `--obs-density` `--obs-hdr` `--obs-full-height-docks on\|off` | OBS の言語と見た目。`hold` と `dock-view` で使う (言語は `hold` だけ)。`--obs-full-height-docks` は [ドック] の [フルハイト ドック] (渡さなければ OBS の既定。OBS 33 は付けたまま) |
| `--call-window` `--real-profile` `--allow-state-json-write` | `call-*` のオプション。上の環境変数の差し替えを参照 |
| `--rehearsal` | `call-1` と `call-hold` で、実際の Vesktop の代わりにテスト用の送信側を使い、人の操作を秒数で自動で進め、待ちを縮める。`--real-profile` と `--call-window` とは一緒に渡せない |
| `--allow-unknown-encoder` | `nvidia-smi` を読めなくても進める |
| `--allow-outside-work` | `--obs` と `--out` の場所の制限を外す |

`call-1` のオプション (`--call-minutes` `--broadcaster-id` `--latency-person` `--voice-channel` `--start-with-add` `--vesktop-share` `--skip-continuous` `--skip-av2` `--skip-speaker-mute` `--extra-checks` `--skip-export-bundle` `--recheck` `--av-check` `--av-only` `--browser-profile` `--shared-content-dir` `--temp-vencord-build`) と、`hold` のオプション (`--hold-minutes` `--no-seed` `--keep-recordings` `--replay-buffer` `--self-mic` `--screen-sizes` `--screen-names` `--screen-images` `--voice-users` `--label-em-ratio-pct` `--wire-composite`) は、対応するステップ以外では受け付けない。誤った組み合わせは、OBS を起動する前に終了コード 1 で止まる。

`--call-accounts <JSON ファイル>`: 実際の通話の実行 (`--step call-1` で、リハーサルでもドライランでもないもの) に必須である。通話に参加するアカウントの、Discord に表示される名前を次の形で書く。リポジトリの外に置く。

```json
{ "main": "メインアカウント", "receiver": "Vesktop のアカウント", "chrome": "Chrome のアカウント", "firefox": "Firefox のアカウント" }
```

### 終了コード

| コード | 意味 |
|---|---|
| 0 | 最後まで実行して、合否の判定がすべて成功した |
| 1 | 引数か実行計画の誤り、または予期しない例外で最後まで進めなかった |
| 2 | 開始前の確認で止まり、始めなかった |
| 3 | 合否の判定が失敗した、期限で打ち切った、または止めるファイルで抜けた。結果 JSON の `gateFailures` に失敗した手順が残る。判定に届かないまま抜けた実行は 0 を返さない |

### 実行中の入力と出力

- 結果は `<--out>/window3b/result-<ステップ>-<tag>.json` に書く。録画、スクリーンショット、判定ツールの出力も `window3b` の下に置く。
- 人の操作を待つステップ (`human`) は、ログに `HUMAN: <操作の文>` と書き、条件を満たすまで 2 秒ごとに見る。条件は次の 5 つである。どの条件でも、`<--out>/window3b/human-go.txt` があれば削除して進む。
  - `go-file`: `human-go.txt` が置かれる
  - `state-json-points-to-placed`: 実際の `state.json` の `vencordDir` が、テスト用にインストールしたフォルダーを指す
  - `state-json-restored`: `state.json` が、製品のバックアップの `previous` に戻る
  - `call-ready`: OBS のログの最後の `Facts` の行で、通話の人数と画面共有の人数が、頼んだ人数に届く (`call-1` の参加の依頼。人の返事は要らないので、この手順では `human-go.txt` を置かない。置くと、全員がそろう前に進む)
  - `seconds`: 指定した秒数だけ待つ (`--rehearsal` は、すべての人の操作をこれにする)
- `<--out>/window3b/stop-run.txt` を置くと、期限に当たったときと同じ経路で残りの手順を抜ける。後片付けは実行する。
- 操作を待つ時限は 1 操作につき 30 分、通話に参加して共有を始めるまでは 40 分である。人が PC の前にすぐ戻れるとは限らないため。待つ長さは合否に使わない。

## ステップの一覧

### OBS の基本の確認

| ステップ | 内容 |
|---|---|
| `place` | テスト用にインストールしたファイルのコピー、起動、接続、モジュール一覧の確認、シーンとソースの存在、`session.json` の存在と `icacls`、2 つ目の OBS が `BlockedByOtherWriter` で止まることの確認 |
| `through` | テスト用の送信側の画面 1 本を 10 秒送り、録画を `frames.py` で読む。`--stream` で配信も始める |
| `integration` | `--n-screen` と `--n-voice` の数を `--seconds` 秒送り、録画を判定ツールで読み、ログから段ごとの計数を集める。`--integration-config` で `default`、`one-screen` (画面 1、60 秒)、`one-screen-one-voice` (画面 1 と声 1、180 秒) を選ぶ |
| `guard` | トークンなしと違うトークンでの接続が拒否されること、送りすぎたときの背圧 (接続まで進み、送信側か受信側で捨てた数が出ること)、エンコードの段階を上から 3 つ使えなくしたときに下の段階へ移ること、負荷を下げる 3 つの手段 (長辺の上限、fps を半分、`prefer-software`)、ログにトークンが出ないことを確認する |
| `second-obs` | 接続中にシーンコレクションを空のもの (`test-empty`) に切り替えて戻し、接続が切れず、枠のソースが残り、映像が再び乗ることを確認する |
| `discord-first` | OBS より先にテスト用の送信側を起動し、5 秒後に OBS を起動して、繋ぎ直しで映像が乗ることを確認する |
| `dock-view` | ドックを状態ごとにスクリーンショットで撮る。`--dock-scenario` の値は後述 |
| `mic-check` | OBS を起動しない。`test/tools/mic-check` で、Chrome のテスト用のマイク入力に WAV を読ませて音が届くことを確認する |
| `webcodecs-check` | OBS を起動しない。`test/tools/webcodecs-check` で、エンコーダを作った直後の黒の長さと、2 つ目のインスタンスの SPS と PPS のビット一致を測る |

`--dock-scenario` の値は `none` (既定。送信側なし)、`connected`、`no-scene`、`degraded`、`contact-missing`、`idle-10min`、`select-four`、`dummy`、`export-bundle` である。`--dock-max-long-edge` (既定 640) は `degraded` だけが見る。

### 録画と導入

| ステップ | 内容 |
|---|---|
| `record` | cordmux の録画を、OBS の録画と同時に実行して対で判定する。途中で ON、解像度の変更 2 回、繋ぎ直し 2 回、空き容量の警告、完成したファイルの確認、同じ OBS の起動の中での 2 回目の録画、OFF に戻っていることの確認までする |
| `install-roundtrip` | OBS を zip から毎回展開し直し、cordmux を zip から置いて、導入、更新、撤去の往復を撮って比べる。`--scenario` で 13 の場面を選ぶ (後述) |

`--scenario` の値は `roundtrip`、`move`、`move-foreign`、`force-update-missing`、`force-update-tamper`、`update`、`update-while-running`、`other-vencord`、`other-vencord-cancel`、`motw`、`dll-clash`、`old-obs`、`newer-libobs` である。`update` と `update-while-running` は `--old-zip`、`newer-libobs` は `--test-dll`、`old-obs` と他は `--obs-zip` と `--zip` が要る。`--obs-zip` には OBS 33 以上の zip を渡す (`old-obs` だけは、OBS 33 の置き方を読まない 32.2.2 の zip)。`--layout` の既定は `plugins` (手順書のポータブル版の OBS の置き方。OBS のフォルダーの `plugins\cordmux`)。`--layout programdata` は VM の中だけで、`--vm` と `--scenario roundtrip` が要る。

### 実際の通話

実際の Vesktop と Discord の通話を使う。`--rehearsal` を付けない実行は `--real-profile` が必須で、`--call-window` で Vesktop の稼働を許す。人の操作を待つ。

| ステップ | 内容 |
|---|---|
| `call-gate-a` | 初期設定 (テスト用の `state.json` は作らない) の後、人が Vesktop を終了してドックの [Vesktopに追加] を押し (Vesktop の起動中は押せない)、実際の `state.json` の `vencordDir` がテスト用にインストールしたフォルダーを指すことを確認する |
| `call-gate-b` | OBS を起動しない。`state.json` を読み直して、`vencordDir` 以外のキーが変わっていないことを確認する。`call-gate-a` と同じ `--out` を渡す |
| `call-gate-c` | OBS を起動し直して、送信側と接続できることをスクリーンショットで確認する |
| `call-gate-d` | `data/vencord/package.json` を退かしてから OBS を起動し、接続を確認してすぐ戻す |
| `call-1` | 通話の本体。4 つの画面と 4 つの声を受け、定常状態を `--call-minutes` 分 (既定 20) 録画し、合否を判定する。終わりにアンインストールして `vencordDir` を元に戻す |
| `call-hold` | `call-gate-a` と同じ初期設定で、実際の Vesktop と繋いだまま、`human-go.txt` が置かれるまで OBS を開いておく。締めは `call-1` と同じ |

`call-1` の主なオプションは次のとおり。

- `--broadcaster-id <ID>`: メインアカウントの Discord ユーザー ID (数字 1 から 20 桁)。サブアカウントを使う構成の手順が加わる。結果 JSON には書かない。
- `--start-with-add`: 開始ゲートを実行せず、Vesktop の終了と [Vesktopに追加] から始める (追加の後に Vesktop を起動してもらう)。
- `--vesktop-share`: 参加の文で、Vesktop の共有も依頼する (メインアカウントだけの構成の使い方)。
- `--latency-person <名前>`: cordmux が足す遅れを測る区間を足す。名前は、遅れを読む画面共有している人として見えている名前。その人の枠は、OBS のログの枠の割り当て (`Screen slot N carries stream M`) と、遅れの録画の区間を覆う cordmux の録画の録画ログ (人とストリームの番号の対応) から引く。覆う録画がない実行 (`--recheck` でない `call-1`) では引けないことがあり、そのときは判定せず、枠の PNG で選んで `latency.py` を手で実行する。
- `--voice-channel <名前>`: 参加の文で入ってもらうボイスチャンネルの名前。渡さなければ、文は名前を挙げない。
- `--skip-continuous`: 連続運転と、2 回目のテスト動画の A/V の待ちを省く。`--skip-av2`: 2 回目のテスト動画の A/V と、1 回目との差を省く。`--skip-speaker-mute`: サブアカウントを使う構成の、スピーカーミュートと解除を省く (`--broadcaster-id` が要る)。`--skip-export-bundle`: 締めの診断情報の書き出しを省く。
- `--extra-checks`: 人の操作が要らない追加の確認。合否は記録するが、実行は止めない。確認するのは、接続が切れていないこと、バージョンの行の表示、使う声の置き換え、声の音量、テスト動画を共有している人の画面の A/V、ログから読む整列のずれの傾き、実際のアバターの並び、映像のバッファの統計である。
- `--recheck`: 送信側の修正を確認する短い実行。`--start-with-add` と `--broadcaster-id` が要る。参加の依頼は、4 つのアカウント (Vesktop を含む) に通話へ入ることと、4 つともマイクをミュートにすることを頼み、Chrome にはテスト動画のタブを音声付きで共有してもらう。声の音を使う確認がないので、Chrome をテスト用のマイクの音つき (`launch-chrome-fake-mic.ps1`) で起動する必要はない (テスト動画のページを開いた Chrome を用意する)。
- `--av-check`: `--recheck` に、テスト動画の A/V の測定を足す。cordmux の録画を遅れの測定の前に始め、テスト動画の共有を 5 分録って `sync.py` で読み (`av1`)、cordmux の録画を止めて、録画の中の A/V も読む (`judge-record-sync`)。テスト動画の共有は参加の依頼で頼んでいるので、人の操作は増えない。合成のソースの判定は、テスト動画を共有している人の枠だけで行う (枠は `--latency-person` と同じ方法で引く)。録画の間は、ドックの行で声と、テスト動画の人のほかの画面共有の音をミュートにする (合成のソースの音は全員の音を混ぜた 1 本なので)。遅れと同じ回に測り、映像のバッファ、描く刻みの半分、遅れ、音の遅れの関係を読む材料にする。`--recheck` が要る。
- `--av-only`: `--recheck --av-check` の手順表を、映像と音のずれ (と `--latency-person` の遅れ) を測るところと締めだけにする。人の操作は、Vesktop の終了と [Vesktopに追加]、Vesktop の起動、通話への参加と共有 (返事は要らない)、遅れの表示 (`--latency-person` のとき)、通話から抜けて Vesktop を終了、Vesktop の起動の案内である。アンインストールは実行スクリプトが押す。`--recheck --av-check` が要る。

映像と音のずれの傾きは、`sync.py` が一時的な段 (一定の高さに上がって戻る組の並び) を外した基準の組で読む。時間とともに広がる (1 ms/分を超えて 0 と分けられる) ときだけ欠陥とし、段は数と高さを記録する。規則と値の理由は `sync.py` の「一時的な段と、時間とともに広がる傾きを分ける」にある。

`--rehearsal` は、テスト用の送信側では確かめられない項目 (自分のアカウントを届けるか、Vesktop の版の表示、遅れ、エンコードの設定での確保し直し、メインアカウントの共有による自分の枠、配信者の抜け戻り、スピーカーミュートと Vesktop の抜け戻り) を判定しない。判定の手順の代わりに `skip` の手順に理由を残し、`judge-lines` の表では判定なしにする (`run-window-plan.mjs` の `REHEARSAL_NOT_JUDGED`)。整列のずれの傾きは、傾きを読む長さ続くストリームがなければ判定しない。別アカウント方式と自分の枠は、テスト用の送信側の声だけの人をメインアカウントにして、ドックの付け外しを判定する。

実行の前に、`--dry-run` で `humanSteps` を見て、人の操作の一覧と順番を確認する。

### 実機で手で操作するためのステップ

| ステップ | 内容 |
|---|---|
| `hold` | 初期設定、起動、接続の後、`human-go.txt` が置かれるまで OBS を開いたまま待ち、閉じて後片付けする。待つ間に、人が PowerShell のツールを手で打つ。`--hold-minutes` (既定 30、1 から 110。`--deadline-min` から後片付けの 10 分を引いた長さまで) が上限 |

`hold` は、待ち始めるときに `<--out>/window3b/hold-<tag>.json` に、OBS の pid、obs-websocket の接続先、`human-go.txt` の場所、後片付けまでに必要な情報を書く。PowerShell のツールの `-Pid` は、ここから読む。Volta の shim から起動すると `node` が子プロセスを作るので、実行スクリプトを止めるときは、`hold-<tag>.json` の `driverPid` を木ごと終了する。

`hold` のオプションは次のとおり。

| オプション | 意味 |
|---|---|
| `--hold-minutes N` | 待つ長さの上限 |
| `--no-seed` | 初期設定を書き直さず、前の実行の設定とシーンコレクションを使う |
| `--n-screen` `--n-voice` `--screen-sizes 1280x720,3840x2160` | 渡したときだけテスト用の送信側を繋ぐ。`--screen-sizes` は画面ごとの最初の寸法 |
| `--keep-recordings` | 後片付けで、`recordings` という名前のフォルダーの下 (cordmux の録画の区間と録画ログ) を残す |
| `--replay-buffer` | テスト用の OBS のプロファイルにリプレイバッファ (10 秒、256 MB) を書く。自動では始まらない |
| `--stream-sink` | 配信を受ける ffmpeg を開いたまま持つ。`call-hold` も受け付ける。配信が止まるたびに次のファイル (`stream-<tag>-<n>.flv`) で開き直す |
| `--self-mic` | 自分の枠の音声ソース (音声入力キャプチャ) を作って始め、音の大きさ (peak の dBFS) の分布を OBS のログから読む。録画も配信もしない。しきい値を実際のマイクで決めるための短い実行 |
| `--screen-names Mika,Rin` | テスト用の送信側の画面ごとの表示名 (`--n-screen` と一緒に。数は `--n-screen` まで)。空の項目、制御文字、64 文字を超える名前は、OBS を起動する前に止まる |
| `--screen-images <画像>,...` | 画面ごとに、動く図形の代わりに描く画像 (PNG か JPEG。`--n-screen` と一緒に)。相対の場所はリポジトリのルートから読む。ない画像があれば、`--dry-run` でも OBS を起動する前に止まる |
| `--voice-users harness-screen-1` | 声のストリームごとの持ち主の userId (`--n-voice` と一緒に)。画面の人 (`harness-screen-<n>`) を書くと、その人の声になる。渡すと、声に音を乗せる通知ファイル (`voice-tone-<tag>.flag`) の場所を `hold-<tag>.json` の `voiceToneFile` に書く。置くとその声が鳴り (中身が空なら全部の声、`5` ならその番号の声)、持ち主の枠に話している人の光が出る |
| `--label-em-ratio-pct 4.0` | cordmux の `settings.json` の名前の大きさ (`label_em_ratio_pct`。1.0 から 8.5、0.1 刻み)。`--no-seed` とは両立しない |
| `--wire-composite` | 接続の後に、合成のソースを `Main` にキャンバス全体で置く。置かないと OBS のプレビューに何も映らない |

### 実際の OBS の画面を撮る

リポジトリの README の画像のように、テスト用の OBS の画面を、4 人の画面共有を 4 分割で並べて名前ラベルを付け、cordmux のドックを OBS の中にドッキングした状態で撮る。`hold` で OBS を 1 回だけ起動し、待っている間に `readme-shot.ps1` で撮る。撮り直しも同じ OBS のまま行える。

前提は上の「実行の前提」と同じで、ほかに次が要る。

- テスト用の送信側のバンドル (test/tools/harness/README.md の「ビルド」)。
- Python と Pillow (枠の中の画像を描く `src-obs/tests/decor_preview/readme_tiles.py`) と、Kenney の素材 (置き方は src-obs/tests/decor_preview/README.md の「README の画像」)。
- 利用者の OBS と Vesktop を閉じておく (上の「開始前の確認」で止まるため)。

Git Bash で、リポジトリのルートから次の順に実行する。`<cordmux をインストールしたフォルダー>` は、上の `--plugin` と同じ。

1. 枠の中の画像を描く (OBS は使わない)。明るさを下げた 4 枚ができる。

   ```
   python src-obs/tests/decor_preview/readme_tiles.py --out _work/tmp/readme-tiles --style kenney --assets _work/assets/kenney
   ```

2. `hold` を起動する。最初に同じ引数に `--dry-run` を付けて、引数の誤りと画像の有無を確かめる。

   ```
   T=_work/tmp/readme-tiles
   node test/tools/obs-run/run-window.mjs --obs _work/obs-33.0.1 --out _work/out/readme-shot \
     --plugin <cordmux をインストールしたフォルダー> --step hold --tag readme --hold-minutes 8 \
     --assign-mode auto --label-em-ratio-pct 4.0 --wire-composite \
     --n-screen 4 --n-voice 2 --screen-sizes 1280x720,1280x720,1280x720,1280x720 \
     --screen-names "Aoi,Mika,Rin,ゆず" \
     --screen-images "$T/tile_tl.png,$T/tile_tr.png,$T/tile_bl.png,$T/tile_br.png" \
     --voice-users harness-screen-2,harness-screen-3 --obs-full-height-docks off
   ```

   `--obs-full-height-docks off` は、OBS の [ドック] の [フルハイト ドック] を外す。OBS 33 の既定 (付けたまま) では、右にドッキングしたドックが主ウィンドウの上端から下端までの列になり、コントロールのドックはその左に並ぶ。外すと、下のドックの列が主ウィンドウの幅いっぱいになり、ドックはプレビューの右で、コントロールのドックの上に入る (左のシーンとソースのドックも、下のドックの列の上に収まる)。OBS 33 の既定の並びで撮るときは、このオプションを除く。

3. 別の端末で撮る。`readme-shot.ps1` は、動いている OBS の `hold-readme.json` が書かれ、4 人の名前ラベルが描かれるまで待ってから、浮いているドックのタイトルバーをダブルクリックしたときと同じメッセージを送って OBS にドッキングし、主ウィンドウの大きさとドックの幅をそろえて撮る。2 の直後に実行してよい。

   ```
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/readme-shot.ps1 \
     -HoldInfo _work/out/readme-shot/window3b/hold-readme.json -VoiceOn
   ```

4. 撮った画像を確かめたら、`hold` を終わらせる。OBS を閉じて後片付けする。

   ```
   touch _work/out/readme-shot/window3b/human-go.txt
   ```

枠の位置は、表示名の順 (自然順。英字は仮名より前) に、左上、右上、左下、右下である (`--assign-mode auto` で全員が枠に載る)。上の例では、左上が Aoi、右上が Mika、左下が Rin、右下がゆずになる。`--screen-names` と `--screen-images` を同じ並びにして、画像の名前 (`tile_tl` は左上) と枠の位置をそろえる。`--voice-users harness-screen-2,harness-screen-3` は 2 人目と 3 人目の画面の人 (Mika と Rin) の声で、`-VoiceOn` で鳴らすと 2 人の枠に話している人の光が出る。`--n-voice` は `--voice-users` の数と同じにする。テスト用のプロファイルは音の監視を切ってあるので、音は鳴らない。

`readme-shot.ps1` の主な引数は次のとおり。

| 引数 | 既定 | 意味 |
|---|---|---|
| `-HoldInfo` | なし | `hold-<tag>.json`。書かれるまで `-WaitSeconds` 待つ。`-Pid` を渡せば使わない |
| `-Width` `-Height` | 1600 x 900 | 主ウィンドウの見える縁の大きさ (論理の画素。画面の拡大率を掛けて物理の画素にする)。その画面の作業領域の左上に置く |
| `-DockWidth` | 420 | ドッキングしたドックの幅 (論理の画素)。ドックの縁の仕切りを引っ張って合わせ、合わなければ止まる (Qt がドックかプレビューの最小の幅で止めたときなど)。0 は幅を変えない |
| `-OutWidth` | 1600 | README に置く画像の幅の上限 (物理の画素)。超えるときだけ縮める |
| `-VoiceOn` | なし | `hold-<tag>.json` の `voiceToneFile` を置き、声を鳴らしてから撮る |
| `-ExpectLabels` | 4 | 名前ラベルを描き終えたタイル (OBS のログの `Composite label ready`) がこの数になるまで待つ。0 は待たない |
| `-WaitSeconds` | 180 | `hold-<tag>.json` と名前ラベルを待つ上限 (秒)。名前ラベルを待ちきれないときは、そのまま撮って `labelsReady` を偽にする |
| `-SettleMs` | 3000 | 置いてから撮るまでの待ち (ミリ秒) |
| `-Method` | `printwindow` | `printwindow` はほかのウィンドウの裏や画面の外でも撮れる。プレビューが黒く写ったときは `screen` で撮り直す。`screen` は主ウィンドウを最前面に出して画面から切り出すので、主ウィンドウが画面に収まっている必要がある |

出力は `-OutDir` (既定は `hold-<tag>.json` のフォルダー) の `readme-shot.png` (README に置く。幅を `-OutWidth` 以下に縮めたもの)、`readme-obs.png` (主ウィンドウの等倍) と、標準出力の 1 行の JSON である。JSON の `labelsReady` と `labelTiles` は名前ラベルを待った結果、`docking` はドッキングの記録 (`wasFloating` は始めに浮いていたか、`attempts` はダブルクリックの回数)、`dock` はドックの四角 (`rect`。物理の画素) と幅 (`widthLogical`。論理の画素) と仕切りを引っ張った記録 (`drags`)、`docks` は主ウィンドウの中のドックの名前と四角 (コントロールのドックとの位置を数で確かめる)、`main` の `frame` は置いた後の見える縁の四角 (物理の画素。`sizeOk` が偽なら、Qt か Windows が最小の大きさか画面の大きさで止めた)、`shot` は README に置く画像の大きさと縮めた比、`beyondWorkArea` は作業領域からはみ出したか (`screen` で撮ると、はみ出した所は写らない) である。止まったときは `ok` が偽で、`error` に理由がある。

PC の画面には次のものが出る。`hold` が起動したテスト用の OBS とドックのウィンドウは前面に出て、フォーカスを奪う。`readme-shot.ps1` はドックを OBS の中にドッキングし、主ウィンドウを動かして大きさを変え、ドックの幅を変えるが、フォーカスは奪わず、マウスのカーソルも動かさない (`-Method screen` は主ウィンドウを最前面に出す)。ダブルクリックと仕切りの引っ張りはウィンドウにメッセージを置いて行うので、`readme-shot.ps1` が終わるまで OBS の上でマウスを動かさない (Qt がダブルクリックや引っ張りと取り違えうる)。名前の大きさ、人数、フルハイト ドックを変えるときは、`human-go.txt` で閉じてから引数を変えて起動し直す (どれも初期設定で書くため)。

## 初期設定 (`seed-config.py`)

実行スクリプトが、OBS の起動前に `seed-config.py` を実行して、次を書く。`--step` と、`--appdata` か `--real-profile` のどちらか 1 つが要る。

- プロファイルとシーンコレクション。シーンは `test` と、空の `test-empty`。`--shape user` は利用者に近い設定にする。
- obs-websocket の待受。
- cordmux の `settings.json`。書くキーは `assign_mode`、`allow_state_json_write`、`decoder` と、必要なときだけ録画や表示のキーである。`allow_state_json_write` は `--appdata` の実行では true、`--real-profile` の実行では `call-*` のときだけ true を書く。
- `--appdata` を渡したときだけ、テスト用の Vesktop の `state.json` と、`--dock-install` に応じたテスト用にインストールした `data/vencord` と `data/vencord-src`。
- `--language` `--theme` `--font-scale` `--density` `--hdr` は、その OBS の `data/obs-studio/locale` と `themes` にある値だけを受け付ける。OBS は知らない値を黙って既定に戻すため。

`--dry-run` は何も書かず、書く内容を出す。`--selftest` は一時フォルダーで確認する。

## 後片付け (`cleanup.ps1`)

実行スクリプトは、終わるときに自動で `cleanup.ps1` を実行する。途中で止まった実行は、手で実行する。

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/cleanup.ps1 [-WorkDir <フォルダー>] [-ObsDir <フォルダー>] [-StateJsonPath <ファイル>] [-BackupPath <ファイル>] [-ArtifactsPath <ファイル>] [-RealProfile] [-CallWindow] [-KeepRecordings] [-WatchProgramData] [-DryRun] [-Json]
```

実行スクリプトが動いている間は実行しない。`-WorkDir` の下のテスト用の OBS を閉じて 5 秒後に強制終了し、録画を削除するため。

| 引数 | 既定 | 意味 |
|---|---|---|
| `-WorkDir` | リポジトリの `_work` | この下にある `obs64.exe` `electron.exe` `ffmpeg.exe` だけを終了し、録画と配信の受信のファイル (`.mkv .mov .mp4 .h264 .wav .flv .jsonl .pcm`) を削除する。JSON、ログ、PNG は残す。`-WorkDir\appdata` はフォルダーごと削除する |
| `-StateJsonPath` | `%VENCORD_USER_DATA_DIR%\state.json` (未設定なら `%APPDATA%\Vesktop\state.json`) | `-CallWindow` で `vencordDir` を戻す対象 |
| `-ObsDir` | `-WorkDir\obs-33.0.1` | バックアップの場所の計算に使う |
| `-BackupPath` | `-ObsDir\config\obs-studio\plugin_config\cordmux\vencord_dir_backup.json` | 製品が書くバックアップ |
| `-ArtifactsPath` | `-WorkDir\call-window-artifacts.json` | 実行スクリプトが用意した一時の Vencord のビルド、ブラウザのプロファイル、共有の中身の一時フォルダーの一覧 |
| `-RealProfile` | なし | `--real-profile` で実行した後の後片付け。`%APPDATA%\cordmux-harness` と `%LOCALAPPDATA%\electron` には手を出さない。差し替えていないので、この実行が作ったものではないため |
| `-CallWindow` | なし | `vencordDir` をバックアップの `previous` に戻し、`-ArtifactsPath` の一時ファイルを削除する |
| `-KeepRecordings` | なし | 名前が `recordings` のフォルダーの下だけ残す |
| `-WatchProgramData` | なし | `%ProgramData%\obs-studio\plugins` に `cordmux` 以外の `cordmux*` がないか読む。VM の往復だけで使う。読むだけで消さない |
| `-DryRun` | なし | 何もせず、することだけを表示する |
| `-Json` | なし | `CORDMUX_CLEANUP_JSON:` で始まる 1 行に、結果を JSON で出す |

終了コードは、0 が「すべて後片付けできた」、1 が「残っているものがある (一覧つき)」、2 が「引数かバックアップの形式の誤り」である。

`call-*` の実行を途中で止めたときは、OBS が落ちていても戻せる次のコマンドで、`vencordDir` を元に戻す。

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/cleanup.ps1 -CallWindow -RealProfile -Json
```

バックアップ (`vencord_dir_backup.json`) は cordmux が書く。キーは `previous` (書く前の値。`null` は書く前にキーがなかったという意味で、戻すときはキーごと削除する)、`written`、`written_at`、`dll_dir` である。`cleanup.ps1` が使うのは `previous` と `written` の 2 つで、残りは報告に載せるだけである。

`cleanup.ps1` が触れるのは、`-WorkDir` の下のテスト用のプロセスとファイル、`%LOCALAPPDATA%\cordmux\session.json`、上の表の対象だけである。利用者の OBS、Vesktop、環境変数 `CORDMUX_PROTECTED_PROCESSES` のファイルで指定したプロセス、`-WorkDir` の外の ffmpeg、cmake、MSBuild、node、npm と利用者のアプリのキャッシュには触れない。親のいなくなった配信の受信プロセスは、`stream-sink-stale.ps1` で見つけて問題として伝えるだけで、止めない。

セルフテストは、架空の `state.json`、バックアップ、`-WorkDir` を作って確認する。実際のファイルには触れない。

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/cleanup.selftest.ps1 [-WorkRoot <フォルダー>]
```

## 判定ツールと判定の基準

判定ツールは、録画か画像を読んで結果を JSON で標準出力に書く。失敗しても例外にせず、`ok: false` か `error` を JSON に書く。実行スクリプトが `execFileSync` で呼ぶので、`frames.py` `smooth.py` `latency.py` は目標を超えても、読めなくても、終了コード 0 を返す。0 以外では、何が読めなかったかの JSON が結果に残らないためである。引数の誤りは終了コード 2 になる。

### 数を出すツール

| ツール | 測るもの |
|---|---|
| `frames.py` | 欠落 (番号が 2 以上進んだ)、重複 (前と同じ番号)、逆行、凍り (同じ番号が続いた最長の長さ `max_freeze_ms`)。時刻はフレームの pts で、ffmpeg にフレームを複製させない |
| `smooth.py` | 枠ごとに、10 秒ごとの欠落、重複、凍りの数。開始 15 秒より後を定常とみなす |
| `sync.py` | 音のマーカー (音の時刻 - 映像の時刻。負は音が先)。`paired` が組の数 |
| `tone_scan.py` | マーカーの音のまとまりの数と位置、全体の音量。判定はしない |
| `plate_measure.py` `border_timing.py` | 板の位置と、枠線のタイミング。判定はしない |

`frames.py` と `smooth.py` は、帯が区画の中に見つからなければ、数えずに `barcode_found: false` と `error` を出す。暗い所を読んだ 0 を番号として数えると、全部のフレームが凍りに見えるためである。clock の帯は、形と CRC-8 の両方が合ったフレームだけを番号として数える。

`sync.py` のマーカーの見つけ方は次のとおり。

- 白のマーカー: 区画を間引いた点ごとに、直前の 4 フレームの最小値を基準にして、160 以上になり、かつ 64 以上明るくなった点が全体の 20% 以上のフレームを白とする。区画の平均では、動画が枠の一部にしか写らない共有で白を見つけられないため。
- 音のマーカー: 5 ms の区間ごとに 2 kHz の強さを見て、近くの中央値の 4 倍を超え、かつ 1 kHz も跳ねている区間を候補にする。60 ms (12 区間) のものだけをマーカーとする。1 kHz が鳴り続けても、手を叩く音のような全帯域の音でも、誤って拾わないようにするため。

### 合否を出すツール

| ツール | 基準 |
|---|---|
| `ledger_check.py` | すべて整数で等号で比べる。ffprobe の `duration_ts` と録画ログの `duration_ticks`、`-count_frames` の `nb_read_frames` と `sample_count`、寸法、fps、音の有無。長さを秒の小数にすると丸めで揺れるため。`arrival_lag` (映像と音の到着の遅れの差) は出すだけで、合否には入れない。完成したファイルの A/V には適用されず、通信の具合で合否が変わってしまうため |
| `black_span.py` | `blackframe` (amount 98、threshold 32) のフレーム番号で、連続 2 フレーム以上を黒とする。1 フレームの黒は場面の切り替わりでも出る。無音は f32 にデコードして、完全な 0 が 0.1 秒以上続くもの。`--mode no-black` は黒が 1 つもなければ成功、`no-leading-black` は先頭が黒でなければ成功 (`--allow-leading N` で録画ログの先頭の穴の長さまで許す)、`match-gaps` は録画ログの穴と黒の区間が 1 対 1 で、前後 1 フレーム以内なら成功。1 フレームの許容は、timescale 60000 を 60 fps に丸める誤差のため |
| `au_hash.py` | 区間の fMP4 の映像の AU の (大きさ, `md5`) の並びが、完成した mov の中に、連続して、区間の順に現れれば成功。pts と dts は比べない (多重化で付け直され、黒の埋めが間に挟まるため)。ProRes の枝は AU を作り直すので比べない |
| `dummy_tile.py` | PNG の RGB の平均が黒 (0) から 8 を超えて離れていれば、色タイルが出ているとする。`--threshold` で変える。PNG への変換の丸めは 1 か 2 程度なので、8 でノイズと見分けられる |
| `check_bundle_zip.py` | zip が開け、`manifest.json` `plugin.log` `plugin-stats.log` `discord.log` `discord-stats.log` `settings.json` の 6 つがあり、トークン (64 桁の 16 進数) がなければ成功。`--session-json` を渡すと、その `secret` そのものを探す |
| `latency.py` | 合成のソースの中の共有 (領域 A) と、Discord のウィンドウのキャプチャ (領域 B) に写った帯の番号の差 (`B - A`) の平均を、cordmux が足す遅れ `cordmux_delay_ms` とする。目標は 90 ms (許容限) と 45 ms (検知限) で、超えても実行は止めず記録する。平均を使うのは、差が共有のフレーム間隔の倍数に寄るので、中央値だと刻みの値に張り付くため。差の絶対値が 2000 ms を超える組は無効として数える。組が 60 未満なら判定しない |

`analyze.py` は 1 kHz のマーカーを調べる。音量が -60 dBFS 未満なら無音、1 kHz の割合が 0.5 を超えればマーカーありとする。

### 実行計画の判定の基準

`run-window-plan.mjs` が、判定ツールの出力と OBS のログを読んで合否を決める。主な基準は次のとおり。

| 項目 | 基準と理由 |
|---|---|
| OBS の出力の欠け | `GetStats` を録画開始の 15 秒後と録画終了の直前に取り、エンコードの欠けが出力フレーム数に占める差分の割合が 0.02% を超えたら失敗。標本がそろわない、区間に 1 フレームも出ていない、計数が戻っているときは成功にしない |
| 音と映像のずれ | `sync.py` の平均が、音が先なら 45 ms 未満、遅れなら 125 ms 未満 (ITU-R BT.1359-1 の検知限) |
| ずれの傾き | 1 ms/分を超え、95% の信頼区間が 0 を含まなければ欠陥。組が 10 未満か、並びが 120 秒に届かない録画は判定しない |
| 2 つの録画のずれの差 | 区画ごとの平均の差の絶対値が 20 ms 未満 |
| 整列のずれの傾き | 0.05% 以下 (時間に対する割合)。ログの `stats` の行が 600 秒以上続いたストリームだけ読む。短いと揺れで傾きが立つため |
| 声のミュートの往復 | ミュートを外した区間が鳴り、ミュートの区間が無音で、外し直した区間が基準との差が 1.5 dB 以内に戻る。区間の両端は 0.5 秒削る。鳴っている声が 1 人だけであることが前提 |
| 声の音量 (200%) | 音量の比が 200% の利得 6.02 dB (増える方向) との差が 1.5 dB 以内。基準の音が大きく、200% で切れるおそれがあるときは決めない |
| 音の鳴っているかどうか | 1 kHz のテスト音の二乗平均の根が 0.02 (-34 dBFS) 以上で、1 kHz のまとまりが半分以上なら鳴っている。0.002 (-54 dBFS) 未満で、まとまりが 10% 以下なら鳴っていない。間は決めない |
| 入力の音量 | 10 秒間の peak が -60 dBFS を超える |
| 共有の途中の解像度の変更 | 全部の枠で `crc_failures` が 0、`unreadable_frames` が 0、`max_freeze_ms` が 1000 以下 |
| 自分の枠の話している判定 | 10 秒ごとの `self_talking_pct` が 50% 以上なら話している |
| 画面キャプチャへの切り替え | ウィンドウキャプチャが Vesktop のウィンドウを捉えられないとき、画面キャプチャに切り替えるのは、ウィンドウが最大化されて画面全体を覆うときだけ。DWM の縁のはみ出しは各辺 16 ピクセルまで許す。覆っていないと、ほかのウィンドウもコピーしてしまうため |
| ダミー映像と診断情報の zip | 上の `dummy_tile.py` と `check_bundle_zip.py` |
| マイク入力 (`mic-check`) | RMS が 0.05 を超え、1 kHz の成分が全体の 0.5 を超える |

## セルフテスト

次のコマンドで、判定ツールとツールのセルフテストを実行する。録画や実機は要らない。

- `python -X utf8 test/tools/obs-run/selfcheck.py [--outdir <フォルダー>] [--keep]`: テストデータを作り、`frames.py` `sync.py` `smooth.py` `analyze.py` が期待した成功と失敗になることを表にする。全部一致すれば終了コード 0。`--keep` でテストデータを残す。
- `--selftest` を持つ Python のツール (`bit_strip.py` `ledger_check.py` `black_span.py` `au_hash.py` `tone_scan.py` `install_snapshot.py` `newer_ver_dll.py` `plate_measure.py` `border_timing.py` `latency.py` `summarize-window.py` `seed-config.py` `tool_text_ids.py`) は、ツールごとに `--selftest` を付けて実行する。
- `-SelfTest` を持つ PowerShell のツール (`a11y-record.ps1` `harness-suspend.ps1` `send-keys.ps1` `window-size.ps1` `stream-sink-stale.ps1` `explorer-windows.ps1` `readme-shot.ps1`) は `-SelfTest` を付けて実行する。ウィンドウは開かない。
- `cleanup.selftest.ps1`: 上の後片付けの節のとおり。
- `npx --no-install vitest run test/tools`: `.test.ts` の単体テスト。

## ファイルの一覧

### 実行スクリプトと関連ファイル

| ファイル | 役割 |
|---|---|
| `run-window.mjs` | 実機テストの実行スクリプト。実行計画を、ファイル操作・子プロセス・obs-websocket で実行する |
| `run-window-plan.mjs` | 実行スクリプトの純粋関数。引数の解釈、ステップごとの実行計画、開始前の確認の判定、合否の判定を持つ。Node の API を呼ばない |
| `run-window-*.test.ts` | `run-window-plan.mjs` と `run-window.mjs` の vitest。ステップやオプションごとに分けてある |
| `obsws.mjs` / `obsws.test.ts` | obs-websocket v5 の最小のクライアント (認証なし、127.0.0.1 だけ)。呼び出しに時限がある |
| `stream-sink.mjs` / `stream-sink.test.ts` | 配信を受ける ffmpeg (`-listen`) の起動、停止、開き直し |
| `tool_text_ids.py` | このフォルダーのツールが名指す製品の画面の文字列のキー (`cordmux.<...>`) が、`ja-JP.ini` と `en-US.ini` の両方にあり、形が正しいことを確認する |
| `id-scan.mjs` / `id-scan.test.ts` | コピーしたファイルに Discord のユーザー ID の形 (17 桁から 20 桁の数) が残っていないかを数える。数そのものは出さない |
| `seed-config.py` | テスト用の OBS の設定一式の書き込み (初期設定)。プロファイル、シーン、obs-websocket の待受、cordmux の `settings.json` |
| `install_snapshot.py` | 導入の往復の前後のファイルの状態を撮り、比べる。zip の展開もここ |
| `newer_ver_dll.py` | 製品の `cordmux.dll` のコピーを作り、`obs_module_ver()` が返すバージョンだけを新しくする |
| `cleanup.ps1` / `cleanup.selftest.ps1` | 実行の後片付けと、そのセルフテスト |
| `ci-selftests.test.ts` | このフォルダーの `--selftest` と `-SelfTest` が CI で実行されることを確認する |

### 判定ツール (Python)

| ファイル | 役割 |
|---|---|
| `frames.py` | 録画の映像からフレーム番号の帯を読み、欠落・重複・逆行・凍りを数える |
| `sync.py` | 映像の白のマーカーと音の 2 kHz のマーカーを組にして、音と映像のずれを測る |
| `smooth.py` | 枠ごとのフレーム間隔の粗さ (欠落・重複・凍り) を 10 秒ごとに数える |
| `analyze.py` | 録画のストリーム、長さ、フレーム数、音量、期待する周波数の割合を出す |
| `latency.py` | 1 本の録画の 2 つの区画に写った帯の番号の差から、cordmux が足す遅れを測る |
| `bit_strip.py` | 時計つきの帯の描き方、探し方、読み方。`frames.py` `smooth.py` `latency.py` と、テストデータと動画の生成が使う |
| `video_frames.py` | 録画からフレーム (pts つき、区画の切り出しつき) と音を取り出す共通のコード |
| `ledger_check.py` | cordmux の録画ログ (`ledger.jsonl`) と実際のファイルを ffprobe で突き合わせる |
| `black_span.py` | 黒の連続区間と無音の連続区間を拾う |
| `au_hash.py` | 送った AU と完成した mov の AU が同じかを区間ごとの `framemd5` で見る |
| `tone_scan.py` | 完成した音からマーカーの音 (既定は 2 kHz) の位置と回数、全体の音量を数える |
| `dummy_tile.py` | スクリーンショットの平均色が黒でないか (ダミー映像の色タイルが出ているか) を見る |
| `check_bundle_zip.py` | 診断情報の zip の中身 (6 ファイル) と、トークンが混ざっていないことを見る |
| `plate_measure.py` | 名前ラベルの板の 4 辺とピクセルの重ね方を、PNG と録画から測る |
| `border_timing.py` | 話している人の光が点く、保つ、消えるタイミングを録画から測る |
| `summarize-window.py` | 実行の結果 JSON と OBS のログから、判定ツールの出力を 1 画面の要約にする |
| `selfcheck.py` | `frames.py` `sync.py` `smooth.py` `analyze.py` のセルフテスト。詳細は `README-selfcheck.md` |
| `make_material.py` | セルフテスト用に、わざと壊した mp4 を作る (ffmpeg と numpy) |
| `make_sync_video.py` | 実際の通話で共有する、音つきのテスト動画 (`sync-video.mp4`) を作る |
| `pattern-strip.test.ts` | `share/pattern.html` の帯の描き方が `bit_strip.py` と同じ値になることを確認する |
| `sync-video-page.test.ts` | `share/sync-video.html` の動作 (再生すると動画を表示域いっぱいに広げる) を確認する |
| `webcodecs-nal.test.ts` | `test/tools/webcodecs-check/nal_pure.js` (Annex B から SPS と PPS を取り出す関数) のテスト |

### 画面と入力を操作するツール (PowerShell)

どれも UI Automation か Win32 の API で動き、実行計画のステップが子プロセスとして呼ぶ。実行中に手で呼ぶこともある。

| ファイル | 役割 |
|---|---|
| `screenshot.ps1` | OBS の見えているトップレベルウィンドウを PNG に撮る |
| `window-rect.ps1` | 実行ファイルの主ウィンドウの四角、最大化か、最小化か、ウィンドウがある画面の四角を物理ピクセルで読む |
| `window-size.ps1` | 指定したウィンドウの大きさを変え、Windows と Qt が止めた大きさを読んで、元に戻す |
| `dock-click.ps1` | 浮いたドックのボタンとチェックボックスを押す。`-Desired on\|off` は状態を読んで、違うときだけ押す。`-NamePrefix` は名前を前方一致で探す (録画中の [録画を終了] は後ろに経過時間が付く) |
| `dock-click-at.ps1` | ドックの指定した座標を押す |
| `dock-width.ps1` | OBS の主ウィンドウと浮いたドックを縮めて、最小の幅と四角を読み、戻す |
| `readme-shot.ps1` | `hold` で開いている OBS に cordmux のドックをドッキングし、主ウィンドウを決まった大きさで撮って PNG にする (上の「実際の OBS の画面を撮る」) |
| `dialog-click.ps1` | タイトルを指定したトップレベルウィンドウ (確認のダイアログなど) のボタンを押す。`-ListOnly` は押さずにボタンと文の一覧を読む |
| `dialog-sendkeys.ps1` | タイトルを指定したウィンドウ (保存ダイアログなど) に、文字と Enter を送る |
| `send-keys.ps1` | 指定したウィンドウにキーを 1 項目 1 キーで送り、送るたびにフォーカスのある要素を読む |
| `a11y-record.ps1` | UI Automation の木 (`-Tree`) と、読み上げが始まる出来事 (`-Record`) を書き出す。ナレーターは使わない |
| `explorer-windows.ps1` | `_work` の下のエクスプローラーのウィンドウを読み、条件に合うものだけを閉じる |
| `harness-suspend.ps1` | テスト用の送信側のプロセスだけを数秒止めて戻し、時刻を書く (ハートビートが切れる経路を測る) |
| `stream-sink-stale.ps1` | 親のいなくなった配信の受信プロセスを見つける。`-Stop` を付けたときだけ止める |
| `launch-chrome-fake-mic.ps1` | Chrome をテスト用のマイク入力 (`--use-file-for-fake-audio-capture`) で起動する。`-DryRun` は起動しない |

### 補助ファイル

| ファイル | 役割 |
|---|---|
| `share/pattern.html` | 1280x720 の canvas に、60 fps で大きなフレーム番号、往復する円、時計つきの帯 (升 32 px) を描く 1 ページ。通話の画面共有に使う |
| `share/sync-video.html` | `sync-video.mp4` を繰り返し再生する 1 ページ。再生を始めると動画をタブの表示域いっぱいに広げ、ボタンと説明を隠す。C キーで操作の帯を出し入れする |
| `README-selfcheck.md` | `selfcheck.py` が作るテストデータと、期待する結果 |

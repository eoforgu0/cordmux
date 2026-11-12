# src-obs/tests/dock_drive

ドックを操作するテスト `dock_drive.exe`。製品のドック、参加者の一覧、設定ウィンドウ、ダイアログをそのまま組み込み、画面を 1 つも開かずに入力と状態の変化を流して、押し間違い、ウィンドウの移り変わり、フォーカスを確認する。シナリオの一覧、mistap が数えるもの、終了コードは docs/testing.md の「ドックを操作するテスト」にある。

## 画面を開かず、音を出さない

- Qt のプラットフォームプラグインを、プレビュー用の `cordmuxpreview` (`src-obs/tests/ui_preview/preview_platform.cpp`) に決める。ほかのプラグインで起動したときは、何もせずに終了コード 3 で終わる。このプラグインは OS のウィンドウを作らない。
- 入力は Qt の `QWindowSystemInterface` に渡すだけで、OS の入力 (実際のマウスとキーボード) には何も送らない (`SendInput` も `SetCursorPos` も使わない)。
- アクセシビリティのイベントは、テストが `QAccessible::installUpdateHandler` で受け取る。受け取る関数があると、Qt はイベントを Windows に渡さないので、スクリーンリーダーには届かず、音も出ない。
- OBS、Vesktop、Discord を起動しない。ファイルは `--dir` のフォルダーにだけ書き、設定のファイルは読み書きしない。

## 使い方

```
dock_drive.exe --dir <フォルダー> [--scenario probe,keyboard,windows,record,a11y,menus,layout,dialogs,summary,rowaudio,audio,selfframe,framework,tools,pages,look,status,positions,mistap,unload,dispatch]
               [--seeds 1,2,3,4,5] [--minutes 10] [--theme yami-classic] [--lang ja-JP]
               [--font-scale 10] [--density -2] [--dblclick-ms 500] [--ini-dir <ini のフォルダー>] [--obs-source <OBS のソースツリー>]
```

- 既定の組み合わせは、表示の倍率 100% (96 dpi)、1920 x 1080、テーマ Yami Classic、[フォントサイズ] 10、[密度] -2 (クラシック)。
- `--dir` は `--out` と同じ。PowerShell のスクリプトを通して実行すると、PowerShell が `--out` を自分の `-OutVariable` か `-OutBuffer` と取り違えることがあるので、そのときは `--dir` を使う。
- `--scenario` の既定は `probe` だけ。`unload` は、フォーカスリングが使う最後の入力の監視を外す (`obs_module_unload` と同じ後片付け) ので、最後に置く。`dispatch` は、製品がクラッシュするとテストも終了するので、別の実行にするか、最後に置く。
- 書くファイル: `results.tsv` (1 つの確認に 1 行。列は group、item、value、expect、verdict、note)、`keyboard/ring-*.png` (フォーカスリングの見え方)、mistap のシードごとの `mistap-seed<N>-incidents.tsv` (見つけた押し間違い)、`-actions.tsv` (動作した操作のすべて)、`-gestures.tsv` (入力の手順の始まり)、`-guard.tsv` (押した位置の行が 500 ms 以内に入れ替わっていたクリックごとの、製品のクリックを捨てるガードの答え)。

## mistap のセルフテスト

数え方が押し間違いを数えられることは、クリックを捨てるガードを止めて実行し (`--scenario mistap --seeds 1 --minutes 2 --dblclick-ms 0`)、mistap の (b) が 0 でなくなる (原因は `ran-within-window`) ことで確認する。(b) の判定は、現在のダブルクリックの時間ではなく 500 ms で行うので、ガードを止めても数えられる。製品が覚えている、行が変わった時刻 (`PersonRowWidget` の `changed_at_ms_`) が入れ替わりより前にある場合 (`mark-before-change`) は、製品がこの時刻を付ける処理を変えないと起こせないので、セルフテストでは確認していない。

## Windows に合わせている所

プレビュー用のプラットフォームプラグインは、次の動作が Windows と違うので、テストの側で Windows に合わせる (`input.cpp` と `main.cpp`)。

| 動作 | Windows | プレビュー用のプラットフォームプラグイン | dock_drive |
|---|---|---|---|
| ダブルクリックの時間 | `GetDoubleClickTime` (既定は 500 ms) | Qt の既定の 400 ms | 500 ms にする (`--dblclick-ms`) |
| `QCursor::pos` | OS のカーソルの位置 (ウィンドウから出た通知の時点で、もう新しい位置) | 最後に届いたポインターのイベントの位置 | Enter と Leave を送る間だけ、新しい位置にする |
| アクティブなウィンドウ | 押したウィンドウがアクティブになる。メニューはアクティブを取らない。ダイアログを閉じると親ウィンドウがアクティブになる。止められたウィンドウは押してもアクティブにならない | ウィンドウを作るたびにアクティブにし、隠れたウィンドウの代わりをアクティブにしない | クリックしたとき、メニューとウィンドウが隠れたときに、Windows と同じにする |
| ウィンドウの前後 | 表示したウィンドウとアクティブにしたウィンドウが前。所有されるウィンドウ (フローティングのドック、設定ウィンドウ) は常に親より前 | 作った順 | その順で押す相手を探す |
| Menu キーと Shift+F10 | OS が `WM_CONTEXTMENU` にし、Qt がキーボードからのコンテキストメニューのイベントにする | キーのイベントだけ (メニューは開かない) | キーボードからのコンテキストメニューのイベントを送る |

## 組み込めないもの

製品の `DockPoll` は libobs、Pipeline、OS の問い合わせに依存するので組み込めない。`sim_poll.*` が `tick` と `run_action` のうち、ドックのウィジェットに届く処理を同じ順で行い、`windows` のシナリオの `Host` が、ウィンドウを開く処理 (`src-obs/dock/dock_poll_settings.cpp` と `src-obs/dock/dock_poll_export.cpp`) を同じ順で行う。次のものは、このテストでは確認できないので、実機テストで確認する: インストールの処理が処理中のフラグを立てるタイミング (`src-obs/dock/dock_poll_install.cpp`)、実際の保存のダイアログ (`QFileDialog`)、`DockPoll` が書くほかのログの行。

## ファイル

| ファイル | 中身 |
|---|---|
| `main.cpp` | エントリポイント (プラットフォームプラグインの確認、引数、シナリオの実行) |
| `drive_env.*` | テーマ、ini、メインウィンドウの用意 (プレビューツールの関数を呼ぶ) |
| `input.*` | 入力を送るクラスと、Windows に合わせた所 (上の表) |
| `sim_poll.*` | 周期の処理 (`DockPoll` の `tick` と `run_action` の再現) と Observation |
| `query.*` | 製品のウィジェットを objectName とプロパティで読む関数 |
| `script.h` | 手順を、間隔を空けて順に実行するクラス (`then`、`then_blocking`、`then_when`) |
| `scenarios.h`、`probe.*`、`mistap.*`、`scen_*.cpp` | シナリオ (`scen_glance.cpp` は summary、`scen_framework.cpp` は framework、`scen_windows.cpp` は windows と tools と pages、`scen_positions.cpp` は positions) |
| `scen_common.*` | フォーカスリングの測定 (リングのウィジェットを一時的に隠した画像との差)、コントラスト比 |
| `owner_hold_probe.*` | 操作を始めた範囲だけを止めるダイアログが開いている間に、どこを操作できるかを確認する関数 |
| `report.*` | 結果の表 |
| `crash_trace.*` | クラッシュしたときに、呼び出し履歴を標準エラー出力に書く (`SetUnhandledExceptionFilter` と dbghelp) |
| `dock_drive.cmake` | ビルド (`src-obs/tests/CMakeLists.txt` の `CORDMUX_BUILD_PREVIEW_TOOLS` の節から include する) |

## ビルド

`src-obs/tests` を CMake で構成するときに、`CORDMUX_BUILD_PREVIEW_TOOLS` を `ON` にしたときだけビルドする。プレビューツールと同じく Qt6::GuiPrivate を使うためである。10 分の実行を含むので、`add_test` には入れない。

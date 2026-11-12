# src-obs/dock

OBS のドック。OBS に追加する Qt のウィジェットと、ポーリングのたびにその表示を更新し、押された操作を実行する `DockPoll` を置く。ドックや OBS の [ツール] のメニューから開く設定ウィンドウ、参加者の設定ウィンドウ、インストールとアンインストールのダイアログもここにある。何を表示するかと、操作を押せるかどうかは ledger/ の純粋関数が決め、ここでは判断しない。仕組みは docs/architecture.md の「ドック」、画面の構成は docs/ui_guidelines.md の「ドックの構成」にある。

## 入口

plugin-main.cpp が、OBS の UI スレッドで次の順に呼ぶ。

1. `add_dock()` (`dock_bind.h`) で `DockWidget` を OBS に追加し、追加できたら `DockPoll` (`dock_poll.h`) を作って `start()` を呼ぶ。追加したウィジェットは OBS のメインウィンドウと一緒に破棄されるので、`QPointer` で持つ。
2. `OBS_FRONTEND_EVENT_FINISHED_LOADING` で、`show_dock_once_after_loading()` と `DockPoll::on_frontend_finished_loading()` を呼ぶ。
3. `OBS_FRONTEND_EVENT_EXIT` で `DockPoll::stop()` を呼ぶ。ウィジェットはこの後、`obs_module_unload` より前に破棄される。

`DockPoll` はポーリングのたびに、`build_observation()` で `ledger::Observation` を作り、`ledger::decide_dock()` と `ledger::stabilize()` の結果を `DockWidget::render()` に渡す。押された操作は `ledger::apply()` に通し、返った `Intent` を実行する。設定ウィンドウは `DockPollSettingsPart::open_settings()` (`dock_poll_settings.h`) で開く。

## ファイル

| ファイル | 中身 |
|---|---|
| `dock_bind.h` | ドックを OBS に追加する関数と、cordmux の設定フォルダーのパスを返す関数 |
| `dock_poll.h`、`dock_poll_config.h` | `DockPoll` と、plugin-main.cpp が渡す設定 `DockPollConfig`。`DockPoll` の実装は、処理ごとに `dock_poll.cpp`、`dock_poll_layout.*`、`dock_poll_install.*`、`dock_poll_settings.*`、`dock_poll_export.*` に分けてある |
| `build_observation.h` | ポーリングで読んだ値から `ledger::Observation` を作る純粋関数と、合成のソースの配置の入力に入れる値を決める純粋関数 (名前ラベルを出す人 `composite_roster_people`、自分の枠の人の声を聞けるか `self_frame_hears_voice`、自分の枠の人 `self_frame_user_id`) |
| `dock_widget.h`、`dock_look.h` | ドックのウィジェット `DockWidget` (1 行目、メインとサブの行、タブ、下のバー) と、テーマから読んだ見た目の値 |
| `first_line.h`、`accounts_bar.h`、`dock_tabs.h`、`bottom_bar.h` | 1 行目、メインとサブの行、タブのバーとタブの中身のページ、下のバー (録画のボタンとドックのメニューのボタン) |
| `inline_confirm.h` | 押した場所の隣の確認 (判断は `ledger/inline_confirm.h`) |
| `notice_list.h` | メッセージのカード (レベル 1、2、3 で同じ形。パネルの色の背景と左端の線) |
| `status_tab.h` | 状態タブの中身 (現在のメッセージ、cordmuxの録画、映像の受信の表と [映像のバッファ]、履歴)。位置が動く変化は並びの保留の間ためる (判断は `ledger/status_tab.h`) |
| `people_list.h`、`person_row.h`、`row_menu.h`、`name_check.h` | 参加者の一覧、1 人の行、行のメニューと入れ替える相手のメニュー、行の幅と省略の計算 |
| `audio_tab.h` | 音声タブの中身。1 人の行 (名前、声と画面共有の音声のつまみ、パーセント、メーター、ミュートのボタン) と、行の並びの保留 (判断は `ledger/audio_tab.h`) |
| `look_tab.h`、`deferred_save.h` | 表示タブの中身 (枠、名前ラベル、話している人の強調、ダミー映像)。変えた値はすぐ配信画面に渡し (動かしている間は `ledger::kLookReflectIntervalMs` ごとに間引く)、保存は `DeferredSave` が変更の 1 秒後に 1 回にまとめる。判断は `ledger/look_tab.h` |
| `dock_menus.h`、`rename_dialog.h`、`layout_hold.h` | ドックのメニューと [変更] のメニュー、「名前を変更」のダイアログ、押す直前に一覧を動かさないための並びの保留と、表示された直後のクリックを捨てるガード |
| `settings_window.h`、`participants_window.h`、`settings_fields.h`、`windows_calc.h` | 設定ウィンドウ ([録画]、[映像の受信]、[詳細設定]、[バージョン情報] の 4 ページ) と参加者の設定ウィンドウ (参加者ごとの列は、自動で枠に載せる、cordmuxでの名前、使う声、通話のみ時ラベル非表示)、settings.json のキーごとに値を書き換える所 (設定ウィンドウ、ドックの表示タブと状態タブ、ActionId を通る操作、ファイルだけ) を決めた表、2 つのウィンドウの計算 |
| `install_flow.h`、`install_dialog_plan.h`、`install_dialogs.h` | インストールとアンインストールの判断と、そのダイアログの中身 |
| `ui/` | ドックとダイアログが使うウィジェット (テーマの読み込み、アイコン、フォントの大きさ、テキストの省略、フォーカスリング、メッセージのダイアログ、音量のつまみ `volume_slider.h`、メーター `level_meter.h`、ホイールを受け取らない入力欄 `no_wheel.h`、つまみと数の入力欄 `slider_spin.h`、入力欄を並べる関数 `form_parts.h` など) |

## ビルド

`dock.cmake` が次の関数を定義する。

- `cordmux_add_dock(<target>)`: 全部のソースを追加する。cordmux.dll のターゲットに使う (src-obs/CMakeLists.txt)。
- `cordmux_add_dock_observation(<target>)`: Qt を含まないソースだけを追加する。単体テスト (align_tests) が呼ぶ。
- `cordmux_add_dock_face(<target>)`: ドックのウィジェットのうち、libobs から `obs_module_text`、`obs_frontend_get_locale_string`、`obs_frontend_get_main_window` だけを使うソースを追加する。libobs をリンクしないプレビューツールとドックを操作するテストも呼ぶ。

参加者の一覧 (`rows.cmake`)、2 つのウィンドウ (`windows.cmake`)、インストールとアンインストールのダイアログ (`install_ui.cmake`)、`ui/` (`ui/dock_ui.cmake`) のソースは、それぞれの .cmake が定義する `cordmux_add_<名前>_observation` と `cordmux_add_<名前>` で追加する。上の関数が、定義されているかを確認してから呼ぶ。

## 依存

- ledger/ (表示の規則)、pipeline/ (状態の読み取りと操作)、config/ (設定とこれまでに見た人の一覧)、install/ (Vesktop への追加とアンインストール)、sources/ (OBS のソースとシーン)、bundle/ (診断情報の zip)、diag/ (ログ)、platform/ (ファイルの読み書きと OS の状態の取得)。Windows の API は直接呼ばず、platform/ の関数を使う。
- Qt 6 の Widgets、libobs、obs-frontend-api。`cordmux_add_dock` が `QT_NO_KEYWORDS` を定義するので、`signals`、`slots`、`emit` は使えない。`Q_OBJECT` も使わず、`QObject::connect` にラムダを渡す。
- ドックのコードは OBS の UI スレッドで動く。ほかのスレッドから UI スレッドに処理を渡すのは、診断情報のエクスポート (`dock_poll_export.h`) と、`dock_poll_layout.h` のブリッジの 2 か所である。ブリッジは、Pipeline の受信スレッドからの枠の並べ直しの依頼を受ける。全画面のホットキーは plugin-main.cpp が登録し、UI のスレッドに渡してから `DockPoll::press_focus_hotkey` と `press_focus_restore_hotkey` を呼ぶ。

テストは src-obs/tests の `test_dock_*.cpp` (Qt を使わない部分の単体テスト)、src-obs/tests/ui_preview (プレビューツール)、src-obs/tests/dock_drive (ドックを操作するテスト) にある。

## 合成のソースに渡す値

合成のソースがあるシーンコレクションでは、ポーリングのたびに `DockPollLayoutPart::push_composite_input` (`dock_poll_layout.cpp`) が、配置の入力 (`sources/composite_input.h` の `CompositeInput`) を作って渡す。合成のソースがないシーンコレクションでは、何もしない (全画面にできる人もいないので、全画面をやめる)。

- 画面共有の枠に載っている人と、声を聞けるか。声の枠に上限がないので、ほかの人の声は常に聞ける。自分の枠の人は、自分の声の枠がいま声の経路を持っているときだけ聞ける (`self_frame_hears_voice`。メインアカウントだけの構成では聞けない)。聞けない人は、話していなくても暗くしない。
- 名前ラベルを出す人 (`composite_roster_people`)。通話の参加者を、画面共有しているか、メインアカウントかに依らず全員入れ、受け取りに使うアカウント (サブアカウントの構成のときだけ)、参加者の設定ウィンドウで「通話のみ時ラベル非表示」を付けた人、ほかの人の「使う声」に選ばれている人、Bot のアカウント (`roster_show_bots` が偽のとき) を外す。名前は cordmux での名前か Discord の表示名で、名前を取得できない人は入れない。その人を枠と画面共有していない参加者の並びのどちらに出すかは、合成のソースが、枠に名前ラベルを描くかと同じ関数で描くときに決める (sources/README.md の「名前ラベルと話している人の光」)。ダミーの間は渡さない。渡す人の名前とアバター (avatar で参加者ごとに届いた画像) は `sources::set_roster_person_decor` で先に渡し、渡さなくなった人の値は `forget_roster_person_decor` で消す。
- 全画面で映す人 (`CompositeInput::focus_user_id`)。`DockPoll` がメモリにだけ持つ全画面の人 (`focus_user_id_`) を渡す。渡す前に、全画面でないときの配置で今描いているタイル (`composite_focus_tiles`) を決め直し、全画面の人がいなくなったか cordmux が無効なら全画面をやめる (`DockPollLayoutPart::settle_focus`)。決め直したタイルは、ホットキーの位置から人を選ぶのと、Observation の全画面にできる人 (行のメニューの [全画面で表示] を押せるか) と 1 行目の名前に使う。全画面にしても、どの人をどの枠に載せているかは変えない (全画面で描かれなくなった枠の人は、合成のソースが並びに出す)。ダミーの間は、ダミーのタイルの userId を、その枠に載っている人 (いなければダミーの人の ID) にする (`sources/dummy_feed.h` の `dummy_tile_user_id`)。

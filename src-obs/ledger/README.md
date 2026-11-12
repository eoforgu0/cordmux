# src-obs/ledger

表示の規則。cordmux.dll の現在の状態から、ドック、設定ウィンドウ、参加者の設定ウィンドウ、ダイアログに表示する内容と、操作を押せるかどうかを決める純粋関数を置く。画面に表示するテキストそのものは持たず、ini のキーで返す。仕組みは docs/architecture.md の「表示の規則」と「メッセージ」にある。

## 入口

| ファイル | 中身 |
|---|---|
| `observation.h` | 入力の `Observation`。ドックが周期ごとに、現在の状態から作り直す |
| `rules.h`、`rules.cpp`、`rules_dock.cpp` | `decide_dock(Observation)`。どの状態でどのメッセージを表示するかの表 (primary と notices) と、表示する内容 (`dockview::DockViewV2`) を作る処理 |
| `actions.h`、`actions.cpp` | `apply(Observation, ActionId, ActionArgs)`。操作を押せるかどうかと、押したときにする処理 (`Intent`) |
| `notice_tracker.h`、`notice_tracker.cpp` | `stabilize()`。メッセージを表示するまでと消すまでの待ちと、表示した順の並べ替え。`settle_first_line()`。1 行目の中身と既読、状態タブの件数。前の周期の状態はドックが持つ |
| `dock_view.h` | 表示する内容の型 (`dockview::DockViewV2` など) |
| `view.h` | テキストの参照 `TextRef` と、名前や値の表示 `Label` |
| `text_ids.h`、`plural.h` | ini のキーの定数 (`text::keys`) と、数で形が変わるテキストのキーの組 (`CountedId`) |
| `natural_order.h` | 名前を自然順で比べる `natural_less` |
| `audio_tab.h`、`audio_tab.cpp` | 音声タブの行の並びと状態、行がないときの 1 文を決める `audio_tab_of`。メーターが出す値と色の使い分けを決める `meter_reading` |
| `status_tab.h`、`status_tab.cpp` | 状態タブの判断。映像途切れの累計 `step_stall_tally`、履歴の出来事 `history_events`、最後の録画 `step_recording_memory`、状態タブの View と次の状態を決める `settle_status`。前の周期の状態 (`StatusState`) はドックが持つ |
| `slot_cells.h` | 枠に載っている人の位置を決める `plan_slot_cells`。配信の配置 (`sources/composite_layout.h`) と行の枠の位置が同じ関数を呼ぶ |
| `inline_confirm.h` | 押した場所の隣で確認するかを決める `confirm_needed` |
| `stream_record.h` | 設定 `record_with_stream` で、配信の開始と終了のイベントのときに録画を始める、終えるかを決める `record_follow_of` と、録画の経過時間の文字列 `record_elapsed_text` |
| `ledger.cmake` | ソースをターゲットに追加する `cordmux_add_ledger(<target>)` |

## 依存

- libobs にも Qt にも依存しない。時計、ファイル、OS も読まない。現在の時刻、時差、しきい値は `Observation` の値として受け取る。同じ `Observation` からは必ず同じ結果が返るので、テスト (`src-obs/tests/test_ledger_*.cpp`) は OBS も Qt も起動せずに実行できる。
- 読み込むのは C++17 の標準ライブラリと、`config/allocation.h` (枠の数と、使う声の組を 1 対 1 に絞る関数)、`config/person_audio.h`、`config/settings.h`、`pipeline/arrival_window.h`、`pipeline/audio_meter.h` だけである。
- テキストは `src-obs/data/locale` の ini にある。`test/lint/locale_ids.py` が、コードの中のキーと ini のキーを突き合わせる。

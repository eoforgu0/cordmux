# src-obs/config

cordmux.dll が持つ状態のうち、枠の割り当て表 (保存しない)、処理の段階ごとの件数、設定 (settings.json)、これまでに見た人の一覧 (seen_people.json) を置く。仕組みは docs/architecture.md の「コンポーネントと状態の管理」「設定の保存場所」「シーンコレクションを切り替えたとき」にある。

## ファイル

| ファイル | 中身 |
|---|---|
| `allocation.h` | 枠の割り当て表 `AllocationTable` (画面共有の枠 4 つと、上限のない声の枠)。割り当て (`try_assign`)、枠の書き直し (`rewrite`)、シーンコレクションを切り替える間の凍結 (`begin_scene_collection_change`、`apply_scene_collection_changed`、`is_frozen`)、枠を空ける操作 (`release`)、使う声の置き換えの組を絞る `usable_voice_links`。割り当て表は、誰が通話に参加しているかを知らない (枠を空ける判断は pipeline/slot_release.h) |
| `label_placement.h` | 名前ラベルの中のアバターの位置 `AvatarSide`、画面共有している参加者の名前ラベルを置く枠の隅 `LabelCorner` (全員一律)、画面共有していない参加者の位置 `RosterAnchor` と向き `RosterDirection`、位置に合う向きを返す `roster_direction_for`。JSON のライブラリを含めずに描く側が使えるよう、設定から分けてある |
| `stage_counters.h` | 処理の段階 (受信、デコード、OBS に渡した、捨てた) ごとの件数を、stream_id と、映像か音声かに分けて数える `StreamStageCounters` と、2 つの時点の件数から毎秒の件数を求める `rate_between`。どの段階で落ちたかの切り分けに使う件数で、合否の判定には使わない |
| `settings.h` | 設定 `Settings` と、settings.json との変換 (`parse_settings`、`settings_to_json`)、範囲の外の値を収める `clamp_settings`。キーごとの既定値と範囲の理由は、フィールドと定数のコメントにある |
| `seen_people.h` | これまでに見た人の一覧の変換 (`seen_people_from_json`、`seen_people_to_json`) と、上限を超えたときに捨てる行の選び方 (`trim_seen_people`)。参加者ごとの値は、自動で載せる、cordmux での名前、使う声、画面共有していない参加者の並びに出さない (`roster_hidden`)、声と画面共有の音の音量 (`voice_volume_pct`、`screen_volume_pct`)。`roster_hidden` は、参加者の設定ウィンドウの列「通話のみ時ラベル非表示」を付けた人で、付けたときだけ書く。音量は 0 から 200 の整数で、100 でないときだけ書く。範囲の外は一覧ごと拒む。ミュートは書かない |
| `person_audio.h` | 参加者ごとの音量の範囲 (0 から 200%、既定 100%) と、音量から利得への変換 |
| `config.cmake` | このフォルダーのヘッダーをビルドに追加する |

どれもヘッダーだけで実装している。

## 依存

- C++17 の標準ライブラリと nlohmann/json (`settings.h`、`seen_people.h`)。`seen_people.h` は、一覧の 1 行の型に `ledger/observation.h` の `ledger::SeenPerson` を使う。libobs にも Qt にも依存しない。
- 割り当て表の所有者は plugin-main.cpp で、pipeline/ が参照を借りて使う。settings.json を読むのは plugin-main.cpp、書くのはドック (dock/) で、seen_people.json を読み書きするのもドックである。
- テストは src-obs/tests の `test_config_*.cpp` にある。

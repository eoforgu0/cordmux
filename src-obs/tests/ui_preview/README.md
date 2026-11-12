# src-obs/tests/ui_preview

プレビューツール (`ui_preview.exe`)。製品のドック (`src-obs/dock`)、設定ウィンドウ、参加者の設定ウィンドウ、ダイアログ、メニューを製品のコードのまま作り、状態ごとの画像 (PNG) とウィジェットの一覧 (`widgets.tsv`) を書き出す。OBS 31.1.1 のテーマ (Yami、Yami Light など) を、OBS と同じ手順で適用する。すべての状態を並べて、見た目と文言を確認するために使う。合否を持たないので、テスト (`add_test`) には入れていない。画面にウィンドウを表示しないので、実行中もほかの作業を妨げない。テストの中での位置付けは docs/testing.md の「プレビューツール」にある。

## 使い方

`src-obs/tests` を CMake で構成するときに `CORDMUX_BUILD_PREVIEW_TOOLS` を `ON` にすると、`ui_preview` もビルドする。`run_ui_preview.py` が既定で使う CMake のプリセット `tests-x64-local` の用意の仕方は、docs/testing.md の「プレビューツール」にある。

```
python src-obs/tests/ui_preview/run_ui_preview.py --out <出力先> [ui_preview.exe の引数 ...]

ui_preview.exe --out <出力先> [--face <描画の対象>] [--themes yami,yami-light] [--langs ja-JP,en-US] [--only <文字列>]
               [--widths-em 15,20,26,32,45|none] [--sizes 230x165,...] [--height 600] [--at-min-width]
               [--at-status-floor] [--font-size 10] [--density classic|compact|normal|comfortable|-2..-5]
               [--dpr 1.5] [--screen 1366x768@125]
               [--obs 31|32] [--obs-install <フォルダー>] [--obs-source <フォルダー>] [--ini-dir <フォルダー>]
               [--first-line-scale 1.5] [--l1-border -1|0|1|2] [--tier-icons theme|windows]
               [--heading-scale 1.1] [--heading-space 1,1,0.5,2.25,0.25]
               [--status-heading-above 1] [--note-below 0.5] [--section-gap 1]
               [--dialog-main-scale 0|1.1|1.5] [--tab people|audio|display|status] [--backend cordmuxpreview|minimal]
ui_preview.exe --list
ui_preview.exe --out <出力先> --dump-theme
ui_preview.exe --probe-fonts

python src-obs/tests/ui_preview/render_sets.py --exe <ui_preview.exe> --out <出力先> [--sets base,classic,...]
python src-obs/tests/ui_preview/cvd_sheet.py --png <画像>... --out <出力先> | --contrast <contrast.tsv>... | --selftest
python src-obs/tests/ui_preview/settings_places_count.py [--out <出力先>]
```

| 引数 | 意味 |
|---|---|
| `--face` | 描画の対象 (既定はドック)。一覧は `faces.h` の `FaceOptions::face` のコメントにある |
| `--widths-em`、`--sizes`、`--height`、`--at-min-width`、`--at-status-floor` | ドックの大きさ (em、px、製品が計算した最小幅、行の状態のテキストを表示するいちばん狭い幅)。参加者の設定ウィンドウ (`--face participants`) では、`--at-min-width` を付けると、ウィンドウが許す最小の幅でも描く (名前の後ろは `-min`) |
| `--font-size`、`--density` | OBS の [外観] の [フォントサイズ] と [密度] (user.ini の `[Appearance]` の `FontScale` と `Density`) |
| `--dpr`、`--screen` | 画像の拡大率と、描画する画面 (解像度と表示の倍率。`main.cpp` の `ScreenChoice` のコメント) |
| `--obs 32`、`--obs-install` | OBS 32.2.2 をインストールした形のフォルダーの、テーマと ini を読む (フォントは 31.1.1 のソースのものを使う) |
| `--first-line-scale`、`--l1-border`、`--tier-icons`、`--dialog-main-scale` | 見た目の値を変えて比べる (`src-obs/dock/ui/dock_style.h` の `DockStyle`) |
| `--heading-scale`、`--heading-space`、`--status-heading-above`、`--note-below`、`--section-gap` | ドックの見出しの段 (大見出しの倍率、大見出しと中見出しの上下の余白、状態タブの見出しの上の余白)、説明の文の下の余白、意味の違う部分の間の余白を変えて比べる (em。`DockStyle` の同じ名前の値。設定ウィンドウと参加者の設定ウィンドウは既定の値のまま) |
| `--tab` | ドックで開くタブ (既定は参加者タブ)。状態タブの場面は `status-empty`、`status-messages`、`status-finalize`、`status-recording`、`status-last-recording`、`status-table`、`status-table-30`、`status-no-composite`、`status-history-60` (`--only` に渡す名前は、`status-recording` が `status-recording-button` にも一致する) |
| `--backend minimal` | Qt 同梱のプラットフォームプラグイン minimal で描画する (`preview_platform.h` の違いが画像に反映されるかを比べる) |
| `--list` | 場面ごとに、成り立つ表の行を出力する |
| `--dump-theme` | テーマの QSS とパレットを書き出す |
| `--probe-fonts` | 製品と同じ QSS で、どのフォントのグリフが使われたかを出力する |

どの場面でも成り立たない表の行 (src-obs/ledger/rules.cpp の primary と notices、src-obs/ledger/rules.h の AddedRuleId) があると、描画せずに 4 で終了する (`fixtures.h` の `uncovered_rules`)。

- `render_sets.py` はセットごと (`sets()` の前のコメント) にすべての画像を描画し、チェックの結果を `summary.tsv` と `summary.md` に書く。
- `cvd_sheet.py` は画像を 1 型・2 型の色覚の見え方と白黒に変換して並べる。
- `settings_places_count.py` は、設定の値ごとに変えられる場所を製品のコードから集める。

## 出力

`<出力先>/face-<描画の対象>/<テーマ>[-<OBS のバージョン>][-f<フォントサイズ>-d<密度>][-x<拡大率の百分率>]/<言語>/` に、画像ごとの `<名前>.png` と `<名前>.widgets.tsv`、すべての画像を並べた `index.html` を書く。既定の設定 (OBS 31.1.1、フォントサイズ 10、[通常]、100%) では、テーマの名前だけのフォルダーになる。

| ファイル | 中身 | 列の説明 |
|---|---|---|
| `<名前>.widgets.tsv` | 描画したすべてのウィジェット (メニューなら、すべての項目) | `faces.cpp` の `kWidgetsHeader` |
| `metrics.tsv` | ドックの場面と大きさごとの、1 行目の高さ、スクロール、最小幅など | `faces.h`、`measure.h` |
| `targets.tsv`、`names.tsv`、`contrast.tsv`、`edges.tsv` | 押せるものと WCAG 2.5.8、名前の見え方、テキストのコントラスト、文字の上下の見切れ | `measure.h` |
| `wraps.tsv`、`fields.tsv`、`screen-fit.tsv` | 折り返す QLabel、1 行の入力欄、ウィンドウが画面に収まるか | `faces.cpp` |
| `focus.tsv`、`timeline.tsv`、`theme-switch.tsv`、`menu-check.tsv`、`obs-layout.tsv` | 1 枚の画像では済まない描画の対象 | `probes.h`、`probes.cpp` |
| `summary.tsv`、`summary.md` | `render_sets.py` の、セットごとのチェックの結果 | `render_sets.py` |

## ファイル

| ファイル | 中身 |
|---|---|
| `main.cpp` | エントリポイント。引数の読み込み、網羅のチェック、`--list`、`--dump-theme`、`--probe-fonts` |
| `faces.h`、`faces.cpp` | 描画の対象ごとの描画と、表 (tsv) の書き出し |
| `fixtures.h`、`fixtures.cpp` | 場面 (Observation) の一覧と、網羅のチェック |
| `measure.h`、`measure.cpp` | 描画したウィジェットを調べる (1 行目、名前、押せるもの、コントラスト、文字の見切れ) |
| `probes.h`、`probes.cpp` | 1 枚の画像では済まない描画の対象 (focus、timeline、theme-switch、probe:*) |
| `render.h`、`render.cpp` | 製品のドックを作り、テーマを適用して、ウィンドウを表示せずに画像にする |
| `obs_theme.h`、`obs_theme.cpp` | OBS のテーマ (.obt と .ovt) を、OBS と同じ QSS とパレットにする |
| `preview_platform.h`、`preview_platform.cpp`、`preview_platform.json` | Qt のプラットフォームプラグイン `cordmuxpreview` (minimal との違いは `preview_platform.h`) |
| `locale_stub.h`、`locale_stub.cpp`、`stub/` | 製品が呼ぶ libobs と OBS のフロントエンドの関数の代わり (ini の読み込み) |
| `obs_dock.h`、`obs_names.h` | OBS と同じ型の名前を持つクラス (OBS のテーマが型の名前で選ぶため) |
| `fidelity.h`、`fidelity.cpp` | プレビューの画像と、OBS の実際の画面のスクリーンショットを比べる関数 |
| `ui_preview.cmake` | ターゲットを追加する `cordmux_add_ui_preview()` |
| `run_ui_preview.py` | ビルドと実行をまとめて行う |
| `render_sets.py` | セットごとの描画とチェック |
| `cvd_sheet.py` | 色覚の見え方と白黒への変換 |
| `settings_places_count.py` | 設定の値ごとの、変えられる場所の集計 |

## 依存

- 製品の `src-obs/ledger` (`cordmux_add_ledger`) と `src-obs/dock` (`src-obs/dock/dock.cmake` の `cordmux_add_dock_face` と、同じフォルダーの .cmake の関数)。ini は `src-obs/data/locale` から読む。libobs は使わない。
- OBS 31.1.1 のソース (テーマ、`frontend/data/locale` の ini、フォント)。製品の構成が `src-obs/.deps` に取得したものを、`src-obs/cmake/cordmux_obs_deps.cmake` の `CORDMUX_OBS_STUDIO_SOURCE_DIR` で読む。
- Qt6 の Core、Gui、GuiPrivate、Widgets、Svg と、Windows の shell32 と ole32。GuiPrivate はプラットフォームプラグインと DirectWrite 版のフォントデータベースに使う。
- Python のスクリプトは Python 3 を使う。`render_sets.py` の画像を読むチェックと `cvd_sheet.py --png` は Pillow も使う。
- `src-obs/tests/dock_drive` も、このフォルダーのプラットフォームプラグイン、テーマの適用、ini の読み込み、場面を使う。

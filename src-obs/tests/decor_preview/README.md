# src-obs/tests/decor_preview

名前ラベルのプレビューツール (`decor_preview.exe`)。製品の合成のソース (cordmux) の名前ラベルと光を、製品と同じ関数で描いた画像 (PNG) と、OBS の中で撮った画像とピクセルで比べるための参照を書き出す。合否を持たないので、テスト (`add_test`) には入れていない。ウィンドウを 1 つも作らないので、実行中もほかの作業を妨げない。テストの中での位置付けは docs/testing.md の「プレビューツール」にある。

## 使い方

`src-obs/tests` を CMake で構成するときに `CORDMUX_BUILD_PREVIEW_TOOLS` を `ON` にすると、`decor_preview` もビルドする。動作は引数で選ぶ。`--composite-sample`、`--composite-sweep`、`--composite-check`、`--composite-check-background`、`--composite-check-avatar` のどれも付けなければ、使い方を表示して終了する。`decor_preview.exe --help` で引数の一覧を見られる。共通の引数は `--backend cordmuxdecorpreview|minimal|windows` と `--system-font NAME` である。

## 合成のソースの見本

`--composite-sample` を付けると、合成のソースの名前ラベルとグローの見本を 1 枚書き出す。`sources/composite_layout.h` の `plan_composite_layout` で、自分の枠と 3 人を 2 x 2 に並べ (左上が自分の枠、右上から表示名の順)、枠ごとに違う背景 (暗いゲーム画面、明るい空、細かい縞、下側が明るい景色) の上に、`sources/composite_label.h` の描画で名前ラベルとグローを描き、製品と同じ置く物の計画 (`sources/composite_draw_plan.h`) のとおりに、CPU で重ねる関数 (`sources/composite_draw_cpu.h`) で重ねる。製品の GPU の描画と同じ画像、同じ順、同じ切り抜き、同じ混ぜ方の式である。値は設定のキーと同じ作り (`composite_label_look_from_settings`) で決めるので、設定の既定値がそのまま見本の既定値になる。

```
decor_preview.exe --composite-sample --out <出力先> [--name-pct 2.2] [--glow avatar,name] [--speaker 1]
```

| 引数 | 意味 | 既定値 |
|---|---|---|
| `--out` | 出力先のフォルダー (必須) | |
| `--name-pct` | 名前の大きさ (キャンバスの高さに対する %。1.0 から 8.5、0.1 刻み) | 設定の既定値 (`label_em_ratio_pct`) |
| `--glow` | 光らせる対象。`none` (何も光らせない)、または `frame`、`avatar`、`name`、`all` から `,` でつなぐ | 設定の既定値 (`avatar,name`) |
| `--speaker` | 話している人の枠の位置。0 左上 (自分)、1 右上、2 左下、3 右下、`none` はだれも話していない。その枠にグローを最大の明るさで描く | 1 |
| `--corner` | 名前ラベルを置く角 `tl`、`tr`、`bl`、`br` (全員同じ) | `bl` |
| `--plate` | 背景を表示する | 表示しない |
| `--no-avatar` | アバターを表示しない | 表示する |
| `--no-slot-labels` | 名前ラベルを枠に描かない (`label_show_in_slot`)。枠の人は、画面共有していない参加者の並びに描く。枠の光 (`--glow frame`) は枠に描く | 枠に描く |
| `--margin-pct`、`--outline-ratio`、`--opacity-pct`、`--avatar-ratio`、`--glow-blur-ratio`、`--glow-spread-ratio` | 設定のキー `label_margin_pct`、`label_outline_ratio`、`label_opacity_pct`、`label_avatar_ratio`、`label_glow_blur_ratio`、`label_glow_spread_ratio` と同じ値と範囲 | 設定の既定値 |
| `--font-family`、`--font-weight` | フォントの名前と太さ | 設定の既定値 |
| `--canvas` | キャンバスの大きさ `WxH` | `1920x1080` |
| `--tag` | 出力のファイル名に付ける名前 | 引数から決める |
| `--people` | 席 (自分の枠、画面共有の枠 1 から 3) の順に、架空の人の id を 4 つ `,` でつなぐ (`composite_scene.cpp` の `scene_people`) | `mika,noa,taro,alexander` |
| `--tile-images` | 枠の位置 (左上、右上、左下、右下) の順に、模様の代わりに描く画像のファイルを 4 つ `,` でつなぐ。枠を覆うように拡大し、はみ出た分を中央で切る | 模様を描く |

範囲の外の値と、刻みに合わない値は、エラーで終了する (設定の画面から入る値と同じ丸め方で変わってしまうため)。

| 出力 | 中身 |
|---|---|
| `composite_<名前>.png` | キャンバス全体 |
| `composite_<名前>_tile.png` | 話している枠 (だれも話していなければ左上の枠) の等倍の画像 |
| `composite_<名前>.json` | 使った設定の値と、枠ごとの名前ラベルの結果 (矩形、省略したか、頭文字の円か) |

人の名前とアバターは見本のために作った架空のもの (アバターの画像がある人が 3 人で、そのうち 1 人は名前が長く、名前を大きくすると省略される。画像がなく頭文字の円になる人が 1 人) で、どの引数でも同じである。製品のフォント (`--font-family` が空なら `UD デジタル 教科書体 NK-B` で、ない PC では `Yu Gothic UI`) と製品の描画をそのまま使うので、実行した PC に入っているフォントで描かれる。

## README の画像

リポジトリの README の画像は、実際の OBS (テスト用の OBS) の画面とドックを撮ったものである。枠の中の画像は `readme_tiles.py` の `--style kenney` で描いた 4 つの画像 (Kenney の素材で組んだ架空のゲームの画面。下の段落にある) で、撮り方は test/tools/obs-run/README.md の「実際の OBS の画面を撮る」にある。

`--people` と `--tile-images` は、OBS を起動せずに、画像の色合いと名前ラベルの重なり方を確かめるときに使う。Git Bash で、リポジトリのルートから次のように描く (Pillow が要る)。

```
python src-obs/tests/decor_preview/readme_tiles.py --out _work/tmp/readme-img/tiles
t=_work/tmp/readme-img/tiles
decor_preview.exe --composite-sample --out _work/tmp/readme-img --tag readme --name-pct 4.0 --speaker 1 \
  --people sakura,mika,rin,yuzu --tile-images "$t/tile_tl.png,$t/tile_tr.png,$t/tile_bl.png,$t/tile_br.png"
```

`--style kenney` を付けると、図形の代わりに Kenney (kenney.nl) の素材 (ライセンスは CC0-1.0) のタイルで組んだ 4 つの画面 (左上から、横スクロールのアクション、上から見たレース、宝石をそろえるパズル、上から見た RPG の町) を描く。図形の画面は彩度とコントラストを大きく下げるが、Kenney の画面は色を素材に近いまま残して明るさを下げ、そのうえで 4 枚の明るさ (L の平均) を 4 枚の平均にそろえる (加工の強さは `readme_tiles.py` の `SHAPES_MUTE` と `KENNEY_MUTE`)。どの画面も、名前ラベルが載る左下には素材を置かず、赤い大きな面を作らない。

素材はリポジトリに入れない。次のパックをそれぞれの頁の [Download] から取得し、zip の中身を、パックごとの空のフォルダー`_work/assets/kenney/<フォルダーの名前>/` に展開して、そのフォルダーの親を `--assets` で渡す。足りないパックがあると、そのパックと取得の方法を表示して止まる。パックの見本の画像 (preview や sample) は使わない。

| フォルダーの名前 | パック | 使う画面 |
|---|---|---|
| `pixel-platformer` | [Pixel Platformer](https://kenney.nl/assets/pixel-platformer) | アクション |
| `racing-pack` | [Racing Pack](https://kenney.nl/assets/racing-pack) | レース |
| `puzzle-pack-2` | [Puzzle Pack 2](https://kenney.nl/assets/puzzle-pack-2) | パズル |
| `tiny-town` | [Tiny Town](https://kenney.nl/assets/tiny-town) | RPG の町 |
| `tiny-dungeon` | [Tiny Dungeon](https://kenney.nl/assets/tiny-dungeon) | RPG の町の人物 |

```
python src-obs/tests/decor_preview/readme_tiles.py --out _work/tmp/readme-img/tiles --style kenney --assets _work/assets/kenney
```

## つまみごとの見本の一覧

`--composite-sweep` を付けると、合成のソースの見た目のつまみを 1 つずつ端から端まで何段かに動かした見本を、つまみごとに 1 枚の一覧の PNG に書き出す。ほかのつまみは設定の既定値のままにする。1920 x 1080 のキャンバスで左上の枠 (名前の長さが中くらいの人) が最大の明るさで話している場面を描き、その名前ラベルのあたり (左上の枠の左下の隅から、下の枠との境をまたいだ 720 x 360 の範囲) を等倍で切り出して、段ごとに値を書き込む。左の列は明るい背景 (空)、右の列は暗い背景 (暗いゲーム画面) で、下の枠も同じ明るさの背景にする。描き方は `--composite-sample` と同じ (製品の計画と CPU で重ねる関数)。

```
decor_preview.exe --composite-sweep --out <出力先> [--knob <名前>]
```

`--knob` を付けると、そのつまみの一覧だけを書く。出力は `sweep_<名前>.png` で、名前と動かす値は次のとおり (値の後の「(既定)」は設定の既定値)。

| 名前 | つまみ | 値 |
|---|---|---|
| `name_size` | 名前の大きさ (`label_em_ratio_pct`) | 1.0、2.5、4.0、5.5、7.0、8.5 (%) |
| `margin` | 余白 (`label_margin_pct`) | 0、0.5、1.0、3.0、7.0、13.9 (%) |
| `outline_width` | 縁取りの太さ (`label_outline_ratio`) | 0、0.05、0.1、0.2、0.3、0.4 |
| `outline_color` | 縁取りの色 (`label_outline_color`) | 黒、白、青紫、赤 |
| `text_color` | 文字の色 (`label_text_color`) | 白、黒、黄、緑 |
| `opacity` | 名前ラベル全体の不透明度 (`label_opacity_pct`) | 0、25、50、75、100 (%) |
| `avatar` | アバターの表示と大きさ (`label_show_avatar`、`label_avatar_ratio`) | 表示しない、0.4、1.0、1.5、2.0、3.0 |
| `plate` | 背景の表示と色と不透明度 (`label_show_plate`、`label_plate_color`、`label_plate_opacity_pct`) | 表示しない、黒の 25・50・75・100%、白の 50%、青紫の 50% |
| `glow_shape` | 光らせる対象 (`label_glow_*`) | アバターと名前、アバター、名前、枠、枠とアバターと名前、なし |
| `glow_color` | 光の色 (`label_border_color`) | 緑、白、赤、青紫、黄 |
| `glow_blur` | ぼかし (`label_glow_blur_ratio`) | 0、0.1、0.5、1.0、2.0、3.0 |
| `glow_spread` | 広がり (`label_glow_spread_ratio`) | 0、0.1、0.25、0.5、0.75、1.0 |
| `level` | 話している間の明るさ (立ち上がりと消える途中の値。時間の代わり) | 0、0.25、0.5、0.75、1 |

## OBS の中の画像と比べる参照

`--composite-check` は、OBS の中で合成のソースを撮った画像とピクセルで比べるための参照を描く。使うのは `test/tools/composite-check/compare.py` で、比べ方はそのフォルダーの README.md にある。設定は製品が使った settings.json を製品と同じ関数 (`config::parse_settings`) で読み、読めなければエラーで終了する (製品は既定値で続けるが、参照がそれに合わせると、書き損じた設定のまま比べてしまうため)。配置 (`make_composite_input` と `plan_composite_layout`)、名前ラベルと光の描画、話していない人の暗さ (`composite_label_dim_factor`)、画面共有していない参加者の並び (`composite_roster_drawn` と `plan_composite_roster`)、置く物の計画、CPU で重ねる処理も製品の関数をそのまま使う。

```
decor_preview.exe --composite-check <要求の JSON> --out <出力先>
decor_preview.exe --composite-check-background <dark-game|bright-sky|stripes|snowy-field> <幅>x<高さ> <PNG>
decor_preview.exe --composite-check-avatar <色相> <PNG>
```

要求の JSON の形は次のとおり。

| キー | 中身 |
|---|---|
| `canvas` | `[幅, 高さ]` |
| `settings_file` | 製品が使った settings.json のパス |
| `self_shown` | 自分の枠を出しているか (省くと、`slot` 0 の人がいれば真) |
| `people` | 枠に載る人の一覧。`slot` (0 は自分の枠、1 から 4 は画面共有の枠の番号)、`user_id`、`name`、`avatar_png` (省くと画像なしで頭文字の円)、`video` (その枠に映像を描いたか。省くと真)、`hears_voice` (cordmux がその人の声を聞けるか。偽なら暗くしない。省くと真)。名前ラベルの隅は設定の `label_corner` を全員に使う (参加者ごとの `corner` を書くとエラーになる) |
| `roster` | 名前ラベルを出す人 (製品のドックが合成のソースに渡す人) の一覧。`user_id`、`name`、`avatar_png`、`hears_voice` (意味は `people` と同じ)。省くと並びは空。製品と同じく、名前ラベル付きで描く枠がない人だけを並びに描く (`people` の人を入れると、枠に名前ラベルを描くときは並びに出ず、[1人のときは非表示] で隠すときと映像がない枠のときは並びに出る)。並べる順は製品と同じく参照が決める |
| `background_png` | キャンバスと同じ大きさの背景 (省くと透明) |
| `renders` | 描く参照の一覧。`name` (英数字と `_` と `-`)、`levels` (`people` と同じ並びの、話している明るさ 0 から 1)、`roster_levels` (`roster` と同じ並び。省くと全員 0)、`mixer_audible` (OBS のミキサーで合成のソースが鳴っているか。偽はミュートか音量 0 で、光らせず暗くもしない。省くと真)、`background_png` (その 1 枚だけの背景) |

- 同じ画面に見えているほかの人の名前は、名前ラベルには `video` が真のほかの人の名前 (製品の `distribute_person_decor` と同じで、並びの人の名前は入れない)、並びの名前ラベルには、名前ラベル付きで描く枠の人と並びの人の名前から本人を除いたもの (製品の `update_roster` と同じ `composite_roster_others`) にする。
- 並びの位置には、設定のずれ (`roster_offset_x_px`、`roster_offset_y_px`) を足す。
- 光の明るさは、話している人の光を表示しない設定かミキサーで鳴っていないときは 0 にして使い (製品の `composite_speaking_level` と同じ)、名前ラベルの暗さの倍率にはその設定に依らずに使う (製品の `composite_label_dim_at` と同じ)。

| 出力 | 中身 |
|---|---|
| `ref-<name>.png` | 背景の上に、計画のとおりに重ねた参照 |
| `obj-<n>.png` | 置く物を 1 つずつ、透明の上に最大の明るさで置いた画像 (名前ラベルは暗くしない) |
| `check.json` | 使った設定の値、名前の大きさと余白、暗さ、配置のログの行 (`Composite layout ...`)、タイルごとの名前ラベルの結果、並びに置いた人の結果 (`roster`。位置と名前ラベルの矩形)、参照ごとの置く物の計画 (種類 `frame_glow`、`label_glow`、`label`、`roster_glow`、`roster_label` と明るさと暗さの倍率)、`obj-<n>.png` の一覧 |

PNG の 4 つの値には、乗算済みアルファの値をそのまま入れる (アルファで割り戻さない)。背景の PNG も同じ約束で読む。OBS で撮った PNG と同じ並びにして、そのまま比べられるようにするためである。`--composite-check-background` は見本の背景 (`composite_scene.h` の `make_scene_background`) を、`--composite-check-avatar` は見本のアバターを PNG に書く。

参照の文字を OBS の中と同じにするときは `--backend windows` を付ける (下の「依存」)。

## ファイル

| ファイル | 中身 |
|---|---|
| `main.cpp` | エントリポイント。プラットフォームプラグインの選択と、動作の振り分け |
| `composite_check.h`、`composite_check.cpp` | OBS の中の画像と比べる参照 (`--composite-check`) と、その背景とアバターの PNG |
| `composite_sample.h`、`composite_sample.cpp` | 合成のソースの見本 (`--composite-sample`) と、つまみごとの見本の一覧 (`--composite-sweep`) |
| `composite_scene.h`、`composite_scene.cpp` | 見本の場面 (人、背景、製品の配置と計画で重ねる処理)。名前ラベルの調整ツール (`src-obs/tests/label_tuner`) も使う |
| `decor_qpa.h`、`decor_qpa.cpp`、`decor_qpa.json` | Qt のプラットフォームプラグイン `cordmuxdecorpreview` |
| `decor_preview.cmake` | ターゲットを追加する `cordmux_add_decor_preview()` |

## 依存

- 製品の `src-obs/sources` の `decor_paint.*` と `decor_paint_common.h` (フォントの候補と、文字と画像の扱い)、`composite_label.*` と `composite_layout.h` (合成のソースの名前ラベルと配置)、`composite_roster.h` (画面共有していない参加者の並び)、`composite_draw_plan.h` と `composite_draw_cpu.*` (置く物の計画と、それを CPU で重ねる関数)、`label_border.h` (光のタイミングの式)、`src-obs/config/settings.h` (設定の既定値)。libobs は使わない。
- Qt6 の Core、Gui、GuiPrivate。GuiPrivate は、プラットフォームプラグインと DirectWrite 版のフォントデータベースに使う。
- Qt のプラットフォームプラグインは、既定の `cordmuxdecorpreview`、`--backend minimal` で選ぶ `minimal`、`--backend windows` で選ぶ `windows` のどれかだけを使い、ほかのプラグインでは実行しない。`cordmuxdecorpreview` は、フォントデータベース、既定のフォント、文字を混ぜるときのガンマ (Qt の `QPlatformIntegration::FontSmoothingGamma`。OS の `SPI_GETFONTSMOOTHINGCONTRAST` の値) を OBS (Qt 6.8.3 の windows プラグイン) と同じにする。`minimal` は、フォントデータベースの違いが画像に反映されるかを比べるためだけに使う (ガンマは Qt の既定の 1.7 のままなので、文字の縁のピクセルが違う)。`windows` は OBS の中と同じプラグインそのもので、ウィンドウを作らないので画面には何も出ない。高 DPI の拡大と dpi は OBS と同じく変えない。

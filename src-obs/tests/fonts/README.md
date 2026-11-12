# src-obs/tests/fonts

`decor_tests` (名前ラベルと話している人の光の描画のテスト。`../decor`) が読むフォント。配布用の zip には入らない。製品の名前ラベルは、設定のフォント (既定は UD デジタル 教科書体 NK-B で、ない PC では Yu Gothic UI。`src-obs/sources/decor_paint.h` の `kDefaultLabelFontFamilies`) で描画する。

| ファイル | 元のファイル | ライセンス |
|---|---|---|
| `OpenSans-Regular.ttf` | obs-studio 31.1.1 の `frontend/forms/fonts/OpenSans-Regular.ttf` | Apache License 2.0 (`LICENSE-Apache-2.0.txt`) |
| `LICENSE-Apache-2.0.txt` | Apache License 2.0 の全文 | - |

フォントの `name` テーブルの値: `Open Sans`、`Regular`、`Version 1.10`、`Digitized data copyright 2010-2011, Google Corporation.`、`Licensed under the Apache License, Version 2.0`。SHA-256: `e64e508b2aa2880f907e470c4550980ec4c0694d103a43f36150ac3f93189bee`。

## 同梱する理由

`decor_tests` は、描画した画像を期待する画像 (`../snapshots`) と 1 ピクセルの違いも許さずに比べる。PC に入っているフォントは PC ごとにバージョンが違い、同じ名前でも文字の形が変わることがあるので、比較に使えない。そのため、同梱したフォントを `QFontDatabase::addApplicationFont` で読み込み、その名前を指定して描画する (`../decor/main.cpp`)。

期待する画像に入れているのは、このフォントにある文字 (ラテン文字と数字) だけである。日本語のように、このフォントにない文字は、Qt が PC ごとのフォントで描画するので、比較に使えない。日本語の名前の見た目は、PC のフォントで描画する `../decor_preview` の見本 (`--composite-sample`) で確認する。

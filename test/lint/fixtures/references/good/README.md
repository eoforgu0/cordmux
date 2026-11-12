# 架空のツリー (OK 例)

このツリーは references.py のセルフテストの OK 例で、どの行でも何も検出されないことを確かめる。

## 存在するパスと文書の中の参照

- 本体は `app/main.cpp` と `app/main.h` (`app/main.h/.cpp` とも書く)。フォルダーは `app/` と `app/sub/`。
- 使い方は `notes/guide.md`、ファイル名だけなら `guide.md`。同じ場所から `./notes/guide.md` とも書ける。
- 見出しを指す参照: notes/guide.md の「使い方」と、`notes/guide.md` の「使い方」。
- 文書の中では、自分の章を「2 章」や「付録 A」のように指してよい。
- 前のルールは `retired/rules.md` にある (ファイル名だけなら rules.md、フォルダーは `retired/`)。<!-- expect-absent: missing-path, missing-path, missing-path -->
- ルールの下書きの保存先は `retired/draft-<名前>.md` に決めてある。<!-- expect-absent: missing-path -->
- フォルダーごと削除した後の `gone/tool.py` も、--absent に残しておけば検出する。<!-- expect-absent: missing-path -->

## 外部のファイルとプレースホルダー

- 外部のツリーの中の場所 `libfoo/src/foo.c` と `include/foo/foo.h` は、リポジトリのツリーと照合しない。
- ビルドの出力 `app/build_x64/Release/app.exe`、ツールの出力 `tools/sender/dist/sender.js`、作業用のフォルダー `_work/out/report.md`。
- パターンとプレースホルダー: `app/*.cpp`、`app/{main,sub}.cpp`、`tools/gen-{lang}.txt`、`DIR/metrics.tsv`、`<保存先>/summary.tsv`、`app/xxx.h`。
- 並べた名前 `index.ts/native.ts` と、スキームを省いた URL `example.com/docs/old.md`、URL https://example.com/docs/old.md の中。
- ツールが出力する `summary.md` (tools/report.py の中に、その名前だけの文字列がある)。

## 番号の形ではない文字列

- 規格とライセンスの名前: UTF-8、SHA-256、CRC-32、ISO-8859-1、ECMA-262、GPL-3.0、LGPL-2.1、BSD-2-Clause、CC0-1.0、AC-3、E-AC-3、AV1、NV12、MD5、C17。
- 省略したパス `app/.../main.cpp` と `tools/.../out.js` は、省略した部分の前までをチェックする。
- キーの名前: Shift+F10、F10 キー、{F6}、Ctrl+F4。
- 列挙子の名前 `Tier::L1` と、正規表現の文字クラス `[A-Z0-9]`。
- GitHub の ID の形 (小文字と数) は番号の形ではない: example-user0。
- 架空の日付 2000-01-01、1999/12/31、2000年1月1日と、昔の年 1999-12-31。月の値の範囲 (01-12) と日の値の範囲 (01-31)。
- 比の 1/2 の高さ、件数の比 (gray 12/12)、暦の説明のうるう年の 2 月 29 日 (4 年ごと) は日付ではない。その 1 秒前は 2 月 28 日。
- 見出しを行の形で書いた参照: notes/guide.md の「# 使い方」。
- ビルドの定義の依存関係のバージョンの名前 2024-05-01 (buildspec.json にある) と、固定した上流のコミットの日付 2031-02-03 (vencord.lock.json にある)。
- 経緯の語の候補は、候補を出力するように指定したときだけ出力する: 前は手で書いていた。<!-- expect-candidate: history-word -->
- 英語の経緯の語も候補になる: Previously, the folder was fixed.<!-- expect-candidate: history-word -->

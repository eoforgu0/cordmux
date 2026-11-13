# 架空のツリー (OK 例)

このツリーは references.py のセルフテストの OK 例で、どの行でも何も検出されないことを確かめる。

## 存在するパスと文書の中の参照

- 本体は `app/main.cpp` と `app/main.h` (`app/main.h/.cpp` とも書く)。フォルダーは `app/` と `app/sub/`。
- 使い方は `notes/guide.md`、ファイル名だけなら `guide.md`。同じ場所から `./notes/guide.md` とも書ける。
- 見出しを指す参照: notes/guide.md の「使い方」と、`notes/guide.md` の「使い方」。
- 見出しを行の形で書いた参照: notes/guide.md の「# 使い方」。

## 外部のファイルとプレースホルダー

- 外部のツリーの中の場所 `libfoo/src/foo.c` と `include/foo/foo.h` は、リポジトリのツリーと照合しない。
- ビルドの出力 `app/build_x64/Release/app.exe`、ツールの出力 `tools/sender/dist/sender.js`、作業用のフォルダー `_work/out/report.md`。
- パターンとプレースホルダー: `app/*.cpp`、`app/{main,sub}.cpp`、`tools/gen-{lang}.txt`、`DIR/metrics.tsv`、`<保存先>/summary.tsv`、`app/xxx.h`。
- 省略したパス `app/.../main.cpp` と `tools/.../out.js` は、省略した部分の前までをチェックする。
- 並べた名前 `index.ts/native.ts` と、スキームを省いた URL `example.com/docs/old.md`、URL https://example.com/docs/old.md の中。
- ツールが出力する `summary.md` (tools/report.py の中に、その名前だけの文字列がある)。

# 架空のツリー (NG 例)

各行の末尾のコメントが、その行で検出されるはずの規則である。

- 本体の `app/missing.cpp` を参照する。<!-- expect: missing-path -->
- ルールは `old/rules.md` と、ファイル名だけの `rules.md` にある。<!-- expect: missing-path, missing-path -->
- 作業の表 `work/table.tsv` を参照する。<!-- expect: missing-path -->
- テストデータの `fixtures/sample.md` は、この文書の場所にもツリーの中にもない。<!-- expect: missing-path -->
- 結果は `app/old/run-<tag>/` に書く (プレースホルダーの前のフォルダーをチェックする)。<!-- expect: missing-path -->
- 保存先は <app/gone の下> に作る。<!-- expect: missing-path -->
- 省略した `app/lost/.../x.txt` も、省略した部分の前までをチェックする。<!-- expect: missing-path -->
- 呼び方は `notes/fixtures/sample.md` の「呼び方」にある (文書はあるが、その見出しがない)。<!-- expect: missing-heading -->
- この形は ZZ-12 と QX-7b で決めた。<!-- expect: record-id, record-id -->
- 見直しの ZZ1b-r5 と、並べた ZZ-1/ZZ-2 も検出する。<!-- expect: record-id, record-id, record-id -->
- 見直しの ZQ7 と XZ12b で変える。<!-- expect: record-id, record-id -->
- 先頭が 1 文字の Q-31 と X10-09、3 文字の ABC-12、4 文字の ABCD-3 も検出する。<!-- expect: record-id, record-id, record-id, record-id -->
- 括弧の中の [QA-31] と、見出しの後の 番号:QB-31 も検出する。<!-- expect: record-id, record-id -->
- 漢字の番号 案-3 と、カタカナの番号 カタ-3 も同じ。<!-- expect: record-id, record-id -->
- 2031-02-03 に決めた。<!-- expect: date -->
- 実際の日付として扱う最初の年の 2020-01-01 も検出する。<!-- expect: date -->
- 03-04 のテストで見つけた。<!-- expect: date -->
- 括弧の中の日付 (03-04) と、03-04 以前と、03/04 のテスト。<!-- expect: date, date, date -->
- 2031/02/03 と 2031年2月3日 と 3 月 4 日に決めた。<!-- expect: date, date, date -->
- 由来: ルールの文書。<!-- expect: history-label -->
- 採らなかった案は 退けた案 の見出しの下に書いていた。<!-- expect: history-label -->

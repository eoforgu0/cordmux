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

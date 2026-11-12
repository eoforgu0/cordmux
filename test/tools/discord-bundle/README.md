# test/tools/discord-bundle

Discord の Web 版のバンドルを取得する `fetch.mjs` と、保存したバンドルのモジュールに、依存箇所の一覧 (src-vencord/ledger.ts の `CONTACT_LEDGER`) の述語を照合する `scan.ts`。この 2 つを使うテストは src-vencord/tests/discord_contacts.test.ts にあり、.github/workflows/discord-contacts.yml が週に 1 回実行する。

## fetch.mjs

```
node test/tools/discord-bundle/fetch.mjs
```

stable (`https://discord.com/app`) と canary (`https://canary.discord.com/app`) のバンドルを取得し、fetch.mjs の `OUT_ROOT` の下の `<channel>/<build>/` に保存する。このフォルダーは git で管理しない。チャンクの探し方は fetch.mjs のコメントにある。途中で止めても、同じコマンドで続きから取得できる。

保存先には `files.json` (ファイルごとの名前、sha256、バイト数) を書き、`test/fixtures/discord-bundles/manifest.json` (git で管理する) に、1 つの build につき 1 つの項目 (`fetchedAt`、`channel`、`build`、`fileCount`、`totalBytes`、`digest`) を追加する。`digest` は、ファイルの名前で並べた「名前:sha256」の行を改行でつないだ文字列の sha256 である。404 以外の理由で取得に失敗したファイルがあれば、項目に `incomplete: true` を付け、終了コードを 1 にする。ファイルごとの sha256 を manifest.json に持たないのは、取得のたびに manifest.json が大きくなるためである。

## scan.ts

```
node test/tools/discord-bundle/scan.ts <buildDir> --props name1,name2
node test/tools/discord-bundle/scan.ts <buildDir> --code str1,str2
```

`<buildDir>` は 1 つの build のチャンク (`*.js`) が入ったフォルダー。テストからは `scanBuildDir` を呼ぶ。照合は、実行する前のモジュールの本文のテキストを使う近似である。近似の限界は scan.ts の `matchesPredicate` のコメントにある。

## 依存

- fetch.mjs は Node の標準のモジュールだけを使い、ブラウザを起動しない。
- scan.ts は、リポジトリのルートの devDependencies の typescript のパーサーを使う。

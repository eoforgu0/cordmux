# test/tools/dist-zip

配布用の zip を作るツールと、zip の中身をチェックするツール。リリースノートと、同梱するソフトウェアのソースのアーカイブもここで作る。CI (`.github/workflows/dist.yml`) もローカルも、同じ引数で実行する。どのツールにも `--selftest` があり、`.github/workflows/lint.yml` の `dist-tools-selftest` のジョブが実行する。Python 3 の標準ライブラリだけを使う。配布の流れは docs/release.md の「配布用の zip を作るワークフロー」と「リリースの手順」にある。

## ファイル

| ファイル | 役割 |
|---|---|
| `make_dist_zip.py` | 配布用の zip を作る。手順書の Markdown を `guide_to_text.py` で変換して入れ、ソースコードの入手先.txt の `{{source_url}}` にソースの URL を差し込む |
| `guide_to_text.py` | 手順書 (`guide/ja.md` と `guide/en.md`。Markdown) を zip に入れるテキスト (はじめに.txt と README-en.txt) にする |
| `check_dist_zip.py` | zip の中身をチェックする |
| `build_info.py` | ビルド ID と zip の名前を決め、タグを確認する。ほかに、`src-vencord/version.ts` を書き換え、obs-deps の bin のパスを出力する |
| `fetch_sources.py` | 同梱するソフトウェアのソースを取得し、コンポーネントごとのアーカイブにする |
| `make_release_notes.py` | リリースノートを作り、pdb を zip にする |
| `dist_common.py` | ほかのツールが共有する関数と定数 (下の「dist_common.py」) |
| `top_level_files.txt` | zip の最上位に `dist/` から置くファイルと、差し込みをするファイル (`fill-values`) の一覧。形は先頭のコメントにある。手順書は書かない (下の「guide_to_text.py」) |
| `release_notes_template.md` | リリースノートの形と、日本語と英語の決まった文 |

ツールは次の情報を自分では持たず、それを決めているファイルから読む。

- 同梱する FFmpeg の一覧: `src-obs/cmake/cordmux_ffmpeg_runtime.cmake` の `# ffmpeg-runtime-begin` と `# ffmpeg-runtime-end` の間
- バージョンと obs-deps: `src-obs/buildspec.json`
- Vencord の固定のコミットとタグ: `src-vencord/vencord.lock.json`
- バージョンごとの変更点: `CHANGELOG.md`
- cordmux-build.txt の名前とキー: `src-obs/install/build_mark.h`
- Vencord の dist に必要なファイル: `test/tools/vencord/check_dist.py`
- 手順書: `test/lint/install_guide_rules.txt` のセットごとの `target` (Markdown のパス)、`zip_name` (zip の中の名前)、`role` (節)。`test/lint/install_guide.py` の `load_rules` と `sections_by_heading` で読む (見出しの文字列を持たない)。Markdown の読み方も同じファイルにあり、手順書の lint と同じ読み方で読む

`dist/` のファイルと手順書の形 (Markdown の形、zip のテキストへの変換、差し込み、ソースコードの入手先.txt) は `dist/README.md` にある。

フォルダーの名前を `dist` にしないのは、`test/lint/` の lint の多くが `dist` という名前のフォルダーを見ない (`PRUNE_DIRS`) ので、ここの Python のファイルが、コメントと識別子の lint から外れてしまうため。

## `make_dist_zip.py`

```
    python test/tools/dist-zip/make_dist_zip.py
        --plugin-dir <cmake --install のインストール先>/cordmux
        --vencord-dist <Vencord のビルドの dist>
        --build-id <バージョン>+<短いコミット>
        --vencord-commit <固定した Vencord のコミット (40 桁)>
        --source-date-epoch <cordmux のコミットの時刻 (秒)>
        --out <フォルダー>/cordmux-<label>.zip
        [--cordmux-commit <cordmux のコミット (40 桁)>] [--dist-dir dist] [--guide-dir guide]
```

- `--plugin-dir` は、`cmake --install src-obs/build_x64 --prefix <インストール先> --config RelWithDebInfo` が作る `<インストール先>/cordmux` (`cordmux.dll`、FFmpeg の DLL と ffmpeg.exe、`data/locale/`。OBS 33 の置き方で、`bin/64bit` は作らない)。`cmake --install` には必ず `--prefix` を付ける。付けずに実行すると、インストールしてある OBS のプラグインのフォルダー (`%ALLUSERSPROFILE%/obs-studio/plugins`。`src-obs/cmake/windows/defaults.cmake`) に書き込むためである。`.pdb` は zip に入れない (CI は別の成果物としてアップロードする)。
- `--vencord-dist` から、Vencord の dist の 5 ファイルと、本体の JS の末尾 (`For license information please see <名前>`) が指す `.LEGAL.txt` を選び、cordmux-build.txt を加えて、`cordmux/data/vencord-src/` と `cordmux/data/vencord/` の両方に同じものを置く。`--vencord-commit` が `src-vencord/vencord.lock.json` の `commit` と違えば作らない。cordmux-build.txt の `vencord_tag` は lock の `tag` から取り、`v<数>.<数>.<数>` の形でなければ作らない。指された `.LEGAL.txt` がなければ作らない。
- `--dist-dir` (既定はリポジトリのルートの `dist/`) から、`top_level_files.txt` のファイルを zip の最上位に置く。`top_level_files.txt` の `fill-values` の行で指定したファイルは、`{{source_url}}` にこのバージョンのソースの URL を差し込んでから置く (zip の中だけで、`dist/` のファイルは変えない)。知らないキー、形の違う `{{` (括弧の中の空白や大文字、閉じていないもの) があれば作らない。
- `--guide-dir` (既定はリポジトリのルートの `guide/`) の手順書の Markdown (rules の `target` と同じ名前の `ja.md` と `en.md`) を、`guide_to_text.py` の規則でテキストにし、rules の `zip_name` (はじめに.txt と README-en.txt) の名前で最上位に置く。手順書の URL とソースの URL は、ソースコードの入手先.txt と同じ決め方 (label に `+` があればコミット、なければタグ)。Markdown がないか、使ってよい記法の外の書き方があれば作らない。`top_level_files.txt` が同じ名前のファイルを置くときも作らない。
- `--cordmux-commit` は、label に `+` があるとき (ソースの URL がコミットのツリーになるとき) に必要。40 桁の小文字の 16 進数で、`--build-id` の短いコミットで始まらなければ作らない (ローカルの `+dev` のような 16 進数でない値は始まりを見ない)。
- 最上位のフォルダーの名前は、`--out` のファイル名から `.zip` を除いたもの (`cordmux-<label>/`)。label は、ビルド ID の形 (`1.0.0+abc1234`、ローカルの `1.0.0+dev`) か、タグから `v` を除いた形 (`1.0.0`、`1.0.0-rc1`) だけを受け入れる。
- zip の中の時刻は `--source-date-epoch` (UTC)、並びは名前の順で、同じ入力からは同じバイト列になる (セルフテストで確認する)。
- `--build-id` は、CI が `src-vencord/version.ts` (Vesktop の中の cordmux のバージョン) にも同じ文字列で書き、cordmux.dll には CMake の定義 (`-DCORDMUX_BUILD_COMMIT=<短いコミット>`) で渡す。3 つが同じ文字列であることを、`check_dist_zip.py --expect-build-id` が確認する。

## `guide_to_text.py`

```
    python test/tools/dist-zip/guide_to_text.py <guide/ja.md> --label <label> [--cordmux-commit <40 桁>] [--out <テキスト>]
    python test/tools/dist-zip/guide_to_text.py --selftest
```

- 手順書の Markdown を、zip に入れるテキストにする (規則は `dist/README.md` の「zip の中の手順書」)。`make_dist_zip.py` は `convert` を import して zip を作るときに使う。このコマンドは、zip を作らずに変換の結果を見るためのもので、`--out` を省くと標準出力に書く。
- 手順書の言語 (案内の文と、インラインのコードを囲む引用符) は、rules のセットの `target` と同じパスならそのセットの `ini` から、なければファイル名 (`ja.md`、`en.md`) から決める。手順書の URL は、リポジトリの中のパス (`--repo-path`。既定は手順書のパスから作る) を指す。
- 使ってよい記法の外の書き方 (`install_guide.py` の記法のチェック) があれば、変換せずに 1 行の JSON (`ok` が false) で失敗する。
- `--selftest` は、規則ごとの例 (タイトルの後ろの案内、差し込みの印、見出し、リンクとコード、折り返さないこと、空行) と、実際の `guide/ja.md` と `guide/en.md` の変換 (Markdown の 1 行が 1 行になり、`##` の節が Markdown と同じ) を確かめる。

## `check_dist_zip.py`

```
    python test/tools/dist-zip/check_dist_zip.py <cordmux-<label>.zip> [--expect-build-id <ビルド ID>] [--formal-release]
    python test/tools/dist-zip/check_dist_zip.py --selftest [--material <実際の zip>]
```

結果は 1 行の JSON (`ok`、`problems` の `code` と `detail`、`guides` など) と終了コード 0 か 1。チェックする内容:

- ファイルの並び: 最上位のファイル、`cordmux/` の直下 (ちょうど cordmux.dll と FFmpeg の DLL と ffmpeg.exe)、locale の 2 つの ini、`data/vencord-src` と `data/vencord`。どれにも当てはまらないファイル、`.pdb`、`.map` は問題にする。
- `data/vencord-src` と `data/vencord` の名前と中身が同じで、どちらも Vencord の dist の 5 ファイル (先頭の `// Standalone: true` と `// Updater Disabled: true` の行も) と、本体の JS が指す `.LEGAL.txt` を持つ。
- cordmux-build.txt の 3 つのキー (この順) の値の形が正しく、`vencord_commit` と `vencord_tag` が `vencord.lock.json` と一致し、`data/vencord-src` と `data/vencord` で同じ値である。
- DLL と exe のほかに、トークンの形 (64 桁の 16 進数) の文字列がない。`licenses/` のほかに CR がない。
- ソースコードの入手先.txt に、必要なコンポーネントの行 (`FIXED_COMPONENTS` と、FFmpeg の configure の `--enable-lib*` と `--enable-zlib`) と、FFmpeg を GPL と書いた行がある (LGPL は数えない)。
- 最上位のファイルに `{{` が残っていない (`licenses/` と `cordmux/` の下は見ない)。手順書には `<!--` も残っていない。
- rules のセットごとの手順書 (`zip_name`) が最上位にある。
- `fill-values` の行で指定したファイルと手順書のソースの URL が、zip の名前の label と合っている。label に `+` がなければ同じタグのリリースの URL、あれば cordmux のコミットのツリーの URL (コミットは、ビルド ID の短いコミットで始まる 40 桁の 16 進数)。手順書の冒頭の、整形された手順書の URL (`blob/<タグかコミット>/guide/ja.md` など) も同じ規則で確かめる。
- 手順書の「互換性」の節が空でなく、placeholder がない (空白と改行を除いて比べる。変換で、日本語とインラインのコードの間の空白が除かれることがあるため)。
- `cordmux/` の直下のファイルだけを置いた一時フォルダーで `ffmpeg.exe -version` を実行し、起動できる。出力の configure は、上のソースコードの入手先.txt のコンポーネントの確認に使う。起動する前に PE の import をたどり、DLL が足りなければ起動しない (Windows のエラーのダイアログを表示しないため)。PATH は System32 と Windows のフォルダーだけにする。
- `--expect-build-id` (CI は必ず渡す): cordmux-build.txt、cordmux.dll、Vencord の本体の JS に、そのビルド ID がある。
- `--formal-release`: 正式版として公開できるか (どの手順書にも placeholder がない。`dist_common.formal_release_problems`)。main へのプッシュの zip には渡さない。

`--selftest` は、`make_dist_zip.py` のテストデータで zip を作り、正しいデータが通ることと、1 ファイルを抜いたり、混ぜたり、形を変えたりしたデータがそれぞれ失敗することを確認する。`--material` に実際の zip を渡すと、その zip が ffmpeg.exe の起動まで通り、`cordmux/` の直下から 1 ファイルずつ抜くとどれも失敗し、そのどれでも ffmpeg.exe を起動しないことも確認する (dist.yml は、作った zip でこれを実行する)。

## `build_info.py`

```
    python test/tools/dist-zip/build_info.py meta [--sha <40 桁>] [--ref <github.ref>]
    python test/tools/dist-zip/build_info.py obs-deps-bin
    python test/tools/dist-zip/build_info.py version-ts --build-id <ビルド ID> --file <version.ts>
```

`meta` は `version`、`commit`、`short_commit`、`build_id`、`label`、`zip_name`、`source_date_epoch`、`vencord_commit`、`is_tag` を `キー=値` の行で出力する (`$GITHUB_OUTPUT` にそのまま追記できる形)。`--ref` がタグなら、`v<数字 3 つ>[-接尾辞]` の数字の部分が `buildspec.json` の `version` と同じでなければ、終了コード 1 で終わる。zip の名前は、タグのときは `cordmux-<タグから v を除いた文字列>`、ほかのときは `cordmux-<build_id>` で、どちらも `cordmux-<label>`。`is_tag` (`true` か `false`) と `label` は、dist.yml の `meta` のジョブの outputs に同じ名前で出力し、`release` のジョブの条件と `--label` に使う。

## `fetch_sources.py`

```
    python test/tools/dist-zip/fetch_sources.py --fetch --label <label> --out <添付するファイルを置くフォルダー>
        [--source-offer <ソースコードの入手先.txt>] [--work <空の作業フォルダー>]
    python test/tools/dist-zip/fetch_sources.py --selftest
```

- 取得するものの一覧は、すでにある事実から作る: obs-deps のタグ (`buildspec.json` の `prebuilt` の `version` と `baseUrl`)、その中の `deps.ffmpeg/*.ps1` の先頭の `param` ブロック (`Name`、`Version`、`Uri`、`Hash`、`Targets`)、`src-obs/ws/ws_deps.cmake` の `FetchContent_Declare` (URL と SHA256)、`vencord.lock.json` の Vencord (中に `pnpm-lock.yaml` があること)。Vencord の取得元の URL だけは、`.github/workflows/vencord-plugin.yml` と同じ文字列をツールに置く (lock が URL を持たないため。変えるときは、リポジトリの中で同じ URL を書いた箇所を全部直す)。
- 取得のしかたは obs-deps の `utils.pwsh` と同じ規則 (obs-deps 2025-07-11 の `Setup-Dependency.ps1`、`Invoke-GitCheckout.ps1`、`Invoke-SafeWebRequest.ps1`)。
  - `Uri` が `.git` で終われば git のコンポーネント (`Hash` はコミット) で、履歴を 1 つだけ取得し (断られたら全部)、submodule も再帰で取得し、本体と submodule の `git archive` を 1 つの `tar.gz` にする (中の最上位は `<コンポーネント>-<バージョン>/`、gzip の時刻は 0)。
  - ほかは上流のアーカイブで、`curl --fail --location` でダウンロードし、obs-deps の `checksums/` (PowerShell の Clixml) か `ws_deps.cmake` の SHA256 と照合して、バイト列を変えずに置く。
  - ps1 が一部だけを checkout するコンポーネント (`-SparseArgs set,<パス>`) は、そのパスだけを `--filter=blob:none` で取得する (今は AMF の `amf/public/include`)。
  - obs-deps 自身は、タグの `git archive` にする。
- 同梱しないものは外す: `Targets` に `x64` がないもの (gas-preprocessor) と、`NOT_SHIPPED_SCRIPTS` の 2 つ (libpng と libdatachannel。理由はツールの中のコメント)。同梱するものの一覧ではなく、同梱しないものの一覧にするのは、obs-deps がコンポーネントを追加したときに、ソースコードの入手先.txt に行がないので失敗し、人が判断できるようにするため。
- 取得する前に、取得するコンポーネントと、ソースコードの入手先.txt のコンポーネントの行を 1 対 1 で突き合わせる。名前 (英数字だけにし、先頭の `lib` を外す)、コミット、`Uri` の順に探し、最初に見つかった段階で行が 1 つだけなら対応とする。対応した行には、そのコンポーネントのコミット (なければバージョン) が書いてあること。コンポーネントのない行、行のないコンポーネント、2 つある行、古い行があれば、何も取得せずに失敗する (`SOURCE_PARTS_WITHOUT_TARBALL` の 3 行は外す)。添付するファイルの名前の `<コンポーネント>` は、対応した行の最初の語。
- 結果は 1 行の JSON で、キーは `ok`、`out`、`obs_deps_commit`、`files`、`skipped` である。`files` はファイルごとの名前、コンポーネント、どこから読んだか、バイト数、SHA256 (git のコンポーネントはコミットも)、`skipped` は外した script と理由。失敗したときは `ok` が `false` で、`error` に理由がある。`--out` に同じ名前のファイルがあれば失敗にする (上書きしない)。`--work` は空のフォルダーだけを受け入れる (既定は一時フォルダーで、終わったら削除する)。
- `--selftest` はネットワークに接続しない。テスト用の obs-deps (注釈付きのタグ、ps1、Clixml)、git のコンポーネント (submodule 付き、一部だけを取得するもの)、アーカイブ (`file://`)、ws_deps、Vencord で全部を取得し、添付するファイルの名前と中身を確認する。照合の値の違い、`pnpm-lock.yaml` のない Vencord、上流にないコミット、行のないコンポーネント、コンポーネントのない行、古いコミットとバージョン、2 つある行、すでにあるファイル、読めない `param` と Clixml は、どれも失敗する。実際の `ws_deps.cmake` と Vencord が、現在のソースコードの入手先.txt の行と対応することも確認する。
- Windows では、`--selftest` が作る一時フォルダーのパスが長い (190 文字ほど) と、`git submodule add` が `'$GIT_DIR' too big` で失敗する。そのときは、TEMP を短いパスにして実行する。

## `make_release_notes.py`

```
    python test/tools/dist-zip/make_release_notes.py --label <label> --assets-dir <添付するファイルを置いたフォルダー>
        --out <リリースノート (.md)> [--pdb <cordmux.pdb>] [--expect-commit <40 桁>]
    python test/tools/dist-zip/make_release_notes.py --selftest
```

- 使うもの: `CHANGELOG.md` の、label の `x.y.z` の節 (`-rc` の接尾辞を除く。`1.0.0-rc1` も `1.0.0` も `## 1.0.0` を使う)、手順書の Markdown (rules の `target`。`guide/ja.md` と `guide/en.md`。言語は rules の `ini` の名前で決める) の「互換性」の節と「更新」の見出し、`--assets-dir` の `cordmux-<label>.zip` の中の 2 つの手順書 (zip にあることと、正式版の placeholder のチェック)、cordmux-build.txt、ソースコードの入手先.txt、`--assets-dir` に置いたファイルの名前、`release_notes_template.md`、`vencord.lock.json` の `tag` と、前のリリースのタグの上の同じファイルの `tag`。
- リリースノートの形: 日本語の部分と英語の部分。どちらも (1) このバージョンの変更点 (プレリリースのときは「このバージョンはリリース候補です。」を先頭に置き、変更履歴の節の `####` の見出しと項目を続ける)、(2) 互換性 (手順書の節の Markdown をそのまま載せる。リンクはこのバージョンのタグの GitHub の URL にし、`###` の見出しは `####` にする)、(3) 添付するファイル (zip と、GitHub で読める手順書の URL、今使っている人への「更新」の節の案内とそのリンク、pdb の zip、ソースのアーカイブの名前の先頭 `cordmux-<label>-src-`) とソースコードの入手先の 1 文、の順。
- `--pdb` を渡すと、先に `cordmux-<label>-pdb.zip` (中は `cordmux.pdb` 1 つ) を `--assets-dir` に作る。`--expect-commit` を渡すと、zip の cordmux-build.txt の `build_id` がその値の短いコミットで終わることと、`v<label>` のタグが checkout にあることを確認する。
- 問題が 1 つでもあればリリースノートを書かない (問題になるものは `compose` のコメントにある)。正式版 (接尾辞のない label) では、`dist_common.formal_release_problems` の問題も加える。結果は 1 行の JSON (`ok`、`out`、`title`、`prerelease`、`problems`)。
- 前のリリース: 現在の label の `x.y.z` より小さい `x.y.z` のタグのうち、最も新しいもの (同じ `x.y.z` なら正式版、次に接尾辞の新しいもの)。`git tag --list 'v*'` と `git show <タグ>:src-vencord/vencord.lock.json` で読む。前のリリースがなければ (最初のリリース)、同梱の Vencord のチェックはしない。

### `CHANGELOG.md` の形

`make_release_notes.py` が、変更履歴の全部の節の形と、label の節の項目をチェックする。書き方は docs/release.md の「変更履歴の書き方」にある。

```
# 変更履歴 (Changelog)

## 1.1.0

### ja

#### 新機能

- ...を追加しました。

#### 変更点

- ボタン [前の名前] の名前を [新しい名前] に変更しました。

#### 不具合の修正

- ...すると...になる問題を修正しました。

### en

#### New Features

- Added ...

#### Changes

- Renamed the [Old Name] button to [New Name]

#### Bug Fixes

- Fixed an issue where ...
```

- バージョンの節は新しい順に並べ、名前は `x.y.z` (`v` も `-rc` も付けない)。日付は書かない。
- 言語の節は `### ja` と `### en` の両方。見出しは言語ごとに 3 つ (「新機能」「変更点」「不具合の修正」と "New Features" "Changes" "Bug Fixes") をこの順に書き、項目のない見出しは書かない。
- 項目は `- ` で始まる 1 行に 1 つ (続きの行を書かない)。ja は「です・ます」で「。」で終える。en は過去形の動詞 (Added、Changed、Fixed、Removed、Renamed、Updated など) で始め、末尾に「.」を付けず、we、our、us を使わない。作業の中だけで通じる番号、コミット、関数の名前、不具合の報告先を書かない。
- 同梱の Vencord を更新したバージョンでは、ja と en の両方の項目に、Vencord と新しいタグ (`v1.15.11` の形) を書く。

## `dist_common.py`

ほかのツールが共有する関数と定数。値を決めているのは、ツールの間の約束だけである。

| 名前 | 何か | 使うツール |
|---|---|---|
| `FILL_TOKEN_RE` / `FILL_KEYS` / `FILL_OPEN` | 差し込みの書式 `{{キー}}` と、使えるキー (`source_url` だけ) | make_dist_zip、check_dist_zip、make_release_notes |
| `fill_text(text, values, where)` | `{{キー}}` を置き換える。知らないキー、値のないキー、形の違う `{{` は `DistError` | make_dist_zip |
| `source_url_for(label, cordmux_commit)` | このバージョンのソースの URL (label に `+` があればコミットのツリー) | make_dist_zip、make_release_notes、guide_to_text |
| `guide_url_for(label, cordmux_commit, path)` | リポジトリの中のファイル (手順書) の、このバージョンの GitHub の URL (label に `+` があればコミット) | make_dist_zip、make_release_notes、guide_to_text |
| `formal_release_problems(guide_texts, placeholders)` | 正式版のチェック (手順書に placeholder が残っていないか。空白と改行を除いて比べる。問題の文の並び。空なら公開してよい) | check_dist_zip (`--formal-release`)、make_release_notes |
| `REPO_URL` / `RELEASE_*_FORMAT` / `source_part_slug(name)` | リポジトリの URL と、リリースに添付するファイルの名前 | fetch_sources、make_release_notes |
| `parse_top_level` / `top_level_entries` / `fill_targets` | `top_level_files.txt` の読み方 | 全部 |
| `run_ffmpeg_version` / `run_ffmpeg_in_copy` | DLL がそろっているかを確認してから `ffmpeg.exe -version` を実行する | check_dist_zip |

正式版 (接尾辞のない label) かどうかは `make_release_notes.py` の `is_prerelease` が決め、`-rc` のほかの接尾辞もプレリリースにする (接尾辞のある label を、正式版として公開しないため)。`check_dist_zip.py --formal-release` は、呼び出す人が正式版と決めたときに、同じチェックを zip にする (ローカルで「この zip を正式版として出せるか」を確認するため)。

## リリースに添付するファイルの名前

| 添付するファイル | 名前 | 中身 |
|---|---|---|
| 配布用の zip | `cordmux-<label>.zip` | dist.yml の成果物のまま (`zip_name` に `.zip` を付けた名前) |
| pdb | `cordmux-<label>-pdb.zip` | `cordmux.pdb` を、名前を変えずに最上位に 1 つ (デバッガーは DLL の中に書かれた名前で探す) |
| 同梱するソフトウェアのソース | `cordmux-<label>-src-<コンポーネント>.<拡張子>` | git のコンポーネントは `git archive` の `tar.gz`。上流のアーカイブは、obs-deps の checksums で照合したバイト列のまま、元の拡張子 (`tar.gz`、`tar.xz`、`zip` など) で |

- `<コンポーネント>` は、ソースコードの入手先.txt のコンポーネントの行の最初の語を `source_part_slug()` で変えたもの (小文字にし、`[a-z0-9._-]` のほかの文字を `-` にする。例: `FFmpeg` は `ffmpeg`、`nlohmann/json` は `nlohmann-json`)。英数字と `._-` のほかの文字を使わないのは、GitHub が、添付するファイルの名前の特殊な文字を置き換えるため (GitHub の REST API の文書の "Release assets")。前に `-src-` を付けるのは、pdb の zip と見分けるため。
- 専用のアーカイブを添付しないコンポーネントの行は 3 つ: `cordmux` と `userplugin` (GitHub がタグに付ける Source code の zip と tar.gz)、`pnpm-lock.yaml` (Vencord のアーカイブの中)。
- SHA256SUMS は添付しない。

## `release` のジョブ

`.github/workflows/dist.yml` の `release` のジョブは、タグ (`v*`) をプッシュしたときだけ実行する (`needs.meta.outputs.is_tag == 'true' && github.event_name == 'push'`。手動の実行では作らない)。`needs: [meta, zip, lint]` で、`lint` のジョブは同じ条件で `lint.yml` を `workflow_call` で呼ぶ。`contents: write` の権限は、このジョブだけに付ける。dist.yml は、リリースを作る前の 5 つのチェックを (a) から (e) で呼ぶ。(c) は `lint` のジョブが成功したこと (`needs`)、(d) は正式版のチェック (`make_release_notes.py` の中) である。ステップの順は次のとおり (正確な内容は dist.yml にある)。

```
    (a) git ls-remote --tags origin の refs/tags/<tag>^{} (注釈付きのタグ)、無ければ refs/tags/<tag> の値が GITHUB_SHA と同じ
    (b) タグのコミットが main の履歴にある (git merge-base --is-ancestor)
    (e) 同じタグのリリースが公開済みなら失敗にする。途中で止まった実行が残した下書きは削除する
    zip と pdb の成果物をダウンロードする
    fetch_sources.py --fetch でソースのアーカイブを作る
    make_release_notes.py でリリースノートを作り、pdb を zip にする ((d) の正式版のチェックもこの中)
    gh release create でタグのリリースを作る (プレリリースのときは --prerelease --latest=false)
```

- (a) は、ランナーの中のタグではなく、リモートのタグをたどって比べる (checkout がタグを取得し直すので、ローカルのタグと `GITHUB_SHA` を比べても必ず同じになるため)。zip の中のビルド ID の短いコミットとの一致は `--expect-commit` が確認する。
- `gh release create` は、添付するファイルがあると、下書きを作り、ファイルを添付してから公開する。途中で止まると下書きが残り、次の実行の (e) が削除して作り直す。`--verify-tag` は、リモートにタグがなければ止める (gh が既定のブランチからタグを作る動作を止める)。`--latest=false` で、プレリリースを最新のリリースにしない (GitHub もプレリリースを最新のリリースにしないが、明示する)。
- 成果物の保持日数は、`checks.yml` が 1 日 (後で使わないため)、`dist.yml` が 7 日 (作った zip をダウンロードして確認する日数を残すため)。どちらも目安の値で、測って決めたものではない。
- タグをプッシュして実行するまで確認できないこと: タグのときの所要時間 (ジョブの上限は 90 分)、`persist-credentials: true` で (a) と (b) が origin に接続できること、`contents: write` だけで同じ実行の成果物をダウンロードできること、`fetch-depth: 0` の checkout にタグが入り、`make_release_notes.py` が前のリリースのタグを読めること。

## ローカルで CI と同じ zip を作る

`src-obs` で `cmake --preset windows-x64-local` と `cmake --build --preset windows-x64-local` を実行してから、次を実行する。プリセットは CONTRIBUTING.md の「ローカルの環境に合わせたプリセット」の CMakeUserPresets.json に書く。ローカルの cordmux.dll のバージョンは `<version>+dev` になる。

`--vencord-dist` には、`.LEGAL.txt` まで入った dist を渡す (5 ファイルだけをコピーした dist では、本体の JS が指す `vencordDesktopMain.js.LEGAL.txt` と `vencordDesktopRenderer.js.LEGAL.txt` がないので作れない)。CI の成果物 (`cordmux-vencord-dist`) か、同じ固定のコミットからビルドした dist を使う。

```
    cmake --install src-obs/build_x64 --prefix src-obs/release --config RelWithDebInfo
    python test/tools/dist-zip/make_dist_zip.py --plugin-dir src-obs/release/cordmux
        --vencord-dist <Vencord の dist> --build-id 1.0.0+dev --vencord-commit <vencord.lock.json の commit>
        --cordmux-commit <git rev-parse HEAD> --source-date-epoch <git log -1 --format=%ct>
        --out <作業フォルダー>/cordmux-1.0.0+dev.zip
    python test/tools/dist-zip/check_dist_zip.py <作業フォルダー>/cordmux-1.0.0+dev.zip
    python test/tools/dist-zip/check_dist_zip.py --selftest --material <作業フォルダー>/cordmux-1.0.0+dev.zip
```

バージョン (`1.0.0`) は `src-obs/buildspec.json` の `version` に合わせる。label が `1.0.0+dev` なので、`source_url` はコミットのツリーになる (`--out` を `cordmux-1.0.0-rc1.zip` にすれば、タグのリリースの URL で作れる)。その zip を正式版として出せるかは、`check_dist_zip.py <zip> --formal-release` で確認する (手順書に placeholder が残っていると失敗する)。

ローカルの Vencord の dist は `CORDMUX_BUILD_ID` が古いことがあり、そのときに `--expect-build-id` を渡すと、Vencord の本体の JS の文字列で失敗する (並びのチェックは、それとは別に通る)。

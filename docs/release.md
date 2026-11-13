# 配布とバージョン

この文書は、cordmux のバージョンの付け方、配布用の zip の作り方と中身、GitHub のリリースで公開する手順、依存関係を更新する手順を書く。リリースを作るときと、依存関係を更新するときに読む。配布用の zip とリリースを作るツールは `test/tools/dist-zip` にあり、CI とローカルの両方から同じ引数で実行できる。引数は `test/tools/dist-zip/README.md` にある。

## バージョン番号の付け方

バージョンは `<major>.<minor>.<patch>` の形式で、`src-obs/buildspec.json` の `version` の 1 か所で決める。

| 名前 | 例 | 使う所 |
|---|---|---|
| バージョン | `1.0.0` | `src-obs/buildspec.json` の `version`。CMake の `PLUGIN_VERSION` になる |
| ビルドの ID | `1.0.0+abc1234` | cordmux.dll、送信側、同梱の Vencord の cordmux-build.txt (下の 3 か所) |
| タグ | `v1.0.0`、`v1.0.0-rc1` | GitHub のタグとリリース |
| ラベル | `1.0.0`、`1.0.0-rc1`、`1.0.0+abc1234` | 配布用の zip の名前 (`cordmux-<ラベル>.zip`) |

- ビルドの ID は `<バージョン>+<短いコミット>` で、短いコミットは、コミットのハッシュの先頭 7 文字である。CI は同じ ID を、cordmux.dll (`-DCORDMUX_BUILD_COMMIT`)、Vencord にコピーした送信側の `version.ts` の `CORDMUX_BUILD_ID`、配布用の zip の `cordmux-build.txt` の `build_id` の 3 か所に入れる。受信側はこの 3 つを比べ、違えばドックに表示する。ローカルのビルドは `<バージョン>+dev` になる。
- `src-vencord/version.ts` には開発用の値 `<バージョン>+dev` を置く。`src-vencord/tests/version.test.ts` が、その数字の部分と `src-obs/buildspec.json` の `version` が同じことを確認する。
- タグは `v<バージョン>` (正式なリリース) か、`v<バージョン>-rc<n>` (リリース候補) にする。タグの数字の部分が `src-obs/buildspec.json` の `version` と違えば、`test/tools/dist-zip/build_info.py meta` が失敗する。
- `-rc1` のような接尾辞が付いたタグのリリースは、プレリリースとして公開し、最新のリリースにはしない。
- 配布用の zip のラベルは、タグのプッシュではタグから `v` を除いたもの、それ以外ではビルドの ID である。

## 配布用の zip を作るワークフロー

配布用の zip は `.github/workflows/dist.yml` が作る。main へのプッシュ、`v` で始まるタグのプッシュ、手動 (workflow_dispatch) で実行する。プルリクエストでは作らない。

| ジョブ | ランナー | すること |
|---|---|---|
| `meta` | ubuntu-24.04 | `build_info.py meta` で、ビルドの ID、ラベル、zip の名前を決め、コミットの時刻を読む。タグなら `version` との一致を確認する |
| `obs-plugin` | (呼び出し先のワークフロー) | `.github/workflows/obs-plugin.yml`。OBS のプラグインのビルド (`windows-ci-x64`)、`cmake --install`、C++ の単体テスト |
| `vencord-plugin` | (呼び出し先のワークフロー) | `.github/workflows/vencord-plugin.yml`。Vencord と送信側のビルド、ビルドのチェック、TypeScript の単体テスト |
| `zip` | windows-2022 | 配布用の zip を作ってチェックし、zip と pdb を別々の成果物としてアップロードする |
| `lint` | (呼び出し先のワークフロー) | タグのプッシュのときだけ、`.github/workflows/lint.yml` を呼ぶ |
| `release` | ubuntu-24.04 | タグのプッシュのときだけ、リリースを作って公開する (「リリースの手順」) |

`zip` のジョブの手順:

1. `test/tools/dist-zip/make_dist_zip.py` が、`cmake --install` の出力、Vencord のビルドの出力、`dist/` のファイルから、配布用の zip を作る。手順書 (`guide/ja.md` と `guide/en.md`) は `guide_to_text.py` でテキストに変換して入れ、先頭に同じバージョンの手順書の URL を、「ソースコードとライセンス」の節にこのバージョンのソースの場所を入れる。`ソースコードの入手先.txt` には、`{{source_url}}` にソースの場所を差し込む (コミットのツリーの場所には、`--cordmux-commit` で渡すコミットを使う)。
2. `test/tools/dist-zip/check_dist_zip.py` が、zip の中身をチェックする (「配布用の zip の中身」)。
3. 同じスクリプトを `--selftest --material` で実行し、作った zip からファイルを 1 つずつ抜いた zip がチェックで失敗することを確認する。
4. zip を `cordmux-<ラベル>` の成果物として、cordmux.pdb を `cordmux-<ラベル>-pdb` の成果物としてアップロードする (保持期間は 7 日)。

入力のファイルが同じなら同じ zip になるように、zip の中の時刻はコミットの時刻にし、ファイルを名前の順に並べ、属性を固定する。Vencord は `src-vencord/vencord.lock.json` の固定したコミットから、`pnpm install --frozen-lockfile` で依存関係を入れ、`SOURCE_DATE_EPOCH` を Vencord のコミットの時刻にしてビルドする。

`checks.yml` も、同じ `obs-plugin.yml` と `vencord-plugin.yml` を呼ぶ (成果物の保持期間は 1 日)。main へのプッシュでは `checks.yml` と `dist.yml` の両方が実行されるので、同じものを 2 回ビルドする。

## 配布用の zip の中身

```
cordmux-<ラベル>/
  はじめに.txt               日本語の手順書 (guide/ja.md から作る)
  README-en.txt              英語の手順書 (guide/en.md から作る)
  ソースコードの入手先.txt         ライセンスと、ソースコードの入手先
  licenses/                  同梱するソフトウェアのライセンスの本文
  cordmux/                   OBS のプラグインのフォルダー (OBS 33 の置き方。bin/64bit は作らない)
    cordmux.dll
    *.dll                    FFmpeg の DLL (11 個)
    ffmpeg.exe               録画ファイルの作成に使う
    data/locale/ja-JP.ini
    data/locale/en-US.ini
    data/vencord-src/        同梱の Vencord (元のファイル)
    data/vencord/            Vesktop が読む Vencord (data/vencord-src と同じ中身)
```

最上位に置く `dist/` のファイルの一覧は `test/tools/dist-zip/top_level_files.txt` にある。

FFmpeg のファイル:

| ファイル | 使うもの |
|---|---|
| `avcodec-61.dll`、`avutil-59.dll`、`swresample-5.dll`、`libx264-164.dll`、`zlib.dll` | cordmux.dll (avcodec と、それが読み込む DLL) |
| `avformat-61.dll`、`srt.dll`、`librist.dll` | cordmux.dll (avformat と、それが読み込む DLL) |
| `avdevice-61.dll`、`avfilter-10.dll`、`swscale-8.dll` | 録画ファイルの作成に使う ffmpeg.exe だけ |
| `ffmpeg.exe` | 録画ファイルの作成 (利用者は起動しない) |

- どのファイルも、obs-deps (`src-obs/buildspec.json` の `prebuilt` のバージョン) の bin フォルダーからコピーする。cordmux.dll はこの obs-deps の FFmpeg にリンクしているので、リンクしたものと同じファイルを配る。
- cordmux.dll が読み込むのは、上の表の 1 行目と 2 行目の 8 つである。avcodec-61.dll は、x264 でエンコードしなくても、読み込むときに libx264-164.dll と zlib.dll を必要とする。avformat-61.dll は srt.dll と librist.dll を必要とする。obs-deps の bin フォルダーにある datachannel.dll は、どれも必要としないので配らない。
- 一覧は `src-obs/cmake/cordmux_ffmpeg_runtime.cmake` の 1 か所にある。cordmux.dll のビルドの `cmake --install` が、この一覧のファイルを `cordmux` フォルダーの直下 (cordmux.dll と同じフォルダー) に置く。C++ の単体テストのビルドは、cordmux.dll が読み込む 8 つを実行ファイルの隣にコピーする。`check_dist_zip.py` は、この一覧と zip の `cordmux` フォルダーの直下を比べる。

同梱の Vencord のファイル (`data/vencord-src` と `data/vencord`):

| ファイル | 中身 |
|---|---|
| `vencordDesktopMain.js`、`vencordDesktopPreload.js`、`vencordDesktopRenderer.js`、`vencordDesktopRenderer.css` | Vencord と送信側をビルドしたもの |
| `package.json` | 中身が `{}` のファイル。置く理由は `test/tools/vencord/check_dist.py` の `REQUIRED_FILES` のコメントにある |
| `*.LEGAL.txt` | ビルドしたファイルの末尾が指す、ライセンスのコメントをまとめたファイル |
| `cordmux-build.txt` | ビルドの ID (`build_id`) と、Vencord のコミット (`vencord_commit`) とタグ (`vencord_tag`) |

- `data/vencord-src` は `data/vencord` のコピー元で、[再インストール] で `data/vencord` を元に戻すときに使う。Vesktop は `data/vencord` だけを読む。
- ソースマップ (`.map`) と pdb は、zip に入れない。pdb はリリースに別のファイルとして添付する。

`check_dist_zip.py` は、主に次のことをチェックする。

- 最上位のファイル、`cordmux` フォルダーの直下のファイル (cordmux.dll と、上の 11 個と、ffmpeg.exe)、言語のファイル、`data/vencord-src` と `data/vencord` がそろっていて、ほかのファイルがない
- `data/vencord-src` と `data/vencord` の名前と中身が同じで、Vesktop が読むファイル (`check_dist.py` と同じ条件) がある
- `cordmux-build.txt` の `vencord_commit` と `vencord_tag` が `src-vencord/vencord.lock.json` と一致し、`data/vencord-src` と `data/vencord` で同じ値である
- `.pdb` と `.map` がなく、トークンの形 (64 文字の 16 進数) の文字列が DLL と exe のほかにない
- `ソースコードの入手先.txt` に、同梱するソフトウェアの行と、FFmpeg を GPL で配ることを書いた行がある
- `cordmux` フォルダーの直下のファイルだけを置いたフォルダーから `ffmpeg.exe -version` を起動できる (出力の configure のオプションは、`ソースコードの入手先.txt` の行の確認に使う)
- 手順書と `ソースコードの入手先.txt` に `{{` が残っていなく、ソースの URL が zip の名前のラベルと合っている。手順書の「互換性」の節が空でない

## 同梱するソフトウェアのソースコードとライセンス

cordmux 自身のコードは Apache-2.0 で配る (リポジトリの `LICENSE`。zip には `licenses/cordmux/LICENSE` として入る)。配布用の zip には、GPL のソフトウェア (Vencord、FFmpeg など) が入る。そのため zip の全体としては、GPL の義務 (対応するソースの提示) を負う。

- obs-deps の FFmpeg は、GPL 第 3 版以降のビルド (`--enable-gpl --enable-version3`) である。
- `dist/ソースコードの入手先.txt` は、同梱するソフトウェアを 1 行に 1 つ、`- <名前> <バージョンかコミット> (<ライセンス>): <入手先>` の形で並べる。名前は空白を含まない。`check_dist_zip.py` と `fetch_sources.py` は、この形の行を読む。
- `ソースコードの入手先.txt` の `{{source_url}}` には、このバージョンの cordmux のソースの場所が入る。タグのプッシュでは同じタグのリリースのページ、それ以外ではそのコミットのファイルの一覧のページである (`test/tools/dist-zip/dist_common.py` の `source_url_for`)。
- `dist/licenses/` には、配るソフトウェアのライセンスの本文を置く。obs-deps の `licenses/` から、配るソフトウェアのフォルダーだけを、中身を変えずにコピーし、cordmux の Apache License 2.0 と、Vencord と FFmpeg の GPLv3 の本文を加える。ただし `zlib/README` は、zlib 1.3.1 の README のうち、ライセンスの部分だけを置く。obs-deps の同梱の README のバージョンが、配る DLL のバージョンと違うためである。
- 配らないソフトウェア (Qt、curl など、obs-deps のほかの部分) のライセンスは入れない。
- srt.dll と librist.dll には Mbed TLS が静的にリンクされている。FFmpeg の DLL には、aom、svt-av1、theora、lame、opus、vorbis、ogg、libvpx が静的にリンクされていて、nv-codec-headers と AMF のヘッダーも使われている。どれも `ソースコードの入手先.txt` と `licenses/` に含める。
- cordmux.dll には websocketpp、asio、nlohmann/json が入る (`src-obs/ws/ws_deps.cmake`)。

タグのプッシュで作るリリースには、同梱するソフトウェアのソースのコピーを 1 つの zip にまとめて添付する。`test/tools/dist-zip/fetch_sources.py` が、次のものから取得するソースの一覧を作り、`ソースコードの入手先.txt` の行と突き合わせてから取得する。片方にしかないソフトウェアがあるか、行のコミットかバージョンが一致しなければ、何も取得せずに失敗する。

- obs-deps のタグ (FFmpeg などに適用したパッチと、ビルドの手順を含む)
- obs-deps の `deps.ffmpeg/*.ps1` が取得するソフトウェア (git のコミットか、SHA256 を確認した tarball)
- `src-obs/ws/ws_deps.cmake` の URL と SHA256
- `src-vencord/vencord.lock.json` の Vencord のコミット (`pnpm-lock.yaml` を含む)

取得したソースは、ソフトウェアごとに 1 つのアーカイブ (git のリポジトリは `git archive` の tar.gz、ほかは配布元のアーカイブを照合したバイト列のまま) にし、それを `cordmux-<ラベル>-sources.zip` にまとめる。zip の中は `cordmux-<ラベル>-sources/` の下に、アーカイブ (`<名前>.<拡張子>`) と、`ソースコードの入手先.txt` の行ごとにそのソースのあるファイルを書いた `README.txt` を置く。アーカイブは圧縮し直さずに入れ、時刻 (コミットの時刻)、並び (名前の順)、属性を固定して、同じ入力からは同じ zip にする。添付を配布用の zip、pdb の zip、ソースをまとめた zip の 3 つにするのは、リリースのページで、どれをダウンロードすればよいかを名前を全部見て確かめなくて済むようにするためである。cordmux 自身のソースは、GitHub がリリースに付ける Source code の zip と tar.gz で配る。

## 対応している OBS のバージョン

配布用の zip とリリースに載せるバージョンの情報は、手順書の「必要なもの」に書く OBS の下限 (33。決め方は「OBS と obs-deps」にある) だけである。動作確認した環境 (Windows、Vesktop、OBS のバージョンの組み合わせや PC の機器) は載せない。保証や約束、対応の期限は書かず、それらをしないと断る文も書かない。

## 録画ファイルと動画編集ソフト

- どの動画編集ソフトで録画ファイルを読み込めるかは、手順書にもリリースにも書かない (動作確認した環境は載せない。「対応している OBS のバージョン」と同じ)。
- 録画ファイルの音声の形式 (設定の `record_output_pcm`) の既定値を fl32 (32 ビット浮動小数点) にしているのは、Vesktop から届いた音声を変換せずに書くためである。

## 変更履歴の書き方

変更履歴は、リポジトリのルートの `CHANGELOG.md` に書く。`test/tools/dist-zip/make_release_notes.py` が、リリースするバージョンの節を読んでリリースノートに入れる。形式が違えば、リリースを作らずに失敗する。

```
## 1.0.0

### ja

#### 新機能

- 最初のリリースです。

### en

#### New Features

- Released the first version
```

形式は次のとおりにする。

- バージョンの節 (`## x.y.z`) を新しい順に並べる。`-rc` の接尾辞は付けない。リリース候補も正式なリリースも、同じバージョンの節を使う。
- バージョンの節ごとに、`### ja` と `### en` の両方を書く。どちらの言語にも、項目のある見出しを 1 つ以上置く。
- 種類の見出しは、日本語が「新機能」「変更点」「不具合の修正」、英語が New Features、Changes、Bug Fixes で、この順に並べる。項目のない見出しは書かない。
- 項目は `- ` で始まる 1 行に書き、続きの行を書かない。

項目の文は次のとおりに書く。

- 1 つの項目には 1 つの変更を書き、利用者から見て何が変わったかを書く。
- 日本語の項目は「です・ます」で書き、「。」で終える。修正したことは「〜すると〜になる問題を修正しました。」、追加したことは「〜を追加しました。」、変えたことは「〜ようにしました。」か「〜に変更しました。」と書く。
- 英語の項目は、OBS のリリースノートと同じく過去形の動詞 (Added、Changed、Fixed、Removed など) で始め、ピリオドで終えず、we、our、us を使わない。不具合の修正は "Fixed an issue where ..." の形にし、where の後に、起きていたことを文で書く。
- 画面の名前は、画面の文字列のまま `[ ]` と「」で書く。
- 利用者が名前で探して操作する UI 要素の名前が別の語になったときは、前の名前と新しい名前を両方書く (「ボタン [...] の名前を [...] に変更しました。」/ "Renamed the [...] button to [...]")。対象の UI 要素は、ボタン、メニュー項目、チェックボックス、設定の入力欄と、OBS の一覧に表示されるソースの種類、シーン、ソース、フィルタと、ソースとフィルタのプロパティである。これらがドックからメニューやほかのウィンドウに移ったときは、移った先を書く。どちらも、利用者が覚えている名前や場所と合わなくなるためである。
- 名前の変化は、言語ごとに、その言語で実際に変わった文字列を書く。片方の言語の文字列だけが変わったときは、その言語にだけ項目を書く。表記、空白、活用、大文字と小文字だけの変化と、語を追加しただけか削除しただけの変化は書かない。メッセージ、状態のテキスト、ツールチップ、ダイアログの本文は、名前で探すものではないので、文言が変わっても名前の変化としては書かない。確認のダイアログのボタンも、ダイアログの問いの文が操作を示すので書かない。
- 日本語の項目は、手順書と同じ表記 (英数字と日本語の間に空白を入れない) にする (docs/ui_guidelines.md の「表記」)。変更履歴は利用者が読むためである。
- 内部の番号、コミットの ID、関数と識別子の名前、不具合の報告先を書かない (`make_release_notes.py` がチェックする)。
- 同梱の Vencord のタグが前のリリースと違うときは、日本語と英語の両方に、「Vencord」と新しいタグを含む項目を書く。Vencord を更新した理由もその項目に書く。
- リリース候補であることを示す文 (「このバージョンはリリース候補です。」/ `This is a release candidate.`) は、変更履歴に書かない。`make_release_notes.py` が、リリース候補のときだけ、リリースノートの変更点の前に入れる (`test/tools/dist-zip/release_notes_template.md`)。

## リリースの手順

1. `src-obs/buildspec.json` の `version` と、`src-vencord/version.ts` の `CORDMUX_BUILD_ID` の数字の部分 (`<バージョン>+dev`) を、新しいバージョンにする。
2. `CHANGELOG.md` に、新しいバージョンの節を書く (「変更履歴の書き方」)。
3. docs/testing.md の「確認する項目」を順に実施する。
4. main にプッシュし、`lint.yml`、`checks.yml`、`dist.yml` が成功することを確認する。`dist.yml` の成果物の zip で、インストールから更新までを確認してもよい (docs/testing.md の「実行計画と判定スクリプト」の `install-roundtrip`)。
5. タグ (`v<バージョン>` か `v<バージョン>-rc<n>`) を 1 つだけプッシュする。一度に 4 つ以上のタグをプッシュすると、GitHub はタグの `push` のイベントを作らない。

タグのプッシュでは、`dist.yml` の `release` のジョブが、`zip` と `lint` のジョブが成功した後に、次の順に進む。書き込みの権限 (`contents: write`) を持つのは、このジョブだけである。

1. GitHub のタグが指すコミットが、この実行のコミット (`GITHUB_SHA`) と同じかを確認する。
2. そのコミットが main にあるかを確認する。
3. 同じタグのリリースが公開済みなら失敗し、公開したリリースの添付ファイルは差し替えない。途中で失敗した実行が残した下書きのリリースは削除する。
4. `fetch_sources.py` で、同梱するソフトウェアのソースのコピーを取得し、1 つの zip にまとめる。
5. `make_release_notes.py` で、リリースノートを作り、pdb を zip にする。リリースノートは、`CHANGELOG.md` のそのバージョンの節と、手順書 (`guide/ja.md` と `guide/en.md`) の「互換性」の節から作る (テンプレートは `test/tools/dist-zip/release_notes_template.md`)。
6. `gh release create` でリリースを作り、添付ファイルを付けて公開する。

リリースの添付ファイル:

| ファイル | 中身 |
|---|---|
| `cordmux-<ラベル>.zip` | 配布用の zip |
| `cordmux-<ラベル>-pdb.zip` | cordmux.pdb (OBS がクラッシュしたときに原因を調べるため) |
| `cordmux-<ラベル>-sources.zip` | 同梱するソフトウェアのソースのコピー (ソフトウェアごとのアーカイブと、`ソースコードの入手先.txt` の行との対応を書いた `README.txt`) |

添付はこの 3 つだけにする。

`make_release_notes.py` は、次のときにリリースを作らずに失敗する。

- 手順書の「互換性」の節がない、空、仮の文字列 (日本語は `(確認してから書きます)`、英語は `(to be written after testing)`) を含む。この節はリリースノートに使うので、リリース候補でも失敗にする
- `CHANGELOG.md` の形式が違う、またはそのバージョンの節がない
- 添付ファイルが上の 3 つでない、zip の `cordmux-build.txt` のビルドの ID がラベルのバージョンやタグのコミットと合わない
- ソースをまとめた zip に、配布用の zip の `ソースコードの入手先.txt` の行のソースがそろっていない (行ごとのアーカイブと、`README.txt` の行)
- 正式なリリースで、手順書のどこかに仮の文字列が残っている

## 依存関係を更新する手順

依存関係は自動では更新しない。配布用の zip に入るもの (obs-deps の FFmpeg、Vencord) と、OBS、Vesktop を更新したら、実機テストで確認し、`CHANGELOG.md` を修正する。

### OBS と obs-deps

`src-obs/buildspec.json` が、ビルドに使う OBS のソース (`obs-studio`)、obs-deps (`prebuilt`)、Qt6 (`qt6`) のバージョンと、プラットフォームごとの SHA256 を持つ。CI の `src-obs/.deps` のキャッシュのキーは、このファイルのハッシュである。

- `obs-studio` のバージョンは 31.1.1 のまま変えない。対応する OBS の下限 (33) は、OBS がプラグインの置き方 (`<plugins>\<名前>\<名前>.dll`) を読む最初のバージョンで決まり、ビルドに使う OBS のバージョンとは別に決まる。古い libobs でビルドしたプラグインは新しい OBS でも読み込める (31.1.1 の libobs でビルドした cordmux が OBS 33 で動くことは、実機で確認している) が、新しい libobs でビルドしたプラグインは古い OBS で読み込めないことがある。そのため、ビルドに使うバージョンは対応する OBS 以下にする。33 に上げても利用者に得がなく、OBS の依存一式の取り直しとビルドのやり直しが要る。
- 対応する OBS の下限を上げるときは、手順書の「必要なもの」と「困ったとき」の「ドックが表示されない」も一緒に修正する (docs/spec.md の「対応する環境とバージョン」)。
- obs-deps を更新するときは、次も確認して直す。
  - FFmpeg の DLL の名前 (メジャーバージョンを含む) と、それぞれが読み込む DLL。`src-obs/cmake/cordmux_ffmpeg_runtime.cmake` の一覧を修正する。読み込む DLL は、DLL のインポートテーブル (`dumpbin /dependents` など) で確認する。
  - `src-obs/tests/test_decode.cpp` の、リンクした FFmpeg のバージョンを確認するテスト。
  - `dist/ソースコードの入手先.txt` の obs-deps と、FFmpeg とその依存の行 (バージョンとコミットは obs-deps の `deps.ffmpeg/*.ps1` にある) と、`dist/licenses/` の本文。
  - websocketpp、asio、nlohmann/json のバージョン (下の「そのほかの依存関係」)。
- `check_dist_zip.py` は、`cordmux` フォルダーの直下の一覧と、`ffmpeg.exe -version` が起動できることと、`ソースコードの入手先.txt` の行をチェックするので、修正し忘れの多くは `dist.yml` で失敗する。

### Vencord と Vesktop

同梱の Vencord のバージョンは、`src-vencord/vencord.lock.json` の 1 か所で決める (`commit`、`tag`、`date`、`reason`)。CI の Vencord のビルドと、`fetch_sources.py` は、Vencord のコミットをこのファイルから読む。

Vencord を更新するのは、次の 2 つのときだけである。

- 新しい Vesktop で動かすために必要なとき
- Discord の内部が変わって依存箇所が読めなくなり、Vencord の修正が必要なとき

更新するときの手順:

1. `src-vencord/vencord.lock.json` の `commit` を、新しいタグが指すコミットにし、`tag`、`date`、`reason` を修正する。
2. `dist/ソースコードの入手先.txt` の Vencord と pnpm-lock.yaml の行を修正する。
3. CI の `vencord-plugin.yml` が成功することを確認する (Vencord のビルド、`pnpm test:ts`、`check_dist.py`)。pnpm のバージョンは、Vencord の `package.json` の `packageManager` に合わせる。このバージョンは、このワークフローと、CONTRIBUTING.md と docs/testing.md にあるローカルの手順に書いてある。
4. cordmux が使う Vencord の API (webpack のモジュールの検索、FluxDispatcher とストア、`definePlugin` など) が変わっていないかを、Vencord の変更の内容で確認する。そのうえで、`discord-contacts.yml` と、実際の通話を使う実機テストを実行する。
5. `CHANGELOG.md` に、「Vencord」と新しいタグを含む項目を日本語と英語で書く (「変更履歴の書き方」)。

Vesktop:

- テスト用の送信側 (`test/tools/harness/package.json`) の Electron を、Vesktop と同じバージョンにする。WebCodecs のエンコーダの動作が Chromium のバージョンで変わりうるので、テスト用の送信側の測定を Vesktop と同じ条件にするためである。
- 新しい Vesktop では、テスト用の送信側の tests モードで、ハードウェアのエンコーダを同時に作れる数を測り直し、実際の通話を使う実機テストで確認する。
- cordmux は Vesktop の state.json の `vencordDir` を書き換えて同梱の Vencord を読ませる (`src-obs/install`)。Vesktop を更新するときは、この形が変わっていないかを確認する。

### そのほかの依存関係

| 依存関係 | バージョンを決めている所 | 更新するときに確認すること |
|---|---|---|
| websocketpp、asio、nlohmann/json | `src-obs/ws/ws_deps.cmake` (コミットの zip の URL と SHA256) | obs-deps が使うのと同じバージョンにする (obs-deps の `deps.windows` のスクリプトにある)。`ソースコードの入手先.txt` と `licenses/` も修正する |
| doctest | `src-obs/tests/doctest.h` (外部からコピーしたファイル) | ファイルを置き換えるだけで、中身を変えない (`test/lint/vendored.txt` に入っている) |
| obs-plugintemplate のファイル | `src-obs/cmake`、`src-obs/build-aux` など (外部からコピーしたファイル) | コピーしたファイルだけを置き換える。名前が `cordmux_` で始まる `.cmake` は cordmux のファイルで、`resource.rc.in` は変更したコピーである (CODESTYLE.md の「SPDX の行」) |
| TypeScript、vitest、@types/node | `package.json` と `package-lock.json` | `npm test` と `npm run typecheck` |
| テスト用の送信側の Electron、esbuild、ws | `test/tools/harness/package.json` と `package-lock.json` | Electron は Vesktop と同じバージョンにする (上の「Vencord と Vesktop」) |
| GitHub Actions のアクション、Node.js、Python | `.github/workflows/*.yml` | CI の全部のワークフロー |

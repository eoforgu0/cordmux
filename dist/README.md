# dist

配布用の zip に入れるファイルの元を置くフォルダー。この README.md は zip に入れない。zip を作るツールとチェックするツールは `test/tools/dist-zip/` にあり、使い方は `test/tools/dist-zip/README.md` にある。配布の流れは docs/release.md の「配布用の zip の中身」と「リリースの手順」にある。

ここに書いたファイルの名前と形は、ほかのファイルが読む。変えるときは、読む側 (`.github/workflows/` のワークフロー、`test/tools/dist-zip/` と `test/tools/obs-run/` のツール、`test/lint/install_guide_rules.txt`、ini の文字列) を一緒に修正する。

## ファイル

| ファイル | 中身 | zip の中の場所 |
|---|---|---|
| `ソースコードの入手先.txt` | 同梱するソフトウェアのライセンスとソースコードの入手先 (下の「ソースコードの入手先.txt の形」) | `cordmux-<label>/ソースコードの入手先.txt` (差し込んだ後) |
| `licenses/` | 同梱するソフトウェアのライセンスの本文 (下の「licenses/ の中身」) | `cordmux-<label>/licenses/` |
| (`guide/ja.md`) | 日本語の手順書 (Markdown。下の「手順書の形」) | `cordmux-<label>/はじめに.txt` (zip を作るときに変換したテキスト) |
| (`guide/en.md`) | 英語の手順書。節の並びと中身は `guide/ja.md` と 1 対 1 で対応する | `cordmux-<label>/README-en.txt` (同じ) |

手順書の正はリポジトリのルートの `guide/` の Markdown で、このフォルダーには置かない。GitHub で見出しと目次の付いた形で読め、zip の中のテキストは `make_dist_zip.py` が zip を作るたびに Markdown から作る (`test/tools/dist-zip/guide_to_text.py`)。zip の中の名前 (`はじめに.txt` と `README-en.txt`) は、画面の文言が指す名前で、`test/lint/install_guide_rules.txt` の `zip_name:` が決める。

リポジトリのルートの `LICENSE` (cordmux の Apache License 2.0) は、zip には `licenses/cordmux/LICENSE` (同じ本文) として入る。zip の最上位に `dist/` から置くファイルの一覧は `test/tools/dist-zip/top_level_files.txt` が決める。

test/lint/ の lint の多くは、`dist` という名前のフォルダーを見ない。このフォルダーには文書とデータだけを置くので、問題にならない。

## 手順書の形

### Markdown の形 (`guide/ja.md` と `guide/en.md`)

- 節の見出しと並び、タイトル、role (ツールが節を探すための名前)、手順の部分の範囲は `test/lint/install_guide_rules.txt` が決め、`test/lint/install_guide.py` がチェックする。書き方は docs/ui_guidelines.md の「手順書の書き方」にある。
- UTF-8 (BOM なし) と LF で書く。手で折り返さない。1 段落を 1 行で書き、箇条書きと番号付きの項目も 1 項目を 1 行で書く (GitHub の表示では、日本語の段落の途中の改行が空白になるブラウザーがある。幅は表示する側が決める)。
- 使ってよい記法 (これ以外は lint が失敗にする):
  - 見出し: `# ` (1 行目に 1 つだけ。タイトル)、`## ` (節)、`### ` (節の中の項目)。「インストール」の手順は `### 1. ` からの見出しで書き、「困ったとき」の項目は `### ` の見出しで書く (画面の文言と、手順の中の「うまくいかないときは」のリンクが、項目の見出しを指す)。
  - 段落。箇条書き `- ` (下に 2 つの空白で字下げした `- ` を 1 段置いてよい)。番号付きの項目 `1. ` (入れ子にしない。下に 3 つの空白で字下げした `- ` を 1 段置いてよい)。番号は見出しごとに 1 から順に書く。
  - インラインのコード `` `...` `` (パスとファイル名だけ)。リンク `[テキスト](#アンカー)` (同じファイルの見出しへ)、`[テキスト](https://...)`、`[テキスト](../dist/...)` (リポジトリの中のファイルへの相対リンク)。「」の中にリンクを置いてよい。アンカーは GitHub が見出しから作るもの (小文字にし、文字・数字・空白・`-`・`_` のほかを除き、空白を `-` にする)。
  - HTML のコメントは、zip 用の差し込みの印の行 `<!-- zip:source_url -->` だけ (1 つ。「ソースコードとライセンス」に置く)。
- 使わない記法: 表、画像、太字と斜体、引用 `>`、コードブロック、HTML、`{{...}}` の差し込み (GitHub の表示にそのまま出る)。
- `[ ]` は押す・選ぶ・入力する UI 要素の名前で、cordmux の UI 要素は ini の文字列のまま (末尾の「...」は省く) 書く。ボタンのほかの UI 要素は、後ろに種類の語を書く ([録画] ページ、[表示] タブ)。ほかのアプリの UI 要素は、そのアプリの文字列のまま書く ([ユーザーIDをコピー])。lint は、cordmux の UI 要素の名前を ini と、ほかのアプリの UI 要素の名前を rules の `outside_button:` と照合する。画面に表示される文は「」で引用し、手順の部分の「」の中は lint が ini と照合する。
- 画面の文言 (ini の値) が手順書を指す所 (「はじめに.txt」の「インストール」の手順2、"Call voices are heard twice" in README-en.txt) は、lint が手順書の `##` と `###` の見出しと、`### N. ` の手順の見出しと照合する (rules の `ini_ref:`)。見出しの名前を変えるときは、ini の文字列も一緒に修正する。
- 2 つの節に role がある: 互換性 (`maintenance`)、更新 (`update`)。zip のチェックとリリースノートのツールは、見出しの文字列を持たず、role で節を探す。
- ini の `cordmux.dialog.uninstall_done_loose.text` は、zip の `cordmux` フォルダーの名前を含む。zip の中のフォルダーの名前を変えるときは、この文字列も修正する。

### zip の中の手順書

`test/tools/dist-zip/guide_to_text.py` が、次の規則で Markdown をテキストにする (`make_dist_zip.py` が zip を作るときに使う)。

1. タイトル (`# ` の行) の後ろに、空行と、整形された手順書の場所の案内の 1 文 (ja「この説明書は、見出しと目次の付いた形で次の場所でも読めます。」、en "You can also read this guide, with headings and a table of contents, at:") と、その URL の行を置く。URL は `source_url` と同じ決め方 (label に `+` がなければタグ、あればコミット) で、`guide/ja.md` などを指す (`https://github.com/eoforgu0/cordmux/blob/<タグかコミット>/guide/ja.md`)。
2. `<!-- zip:source_url -->` の行を、ja「このバージョンのソースコード: <source_url>」、en "Source code of this version: <source_url>" の 1 行の段落にする。
3. 見出しの `#` の印はそのまま残す (テキストでも見出しと分かり、GitHub の表示と同じ構成で読める)。見出しの行は折り返さない。
4. リンクのうち、同じファイルの見出しへのリンクと相対リンクはテキストだけにし、`[テキスト](https://...)` は `テキスト (https://...)` にする (テキストと URL が同じなら URL だけ)。インラインのコードは、ja は「」、en は "" で囲む (ja では、コードと日本語の文字の間の空白を 1 つ除く)。
5. 決まった桁で改行を入れず、Markdown の 1 行を 1 行にする (入れ子の項目は、Markdown と同じ字下げを残す)。表示のときの折り返しは、メモ帳などの読む側の設定に任せる。
6. 見出し、段落、リストの前後の空行を 1 行にそろえる (続けて書いた項目の間と、空行を挟まずに続く段落の行の間には空行を入れない)。末尾は改行 1 つ。
7. 使ってよい記法の外の書き方 (lint の記法のチェックと同じ) があるときと、変換したテキストに `{{` か `<!--` が残るときは、変換せずに失敗する。

### 差し込み

- `dist/` のファイルの差し込みの書式は `{{キー}}` (キーは `[a-z0-9_]+`。括弧の中に空白を入れない)。使えるキーは `source_url` だけ。
- 差し込みをするファイルは、`top_level_files.txt` の `fill-values` の行で指定する: `ソースコードの入手先.txt` (ja)。手順書は差し込みの書式を使わず、変換するときに `<!-- zip:source_url -->` の行を置き換える (上の 2)。
- `source_url` は、zip の名前の label に `+` がなければ同じタグのリリース (`https://github.com/eoforgu0/cordmux/releases/tag/v<label>`)、あればコミットのツリー (`https://github.com/eoforgu0/cordmux/tree/<cordmux のコミット>`)。コミットは `make_dist_zip.py` の `--cordmux-commit` で渡す。
- 知らないキーがあれば `make_dist_zip.py` が zip を作らない。zip の最上位のファイルに `{{` が残っていれば (手順書は `<!--` も) `check_dist_zip.py` が失敗にする (`licenses/` と `cordmux/` の下は見ない。上流のライセンスの本文と Vencord の本体の JS は `{{` を含むことがある)。

### 対応している OBS のバージョン

- 手順書に書くバージョンの情報は、「必要なもの」の OBS の下限 (33 以上) だけ。動作確認した環境は載せず、保証や約束、対応の期限も書かない。下限の決め方と、ビルドに使う OBS のバージョン (31.1.1) を上げない理由は、docs/release.md の「対応している OBS のバージョン」と「OBS と obs-deps」にある。
- OBS の下限を変えるときは、手順書の「必要なもの」と「困ったとき」の「ドックが表示されない」のバージョンを一緒に修正する。

### placeholder と定型の文

- placeholder (後で書く所に置く仮の文字列) は ja `(確認してから書きます)`、en `(to be written after testing)` (括弧は ASCII)。rules の `placeholder:` が決める。手順書の lint は placeholder を許すが、接尾辞のないタグ (正式版) のリリースは、1 つでも残っていれば作らない。「互換性」の節はリリースノートにそのまま載るので、この節には placeholder を置かない (`-rc` のタグでもリリースを作らない)。
- 実機テストで確認してから書く所ができたときは、リリースノートに載らない節に置く。`-rc` のタグのリリースは作れるが、正式版は作れない。
- 定型の文は rules の `contains:` の行が決め、手順書にはその文字列のまま書く (空白の位置は自由)。ini からコピーする文は rules の `copied:` の行が決める。

### 手順書の記述の根拠

- インストールの手順 2: 展開した `cordmux` フォルダーを `<plugins>` フォルダーの中へ移動する。`<plugins>` は、インストール版の OBS では `C:\ProgramData\obs-studio\plugins`、ポータブル版の OBS では OBS のフォルダーの `plugins` (OBS 33 の frontend/plugin-manager の PluginModuleLoader_Windows.cpp)。OBS 33 は `<plugins>\<名前>\<名前>.dll` と `<plugins>\<名前>\data` を読む。同梱の FFmpeg の DLL は `cordmux` フォルダーの中に並び、OBS 自身の DLL とは同じフォルダーに並ばない。OBS はフォルダーの中の cordmux.dll だけをモジュールとして読む (同梱の DLL は読まない)。
- OBS 31 と 32 は、旧方式 (`bin\64bit`、ポータブル版は `obs-plugins\64bit`) の置き方しか読まない。この置き方の cordmux は OBS 31 と 32 では読み込まれない (ログに何も出ない)。そのため、手順書の「ドックが表示されない」に OBS のバージョンの確認を書く。
- 「cordmux」のソースを配信のシーンに置く手順 (インストールの手順 3): 参加者ごとの声と画面共有の音は、映像を描くソースの「cordmux」からだけ出る。そのため「cordmux」を入れていないシーンでは、映像も声も画面共有の音も配信に出ない。[+] で「cordmux」を選んだ後の [新規作成] は、「ソースを追加」のウィンドウのボタン (`outside_button:` の行のコメントにある)。追加した後のプロパティのウィンドウは、OBS 32 以降はいつも開く (`OBSBasicSourceSelect::createNew`)。
- サブアカウントの招待 (インストールの手順 4): メインアカウントで招待リンクを作り、Vesktop にサブアカウントでログインしてその招待リンクからサーバーに参加する。Discord の招待の画面の項目の名前は書かない (日本語と英語の定訳を、Discord のヘルプで確認できなかったため)。
- ユーザー ID の取り方: Discord のヘルプ「ユーザー/サーバー/メッセージIDはどこで見つけられる？」の、ユーザー設定の詳細設定にある開発者モードと、[ユーザーIDをコピー]。
- Vesktop の通知領域のアイコンのメニューの項目 ([Restart]、[Quit]、[Repair Vencord]) は、Vesktop 1.6.7 の `resources\app.asar` と 1.6.9 のソースを読んで書いた。Vesktop 1.6.7 と 1.6.9 はウィンドウを閉じても終了せずに通知領域に残るので、終了と再起動はこのメニューで書く。項目ごとの説明は `test/lint/install_guide_rules.txt` の `outside_button:` の行のコメントにある。
- 「互換性」の節は、リリースノートが role で取り出してそのまま載せるので、位置でほかの節を指す語 (「上の」「この文書の」) を使わない。

## `ソースコードの入手先.txt` の形

zip のチェック (`check_dist_zip.py`) とソースを取得するツール (`fetch_sources.py`) が読めるように、同梱するコンポーネントの行の形を決めている。

```
    - <コンポーネントの名前> <バージョンかコミット> (<ライセンス>): <入手先>
```

- コンポーネントの名前は空白を含まない。`check_dist_zip.py` は最初の語を、`fetch_sources.py` はコミットかバージョンの文字列を読む。
- FFmpeg のコンポーネントの行のライセンスは `GPL-3.0-or-later` で、加えて、同梱の FFmpeg が GPL 第 3 版以降のビルドであることを書いた 1 行がある (zip のチェックが読む)。
- コンポーネントの行の括弧の中の説明 (「このzipファイルと同じバージョンのタグ」など) は、英語と日本語を ` / ` で並べる (英語が先)。
- 先頭に日本語と英語の 4 行ずつがある。どの行も `- ` で始めない (コンポーネントの行として読まれるため)。
  - 全体のライセンス (ライセンスの本文の場所を含む)
  - FFmpeg の GPL の行
  - このバージョンのソース `{{source_url}}` (手順書の「ソースコードとライセンス」と同じ値)
  - リリースではコンポーネントごとのソースを同じリリースに添付すること。この行に「リリースとして公開するバージョンでは」と書くのは、main へのプッシュで作る zip は `source_url` がコミットのツリーで、リリースがないため。
- obs-deps の一覧に名前がないコンポーネントの行もある。`ogg` は vorbis と theora の中に入っている (obs-deps の `deps.ffmpeg/30-libogg.ps1`)。`nv-codec-headers` と `AMF` は、avcodec-61.dll のビルドに使われたヘッダーである (DLL の中に `nvEncodeAPI64.dll` と `amfrt64.dll` の名前がある)。バージョンとコミットは、obs-deps 2025-07-11 の `deps.ffmpeg/*.ps1` から取った。
- `mbedtls` は srt.dll に静的に入っている (下の「licenses/ の中身」の確認の方法)。librist の行のライセンスには、librist の中に入っている Mbed TLS 2.26.0 (librist のソースの `contrib/mbedtls`。入手先は librist と同じ) の Apache-2.0 を並べた。
- `srt` と `vpx` のコミット: obs-deps 2025-07-11 の `deps.ffmpeg/60-srt.ps1` と `30-libvpx.ps1` は、タグの zip (`v1.5.2.zip` と `v1.14.1.zip`) を checksums のファイルで照合して取得し、コミットを書いていない。行には、そのタグが指すコミットを書いた: srt `v1.5.2` (軽量のタグ) は `9448e26fcd7602098b4bf9cd7fe535136e89e10b`、libvpx `v1.14.1` (注釈付きのタグ `3688b17187d40a71f9a7de4bfac92201f3e4a533`) は `12f3a2ac603e8f10742105519e0cd03c3b8f71dd`。

## `licenses/` の中身

obs-deps 2025-07-11 に入っている `licenses/<コンポーネント>/` を、配るコンポーネントの分だけフォルダーごとそのままコピーし、cordmux の Apache License 2.0 の本文と、Vencord と FFmpeg の GPLv3 の本文を加えた。例外は `zlib/README` だけ (下の表)。

- 上流で改行が CRLF の 7 つのファイル (`amf/LICENSE.txt`、`svt-av1/` の `LICENSE.md` と `PATENTS.md` とほかのライブラリの LICENSE 4 つ) は、リポジトリには `.gitattributes` (`* text=auto eol=lf`) で LF で入る。ライセンスの中身は変わらない。
- コンポーネントのフォルダーは丸ごとコピーするので、配る DLL に入らないライブラリの本文 (`svt-av1/googletest` のような、上流のテストのフレームワーク) も入っている。

| `licenses/` の下 | 中身 | 元 |
|---|---|---|
| `cordmux/LICENSE` | Apache License 2.0 の本文 (リポジトリのルートの `LICENSE` と同じ) | apache.org の `LICENSE-2.0.txt` (MD5 3b83ef96387f14655fc854ddc3c6bd57、11358 バイト) の APPENDIX の例の行 `Copyright [yyyy] [name of copyright owner]` を `Copyright 2026 eoforgu0` にしたもの (MD5 b47dc9f07285c9be42f2b015ba9ee3d5、11339 バイト) |
| `Vencord/LICENSE` | GPL-3.0 の本文 | Vencord の `LICENSE` (固定したコミットのものと同じバイト列。gnu.org の `gpl-3.0.txt` と同じで、MD5 1ebbd3e34237af26da5dc08a4e440464、35149 バイト) |
| `FFmpeg/COPYING.GPLv3` | GPL-3.0 の本文 (obs-deps には `COPYING.GPLv2` と `MAINTAINERS` しかない) | gnu.org の `gpl-3.0.txt` (上の行と同じバイト列) |
| `x264/` `srt/` `librist/` `zlib/` | FFmpeg とは別の DLL で配る 4 つ | obs-deps の `licenses/` (`zlib/README` を除く) |
| `zlib/README` | zlib 1.3.1 の README のうち、タイトルの行、バージョンの 1 文、「Copyright notice:」の節 (ライセンスの本文) だけ | obs-deps には 1.2.11 の README が入っていて、配る DLL のバージョン (1.3.1) と違う。zlib のリポジトリのタグ `v1.3.1` の README からコピーした。README の全文は、同じリリースに添付するソースの zip の中の、zlib のアーカイブに入っている |
| `mbedtls/` | srt.dll と librist.dll に静的に入っている Mbed TLS (Apache-2.0。srt は obs-deps がビルドした 3.6.4、librist は自分のソースに持つ 2.26.0。本文は同じ Apache License 2.0) | obs-deps の `licenses/` |
| `aom/` `svt-av1/` `libtheora/` `lame/` `opus/` `libvorbis/` `libogg/` `libvpx/` `nv-codec-headers/` `amf/` | FFmpeg の DLL に静的に入っているコンポーネント | obs-deps の `licenses/` |
| `websocketpp/` `asio/` `nlohmann-json/` | cordmux.dll に入っているコンポーネント (`src-obs/ws/ws_deps.cmake`) | obs-deps の `licenses/` |

配らないコンポーネント (Qt、curl、luajit など、obs-deps のほかのコンポーネント) のライセンスは入れない。

Mbed TLS が入っていることの確認の方法:

- Mbed TLS は自分の名前を DLL に埋め込まないので、名前の文字列では見つからない。
- srt.dll には Mbed TLS の暗号の表の名前 (`ARIA-128-CCM*-NO-TAG`、`CAMELLIA-128-GCM`、`AES-128-KWP` など) が並ぶ。srt は obs-deps の `deps.ffmpeg/60-srt.ps1` で `-DUSE_ENCLIB:STRING=mbedtls` を付けてビルドされ、Mbed TLS は `60-mbedtls.ps1` で静的なライブラリとしてビルドされる (3.6.4、コミット c765c831e5c2a0971410692f92f7a81d6ec65ec2。obs-deps の `include/mbedtls/build_info.h`)。
- librist.dll は `70-librist.ps1` で `-Duse_mbedtls=true -Dbuiltin_mbedtls=true` を付けてビルドされ (librist のソースの `contrib/mbedtls` の 2.26.0 を中に入れる。obs-deps の `include/librist/librist_config.h` の `HAVE_MBEDTLS 1`)、DLL に EAP-SRP の文字列があり、Mbed TLS の DLL を読み込まない。

ほかに静的に入っているコンポーネントがないかも確認した: avcodec の LZMA と iconv は「not compiled in」と「without iconv」の文字列だけで入っておらず、libvpl (Intel の QSV) も入っていない。

## インストールの往復のテストのシナリオ

インストールの往復のテスト (`node test/tools/obs-run/run-window.mjs --step install-roundtrip`) は、配布用の zip を OBS のポータブル版にインストールしてからアンインストールし、残るファイルと設定を確認する。使い方は `test/tools/obs-run/README.md` にある。`--scenario` に渡す名前は次のとおり (省くと `roundtrip`)。`test/tools/obs-run/run-window-roundtrip.test.ts` が、この表の名前とツールの名前が同じかを確認する。

| 名前 | 確認すること |
|---|---|
| `roundtrip` | インストールして OBS を閉じたときの差が、インストールしたファイルと毎回変わる値だけであること。[cordmuxをアンインストール] の後にファイルを削除して OBS を起動したときに、毎回変わる値のほかに差がないこと |
| `move` / `move-foreign` | フォルダーごと名前を変えて、何も押さずに OBS を起動すると、Vesktop の設定 (state.json) の vencordDir が移した先を指すように書き直され、ほかのキーとバックアップが保たれること / 利用者が書き換えた値なら書き直さず、ドックに [Vesktopに追加] を表示すること |
| `force-update-missing` | data/vencord のファイルを消して OBS を起動すると「Vesktop側のcordmuxが上書きされています」と [再インストール] を表示し、[再インストール] の後に data/vencord-src と同じハッシュになること |
| `force-update-tamper` | OBS の起動中に data/vencord のファイルの 1 つに 1 バイト追加すると、同じメッセージと [再インストール] を表示し、[再インストール] で元に戻ること |
| `update` / `update-while-running` | 前のビルドの zip でインストールして設定を変え、OBS を閉じて新しい zip を上書きで展開すると、設定と Vesktop の設定とバックアップが変わらず、cordmux-build.txt が新しいものになること / OBS の起動中に上書きすると cordmux.dll が置き換わらないこと (手順書が OBS を閉じるように書く理由) |
| `other-vencord` / `other-vencord-cancel` | Vesktop がほかの Vencord を指しているときに確認のダイアログを表示し、[切り替え] で切り替え、アンインストールの後に元の Vencord を指すように戻すこと / ダイアログで [キャンセル] を押すと Vesktop の設定が変わらないこと |
| `motw` | 全部のファイルに Zone.Identifier (ZoneId=3。インターネットから取得したファイルを示す) を付けても、cordmux.dll が読み込まれ、ffmpeg.exe が起動すること |
| `dll-clash` | OBS (33.0.1) が読み込んだ FFmpeg の DLL (av* と sw*) の一覧を取ること (同梱の DLL と OBS の DLL の名前がぶつかるかを調べるため) |
| `old-obs` | OBS 33 の置き方の cordmux を OBS 32.2.2 に置いたときに、読み込まれないこと (OBS のログに cordmux の Loading module がない) と、手順書の「ドックが表示されない」に OBS 33 以上への更新が書いてあること。ログと画面も記録する |
| `newer-libobs` | `obs_module_ver()` だけを新しくした DLL を OBS 33.0.1 に置いたときの、ログとプラグインマネージャーの画面を記録すること (失敗にはしない) |

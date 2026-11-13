# test/lint

コード、コメント、画面の文字列、手順書、文書をチェックする lint。どれも Python 3 の標準ライブラリだけで書く。この文書は、lint ごとのチェックの中身と、一覧のファイルの書き方と、セルフテストの例を説明する。CI で実行するジョブは docs/testing.md の「lint とセルフテスト」にある。

## 使い方

```
python test/lint/<lint>.py              既定の対象をチェックし、検出があれば 1 で終わる
python test/lint/<lint>.py PATH...      指定したファイルとフォルダーだけをチェックする
python test/lint/<lint>.py --selftest   NG 例と OK 例でセルフテストを実行する
```

引数の形が違う lint がある。

- `commit_messages_japanese.py` は、コミットの範囲 (`A..B`) を取る。`--message-file PATH` で、ファイルの 1 行目を件名としてチェックする。
- `locale_ids.py` は、リポジトリのルートとみなすフォルダー `ROOT` を 1 つ取る (`ROOT/src-obs` と、`ROOT/src-obs/data/locale` の `ja-JP.ini` と `en-US.ini` を読む)。コードの全体と 2 つの ini を合わせてチェックするためである。
- `install_guide.py` は `--guide G [--ini I]` で手順書を 1 つ、`removal_table.py` はヘッダーを 1 つ、`locale_shapes.py` は `ja-JP.ini` と `en-US.ini` があるフォルダーを 1 つ取る。
- `locale_shapes.py`、`forbidden_qt_usage.py`、`log_lines.py` は、`--report-only` を付けると、検出があっても 0 で終わる (`--strict` は既定と同じ)。
- `forbidden_ui_words.py` は、`--spacing` と `--question` で、`ja_style.txt` の代わりの書き方を指定できる。

出力の行は「ファイル:行番号: チェック項目の名前: メッセージ」の形を基本にする。行番号が決まらない問題は行番号を省く。チェック項目が 1 つだけの lint には、名前を付けないものもある。標準出力は UTF-8 にする (Windows の既定のコードページでは、日本語が文字化けするため)。

## セルフテスト

どの lint も、NG 例 (検出されるはずの例) と OK 例 (検出されないはずの例) を `fixtures/<lint の名前>/` に持つ。`--selftest` は両方を実際にチェックし、NG 例が検出され、OK 例が検出されないことを確認する。NG 例は、検出の仕組みが壊れて常に成功を返す状態に、OK 例は、正しい書き方まで検出する状態に気付くためにある。NG 例か OK 例が 1 つもないときは、何も確認せずに成功しないように、セルフテストを失敗にする。多くの lint は、NG 例の行ごとに検出されるはずのチェック項目を `expect` の行に書き、検出されたものと比べる。別の理由で検出されて、確認したいチェックが壊れたことに気付けない状態を避けるためである。

## lint の一覧

| lint | チェックするファイル | 検出するもの |
|---|---|---|
| `identifiers_english.py` | src-obs、src-vencord、test の `.h .hpp .c .cc .cpp .ts .tsx .mjs .js .py .ps1` | コード (コメントと文字列を除いた部分) の、ASCII でない文字 |
| `comments_japanese.py` | 同じ | 英字の語 (2 文字以上) が 3 つ以上あり、日本語 (ひらがな、カタカナ、漢字) を含まないコメント。続けて書いた 1 行のコメントは、1 つのコメントとして数える。例外がある (下の「comments_japanese.py」) |
| `commit_messages_japanese.py` | コミットの範囲の件名 (`git log --no-merges --format=%s`。範囲を省くと HEAD までのすべてのコミット) | 日本語を含まない件名 |
| `windows_api_confinement.py` | src-obs と src-vencord (tests フォルダーを除く) の `.h .hpp .c .cc .cpp .ts .tsx .mjs .js` | `windows_names.txt` の名前が、`src-obs/platform` と `src-vencord/paths.ts` の外の、コメントを除いた部分 (文字列は含む) にある。`paths.ts` は、src-vencord の直下の 1 つだけを許す |
| `forbidden_libobs_usage.py` | src-obs (tests フォルダーを除く) の `.h .hpp .c .cc .cpp` | `forbidden_libobs_names.txt` の libobs の使い方が、コメントを除いた部分にある。許す場所はない |
| `forbidden_qt_usage.py` | 同じ | `forbidden_qt_names.txt` の Qt と Windows の使い方 (音の出るメッセージボックスなど) がコードにある。先頭が `string:` の行は、文字列リテラルの中 (スタイルシートに書かない値) |
| `log_lines.py` | 同じ (`diag::logf(` と `blog(` の呼び出しの中の文字列リテラル。ws フォルダーの下では `log(` も) | OBS のログに書く行の形の誤り (CODESTYLE.md の「OBS のログ」)。チェック項目は `ascii`、`cordmux-start`、`lowercase-start`、`period-end` |
| `locale_ids.py` | src-obs (tests も含む) の文字列リテラルのキーと、`src-obs/data/locale` の `ja-JP.ini` と `en-US.ini` | コードが指すキーと ini のキーの過不足、2 回定義したキー、空の値、ja と en で数が違う `%N`、`%N` に何が入るかを書いたコメントがないキー、元のキーがない `_one` のキー、元のキーと `%N` が違う en の `_one` のキー |
| `locale_shapes.py` | `src-obs/data/locale` の `ja-JP.ini` と `en-US.ini` | キーの最後の種類の語 (`.button`、`.status` など) と、値の形 (文の終わり、「...」、英語の大文字の使い方) の食い違いと、種類の語がないキー。正しいかを決められない英語の大文字の使い方は、警告として出力する |
| `forbidden_ui_words.py` | `src-obs/data/locale` の 2 つの ini の値 (キーとコメントはチェックしない) | `forbidden_ui_words.txt` の記号、表記、空白の誤り |
| `install_guide.py` | `guide/ja.md` と `guide/en.md` | 手順書 (Markdown) の誤り (下の「install_guide.py」) |
| `removal_table.py` | `src-obs/install/removal_table.h` | アンインストールで消す対象の表の、Python のツールが読めない形 (下の「removal_table.py」) |
| `references.py` | リポジトリ全体 | リポジトリにないファイルとフォルダーへの参照と、文書にない見出しへの参照 (下の「references.py」) |

## コメントと文字列の切り分け

C と C++ (`.h .hpp .c .cc .cpp`) と TypeScript と JavaScript (`.ts .tsx .mjs .js`) は、同じ処理で、コメント (`//` と `/* */`)、文字列 (`"..."`、`'...'`、`` `...` ``)、生文字列リテラル (`R"delim(...)delim"`) を分ける。文字列の中の `//` (URL など) をコメントと読み違えないことは、`fixtures/identifiers_english/good.cpp` で確認する。C++14 の桁区切り (`2'000'000`) の `'` は、文字リテラルの始まりと読まない。桁区切りの後の名前とコメントを読み飛ばさないことは、`fixtures/identifiers_english/bad_digit_separator.cpp` と `fixtures/comments_japanese/bad_digit_separator.cpp` で確認する。テンプレート文字列の中の `${...}` は、コードとして読まずに文字列の一部とする。Python は `#` と三重引用符を、PowerShell は `#`、`<# #>`、here-string (`@"..."@` と `@'...'@`) を、同じ考え方で分ける。

## comments_japanese.py

検出の例外は次のとおり。

- URL だけのコメント
- SPDX の行とライセンスの表記 (その語を除いた残りの語が 3 つより少ないもの)
- ツールへの指示 (`NOLINT` `clang-format` `eslint` `@ts-` `prettier` `noqa` で始まるもの)
- ファイルの先頭のシェバン行
- clang-format が書き足す namespace の終わりのコメント (`// namespace X::Y`)
- `#endif` の後のインクルードガードの名前

## vendored.txt

`identifiers_english.py`、`comments_japanese.py`、`windows_api_confinement.py`、`forbidden_libobs_usage.py` (と、同じファイルの選び方を使う `forbidden_qt_usage.py` と `log_lines.py`) がチェックしないファイルの一覧で、各行は fnmatch のパターンである。obs-plugintemplate からコピーして変更していない `src-obs/cmake` と `src-obs/build-aux` のファイル、`src-obs/.clang-format`、`src-obs/.gersemirc`、doctest のヘッダー (`src-obs/tests/doctest.h`) と、テスト用の送信側 (`test/tools/harness`) を並べる。自前のファイル (`src-obs/cmake/cordmux_*.cmake`) と、コピーして変更したファイル (`src-obs/cmake/windows/resources/resource.rc.in`) は入れない (CODESTYLE.md の「SPDX の行」)。

## windows_names.txt、forbidden_libobs_names.txt、forbidden_qt_names.txt

各行は Python の正規表現 (`re.search`、大文字と小文字を区別する) で、空行と `#` で始まる行は読み飛ばす。行ごとの理由は、各ファイルのコメントにある。

- `windows_names.txt` は、`src-obs/platform` と `src-vencord/paths.ts` の外に書いてはいけない名前の一覧である。関数の名前は末尾の A と W を除いた形で書き、どちらの関数にも一致させる。Windows と関係のない識別子 (`CopyFileFn` など) に一致しないように、単語境界 (`\b`) と、なくてもよい末尾 (`[AW]?` など) を付けた行もある。
- `forbidden_libobs_names.txt` の行の末尾に「  REQUIRES: <正規表現>」(前に半角の空白 2 つ) を付けると、その行に一致しても、同じファイルのコメントの外にその正規表現が見つかればエラーにしない。`obs_source_add_active_child` の行は 2 つに分けて、`enum_active_sources` と `enum_all_sources` を 1 つずつ求める。

## forbidden_ui_words.txt と ja_style.txt

`forbidden_ui_words.txt` は、画面の文字列に使わない記号、長音と OBS が表示する名前の表記の誤り、空白の誤りの一覧で、一致するとエラーになる。書き方の詳細は `forbidden_ui_words.txt` の先頭のコメントにある。

- 各行は Python の正規表現 (`re.search`、大文字と小文字を区別する、`re.ASCII`) で、行の先頭の空白も正規表現の一部である。
- 「`# set: <名前> <説明>`」の行から次のセットの行の前までが、1 つのセットになる。セットの名前は、出力の行のチェック項目の名前になる。
- セットは、`symbols` (記号)、`notation` (長音と、OBS が表示する名前の表記)、`spacing` (空白の誤り) である。
- `install_guide.py` は `spacing` を `[ ]` と「」の中を伏せた手順書の各行に、`test/tools/dist-zip/make_release_notes.py` は `spacing` を引用の中を伏せたリリースノートに使う。どちらも `load_word_rules` で読む。ほかのセットは、ほかのアプリの画面の文字列を引用することがあるので、手順書とリリースノートには使わない。
- 行の先頭の指定: `ja:` と `en:` は、その言語の ini にだけ使う (言語はファイル名の先頭で決める)。`spacing=A:`、`spacing=B:`、`question=halfwidth:`、`question=fullwidth:` は、`ja_style.txt` の値と合うときだけ使う。`not_names:` は、`ja_style.txt` の `name_keys` のキー (OBS の一覧に表示する名前) には使わない。言語が分からない ini には言語を指定した行を使わず、NOTE の行を出力する。
- `ja_style.txt` は、日本語の 2 つの表記の設定 (`spacing` と `question`) と、例外のキー (`name_keys` と `suffix_keys`) を持つ。値だけを手で書き換えず、`test/tools/wording/apply_ja_style.py` で ini の値と一緒に書き換える。
- 画面で使う語は docs/ui_guidelines.md の「用語集」にある。

## install_guide.py

手順書 (`guide/ja.md` と `guide/en.md`。Markdown) の lint。見出しと語は `install_guide_rules.txt` にあり、手順書ごとに 1 つのセットを持つ。キーの書き方は `install_guide_rules.txt` の先頭のコメントに、チェックの中身は `install_guide.py` の `check_guide` と `syntax_problems` のコメントにある。手順書の書き方は docs/ui_guidelines.md の「手順書の書き方」に、Markdown の形と zip のテキストへの変換は dist/README.md の「手順書の形」にある。

主なチェックは、使ってよい記法の範囲 (使えない記法と、項目と段落の間の空行)、同じファイルの見出しへのリンクのアンカーと相対リンクの行き先、タイトルと `##` の見出しの並び、手順の数、手順の部分の禁止語、`[ ]` の名前と ini の照合、引用の中の文字列、ini からコピーした文と決まった文、画面の文言 (ini の値) が手順書の見出しと手順の番号を指す所、`forbidden_ui_words.txt` の `spacing` のセット、CR である。引数なしで実行すると、日本語と英語の構成 (`##` の数、節ごとの `###` の数と番号付きの項目の数) が同じかも見る。アンカーは GitHub の作り方 (小文字にし、文字・数字・空白・`-`・`_` のほかを除き、空白を `-` にする。重なれば `-1`、`-2` を付ける) で作る。

Markdown の読み方 (行の種類、インラインのコードとリンク、見出しのアンカー) は `install_guide.py` にあり、`test/tools/dist-zip` のツール (`guide_to_text.py`、`check_dist_zip.py`、`make_dist_zip.py`、`make_release_notes.py`) が import して同じ読み方で読む。使う名前は、`load_rules`、`parse_rules`、`GuideRules` とその属性 (`target`、`zip_name`、`title`、`headings`、`roles`、`placeholder`)、`guide_lang`、`sections_by_heading`、`render_heading`、`parse_markdown`、`split_inline`、`render_inline`、`plain_text`、`syntax_problems`、`display_width`、`MARKER_LINE`、`HEADING_KINDS`、`ITEM_KINDS` である。ツールは見出しの文字列を持たず、手順書の節を role から探す。名前を変えるときは、読む側も一緒に修正する。

セルフテストは、rules のセットごとに、`fixture:` の OK 例に問題がなく、同じ親フォルダーの `bad_*` の NG 例にどれも問題があり、その問題のメッセージが `expect.txt` の文字列を含むことを確認する。加えて、OK 例をその場で書き換えた例 (禁止語を手順の部分に移す、決まった文を消す、使えない記法を足す、ini の参照の見出しを変える、など) でも確認する。OK 例と NG 例の手順書は、セットの target と同じ名前 (`ja.md`、`en.md`) にする。

| 例 | 中身 |
|---|---|
| `fixtures/install_guide/good/` と `bad_*` | 日本語のセットの OK 例 (`ja.md` と `ja-JP.ini`) と NG 例 |
| `fixtures/install_guide/en/` | 英語のセットの OK 例 (`good/` の `en.md` と `en-US.ini`) と NG 例 (`bad_*`) |
| `fixtures/install_guide/mechanism/` | rules のキーで入れる仕組み (2 つ目の手順の範囲、引用、UI 要素の種類の語、「...」、空白) を確認する、テスト用の rules (`rules.txt`) と例。空白のチェックの語は `fixtures/forbidden_ui_words/mechanism/words.txt` から取る |

## removal_table.py

アンインストールで消す対象の表 (`src-obs/install/removal_table.h`) を文字列として読み、Python のツールが読める形かをチェックする。形はそのヘッダーの「Python が読む形」のコメントにある。チェックするもの:

- 表の始まりと終わりの目印 (`// removal-table-begin` と `// removal-table-end`) が、行頭に 1 回ずつ、この順にある
- 目印の間の行が、表の行 (1 行のソースに 1 つ)、配列の始まりと終わり、空行、コメントのどれかである
- `kRemovalItemCount` と、表の行の数と、`enum class RemovalItem` の列挙子の数が同じで、どの列挙子も表にちょうど 1 回ずつある
- 表の `Remover`、`Locate`、`UninstallStep` の名前が、それぞれの `enum class` の列挙子にあり、`Remover` の列挙子が `Product`、`User`、`Nobody` の 3 つである
- 消さない行 (`Remover::Nobody`) の段階が `UninstallStep::NotRemoved` で、ほかの行の段階は `NotRemoved` ではない
- 引数を省いたときだけ、`Locate` の列挙子ごとに、`test/tools/obs-run/install_snapshot.py` の `LOCATE_VIEWS` に調べ方がある

`install_snapshot.py` は、`parse_removal_table`、`TABLE_BEGIN`、`TABLE_END` を import して、同じ読み方で表を読む。NG 例 (`fixtures/removal_table/bad_*/removal_table.h`) の 2 行目の `// expect: <文字列>` は、検出されるはずの問題のメッセージの一部である。セルフテストは、リポジトリの表のコピーに、調べ方のない `Locate` を追加した例も確認する。

## NG 例と OK 例

```
fixtures/
  identifiers_english/       bad.cpp (日本語の識別子) / bad_digit_separator.cpp (C++14 の桁区切りの後の日本語の識別子) / good.cpp (ASCII だけ。// を含む URL の文字列がある)
  comments_japanese/         bad.ts (英語だけのコメント) / bad_digit_separator.cpp (C++14 の桁区切りの後の英語だけのコメント) / bad_directive_prose.ts (eslint の語を含むだけの英語の説明文) /
                             bad_license_prose.ts (copyright の語を含むだけの英語の説明文) / good.ts (SPDX の行、URL だけの
                             コメント、eslint の指示と、日本語のコメント)
  commit_messages_japanese/  bad_subject.txt (英語だけの件名) / good_subject.txt (日本語を含む件名)
  windows_api_confinement/   bad と good を、それぞれリポジトリのルートとみなす
    bad/src-obs/ws/listener.cpp                   platform/ の外の <windows.h>
    bad/src-obs/ws/digit_separator_bad.cpp        C++14 の桁区切りの後の <windows.h>
    bad/src-obs/install/remove_files.cpp          platform/ の外の、ファイルの操作と OS のバージョンの関数 (名前ごとに数を確認する)
    bad/src-vencord/transport/platform_sniff.ts   paths.ts の外の、OS を見分ける名前 (名前ごとに数を確認する)
    bad/src-vencord/transport/paths.ts            src-vencord の直下ではない paths.ts
    good/src-obs/platform/platform_win.cpp        platform/ の中の <windows.h>
    good/src-obs/dock/copy_fn.cpp                 名前の一部に Windows の関数の名前を含む識別子 (CopyFileFn など)
    good/src-vencord/paths.ts                     src-vencord の直下の paths.ts の中の process.platform と "win32"
  forbidden_libobs_usage/
    bad/src-obs/sources/                          音量とミュートの書き換え、audio_render の自作、enum_active_sources か
                                                   enum_all_sources のない add_active_child、C++14 の桁区切りの後の呼び出し
    good/src-obs/sources/mirror_good.cpp          enum_active_sources と enum_all_sources の両方を実装した add_active_child
    good/src-obs/config/read_only_good.cpp        音量とミュートを読むだけ
  forbidden_qt_usage/        行の末尾の // expect: <一覧の行>。一覧のすべての行が、どれかの NG 例で一致することも確認する
    bad/sounds.cpp                                一覧の 11 行
    good/dialog.cpp                               QDialog と、計算した後の値のスタイルシート (コメントと文字列の中の名前は読まない)
  log_lines/                 行の末尾の // expect: <チェック項目の名前>
    bad/lines.cpp、bad/ws/listener.cpp            日本語、cordmux で始まる行、小文字で始まる行、ピリオドで終わる行
    good/lines.cpp、good/ws/listener.cpp          正しい形のログの行
  locale_ids/                フォルダーごとに src-obs と 2 つの ini を持ち、そのフォルダーをリポジトリのルートとみなしてチェックする
    bad_code_missing_from_ini/   コードのキーが ini にない
    bad_ini_orphan/              どのコードも指していない ini のキー
    bad_locale_mismatch/         ja-JP.ini にあり、en-US.ini にないキー
    bad_duplicate_key/           同じキーを 2 回定義した
    bad_empty_value/             値が空
    bad_arg_mismatch/            ja と en で %N の番号が違う
    bad_arg_count_mismatch/      同じ %N の数が、ja と en で違う
    bad_arg_unannotated/         %N に何が入るかを書いたコメントがない
    bad_one_without_base/        元のキーがない _one のキー
    bad_one_args_mismatch/       元のキーと %N が違う en の _one のキー
    bad_one_one_locale/          片方の ini にだけある _one のキー
    good/                        過不足なく一致する (C++14 の桁区切りの後の文字列と、連結してキーを作るときの先頭部分の
                                 文字列も含む)
  locale_shapes/             ja-JP.ini と en-US.ini の、値の行の前の # expect: <チェック項目の名前> と # expect: review
    good/                        どのチェックでも検出されない値
    bad/                         チェック項目ごとの誤りと、警告になる値
  forbidden_ui_words/        値の行の前の # expect: <規則> (書き方の条件があるときは # expect[spacing=B]: <規則>)
    bad/ja-symbols.ini、bad/en-symbols.ini   記号と表記の誤り
    good/ja-natural.ini、good/en-natural.ini 一覧の行に似ているが、正しい書き方 (4 通りの書き方のすべてで確認する)
    good/key_and_comment_only.ini            キーとコメントの中にだけある、一覧の記号と表記
    mechanism/words.txt と ja-JP.ini、en-US.ini、other.ini
                                             セット、言語の指定、書き方の指定、not_names、re.ASCII の \b を確認するテスト用の一覧と例
  install_guide/             上の「install_guide.py」
  removal_table/             上の「removal_table.py」
  references/                下の「references.py」
```

## references.py

コメント・文書・文字列・設定ファイル (CI の設定のステップ名とコマンドを含む) に、読み手が開けないものへの参照がないかをチェックする。どの規則も、検出があれば終了コードが 1 になる。

| 規則 | 検出するもの |
|---|---|
| `missing-path` | リポジトリにないファイルかフォルダーを指すパス (判定の方法は下の「パスの判定」) |
| `missing-heading` | 「`<パス>.md` の「<見出し>」」の形で文書の見出しを参照する文字列のうち、文書はあるのに、その見出しがないもの |

```
python references.py                     リポジトリ全体をチェックし、検出があれば 1 を返す
python references.py PATH...             指定したファイルかフォルダーだけをチェックする (存在の判定はリポジトリ全体で行う)
python references.py --summary           規則ごとの件数だけを出力する
python references.py --root DIR          DIR をリポジトリのルートとみなす
python references.py --selftest          fixtures/references の NG 例と OK 例でセルフテストを実行する
```

チェックの対象は、git が追跡しているファイルと、追跡していないが無視もしていないファイルである。`PATH` にリポジトリの外のファイル (コミットメッセージを書いたファイルなど) を指定すると、そのファイルもチェックする。対象外は次の 2 つである。

- 外部からコピーしたファイルと生成したファイル: `licenses` フォルダーの下、`LICENSE`、`package-lock.json`、obs-plugintemplate からコピーした `src-obs/cmake` と `src-obs/build-aux`、`src-obs/.clang-format`、`src-obs/.gersemirc`、`src-obs/tests/doctest.h`。ただし、`src-obs/cmake` の中で名前が `cordmux_` で始まるファイルと、製品の説明の文字列を追加した `src-obs/cmake/windows/resources/resource.rc.in` は、自前の文字列を含むのでチェックする (references.py の中の `THIRD_PARTY_GLOBS` と `OWN_IN_THIRD_PARTY_GLOBS`)。
- このフォルダーの `fixtures/` (lint の NG 例と OK 例)。

### ファイルの種類ごとのチェック範囲

| 種類 | 見出しの規則 | パスの規則 |
|---|---|---|
| C・C++・TypeScript・JavaScript (`.c` `.cc` `.cpp` `.h` `.hpp` `.ts` `.tsx` `.js` `.mjs` `.cjs` `.jsx`)、Python、PowerShell | コメントと文字列 (切り分けには `forbidden_libobs_usage.py` と `comments_japanese.py` の処理を使う) | コメント・文字列・コード。`./` と `../` はコメントの中だけ |
| `.md` と、`#` の行を持たない `.txt` (手順書など) | すべて | すべて |
| そのほか (YAML・CMake・ini・JSON・TSV と、`#` の行を持つ `.txt`) | すべて。`#` の行 (ini は `;` の行も) をコメント、残りをデータとして扱う | すべて。`./` と `../` と `.tsv` はコメントの中だけ |
| `.gitignore` と `.gitattributes` | すべて | コメントの行だけ (ほかの行には、リポジトリにないものを書くため) |

### パスの判定

パスの多くは、OBS・Qt・Vencord のようなリポジトリの外のツリーにある場所か、実行時に作られる場所を指す。外のツリーの名前をすべて列挙して許可することはできないので、次のものだけをリポジトリのツリーと照合する。

- リポジトリのルートにある名前で始まるパス: ルートからたどって、ファイルかフォルダーがあれば問題なしとする。拡張子を省いた参照 (import の参照先) も、同じ名前に拡張子を付けたファイルがあれば問題なしとする。
- コメントと文書の中の `./` と `../` で始まるパス: そのファイルの場所からたどる (`./` はルートからもたどる)。
- コメントと文書にあるほかのパスも、そのファイルの場所からたどってあれば問題なしとする (Markdown の相対リンクと同じ書き方)。
- ルートにある名前で始まらない `.md` の文書のパス (コメントと文書では `.tsv` も): ツリーの中で末尾が一致するパスを探す。
- ファイル名だけの `.md` の文書: ツリーの中で同じ名前のファイルを探す。

ファイル名だけで探すときと、末尾の一致で探すときは、`fixtures` フォルダーの下のファイルを使わない (NG 例と OK 例のファイル名が、文書の名前と同じことがあるため)。

見出しへの参照 (`missing-heading`) の文書は、コメントと文書では、そのファイルの場所からたどり、なければルートからたどる。どちらにもなければ、ツリーの中で末尾が一致するパスが 1 つに決まるときだけ、その文書をチェックする。文書が見つからないときは `missing-path` として出力する。見出しは `#` で始まる行の文字列 (コードブロックの中は除く) と比べ、文字列が完全に一致するときだけ、見出しがあるとみなす。「## 見出し」のように行の形で書いた参照は、先頭の `#` を外して比べる。

ツリーになくても許可するもの (どれも references.py の中の表か正規表現。追加するときは references.py に追加し、OK 例に 1 行追加する):

| 許可するもの | 理由 | references.py の中の名前 |
|---|---|---|
| ビルドの出力と作業用フォルダーの名前 (`.deps`・`build`・`build_x64`・`release`・`dist`・`node_modules`・`_work`・`_share`・`out` ほか) を含むパス | ツリーの中にないのが正しい | `BUILD_DIR_NAMES` |
| ワイルドカードとプレースホルダー (`*`・`?`・`{`・`<`・`$`・`%`・`[` の前) と、文字列を連結して作るパスの先頭部分 (`-` か `_` で終わる) | 最後の `/` までをフォルダーとしてチェックする | `SKIP_NEXT_CHARS` |
| 省略した部分 (`...`) | `...` の前の `/` までをチェックする | - |
| 途中のフォルダーが大文字だけの名前のもの (`DIR`・`ROOT`) と、`xxx` を含むもの | プレースホルダー | `PLACEHOLDER_SEG_RE` |
| URL と、スキームを省いた URL (`example.com/...`) | リポジトリの中ではない | `URL_RE`・`DOMAIN_RE` |
| 列挙した名前 (`a.ts/b.ts`) と、`名前.h/.cpp` の後半 | パスではない | `ENUM_SEG_RE`・`PURE_EXT_RE` |
| ツールが出力するファイルの名前 | コードにその名前だけの文字列があり、ツリーにはない名前 | `Tree.output_names` |

### セルフテスト (`--selftest`)

`fixtures/references/good` と `fixtures/references/bad` は、それぞれをリポジトリのルートとみなす架空のツリーである。

- bad の行末の「expect: <規則>, <規則>」は、その行で検出されるはずの規則である (同じ規則が 2 回なら 2 回書く)。書いていない行では何も検出されない。すべての規則が bad で 1 回以上検出されることも確認する。
- good は、上の許可するものを含み、何も検出されない。
- 終了コードは、bad が 1、good が 0 である。

| 例 | 中身 |
|---|---|
| `fixtures/references/good/README.md` | 存在するパス、存在する見出しへの参照、外のツリーのパス、出力ファイル、ワイルドカードとプレースホルダー、省略したパス |
| `fixtures/references/good/.gitignore` | 無視するファイルの一覧の行 (パスをチェックしない) と、コメントの行 |
| `fixtures/references/good/app/` | コメントと文字列のパス、./ と ../、コメントの中の見出しへの参照 (`h264_frame.h`) |
| `fixtures/references/good/notes/guide.md` | ../ で上のフォルダーに戻るパスと見出し、文書の場所から書いたパス (同じフォルダーの下にある fixtures の例を指す) |
| `fixtures/references/good/tools/` | 出力するファイルの名前 (その名前だけの文字列) と、.json に書いたパス |
| `fixtures/references/good/.github/workflows/check.yml` | ステップ名とコマンドの中のパス、uses の ./ |
| `fixtures/references/bad/README.md` | 存在しないパス (ルート・ファイル名だけ・.tsv・プレースホルダーの前・< の後・省略した部分の前・fixtures の下の名前)、存在しない見出し |
| `fixtures/references/bad/app/` | 文字列の中の存在しないパス、../ で書いた存在しないパス、同じフォルダーの README.md にない見出し |
| `fixtures/references/bad/tools/` | .json の中の存在しないパス |
| `fixtures/references/bad/src-obs/cmake/` | コピーしたテンプレートのフォルダーの中の、自前のファイルと変更したコピー (検出される) と、コピーしたファイル (チェックしない) |
| `fixtures/references/bad/.github/workflows/check.yml` | CI の設定ファイルのコマンドにある存在しないパス |

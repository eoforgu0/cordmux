# cordmux の開発

This guide is in Japanese. The code comments and developer documents in this repository are also in Japanese. You can write issues in English.

この文書では、cordmux をソースコードからビルドしてテストする方法と、変更するときのルールを説明します。

## 初めての変更までの流れ

1. [必要なもの](#必要なもの)のツールを入れます。
2. 変更する部分のビルドとテストを実行します。OBS のプラグインは「[OBS のプラグインのビルド](#obs-のプラグインのビルド)」と「[C++ の単体テスト](#c-の単体テスト)」、送信側は「[送信側のテストと型チェック](#送信側のテストと型チェック)」にあります。
3. [lint](#lint) を実行します。
4. [コミットメッセージ](#コミットメッセージ)の形でコミットし、プルリクエストを作ります。

## リポジトリの構成

| フォルダー | 内容 |
|---|---|
| src-obs | OBS の中で動く cordmux (受信側)。C++ と Qt で書いた OBS のプラグインと、その単体テスト (src-obs/tests) |
| src-vencord | Vesktop の中で動く cordmux (送信側)。TypeScript で書いた Vencord のプラグインと、その単体テスト (src-vencord/tests) |
| test/lint | lint |
| test/tools | テスト、動作確認、配布用の zip の作成に使うツール |
| test/fixtures | テストデータ |
| guide | 利用者向けの手順書 (Markdown。配布用の zip には、ここから作ったテキストを入れる) |
| dist | 配布用の zip に入れるライセンスとソースコードの入手先 |
| docs | 開発者向けの文書 |
| .github | CI のワークフローと、Issue のフォーム |

## 文書

| 文書 | 内容 |
|---|---|
| docs/spec.md | 仕様。機能、品質、対応する環境とバージョン |
| docs/architecture.md | 仕組み。コンポーネント、映像と音声の流れ、録画、インストール |
| docs/wire_format.md | ワイヤー形式。送信側と受信側が WebSocket でやり取りするメッセージの形式 |
| docs/ui_guidelines.md | 画面の文言と手順書の書き方、画面の見た目とふるまい |
| docs/testing.md | テストと動作確認 |
| docs/release.md | 配布用の zip、バージョン、リリースの手順 |
| CODESTYLE.md | コーディング規約と用語集 |
| CHANGELOG.md | 変更履歴 |

## 必要なもの

| ツール | バージョン | 用途 |
|---|---|---|
| Windows | 11 の x64 版 | OBS のプラグインと C++ の単体テスト |
| Visual Studio | 2022 か 2026 (「C++ によるデスクトップ開発」のワークロード) | OBS のプラグインと C++ の単体テスト |
| Windows SDK | 10.0.22621 | OBS のプラグイン |
| CMake | 3.28 以降 | OBS のプラグインと C++ の単体テスト |
| Git | | すべて |
| Python | 3.12 (CI と同じ) | lint とツール |
| Node.js | 22 (22.12 以降)、24、26 以降 (vitest 5 が対応するバージョン) | 送信側のテストとビルド |
| pnpm | 12.15.0 (corepack で用意する) | 送信側のビルド |

Visual Studio 2022 のバージョン 17.2 では、obs-deps の Qt 6 のヘッダーがコンパイルエラーになります。CI は、GitHub Actions の windows-2022 のイメージにある Visual Studio 2022 でビルドします。

Node.js 25 以降は corepack が同梱されないので、pnpm を用意する前に `npm install -g corepack` を実行します。

テストツールとプレビューツールの一部は、numpy、Pillow、ffmpeg を使います (docs/testing.md)。

## OBS のプラグインのビルド

src-obs/CMakePresets.json に、次のプリセットがあります。

| プリセット | 用途 |
|---|---|
| windows-x64 | ローカルのビルド。Visual Studio 2022 と Windows SDK 10.0.22621 を使います |
| windows-ci-x64 | CI のビルド。windows-x64 の設定に加えて、警告をエラーにします |

src-obs で次のコマンドを実行します。

```
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --install build_x64 --prefix release --config RelWithDebInfo
```

src-obs/release/cordmux/cordmux.dll と、src-obs/release/cordmux/data/locale の ja-JP.ini と en-US.ini ができていれば成功です。

`cmake --install` には、必ず `--prefix` を付けます。省くと、OBS が実際に読み込むプラグインのフォルダー (`%ALLUSERSPROFILE%\obs-studio\plugins`) にインストールします。

初めて `cmake --preset windows-x64` を実行すると、src-obs/buildspec.json で指定したバージョンの OBS のソースコード、obs-deps、Qt をダウンロードして src-obs/.deps に置き、OBS のライブラリ (libobs など) をビルドします。そのため時間がかかります。

### ローカルの環境に合わせたプリセット

Visual Studio や Windows SDK のバージョンが windows-x64 と違うときは、src-obs の中に CMakeUserPresets.json を作り、windows-x64 を継承したプリセットを書きます。このファイルは各自の環境の設定なので、Git で追跡しません (.gitignore)。

```json
{
  "version": 8,
  "configurePresets": [
    {
      "name": "windows-x64-local",
      "inherits": ["windows-x64"],
      "generator": "<Visual Studio のジェネレーター>",
      "architecture": "x64,version=<Windows SDK のバージョン>"
    }
  ],
  "buildPresets": [
    {
      "name": "windows-x64-local",
      "configurePreset": "windows-x64-local",
      "configuration": "RelWithDebInfo"
    }
  ]
}
```

ジェネレーターの名前は、`cmake --help` の Generators の一覧にあります (Visual Studio 2022 は `Visual Studio 17 2022`)。コマンドの `--preset` には、このプリセットの名前を渡します。

- ローカルでも警告をエラーにするときは、`configurePresets` の `cacheVariables` に `"CMAKE_COMPILE_WARNING_AS_ERROR": true` を書きます。
- 環境によっては、CMake がダウンロードのときに TLS の証明書を検証できずに失敗します。そのときは、`configurePresets` の `environment` で、環境変数 `SSL_CERT_FILE` に CA 証明書のファイル (Git for Windows の `C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt` など) を指定します。

C++ の単体テストのプリセットも、同じように src-obs/tests の中の CMakeUserPresets.json に、tests-x64 を継承して書きます。

## C++ の単体テスト

C++ の単体テストは、src-obs/tests にある別の CMake のプロジェクトです。obs-deps の FFmpeg と Qt を使うので、先に src-obs で `cmake --preset windows-x64` を実行して、src-obs/.deps に依存関係を用意しておきます。

録画ファイルの作成 (セグメントファイルを 1 つの録画ファイルにまとめる処理) のテストなどは、実際の ffmpeg と ffprobe を使います。これらのテストは、環境変数 `CORDMUX_FFMPEG_EXE` と `CORDMUX_FFPROBE_EXE` が obs-deps の ffmpeg.exe と ffprobe.exe を指すときだけ実行し、指さないときはスキップします。CI は、スキップしたテストが 1 つでもあると失敗にします。テストを実行する前に、Git Bash で、リポジトリのルートから次のように設定します。

```bash
bin="$(python test/tools/dist-zip/build_info.py obs-deps-bin)"
export CORDMUX_FFMPEG_EXE="$bin/ffmpeg.exe" CORDMUX_FFPROBE_EXE="$bin/ffprobe.exe"
```

src-obs/tests で次のコマンドを実行すると、テストの実行ファイル align_tests.exe と decor_tests.exe をビルドして実行します。

```
cmake --preset tests-x64
cmake --build --preset tests-x64
../../build_x64/align-tests/Release/align_tests.exe
../../build_x64/align-tests/Release/decor_tests.exe
```

`cmake --preset tests-x64` に `-DCORDMUX_BUILD_PREVIEW_TOOLS=ON` を付けると、プレビューツール (ui_preview と decor_preview) と、ドックを操作するテスト (dock_drive) もビルドします。CI ではビルドしません。使い方は、docs/testing.md の「プレビューツール」と「ドックを操作するテスト」にあります。

ui_preview をビルドして実行する src-obs/tests/ui_preview/run_ui_preview.py は、既定でプリセット tests-x64-local を使います (decor_preview は、同じプリセットでビルドして直接実行します)。このプリセットは、src-obs/tests の CMakeUserPresets.json に書きます。書き方は、docs/testing.md の「プレビューツール」にあります。

テストの中身と、一部のテストだけを実行する方法は、docs/testing.md の「C++ の単体テスト」にあります。

## 送信側のテストと型チェック

リポジトリのルートで次のコマンドを実行します。`npm test` は vitest で単体テストを実行し、`npm run typecheck` は tsc で src-vencord の型をチェックします。

```
npm ci
npm test
npm run typecheck
```

Vencord のモジュールを import するファイルなど、tsconfig.json の `exclude` に挙げたファイルは、送信側のビルドの `pnpm test:ts` がチェックします。テストの対象は、docs/testing.md の「TypeScript の単体テスト」にあります。

## 送信側のビルド

送信側は、Vencord のソースツリーの src/userplugins/cordmux にコピーして、Vencord と一緒にビルドします。CI の手順は .github/workflows/vencord-plugin.yml にあります。ローカルでは、Git Bash で、リポジトリのルートから次のように実行します。

```bash
vencord='<Vencord のフォルダー>'   # 短いパスの空のフォルダー
commit=$(python -c "import json; print(json.load(open('src-vencord/vencord.lock.json', encoding='utf-8'))['commit'])")
git init "$vencord"
git -C "$vencord" fetch --depth 1 https://github.com/Vendicated/Vencord.git "$commit"
git -C "$vencord" checkout FETCH_HEAD
mkdir -p "$vencord/src/userplugins/cordmux"
cp -r src-vencord/. "$vencord/src/userplugins/cordmux/"
rm -rf "$vencord/src/userplugins/cordmux/vencord.lock.json" "$vencord/src/userplugins/cordmux/tests"
cd "$vencord"
corepack enable
corepack prepare pnpm@12.15.0 --activate
pnpm install --frozen-lockfile
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) VENCORD_REMOTE=Vendicated/Vencord pnpm build --standalone --disable-updater
[ -f dist/package.json ] || echo '{}' > dist/package.json
pnpm test:ts
cd -
python test/tools/vencord/check_dist.py "$vencord/dist"
```

- ビルドする Vencord のコミットは、src-vencord/vencord.lock.json に書いてあります。
- vencord.lock.json と tests はコピーしません。vencord.lock.json はビルドの手順だけが読むファイルで、tests はリポジトリのルートの vitest で実行するテストです。
- Vencord のフォルダーを長いパスの下に置くと、`pnpm install` が、パスの長さの上限 (Windows の 260 文字) を超えて失敗することがあります。
- Vencord のビルドスクリプトは、環境変数 `VENCORD_REMOTE` がないと、リモートの origin の URL を git から読みます。上の手順で取得したフォルダーには origin がなく、そのままではビルドが失敗するので、`VENCORD_REMOTE` でリポジトリを指定します。
- `pnpm build` は dist/package.json を作りません。check_dist.py は、Vesktop が読む 5 つのファイル (package.json を含む) がそろっているかを確認するので、`{}` だけの package.json を置きます。
- CI は、コピーした version.ts の `CORDMUX_BUILD_ID` を `<バージョン>+<短いコミット>` に書き換えてからビルドします。ローカルのビルドでは `<バージョン>+dev` のままです。

## 配布用の zip

配布用の zip は、CI の dist.yml が作ります。zip を作るツールは test/tools/dist-zip にあり、ローカルでも CI と同じ引数で実行できます (test/tools/dist-zip/README.md)。zip の作り方と中身は、docs/release.md の「配布用の zip を作るワークフロー」と「配布用の zip の中身」にあります。

## lint

lint は test/lint にあり、Python 3 の標準ライブラリだけで動きます。リポジトリのルートで、次の形で実行します。

```
python test/lint/<lint の名前>.py
python test/lint/<lint の名前>.py --selftest
```

問題が見つかると、終了コード 1 で終わります。`--selftest` は、その lint が NG 例を検出し、OK 例を検出しないことを確認します。lint の一覧と引数は、docs/testing.md の「lint とセルフテスト」にあります。

## CI

CI のワークフローは .github/workflows にあります。プルリクエストと main へのプッシュでは、checks.yml が OBS のプラグインと送信側をビルドして単体テストを実行し、lint.yml が lint とツールのセルフテストを実行します。ワークフローの一覧は、docs/testing.md の「テストの種類と実行する環境」にあります。

OBS と Vesktop を起動して確認する実機テストは、CI では実行しません。実機テストと、そこで使うテスト用の送信側は、docs/testing.md の「実機テスト」と「テスト用の送信側」にあります。

## コーディング規約

コメント、名前、ログ、ツールの出力メッセージの書き方と用語は、CODESTYLE.md に従います。主なルールは次のとおりです。

- 識別子は英語、コメントとテストの名前は日本語で書きます。
- ファイルは UTF-8 で、改行は LF にします (.editorconfig と .gitattributes)。.ps1 は BOM 付きの UTF-8 にします。
- C++ のコードは、src-obs/.clang-format に合わせて整形します (clang-format 16 以降)。

画面の文言 (src-obs/data/locale の ja-JP.ini と en-US.ini) と手順書 (guide/ja.md と guide/en.md) は、docs/ui_guidelines.md に従って、日本語と英語の両方を書きます。

## コミットメッセージ

件名は「<分野>: <変更内容>」の形で、日本語で書きます。

- 分野は変更した場所を表す語で、下の表から選びます。
- 件名は 72 桁までにします。全角の文字は 2 桁に数えます。
- 件名を句点で終えません。
- 本文を書くときは、件名の後に空行を置きます。

| 分野 | 対象 |
|---|---|
| OBS プラグイン | src-obs (受信側) |
| 送信側 | src-vencord |
| テストツール | test/tools (test/tools/dist-zip を除く) と test/fixtures |
| lint | test/lint |
| CI | .github/workflows |
| 配布 | 配布用の zip の中身 (dist/licenses と dist/ソースコードの入手先.txt) と、zip を作るツール (test/tools/dist-zip) |
| 手順書 | guide/ja.md と guide/en.md |
| ドキュメント | docs と、README.md や CODESTYLE.md などのルートの文書 |
| リポジトリ | ルートの設定ファイル (.gitignore や package.json など) と .github/ISSUE_TEMPLATE |

件名の例: `ドキュメント: CONTRIBUTING.md に単体テストの実行方法を書く`

件名に日本語があることは、CI で test/lint/commit_messages_japanese.py がチェックします。

利用者に関わる変更は、CHANGELOG.md にも書きます (docs/release.md の「変更履歴の書き方」)。

## リリースと依存関係の更新

バージョンの付け方とリリースの手順は、docs/release.md の「バージョン番号の付け方」と「リリースの手順」にあります。OBS、Vencord、Vesktop などの依存関係を更新する手順は、docs/release.md の「依存関係を更新する手順」にあります。

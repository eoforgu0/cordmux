# テストと動作確認

cordmux のテストは、CI で実行するもの (単体テスト、lint、ツールのセルフテスト、ビルドのチェック) と、Windows の PC で実行するもの (ドックを操作するテスト、プレビューツール、実機テスト) に分かれる。この文書は、テストの種類と実行のしかた、リリースの前に確認する項目、リポジトリにあるテストデータを書く。テストを実行するとき、テストを足すとき、リリースの前に読む。

## テストの種類と実行する環境

| 種類 | 場所 | 実行する環境 | 合否 |
|---|---|---|---|
| C++ の単体テスト | `src-obs/tests` | CI と、ローカルの Windows | あり |
| TypeScript の単体テスト | `src-vencord/tests`、`test/tools/obs-run`、`test/tools/harness`、`test/tools/composite-check` の `*.test.ts` | CI と、ローカル | あり |
| lint とセルフテスト | `test/lint` | CI と、ローカル | あり |
| ツールのセルフテスト | `test/tools` | CI と、ローカル | あり |
| Vencord のビルドのチェック | `.github/workflows/vencord-plugin.yml`、`test/tools/vencord/check_dist.py` | CI | あり |
| 配布用の zip のチェック | `test/tools/dist-zip/check_dist_zip.py` | CI | あり |
| Discord の内部への依存箇所のテスト | `src-vencord/tests/discord_contacts.test.ts` | CI (週 1 回) と、ローカル | あり |
| ドックを操作するテスト | `src-obs/tests/dock_drive` | ローカルの Windows | あり |
| プレビューツール | `src-obs/tests/ui_preview`、`src-obs/tests/decor_preview` | ローカルの Windows | なし (画像を人が見る) |
| 実機テスト | `test/tools/obs-run` | GPU のある Windows の PC | あり (判定スクリプト) |

GitHub がホストする CI のランナーには GPU がないので、ハードウェアのエンコーダを使うテストは CI で実行しない。OBS と Vesktop を起動して映像と音声を流すテストは、GPU のある PC で実機テストとして実行する (「実機テスト」)。

CI のワークフローは次の 6 つである。

| ワークフロー | 実行するとき | 実行するもの |
|---|---|---|
| `checks.yml` | main へのプッシュと、プルリクエスト | `obs-plugin.yml` と `vencord-plugin.yml` を呼ぶ。配布用の zip は作らない |
| `lint.yml` | main へのプッシュと、プルリクエスト。タグのプッシュのときは `dist.yml` が呼ぶ | lint とセルフテスト、ツールのセルフテスト (ジョブは「lint とセルフテスト」) |
| `obs-plugin.yml` | ほかのワークフローが呼ぶ | OBS のプラグインのビルドと、C++ の単体テスト |
| `vencord-plugin.yml` | ほかのワークフローが呼ぶ | Vencord と送信側のビルド、ビルドのチェック、TypeScript の単体テストと型チェック |
| `dist.yml` | main へのプッシュ、`v` で始まるタグのプッシュ、手動 | 配布用の zip を作ってチェックする (docs/release.md の「配布用の zip を作るワークフロー」) |
| `discord-contacts.yml` | 週 1 回、手動、依存箇所に関わるファイルを変えるプルリクエスト | Discord の内部への依存箇所のテスト |

## C++ の単体テスト

C++ の単体テストは doctest で書き、実行ファイルを 2 つ作る。

| 実行ファイル | 中身 | 依存 |
|---|---|---|
| `align_tests.exe` | `src-obs/tests/test_*.cpp` の全部 | libobs と Qt を使わない。FFmpeg (obs-deps)、websocketpp、asio、nlohmann/json を使う |
| `decor_tests.exe` | `src-obs/tests/decor/` (名前ラベルと話している人の光の描画) | Qt6::Gui を使う |

2 つに分けるのは、表示の規則と、その入力を作る処理を、Qt を起動せずに確認するためである。同じ実行ファイルに Qt を入れると、Qt に依存する判断が混ざっても気付けない。libobs に依存するコード (`src-obs/plugin-main.cpp`、`src-obs/pipeline/pipeline.cpp`、ドックのウィジェットなど) は、どちらにも入らない。

テストのファイル名は、テストするファイルから決める (CODESTYLE.md の「ファイル」)。`align_tests.exe` は、次のフォルダーのコードを確認する。

| フォルダー | テストのファイル |
|---|---|
| `src-obs/align` | `test_timestamp_aligner.cpp`、`test_av_offset.cpp`、`test_align_audio_drift.cpp`、`test_align_audio_drift_ops.cpp` |
| `src-obs/bundle` | `test_bundle_*.cpp` |
| `src-obs/config` | `test_config_*.cpp` |
| `src-obs/decode` | `test_decode.cpp` |
| `src-obs/diag` | `test_diag_log.cpp` |
| `src-obs/dock` | `test_dock_*.cpp` (Qt を使わない部分だけ。下の表) |
| `src-obs/install` | `test_install_*.cpp` |
| `src-obs/ledger` | `test_ledger_*.cpp` |
| `src-obs/pipeline` | `test_pipeline_*.cpp` (libobs を使わない部分だけ) |
| `src-obs/platform` | `test_platform_*.cpp` |
| `src-obs/record` | `test_record_*.cpp` |
| `src-obs/sources` | `test_sources_*.cpp` (libobs を使わない部分だけ) |
| `src-obs/ws` | `test_ws_*.cpp` |

`src-obs/ws` のテストは、127.0.0.1 で実際に待ち受け、生の TCP のクライアントと websocketpp のクライアントから、不正なハンドシェイクと WebSocket の不正なメッセージも送る。

ドックは、次のように分けて確認する。ドックに何を表示するか (ドックのメッセージ、参加者の一覧、ボタンを押せるか) は表示の規則 (`src-obs/ledger`) が決め、`test_ledger_*.cpp` が確認する。

| 確認する部分 | テスト |
|---|---|
| 周期ごとに、Pipeline の状態、OS への問い合わせの結果、ドックの設定から、表示の規則の入力 (Observation) を作る (`src-obs/dock/build_observation.h`) | `test_dock_build_observation.cpp` |
| 自分の行のチェックを Pipeline に渡して答えを受ける所: Pipeline が自分の枠を使い始めることを断ったときに、チェックと配置の入力から自分の枠を外して settings.json に残すこと、[自分の枠に映す映像] を替えたときの引き継ぎ (`self_frame_check.h`)。Pipeline の代わりに、割り当て表と同じロックの中で呼ぶ関数を挟む | `test_dock_self_frame_check.cpp` |
| [Vesktopに追加] で、ほかの Vencord から切り替える確認のダイアログを表示する条件、[再インストール] のコピー、[Vesktopから削除] の結果をドックに残す失敗に変える表、アンインストールの結果と残ったものの並び、確認と結果のダイアログの中身 (`install_flow.h`、`install_dialog_plan.h`) | `test_dock_install_flow.cpp` |
| 参加者の一覧の行: 名前の見分け方、2 行目に残す部分、状態のテキストと解像度を表示する幅の境目、名前と状態のテキストの幅の分け方、クリックを無視する時間 (`name_check.h`) | `test_dock_rows_calc.cpp` |
| settings.json のキーごとに、値を変更できる場所 (設定ウィンドウ、ドックなど) を決めた表が、すべてのキーを持つこと。設定ウィンドウがどの値をどのページに置き、どの値の変更に再起動が必要か (`settings_fields.h`) | `test_dock_settings_fields.cpp` |
| コントラスト比、メッセージのレベル 1 のパネルの枠線、テキストを省略するときと折り返すときの位置、余白、ダイアログの主文の折り返しと高さ (`src-obs/dock/ui/dock_ui_calc.h`、`dock_metrics.h`、`text_arg.h`) | `test_dock_ui_calc.cpp` |
| 設定ウィンドウの計算のうち、Qt に依存しない部分 (`windows_calc.h`) | `test_dock_windows_calc.cpp` |
| ドックと設定ウィンドウとダイアログのウィジェットを、状態ごとに描画した見た目 | プレビューツール (`ui_preview`) |
| ウィジェットに入力を送ったときの動き (押し間違い、キーボードの操作、フォーカス、ダイアログの親ウィンドウ) | ドックを操作するテスト (`dock_drive`) |

ビルドと実行のコマンドは、CONTRIBUTING.md の「C++ の単体テスト」にある。

- テストは、製品の構成 (`windows-x64`) が `src-obs/.deps` に取得した obs-deps の FFmpeg と Qt6 を使う。CI の `obs-plugin.yml` は、ビルドのジョブが作った `src-obs/.deps` のキャッシュを、単体テストのジョブで復元する。
- `tests-x64` のプリセットは Visual Studio 2022 のジェネレーターを使う。ほかのバージョンの Visual Studio を使うときは、`src-obs/tests` に CMakeUserPresets.json (Git の管理の外) を作って `tests-x64` を継承したプリセットを書き、ジェネレーターを変える。
- ビルドは、`align_tests.exe` の隣に、cordmux.dll が読み込むのと同じ 8 つの FFmpeg の DLL (一覧は `src-obs/cmake/cordmux_ffmpeg_runtime.cmake`) をコピーする。`decor_tests.exe` の隣には Qt6Core.dll と Qt6Gui.dll を、同じフォルダーの `platforms` の下には、ウィンドウを開かない Qt のプラットフォームプラグイン (minimal) をコピーする。

実際の FFmpeg を使うテスト:

- `test_record_io_finalize.cpp` の全部と、`test_record_io_interrupted.cpp` と `test_record_io_phases.cpp` の一部は、実際の ffmpeg.exe で録画ファイルを作成する。できたファイルを ffprobe.exe で読み、長さ、フレームの数、サンプルの数を、作成の計画と比べる。
- このテストは、環境変数 `CORDMUX_FFMPEG_EXE` と `CORDMUX_FFPROBE_EXE` が、obs-deps の ffmpeg.exe と ffprobe.exe を指すときだけ実行する。指さなければ (ファイルがないときも)、doctest がスキップする。ほかの ffmpeg では代わりにならない。配布用の zip に入れる ffmpeg.exe と同じビルドで確認するためである。
- CI の `obs-plugin.yml` の単体テストのジョブ (windows-2022) は、この 2 つの環境変数を設定し、doctest の結果の行に `0 skipped` がなければ失敗にする。環境変数を設定し忘れて、このテストをスキップしたまま成功にしないためである。
- ローカルで設定するコマンドは、CONTRIBUTING.md の「C++ の単体テスト」にある。

一部のテストだけを実行するときは、doctest のオプションを使う。

| オプション | 意味 |
|---|---|
| `-ltc` | テストの名前を一覧にするだけで、実行しない |
| `-tc="<パターン>"` | 名前がパターンに一致するテストだけを実行する |
| `-sf="<パターン>"` | ファイルの名前がパターンに一致するテストだけを実行する |

日本語を含む名前のテストを選ぶときは、`-sf` でファイルを選ぶか、名前の ASCII の部分をパターンに使う。Windows では、テストの実行ファイルが受け取るコマンドラインの引数は、ANSI コードページ (日本語の Windows では CP932) の文字列になる。テストの名前は UTF-8 なので、日本語を含む `-tc` のパターンは、ANSI コードページが UTF-8 でない PC では、どのテストにも一致しない。

- テスト「fixture ごとの整列の結果を表の形で出力する」(`test_timestamp_aligner.cpp`) は、タイムスタンプの列 (fixture) ごとに、整列の統計の表を標準出力に書く。何も確認しないテストで、値を見るためにある。`-tc="fixture*"` で、このテストだけを実行できる。
- `test_align_audio_drift.cpp` の「10 時間の模擬」のテストは、音を取り出す時計の速さの差、時刻の揺れ、上流の段、受信の途切れを入れた 10 時間分を、配信の経路と録画の経路の両方で流し、音と映像のずれの表 (終わり - 始め、最大の飛び、置き直しの回数) を標準出力に書く。`-sf="*test_align_audio_drift*"` で、このファイルのテストを実行できる。
- `test_align_audio_drift_ops.cpp` は、音の変換の計算の 2 つの実装 (x64 の既定の SSE2 の計算と、組み込み関数を使わない予備の計算) を同じ入力で比べる。予備の計算は製品のビルドでは使われないので、ここでコンパイルして確かめる。書き出す位置と数は完全に一致すること、値の差は 32 個の積の和を足す順による丸めの誤差の上限 (入力の大きさの約 9e-6 倍) 以内であることを確かめる。
- 製品の経路を通す長い模擬は、音の時計が +-500 ppm ずれたチャンクを、実物の `AudioMixer` (`test_pipeline_audio_mixer.cpp`)、実物の `place_audio` (`test_record_audio.cpp`)、`Recorder` の音声の書き込み (`test_record_io_recorder.cpp`) に流す。`AudioMixer` では、画面の音は `screen_audio_at_us`、声は `voice_audio_at_us` で置く時刻を作る。音に 1 秒ごとの目印のパルスを入れ、出した音か書いた音の中のパルスの時刻を測って、表を標準出力に書く。
- 時間を測るテスト (`test_align_audio_drift.cpp` の 480 サンプルのチャンクを 1 つ置く時間、`test_pipeline_audio_mixer.cpp` の 30 組を混ぜる時間) は、Release の値を表で出し、値では合否を決めない。

`decor_tests.exe` は、名前ラベルを描画した画像を、期待する画像 (`src-obs/tests/snapshots` の PNG) と比べる。フォントと dpi を固定して、同じ入力から同じピクセルが出るようにしてあるので、違いを許すしきい値は置かない。失敗したときは、期待、実際、差分の 3 つの画像を書き出す。期待する画像を作り直すときは `--cordmux-snapshot-update` を付けて実行する。このオプションを付けた実行は成功で終わらないので、続けてオプションなしで 1 回実行して確認する。

## TypeScript の単体テスト

TypeScript の単体テストは vitest で書く。リポジトリのルートで実行する。

```
npm ci
npm test              # vitest run
npm run typecheck     # tsc --noEmit
```

- `vitest.config.ts` が、`src-vencord/tests/**/*.test.ts`、`test/tools/obs-run/*.test.ts`、`test/tools/harness/*.test.ts`、`test/tools/composite-check/*.test.ts` を対象にする。
- `src-vencord/tests` のファイル名は `<フォルダー>_<ファイル>.test.ts` にする。
- `test/tools/obs-run/*.test.ts` は、実機テストの実行計画を作る純粋関数 (`run-window-plan.mjs`) と、実機テストのツールを確認する。`ci-selftests.test.ts` は、セルフテストを持つ `test/tools/obs-run` のツールを `lint.yml` が全部実行していること (実行しないものは、理由と一緒に除外の表に載っていること) を確認する。
- `test/tools/harness/*.test.ts` は、テスト用の送信側の純粋関数を確認する。
- `test/tools/composite-check/*.test.ts` は、合成のソースの描画を比べるツール (`run-check.mjs`) の、OBS を起動しない部分を確認する。
- `tsconfig.json` の `exclude` に入れた 5 つのファイル (Vencord を import するファイルと、ブラウザーにしかない API を使うファイル) は、リポジトリのルートの tsc ではなく、`vencord-plugin.yml` が Vencord のソースツリーの中で `pnpm test:ts` を実行して型をチェックする (ローカルで実行する手順は下にある)。どのファイルをなぜ外すかは、`src-vencord/README.md` の「型のチェック」にある。
- `audio/track_source.ts` は `exclude` に入っているが、`src-vencord/tests/audio_track_source.test.ts` が import するので、ルートの tsc もチェックする。このテストは、`MediaStreamTrackProcessor` を `vi.stubGlobal` でテスト用の実装に置き換えて、vitest で実行する。`encode/track_source.ts` を読み込むテストはない。
- `exclude` の 5 つを除く `src-vencord` のファイルは、Vencord も DOM の型も使わないので、テストが Node で直接読み込める。

ローカルで `pnpm test:ts` を実行するときは、CI (`vencord-plugin.yml`) と同じく、`src-vencord/vencord.lock.json` のコミットの Vencord を取得し、その `src/userplugins/cordmux` に `src-vencord` をコピーする。Git Bash で、リポジトリのルートから次のように実行する。

```
vencord='<Vencord のフォルダー>'
commit=$(python -c "import json; print(json.load(open('src-vencord/vencord.lock.json', encoding='utf-8'))['commit'])")
git init "$vencord"
git -C "$vencord" fetch --depth 1 https://github.com/Vendicated/Vencord.git "$commit"
git -C "$vencord" checkout FETCH_HEAD
(cd "$vencord" && corepack enable && corepack prepare pnpm@12.15.0 --activate && pnpm install --frozen-lockfile)

# src-vencord を変えるたびに、ここから実行する
dest="$vencord/src/userplugins/cordmux"
rm -rf "$dest"
mkdir -p "$dest"
cp -r src-vencord/. "$dest/"
rm -rf "$dest/vencord.lock.json" "$dest/tests"
(cd "$vencord" && pnpm test:ts)
```

- `vencord.lock.json` はビルドの手順だけが読むファイルで、`tests/` はリポジトリのルートの vitest で実行するテストなので、CI と同じくコピーしない。
- コピーは毎回作り直す。前のコピーが残っていると、変える前のファイルをチェックして成功することがある。
- Vencord を取得するフォルダーには、短いパスの空のフォルダーを使う。長いパスの下では、`pnpm install` で、パスの長さが Windows の上限 (260 文字) を超えて失敗することがある。
- 上の手順は、指定したフォルダーのコミットを切り替えて、送信側のコピーを置く。Vesktop が読んでいる Vencord のソースツリー (Vesktop の state.json の `vencordDir` が、その `dist` を指しているもの) を指定すると、そのツリーで次にビルドしたときに、Vesktop が読む Vencord が変わる。

Discord の内部への依存箇所:

- 送信側が読む Discord の内部の部分は、依存箇所の一覧 (`src-vencord/ledger.ts`) にある。
- `src-vencord/tests/fake_adapter.ts` は、依存箇所を 1 つずつ読めなくしたテスト用の adapter を作る。`fake_adapter.test.ts` は、読めなくした依存箇所の id が warnings に入ることと、すでに送っている映像と音声を止める判断が起きないことを確認する。`fake_adapter_count.test.ts` と `ledger.test.ts` は、一覧の行の数と並びが期待どおりかを確認する。一覧の行が減ったときに、テストの数も同じだけ減って、成功のままになることを防ぐためである。
- `discord_contacts.test.ts` は、Discord が今配っている JavaScript のバンドルに対して、依存箇所の一覧の各行がちょうど 1 つのモジュールに一致するかを確認する (複数の一致を許す行が一部にある)。バンドルは `test/tools/discord-bundle/fetch.mjs` が取得してローカルに保存し、リポジトリには入れない (取得したビルドの一覧だけを `test/fixtures/discord-bundles/manifest.json` に書く)。バンドルがなければ、このテストはスキップする。
- `discord-contacts.yml` は、バンドルを取得してこのテストを実行する。main のチェックと配布用の zip には入れない。このワークフローの失敗は、cordmux のビルドが壊れたことではなく、Discord の内部が変わったことを伝えるものだからである。

## lint とセルフテスト

`test/lint` の lint は、Python 3 の標準ライブラリだけで書く。

```
python test/lint/<lint>.py              # 既定の対象 (多くはリポジトリ全体) をチェックする
python test/lint/<lint>.py <パス>...     # 指定したファイルとフォルダーだけをチェックする
python test/lint/<lint>.py --selftest   # セルフテスト
```

検出がなければ 0、あれば 1 で終わる。引数の形が違うものがある。`commit_messages_japanese.py` はコミットの範囲、`locale_ids.py` はリポジトリのルートとみなすフォルダー、`removal_table.py` はヘッダーのファイルを受け取る。詳しくは `test/lint/README.md` にある。

| lint | 検出するもの |
|---|---|
| `identifiers_english.py` | コード (文字列とコメントを除いた部分) の、ASCII でない文字 |
| `comments_japanese.py` | 日本語を含まない、英語だけのコメント |
| `commit_messages_japanese.py` | 件名に日本語を含まないコミット |
| `windows_api_confinement.py` | `src-obs/platform` と `src-vencord/paths.ts` の外にある、Windows の名前 (`windows_names.txt`) |
| `forbidden_libobs_usage.py` | 使わないと決めた libobs の使い方 (`forbidden_libobs_names.txt`。ソースの音量とミュートを書き換えるなど) |
| `forbidden_qt_usage.py` | 使わないと決めた Qt と Windows の名前 (`forbidden_qt_names.txt`。音の出るメッセージボックスなど) と、スタイルシートの書き方 |
| `locale_ids.py` | コードの文字列の ini のキー (`cordmux.` で始まるもの) と、2 つの ini (`src-obs/data/locale`) のキーの過不足。空の値、重なったキー、言語によって違う `%N` |
| `locale_shapes.py` | ini のキーの最後の部分 (`.button`、`.status` などの種類) と、値の形 (句点、疑問符、「...」) の食い違い |
| `forbidden_ui_words.py` | ini の値の、使わない記号、長音と OBS が表示する名前の表記の誤り、空白の誤り (`forbidden_ui_words.txt`) |
| `log_lines.py` | OBS のログに書く行の書式の誤り (CODESTYLE.md の「OBS のログ」) |
| `install_guide.py` | 手順書 (`guide/ja.md` と `guide/en.md`) の誤り。規則は `install_guide_rules.txt` にある (見出しの並び、使える Markdown の記法、手順の部分の禁止語、番号の数、`[ ]` の名前と ini の照合、ini と同じでなければならない文、決まった文、見出しへのリンク、ini から手順書の見出しへの参照など) |
| `removal_table.py` | アンインストールで消すものの表 (`src-obs/install/removal_table.h`) を、Python が文字列として読めない形 |
| `references.py` | リポジトリにないファイルと文書の見出しへの参照 |

どの lint も、セルフテストの NG 例と OK 例を `test/lint/fixtures/<lint>/` に持つ。`--selftest` は、NG 例が検出され、OK 例が検出されないことを確認する。どちらかが外れたとき、または例が 1 つもないときは、セルフテストが失敗する。検出の仕組みが壊れて、常に成功を返す状態になったことに気付くためである。

- 外部からコピーしたファイルは、`test/lint/vendored.txt` に並べて、一部の lint (`identifiers_english.py`、`comments_japanese.py`、`windows_api_confinement.py`、`forbidden_libobs_usage.py`) の対象から外す。
- `references.py` は CI では実行しない。
- `commit_messages_japanese.py` に渡すコミットの範囲は、プルリクエストでは `origin/<base>..HEAD`、プッシュでは `<プッシュの前のコミット>..<プッシュしたコミット>` (前のコミットがないときは `HEAD~10..HEAD`) である。

`lint.yml` のジョブ:

| ジョブ | ランナー | 実行するもの |
|---|---|---|
| `lints` | ubuntu-24.04 | `test/lint` の 12 の lint とそれぞれの `--selftest`、`test/tools/wording/apply_ja_style.py --selftest`、`test/tools/obs-run/tool_text_ids.py` |
| `judges-selfcheck` | ubuntu-24.04 | 判定スクリプトのセルフテスト。ffmpeg (Ubuntu のパッケージ) と numpy を入れて実行する |
| `dist-tools-selftest` | ubuntu-24.04 | 配布用の zip とリリースを作るツールのセルフテストと、`test/tools/notice-count/notice_count.py --selftest` |
| `cleanup-selftest` | windows-2022 | `test/tools/obs-run/cleanup.selftest.ps1` |
| `windows-tools-selftest` | windows-2022 | 実機テストのツールのセルフテストと、`test/tools/obs-run` の `.ps1` の構文のチェック |
| `harness-bundle` | ubuntu-24.04 | テスト用の送信側のバンドルと型チェック、実機テストの実行スクリプトの構文のチェック |

ツールのセルフテスト:

`test/tools` のツールのうち、合否を出すものの多くは、`--selftest` (PowerShell は `-SelfTest`) を持つ。わざと壊したデータを作り、ツールがそれを検出することを確認する。

| 対象 | ジョブ | 主なツール |
|---|---|---|
| 判定スクリプト | `judges-selfcheck` | `selfcheck.py` (`frames.py`、`sync.py`、`smooth.py`、`analyze.py` をまとめて確認する)、`bit_strip.py`、`ledger_check.py`、`black_span.py`、`au_hash.py`、`tone_scan.py`、`plate_measure.py`、`border_timing.py`、`latency.py` |
| 配布用の zip とリリースのツール | `dist-tools-selftest` | `test/tools/dist-zip` の 5 つ (`build_info.py`、`make_dist_zip.py`、`check_dist_zip.py`、`fetch_sources.py`、`make_release_notes.py`) |
| 後片付けのスクリプト | `cleanup-selftest` | `cleanup.selftest.ps1` |
| 実機テストのツール | `windows-tools-selftest` | `seed-config.py`、`install_snapshot.py`、`newer_ver_dll.py`、`summarize-window.py` と、`.ps1` の 5 つ |
| Vencord のビルド | `vencord-plugin.yml` | `test/tools/vencord/check_dist.py` |

- `judges-selfcheck` は Ubuntu のパッケージの ffmpeg (6.1 系) を使う。実機テストでは、録画の判定スクリプトに obs-deps の ffmpeg を `--ffmpeg` で渡す。
- `a11y-record.ps1 -SelfTest` は CI で実行しない。ウィンドウを表示しない子のプロセスから UI Automation のイベントが届くまで待つ形で、GitHub のランナーのセッションでイベントが届くかを確認していないためである。

## ドックを操作するテスト

`src-obs/tests/dock_drive` の `dock_drive.exe` は、製品のドック、参加者の一覧、設定ウィンドウ、ダイアログをそのまま組み込み、入力と状態の変化を流して、押し間違い、ウィンドウの移り変わり、フォーカスを数える。数が期待と違えば失敗にする。入力は、ポインターの移動、ボタンを押す操作と離す操作、キー、ホイール、メニューである。状態の変化は、人の出入り、名前の変更、ドックのメッセージの表示と消去、録画の段階である。

- 画面を開かない。Qt のプラットフォームプラグインを、OS のウィンドウを作らないプレビュー用のもの (`src-obs/tests/ui_preview/preview_platform.cpp` の `cordmuxpreview`) に決め、ほかのプラグインで起動したときは何もせずに終わる。
- 入力は Qt の `QWindowSystemInterface` に渡すだけで、OS の入力 (実際のマウスとキーボード) には何も送らない。
- アクセシビリティのイベントはテストが受け取るので、Windows のスクリーンリーダーには届かず、音も出ない。
- OBS、Vesktop、Discord を起動しない。ファイルは `--dir` のフォルダーにだけ書く。

```
dock_drive.exe --dir <出力先> [--scenario <シナリオ>,...] [--seeds 1,2,3,4,5] [--minutes 10]
               [--theme yami-classic] [--lang ja-JP] [--font-scale 10] [--density -2]
```

| シナリオ | 確認すること |
|---|---|
| `probe` | 入力が製品のウィジェットに届くか (既定のシナリオ) |
| `keyboard` | Tab キーでフォーカスが順に移ること、フォーカスリングの見え方、キーボードでの操作 |
| `windows` | 設定ウィンドウの [適用] と [キャンセル]、再起動が必要な設定の表示、診断情報のエクスポートの流れ、保存に失敗したときのダイアログ |
| `record` | 録画のボタンの見た目が、録画の側の状態の変化に合っているか |
| `a11y` | スクリーンリーダーに読み上げさせるイベントの数と内容 |
| `menus` | ドックのメニューと参加者の行のメニューの項目、UI Automation で項目を実行したときにメニューが閉じて操作が実行されるか、時間のかかる操作のボタンが処理中の見た目になるまでの時間、行のメニューの [全画面で表示] と 1 行目の「全画面: 名前」と [元に戻す] (押せる行と押せない行、チェック、画面共有をやめたときに戻るか) |
| `layout` | 状態タブのメッセージの並び、ドックの大きさを変えた直後に押した操作が別の人に届かないか、メッセージのラベルの高さが 0 になっても落ちないか |
| `dialogs` | 確認、結果、失敗のダイアログの親ウィンドウ、既定のボタン、閉じた後のフォーカス |
| `summary` | 平常の 1 行 (ドックのメッセージがないときの 1 行目) の「表示中」の人数が、規則で数えた人数と同じか |
| `rowaudio` | 参加者タブの人の行のミュートのボタン (声と画面共有の音) |
| `audio` | 音声タブ。メーターを読む周期が参加者タブと音声タブが見えている間だけ動くこと、つまみの上のホイールで値が変わらずページがスクロールすること、ダブルクリックで 100% に戻ること、つまみがクリックでフォーカスを取り、矢印、PageUp、Home、End で動くこと、つまみを動かすと音量の操作を実行すること (動かす間は間引き、離したときに最後の値を送る)、1 人の高さと位置が共有の有無と声が届いているかで変わらないこと、ミュートのボタン、メーターの色 (ミュート中と音量 0 は掛ける前の値を灰色で出す) と、値が変わったメーターだけを塗り直すこと、つまみをドラッグしている間に行が動かないこと、接続が切れたとき、合成のソースがないとき、行がないときの表示、Tab の順 |
| `selfframe` | 自分の行のチェックで自分の枠を付け外しできること (枠が埋まっているときは付けられない)、自分の共有の始まりでは、ほかの人と同じ規則 (枠に載せる人の選び方が自動) のときだけ自分の枠が付くこと (共有の終わりでは外れない。ドックで外した後は始め直すまで付かない)、共有の状態が分からない間は変わらないこと、映像を OBS から選んでいない間は、サブアカウントがあればチェックがほかの人と同じ意味で、サブアカウントがなければ押せず理由が付くこと、サブアカウントがない構成の自分の行に [自動で枠に載せる] だけのメニューが出ること、自分の枠を使っている間 (選んだ映像が見つからない間も) は Discord の人の 4 人目のチェックを押せず、自分の行のチェックを外すと押せること |
| `look` | ドックの表示タブ。入力欄が設定の形で値を書けること (刻みと範囲の境、手で書いた桁の多い値を別の入力欄の操作で書き換えないこと)、押せなくする条件、並びの選択肢の制限、ソースと太さの選択肢。ドックの中では、値を変えると保存の前に配信画面へ渡すこと、保存が最後の変更の 1 秒後に 1 回にまとまること、ホイールでつまみ、スピンボックス、コンボボックスの値が変わらずページがスクロールすること、設定ウィンドウの [適用] とメインアカウントの [変更] が表示タブの値を戻さないこと、ダミー映像の切り替えで 1 行目が変わること、値を変えても 1 行目、メインとサブの行、タブのバー、下のバーの位置が動かないこと、外から届いた値の追従、OBS を閉じるときの保存、色のダイアログが止める範囲、つまみがクリックでフォーカスを取り、矢印、PageUp、Home、End で動き、ダブルクリックでその設定の既定値に戻ること |
| `status` | ドックの状態タブ。メッセージがないときの「ありません」と、メッセージのカードの見た目 (パネルの色の背景と左端の線で、枠線と赤い背景がないこと)、映像のバッファ (つまみのダブルクリックで既定値に戻ること、変えるとすぐ表の遅延を計算し直し、入力が止まってから配信の処理へ 1 回だけ渡すこと、つまみを動かしている間と数を打っている途中は渡さないこと、保存が 1 回にまとまること、設定ウィンドウの [適用] と OBS を閉じる処理で戻らないこと、外から届いた値の追従)、映像途切れの累計 (リセットと配信の開始でゼロに戻ること、抜けた人の行が残ること)、履歴の順序と 60 行の上限、ホイールでタブがスクロールして値が変わらないこと、最小の幅で横にはみ出さないこと、並びの保留の間に座標が動かないこと、1 行目を押すと状態タブが開いてレベル 2 が既読になること、Vesktop の起動中は参加者タブの [Vesktopに追加] を押せず、押せない理由をボタンの下に文で出すこと (終了すると押せる)、状態タブの「Vesktop」の組は Vesktop の起動中と起動しているかを読めていない間は出さず、終了すると現れること (Vesktop の設定を書かない起動では [Vesktopから削除] を押せず理由を文で出し、[Vesktopから削除] は確認せずに実行し、外れた後は組を出さない) |
| `positions` | 配信者にとっていちばん悪い条件で、押す場所が動かないこと。ドックにメッセージを出すすべての場面 (ui_preview の場面から機械的に集める) で、1 行目、メインとサブの行、タブのバー、下のバーの座標が、メッセージのない同じ構成の場面と同じであること (参加者の一覧が同じ場面は行の座標も)。1 人の「映像途切れ」が 20 回出入りする間、座標が動かず、1 行目がレベル 1 の見た目にならず、状態タブの累計が 20 回になること。30 人の参加者タブ、音声タブ、状態タブで、横にはみ出さず、スクロールで全員に届き、ホイールで値が変わらないこと。235x300 で、幅が最小の幅になり、一覧の高さが 0 にならず、タブと下のバーを押せること。メッセージのレベル 1、2、3 が重なるとき、1 行目は 1 つだけで、状態タブにすべてあること。最後に、ドックの更新 1 回の時間 (build_observation、decide_dock、ドックへの反映の合計。4、8、30 人) を、中央値、95 パーセンタイル、最大で info に出す (合否はない。`Pipeline::snapshot()` は dock_drive では測れないので含まない) |
| `framework` | ドックの枠組み。メッセージが出ても消えても 1 行目、メインとサブの行、タブ、下のバーと行が動かないこと、1 行目を押すと状態タブが開くこと、タブの切り替え、押した場所の隣の確認、入れ替える相手のメニュー、組の間の移動の保留と反映 |
| `tools` | OBS の [ツール] からアンインストールを始めたときの流れ |
| `pages` | 設定ウィンドウのページの一覧 (4 ページ) とフォーカス、UI Automation でページを選んだときにそのページが表示されるか |
| `mistap` | 押し間違いの数 (下の説明) |
| `dispatch` | 表示を変える処理の途中で、ポインターの下のボタンを消さないか |
| `unload` | ウィンドウを開いたまま後片付けをしても、クラッシュしないか |

`mistap` は、8 人の参加者がいる一覧の状態 (人の出入り、名前の変更、画面共有の開始と終了、ドックのメッセージ) を、平均 1.8 秒ごとに乱数で変えながら、人の操作を粗くまねた入力を流す。シード 5 つで 10 分ずつ実行する。次の数がすべて 0 なら合格である。

- 押した瞬間にポインターの下にあった行とは別の人に、操作が適用された回数
- 押す前の 500 ms の間に、押した位置の行が入れ替わっていて、そこで押した操作が適用された回数
- フォーカスのある人が通話から抜けた後に押したキーが、別の人に適用された回数
- 表示が、表示の規則が決めた内容と違うまま 500 ms を超えた回数
- 状態を変えない区間に、ログの行とドックのメッセージの出入りが書かれた回数

結果は `<出力先>/results.tsv` (1 つの確認に 1 行) に書く。終了コードは、すべて合格なら 0、失敗があれば 1、引数の誤りは 2、プレビュー用でないプラットフォームプラグインで起動したときは 3、テーマや ini の準備に失敗したときは 4 である。

`dock_drive.exe` は、`src-obs/tests` を CMake で構成するときに `CORDMUX_BUILD_PREVIEW_TOOLS` を `ON` にしたときだけビルドする (既定は `OFF`)。Qt の非公開の API (Qt6::GuiPrivate) を使い、CI の Visual Studio 2022 でビルドできるかを確認していないためである。10 分の実行を含むので、`add_test` にも入れない。ドックのポーリングの処理 (`DockPoll`) は libobs と Pipeline に依存するので組み込めず、`src-obs/tests/dock_drive/sim_poll.cpp` が同じ順の処理を代わりに行う。ビルドに使うプリセットは「プレビューツール」にある。

## プレビューツール

プレビューツールは、製品の描画のコードをそのまま使って、状態ごとの画面を画像に書き出すツールである。見た目と文言を、状態をすべて並べて人が確認するために使う。ウィンドウを 1 つも作らないので、実行中もほかの作業を妨げない。

| ツール | 書き出すもの |
|---|---|
| `ui_preview.exe` (`src-obs/tests/ui_preview`) | ドック、設定ウィンドウ、参加者の設定ウィンドウ、ダイアログ、メニューを、状態、幅、テーマ (OBS の Yami と Yami Light など)、言語ごとに描いた PNG と、ウィジェットの一覧 (`widgets.tsv`) |
| `decor_preview.exe` (`src-obs/tests/decor_preview`) | 合成のソース (cordmux) の名前ラベルと光を、製品と同じ関数で描いた PNG と、OBS の中で撮った画像と比べるための参照 |

- `ui_preview.exe` は、OBS 31.1.1 のテーマを OBS と同じ手順で適用する。テーマのファイルは、製品の構成が `src-obs/.deps` に取得した OBS のソースから読む。
- `ui_preview.exe` は、ドックの場面の値を、ドックを表示して初めての表示の読み直しが済んでから渡す。製品でも、通話の参加者の行やメッセージは、OBS がドックを表示した後に届いた値で作られる。表示する前に作った行は、その読み直しで測り直されるので、表示した後に作られた行だけに起きる測り違いが画像に出ない。
- `run_ui_preview.py` は、ビルドから実行までをまとめて行う。結果は、出力先の `index.html` で並べて見られる。
- `decor_preview.exe` は、ビルドしてから `--composite-sample` などの引数で直接実行する (使い方は `src-obs/tests/decor_preview/README.md`)。OBS の中で撮った画像との比較は、`test/tools/composite-check` が行う。
- `src-obs/tests/ui_preview/cvd_sheet.py` は、描いた画像を 1 型と 2 型の色覚の見え方と白黒に変換して並べ、変換した後のテキストと背景のコントラスト比を数える。

どちらのツールも、`CORDMUX_BUILD_PREVIEW_TOOLS` を `ON` にしたときだけビルドする (ドックを操作するテストと同じ理由)。`src-obs/tests/CMakePresets.json` の `tests-x64` はこの値を設定しないので、既定の `OFF` のままになる。そこで、`src-obs/tests` に CMakeUserPresets.json (Git の管理の外) を作り、`tests-x64` を継承して `ON` にしたプリセット `tests-x64-local` を書く。`run_ui_preview.py` は、既定でこの名前のプリセットを使う (`--preset` で変えられる)。ドックを操作するテストは、このプリセットで構成してから、`cmake --build --preset tests-x64-local --target dock_drive` でビルドする。

```json
{
  "version": 8,
  "configurePresets": [
    {
      "name": "tests-x64-local",
      "inherits": ["tests-x64"],
      "cacheVariables": {
        "CORDMUX_BUILD_PREVIEW_TOOLS": "ON"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "tests-x64-local",
      "configurePreset": "tests-x64-local",
      "configuration": "Release"
    }
  ]
}
```

- ビルドの構成は `Release` にする。`run_ui_preview.py` は、`build_x64/align-tests/Release` の実行ファイルを実行する。
- Visual Studio のバージョンが違うときは、同じプリセットに `generator` を書く (「C++ の単体テスト」)。
- `tests-x64-local` は `tests-x64` と同じフォルダー (`build_x64/align-tests`) を使う。`run_ui_preview.py` は、このフォルダーに CMake のキャッシュがあると `cmake --preset` を実行しない。`tests-x64` で構成したことがあるときは、初めの 1 回だけ `--reconfigure` を付けて実行する。付けないと、キャッシュの `CORDMUX_BUILD_PREVIEW_TOOLS` が `OFF` のままなので、ビルドが失敗する。

引数と出力の形は、それぞれのフォルダーの README.md にある。ドックのどの部分をどのテストで確認するかは、「C++ の単体テスト」の表にある。

## テスト用の送信側

`test/tools/harness` は、送信側の代わりに映像と音声を送るツールである。Discord にも Vesktop にも接続しない。動く Electron のバージョンと、Vesktop のどのバージョンと同じエンコーダを使うかは、`test/tools/harness/README.md` にある。

- product モードでは、canvas に描いた映像と正弦波の音声を、製品の送信側のコード (`src-vencord/transport/socket.ts`、`encode/session.ts`、`encode/serial.ts`、`audio/chunk.ts`、`core/wire.ts`) に流して、受信側に送る。`src-vencord/index.ts` は Vencord を import するので使わず、`index.ts` の処理のうち Vencord に依存しない部分を、`sender.ts` に同じように持つ。
- 映像のフレームには、フレームの番号を表すビットパターンと、5 秒ごとの同期用のマーカー (白いフレーム 3 つと、60 ms の 2 kHz の音) を入れる。判定スクリプトは、録画からこれを読んで、フレームの欠落とずれを数える。
- 画面共有と声の数、長さ、fps、エンコーダの選び方、エンコードの段階を強制的に下げること、送りすぎ、途中の解像度の変更、依存箇所を読めなくすること、映像のフレームだけを止めることを、環境変数で指定できる。名前と既定値は `test/tools/harness/README.md` にある。
- トークンファイルは、環境変数 `CORDMUX_HANDSHAKE_DIR` のフォルダーから読む。指定がなければ、実際の `%LOCALAPPDATA%\cordmux` を読まずに、接続しない。
- product モードでは、WebSocket の接続の Origin ヘッダーを `https://discord.com` にする (Vesktop の送信側が、discord.com のページの中で動くことに合わせる)。
- 結果 (接続できたか、捨てたフレームの数、ストリームごとのエンコードの段階、受信側から届いた recording、media、limits の変化など) を JSON に書く。
- `npm run build` で、`sender.ts` を esbuild で 1 つのファイルにバンドルする (clock-probe モードの `clock_probe.ts` も、別の 1 つのファイルにバンドルする)。

ほかのモード:

- tests: WebCodecs のハードウェアのエンコーダを同時に何本作れるかと、作れなかったときの失敗の形を測る。
- sustain: 決めた数のエンコーダで決めた時間エンコードを続け、毎秒の出力の数を測る。
- send: 製品の送信側のコードを使わずに、エンコードした映像と正弦波の音声を WebSocket で送る。
- clock-probe: 受けた映像と音声のタイムスタンプが何の時計かを、同じページの中の WebRTC の接続で確かめる。受信側には送らない。Chromium のバージョンが変わったときに使う。

ほかに、次のツールがある。

| ツール | 中身 |
|---|---|
| `test/tools/mic-check` | Chromium の起動のオプションで、音声のファイルをマイクの代わりに入力できるかを確認する |
| `test/tools/webcodecs-check` | エンコーダを作った直後の黒いフレームの遅延と、2 つのエンコーダのインスタンスの SPS と PPS が同じかを測る |

## 実機テスト

実機テストは、ポータブル版の OBS 33 に cordmux を入れて起動し、テスト用の送信側か実際の Vesktop から映像と音声を流して、OBS の録画を判定スクリプトで読む。GPU のある Windows の PC で実行する。ツールは `test/tools/obs-run` にあり、使い方の詳細は `test/tools/obs-run/README.md` にある。

### 実行計画と判定スクリプト

実機テストの実行スクリプトは 2 つのファイルに分かれる。

- `run-window-plan.mjs`: 引数を読み、実行する手順の一覧 (実行計画) をデータとして作る純粋関数と、結果を判定する純粋関数。Node の fs も child_process も使わないので、vitest で確認する (`test/tools/obs-run/*.test.ts`)。
- `run-window.mjs`: 実行計画を実際に行う。OBS の起動と終了、obs-websocket (`obsws.mjs`) での操作、テスト用の送信側の起動、判定スクリプトの実行、後片付けを行う。

```
node test/tools/obs-run/run-window.mjs --obs <ポータブル版の OBS> --out <出力先> --plugin <cordmux のフォルダー>
    --step <ステップ> [オプション] [--dry-run]
node test/tools/obs-run/run-window.mjs --out <出力先> --step install-roundtrip --tag <名前> --zip <配布用の zip>
    --obs-zip <ポータブル版の OBS の zip> [--scenario <場面>] [--dry-run]
```

- `--dry-run` は、実行計画を JSON で表示するだけで、OBS もテスト用の送信側も起動しない。
- 始める前に、Vesktop と、Program Files にインストールした OBS が動いていないことと、GPU のエンコーダが使われていないこと (nvidia-smi) を確認する。満たさなければ始めない。実行ファイルの場所が分からない OBS も確認の対象にする。実際の通話のステップでは、Vesktop を確認の対象から除く。
- 守りたいプロセスを確認の対象に足すときは、環境変数 `CORDMUX_PROTECTED_PROCESSES` に、プロセス名を 1 行に 1 つ書いたテキストファイルのパスを入れる。空行と `#` で始まる行は読まない。大文字と小文字は区別しない。指定したのにファイルが読めないときも始めない。
- `--obs` に指定できるフォルダーは、リポジトリ直下の `_work` の下だけである。`--out` に指定できるフォルダーは、`_work` か `build_x64` の下だけである (`run-window-plan.mjs` の `OBS_DIR_ROOTS_REL` と `OUT_DIR_ROOTS_REL`)。実行スクリプトは `--obs` の OBS に cordmux をインストールして設定を書き込むので、書き違えて、実際に使っている OBS や、決めたフォルダーの外に書かないためである。外の場所を使うときは `--allow-outside-work` を付ける。
- OBS の設定 (プロファイル、シーン、obs-websocket、cordmux の settings.json) は、`seed-config.py` が書く。実際の通話のステップのほかは、`APPDATA` と `LOCALAPPDATA` を `_work` の下のフォルダーに差し替えて OBS を起動し、利用者の実際の設定とトークンファイルには触れない。
- 人の操作が必要な手順では、実行スクリプトがログに `HUMAN:` で始まる 1 行を書いて止まり、出力先の下に決まった名前のファイル (`human-go.txt`) が置かれるか、決まった状態になるのを待つ。
- 終了コードは、0 が成功、1 が引数か実行計画の誤り (または途中の例外)、2 が始める前の確認で止めた、3 が判定の失敗である。期限 (`--deadline-min`) を過ぎたときや、出力先の下に `stop-run.txt` が置かれたときに打ち切って判定できなかった場合と、Vesktop の vencordDir が前の値に戻ったと確認できない場合も 3 になる。

主なステップ:

| ステップ | 中身 |
|---|---|
| `place` | OBS に cordmux をインストールして起動し、プラグインの読み込み、ソース、トークンファイルとアクセス許可を確認する。2 つ目の OBS が待ち受けを始めないことも確認する |
| `through`、`integration` | テスト用の送信側から映像と音声を送り、OBS で録画して判定スクリプトで読む (`integration` は既定で画面共有 4 と声 8) |
| `guard` | トークンのない接続と違うトークンの接続を断ること、送りすぎたときに捨てること、エンコードの段階を下げること |
| `second-obs`、`discord-first` | シーンコレクションの切り替えの後も接続が続くこと。OBS より先に送信側を起動しても接続すること |
| `dock-view` | ドックのスクリーンショットを状態ごとに撮る (`--dock-scenario` で状態を選ぶ。未接続、エンコードの段階を下げた、依存箇所が読めない、10 分間操作しない、ダミー、診断情報のエクスポートなど) |
| `record` | cordmux の録画を途中で ON にし、解像度の変更、再接続、空き容量の警告、録画ファイルの作成、OBS の再起動の後に OFF に戻ることまでを 1 回で通す。残した録画ファイルは、ffprobe で長さ、fps、解像度、音の有無を調べる |
| `install-roundtrip` | 配布用の zip でインストールしてからアンインストールし、インストールの前の状態と比べる (`--scenario` で場面を選ぶ。フォルダーの移動、更新、既に Vesktop を使っている環境、古い OBS など) |
| `hold` | OBS を開いたまま、決まった名前のファイルが置かれるまで待つ。その間に UI Automation のツールを手で実行する |
| `mic-check`、`webcodecs-check` | OBS を起動せずに、`test/tools/mic-check` と `test/tools/webcodecs-check` を実行する |
| `call-1`、`call-hold` | 実際の Vesktop と通話を使う (「テストに使うアカウントの役割」)。`call-1` の前に、Vesktop の設定の書き換えを 1 つずつ確認するステップを 4 つ実行する (名前は `test/tools/obs-run/README.md` にある) |

cordmux は OBS 33 の置き方で置く (OBS のフォルダーの `plugins\cordmux\` の直下に cordmux.dll と同梱のファイル、`plugins\cordmux\data\` にデータ)。`install-roundtrip` の `old-obs` は、その置き方の cordmux を OBS 32.2.2 のポータブル版に置いて、読み込まれないこと (OBS のログに cordmux の Loading module がない) と、手順書の「ドックが表示されない」に OBS 33 以上への更新が書いてあることを見る。

`call-1` と `call-hold` は `--rehearsal` を付けると、実際の Vesktop の代わりにテスト用の送信側を使い、人の操作を待たずに同じ実行計画を短く通す。実際の通話の前に、実行計画が最後まで進むことを確認するために使う。

判定スクリプト:

| スクリプト | 読むもの | 判定すること |
|---|---|---|
| `frames.py` | OBS の録画 | フレームの欠落、重複、停止 |
| `smooth.py` | 4 つの枠を並べた OBS の録画 | 枠ごとの、10 秒ごとのフレームの欠落と重複 |
| `sync.py` | OBS の録画 | 映像と音声のずれ (同期用のマーカーの白と 2 kHz の時刻の差) |
| `analyze.py` | OBS の録画 | 1 kHz の音の有無 |
| `ledger_check.py` | cordmux の録画ログと録画ファイル | ffprobe で読んだ長さ、フレーム数、解像度、fps、音声の有無が、録画ログと整数で一致するか |
| `black_span.py` | cordmux の録画ファイル | 黒いフレームと無音が続く範囲 |
| `au_hash.py` | cordmux の録画ファイル | 送ったアクセスユニットと、録画ファイルの中のアクセスユニットが同じか (範囲ごとの framemd5) |
| `tone_scan.py` | 録画ファイル | 同期用のマーカーの音の数と位置、範囲ごとの音量 |
| `plate_measure.py` | 画像と録画 | 名前ラベルの背景の位置と重ね方 |
| `border_timing.py` | 録画 | 話している人の光を表示する、続ける、消すタイミング |
| `latency.py` | OBS の録画 | cordmux が加える遅延 (cordmux の枠と、Vesktop のウィンドウのキャプチャに映ったビットパターンの番号の差) |
| `dummy_tile.py` | ソースの画像 | ダミー映像の色が出ているか |
| `check_bundle_zip.py` | 診断情報の zip | 中身がそろっていて、トークンの形の文字列がないか |
| `install_snapshot.py` | インストールの前後の OBS とファイル | cordmux が追加したもののほかに差がないか (消す対象の表を `src-obs/install/removal_table.h` から読む) |

判定スクリプトのセルフテストのデータは、`make_material.py` が ffmpeg と numpy で作る (わざとフレームを落とした、止めた、音声をずらした動画など)。実際の通話で共有するテスト動画は `make_sync_video.py` が作る。

テストが終わったら、`cleanup.ps1` が、`_work` の下でテストが起動したプロセスを終了し、録画とテストのデータを削除する。録画を残すときは `-KeepRecordings` を付ける。実際の通話のステップで Vesktop の設定 (state.json の vencordDir) を書き換えたときは、`-CallWindow -RealProfile` で、cordmux が書いたバックアップから元の値に戻す。実行スクリプトが動いている間は実行しない。

### テストに使うアカウントの役割

実際の通話を使う実機テスト (`call-1` と `call-hold`) は、4 つの Discord のアカウントで、1 つの PC から同じボイスチャンネルに参加して行う。役割は次のとおりである。

| 役割 | 使うアプリ | 通話ですること |
|---|---|---|
| メインアカウント | Discord のデスクトップアプリ | 通話に参加し、テストのページ (`test/tools/obs-run/share/pattern.html`) を画面共有する。メインアカウントが通話に参加していないことのドックのメッセージを確認する手順では、通話から抜けて戻る |
| サブアカウント 1 | Vesktop | cordmux の送信側を動かし、ほかの 3 つのアカウントの画面共有と声を受け取る。`--vesktop-share` を付けて実行したときだけ、自分も画面共有する |
| サブアカウント 2 | Chrome | マイクの代わりにテスト用の音 (1 kHz) を声として入力し、画面共有する。映像と音声のずれを測る手順では、テスト動画のタブを音声付きで共有する。使う声の置き換えを確認する手順では、置き換えた後の声の元になる |
| サブアカウント 3 | Firefox | テストのページを画面共有する。使う声の置き換えと、参加者の一覧のチェックの付け外しを確認する手順で、その対象の人になる |

- サブアカウント 1 は、製品の言うサブアカウント (Vesktop にログインして映像と声を受け取るアカウント) である。サブアカウント 2 と 3 は、通話のほかの参加者の代わりに使う。
- Chrome は `test/tools/obs-run/launch-chrome-fake-mic.ps1` で起動し、テスト用の音のファイルをマイクの代わりに入力する。テスト用の音を使うのは声の確認 (使う声の置き換えと声のミュートの往復) だけなので、声の確認のない `call-1 --recheck` では、テスト動画のページを開いた Chrome でよい。
- 通話に入るときは、テスト用の音を送る Chrome のほかは、マイクをミュートにする (`--recheck` は 4 つともミュート)。同じ PC のスピーカーから出たテスト動画の音をマイクが拾うと、声として配信の音に入り、映像と音のずれの測定の組が別の音と組になるため。
- ボイスチャンネルの名前とメインアカウントのユーザー ID は、ツールに書かず、実行するときに `--voice-channel` と `--broadcaster-id` で渡す。

## 確認する項目

リリースの前に確認する項目を、場面ごとに並べる。「確認の方法」の種類は次のとおりである。

| 確認の方法 | 意味 |
|---|---|
| 単体テスト | C++ と TypeScript の単体テスト (CI で実行する) |
| lint | `test/lint` の lint (CI で実行する) |
| CI | 配布用の zip とリリースのワークフローのチェック |
| 実機テスト | テスト用の送信側を使う実機テスト (「実機テスト」の表のステップ) |
| 実際の通話 | 4 つのアカウントで実際の通話を使う実機テスト (`call-1` など) |
| 初めての PC | cordmux を入れたことのない Windows の PC (仮想マシンでもよい) に、手順書だけを見てインストールする |
| 目視 | 画面や録画を人が見る |

### インストールする日

| 確認すること | 確認の方法 |
|---|---|
| 初めての Windows の PC で、配布用の zip を展開してから、OBS にソースとドックが表示され、接続しているかがドックで分かるまでを、手順書だけを見て 15 分以内に終えられる。配布用の zip のほかに、インターネットから取得するものはない (サーバーへの招待を待つ時間は数えない)。映像が実際に表示されることは、通話かダミー表示で別に確認する | 初めての PC |
| インストールの手順に、コマンドラインの入力とビルドがない | lint (`install_guide.py`)、初めての PC |
| ポータブル版の OBS のフォルダーごと別の場所に移しても動く。移した後に OBS を起動し直すと接続し、Vesktop が公式の Vencord をダウンロードしない | 実機テスト (`install-roundtrip` の `move` と `move-foreign`) |
| インストールの前と後で、OBS のシーン、ソース、設定のファイルに、cordmux が追加したもの (利用者が [+] で置いた cordmux を含む) のほかの差がない。cordmux を置いてからアンインストールした後も、導入前と差がない | 実機テスト (`install-roundtrip` の `roundtrip`) |
| 一度も接続したことのない状態で OBS を起動すると、接続していないことがドックに表示される | 単体テスト (表示の規則)、実機テスト (`dock-view`) |
| ドックを開いたまま 10 分操作しなくても、バックグラウンドで動く部分が止まらない | 実機テスト (`dock-view` の `idle-10min`) |
| 対応するバージョンより古い OBS に入れたときと、OBS より新しい libobs でビルドした cordmux を読み込ませたときに、読み込まれなかったことが OBS の画面かログで分かり、することが手順書に書いてある | 実機テスト (`install-roundtrip` の `old-obs` と `newer-libobs`) |
| cordmux の音声は OBS のミキサーに入り、PC の既定の再生デバイスから直接は鳴らない | 実機テスト、目視 |
| サブアカウントを使う構成で、メインアカウントが通話に参加していないと、配信を始める前に、ドックにそのことと次の操作がメッセージとして表示される | 単体テスト (表示の規則)、実際の通話 (`call-1` の `--broadcaster-id`) |
| 画面共有している人の映像が届かないとき、ドックのメッセージが Vesktop で画面共有の映像が映っているかを確認するよう勧め、手順書の「困ったとき」にも同じ文がある | lint (`install_guide.py` が ini の `cordmux.dock.no_video_next.text` と同じ文があることを確認する)、実際の通話 (ドックにメッセージが表示されることと、映像が実際に止まっていることが一致するか) |
| アンインストールした後、インストールする前と比べて差がない。消す対象の表 (`src-obs/install/removal_table.h`) の項目ごとに確認する | 実機テスト (`install-roundtrip` の `roundtrip`)、初めての PC |
| Vesktop がほかの Vencord を使っている環境では、切り替える前に確認のダイアログを表示し、[キャンセル] なら Vesktop の設定を変えない。切り替えた後も、アンインストールすると元の Vencord を使う設定に戻る | 実機テスト (`install-roundtrip` の `other-vencord` と `other-vencord-cancel`) |

### 配信を乱さない・止めない

| 確認すること | 確認の方法 |
|---|---|
| 利用者が止めた音 (ドックの人の行のミュートと音量 0%) が、ダミー映像への切り替え、参加者の一覧が空になったとき、再接続、シーンコレクションの切り替えのどれでも鳴り出さない。OBS を起動し直したときに残るのは、参加者ごとの音量 (0% を含む) とメインアカウントの声のミュートで、ほかの人の声のミュートは解ける (前のミュートが残って、声が出ない理由が分からない状態を作らないため) | lint (`forbidden_libobs_usage.py`。cordmux が OBS のソースの音量とミュートを書き換えないこと)、単体テスト (`test_pipeline_mixer_levels.cpp` の名簿から外れて戻ったときのテスト、`test_config_allocation.cpp` の切り替えの間の凍結のテスト、`test_pipeline_stream_router.cpp` の OBS を起動し直した後のテスト) |
| 画面共有の接続から「話している」の状態が届いても、その音声を声として扱わない | 単体テスト (`core_state.test.ts`) |
| 受信の途中で映像の送り元が替わっても、前の枠に別の人の映像を表示しない | 単体テスト (`test_pipeline_stream_router.cpp`) |
| 警告と診断の表示を、OBS の出力の映像に描かず、出力の音声にも混ぜない | 実機テスト、目視 |
| ハードウェアのエンコーダが使えないとき、エラーで止まらずに解像度か fps を下げて続け、下げたことをドックに表示する | 単体テスト (`src-vencord/encode` のテスト)、実機テスト (`guard`、`dock-view` の `degraded`) |
| 画面共有の解像度が途中で変わっても、配信の映像も録画も壊れない | 単体テスト (`src-obs/record` のテスト)、実機テスト (`record`) |
| 1080p60 の映像を受け取ったとき、出力の解像度と fps が下がらない (設定で上限を下げたときを除く) | 実機テスト (`integration`)、実際の通話 (1080p60 で画面共有する人がいるとき) |
| 4 人が画面共有している通話で、同じ PC でゲームを動かしても、配信の映像がなめらかなまま。判定に使うのは、配信に出た映像のフレームの欠落と停止で、OBS 自身の描画の遅延ではない | 実際の通話 (`call-1`。`frames.py` と `smooth.py`)、目視 |
| cordmux がエンコーダを使っている間も、OBS の配信のエンコードがフレームを落とさない | 実際の通話 (`call-1`。obs-websocket の GetStats の 2 回の値の差から、落ちたフレームの割合を 0.02% と比べる) |
| トークンを持たないプログラムが接続しても、操作も映像と音声の受け取りもできない。ログにトークンが書かれない | 単体テスト (`test_ws_listener.cpp`)、実機テスト (`guard`) |
| cordmux を無効にしている間、送信側は Discord のトラックの読み出しも、エンコードも、映像と音声の送信もしない (127.0.0.1 への再接続の試行は数えない) | 単体テスト (`core_gate.test.ts`、`test_pipeline_wiring.cpp`)、実機テスト (テスト用の送信側が、受信側に止められている間に作ったエンコーダと送ったフレームを数える)、実際の通話 (表の後の説明) |
| OBS との接続が切れている間、送信側は Discord のトラックの読み出しもエンコードもしない。接続し直して media が true なら、読み出しとエンコードを始め直す | 実際の通話 (表の後の説明) |
| cordmux が加える遅延 (Discord 自身の再生と比べた OBS の出力の遅延) を測り、目安の 90 ms と比べる (超えても実行は止めない) | 実際の通話 (`call-1` の `--latency-person`。`latency.py`) |

送信側の `src-vencord/index.ts` が、受信側との接続と media の値に合わせてトラックの読み出しとエンコードを始めて止める処理と、`stop()` で接続を閉じる順序 (「録画」の表) は、実際の Vesktop でだけ確認できる。

- 画面共有している人がいる通話で、cordmux の録画を OFF にしたまま、ドックのメニューから cordmux を無効にして有効に戻し、OBS を終了して起動し直す。OBS は `--verbose` で起動し、OBS のログで次のことを見る。
  - cordmux を無効にしている間は、受信側が 10 秒ごとに書く統計の行 (`Stats video` と `Stats audio`) で、`received` の値が増えない。録画を OFF にしておくのは、ON のまま無効にすると、エンコーダを閉じるときに作った黒埋め用のフレームが無効にした直後に届き、`received` に数えられるためである。
  - OBS を起動し直した後に、送信側のログの行で、画面共有のストリームが新しい stream_id で作り直されている。送信側が接続の切れている間に書いた行も、接続し直した後に OBS のログに書かれる。
- 同じ通話で cordmux の録画を ON にしてから、開発者ツールのコンソールで `Vencord.Plugins.stopPlugin(Vencord.Plugins.plugins.cordmux)` を実行して `stop()` を呼び出す。エンコーダを閉じるときに黒埋め用のフレームを作ったことを書く送信側のログの行が、接続が閉じる前に OBS のログに書かれることを見る。`stop()` を呼び出す操作は、Vesktop の画面にない。送信側のプラグインは `required: true` なので、Vencord v1.15.17 のプラグインの一覧では無効にできない。このコードで `stop()` が呼び出されることは Vencord のソースで確認したもので、この手順は、実行して確認していない。

`index.ts` は Vencord を import するので、vitest から読み込めない。`core_gate.test.ts` は同じ処理をテストの中にコピーして確認するので、コピーが `index.ts` とずれても成功する。テスト用の送信側も `index.ts` を使わず、media が false の間は止めるが、接続が切れても止めない。

### 機能

| 確認すること | 確認の方法 |
|---|---|
| 4 人が画面共有している通話で、4 人の映像が 4 分割で表示され、参加者の一覧のチェックを付け外しすると入れ替わる | 実際の通話 (`call-1`) |
| 名前ラベル (名前とアバター) と話している人の光が、自分の枠を含むすべての枠に表示され、設定を変えると表示が変わる | 単体テスト (`decor_tests.exe`)、実際の通話 (`call-1`) |
| 参加者ごとの音量とミュートが、ドックの操作で配信の音に反映され、音量を 200% まで上げられる | 実際の通話 (`call-1`)、lint (`forbidden_libobs_usage.py`。cordmux がソースの音量とミュートを書き換えないこと) |
| ログに、映像の途切れ、届いている解像度と fps、音声が届いているかが残り、配信の後に読める | 実機テスト (`summarize-window.py` が OBS のログから読む)、目視 |
| 設定の「映像のデコード」を切り替えると、OBS を再起動した後に使うデコーダーが変わる | 単体テスト (`test_decode.cpp`)、実機テスト (`--decoder`) |
| cordmux を 2 つ以上のシーンに置き、シーンを切り替えても、映像が消えない | 実機テスト、目視 |
| シーンコレクションを切り替えて戻しても、接続が切れず、映像がまた表示される | 実機テスト (`second-obs`)、実際の通話 (`call-1`) |
| 「自動で枠に載せる」を付けた人が画面共有を始めると、自動で枠に載る。通話に参加していない人にも付け外しできる | 単体テスト (`test_pipeline_stream_router.cpp`、`src-obs/config` のテスト) |
| 名前を変更すると配信に表示される名前が変わり、変更していることが参加者の一覧で分かる | 単体テスト (`test_dock_build_observation.cpp`、表示の規則)、目視 (プレビューツール) |
| 使う声を別の人の声に置き換えると、声だけがその人から届き、映像と画面共有の音声は元の人のまま。使う声が画面に表示される | 単体テスト (`test_ledger_actions.cpp`、`test_ledger_rules.cpp`、`test_pipeline_stream_router.cpp`、`test_config_allocation.cpp`)、実際の通話 (`call-1`。`--extra-checks` で、テスト用の音が声の枠の間を移るかを `tone_scan.py` で数える) |
| 枠に載せる人の選び方を自動にすると、画面共有を始めた人が枠の数まで載り、手動に戻すと利用者の選択が残る | 単体テスト (`test_pipeline_stream_router.cpp`) |
| Vesktop のアカウントの画面共有は、選び方が「自動」でも、[自動で枠に載せる] でも、手動でも枠に載らず、ほかの人が上限まで載る (メインアカウントだけの構成と、サブアカウントの構成の両方) | 単体テスト (`test_pipeline_stream_router.cpp` の `receiver_user_id`) |
| 画面共有をやめた人と通話から抜けた人の枠が空く。Vesktop のアカウントが通話を抜けたときは全員の枠が空き、戻っても手動の人は自動では載らない。声のストリームが生きている間は、roster から一瞬消えても声の枠と経路が残る | 単体テスト (`test_pipeline_slot_release.cpp`) |
| 自分の枠に OBS の既存のソースかシーンを選ぶと、4 つの枠の 1 つに入り、その映像の画質のまま表示される。選んでいる間は、Discord での自分の画面共有を枠に載せない。cordmux を含むシーンは候補にしない (`test_sources_self_frame_plan.cpp`) | 単体テスト (`test_sources_composite_layout.cpp`、`test_pipeline_stream_router.cpp`)、実際の通話 |
| 自分の枠は、ドックの自分の行のチェックが付いている間に出る。チェックは、ドックで付けたとき、またはほかの人と同じ規則 (枠に載せる人の選び方が自動、[自動で枠に載せる]) で自分の共有を始めたときに付く。共有の終わりでは外れず、ドックで外した後は始め直すまで付かない。共有の状態が分からない間は変わらない。Discord の人で枠が埋まっていれば、自分の枠を出さない (付けた直後に、同じ瞬間に自動で 4 人目が載っていたときも、チェックを外す) | 単体テスト (`test_sources_self_frame_plan.cpp` の `plan_self_frame`、`test_pipeline_stream_router.cpp` の `accepted_self_frame_in_use`、`test_dock_self_frame_check.cpp`、`test_ledger_actions.cpp`、`test_ledger_dock_rules.cpp`)、ドックを操作するテスト (`selfframe`) |
| 自分の枠を使っている間は、Discord の人を 3 人まで枠に載せる (手動、「自動」、[自動で枠に載せる] のどれでも)。ドックが押せると判断した直後に自動で人が載っても、[自分の枠に映す映像] を替えても、描く物は 4 つを超えない | 単体テスト (`test_pipeline_stream_router.cpp` の「競合」と「どの経路を通っても」、`test_dock_self_frame_check.cpp` の「競合」、`test_ledger_actions.cpp`)、ドックを操作するテスト (`selfframe`。競合は Pipeline を組み込めないので単体テストだけ) |
| 枠のチェックを押した直後に枠が埋まって載せられなかったときは、その人の行に「空いている枠がありません。」と出し、その人が載るか、枠が空くか、その人が画面共有をやめるか通話を抜けるか、接続が切れたら消える。消えた後に枠がまた埋まっても出ない | 単体テスト (`test_ledger_dock_rules.cpp`) |
| 自分の枠のチェックの状態が、次に OBS を起動したときに残る (付いたり外れたりするたびに書く) | 単体テスト (`test_config_settings.cpp`、`test_dock_settings_fields.cpp`)、ドックを操作するテスト (`selfframe`) |
| 自分の枠に映す映像を OBS から選んでいる間、メインアカウントの Discord の画面共有を枠に載せない。声と録画は変わらない。選択を外したら、ほかの人と同じに扱う | 単体テスト (`test_pipeline_stream_router.cpp` の `self_frame_selected`) |
| 画質の上限を下げると、出力の解像度か fps が実際に下がる。画質を上げる操作はどこにもない | 単体テスト (`encode_limits.test.ts`、`encode_session.test.ts`)、実機テスト (`dock-view` の `degraded`) |
| 設定ウィンドウで映像エンコーダとエンコードで優先する項目を変えると、次に始まる画面共有のエンコードが変わる (送信側のログの行の `preference` と `latencyMode` に出る) | 単体テスト (`encode_limits.test.ts`、`transport_control.test.ts`、`test_ws_control_messages.cpp`、`test_pipeline_wiring.cpp`)、ドックを操作するテスト (`windows`)、実機テスト (設定を変えて画面共有を始め直し、そのときの送信側のログの行を見る) |
| 通話がない状態でも、ダミー映像で見た目を確認でき、その時間帯が録画ログで実際の映像と区別できる。ダミー映像の間は、自分の枠の条件 (チェック、映す映像、自分の画面共有、[先頭に配置]) に依らず自分の枠を描かず、4 つの枠すべてにダミーを描き、やめると自分の枠が戻る | 単体テスト (`test_pipeline_dummy_interval.cpp`、`test_sources_composite_input.cpp` の `dummy_composite_input`、`test_sources_dummy_tiles.cpp`)、実機テスト (`dock-view` の `dummy`) |
| 映像と音声がずれない | 単体テスト (`test_align_audio_drift.cpp` と `encode_clock_map.test.ts` の 10 時間の模擬)、実機テスト (`integration`。`sync.py`)、実際の通話 (`call-1`。テスト動画を共有して `sync.py` で測る) |
| 画面共有を受け取り始めた直後に、映像のタイムスタンプが届く時刻より先の値になり (WebRTC のデコードの時間の見積もりが大きい間)、後で戻っても、映像と音声のずれが残らない。受け取り始めてから 60 秒以内に、下がりのない場合と同じ関係に戻る | 単体テスト (`encode_clock_map.test.ts` の「上げる向き」と、「10 時間の模擬」の pushAudio を通す下がりの列)、実際の通話 (送信側のログの `clock lag` の行の 50 パーセンタイル) |
| 1 時間や 10 時間続けても、映像と音声のずれが時間とともに開かない。音を取り出す時計と単調な時計の速さが +-100 ppm 違っても、システム時刻が飛んでも同じ | 単体テスト (`test_align_audio_drift.cpp` の「10 時間の模擬」、`encode_clock_map.test.ts` の「10 時間の模擬」、`encode_sender_clock.test.ts`、`test_pipeline_sender_clock.cpp`) |
| チェックの付け外し、音量、ミュートの結果が、待たずに出力に反映される | 実際の通話、目視 |

### 録画

| 確認すること | 確認の方法 |
|---|---|
| cordmux の録画を OFF にしている間は、録画のためのエンコーダ、スレッド、ファイルを 1 つも作らない | 単体テスト (`test_record_gate.cpp`、`src-vencord/encode` のテスト) |
| 配信の途中で cordmux の録画を ON にしても、そのセッションに映像と音声の両方が残る | 単体テスト (`src-obs/record` のテスト)、実機テスト (`record`)、実際の通話 (`call-1`) |
| 受信が切れて戻った区間が、黒い映像と無音で埋まった 1 つのファイルとして残る | 単体テスト (`test_record_black.cpp`、`test_record_io_finalize.cpp`)、実機テスト (`record`。`black_span.py`) |
| 送信側のプラグインの `stop()` は、cordmux の録画が ON なら、黒埋め用のフレームを送り終えてから接続を閉じる。2 秒待っても送り終えなければ、そこで閉じる | 実際の通話 (「配信を乱さない・止めない」の表の後の説明) |
| 参加者ごとに、マイクの声のファイルと、画面共有の映像と音声のファイルが分かれて出る。後処理のコマンドを使わずに動画編集ソフトで開け、映像と音声が最初から最後まで正しい長さで再生できる。解像度が途中で変わった録画と、再接続が多い録画も同じ | 単体テスト (実際の FFmpeg で作成して ffprobe で比べる)、実機テスト (`record`。`ledger_check.py`)、動画編集ソフトで開く (docs/release.md の「録画ファイルと動画編集ソフト」) |
| 録画に名前ラベルと話している人の光が映っておらず、声のファイルの音声が話した人のものだけである | 目視 |
| 保存先の空き容量が足りなくなる前に、ドックに表示される | 単体テスト (`test_record_space.cpp`)、実機テスト (`record`) |
| 録画が ON か OFF かがドックで分かり、ON のまま OBS を再起動すると OFF に戻っている | 実機テスト (`record`)、目視 |
| 録画の画質が、受信した画質より落ちていない。エンコードし直さずに作成したときは、送ったアクセスユニットと録画ファイルの中のアクセスユニットが同じことで確認する。ProRes で作成したときの画質の指標は決めていない | 実機テスト (`record`。`au_hash.py`) |

### 公開と運用

| 確認すること | 確認の方法 |
|---|---|
| タグをプッシュすると、GitHub Actions が配布用の zip を人の手なしに作り、リリースを公開する (docs/release.md の「リリースの手順」) | CI (`dist.yml`) |
| 配布用の zip から、対応するソースコードの入手先が分かる | CI (`check_dist_zip.py`、`make_release_notes.py`) |
| 配布用の zip の手順書とリリースノートに、「互換性」の節がある | CI (`check_dist_zip.py`、`make_release_notes.py`) |
| 手順書に、通話のほかの参加者はインストールも設定も要らないことが書いてある | lint (`install_guide.py`) |
| 手順書に、画面共有する人がする操作がない | lint (`install_guide.py`。依頼の形の語がないことと、「始める前に」に決まった文があること) |
| 英語の画面と手順書を、英語を書いた人とは別の人が英語だけで読み、意味が通る | 目視 |
| 1 回の配信の間に利用者が触る操作が、チェックの付け外しと音量とミュートのほかにない (音量とミュートも触らなくてよい) | 実際の通話 (`call-1` が配信中の操作を種類ごとに数える) |
| 一度決めた設定が、次の配信でもそのまま残っている | 単体テスト (`test_config_settings.cpp`) |
| 長い配信で、表示したドックのメッセージがすべて実際の異常と対応していて、問題が解消したメッセージは消えている | 実際の通話 (`call-1` で、わざとファイルを書き換えてメッセージを出し、戻して消えることを見る)、`test/tools/notice-count` でメッセージの表示と消去をログから数える |
| コードの識別子が英語、コメントとコミットメッセージが日本語である | lint |
| Windows に依存する部分が、`src-obs/platform` と `src-vencord/paths.ts` に閉じている | lint (`windows_api_confinement.py`) |
| OBS と Vesktop の起動の順を入れ替えても接続する。配信中にどちらかを再起動しても、もう片方を触らずに接続が戻る | 単体テスト (`src-vencord/transport` のテスト)、実機テスト (`discord-first`) |

### 問題が起きたとき

| 確認すること | 確認の方法 |
|---|---|
| Discord の内部への依存箇所を 1 つ読めなくすると、何が読めなくなったかがドックに表示され、そのとき表示している映像と音声は止まらない | 単体テスト (`fake_adapter.test.ts`)、実機テスト (`dock-view` の `contact-missing`) |
| 依存箇所の一覧の行が増えたり減ったりすると、テストが失敗する | 単体テスト (`fake_adapter_count.test.ts`、`ledger.test.ts`) |
| ドックのメッセージが、問題が解消したら消える。接続する前に起きた問題も、接続した後に表示される。利用者が自分で選んだ状態 (録画 OFF など) を、異常として表示しない | 単体テスト (`test_ledger_rules.cpp`) |
| ドックのボタンとチェックが最後まで動作する。表示だけが変わって何も起きない操作がない | 単体テスト (`test_ledger_actions.cpp`)、ドックを操作するテスト |
| 画面の文言の記号と表記が決まりに合い、文言の形 (句点、疑問符、「...」) が UI 要素の種類に合っている | lint (`forbidden_ui_words.py`、`locale_shapes.py`) |
| 参加者の一覧が読めない間も、利用者が入れた選択が消えない | 単体テスト (`src-obs/config` のテスト) |
| 映像がまだ一度も届いていない人と、届かなくなった人を、画面で別の状態として表示する。ほかの人の映像が届いていても、届いていない人がいることを表示する | 単体テスト (`test_ledger_dock_rules.cpp`) |
| 今動いている部分のバージョンが設定ウィンドウの [バージョン情報] ページに表示され、ログとバージョンを、1 つの操作で書き出せる (診断情報のエクスポート) | 単体テスト (`src-obs/bundle` のテスト)、実機テスト (`dock-view` の `export-bundle`。`check_bundle_zip.py`) |
| 設定を入れた状態で新しいバージョンに更新すると、設定が残っていて、更新の手順が手順書のとおりに通る | 実機テスト (`install-roundtrip` の `update`) |
| 配布用の zip の Vencord が、Vesktop が有効な Vencord とみなす形になっていることを、zip を作るときに確認する | CI (`check_dist.py`、`check_dist_zip.py`) |

## テストデータ

テストデータの書き方は、CODESTYLE.md の「テストの名前とテストデータ」にある。タイムスタンプの差や SSRC のように、テストの意味が値に依存するものは、人や PC を特定できない値ならそのまま使う。

| データ | 中身 |
|---|---|
| `src-obs/tests/fixtures/h264_64x64_10f.h264` | デコードのテストに使う H.264 (Annex B)。64 x 64、10 フレームで、フレーム 0 と 5 が IDR である。各フレームの先頭に AUD (NAL ユニットのタイプ 9) があり、キーフレームごとに SPS と PPS が入っている |
| `src-obs/tests/fixtures.h` | タイムスタンプの整列 (`src-obs/align`) のテストに使う、タイムスタンプの列 (fixture) を作る関数 |
| `src-obs/tests/snapshots/` | `decor_tests.exe` が比べる、名前ラベルを描いた期待する画像 |
| `src-obs/tests/fonts/` | `decor_tests.exe` が使うフォント (Open Sans)。描画の結果が PC のフォントに依存しないようにする |
| `test/lint/fixtures/` | lint のセルフテストの NG 例と OK 例 |
| `test/fixtures/discord-bundles/manifest.json` | Discord のバンドルを取得したビルドの一覧 (`discord_contacts.test.ts` が読む) |

`h264_64x64_10f.h264` は、ffmpeg 8.0 の次のコマンドで作った (`src-obs/tests/test_decode.cpp` のコメント)。

```
ffmpeg -y -f lavfi -i testsrc2=size=64x64:rate=30 -frames:v 10 -an \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -g 5 -bf 0 \
  -x264-params repeat-headers=1:aud=1 -f h264 h264_64x64_10f.h264
```

`-g 5` で 5 フレームに 1 回キーフレームが入り、`repeat-headers=1` でキーフレームごとに SPS と PPS が入り、`aud=1` で各フレームの先頭に AUD が付く。テストは AUD の位置でアクセスユニットに分け、1 つずつデコーダーに渡す (docs/wire_format.md の「映像のペイロード」)。

判定スクリプトのセルフテストのデータと、実際の通話で共有するテスト動画は、ツールが作り、リポジトリには入れない (「実行計画と判定スクリプト」)。

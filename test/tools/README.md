# test/tools

単体テスト (`src-obs/tests`、`src-vencord/tests`) の外で使う、テストと配布のツール。lint は `test/lint/` にある。

| フォルダー | 使う所 | 中身 |
|---|---|---|
| `harness/` | 実機テスト、CI (バンドルと型チェック) | テスト用の送信側。製品の送信側のコードを esbuild でバンドルして Electron の中で動かし、受信側へ映像と音声を送る |
| `obs-run/` | 実機テスト、CI (セルフテスト) | ポータブル版の OBS の初期設定の書き込み、obs-websocket での操作、録画と画面の判定スクリプト、後片付け |
| `composite-check/` | 実機テスト | テスト用の OBS の中で、合成のソースが GPU で描いた名前ラベルや光を、CPU で描いた参照とピクセルで比べる |
| `vencord/` | CI (`vencord-plugin.yml`) | Vencord のビルドの dist に、Vesktop が読む 5 つのファイルがそろっているかのチェック (`check_dist.py`) |
| `dist-zip/` | CI (`dist.yml`、`checks.yml`、`lint.yml`、`obs-plugin.yml`、`vencord-plugin.yml`)、ローカル | 配布用の zip を作るツールと、zip の中身のチェック |
| `discord-bundle/` | CI (`discord-contacts.yml`) | Discord のバンドルの取得と、依存箇所の一覧の述語の照合 |
| `mic-check/` | ローカル | マイクの代わりに WAV ファイルを音声の入力にできるかの確認 |
| `webcodecs-check/` | ローカル、CI (構文チェック) | WebCodecs のエンコーダの遅延と、SPS と PPS の一致の測定 |
| `notice-count/` | 実機テスト、CI (セルフテスト) | cordmux のログから、メッセージの表示と消去と、映像や接続にかかる時間を集計する |
| `wording/` | ローカル、CI (セルフテスト) | ja の文字列の、英数字の前後の空白と疑問符の書き方の切り替え (`apply_ja_style.py`) |

使い方は、各フォルダーの README.md か、ツールのファイルの先頭のコメントにある。

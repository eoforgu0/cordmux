# src-obs/record

cordmux の録画。受信した画面共有と声を再エンコードせずにセグメントのファイル (fMP4 の mov) に書き、録画ログ (JSONL) に追記する。録画を止めると、同梱の FFmpeg を子プロセスで起動して、参加者ごとに画面共有と声のファイルに録画ファイルを作成する。受信が途切れていた間は、黒い画面と無音で埋める。録画が OFF の間は、セグメントも録画ログも書かず、書き込むスレッドも起動しない。

録画ログの行と、保存先に作るフォルダーの構成は docs/architecture.md の「セグメントファイルと録画ログ」に、録画ファイルの作成の流れは同じ文書の「録画ファイルの作成」にある。

## 入口

- `Recorder` (`recorder.h`): `pipeline::Pipeline` が作って持つ。`on_video`、`on_audio`、`on_configure` などは ws の io_thread から、`set_enabled`、`request_disable`、`resume_pending`、`stop` は UI のスレッドから呼ぶ。状態は `snapshot()` で読む。
- `make_real_record_platform_ops` (`recorder.h`): 製品で使う `RecordPlatformOps` を作る。テストは関数を差し替えて、呼び出しを数えたり、失敗を起こしたりする。
- `run_finalize` (`record_finalize.h`): 録画ログから、録画ファイルを 1 つずつ作成する。
- `pending_dirs_file_store` (`record_search_dirs.h`): 保存先の一覧のファイルを読み書きする。

## ファイル

`cordmux_add_record_core` が追加するのは、ヘッダーだけでできている部分である。libobs、Qt、libavformat に依存しない。ほとんどは純粋関数で、ファイルの操作は `RecordPlatformOps` か引数の関数を通す。ただし `record_search_dirs.h` の `pending_dirs_file_store` は、ファイルを直接読み書きする。

| ファイル | 役割 |
|---|---|
| `record_types.h` | 録画のコードが共有する型 (`RecordingSnapshot`、録画ログの行、`RecordPlatformOps` など) と定数 |
| `record_clock.h` | 映像のタイムスタンプが戻ったときの扱い |
| `record_segment.h` | セグメントを分ける条件、ファイルとフォルダーの名前、セッションを開く処理 (`ensure_session_open`) |
| `record_ledger.h` | 録画ログの書き出しと読み込み |
| `record_avcc.h` | Annex B からの avcC の組み立てと、AVCC への詰め替え |
| `record_black.h` | 欠落を黒で埋める長さの計算 |
| `record_audio.h` | 音声を書く位置の計算 (align/audio_drift.h でタイムスタンプに追従させる) |
| `record_args.h` | 録画ログから録画ファイルの作成の計画と FFmpeg の引数を作る (`build_finalize_plan`) |
| `record_branch.h` | 録画ファイルの作成の方式 (Remux か ProRes) の判定 |
| `record_space.h` | 録画中の空き容量の消費と、録画ファイルの作成に要る空き容量の見積もり |
| `record_location.h` | 保存先の決め方 |
| `record_search_dirs.h` | 録画ファイルの作成の済んでいないセッションが残る保存先の一覧 (`record_pending_dirs.json`) |

`cordmux_add_record_io` が追加するのは I/O の部分で、libavformat と `RecordPlatformOps` を使う。libobs と Qt には依存しない。

| ファイル | 役割 |
|---|---|
| `recorder.h`、`recorder.cpp` | `Recorder`。受け取ったデータをキューに積み、書き込み専用のスレッドでセグメントと録画ログを書き、録画ファイルの作成を別のスレッドで実行する |
| `record_writer.h`、`record_writer.cpp` | セグメントを書く `SegmentWriter` と、セグメントの読み出し。libavformat のヘッダーを読み込むのは `record_writer.cpp` だけである |
| `record_finalize.h`、`record_finalize.cpp` | 録画ファイルの作成 (`run_finalize`) と、途中で切れたセグメントの修復 (`repair_truncated_segments`) |
| `record.cmake` | ファイルをターゲットに追加する `cordmux_add_record_core` と `cordmux_add_record_io` |

## 依存

- 使うもの: C++17 の標準ライブラリ、nlohmann/json、`config/settings.h`、`pipeline/pipeline.h` (値を持つ構造体だけ)、`pipeline/dummy_interval.h`、`platform/`、`ws/wire.h` と `ws/listener.h` (型だけ)。
- Windows に固有の名前は書かず、OS の操作は `src-obs/platform` の関数を通す。
- `record.cmake` は、`src-obs/ws/ws_deps.cmake` と `src-obs/decode/decode.cmake` の後に include する。

テストは `src-obs/tests/test_record_*.cpp` にある。実際の FFmpeg を使うテストは、環境変数 `CORDMUX_FFMPEG_EXE` と `CORDMUX_FFPROBE_EXE` が obs-deps の ffmpeg.exe と ffprobe.exe を指すときだけ実行し、指さなければスキップする (docs/testing.md の「C++ の単体テスト」)。

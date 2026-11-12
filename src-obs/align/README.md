# src-obs/align

送信側の映像のタイムスタンプを、推定した周期の間隔で並ぶグリッドに合わせ、同じオフセットを音声のタイムスタンプにも足す。送信側のタイムスタンプをそのまま OBS に渡すと、ジッターのために 60 Hz の描画でフレームの重複と欠落が出るためである。合わせ方は docs/architecture.md の「タイムスタンプの合わせ方」にある。

## 主な型と関数

- `TimestampAligner` (`timestamp_aligner.h`): `align()` に送信側のタイムスタンプを 1 フレームずつ渡すと、OBS に渡すタイムスタンプを返す。ふるまいを決める値は `AlignerConfig` にあり、値ごとの理由はそのコメントにある。統計は `stats()` で読む。
- `AvOffsetTranscriber` (`av_offset.h`): `note_video()` に映像の送信側と出力のタイムスタンプを渡し、`map_audio()` で音声のタイムスタンプに同じオフセットを足す。
- `AudioDriftCorrector` (`audio_drift.h`): 音のチャンクを、チャンクごとの目標の位置に追従させて書き出す。音を取り出す時計と出力の時計の速さの差を、ごくわずかなサンプルレートの変換で連続的に吸収する。位置だけを決める `AudioDriftTracker` を含む。決め方は docs/architecture.md の「音声の時計の差の吸収」にある。

pipeline/ が、画面共有のストリームごとに `TimestampAligner` と `AvOffsetTranscriber` を 1 つずつ作る。`AudioDriftCorrector` は、pipeline/ の `AudioMixer` が組ごとに、record/ の録画が音声のストリームごとに 1 つずつ持つ。

## 依存

C++17 の標準ライブラリだけを使う。audio_drift.h は、x64 では変換の計算に SSE2 の組み込み関数 (emmintrin.h) も使う。組み込み関数を使わない予備の計算も持ち、テストが 2 つを比べる。

テストは src-obs/tests/test_timestamp_aligner.cpp、src-obs/tests/test_av_offset.cpp、src-obs/tests/test_align_audio_drift.cpp、src-obs/tests/test_align_audio_drift_ops.cpp にある。テストに使うタイムスタンプの列 (fixture) は src-obs/tests/fixtures.h が作る。fixture ごとの統計は、テスト「fixture ごとの整列の結果を表の形で出力する」が標準出力に書く。

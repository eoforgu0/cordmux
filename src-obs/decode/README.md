# src-obs/decode

送信側から届いた H.264 (Annex B) のアクセスユニットを obs-deps の FFmpeg でデコードし、I420 か NV12 のフレームを返す。

## 主な型と関数

- `H264Decoder` (`h264_decoder.h`): `push()` にアクセスユニットを 1 つずつ渡すと、デコードできたフレームを返す。戻り値は、送信側にキーフレームを要求するべきかを示す。ハードウェアデコードの種類は `DecoderConfig::hw_type` で渡す。
- `ffmpeg_link_versions()` (`ffmpeg_link_versions.h`): リンクした FFmpeg のライブラリのメジャーバージョンを返す。
- `cordmux_add_decode(<target>)` (`decode.cmake`): target に decode/ のソースファイルと FFmpeg のライブラリを追加する。

pipeline/ が、画面共有のストリームごとに `H264Decoder` を 1 つ作る。

## 依存

libobs にも Qt にも依存しない。obs-deps の FFmpeg (avcodec、avutil、swresample) にリンクし、`ffmpeg_link_versions()` は avformat も呼び出す。FFmpeg の場所は src-obs/cmake/cordmux_obs_deps.cmake が src-obs/buildspec.json から決める。cordmux.dll と一緒に配布する DLL の一覧は src-obs/cmake/cordmux_ffmpeg_runtime.cmake にある。

テストは src-obs/tests/test_decode.cpp にある。テストデータは src-obs/tests/fixtures/h264_64x64_10f.h264 (64x64、10 フレーム) で、作り方は test_decode.cpp のコメントにある。

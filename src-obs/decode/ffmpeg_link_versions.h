// SPDX-License-Identifier: Apache-2.0

// リンクした FFmpeg のライブラリ (avcodec、avutil、swresample、avformat) のメジャーバージョンを読む。
// libobs にも Qt にも依存しない。

#pragma once

#include <cstdint>

namespace cordmux::decode {

struct FfmpegLinkVersions {
	unsigned avcodec_major = 0;
	unsigned avutil_major = 0;
	unsigned swresample_major = 0;
	unsigned avformat_major = 0;
};

// avcodec_version()、avutil_version()、swresample_version()、avformat_version() を呼び出して、
// メジャーバージョンを返す。
//
// 呼び出すこと自体にも意味がある。OBS はプラグインの DLL を LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR で読み込むので、
// プラグインのフォルダーから読み込まれるのは、cordmux.dll が直接依存する DLL だけである。swresample は avcodec が
// 使うだけなので、cordmux のコードが 1 回も呼び出さないと、リンカがインポートを最適化で消し、直接の依存にならない。
// avformat は録画のコードも呼び出すが、FFmpeg のバージョンを 1 か所で読めるように、ここでも読む。
// plugin-main.cpp が起動時に呼び出して、バージョンをログに書く。テストは、返す値を比べて、ビルドが
// obs-deps 以外の FFmpeg をリンクしていないことを確認する。
FfmpegLinkVersions ffmpeg_link_versions();

} // namespace cordmux::decode

// SPDX-License-Identifier: Apache-2.0

// decode/ffmpeg_link_versions.h の実装。

#include "decode/ffmpeg_link_versions.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // FFmpeg のヘッダーの暗黙の型変換の警告 (h264_decoder.h のコメントと同じ)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace cordmux::decode {

FfmpegLinkVersions ffmpeg_link_versions()
{
	FfmpegLinkVersions v;
	v.avcodec_major = AV_VERSION_MAJOR(avcodec_version());
	v.avutil_major = AV_VERSION_MAJOR(avutil_version());
	v.swresample_major = AV_VERSION_MAJOR(swresample_version());
	v.avformat_major = AV_VERSION_MAJOR(avformat_version());
	return v;
}

} // namespace cordmux::decode

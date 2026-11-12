// SPDX-License-Identifier: Apache-2.0

// decode/h264_decoder.h の実装。

#include "decode/h264_decoder.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // FFmpeg のヘッダーの暗黙の型変換の警告 (h264_decoder.h のコメントと同じ)
#endif
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstring>

namespace cordmux::decode {

namespace {

// AVFrame のピクセルフォーマットを PixelFormat に変換できるか調べる。
// libx264 の既定 (limited range の YUV420P) と、色の範囲を full にしたとき (YUVJ420P。FFmpeg は
// 範囲だけが違う形式に別の値を割り当てている) の両方を I420 として扱う。
bool map_pixel_format(int av_format, PixelFormat *out, bool *full_range)
{
	switch (av_format) {
	case AV_PIX_FMT_YUV420P:
		*out = PixelFormat::I420;
		*full_range = false;
		return true;
	case AV_PIX_FMT_YUVJ420P:
		*out = PixelFormat::I420;
		*full_range = true;
		return true;
	case AV_PIX_FMT_NV12:
		*out = PixelFormat::NV12;
		*full_range = false;
		return true;
	default:
		return false;
	}
}

// プレーンの高さ (行数)。このクラスが扱う形式は、I420 も NV12 も 4:2:0 である。
int plane_height(int plane_index, int frame_height)
{
	if (plane_index == 0)
		return frame_height;
	// I420 の U と V、NV12 の UV は、どれも縦が半分 (高さが奇数なら切り上げる)。
	return (frame_height + 1) / 2;
}

int plane_count(PixelFormat format)
{
	return format == PixelFormat::NV12 ? 2 : 3;
}

} // namespace

H264Decoder::H264Decoder(const DecoderConfig &config) : config_(config)
{
	packet_ = av_packet_alloc();
	frame_ = av_frame_alloc();
	hw_sw_frame_ = av_frame_alloc();
}

H264Decoder::~H264Decoder()
{
	close_codec();
	av_frame_free(&hw_sw_frame_);
	av_frame_free(&frame_);
	av_packet_free(&packet_);
}

void H264Decoder::close_codec()
{
	if (ctx_)
		avcodec_free_context(&ctx_);
	if (hw_device_ctx_)
		av_buffer_unref(&hw_device_ctx_);
	hw_pix_fmt_ = AV_PIX_FMT_NONE;
}

enum AVPixelFormat H264Decoder::choose_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
	auto *self = static_cast<H264Decoder *>(ctx->opaque);
	if (self) {
		for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; ++p) {
			if (*p == self->hw_pix_fmt_)
				return *p;
		}
	}
	// 選んだ形式が候補になければ、avcodec の既定 (普通はソフトウェアの形式) に任せる。
	// 想定していない状況で、結果はソフトウェアデコードに切り替えたのと同じになる。
	return formats[0];
}

bool H264Decoder::open_codec()
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec)
		return false;

	ctx_ = avcodec_alloc_context3(codec);
	if (!ctx_)
		return false;

	// フレーム単位のスレッドを使うと、スレッドの数に応じてフレームの遅延が増えるので、スライス単位の
	// スレッドだけを使う。遅延を増やさないよう、低遅延のフラグも立てる。
	ctx_->thread_count = config_.thread_count > 0 ? config_.thread_count : 2;
	ctx_->thread_type = FF_THREAD_SLICE;
	ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

	if (config_.hw_type != AV_HWDEVICE_TYPE_NONE) {
		// このデコーダーがその hw_type に対応しているかを、avcodec の一覧で確認する
		// (対応していない組み合わせを av_hwdevice_ctx_create にだけ聞くと、デバイスは作れたのに
		//  デコードには使われない形になりうる)。
		for (int i = 0;; ++i) {
			const AVCodecHWConfig *hw_config = avcodec_get_hw_config(codec, i);
			if (!hw_config)
				break;
			if (hw_config->device_type == config_.hw_type &&
			    (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
				hw_pix_fmt_ = hw_config->pix_fmt;
				break;
			}
		}

		if (hw_pix_fmt_ == AV_PIX_FMT_NONE) {
			// このビルドの H.264 デコーダーが対応していない種類。ソフトウェアデコードに切り替える。
			stats_.hw_fallback = true;
		} else if (av_hwdevice_ctx_create(&hw_device_ctx_, config_.hw_type, nullptr, nullptr, 0) < 0) {
			// 対応しているが、デバイスを開けなかった (ドライバーがないなど)。同じくソフトウェアデコードに
			// 切り替える。
			hw_pix_fmt_ = AV_PIX_FMT_NONE;
			stats_.hw_fallback = true;
		} else {
			ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
			ctx_->opaque = this;
			ctx_->get_format = &H264Decoder::choose_hw_format;
		}
	}

	if (avcodec_open2(ctx_, codec, nullptr) < 0) {
		close_codec();
		return false;
	}
	return true;
}

bool H264Decoder::contains_idr(const uint8_t *data, size_t size)
{
	if (!data || size < 4)
		return false;
	// 3 バイトの開始コード (00 00 01) を探す。4 バイトの開始コード (00 00 00 01) も、末尾の 3 バイトが
	// 同じなので、これで見つかる。
	size_t i = 0;
	while (i + 3 <= size) {
		if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
			size_t header = i + 3;
			if (header < size) {
				uint8_t nal_type = data[header] & 0x1F;
				if (nal_type == 5) // IDR スライス
					return true;
			}
			i = header;
			continue;
		}
		++i;
	}
	return false;
}

bool H264Decoder::convert_frame(AVFrame *frame, DecodedFrame *result)
{
	AVFrame *source = frame;

	if (hw_pix_fmt_ != AV_PIX_FMT_NONE && frame->format == hw_pix_fmt_) {
		// ハードウェアのフレームを CPU のメモリに読み戻す (OBS の非同期のソースには CPU のメモリの
		// フレームを渡すので、共有テクスチャは使わない)。
		av_frame_unref(hw_sw_frame_);
		if (av_hwframe_transfer_data(hw_sw_frame_, frame, 0) < 0)
			return false;
		source = hw_sw_frame_;
	}

	PixelFormat format;
	bool full_range = false;
	if (!map_pixel_format(source->format, &format, &full_range))
		return false;

	result->format = format;
	result->width = source->width;
	result->height = source->height;
	result->full_range = full_range;
	result->timestamp_us = (source->pts != AV_NOPTS_VALUE) ? source->pts : frame->pts;

	int planes = plane_count(format);
	for (int p = 0; p < planes; ++p) {
		int stride = source->linesize[p];
		int rows = plane_height(p, source->height);
		result->linesize[p] = stride;
		if (stride <= 0 || rows <= 0 || !source->data[p]) {
			result->plane[p].clear();
			continue;
		}
		result->plane[p].assign(source->data[p], source->data[p] + static_cast<size_t>(stride) * rows);
	}
	for (int p = planes; p < 3; ++p) {
		result->linesize[p] = 0;
		result->plane[p].clear();
	}

	if (source != frame)
		av_frame_unref(hw_sw_frame_);
	return true;
}

void H264Decoder::drain(std::vector<DecodedFrame> *out, bool *avcodec_error)
{
	while (true) {
		int ret = avcodec_receive_frame(ctx_, frame_);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			if (avcodec_error)
				*avcodec_error = true;
			break;
		}

		DecodedFrame decoded;
		if (convert_frame(frame_, &decoded) && out) {
			out->push_back(std::move(decoded));
			stats_.decoded++;
		}
		// 変換できなかったフレーム (対応していないピクセルフォーマット、読み戻しの失敗) は、統計にも
		// ログにも残さずに捨てる。avcodec はデコードできているので、このアクセスユニットを失敗とは数えない。

		av_frame_unref(frame_);
	}
}

void H264Decoder::decode_au(const uint8_t *data, size_t size, int64_t timestamp_us, std::vector<DecodedFrame> *out,
                             bool *avcodec_error)
{
	if (!ctx_ && !open_codec()) {
		*avcodec_error = true;
		return;
	}
	if (!data || size == 0) {
		*avcodec_error = true;
		return;
	}

	av_packet_unref(packet_);
	if (av_new_packet(packet_, static_cast<int>(size)) < 0) {
		// メモリ不足など。クラッシュせず、失敗として返す。
		*avcodec_error = true;
		return;
	}
	// av_new_packet は size + AV_INPUT_BUFFER_PADDING_SIZE を確保し、余白を 0 で埋める (FFmpeg の約束。
	// H.264 のビットリーダーが末尾を読み越えても壊れない)。
	std::memcpy(packet_->data, data, size);
	packet_->pts = timestamp_us;

	int send_ret = avcodec_send_packet(ctx_, packet_);
	if (send_ret == AVERROR(EAGAIN)) {
		// 中の出力のキューがいっぱいになっている。先に受け取ってから送り直す。
		drain(out, avcodec_error);
		send_ret = avcodec_send_packet(ctx_, packet_);
	}
	if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
		*avcodec_error = true;
	}

	drain(out, avcodec_error);
}

bool H264Decoder::push(const uint8_t *data, size_t size, int64_t timestamp_us, std::vector<DecodedFrame> *out)
{
	stats_.received++;

	bool has_idr = contains_idr(data, size);
	if (!saw_idr_ && !has_idr) {
		// キーフレームより前のデータ。デコーダーに渡さずに捨てる (壊れた入力をそのまま渡さない)。
		stats_.failed++;
		return true;
	}

	bool avcodec_error = false;
	decode_au(data, size, timestamp_us, out, &avcodec_error);

	if (avcodec_error) {
		stats_.failed++;
		// デコードに失敗したら、キーフレームを要求し直す。
		saw_idr_ = false;
		return true;
	}

	if (has_idr)
		saw_idr_ = true;
	return false;
}

void H264Decoder::flush(std::vector<DecodedFrame> *out)
{
	if (!ctx_)
		return;
	// EOF を送って、中に残っているフレームを受け取る。エラーは無視する (呼び出し元はストリームを
	// 終えるだけで、失敗として扱う相手がいない)。
	avcodec_send_packet(ctx_, nullptr);
	bool ignored_error = false;
	drain(out, &ignored_error);
	// EOF を送った後は send_packet がエラーを返す状態になるので、続けて push() できるように、状態を
	// 戻しておく (avcodec のドレインの手順)。
	avcodec_flush_buffers(ctx_);
}

} // namespace cordmux::decode

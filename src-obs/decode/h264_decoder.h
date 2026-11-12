// SPDX-License-Identifier: Apache-2.0

// H.264 (Annex B) のアクセスユニットを 1 つずつデコードし、I420 か NV12 のフレームを返す (H264Decoder)。
// キーフレームのアクセスユニットに SPS と PPS が含まれていることを前提にする (docs/wire_format.md の
// 「映像のペイロード」)。
// libobs にも Qt にも依存しない。C++17 と obs-deps の FFmpeg (avcodec と avutil) だけを使う。
// 例外を投げない。壊れた入力 (乱数のバイト列、キーフレームより前のデータ、壊れたヘッダー) でも
// クラッシュせず、失敗を統計と戻り値で返す。

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// FFmpeg のヘッダー (libavutil/common.h の av_clip など) は int への暗黙の型変換を多く含み、/W4 では
// C4244 の警告が出る。cordmux のコードではないので、ここだけ抑える (tests/CMakeLists.txt は、
// align/*.cpp と test_*.cpp を /W4 で警告なしにビルドする)。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace cordmux::decode {

// このクラスが返すフレームのピクセルフォーマット。どちらも 4:2:0。
enum class PixelFormat {
	I420, // Y、U、V の 3 プレーン (ソフトウェアデコードの既定)
	NV12, // Y と UV (インターリーブ) の 2 プレーン (多くの GPU デコードの既定)
};

struct DecoderConfig {
	// デコードのスレッド数 (ストリームごと)。0 以下なら 2 にする。
	int thread_count = 2;

	// ハードウェアデコードの種類。AV_HWDEVICE_TYPE_NONE なら、ソフトウェアデコードだけを使う。
	// どの種類を使うかは呼び出し元が決める (pipeline/ が設定の decoder から決める)。指定した種類に、
	// このビルドの H.264 デコーダーが対応していないか、デバイスを開けなければ、ソフトウェアデコードに
	// 切り替えて DecoderStats::hw_fallback を true にする。
	AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
};

// デコードの統計。
struct DecoderStats {
	uint64_t received = 0; // push() が受け取ったアクセスユニットの数
	uint64_t decoded = 0;  // デコードして DecodedFrame にしたフレームの数
	uint64_t failed = 0;   // 失敗として数えたアクセスユニットの数
	                       // (キーフレームを待つ間にデコーダーに渡さずに捨てたものと、avcodec が
	                       //  エラーを返したもの。デコードできても、変換できずに捨てたフレームは数えない)
	bool hw_fallback = false; // 指定した hw_type が使えず、ソフトウェアデコードに切り替えたか
};

// デコードしたフレーム 1 つ。データはこの構造体にコピーしてあるので、呼び出し元は FFmpeg の
// バッファの寿命を気にしなくてよい。
struct DecodedFrame {
	PixelFormat format = PixelFormat::I420;
	int width = 0;
	int height = 0;
	// プレーンごとの 1 行のバイト数 (AVFrame::linesize のまま。幅より大きいことがある)。
	int linesize[3] = {0, 0, 0};
	// プレーンごとのデータ。I420 は [0]=Y [1]=U [2]=V、NV12 は [0]=Y [1]=UV ([2] は空)。
	std::vector<uint8_t> plane[3];
	int64_t timestamp_us = 0; // push() に渡した値をそのまま返す (整列は align/ が行う)
	bool full_range = false;  // true なら full range、false なら limited range
};

class H264Decoder {
public:
	explicit H264Decoder(const DecoderConfig &config = DecoderConfig{});
	~H264Decoder();

	H264Decoder(const H264Decoder &) = delete;
	H264Decoder &operator=(const H264Decoder &) = delete;

	// Annex B の 1 アクセスユニットを渡し、デコードできたフレーム (0 個以上) を *out の末尾に追加する。
	// data が nullptr でも size が 0 でも、失敗として数えるだけでクラッシュしない。
	//
	// 戻り値は、この呼び出しの後に送信側へキーフレームを要求するべきか。true になるのは、(a) まだ 1 つも
	// キーフレームをデコードできていないとき、(b) この呼び出しのデコードが失敗したとき。(b) の後も、
	// 次のキーフレームをデコードするまでは (a) と同じく true を返し続ける。呼び出し元はこれを見て、
	// 送信側に keyframe_request を送るかを決める。
	bool push(const uint8_t *data, size_t size, int64_t timestamp_us, std::vector<DecodedFrame> *out);

	// ストリームの終わりで、デコーダーの中に残っているフレームを *out に追加する。デコーダーを開いて
	// いなければ何もしない。呼び出した後も push() を続けられる (avcodec_flush_buffers で状態を戻す)。
	void flush(std::vector<DecodedFrame> *out);

	const DecoderStats &stats() const { return stats_; }

private:
	bool open_codec();
	void close_codec();
	static bool contains_idr(const uint8_t *data, size_t size);
	// 1 アクセスユニットを avcodec に送り、その場で受け取れるフレームをすべて *out に追加する。
	// 送信か受信が (EAGAIN と EOF 以外の) エラーを返したら *avcodec_error を true にする
	// (呼び出す前に false にしておく)。
	void decode_au(const uint8_t *data, size_t size, int64_t timestamp_us, std::vector<DecodedFrame> *out,
	                bool *avcodec_error);
	// avcodec_receive_frame を EAGAIN か EOF まで呼び、得られたフレームを変換して *out に追加する。
	void drain(std::vector<DecodedFrame> *out, bool *avcodec_error);
	// AVFrame を DecodedFrame に変換する (タイムスタンプは frame->pts から取る)。ハードウェアのフレームは
	// 先に読み戻す。ピクセルフォーマットが対応していなければ false を返し、呼び出し元はそのフレームを
	// 捨てて続ける。
	bool convert_frame(AVFrame *frame, DecodedFrame *result);
	static enum AVPixelFormat choose_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *formats);

	DecoderConfig config_{};
	DecoderStats stats_{};

	AVCodecContext *ctx_ = nullptr;
	AVBufferRef *hw_device_ctx_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVFrame *hw_sw_frame_ = nullptr; // ハードウェアのフレームを読み戻す先 (hw_type が NONE のときは使わない)

	enum AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE; // choose_hw_format が選ぶピクセルフォーマット

	// 一度でもキーフレームをデコードできたか。false の間は、キーフレームを含まないアクセスユニットを
	// avcodec に渡さずに捨てる (壊れた入力をそのまま渡さない)。
	bool saw_idr_ = false;
};

} // namespace cordmux::decode

// SPDX-License-Identifier: Apache-2.0

// record/record_writer.h の実装。
// record/ の中で libavformat、libavcodec、libavutil を読み込むのは、このファイルだけである (ヘッダーには
// 出さない)。
// 読み込み方 (C4244 の警告の抑止) は decode/ffmpeg_link_versions.cpp と同じである。

#include "record/record_writer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // FFmpeg のヘッダーの暗黙の型変換の警告 (decode/ffmpeg_link_versions.cpp と同じ)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace cordmux::record {

namespace {

// 0 を書くときに、1 回で書く大きさ (この大きさの 0 の配列を使い回す)。
constexpr std::size_t kZeroChunkBytes = 1024 * 1024;

// マイクロ秒を timescale の単位に修正する。最も近い整数に丸める (record_black.h と同じ)。
std::int64_t us_to_ticks(std::int64_t us, int timescale)
{
	if (timescale <= 0)
		return 0;
	const std::int64_t scale = static_cast<std::int64_t>(timescale);
	if (us >= 0)
		return (us * scale + 500000) / 1000000;
	return -((-us * scale + 500000) / 1000000);
}

// UTF-8 のパスをそのまま受け取る。std::filesystem::u8path は、文字列をアクティブコードページとして読まない
// (record/ から std::ofstream を直接使わないのと同じ理由。record_types.h の FileHandle のコメント)。
std::filesystem::path utf8_path(const std::string &path)
{
	return std::filesystem::u8path(path);
}

} // namespace

// ---------------------------------------------------------------------------
// SegmentWriter
// ---------------------------------------------------------------------------

struct SegmentWriter::Impl {
	AVFormatContext *ctx = nullptr;
	AVStream *stream = nullptr;
	AVPacket *pending = nullptr; // 1 フレーム遅らせて書く映像 (record_writer.h の write_video)
	std::int64_t pending_ticks = 0;
	bool has_pending = false;
	bool header_written = false;
	MediaKind media = MediaKind::Video;
	int timescale = 0;
	std::int64_t frame_period_ticks = 0; // 最後の 1 フレームの長さ
	std::int64_t audio_cursor_samples = 0;
	int audio_block_align = 8; // 2ch x f32
	SegmentWriteStats stats;

	void teardown()
	{
		if (pending != nullptr) {
			av_packet_free(&pending);
			pending = nullptr;
		}
		has_pending = false;
		if (ctx != nullptr) {
			if (ctx->pb != nullptr)
				avio_closep(&ctx->pb);
			avformat_free_context(ctx);
			ctx = nullptr;
		}
		stream = nullptr;
		header_written = false;
	}

	bool begin(const std::string &path)
	{
		if (avformat_alloc_output_context2(&ctx, nullptr, "mov", path.c_str()) < 0 || ctx == nullptr)
			return false;
		stream = avformat_new_stream(ctx, nullptr);
		if (stream == nullptr) {
			teardown();
			return false;
		}
		return true;
	}

	// movflags と frag_duration を渡す。empty_moov は途中で止まっても読めるようにするため、hybrid_fragmented は
	// 閉じたときに普通の mov に書き直し、録画ファイルの作成の concat がそのまま読めるようにするためである (hybrid_fragmented は
	// obs-deps の FFmpeg n7.1.1 の mov の muxer にある)。frag_keyframe はキーフレームでフラグメントを区切る。
	bool write_header(const std::string &path)
	{
		if (avio_open(&ctx->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
			teardown();
			return false;
		}
		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof+hybrid_fragmented", 0);
		av_dict_set_int(&opts, "frag_duration", kFragDurationUs, 0);
		const int rc = avformat_write_header(ctx, &opts);
		av_dict_free(&opts);
		if (rc < 0) {
			teardown();
			return false;
		}
		header_written = true;
		// muxer が timescale を変えることがあるので、ヘッダーを書いた後の値を使う。
		if (stream->time_base.den > 0)
			timescale = stream->time_base.den;
		stats.timescale = timescale;
		return true;
	}

	bool send(AVPacket *pkt)
	{
		if (ctx == nullptr || !header_written)
			return false;
		if (av_write_frame(ctx, pkt) < 0)
			return false;
		// 途中で止まってもそこまで読めるように、書いた分をすぐにフラッシュする。
		if (ctx->pb != nullptr) {
			avio_flush(ctx->pb);
			// 書いたバイト数をここでも進める。空き容量の監視は開いたままのセグメントの分も数える
			// (record_space.h の bytes_per_second_of) ので、閉じたときにだけ数えると、開いている間の消費が
			// 0 に見える。位置を聞くだけなので、読み書きは起きない。
			stats.bytes = avio_tell(ctx->pb);
		}
		return true;
	}
};

SegmentWriter::SegmentWriter() : impl_(new Impl()) {}

SegmentWriter::~SegmentWriter()
{
	if (impl_)
		impl_->teardown();
}

bool SegmentWriter::is_open() const
{
	return impl_ && impl_->ctx != nullptr && impl_->header_written;
}

int SegmentWriter::timescale() const
{
	return impl_ ? impl_->timescale : 0;
}

const SegmentWriteStats &SegmentWriter::stats() const
{
	return impl_->stats;
}

bool SegmentWriter::open_video(const std::string &path, const SegmentVideoSpec &spec)
{
	if (is_open() || spec.avcc.empty() || spec.width <= 0 || spec.height <= 0)
		return false;
	impl_->teardown();
	impl_->stats = SegmentWriteStats();
	impl_->media = MediaKind::Video;
	impl_->timescale = kVideoTimescale;
	impl_->frame_period_ticks = 1;
	if (spec.fps_num > 0 && spec.fps_den > 0) {
		impl_->frame_period_ticks =
			(static_cast<std::int64_t>(kVideoTimescale) * spec.fps_den + spec.fps_num / 2) / spec.fps_num;
		if (impl_->frame_period_ticks <= 0)
			impl_->frame_period_ticks = 1;
	}

	if (!impl_->begin(path))
		return false;

	AVCodecParameters *par = impl_->stream->codecpar;
	par->codec_type = AVMEDIA_TYPE_VIDEO;
	par->codec_id = AV_CODEC_ID_H264;
	par->codec_tag = 0;
	par->width = spec.width;
	par->height = spec.height;
	// avcC は、組み立てたバイト列をそのまま入れる (muxer に組み立てさせない。record_avcc.h のコメント)。
	par->extradata = static_cast<std::uint8_t *>(av_mallocz(spec.avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
	if (par->extradata == nullptr) {
		impl_->teardown();
		return false;
	}
	std::memcpy(par->extradata, spec.avcc.data(), spec.avcc.size());
	par->extradata_size = static_cast<int>(spec.avcc.size());

	impl_->stream->time_base.num = 1;
	impl_->stream->time_base.den = kVideoTimescale;
	if (spec.fps_num > 0 && spec.fps_den > 0) {
		impl_->stream->avg_frame_rate.num = spec.fps_num;
		impl_->stream->avg_frame_rate.den = spec.fps_den;
	}
	return impl_->write_header(path);
}

bool SegmentWriter::open_audio(const std::string &path, const SegmentAudioSpec &spec)
{
	if (is_open() || spec.sample_rate <= 0 || spec.channels <= 0)
		return false;
	impl_->teardown();
	impl_->stats = SegmentWriteStats();
	impl_->media = MediaKind::Audio;
	impl_->timescale = spec.sample_rate;
	impl_->audio_cursor_samples = 0;
	impl_->audio_block_align = spec.channels * 4; // f32 (4 バイト)

	if (!impl_->begin(path))
		return false;

	AVCodecParameters *par = impl_->stream->codecpar;
	par->codec_type = AVMEDIA_TYPE_AUDIO;
	par->codec_id = AV_CODEC_ID_PCM_F32LE;
	par->codec_tag = 0;
	par->format = AV_SAMPLE_FMT_FLT;
	par->sample_rate = spec.sample_rate;
	par->bits_per_coded_sample = 32;
	par->block_align = impl_->audio_block_align;
	av_channel_layout_default(&par->ch_layout, spec.channels);

	impl_->stream->time_base.num = 1;
	impl_->stream->time_base.den = spec.sample_rate;
	return impl_->write_header(path);
}

bool SegmentWriter::write_video(const std::uint8_t *sample, std::size_t len, std::int64_t pts_us, bool keyframe)
{
	if (!is_open() || impl_->media != MediaKind::Video || sample == nullptr || len == 0)
		return false;

	std::int64_t ticks = us_to_ticks(pts_us, impl_->timescale);
	if (ticks < 0)
		ticks = 0;

	// 先に、前のフレームを「このフレームのタイムスタンプとの差」を長さにして書く (record_writer.h の write_video)。
	if (impl_->has_pending) {
		if (ticks <= impl_->pending_ticks) {
			// 同じ位置には置けないので、1 単位ずらして数える (record_writer.h の write_video)。
			ticks = impl_->pending_ticks + 1;
			++impl_->stats.nonmonotonic_pushed;
		}
		impl_->pending->duration = ticks - impl_->pending_ticks;
		if (!impl_->send(impl_->pending))
			return false;
		av_packet_unref(impl_->pending);
		impl_->has_pending = false;
	} else if (impl_->stats.sample_count == 0) {
		// セグメントの先頭は必ず 0 にする。呼び出し元が 0 以外を渡しても 0 にする。
		ticks = 0;
	}

	if (impl_->pending == nullptr) {
		impl_->pending = av_packet_alloc();
		if (impl_->pending == nullptr)
			return false;
	}
	if (av_new_packet(impl_->pending, static_cast<int>(len)) < 0)
		return false;
	std::memcpy(impl_->pending->data, sample, len);
	impl_->pending->stream_index = impl_->stream->index;
	impl_->pending->pts = ticks;
	impl_->pending->dts = ticks;
	impl_->pending->duration = impl_->frame_period_ticks;
	// ワイヤー形式のキーフレームのフラグを AV_PKT_FLAG_KEY に反映する (反映しないと frag_keyframe が動作しない)。
	impl_->pending->flags = keyframe ? AV_PKT_FLAG_KEY : 0;
	impl_->pending_ticks = ticks;
	impl_->has_pending = true;

	++impl_->stats.sample_count;
	impl_->stats.duration_ticks = ticks + impl_->frame_period_ticks;
	return true;
}

bool SegmentWriter::write_audio(const void *pcm, std::size_t bytes, std::int64_t samples)
{
	if (!is_open() || impl_->media != MediaKind::Audio || pcm == nullptr || bytes == 0 || samples <= 0)
		return false;

	AVPacket *pkt = av_packet_alloc();
	if (pkt == nullptr)
		return false;
	bool ok = av_new_packet(pkt, static_cast<int>(bytes)) >= 0;
	if (ok) {
		std::memcpy(pkt->data, pcm, bytes);
		pkt->stream_index = impl_->stream->index;
		pkt->pts = impl_->audio_cursor_samples;
		pkt->dts = impl_->audio_cursor_samples;
		pkt->duration = samples;
		pkt->flags = AV_PKT_FLAG_KEY; // PCM はどのサンプルも単独で読める (frag_duration で区切れるようにする)
		ok = impl_->send(pkt);
	}
	av_packet_free(&pkt);
	if (!ok)
		return false;

	impl_->audio_cursor_samples += samples;
	impl_->stats.sample_count = impl_->audio_cursor_samples;
	impl_->stats.duration_ticks = impl_->audio_cursor_samples;
	return true;
}

bool SegmentWriter::write_audio_silence(std::int64_t samples)
{
	if (!is_open() || impl_->media != MediaKind::Audio || samples <= 0)
		return false;
	const std::size_t block = static_cast<std::size_t>(impl_->audio_block_align);
	const std::int64_t per_chunk = static_cast<std::int64_t>(kZeroChunkBytes / (block > 0 ? block : 1));
	const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(std::min(samples, per_chunk)) * block, 0);
	std::int64_t left = samples;
	while (left > 0) {
		const std::int64_t now = std::min(left, per_chunk);
		if (!write_audio(zeros.data(), static_cast<std::size_t>(now) * block, now))
			return false;
		left -= now;
	}
	return true;
}

bool SegmentWriter::close(SegmentWriteStats *stats)
{
	if (impl_->ctx == nullptr)
		return false;
	bool ok = impl_->header_written;
	if (ok && impl_->has_pending) {
		// 最後の 1 フレームは長さを決められないので、フレームの間隔を使う (record_writer.h の write_video)。
		impl_->pending->duration = impl_->frame_period_ticks;
		ok = impl_->send(impl_->pending);
		av_packet_unref(impl_->pending);
		impl_->has_pending = false;
	}
	if (ok)
		ok = av_write_trailer(impl_->ctx) >= 0;
	// 書いたバイト数は trailer の後に読む (record_writer.h の close のコメント)。
	if (impl_->ctx->pb != nullptr) {
		const std::int64_t size = avio_size(impl_->ctx->pb);
		impl_->stats.bytes = size > 0 ? size : avio_tell(impl_->ctx->pb);
	}
	if (stats != nullptr)
		*stats = impl_->stats;
	impl_->teardown();
	return ok;
}

void SegmentWriter::abort()
{
	impl_->teardown();
}

// ---------------------------------------------------------------------------
// 読み出し
// ---------------------------------------------------------------------------

bool read_segment_pcm(const std::string &path, const std::function<bool(const void *, std::size_t)> &sink,
		      SegmentPcmInfo *info)
{
	AVFormatContext *ctx = nullptr;
	if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
		return false;
	bool ok = avformat_find_stream_info(ctx, nullptr) >= 0;

	int index = -1;
	if (ok) {
		for (unsigned i = 0; i < ctx->nb_streams; ++i) {
			if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				index = static_cast<int>(i);
				break;
			}
		}
		ok = index >= 0;
	}

	int block = 8;
	if (ok) {
		const AVCodecParameters *par = ctx->streams[index]->codecpar;
		const int channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
		const int bits = av_get_bits_per_sample(par->codec_id);
		block = channels * (bits > 0 ? bits / 8 : 4);
		if (block <= 0)
			block = 8;
		if (info != nullptr) {
			info->sample_rate = par->sample_rate;
			info->channels = channels;
			info->samples = 0;
		}
	}

	if (ok) {
		AVPacket *pkt = av_packet_alloc();
		ok = pkt != nullptr;
		while (ok && av_read_frame(ctx, pkt) >= 0) {
			if (pkt->stream_index == index && pkt->size > 0) {
				if (sink && !sink(pkt->data, static_cast<std::size_t>(pkt->size)))
					ok = false;
				else if (info != nullptr)
					info->samples += pkt->size / block;
			}
			av_packet_unref(pkt);
		}
		if (pkt != nullptr)
			av_packet_free(&pkt);
	}

	avformat_close_input(&ctx);
	return ok;
}

OutputProbe probe_output(const std::string &path, int expected_timescale)
{
	OutputProbe probe;
	if (expected_timescale <= 0)
		return probe;

	AVFormatContext *ctx = nullptr;
	if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
		return probe;
	if (avformat_find_stream_info(ctx, nullptr) < 0) {
		avformat_close_input(&ctx);
		return probe;
	}

	const AVRational target{1, expected_timescale};
	int video_index = -1;
	int audio_index = -1;
	std::int64_t audio_block = 8;
	for (unsigned i = 0; i < ctx->nb_streams; ++i) {
		const AVCodecParameters *par = ctx->streams[i]->codecpar;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0)
			video_index = static_cast<int>(i);
		if (par->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
			audio_index = static_cast<int>(i);
			const int channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
			const int bits = av_get_bits_per_sample(par->codec_id);
			audio_block = channels * (bits > 0 ? bits / 8 : 4);
			if (audio_block <= 0)
				audio_block = 8;
		}
	}
	probe.has_video = video_index >= 0;
	probe.has_audio = audio_index >= 0;

	const int length_index = video_index >= 0 ? video_index : audio_index;
	if (length_index >= 0) {
		const AVStream *st = ctx->streams[length_index];
		if (st->duration > 0) {
			probe.duration_ticks = av_rescale_q(st->duration, st->time_base, target);
			if (st->time_base.num == 1 && st->time_base.den > 0) {
				probe.stream_timescale = st->time_base.den;
				probe.stream_duration_ticks = st->duration;
			}
		}
	}
	if (video_index >= 0)
		probe.frames = ctx->streams[video_index]->nb_frames;

	// フレーム数とサンプル数を数え直す。mov のヘッダーが 0 を示すとき (フラグメントのまま閉じたとき) も、整数で
	// 比べられるようにするためである。
	const bool need_video_count = video_index >= 0 && probe.frames <= 0;
	const bool need_audio_count = audio_index >= 0;
	if (need_video_count || need_audio_count) {
		AVPacket *pkt = av_packet_alloc();
		if (pkt != nullptr) {
			while (av_read_frame(ctx, pkt) >= 0) {
				if (need_video_count && pkt->stream_index == video_index)
					++probe.frames;
				if (need_audio_count && pkt->stream_index == audio_index && pkt->size > 0)
					probe.audio_samples += pkt->size / audio_block;
				av_packet_unref(pkt);
			}
			av_packet_free(&pkt);
		}
	}
	if (probe.duration_ticks <= 0 && video_index < 0 && audio_index >= 0)
		probe.duration_ticks = probe.audio_samples;

	probe.ok = length_index >= 0;
	avformat_close_input(&ctx);
	return probe;
}

// ---------------------------------------------------------------------------
// 途中で切れたセグメントの読める部分
// ---------------------------------------------------------------------------

namespace {

// AVCC のサンプルが、NAL の並びとしてちょうど終わっているか (前に付ける長さは、avcC が示す 1 から 4 バイト)。
// 長さが 0 の NAL、forbidden_zero_bit が 1 の NAL、型が 0 の NAL、サンプルの外にはみ出す長さは、読めないとする。
// 電源が落ちたセッションの、0 で埋まった末尾は、最初の長さが 0 なのでここで止まる。
bool avcc_sample_well_formed(const std::uint8_t *data, int size, int length_size)
{
	if (data == nullptr || size <= 0 || length_size < 1 || length_size > 4)
		return false;
	std::int64_t at = 0;
	const std::int64_t end = size;
	while (at < end) {
		if (at + length_size > end)
			return false;
		std::int64_t nal = 0;
		for (int i = 0; i < length_size; ++i)
			nal = (nal << 8) | data[at + i];
		at += length_size;
		if (nal <= 0 || at + nal > end)
			return false;
		const std::uint8_t head = data[at];
		if ((head & 0x80) != 0 || (head & 0x1F) == 0)
			return false;
		at += nal;
	}
	return at == end;
}

// avcC が示す、前に付ける長さのバイト数 (読めなければ 4。録画のコードが組み立てる値と同じ。record_avcc.h)。
int avcc_length_size(const AVCodecParameters *par)
{
	if (par == nullptr || par->extradata == nullptr || par->extradata_size < 5 || par->extradata[0] != 1)
		return 4;
	return (par->extradata[4] & 0x03) + 1;
}

} // namespace

ReadablePart copy_readable_part(const std::string &in_path, const std::string &out_path, std::int64_t max_samples,
				std::int64_t last_duration_ticks, const std::function<bool()> &should_stop)
{
	ReadablePart part;
	AVFormatContext *in = nullptr;
	const int open_rc = avformat_open_input(&in, in_path.c_str(), nullptr, nullptr);
	if (open_rc < 0) {
		// 中身の形で開けないとき (0 バイト、ヘッダーの途中で切れた) は AVERROR_INVALIDDATA か
		// AVERROR_EOF になる。それ以外は読み出しそのものの失敗である (開けない、読めない、ない。
		// ReadablePart::io_failed)。
		part.io_failed = open_rc != AVERROR_INVALIDDATA && open_rc != AVERROR_EOF;
		return part;
	}

	// セグメントは映像か音声のどちらか 1 つだけを持つ。ヘッダーの stsd から形が分かるので、
	// avformat_find_stream_info は呼ばない (切れた末尾まで先読みしないため)。
	int index = -1;
	for (unsigned i = 0; i < in->nb_streams; ++i) {
		const AVMediaType type = in->streams[i]->codecpar->codec_type;
		if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO) {
			index = static_cast<int>(i);
			break;
		}
	}
	if (index < 0 || in->pb == nullptr) {
		avformat_close_input(&in);
		return part;
	}
	part.opened = true;
	const AVStream *in_st = in->streams[index];
	const AVCodecParameters *in_par = in_st->codecpar;
	part.media = in_par->codec_type == AVMEDIA_TYPE_VIDEO ? MediaKind::Video : MediaKind::Audio;
	part.timescale = (in_st->time_base.num == 1 && in_st->time_base.den > 0) ? in_st->time_base.den : 0;
	part.source_bytes = avio_size(in->pb);
	const int length_size = avcc_length_size(in_par);
	int block = 8;
	if (part.media == MediaKind::Audio) {
		const int channels = in_par->ch_layout.nb_channels > 0 ? in_par->ch_layout.nb_channels : 2;
		const int bits = av_get_bits_per_sample(in_par->codec_id);
		block = channels * (bits > 0 ? bits / 8 : 4);
		if (block <= 0)
			block = 8;
	}

	AVFormatContext *out = nullptr;
	AVStream *out_st = nullptr;
	bool out_ok = true;
	std::int64_t first_dts = AV_NOPTS_VALUE;
	std::int64_t end_dts = AV_NOPTS_VALUE;

	// 出力は、最初の読めるサンプルで開く (読めるものがなければ作らない)。SegmentWriter と同じ movflags で書く
	// (hybrid_fragmented で閉じると普通の mov になり、録画ファイルの作成の concat がそのまま読める)。
	auto open_output = [&]() -> bool {
		if (avformat_alloc_output_context2(&out, nullptr, "mov", out_path.c_str()) < 0 || out == nullptr)
			return false;
		out_st = avformat_new_stream(out, nullptr);
		if (out_st == nullptr || avcodec_parameters_copy(out_st->codecpar, in_par) < 0)
			return false;
		out_st->codecpar->codec_tag = 0;
		out_st->time_base = in_st->time_base;
		if (avio_open(&out->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0)
			return false;
		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof+hybrid_fragmented", 0);
		av_dict_set_int(&opts, "frag_duration", kFragDurationUs, 0);
		const int rc = avformat_write_header(out, &opts);
		av_dict_free(&opts);
		return rc >= 0;
	};

	// 1 つ遅らせて書く (SegmentWriter::write_video と同じ)。読み出したパケットの長さは当てにならない。
	// フラグメントの mov では、libavformat が推測した値が入ることがある (10 fps のセグメントで、1/30 秒の長さが
	// 返る)。長さは次のサンプルの時刻との差で決め、最後の 1 つだけは、呼び出し元が渡す長さ (映像では
	// フレームの間隔) を使う。
	AVPacket *pkt = av_packet_alloc();
	AVPacket *held = av_packet_alloc();
	bool have_held = false;
	std::int64_t held_dts = AV_NOPTS_VALUE;
	auto put_held = [&](std::int64_t duration) {
		if (!have_held)
			return;
		if (duration > 0)
			held->duration = duration;
		if (held_dts != AV_NOPTS_VALUE)
			end_dts = held_dts + (held->duration > 0 ? held->duration : 0);
		if (!out_path.empty() && out_ok) {
			if (out == nullptr && !open_output())
				out_ok = false;
			if (out_ok) {
				av_packet_rescale_ts(held, in_st->time_base, out_st->time_base);
				held->stream_index = 0;
				held->pos = -1;
				if (av_write_frame(out, held) < 0)
					out_ok = false;
			}
		}
		av_packet_unref(held);
		have_held = false;
	};
	std::int64_t polled = 0;
	while (pkt != nullptr && held != nullptr && out_ok) {
		// 打ち切りの指示は 64 サンプルごとに確認する (サンプルごとに確認するほど急ぐ必要はない)。
		if (should_stop && (polled++ % 64) == 0 && should_stop()) {
			part.stopped = true;
			break;
		}
		if (av_read_frame(in, pkt) < 0) {
			// 読み終わったか、ここから先は読めない (どちらも末尾として捨てる)。ただし読み出しそのものが
			// 失敗したとき (avio が OS の読み出しの失敗を error に残した。入力の終わりは error に
			// 残らない) は、後ろに読めるものが残っているかもしれないので io_failed にする。
			if (in->pb != nullptr && in->pb->error < 0 && in->pb->error != AVERROR_EOF)
				part.io_failed = true;
			break;
		}
		if (pkt->stream_index != index) {
			av_packet_unref(pkt);
			continue;
		}
		bool readable = pkt->size > 0 && (pkt->flags & AV_PKT_FLAG_CORRUPT) == 0 && pkt->pos >= 0 &&
				(part.source_bytes <= 0 || pkt->pos + pkt->size <= part.source_bytes);
		if (readable && part.media == MediaKind::Video)
			readable = avcc_sample_well_formed(pkt->data, pkt->size, length_size);
		if (readable && part.media == MediaKind::Audio)
			readable = pkt->size % block == 0;
		if (!readable) {
			av_packet_unref(pkt);
			// サンプルの途中で OS の読み出しが失敗すると、libavformat は、読めた所までに縮めたサンプルに
			// AV_PKT_FLAG_CORRUPT を付けて返す (av_get_packet)。中身の形で読めない末尾 (電源が落ちたときの
			// 切れ目は入力の終わりなので、error に残らない) と分けるため、ここでも avio の error を見る
			// (上の av_read_frame の失敗と同じ分け方)。
			if (in->pb != nullptr && in->pb->error < 0 && in->pb->error != AVERROR_EOF)
				part.io_failed = true;
			break; // 最初に読めなかった所から後ろは捨てる (record_writer.h のコメント)
		}

		const std::int64_t count = part.media == MediaKind::Video ? 1 : pkt->size / block;
		const std::int64_t dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
		if (first_dts == AV_NOPTS_VALUE && dts != AV_NOPTS_VALUE)
			first_dts = dts;
		// 前のサンプルの長さは、このサンプルの時刻との差 (音声はサンプル数そのもの。timescale が
		// サンプリングレートのため)。
		if (have_held) {
			const std::int64_t gap = (dts != AV_NOPTS_VALUE && held_dts != AV_NOPTS_VALUE) ? dts - held_dts : 0;
			put_held(part.media == MediaKind::Audio ? 0 : gap);
		}
		if (part.media == MediaKind::Audio)
			pkt->duration = count;
		part.samples += count;
		part.readable_end = pkt->pos + pkt->size;
		av_packet_move_ref(held, pkt);
		have_held = true;
		held_dts = dts;
		if (out_path.empty() && max_samples > 0 && part.samples >= max_samples)
			break;
	}
	// 最後の 1 つ: 映像はフレームの間隔 (呼び出し元が渡す)、音声はサンプル数。渡されなければ読み出した値のまま
	// 使う。
	if (!part.stopped)
		put_held(part.media == MediaKind::Video ? last_duration_ticks : 0);
	else if (have_held)
		av_packet_unref(held);
	if (pkt != nullptr)
		av_packet_free(&pkt);
	if (held != nullptr)
		av_packet_free(&held);

	if (part.media == MediaKind::Audio)
		part.duration_ticks = part.samples; // 音声の timescale はサンプリングレート
	else if (first_dts != AV_NOPTS_VALUE && end_dts != AV_NOPTS_VALUE && end_dts > first_dts)
		part.duration_ticks = end_dts - first_dts;

	if (out != nullptr) {
		if (out_ok && !part.stopped)
			out_ok = av_write_trailer(out) >= 0;
		if (out->pb != nullptr) {
			const std::int64_t size = avio_size(out->pb);
			part.written_bytes = size > 0 ? size : avio_tell(out->pb);
			avio_closep(&out->pb);
		}
		avformat_free_context(out);
		part.written = out_ok && !part.stopped;
	}
	avformat_close_input(&in);
	return part;
}

// ---------------------------------------------------------------------------
// RawFile
// ---------------------------------------------------------------------------

struct RawFile::Impl {
	AVIOContext *pb = nullptr;
};

RawFile::RawFile() : impl_(new Impl()) {}

RawFile::~RawFile()
{
	close();
}

bool RawFile::is_open() const
{
	return impl_ && impl_->pb != nullptr;
}

bool RawFile::create_zeroed(const std::string &path, std::int64_t size_bytes)
{
	close();
	if (size_bytes < 0)
		return false;
	if (avio_open(&impl_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
		impl_->pb = nullptr;
		return false;
	}
	const std::vector<std::uint8_t> zeros(kZeroChunkBytes, 0);
	std::int64_t left = size_bytes;
	while (left > 0) {
		const std::int64_t now = std::min<std::int64_t>(left, static_cast<std::int64_t>(kZeroChunkBytes));
		avio_write(impl_->pb, zeros.data(), static_cast<int>(now));
		left -= now;
	}
	avio_flush(impl_->pb);
	return true;
}

bool RawFile::open_append(const std::string &path)
{
	close();
	// truncate=0 を指定する。libavformat の file プロトコルは、読み書きで開いても既定で中身を捨てる (指定しないと、
	// 録画ログの前の行が消える)。
	AVDictionary *opts = nullptr;
	av_dict_set_int(&opts, "truncate", 0, 0);
	const int rc = avio_open2(&impl_->pb, path.c_str(), AVIO_FLAG_READ_WRITE, nullptr, &opts);
	av_dict_free(&opts);
	if (rc < 0) {
		impl_->pb = nullptr;
		return false;
	}
	// avio_seek は SEEK_SET と SEEK_CUR しか受け付けない (SEEK_END は EINVAL になる)。末尾へ移るには、大きさを
	// 調べてから絶対位置で移る。
	const std::int64_t size = avio_size(impl_->pb);
	if (size < 0)
		return false;
	return avio_seek(impl_->pb, size, SEEK_SET) >= 0;
}

bool RawFile::append(const void *data, std::size_t len)
{
	if (!is_open() || data == nullptr)
		return false;
	if (len == 0)
		return true;
	avio_write(impl_->pb, static_cast<const std::uint8_t *>(data), static_cast<int>(len));
	avio_flush(impl_->pb);
	return true;
}

bool RawFile::write_at(std::int64_t offset_bytes, const void *data, std::size_t len)
{
	if (!is_open() || data == nullptr || offset_bytes < 0)
		return false;
	if (len == 0)
		return true;
	if (avio_seek(impl_->pb, offset_bytes, SEEK_SET) < 0)
		return false;
	avio_write(impl_->pb, static_cast<const std::uint8_t *>(data), static_cast<int>(len));
	return true;
}

bool RawFile::close()
{
	if (!impl_ || impl_->pb == nullptr)
		return true;
	avio_flush(impl_->pb);
	const bool ok = avio_closep(&impl_->pb) >= 0;
	impl_->pb = nullptr;
	return ok;
}

// ---------------------------------------------------------------------------
// ファイルとフォルダー
// ---------------------------------------------------------------------------

bool remove_file(const std::string &path)
{
	std::error_code ec;
	return std::filesystem::remove(utf8_path(path), ec);
}

std::vector<std::string> list_subdirectories(const std::string &path)
{
	ListOutcome outcome = ListOutcome::Listed;
	return list_subdirectories(path, &outcome);
}

std::vector<std::string> list_subdirectories(const std::string &path, ListOutcome *outcome)
{
	if (outcome != nullptr)
		*outcome = ListOutcome::Listed;
	// UTF-8 として読めないパスは、開けない場所と同じに答える (u8path は例外を投げる。作成待ちを探す保存先の一覧の
	// ファイルが壊れていても、OBS を終了させないため)。
	std::filesystem::path dir;
	try {
		dir = utf8_path(path);
	} catch (const std::system_error &) {
		if (outcome != nullptr)
			*outcome = ListOutcome::Failed;
		return std::vector<std::string>();
	}
	std::error_code ec;
	std::filesystem::directory_iterator it(dir, ec);
	if (ec) {
		if (outcome != nullptr)
			*outcome = ec == std::errc::no_such_file_or_directory ? ListOutcome::Missing : ListOutcome::Failed;
		return std::vector<std::string>();
	}
	// 2 つ目からの読み出しの失敗は Failed で返す (例外を投げる ++ を使わない。record_writer.h の
	// collect_subdirectory_names)。
	return collect_subdirectory_names(std::move(it), outcome);
}

bool read_whole_file(const std::string &path, std::string *out)
{
	if (out == nullptr)
		return false;
	out->clear();
	AVIOContext *pb = nullptr;
	if (avio_open(&pb, path.c_str(), AVIO_FLAG_READ) < 0)
		return false;
	std::vector<std::uint8_t> buffer(64 * 1024);
	for (;;) {
		const int got = avio_read(pb, buffer.data(), static_cast<int>(buffer.size()));
		if (got <= 0)
			break;
		out->append(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(got));
	}
	// 途中で読み出しが失敗したとき (avio が OS の失敗を error に残した。終わりは残らない) は、読めなかったと
	// 答える。途中までの中身を読めたとして渡すと、作成待ちの集計が録画ログを短いものとして読むためである。
	const bool failed = pb->error < 0 && pb->error != AVERROR_EOF;
	avio_closep(&pb);
	if (failed)
		out->clear();
	return !failed;
}

bool read_file_tail(const std::string &path, std::size_t max_bytes, std::string *out)
{
	if (out == nullptr)
		return false;
	out->clear();
	AVIOContext *pb = nullptr;
	if (avio_open(&pb, path.c_str(), AVIO_FLAG_READ) < 0)
		return false;
	const std::int64_t size = avio_size(pb);
	if (size > 0 && static_cast<std::uint64_t>(size) > max_bytes)
		avio_seek(pb, size - static_cast<std::int64_t>(max_bytes), SEEK_SET);
	std::vector<std::uint8_t> buffer(max_bytes > 0 ? max_bytes : 1);
	for (;;) {
		const int got = avio_read(pb, buffer.data(), static_cast<int>(buffer.size()));
		if (got <= 0)
			break;
		out->append(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(got));
		if (out->size() >= max_bytes)
			break;
	}
	avio_closep(&pb);
	return true;
}

unsigned record_avformat_major()
{
	return AV_VERSION_MAJOR(avformat_version());
}

std::string record_ffmpeg_version()
{
	const char *text = av_version_info();
	return text != nullptr ? std::string(text) : std::string();
}

} // namespace cordmux::record

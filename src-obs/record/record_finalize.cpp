// SPDX-License-Identifier: Apache-2.0

// record_finalize.h の実装。run_finalize は Recorder の録画ファイルの作成のスレッドで動き、録画ファイルごとに一時ファイルを作って
// FFmpeg の子プロセスを起動し、終わるまでポーリングで待つ。セグメントの読み出しは record_writer.h の関数を通し、
// libavformat のヘッダーは読み込まない。

#include "record/record_finalize.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <system_error>
#include <set>
#include <vector>

#include "record/record_branch.h"
#include "record/record_ledger.h"
#include "record/record_segment.h"
#include "record/record_writer.h"

namespace cordmux::record {

namespace {

// `-progress` の出力を末尾から読む量。1 回分の出力は 300 バイト前後なので、最後の 1 回分が必ず入る量にした。
constexpr std::size_t kProgressTailBytes = 4096;

// FFmpeg の子プロセスの出力を書き込むファイルのパス。失敗の種類を分ける手がかりになるので、録画ファイルの作成の後も残す。
// 録画の中間ファイルのフォルダーに置く。
std::string child_log_path(const std::string &materials_dir, const std::string &tag)
{
	return join_path(materials_dir, "ffmpeg_" + tag + ".log");
}

// 本文を 1 つのファイルとして書く。ファイルの操作は RecordPlatformOps の関数だけを通す。録画が OFF の間に
// ファイルを作らないことを、テストが RecordPlatformOps の呼び出しの回数で確認するためである。
bool write_text_file(const RecordPlatformOps &ops, const std::string &path, const std::string &body)
{
	if (!ops.open_file || !ops.append || !ops.close)
		return false;
	const FileHandle file = ops.open_file(path);
	if (file == kInvalidFile)
		return false;
	const bool ok = body.empty() || ops.append(file, body.data(), body.size());
	ops.close(file);
	return ok;
}

// f32 の PCM のチャンクを、録画ファイルに入れる形 (f32 か 24 ビット) に変換して書き込む。変換するのはここだけで、
// セグメントのファイルは f32 のまま変えない。
class PcmSink {
public:
	PcmSink(RawFile &file, config::RecordPcmFormat format, int channels)
		: file_(file), format_(format), channels_(channels > 0 ? channels : 2)
	{
	}

	void seek_to_sample(std::int64_t sample_index)
	{
		byte_cursor_ = sample_index * channels_ * pcm_bytes_per_sample(format_);
	}

	bool write_f32(const void *data, std::size_t len)
	{
		if (format_ == config::RecordPcmFormat::Float32)
			return put(data, len);

		// f32 を s24le に変換する。1 サンプルずつ丸め、範囲を外れた値は端の値にする。
		const std::size_t count = len / sizeof(float);
		const float *source = static_cast<const float *>(data);
		scratch_.resize(count * 3);
		for (std::size_t i = 0; i < count; ++i) {
			double value = static_cast<double>(source[i]) * 8388607.0;
			if (value > 8388607.0)
				value = 8388607.0;
			if (value < -8388608.0)
				value = -8388608.0;
			const std::int32_t whole = static_cast<std::int32_t>(std::lround(value));
			scratch_[i * 3 + 0] = static_cast<std::uint8_t>(whole & 0xFF);
			scratch_[i * 3 + 1] = static_cast<std::uint8_t>((whole >> 8) & 0xFF);
			scratch_[i * 3 + 2] = static_cast<std::uint8_t>((whole >> 16) & 0xFF);
		}
		return put(scratch_.data(), scratch_.size());
	}

	std::int64_t written_bytes() const { return written_; }

private:
	bool put(const void *data, std::size_t len)
	{
		if (!file_.write_at(byte_cursor_, data, len))
			return false;
		byte_cursor_ += static_cast<std::int64_t>(len);
		written_ += static_cast<std::int64_t>(len);
		return true;
	}

	RawFile &file_;
	config::RecordPcmFormat format_;
	int channels_;
	std::int64_t byte_cursor_ = 0;
	std::int64_t written_ = 0;
	std::vector<std::uint8_t> scratch_;
};

// 録画ファイルの作成の進み具合を持ち、変わったときだけ on_progress に渡す。
// 録画ファイル 1 つ分の割合 (0 から 1000) は、音声を組み立てる段階と FFmpeg の段階を、それぞれが書く量で重み付け
// してつなぐ。音声の段階の重みは生の PCM の量、FFmpeg の段階の重みは生の PCM の量と、その録画ファイルに入る映像の
// セグメントの量 (FFmpeg が読む量) の和である。段階ごとに 0 から数え直すと、同じ録画ファイルの割合が 100% から 0% に
// 戻る。半分ずつに固定すると、声だけの録画ファイルでも映像の段階の重みを持つ。割合は増えるときだけ書き換え、同じ
// 録画ファイルの中で戻ることはない。ProRes に再エンコードする録画ファイルは、FFmpeg の段階が量の割に遅いので、
// 後半の進みが遅く見える。
// 進み具合が止まったかは、最後にどれかの数が進んでからの時間を単調な時計で測って判定する。判定するのは、
// 子プロセスを待つポーリングのたびと、音声を書き込むたびである。どれかの数が進めば、止まったという判定を取り消す。
class ProgressMeter {
public:
	ProgressMeter(const FinalizeRun &run, FinalizeProgress &progress) : run_(run), progress_(progress)
	{
		const int stall_s = run.options.finalize_stall_s > 0 ? run.options.finalize_stall_s
								    : kDefaultFinalizeStallS;
		stall_us_ = static_cast<std::int64_t>(stall_s) * 1000000;
	}

	// 録画ファイル 1 つの作成を始める。2 つの段階の重み (書く量) を受け取り、割合を値なしに戻す。
	void begin_file(int index, std::int64_t pcm_weight, std::int64_t ffmpeg_weight)
	{
		progress_.current_index = index;
		progress_.current_permille.reset();
		pcm_weight_ = pcm_weight > 0 ? pcm_weight : 0;
		ffmpeg_weight_ = ffmpeg_weight > 0 ? ffmpeg_weight : 0;
		mark_progress();
		publish();
	}

	// 音声を組み立てる段階で書いた量を受け取る。段階の中の割合は、書いた量 / 全体の量。
	void pcm_written(std::int64_t written, std::int64_t total)
	{
		const std::int64_t whole = pcm_weight_ + ffmpeg_weight_;
		if (total <= 0 || whole <= 0) {
			check_stall();
			return;
		}
		const double part = static_cast<double>(std::min(written, total)) / static_cast<double>(total);
		offer(static_cast<int>(part * static_cast<double>(pcm_weight_) * 1000.0 / static_cast<double>(whole)));
		check_stall();
	}

	// FFmpeg の段階の進みを受け取る。段階の中の割合は、FFmpeg が報告した out_time_us / 録画ログから求めた長さ。
	void ffmpeg_time(std::int64_t out_time_us, std::int64_t expected_us)
	{
		const std::int64_t whole = pcm_weight_ + ffmpeg_weight_;
		if (expected_us > 0 && whole > 0 && out_time_us >= 0) {
			const double part = static_cast<double>(std::min(out_time_us, expected_us)) /
					    static_cast<double>(expected_us);
			const double value = (static_cast<double>(pcm_weight_) +
					      part * static_cast<double>(ffmpeg_weight_)) *
					     1000.0 / static_cast<double>(whole);
			offer(static_cast<int>(value));
		}
		check_stall();
	}

	// 録画ファイルの作成が済んだ録画ファイルか、作成に失敗した録画ファイルの数が変わった。
	void count_changed()
	{
		mark_progress();
		publish();
	}

	// すべての録画ファイルの作成が終わったので、作成中の録画ファイルがない状態に戻す。
	void end_all()
	{
		progress_.current_index = 0;
		progress_.current_permille.reset();
		progress_.stalled = false;
	}

	void check_stall()
	{
		if (progress_.stalled || !run_.ops.now_steady_us)
			return;
		if (run_.ops.now_steady_us() - progress_.last_progress_steady_us >= stall_us_) {
			progress_.stalled = true;
			publish();
		}
	}

	void publish()
	{
		if (run_.on_progress)
			run_.on_progress(progress_);
	}

private:
	void offer(int permille)
	{
		if (permille < 0)
			permille = 0;
		if (permille > 1000)
			permille = 1000;
		if (progress_.current_permille && permille <= *progress_.current_permille)
			return; // 割合は増えたときだけ書き換える
		progress_.current_permille = permille;
		mark_progress();
		publish();
	}

	void mark_progress()
	{
		progress_.last_progress_steady_us = run_.ops.now_steady_us ? run_.ops.now_steady_us() : 0;
		progress_.last_progress_wall_us = run_.ops.now_wall_us ? run_.ops.now_wall_us() : 0;
		progress_.stalled = false;
	}

	const FinalizeRun &run_;
	FinalizeProgress &progress_;
	std::int64_t stall_us_ = 0;
	std::int64_t pcm_weight_ = 0;
	std::int64_t ffmpeg_weight_ = 0;
};

// 子プロセスを起動し、終わるまで kFinalizePollMs ごとのポーリングで待つ。打ち切りの要求が来たら、子プロセスを
// 止めて Timeout を返す。`progress_path` が空でなければ、ポーリングのたびにその末尾を読んで meter に渡す。
ChildWait run_child(const FinalizeRun &run, const std::vector<std::string> &args, const std::string &log_tag,
		    int *exit_code, bool *not_started, ProgressMeter *meter = nullptr,
		    const std::string &progress_path = std::string(), std::int64_t expected_us = 0)
{
	// FFmpeg を起動できなかったときは not_started を真にする (起動した後の失敗とは次の操作が違う)。
	if (!run.ops.start_child || !run.ops.wait_child || !run.ops.ffmpeg_path) {
		*not_started = true;
		return ChildWait::Failed;
	}
	const std::string program = run.ops.ffmpeg_path();
	if (program.empty()) {
		*not_started = true;
		return ChildWait::Failed;
	}

	if (run.set_child_log)
		run.set_child_log(child_log_path(materials_dir_of(run.options), log_tag));

	const ChildHandle child = run.ops.start_child(program, args);
	if (child == kInvalidChild) {
		*not_started = true;
		return ChildWait::Failed;
	}

	for (;;) {
		const ChildWaitResult result = run.ops.wait_child(child, kFinalizePollMs);
		if (meter != nullptr) {
			std::string tail;
			if (!progress_path.empty() && read_file_tail(progress_path, kProgressTailBytes, &tail)) {
				const std::optional<std::int64_t> out_us = parse_progress_out_time_us(tail);
				if (out_us)
					meter->ffmpeg_time(*out_us, expected_us);
				else
					meter->check_stall();
			} else {
				meter->check_stall();
			}
		}
		if (result.outcome == ChildWait::Exited) {
			if (exit_code != nullptr)
				*exit_code = result.exit_code;
			return ChildWait::Exited;
		}
		if (result.outcome == ChildWait::Failed)
			return ChildWait::Failed;
		if (run.should_stop && run.should_stop()) {
			if (run.ops.kill_child)
				run.ops.kill_child(child);
			return ChildWait::Timeout;
		}
	}
}

std::string output_tag(const FinalizePlan &plan)
{
	return plan.user_id + "_" + stream_kind_name(plan.kind);
}

// この録画ファイルに入る映像のセグメントを合計したバイト数。FFmpeg の段階の重みに使う (ProgressMeter のコメント)。
std::int64_t video_bytes_of(const LedgerDocument &doc, const std::string &user_id, StreamKind kind)
{
	std::int64_t total = 0;
	for (const LedgerSegmentRow &row : doc.segments) {
		if (row.user_id == user_id && row.kind == kind && row.media == MediaKind::Video && row.bytes > 0)
			total += row.bytes;
	}
	return total;
}

// 録画ログから求めた録画ファイルの長さ (マイクロ秒)。
std::int64_t expected_duration_us(const FinalizePlan &plan)
{
	if (plan.expected_timescale <= 0 || plan.expected_duration_ticks <= 0)
		return 0;
	return plan.expected_duration_ticks * 1000000 / plan.expected_timescale;
}

} // namespace

std::optional<std::int64_t> parse_progress_out_time_us(const std::string &text)
{
	static const char kKey[] = "out_time_us=";
	const std::size_t key_len = sizeof(kKey) - 1;
	std::optional<std::int64_t> last;
	std::size_t pos = 0;
	while (pos < text.size()) {
		std::size_t end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(pos, end - pos);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		// 書き込みの途中で切れた最後の行 (改行がない) は数えない。
		if (line.compare(0, key_len, kKey) == 0 && end < text.size()) {
			// 値の全体が 64 ビットの整数のときだけ数え、例外を投げない読み方にする。std::stoll は、
			// 壊れた出力の 19 桁を超える数字の並びで std::out_of_range を投げ、それが録画ファイルの作成のスレッドの
			// 外まで抜けると OBS がクラッシュする。`N/A`、符号だけの値、途中に数字でない文字がある値、
			// 収まらない桁は、どれもその行を数えない (前の行の値が残る)。
			const char *first = line.data() + key_len;
			const char *stop = line.data() + line.size();
			std::int64_t value = 0;
			const std::from_chars_result read = std::from_chars(first, stop, value);
			if (first != stop && read.ec == std::errc() && read.ptr == stop)
				last = value;
		}
		pos = end + 1;
	}
	return last;
}

bool build_finalize_pcm(const FinalizePlan &plan, const std::string &session_dir,
			config::RecordPcmFormat output_pcm)
{
	return build_finalize_pcm(plan, session_dir, output_pcm, nullptr);
}

bool build_finalize_pcm(const FinalizePlan &plan, const std::string &session_dir,
			config::RecordPcmFormat output_pcm,
			const std::function<void(std::int64_t written_bytes, std::int64_t total_bytes)> &on_written)
{
	const int bytes_per_sample = pcm_bytes_per_sample(output_pcm);
	const std::int64_t total_bytes = plan.pcm_total_samples * plan.channels * bytes_per_sample;

	// 書く予定の全体の量。セグメントのサンプルの数から、書く前に分かる。
	std::int64_t planned_bytes = 0;
	for (const AudioWrite &write : plan.audio_writes) {
		if (!write.file.empty() && write.sample_count > 0)
			planned_bytes += write.sample_count * plan.channels * bytes_per_sample;
	}

	RawFile file;
	if (!file.create_zeroed(plan.pcm_path, total_bytes))
		return false;

	PcmSink sink(file, output_pcm, plan.channels);
	if (on_written)
		on_written(0, planned_bytes);
	// 後の書き込みが勝つ。音声の原点を置き直したセッションでは範囲が重なり、後のセグメントで上書きする。
	for (const AudioWrite &write : plan.audio_writes) {
		if (write.file.empty())
			continue;
		sink.seek_to_sample(write.offset_samples);
		const std::string path = join_path(session_dir, write.file);
		SegmentPcmInfo info;
		const bool ok = read_segment_pcm(
			path,
			[&sink, &on_written, planned_bytes](const void *data, std::size_t len) {
				const bool written = sink.write_f32(data, len);
				if (on_written)
					on_written(sink.written_bytes(), planned_bytes);
				return written;
			},
			&info);
		if (!ok) {
			// 読めないセグメントの範囲は 0 (無音) のまま残す。1 つのセグメントが読めないだけで録画ファイルの全体を
			// 諦めるより、その範囲を無音にして残りから録画ファイルを作成するほうがよい。
			continue;
		}
	}
	return file.close();
}

RepairOutcome repair_truncated_segments(const FinalizeRun &run, LedgerDocument &working)
{
	RepairOutcome outcome;
	if (!run.options.repair_truncated)
		return outcome;
	const std::string &materials = materials_dir_of(run.options);

	std::vector<LedgerSegmentRow> kept;
	kept.reserve(working.segments.size());
	std::set<std::string> seen;
	for (LedgerSegmentRow &row : working.segments) {
		// 切れていない行、同じセグメントの 2 つ目の行、打ち切った後の行は修復しない。
		if (!row.truncated || row.file.empty() || !seen.insert(row.file).second || outcome.stopped) {
			kept.push_back(row);
			continue;
		}
		if (run.should_stop && run.should_stop()) {
			outcome.stopped = true;
			kept.push_back(row);
			continue;
		}
		// 読めるサンプルだけを新しいファイルにコピーし、行の長さを読めた長さに修正する。修復はリンクした
		// libavformat で行う (record_writer.h の copy_readable_part)。読めた数をその場で数えられ、欠けた
		// サンプルの手前で止められる。
		// FFmpeg の子プロセスの `-c copy` は欠けた最後のサンプルもコピーすることがあり、ProRes に
		// 再エンコードする録画ファイルでは `-xerror` のデコードがそこで止まる。
		// 映像では、最後の 1 フレームの長さを、セグメントを書いたときと同じフレームの間隔にする
		// (record_writer.cpp の open_video と同じ丸め)。
		std::int64_t last_ticks = 0;
		if (row.media == MediaKind::Video && row.fps_num > 0 && row.fps_den > 0 && row.timescale > 0)
			last_ticks = (static_cast<std::int64_t>(row.timescale) * row.fps_den + row.fps_num / 2) / row.fps_num;
		const std::string repaired = repaired_segment_file(row.file);
		const ReadablePart part = copy_readable_part(join_path(materials, row.file), join_path(materials, repaired),
							     0, last_ticks, run.should_stop);
		if (part.stopped) {
			// 打ち切りの要求で止めた。行は変えずに残し、セッションは Pending になる (run_finalize)。
			outcome.stopped = true;
			kept.push_back(row);
			continue;
		}
		if (part.io_failed) {
			// 読み出しそのものに失敗した (開けない、読めない)。多くは一時的な失敗 (ほかのソフトが読み取りを
			// 許さずに開いている、ネットワークが一瞬切れた) である。読めるサンプルがないセグメントと
			// 同じように外すと、セグメントのファイルが残っているのにセッションが Done になり、
			// 作成待ちから消える。行は変えずに残し、このセグメントを使う録画ファイルの作成を失敗にする。
			// セッションは Failed として作成待ちに残り、原因が直った後に再開すれば、もう一度試す。
			outcome.unwritten.push_back(row.file);
			kept.push_back(row);
			continue;
		}
		if (part.samples > 0 && !part.written) {
			// 読めるサンプルはあるのに、修復したファイルを書けなかった (保存先に書けない、空き容量が
			// 尽きた)。
			// 外すと、残せる録画を捨ててセッションが Done になり、再開できなくなる。行は切れたまま残し、
			// このセグメントを使う録画ファイルの作成を失敗にする (セッションは Failed として作成待ちに残る)。
			outcome.unwritten.push_back(row.file);
			kept.push_back(row);
			continue;
		}
		const std::int64_t tail = part.source_bytes > part.readable_end ? part.source_bytes - part.readable_end : 0;
		if (part.samples <= 0) {
			// 読めるサンプルがないセグメント (中身の形で開けない、サンプルが 1 つも読めない) は
			// 録画ファイルの作成から外し、外したことを録画ログに残す。長さ 0 のまま計画に入れると、concat が読めずに
			// セッションが Failed に戻る。
			// 電源が落ちたときに開いていたセグメントは、中身の形で読めないこと (0 バイト、ヘッダーの途中で
			// 切れた) がある。これを失敗にすると、何度再開しても失敗に戻る。
			row.sample_count = 0;
			row.duration_ticks = 0;
			row.bytes = 0;
			row.dropped_tail_bytes = part.source_bytes > 0 ? part.source_bytes : 0;
			if (run.append_line)
				run.append_line(to_jsonl(row));
			continue;
		}
		if (row.timescale <= 0)
			row.timescale = part.timescale;
		row.sample_count = part.samples;
		row.duration_ticks = (part.timescale > 0 && part.timescale != row.timescale)
					     ? (part.duration_ticks * row.timescale + part.timescale / 2) / part.timescale
					     : part.duration_ticks;
		row.bytes = part.written_bytes;
		row.dropped_tail_bytes = tail;
		// 録画ログには元のセグメントの名前で追加する。セグメントの行のキーは `file` で、途中で切れた
		// セグメントであることも残す。
		if (run.append_line)
			run.append_line(to_jsonl(row));
		// 録画ファイルの作成に使うコピーでは、修復したセグメントを「切れていない、修復したファイルを指す
		// セグメント」にする。
		// サンプルを 1 つも作り直さないコピーで、avcC も元のままなので、閉じたセグメントと同じく Remux で
		// 作成できる。
		// 切れたセグメントとして ProRes に再エンコードすると、中断のたびに、その人の録画ファイルがすべて
		// 再エンコードになる (1080p60 で 1 時間あたり 200 GB 前後)。フレームの間隔がそろわないセッションでは、
		// fps のフィルターがフレームを複製したり捨てたりするので、フレームの数が期待と合わず、再開しても同じ
		// 失敗に戻る。壊れたフレームがあると、`-xerror` を付けたデコードがそこで止まる。
		row.file = repaired;
		row.truncated = false;
		outcome.repaired.push_back(repaired);
		kept.push_back(row);
	}
	working.segments = std::move(kept);
	return outcome;
}

FinalizeProgress run_finalize(const FinalizeRun &run)
{
	FinalizeProgress progress;
	if (run.doc == nullptr)
		return progress;
	ProgressMeter meter(run, progress);
	const std::string &materials = materials_dir_of(run.options);

	// 途中で切れたセグメントを修復する (再開のときだけ)。作る録画ファイルを数える前に行い、修復した録画ログの
	// コピーで録画ファイルを数えて計画を組む。中断したセッションで開いていたセグメントの行は長さを持たない (0) ので、
	// 修復せずに録画ファイルの長さを比べると必ず食い違う。
	progress.state = FinalizeState::Running;
	meter.count_changed();
	LedgerDocument working = *run.doc;
	const RepairOutcome repair = repair_truncated_segments(run, working);

	const std::vector<OutputKey> outputs = finalize_outputs(working);
	progress.files_total = static_cast<int>(outputs.size());
	for (const OutputKey &key : outputs) {
		if (decide_branch(collect_branch_input(working, key.user_id, key.kind)) == FinalizeBranch::ProRes)
			progress.any_reencode = true;
	}
	meter.count_changed();

	bool stopped = repair.stopped;
	int index = 0;
	for (const OutputKey &key : outputs) {
		if (stopped || (run.should_stop && run.should_stop())) {
			stopped = true;
			break;
		}
		++index;

		const FinalizePlan plan = build_finalize_plan(working, key.user_id, key.kind, run.options);
		progress.branch = plan.branch;

		LedgerFinalizeRow row;
		row.user_id = key.user_id;
		row.kind = key.kind;
		row.branch = plan.branch;
		// 録画ログの `output_file` は、セッションのフォルダーからの相対パスにする。
		row.output_file = plan.output_file.size() > run.options.session_dir.size()
					  ? plan.output_file.substr(run.options.session_dir.size() + 1)
					  : plan.output_file;
		row.args = plan.args;
		row.gaps = plan.gaps;
		row.output_timescale = plan.expected_timescale;

		const std::int64_t pcm_bytes = plan.pcm_total_samples * plan.channels *
					       pcm_bytes_per_sample(run.options.output_pcm);
		meter.begin_file(index, pcm_bytes, pcm_bytes + video_bytes_of(working, key.user_id, key.kind));

		// 修復したファイルを書けなかったセグメント (読み出しに失敗したセグメントを含む) を使う録画ファイルは、
		// FFmpeg を起動せずに失敗にする。修復したファイルがないか前に作った古いファイルしかないので、作成しても
		// 正しい録画ファイルにならない。セグメントのファイルは削除しない。
		bool repair_unwritten = false;
		for (const LedgerSegmentRow &segment : working.segments) {
			if (segment.user_id == key.user_id && segment.kind == key.kind &&
			    std::find(repair.unwritten.begin(), repair.unwritten.end(), segment.file) != repair.unwritten.end())
				repair_unwritten = true;
		}
		if (repair_unwritten) {
			row.exit_code = -1;
			row.check = FinalizeCheck::ProcessFailed;
			++progress.files_failed;
			if (run.append_line)
				run.append_line(to_jsonl(row));
			meter.count_changed();
			continue;
		}

		// 書き始める前に空き容量を確認する。全長分の生の PCM は 1 時間で十数 GB になるので、足りないまま
		// 書き始めると、保存先を埋め切ってから失敗する。
		if (pcm_bytes > 0 && run.ops.free_space_bytes) {
			const std::optional<std::uint64_t> free = run.ops.free_space_bytes(materials);
			// 空き容量を読めなかったときは進む。読めないことを、空き容量が足りないことと同じには扱わない。
			if (free && static_cast<std::uint64_t>(pcm_bytes) >= *free) {
				row.exit_code = -1;
				row.check = FinalizeCheck::SpaceExhausted;
				++progress.files_failed;
				if (run.append_line)
					run.append_line(to_jsonl(row));
				meter.count_changed();
				continue;
			}
		}

		bool ok = true;
		if (plan.branch == FinalizeBranch::Remux && plan.has_video)
			ok = write_text_file(run.ops, plan.concat_path, plan.concat_list);
		else if (plan.branch == FinalizeBranch::ProRes && plan.has_video)
			ok = write_text_file(run.ops, plan.filter_script_path, plan.filter_script);
		if (ok) {
			ok = build_finalize_pcm(plan, materials, run.options.output_pcm,
						[&meter](std::int64_t written, std::int64_t total) {
							meter.pcm_written(written, total);
						});
		}

		if (!ok) {
			row.exit_code = -1;
			row.check = FinalizeCheck::ProcessFailed;
		} else {
			int exit_code = 0;
			bool not_started = false;
			const ChildWait outcome = run_child(run, plan.args, output_tag(plan), &exit_code, &not_started, &meter,
							    plan.progress_path, expected_duration_us(plan));
			if (outcome == ChildWait::Timeout) {
				// 打ち切ったので、作りかけの録画ファイルと、生の PCM と `-progress` の出力を削除する。入力の
				// セグメントは削除しない。
				remove_file(plan.output_file);
				remove_file(plan.pcm_path);
				remove_file(plan.progress_path);
				stopped = true;
				break;
			}
			if (outcome != ChildWait::Exited) {
				if (not_started)
					progress.ffmpeg_unavailable = true;
				row.exit_code = -1;
				row.check = FinalizeCheck::ProcessFailed;
			} else {
				row.exit_code = exit_code;
				const OutputProbe probe = probe_output(plan.output_file, plan.expected_timescale);
				row.output_duration_ticks = probe.duration_ticks;
				// 録画ログには、録画ファイルそのもののタイムスケールと長さを書く (ffprobe で読める値と
				// 同じにする)。
				if (probe.stream_timescale > 0) {
					row.output_timescale = probe.stream_timescale;
					row.output_duration_ticks = probe.stream_duration_ticks;
				}
				if (exit_code != 0)
					row.check = FinalizeCheck::ProcessFailed;
				else if (!probe.ok)
					row.check = FinalizeCheck::DurationMismatch;
				else if (finalize_output_matches(plan, row.output_duration_ticks, probe.frames,
								 row.output_timescale))
					row.check = FinalizeCheck::Matched;
				else
					row.check = FinalizeCheck::DurationMismatch;
			}
		}

		// 生の PCM は、成功しても失敗しても削除する。全長分の生の PCM は 1 時間で十数 GB になり、残すと
		// 保存先を 2 倍使う。
		remove_file(plan.pcm_path);
		// 録画ファイルの作成に成功した録画ファイルでは、ほかの一時ファイル (concat の一覧、フィルターのスクリプト、`-progress` の
		// 出力) も削除する。録画ファイルの作成に失敗した録画ファイルの一時ファイルは、原因を調べるために残す。FFmpeg のログ
		// (`ffmpeg_*.log`) は一時ファイルではないので削除しない。
		if (row.check == FinalizeCheck::Matched) {
			if (!plan.concat_path.empty())
				remove_file(plan.concat_path);
			if (!plan.filter_script_path.empty())
				remove_file(plan.filter_script_path);
			if (!plan.progress_path.empty())
				remove_file(plan.progress_path);
		}

		if (row.check == FinalizeCheck::Matched)
			++progress.files_done;
		else
			++progress.files_failed;
		if (run.append_line)
			run.append_line(to_jsonl(row));
		meter.count_changed();
	}

	if (stopped)
		progress.state = FinalizeState::Pending;
	else if (progress.files_failed > 0)
		progress.state = FinalizeState::Failed;
	else
		progress.state = FinalizeState::Done;
	meter.end_all();
	meter.publish();
	return progress;
}

} // namespace cordmux::record

// SPDX-License-Identifier: Apache-2.0

// 録画ファイルの作成の計画を、録画ログから作る (build_finalize_plan)。録画ファイルの作成はメディアのファイルを調べずに、録画ログの値だけ
// から FFmpeg の引数、concat demuxer の本文、ProRes で録画ファイルを作成するときのフィルターのスクリプト、音声の書き込み位置を作る。
// FFmpeg は 1 人の 1 つのファイルにつき 1 回だけ起動する。映像はセグメントファイルを concat demuxer で並べ、音声は
// 全体の長さの生の PCM を -f で読ませ、どちらも -c copy で 1 つの mov にする (ProRes で録画ファイルを作成するときは、映像だけを
// 再エンコードする)。
// libobs、Qt、avformat に依存しない。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config/settings.h"
#include "record/record_audio.h"
#include "record/record_black.h"
#include "record/record_branch.h"
#include "record/record_ledger.h"
#include "record/record_segment.h"
#include "record/record_types.h"

namespace cordmux::record {

struct FinalizeOptions {
	std::string ffmpeg_path; // 同梱の FFmpeg の絶対パス (RecordPlatformOps::ffmpeg_path の結果)
	std::string session_dir; // このセッションのフォルダー (UTF-8 の絶対パス。録画ファイルを置く)
	config::RecordPcmFormat output_pcm = config::RecordPcmFormat::Float32;
	bool repair_truncated = false; // 途中で切れたセグメントを、書き直した後のファイルで読むか

	// 中間ファイルのフォルダー (録画ログの `file` の基準。一時ファイルと FFmpeg のログもここに置く)。
	// 空は session_dir と同じことを示す。
	std::string materials_dir;
	// 録画ファイルの名前の書式と種類の語 (空なら record_types.h の既定を使う)。呼び出し元は、セッションを始めた
	// 時点の値を渡す。ただし、録画ファイルの作成を再開するセッションでは、録画ログにこの値がないので、再開した時点の値になる。
	std::string output_name_format;
	std::string screen_kind_word;
	std::string voice_kind_word;
	// どの値もこの秒数の間進まなければ、進んでいないとみなす (0 以下は既定の値を使う)。
	int finalize_stall_s = kDefaultFinalizeStallS;
};

// 録画ログの `file` と一時ファイルの基準になるフォルダー (materials_dir が空なら session_dir)。
inline const std::string &materials_dir_of(const FinalizeOptions &options)
{
	return options.materials_dir.empty() ? options.session_dir : options.materials_dir;
}

// 並べる 1 行 (セグメントか、黒埋め用のフレーム)。
struct FinalizeItem {
	std::string file;                 // 中間ファイルのフォルダーからの相対パス
	bool black = false;               // 黒埋め用のフレームか (偽ならセグメント)
	std::int64_t out_point_ticks = 0; // 0 は最後まで使うことを示す。端数の行だけが値を持つ
	std::int64_t frames = 0;          // この行が持つフレーム数
	std::int64_t ticks = 0;           // この行の長さ (timescale の単位)
};

// 1 つの録画ファイルの作成の計画。build_concat_list と build_filter_script は FinalizePlan だけを受け取るので、並べる
// 順 (items) と一時ファイルのパスも計画に持つ。
struct FinalizePlan {
	FinalizeBranch branch = FinalizeBranch::Remux;
	std::vector<std::string> args;
	std::string concat_list;   // concat demuxer の本文
	std::string filter_script; // ProRes で録画ファイルを作成するときの -filter_complex_script の本文
	std::string pcm_path;      // 全体の長さの生の PCM (I/O の部分が書く)
	std::string output_file;   // 録画ファイル (絶対パス)
	std::vector<GapFill> gaps;
	std::int64_t expected_duration_ticks = 0;
	int expected_timescale = 0;
	std::int64_t expected_frames = 0;
	// 録画ファイルと比べるときに、フレーム数がずれてよい数 (finalize_output_matches)。ProRes で録画ファイルを作成するときは fps の
	// フィルターに入る並びの数で、それ以外は 0 (ちょうど一致する)。
	std::int64_t frame_tolerance = 0;

	std::vector<FinalizeItem> items;
	// 全体の長さの無音の、どこに各セグメントの音声を書き込むか。
	std::vector<AudioWrite> audio_writes;
	std::int64_t pcm_total_samples = 0; // 生の PCM の長さ (1 チャンネルあたりのサンプル数)
	std::string concat_path;
	std::string filter_script_path;
	// FFmpeg が進み具合を書き出すファイル (`-progress`。1 つのファイルの中の割合を求めるのに使う)。
	std::string progress_path;
	std::string user_id;
	StreamKind kind = StreamKind::Screen;
	int sample_rate = 48000;
	int channels = 2;
	DominantSize size;      // ProRes で録画ファイルを作成するときにそろえる解像度 (dominant_size)
	int fps_num = 0;        // 計画の fps (黒を作る color と、fps の指定に使う)
	int fps_den = 0;
	bool has_video = false; // 映像のセグメントが 1 つもない録画ファイル (声と、音声だけの画面共有) では偽
	// 黒で埋める範囲に上限を掛けたか (black_limit_from_origin_us)。真なら録画ログの 2 つの時刻が食い違っていて、
	// そのままでは何日分もの黒を作るところだった。
	bool tail_gap_clamped = false;
};

// ---------------------------------------------------------------------------
// 書式 (小数点も改行も、地域の設定によらない形で組み立てる)
// ---------------------------------------------------------------------------

namespace detail {

// ticks / timescale を「秒.小数 6 桁」の文字列にする (負の値は 0 にする)。整数の計算で組み立てる。小数点の書式は、
// 地域の設定によって `,` になることがあるためである。
inline std::string ticks_to_seconds_text(std::int64_t ticks, int timescale)
{
	if (timescale <= 0 || ticks <= 0)
		return "0.000000";
	const std::int64_t whole = ticks / timescale;
	const std::int64_t rest = ticks % timescale;
	const std::int64_t micro = (rest * 1000000) / timescale;
	std::string fraction = std::to_string(micro);
	while (fraction.size() < 6)
		fraction.insert(fraction.begin(), '0');
	return std::to_string(whole) + "." + fraction;
}

// concat demuxer の file 行に書くパス (単引用符で囲む。中の単引用符は '\'' にする)。
inline std::string concat_quote(const std::string &path)
{
	std::string out = "'";
	for (const char ch : path) {
		if (ch == '\'')
			out += "'\\''";
		else
			out.push_back(ch);
	}
	out.push_back('\'');
	return out;
}

inline std::string rational_text(int num, int den)
{
	if (num <= 0 || den <= 0)
		return "60";
	if (den == 1)
		return std::to_string(num);
	return std::to_string(num) + "/" + std::to_string(den);
}

} // namespace detail

// 録画ファイルに入れる PCM の形を、FFmpeg の -f に渡す名前にする。
inline const char *pcm_input_format_name(config::RecordPcmFormat format)
{
	return format == config::RecordPcmFormat::Int24 ? "s24le" : "f32le";
}

inline int pcm_bytes_per_sample(config::RecordPcmFormat format)
{
	return format == config::RecordPcmFormat::Int24 ? 3 : 4;
}

// 途中で切れたセグメントを、読める部分だけ書き直したファイルの名前。書き直しは FFmpeg を起動せず、リンクした
// libavformat で読めるサンプルだけをコピーする (record_writer.h の copy_readable_part)。FFmpeg の -c copy で
// 書き直すと、書き直したセグメントの長さを録画ログに反映できず、欠けた最後のサンプルもコピーしうるためである。
inline std::string repaired_segment_file(const std::string &file)
{
	const std::string suffix = ".mov";
	if (file.size() > suffix.size() && file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0)
		return file.substr(0, file.size() - suffix.size()) + "_repair" + suffix;
	return file + "_repair" + suffix;
}

// ---------------------------------------------------------------------------
// concat demuxer の本文とフィルターのスクリプト
// ---------------------------------------------------------------------------

// concat demuxer の本文を作る。duration は 1 行も書かない (record_black.h のコメント)。
// 端数の行の outpoint は、フレームの真ん中に置く。端数が N フレームのとき、終わりの位置 (N フレーム分の長さ) をそのまま
// 書くと、丸めが上に寄ったときに、もう 1 フレーム余計に入る。(N - 0.5) フレームの位置に書けば、どちらに丸めても
// N フレームちょうどになる。
inline std::string build_concat_list(const FinalizePlan &plan)
{
	std::string out = "ffconcat version 1.0\n";
	// 切るのは黒埋め用のフレームなので、その 1 フレームの長さで半フレーム手前に寄せる (計画の平均で割ると、fps が
	// 混ざったセッションで、別のストリームのフレームの長さを使ってしまう)。
	const std::int64_t frame_ticks =
		black_frames_to_ticks(1, plan.fps_num, plan.fps_den, plan.expected_timescale);
	for (const FinalizeItem &item : plan.items) {
		// 黒埋め用のフレームがない欠落は行を持たない (その人は ProRes で録画ファイルを作成する)。
		if (item.file.empty())
			continue;
		out += "file " + detail::concat_quote(item.file) + "\n";
		if (item.out_point_ticks > 0) {
			// フレームの真ん中に置く (上のコメント)。
			const std::int64_t at = item.out_point_ticks - frame_ticks / 2;
			out += "outpoint " + detail::ticks_to_seconds_text(at, plan.expected_timescale) + "\n";
		}
	}
	return out;
}

// fps のフィルター (ProRes で録画ファイルを作成するときの `fps=`) が、長さ ticks (単位は timescale) の入力 1 つから出すフレームの数。
// 始まり 0 と終わり ticks を、出力のフレームの時刻 (fps_num / fps_den の間隔) のうち最も近いものに丸めた差である
// (libavutil の av_rescale_rnd の AV_ROUND_NEAR_INF と同じ丸め。正の値では半分を上に丸める)。fps のフィルターは、入力の
// 始まりと終わりをこの丸めで出力の時刻にそろえ、その間のフレームを出す (libavfilter の vf_fps.c。既定の round=near と
// eof_action=round)。
//
// ProRes で録画ファイルを作成するとき、録画ファイルと比べるフレーム数はこの数え方で求める (build_finalize_plan)。録画ファイルの作成のチェックの
// 目的は、FFmpeg が終了コード 0 のまま途中で止まったり短い録画ファイルを書いたりしたことを見逃さないことで、
// 比べる相手は、この引数で最後まで正しく動いたときに出る数である。ProRes で録画ファイルを作成するときは、セグメントごとに fps の
// フィルターを通してフレームの間隔を一定にするので、出る数はセグメントのサンプル数と同じとは限らない。たとえば、
// 画面が動かない間に送信側がフレームを間引き、200 ms の空きが 1 つできた 24 フレームのセグメントは、25 フレームに
// なる。サンプル数と比べると、ProRes で録画ファイルを作成する人 (画面共有の途中で解像度が変わった人、黒埋め用のフレームがない人)
// の録画ファイルの作成が、何度再開しても DurationMismatch で失敗する。
// fps= を外して ProRes も元の時刻のまま (可変フレームレート) にする方法は使わない。FFmpeg は mov を書くとき、既定で
// フレームを一定の間隔にそろえるので -fps_mode まで変えることになり、編集ソフトで扱いやすい形にするという、ProRes で
// 録画ファイルを作成する目的にも反するためである。
inline std::int64_t fps_filter_frames(std::int64_t ticks, int timescale, int fps_num, int fps_den)
{
	if (ticks <= 0 || timescale <= 0 || fps_num <= 0 || fps_den <= 0)
		return 0;
	return detail::mul_div_round(ticks, fps_num, static_cast<std::int64_t>(timescale) * fps_den);
}

// ProRes で録画ファイルを作成するときの -filter_complex_script の本文を作る。
// 黒は lavfi の color で作る。黒埋め用のフレームがない人も、ProRes で録画ファイルを作成するためである。
inline std::string build_filter_script(const FinalizePlan &plan)
{
	if (plan.branch != FinalizeBranch::ProRes || !plan.has_video)
		return std::string();

	// color の s は "WxH" の形で、pad は幅と高さを別の引数で受け取るので ":" でつなぐ。この 2 つを同じ文字列で
	// 済ませない。pad=WxH は Invalid chars のエラーで終了コード -22 になり、ProRes で録画ファイルを作成するファイルが 1 つも
	// 書けなくなる (同梱の FFmpeg で確認した)。
	const std::string size_text = std::to_string(plan.size.width) + "x" + std::to_string(plan.size.height);
	const std::string pad_size_text = std::to_string(plan.size.width) + ":" + std::to_string(plan.size.height);
	const std::string rate_text = detail::rational_text(plan.fps_num, plan.fps_den);
	std::string out;
	std::string chain;
	int input_index = 0;
	int label = 0;

	for (std::size_t i = 0; i < plan.items.size(); ++i) {
		const FinalizeItem &item = plan.items[i];
		const std::string tag = "v" + std::to_string(label);
		if (item.black) {
			// 続いた黒は、まとめて 1 つの color にする (concat demuxer では黒埋め用のフレームのファイルを
			// 並べるが、lavfi は長さを指定できるので、同じ欠落を 1 行で書ける)。長さは、フレーム数の
			// 合計から 1 度だけ timescale の単位に修正する。行ごとの ticks は丸めた値なので、足していくと、
			// 60000 x fps_den x フレーム数 / fps_num が整数にならない組 (59 fps で 30 フレームの黒埋め用の
			// フレーム) で丸めが積もり、長い黒で color のフレームが足りなくなる (2 時間で 6 フレーム)。
			std::int64_t frames = item.frames;
			while (i + 1 < plan.items.size() && plan.items[i + 1].black) {
				++i;
				frames += plan.items[i].frames;
			}
			const std::int64_t ticks =
				black_frames_to_ticks(frames, plan.fps_num, plan.fps_den, plan.expected_timescale);
			out += "color=c=black:s=" + size_text + ":r=" + rate_text +
			       ":d=" + detail::ticks_to_seconds_text(ticks, plan.expected_timescale) +
			       ",format=yuv422p10le,setsar=1[" + tag + "];\n";
		} else {
			out += "[" + std::to_string(input_index) + ":v]scale=" + std::to_string(plan.size.width) +
			       ":" + std::to_string(plan.size.height) +
			       ":force_original_aspect_ratio=decrease,pad=" + pad_size_text +
			       ":(ow-iw)/2:(oh-ih)/2,setsar=1,fps=" + rate_text + ",format=yuv422p10le[" + tag +
			       "];\n";
			++input_index;
		}
		chain += "[" + tag + "]";
		++label;
	}

	if (label == 0)
		return std::string();
	// 並べた後のフレームの時刻を、通し番号から付け直す (setpts=N/(fps)/TB)。concat のフィルターは、映像の並び
	// 1 つの終わりを「最後のフレームの時刻 x フレーム数 / (フレーム数 - 1)」(平均の間隔) で見積もるので、
	// 1 フレームだけの並び (1 フレームのセグメント、1 フレーム分の黒) では、終わりが始まりと同じになる。すると次の
	// 並びの最初のフレームと時刻が重なり、mov を書くときに一定の間隔にそろえる処理 (mov の既定) がそのフレームを落とす
	// (FFmpeg 7.1 で、60 fps の 1 フレームのセグメント 5 つが 2 フレームになる)。どの並びも、fps のフィルターか
	// color が一定の間隔で出したフレームなので、並べた順の通し番号がそのまま正しい時刻になる。
	// セグメントごとに tpad で 1 フレームを足して trim で切りそろえる方法は使わない (1 フレームも出さなかった並びは
	// コピーするフレームがないので直らず、スクリプトも 3 段階増える)。setpts の後にもう 1 度 fps= を通す方法も
	// 使わない (concat が伝える終わりの時刻が 1 フレームの並びの分だけ早く、その後ろのフレームを落としうる)。
	out += chain + "concat=n=" + std::to_string(label) + ":v=1:a=0,setpts=N/(" + rate_text + ")/TB[vout]\n";
	return out;
}

// ---------------------------------------------------------------------------
// 計画 (録画ログ -> 並び -> 引数)
// ---------------------------------------------------------------------------

// 同じ PtsDomain とみなす食い違いの上限 (build_session_timeline)。原点がセッションの開始より前にあってよい幅
// (black_limit_from_origin_us) にも同じ値を使う。
inline constexpr std::int64_t kSameConnectionToleranceUs = 1000000;

// セグメント 1 つと、セッションの原点からの位置。
struct PlacedSegment {
	const LedgerSegmentRow *row = nullptr;
	std::int64_t start_us = 0;
	std::int64_t duration_us = 0;
};

inline std::int64_t segment_duration_us(const LedgerSegmentRow &row)
{
	if (row.timescale <= 0)
		return 0;
	return (row.duration_ticks * 1000000) / row.timescale;
}

// 「受信側の steady_clock の時刻 - 送信側のタイムスタンプ」(first_pts_steady_us - first_pts_us) が 1 秒以内でそろう
// セグメントのまとまり。同じまとまりの中では、送信側のタイムスタンプの差がそのまま経過時間を表す。
// offset_us に差の最小値を使うのは、到着の遅延が最も小さかったセグメントの差が、2 つの時計の差に最も近いためである
// (pipeline/sender_clock.h と同じ見積もり)。
struct PtsDomain {
	std::int64_t reference_us = 0; // まとまりの最初のセグメントの差 (このまとまりに入るかの基準)
	std::int64_t offset_us = 0;     // まとまりの中の差の最小値 (タイムスタンプを steady_clock の時刻に修正する差)
};

// 録画ログ全体の時刻の基準 (build_session_timeline)。人と種類ごとの計画は、どれもこの 1 つを使う。
struct SessionTimeline {
	std::vector<PtsDomain> domains;
	std::vector<std::size_t> domain_of; // doc.segments と同じ順。各セグメントがどの PtsDomain に入ったか
	std::int64_t origin_steady_us = 0;  // セッションの原点 (steady_clock の時刻)
};

namespace detail {

inline std::int64_t abs_difference(std::int64_t a, std::int64_t b)
{
	return a > b ? a - b : b - a;
}

} // namespace detail

// セグメントを PtsDomain に分け、セッションの原点を決める。録画ログ全体で 1 回だけ、人、種類、映像と音声をまたいで
// 分ける。セグメントの位置は、同じ PtsDomain の中では送信側のタイムスタンプで決め、PtsDomain をまたぐときだけ受信側の
// steady_clock の時刻で決める。送信側のタイムスタンプのほうが、到着の時刻よりストリームごとのずれが小さいためである。
// 到着の時刻で置くと、録画を ON にした直後の最初の IDR が音声より遅れて届いた分 (0.14 から 0.2 秒) が、ファイル全体で
// 音声が先に鳴るずれとして残る。音声を映像のセグメントの始まりに合わせる方法は使わない (IDR より前の音声を捨てることに
// なる)。一定の値で補正する方法も使わない (IDR の遅延は、ON にしたときの負荷で毎回変わる)。
// 送信側のタイムスタンプは送信側の時計 (Vesktop のレンダラーの performance の時計) の値で、受信側の steady_clock と同じ
// Windows の単調な時計で進む。そのため差は、送信側の時計の原点が変わる (Vesktop を起動し直す) までは変わらず、再接続の
// 前後も同じ PtsDomain に入る。PtsDomain が変わるのは、Vesktop を起動し直したときである。受信側のシステム時刻との差
// (first_pts_wall_us - first_pts_us) で分けないのは、システム時刻は合わせ直しで飛び、単調な時計との速さも違うので、差が
// 経過時間と関係なく動くためである。
// まとまりに入るかは、まとまりの最初のセグメントの差と比べる (最小値と比べると、入った後で最小値が動いて境が揺れる)。
// 1 秒は、タイムスタンプの不連続でセグメントを閉じるしきい値と同じで、到着の遅延の幅 (0.2 秒ほど) は十分に収まる。
// Vesktop を起動し直す前と後の原点の差が 1 秒より小さいと、2 つは同じ PtsDomain に入り、後の分の位置は原点の差だけ
// ずれる。原点はどちらも起動したときのシステム時刻なので、差はその間のシステム時刻と単調な時計の速さの違い (測った例では
// 10 ppm ほどで、1 日で 1 秒に届かない) と、その間のシステム時刻の合わせ直しの量である。
// 原点は、全セグメントの「タイムスタンプ + PtsDomain の差」の最小値 (全セグメントの先頭のタイムスタンプの最小値を
// steady_clock の時刻で表したもの) にする。steady_clock の時刻にするのは、セッションの終わり (ended_at_steady_us) との
// 差で末尾の欠落を決めるためである。録画を ON にした時刻を原点にしないのは、ON から最初のキーフレームまでの間が、
// すべてのファイルの先頭に黒として入るためである。
inline SessionTimeline build_session_timeline(const LedgerDocument &doc)
{
	SessionTimeline timeline;
	timeline.domain_of.reserve(doc.segments.size());
	for (const LedgerSegmentRow &row : doc.segments) {
		const std::int64_t offset = row.first_pts_steady_us - row.first_pts_us;
		// 入れる先は、基準との食い違いが 1 秒以内の PtsDomain のうち、最も近いもの。
		std::size_t chosen = timeline.domains.size();
		std::int64_t chosen_distance = 0;
		for (std::size_t i = 0; i < timeline.domains.size(); ++i) {
			const std::int64_t distance = detail::abs_difference(offset, timeline.domains[i].reference_us);
			if (distance > kSameConnectionToleranceUs)
				continue;
			if (chosen == timeline.domains.size() || distance < chosen_distance) {
				chosen = i;
				chosen_distance = distance;
			}
		}
		if (chosen == timeline.domains.size()) {
			PtsDomain domain;
			domain.reference_us = offset;
			domain.offset_us = offset;
			timeline.domains.push_back(domain);
		} else if (offset < timeline.domains[chosen].offset_us) {
			timeline.domains[chosen].offset_us = offset;
		}
		timeline.domain_of.push_back(chosen);
	}

	// 原点は、全セグメントの「タイムスタンプ + PtsDomain の差」の最小値 (上のコメント)。
	bool found = false;
	for (std::size_t i = 0; i < doc.segments.size(); ++i) {
		const std::int64_t at =
			doc.segments[i].first_pts_us + timeline.domains[timeline.domain_of[i]].offset_us;
		if (!found || at < timeline.origin_steady_us) {
			timeline.origin_steady_us = at;
			found = true;
		}
	}
	return timeline;
}

// セッションの原点 (steady_clock の時刻)。
inline std::int64_t session_origin_steady_us(const LedgerDocument &doc)
{
	return build_session_timeline(doc).origin_steady_us;
}

// この人のこの種類の、映像か音声のセグメントを、セッションの原点からの位置を付けて並べる。位置は送信側のタイムスタンプ
// から作る。差は PtsDomain ごとに 1 つなので、同じ PtsDomain のセグメントどうしの間隔はタイムスタンプの
// 差そのものになり、到着の遅延は入らない。
inline std::vector<PlacedSegment> place_segments(const LedgerDocument &doc, const SessionTimeline &timeline,
						 const std::string &user_id, StreamKind kind, MediaKind media)
{
	std::vector<PlacedSegment> placed;
	for (std::size_t i = 0; i < doc.segments.size(); ++i) {
		const LedgerSegmentRow &row = doc.segments[i];
		if (row.user_id != user_id || row.kind != kind || row.media != media)
			continue;

		std::int64_t start = 0;
		if (i < timeline.domain_of.size() && timeline.domain_of[i] < timeline.domains.size()) {
			start = row.first_pts_us + timeline.domains[timeline.domain_of[i]].offset_us -
				timeline.origin_steady_us;
		}
		if (start < 0)
			start = 0;

		PlacedSegment one;
		one.row = &row;
		one.start_us = start;
		one.duration_us = segment_duration_us(row);
		placed.push_back(one);
	}
	return placed;
}

// 黒で埋めてよい範囲の上限 (原点からの位置。0 は上限を持たないことを示す)。黒で埋める長さが、セッションの長さを超えない
// ようにする。末尾の欠落の長さは「session の行の終わり - 原点」で求めるが、この 2 つは別の行から求めた値である (原点は
// セグメントの行から、終わりは session の行から)。first_pts_steady_us が 0 のセグメントの行が 1 つでもあると原点が
// 0 になり、終わりは steady_clock の生の値 (起動からの経過で、日の単位になる) なので、何日分もの黒と、数十 GB の生の
// PCM を作る計画になる。session の行の開始と終わりは常に一貫した組なので、その差を上限にする。正常な録画ログでは
// 欠落はどれもセッションの中に収まるので、この上限は何も変えない。上限を掛けるのは黒だけで、セグメントそのものは
// 切らない (実際に書いたものは残す)。上限が動作したことは FinalizePlan::tail_gap_clamped に残す。
// 上限には、原点がセッションの開始より前にある分を、1 秒まで加える。セッションの開始 (started_at_steady_us) は最初の
// セグメントを開くときの時刻で、そのセグメントが届いた時刻より後になる。原点は送信側が取り込んだ時刻に近い値で、届いた
// 時刻よりさらに前にある。加えないと、正常な録画ログでも末尾がその差だけ削られて tail_gap_clamped が真になる。原点が
// 0 になった録画ログでは差が日の単位なので、加えない。
// 上限を「終わり - 原点」にしないのは、原点が 0 になった録画ログで上限がなくなるためである。原点をセッションの開始に
// 寄せる方法も使わない (全セグメントの位置が一緒に動き、build_session_timeline で決めた位置が崩れる)。
inline std::int64_t black_limit_from_origin_us(const LedgerSessionRow &session, std::int64_t origin_steady_us)
{
	// session の行の 2 つの時刻がそろっているセッションだけが上限を持つ。そろっていない録画ログにはセッションの
	// 長さを示す値がなく、セグメントの長さの合計を上限にすると、実際の欠落まで削ってしまう (欠落はセグメントの
	// 長さに入らない)。
	if (session.started_at_steady_us <= 0 || session.ended_at_steady_us <= session.started_at_steady_us)
		return 0;
	std::int64_t limit = session.ended_at_steady_us - session.started_at_steady_us;
	// 原点がセッションの開始より前にある分を、1 秒まで加える (上のコメント)。
	const std::int64_t lead = session.started_at_steady_us - origin_steady_us;
	if (lead > 0 && lead <= kSameConnectionToleranceUs)
		limit += lead;
	return limit;
}

namespace detail {

// この人の黒埋め用のフレーム (エンコーダのインスタンスごとに 1 つある。最初のものを使う)。
inline const LedgerBlackRow *black_for(const LedgerDocument &doc, const std::string &user_id)
{
	for (const LedgerBlackRow &black : doc.blacks) {
		if (black.user_id == user_id)
			return &black;
	}
	return nullptr;
}

// 一時ファイルの名前 (`concat_<user_id>_<種類>.txt` など)。中間ファイルのフォルダーに置くので、保存先と同じボリュームに
// なる (空き容量の見積もりと同じ場所)。中間ファイルの名前には、表示名を使わない (まだ読めていない間があるため)。
inline std::string temp_file_name(const char *prefix, const std::string &user_id, StreamKind kind,
				  const char *extension)
{
	return std::string(prefix) + "_" + user_id + "_" + stream_kind_name(kind) + extension;
}

} // namespace detail

// 録画ログから、この人のこの種類の録画ファイルの作成の計画を作る。
inline FinalizePlan build_finalize_plan(const LedgerDocument &doc, const std::string &user_id, StreamKind kind,
					const FinalizeOptions &options)
{
	FinalizePlan plan;
	plan.user_id = user_id;
	plan.kind = kind;
	plan.branch = decide_branch(collect_branch_input(doc, user_id, kind));

	// 音声の形。チャンネル数は 2 に固定する (録画ログの segment の行はチャンネル数を持たないので、ワイヤー形式の
	// 値を使う)。サンプリングレートは、録画ログの音声のセグメントの timescale から読む (なければ 48000)。
	for (const LedgerSegmentRow &row : doc.segments) {
		if (row.user_id == user_id && row.kind == kind && row.media == MediaKind::Audio &&
		    row.timescale > 0) {
			plan.sample_rate = row.timescale;
			break;
		}
	}

	// 録画ファイルの名前に使う人の名前 (表示名。重なったら user_id の先頭を付ける)。
	std::vector<std::string> names;
	std::vector<std::string> ids;
	for (const LedgerPersonRow &person : doc.people) {
		names.push_back(sanitize_display_name(person.display_name, person.user_id));
		ids.push_back(person.user_id);
	}
	names = resolve_name_collisions(names, ids);
	std::string output_name = sanitize_display_name(std::string(), user_id);
	for (std::size_t i = 0; i < ids.size(); ++i) {
		if (ids[i] == user_id) {
			output_name = names[i];
			break;
		}
	}
	// 録画ファイルはセッションのフォルダーの直下に、一時ファイルは中間ファイルのフォルダーに置く。
	// 名前は output_file_name_for_session で作る。
	const std::string &word = kind == StreamKind::Voice ? options.voice_kind_word : options.screen_kind_word;
	const std::string used_word =
		!word.empty() ? word : std::string(kind == StreamKind::Voice ? kDefaultVoiceKindWord : kDefaultScreenKindWord);
	plan.output_file = join_path(options.session_dir, output_file_name_for_session(options.output_name_format, output_name,
									       used_word, doc.session.session_id));
	const std::string &materials = materials_dir_of(options);
	plan.pcm_path = join_path(materials, detail::temp_file_name("audio", user_id, kind, ".pcm"));
	plan.concat_path = join_path(materials, detail::temp_file_name("concat", user_id, kind, ".txt"));
	plan.filter_script_path = join_path(materials, detail::temp_file_name("filter", user_id, kind, ".txt"));
	plan.progress_path = join_path(materials, detail::temp_file_name("progress", user_id, kind, ".txt"));

	// 映像の並び (セグメントと、その間の黒)。映像と音声は同じ基準で置く (build_session_timeline)。
	const SessionTimeline timeline = build_session_timeline(doc);
	const std::int64_t origin = timeline.origin_steady_us;
	const std::vector<PlacedSegment> placed = place_segments(doc, timeline, user_id, kind, MediaKind::Video);
	plan.has_video = !placed.empty();
	plan.expected_timescale = 60000;
	if (plan.has_video && placed.front().row->timescale > 0)
		plan.expected_timescale = placed.front().row->timescale;
	if (plan.has_video) {
		plan.fps_num = placed.front().row->fps_num;
		plan.fps_den = placed.front().row->fps_den;
	}
	plan.size = dominant_size(doc.segments, user_id, kind);

	const LedgerBlackRow *material = detail::black_for(doc, user_id);
	const std::int64_t material_frames = material != nullptr ? material->frame_count : 0;

	// 黒で埋めてよい範囲の上限 (black_limit_from_origin_us)。セグメントそのものは切らない (実際に書いたものは
	// 残す) ので、上限が動作するのは黒だけである。
	const std::int64_t black_limit_us = black_limit_from_origin_us(doc.session, origin);

	auto append_black = [&plan, material, material_frames, black_limit_us](std::int64_t gap_us,
									      std::int64_t at_us) {
		if (gap_us <= 0)
			return;
		if (black_limit_us > 0 && at_us + gap_us > black_limit_us) {
			// 録画ログの 2 つの時刻が食い違っている (原点が 0 になったセッションなど)。そのまま使うと、
			// 何日分もの黒と数十 GB の生の PCM を作ることになる。
			gap_us = at_us < black_limit_us ? black_limit_us - at_us : 0;
			plan.tail_gap_clamped = true;
			if (gap_us <= 0)
				return;
		}
		const BlackPlan black = plan_black_frames(gap_us, material_frames, plan.fps_num, plan.fps_den,
							  plan.expected_timescale);
		if (black.total_frames <= 0)
			return;

		const std::string file = material != nullptr ? material->file : std::string();
		const std::int64_t whole_ticks = black_frames_to_ticks(black.material_frames, plan.fps_num,
								      plan.fps_den, plan.expected_timescale);
		for (int i = 0; i < black.repeat_count; ++i) {
			FinalizeItem item;
			item.file = file;
			item.black = true;
			item.frames = black.material_frames;
			item.ticks = whole_ticks;
			plan.items.push_back(item);
		}
		if (black.remainder_ticks > 0) {
			FinalizeItem item;
			item.file = file;
			item.black = true;
			item.out_point_ticks = black.out_point_ticks;
			item.frames = black.total_frames - black.repeat_count * black.material_frames;
			item.ticks = black.remainder_ticks;
			plan.items.push_back(item);
		}
		if (black.material_frames <= 0) {
			// 黒埋め用のフレームがない欠落 (ProRes で作成し、lavfi が埋める)。ファイルの行は作らないが、
			// 長さは持つ。
			FinalizeItem item;
			item.black = true;
			item.frames = black.total_frames;
			item.ticks = black.total_ticks;
			plan.items.push_back(item);
		}
		plan.gaps.push_back(to_gap_fill(black, at_us, material != nullptr ? material->encoder_instance_id
										 : std::string()));
	};

	std::int64_t cursor_us = 0;
	for (const PlacedSegment &one : placed) {
		append_black(one.start_us - cursor_us, cursor_us);

		FinalizeItem item;
		item.file = (options.repair_truncated && one.row->truncated) ? repaired_segment_file(one.row->file)
									    : one.row->file;
		item.frames = one.row->sample_count;
		item.ticks = one.row->duration_ticks;
		plan.items.push_back(item);
		cursor_us = one.start_us + one.duration_us;
	}

	// このセッションの終わりの位置 (原点から)。セッションの長さを超えないようにする (black_limit_from_origin_us)。
	std::int64_t session_end_us =
		doc.session.ended_at_steady_us > origin ? doc.session.ended_at_steady_us - origin : 0;
	if (black_limit_us > 0 && session_end_us > black_limit_us) {
		session_end_us = black_limit_us;
		plan.tail_gap_clamped = true;
	}

	// 末尾の欠落。画面共有をやめた後も、このセッションが終わるまでは黒で埋める (録画ファイルの長さを、
	// セッション全体の長さにそろえるため)。
	if (plan.has_video) {
		const std::int64_t end_us = session_end_us > cursor_us ? session_end_us : cursor_us;
		append_black(end_us - cursor_us, cursor_us);
	}

	if (plan.has_video && plan.branch == FinalizeBranch::ProRes) {
		// ProRes で録画ファイルを作成するときは、fps のフィルターが出す数で数える (fps_filter_frames のコメント)。
		// セグメントの行のフレーム数を fps のフィルターが出す数に修正し、長さはフレーム数 x 1 フレームの
		// 長さにする。セグメントごとの半フレーム未満の丸めを足し続けると、セグメントの多い人で 1 フレームの
		// 許容を超えるためである。生の PCM の長さもこの長さから作る (映像と音声を同じ長さにする)。
		std::int64_t chains = 0;
		bool in_black_run = false;
		for (FinalizeItem &item : plan.items) {
			if (!item.black)
				item.frames = fps_filter_frames(item.ticks, plan.expected_timescale, plan.fps_num, plan.fps_den);
			plan.expected_frames += item.frames;
			// 並びの数は build_filter_script と同じ数え方にする (続いた黒は 1 つの color にまとまる)。
			if (!item.black || !in_black_run)
				++chains;
			in_black_run = item.black;
		}
		plan.expected_duration_ticks =
			black_frames_to_ticks(plan.expected_frames, plan.fps_num, plan.fps_den, plan.expected_timescale);
		plan.frame_tolerance = chains < plan.expected_frames ? chains : plan.expected_frames - 1;
		if (plan.frame_tolerance < 0)
			plan.frame_tolerance = 0;
	} else {
		for (const FinalizeItem &item : plan.items) {
			plan.expected_frames += item.frames;
			plan.expected_duration_ticks += item.ticks;
		}
	}

	// 音声の書き込み位置。全体の長さの無音に、各セグメントの音声を実際の位置で書き込む。映像と同じ原点で置くので、
	// セグメントの間の欠落は 0 のサンプルのまま残る。
	const std::vector<PlacedSegment> audio_placed =
		place_segments(doc, timeline, user_id, kind, MediaKind::Audio);
	std::int64_t audio_end_us = 0;
	for (const PlacedSegment &one : audio_placed) {
		AudioWrite write;
		write.file = (options.repair_truncated && one.row->truncated)
				     ? repaired_segment_file(one.row->file)
				     : one.row->file;
		write.offset_samples = audio_offset_samples(one.start_us, plan.sample_rate);
		write.sample_count = one.row->sample_count;
		plan.audio_writes.push_back(write);
		const std::int64_t end_us = one.start_us + one.duration_us;
		if (end_us > audio_end_us)
			audio_end_us = end_us;
	}

	if (plan.has_video) {
		// 映像と同じ長さにする (-shortest を付けないので、長さが違うと長いほうが残る)。
		plan.pcm_total_samples = plan.expected_timescale > 0
						 ? (plan.expected_duration_ticks * plan.sample_rate) /
							   plan.expected_timescale
						 : 0;
	} else {
		// 映像を持たない録画ファイル (声と、音声だけの画面共有)。長さは音声で決まるので、timescale を
		// サンプリングレートにして録画ログと比べる (probed_frames には 0 を渡す)。
		std::int64_t end_us = audio_end_us;
		if (session_end_us > end_us)
			end_us = session_end_us; // 上と同じ上限を掛けた値を使う
		plan.pcm_total_samples = silence_samples_for(end_us, plan.sample_rate);
		plan.expected_timescale = plan.sample_rate;
		plan.expected_duration_ticks = plan.pcm_total_samples;
		plan.expected_frames = 0;
	}

	plan.concat_list = build_concat_list(plan);
	plan.filter_script = build_filter_script(plan);

	// ---- 引数 ----
	const char *const pcm_format = pcm_input_format_name(options.output_pcm);
	std::vector<std::string> args;
	args.push_back("-hide_banner");
	args.push_back("-nostdin");
	args.push_back("-y");
	args.push_back("-xerror"); // 必ず付ける (エラーのまま最後まで進んで、終了コード 0 で終わらないように)
	// 進み具合を、プログラムが読める形でファイルに書かせる (1 秒ごとの out_time_us)。標準出力のパイプで読むと
	// RecordPlatformOps の子プロセスの関数を変えることになるので、ファイルで受け取る。
	args.push_back("-progress");
	args.push_back(plan.progress_path);
	args.push_back("-stats_period");
	args.push_back("1");

	int audio_input_index = 0;
	if (plan.has_video && plan.branch == FinalizeBranch::Remux) {
		args.push_back("-f");
		args.push_back("concat");
		args.push_back("-safe");
		args.push_back("0");
		args.push_back("-i");
		args.push_back(plan.concat_path);
		audio_input_index = 1;
	} else if (plan.has_video) {
		for (const FinalizeItem &item : plan.items) {
			if (item.black)
				continue; // ProRes で録画ファイルを作成するときの黒は lavfi で作る
			args.push_back("-i");
			args.push_back(join_path(materials, item.file));
			++audio_input_index;
		}
	}

	args.push_back("-f");
	args.push_back(pcm_format);
	args.push_back("-ar");
	args.push_back(std::to_string(plan.sample_rate));
	args.push_back("-ac");
	args.push_back(std::to_string(plan.channels));
	args.push_back("-i");
	args.push_back(plan.pcm_path);

	// -map の後に -i を加えない。入力はここまでにする。
	if (plan.has_video && plan.branch == FinalizeBranch::ProRes) {
		args.push_back("-filter_complex_script");
		args.push_back(plan.filter_script_path);
		args.push_back("-map");
		args.push_back("[vout]");
	} else if (plan.has_video) {
		args.push_back("-map");
		args.push_back("0:v:0");
	}
	args.push_back("-map");
	args.push_back(std::to_string(audio_input_index) + ":a:0");

	if (plan.has_video && plan.branch == FinalizeBranch::ProRes) {
		args.push_back("-c:v");
		args.push_back("prores_ks");
		args.push_back("-profile:v");
		args.push_back("3");
		args.push_back("-pix_fmt");
		args.push_back("yuv422p10le");
		args.push_back("-vendor");
		args.push_back("apl0");
		// 録画ファイルの映像の fps を、計画の fps と指定する。スクリプトの最後の setpts を通るとフレームレートが不明に
		// なるので、指定しないと最後のフレームの長さが 0 になり、コンテナーの上で 1 フレーム短くなる (25 フレームを
		// 書いて 24 フレームと読める)。フレームはどれも一定の間隔の時刻に載っているので、-r でフレームが増えたり
		// 減ったりはしない。
		args.push_back("-r");
		args.push_back(detail::rational_text(plan.fps_num, plan.fps_den));
		// 映像の timescale を、録画ログの output_timescale にそろえる。付けないと、mov の muxer が fps から
		// timescale を選び (60 fps で 15360)、録画ログの値と実際のファイルが食い違う。
		args.push_back("-video_track_timescale");
		args.push_back(std::to_string(plan.expected_timescale));
		args.push_back("-c:a");
		args.push_back("copy");
	} else {
		args.push_back("-c");
		args.push_back("copy");
	}
	// -shortest は付けない。長さがずれたときに、メッセージもログも出さずに短いほうへ切り詰め、ずれを隠すためである。
	args.push_back(plan.output_file);

	plan.args = args;
	return plan;
}

// 録画ファイルの長さを録画ログと比べる。長さは整数で比べる。
// フレーム数はちょうど一致しなければならない。長さはコンテナーの丸めで 1 フレーム分まで動きうるので、そこまでは許す。
// ProRes で録画ファイルを作成するときだけは、plan.frame_tolerance (fps のフィルターに入る並びの数) のフレームまでずれてよく、長さは
// それに 1 フレームを加えた分まで許す。セグメントの fps が計画の fps と同じなら fps_filter_frames の数え方は丸めまで
// 一致するが、fps が混ざった人 (セグメントの最後のフレームの長さが計画の 1 フレームと違う)、color の長さの小数 6 桁の
// 丸め、デコーダーが見積もった最後のフレームの長さは、並び 1 つの終わりを隣のフレームへ動かしうるためである (並び
// 1 つで 1 フレームを超えては動かない)。許容の上限は期待のフレーム数より 1 少ない数にし、期待があるのに 1 フレームも
// ない録画ファイルは通さない (短い並びばかりの人で、0 フレームの録画ファイルまで許容に入らないように)。
// セグメントファイルが録画ログの示す長さより短いと (途中で切れた、途中で止まった)、fps のフィルターはファイルにある
// 長さの分しか出さないので、並びの数を超えて足りなければ失敗になる。
// 期待をちょうどの一致で求めると、並び 1 つで 1 フレームずれる形が残り、その人の録画ファイルの作成が失敗し続ける。長さだけを
// 1 フレームの許容で見ると、セグメントごとの丸めが積もってセグメントの多い人で外れる。許容を録画ファイル全体で
// 1 フレームにする方法も同じ理由で外れ、長さの割合にする方法は、長いセッションほど本当に欠けた録画ファイルを見逃す。
// 映像を持たない録画ファイルでは expected_frames が 0 で、timescale はサンプリングレートである。呼び出し元は
// probed_frames に 0 を、probed_duration_ticks に音声の長さを渡す。書いたサンプル数がそのまま長さなので、
// 1 サンプルも違わないことを求める。probed_timescale が録画ログの timescale と違えば、秒に修正して許容の中かを見る。
// 割り算で丸めずに、両辺に相手の timescale を掛けて整数のまま比べる。0 は録画ログと同じ timescale を示す。
inline bool finalize_output_matches(const FinalizePlan &plan, std::int64_t probed_duration_ticks,
				    std::int64_t probed_frames, int probed_timescale)
{
	if (plan.expected_frames > 0 && probed_frames <= 0)
		return false; // 映像を期待しているのに 1 フレームもない (上のコメント)
	const std::int64_t tolerance = plan.frame_tolerance > 0 ? plan.frame_tolerance : 0;
	const std::int64_t frame_difference = plan.expected_frames > probed_frames ? plan.expected_frames - probed_frames
										  : probed_frames - plan.expected_frames;
	if (frame_difference > tolerance)
		return false;
	const std::int64_t one_frame_ticks =
		plan.expected_frames > 0 ? plan.expected_duration_ticks / plan.expected_frames : 0;
	const std::int64_t frame_ticks = one_frame_ticks * (tolerance + 1);
	if (probed_timescale <= 0 || probed_timescale == plan.expected_timescale || plan.expected_timescale <= 0) {
		const std::int64_t difference = probed_duration_ticks > plan.expected_duration_ticks
							? probed_duration_ticks - plan.expected_duration_ticks
							: plan.expected_duration_ticks - probed_duration_ticks;
		return difference <= frame_ticks;
	}
	// probed / probed_ts と expected / expected_ts の差が frame_ticks / expected_ts 以内か。
	const std::int64_t probed_scaled = probed_duration_ticks * plan.expected_timescale;
	const std::int64_t expected_scaled = plan.expected_duration_ticks * probed_timescale;
	const std::int64_t difference = probed_scaled > expected_scaled ? probed_scaled - expected_scaled
									: expected_scaled - probed_scaled;
	return difference <= frame_ticks * probed_timescale;
}

inline bool finalize_output_matches(const FinalizePlan &plan, std::int64_t probed_duration_ticks,
				    std::int64_t probed_frames)
{
	return finalize_output_matches(plan, probed_duration_ticks, probed_frames, 0);
}

} // namespace cordmux::record

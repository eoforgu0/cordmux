// SPDX-License-Identifier: Apache-2.0

// セグメントファイル (fMP4) を書き込む SegmentWriter と、libavformat で録画のファイルを読む関数 (PCM の読み出し、録画
// ファイルの検査、途中で切れたセグメントの読める部分のコピー) と、録画ファイルの作成の一時ファイルを書く RawFile。
// セグメントは libavformat をリンクして C++ から書く。empty_moov を付けない mov は、強制終了した時点で 1 フレームも
// 読めないが、付けた mov は書き終えたフラグメントまで読める (途中で OBS が止まっても、そこまでのセグメントを
// 編集ソフトで開ける)。
// このヘッダーは libavformat のヘッダーを読み込まない (実装は record_writer.cpp の中に隠す)。libobs にも Qt にも
// 依存せず、Windows の名前 (test/lint/windows_names.txt) を書かない。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "record/record_types.h"

namespace cordmux::record {

// 映像の timescale。60000 にすると、60、59.94、30、29.97 fps のフレームの長さを、どれも整数で表せる (マイクロ秒の単位
// では、どれも整数にならない)。
inline constexpr int kVideoTimescale = 60000;

// フラグメントの長さ (1 秒。libavformat の frag_duration はマイクロ秒で受け取る)。音声だけのセグメントでは、
// frag_duration がないとフラグメントが 1 つしかできず、途中で止まったときに、ヘッダーが示す長さと中身の長さが
// 違うファイルになる。
inline constexpr int kFragDurationUs = 1000000;

// 映像のセグメントを開くときに決まっている値。
struct SegmentVideoSpec {
	// record_avcc.h の build_avcc で組み立てた avcC のバイト列そのもの (muxer に組み立てさせない)。
	std::vector<std::uint8_t> avcc;
	int width = 0;
	int height = 0;
	int fps_num = 0; // フレームの間隔の既定値に使う (最後の 1 フレームの長さ)
	int fps_den = 0;
};

// 音声のセグメントを開くときに決まっている値。音声は、劣化させないように、変換せずにワイヤー形式のままの f32 で
// 書く。
struct SegmentAudioSpec {
	int sample_rate = 48000;
	int channels = 2; // ワイヤー形式では 2 に固定
};

// セグメントを閉じたときに、録画ログの segment の行に書く値。
struct SegmentWriteStats {
	std::int64_t sample_count = 0;   // 映像はフレーム数、音声は 1 チャンネルあたりのサンプル数
	std::int64_t duration_ticks = 0; // timescale の単位で数えた長さ
	std::int64_t bytes = 0;          // 実際に書いたバイト数 (閉じた後の大きさ。close のコメント)
	std::int64_t nonmonotonic_pushed = 0; // 進まなかったタイムスタンプをずらした回数 (write_video)
	int timescale = 0;               // 映像は 60000、音声はサンプリングレート
};

// セグメントファイル 1 つを書く。映像か音声のどちらか 1 つだけを持つ。
class SegmentWriter {
public:
	SegmentWriter();
	~SegmentWriter();
	SegmentWriter(const SegmentWriter &) = delete;
	SegmentWriter &operator=(const SegmentWriter &) = delete;

	// path は絶対パス (UTF-8)。開けなければ false を返す (例外は投げない)。
	bool open_video(const std::string &path, const SegmentVideoSpec &spec);
	bool open_audio(const std::string &path, const SegmentAudioSpec &spec);

	bool is_open() const;
	int timescale() const;

	// AVCC に詰め替えたサンプルを書く。pts_us はセグメントの先頭からの位置で、最初のフレームには 0 を渡す
	// (セグメントのファイルを単独で開けるように、先頭を 0 にする)。keyframe はワイヤー形式の ws::kFlagKeyframe の
	// コピーで、AV_PKT_FLAG_KEY に反映する (反映しないと frag_keyframe が動作しない)。
	// 映像は 1 フレーム遅らせて書く。mov の trun はサンプルごとの長さを持ち、表示の時刻はその積み上げで決まるので、
	// 長さを一定の値 (フレームの間隔) で書くと、ずらしたり置き直したりした実際のタイムスタンプがセグメントファイル
	// から消える。1 フレーム遅らせれば、長さを「次のタイムスタンプ - 現在のタイムスタンプ」で正確に書ける。代わりに、
	// 途中で止まったときは最後の 1 フレームを失う (セグメントが読めるかには影響しない)。
	// タイムスタンプが進まなかったフレームは 1 単位ずらして書き、数える (SegmentWriteStats::nonmonotonic_pushed)。
	// ずらすのは record_clock.h の役目だが、ここでも同じ確認をする。mov は同じ位置に 2 つのサンプルを置けず、
	// muxer が失敗するとセグメントごと失うためである。この数を書く録画ログのフィールドはなく、今はこの統計にだけ
	// 残す。
	bool write_video(const std::uint8_t *sample, std::size_t len, std::int64_t pts_us, bool keyframe);

	// インターリーブした f32 の PCM を書く。samples は 1 チャンネルあたりのサンプル数。
	bool write_audio(const void *pcm, std::size_t bytes, std::int64_t samples);
	// 0 のサンプルを samples の数だけ書く (欠落を詰め直すときに使う)。詰め直しは 1 チャンネルあたり数千サンプルの
	// 単位なので、呼び出し元が毎回 0 の配列を作るより、ここで使い回す 0 の配列から書くほうが、メモリーの確保が
	// 少なくて済む。
	bool write_audio_silence(std::int64_t samples);

	// 閉じて、録画ログに書く値を返す。閉じた後は is_open() が偽になる。
	// 書いたバイト数は av_write_trailer の後に読む。hybrid_fragmented は閉じるときにファイルを普通の mov に
	// 書き直すので、途中で数えた合計は最後の大きさと一致しない。
	bool close(SegmentWriteStats *stats);
	// 書きかけのまま、閉じずに手放す。ファイルは削除しない (セグメントファイルは、失敗したときも削除しない)。
	void abort();

	// 閉じる前に読める途中の値 (画面に表示する、書いたバイト数に使う)。
	const SegmentWriteStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// 読み出し
// ---------------------------------------------------------------------------

// 読み出しも、FFmpeg の子プロセスを増やさないように libavformat で行い、書き込みと同じファイルに置く。avformat の
// ヘッダーの読み込み方 (C4244 の警告の抑止を含む) を 1 か所にまとめるためである。

// セグメントの mov から読み出した PCM の形。
struct SegmentPcmInfo {
	int sample_rate = 0;
	int channels = 0;
	std::int64_t samples = 0; // 1 チャンネルあたりのサンプル数
};

// セグメントの mov の PCM を先頭から読み、チャンクごとに sink に渡す。sink が false を返したら、そこで止めて false を
// 返す。
bool read_segment_pcm(const std::string &path, const std::function<bool(const void *data, std::size_t len)> &sink,
		      SegmentPcmInfo *info);

// 録画ファイルを調べた結果 (録画ファイルの作成のチェックに使う)。
struct OutputProbe {
	bool ok = false;
	std::int64_t duration_ticks = 0; // expected_timescale の単位に修正した長さ
	// 録画ファイルのストリームそのものの timescale と長さ。録画ログの finalize の行にはこちらを書く (録画ログの
	// 値と、ffprobe で読める値を同じにするため)。読めなければ 0。
	int stream_timescale = 0;
	std::int64_t stream_duration_ticks = 0;
	std::int64_t frames = 0;         // 映像のフレーム数 (映像がなければ 0)
	std::int64_t audio_samples = 0;  // 音声のサンプル数 (1 チャンネルあたり)
	bool has_video = false;
	bool has_audio = false;
};

// 録画ファイルの mov を開いて、長さとフレーム数を読む。録画ファイルの作成が調べるのは録画ファイルだけである (入力の値はすべて
// 録画ログにある)。例外は途中で切れたセグメントである (copy_readable_part)。閉じる前に止まったので、録画ログの行が
// 読める長さを示せず、セグメントのファイルそのものから読む。
OutputProbe probe_output(const std::string &path, int expected_timescale);

// 途中で切れたセグメントのうち、読める部分 (copy_readable_part の結果)。
struct ReadablePart {
	bool opened = false;              // 入力をセグメントとして開けたか
	// 読み出しそのものが失敗した (開けない、読めない。ほかのソフトが読み取りを許さない形で開いている、
	// ネットワークが一時的に切れた、ファイルがない)。中身の形で読めないとき (0 バイト、ヘッダーの途中で切れた、
	// 欠けたフラグメント) とは分ける。そちらでは偽にして、読めた所までを結果にする。
	bool io_failed = false;
	bool written = false;             // 出力を書き終えたか (出力を求めなかったときと、読めるものがなかったときは偽)
	MediaKind media = MediaKind::Video;
	int timescale = 0;                // 入力の timescale (映像は 60000、音声はサンプリングレート)
	std::int64_t samples = 0;         // 読めた数 (映像はフレーム数、音声は 1 チャンネルあたりのサンプル数)
	std::int64_t duration_ticks = 0;  // 読めた長さ (timescale の単位)
	std::int64_t source_bytes = 0;    // 入力の大きさ
	std::int64_t readable_end = 0;    // 最後に使ったサンプルの終わりの位置 (入力の先頭からのバイト数)
	std::int64_t written_bytes = 0;   // 出力の大きさ
	bool stopped = false;             // should_stop が true を返して途中で止めた (出力は不完全なので使わない)
};

// 途中で切れたセグメント (fMP4) を先頭から読み、読めるサンプルだけをコピーしたセグメントを out_path に書く
// (再エンコードしない)。読めるサンプルとは、読み出しが失敗せず、欠けたことを示すフラグ (AV_PKT_FLAG_CORRUPT) がなく、
// 中身が入力の終わりより手前に収まり、映像なら AVCC の NAL の並びがちょうどサンプルの大きさで終わる (0 で埋まった末尾を
// 読ませない) ものである。最初に読めなかった所で止める (それより後の映像は、欠けたフレームを参照しうるため)。
// out_path が空なら書かずに数えるだけで、max_samples が正ならその数に届いたところで止める (作成待ちの集計が、読める
// ものが 1 つでもあるかを見るため)。読めるサンプルが 1 つもなければ出力を作らない。
// サンプルの長さは次のサンプルの時刻との差で決め直す (読み出した長さは当てにならない)。映像の最後の 1 フレームには
// last_duration_ticks (入力の timescale の単位。セグメントを書いたときのフレームの間隔) を使う。SegmentWriter が閉じる
// ときに、最後の 1 フレームにフレームの間隔を使うのと同じである。0 なら読み出した値のまま使う。
// should_stop が true を返したら途中で止める (録画ファイルの作成を打ち切るとき。大きなセグメントのコピーで OBS の終了を
// 待たせない)。
ReadablePart copy_readable_part(const std::string &in_path, const std::string &out_path,
				std::int64_t max_samples = 0, std::int64_t last_duration_ticks = 0,
				const std::function<bool()> &should_stop = std::function<bool()>());

// ---------------------------------------------------------------------------
// 生のファイル (録画ファイルの作成の一時ファイルの PCM)
// ---------------------------------------------------------------------------

// 全体の長さの無音を作り、各セグメントの音声を実際の位置に書き込むためのファイル。位置を戻して書くので、末尾に追加する
// だけの RecordPlatformOps::append では足りない。実装は libavformat の avio で、UTF-8 のパスをそのまま受け取る
// (decode/ と同じ)。
class RawFile {
public:
	RawFile();
	~RawFile();
	RawFile(const RawFile &) = delete;
	RawFile &operator=(const RawFile &) = delete;

	// 新しく作り (すでにあれば中身を捨てる)、size_bytes バイトの 0 で埋める。
	bool create_zeroed(const std::string &path, std::int64_t size_bytes);
	// すでにあるファイルを切り詰めずに開き、末尾に続けて書く。次の起動で録画ファイルの作成を再開するとき、前のセッションの
	// 録画ログに行を追加するために使う。RecordPlatformOps::open_file はすでにあれば中身を捨てるので、ここだけは
	// avio を直接使う。
	bool open_append(const std::string &path);
	// 末尾に追加する (open_append で開いたときだけ意味を持つ)。
	bool append(const void *data, std::size_t len);
	// 先頭からの位置を指定して書く。後の書き込みが前の内容を上書きする (原点の置き直しで、セグメントの書き込みが
	// 重なりうるため)。
	bool write_at(std::int64_t offset_bytes, const void *data, std::size_t len);
	bool close();
	bool is_open() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// ファイルを 1 つ削除する。呼び出し元が削除するのは、録画ファイルの作成の一時ファイルと、作成を打ち切ったときの作りかけの
// 録画ファイルだけで、入力のセグメントファイルは 1 バイトも削除しない。
bool remove_file(const std::string &path);

// path の直下のフォルダーの名前を並べる (次の起動で、保存先の中の前のセッションを探すのに使う)。読めなければ (途中で
// 読めなくなったときも) 空を返す。
std::vector<std::string> list_subdirectories(const std::string &path);

// 上と同じ。読めたかを `*outcome` に入れる (ListOutcome は record_types.h にある)。OS の読み出しの失敗では例外を
// 投げない (collect_subdirectory_names)。
std::vector<std::string> list_subdirectories(const std::string &path, ListOutcome *outcome);

// 開いた一覧 it を終わりまでたどり、フォルダーの名前を並べて返す (list_subdirectories の本体)。テストが、途中で読めなく
// なる一覧を渡せるように、一覧の型をテンプレートの引数にする。DirIterator は std::filesystem::directory_iterator と
// 同じ形にする (既定の構築が終わりを表し、`!=` と `increment(ec)` を持ち、`*it` が `is_directory(ec)` と `path()` を
// 持つ)。
//   - 次へ進むのは increment(ec) だけにする (範囲 for と ++ を使わない)。標準の ++ は、2 つ目からの項目の読み出しの
//     失敗を filesystem_error で投げる。ここは問い合わせのスレッド、起動のときの UI のスレッド、録画ファイルの作成のスレッドの集計
//     から呼ばれ、どこにも例外を受ける所がないので、投げると OBS が終了する。
//   - 途中で読めなくなったら、読めた所までの名前を捨てて空を返し、Failed にする (作成待ちを探す保存先の一覧からは
//     外さず、届かない場所と同じに扱う)。読めた所までの名前を返すと、どこで切れたかで集計の結果が変わり、録画ファイルの作成が
//     済んでいないセッションの一部だけが集計に出る (platform の list_directory も、途中までの一覧を返さない)。
//   - 名前を UTF-8 に修正できない項目は飛ばす (Windows のファイル名は対になっていないサロゲートを持てて、u8string は例外を
//     投げる)。cordmux が作るセッションのフォルダーの名前は日時の ASCII なので、飛ばした項目は cordmux のセッションでは
//     ない。一覧ごと Failed にすると、ほかの誰かが置いたフォルダー 1 つのせいで、その保存先のセッションがずっと集計に
//     出なくなる。
//   - メモリーの確保の失敗 (bad_alloc) は受けない (どこで起きても同じで、ここだけで受けても OBS は続けられない)。
//     問い合わせのスレッドは、処理の全体で受ける (recorder.cpp の remember_pending_dirs)。
template <typename DirIterator>
std::vector<std::string> collect_subdirectory_names(DirIterator it, ListOutcome *outcome)
{
	if (outcome != nullptr)
		*outcome = ListOutcome::Listed;
	std::vector<std::string> names;
	const DirIterator end;
	while (it != end) {
		const auto &entry = *it;
		std::error_code dir_ec;
		if (entry.is_directory(dir_ec) && !dir_ec) {
			std::string name;
			try {
				name = entry.path().filename().u8string();
			} catch (const std::system_error &) {
				name.clear(); // UTF-8 に修正できない名前 (cordmux のセッションではない)
			}
			if (!name.empty())
				names.push_back(std::move(name));
		}
		std::error_code ec;
		it.increment(ec);
		if (ec) {
			if (outcome != nullptr)
				*outcome = ListOutcome::Failed;
			names.clear();
			return names;
		}
	}
	std::sort(names.begin(), names.end());
	return names;
}

// ファイルを丸ごと読む (録画ログをもう一度読むのに使う)。読めなければ false を返す。
bool read_whole_file(const std::string &path, std::string *out);

// ファイルの末尾の max_bytes だけを読む (FFmpeg の -progress の出力は 1 秒ごとに伸びるので、毎回丸ごとは読まない)。
// 読めなければ false を返す。
bool read_file_tail(const std::string &path, std::size_t max_bytes, std::string *out);

// リンクした libavformat のメジャーバージョン (テストが、リンクした libavformat のバージョンを確認するのに使う)。
unsigned record_avformat_major();

// 同梱の FFmpeg のバージョン (録画ログの session の行の ffmpeg_version)。子プロセスを起動せずに、リンクした libavutil
// から読む。同梱の ffmpeg の実行ファイルも、リンクした FFmpeg も obs-deps の同じビルドなので、録画を始めるたびに
// -version のためだけに子プロセスを起動しなくてよい。
std::string record_ffmpeg_version();

} // namespace cordmux::record

// SPDX-License-Identifier: Apache-2.0

// 録画の I/O の部分 (record/) のテストが共有するヘルパー。一時フォルダー、H.264 のテストデータの読み込みと
// アクセスユニットへの分割、音声のチャンクの作成、実際の ffmpeg と ffprobe の実行をまとめる。
// tests/ は windows_api_confinement.py の対象外なので、実行ファイルの名前を含む環境変数をここで読む。
// 製品の record/ には書かない。

#pragma once

#include "doctest.h"

#include "platform/platform.h"
#include "record/record_avcc.h"
#include "record/record_types.h"
#include "record/record_writer.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef CORDMUX_TEST_FIXTURES_DIR
#define CORDMUX_TEST_FIXTURES_DIR "fixtures"
#endif

namespace cordmux::tests::record_io {

// リポジトリの外の一時フォルダー。デストラクターが中身ごと削除し、テストの後にファイルを残さない
// (test_platform_record_ops.cpp の TempDir と同じ)。
class TempDir {
public:
	explicit TempDir(const char *tag)
	{
		static std::atomic<int> counter{0};
		const std::string name = std::string("cordmux_record_io_") + tag + "_" +
					 std::to_string(++counter) + "_" +
					 std::to_string(reinterpret_cast<std::uintptr_t>(this));
		dir_ = std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
		std::filesystem::create_directories(dir_);
	}

	~TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
	}

	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	std::string path() const { return dir_.u8string(); }
	std::string child(const std::string &leaf) const { return (dir_ / std::filesystem::u8path(leaf)).u8string(); }

private:
	std::filesystem::path dir_;
};

// H.264 のテストデータ (64x64、10 フレーム) を読む。作り方は tests/test_decode.cpp のコメントにある。
inline std::vector<std::uint8_t> read_h264_fixture()
{
	const std::string path = std::string(CORDMUX_TEST_FIXTURES_DIR) + "/h264_64x64_10f.h264";
	std::ifstream file(path, std::ios::binary);
	REQUIRE(file.good());
	return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// AUD (NAL type 9) の位置でアクセスユニットに分ける。ワイヤー形式では 1 つのメッセージが 1 つのアクセスユニット
// なので、1 つのファイルからその形を作る (test_decode.cpp と同じ分け方)。
inline std::vector<std::vector<std::uint8_t>> split_access_units(const std::vector<std::uint8_t> &bytes)
{
	std::vector<std::size_t> starts;
	std::size_t i = 0;
	while (i + 4 <= bytes.size()) {
		if (bytes[i] == 0x00 && bytes[i + 1] == 0x00 && bytes[i + 2] == 0x01) {
			if ((bytes[i + 3] & 0x1F) == 9)
				starts.push_back(i);
			i += 3;
			continue;
		}
		++i;
	}
	std::vector<std::vector<std::uint8_t>> units;
	for (std::size_t n = 0; n < starts.size(); ++n) {
		const std::size_t begin = starts[n];
		const std::size_t end = n + 1 < starts.size() ? starts[n + 1] : bytes.size();
		units.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
				   bytes.begin() + static_cast<std::ptrdiff_t>(end));
	}
	return units;
}

// そのアクセスユニットがキーフレーム (IDR) を含むか。
inline bool unit_is_keyframe(const std::vector<std::uint8_t> &unit)
{
	return cordmux::record::scan_annex_b(unit.data(), unit.size()).has_idr;
}

// 48 kHz、2 ch、f32 の音声のチャンクを作る (10 ms なら 480 サンプル)。読み出した結果が 1 バイトも違わない
// ことを確認しやすいように、どのサンプルも同じ値にする。
inline std::vector<std::uint8_t> make_audio_chunk(int samples, float value)
{
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(samples) * 2 * sizeof(float));
	float *out = reinterpret_cast<float *>(bytes.data());
	for (int i = 0; i < samples * 2; ++i)
		out[i] = value;
	return bytes;
}

// ---------------------------------------------------------------------------
// 実際の ffmpeg と ffprobe
// ---------------------------------------------------------------------------
//
// 環境変数 (CORDMUX_FFMPEG_EXE と CORDMUX_FFPROBE_EXE) が実在するファイルを指すときだけ実行し、指さなければ
// doctest::skip でスキップする。環境変数には obs-deps の ffmpeg.exe と ffprobe.exe を指定する。配布用の zip に
// 入れる ffmpeg.exe と同じビルドで確認するためで、ほかの ffmpeg では代わりにならない。CI は 2 つの環境変数を設定し、
// スキップしたテストが 1 件でもあれば失敗にする (.github/workflows/obs-plugin.yml)。

inline std::string env_path(const char *name)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // getenv は移植性があり、ここはテストだけが使う
#endif
	const char *value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
	if (value == nullptr || value[0] == '\0')
		return std::string();
	std::error_code ec;
	if (!std::filesystem::exists(std::filesystem::u8path(std::string(value)), ec))
		return std::string();
	return std::string(value);
}

inline std::string ffmpeg_exe()
{
	return env_path("CORDMUX_FFMPEG_EXE");
}

inline std::string ffprobe_exe()
{
	return env_path("CORDMUX_FFPROBE_EXE");
}

inline bool have_real_ffmpeg()
{
	return !ffmpeg_exe().empty() && !ffprobe_exe().empty();
}

// 子プロセスを起動して終わるまで待ち、その出力 (log_path に書かれたもの) を `output` に入れて、終了コードを返す。
// 起動できないか、120 秒で終わらなければ -1 を返す。
inline int run_tool(const std::string &program, const std::vector<std::string> &args, const std::string &log_path,
		    std::string *output)
{
	const cordmux::platform::ChildHandle child = cordmux::platform::start_child(program, args, log_path);
	if (child == 0)
		return -1;
	cordmux::record::ChildWaitResult result = cordmux::platform::wait_child(child, 120000);
	if (result.outcome != cordmux::record::ChildWait::Exited) {
		cordmux::platform::kill_child(child);
		return -1;
	}
	if (output != nullptr)
		cordmux::record::read_whole_file(log_path, output);
	return result.exit_code;
}

// `key=value` の行の並び (ffprobe の default の出力) から、`key` の値を取り出す。
inline std::string probe_value(const std::string &text, const std::string &key)
{
	const std::string needle = key + "=";
	std::size_t at = 0;
	while (at < text.size()) {
		const std::size_t line_end = text.find('\n', at);
		const std::string line = text.substr(at, line_end == std::string::npos ? std::string::npos
										     : line_end - at);
		if (line.compare(0, needle.size(), needle) == 0) {
			std::string value = line.substr(needle.size());
			while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
				value.pop_back();
			return value;
		}
		if (line_end == std::string::npos)
			break;
		at = line_end + 1;
	}
	return std::string();
}

struct ProbeStream {
	bool present = false;
	std::int64_t frames = 0;        // 映像のフレーム数 (-count_frames で数えた数)
	std::int64_t duration_ts = 0;   // そのストリームのタイムベースで数えた長さ
	int time_base_den = 0;          // タイムベース (1/den)
	std::int64_t sample_count = 0;  // 音声のサンプルの数 (1 ch あたり)
	int sample_rate = 0;
};

// 実際の ffprobe で映像のストリームを読む。
inline ProbeStream probe_video(const std::string &file, const std::string &log_path)
{
	ProbeStream out;
	std::string text;
	const int code = run_tool(ffprobe_exe(),
				  {"-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
				   "stream=nb_read_frames,duration_ts,time_base", "-of",
				   "default=noprint_wrappers=1", file},
				  log_path, &text);
	if (code != 0)
		return out;
	const std::string frames = probe_value(text, "nb_read_frames");
	const std::string duration = probe_value(text, "duration_ts");
	const std::string base = probe_value(text, "time_base");
	if (frames.empty() && duration.empty())
		return out;
	out.present = true;
	if (!frames.empty() && frames != "N/A")
		out.frames = std::stoll(frames);
	if (!duration.empty() && duration != "N/A")
		out.duration_ts = std::stoll(duration);
	const std::size_t slash = base.find('/');
	if (slash != std::string::npos)
		out.time_base_den = std::stoi(base.substr(slash + 1));
	return out;
}

// 実際の ffprobe で音声のストリームを読む。
inline ProbeStream probe_audio(const std::string &file, const std::string &log_path)
{
	ProbeStream out;
	std::string text;
	const int code = run_tool(ffprobe_exe(),
				  {"-v", "error", "-select_streams", "a:0", "-show_entries",
				   "stream=duration_ts,time_base,sample_rate", "-of",
				   "default=noprint_wrappers=1", file},
				  log_path, &text);
	if (code != 0)
		return out;
	const std::string duration = probe_value(text, "duration_ts");
	const std::string rate = probe_value(text, "sample_rate");
	const std::string base = probe_value(text, "time_base");
	if (duration.empty() && rate.empty())
		return out;
	out.present = true;
	if (!duration.empty() && duration != "N/A")
		out.duration_ts = std::stoll(duration);
	if (!rate.empty() && rate != "N/A")
		out.sample_rate = std::stoi(rate);
	const std::size_t slash = base.find('/');
	if (slash != std::string::npos)
		out.time_base_den = std::stoi(base.substr(slash + 1));
	out.sample_count = out.duration_ts;
	return out;
}

} // namespace cordmux::tests::record_io

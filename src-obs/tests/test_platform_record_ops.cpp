// SPDX-License-Identifier: Apache-2.0

// platform/platform.h の、録画で使う関数の単体テスト。引数の引用符の付け方 (build_child_command_line) は、子プロセスを
// 起動せずに全部の場合を確認する (崩れると、空白を含む場所が ffmpeg から見て別の引数に分かれる)。ASCII でない文字を
// 含む保存先で、フォルダーを作りファイルに書けることも確認する (tests/test_platform_file_io.cpp と同じ理由)。子プロセスは、
// 終了コードを読めること、待つ時間の上限で戻ること、止めたら止まるまで待つこと (止めた直後に作りかけの録画ファイルを
// 削除するため) を確認する。子プロセスを実際に起動するテストは Windows でだけ実行し (_WIN32 で囲む)、起動するのは
// 害のないプログラム (終了コードを返すだけ、1 行を出力するだけ、テストが止めるまで待つだけのもの) に限り、どれも
// 後始末する。

#include "doctest.h"

#include "platform/platform.h"
#include "record/record_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using cordmux::platform::append;
using cordmux::platform::build_child_command_line;
using cordmux::platform::close;
using cordmux::platform::ffmpeg_path;
using cordmux::platform::free_space_bytes;
using cordmux::platform::is_absolute_path;
using cordmux::platform::kill_child;
using cordmux::platform::make_directories;
using cordmux::platform::open_file;
using cordmux::platform::start_child;
using cordmux::platform::wait_child;

namespace {

// ASCII でない 2 文字を含む名前の一部 (tests/test_platform_file_io.cpp と同じ 2 文字)。どちらも日本語のコードページで表せない
// 文字で、幅が決まっている (タイ文字は 1 文字の幅、ハングルは 2 文字の幅)。曖昧幅の文字は、失敗したときに表示される
// 場所の文字列で桁がずれるので使わない。
constexpr const char *kNonAsciiPart = u8"ก한";

// リポジトリの外の一時フォルダー。デストラクターで中身ごと削除する。
class TempDir {
public:
	TempDir()
	{
		static std::atomic<int> counter{0};
		const std::string name = "cordmux_record_ops_test_" + std::to_string(++counter) + "_" +
					 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
					 std::string(kNonAsciiPart);
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

	// このフォルダーの下の場所を、UTF-8 の文字列で返す (platform/platform.h の関数が受け取る形)。
	std::string utf8_path(const std::string &relative) const
	{
		return (dir_ / std::filesystem::u8path(relative)).u8string();
	}

	std::filesystem::path fs_path(const std::string &relative) const
	{
		return std::filesystem::u8path(utf8_path(relative));
	}

	std::string utf8_root() const { return dir_.u8string(); }

private:
	std::filesystem::path dir_;
};

} // namespace

// ---------------------------------------------------------------------------
// 引数の引用符の付け方 (OS に依存しないので、どの OS でも実行する)
// ---------------------------------------------------------------------------

TEST_CASE("program は常に引用符で囲む (空白を含むフォルダーでも 1 つの実行ファイルとして渡る)")
{
	CHECK(build_child_command_line("C:\\Program Files\\obs\\ffmpeg", {}) ==
	      "\"C:\\Program Files\\obs\\ffmpeg\"");
}

TEST_CASE("空白も引用符も含まない引数は、そのまま並べる")
{
	CHECK(build_child_command_line("ff", {"-i", "a.mov", "-c", "copy"}) == "\"ff\" -i a.mov -c copy");
}

TEST_CASE("空白を含む引数は、引用符で囲む")
{
	CHECK(build_child_command_line("ff", {"-i", "my clip.mov"}) == "\"ff\" -i \"my clip.mov\"");
}

TEST_CASE("引用符で囲む引数の末尾のバックスラッシュは 2 倍にする (閉じる引用符をエスケープしないため)")
{
	// C:\my dir\ をそのまま引用符で囲むと、末尾のバックスラッシュが閉じる引用符をエスケープしてしまい、次の引数までが
	// 1 つの引数になる。
	CHECK(build_child_command_line("ff", {"C:\\my dir\\"}) == "\"ff\" \"C:\\my dir\\\\\"");
}

TEST_CASE("引用符で囲まない引数の中のバックスラッシュは、そのままにする (普通のパスのバックスラッシュを 2 倍にしない)")
{
	CHECK(build_child_command_line("ff", {"C:\\clips\\a.mov"}) == "\"ff\" C:\\clips\\a.mov");
}

TEST_CASE("引用符を含む引数は、引用符をエスケープして全体を引用符で囲む")
{
	CHECK(build_child_command_line("ff", {"say\"hi"}) == "\"ff\" \"say\\\"hi\"");
	// 引用符の直前のバックスラッシュは、2 倍にしてからエスケープする。
	CHECK(build_child_command_line("ff", {"a\\\"b"}) == "\"ff\" \"a\\\\\\\"b\"");
}

TEST_CASE("空の引数も、1 つの引数として残る")
{
	CHECK(build_child_command_line("ff", {"-metadata", ""}) == "\"ff\" -metadata \"\"");
}

// ---------------------------------------------------------------------------
// フォルダーとファイル (ASCII でない文字を含む保存先で確認する)
// ---------------------------------------------------------------------------

TEST_CASE("make_directories は途中の階層のフォルダーも作り、既にあっても成功する")
{
	const TempDir dir;
	const std::string deep = dir.utf8_path(std::string("cordmux/20000101-000000/segments_") + kNonAsciiPart);

	REQUIRE(make_directories(deep));
	CHECK(std::filesystem::is_directory(dir.fs_path(std::string("cordmux/20000101-000000/segments_") +
							kNonAsciiPart)));

	CHECK(make_directories(deep)); // 2 回目も成功する (既にあることは失敗ではない)
}

TEST_CASE("open_file で開いて append で書いて閉じると、書いた分だけの大きさのファイルになる (1 MiB を 1 回で書ける)")
{
	const TempDir dir;
	const std::string path = dir.utf8_path(std::string("black_") + kNonAsciiPart + ".bin");

	const cordmux::platform::FileHandle file = open_file(path);
	REQUIRE(file != cordmux::record::kInvalidFile);

	const std::string zeros(1024u * 1024u, '\0');
	CHECK(append(file, zeros.data(), zeros.size()));
	CHECK(append(file, "tail", 4));
	CHECK(append(file, nullptr, 0)); // 書くものがないのは失敗ではない
	close(file);

	CHECK(std::filesystem::file_size(dir.fs_path(std::string("black_") + kNonAsciiPart + ".bin")) ==
	      zeros.size() + 4);
}

TEST_CASE("open_file で開き直すと、前の中身は残らない")
{
	const TempDir dir;
	const std::string path = dir.utf8_path("ledger.jsonl");

	cordmux::platform::FileHandle file = open_file(path);
	REQUIRE(file != cordmux::record::kInvalidFile);
	CHECK(append(file, "aaaaaaaaaa", 10));
	close(file);

	file = open_file(path);
	REQUIRE(file != cordmux::record::kInvalidFile);
	CHECK(append(file, "b", 1));
	close(file);

	CHECK(std::filesystem::file_size(dir.fs_path("ledger.jsonl")) == 1u);
}

TEST_CASE("フォルダーがない場所は、open_file が開けない")
{
	const TempDir dir;
	CHECK(open_file(dir.utf8_path("no-such-dir/inner.mov")) == cordmux::record::kInvalidFile);
}

TEST_CASE("0 のハンドルには、append が書かずに false を返し、close も何もしない")
{
	CHECK_FALSE(append(cordmux::record::kInvalidFile, "x", 1));
	close(cordmux::record::kInvalidFile); // 何も起きない
}

// ---------------------------------------------------------------------------
// 空き容量
// ---------------------------------------------------------------------------

TEST_CASE("free_space_bytes は空き容量を読み、まだないフォルダーには上のフォルダーで調べて返す")
{
	const TempDir dir;
	const auto here = free_space_bytes(dir.utf8_root());
	REQUIRE(here.has_value());
	CHECK(*here > 0);

	// 録画を始める前の見積もりでは、まだ作っていない保存先の空き容量を調べる。
	const auto not_yet = free_space_bytes(dir.utf8_path("cordmux/20000101-000000"));
	REQUIRE(not_yet.has_value());
	CHECK(*not_yet > 0);
}

TEST_CASE("場所が空なら、free_space_bytes は nullopt を返す (0 を、空き容量が尽きたと読ませない)")
{
	CHECK_FALSE(free_space_bytes(std::string()).has_value());
}

// ---------------------------------------------------------------------------
// 同梱の FFmpeg の場所と、録画の保存先
// ---------------------------------------------------------------------------

TEST_CASE("ffmpeg_path は、同梱の FFmpeg が実際にあるときだけ、その場所を返す")
{
	// テストのビルドは ffmpeg.exe を実行ファイルのフォルダーにコピーしないので、ふつうは空が返る。置いてある環境では、
	// そのフォルダーの中の絶対パスが返る。
	const std::string path = ffmpeg_path();
	if (!path.empty()) {
		CHECK(path.find("ffmpeg") != std::string::npos);
		CHECK(path.find(':') != std::string::npos); // 絶対パス
	}
}

TEST_CASE("is_absolute_path は、ルートを持つ場所だけを絶対パスとし、ドライブだけが付いた相対の場所は絶対パスとしない")
{
	CHECK(is_absolute_path("C:/rec"));
	CHECK(is_absolute_path("C:\\rec\\cordmux"));
	CHECK(is_absolute_path("//server/share/rec"));

	CHECK_FALSE(is_absolute_path(""));
	CHECK_FALSE(is_absolute_path("rec"));
	CHECK_FALSE(is_absolute_path("./rec"));
	CHECK_FALSE(is_absolute_path("../rec"));
	CHECK_FALSE(is_absolute_path("/rec")); // ルート (ドライブ) を持たない場所

	// ドライブだけが付いた相対の場所 (保存先の入力欄で `C:\rec` の `\` を入れ忘れた形)。
	CHECK_FALSE(is_absolute_path("C:rec"));
	CHECK_FALSE(is_absolute_path("c:rec\\cordmux"));
	CHECK_FALSE(is_absolute_path("C:"));
	// 区切りが 1 つでもあればルートを持つ (`C:\` は正規化で `C:` になるので、区切りは正規化の前に見る)。
	CHECK(is_absolute_path("C:\\"));
	CHECK(is_absolute_path("C:/"));
	CHECK(is_absolute_path("\\\\server\\share"));
}

// ---------------------------------------------------------------------------
// RecordPlatformOps に入れられること
// ---------------------------------------------------------------------------

TEST_CASE("platform の関数を RecordPlatformOps のフィールドにそのまま入れて呼べる")
{
	// RecordPlatformOps (record/record_types.h) のフィールドの型と platform の関数の型が食い違うと、関数を入れる
	// record/ の側だけがコンパイルできなくなる。ここで 1 回入れておくと、型が変わったときに platform の側でも分かる。
	cordmux::record::RecordPlatformOps ops;
	ops.make_directories = &cordmux::platform::make_directories;
	ops.open_file = &cordmux::platform::open_file;
	ops.append = &cordmux::platform::append;
	ops.close = &cordmux::platform::close;
	ops.free_space_bytes = &cordmux::platform::free_space_bytes;
	ops.start_child = [](const std::string &program, const std::vector<std::string> &args) {
		return cordmux::platform::start_child(program, args); // 子プロセスの出力は捨てる
	};
	ops.wait_child = &cordmux::platform::wait_child;
	ops.kill_child = &cordmux::platform::kill_child;
	ops.now_steady_us = &cordmux::platform::now_steady_us;
	ops.now_wall_us = &cordmux::platform::now_wall_us;
	ops.ffmpeg_path = &cordmux::platform::ffmpeg_path;

	CHECK(bool(ops.make_directories));
	CHECK(bool(ops.open_file));
	CHECK(bool(ops.append));
	CHECK(bool(ops.close));
	CHECK(bool(ops.free_space_bytes));
	CHECK(bool(ops.start_child));
	CHECK(bool(ops.wait_child));
	CHECK(bool(ops.kill_child));
	CHECK(bool(ops.now_steady_us));
	CHECK(bool(ops.now_wall_us));
	CHECK(bool(ops.ffmpeg_path));

	// RecordPlatformOps から実際に呼んでみる (入れた関数が platform の関数に届いていること)。
	const TempDir dir;
	CHECK(ops.make_directories(dir.utf8_path("cordmux/20000101-000000/black")));
	CHECK(ops.now_wall_us() > 0);
	CHECK(ops.now_steady_us() > 0);
}

// ---------------------------------------------------------------------------
// 子プロセス (Windows でだけ実行する)
// ---------------------------------------------------------------------------

#ifdef _WIN32

namespace {

// 起動しても害のないプログラムとして、Windows に必ずある 2 つを使う。場所は環境変数から読む (テストのソースに
// フォルダーを書かない)。
//
// std::getenv には MSVC が安全でないという警告 (C4996) を出すが、ここはテストの中で、値を 1 つの文字列として
// そのまま使うだけである。製品のコードは platform/platform_win.cpp で GetEnvironmentVariableW を使い、警告を止めるのはこの
// 1 か所だけにする。
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
std::string command_interpreter_path()
{
	const char *value = std::getenv("ComSpec");
	return value == nullptr ? std::string() : std::string(value);
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ComSpec と同じフォルダーにある、待つだけのプログラム (kill_child で止めるテストに使う)。cmd.exe を通して起動しない。
// 通すと、止めたときに孫のプロセスが残る。
//
// このファイルで実行ファイルの名前を書くのは、この 1 か所だけにする (windows_api_confinement.py は tests/ を見ないが、
// 製品のコードと同じく 1 か所にまとめる)。
std::string loopback_wait_program()
{
	const std::string shell = command_interpreter_path();
	const std::size_t cut = shell.find_last_of("\\/");
	if (cut == std::string::npos)
		return std::string();
	return shell.substr(0, cut + 1) + "ping.exe";
}

} // namespace

TEST_CASE("wait_child で、子プロセスの終了コードを読める")
{
	const std::string shell = command_interpreter_path();
	REQUIRE_FALSE(shell.empty());

	const cordmux::platform::ChildHandle child = start_child(shell, {"/c", "exit", "3"});
	REQUIRE(child != cordmux::record::kInvalidChild);

	const cordmux::record::ChildWaitResult result = wait_child(child, 10000);
	CHECK(result.outcome == cordmux::record::ChildWait::Exited);
	CHECK(result.exit_code == 3);

	// 終わった子プロセスに 2 回目の wait_child を呼ぶと Failed になる (使い回された値の、別のプロセスを指さない)。
	CHECK(wait_child(child, 0).outcome == cordmux::record::ChildWait::Failed);
}

TEST_CASE("start_child は、子プロセスの出力をファイルに書ける (録画ファイルの作成の失敗の原因を分けるのに使う)")
{
	const TempDir dir;
	const std::string shell = command_interpreter_path();
	REQUIRE_FALSE(shell.empty());

	const std::string log = dir.utf8_path(std::string("ffmpeg_") + kNonAsciiPart + ".log");
	const cordmux::platform::ChildHandle child = start_child(shell, {"/c", "echo", "shown"}, log);
	REQUIRE(child != cordmux::record::kInvalidChild);
	REQUIRE(wait_child(child, 10000).outcome == cordmux::record::ChildWait::Exited);

	std::string text;
	REQUIRE(cordmux::platform::read_file_bytes(log, text, 1024u * 1024u));
	CHECK(text.find("shown") != std::string::npos);
}

TEST_CASE("wait_child は待つ時間の上限で戻り、kill_child は子プロセスが止まるまで待ってから戻る")
{
	const std::string waiter = loopback_wait_program();
	REQUIRE_FALSE(waiter.empty());

	// 127.0.0.1 に 10 回だけ送る。外には 1 バイトも出ない。
	const cordmux::platform::ChildHandle child = start_child(waiter, {"-n", "10", "127.0.0.1"});
	REQUIRE(child != cordmux::record::kInvalidChild);

	CHECK(wait_child(child, 50).outcome == cordmux::record::ChildWait::Timeout);

	kill_child(child);
	// 止めた後は、その子プロセスを指す値が無効になる (プロセスも残っていない)。
	CHECK(wait_child(child, 0).outcome == cordmux::record::ChildWait::Failed);
	kill_child(child); // 2 回目は何も起きない
}

TEST_CASE("ない実行ファイルは、start_child が起動できずに 0 を返す")
{
	const TempDir dir;
	CHECK(start_child(dir.utf8_path("no-such-tool"), {}) == cordmux::record::kInvalidChild);
	CHECK(start_child(std::string(), {}) == cordmux::record::kInvalidChild);
}

TEST_CASE("知らない値には、wait_child が Failed を返し、kill_child は何もしない")
{
	CHECK(wait_child(cordmux::record::kInvalidChild, 0).outcome == cordmux::record::ChildWait::Failed);
	kill_child(cordmux::record::kInvalidChild); // 何も起きない
}

TEST_CASE("utc_offset_sec_now は 15 分単位の時差を返し、videos_dir は絶対パスを返す")
{
	// 時差は 15 分単位で、-12 時間から +14 時間の範囲に入る (録画のセッションのフォルダーの名前を、OBS の録画の既定の
	// 名前と同じく現地の時刻で付けるために使う)。
	const int offset = cordmux::platform::utc_offset_sec_now();
	CHECK(offset % (15 * 60) == 0);
	CHECK(offset >= -12 * 3600);
	CHECK(offset <= 14 * 3600);
	// ビデオのフォルダーは絶対パス (OBS の録画の保存先を読めないときに、録画の保存先として使う)。
	const std::string videos = cordmux::platform::videos_dir();
	REQUIRE_FALSE(videos.empty());
	CHECK(is_absolute_path(videos));
}

#endif // _WIN32

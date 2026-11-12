// SPDX-License-Identifier: Apache-2.0

// platform/platform.h の Windows の実装。cordmux.dll の中で Windows の API を呼ぶのはこのファイルだけである。
// 場所の文字列は UTF-8 で受け取り、UTF-16 に変えてから Windows の API に渡す。

// winsock2.h は windows.h より先に読み込む (obs や Qt が間接的に windows.h を読み込むため)。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <aclapi.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <tlhelp32.h>

#include "platform/platform.h"

// wait_child の戻り値の型 (ChildWaitResult) は record/record_types.h の型をそのまま使う (platform/platform.h は前方宣言だけを
// 持つ)。
#include "record/record_types.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace cordmux::platform {

namespace {

// FILETIME (1601-01-01 からの 100 ナノ秒単位) を 1 つの int64 にする。is_process_alive で比べるための値で、時刻として
// は使わない (時刻にするときは os_time_to_epoch_us を通す)。
int64_t filetime_to_i64(const FILETIME &ft)
{
	ULARGE_INTEGER u;
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return int64_t(u.QuadPart);
}

// OS の時刻 (1601-01-01 からの 100 ナノ秒単位) を、エポック (1970-01-01) からのマイクロ秒にする。Vesktop の設定を
// 書いた時刻と、Vesktop の起動の時刻を同じ基準で比べるための変換である (platform/platform.h の VesktopProcessInfo)。
// kEpochDeltaIn100ns は、1601-01-01 から 1970-01-01 までの日数 (134774 日) を 100 ナノ秒単位で数えた値。
int64_t os_time_to_epoch_us(int64_t os_time_100ns)
{
	constexpr int64_t kEpochDeltaIn100ns = 116444736000000000LL;
	if (os_time_100ns <= 0)
		return 0;
	return (os_time_100ns - kEpochDeltaIn100ns) / 10;
}

// 利用者のプロファイルの下のフォルダー (%LOCALAPPDATA% など) を決める。先に環境変数を読み、ないか空のときだけ
// SHGetKnownFolderPath を使う。実機テストの実行スクリプトは、環境変数を差し替えたポータブル版の OBS を起動して、
// 利用者の実際のプロファイルに触らない。SHGetKnownFolderPath は環境変数を見ないので、先に環境変数を読まないと
// 差し替えが動作しない。Windows はふだん同じ値を環境変数にも入れているので、テストでないときの動作は変わらない。
std::optional<std::wstring> profile_dir_w(const wchar_t *env_name, REFKNOWNFOLDERID folder_id)
{
	wchar_t env_buf[MAX_PATH + 1] = {};
	const DWORD env_len = GetEnvironmentVariableW(env_name, env_buf, MAX_PATH + 1);
	if (env_len > 0 && env_len <= MAX_PATH)
		return std::wstring(env_buf, env_len);

	PWSTR path = nullptr;
	const HRESULT hr = SHGetKnownFolderPath(folder_id, 0, nullptr, &path);
	if (FAILED(hr) || path == nullptr) {
		if (path)
			CoTaskMemFree(path);
		return std::nullopt;
	}
	std::wstring result(path);
	CoTaskMemFree(path);
	return result;
}

// session.json を置くフォルダーの親 (%LOCALAPPDATA%)。
std::optional<std::wstring> local_app_data_dir_w()
{
	return profile_dir_w(L"LOCALAPPDATA", FOLDERID_LocalAppData);
}

// session.json を置くフォルダー (%LOCALAPPDATA% の下の cordmux)。フォルダーは作らない。アンインストールがこの
// フォルダーを削除した後に場所を調べ直すことがあり、場所を調べるだけでフォルダーができると、削除したものが
// 戻ってしまう。フォルダーを作るのは、session.json を書く session_file_path_w だけである。
std::optional<std::wstring> local_data_dir_w()
{
	const auto base = local_app_data_dir_w();
	if (!base)
		return std::nullopt;
	return *base + L"\\cordmux";
}

// Windows の RoamingAppData の既知のフォルダー。Vesktop の既定の保存先の親で、Vesktop (Electron の
// app.getPath("userData")) も同じ既知のフォルダーを読む。環境変数の APPDATA は見ない: Vesktop が見ない値を見ると、
// 環境変数だけが違う OBS で、Vesktop が読まない場所に書いてしまう。
std::optional<std::wstring> roaming_app_data_dir_w()
{
	PWSTR path = nullptr;
	const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path);
	if (FAILED(hr) || path == nullptr) {
		if (path)
			CoTaskMemFree(path);
		return std::nullopt;
	}
	std::wstring result(path);
	CoTaskMemFree(path);
	return result;
}

// 末尾の区切りを除く (除くと空になるルートと、"C:\" は除かない)。
std::wstring trim_trailing_separators_w(std::wstring text)
{
	while (text.size() > 1 && (text.back() == L'\\' || text.back() == L'/') && !(text.size() == 3 && text[1] == L':'))
		text.pop_back();
	return text;
}

// Vesktop が設定を置くフォルダー (Vesktop v1.6.9 の src/main/constants.ts の DATA_DIR) を、Vesktop と同じ順で決める。
//   (1) 環境変数 VENCORD_USER_DATA_DIR があればそこ。
//   (2) ポータブル版の Vesktop は exe の隣の Data。OBS の側からは、Vesktop が動いていない間は exe の場所を知れない
//       ので、ここでは扱わない (動いているときに見つけて、書かずに伝える。vesktop_portable_seen)。
//   (3) それ以外は RoamingAppData の既知のフォルダーの下の Vesktop。
// 実機テストの実行スクリプトは (1) を使って、利用者の実際の設定に触らずにテストする (Vesktop 自身が持つ上書きの手段)。
std::optional<std::wstring> vesktop_data_dir_w()
{
	// 環境変数の値の上限 (Windows の環境変数の値の上限は 32767 文字)。値があるのに読めないときは、Vesktop が使う場所を
	// 知れないので、既知のフォルダーに落とさず、場所が決まらないものとする (別の場所を黙って書かないため)。
	std::vector<wchar_t> env_buf(32768);
	const DWORD env_len = GetEnvironmentVariableW(L"VENCORD_USER_DATA_DIR", env_buf.data(), DWORD(env_buf.size()));
	if (env_len >= env_buf.size())
		return std::nullopt;
	if (env_len > 0)
		return trim_trailing_separators_w(std::wstring(env_buf.data(), env_len));

	const auto base = roaming_app_data_dir_w();
	if (!base)
		return std::nullopt;
	return *base + L"\\Vesktop";
}

// cordmux.dll があるフォルダー (末尾の区切りは付けない)。この DLL の中のアドレスから調べるので、実行ファイル
// (obs64.exe) ではなく DLL のフォルダーを返す (同梱のファイルは DLL と同じフォルダーに置く)。ffmpeg_path と
// module_dir が共有する (調べ方を 2 か所に書かない)。
std::optional<std::wstring> module_dir_w()
{
	static const char kAddressInThisModule = 0;
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&kAddressInThisModule), &module))
		return std::nullopt;

	std::vector<wchar_t> buf(MAX_PATH + 1);
	for (;;) {
		const DWORD written = GetModuleFileNameW(module, buf.data(), DWORD(buf.size()));
		if (written == 0)
			return std::nullopt;
		if (written < buf.size()) {
			buf.resize(written);
			break;
		}
		if (buf.size() >= 32768)
			return std::nullopt; // 場所の長さの上限を超えたので、読めないものとする
		buf.resize(buf.size() * 2);
	}

	const std::wstring module_path(buf.begin(), buf.end());
	const size_t cut = module_path.find_last_of(L"\\/");
	if (cut == std::wstring::npos || cut == 0)
		return std::nullopt;
	return module_path.substr(0, cut);
}

// UTF-8 と UTF-16 の変換。場所の文字列は、platform/ の外では UTF-8 にそろえる。ほかの OS では変換が要らないので、
// この 2 つもこのファイルの中だけに置く。
std::string wide_to_utf8(const std::wstring &text)
{
	if (text.empty())
		return std::string();
	const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
	if (needed <= 0)
		return std::string();
	std::string out(size_t(needed), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), needed, nullptr, nullptr);
	return out;
}

std::optional<std::wstring> utf8_to_wide(const std::string &text)
{
	if (text.empty())
		return std::wstring();
	const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), int(text.size()), nullptr, 0);
	if (needed <= 0)
		return std::nullopt; // UTF-8 として読めない場所は扱わない (文字化けした場所を使わない)
	std::wstring out(size_t(needed), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), int(text.size()), out.data(), needed);
	return out;
}

// %LOCALAPPDATA%\cordmux\session.json のパス。cordmux のフォルダーがなければ作る。場所は local_data_dir_w で
// 決める (local_data_dir() と同じ場所を指す)。
std::optional<std::wstring> session_file_path_w()
{
	const auto dir_opt = local_data_dir_w();
	if (!dir_opt)
		return std::nullopt;
	const std::wstring &dir = *dir_opt;
	if (!CreateDirectoryW(dir.c_str(), nullptr)) {
		if (GetLastError() != ERROR_ALREADY_EXISTS)
			return std::nullopt;
	}
	return dir + L"\\session.json";
}

// 現在の利用者の SID を、ConvertSidToStringSidW が返す文字列の形で返す。
std::optional<std::wstring> current_user_sid_string_w()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return std::nullopt;

	DWORD needed = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
	if (needed == 0) {
		CloseHandle(token);
		return std::nullopt;
	}
	std::vector<uint8_t> buf(needed);
	const BOOL ok = GetTokenInformation(token, TokenUser, buf.data(), needed, &needed);
	CloseHandle(token);
	if (!ok)
		return std::nullopt;

	const auto *user = reinterpret_cast<const TOKEN_USER *>(buf.data());
	LPWSTR sid_str = nullptr;
	if (!ConvertSidToStringSidW(user->User.Sid, &sid_str))
		return std::nullopt;
	std::wstring result(sid_str);
	LocalFree(sid_str);
	return result;
}

// path の DACL を、現在の利用者だけがフルアクセスで、親から継承しない形に置き換える。同じ利用者の権限で動く
// プログラムはこのファイルを読めるので、それは防げない。防ぐのは、同じ PC のほかの利用者のアカウントから読まれる
// ことである。
bool apply_owner_only_acl(const std::wstring &path)
{
	const auto sid = current_user_sid_string_w();
	if (!sid)
		return false;

	// D:P は、親のフォルダーからの継承を止めた DACL。ACE は 1 つだけ (現在の利用者にフルアクセス)。
	const std::wstring sddl = L"D:P(A;;FA;;;" + *sid + L")";

	PSECURITY_DESCRIPTOR sd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
	                                                            nullptr))
		return false;

	PACL dacl = nullptr;
	BOOL dacl_present = FALSE;
	BOOL dacl_defaulted = FALSE;
	const bool got_dacl =
		GetSecurityDescriptorDacl(sd, &dacl_present, &dacl, &dacl_defaulted) && dacl_present && dacl;

	bool applied = false;
	if (got_dacl) {
		const DWORD rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
		                                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
		                                        nullptr, nullptr, dacl, nullptr);
		applied = (rc == ERROR_SUCCESS);
	}
	LocalFree(sd);
	return applied;
}

// session.json は 4 つのフィールドだけの平らな JSON なので、書く関数と読む関数を小さく自分で持ち、JSON のライブラリ
// に依存しない (形式は docs/wire_format.md の「トークンファイル」)。
std::string escape_json_string(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (const char c : s) {
		if (c == '"' || c == '\\')
			out.push_back('\\');
		out.push_back(c);
	}
	return out;
}

std::string build_session_json(const SessionInfo &info)
{
	std::ostringstream os;
	os << "{\"port\":" << info.port << ",\"secret\":\"" << escape_json_string(info.secret_hex)
	   << "\",\"pid\":" << info.writer_pid << ",\"started_at\":" << info.writer_started_at << "}";
	return os.str();
}

// "key":value と "key":"value" の値を 1 つ読む。build_session_json が作った形だけを読む前提の、最小限の読み方である。
bool find_string_field(const std::string &text, const std::string &key, std::string &out)
{
	const std::string needle = "\"" + key + "\":\"";
	const size_t pos = text.find(needle);
	if (pos == std::string::npos)
		return false;
	const size_t start = pos + needle.size();
	std::string value;
	for (size_t i = start; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			value.push_back(text[i + 1]);
			i++;
			continue;
		}
		if (text[i] == '"') {
			out = value;
			return true;
		}
		value.push_back(text[i]);
	}
	return false;
}

bool find_int_field(const std::string &text, const std::string &key, int64_t &out)
{
	const std::string needle = "\"" + key + "\":";
	const size_t pos = text.find(needle);
	if (pos == std::string::npos)
		return false;
	const size_t start = pos + needle.size();
	size_t end = start;
	while (end < text.size() && (isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-'))
		end++;
	if (end == start)
		return false;
	// 例外を投げない std::from_chars で読み、取り出した文字列の全体が 64 ビットの整数のときだけ読めたとする。
	// session.json は OBS がプラグインを読み込む途中で読むので、壊れた中身 (書きかけやディスクのエラー) で std::stoll が
	// 例外を投げると、OBS がクラッシュする。符号だけ、符号が 2 つ、途中に符号がある (12-3)、64 ビットに収まらない、の
	// どれも、読めないものとする (読めていない値を、値として使わない)。
	const char *first = text.data() + start;
	const char *last = text.data() + end;
	int64_t value = 0;
	const std::from_chars_result read = std::from_chars(first, last, value);
	if (read.ec != std::errc() || read.ptr != last)
		return false;
	out = value;
	return true;
}

// ---- 録画で使う関数の下請け ----

// 子プロセスを指す値 (通し番号) と、OS のハンドルの対応表。
//
// 通し番号にするのは、次の理由による。RecordPlatformOps には子プロセスを閉じる関数がなく、終わった子プロセスの
// ハンドルは wait_child か kill_child が閉じる。閉じたハンドルの値は OS が使い回すので、OS のハンドルをそのまま
// 返すと、終わった後の値で kill_child を呼んだときに、別のプロセスを止めてしまう。OBS の中ではほかのコードが絶えず
// ハンドルを開け閉めしているので、実際に起こりうる。通し番号は 1 から増えるだけで、同じ値を二度使わない。
struct ChildTable {
	std::mutex mu;
	uint64_t next_id = 1;
	std::unordered_map<uint64_t, HANDLE> handles;
};

ChildTable &child_table()
{
	static ChildTable table;
	return table;
}

uint64_t register_child(HANDLE process)
{
	ChildTable &table = child_table();
	std::lock_guard<std::mutex> lock(table.mu);
	const uint64_t id = table.next_id++;
	table.handles.emplace(id, process);
	return id;
}

// 通し番号からハンドルを返す (表からは外さない)。知らない番号には nullptr を返す。
HANDLE find_child(uint64_t id)
{
	ChildTable &table = child_table();
	std::lock_guard<std::mutex> lock(table.mu);
	const auto it = table.handles.find(id);
	return it == table.handles.end() ? nullptr : it->second;
}

// 番号を表から外して、ハンドルを閉じる (2 回呼んでも、閉じるのは 1 回だけ)。
void forget_child(uint64_t id)
{
	HANDLE process = nullptr;
	{
		ChildTable &table = child_table();
		std::lock_guard<std::mutex> lock(table.mu);
		const auto it = table.handles.find(id);
		if (it == table.handles.end())
			return;
		process = it->second;
		table.handles.erase(it);
	}
	if (process != nullptr)
		CloseHandle(process);
}

// 引数 1 つに引用符を付ける (子プロセスが引数を読む規則に合わせる。platform/platform.h の build_child_command_line)。
//   - 空白も引用符も含まない、空でない文字列はそのまま
//   - それ以外は前後を引用符で囲む。引用符の前のバックスラッシュは 2 倍にし、引用符にもバックスラッシュを付ける。
//     末尾のバックスラッシュも 2 倍にする (閉じる引用符をエスケープしてしまわないため)
std::string quote_argument(const std::string &arg)
{
	bool needs_quotes = arg.empty();
	for (const char c : arg) {
		if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
			needs_quotes = true;
			break;
		}
	}
	if (!needs_quotes)
		return arg;

	std::string out;
	out.push_back('"');
	for (size_t i = 0; i < arg.size(); i++) {
		size_t backslashes = 0;
		while (i < arg.size() && arg[i] == '\\') {
			backslashes++;
			i++;
		}
		if (i == arg.size()) {
			out.append(backslashes * 2, '\\'); // 末尾なので、閉じる引用符をエスケープしないように 2 倍にする
			break;
		}
		if (arg[i] == '"') {
			out.append(backslashes * 2 + 1, '\\');
			out.push_back('"');
			continue;
		}
		out.append(backslashes, '\\');
		out.push_back(arg[i]);
	}
	out.push_back('"');
	return out;
}

// 区切りを 1 種類にそろえ、末尾の区切りを取り除く (フォルダーを作る関数が使う)。
std::wstring normalize_dir_path_w(const std::wstring &path)
{
	std::wstring out = path;
	for (wchar_t &ch : out) {
		if (ch == L'/')
			ch = L'\\';
	}
	while (out.size() > 1 && out.back() == L'\\')
		out.pop_back();
	return out;
}

// ルート (これより上はフォルダーを作れない部分) の長さ。"C:" なら 2、"\\\\server\\share" ならその末尾まで。
// 相対の場所と、1 文字目から始まる場所は 0 (最初の階層から作る)。
size_t root_length_w(const std::wstring &path)
{
	if (path.size() >= 2 && path[1] == L':')
		return 2;
	if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
		const size_t server_end = path.find(L'\\', 2);
		if (server_end == std::wstring::npos)
			return path.size();
		const size_t share_end = path.find(L'\\', server_end + 1);
		return share_end == std::wstring::npos ? path.size() : share_end;
	}
	return 0;
}

// ---- ファイルの置き換えの下請け ----

// 開いたファイルに len バイトをすべて書く (append と、一時ファイルを書く関数が共有する)。len が 0 なら何もせずに
// true を返す。
bool write_all(HANDLE handle, const void *data, std::size_t len)
{
	if (len == 0)
		return true;
	if (data == nullptr)
		return false;
	const auto *cursor = static_cast<const uint8_t *>(data);
	std::size_t left = len;
	while (left > 0) {
		// 1 回に書くのは 64 MiB までに分ける (大きなデータを 1 回で渡されても、WriteFile の長さの型 (DWORD) から
		// あふれないようにするため。64 MiB という値に意味はない)。
		constexpr std::size_t kMaxChunk = 64u * 1024u * 1024u;
		const DWORD chunk = DWORD(left < kMaxChunk ? left : kMaxChunk);
		DWORD written = 0;
		if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0)
			return false;
		cursor += written;
		left -= written;
	}
	return true;
}

// その場所が、いまフォルダーとしてあるか (ないか、ファイルなら false)。
bool is_directory_w(const std::wstring &path)
{
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// もうないことを示すエラーか (削除の関数は、もうなければ true を返すため)。
bool is_not_found_error(DWORD error)
{
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// 置き換える先と同じフォルダーに置く一時ファイルの場所。名前の形は `<置き換える先の名前>.cordmux-tmp-<pid>-<通し番号>`
// である (platform/platform.h の、インストールで使うファイルの操作のコメント)。同じフォルダーに置くのは、置き換えを 1 回の
// 名前の変更で済ませるためである (別のボリュームをまたぐとコピーになり、途中で失敗すると途中までの中身が残る)。
std::wstring temp_path_for_w(const std::wstring &target)
{
	static std::atomic<uint64_t> sequence{0};
	return target + L".cordmux-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
	       std::to_wstring(++sequence);
}

// 一時ファイルを新しく作り (同じ名前があれば作らずに失敗する)、fill に中身を書かせ、ディスクに書き出してから
// 閉じる。途中で失敗したら一時ファイルを削除して false を返す。書いている間は、ほかのだれにも開かせない
// (共有の指定を 0 にして開く)。
bool write_temp_w(const std::wstring &temp, const std::function<bool(HANDLE)> &fill)
{
	const HANDLE file =
		CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	// 名前を変える前にディスクに書き出す。書き出さずに名前を変えると、電源が落ちたときに、名前は新しいのに中身が空の
	// ファイルが残ることがある (元の中身も新しい中身も失う)。
	const bool ok = fill(file) && FlushFileBuffers(file) != 0;
	CloseHandle(file);
	if (!ok)
		DeleteFileW(temp.c_str());
	return ok;
}

// 一時ファイルの名前を target に変える (target が既にあれば置き換える)。失敗したら一時ファイルを削除して false を
// 返す (target は元のまま)。
//
// ReplaceFileW は使わない。バックアップのファイル名を渡さずに呼ぶと、名前の変更に失敗したとき
// (ERROR_UNABLE_TO_MOVE_REPLACEMENT) に、元のファイルはもうなく、新しい中身は一時ファイルの名前のまま、という状態に
// なることがあり、失敗したら元のままという約束を守れない。MoveFileExW の置き換えは、同じボリュームの中では 1 回の
// 名前の変更で、元のファイルか新しいファイルのどちらかしか見えない。ReplaceFileW なら元のファイルの権限と属性が
// 残るが、MoveFileExW では残らない。一時ファイルは同じフォルダーに作るので、新しいファイルの権限は、そのフォルダーに
// 新しく作ったファイルと同じになる (session.json は、名前を変える前に一時ファイルに権限を付ける = write_session_file)。
//
// 一時的な失敗は、少し待ってやり直す。ウイルス対策のソフトウェアなどが、作ったばかりのファイルを一瞬開くことがあり、
// そのときの失敗は共有違反かアクセス拒否になる。1 回の失敗であきらめると、利用者の操作が理由なく失敗したように
// 見える。回数と間隔は目安の値で、測って決めたものではない (合計の待ちは 100 ms 以下で、ドックのポーリングの間隔
// 500 ms より短い)。
bool replace_from_temp_w(const std::wstring &temp, const std::wstring &target)
{
	constexpr int kAttempts = 5;
	constexpr DWORD kRetryWaitMs = 20;
	for (int attempt = 1;; attempt++) {
		if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		const DWORD error = GetLastError();
		const bool transient =
			error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
		if (!transient || attempt >= kAttempts)
			break;
		Sleep(kRetryWaitMs);
	}
	DeleteFileW(temp.c_str());
	return false;
}

// 同じフォルダーの一時ファイルに fill で書き終えてから、target と置き換える (write_file_atomic と session.json が
// 共有する)。before_replace は、名前を変える直前に一時ファイルに対して行う処理 (権限を付けるなど。空でもよい)。
// target がフォルダーのときは何も作らずに false を返す (置き換えのやり直しで待たせない)。
bool write_replace_w(const std::wstring &target, const std::function<bool(HANDLE)> &fill,
                     const std::function<void(const std::wstring &)> &before_replace = nullptr)
{
	if (target.empty() || is_directory_w(target))
		return false;
	const std::wstring temp = temp_path_for_w(target);
	if (!write_temp_w(temp, fill))
		return false;
	if (before_replace)
		before_replace(temp);
	return replace_from_temp_w(temp, target);
}

} // namespace

bool startup()
{
	WSADATA data{};
	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void shutdown()
{
	WSACleanup();
}

bool write_session_file(const SessionInfo &info)
{
	const auto path = session_file_path_w();
	if (!path)
		return false;

	const std::string text = build_session_json(info);

	// 一時ファイルに書き終えてから置き換える (送信側が、書いている途中の中身を読まないため)。ACL は名前を変える前に
	// 一時ファイルに付けるので、置き換わった時点から、この利用者だけが読める。広い権限のまま読める時間がない。前に
	// 別のプロセスが広い権限で作ったファイルも、置き換えで入れ替わる。ACL を付けられなくても、中身は書く
	// (session.json がないと、送信側が接続できない)。
	return write_replace_w(
		*path, [&text](HANDLE file) { return write_all(file, text.data(), text.size()); },
		[](const std::wstring &temp) { apply_owner_only_acl(temp); });
}

bool read_session_file(SessionInfo &out)
{
	const auto path = session_file_path_w();
	if (!path)
		return false;

	std::ifstream f(*path, std::ios::binary);
	if (!f.is_open())
		return false;
	std::ostringstream buf;
	buf << f.rdbuf();
	const std::string text = buf.str();

	SessionInfo parsed;
	std::string secret;
	int64_t port = 0, pid = 0, started_at = 0;
	if (!find_int_field(text, "port", port))
		return false;
	if (!find_string_field(text, "secret", secret))
		return false;
	if (!find_int_field(text, "pid", pid))
		return false;
	if (!find_int_field(text, "started_at", started_at))
		return false;
	// 型に収まらないポート番号と pid は、読めなかったものとする。切り詰めると、70000 を 4464、-1 を 65535 のような、
	// 別の番号として読んでしまう。
	if (port < 0 || port > 65535 || pid < 0 || pid > int64_t(UINT32_MAX))
		return false;

	parsed.port = uint16_t(port);
	parsed.secret_hex = secret;
	parsed.writer_pid = uint32_t(pid);
	parsed.writer_started_at = started_at;
	out = parsed;
	return true;
}

bool delete_session_file()
{
	const auto dir = local_data_dir_w();
	if (!dir)
		return false;
	// delete_file と同じく、もうないなら成功とする。
	if (DeleteFileW((*dir + L"\\session.json").c_str()))
		return true;
	return is_not_found_error(GetLastError());
}

ProcessIdentity current_process_identity()
{
	ProcessIdentity id;
	id.pid = GetCurrentProcessId();

	FILETIME creation{}, exit_time{}, kernel{}, user{};
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
		id.started_at = filetime_to_i64(creation);
	return id;
}

bool is_process_alive(uint32_t pid, int64_t started_at)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
	if (h == nullptr)
		return false; // 開けないのは、プロセスがないか権限がないときで、どちらも動いていないものとする

	// 先に、終了しているかを確認する。終了したプロセスでも、どこかがハンドルを持っている間は OpenProcess が成功し、
	// GetProcessTimes も起動の時刻を返す。起動の時刻だけで比べると、強制終了された OBS が残した session.json の
	// プロセスを動いているものとみなし、次に起動した OBS が待ち受けを始めない (送信側が接続できない)。終了コードが
	// STILL_ACTIVE のときだけ、動いているものとする。
	DWORD exit_code = 0;
	const BOOL got_exit = GetExitCodeProcess(h, &exit_code);
	FILETIME creation{}, exit_time{}, kernel{}, user{};
	const BOOL ok = GetProcessTimes(h, &creation, &exit_time, &kernel, &user);
	CloseHandle(h);
	if (!got_exit || exit_code != STILL_ACTIVE || !ok)
		return false;

	// pid が再利用された別のプロセスと見分けるため、起動の時刻が session.json の値と同じときだけ、同じプロセスとする。
	return filetime_to_i64(creation) == started_at;
}

int64_t os_process_time_to_epoch_us(int64_t os_process_time)
{
	// 変換は無名名前空間の os_time_to_epoch_us の 1 か所に置き、ここから呼ぶ (理由は platform/platform.h のコメント)。
	return os_time_to_epoch_us(os_process_time);
}

int64_t current_process_started_at_us()
{
	FILETIME creation{}, exit_time{}, kernel{}, user{};
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
		return 0;
	return os_time_to_epoch_us(filetime_to_i64(creation));
}

namespace {

// Vesktop の実行ファイルの名前。この名前は、この 1 か所にだけ書く (find_vesktop_process と
// is_discord_client_exe_name が使う)。
constexpr wchar_t kVesktopImageName[] = L"Vesktop.exe";

} // namespace

namespace {

// process のトークンの利用者の SID を、SID そのもののバイト列で返す。トークンを開けないとき、読めないときは空を返す。
std::vector<uint8_t> process_user_sid_bytes(HANDLE process)
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(process, TOKEN_QUERY, &token))
		return {};

	DWORD needed = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
	if (needed == 0) {
		CloseHandle(token);
		return {};
	}
	std::vector<uint8_t> buf(needed);
	const BOOL ok = GetTokenInformation(token, TokenUser, buf.data(), needed, &needed);
	CloseHandle(token);
	if (!ok)
		return {};

	PSID sid = reinterpret_cast<const TOKEN_USER *>(buf.data())->User.Sid;
	if (sid == nullptr || !IsValidSid(sid))
		return {};
	const auto *first = static_cast<const uint8_t *>(sid);
	return std::vector<uint8_t>(first, first + GetLengthSid(sid));
}

// このプロセスの利用者とセッション。find_vesktop_process は、1 回の探索の初めに 1 回だけ読む。
struct CurrentUser {
	std::vector<uint8_t> sid;
	bool has_session = false;
	DWORD session = 0;
};

CurrentUser read_current_user()
{
	CurrentUser user;
	user.sid = process_user_sid_bytes(GetCurrentProcess());
	user.has_session = ProcessIdToSessionId(GetCurrentProcessId(), &user.session) != 0;
	return user;
}

// pid のプロセス (process はそのハンドル。開けなければ nullptr) を、同じ利用者のものとして数えるか。
bool counts_for_current_user(const CurrentUser &user, HANDLE process, DWORD pid)
{
	const std::vector<uint8_t> other_sid =
		process != nullptr ? process_user_sid_bytes(process) : std::vector<uint8_t>();
	DWORD session = 0;
	const bool same_session =
		user.has_session && ProcessIdToSessionId(pid, &session) != 0 && session == user.session;
	return counts_as_same_user(user.sid, other_sid, same_session);
}

} // namespace

bool counts_as_same_user(const std::vector<uint8_t> &own_sid, const std::vector<uint8_t> &other_sid, bool same_session)
{
	if (own_sid.empty())
		return true;
	if (!other_sid.empty())
		return own_sid == other_sid;
	return same_session;
}

bool is_process_of_current_user(uint32_t pid)
{
	const CurrentUser user = read_current_user();
	const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	const bool counts = counts_for_current_user(user, process, pid);
	if (process != nullptr)
		CloseHandle(process);
	return counts;
}

namespace {

// ポータブル版の Vesktop のプロセスを 1 度でも見たか (この OBS のプロセスの間)。
std::atomic<bool> g_portable_vesktop_seen{false};

// Vesktop が動いていなければ、ポータブル版かは分からない。動いているものを見つけたときに分かったことを残す。
// このプロセスの実行ファイルを調べ、Vesktop (v1.6.9 の src/main/constants.ts の PORTABLE) と同じ規則で決める。
// exe の隣に "Uninstall Vesktop.exe" がなければポータブル版 (electron.exe で動く開発用の起動は除く)。
// 実行ファイルの場所を読めないときは、ポータブル版とは言わない (誤って書き込みを止めない)。
bool portable_layout_w(const std::wstring &exe_path)
{
	const std::size_t cut = exe_path.find_last_of(L"\\/");
	if (cut == std::wstring::npos)
		return false;
	std::wstring name = exe_path.substr(cut + 1);
	std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
	if (name == L"electron.exe")
		return false;
	const std::wstring uninstaller = exe_path.substr(0, cut) + L"\\Uninstall Vesktop.exe";
	if (GetFileAttributesW(uninstaller.c_str()) != INVALID_FILE_ATTRIBUTES)
		return false;
	// ないと分かったときだけポータブル版とする (権限がなくて調べられないときは、そうとは言わない)。
	const DWORD err = GetLastError();
	return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

bool vesktop_process_is_portable(HANDLE process)
{
	std::vector<wchar_t> buf(32768);
	DWORD len = DWORD(buf.size());
	if (!QueryFullProcessImageNameW(process, 0, buf.data(), &len) || len == 0)
		return false;
	return portable_layout_w(std::wstring(buf.data(), len));
}

// find_vesktop_process と vesktop_running_now の本体。listed には、プロセスの一覧を読めたかを入れる (読めなければ、
// Vesktop が動いていないとは言えない)。
bool scan_vesktop_processes(VesktopProcessInfo &out, bool &listed)
{
	listed = false;
	// 探すのは Vesktop の実行ファイルの名前で、OBS と同じ利用者のプロセスだけを数える。
	const CurrentUser user = read_current_user();

	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return false;

	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);

	bool found = false;
	bool any_portable = false;
	VesktopProcessInfo best; // いちばん早く起動したもの (理由は platform/platform.h の find_vesktop_process のコメント)
	bool best_has_time = false;

	if (Process32FirstW(snapshot, &entry)) {
		listed = true;
		do {
			if (_wcsicmp(entry.szExeFile, kVesktopImageName) != 0)
				continue;

			// 開けないプロセスも、同じセッションなら数える (counts_as_same_user)。そのときは起動の時刻が分からない。
			const HANDLE process =
				OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (!counts_for_current_user(user, process, entry.th32ProcessID)) {
				if (process != nullptr)
					CloseHandle(process);
				continue;
			}

			found = true;

			int64_t started_at_us = 0;
			bool has_time = false;
			if (process != nullptr) {
				if (vesktop_process_is_portable(process))
					any_portable = true;
				FILETIME creation{}, exit_time{}, kernel{}, user_time{};
				if (GetProcessTimes(process, &creation, &exit_time, &kernel, &user_time)) {
					started_at_us = os_time_to_epoch_us(filetime_to_i64(creation));
					has_time = started_at_us > 0;
				}
				CloseHandle(process);
			}

			// 起動の時刻が読めた中で、いちばん早いものを選ぶ。1 つも読めなければ、最初に見つけた pid だけを返す
			// (started_at_us は 0 で、分からないことを示す)。
			if (has_time) {
				if (!best_has_time || started_at_us < best.started_at_us) {
					best.pid = entry.th32ProcessID;
					best.started_at_us = started_at_us;
					best_has_time = true;
				}
			} else if (!best_has_time && best.pid == 0) {
				best.pid = entry.th32ProcessID;
				best.started_at_us = 0;
			}
		} while (Process32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);
	if (!found)
		return false;
	best.portable = any_portable;
	if (any_portable)
		g_portable_vesktop_seen.store(true);
	out = best;
	return true;
}

} // namespace

bool vesktop_exe_is_portable(const std::string &exe_path_utf8)
{
	const auto wide = utf8_to_wide(exe_path_utf8);
	return wide.has_value() && portable_layout_w(*wide);
}

bool vesktop_portable_seen()
{
	return g_portable_vesktop_seen.load();
}

bool find_vesktop_process(VesktopProcessInfo &out)
{
	bool listed = false;
	return scan_vesktop_processes(out, listed);
}

std::optional<bool> vesktop_running_now()
{
	VesktopProcessInfo info;
	bool listed = false;
	const bool found = scan_vesktop_processes(info, listed);
	if (found)
		return true;
	if (!listed)
		return std::nullopt;
	return false;
}

bool is_discord_client_exe_name(const std::string &name_utf8)
{
	// Discord のデスクトップクライアントは、安定版、PTB、Canary で実行ファイルの名前が違う。
	static const wchar_t *const kNames[] = {kVesktopImageName, L"Discord.exe", L"DiscordPTB.exe",
						L"DiscordCanary.exe"};
	const auto wide = utf8_to_wide(name_utf8);
	if (!wide || wide->empty())
		return false;
	for (const wchar_t *name : kNames) {
		if (_wcsicmp(wide->c_str(), name) == 0)
			return true;
	}
	return false;
}

bool vesktop_data_dir(std::string &out)
{
	const auto dir = vesktop_data_dir_w();
	if (!dir)
		return false;
	std::string utf8 = wide_to_utf8(*dir);
	if (utf8.empty())
		return false;
	out = std::move(utf8);
	return true;
}

bool vesktop_state_json_path(std::string &out)
{
	std::string dir;
	if (!vesktop_data_dir(dir))
		return false;
	out = dir + "\\state.json";
	return true;
}

bool vesktop_settings_json_path(std::string &out)
{
	std::string dir;
	if (!vesktop_data_dir(dir))
		return false;
	out = dir + "\\settings\\settings.json";
	return true;
}

std::string vesktop_settings_json_beside(const std::string &state_json_path)
{
	const std::size_t cut = state_json_path.find_last_of("\\/");
	if (cut == std::string::npos || cut == 0)
		return std::string();
	return state_json_path.substr(0, cut) + "\\settings\\settings.json";
}

bool read_file_bytes(const std::string &path_utf8, std::string &out, std::size_t max_bytes)
{
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide)
		return false;

	std::ifstream f(*wide, std::ios::binary | std::ios::ate);
	if (!f.is_open())
		return false;

	const std::streamoff size = f.tellg();
	if (size < 0)
		return false;
	if (std::uint64_t(size) > std::uint64_t(max_bytes))
		return false; // 大きすぎるファイルは読まない (platform/platform.h の read_file_bytes のコメント)

	std::string text(size_t(size), '\0');
	f.seekg(0, std::ios::beg);
	if (size > 0)
		f.read(text.data(), size);
	if (f.bad())
		return false;
	text.resize(size_t(f.gcount()));
	out = std::move(text);
	return true;
}

bool write_file_bytes(const std::string &path_utf8, const std::string &text)
{
	// state.json、バックアップ、設定のファイルを書くコードはこの関数を呼ぶ。どれも一時ファイルと置き換えて書く
	// (write_file_atomic) ので、書いている途中で失敗しても、元のファイルは壊れない。
	return write_file_atomic(path_utf8, text);
}

// ---- 録画の書き込みと録画ファイルの作成 ----

bool make_directories(const std::string &path_utf8)
{
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return false;

	const std::wstring path = normalize_dir_path_w(*wide);
	const size_t root = root_length_w(path);
	if (root >= path.size())
		return true; // ルート (ドライブや共有) だけなので、作るものがない

	for (size_t pos = root;;) {
		const size_t next = path.find(L'\\', pos + 1);
		const std::wstring prefix = (next == std::wstring::npos) ? path : path.substr(0, next);
		if (prefix.size() > root && !CreateDirectoryW(prefix.c_str(), nullptr)) {
			if (GetLastError() != ERROR_ALREADY_EXISTS)
				return false;
		}
		if (next == std::wstring::npos)
			return true;
		pos = next;
	}
}

bool make_new_directory(const std::string &path_utf8, bool *already_exists)
{
	if (already_exists != nullptr)
		*already_exists = false;
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return false;
	const std::wstring path = normalize_dir_path_w(*wide);
	if (root_length_w(path) >= path.size())
		return false; // ルート (ドライブや共有) だけなので、作るものがない
	if (CreateDirectoryW(path.c_str(), nullptr))
		return true;
	if (already_exists != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
		*already_exists = true;
	return false;
}

FileHandle open_file(const std::string &path_utf8)
{
	// 場所の文字列は UTF-16 に変えてから開く (char の場所はアクティブコードページの文字列として読まれ、ASCII でない
	// 保存先を開けない。platform/platform.h の write_file_bytes のコメント)。
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return 0;

	const HANDLE file = CreateFileW(wide->c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
	                                FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	return FileHandle(reinterpret_cast<uintptr_t>(file));
}

bool append(FileHandle file, const void *data, std::size_t len)
{
	if (file == 0)
		return false;
	if (len == 0)
		return true; // 書くものがないのは失敗ではない
	if (data == nullptr)
		return false;

	const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(file));
	return write_all(handle, data, len); // 64 MiB ずつに分けて書く (無名名前空間の write_all)
}

void close(FileHandle file)
{
	if (file == 0)
		return;
	CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(file)));
}

// 録画のセッションが使用中であることを示すロックファイル (使い方は record/recorder.cpp の take_session_lock と、
// record/recorder.h の resume_pending のコメント)。共有を許さずに (共有の指定を 0 にして) 開き、閉じたら削除する
// 指定 (FILE_FLAG_DELETE_ON_CLOSE) を付ける。保持しているプロセスがクラッシュしても OS がハンドルを閉じるので、
// ファイルは残らず、ほかのプロセスが開けるようになる。保存先がネットワークの共有でも、共有の側がハンドルが閉じた
// ことを知る。ただし遅れることがあり、電源が落ちた PC のハンドルは、共有の側が接続の切断に気付くまで残る (SMB の
// ハンドルの種類によっては、その後もしばらく残る)。電源が落ちたときだけファイルが残るが、保持しているプロセスが
// いないので次は開け、lock_file_held は false を返す。
// 共有違反以外の理由で拒否されたとき (削除を許さない保存先や、読み取り専用にされた古いロックファイル) は、削除の
// 指定と DELETE を外し、共有を許さない開き方のまま開き直す。このときは閉じてもファイルが残るが、残ったファイルは、
// 電源が落ちたときの残りと同じく、だれも保持していないと判定される。
FileHandle hold_lock_file(const std::string &path_utf8, bool *held_elsewhere)
{
	if (held_elsewhere != nullptr)
		*held_elsewhere = false;
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return 0;
	HANDLE file = CreateFileW(wide->c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_ALWAYS,
	                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_SHARING_VIOLATION)
		file = CreateFileW(wide->c_str(), GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		if (held_elsewhere != nullptr && GetLastError() == ERROR_SHARING_VIOLATION)
			*held_elsewhere = true;
		return 0;
	}
	return FileHandle(reinterpret_cast<uintptr_t>(file));
}

bool lock_file_held(const std::string &path_utf8)
{
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return false;
	// 読み取りで、共有を許さずに、あるときだけ開く (作らない)。だれかが共有を許さずに保持していれば、共有違反で
	// 開けない。削除の指定は付けない (ここは調べるだけで、ファイルを削除しない)。
	const HANDLE file =
		CreateFileW(wide->c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_SHARING_VIOLATION;
	CloseHandle(file);
	return false;
}

std::optional<std::uint64_t> free_space_bytes(const std::string &path_utf8)
{
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return std::nullopt;

	// まだ作っていないフォルダーを渡されることがある (録画を始める前の見積もり) ので、読めなければ 1 つ上の
	// フォルダーで調べる。同じボリュームなら、空き容量は変わらない。
	std::wstring dir = normalize_dir_path_w(*wide);
	for (int depth = 0; depth < 64 && !dir.empty(); depth++) {
		ULARGE_INTEGER available{};
		// この利用者が使える空き容量 (クォータがあるときは、その制限が反映される)。
		if (GetDiskFreeSpaceExW(dir.c_str(), &available, nullptr, nullptr))
			return std::uint64_t(available.QuadPart);

		const size_t cut = dir.find_last_of(L'\\');
		if (cut == std::wstring::npos)
			break;
		dir.erase(cut);
		if (!dir.empty() && dir.back() == L':')
			dir.push_back(L'\\'); // ルートは "C:" ではなく "C:\" と書く
	}
	return std::nullopt;
}

bool is_absolute_path(const std::string &path_utf8)
{
	if (path_utf8.empty())
		return false;
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide)
		return false;
	// ドライブ名の後に区切りがない `C:rec` と `C:` は、そのドライブの作業フォルダーからの相対の場所である (OBS の
	// 作業フォルダーの下を指すことになる)。root_length_w は 2 文字目が `:` なら 2 を返すので、ここで先に false にする。
	// 確認しないと、保存先の入力欄の `C:rec` を絶対パスとして受け入れ、OBS の bin の下に録画を書いてしまう。区切りの
	// 有無は正規化の前に見る (`C:\` は正規化で `C:` になる)。
	const std::wstring &w = *wide;
	if (w.size() >= 2 && w[1] == L':' && (w.size() == 2 || (w[2] != L'\\' && w[2] != L'/')))
		return false;
	// 残りは、ルートの長さが 0 より大きいかで決める (フォルダーを作る関数と同じ root_length_w で判断する)。相対の場所
	// と、ルートを持たない "\\foo" のような場所では 0 になる。
	return root_length_w(normalize_dir_path_w(w)) > 0;
}

std::string absolute_path(const std::string &path_utf8)
{
	if (path_utf8.empty() || is_absolute_path(path_utf8))
		return path_utf8;
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return path_utf8;
	// GetFullPathNameW は、相対の場所をプロセスの作業フォルダーと結び、`.` と `..` を解き、区切りを `\` にそろえる。
	const DWORD needed = GetFullPathNameW(wide->c_str(), 0, nullptr, nullptr);
	if (needed == 0)
		return path_utf8;
	std::wstring full(needed, L'\0');
	const DWORD written = GetFullPathNameW(wide->c_str(), needed, full.data(), nullptr);
	if (written == 0 || written >= needed)
		return path_utf8;
	full.resize(written);
	return wide_to_utf8(full);
}

std::string build_child_command_line(const std::string &program_utf8, const std::vector<std::string> &args)
{
	// 先頭 (program) は常に引用符で囲む。囲まないと、空白を含むフォルダーで別の引数に分かれる。
	std::string out = "\"" + program_utf8 + "\"";
	for (const std::string &arg : args) {
		out.push_back(' ');
		out += quote_argument(arg);
	}
	return out;
}

ChildHandle start_child(const std::string &program_utf8, const std::vector<std::string> &args,
                        const std::string &output_path_utf8)
{
	const auto program_w = utf8_to_wide(program_utf8);
	if (!program_w || program_w->empty())
		return 0;
	const auto command_w = utf8_to_wide(build_child_command_line(program_utf8, args));
	if (!command_w)
		return 0;

	// CreateProcessW のコマンドラインは書き換えられる場所でなければならないので、コピーを渡す。
	std::vector<wchar_t> command_buf(command_w->begin(), command_w->end());
	command_buf.push_back(L'\0');

	// 出力先のファイルを開けなくても、子プロセスは起動する。出力は失敗の原因を調べるためのもので、録画ファイルの作成に必須では
	// ない。ここで戻ると、フォルダーがまだないとき、読み取り専用のとき、ほかのツールがファイルを開いているときに、
	// 録画ファイルの作成が 1 つも動かなくなる。開けなければ、出力先なしで起動する。
	HANDLE output = INVALID_HANDLE_VALUE;
	if (!output_path_utf8.empty()) {
		const auto output_w = utf8_to_wide(output_path_utf8);
		if (output_w && !output_w->empty()) {
			SECURITY_ATTRIBUTES inheritable{};
			inheritable.nLength = sizeof(inheritable);
			inheritable.bInheritHandle = TRUE;
			output = CreateFileW(output_w->c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
			                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		}
	}

	STARTUPINFOEXW startup_info{};
	startup_info.StartupInfo.cb = sizeof(startup_info);
	std::vector<uint8_t> attribute_buf;
	DWORD flags = CREATE_NO_WINDOW; // コンソールのウィンドウを表示しない
	BOOL inherit = FALSE;

	if (output != INVALID_HANDLE_VALUE) {
		// 子プロセスに継承させるハンドルは、出力先のファイルの 1 つだけにする (PROC_THREAD_ATTRIBUTE_HANDLE_LIST)。
		// 単に継承を許すと、OBS が持つ継承できるハンドル (待ち受けのソケットを含む) が、子プロセスが動いている間ずっと
		// 子プロセスにも残り、録画ファイルの作成の数十秒の間、閉じられなくなる。
		SIZE_T attribute_size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
		attribute_buf.resize(attribute_size);
		auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buf.data());
		HANDLE inherited[1] = {output};
		if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
			CloseHandle(output);
			return 0;
		}
		if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
		                               sizeof(inherited), nullptr, nullptr)) {
			DeleteProcThreadAttributeList(attributes);
			CloseHandle(output);
			return 0;
		}
		startup_info.lpAttributeList = attributes;
		startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup_info.StartupInfo.hStdInput = nullptr;
		startup_info.StartupInfo.hStdOutput = output;
		startup_info.StartupInfo.hStdError = output;
		flags |= EXTENDED_STARTUPINFO_PRESENT;
		inherit = TRUE;
	}

	PROCESS_INFORMATION process_info{};
	const BOOL started = CreateProcessW(program_w->c_str(), command_buf.data(), nullptr, nullptr, inherit, flags,
	                                    nullptr, nullptr, &startup_info.StartupInfo, &process_info);

	if (startup_info.lpAttributeList != nullptr)
		DeleteProcThreadAttributeList(startup_info.lpAttributeList);
	if (output != INVALID_HANDLE_VALUE)
		CloseHandle(output); // OBS の側のハンドルは渡した後すぐに閉じる (子プロセスが閉じれば、ファイルが解放される)
	if (!started)
		return 0;

	CloseHandle(process_info.hThread); // 使わない (待つのはプロセスのハンドル)
	return register_child(process_info.hProcess);
}

cordmux::record::ChildWaitResult wait_child(ChildHandle child, int timeout_ms)
{
	cordmux::record::ChildWaitResult result;
	result.outcome = cordmux::record::ChildWait::Failed;

	const HANDLE process = find_child(child);
	if (process == nullptr)
		return result; // 知らない番号か、既に終わって表から外した番号

	const DWORD waited = WaitForSingleObject(process, timeout_ms <= 0 ? 0 : DWORD(timeout_ms));
	if (waited == WAIT_TIMEOUT) {
		result.outcome = cordmux::record::ChildWait::Timeout;
		return result;
	}
	if (waited != WAIT_OBJECT_0)
		return result;

	DWORD exit_code = 0;
	// 終わったことが分かってから読む (動いている間は STILL_ACTIVE が返り、実際の終了コードと区別できない)。
	if (!GetExitCodeProcess(process, &exit_code))
		return result;

	result.outcome = cordmux::record::ChildWait::Exited;
	result.exit_code = int(exit_code);
	forget_child(child);
	return result;
}

void kill_child(ChildHandle child)
{
	const HANDLE process = find_child(child);
	if (process == nullptr)
		return;

	TerminateProcess(process, 1);
	// 止まるまで待つ。TerminateProcess は終わるのを待たずに戻るので、すぐに作りかけの録画ファイルを削除しようと
	// しても、子プロセスがまだファイルを開いていて削除できない。待つ時間の上限は目安の値で、止まらない子プロセスで
	// ここから戻れなくならないためのものである。
	constexpr DWORD kKillWaitMs = 5000;
	WaitForSingleObject(process, kKillWaitMs);
	forget_child(child);
}

std::int64_t now_steady_us()
{
	return std::int64_t(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

std::int64_t now_wall_us()
{
	return std::int64_t(std::chrono::duration_cast<std::chrono::microseconds>(
				    std::chrono::system_clock::now().time_since_epoch())
				    .count());
}

int utc_offset_sec_now()
{
	// 現在の UTC を現地の時刻に変え、その差を求める (夏時間の差も入る)。GetTimeZoneInformation の Bias を足し合わせる
	// より、OS の変換をそのまま使うほうが、OS の表示と食い違わない。
	SYSTEMTIME utc = {};
	GetSystemTime(&utc);
	SYSTEMTIME local = {};
	if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
		return 0;
	FILETIME utc_ft = {};
	FILETIME local_ft = {};
	if (!SystemTimeToFileTime(&utc, &utc_ft) || !SystemTimeToFileTime(&local, &local_ft))
		return 0;
	ULARGE_INTEGER a;
	a.LowPart = utc_ft.dwLowDateTime;
	a.HighPart = utc_ft.dwHighDateTime;
	ULARGE_INTEGER b;
	b.LowPart = local_ft.dwLowDateTime;
	b.HighPart = local_ft.dwHighDateTime;
	const long long diff_100ns = static_cast<long long>(b.QuadPart) - static_cast<long long>(a.QuadPart);
	return static_cast<int>(diff_100ns / 10000000LL);
}

std::string ffmpeg_path()
{
	// 同梱の FFmpeg の実行ファイルの名前。この名前は、この 1 か所にだけ書く (find_vesktop_process の Vesktop.exe と
	// 同じ扱い)。
	static const wchar_t kFfmpegExeName[] = L"ffmpeg.exe";

	// cordmux.dll と同じフォルダーを見る。実行ファイル (obs64.exe) のフォルダーではなく DLL のフォルダーを見るのは、
	// 同梱のファイルを DLL と同じフォルダーに置くためである。調べ方は module_dir と共有する。
	const auto dir = module_dir_w();
	if (!dir)
		return std::string();

	const std::wstring candidate = *dir + L"\\" + kFfmpegExeName;
	const DWORD attributes = GetFileAttributesW(candidate.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return std::string(); // なければ空を返す (platform/platform.h の ffmpeg_path のコメント)
	return wide_to_utf8(candidate);
}

std::string videos_dir()
{
	PWSTR path = nullptr;
	const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path);
	if (FAILED(hr) || path == nullptr) {
		if (path)
			CoTaskMemFree(path);
		return std::string();
	}
	std::wstring result(path);
	CoTaskMemFree(path);
	return wide_to_utf8(result);
}

// ---- インストール、更新、アンインストールで使うファイルの操作 ----
//
// 削除、コピー、置き換え、一時ファイルを使った書き込みは、どれも、同じフォルダーの一時ファイルを使うことと、もう
// なければ成功とすることの 2 つの約束でそろえてある (platform/platform.h の、インストールで使うファイルの操作の
// コメント)。

bool delete_file(const std::string &path_utf8)
{
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return false;

	const DWORD attributes = GetFileAttributesW(wide->c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return is_not_found_error(GetLastError()); // もうないなら成功 (やり直しても同じ結果になる)
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return false; // フォルダーは削除しない (フォルダーの中身まで削除してしまわないため)

	// 読み取り専用の属性が付いたファイルは、削除できずに false を返す (利用者かほかのツールが付けた属性を、メッセージも
	// ログも出さずに外さない。アンインストールは、削除できなかったファイルとして表示する)。
	if (DeleteFileW(wide->c_str()))
		return true;
	return is_not_found_error(GetLastError());
}

bool remove_directory_if_empty(const std::string &path_utf8)
{
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return false;

	const DWORD attributes = GetFileAttributesW(wide->c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return is_not_found_error(GetLastError());
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return false; // ファイルは削除しない (delete_file の役目)

	// 中身が残っていれば OS が断る (ERROR_DIR_NOT_EMPTY)。フォルダーを指すジャンクションとシンボリックリンクは、
	// リンクだけが消え、指している先には触らない。
	if (RemoveDirectoryW(wide->c_str()))
		return true;
	return is_not_found_error(GetLastError());
}

bool list_directory(const std::string &path_utf8, std::vector<DirEntry> &out)
{
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return false;
	if (!is_directory_w(*wide))
		return false; // ないか、ファイルである (out は変えない)

	const std::wstring pattern = normalize_dir_path_w(*wide) + L"\\*";
	WIN32_FIND_DATAW data{};
	const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
	                                     FIND_FIRST_EX_LARGE_FETCH);
	std::vector<DirEntry> entries;
	if (find == INVALID_HANDLE_VALUE) {
		if (GetLastError() != ERROR_FILE_NOT_FOUND)
			return false;
		// 1 つもない (ドライブのルートの空のフォルダーは、この形で返る)
	} else {
		bool ok = true;
		do {
			const std::wstring name(data.cFileName);
			if (name == L"." || name == L"..")
				continue;
			DirEntry entry;
			entry.name = wide_to_utf8(name);
			entry.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			// is_link は、再解析ポイントの種類がシンボリックリンクかジャンクションのときだけ true にする。クラウドの同期の
			// ファイル (OneDrive の、中身をダウンロードしていないファイルなど) も同じ属性を持つが、普通のファイルとして
			// 扱う (削除してよく、中に入っても外には出ない)。
			entry.is_link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
			                (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
			                 data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
			entries.push_back(std::move(entry));
		} while (FindNextFileW(find, &data));
		if (GetLastError() != ERROR_NO_MORE_FILES)
			ok = false; // 途中で読めなくなった (途中までの一覧を返さない)
		FindClose(find);
		if (!ok)
			return false;
	}

	// 並びは名前の UTF-8 のバイト順にする (理由は platform/platform.h の list_directory のコメント)。
	std::sort(entries.begin(), entries.end(),
	          [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
	out.swap(entries);
	return true;
}

bool copy_file_replace(const std::string &from_utf8, const std::string &to_utf8)
{
	const auto from_w = utf8_to_wide(from_utf8);
	const auto to_w = utf8_to_wide(to_utf8);
	if (!from_w || from_w->empty() || !to_w || to_w->empty())
		return false;
	if (is_directory_w(*to_w))
		return false; // フォルダーをコピーで置き換えない (置き換えのやり直しで待たせない)

	// コピーするのは中身のバイトだけである。CopyFileW を使わないのは、属性 (読み取り専用) と代替データストリーム
	// (ダウンロード元を示す Zone.Identifier など) までコピーするためである。コピー元に読み取り専用が付いていると、
	// コピー先 (data/vencord) も読み取り専用になり、次にインストールし直すときの置き換えが拒否される。中身だけをコピー
	// すれば、置き換えた後の属性は、write_file_atomic と同じく、そのフォルダーに新しく作ったファイルと同じになる。
	// コピー元は、ほかのプロセスが書き込めないように開く (コピーしている間に中身が変わると、新しい中身と古い中身が
	// 混ざったコピーになる)。
	const HANDLE source = CreateFileW(from_w->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (source == INVALID_HANDLE_VALUE)
		return false; // ないか、フォルダーか、開けない

	std::vector<uint8_t> buf(1u * 1024u * 1024u);
	const auto fill = [source, &buf](HANDLE destination) {
		for (;;) {
			DWORD got = 0;
			if (!ReadFile(source, buf.data(), DWORD(buf.size()), &got, nullptr))
				return false;
			if (got == 0)
				return true; // 最後まで読んだ
			if (!write_all(destination, buf.data(), got))
				return false;
		}
	};

	// 一時ファイルを書き終えたら、コピー元を閉じてから置き換える (from と to が同じ場所のとき、コピー元を開いたままだと
	// 置き換えが共有違反で拒否されるため)。
	const std::wstring temp = temp_path_for_w(*to_w);
	const bool written = write_temp_w(temp, fill);
	CloseHandle(source);
	if (!written)
		return false;
	return replace_from_temp_w(temp, *to_w);
}

bool write_file_atomic(const std::string &path_utf8, const std::string &text)
{
	// 場所の文字列は UTF-16 に変えてから開く (char の場所はアクティブコードページの文字列として読まれ、ASCII でない
	// 場所を開けない。platform/platform.h の write_file_bytes のコメント)。
	const auto wide = utf8_to_wide(path_utf8);
	if (!wide || wide->empty())
		return false;
	return write_replace_w(*wide, [&text](HANDLE file) { return write_all(file, text.data(), text.size()); });
}

std::string module_dir()
{
	const auto dir = module_dir_w();
	return dir ? wide_to_utf8(*dir) : std::string();
}

std::string local_data_dir()
{
	// 場所を返すだけで、フォルダーは作らない (理由は local_data_dir_w のコメント)。
	const auto dir = local_data_dir_w();
	return dir ? wide_to_utf8(*dir) : std::string();
}

std::string user_profile_dir()
{
	const auto dir = profile_dir_w(L"USERPROFILE", FOLDERID_Profile);
	return dir ? wide_to_utf8(*dir) : std::string();
}

std::string os_version_text()
{
	// バージョンは RtlGetVersion で読む。GetVersionEx は、アプリケーションのマニフェストと互換モードによって、実際とは
	// 違うバージョンを返すので使わない。RtlGetVersion は SDK のヘッダーで宣言されていないので、GetProcAddress で探す。
	using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr)
		return std::string();
	const auto get_version = reinterpret_cast<RtlGetVersionFn>(
		reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")));
	if (get_version == nullptr)
		return std::string();

	OSVERSIONINFOW info{};
	info.dwOSVersionInfoSize = sizeof(info);
	if (get_version(&info) != 0 || (info.dwMajorVersion == 0 && info.dwMinorVersion == 0 && info.dwBuildNumber == 0))
		return std::string(); // 読めなかった

	// 4 つ目の数 (更新の番号) は RtlGetVersion では読めないので、レジストリの UBR から読む。読めなければ 0 にする
	// (3 つ目までは正しいので、全体を捨てない)。
	DWORD revision = 0;
	DWORD revision_size = sizeof(revision);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
	                 RRF_RT_REG_DWORD, nullptr, &revision, &revision_size) != ERROR_SUCCESS)
		revision = 0;

	return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
	       std::to_string(info.dwBuildNumber) + "." + std::to_string(revision);
}

// ---- ファイルの場所をエクスプローラーで開く ----

namespace {

// フォルダーとして Windows の API に渡す形。`C:` はそのドライブの作業フォルダーを指すので、ルートだけは `C:\` にする。
std::wstring as_folder_w(const std::wstring &path)
{
	if (path.size() == 2 && path[1] == L':')
		return path + L"\\";
	return path;
}

// その場所がいまあるか (ファイルでもフォルダーでもよい)。
bool exists_w(const std::wstring &path)
{
	return GetFileAttributesW(as_folder_w(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

bool resolve_file_location(const std::string &path_utf8, FileLocationTarget &out)
{
	const auto wide = utf8_to_wide(path_utf8);
	// 相対の場所と `C:foo` (ドライブだけが付いた相対の場所) は扱わない (platform/platform.h のコメント)。どちらも
	// is_absolute_path で確認する。
	if (!wide || wide->empty() || !is_absolute_path(path_utf8))
		return false;

	// `.` と `..` を正規化し、区切りを `\` にそろえる (親のフォルダーを文字列でたどるので、正規化しないと `..` の 1 つ
	// 上を親と読んでしまう)。
	const DWORD needed = GetFullPathNameW(wide->c_str(), 0, nullptr, nullptr);
	if (needed == 0)
		return false;
	std::wstring full(needed, L'\0');
	const DWORD written = GetFullPathNameW(wide->c_str(), needed, full.data(), nullptr);
	if (written == 0 || written >= needed)
		return false;
	full.resize(written);
	full = normalize_dir_path_w(full); // 末尾の区切りを取り除く (`C:\` は `C:` になり、as_folder_w が戻す)

	const size_t root = root_length_w(full);
	if (root == 0)
		return false;

	// 1 つ上のフォルダー。ルートより上には行かない。
	const auto parent_of = [root](const std::wstring &path) {
		const size_t cut = path.rfind(L'\\');
		if (cut == std::wstring::npos || cut < root)
			return path.substr(0, root);
		return path.substr(0, cut);
	};

	if (exists_w(full)) {
		if (full.size() <= root) {
			// ドライブか共有のルート。親がないので、ルートを開く。
			out.folder = wide_to_utf8(as_folder_w(full));
			out.item.clear();
			return true;
		}
		out.folder = wide_to_utf8(as_folder_w(parent_of(full)));
		out.item = wide_to_utf8(full);
		return true;
	}

	// もうない。存在するフォルダーのうち、いちばん近い親を開く ([フォルダーを開く] の dock/ui/dock_ui_calc.h の
	// nearest_folder と同じ規則)。
	std::wstring cur = full;
	while (cur.size() > root) {
		cur = parent_of(cur);
		if (is_directory_w(as_folder_w(cur))) {
			out.folder = wide_to_utf8(as_folder_w(cur));
			out.item.clear();
			return true;
		}
	}
	return false; // ルートまでさかのぼっても、存在するフォルダーがない (外したドライブや、切断された共有)
}

bool open_file_location(const std::string &path_utf8)
{
	FileLocationTarget target;
	if (!resolve_file_location(path_utf8, target))
		return false;
	const auto folder = utf8_to_wide(target.folder);
	const auto item = utf8_to_wide(target.item);
	if (!folder || !item)
		return false;

	// シェルの API は COM の初期化を必要とする。UI のスレッドでは Qt が既に初期化している (S_FALSE が返るので、回数を
	// 合わせるために CoUninitialize も呼ぶ)。別のモードで初期化されている (RPC_E_CHANGED_MODE) ときは、
	// CoUninitialize を呼ばずにそのまま使う。
	const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool balance = SUCCEEDED(co);

	bool opened = false;
	if (!item->empty()) {
		// 選ぶものの場所を 1 つ渡し、選ぶ数を 0 にすると、エクスプローラーはその親のフォルダーを開いてそれを選ぶ
		// (SHOpenFolderAndSelectItems のドキュメントのとおり)。既にその親のフォルダーを開いたウィンドウがあれば、その
		// ウィンドウで選ぶはずだが、確認していない (実機で、親のフォルダーを開いたまま押して確認すれば決まる)。
		// explorer.exe を `/select,` の引数で起動する方法は使わない (子プロセスを作り、引数の引用符の扱いを自分で
		// 持つことになる)。
		// ILCreateFromPathW は、MAX_PATH (260 文字) を超える場所を変換できないことがある。そのときは下で、フォルダー
		// だけを開く (選んだ状態にならないだけで、フォルダーは開く)。
		PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(item->c_str());
		if (pidl != nullptr) {
			opened = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
			ILFree(pidl);
		}
	}
	if (!opened) {
		// 選ぶものがない (もうない) か、選んで開けなかったので、フォルダーを開く。SHOpenFolderAndSelectItems にフォルダー
		// を渡すと、その親を開いてフォルダーを選ぶ形になるので、ここでは ShellExecuteW の open を使う。
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", folder->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		opened = reinterpret_cast<INT_PTR>(result) > 32;
	}

	if (balance)
		CoUninitialize();
	return opened;
}

} // namespace cordmux::platform

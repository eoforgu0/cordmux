// windows_api_confinement.py の NG 例。install/ は platform/ ではないので、ファイルを削除する、名前を変える、コピーする、
// 置き換える、一覧を読む関数と、OS のバージョンを読む関数の名前を書いてはいけない。--selftest が名前ごとに検出を
// 確かめるので、名前を 1 つずつ書く。

void remove_everything(const wchar_t *path, const wchar_t *temp, const wchar_t *copy)
{
	DeleteFileW(path);
	RemoveDirectoryW(path);
	MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING);
	ReplaceFileW(path, temp, nullptr, 0, nullptr, nullptr);
	CopyFileW(path, copy, FALSE);
	SHFILEOPSTRUCTW op{};
	SHFileOperationW(&op);
}

void list_and_flush(const wchar_t *pattern, void *file)
{
	WIN32_FIND_DATAW data{};
	void *find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
	FindNextFileW(find, &data);
	FindClose(find);
	const bool link = data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
	const bool dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	SetFileAttributesW(pattern, 0);
	FlushFileBuffers(file);
	(void)GetLastError();
	(void)link;
	(void)dir;
}

void read_versions()
{
	OSVERSIONINFOW info{};
	auto fn = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	RegGetValueW(nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr);
	RegOpenKeyW(nullptr, nullptr, nullptr);
	RegQueryValueExW(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
	(void)fn;
	(void)info;
}

// 実行ファイルの名前を platform/ の外で書く (子プロセスを起動する関数は platform/ にだけ置く)。
const char *const kHelper = "ffmpeg.exe";

// windows_api_confinement.py の OK 例。dock/ は platform/ ではないが、Windows の関数ではない識別子が、Windows の
// 名前を一部に含むだけなら検出しない (単語境界を付けた行の確認)。コピーする関数を関数オブジェクトとして受け取る形。
// イベントループを実行する関数の名前 exec は、実行ファイルの名前の行 (.exe の後ろの単語境界) に一致しない。

#include <functional>
#include <string>

using CopyFileFn = std::function<bool(const std::string &, const std::string &)>;
using DeleteFileFn = std::function<bool(const std::string &)>;

bool run_copy(const CopyFileFn &copy_file, const DeleteFileFn &delete_file_fn)
{
	return copy_file("a", "b") && delete_file_fn("a");
}

struct Loop {
	int exec() { return 0; }
};

int wait_loop(Loop &loop)
{
	return loop.exec();
}

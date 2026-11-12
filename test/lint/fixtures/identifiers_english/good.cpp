// identifiers_english.py の OK 例。識別子は ASCII だけで、コメントは日本語で書く。
//
// 文字列の中の // をコメントの始まりと読まないことも確かめる:
// 下の "https://example.com/a//b" の // は文字列の中にあるので、コメントではない。

#include <cstdint>

// 2 つの整数を足す
int add(int a, int b) {
    const char* note = "https://example.com/a//b";
    (void)note;
    int total = a + b;
    return total;
}

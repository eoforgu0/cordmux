// identifiers_english.py の NG 例。C++14 の桁区切りの後にある、日本語の変数の名前を検出する。

#include <cstdint>

int addLarge(int a) {
    const int big = 1'000;
    // 桁区切りの引用符を文字リテラルの始まりと読むと、この先の名前を読み飛ばす
    int 合計 = a + big;
    return 合計;
}

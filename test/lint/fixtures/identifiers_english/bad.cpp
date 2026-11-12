// identifiers_english.py の NG 例。変数の名前に日本語を使う。

#include <cstdint>

int add(int a, int b) {
    // 合計を持つ変数の名前が日本語なので、identifiers_english.py が検出する
    int 合計 = a + b;
    return 合計;
}

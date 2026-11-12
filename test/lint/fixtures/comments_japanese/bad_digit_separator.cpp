// comments_japanese.py の NG 例。C++14 の桁区切りの後にある、英語だけのコメントを検出する。

#include <cstdint>

int addLarge(int a) {
    const int big = 1'000;
    // This comment is written only in English on purpose, so the checker
    // below should turn red because it has three or more English words
    // and not a single Japanese character.
    return a + big;
}

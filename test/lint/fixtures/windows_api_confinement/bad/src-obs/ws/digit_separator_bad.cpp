// windows_api_confinement.py の NG 例。C++14 の桁区切り 2'000'000 の ' を文字リテラルの始まりと読むと、
// その後の <windows.h> を検出できない。

static const unsigned long long kBig = 2'000'000'000ULL;

#include <windows.h>

void start_listener_after_digit_separator() {}

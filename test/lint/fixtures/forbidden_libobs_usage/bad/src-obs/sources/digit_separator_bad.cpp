// forbidden_libobs_usage.py の NG 例。C++14 の桁区切り 2'000'000 の ' を文字リテラルの始まりと読むと、
// その後の使わない関数の呼び出しを検出できない。

#include <obs-module.h>

static const unsigned long long kBig = 2'000'000'000ULL;

void bad_after_digit_separator(obs_source_t *src)
{
	obs_source_set_muted(src, true);
}

// forbidden_libobs_usage.py の NG 例。ミュートを書き換える。

#include <obs-module.h>

void bad_set_muted(obs_source_t *source)
{
	obs_source_set_muted(source, true); // cordmux はミュートを書き換えない
}

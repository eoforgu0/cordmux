// forbidden_libobs_usage.py の NG 例。音量を書き換える。

#include <obs-module.h>

void bad_set_volume(obs_source_t *source)
{
	obs_source_set_volume(source, 2.0f); // cordmux は音量を書き換えない
}

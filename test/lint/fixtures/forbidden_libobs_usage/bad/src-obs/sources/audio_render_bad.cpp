// forbidden_libobs_usage.py の NG 例。audio_render を自作する。

#include <obs-module.h>

void my_audio_render(void *data, uint64_t *ts, struct obs_source_audio_mix *mix, uint32_t channels, size_t rate)
{
	(void)data;
	(void)ts;
	(void)mix;
	(void)channels;
	(void)rate;
}

struct obs_source_info bad_source_info = {};

void register_bad_source(void)
{
	bad_source_info.audio_render = my_audio_render; // cordmux は audio_render を自作しない
}

// forbidden_libobs_usage.py の OK 例。音量とミュートを読むだけで、書き換えない。

#include <obs-module.h>

void read_mixer_state(obs_source_t *source)
{
	bool muted = obs_source_muted(source);
	float volume = obs_source_get_volume(source);
	(void)muted;
	(void)volume;
}

// forbidden_libobs_usage.py の NG 例。enum_active_sources は実装しているが、enum_all_sources を実装していない。
// REQUIRES: を 1 行にだけ付けると、この形を検出できない。

#include <obs-module.h>

void bad_mirror_enum(void *data, obs_source_enum_proc_t cb, void *param)
{
	(void)data;
	(void)cb;
	(void)param;
}

void bad_mirror_update(obs_source_t *self, obs_source_t *target)
{
	obs_source_add_active_child(self, target);
}

struct obs_source_info bad_mirror_info = {};

void register_bad_mirror(void)
{
	bad_mirror_info.enum_active_sources = bad_mirror_enum;
	// enum_all_sources を実装していない。
}

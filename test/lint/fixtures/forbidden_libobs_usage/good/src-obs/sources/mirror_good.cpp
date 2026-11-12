// forbidden_libobs_usage.py の OK 例。enum_active_sources と enum_all_sources の両方を実装しているので、
// obs_source_add_active_child を呼んでよい。

#include <obs-module.h>

void good_mirror_enum(void *data, obs_source_enum_proc_t cb, void *param)
{
	(void)data;
	(void)cb;
	(void)param;
}

void good_mirror_update(obs_source_t *self, obs_source_t *target)
{
	obs_source_add_active_child(self, target);
}

struct obs_source_info good_mirror_info = {};

void register_good_mirror(void)
{
	good_mirror_info.enum_active_sources = good_mirror_enum;
	good_mirror_info.enum_all_sources = good_mirror_enum;
}

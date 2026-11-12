// forbidden_libobs_usage.py の NG 例。enum_active_sources を実装せずに、obs_source_add_active_child だけで子の
// ソースを持つ。

#include <obs-module.h>

void bad_mirror_update(obs_source_t *self, obs_source_t *target)
{
	obs_source_add_active_child(self, target);
}

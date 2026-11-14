// SPDX-License-Identifier: Apache-2.0

// cordmux_removal.h の実装。

#include "sources/cordmux_removal.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include <obs-module.h>

#include "diag/diag_log.h"
#include "sources/scene_names.h"

namespace cordmux::sources {

namespace {

using install::RemovalCount;
using install::RemovalItem;

// 参照を 1 つずつ持つソースの並び (破棄するときに、すべての参照を解放する)。列挙のコールバックの中では何も
// 削除せず、ここに集めてから列挙の外で削除する (列挙は libobs のロックを持ったままコールバックを呼ぶため)。
class SourceRefs {
public:
	SourceRefs() = default;
	SourceRefs(const SourceRefs &) = delete;
	SourceRefs &operator=(const SourceRefs &) = delete;
	~SourceRefs()
	{
		for (obs_source_t *source : items_)
			obs_source_release(source);
	}

	void add(obs_source_t *source)
	{
		if (obs_source_t *ref = obs_source_get_ref(source))
			items_.push_back(ref);
	}

	const std::vector<obs_source_t *> &items() const { return items_; }

private:
	std::vector<obs_source_t *> items_;
};

bool collect_source_proc(void *param, obs_source_t *source)
{
	static_cast<SourceRefs *>(param)->add(source);
	return true;
}

// 公開の入力ソースとグループ (obs_enum_sources はプライベートなソースを返さない)。
void collect_inputs(SourceRefs &out)
{
	obs_enum_sources(collect_source_proc, &out);
}

// OBS のシーンの一覧に表示されるシーン。
void collect_scenes(SourceRefs &out)
{
	obs_enum_scenes(collect_source_proc, &out);
}

// cordmux の種類のソース (合成のソース)。利用者が OBS の [+] から追加したソースも削除する。
bool is_cordmux_input(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);
	if (id == nullptr)
		return false;
	return std::strcmp(id, kCompositeSourceTypeId) == 0;
}

// 利用者が自分のソースに付けた、1 人のときだけ見せるフィルタ (cordmux_solo_filter)。
bool is_cordmux_filter(obs_source_t *filter)
{
	const char *id = obs_source_get_id(filter);
	if (id == nullptr)
		return false;
	return std::strcmp(id, kSoloFilterTypeId) == 0;
}

// ---- フィルタを外す ----

// フィルタを付けられるソース (公開の入力ソース、グループ、シーン)。参照を 1 つずつ持つ。
void collect_filter_parents(SourceRefs &out)
{
	collect_inputs(out);
	collect_scenes(out);
}

void collect_filter_proc(obs_source_t * /*parent*/, obs_source_t *child, void *param)
{
	if (is_cordmux_filter(child))
		static_cast<SourceRefs *>(param)->add(child);
}

// parent に付いている cordmux のフィルタ (参照を 1 つずつ持つ)。列挙は filter_mutex を持ったままコールバックを呼ぶので、
// 外すのは列挙の外で行う。
void collect_cordmux_filters(obs_source_t *parent, SourceRefs &out)
{
	obs_source_enum_filters(parent, collect_filter_proc, &out);
}

// cordmux のフィルタを、すべてのソースから外す。付けたソースそのものは利用者の物なので、消しも隠しもしない (アンインストールの後は、
// 付けたソースが常に表示される。guide の「アンインストール」)。cordmux を読み込めない OBS では、libobs が種類の分からない
// フィルタを素通しにして付けたソースを常に描くので、残しても働かない (obs-source.c の render_video)。
void remove_cordmux_filters()
{
	SourceRefs parents;
	collect_filter_parents(parents);
	for (obs_source_t *parent : parents.items()) {
		SourceRefs filters;
		collect_cordmux_filters(parent, filters);
		for (obs_source_t *filter : filters.items())
			obs_source_filter_remove(parent, filter);
	}
}

std::size_t count_cordmux_filters()
{
	SourceRefs parents;
	collect_filter_parents(parents);
	std::size_t count = 0;
	for (obs_source_t *parent : parents.items()) {
		SourceRefs filters;
		collect_cordmux_filters(parent, filters);
		count += filters.items().size();
	}
	return count;
}

// ---- シーンアイテムを外す ----

struct ItemCollect {
	bool (*match)(obs_source_t *) = nullptr;
	std::vector<obs_sceneitem_t *> items; // 参照を 1 つずつ持つ
};

bool collect_item_proc(obs_scene_t * /*scene*/, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<ItemCollect *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (source != nullptr && ctx->match(source)) {
		obs_sceneitem_addref(item);
		ctx->items.push_back(item);
	}
	return true;
}

// match に一致するソースを指すシーンアイテムを、すべてのシーンとグループから外す (保存の前にシーンアイテムが
// 残らないようにする。cordmux_removal.h の削除の方法)。
void remove_items_pointing_at(bool (*match)(obs_source_t *))
{
	SourceRefs scenes;
	SourceRefs inputs;
	collect_scenes(scenes);
	collect_inputs(inputs);

	ItemCollect ctx;
	ctx.match = match;
	for (obs_source_t *source : scenes.items()) {
		if (obs_scene_t *scene = obs_scene_from_source(source))
			obs_scene_enum_items(scene, collect_item_proc, &ctx);
	}
	for (obs_source_t *source : inputs.items()) {
		if (obs_scene_t *group = obs_group_from_source(source))
			obs_scene_enum_items(group, collect_item_proc, &ctx);
	}

	for (obs_sceneitem_t *item : ctx.items) {
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}
}

// ---- 種類ごとに削除する、数える ----

void remove_cordmux_inputs()
{
	remove_cordmux_filters();
	remove_items_pointing_at(is_cordmux_input);
	SourceRefs inputs;
	collect_inputs(inputs);
	for (obs_source_t *source : inputs.items()) {
		if (is_cordmux_input(source) && !obs_source_removed(source))
			obs_source_remove(source);
	}
}

std::size_t count_cordmux_inputs()
{
	SourceRefs inputs;
	collect_inputs(inputs);
	std::size_t count = 0;
	for (obs_source_t *source : inputs.items()) {
		if (is_cordmux_input(source) && !obs_source_removed(source))
			++count;
	}
	return count;
}

// 知らない種類は「1 つ残っている」と数える (cordmux_removal.h の remove_cordmux_objects のコメント)。
std::size_t count_one(RemovalItem item)
{
	switch (item) {
	case RemovalItem::CordmuxSources:
		return count_cordmux_inputs() + count_cordmux_filters();
	default:
		return 1;
	}
}

std::vector<RemovalCount> count_remaining(const std::vector<RemovalItem> &items)
{
	std::vector<RemovalCount> out;
	out.reserve(items.size());
	for (RemovalItem item : items)
		out.push_back(RemovalCount{item, count_one(item)});
	return out;
}

} // namespace

std::vector<install::RemovalCount> remove_cordmux_objects(const std::vector<install::RemovalItem> &items)
{
	for (RemovalItem item : items) {
		switch (item) {
		case RemovalItem::CordmuxSources:
			remove_cordmux_inputs();
			break;
		default:
			diag::logf(LOG_WARNING,
				   "Uninstall: an item has no way to be removed from scenes and was left as is "
				   "(item=%d)",
				   static_cast<int>(item));
			break;
		}
	}
	return count_remaining(items);
}

} // namespace cordmux::sources

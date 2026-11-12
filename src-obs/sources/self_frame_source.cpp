// SPDX-License-Identifier: Apache-2.0

// self_frame_source.h の実装。

#include "sources/self_frame_source.h"

#include <algorithm>

#include <obs-module.h>

#include "diag/diag_log.h"
#include "sources/composite_source.h"
#include "sources/self_frame_plan.h"

namespace cordmux::sources {

namespace {

// UI のスレッドだけが触る状態。
struct State {
	// 合成のソースに結び付けているか。
	bool bound = false;
	// 探している指定。設定の値が変わったかを比べる。
	std::string wanted_uuid;
	std::string wanted_name;
	bool wanted_valid = false;
	bool not_found_logged = false;
	// 選んだシーンが循環のために使えなくなったことを、ログに書いたか。
	bool cycle_logged = false;
};

State &state()
{
	static State instance;
	return instance;
}

// 結び付けを外す。
void unbind(bool log_lost)
{
	State &st = state();
	const bool was_bound = st.bound;
	if (was_bound)
		composite_bind_self_frame("");
	st.bound = false;
	if (was_bound && log_lost)
		diag::logf(LOG_INFO, "Self slot video source lost");
}

// obs_enum_sources の中では obs_get_source_by_* を呼ばない。値を集めるだけにする。
bool collect_source(void *param, obs_source_t *source)
{
	auto *out = static_cast<std::vector<SelfSourceCandidate> *>(param);
	const char *id = obs_source_get_id(source);
	const char *name = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);
	if (id == nullptr || name == nullptr || uuid == nullptr)
		return true;
	SelfSourceCandidate candidate;
	candidate.uuid = uuid;
	candidate.name = name;
	candidate.eligible = self_frame_source_eligible((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0, id);
	candidate.removed = obs_source_removed(source);
	out->push_back(std::move(candidate));
	return true;
}

// シーンの展開の深さの上限 (入れ子のシーンとグループ)。超えた所は中が分からないものとして扱う (SceneNode::truncated)。
constexpr int kSceneDepthLimit = 16;

struct SceneWalk {
	SceneNode *node = nullptr;
	int depth = 0;
};

void fill_scene_node(obs_scene_t *scene, SceneNode *node, int depth);

// シーンの項目 1 つ。入れ子のシーンとグループは、中も木に展開する。obs_scene_enum_items の中では項目を解放しない。
bool collect_scene_item(obs_scene_t * /*scene*/, obs_sceneitem_t *item, void *param)
{
	auto *walk = static_cast<SceneWalk *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (source == nullptr)
		return true;
	SceneNode child;
	const char *id = obs_source_get_id(source);
	child.type_id = id != nullptr ? id : "";
	if (obs_scene_t *inner = obs_scene_from_source(source))
		fill_scene_node(inner, &child, walk->depth + 1);
	walk->node->children.push_back(std::move(child));
	return true;
}

void fill_scene_node(obs_scene_t *scene, SceneNode *node, int depth)
{
	if (depth >= kSceneDepthLimit) {
		node->truncated = true;
		return;
	}
	SceneWalk walk;
	walk.node = node;
	walk.depth = depth;
	obs_scene_enum_items(scene, collect_scene_item, &walk);
}

// source のシーンの中身。シーンでなければ空の木を返す。
SceneNode scene_contents(obs_source_t *source)
{
	SceneNode node;
	const char *id = obs_source_get_id(source);
	node.type_id = id != nullptr ? id : "";
	if (obs_scene_t *scene = obs_scene_from_source(source))
		fill_scene_node(scene, &node, 0);
	return node;
}

// obs_enum_scenes の中でも、obs_get_source_by_* は呼ばない。値を集めるだけにする。
bool collect_scene(void *param, obs_source_t *source)
{
	auto *out = static_cast<std::vector<SelfSourceCandidate> *>(param);
	const char *id = obs_source_get_id(source);
	const char *name = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);
	if (id == nullptr || name == nullptr || uuid == nullptr)
		return true;
	SelfSourceCandidate candidate;
	candidate.uuid = uuid;
	candidate.name = name;
	candidate.is_scene = true;
	candidate.removed = obs_source_removed(source);
	// 削除したシーンの中身は展開しない (候補にならないので、中を見る理由がない)。
	candidate.eligible = !candidate.removed && self_frame_scene_eligible(obs_source_is_group(source), scene_contents(source));
	out->push_back(std::move(candidate));
	return true;
}

// 入力のソースの後ろに、シーンを並べる。
std::vector<SelfSourceCandidate> enumerate_sources()
{
	std::vector<SelfSourceCandidate> list;
	obs_enum_sources(collect_source, &list);
	obs_enum_scenes(collect_scene, &list);
	return list;
}

// いま結び付けているものがシーンで、cordmux の合成のソースを含むようになったか。
bool bound_scene_has_cordmux(obs_source_t *source)
{
	if (obs_scene_from_source(source) == nullptr)
		return false;
	return scene_contains_cordmux(scene_contents(source));
}

} // namespace

SelfFrameSync sync_self_frame(const SelfFrameSetting &setting)
{
	SelfFrameSync sync;
	State &st = state();

	const bool designated = !setting.uuid.empty() || !setting.name.empty();
	if (!designated) {
		unbind(false);
		st.wanted_valid = false;
		st.not_found_logged = false;
		st.cycle_logged = false;
		return sync;
	}
	if (!st.wanted_valid || st.wanted_uuid != setting.uuid || st.wanted_name != setting.name) {
		// 指定が変わった。結び付けを外して、探し直す。
		unbind(false);
		st.wanted_uuid = setting.uuid;
		st.wanted_name = setting.name;
		st.wanted_valid = true;
		st.not_found_logged = false;
		st.cycle_logged = false;
	}

	if (st.bound) {
		// 結び付けているソースがまだあるか。名前を変えられていたら、設定の名前を修正する。利用者が OBS で削除した
		// ソースは、参照が残る間は弱参照から取り出せるので、削除されたかも見る。削除されていたら外し、下で探し直す。
		obs_source_t *source = composite_self_frame_ref();
		if (source != nullptr && obs_source_removed(source)) {
			obs_source_release(source);
			source = nullptr;
		}
		if (source != nullptr) {
			// 選んだ後に cordmux をそのシーンへ足されたら、合成のソースが自分自身を描く循環になるので、結び付けを外す。
			if (bound_scene_has_cordmux(source)) {
				obs_source_release(source);
				unbind(false);
				if (!st.cycle_logged) {
					diag::logf(LOG_INFO, "Self slot video scene now contains cordmux, so it was unbound");
					st.cycle_logged = true;
				}
				st.not_found_logged = true; // 候補から外れるので、次の探し直しは見つからない。その行は足さない
				return sync;
			}
			const char *name = obs_source_get_name(source);
			if (name != nullptr && setting.name != name) {
				sync.rewrite = true;
				sync.uuid = setting.uuid;
				sync.name = name;
				st.wanted_name = name;
			}
			obs_source_release(source);
			return sync;
		}
		unbind(true);
	}

	const std::vector<SelfSourceCandidate> all = enumerate_sources();
	const SelfFrameLookup found = lookup_self_frame(all, setting.uuid, setting.name);
	if (found.index < 0) {
		if (!st.not_found_logged) {
			diag::logf(LOG_INFO, "Self slot video source not found");
			st.not_found_logged = true;
		}
		return sync;
	}

	if (!composite_bind_self_frame(found.uuid.c_str())) {
		// 合成のソースの活性を伝えられなかった (子にすると循環になる)。結び付けず、同じ指定の間は探し直さない。
		if (!st.cycle_logged) {
			diag::logf(LOG_INFO, "Self slot video source can't be used because it would draw itself");
			st.cycle_logged = true;
		}
		st.not_found_logged = true;
		return sync;
	}
	obs_source_t *bound = composite_self_frame_ref();
	if (bound == nullptr) {
		// 列挙してから結び付けるまでの間にソースが消えた。次の周期で探し直す。
		return sync;
	}
	obs_source_release(bound);
	st.bound = true;
	st.not_found_logged = false;
	diag::logf(LOG_INFO, "Self slot video source found (match=%s)", found.by_uuid ? "uuid" : "name");

	if (found.rewrite) {
		sync.rewrite = true;
		sync.uuid = found.uuid;
		sync.name = found.name;
		// 書き直した値が次の周期で指定として届く。変わった指定と見て外さないように、現在の値を覚える。
		st.wanted_uuid = found.uuid;
		st.wanted_name = found.name;
	}
	return sync;
}

void release_self_frame()
{
	unbind(false);
	state().wanted_valid = false;
	state().not_found_logged = false;
	state().cycle_logged = false;
}

std::vector<SelfSourceCandidate> list_self_frame_candidates()
{
	std::vector<SelfSourceCandidate> all = enumerate_sources();
	all.erase(std::remove_if(all.begin(), all.end(),
				 [](const SelfSourceCandidate &c) { return !usable_self_source(c); }),
		  all.end());
	// ソースを名前の順に並べ、その後ろにシーンを名前の順に並べる。
	std::stable_sort(all.begin(), all.end(), [](const SelfSourceCandidate &a, const SelfSourceCandidate &b) {
		if (a.is_scene != b.is_scene)
			return !a.is_scene;
		return a.name < b.name;
	});
	return all;
}

} // namespace cordmux::sources

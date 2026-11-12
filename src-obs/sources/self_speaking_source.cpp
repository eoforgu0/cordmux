// SPDX-License-Identifier: Apache-2.0

// self_speaking_source.h の実装。

#include "sources/self_speaking_source.h"

#include <algorithm>
#include <cstdint>
#include <mutex>

#include <obs-module.h>
#include <util/platform.h>

#include "diag/diag_log.h"
#include "sources/decor_registry.h"

namespace cordmux::sources {

namespace {

// コールバックと UI のスレッドが共有する状態。ソースが破棄される途中の呼び出しとの前後は libobs が決めるので、
// 解放しない静的な値にする。
struct Shared {
	// judge と hist と sample_rate と threshold_db を守る。持ったまま取るのは decor_registry.cpp の表のロックだけで、逆の順に
	// 取る処理はない (表のロックを持ったままこのロックを取らない)。持っている間は OBS の関数を呼ばない。
	std::mutex mu;
	SelfSpeakingJudge judge;
	PeakHistogram hist;
	std::uint32_t sample_rate = 48000;
	int threshold_db = config::kDefaultSelfSpeakingThresholdDb;
};

Shared &shared()
{
	static Shared *instance = new Shared();
	return *instance;
}

// UI のスレッドだけが触る状態。
struct Attached {
	obs_weak_source_t *weak = nullptr;
	// 探している指定。設定の値が変わったかを比べる。
	std::string wanted_uuid;
	std::string wanted_name;
	bool wanted_valid = false;
	bool not_found_logged = false;
};

Attached &attached()
{
	static Attached instance;
	return instance;
}

void capture_callback(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	if (audio == nullptr || audio->frames == 0 || source == nullptr)
		return;
	auto *state = static_cast<Shared *>(param);

	// OBS の値は、ロックを取る前に読む。
	const std::uint64_t now_ns = os_gettime_ns();
	const float fader = obs_source_get_volume(source);
	const bool active = obs_source_active(source);
	const std::int64_t sync_offset = obs_source_get_sync_offset(source);

	float peak = 0.0f;
	for (int channel = 0; channel < MAX_AUDIO_CHANNELS; ++channel) {
		const auto *samples = reinterpret_cast<const float *>(audio->data[channel]);
		if (samples == nullptr)
			continue;
		peak = std::max(peak, plane_peak(samples, audio->frames));
	}

	std::lock_guard<std::mutex> lock(state->mu);
	SelfSpeakingPacket packet;
	const std::int64_t start = static_cast<std::int64_t>(audio->timestamp) + sync_offset;
	packet.start_ns = start > 0 ? static_cast<std::uint64_t>(start) : 1;
	packet.length_ns = static_cast<std::int64_t>(audio->frames) * 1'000'000'000LL /
			   static_cast<std::int64_t>(state->sample_rate > 0 ? state->sample_rate : 48000);
	packet.peak_db = peak_to_db(static_cast<double>(peak));
	packet.audible = self_source_audible(muted, fader, active);

	note_source_voice_packet(now_ns);
	const SelfSpeakingJudge::Result result = state->judge.observe(packet, now_ns);
	if (result.restarted)
		clear_source_speaking();
	for (const SelfSpeakingChange &change : result.changes)
		set_source_speaking(change.start_ns, change.stop_ns);
	state->hist.add(packet.peak_db, state->judge.talking());
}

// コールバックを外し、判定と表の状態を捨てる。表のロックとこのファイルの状態のロックを持たずに remove を呼ぶ
// (remove は走っているコールバックの終わりを待ち、コールバックはどちらのロックも取る)。
void detach(bool log_lost)
{
	Attached &state = attached();
	const bool was_attached = state.weak != nullptr;
	if (state.weak != nullptr) {
		if (obs_source_t *source = obs_weak_source_get_source(state.weak)) {
			obs_source_remove_audio_capture_callback(source, capture_callback, &shared());
			obs_source_release(source);
		}
		obs_weak_source_release(state.weak);
		state.weak = nullptr;
	}
	{
		Shared &sh = shared();
		std::lock_guard<std::mutex> lock(sh.mu);
		sh.judge.reset();
	}
	clear_source_speaking();
	set_source_voice_found(false);
	if (was_attached && log_lost)
		diag::logf(LOG_INFO, "Self slot audio source lost");
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
	candidate.eligible = (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0 && !is_cordmux_source_id(id);
	candidate.removed = obs_source_removed(source);
	out->push_back(std::move(candidate));
	return true;
}

std::vector<SelfSourceCandidate> enumerate_sources()
{
	std::vector<SelfSourceCandidate> list;
	obs_enum_sources(collect_source, &list);
	return list;
}

} // namespace

SelfSpeakingSync sync_self_speaking(const SelfSpeakingSetting &setting, const std::string &user_id)
{
	SelfSpeakingSync sync;
	Attached &state = attached();
	Shared &sh = shared();

	const bool designated = !setting.uuid.empty() || !setting.name.empty();
	set_source_voice_user(designated ? user_id : std::string());
	{
		std::lock_guard<std::mutex> lock(sh.mu);
		sh.threshold_db = clamp_self_speaking_threshold_db(setting.threshold_db);
		sh.judge.set_threshold_db(sh.threshold_db);
	}

	if (!designated) {
		detach(false);
		state.wanted_valid = false;
		state.not_found_logged = false;
		return sync;
	}
	if (!state.wanted_valid || state.wanted_uuid != setting.uuid || state.wanted_name != setting.name) {
		// 指定が変わった。付けているコールバックを外して、探し直す。
		detach(false);
		state.wanted_uuid = setting.uuid;
		state.wanted_name = setting.name;
		state.wanted_valid = true;
		state.not_found_logged = false;
	}

	if (state.weak != nullptr) {
		// 付けているソースがまだあるか。名前を変えられていたら、設定の名前を修正する。利用者が OBS で削除したソースは、
		// 参照が残る間は弱参照から取り出せるので、削除されたかも見る (自分の枠の映像の sync_self_frame と同じ)。
		obs_source_t *source = obs_weak_source_get_source(state.weak);
		if (source != nullptr && obs_source_removed(source)) {
			obs_source_release(source);
			source = nullptr;
		}
		if (source == nullptr) {
			detach(true);
		} else {
			const char *name = obs_source_get_name(source);
			if (name != nullptr && setting.name != name) {
				sync.rewrite = true;
				sync.uuid = setting.uuid;
				sync.name = name;
				state.wanted_name = name;
			}
			obs_source_release(source);
			set_source_voice_found(true);
			return sync;
		}
	}

	const std::vector<SelfSourceCandidate> all = enumerate_sources();
	const int found = find_self_source(all, setting.uuid, setting.name);
	obs_source_t *source = found >= 0 ? obs_get_source_by_uuid(all[static_cast<std::size_t>(found)].uuid.c_str())
					  : nullptr;
	if (source == nullptr) {
		if (!state.not_found_logged) {
			diag::logf(LOG_INFO, "Self slot audio source not found");
			state.not_found_logged = true;
		}
		set_source_voice_found(false);
		return sync;
	}

	obs_audio_info audio_info{};
	const bool have_audio_info = obs_get_audio_info(&audio_info);
	{
		std::lock_guard<std::mutex> lock(sh.mu);
		if (have_audio_info && audio_info.samples_per_sec > 0)
			sh.sample_rate = audio_info.samples_per_sec;
		sh.judge.reset();
	}
	clear_source_speaking();
	const SelfSourceCandidate &chosen = all[static_cast<std::size_t>(found)];
	const bool by_uuid = !setting.uuid.empty() && chosen.uuid == setting.uuid;
	state.weak = obs_source_get_weak_source(source);
	obs_source_add_audio_capture_callback(source, capture_callback, &sh);
	obs_source_release(source);
	state.not_found_logged = false;
	set_source_voice_found(true);
	diag::logf(LOG_INFO, "Self slot audio source found (match=%s)", by_uuid ? "uuid" : "name");

	if (chosen.uuid != setting.uuid || chosen.name != setting.name) {
		sync.rewrite = true;
		sync.uuid = chosen.uuid;
		sync.name = chosen.name;
		// 書き直した値が次の周期で指定として届く。変わった指定と見て外さないように、現在の値を覚える。
		state.wanted_uuid = chosen.uuid;
		state.wanted_name = chosen.name;
	}
	return sync;
}

void release_self_speaking()
{
	detach(false);
	attached().wanted_valid = false;
	attached().not_found_logged = false;
}

std::vector<SelfSourceCandidate> list_self_speaking_candidates()
{
	std::vector<SelfSourceCandidate> all = enumerate_sources();
	all.erase(std::remove_if(all.begin(), all.end(),
				 [](const SelfSourceCandidate &c) { return !usable_self_source(c); }),
		  all.end());
	std::stable_sort(all.begin(), all.end(),
			 [](const SelfSourceCandidate &a, const SelfSourceCandidate &b) { return a.name < b.name; });
	return all;
}

SelfSpeakingStats take_self_speaking_stats()
{
	Shared &sh = shared();
	std::lock_guard<std::mutex> lock(sh.mu);
	SelfSpeakingStats stats;
	stats.summary = sh.hist.take();
	stats.threshold_db = sh.threshold_db;
	return stats;
}

} // namespace cordmux::sources

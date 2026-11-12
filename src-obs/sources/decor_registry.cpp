// SPDX-License-Identifier: Apache-2.0

// decor_registry.h の実装。人の情報、名前ラベルに描く人、発言、見た目の値の表を持つ。

#include "sources/decor_registry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <QImage>

#include <util/platform.h>

#include "sources/decor_paint.h"
#include "sources/dummy_feed.h"
#include "sources/label_border.h"
#include "sources/scene_names.h"

namespace cordmux::sources {

namespace {

// ---- 人の情報、名前ラベルに描く人、発言、見た目の値の表 (cordmux 全体で 1 つ) ----

struct PersonEntry {
	std::string name;
	std::vector<std::uint8_t> avatar_png;
	QImage avatar; // avatar_png を読んだ結果 (読めなければ空)
	DecorCorner corner = DecorCorner::BottomLeft;
	std::vector<std::string> others;
};

// 画面共有していない参加者 1 人分の情報 (set_roster_person_decor)。
struct RosterPersonEntry {
	std::string name;
	std::vector<std::uint8_t> avatar_png;
	QImage avatar; // avatar_png を読んだ結果 (読めなければ空)
	std::uint64_t version = 0;
};

struct DecorRegistry {
	// ほかのロックを取らないロック。持っている間は OBS の関数も呼ばない。
	std::mutex mutex;
	std::map<std::string, PersonEntry> people;
	// 添字 0 が自分の枠 (kSelfSlot)、1 から 4 が画面共有の枠、5 が全画面だけで映す人の子 (kFocusOnlySlot)。
	std::string owners[kScreenTileCount + 1];
	DecorLabelParams params;
	// 合成のソースの名前ラベルの見た目の値。変わるたびに composite_look_version が進む。version とは別に進める。
	CompositeLabelLook composite_look;
	std::uint64_t composite_look_version = 1;
	// 枠の背景の値。version は持たない (描画のたびにコピーする小さな値で、描き直す画像がない)。
	CompositeBackground composite_background;
	// ミキサーが出した声の区間から求めた発言 (set_voice_speaking)。発言では version を進めない。
	std::map<std::string, SpeakingTimes> voice_speaking;
	// 音声ソースで発言を決める人 (set_source_voice_user。空なら指定なし) と、その人の発言、ソースを見つけているか、最後の
	// パケットの時刻。発言の時刻は voice_speaking と別に持ち、読むときに user_id で選ぶ。
	std::string source_user;
	SpeakingTimes source_speaking;
	bool source_found = false;
	std::uint64_t source_last_packet_ns = 0;
	// 人の情報、名前ラベルに描く人、見た目の値のどれかが変わるたびに 1 つ進む (読む側は、これが変わったときだけ
	// 値をコピーし直し、描画し直すかを決める)。発言では進めない (発言は名前ラベルの画像に描かない)。
	std::uint64_t version = 1;
	// 合成のソースの、画面共有していない参加者の並びの設定と、人の情報 (user_id で引く)。人の情報の version は、
	// 変わるたびに roster_version を 1 つ進めた値にする (消して入れ直した人も、前と違う version になる)。どちらも
	// version は進めない。
	CompositeRosterParams composite_roster;
	std::map<std::string, RosterPersonEntry> roster_people;
	std::uint64_t roster_version = 0;
};

DecorRegistry &registry()
{
	static DecorRegistry instance;
	return instance;
}

// dim_pct は比べない。暗さは画像に描かず置くときに掛けるので、変わっても描き直さない (set_decor_label_params)。
bool same_params(const DecorLabelParams &a, const DecorLabelParams &b)
{
	return a.em_ratio == b.em_ratio && a.show_name == b.show_name && a.show_avatar == b.show_avatar &&
	       a.show_speaking == b.show_speaking && a.plate_argb == b.plate_argb && a.text_argb == b.text_argb &&
	       a.border_light_argb == b.border_light_argb && a.border_rise_ms == b.border_rise_ms &&
	       a.border_hold_ms == b.border_hold_ms && a.border_fade_ms == b.border_fade_ms &&
	       a.min_visible_graphemes == b.min_visible_graphemes && a.font_family == b.font_family &&
	       a.font_weight == b.font_weight && a.font_italic == b.font_italic;
}

BorderTiming timing_of(const DecorLabelParams &params)
{
	BorderTiming timing;
	timing.rise_ms = params.border_rise_ms;
	timing.hold_ms = params.border_hold_ms;
	timing.fade_ms = params.border_fade_ms;
	return timing;
}

// 枠の名前ラベルに描く人 (呼び出し元が registry().mutex を持っていること)。ダミー表示の間の画面共有の枠では、
// ダミーの人を返す (全画面だけで映す人の子は、ダミー表示の間は使わないので、描く人なし)。
std::string effective_owner_locked(const DecorRegistry &reg, int slot)
{
	if (slot < 0 || slot > kScreenTileCount)
		return std::string();
	if (slot >= 1 && dummy_mode_active())
		return slot <= kScreenSlotCount ? dummy_person_id(slot) : std::string();
	return reg.owners[slot];
}

// user_id の人の発言を音声ソースで決めているか (呼び出し元が registry().mutex を持っていること)。
SourceVoice source_voice_locked(const DecorRegistry &reg, const std::string &user_id, std::uint64_t now_ns)
{
	if (user_id.empty() || reg.source_user.empty() || user_id != reg.source_user)
		return SourceVoice::NotJudged;
	return source_voice_at(true, reg.source_found, reg.source_last_packet_ns, now_ns);
}

// 読む側が使う、user_id の人の発言 (呼び出し元が registry().mutex を持っていること)。
SpeakingTimes speaking_of_locked(const DecorRegistry &reg, const std::string &user_id, SourceVoice voice)
{
	SpeakingTimes discord;
	const auto spoken = reg.voice_speaking.find(user_id);
	if (spoken != reg.voice_speaking.end())
		discord = spoken->second;
	return source_voice_speaking(voice, discord, reg.source_speaking);
}

} // namespace

void set_decor_label_params(const DecorLabelParams &params)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.params.dim_pct = params.dim_pct; // version を進めない (same_params のコメント)
	if (same_params(reg.params, params))
		return;
	reg.params = params;
	++reg.version;
}

// 合成のソースが要る値を表からコピーする。表のロックだけを取る (呼び出し元はほかの cordmux のロックを持たない)。
// known_* は呼び出し元が前にコピーしたときの値で、表の version、見た目の version、枠の描く人のどれも同じなら、人の情報と
// 見た目の値はコピーしない (フレームごとのコピーを避ける)。
CompositeDecorView read_composite_decor(std::uint64_t known_version, std::uint64_t known_look_version,
					const std::array<std::string, kScreenTileCount + 1> &known_owners)
{
	CompositeDecorView view;
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	view.version = reg.version;
	view.look_version = reg.composite_look_version;
	view.dim_pct = reg.params.dim_pct;
	const std::uint64_t now_ns = os_gettime_ns();
	for (int slot = 0; slot <= kScreenTileCount; ++slot) {
		const auto index = static_cast<std::size_t>(slot);
		view.owners[index] = effective_owner_locked(reg, slot);
		if (view.owners[index].empty())
			continue;
		view.source_voice[index] = source_voice_locked(reg, view.owners[index], now_ns);
		view.speaking[index] = speaking_of_locked(reg, view.owners[index], view.source_voice[index]);
	}
	if (known_version == reg.version && known_look_version == reg.composite_look_version &&
	    view.owners == known_owners)
		return view;
	view.material_valid = true;
	view.params = reg.params;
	view.look = reg.composite_look;
	for (std::size_t index = 0; index < view.owners.size(); ++index) {
		if (const PersonEntry *person = owner_material(reg.people, view.owners[index])) {
			view.person_present[index] = true;
			view.person[index].name = person->name;
			view.person[index].avatar = person->avatar;
			view.person[index].corner = person->corner;
			view.person[index].other_visible_names = person->others;
		}
	}
	return view;
}

void set_composite_label_look(const CompositeLabelLook &look)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	if (reg.composite_look == look)
		return;
	reg.composite_look = look;
	++reg.composite_look_version;
}

void set_composite_background(const CompositeBackground &background)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.composite_background = background;
}

CompositeBackground read_composite_background()
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	return reg.composite_background;
}

void set_composite_roster_params(const CompositeRosterParams &params)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.composite_roster = params;
}

void set_roster_person_decor(const std::string &user_id, const RosterPersonDecor &decor)
{
	if (user_id.empty())
		return;
	DecorRegistry &reg = registry();

	// アバターの PNG の読み込みはロックの外で、PNG が変わったときだけ行う (set_person_decor と同じ)。
	bool same_png = false;
	QImage avatar;
	{
		std::lock_guard<std::mutex> lock(reg.mutex);
		const auto found = reg.roster_people.find(user_id);
		if (found != reg.roster_people.end() && found->second.avatar_png == decor.avatar_png) {
			same_png = true;
			avatar = found->second.avatar;
		}
	}
	if (!same_png && !decor.avatar_png.empty())
		avatar.loadFromData(decor.avatar_png.data(), static_cast<int>(decor.avatar_png.size()), "PNG");

	std::lock_guard<std::mutex> lock(reg.mutex);
	const auto found = reg.roster_people.find(user_id);
	const bool fresh = found == reg.roster_people.end();
	RosterPersonEntry &entry = fresh ? reg.roster_people[user_id] : found->second;
	const bool changed = fresh || entry.name != decor.name || entry.avatar_png != decor.avatar_png ||
			     (entry.avatar.isNull() != avatar.isNull());
	if (!changed)
		return;
	entry.name = decor.name;
	entry.avatar_png = decor.avatar_png;
	entry.avatar = avatar;
	entry.version = ++reg.roster_version;
}

void forget_roster_person_decor(const std::string &user_id)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.roster_people.erase(user_id);
}

CompositeRosterView read_composite_roster(const std::vector<std::string> &user_ids,
					  const std::vector<std::uint64_t> &known_versions)
{
	CompositeRosterView view;
	view.speaking.resize(user_ids.size());
	view.source_voice.assign(user_ids.size(), SourceVoice::NotJudged);
	view.people.resize(user_ids.size());
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	view.params = reg.composite_roster;
	const std::uint64_t now_ns = os_gettime_ns();
	for (std::size_t i = 0; i < user_ids.size(); ++i) {
		view.source_voice[i] = source_voice_locked(reg, user_ids[i], now_ns);
		view.speaking[i] = speaking_of_locked(reg, user_ids[i], view.source_voice[i]);
		const auto found = reg.roster_people.find(user_ids[i]);
		if (found == reg.roster_people.end())
			continue;
		CompositeRosterMaterial &out = view.people[i];
		out.present = true;
		out.version = found->second.version;
		const std::uint64_t known = i < known_versions.size() ? known_versions[i] : 0;
		if (out.version != known) {
			out.copied = true;
			out.name = found->second.name;
			out.avatar = found->second.avatar;
		}
	}
	return view;
}

void set_person_decor(const std::string &user_id, const PersonDecor &decor)
{
	if (user_id.empty())
		return;
	DecorRegistry &reg = registry();

	// アバターの PNG の読み込みはロックの外で、PNG が変わったときだけ行う (描画のスレッドを待たせないため)。
	bool same_png = false;
	QImage avatar;
	{
		std::lock_guard<std::mutex> lock(reg.mutex);
		const auto found = reg.people.find(user_id);
		if (found != reg.people.end() && found->second.avatar_png == decor.avatar_png) {
			same_png = true;
			avatar = found->second.avatar;
		}
	}
	if (!same_png && !decor.avatar_png.empty())
		avatar.loadFromData(decor.avatar_png.data(), static_cast<int>(decor.avatar_png.size()), "PNG");

	std::lock_guard<std::mutex> lock(reg.mutex);
	PersonEntry &entry = reg.people[user_id];
	const bool changed = entry.name != decor.name || entry.avatar_png != decor.avatar_png ||
			     entry.corner != decor.corner || entry.others != decor.other_visible_names ||
			     (entry.avatar.isNull() != avatar.isNull());
	if (!changed)
		return;
	entry.name = decor.name;
	entry.avatar_png = decor.avatar_png;
	entry.avatar = avatar;
	entry.corner = decor.corner;
	entry.others = decor.other_visible_names;
	++reg.version;
}

void forget_person_decor(const std::string &user_id)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	const bool had_person = reg.people.erase(user_id) > 0;
	reg.voice_speaking.erase(user_id);
	if (had_person)
		++reg.version;
}

void set_decor_owner(int slot, const std::string &user_id)
{
	if (slot < 0 || slot > kScreenTileCount)
		return;
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	if (reg.owners[slot] == user_id)
		return;
	reg.owners[slot] = user_id;
	++reg.version;
}

void set_voice_speaking(const std::string &user_id, std::uint64_t start_ns, std::uint64_t stop_ns)
{
	if (user_id.empty())
		return;
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	SpeakingTimes &times = reg.voice_speaking[user_id];
	times = merge_speaking(times, start_ns, stop_ns, timing_of(reg.params));
}

void clear_voice_speaking()
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.voice_speaking.clear();
}

void set_source_voice_user(const std::string &user_id)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.source_user = user_id;
}

void set_source_voice_found(bool found)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.source_found = found;
	if (!found)
		reg.source_last_packet_ns = 0;
}

void note_source_voice_packet(std::uint64_t now_ns)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.source_last_packet_ns = now_ns;
}

void set_source_speaking(std::uint64_t start_ns, std::uint64_t stop_ns)
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.source_speaking = merge_speaking(reg.source_speaking, start_ns, stop_ns, timing_of(reg.params));
}

void clear_source_speaking()
{
	DecorRegistry &reg = registry();
	std::lock_guard<std::mutex> lock(reg.mutex);
	reg.source_speaking = SpeakingTimes{};
}


} // namespace cordmux::sources

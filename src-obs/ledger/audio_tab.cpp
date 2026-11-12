// SPDX-License-Identifier: Apache-2.0

// 音声タブの表示の判断 (audio_tab.h) を実装する。

#include "ledger/audio_tab.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "config/person_audio.h"
#include "ledger/natural_order.h"
#include "ledger/text_ids.h"
#include "pipeline/audio_meter.h"

namespace cordmux::ledger {

namespace {

namespace keys = text::keys;

TextRef ref(const char *id)
{
	TextRef t;
	t.id = id;
	return t;
}

dockview::AudioRowView row_of(const dockview::PersonRow &person)
{
	dockview::AudioRowView row;
	row.user_id = person.user_id;
	row.name = person.name;
	row.is_main = person.audio.is_main_account;
	row.off = person.off;

	const bool no_voice = person.mic == dockview::MicState::NoVoice;
	row.voice.control = !no_voice;
	if (no_voice)
		row.voice.note = ref(keys::kDockAudioNoVoiceStatus);
	row.voice.mute_shown = true;
	row.voice.mute_enabled = !person.off && !no_voice;
	row.voice.muted = person.audio.voice_muted;
	row.voice.volume_pct = person.audio.voice_volume_pct;

	const bool has_audio = person.audio.has_screen_audio;
	row.screen.control = has_audio;
	if (!has_audio)
		row.screen.note = ref(person.sharing ? keys::kDockAudioNoScreenAudioStatus : keys::kDockAudioNotSharingStatus);
	row.screen.mute_shown = has_audio;
	row.screen.mute_enabled = !person.off && has_audio;
	row.screen.muted = person.audio.screen_muted;
	row.screen.volume_pct = person.audio.screen_volume_pct;
	return row;
}

} // namespace

dockview::AudioTabView audio_tab_of(const dockview::PeopleListView &list, bool composite_present)
{
	dockview::AudioTabView view;
	if (!composite_present) {
		view.notice = ref(keys::kDockAudioNoSourceText);
		return view;
	}

	// 先頭はメインアカウント。Vesktop のアカウントの行 (audio.applies が偽) は行にしない。
	if (list.pinned.has_value() && list.pinned->audio.applies)
		view.rows.push_back(row_of(*list.pinned));

	std::vector<const dockview::PersonRow *> others;
	for (const std::vector<dockview::PersonRow> *group : {&list.sharing, &list.in_call}) {
		for (const dockview::PersonRow &person : *group) {
			if (person.audio.applies)
				others.push_back(&person);
		}
	}
	std::sort(others.begin(), others.end(), [](const dockview::PersonRow *a, const dockview::PersonRow *b) {
		if (a->name.literal != b->name.literal)
			return natural_less(a->name.literal, b->name.literal);
		return a->user_id < b->user_id;
	});
	for (const dockview::PersonRow *person : others)
		view.rows.push_back(row_of(*person));

	if (view.rows.empty())
		view.notice = ref(keys::kDockAudioEmptyEmpty);
	return view;
}

MeterReading meter_reading(float peak_before, float peak_after, bool silenced, int volume_pct)
{
	MeterReading reading;
	reading.gray = silenced;
	// 音を出している間は、配信に出ている音の大きさ。ミュート中は掛けた後が 0 なので、ミュートを外したときに出る大きさ
	// (掛ける前の値に、つまみの音量の利得を掛けた値) にする。利得はミキサーに渡す値と同じ関数で求める。
	const float level = silenced ? peak_before * config::gain_of_person_volume(volume_pct) : peak_after;
	reading.position = pipeline::meter_position(pipeline::peak_to_dbfs(level));
	return reading;
}

} // namespace cordmux::ledger

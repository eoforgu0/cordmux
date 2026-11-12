// SPDX-License-Identifier: Apache-2.0

// double_audio.h の実装。

#include "sources/double_audio.h"

#include <cstring>

#include <obs-module.h>

#include "platform/platform.h"
#include "sources/scene_names.h"

namespace cordmux::sources {

namespace {

// ミュートでも音量 0% でもないか。OBS の音量は 0% から 2000% まで設定でき、0% にして止めている人もいるので、0% も
// ミュートと同じに扱う。
bool audible(obs_source_t *source)
{
	return !obs_source_muted(source) && obs_source_get_volume(source) > 0.0f;
}

// ソースの設定の kAppAudioCaptureWindowKey から、取り込む先が Vesktop か Discord かを読む。
bool captures_discord_client(obs_source_t *source)
{
	obs_data_t *settings = obs_source_get_settings(source);
	if (settings == nullptr)
		return false;
	const char *window = obs_data_get_string(settings, kAppAudioCaptureWindowKey);
	const bool found = window != nullptr && platform::is_discord_client_exe_name(window_setting_exe(window));
	obs_data_release(settings);
	return found;
}

bool scan_enum_proc(void *param, obs_source_t *source)
{
	auto *state = static_cast<DoubleAudioState *>(param);
	// バージョンの付かない ID で比べる。OBS がソースの種類のバージョンを上げると、obs_source_get_id は末尾に
	// "_v2" などを付けた ID を返すためである。
	const char *id = obs_source_get_unversioned_id(source);
	if (id == nullptr)
		return true;

	if (std::strcmp(id, kCompositeSourceTypeId) == 0) {
		// 出力に出ている cordmux のソースのうち、鳴るものがあるか。出ていないソースの音量は、配信の音に関係しない。
		if (obs_source_active(source)) {
			state->composite_active = true;
			if (audible(source))
				state->composite_audible = true;
		}
	} else if (!state->discord_capture_name.has_value() && std::strcmp(id, kAppAudioCaptureTypeId) == 0) {
		// 設定を読む前に、軽い条件 (配信の出力、ミュート、音量) で絞る。
		if (obs_source_active(source) && audible(source) && captures_discord_client(source)) {
			const char *name = obs_source_get_name(source);
			state->discord_capture_name = name != nullptr ? std::string(name) : std::string();
		}
	}
	// 両方が分かれば、残りのソースは読まない。
	return !(state->composite_audible && state->discord_capture_name.has_value());
}

} // namespace

DoubleAudioState read_double_audio_state()
{
	DoubleAudioState state;
	// obs_enum_sources は、enum_proc を呼び出す前後で自分で参照を取って解放する。ここでは名前を文字列にコピー
	// するだけで、ソースのポインタを列挙の外に持ち出さない。
	obs_enum_sources(scan_enum_proc, &state);
	return state;
}

} // namespace cordmux::sources

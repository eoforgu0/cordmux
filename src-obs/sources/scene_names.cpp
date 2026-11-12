// SPDX-License-Identifier: Apache-2.0

// scene_names.h の、OBS の現在の言語の文字列から名前を作る関数の実装。

#include "sources/scene_names.h"

#include <obs-module.h>

#include "ledger/text_ids.h"

namespace cordmux::sources {

namespace keys = ledger::text::keys;

namespace {

std::string text_of(const char *key)
{
	const char *value = obs_module_text(key);
	return value ? value : "";
}

} // namespace

std::string dummy_label_format()
{
	return text_of(keys::kSourcePlaceholderLabel);
}

std::string dummy_display_name(int slot)
{
	return fill_template(dummy_label_format(), {std::to_string(slot)});
}

} // namespace cordmux::sources

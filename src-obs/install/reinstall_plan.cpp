// SPDX-License-Identifier: Apache-2.0

// install/reinstall_plan.h の実装。

#include "install/reinstall_plan.h"

#include "install/build_mark.h"
#include "platform/bundle_check.h"

#include <utility>

namespace cordmux::install {

std::vector<CopyStep> plan_reinstall(std::string_view original_dir, std::string_view shown_dir,
				     const std::vector<std::string> &original_file_names)
{
	std::vector<CopyStep> steps;
	steps.reserve(original_file_names.size());

	const auto step_of = [&](const std::string &name) {
		CopyStep step;
		step.name = name;
		step.from = platform::join_path(original_dir, name);
		step.to = platform::join_path(shown_dir, name);
		return step;
	};

	// マーカー以外を渡された順に並べ、マーカーは最後に 1 つだけ追加する (ヘッダーのコメント)。
	bool has_mark = false;
	for (const std::string &name : original_file_names) {
		if (name == kBuildMarkFileName) {
			has_mark = true;
			continue;
		}
		steps.push_back(step_of(name));
	}
	if (has_mark)
		steps.push_back(step_of(kBuildMarkFileName));
	return steps;
}

} // namespace cordmux::install

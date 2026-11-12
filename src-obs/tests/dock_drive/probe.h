// SPDX-License-Identifier: Apache-2.0

// probe のシナリオ。テストの入力 (input.h) が製品のウィジェットに届くかを確認する。ほかのシナリオはどれも
// この入力の経路を使うので、ほかのシナリオより前に実行する (--scenario の既定は probe だけ)。

#pragma once

#include "dock/ui/dock_style.h"
#include "report.h"

namespace cordmux::dock_drive {

void run_probe(const dock::ui::DockStyle &style, Results &results);

} // namespace cordmux::dock_drive

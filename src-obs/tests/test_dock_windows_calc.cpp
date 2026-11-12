// SPDX-License-Identifier: Apache-2.0

// dock/windows_calc.h の単体テスト (設定ウィンドウの計算のうち、Qt に依存しない部分)。
// Qt を起動しない。ウィンドウのウィジェット (settings_window.cpp) は、プレビューツール (ui_preview) とドックを操作する
// テスト (dock_drive) が描画して確認する。

#include "doctest.h"

#include "config/settings.h"
#include "dock/windows_calc.h"
#include "ledger_dock_test_helpers.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace calc = cordmux::dock::windows_calc;

TEST_CASE("名前ラベルのフォントの大きさの px は、配信の画面の高さに割合を掛けて四捨五入した値になる")
{
	// 1920x1080 で 4.5% は 49 px。
	CHECK(calc::label_font_px(1080, 4.5) == 49);
	CHECK(calc::label_font_px(1080, 2.5) == 27);
	CHECK(calc::label_font_px(1080, 8.0) == 86);
	CHECK(calc::label_font_px(720, 4.5) == 32);
	CHECK(calc::label_font_px(2160, 4.5) == 97);
	// 画面の高さが分からないときは 0 (ウィンドウはツールチップを表示しない)。
	CHECK(calc::label_font_px(0, 4.5) == 0);
	CHECK(calc::label_font_px(-1, 4.5) == 0);
	CHECK(calc::label_font_px(1080, 0.0) == 0);
}

TEST_CASE("最大解像度の選択肢は制限なしが先頭で、一覧にない値は末尾に加える")
{
	const std::vector<int> choices = calc::long_edge_choices();
	REQUIRE(!choices.empty());
	CHECK(choices.front() == cordmux::config::kLimitMaxLongEdgeNone);
	CHECK(calc::with_value(choices, 1280) == choices);
	const std::vector<int> extended = calc::with_value(choices, 1600);
	CHECK(extended.size() == choices.size() + 1);
	CHECK(extended.back() == 1600);
	// どの選択肢も settings.json の範囲の中にある (ウィンドウが範囲の外の値を保存しないため)。
	for (const int value : choices) {
		if (value == cordmux::config::kLimitMaxLongEdgeNone)
			continue;
		CHECK(value >= cordmux::config::kLimitMaxLongEdgeMin);
		CHECK(value <= cordmux::config::kLimitMaxLongEdgeMax);
	}
}

TEST_CASE("入力欄の objectName のキーを「+」で分けられる")
{
	CHECK(calc::split_field_keys("assign_mode") == std::vector<std::string>{"assign_mode"});
	CHECK(calc::split_field_keys("self_frame_source_uuid+self_frame_source_name") ==
	      std::vector<std::string>{"self_frame_source_uuid", "self_frame_source_name"});
	CHECK(calc::split_field_keys("label_font_family+label_font_weight+label_font_italic").size() == 3);
}

TEST_CASE("[ツール] の「cordmuxをアンインストール...」は、ページのボタンと同じ判定で、押せるときだけ確認のダイアログを開く")
{
	using cordmux::ledger::UninstallPhase;
	using cordmux::ledger::VesktopProcess;
	using calc::UninstallEntry;
	namespace t = ledger_dock_test;

	// 押せる状態: 配信、OBS の録画、cordmux の録画をしておらず、Vesktop が起動していない。
	cordmux::ledger::Observation ok = t::healthy();
	ok.install.process = VesktopProcess::NotRunning;
	const auto entry_of = [](const cordmux::ledger::Observation &obs) {
		return calc::uninstall_entry(t::decide_dock(obs).about.uninstall_button);
	};
	REQUIRE_FALSE(t::decide_dock(ok).about.uninstall_button.disabled_reason.has_value());
	CHECK(entry_of(ok) == UninstallEntry::Confirm);

	// 押せない状態では、どれも確認のダイアログを開かずに理由を表示する (理由は、ページのボタンの下と同じ View の
	// テキスト)。
	std::vector<cordmux::ledger::Observation> blocked;
	{
		auto o = ok; // 配信中
		o.uninstall.obs_streaming = true;
		o.obs_outputs.streaming = true;
		blocked.push_back(o);
	}
	{
		auto o = ok; // OBS の録画中
		o.uninstall.obs_recording = true;
		o.obs_outputs.recording = true;
		blocked.push_back(o);
	}
	{
		auto o = ok; // cordmux の録画中
		o.choices.recording = cordmux::ledger::FeatureSwitch::On;
		blocked.push_back(o);
	}
	{
		auto o = ok; // Vesktop の起動中
		o.install.process = VesktopProcess::Running;
		blocked.push_back(o);
	}
	{
		auto o = ok; // 状態の確認中 (配信と録画の状態をまだ読めていない)
		o.uninstall.obs_streaming.reset();
		o.obs_outputs.streaming.reset();
		blocked.push_back(o);
	}
	{
		auto o = ok; // アンインストールを始めた後
		o.uninstall.phase = UninstallPhase::Running;
		blocked.push_back(o);
	}
	for (const auto &o : blocked) {
		const auto button = t::decide_dock(o).about.uninstall_button;
		CHECK(button.disabled_reason.has_value());
		CHECK(calc::uninstall_entry(button) == UninstallEntry::ShowReason);
	}
}

TEST_CASE("4 桁以上の数には 3 桁ごとにカンマを入れる")
{
	CHECK(calc::group_digits(0) == "0");
	CHECK(calc::group_digits(999) == "999");
	CHECK(calc::group_digits(1000) == "1,000");
	CHECK(calc::group_digits(3600) == "3,600");
	CHECK(calc::group_digits(123456) == "123,456");
	CHECK(calc::group_digits(1234567) == "1,234,567");
	CHECK(calc::percent_text(0.0) == "0.0");
	CHECK(calc::percent_text(12.345) == "12.3");
	CHECK(calc::percent_text(100.0) == "100.0");
}

TEST_CASE("受け取る映像の表のセルは、入力欄の値のバッファで直近 1 分を数え直す")
{
	cordmux::pipeline::ArrivalWindow w;
	w.delays_ms = {20, 150, 160, 30, 400, 10};
	w.mean_ms = 128.6;
	w.max_ms = 400.4;
	const std::vector<cordmux::pipeline::ArrivalWindow> streams = {w};
	const calc::ArrivalCells at100 = calc::arrival_cells(streams, 100);
	CHECK(at100.frames == 6);
	CHECK(at100.late == 3);
	CHECK(at100.stutters == 2);
	CHECK(at100.late_pct == doctest::Approx(50.0));
	CHECK(at100.mean_ms == 129);
	CHECK(at100.max_ms == 400);
	// 入力欄の値を変えると、同じ列から別の数になる。
	const calc::ArrivalCells at200 = calc::arrival_cells(streams, 200);
	CHECK(at200.late == 1);
	CHECK(at200.stutters == 1);
	CHECK(at200.late_pct == doctest::Approx(100.0 / 6.0));
	const calc::ArrivalCells at0 = calc::arrival_cells(streams, 0);
	CHECK(at0.late == 6);
	CHECK(at0.stutters == 1);
	// 平均と最大は入力欄の値に依らない。
	CHECK(at200.mean_ms == at100.mean_ms);
	CHECK(at200.max_ms == at100.max_ms);
	const calc::ArrivalCells none = calc::arrival_cells({}, 100);
	CHECK(none.frames == 0);
	CHECK(none.late_pct == 0.0);
}

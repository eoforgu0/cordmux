// SPDX-License-Identifier: Apache-2.0

// 設定ウィンドウと参加者の設定ウィンドウと状態タブの表のうち、Qt に依存しない計算と選択肢の一覧。
// ウィジェットを作らずに単体テスト (tests/test_dock_windows_calc.cpp) で確認できるように、ウィンドウの
// 実装 (settings_window.cpp) から分けてある。

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.h"
#include "ledger/dock_view.h"
#include "pipeline/arrival_window.h"

namespace cordmux::dock::windows_calc {

// 名前ラベルのフォントの大きさ (配信の画面の px)。1 em はキャンバスの高さの pct% で、cordmux.look.font_size.tip の
// %3 に入れる。キャンバスの高さが分からない (0 以下) か pct が 0 以下なら 0 を返し、呼び出し元はツールチップを表示しない。
inline int label_font_px(int canvas_height, double pct)
{
	if (canvas_height <= 0 || pct <= 0.0)
		return 0;
	return static_cast<int>(std::lround(static_cast<double>(canvas_height) * pct / 100.0));
}

// 受け取る映像の最大解像度 (長い辺) の選択肢。先頭は制限なし (0 = config::kLimitMaxLongEdgeNone) で、残りは大きい順。
// 一覧にない値 (settings.json を手で書いた値) は、呼び出し元がその値の項目を追加して表示する。近い値に丸めて表示すると、
// 利用者が変えていないのに値が変わる。
inline std::vector<int> long_edge_choices()
{
	return {config::kLimitMaxLongEdgeNone, 1920, 1280, 960, 640};
}

// 選択肢の一覧に value がなければ、追加した一覧を返す (並びは保ち、追加した値は末尾に置く)。
template <class T> inline std::vector<T> with_value(std::vector<T> choices, const T &value)
{
	for (const T &c : choices) {
		if (c == value)
			return choices;
	}
	choices.push_back(value);
	return choices;
}

// 入力欄の objectName は `cordmuxField_<json_key>` で、ドックを操作するテストがこの名前で入力欄を探す。2 つ以上の
// キーを持つ入力欄は、キーを「+」でつなぐ (`<キー>+<キー>`)。この関数は、つないだキーを分けて返す。
inline std::vector<std::string> split_field_keys(const std::string &joined)
{
	std::vector<std::string> keys;
	std::string current;
	for (const char c : joined) {
		if (c == '+') {
			keys.push_back(current);
			current.clear();
		} else {
			current.push_back(c);
		}
	}
	keys.push_back(current);
	return keys;
}

// 「cordmux について」のページの [cordmuxをアンインストール...] を押したとき、または OBS の [ツール] メニューの
// 「cordmuxをアンインストール...」を選んだときに、次に何をするか。押せない理由がなければ確認のダイアログを開く。
// 押せない理由があれば (配信中、OBS の録画中、cordmux の録画中、Vesktop の起動中、状態の確認中、アンインストールを
// 始めた後)、確認のダイアログを開かずに、ボタンの下の理由を表示する。
// ページのボタンと [ツール] の項目のどちらから来ても同じ判定になるように、両方をこの関数で決める。[ツール] の側で
// 配信と録画を別に問い合わせると、ページのボタンと食い違う判定がもう 1 つできる。どの状態で押せないかは ledger の
// View (AboutPageView::uninstall_button) が決め、ウィンドウは理由の有無だけを見る。
enum class UninstallEntry {
	Confirm,    // 確認のダイアログを開く (ページのボタンを押したのと同じ)
	ShowReason, // 確認のダイアログを開かず、ページのいちばん下 (ボタンと理由) を表示する
};

inline UninstallEntry uninstall_entry(const ledger::dockview::ViewButton &button)
{
	return button.disabled_reason.has_value() ? UninstallEntry::ShowReason : UninstallEntry::Confirm;
}

// 4 桁以上の数に 3 桁ごとのカンマを入れる (言語によらず同じ形)。
inline std::string group_digits(std::uint64_t value)
{
	std::string digits = std::to_string(value);
	std::string out;
	for (std::size_t i = 0; i < digits.size(); ++i) {
		if (i != 0 && (digits.size() - i) % 3 == 0)
			out += ',';
		out += digits[i];
	}
	return out;
}

// 状態タブの映像の受信の表の 1 行のセル。映像のバッファを buffer_ms にしていたら、直近 1 分のフレームのうち何が間に合わなかったか。
// 率は 0 から 100 の百分率、平均と最大の遅れは ms の整数 (OBS の統計ウィンドウは小数 1 桁だが、遅れは 1 ms 単位で足りる)。
struct ArrivalCells {
	std::uint64_t frames = 0;
	std::uint64_t late = 0;
	std::uint64_t stutters = 0; // 続けて遅れたまとまりの数
	double late_pct = 0.0;
	long long mean_ms = 0;
	long long max_ms = 0;
};

inline ArrivalCells arrival_cells(const std::vector<pipeline::ArrivalWindow> &streams, int buffer_ms)
{
	const pipeline::LateCount count = pipeline::late_count(streams, buffer_ms);
	ArrivalCells out;
	out.frames = count.frames;
	out.late = count.late;
	out.stutters = count.runs;
	out.late_pct = 100.0 * count.fraction();
	out.mean_ms = std::llround(pipeline::mean_delay_ms(streams));
	out.max_ms = std::llround(pipeline::max_delay_ms(streams));
	return out;
}

// 率の表示 (小数 1 桁。OBS の統計ウィンドウと同じ)。
inline std::string percent_text(double pct)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f", pct);
	return buf;
}

} // namespace cordmux::dock::windows_calc

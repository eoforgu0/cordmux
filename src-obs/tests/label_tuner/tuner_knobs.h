// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールのつまみの一覧と、つまみの値の保存と読み込み。つまみは合成のソースの名前ラベルと話している人の
// 光に反映される設定のキー全部で、値は config::Settings に直接読み書きする。範囲は src-obs/config/settings.h の定数を
// そのまま使う。刻みは、設定が刻みを決めているキー (フォントサイズの 0.1) はその刻み、決めていないキーはこのツールが
// 決めた刻み (KnobDef::divisor) にする。Qt Widgets には依存しない。

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "config/settings.h"

namespace cordmux::label_tuner {

// 画面に描くキャンバスの大きさ。フォントサイズのヒントの px はこの高さで求める。
inline constexpr int kPreviewCanvasWidth = 1920;
inline constexpr int kPreviewCanvasHeight = 1080;

enum class KnobKind {
	Number, // 数。スライダーと数の欄で動かす
	Toggle, // チェックボックス
	Color,  // `#RRGGBB`
	Font,   // フォントの名前 (空なら製品の既定のフォント)
	Choice, // 決まった選択肢から 1 つ (コンボボックス)
};

struct KnobDef {
	const char *key;   // 設定のキー (保存する JSON のキーと同じ)
	const char *group; // まとまりの見出し
	const char *title; // つまみの名前 (チェックボックスは機能の名前)
	KnobKind kind = KnobKind::Number;
	// 数のつまみの範囲と刻み。値は「整数 / divisor」で、刻みは 1 / divisor。unit は数の欄の後ろに付ける単位。
	double min = 0.0;
	double max = 0.0;
	int divisor = 1;
	// 刻みの段数。刻みは step / divisor (既定の 1 なら 1 / divisor)。5 刻みの暗さは divisor 1、step 5。
	int step = 1;
	const char *unit = "";
	// ヒント (ツールチップ)。説明は名前に詰めずにここへ書く。値で変わるヒントは get_hint が作る (null なら hint)。
	const char *hint = "";
	std::function<std::string(const config::Settings &)> get_hint;
	// 値が空でなければ、同じ値が続くチェックボックスを、この名前の 1 行にまとめて並べる。
	const char *row = nullptr;
	std::function<double(const config::Settings &)> get_number;
	std::function<void(config::Settings &, double)> set_number;
	std::function<bool(const config::Settings &)> get_toggle;
	std::function<void(config::Settings &, bool)> set_toggle;
	std::function<std::string(const config::Settings &)> get_text; // 色とフォント
	std::function<void(config::Settings &, const std::string &)> set_text;
	// 選択肢 (Choice)。options は画面に出す名前で、get_choice と set_choice は options の添字を読み書きする。
	std::vector<const char *> options;
	std::function<int(const config::Settings &)> get_choice;
	std::function<void(config::Settings &, int)> set_choice;
	// 選択肢の添字 index が、現在の設定で選べるか (null ならどれも選べる)。選べない選択肢は、画面で押せなくする。
	std::function<bool(const config::Settings &, int)> choice_allowed;
};

// つまみの一覧 (画面に並べる順)。
const std::vector<KnobDef> &knob_defs();

// 現在の設定でのつまみのヒント。なければ空。
std::string knob_hint(const KnobDef &knob, const config::Settings &settings);

// 数のつまみの値を刻みに合わせる (整数の段 k を k / divisor にする)。範囲の外は端にする。
double snap_to_step(const KnobDef &knob, double value);

// つまみのキーだけを持つ JSON の文字列 (2 段の字下げ)。値の書き方は設定のファイル (config::settings_to_json) と同じ。
std::string knobs_to_json(const config::Settings &settings);

// path の JSON を読んで *out に入れる。キーがなければ製品の既定値、形の違う値が 1 つでもあれば全部を既定値にして
// false を返す (config::parse_settings と同じ)。error は人が読む理由。
bool load_knobs(const std::string &json_text, config::Settings *out, std::string *error);

// 保存する JSON の中身を作る。existing_text は保存先にいまある中身 (なければ空)。つまみのキーの値だけを置き換え、
// ほかのキーは残す (保存先を設定のファイルと取り違えても、ほかの設定を消さない)。existing_text が JSON の
// オブジェクトとして読めなければ、書かずに false を返す。
bool merge_knobs_into(const std::string &existing_text, const config::Settings &settings, std::string *out,
		      std::string *error);

// つまみの値が、範囲と刻みの中にあるか (config::clamp_settings と、刻みに合わせても値が変わらないか)。
bool knobs_within_range(const config::Settings &settings, std::string *error);

} // namespace cordmux::label_tuner

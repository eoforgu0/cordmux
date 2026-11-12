// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールの、つまみ以外の状態 (人数と人、背景、等倍で見るタイル) と、--shot で使う
// 話す時刻と描く時刻。--state の文字列から作る (書き方は README.md の「--state」)。

#pragma once

#include <array>
#include <vector>

#include <QString>

#include "sources/decor_paint.h"
#include "sources/composite_layout.h"

namespace cordmux::label_tuner {

// 背景の選び方。Mixed は見本と同じく枠の位置ごとに 4 種類を使い分け、Kind はどの枠も background_kind の 1 種類、
// Picture は開いた画像のファイルを、製品が映像を置くのと同じく縦横比を保って枠に収める。
enum class BackgroundMode { Mixed, Kind, Picture };

// 話している区間 (起動からの ms)。to_ms が負なら、話したまま。
struct SpeakSpan {
	int from_ms = 0;
	int to_ms = -1;
};

struct SeatState {
	int person = 0; // decor_preview::scene_people() の添字
	// --shot だけが使う。話している区間と、「2 秒話して 1 秒黙る」を始める時刻 (負なら繰り返さない)。
	std::vector<SpeakSpan> speak;
	int cycle_from_ms = -1;
};

// 画面共有していない参加者の最大の人数 (声の枠の数と同じ)。
inline constexpr int kRosterRows = 8;

// 画面共有していない参加者 1 人の行。person は decor_preview::scene_people() の添字。speak と cycle_from_ms は
// SeatState と同じ意味 (--shot だけが使う)。
struct RosterRowState {
	int person = 0;
	std::vector<SpeakSpan> speak;
	int cycle_from_ms = -1;
};

struct ToolState {
	int people = sources::kCompositeCellCount;
	std::array<SeatState, sources::kCompositeCellCount> seats;
	// 画面共有していない参加者の人数 (0 から kRosterRows) と行。既定は 0 人 (並びを出さない)。
	int roster = 0;
	std::array<RosterRowState, kRosterRows> roster_rows;
	// 並びの最後の 1 人を、cordmux が声を聞けない人 (声の枠が埋まっている人) にする。
	bool roster_last_unheard = false;
	BackgroundMode background = BackgroundMode::Mixed;
	int background_kind = 0;
	QString picture_path;
	int view_tile = 0; // 等倍と 2 倍で見るタイル (左上、右上、左下、右下の順の添字)
	int view_roster = -1; // 等倍と 2 倍で見る、画面共有していない参加者の行 (0 から。負ならタイルを見る)
	// --shot だけが使う。描く時刻 (起動からの ms)、ウィンドウの大きさ、つまみの並びで見えるようにするつまみのキー、
	// 書き出した後に 1 フレームの時間を測るフレームの数 (0 なら測らない)。
	int at_ms = 10000;
	int window_w = 1600;
	int window_h = 1000;
	QString focus_key;
	int bench_frames = 0;
	// 書き出す前に押すボタン ("save" は [保存]、"reset" は [既定に戻す]。空なら押さない)。
	QString press;
};

// 既定の状態。人は見本 (decor_preview --composite-sample) と同じ 4 人。
ToolState default_tool_state();

// --state の文字列を state に重ねる。書き方が違えば false を返し、error に理由を書く (state は途中まで変わる)。
bool parse_state(const QString &text, ToolState *state, QString *error);

// 名前ラベルの隅の、画面に出す名前と --state の書き方。
QString corner_title(sources::DecorCorner corner);

} // namespace cordmux::label_tuner

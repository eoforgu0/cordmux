// SPDX-License-Identifier: Apache-2.0

// 参加者の一覧の行の幅と省略の計算のうち、Qt に依存しない部分。tests/test_dock_rows_calc.cpp が単体テストする。
// 名前の見分け、2 行目の部分の残し方、狭いときに消す項目、名前と状態のテキストの幅の分け方、状態のテキストの
// 短い形、クリックを捨てる時間、[...] の周りの間隔を扱う。行のウィジェットは person_row.h にある。
// テキストは UTF-16 の並び (Qt の QString と同じ単位) で受け取る。幅は px で、使う側が QFontMetrics で測った値を渡す。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cordmux::dock::rows {

// ---- 名前の見分け ----

// 1 行の名前の見え方。visible_prefix は、末尾を省いたテキストのうち省略記号より前の部分 (省いていなければ全文)。
struct NameShown {
	std::u16string full;
	std::u16string visible_prefix;
	bool elided = false;
};

// 真ん中を省く表示に替える行に true を返す。替えるのは末尾を省いた行のうち、見えているテキスト (省略記号の前) が
// ほかの人の名前の先頭にも一致する行だけ (見えているテキストがほかの行と同じになる場合も含む)。
// 全文が見えている行は、それだけで見分けられるので替えない。
std::vector<bool> middle_elide_rows(const std::vector<NameShown> &rows);

// ---- 2 行目の部分 ----
// 2 行目は名前の下の行で、部分 (声、Discord での名前、ユーザー名など) を別々のラベルとして横に並べる。狭いときに
// 部分ごとに落とせるように、部分を区切りの文字でつながない。

// 部分の種類。ini のキーから決める (part_kind_of)。
enum class PartKind {
	Voice,       // 声: alice / 声: alice (通話に参加していません)。この人の枠で使う声を示す
	DiscordName, // Discord での名前: alice (cordmux での名前を付けた人)
	Username,    // ユーザー名: alice_1 (同じ名前の人の見分け)
	Other,       // それ以外 (cordmux.dock.row_receiving_account.status ほか)。末尾を省く
};

PartKind part_kind_of(const std::string &key);

enum class ElideMode { None, End, Middle };

struct PartIn {
	PartKind kind = PartKind::Other;
	int full_px = 0;  // 全文の幅
	int short_px = -1; // 短い形の幅 (ユーザー名の語を落として値だけにした形)。なければ -1
	// 省いても中身が読める最小の幅 (使う側が測る)。末尾を省く部分は、「声:」のような語と値の最初の 1 字と省略記号の
	// 幅 (「声: a...」)。真ん中を省くユーザー名は、最初の 1 字と省略記号と最後の 1 字の幅 (「a...1」)。短い形があるときは、
	// fit_line2 が幅を分ける時点では短い形に替えてあるので、短い形で測る。0 なら下限なし。
	int head_px = 0;
};

struct PartOut {
	bool shown = true;
	bool use_short = false;
	int width_px = 0; // ラベルに与える幅 (省かないならテキストの幅そのもの)
	ElideMode elide = ElideMode::None;
};

// 2 行目の部分を available_px に収める。gap_px は部分の間 (大きい余白)。並びは変えない。
// 次の順に試し、収まったところで止める。
//   1. 全部が全文で入れば、そのまま。
//   2. ほかの部分があるときは、Discord での名前の部分を落とす (見分けにも、使う声の表示にも必要のない部分から落とす)。
//   3. ユーザー名を値だけの短い形にする。
//   4. 残った部分で幅を分け、ユーザー名は真ん中を、ほかは末尾を省く (ユーザー名は見分けに要る末尾を残す)。
//      分け方は等しい水位にする。短い部分は全文のまま残し、余りを長い部分で等しく分ける。ただし全部の部分の
//      head_px が入る幅なら、どの部分も head_px を下回らせない (「声: ...」のように誰の声かが消える形を作らない)。
//      head_px も全部は入らない幅で、ユーザー名とほかの部分が並んでいれば、1 つだけを残してほかを丸ごと落とし、
//      残りの幅を全部その部分に与える (入らなければ、声は末尾を、ユーザー名は真ん中を省く)。声があれば声を残し、
//      ユーザー名を落とす (1 行目に名前があるので、ユーザー名は同じ名前の人を見分ける補い)。声がなければユーザー名を
//      残す。落とした部分は、使う側がツールチップと accessibleDescription に入れる。ユーザー名がなければ、
//      どれかを落とす理由がないので、head_px の下限を使わずに等しい水位で分ける。
std::vector<PartOut> fit_line2(const std::vector<PartIn> &parts, int available_px, int gap_px);

// ---- 狭いときに消す項目 ----
// 境目はドックの幅ではなく、一覧の行が実際に使える幅 (スクロールバーを除いた幅) で決め、全部の行に同じ結果を
// 適用する。行ごとに表示したり消したりすると、同じ情報が縦にそろわない。

// 解像度を表示する境目 (行が使える幅の em)。目安の値で、測って決めたものではない。見た目を決め直すときは、
// この 1 か所だけを変える。
inline constexpr double kResolutionMinEm = 32.0;

struct WidthRules {
	bool status_text = true; // 行の状態のテキスト (アイコンは残す)
	bool resolution = true;  // 解像度とフレームレート
	bool avatar = true;      // アバター
};

// 行が使える幅 (px) から、全部の行に適用する結果を決める。status_text_min_px は status_text_min_width の値、
// resolution_min_px は kResolutionMinEm を px にした値、avatar_min_px は avatar_min_width の値 (省くと常に表示する)。
WidthRules width_rules(int row_px, int status_text_min_px, int resolution_min_px, int avatar_min_px = 0);

// アバターを表示する境目 (px)。狭くすると、状態のテキストより先にアバターが消える。アバターがなくても名前で人が分かるが、状態の
// テキストは消すと意味を失う。アバターがある行は、アバターで人を見分けられるので、名前に残す幅 name_floor_with_avatar の分だけ
// 名前を狭くしてよい。この分を境目から引く。
int avatar_min_width(int status_text_min_px, int avatar_px, int gap_px, int name_floor_px);
// アバターを表示している行で、名前に残す幅。下限の 4 分の 3 にする。26 em のドックで、アバターと状態のテキストと名前を同時に
// 置くためである。
int name_floor_with_avatar(int name_floor_px);

// 行の状態のテキストを表示する境目 (px)。
//   チェックとその後の空き + 名前の幅の下限 (4 em) + 大きい余白 + 状態のアイコン + 小さい余白 +
//   状態のテキストに残す幅 (kStatusTextKeepEm) + 共有の列の前の空き (大きい余白) + 共有のアイコン +
//   共有のアイコンと [...] の間 + [...]
// = 一覧の 1 行の最小幅 (min_row_width) + 状態のテキストに残す幅 + 大きい余白。状態のアイコンの後の小さい余白は
// 最小幅にも含まれ、テキストの後に小さい余白と大きい余白を足すので、大きい余白が 1 つ増える。この幅なら、名前は下限、
// 状態のテキストは残す幅で、どちらも削らずに 1 行に入る (split_name_status と同じ数え方)。
// 目安の em ではなく中身から計算するのは、空いている幅があるのに全部の行で状態のテキストが消えないようにするため。
int status_text_min_width(int min_row_px, int status_keep_px, int large_px);

// ---- 名前と行の状態のテキストの幅の分け方 ----
// 狭いときは、名前を最後まで残し、状態のテキストを先に縮める。

// 行の状態のテキストを表示している行で、名前の全文を残すために状態のテキストを縮めるときに、状態のテキストに
// 残す幅 (em)。全部の行で状態のテキストを消さずに名前を先に残すための下限で、状態のテキストを表示する境目
// (status_text_min_width) もこの幅で数える。見た目を決め直すときは、この 1 か所だけを変える。
// 状態のテキストの先頭で何の状態かが読める幅にする。日本語の状態は、全部が短い形 (5 字まで。「映像待ち」「映像途切れ」
// 「画質低下」など。status_short_key_for) を省かずに表示する。
// 5.75 em は測って決めた値である (短い形ではなく、全文の先頭を省いて表示していた時期に測った)。Yami、フォントサイズ 10
// (1 em は 13.33 px) でラベルの幅を 1 px ずつ変えて描くと、日本語の先頭が全部そろういちばん狭い幅は 77 px (「映像を待って...」が
// 入る幅) で、5.75 em は 77 px になる。短い形の最長 (5 字) は 5 em (67 px) で、この幅に入る。
// 6 em (80 px) にすると、267 px のドックでスクロールバーが出たときに行の幅 (233 px) が境目 (236 px) を下回り、
// 全部の行で状態のテキストが消える。5 em まで下げると、短い形の長さに合うが、この境目を測り直していないので変えていない。
// 英語の状態は全文のまま (一部の短い形を除く) なので、狭いときは末尾が省かれる (「Lower fra...」「Lower reso...」
// 「Lower qual...」)。それで読めるかは確認していない。
inline constexpr double kStatusTextKeepEm = 5.75;

struct NameStatusSplit {
	int name_px = 0;   // 名前のラベルの幅
	int status_px = 0; // 状態のテキストのラベルの幅 (状態のテキストの全文の幅を超えない)
};

// 名前のラベルと状態のテキストのラベルが分け合う幅 text_px を分ける。
//   - 両方の全文の幅が入れば、どちらも全文にする (余りは名前に与える)。
//   - 入らなければ、名前 = min(名前の全文の幅, max(名前の幅の下限, text_px - 状態のテキストの下限))、
//     状態のテキスト = 残り (末尾を省く)。状態のテキストの下限 = min(状態のテキストの全文の幅, status_keep_px)。
//     名前の幅の下限は name_floor_px (4 em)。
// text_px を広げたときに、名前も状態のテキストも狭くならない。
// status_natural_px が 0 以下 (状態のテキストを表示していない) なら、全部を名前に与える。
NameStatusSplit split_name_status(int text_px, int name_natural_px, int status_natural_px, int name_floor_px,
				  int status_keep_px);

// ---- 行の状態のテキストの短い形 ----

// 状態のテキストのキー (行の状態のテキストか画質のテキストのキー) から、行に表示する短い形のキーを返す。短い形のない
// テキスト (載せられなかったことを言うテキストなど) は nullptr。
// 行には、幅にかかわらず短い形を表示する (狭い所で文が省かれて、「映像が届いて...」のように逆の意味の先頭が残ることも、
// 全文と短い形が幅によって入れ替わることもない)。短い形 (「映像待ち」「映像途切れ」「共有なし」「画質低下」「表示不可」
// 「映像なし」など) は 5 字までで、kStatusTextKeepEm の幅に省かずに入る。全文は、ツールチップの 1 行目とアクセシブル名に残る。
// 言語によっては短い形が全文と同じテキストになる (英語は語順で先頭が読める状態)。そのときの扱いは使う側が決める。
// Qt に依存しないテストで確認できるように、キーの対応は person_row.cpp ではなくここに置く。状態の種類 (RowState)
// ではなくテキストのキーで引くのは、「表示できない」が RowState ではなく画質のテキストの側にあり、キーで引けば
// 1 つの関数で済むためである。
const char *status_short_key_for(const std::string &status_key);

// ---- クリックを捨てる時間 ----

// 現れた時刻か最後に動いた時刻 (単調時計の ms) から interval_ms の間は true (クリックを受け付けない) を返す。
// 時刻がない (0 以下) ときと、interval_ms が 0 以下のときは false。
bool press_blocked(std::int64_t changed_at_ms, std::int64_t now_ms, int interval_ms);

// ---- [...] の周りの間隔 ----
// [...] は OBS の btn-tool の大きさ (24 px 未満) のままにするので、WCAG 2.5.8 (ターゲットのサイズ) は間隔の例外で満たす。

// [...] の中心に置いた直径 24 px の円が、左のチェックの押せる範囲と交わらないか。check_right は
// チェックの押せる範囲の右端 (含まない端)、dots_left と dots_width は [...] の左端と幅 (同じ座標)。
bool dots_circle_clear(int check_right, int dots_left, int dots_width);

// ---- 最小幅 ----

// 行のアバターの大きさ (px)。名前ラベルに渡している画像を、この大きさの円にして表示する。
inline constexpr int kRowAvatarPx = 20;

// 一覧の 1 行に必要な最小の幅。チェックボックスのインジケーターと名前までの空き + 名前の幅の下限 + 状態のアイコン +
// 画面共有のアイコンの列 + [...] に、間の余白を足す。名前と、その右のまとまり (状態のアイコンから [...] まで) の間は
// large_px、状態のアイコンと共有のアイコンの間は small_px、共有のアイコンと [...] の間は dots_gap_px。
int min_row_width(int indicator_and_gap_px, int name_floor_px, int mark_px, int dots_px, int small_px, int large_px,
		  int dots_gap_px);

// 行のミュートのボタン 2 つ (声と画面共有の音) が行の最小の幅に足す幅。画面共有の音のボタンは、その人が画面共有の音を
// 持たない間も場所を空ける (声のボタンの位置を動かさないため)ので、2 つ分を数える。間は小さい余白。
int audio_buttons_px(int button_px, int small_px);

// 共有のアイコンと [...] の間。[...] にフォーカスがあるときに、フォーカスリング (空き + 線) の外に背景の色が 1 px 以上
// 残る幅にする。大きい余白を使い、小さいフォントで大きい余白がリング + 1 px を下回るときだけ、リング + 1 px にする。
// 小さい余白にすると、リングの左の辺が共有のアイコンに接し、接した所のコントラスト比が足りなくなる。リングの外に
// 背景の色の枠線を描く方法は、ほかの場所のフォーカスリングまで見た目が変わるので使わない。
int dots_gap_px(int large_px, int ring_px, int ring_gap_px);

// 一覧の行どうしの間の下限。基本は小さい余白 (0.25 em) にする。上下の行の [...] は行の高さ全部を使うことがあり
// (行の高さは [...] の sizeHint を含む最大 = PersonRowWidget::line1_height)、そのときは行の間がそのまま [...] どうしの
// 間になる。フォーカスリング (空き + 線) の外に背景の色が 1 px 以上残るように、小さい余白とリング + 1 px の大きい方に
// する (dots_gap_px と同じ考え方)。10 pt では小さい余白が 3 px に丸まり、3 px のリングで使い切って、下の行の [...] の
// 背景にリングが接する。大きい余白にすると行の間が 0.5 em になり、8 人が 1 画面に入りにくい。
int row_gap_px(int small_px, int ring_px, int ring_gap_px);

} // namespace cordmux::dock::rows

// SPDX-License-Identifier: Apache-2.0

// ドック、設定ウィンドウ、参加者の設定ウィンドウ、ダイアログに表示する内容の型 (名前空間 dockview)。
// decide_dock() (ledger/rules.h) が Observation から作る。テキストは持たず、ini のキー (TextRef) で持つ。
// ドックは前の値と比べて変わったウィジェットだけを書き換えるので、どの型も operator== を持つ。C++17 なので
// フィールドごとに手で書く。フィールドを追加したら、その型の operator== にも追加する
// (tests/test_ledger_dock_view.cpp がフィールドを 1 つずつ変えて確認するので、追加し忘れるとテストが失敗する)。
// libobs にも Qt にも依存しない。

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "ledger/actions.h"
#include "ledger/observation.h"
#include "ledger/view.h"

namespace cordmux::ledger {

// TextRef と Label の等値比較は ledger/view.h にある。

namespace dockview {

// ---- 日時の引数 ----
//
// TextRef::args は文字列のままにし、日時はマーカーを付けた値で入れる。表示する側 (dock/ui/text_resolve.h の
// resolve) はマーカーを見つけたら、Windows の地域の設定に従う短い形 (QLocale) にする。秒は表示しない。
// 値は、制御文字 U+001F (名前などの文字列に現れない) と種類の 1 文字 (T = 時と分、D = 日付と時刻) の後ろに、システム時刻のエポックからのマイクロ秒を 10 進で書いたものである。
// 表示の規則の側では日時を文字列にしない。表示の規則は Qt に依存しないので、地域の設定に従った形にできない。

inline constexpr char kTimeArgMark = '\x1F';

enum class TimeArgKind {
	Time,     // 時と分 (今日起きたこと)
	DateTime, // 日付と時刻 (今日でない日に起きたこと。Time とどちらにするかは表示の規則が選ぶ)
};

// 時と分で表示する日時の引数を返す。epoch_us はシステム時刻のマイクロ秒。
inline std::string time_arg(std::int64_t epoch_us)
{
	std::string out(1, kTimeArgMark);
	out += 'T';
	out += std::to_string(epoch_us);
	return out;
}

// 日付と時刻で表示する日時の引数を返す。
inline std::string date_time_arg(std::int64_t epoch_us)
{
	std::string out(1, kTimeArgMark);
	out += 'D';
	out += std::to_string(epoch_us);
	return out;
}

// arg が日時の引数なら、kind と epoch_us に値を入れて true を返す (どちらも nullptr でよい)。マーカーがないか
// 形が違えば false を返し、表示する側は arg をそのまま文字列として表示する。値の形を読むのはこの関数だけにする。
inline bool parse_time_arg(const std::string &arg, TimeArgKind *kind, std::int64_t *epoch_us)
{
	if (arg.size() < 3 || arg[0] != kTimeArgMark)
		return false;
	TimeArgKind parsed_kind = TimeArgKind::Time;
	if (arg[1] == 'T')
		parsed_kind = TimeArgKind::Time;
	else if (arg[1] == 'D')
		parsed_kind = TimeArgKind::DateTime;
	else
		return false;
	std::int64_t value = 0;
	const char *first = arg.data() + 2;
	const char *last = arg.data() + arg.size();
	const std::from_chars_result r = std::from_chars(first, last, value);
	if (r.ec != std::errc() || r.ptr != last)
		return false;
	if (kind)
		*kind = parsed_kind;
	if (epoch_us)
		*epoch_us = value;
	return true;
}

// ---- 共通の型 ----

// メッセージのレベル。Tier::L1 がいちばん重大である。決め方は docs/ui_guidelines.md の「メッセージのレベルと表示方法」にある。
enum class Tier { L1, L2, L3 };

// メッセージを OBS のログに書くときのログレベル。メッセージのレベル 1 は Error、メッセージのレベル 2 と、メッセージを表示しない
// 操作の失敗は Warning、ほかは Info にする。
enum class LogLevel { Error, Warning, Info };

// メッセージと人の行の状態を、表示するまでと消すまでに待つかどうか。待つのは ledger/notice_tracker.h の stabilize()。
//   Immediate  待たない (既定)。
//   Debounced  表示する前と消す前の両方を待つ。ハートビートのように、しきい値を持たずに推測する状態に使う。
//   ClearOnly  表示する前の待ちは規則のしきい値そのもの (映像が止まったとみなすしきい値、最初のフレームを待つ
//              しきい値) で、消すときだけ待つ。
enum class Settle { Immediate, Debounced, ClearOnly };

// ボタンかメニュー項目 1 つ。押せるかどうかと押せない理由は ledger::apply() (ledger/actions.h) が決め、
// disabled_reason に入る (持たない = 押せる)。primary は、次に押すボタンとして目立たせるプライマリのボタンか
// (mark_primary_button() がメッセージの並びの中で 1 つだけにする)。
struct ViewButton {
	ActionId id = ActionId::ExportBundle;
	TextRef label;
	ActionArgs args;
	std::optional<TextRef> disabled_reason;
	bool primary = false;
};

inline bool operator==(const ViewButton &a, const ViewButton &b)
{
	return a.id == b.id && a.label == b.label && a.args == b.args && a.disabled_reason == b.disabled_reason &&
	       a.primary == b.primary;
}

inline bool operator!=(const ViewButton &a, const ViewButton &b)
{
	return !(a == b);
}

// メッセージのカードに表示する、準備の段階 (Vesktop との接続、通話への参加、シーン) の「名前: 値」の 1 行
// (例「Vesktop: 接続されていません」= cordmux.dock.stage_vesktop_disconnected.status)。テキストは ini の 1 つの
// キーで持ち、名前と値に分けない。
// 名前と値を別のキーにしてドックが「: 」でつなぐと、翻訳する単位をコードで組み立てることになる。
struct StageItem {
	TextRef text;
};

inline bool operator==(const StageItem &a, const StageItem &b)
{
	return a.text == b.text;
}

inline bool operator!=(const StageItem &a, const StageItem &b)
{
	return !(a == b);
}

// 場所の行の種類。Folder はフォルダー、File はファイル 1 つ。File になるのは、アンインストールで残ったもののうち
// ファイル 1 つのもの (Vesktop の設定のファイル state.json など) と、診断情報のエクスポートの結果の zip である。
enum class LocationKind { Folder, File };

// 場所の行。パスを選んでコピーできる形で 1 行に表示する。path は UTF-8 の絶対パス。
// open_folder は、Folder の行に [フォルダーを開く] を添えるかどうか。File の行には、表示する側が kind を見て
// [ファイルの場所を開く] (cordmux.dialog.open_file_location.button。エクスプローラーでそのファイルを選んだ
// 状態で開く。platform/platform.h の open_file_location) を置き、open_folder は使わない。File の行の open_folder は
// false にしておく。kind を読まない表示する側が、ファイルのパスをフォルダーとして開くボタンを表示しないためである。
struct LocationLine {
	std::string path;
	bool open_folder = true;
	LocationKind kind = LocationKind::Folder;
};

inline bool operator==(const LocationLine &a, const LocationLine &b)
{
	return a.path == b.path && a.open_folder == b.open_folder && a.kind == b.kind;
}

inline bool operator!=(const LocationLine &a, const LocationLine &b)
{
	return !(a == b);
}

// 本文の段落の間に置く場所の行。今は、アンインストールが途中で止まったときのカードだけが使う。このカードは
// ダイアログと同じく残ったものの種類ごとのまとまりで組み立て、まとまりに残った場所の数だけ、項目や注意の直後に
// 場所の行を置く。after は、Notice::body の何番目 (0 始まり) の段落の直後に置くか。同じ after の行は並びの順に
// 続けて置く。after が body の長さ以上の行は表示しない (表示する側がクラッシュしないための約束)。
struct BodyLocation {
	std::size_t after = 0;
	LocationLine line;
};

inline bool operator==(const BodyLocation &a, const BodyLocation &b)
{
	return a.after == b.after && a.line == b.line;
}

inline bool operator!=(const BodyLocation &a, const BodyLocation &b)
{
	return !(a == b);
}

// ---- 1 行目 ----

// ドックの 1 行目に表示しているものの種類。メッセージの見出し、平常の要約、利用者が自分で選んだ状態 (無効、全画面、ダミー)、
// 処理中のどれか。
enum class FirstLineKind { Notice, Summary, UserChoice, Working };

struct FirstLine {
	FirstLineKind kind = FirstLineKind::Summary;

	// kind == Notice のときだけ値を持つ。
	std::optional<Tier> tier;

	// 表示するテキストの候補を長い順に並べたもの。どれも ini の 1 つのキーの完全なテキストである (例:
	// 「通話中 (%1人) - 表示中 (%2人)」の次に「通話中 (%1人)」)。ドックは幅に入る最初の候補を表示し、どれも
	// 入らないときだけ最後の候補の末尾を省略する。区切りの「 - 」を含めて ini に持ち、短い形も ini に持つ。
	// 区切りをドックがコードでつなぐと、翻訳する単位をコードで組み立てることになる。
	std::vector<TextRef> forms;

	// メッセージのレベル 1 の残りの件数 (cordmux.dock.more_notices.status)。幅が足りなくても省略しない。
	std::optional<TextRef> more_count;

	// 1 行目に添えるボタン (無効にしている間の [有効にする] と、1 人を全画面で表示している間の [元に戻す])。
	std::optional<ViewButton> button;

	// cordmux を無効にしている間の「cordmuxは無効です」の行か。真の行は、未読のレベル 2 のメッセージより優先して 1 行目に出す
	// (幅によらず必ず出す。レベル 1 だけがこれより優先する)。無効の間は、レベル 2 を既読にしない。
	bool disabled = false;

	// kind == Notice のとき、見出しを表示しているメッセージの Notice::key。ドックはそのメッセージの本文の先頭に
	// 見出しの全文を表示する (1 行目で省略されても読めるようにするため)。
	std::string notice_key;
};

inline bool operator==(const FirstLine &a, const FirstLine &b)
{
	return a.kind == b.kind && a.tier == b.tier && a.forms == b.forms && a.more_count == b.more_count &&
	       a.button == b.button && a.disabled == b.disabled && a.notice_key == b.notice_key;
}

inline bool operator!=(const FirstLine &a, const FirstLine &b)
{
	return !(a == b);
}

// ---- メッセージ ----

struct Notice {
	// 周期をまたいで同じメッセージを見分けるキー。`<rule_name>` か `<rule_name>:<対象>` (対象は userId か枠の番号。
	// 何人かを 1 つにまとめたメッセージは対象なし)。表示するまでの待ち、並べる順、スクリーンリーダーへの通知、
	// ログの行に使う。
	std::string key;

	// 規則の英語の名前 (列挙子の名前のまま。例 `AllVideoStoppedHint`)。ログに書く。
	std::string rule_name;

	// ログに書く対象 (枠に載っている人なら `slot <n>`、ほかは空)。ログはほかの人に渡されることがあるので、
	// userId と名前は入れない。
	std::string log_subject;

	// 既定は Tier::L3 (いちばん軽い)。規則は必ずメッセージのレベルを入れる (tests/test_ledger_tiers.cpp が
	// 規則ごとのメッセージのレベルを確認する)。
	Tier tier = Tier::L3;
	LogLevel log_level = LogLevel::Info;
	Settle settle = Settle::Immediate;

	TextRef headline;           // 見出し
	// 1 行目に見出しの全文が入らないときの短い形 (問題を先に伝える ini のテキスト)。first_line_of() が
	// FirstLine::forms の 2 つ目に入れる。ドック (ElideLabel) は短い形を表示したときも見出しを省略したとみなし、
	// カードの先頭に全文を表示する。持たなければ、ドックが末尾を省略する。どの見出しが持つかは rules_dock.cpp の
	// kShortHeadlines が決め、短い形の書き方は text_ids.h の「1 行目の短い形」のコメントにある。数を含む括弧
	// (「(%1人)」) は、ドックが末尾を省略するときに括弧ごと省く (dock/ui/dock_ui_calc.h の safe_cut) ので、
	// それだけのためには必要ない。
	std::optional<TextRef> headline_short;
	std::vector<TextRef> body;  // 影響と次の操作 (1 から 2 段落)
	// 本文の段落の間の場所の行 (上の BodyLocation)。
	std::vector<BodyLocation> body_locations;
	// 本文のまとまりが始まる段落の番号 (body の添字。昇順で、0 は入れない)。表示する側はその段落の前の間隔を、
	// まとまりの中の段落の間 (と場所の行の前後) より広くする。アンインストールが途中で止まったときのカードで、
	// まとまりの見出し、項目、次の操作と、次のまとまりを見分けられるようにするためである。空なら、まとまりに
	// 分けない。ダイアログのように「- 」の箇条書きと字下げにしないのは、ほかのカードの段落と形が分かれ、狭い
	// ドックで字下げの分だけ折り返しが増えるためである。
	std::vector<std::size_t> body_group_starts;
	std::optional<LocationLine> location; // 場所の行 (書き込めなかった保存先など)。カードのすべての段落の後に置く
	std::optional<TextRef> line2;         // メッセージのレベル 3 の 2 行目 (録画ファイルの作成の進み具合だけが使う)
	std::vector<StageItem> stages;        // 準備の段階の「名前: 値」
	std::vector<ViewButton> buttons;      // 0 個から 2 個
	// メッセージのレベル 3 の行のツールチップ (例: 録画のためだけに受け取っている理由 =
	// cordmux.dock.recording_only.tip)。メッセージのレベル 1 と 2 のカードは本文を持つので使わない。
	std::optional<TextRef> tip;

	// 表示した時刻 (システム時刻のマイクロ秒)。stabilize() が入れる。decide_dock() の結果では 0。
	std::int64_t appeared_at_us = 0;

	// 真なら、メッセージのレベル 1 でも、状態タブを開いた時点で既読にして 1 行目から消す (メッセージのレベル 2 と同じ
	// 既読の扱い。未読に戻る条件も同じ)。メッセージは状態タブに残る。立てるのは、配信の映像と音には響かず、録画だけが
	// 止まった失敗 (録画の停止、録画の開始の失敗) の規則である。ほかのメッセージのレベル 1 は、解決するまで 1 行目に
	// 残る。メッセージのレベル 2 と 3 では見ない。
	bool read_on_status_tab = false;

	// 同じキーのまま、深刻さが進んだことを表す段階 (0 から)。既読にした時の段階より大きくなると、未読に戻して 1 行目に
	// もう一度出す (settle_first_line)。キーを変えないので、同じメッセージが 2 つ並ぶことはない。
	int read_stage = 0;
};

inline bool operator==(const Notice &a, const Notice &b)
{
	return a.key == b.key && a.rule_name == b.rule_name && a.log_subject == b.log_subject && a.tier == b.tier &&
	       a.log_level == b.log_level && a.settle == b.settle && a.headline == b.headline &&
	       a.headline_short == b.headline_short && a.body == b.body && a.body_locations == b.body_locations &&
	       a.body_group_starts == b.body_group_starts && a.location == b.location && a.line2 == b.line2 && a.stages == b.stages && a.buttons == b.buttons &&
	       a.tip == b.tip && a.appeared_at_us == b.appeared_at_us && a.read_on_status_tab == b.read_on_status_tab &&
	       a.read_stage == b.read_stage;
}

inline bool operator!=(const Notice &a, const Notice &b)
{
	return !(a == b);
}

// ---- ドックの参加者の一覧の 1 行 ----

// 人の行の状態の種類。並びの前のものほど優先して選ぶ。
enum class RowState { NotSharing, Waiting, NoVideo };

struct RowStatus {
	RowState state = RowState::NotSharing;
	TextRef text;
	Settle settle = Settle::Immediate;
	// 状態のテキストに続けて、ツールチップとアクセシブルな説明に表示する 1 文。表示の規則が状態ごとに埋める。
	// 枠に載っていないが cordmux の録画の対象になっている人には、枠の話をしない録画の文
	// (row_waiting_recording.tip、row_not_receiving_recording.tip) を入れる。状態のテキストのキーは同じなので、
	// 状態のキーから 1 文を引くと、枠の話の文になって事実と違う。表示する側 (dock/person_row.cpp) は、空でなければ
	// これを使う。表示する側が「枠に載っているか」を見て文を選ばないのは、どの文が正しいかを決める所が View の外に
	// もう 1 つできるためである。
	TextRef tip;
};

inline bool operator==(const RowStatus &a, const RowStatus &b)
{
	return a.state == b.state && a.text == b.text && a.settle == b.settle && a.tip == b.tip;
}

inline bool operator!=(const RowStatus &a, const RowStatus &b)
{
	return !(a == b);
}

// 「使う声」のメニューの 1 項目。先頭は「本人の声」(action.args.voice_user_id が空) で、区切りの後に通話に
// 参加している人が並ぶ。押せない項目は action.disabled_reason を持つ。
struct VoiceChoice {
	Label label;
	bool checked = false;
	ViewButton action;
};

inline bool operator==(const VoiceChoice &a, const VoiceChoice &b)
{
	return a.label == b.label && a.checked == b.checked && a.action == b.action;
}

inline bool operator!=(const VoiceChoice &a, const VoiceChoice &b)
{
	return !(a == b);
}

// 人の行の [...] のメニュー。
struct RowMenuView {
	// メニューに [自動で載せる] だけを出す (サブアカウントがない構成の、自分の行)。このとき always_on と always_on_toggle
	// だけが値を持つ。
	bool auto_add_only = false;
	bool always_on = false;
	ViewButton always_on_toggle; // SetAlwaysOn (args.on は現在の値の逆)
	// [全画面で表示] (チェックの付く項目)。focus はこの行の人を全画面で表示しているか。focus_toggle は SetFocus で、
	// 全画面で表示している人の行では元に戻す向き (args.user_id が空)、ほかの行ではこの行の人を全画面にする向き。
	// auto_add_only の行 (サブアカウントがない構成の自分の行) にも出す (自分の枠も全画面にできる)。
	bool focus = false;
	ViewButton focus_toggle;
	ViewButton rename;           // SetNameOverride (args.name は空のまま。ダイアログで入力した名前をドックが入れる)
	std::string name_override;   // 現在の cordmux での名前 (空 = なし。名前を変更するダイアログの初期値)
	std::vector<VoiceChoice> voices;
	// 「使う声」の項目そのものを押せないときのテキストと理由。この行の人の声をほかの行で使っていて、現在の値が本人の
	// 声のときに持つ。値があれば、ドック (dock/row_menu.cpp) は「使う声」を押せない項目にし、テキストを
	// voices_disabled_label、ツールチップとアクセシブルな説明を voices_disabled_reason にする (下の階層は
	// 開かない)。理由は候補の全員で同じなので、候補ごとには並べない。2 つとも値を持つか、2 つとも持たない。
	// 候補を押せない項目として並べ、理由をツールチップだけにしないのは、理由の見えない押せない項目が並ぶため
	// である。
	std::optional<TextRef> voices_disabled_label;  // row_voice_from_lent.menu {この行の人, 使っている人}
	std::optional<TextRef> voices_disabled_reason; // participants.voice_taken.text {この行の人の声の名前, 使っている人}
};

inline bool operator==(const RowMenuView &a, const RowMenuView &b)
{
	return a.auto_add_only == b.auto_add_only && a.always_on == b.always_on && a.always_on_toggle == b.always_on_toggle &&
	       a.focus == b.focus && a.focus_toggle == b.focus_toggle && a.rename == b.rename &&
	       a.name_override == b.name_override && a.voices == b.voices &&
	       a.voices_disabled_label == b.voices_disabled_label && a.voices_disabled_reason == b.voices_disabled_reason;
}

inline bool operator!=(const RowMenuView &a, const RowMenuView &b)
{
	return !(a == b);
}

// 人の行の、音量とミュートの値。ドックはこの値でミュートのボタンとメニューのスライダーを描く。操作は ActionId の
// SetVoiceMute、SetScreenAudioMute、SetVoiceVolume、SetScreenAudioVolume を、行の user_id で押す
// (ledger/actions.h)。値を持たない行 (Vesktop のアカウントの行) は、applies が false で、ほかは既定の値のままである。
struct RowAudio {
	// 値が配信の音に反映されるか。合成のソースがあるシーンコレクションでだけ true にする (ない場合、参加者ごとの値は枠のソースの
	// 音には掛からない。ドックがコンポーネントを出すかを決めるのに使う)。
	bool applies = false;
	// メインアカウントの行か (settings の broadcaster_user_id の人)。声のミュートは、Discord を通った自分の声を配信に
	// 載せるかの選択になり、次の起動にも残る。ほかの人のミュートは残らない。
	bool is_main_account = false;

	bool voice_muted = false;
	int voice_volume_pct = config::kPersonVolumeDefaultPct;

	// 画面共有の音を持っているか。false の間、画面共有の音の値は画面に出さない。
	bool has_screen_audio = false;
	bool screen_muted = false;
	int screen_volume_pct = config::kPersonVolumeDefaultPct;
};

inline bool operator==(const RowAudio &a, const RowAudio &b)
{
	return a.applies == b.applies && a.is_main_account == b.is_main_account && a.voice_muted == b.voice_muted &&
	       a.voice_volume_pct == b.voice_volume_pct && a.has_screen_audio == b.has_screen_audio &&
	       a.screen_muted == b.screen_muted && a.screen_volume_pct == b.screen_volume_pct;
}

inline bool operator!=(const RowAudio &a, const RowAudio &b)
{
	return !(a == b);
}

// 行の枠の位置。配信の並びと同じ関数 (ledger/slot_cells.h の plan_slot_cells) で決める。None は配信に映っていない人である。
enum class SlotPosition { None, TopLeft, TopRight, BottomLeft, BottomRight, Full };

// 行のマイクのボタンの状態。NoVoice は、まだ声が届いていない人で、押せない。
enum class MicState { Normal, Muted, NoVoice };

// 行のスピーカーのボタンの状態。Hidden は、画面共有の音がない人で、場所だけ取って表示しない。
enum class SpeakerState { Hidden, Normal, Muted };

// 枠が埋まっているときに、チェックを押して開く「入れ替える相手」のメニューの 1 項目。action は ReplaceSlot。
struct ReplaceChoice {
	Label name;
	SlotPosition position = SlotPosition::None;
	ViewButton action;
};

inline bool operator==(const ReplaceChoice &a, const ReplaceChoice &b)
{
	return a.name == b.name && a.position == b.position && a.action == b.action;
}

inline bool operator!=(const ReplaceChoice &a, const ReplaceChoice &b)
{
	return !(a == b);
}

struct PersonRow {
	std::string user_id; // 行を見分けるキー (ドックはこのキーで行を書き換える)
	Label name;          // cordmux での名前、Discord での名前、userId のうち、最初にあるもの

	bool checkable = true;     // Vesktop のアカウントの行は false
	bool checked = false;      // 枠に載っている
	std::optional<int> slot;   // 載っている画面共有の枠の番号 (画面には表示しない。行の状態のログの対象に使う)
	// チェックを押せるか。checkable が false の行では false になる。ほかの行では、載せていない人で、空きがないか、
	// 自分の枠を表示していて 3 人に達したときに false になる。
	bool check_enabled = true;
	ViewButton check_action;   // SetSlot。self_frame_check のときは SetSelfFrame (押したときに apply() に渡す引数を含む)
	// この行のチェックが、枠に載せる代わりに自分の枠の付け外しをするか (自分の枠の人の行で、自分の枠に映す映像を OBS から
	// 選んでいる間。サブアカウントがなければ、選んでいない間も true で、押せない理由が付く)。Vesktop のアカウントの行
	// (checkable が false) でも true になり、その行はチェックと、[自動で載せる] だけのメニューを出す (音量は checkable に
	// 従って出さない)。このとき check_enabled は check_action の押せるかで決まる。
	bool self_frame_check = false;

	bool sharing = false;              // 画面共有していると分かっているときだけ true
	std::optional<RowStatus> status;   // 行の状態
	std::optional<TextRef> quality;    // 画質を下げているときのテキスト (status がないときだけ行に表示する)
	std::optional<TextRef> resolution; // ドックの幅が 32 em 以上のときだけ表示する

	// 2 行目の部分 (声、Discord での名前、ユーザー名の順。空なら 2 行目なし)。どの部分も ini の 1 つのキーの完全な
	// テキスト (row_voice、row_voice_absent、row_discord_name、row_username の .status) で、区切りの文字では
	// つながない。ドックは部分ごとの QLabel を広い余白で横に並べ、狭いときにどの部分を省くかを決める
	// (dock/name_check.h の fit_line2)。
	std::vector<TextRef> line2_parts;

	// 同じ Discord での名前の人がほかにいるか (いれば 2 行目にユーザー名を表示する) と、表示している名前が同じ人が
	// ほかにいるか (名前の省略のしかたと、アクセシブルな説明に使う)。
	bool same_discord_name_exists = false;
	bool same_visible_name_exists = false;

	// Discord のユーザー名 (PersonFact::username のコピー。Vesktop のアカウントの行は持たない)。持たない = 届いて
	// いない。2 行目に表示するか (same_discord_name_exists) とは別に、分かれば入れる。ドックは、表示している
	// 名前が同じ行の [...] のアクセシブル名 (more_actions_for_username.a11y の %2) に使う。表示している名前が
	// 同じで Discord での名前が違う行 (cordmux での名前がほかの人の名前と同じになったときなど) は、2 行目に
	// ユーザー名がないので、そこからは読めない。
	std::optional<std::string> username;

	RowMenuView menu;

	// 音量とミュートの値。
	RowAudio audio;

	// 押した直後に空きがなくなって枠に載せられなかったときの 1 行。原因が続く間だけ持つ。
	std::optional<TextRef> refused_note;

	// ---- 参加者タブの行が読む値 (PeopleListView の行だけが値を持つ) ----

	SlotPosition position = SlotPosition::None;
	bool is_main = false;  // [メイン] のタグ。メインアカウントだけの構成では Vesktop のアカウントの行
	bool auto_add = false; // [自動] のタグ。[自動で枠に載せる] が付いている人
	MicState mic = MicState::Normal;
	SpeakerState speaker = SpeakerState::Hidden;
	// 枠が埋まっていて、チェックを押すと「入れ替える相手」のメニューを開くか。チェックは薄く表示し、理由は
	// check_action.disabled_reason をツールチップにする。
	bool check_replaces = false;
	std::vector<ReplaceChoice> replace_choices; // 枠に載っている人。位置の順
	// Vesktop との接続が切れていて、切れる前に分かっていた値を残している行。薄く表示し、どのボタンも押せない。行の状態を
	// 持たない。
	bool off = false;
};

inline bool operator==(const PersonRow &a, const PersonRow &b)
{
	return a.user_id == b.user_id && a.name == b.name && a.checkable == b.checkable && a.checked == b.checked &&
	       a.slot == b.slot && a.check_enabled == b.check_enabled && a.check_action == b.check_action &&
	       a.self_frame_check == b.self_frame_check && a.sharing == b.sharing && a.status == b.status && a.quality == b.quality &&
	       a.resolution == b.resolution && a.line2_parts == b.line2_parts &&
	       a.same_discord_name_exists == b.same_discord_name_exists &&
	       a.same_visible_name_exists == b.same_visible_name_exists && a.username == b.username &&
	       a.menu == b.menu && a.audio == b.audio && a.refused_note == b.refused_note && a.position == b.position &&
	       a.is_main == b.is_main && a.auto_add == b.auto_add && a.mic == b.mic && a.speaker == b.speaker &&
	       a.check_replaces == b.check_replaces && a.replace_choices == b.replace_choices && a.off == b.off;
}

inline bool operator!=(const PersonRow &a, const PersonRow &b)
{
	return !(a == b);
}

// ---- 参加者タブの一覧 ----

// 参加者タブの一覧。メインアカウントの行を先頭に固定し、その下を「画面共有中」と「通話中」の 2 組に分ける。組の中は表示名の
// 自然順。サブアカウントの行は持たない (上部のサブの行にある)。
struct PeopleListView {
	std::optional<PersonRow> pinned;
	std::vector<PersonRow> sharing;
	std::vector<PersonRow> in_call; // 通話にいて、画面共有していない人
	int slots_used = 0;             // 枠の数 N/4 の N。自分の枠を使っていれば数える (observation.h の self_frame_in_use)
	int slots_total = config::kScreenSlotCount;
	TextRef slots_label;     // 「枠3/4」
	TextRef sharing_heading; // 「画面共有中 (3)」
	TextRef in_call_heading; // 「通話中 (2)」
};

inline bool operator==(const PeopleListView &a, const PeopleListView &b)
{
	return a.pinned == b.pinned && a.sharing == b.sharing && a.in_call == b.in_call && a.slots_used == b.slots_used &&
	       a.slots_total == b.slots_total && a.slots_label == b.slots_label &&
	       a.sharing_heading == b.sharing_heading && a.in_call_heading == b.in_call_heading;
}

inline bool operator!=(const PeopleListView &a, const PeopleListView &b)
{
	return !(a == b);
}

// ---- 音声タブ ----

// 音声タブの 1 人の 2 行 (声と画面共有の音声) のうちの 1 行。control が真の行は、つまみ、パーセント、メーターを置く。偽の行は
// 同じ場所に note の 1 文を置く。どちらでも行の高さは同じである。
struct AudioLine {
	bool control = false;
	TextRef note;
	// ミュートのボタンを置くか。置かない行も場所は空ける (行の右端がそろう)。
	bool mute_shown = false;
	bool mute_enabled = false;
	bool muted = false;
	int volume_pct = config::kPersonVolumeDefaultPct;
};

inline bool operator==(const AudioLine &a, const AudioLine &b)
{
	return a.control == b.control && a.note == b.note && a.mute_shown == b.mute_shown &&
	       a.mute_enabled == b.mute_enabled && a.muted == b.muted && a.volume_pct == b.volume_pct;
}

inline bool operator!=(const AudioLine &a, const AudioLine &b)
{
	return !(a == b);
}

// 音声タブの 1 人。
struct AudioRowView {
	std::string user_id; // 行を見分けるキー。メーターの値もこの人の user_id で引く
	Label name;
	// メインアカウントか (声のミュートのボタンのツールチップに補足を添える)。
	bool is_main = false;
	// Vesktop との接続が切れていて、切れる前に分かっていた値を残している行。薄く表示し、操作できない。
	bool off = false;
	AudioLine voice;
	AudioLine screen;
};

inline bool operator==(const AudioRowView &a, const AudioRowView &b)
{
	return a.user_id == b.user_id && a.name == b.name && a.is_main == b.is_main && a.off == b.off &&
	       a.voice == b.voice && a.screen == b.screen;
}

inline bool operator!=(const AudioRowView &a, const AudioRowView &b)
{
	return !(a == b);
}

// 音声タブ。全員を、メインアカウントが先頭、ほかは表示名の自然順に並べる (組分けしない)。サブアカウントと、値が配信の音に
// 反映されない人 (Vesktop のアカウントの行、合成のソースがない間の全員) の行は持たない。行がない間は、その理由の 1 文を持つ。
struct AudioTabView {
	std::vector<AudioRowView> rows;
	std::optional<TextRef> notice;
};

inline bool operator==(const AudioTabView &a, const AudioTabView &b)
{
	return a.rows == b.rows && a.notice == b.notice;
}

inline bool operator!=(const AudioTabView &a, const AudioTabView &b)
{
	return !(a == b);
}

// 表示タブ。値の入力欄 (config::Settings の値) は設定から読むので、View に持つのは、押せるかを ledger が決める操作と、
// 入力欄の文字列を決める事実だけである。
struct LookTabView {
	ViewButton dummy_toggle;          // ToggleDummy (args.on は押した後の値。押せないときは disabled_reason)
	bool dummy_on = false;            // ダミー映像を表示しているか (チェックの状態)
	bool has_main_account = false;    // メインアカウントを設定しているか ([自分の枠に映す映像] の先頭の選択肢の文字列を決める)
};

inline bool operator==(const LookTabView &a, const LookTabView &b)
{
	return a.dummy_toggle == b.dummy_toggle && a.dummy_on == b.dummy_on && a.has_main_account == b.has_main_account;
}

inline bool operator!=(const LookTabView &a, const LookTabView &b)
{
	return !(a == b);
}

// ---- 上部のメインとサブ ----

// メインとサブの行の、通話の状態。Unknown は、まだ分からない (通話の名前と同じく「-」と表示する)。
enum class AccountCallState { Unknown, SameCall, OtherCall, NoCall, NotConnected };

struct AccountRow {
	Label name;
	AccountCallState state = AccountCallState::Unknown;
	// 通話の名前。分からないときは「-」、NoCall は「通話なし」、NotConnected は「未接続」。
	Label call;
	// 通話の名前を注意の色にするか (サブアカウントがメインアカウントと別の通話に参加しているとき)。
	bool call_warn = false;
};

inline bool operator==(const AccountRow &a, const AccountRow &b)
{
	return a.name == b.name && a.state == b.state && a.call == b.call && a.call_warn == b.call_warn;
}

inline bool operator!=(const AccountRow &a, const AccountRow &b)
{
	return !(a == b);
}

// [変更] のメニューの 1 項目。action は SetMainAccount。
struct MainAccountChoice {
	Label label;
	bool checked = false;
	bool in_call = false; // 通話中の人か (偽は過去の参加者)
	ViewButton action;
};

inline bool operator==(const MainAccountChoice &a, const MainAccountChoice &b)
{
	return a.label == b.label && a.checked == b.checked && a.in_call == b.in_call && a.action == b.action;
}

inline bool operator!=(const MainAccountChoice &a, const MainAccountChoice &b)
{
	return !(a == b);
}

struct MainAccountView {
	AccountRow row;
	ViewButton change;                      // [変更]
	std::vector<MainAccountChoice> choices; // 通話中の人、過去の参加者の順で、それぞれ自然順
	MainAccountChoice no_main;              // 「メインアカウントを使わない」
};

inline bool operator==(const MainAccountView &a, const MainAccountView &b)
{
	return a.row == b.row && a.change == b.change && a.choices == b.choices && a.no_main == b.no_main;
}

inline bool operator!=(const MainAccountView &a, const MainAccountView &b)
{
	return !(a == b);
}

struct SubAccountView {
	AccountRow row;
	ViewButton move;  // [移動]
	ViewButton leave; // [退出]
};

inline bool operator==(const SubAccountView &a, const SubAccountView &b)
{
	return a.row == b.row && a.move == b.move && a.leave == b.leave;
}

inline bool operator!=(const SubAccountView &a, const SubAccountView &b)
{
	return !(a == b);
}

// ドックの上部の固定の行。sub を持たないのはメインアカウントだけの構成で、メインの行は Vesktop のアカウントを表す。
struct AccountsView {
	MainAccountView main;
	std::optional<SubAccountView> sub;
};

inline bool operator==(const AccountsView &a, const AccountsView &b)
{
	return a.main == b.main && a.sub == b.sub;
}

inline bool operator!=(const AccountsView &a, const AccountsView &b)
{
	return !(a == b);
}

// ---- 一覧の上の行、空の一覧、録画のボタン、メニュー ----

// 一覧の上の 1 行 (チェックを押せない理由)。幅が足りなければ後ろの文から省く。
struct ListReason {
	TextRef text;
	// 幅が足りないときに表示する、後ろの文を省いた形 (例「空いている枠がありません。」=
	// cordmux.dock.no_free_slot_short.text)。持たない = 短い形がない (ドックは text の末尾を省略する)。
	// 文の区切りの位置をコードで決めないので、短い形も ini の 1 つのキーの完全なテキストにする。
	std::optional<TextRef> short_text;
};

inline bool operator==(const ListReason &a, const ListReason &b)
{
	return a.text == b.text && a.short_text == b.short_text;
}

inline bool operator!=(const ListReason &a, const ListReason &b)
{
	return !(a == b);
}

// 一覧が空の理由。Uninstalled はアンインストールを始めた後 (最中、完了、途中で止まったとき) で、この間は人の行を
// 作らない (rules_dock.cpp の decide_dock() のコメント)。
enum class EmptyKind { NotConnected, NotInCall, Alone, Uninstalled };

struct EmptyList {
	EmptyKind kind = EmptyKind::NotConnected;
	TextRef text;
	std::optional<ViewButton> button;
};

inline bool operator==(const EmptyList &a, const EmptyList &b)
{
	return a.kind == b.kind && a.text == b.text && a.button == b.button;
}

inline bool operator!=(const EmptyList &a, const EmptyList &b)
{
	return !(a == b);
}

// cordmux の録画のボタンの 5 つの状態。
enum class RecordButtonState { Idle, Starting, Recording, Stopping, Failed };

// label は、開始か停止の処理に入ってから 1 秒までは前のテキストのままにする。ふつうはドックの次の周期 (500 ms) で
// 処理を終えた状態が届くので、すぐに処理中のテキストにすると、テキストが 2 度変わってちらつく。
struct RecordButtonView {
	RecordButtonState state = RecordButtonState::Idle;
	TextRef label;              // ボタンに見えるテキスト
	// ボタンの名前 (読み上げソフトが読む名前)。label から経過時間を除いたもの。経過時間は毎秒変わるので、名前に入れると、
	// フォーカスがボタンにある間、読み上げソフトが経過時間を読み続ける。
	TextRef name;
	bool pressed_look = false;  // Recording と Stopping で true
	ViewButton action;          // ToggleRecording (押せないときは disabled_reason)
	std::vector<TextRef> width_labels;   // ボタンの幅を決める 4 つのテキスト (どの状態でも同じ幅にする)
};

inline bool operator==(const RecordButtonView &a, const RecordButtonView &b)
{
	return a.state == b.state && a.label == b.label && a.name == b.name && a.pressed_look == b.pressed_look &&
	       a.action == b.action && a.width_labels == b.width_labels;
}

inline bool operator!=(const RecordButtonView &a, const RecordButtonView &b)
{
	return !(a == b);
}

// ドックの [...] のメニュー。
struct DockMenuView {
	ViewButton open_settings;     // OpenSettings (args.page は Recording)
	ViewButton open_participants; // OpenParticipants
	ViewButton export_bundle;     // ExportBundle (設定ウィンドウを [バージョン情報] で開いてから同じ処理をする)
	ViewButton feature_toggle;    // ToggleFeature
};

inline bool operator==(const DockMenuView &a, const DockMenuView &b)
{
	return a.open_settings == b.open_settings && a.open_participants == b.open_participants &&
	       a.export_bundle == b.export_bundle && a.feature_toggle == b.feature_toggle;
}

inline bool operator!=(const DockMenuView &a, const DockMenuView &b)
{
	return !(a == b);
}

// ---- ウィンドウとダイアログの View ----

// 参加者の設定ウィンドウの 1 行。通話に参加している人と過去の参加者を 1 つの並びで持つ。
struct ParticipantRow {
	std::string user_id;
	Label name;                                   // Discord での名前 (読めなければ userId)
	bool in_call = false;                         // 「現在の参加者」か「過去の参加者」か
	// 最終参加日時の列の値 (表示の形は QLocale で決める)。現在の参加者は、この通話に参加した時刻 (Seen::call_joined_at_us。
	// 分からなければ持たない)。過去の参加者は、最後に通話にいた時刻。
	std::optional<std::int64_t> last_seen_at_us;
	bool always_on = false;
	ViewButton always_on_toggle;                  // SetAlwaysOn
	std::string name_override;
	ViewButton rename;                            // SetNameOverride
	std::vector<VoiceChoice> voices;              // 使う声の項目
	bool roster_shown = true;                     // 画面共有していない参加者の並びに出すか (列「通話のみ時ラベル非表示」)
	ViewButton roster_toggle;                     // SetRosterShown (押すと roster_shown の逆にする)
};

inline bool operator==(const ParticipantRow &a, const ParticipantRow &b)
{
	return a.user_id == b.user_id && a.name == b.name && a.in_call == b.in_call &&
	       a.last_seen_at_us == b.last_seen_at_us && a.always_on == b.always_on &&
	       a.always_on_toggle == b.always_on_toggle && a.name_override == b.name_override && a.rename == b.rename &&
	       a.voices == b.voices && a.roster_shown == b.roster_shown && a.roster_toggle == b.roster_toggle;
}

inline bool operator!=(const ParticipantRow &a, const ParticipantRow &b)
{
	return !(a == b);
}

// バージョンの一覧の 1 行 (読めない値は `-`)。
struct VersionRow {
	TextRef label;
	Label value;
};

inline bool operator==(const VersionRow &a, const VersionRow &b)
{
	return a.label == b.label && a.value == b.value;
}

inline bool operator!=(const VersionRow &a, const VersionRow &b)
{
	return !(a == b);
}

// 診断情報のエクスポートの結果 (成功したときだけ。失敗はその場でダイアログに表示する)。
struct ExportResultView {
	TextRef text;
	std::optional<LocationLine> location;
};

inline bool operator==(const ExportResultView &a, const ExportResultView &b)
{
	return a.text == b.text && a.location == b.location;
}

inline bool operator!=(const ExportResultView &a, const ExportResultView &b)
{
	return !(a == b);
}

// 設定ウィンドウの [バージョン情報] のページ。
struct AboutPageView {
	std::vector<VersionRow> versions;
	std::optional<TextRef> vesktop_rewrote_at_startup;    // 起動時の Vesktop の設定の 1 行
	std::optional<ExportResultView> export_result;
	ViewButton export_button;    // ExportBundle
	ViewButton uninstall_button; // Uninstall
	// アンインストールを始めた後は、OBS を終了するまで設定を変更できない。削除した設定のフォルダーを、書き込みで
	// 作り直さないためである。
	bool settings_locked = false;
};

inline bool operator==(const AboutPageView &a, const AboutPageView &b)
{
	return a.versions == b.versions &&
	       a.vesktop_rewrote_at_startup == b.vesktop_rewrote_at_startup && a.export_result == b.export_result &&
	       a.export_button == b.export_button && a.uninstall_button == b.uninstall_button &&
	       a.settings_locked == b.settings_locked;
}

inline bool operator!=(const AboutPageView &a, const AboutPageView &b)
{
	return !(a == b);
}

// ---- 状態タブ ----

// 状態タブの [映像の受信] の表の 1 行 (1 人)。遅れたフレームの数と率は、バッファの入力欄の値でドックが計算するので、
// View は列そのものを持つ (pipeline/arrival_window.h)。
struct StatusReceiveRow {
	std::string user_id;
	Label name; // cordmux での名前
	// 配信を通した映像途切れの回数と時間の合計 (累計。ledger/status_tab.h)。途切れが続いている間は、今までの分を含む。
	std::uint64_t gap_count = 0;
	std::int64_t gap_total_us = 0;
	// 直近 1 分の遅れの列。直近 1 分にフレームが届いていない人は空。
	std::vector<pipeline::ArrivalWindow> streams;
};

inline bool operator==(const StatusReceiveRow &a, const StatusReceiveRow &b)
{
	return a.user_id == b.user_id && a.name == b.name && a.gap_count == b.gap_count &&
	       a.gap_total_us == b.gap_total_us && a.streams == b.streams;
}

inline bool operator!=(const StatusReceiveRow &a, const StatusReceiveRow &b)
{
	return !(a == b);
}

// [cordmuxの録画] の節の 3 つの状態。
enum class StatusRecordingSection {
	None,      // この起動でまだ録画していない
	Recording, // 録画中
	Last,      // 録画していない。最後の録画がある
};

struct StatusRecordingView {
	StatusRecordingSection section = StatusRecordingSection::None;
	TextRef last;       // Last のときの 1 文 (最後の録画: 21:04 (12分))
	std::string folder; // Last のとき [フォルダーを開く] で開く場所 (UTF-8 の絶対パス)。空ならボタンを置かない
};

inline bool operator==(const StatusRecordingView &a, const StatusRecordingView &b)
{
	return a.section == b.section && a.last == b.last && a.folder == b.folder;
}

inline bool operator!=(const StatusRecordingView &a, const StatusRecordingView &b)
{
	return !(a == b);
}

// 履歴の 1 行。when は時刻の引数 (time_arg か date_time_arg)。subject があるときは、表示する側が subject を解決した
// 文字列を text の %1 に入れる (メッセージの見出しなど、ini の別のテキストを埋める行。ledger はテキストを解決できない)。
struct StatusHistoryLine {
	std::string when;
	TextRef text;
	std::optional<TextRef> subject;
};

inline bool operator==(const StatusHistoryLine &a, const StatusHistoryLine &b)
{
	return a.when == b.when && a.text == b.text && a.subject == b.subject;
}

inline bool operator!=(const StatusHistoryLine &a, const StatusHistoryLine &b)
{
	return !(a == b);
}

// 履歴の行数の上限。
inline constexpr std::size_t kStatusHistoryMax = 60;

// 状態タブの「Vesktop」の組。Vesktop の設定が同梱の Vencord を指していて (VencordDirState の Matches と
// HashMismatch)、Vesktop が起動していないと分かっている間だけ持つ。状態の 1 文と [Vesktopから削除]
// (ActionId::RemoveFromVesktop) を並べ、押せない理由 (この起動で Vesktop の設定を書かないとき) はボタンの下に文で
// 表示する (メッセージのカードのボタンと同じ形)。Vesktop の起動中に組を出さない理由は ledger/actions.cpp の
// RemoveFromVesktop の行にある。
// メッセージのカードに置かないのは、置く先のメッセージ (Discord の内部への依存箇所が見つからない) が、Vesktop を
// 終了すると接続が切れて消えるためである。押せるのは Vesktop を終了した後なので、押せるときにはボタンがなくなる。
struct StatusVesktopView {
	TextRef text;      // 状態の 1 文 (cordmux.status.vesktop_added.text)
	ViewButton remove; // [Vesktopから削除]
};

inline bool operator==(const StatusVesktopView &a, const StatusVesktopView &b)
{
	return a.text == b.text && a.remove == b.remove;
}

inline bool operator!=(const StatusVesktopView &a, const StatusVesktopView &b)
{
	return !(a == b);
}

// 状態タブの View。「現在のメッセージ」は DockViewV2::notices を使う。decide_dock() は composite_present、表の行のうち
// 直近 1 分の列を持つもの、reset、vesktop を埋め、累計と録画の節と履歴は settle_status() (ledger/status_tab.h) が埋める
// (前の周期の状態が要る)。
struct StatusTabView {
	// 合成のソースがあるシーンコレクションか。ないときは表を出さず、バッファが動作するのは合成のソースがあるときだと伝える。
	bool composite_present = false;
	// 表の行 (名前の自然順)。直近 1 分にフレームが届いた人と、累計が 0 でない人。
	std::vector<StatusReceiveRow> rows;
	ViewButton reset; // ResetStallTally
	StatusRecordingView recording;
	// 「Vesktop」の組。持たない = 組ごと表示しない (StatusVesktopView のコメント)。
	std::optional<StatusVesktopView> vesktop;
	std::vector<StatusHistoryLine> history; // 新しいものが先頭。最大 kStatusHistoryMax 行
};

inline bool operator==(const StatusTabView &a, const StatusTabView &b)
{
	return a.composite_present == b.composite_present && a.rows == b.rows && a.reset == b.reset &&
	       a.recording == b.recording && a.vesktop == b.vesktop && a.history == b.history;
}

inline bool operator!=(const StatusTabView &a, const StatusTabView &b)
{
	return !(a == b);
}

// アンインストールの結果の種類。ダイアログのアイコンは、Done が情報、ほかは警告にする。
enum class UninstallResultKind { Done, DoneUnconfirmed, Stopped };

// アンインストールの後に残ったものの、種類ごとのまとまり。uninstall_result は「削除できなかったもの」「元に戻せなかった
// 設定」「確認できなかったもの」の順に積み、空のまとまりは入れない。画面に表示する順はこの順ではない。ダイアログ
// (dock/install_dialog_plan.cpp の group_rank) とドックのカード (rules_dock.cpp の stopped_card_body) は、どちらも
// 見出しでまとまりを見分けて、確認、設定、削除の順に並べる。OBS を終了する前にしてもらう確認を先に読ませるため
// である。表示する側は、積んだ順に頼らない。
struct LeftoverGroup {
	TextRef heading;
	std::vector<Label> items;
	// 項目ごとに、その下に表示する場所の行 (0 個以上)。空なら、どの項目にも場所の行がない。空でなければ items と
	// 同じ長さ。例: 「cordmuxの設定のファイル」の下に設定のフォルダー (トークンとポート番号を保存するフォルダーが
	// 残ったときは、名前を表示せずに、その場所も同じ項目の下に置く)、「Vesktopの設定」の下に Vesktop の設定の
	// ファイル。行の種類 (LocationKind) は、アンインストールの処理が返した事実 (UninstallLeftoverFact::is_file)
	// のまま決める。削除できなかったものがファイルなら File (表示する側は [ファイルの場所を開く] を置く)、
	// フォルダーなら Folder ([フォルダーを開く])。場所は文の中に入れず、1 行に分けて表示する。
	std::vector<std::vector<LocationLine>> item_locations;
	// 「削除できなかったもの」のまとまりの次の操作。アンインストールが途中で止まったときの、このまとまりだけが
	// 持つ。このまとまりに cordmux の設定のフォルダー (OBS の plugin_config の下の cordmux。
	// UninstallLeftoverKind::SettingsFiles) が残っているときは「もう一度アンインストールしてください。」
	// (not_deleted_uninstall_again_next)、それ以外は「もう一度アンインストールするか、手で削除してください。」
	// (not_deleted_next) にする。そのフォルダーには Vesktop の設定を戻すためのバックアップ
	// (vencord_dir_backup.json) があり、手動で削除すると次のアンインストールで戻せなくなるためである。
	// ダイアログ (dock/install_dialog_plan.cpp) とドックのカード (rules_dock.cpp の stopped_card_body) は、どちらも
	// この値を使い、まとまりの中身から選び直さない (同じ条件を 2 か所に持たないため)。ほかの 2 つのまとまりの次の
	// 操作は見出しだけで決まり、選ぶものがないので持たない。
	std::optional<TextRef> next;
	// 「元に戻せなかった設定」のまとまりの注意 (元に戻すまで削除しないでほしいもの) と、注意の直後に置く場所の行。
	// アンインストールが途中で止まったときの、このまとまりだけが持つ。場所の行は、プラグインのフォルダー
	// (分かるとき)、cordmux の設定のフォルダーの順に並べる。設定のフォルダーを並べるのは、アンインストールの
	// 処理がそこにバックアップを残したと返したとき (UninstallFacts::settings_dir) か、残ったものとして返した
	// ときである。テキストは、場所の行の中身と数から rules_dock.cpp の uninstall_result が選ぶ
	// (dont_delete_folder、dont_delete_folders、dont_delete_files、dont_delete_files_and_folders)。ダイアログと
	// ドックのカードが同じ値を読む。next に入れないのは、注意は次の操作の後に置く別の段落で、ダイアログでは
	// 場所の行の前置きになるためである。1 つのフィールドに 2 つの役を持たせると、表示する側がキーの名前で役を
	// 見分けることになる。
	std::optional<TextRef> caution;
	std::vector<LocationLine> caution_locations;
};

inline bool operator==(const LeftoverGroup &a, const LeftoverGroup &b)
{
	return a.heading == b.heading && a.items == b.items && a.item_locations == b.item_locations && a.next == b.next &&
	       a.caution == b.caution && a.caution_locations == b.caution_locations;
}

inline bool operator!=(const LeftoverGroup &a, const LeftoverGroup &b)
{
	return !(a == b);
}

// アンインストールの結果のダイアログ。
struct UninstallResultView {
	UninstallResultKind kind = UninstallResultKind::Done;
	bool loose = false;              // 削除するフォルダーが 1 つに決まらない形 (DLL のフォルダーが plugins\<名前> でない)
	TextRef main;
	std::vector<TextRef> notes;      // 補足
	// 完了したときは「削除するフォルダー」、途中で止まったときは「元に戻せなかった設定」の注意が指すフォルダー
	// (どちらもプラグインのフォルダー)。Observation に場所がなければ持たない。
	std::optional<LocationLine> folder_to_delete;
	std::optional<LocationLine> record_dir; // cordmux の録画の場所。録画が残っていなければ持たない
	std::vector<LeftoverGroup> groups;
};

inline bool operator==(const UninstallResultView &a, const UninstallResultView &b)
{
	return a.kind == b.kind && a.loose == b.loose && a.main == b.main && a.notes == b.notes &&
	       a.folder_to_delete == b.folder_to_delete && a.record_dir == b.record_dir && a.groups == b.groups;
}

inline bool operator!=(const UninstallResultView &a, const UninstallResultView &b)
{
	return !(a == b);
}

// ---- ドック全体の View ----

struct DockViewV2 {
	FirstLine first_line;    // 待ちを通した後の 1 行目 (stabilize() が決め直す)
	FirstLine fallback_line; // メッセージが 1 つもないときの 1 行目 (平常の要約、自分で選んだ状態、処理中)
	// レベル 2 をすべて既読にした後の 1 行目 (ledger/notice_tracker.h の settle_first_line が使う)。fallback_line と違い、
	// primary の行のメッセージを既読にしたときに「確認中...」にならない。
	FirstLine read_line;
	// メッセージのレベルの順に並べ、同じメッセージのレベルの中は表示した順にする (並べるのは stabilize())。
	std::vector<Notice> notices;
	std::optional<ListReason> list_reason;
	std::optional<EmptyList> empty;
	std::vector<PersonRow> people; // 表示している名前の自然順
	PeopleListView people_list;    // 参加者タブの一覧
	AudioTabView audio_tab;        // 音声タブ
	LookTabView look_tab;          // 表示タブ
	AccountsView accounts;         // 上部のメインとサブ
	RecordButtonView record;
	DockMenuView menu;
	std::vector<ParticipantRow> participants; // 既定の並び (最終参加日時の新しい順、同じ時刻は名前の自然順)。現在の参加者が先
	AboutPageView about;
	StatusTabView status_tab; // 状態タブ
	std::optional<UninstallResultView> uninstall_result; // アンインストールが完了したか、途中で止まった後だけ持つ
	bool list_full = false; // 枠がすべて使用中 (スクリーンリーダーへの通知に使う)
};

inline bool operator==(const DockViewV2 &a, const DockViewV2 &b)
{
	return a.first_line == b.first_line && a.fallback_line == b.fallback_line && a.read_line == b.read_line &&
	       a.notices == b.notices && a.list_reason == b.list_reason && a.empty == b.empty && a.people == b.people &&
	       a.people_list == b.people_list && a.audio_tab == b.audio_tab && a.look_tab == b.look_tab &&
	       a.accounts == b.accounts && a.record == b.record &&
	       a.menu == b.menu && a.participants == b.participants && a.about == b.about && a.status_tab == b.status_tab &&
	       a.uninstall_result == b.uninstall_result && a.list_full == b.list_full;
}

inline bool operator!=(const DockViewV2 &a, const DockViewV2 &b)
{
	return !(a == b);
}

} // namespace dockview
} // namespace cordmux::ledger

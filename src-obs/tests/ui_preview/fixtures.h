// SPDX-License-Identifier: Apache-2.0

// ドックと cordmux のウィンドウを描画する場面 (状態ごとに 1 つ)。どの場面も Observation を組んで、表示の規則に通す
// (製品のドックが周期ごとに行うのと同じく、decide_dock、stabilize、DockWidget::render の順)。画面に表示される文と、
// 押せるか押せないかとその理由は、すべて製品の表 (ledger/rules.cpp、ledger/rules_dock.cpp、ledger/actions.cpp) が
// 決めたものである。ここで View を手で組まないのは、製品が表示しない画面を描画してしまうためである。名前と ID は
// すべて架空のもの (実在の Discord の ID や表示名を書かない)。ID は 18 桁で、桁の数だけを実際の ID に合わせる
// (幅の見え方を実際に近づけるため)。どの描画の対象に描くかは、--face が決める (faces.h)。

#pragma once

#include <string>
#include <vector>

#include "config/settings.h"
#include "ledger/dock_view.h"
#include "ledger/notice_tracker.h"
#include "ledger/observation.h"
#include "ledger/status_tab.h"
#include "pipeline/audio_mixer.h"
#include "sources/composite_input.h"

namespace cordmux::ui_preview {

struct Fixture {
	// 話している人の user_id。製品では Pipeline::audio_levels から届く値で、View には入らない。
	std::vector<std::string> speaking;
	// 組ごとの音の大きさ。製品では Pipeline::audio_levels から届く値で、View には入らない。音声タブのメーターに出る。
	std::vector<pipeline::AudioTrackLevel> levels;
	std::string id;   // ファイル名の先頭 (ASCII)
	std::string note; // 何の場面か (一覧に表示する 1 行)
	ledger::Observation obs;
	// 状態タブの累計、履歴、最後の録画 (製品では DockPoll が周期をまたいで持つ状態)。history が空でなければ、その履歴を
	// そのまま見せる (今あるメッセージの発生を履歴に足さない)。
	ledger::StatusState status;
	config::Settings settings;
	bool dummy_on = false;
	std::string dialog_vencord; // 切り替えの確認のダイアログ: 押した時点の vencordDir
	// 設定ウィンドウ: 保存先の入力欄が空のときに書き込む場所 (製品の record_dir_now が返す値。プレースホルダーに
	// 表示される)。既定値は短い架空の場所。普通の長さの場所 (OBS の既定の録画の保存先である、利用者の「ビデオ」
	// の下) は場面ごとに替える。
	std::string record_dir_default = "D:\\Videos\\cordmux";
	// 参加者の設定ウィンドウ: 見出しで並べ替えた状態 (ParticipantsWindow::Column の値。-1 は並べ替えを選ばない既定の並び)。
	int participants_sort_column = -1;
	bool participants_sort_ascending = false;
};

// すべての場面 (製品の画面のすべての状態)。
std::vector<Fixture> all_fixtures();

// ドックに描画する場面 (状態の行・メッセージ・参加者の一覧。ウィンドウだけの場面を除く)。
std::vector<Fixture> dock_fixtures();

// id の場面を返す。なければ id が空の場面を返す。
Fixture fixture_by_id(const std::string &id);

// 場面の Observation に、設定から決まるフィールドをコピーしたもの (製品の dock/dock_poll.cpp の current_facts と同じ
// 向きで、設定から Observation へ)。ドックに渡すのも、網羅のチェックに使うのもこれ。全画面にできる人と全画面の人の名前も、
// 下の focus_tiles_for で入れる。
ledger::Observation observation_for(const Fixture &fixture);

// 合成のソースのタイルに描く人 (全画面にできる人) を、製品の配置の入力 (dock/dock_poll_layout.cpp の push_composite_input)
// と同じ並び方で決める。描くのは、枠に載っていて画面共有していないと分かっていない人 (observation_for の slot_visible と
// 同じ決め方) と、自分の枠を出していれば自分の枠の人。名前は行と同じ表示名。ダミー映像の表示の間のタイルは再現しない。
std::vector<sources::CompositeFocusTile> focus_tiles_for(const ledger::Observation &obs);

// 製品の判断を通した View。decide_dock の後に、製品と同じ待ち時間の値で stabilize を 2 回通し、待ちが済んだ形に
// したもの (1 回目は表示する前の待ちの始まり、2 回目は待ちが済んだ時刻)。1 行目は下の first_line_for の値にする。
ledger::dockview::DockViewV2 view_for(const Fixture &fixture);
// 状態タブの映像途切れの表の見本 (3 人。60 fps で 1 分の決まった乱数で作り、遅れの多い人と少ない人、長い名前の人を入れる)。
// user_id は fake_id(first_n) から順に付く。
std::vector<ledger::ArrivalFact> sample_arrival_facts(int first_n, int count);

// 製品の DockPoll と同じく、待ちが済んだ View から 1 行目と状態タブのテキストを決める (ledger::settle_first_line)。
// status_tab_open が真なら、状態タブを開いている形 (その時点のレベル 2 が既読になる) にする。
ledger::FirstLineResult first_line_for(const Fixture &fixture, bool status_tab_open);

// 時間を進める場面。Observation の並びで、プレビューツールは 1 つの NoticeTrackerState を引き継ぎながら stabilize に
// 順に通し (製品の DockPoll と同じ)、1 ステップごとにドックを描画する。表示する前の待ち、消す前の待ち、表示の順
// (新しいものは下)、並びを保留している間の位置の変化を見る。
struct TimelineStep {
	std::string id;   // 画像の名前の先頭 (ASCII)
	std::string note; // そのステップで何が起きたか
	ledger::Observation obs; // now_us はステップごとに進む
	bool press_hold = false; // このステップの間、スクロール領域の中でマウスのボタンを押している (並びの保留)
};
std::vector<TimelineStep> timeline_steps();

// 網羅のチェック。ledger/rules.cpp の primary の表と notices の表のどの行にも、その行の条件が成り立つ場面が 1 つ以上
// あるかを見る。decide_dock だけが使う行 (ledger/rules.h の AddedRuleId) も、メッセージの rule_name で数える。条件が
// 成り立つ場面のない行の名前を返す (空なら、すべての行に場面がある)。場面を追加し忘れたことや、場面の組み方を
// 間違えて別の行の条件が成り立っていることを見つけるために使う。
std::vector<std::string> uncovered_rules(const std::vector<Fixture> &fixtures);

} // namespace cordmux::ui_preview

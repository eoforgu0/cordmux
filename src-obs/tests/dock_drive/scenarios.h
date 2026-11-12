// SPDX-License-Identifier: Apache-2.0

// probe と mistap 以外のシナリオのエントリポイント。どのシナリオも、プレビュー用のプラットフォームプラグインの上で、
// 入力を input.h の経路 (OS の入力と同じ経路) で流し、確認の結果を report.h の Results に 1 行ずつ加える。
// シナリオの一覧は docs/testing.md の「ドックを操作するテスト」にある。製品の DockPoll の処理のうち libobs に依存する
// 部分は組み込めないので、ウィンドウを開く、エクスポートを始める、保存の失敗をメッセージにする、の 3 つは、
// DockPoll の処理 (dock/dock_poll_settings.cpp と dock/dock_poll_export.cpp) と同じ順をテストの中で行う。

#pragma once

#include <QString>

#include "dock/ui/dock_style.h"
#include "report.h"

namespace cordmux::dock_drive {

struct ScenarioOptions {
	QString out_dir;
	dock::ui::DockStyle style;
};

// 各シナリオを実行し、確認の結果を results に加える。画像などのファイルは options.out_dir の下に書く。
void run_keyboard(const ScenarioOptions &options, Results &results);
void run_windows(const ScenarioOptions &options, Results &results);
void run_record(const ScenarioOptions &options, Results &results);
void run_a11y(const ScenarioOptions &options, Results &results);
void run_menus(const ScenarioOptions &options, Results &results);
void run_layout(const ScenarioOptions &options, Results &results);
void run_dialogs(const ScenarioOptions &options, Results &results);
// 次の 3 つは、ほかのシナリオの最後にも呼ばれる (menus が run_summary を、layout が run_framework を、
// windows が run_tools_uninstall を呼ぶ)。1 つだけを実行するときは --scenario summary、framework、tools を渡す。
void run_summary(const ScenarioOptions &options, Results &results);
void run_framework(const ScenarioOptions &options, Results &results);
void run_tools_uninstall(const ScenarioOptions &options, Results &results);
// 参加者タブの人の行のミュートのボタンを、ポインターとキーボードで操作して確認する。menus が run_summary の後に呼ぶ。1 つだけを
// 実行するときは --scenario rowaudio を渡す。
void run_row_audio(const ScenarioOptions &options, Results &results);
// 音声タブのつまみ、メーター、ミュートのボタンを、ポインターとキーボードで操作して確認する。1 つだけを実行するときは
// --scenario audio を渡す。
void run_audio(const ScenarioOptions &options, Results &results);
// 自分の行のチェックによる自分の枠の付け外しと、自分の Discord の共有の始まりと終わりでの出入りを確認する。1 つだけを実行するときは
// --scenario selfframe を渡す。
void run_self_frame(const ScenarioOptions &options, Results &results);
// 設定ウィンドウのページの一覧でページを切り替えてもフォーカスが一覧に残ることと、ページの QScrollArea が
// タブストップでないことを確認する。windows が run_tools_uninstall の後に呼ぶ。1 つだけを実行するときは
// --scenario pages を渡す。
void run_settings_pages(const ScenarioOptions &options, Results &results);
// ドックの表示タブを確認する。入力欄が刻みと範囲に合う値を書くこと、別の入力欄を変えても手で書いた値を書き換えないこと、
// 押せなくする条件、並びの選択肢の制限、太さの選択肢、ソースの選択肢と、ドックの中の動き (変えるとすぐ配信の画面へ渡し、
// 保存は 1 秒後に 1 回にまとまること、ホイールで値が変わらないこと、設定ウィンドウの [適用] とメインアカウントの変更で
// 戻らないこと、ダミー映像の切り替え、位置が動かないこと)。1 つだけを実行するときは --scenario look を渡す。
void run_look(const ScenarioOptions &options, Results &results);
// ドックの状態タブを確認する。メッセージのカードの見た目、映像のバッファ (すぐ反映、保存は 1 秒後に 1 回、[適用] で戻らない、
// 閉じる処理)、映像途切れの累計 (リセット、配信の開始、抜けた人の行)、履歴の順序と上限、ホイール、最小の幅、並びの保留の間に
// 位置が動かないこと、1 行目を押すと状態タブが開いてレベル 2 が既読になること。1 つだけを実行するときは --scenario status を渡す。
void run_status(const ScenarioOptions &options, Results &results);
// 配信者にとっていちばん悪い条件で、操作する場所が動かないことと、はみ出さないことを確認する。メッセージのすべての場面で
// 上部、タブ、下のバー、行の座標が平常と同じこと、映像途切れの繰り返し、30 人、235x300、メッセージの重なり。1 つだけを
// 実行するときは --scenario positions を渡す。
void run_positions(const ScenarioOptions &options, Results &results);
// 並びの保留の間にためた状態タブの変化が、ポインターがスクロール領域からタブのバーへ移るときの Enter と
// Leave のイベントの最中に反映されても、カードのボタンをその場で削除しないかを確認する。製品がクラッシュすると
// dock_drive も終了するので、別の実行にするか、--scenario の最後に置く。
void run_dispatch(const ScenarioOptions &options, Results &results);
// 設定ウィンドウと確認のダイアログを開いたまま、obs_module_unload と同じ後片付け (フォーカスリングが使う最後の入力の
// 監視を外す) をしてから破棄し、クラッシュしないことを確認する。後片付けの後はフォーカスリングが表示されなくなり、
// 同じ実行の後のシナリオに影響するので、--scenario の最後に置く。
void run_unload(const ScenarioOptions &options, Results &results);

} // namespace cordmux::dock_drive

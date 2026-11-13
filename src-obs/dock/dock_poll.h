// SPDX-License-Identifier: Apache-2.0

// ドックのポーリング (QTimer) と、押された操作の実行。OBS の UI スレッドだけで動く。ポーリングのたびに
// Pipeline::snapshot()、OsProbe の結果、DockPoll の設定から Observation を作り、ledger::decide_dock に渡して、
// 前と違う所だけを描き直す。押せるかどうかは ledger::apply() が決め、ここではその Intent を実際の
// 呼び出しにするだけである。
// Qt (QTimer) を含む。Observation を作る純粋関数は、Qt を含まない dock/build_observation.h にある。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <map>
#include <set>
#include <vector>

#include <QPointer>
#include <QTimer>

#include "bundle/bundle_files.h"
#include "config/settings.h"
#include "dock/build_observation.h"
#include "dock/deferred_save.h"
#include "dock/dock_poll_config.h"
#include "dock/dock_poll_export.h"
#include "dock/dock_poll_install.h"
#include "dock/dock_poll_layout.h"
#include "dock/dock_poll_settings.h"
#include "dock/dock_widget.h"
#include "dock/self_frame_check.h"
#include "install/build_mark.h"
#include "install/uninstall.h"
#include "install/vencord_dir.h"
#include "ledger/actions.h"
#include "ledger/notice_tracker.h"
#include "ledger/status_tab.h"
#include "ledger/stream_record.h"
#include "ledger/observation.h"
#include "pipeline/pipeline.h"
#include "platform/os_probe.h"
#include "sources/self_frame_plan.h"

class QTimer;

namespace cordmux::dock {

// DockPollConfig は、Qt を含まない dock/dock_poll_config.h にある。

// ポーリングのたびに状態を読んでドックを描き、押された操作を実行する。作るのも破棄するのも、OBS の UI スレッドで行う。
class DockPoll {
public:
	// pipeline と probe は参照で持つので、DockPoll より後に破棄する。widget は nullptr でもよく、
	// そのときは描画しない (OBS が破棄するので QPointer で持つ)。OBS の UI スレッドで作り、start() を呼ぶまでは
	// ポーリングしない。
	DockPoll(pipeline::Pipeline &pipeline, platform::OsProbe &probe, DockWidget *widget, DockPollConfig cfg);
	~DockPoll();

	DockPoll(const DockPoll &) = delete;
	DockPoll &operator=(const DockPoll &) = delete;

	// ポーリングを始める (すぐに 1 回読む)。何度呼んでもよい。
	void start();

	// ポーリングを止め、ウィジェットのコールバックを外す。何度呼んでもよい。OBS_FRONTEND_EVENT_EXIT で必ず呼ぶ。
	// ドックのウィジェットは OBS のメインウィンドウと一緒に、obs_module_unload より前に破棄されるためである
	// (dock_bind.h の add_dock)。
	void stop();

	// OBS_FRONTEND_EVENT_FINISHED_LOADING から呼ぶ。これより前は、合成のソースがあるかを調べない。シーンの
	// 読み込みが終わる前に調べると、起動直後の普通の状態を、cordmux を置いていないと表示することになる。
	void on_frontend_finished_loading();

	// OBS のテーマ、[フォントサイズ]、[密度] が変わったときに、plugin-main.cpp から呼ぶ
	// (OBS_FRONTEND_EVENT_THEME_CHANGED)。ドックのウィジェットの色、アイコン、大きさを読み直し、開いている
	// cordmux のウィンドウにも伝える。
	void on_theme_changed();

	// OBS の配信が始まった、終わったときに、plugin-main.cpp から呼ぶ (OBS_FRONTEND_EVENT_STREAMING_STARTED と
	// STREAMING_STOPPED。UI のスレッドから)。設定 record_with_stream がオンなら、cordmux の録画を始める、終える。
	// オフのときは何もしない。録画の操作は押されたボタンと同じ経路 (run_action) で行うので、押せない状態なら実行せず
	// ログに書く。
	void on_streaming_event(ledger::StreamEvent event);

	// OBS のホットキー「cordmux: <位置>の枠を全画面で表示」と「cordmux: 全画面を元に戻す」を押したときに、plugin-main.cpp が
	// UI のスレッドで呼ぶ (ホットキーのスレッドからは呼ばない)。cell は 4 等分の位置 (0 左上、1 右上、2 左下、3 右下)。
	// 全画面でないときの配置でその位置に描いている人を全画面にし、その人が既に全画面なら元に戻す。その位置に誰も描いて
	// いなければ何もしない。どちらも行のメニューと 1 行目のボタンと同じ ActionId::SetFocus を通す。cordmux が無効の間と、
	// ポーリングを止めた後は何もしない。
	void press_focus_hotkey(int cell);
	void press_focus_restore_hotkey();

private:
	// DockPoll の実装は 5 つの .cpp に分けてあり、メンバーの宣言はすべてこのヘッダーにある。
	//   dock_poll.cpp           ポーリング、描画、押された操作の実行、録画とダミーと枠の割り当て
	//   dock_poll_layout.cpp    OBS の中の合成のソースと名前ラベル (push_layout_now、distribute_person_decor など)
	//   dock_poll_install.cpp   Vesktop への追加、再インストール、照合の結果の読み直し、アンインストール
	//   dock_poll_settings.cpp  settings.json と seen_people.json の読み書き、設定ウィンドウ
	//   dock_poll_export.cpp    診断情報のエクスポート (export_bundle_now)
	// 2 つ以上の .cpp が使う関数は dock_poll_internal.h にある。dock_poll_<名前>.h は、その .cpp の状態
	// (DockPoll<名前>State。下のメンバー *_state_) と、static の関数だけを持つ構造体 (DockPoll<名前>Part) を
	// 宣言する。Part は第 1 引数に DockPoll を取り、DockPoll の private のメンバーを使うので、friend にする。
	friend struct DockPollLayoutPart;
	friend struct DockPollInstallPart;
	friend struct DockPollSettingsPart;
	friend struct DockPollExportPart;

	void tick();
	// 枠の並べ方と自分の枠を出すかの判断を 1 回進める。ドックの表示の更新 (tick) とは別のタイマー (layout_timer_) が
	// kLayoutStepIntervalMs ごとに呼ぶ。周期ごとに自分の枠の状態 (self_frame_.state) を進めるのは、ここだけである。tick は
	// 結果 (self_frame_.facts) を読むだけで、状態を進めない。
	void layout_step();
	// layout_step の直後に tick を行う。起動時、シーンの読み込みの終わり、押した操作の後のように、次の周期を待たずに
	// 画面と配置をそろえるときに使う。
	void step_and_tick();
	void run_action(ledger::ActionId id, const ledger::ActionArgs &args);

	// メッセージが表示されるか消えるたびに、OBS のログに 1 行書き、スクリーンリーダーに読み上げを渡す。diff は
	// ledger::stabilize の差分で、人の行の状態の変化はログにだけ書き、読み上げには渡さない。first_line は
	// 変化した後のドックの 1 行目で、メッセージのレベル 1 が消えたときに、そのテキストを読み上げに渡す。
	void report_notice_changes(const ledger::NoticeDiff &diff, const ledger::dockview::FirstLine &first_line);

	// [Vesktopに追加] を押したときの処理。scene は、追加されていないときに押したか (Pressed)、
	// Vesktop の設定がほかの Vencord を指しているときに押したか (FixRequested) を表す。書き込む前に、ほかの
	// Vencord を使っている利用者への確認のダイアログが必要かを調べ (dock/install_flow.h の
	// other_vencord_dialog_needed)、開いたときは、閉じた後に state.json を読み直してから install::plan_write を
	// 呼ぶ。state.json とバックアップは、一時ファイルに書いてから置き換える。書く直前に、Vesktop が起動していないことを
	// 確認する (vesktop_free_to_write)。
	void write_vencord_dir(install::WriteScene scene);

	// 状態タブの [Vesktopから削除] を押したときの処理。Vesktop の設定を cordmux を追加する前の値に戻す
	// (install::remove_from_vesktop。バックアップと cordmux のファイルは消さない)。確認のダイアログは開かない。
	// 失敗は ops_.vesktop_remove_failure に残す。
	void remove_from_vesktop();

	// 書く直前に、Vesktop が起動していないことを確認する (platform::vesktop_running_now)。起動していないと分かったとき
	// だけ true を返す。起動していた (か、調べられなかった) ときは、その結果を vesktop_process_seen_ に覚え、OsProbe に
	// すぐ問い合わせ直させて、false を返す。覚えた値は、OsProbe の新しい結果が届くまで Observation に入る
	// (DockFacts::vesktop_process_seen)。押したボタンが、押せない理由と一緒に押せない見た目にすぐ変わる。
	bool vesktop_free_to_write();

	// data/vencord-src のすべてのファイルを、data/vencord に 1 つずつコピーする (コピーする関数は
	// platform/ にあり、順は install::plan_reinstall が決める)。コピーできなかったファイルは、名前を
	// reinstall_failed_files_ に残す。advance_written_at が true (利用者が [再インストール] を押したとき) なら、
	// すべてコピーできたときに、バックアップの written_at だけを現在の時刻にし、「Vesktopを再起動してください」の
	// メッセージが表示されるようにする。false は、data/vencord に前のバージョンの cordmux のファイルが見つかり、
	// メッセージを表示せずにコピーし直すときに使う。すべてコピーできたら true を返す。
	bool reinstall_vencord_files(bool advance_written_at);

	// アンインストールする。確認のダイアログを開き、[アンインストール] を選んだときだけ install::run_uninstall を
	// 呼び、結果を uninstall_report_ に置いて、結果のダイアログを開く。
	void uninstall_now();

	// OsProbe が新しい結果を置いたら、data/vencord-src と data/vencord の cordmux-build.txt を読み直す。
	// data/vencord が前のバージョンの cordmux のファイルなら、メッセージを表示せずに (ログには 1 行書く)
	// data/vencord-src からコピーし直す。
	void on_probe_result(const platform::OsProbeResult &probe);

	// cordmux の有効と無効を切り替える。settings に保存し、notify_runtime_settings で Pipeline にも渡すので、
	// OBS を再起動しなくても反映される。
	void set_feature(bool on);

	// OBS を再起動しなくても反映される設定が変わったことを、DockPollConfig::apply_runtime_settings で Pipeline に
	// 渡す (この関数は plugin-main.cpp が設定する)。
	void notify_runtime_settings();

	// アンインストールを始めた後か (true の間は、OBS を閉じるまで設定と一覧を書き込まない)。ドックが持つ進み具合
	// (uninstall_phase_) が NotStarted でないか、install::is_uninstalling が true なら true を返す。
	bool writes_stopped() const;

	// ダイアログを開く操作 ([Vesktopに追加] とアンインストール) を、押されたボタンのシグナルから戻った後に
	// 実行する。ボタンのシグナルの中でダイアログを開かないのは、ダイアログが開いている間もポーリングが続いて画面を
	// 描き直し、押されたボタンが載っているメッセージのカードが作り直されると、シグナルの途中でボタンが
	// 破棄されるためである (notice_list.cpp の retire_widget のコメントと同じ問題)。同じ操作が
	// 二重に実行されないようにもする。
	void run_dialog_action(std::function<void()> work);
	// frozen は、シーンコレクションを切り替えている途中か。途中のときは合成のソースの有無を調べない。
	DockFacts current_facts(bool frozen) const;

	// 設定を settings.json に書く (中身が変わったときだけ)。
	//
	// 読めなかったファイルには一切書き込まない (settings_file_unreadable)。そのことは起動時に 1 回だけログに書く。
	// この関数は設定を変えるたびに呼ばれるので、ここで書くと同じ行がリングバッファーを埋める。
	void write_settings();

	// これまでに見た人の一覧を読み書きする (書くのは中身が変わったときだけ)。読めなかったファイルには
	// 一切書き込まない。空の一覧で上書きすると、利用者が設定した値がその場で消える。
	void load_seen_people();
	void write_seen_people();

	// 通話に参加した人と抜けた人を一覧に反映する (時刻を更新するのは、入ったときと抜けたときだけ)。
	void update_seen_people(const pipeline::PipelineSnapshot &snapshot, std::int64_t now_us);

	// 合成のソースに配置の入力を 1 回渡す。合成のソースがないシーンコレクションでは何もしない。
	void push_layout_now(const pipeline::PipelineSnapshot &snapshot, bool self_frame_shown);

	// 自分の枠を載せた順の印 (self_frame_.mark) を、まだ決めていなくて自分の枠が映っていれば決める。チェックが外れて
	// いれば、決め直す印にする。行の位置 (facts) と配信の配置 (push_composite_input) に同じ値を渡すために、どちらも作る前に
	// 呼ぶ。チェックを変える所 (set_self_frame、step_self_frame) ごとに書かず、ここ 1 か所で見る。
	void update_self_added_mark(const pipeline::PipelineSnapshot &snapshot);

	// 自分の枠を出すかの状態 (self_frame_.state) を、layout_step のたびに進める。自分の Discord の共有の始まりと、
	// ほかの人と同じ規則で点け (sources/self_frame_plan.h の plan_self_frame)、結果を self_frame_.facts に入れる。bound は、
	// 自分の枠に映す映像が結び付いているか (シーンの読み込みが終わっていて、切り替えの途中でないときだけ判断する)。
	void step_self_frame(const pipeline::PipelineSnapshot &snapshot, bool bound);

	// 自分の行のチェックで、自分の枠を出すか消す。ドックで付け外しした値は settings.json に残す。
	void set_self_frame(bool on);

	// 自分の枠に映す映像として、OBS のソースかシーンを選んでいるか (設定の値。結び付いているかとは別)。
	bool self_frame_selected() const;

	// 映す映像を「Discord の画面共有」と OBS のソースかシーンの間で替えたときに、メインアカウントの行のチェックを引き継ぐ
	// (設定を替えた直後に呼ぶ。起動の途中の結び付きでは呼ばない)。now_selected は替えた後の値。引き継ぎ方と、Pipeline の答え
	// (Pipeline::move_self_frame の戻り値) を受けた後の扱いは、dock/self_frame_check.h の plan_self_frame_carry と
	// settle_self_frame_carry が決める。
	void carry_self_frame_check(bool now_selected);

	// 自分の枠が配置に数えられるか (自分の枠を出す状態で、映すソースが結び付いている)。ポーリングの外で並べ直すときに使う。
	bool self_frame_shown_now() const;

	// 自分の枠を使っているか (dock/self_frame_check.h の self_frame_in_use_of。映像を OBS から選んでいて、自分の行のチェックが
	// 付いている。映像が結び付いているかは見ない)。Pipeline に渡し、画面の枠に新しく載せられる人数を 1 つ減らす
	// (push_assign_policy)。
	bool self_frame_in_use_now() const;

	// 自分の枠を使っているかが、最後に Pipeline に渡した値から変わっていれば、渡し直す (push_assign_policy)。自分の行の
	// チェックが変わる所 (layout_step、set_self_frame) から呼ぶ。設定を替えたときは、設定の後処理が push_assign_policy を呼ぶ。
	void push_self_frame_in_use_if_changed();

	// 自分の枠の判断 (dock/self_frame_check.h) が返した結果を当てる。self_frame_ を置き換え、settings の self_frame_on を
	// 合わせ、ログを順に書き、settings.json を書き (write_settings が真のとき)、空けた枠を片付ける (after_slot_release)。
	// 自分の枠を使い始めることを Pipeline が断ったときのチェックの外し方 (この周期の配置の入力からも外す) も、結果に入っている
	// (refuse_self_frame_step)。
	void apply_self_frame_step(const SelfFrameStep &step);

	// ダミーの表示をオンかオフにする。ダミーを表示していた時間帯は Pipeline が記録する。
	void set_dummy_mode(bool on);

	// 全画面で映す人を替える (空なら元の配置に戻す)。変わったときだけログに 1 行書き、待たずに合成のソースへ渡す。どの人を
	// どの枠に載せているかは変えない。
	void set_focus(const std::string &user_id);

	// cordmux の録画を開始または終了する。録画のオンとオフを持つのは Pipeline だけで、settings にも書かないので、
	// 起動するたびにオフに戻る。ここでは値が変わるときだけ渡す。
	void set_recording(bool on);

	// 参加者の一覧の行にあるチェックで、人を枠に載せるか外す。割り当て表を変えるのは Pipeline の関数だけで、
	// ドックは config::AllocationTable にも Listener の内部にも直接触らない。枠から外したときは、その枠を空ける
	// (after_slot_release)。
	void set_slot(const ledger::ActionArgs &args);

	// 枠に載っている人 (args.replace_user_id) を外し、同じ枠に args.user_id の人を載せる。割り当て表と経路は
	// Pipeline::replace_slot が 1 回のロックの中で書き換え、外した枠の画面と名前ラベルは、枠から外したときと同じに空ける
	// (after_slot_release)。
	void replace_slot(const ledger::ActionArgs &args);

	// メインアカウントを替える。設定の broadcaster_user_id を書き換えて保存し、Pipeline と合成のソースへすぐ渡す。
	// 空の文字列は、メインアカウントを使わないこと。
	void set_main_account(const std::string &user_id);

	// 「自動で載せる」を付けた人の集合、載せる人の選び方 (settings の assign_mode)、「使う声」の組み合わせ
	// (その人の userId -> 使う声のアカウントの userId) を、Pipeline に渡し直す。元の値は seen_people.json と
	// settings.json にあり、Pipeline が持つのはそのコピーである。中身が変わっていなければ Pipeline は何もしない
	// ので、ポーリングのたびに呼んでよい。
	// 自分の枠を使っているか (self_frame_in_use_now) も渡す。使い始めることは Pipeline が割り当て表と同じロックの中で受け付けるか
	// 断り、断られたら自分の行のチェックを外す (dock/self_frame_check.h の assign_policy_pushed)。
	// 「自動」から「手動」に戻したときに、枠を空ける関数は持たない。今載っている人はそのまま残る (理由は
	// dock_poll_settings.cpp の on_settings_changed)。
	void push_assign_policy();

	// 参加者ごとの声と画面共有の音のミュートを、Pipeline の表に当てる。メインアカウントの声のミュートだけは、settings.json
	// にも書く (起動のたびに戻ると、Discord を通った自分の声を配信に載せるかの選択が戻る)。ほかのミュートは覚えない。
	void set_person_mute(const ledger::ActionArgs &args, pipeline::MixerKind kind);

	// 参加者ごとの声と画面共有の音の音量を、Pipeline の表に当て、seen_people.json の人の行に書く (100% のときは書かない)。
	// スライダーを動かす間は何度も呼ばれるので、表への反映はすぐ行い、ファイルへの書き込みは
	// schedule_seen_flush でまとめる。
	void set_person_volume(const ledger::ActionArgs &args, pipeline::MixerKind kind);

	// 覚えている音量を Pipeline の表に入れ替える (元の値は seen_people.json にある)。起動のときと、人の行を捨てたときに
	// 呼ぶ。
	void push_person_volumes();

	// 音量の変更を seen_people.json に書くのを、最後の変更から kSeenFlushDelayMs の後にまとめる。変更のたびに時間を
	// 数え直すので、スライダーを動かしている間は書かず、止めたとき (離したとき) に 1 回書く。書き込みがポーリングの
	// 周期を止めないことと、ファイルを書き直す回数を減らすことが理由である。OBS を閉じるときは stop() が書く。
	// 変えた人と種類の数は、書いたときに 1 行のログにする (変更のたびにログに書くと、リングバッファーを埋める)。
	void schedule_seen_flush(const std::string &user_id, pipeline::MixerKind kind);
	void flush_seen_volumes();

	// 枠から外した後の処理 (画面共有の枠を空け、名前ラベルの人を外して、並べ直す)。set_slot から呼ぶ。実装は
	// dock_poll_layout.cpp にある。
	void after_slot_release(config::SlotKind kind, int slot);

	// 設定ウィンドウの [OK] か [適用] で設定が変わったときに呼ぶ (実装は dock_poll_settings.cpp)。設定ウィンドウの
	// 項目だけをコピーする。settings にほかの値が入っていても、DockPoll が持つ設定 (設定ウィンドウに欄のない項目)
	// を上書きさせない。
	void on_settings_changed(const config::Settings &settings);

	// ドックの表示タブで値を変えたときに呼ぶ (実装は dock_poll_settings.cpp)。表示タブが持つ項目 (settings_fields.h の
	// SettingsOwner::DockTab の行) だけをコピーして、すぐ配信の画面に反映する。保存は、変更の少し後に 1 回にまとめる
	// (look_save_)。設定ウィンドウの値は上書きしない。
	void on_look_changed(const config::Settings &from_tab);
	// 状態タブで映像のバッファを変えたときに呼ぶ。バッファだけを変えて on_look_changed に渡す (表示タブの値は変えない)。
	void on_buffer_changed(int ms);

	// 設定ウィンドウの「バージョン情報」のページに表示する、OBS と OS のバージョンと GPU の名前を、1 回だけ読む。
	// 起動中に変わらない値なので、ポーリングのたびには読まない。特に GPU の名前は、読むたびに描画のコンテキストを
	// 取るので、何度も読まない。
	void read_platform_versions();

	// 診断情報をエクスポートする。zip に入れるデータを UI スレッドで集め、トークンを消す処理、残っていないかの
	// 確認、zip の作成をワーカースレッドで始める。ダイアログは開かず、結果は DockPollExportPart が受け取って
	// 設定ウィンドウに表示する。呼ぶのは DockPollExportPart::begin だけである (流れは dock_poll_export.h の
	// DockPollExportPart)。
	void export_bundle_now(const pipeline::PipelineSnapshot &snapshot, const platform::OsProbeResult &probe);

	pipeline::Pipeline &pipeline_;
	platform::OsProbe &probe_;
	QPointer<DockWidget> widget_;
	DockPollConfig cfg_;

	std::unique_ptr<QTimer> timer_;
	// 枠の並べ方と自分の枠の判断を進めるタイマー (layout_step)。timer_ と同じく、親を持たない。
	std::unique_ptr<QTimer> layout_timer_;
	bool loading_finished_ = false;
	// DockPollConfig::maintain_listener を最後に呼んだ時刻 (システム時刻のマイクロ秒)。
	std::int64_t last_maintain_listener_us_ = 0;
	// 前のポーリングで Vesktop と接続していたか (切れたことを見つけるため)。
	bool was_link_connected_ = false;
	// 前のポーリングで、映像を受け取ると決めていたか (ledger::video_expected)。決めていない状態から決めている状態に
	// 戻った時刻を、video_expected_since_us_ に残す。起動してからずっと同じ状態のときは残さない。
	bool was_video_expected_ = true;
	std::optional<std::int64_t> video_expected_since_us_;

	// 自分の枠の値 (出すかの状態、最後に Pipeline が受け付けた使っているか、載せた順の印、layout_step が出した結果)。中身は
	// dock/self_frame_check.h の SelfFrameCheck にある。Pipeline に渡して答えを受ける所 (push_assign_policy、
	// carry_self_frame_check) は、そこの関数の結果を apply_self_frame_step で当てる。
	SelfFrameCheck self_frame_;
	// 枠が埋まっていて自分の枠を付けられない間に、そのログを書いたか。
	bool self_frame_deferred_logged_ = false;

	// 最後に作った Observation。押された操作を実行できるかは、これで判断する。画面に表示していた状態と同じ
	// Observation で判断するためである。
	ledger::Observation last_observation_;
	std::vector<ledger::PersonFact> last_known_people_; // 接続している間の最後の参加者 (DockFacts::last_known_people)
	std::string last_known_vesktop_user_id_;            // 最後に分かった Vesktop のアカウント (DockFacts の同名の欄)
	std::string last_known_vesktop_name_;

	// 現在の設定。cfg_.settings から始まり、利用者の操作で変わる。
	config::Settings settings_;
	std::string settings_written_json_; // 最後に書いた中身 (同じなら書かない)

	// これまでに見た人の一覧。
	std::vector<ledger::SeenPerson> seen_people_;
	std::string seen_written_json_;
	int seen_discarded_with_marks_ = 0; // この起動で削除した、参加者の設定がある行の数 (画面に表示する)
	bool seen_file_unreadable_ = false; // 読めなかったファイルを上書きしないためのフラグ
	std::set<std::string> last_in_call_; // 前のポーリングで見えていた人 (入ったか抜けたかの判断に使う)
	// 現在通話に参加している人が、通話の参加者に現れた時刻 (OBS の起動中だけ。保存しない。ledger::Seen::call_joined_at_us)。
	std::map<std::string, std::int64_t> call_joined_at_us_;
	// 音量の書き込みを待たせるタイマー (1 回だけ鳴る。変更のたびに数え直す)。待っている間に変えた人 (声と画面共有の音を
	// 別に数える)。
	static constexpr int kSeenFlushDelayMs = 500;
	std::unique_ptr<QTimer> seen_flush_timer_;
	// 表示タブで変えた値の保存を待たせる (最後の変更から ledger::kLookSaveDelayMs の後に 1 回だけ書く)。OBS を閉じるときは
	// stop() が待たずに書く。
	std::unique_ptr<DeferredSave> look_save_;
	std::set<std::string> pending_voice_volume_users_;
	std::set<std::string> pending_screen_volume_users_;

	// Discord の内部への依存箇所ごとの、最後に読めた時刻。診断情報のエクスポートで使う。
	std::map<std::string, std::int64_t> contact_last_ok_;

	// 設定ウィンドウに表示する OBS と OS のバージョンと GPU の名前。読めなければ空のままで、画面には "-" を
	// 表示する。
	std::string obs_version_;
	std::string os_version_;
	std::string gpu_name_;
	bool platform_versions_read_ = false;
	bool gpu_name_read_ = false;

	// data/vencord-src と data/vencord の cordmux-build.txt の中身。読めなければ持たない。読み直すのは、
	// OsProbe の結果が新しくなったとき (probed_at_us が進んだとき) だけ。
	std::optional<install::BuildMark> original_mark_;
	std::optional<install::BuildMark> shown_mark_;
	std::optional<std::int64_t> marks_read_for_probe_at_;
	// data/vencord にあった前のバージョンのファイルをコピーし直したときの、元になった OsProbe の結果の時刻。
	// この時刻の結果が続く間は、ハッシュの不一致を、上書きされたと表示しない (DockFacts::hide_hash_mismatch)。
	std::optional<std::int64_t> stale_copied_for_probe_at_;

	// [再インストール] でコピーできなかったファイル。次にコピーし直すか、照合で一致したら空に戻す。
	std::vector<std::string> reinstall_failed_files_;

	// DockPoll が自分で見た、Vesktop の状態。書く直前に見て起動していた (か、調べられなかった) とき
	// (vesktop_free_to_write) と、接続していない間に終了したのを見たとき (tick の vesktop_quit_check_due) に入れ、見た
	// 時刻の後に OsProbe の結果が届いたら消す (DockFacts::vesktop_process_seen)。
	std::optional<ledger::VesktopProcess> vesktop_process_seen_;
	std::int64_t vesktop_process_seen_at_us_ = 0;
	// Vesktop の終了を自分で調べた最後の時刻 (dock_poll_config.h の vesktop_quit_check_due)。
	std::int64_t last_vesktop_quit_check_us_ = 0;

	// この起動で vencordDir を書き直したか (設定ウィンドウの「バージョン情報」に出す)。起動したときの値は
	// DockPollConfig::rewrote_vencord_dir_at_startup で、Vesktop の終了を待って書き直したときに true にする。
	bool rewrote_at_startup_ = false;
	// 起動時の書き直しを、Vesktop の終了を待ってもう一度試すか (DockPollConfig::retry_startup_rewrite があり、まだ
	// Waiting でない結果が返っていない)。
	bool startup_rewrite_waiting_ = false;

	// アンインストールの進み具合と結果。メモリにだけ持つ (install::is_uninstalling と同じ)。
	ledger::UninstallPhase uninstall_phase_ = ledger::UninstallPhase::NotStarted;
	std::optional<install::UninstallReport> uninstall_report_;

	// ダイアログを開く操作 ([Vesktopに追加] とアンインストール) の途中か。ダイアログが開いている間もポーリングは
	// 続くので、同じ操作が二重に実行されないようにする。
	bool dialog_action_running_ = false;

	// メッセージを表示するまで待つ時間と、表示した順の状態。純粋関数 ledger::stabilize に毎回渡し、返った状態を
	// 持ち続ける。
	ledger::NoticeTrackerState tracker_state_;
	// 既読にしたメッセージ。純粋関数 ledger::settle_first_line に毎回渡し、返った状態を持ち続ける。保存しない。
	ledger::NoticeReadState read_state_;
	// 状態タブの映像途切れの累計、履歴、最後の録画。純粋関数 ledger::settle_status に毎回渡し、返った状態を持ち続ける。
	// メモリだけに持ち、OBS を起動し直すとゼロから始める。
	ledger::StatusState status_state_;
	// マイクの光と音声タブのメーターの値を読むタイマー (1 秒に 10 回)。参加者タブか音声タブが見えている間だけ動かす
	// (DockWidget::set_on_levels_wanted)。
	std::unique_ptr<QTimer> level_timer_;
	// 前に描いた View に参加者の行があったか。枠がすべて使用中になった、または空いたという読み上げは、一覧が前から
	// 表示されていたときだけ渡す。一覧が表示された直後 (起動の直後や接続した直後) の使用中は、変化ではなく最初の
	// 状態なので、一覧の読み上げの説明が伝える。
	bool rendered_people_ = false;

	// 押した操作の結果。実装を分けた各 .cpp と dock_poll.cpp が書き、tick が build_observation の直後に
	// Observation にコピーする。
	ledger::OperationFacts ops_;

	// 全画面で表示している人の userId (空 = 全画面でない)。メモリだけに持ち、保存しない (OBS を起動し直すと空)。変えるのは
	// set_focus と、その人を描かなくなったときと cordmux を無効にしたときの後始末 (dock_poll_layout.cpp の settle_focus)
	// だけである。
	std::string focus_user_id_;
	// 全画面にした時点で、その人が全画面でないときの配置に描かれていなかったか (枠に載せていない人を全画面にした)。真の間は、
	// その人が画面共有を続ける限り全画面を続け、その人の映像を受信する (Pipeline::set_focus_target)。偽なら、その人が配置から
	// 消えたら全画面をやめる (sources/composite_input.h の focus_after_layout)。
	bool focus_off_slot_ = false;

	// ---- 実装を分けた .cpp ごとの状態 (dock_poll_<名前>.h の DockPoll<名前>State) ----
	DockPollLayoutState layout_state_;
	DockPollInstallState install_state_;
	DockPollSettingsState settings_state_;
	DockPollExportState export_state_;
};

} // namespace cordmux::dock

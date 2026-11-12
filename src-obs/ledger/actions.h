// SPDX-License-Identifier: Apache-2.0

// ドックの操作の純粋関数。apply() は、操作 (ActionId) を現在の状態で押したときにさせる仕事 (Intent) と、押せない
// ときの理由を返す。押せる条件は、actions.cpp の表に ActionId ごとに 1 行ずつ持つ。
// ボタンを作るとき (rules_dock.cpp) と押されたとき (dock/dock_poll.cpp) の両方が apply() を呼び、ドックは押せるか
// どうかを自分で判断しない。判断が 2 か所に分かれると、押しても何も起きないボタンがないことを、Qt を使わない
// テストで確認できなくなる。libobs にも Qt にも依存しない。

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "config/allocation.h"
#include "ledger/observation.h"

namespace cordmux::ledger {

// 画面に表示する操作 (ボタン、チェック、メニューの項目)。ドック、設定ウィンドウ、参加者の設定ウィンドウが表示する
// 操作を、ここに 1 か所で持つ。ActionId は画面の操作 1 つで、それがさせる仕事は IntentKind である。WriteVencordDir と
// RepairVencordDir の仕事はどちらも state.json の vencordDir を書くことなので、同じ IntentKind::WriteVencordDir に
// なる。ActionId を分けるのは、install/vencord_dir.h の plan_write() に渡す場面 (WriteScene) が違うためである。
enum class ActionId {
	WriteVencordDir,        // [Vesktopに追加] (Vesktop の設定に vencordDir がないときなど)
	RepairVencordDir,       // [Vesktopに追加] (Vesktop の設定が別の場所を指しているとき。その値を書き換える)
	ReinstallVencordFiles,  // [再インストール] (同梱の Vencord のファイルが配布時と違うとき)
	ExportBundle,           // 診断情報のエクスポート
	SetAlwaysOn,            // 「自動で載せる」の切り替え
	SetSlot,                // 人を枠に載せる、枠から外す
	SetVoiceSource,         // 使う声を別のアカウントにする、本人の声に戻す
	SetNameOverride,        // 配信に表示する名前を上書きする、上書きをやめる
	ToggleFeature,          // 機能の ON/OFF
	ToggleRecording,        // cordmux の録画の ON/OFF (OFF は異常として扱わない)
	ToggleDummy,            // ダミーの表示の ON/OFF
	// [作成を再開]。OBS を終了するときに打ち切った録画ファイルの作成を、次に起動したときに再開する。
	// NoticeRuleId::RecordFinalizePending (録画ファイルの作成が終わっていない録画がある) と NoticeRuleId::RecordFinalizeFailed
	// (録画ファイルの作成に失敗した) のメッセージのカードに置く。
	ResumeFinalize,
	// cordmux のアンインストール。設定ウィンドウのボタンで、押すと確認のダイアログを表示する。OBS の配信や録画の
	// 最中、cordmux の録画中、Vesktop の起動中などは押せず、ボタンのそばに理由を表示する (条件は actions.cpp)。
	Uninstall,
	// cordmux の設定ウィンドウを開く。開くページは ActionArgs::page ([保存先を変更] は SettingsPage::Recording)。
	OpenSettings,
	// 参加者の設定ウィンドウを開く。引数は使わない。
	OpenParticipants,
	// 合成のソースの、画面共有していない参加者の並びに出すか (参加者の設定ウィンドウの列「通話のみ時ラベル非表示」)。ActionArgs の
	// user_id と on (true なら出す) を使う。
	SetRosterShown,
	// 参加者ごとの声のミュートを切り替える。ActionArgs の user_id と on (true ならミュートにする) を使う。メインアカウントの
	// 声のミュートは settings.json に残り、ほかの人のミュートは残さない。
	SetVoiceMute,
	// 参加者ごとの画面共有の音のミュートを切り替える。引数は SetVoiceMute と同じ。ミュートは残さない (画面共有の音は、共有が
	// 始まるたびにミュートから始まる)。
	SetScreenAudioMute,
	// 参加者ごとの声の音量を決める。ActionArgs の user_id と volume_pct (0 から 200) を使う。参加者ごとに覚える。
	SetVoiceVolume,
	// 参加者ごとの画面共有の音の音量を決める。引数は SetVoiceVolume と同じ。声とは別の値で、参加者ごとに覚える。
	SetScreenAudioVolume,
	// 自分の枠を出すか (ドックの自分の行のチェック)。ActionArgs の on (true なら出す) を使う。出した・消した値は
	// settings.json に残る。
	SetSelfFrame,
	// [移動]。サブアカウントをメインアカウントがいる通話へ移動する。移動先はメインアカウントがいる通話だけで、引数で
	// 選べない。自動では動かない。
	MoveToMainCall,
	// [退出]。サブアカウントを通話から退出させる。全員の映像と声が止まるので、押した場所の隣で確認してから仕事に渡す。
	LeaveCall,
	// [変更] のメニューの項目。ActionArgs の user_id の人をメインアカウントにする。空なら、メインアカウントを使わない。
	SetMainAccount,
	// 枠が埋まっているときに、チェックを押して開くメニューの項目。ActionArgs の replace_user_id の人を枠から外し、user_id
	// の人を載せる。1 つの操作にするのは、途中の並びが配信に出ないようにするためである。
	ReplaceSlot,
	// 状態タブの [途切れの累計をリセット]。映像途切れの回数と時間の累計 (ledger/status_tab.h) をゼロに戻す。累計はメモリだけに
	// あるので、いつでも押せる。
	ResetStallTally,
	// 合成のソースに一時的に 1 人だけを全画面で映す (行のメニューの [全画面で表示]、1 行目の [元に戻す]、OBS のホットキー)。
	// ActionArgs の user_id の人を全画面にし、空なら元の配置に戻す。どの人をどの枠に載せているかは変えず、保存もしない。
	SetFocus,
	// 状態タブの [Vesktopから削除]。Vesktop の設定 (state.json の vencordDir と、Vencord の設定の cordmux の項) を、
	// cordmux を追加する前の値に戻す。cordmux のファイルと設定には触らない。引数は使わない。確認のダイアログは
	// 表示しない ([Vesktopに追加] で戻せるため)。
	RemoveFromVesktop,
};

// ActionId の値の数。値を追加したら、この数も変える。テストが、押せる条件の表 (actions.cpp) が ActionId を
// 1 つずつ持っていることを、この数と比べて確認する。
inline constexpr std::size_t kActionIdCount = 28;

// 操作の引数。どのフィールドを使うかは ActionId ごとに決まっている (使わないフィールドは既定値のまま)。
//
// 誰に対する操作かは user_id で指す。枠の番号 (PersonFact::slot) は画面共有の枠に載っている人だけが持つので、
// 押せるかどうかは user_id で判断する。kind と slot は、仕事をする側が使う補助の値である。
struct ActionArgs {
	std::string user_id;                              // 誰に対する操作か (人を指す操作が使う)
	config::SlotKind kind = config::SlotKind::Screen; // 枠の種類 (SetSlot が使う)
	int slot = 0;                                     // 枠の番号 (1 から)。0 は枠から外す (SetSlot)
	bool on = false;                                  // ON か OFF か (SetAlwaysOn、SetRosterShown、Set*Mute、Toggle* が使う)

	// 配信に表示する名前 (SetNameOverride だけが使う)。空なら上書きをやめる。やめる操作を別の ActionId に
	// しないのは、どちらも同じ設定の書き換えで、1 つの仕事に 2 つの名前が付くためである。
	// 利用者が入力した文字列なので、ini を通さない。
	std::string name;

	// 使う声のアカウントの userId (SetVoiceSource だけが使う)。user_id の人の枠で、このアカウントの声を再生する。
	// 空か user_id と同じなら、本人の声に戻す (戻す操作を別の ActionId にしない理由は name と同じ)。
	// kind と slot は使わない。声の枠の番号は Observation になく、ドックには渡せる値がないためである。
	// user_id の人のどの声の枠に入れるかは、pipeline/stream_router.h の AssignPolicy が決める。
	std::string voice_user_id;

	// 開く設定ウィンドウのページ (OpenSettings だけが使う)。ページごとに ActionId を作らないのは、ページを
	// 追加するたびに押せる条件の表とテストが増えるためである。
	SettingsPage page = SettingsPage::Recording;

	// 音量 (%。0 から 200。Set*Volume だけが使う)。スライダーを動かす間に何度も来るので、押せる条件は範囲だけを見る
	// (今と同じ値でも押せる。ドックが見ている値は最大で 1 周期古く、同じ値を理由に捨てると、動かした先の値が
	// 反映されないことがある)。
	int volume_pct = config::kPersonVolumeDefaultPct;

	// 枠から外す人の userId (ReplaceSlot だけが使う)。slot には、その人の枠の番号を入れる。
	std::string replace_user_id;
};

// ドックは、前の周期に描いた View と比べて変わったところだけを描き直すので、ViewButton が持つ ActionArgs にも
// 等値比較が必要になる。値だけの構造体なので、フィールドごとに比べる。
inline bool operator==(const ActionArgs &a, const ActionArgs &b)
{
	return a.user_id == b.user_id && a.kind == b.kind && a.slot == b.slot && a.on == b.on && a.name == b.name &&
	       a.voice_user_id == b.voice_user_id && a.page == b.page && a.volume_pct == b.volume_pct &&
	       a.replace_user_id == b.replace_user_id;
}

inline bool operator!=(const ActionArgs &a, const ActionArgs &b)
{
	return !(a == b);
}

// 操作がさせる仕事。ドックはこれを見て呼び出す先を決める。行末のフォルダーは、仕事をするコンポーネントである。
enum class IntentKind {
	None, // 既定値。apply() はこの値を返さない (総当たりのテストで確認する)
	WriteVencordDir,       // install/: state.json の vencordDir を書き、元に戻すためのバックアップを更新する
	ReinstallVencordFiles, // install/: 同梱の Vencord の 4 つのファイルをコピーし直す
	ExportBundle,          // ログ・バージョン・設定を、診断情報の zip にエクスポートする
	SetAlwaysOn,           // config/: 「自動で載せる」を切り替える
	SetSlot,               // config/: 枠の割り当てを書き換える
	SetVoiceSource,        // config/: 使う声を変える
	SetNameOverride,       // config/: 配信に表示する名前の上書きを書き換える (空なら消す)
	SetFeature,            // config/: 機能の ON/OFF
	SetRecording,          // config/: cordmux の録画の ON/OFF
	SetDummy,              // config/: ダミーの表示の ON/OFF
	// record/: 途中で打ち切った録画ファイルの作成を、続きから実行する。録画ファイルの作成は録画の中間ファイルを消さないので、押し直しても
	// 録画ファイルの作成に使うファイルは残っている。
	ResumeFinalize,
	// install/: cordmux をアンインストールする (install::run_uninstall)。確認のダイアログを表示するのはドックで、
	// ダイアログで [アンインストール] を選んだときだけ仕事に渡す。
	Uninstall,
	OpenSettings,      // dock/: 設定ウィンドウを ActionArgs::page のページで開く
	OpenParticipants,  // dock/: 参加者の設定ウィンドウを開く
	SetRosterShown,    // config/: seen_people.json の、画面共有していない参加者の並びに出すか (roster_hidden) を書き換える
	SetVoiceMute,         // pipeline/: 参加者ごとの声のミュート (メインアカウントの声だけ settings.json に残す)
	SetScreenAudioMute,   // pipeline/: 参加者ごとの画面共有の音のミュート (残さない)
	SetVoiceVolume,       // pipeline/ と config/: 参加者ごとの声の音量 (seen_people.json に残す)
	SetScreenAudioVolume, // pipeline/ と config/: 参加者ごとの画面共有の音の音量 (seen_people.json に残す)
	SetSelfFrame,         // config/ と sources/: 自分の枠を出すか (settings.json の self_frame_on に残す)
	MoveToMainCall,       // ws/: サブアカウントをメインアカウントがいる通話へ移動させる
	LeaveCall,            // ws/: サブアカウントを通話から退出させる
	SetMainAccount,       // config/: メインアカウントを書き換える (settings.json の broadcaster_user_id に残す)
	ReplaceSlot,          // config/: 枠に載っている人を、1 回で別の人に替える
	ResetStallTally,      // dock/: 映像途切れの累計をゼロに戻す (メモリだけ)
	SetFocus,             // dock/ と sources/: 全画面で映す人を替える (空なら元の配置に戻す。メモリだけ)
	// install/: Vesktop の設定を cordmux を追加する前の値に戻す (install::remove_from_vesktop。バックアップは消さない)
	RemoveFromVesktop,
};

// 押せない理由。テキストは ini にあり、ここが持つのは ini のキー (text_ids.h の text::keys) だけである。
// 画面に表示しない理由には、下の reason のマーカーを入れる。
struct Disabled {
	const char *reason_text_id = nullptr;
};

// 画面に表示しない「押せない理由」のマーカー。ini のキーではない。
//
// 押せない理由を画面に表示しない操作がある。例: [Vesktopに追加] は、追加した後はボタンごと表示しない。行の
// チェックを押せない理由は、行ごとではなく一覧の上に 1 回だけ表示する。それでも apply() は、押しても何も
// 起きない操作を作らないように「押せない」を返す必要があるので、理由のフィールドに ini のキーではないマーカーを
// 入れる。rules_dock.cpp は、このマーカーを画面に表示するテキストのキーに替えるか、理由ごと外す。マーカーが
// View に残らないことは tests/test_ledger_disabled_reasons.cpp が確認する。マーカーを `cordmux.` で
// 始めないのは、test/lint/locale_ids.py が ini のキーとして扱わないようにするためである。
// 理由を持たない Disabled にしないのは、ViewButton が、理由の有無で押せるかどうかを表すためである。
namespace reason {
inline constexpr const char *kDirMatches = "disabled.dir_matches";                     // 同梱の Vencord を指している
inline constexpr const char *kDirNotElsewhere = "disabled.dir_not_elsewhere";          // 別の場所を指していない
inline constexpr const char *kNotAdded = "disabled.not_added";                         // 同梱の Vencord を指していない
inline constexpr const char *kVesktopNotStopped = "disabled.vesktop_not_stopped";      // Vesktop が起動しているか、分からない
inline constexpr const char *kFilesMatch = "disabled.files_match";                     // 4 つのファイルは配布時のまま
inline constexpr const char *kNoUser = "disabled.no_user";                             // 誰に対する操作かを指していない
inline constexpr const char *kBadSlot = "disabled.bad_slot";                           // 枠の番号が範囲の外
inline constexpr const char *kNoOverride = "disabled.no_override";                     // やめる名前の上書きがない
inline constexpr const char *kSameName = "disabled.same_name";                         // 今と同じ名前
inline constexpr const char *kNoVoiceSource = "disabled.no_voice_source";              // 戻す設定 (使う声) がない
inline constexpr const char *kSameVoiceSource = "disabled.same_voice_source";          // 今と同じ使う声
inline constexpr const char *kVoiceSourceLent = "disabled.voice_source_lent";          // この人の声を別の行が使っている
inline constexpr const char *kSlotTaken = "disabled.slot_taken";                       // 指した枠に別の人がいる
inline constexpr const char *kAlreadyOnSlot = "disabled.already_on_slot";              // もう枠に載っている
inline constexpr const char *kNotOnSlot = "disabled.not_on_slot";                      // 枠に載っていない
inline constexpr const char *kNoPendingFinalize = "disabled.no_pending_finalize";      // 作成待ちの録画がない
inline constexpr const char *kBadVolume = "disabled.bad_volume";                       // 音量が範囲 (0 から 200) の外
inline constexpr const char *kSelfFrameRow = "disabled.self_frame_row";                // 自分の枠の行は、自分の枠の付け外しで動かす
inline constexpr const char *kSameSelfFrame = "disabled.same_self_frame";              // 自分の枠が今と同じ状態
inline constexpr const char *kNoSubAccount = "disabled.no_sub_account";                // サブアカウントを使う構成でない
inline constexpr const char *kSameMainAccount = "disabled.same_main_account";          // 今と同じメインアカウント
inline constexpr const char *kMainIsVesktopAccount = "disabled.main_is_vesktop_account"; // Vesktop のアカウントを選んだ
inline constexpr const char *kNoFocus = "disabled.no_focus";                           // 元に戻す全画面がない
inline constexpr const char *kSameFocus = "disabled.same_focus";                       // その人を既に全画面にしている
} // namespace reason

struct Intent {
	IntentKind kind = IntentKind::None;
	ActionArgs args;
	std::optional<Disabled> disabled; // 値があれば押せない

	bool can_run() const { return !disabled.has_value(); }
};

// obs の状態でこの操作を押したときにさせる仕事を返す。押せないときも kind には仕事を入れ、disabled に理由を入れる
// (押せなくても、何をする操作かは変わらないため)。args はそのまま Intent::args にコピーする。
// disabled があるとき、ドックは仕事を実行しない。
Intent apply(const Observation &obs, ActionId id, const ActionArgs &args = ActionArgs{});

// この操作がさせる仕事。
IntentKind action_intent_kind(ActionId id);

// 押せる条件の表 (actions.cpp) の行数と、i 番目 (0 始まり) の行の ActionId。表が ActionId を 1 つずつ持って
// いることを、テストが kActionIdCount と比べて確認するために公開する。
std::size_t action_rule_count();
ActionId action_rule_id_at(std::size_t index);

} // namespace cordmux::ledger

// SPDX-License-Identifier: Apache-2.0

// 画面に表示するテキストのキー (ini のキー) の定数。テキストは data/locale/ の ja-JP.ini と en-US.ini にあり、
// キーの名前の付け方は docs/ui_guidelines.md の「ini のキーの書式」にある。test/lint/locale_ids.py が、ini のキーと
// C++ の文字列リテラルを双方向に突き合わせる (定数があれば、そのキーは使われているとみなす)。ini にないキーは、
// 表示する側でキーがそのまま表示される。TextRef::args は %1 から順に埋まり、何が入るかは各定数のコメントと、
// ini のキーの行の真上のコメント (%N=中身) に書く。

#pragma once

#include "ledger/plural.h"

namespace cordmux::ledger::text {

// キーの形は `cordmux.<画面>.<もの>.<種類>` である (画面 = dock、settings、participants、dialog、source、
// filter、plugin。もの = snake_case。種類 = .button や .text のような、UI の要素の種類を表す語)。キーを修正するときは、
// ja-JP.ini と en-US.ini の同じ節も一緒に修正する。
//
// 定数の名前はキーから決まる。`cordmux.` を除き、残りを `.` と `_` で区切って語ごとに先頭を大文字にし、`k` を
// 付けてつなぐ (`cordmux.dock.summary_call.status` -> `kDockSummaryCallStatus`)。英語の形が数で変わるテキストは
// CountedId (ledger/plural.h) で持ち、pick(id, 数) でしかキーを取り出せない (数がちょうど 1 のときだけ `_one` の
// キー)。`_one` の行がないキーで `_one` を引いて、キーがそのまま画面に表示されることを、型で防ぐ。
// 各定数のコメントには、そのテキストを使う所と、%N に入るもの (ini のコメントと同じ) を書く。
namespace keys {

// ---- dock: ドック ----
// ドックのタイトルと、OBS の [ドック] メニューの項目。ドックのスクロール領域 (cordmuxScroll) のアクセシブル名にも
// 使う (参加者の設定ウィンドウと設定ウィンドウも、スクロール領域にウィンドウのタイトルを付けているので、それに
// 合わせて別のキーは持たない)
inline constexpr const char *kDockWindowTitle = "cordmux.dock.window.title";
// ドックのタブの名前。状態のタブは、レベル 1 と 2 のメッセージがあれば件数を添える (%1=件数)
inline constexpr const char *kDockTabPeopleLabel = "cordmux.dock.tab_people.label";
inline constexpr const char *kDockTabAudioLabel = "cordmux.dock.tab_audio.label";
inline constexpr const char *kDockTabDisplayLabel = "cordmux.dock.tab_display.label";
inline constexpr const char *kDockTabStatusLabel = "cordmux.dock.tab_status.label";
inline constexpr const char *kDockTabStatusCountLabel = "cordmux.dock.tab_status_count.label";
// ドックの上部の行の名前と、参加者の行の [メイン] のタグ
inline constexpr const char *kDockAccountMainLabel = "cordmux.dock.account_main.label";
inline constexpr const char *kDockAccountSubLabel = "cordmux.dock.account_sub.label";
// 上部の行のボタン
inline constexpr const char *kDockAccountMoveButton = "cordmux.dock.account_move.button";
inline constexpr const char *kDockAccountLeaveButton = "cordmux.dock.account_leave.button";
inline constexpr const char *kDockAccountChangeButton = "cordmux.dock.account_change.button";
// 上部の行の、通話の名前の代わりに表示する値
inline constexpr const char *kDockAccountNotConnectedStatus = "cordmux.dock.account_not_connected.status";
inline constexpr const char *kDockAccountNoCallStatus = "cordmux.dock.account_no_call.status";
// [移動] と [退出] を押せない理由
inline constexpr const char *kDockAccountNotConnectedTip = "cordmux.dock.account_not_connected.tip";
inline constexpr const char *kDockAccountSameCallTip = "cordmux.dock.account_same_call.tip";
inline constexpr const char *kDockAccountMainCallUnknownTip = "cordmux.dock.account_main_call_unknown.tip";
inline constexpr const char *kDockAccountCannotJoinTip = "cordmux.dock.account_cannot_join.tip";
inline constexpr const char *kDockAccountSubNoCallTip = "cordmux.dock.account_sub_no_call.tip";
inline constexpr const char *kDockAccountCallControlMissingTip = "cordmux.dock.account_call_control_missing.tip";
// [変更] のメニューの項目
inline constexpr const char *kDockAccountNoMainMenu = "cordmux.dock.account_no_main.menu";
// 参加者タブの上の行。%1=枠に載っている数、%2=枠の数
inline constexpr const char *kDockSlotsUsedStatus = "cordmux.dock.slots_used.status";
// 参加者タブの 2 組の見出し。%1=その組の人数
inline constexpr const char *kDockGroupSharingHeading = "cordmux.dock.group_sharing.heading";
inline constexpr const char *kDockGroupInCallHeading = "cordmux.dock.group_in_call.heading";
// 参加者の行の [自動] のタグ
inline constexpr const char *kDockRowBadgeAutoLabel = "cordmux.dock.row_badge_auto.label";
// 参加者の行の、画面のアイコンのツールチップ (映っている枠の位置)
inline constexpr const char *kDockSlotTopLeftTip = "cordmux.dock.slot_top_left.tip";
inline constexpr const char *kDockSlotTopRightTip = "cordmux.dock.slot_top_right.tip";
inline constexpr const char *kDockSlotBottomLeftTip = "cordmux.dock.slot_bottom_left.tip";
inline constexpr const char *kDockSlotBottomRightTip = "cordmux.dock.slot_bottom_right.tip";
inline constexpr const char *kDockSlotFullTip = "cordmux.dock.slot_full.tip";
// 参加者の行の、マイクのボタンを押せない理由 (まだ声が届いていない)
inline constexpr const char *kDockRowNoVoiceTip = "cordmux.dock.row_no_voice.tip";
// 押した場所の隣で確認するときのボタン。[退出] の確定は account_leave.button を使う
// やめる側のボタン。押した結果が分かる文言にする
inline constexpr const char *kDockConfirmStayInCallButton = "cordmux.dock.confirm_stay_in_call.button";
// 枠が埋まっているときに、チェックを押して開くメニューの名前
inline constexpr const char *kDockReplaceWithMenu = "cordmux.dock.replace_with.menu";
// 表示タブの組の見出し (ダミー映像。表示タブのいちばん下)
inline constexpr const char *kLookHeadingDummyHeading = "cordmux.look.heading_dummy.heading";
// 表示タブの組の見出し (枠)。名前ラベルと話している人の強調の組の見出しは kLookHeadingLabelsHeading と kLookHeadingSpeakingHeading
inline constexpr const char *kLookHeadingSlotsHeading = "cordmux.look.heading_slots.heading";
// 同上 (名前ラベル)
inline constexpr const char *kLookHeadingLabelsHeading = "cordmux.look.heading_labels.heading";
// 表示タブの [並び順]
inline constexpr const char *kLookSlotOrderLabel = "cordmux.look.slot_order.label";
// 表示タブの [並び順] の選択肢
inline constexpr const char *kLookSlotOrderNameOption = "cordmux.look.slot_order_name.option";
inline constexpr const char *kLookSlotOrderAddedOption = "cordmux.look.slot_order_added.option";
// ドックの 1 行目の平常の要約
// %1=通話に参加している人数、%2=枠に映像が表示されている人数。%1 が 1 のときは _one
inline constexpr CountedId kDockSummaryCallStatus = {
	"cordmux.dock.summary_call.status",
	"cordmux.dock.summary_call.status_one",
};
// 同上 (枠に映像が表示されている人がいないとき)
// %1=通話に参加している人数。1 のときは _one
inline constexpr CountedId kDockSummaryNoneDisplayedStatus = {
	"cordmux.dock.summary_none_displayed.status",
	"cordmux.dock.summary_none_displayed.status_one",
};
// 幅が足りないときの、区切りの後ろを省いた形。summary_call と summary_none_displayed の両方の短い形
// %1=通話に参加している人数。1 のときは _one
inline constexpr CountedId kDockSummaryCallShortStatus = {
	"cordmux.dock.summary_call_short.status",
	"cordmux.dock.summary_call_short.status_one",
};
// ドックの 1 行目の平常の要約 (Vesktop と接続していて、通話に参加していないとき)
inline constexpr const char *kDockSummaryConnectedStatus = "cordmux.dock.summary_connected.status";
// summary_connected の、区切りの後ろを省いた形
inline constexpr const char *kDockSummaryConnectedShortStatus = "cordmux.dock.summary_connected_short.status";
// ドックの 1 行目の平常の要約 (Vesktop が起動していないとき)。警告のアイコンは付けない
inline constexpr const char *kDockSummaryVesktopNotRunningStatus = "cordmux.dock.summary_vesktop_not_running.status";
// ドックの 1 行目。起動した直後に、Vesktop が動いているかを問い合わせている間だけ
inline constexpr const char *kDockSummaryCheckingStatus = "cordmux.dock.summary_checking.status";
// ドックの 1 行目。利用者が cordmux を無効にしている間
inline constexpr const char *kDockSummaryDisabledStatus = "cordmux.dock.summary_disabled.status";
// ドックの 1 行目。ダミーを表示している間
inline constexpr const char *kDockSummaryPlaceholdersStatus = "cordmux.dock.summary_placeholders.status";
// summary_placeholders の、区切りの後ろを省いた形
inline constexpr const char *kDockSummaryPlaceholdersShortStatus = "cordmux.dock.summary_placeholders_short.status";
// ドックの 1 行目。1 人を全画面で表示している間 (%1=全画面で表示している人の、配信に出している名前)
inline constexpr const char *kDockSummaryFullScreenStatus = "cordmux.dock.summary_full_screen.status";
// 1 人を全画面で表示している間の、1 行目のボタン (元の配置に戻す)
inline constexpr const char *kDockFullScreenRestoreButton = "cordmux.dock.full_screen_restore.button";
// 無効にしている間の、1 行目のボタン
inline constexpr const char *kDockEnableButton = "cordmux.dock.enable.button";
// 同上。1 行目に「cordmuxは無効です」と並べて入らない幅で使う短い形
inline constexpr const char *kDockEnableShortButton = "cordmux.dock.enable_short.button";
// 1 行目のアクセシブルな説明とツールチップ (押すと何が起きるか)
inline constexpr const char *kDockFirstLineA11y = "cordmux.dock.first_line.a11y";
// 1 行目の見出しの後ろに添える、残りの件数
// %1=1 行目の見出しのほかに表示している、メッセージのレベル 1 の件数
inline constexpr const char *kDockMoreNoticesStatus = "cordmux.dock.more_notices.status";
// ドックの 1 行目。アンインストールの最中
inline constexpr const char *kDockUninstallingStatus = "cordmux.dock.uninstalling.status";
// アンインストールを終えた後の、ドックのカードの見出し (1 行目にも表示する)
inline constexpr const char *kDockUninstallDoneHeadline = "cordmux.dock.uninstall_done.headline";
// 同じカードの本文 (次にすること。場所の行には削除するフォルダーを表示する)
inline constexpr const char *kDockUninstallDoneText = "cordmux.dock.uninstall_done.text";
// 手順書と違う置き方 (DLL のフォルダーが plugins\<名前> でない) で、削除するものが 1 つのフォルダーにまとまっていないときの本文
inline constexpr const char *kDockUninstallDoneLooseText = "cordmux.dock.uninstall_done_loose.text";
// カードの中の、準備の段階の行 (「名前: 値」の全体を 1 つのキーに持つ。「: 」をコードでつながないため)
inline constexpr const char *kDockStageVesktopConnectedStatus = "cordmux.dock.stage_vesktop_connected.status";
// 同上
inline constexpr const char *kDockStageVesktopDisconnectedStatus = "cordmux.dock.stage_vesktop_disconnected.status";
// 同上。問い合わせている間だけ
inline constexpr const char *kDockStageVesktopCheckingStatus = "cordmux.dock.stage_vesktop_checking.status";
// 同上
// %1=通話に参加している人数。1 のときは _one
inline constexpr CountedId kDockStageCallJoinedStatus = {
	"cordmux.dock.stage_call_joined.status",
	"cordmux.dock.stage_call_joined.status_one",
};
// 同上。Vesktop と接続していない間は、行ごと表示しない
inline constexpr const char *kDockStageCallNotJoinedStatus = "cordmux.dock.stage_call_not_joined.status";
// 同上
inline constexpr const char *kDockStageCompositeAddedStatus = "cordmux.dock.stage_composite_added.status";
// 同上
inline constexpr const char *kDockStageCompositeNotAddedStatus = "cordmux.dock.stage_composite_not_added.status";
// メッセージのレベル 2 の見出し (規則の VencordDirNotConfigured)
inline constexpr const char *kDockVesktopNotAddedHeadline = "cordmux.dock.vesktop_not_added.headline";
// 影響。Vesktop と接続していないときの、vesktop_not_added、vencord_elsewhere、vencord_overwritten と、追加の失敗の
// メッセージに共通
inline constexpr const char *kDockCannotReceiveText = "cordmux.dock.cannot_receive.text";
// 影響。vencord_elsewhere と vencord_overwritten のメッセージを、接続している間に表示するとき (規則の InstallMismatch が
// 同じ行を表示する)
inline constexpr const char *kDockCannotReceiveAfterRestartText = "cordmux.dock.cannot_receive_after_restart.text";
// vesktop_not_added と vencord_elsewhere のメッセージのボタン。インストールした日も、設定を書き直すときも同じ名前
inline constexpr const char *kDockAddToVesktopButton = "cordmux.dock.add_to_vesktop.button";
// [Vesktopに追加] を押してから終わるまでのボタンのテキスト。アクセシブルな説明には add_to_vesktop.button のテキストを
// 残す
inline constexpr const char *kDockAddToVesktopBusyButton = "cordmux.dock.add_to_vesktop_busy.button";
// [Vesktopに追加] を押せない理由 (Vesktop の起動中)。ボタンの下に文で表示する。アンインストールの
// settings.uninstall_vesktop_running.text と同じ形 (できないこと、次の操作) にそろえる。Vesktop は起動したときに
// 1 回だけ設定を読み、ウィンドウを動かすたびにメモリの値で設定のファイルを書き直すので、起動中に書いた値は消える
// (actions.cpp の WriteVencordDir の行)
inline constexpr const char *kDockAddVesktopRunningText = "cordmux.dock.add_vesktop_running.text";
// [Vesktopに追加] を押せない理由 (Vesktop が起動しているかを、まだ読めていない)。
// settings.uninstall_checking.text と同じ「確認しています」の形
inline constexpr const char *kDockVesktopCheckingText = "cordmux.dock.vesktop_checking.text";
// メッセージのレベル 2 (配信中で枠の映像が損なわれているならメッセージのレベル 1) の見出し。影響は cannot_receive、ボタンは
// add_to_vesktop
inline constexpr const char *kDockVesktopAddFailedHeadline = "cordmux.dock.vesktop_add_failed.headline";
// [Vesktopに追加] の失敗の理由の 1 文。見出しの直後、影響の前に置く。VesktopAddFailure::StatePathUnknown。
// Vesktop を一度も起動していない人がここで止まるので、次の操作 (一度起動してから押し直す) を同じキーに続ける
// (bundled_missing と同じく、理由と次の操作を 1 つのキーに持つ)
inline constexpr const char *kDockVesktopAddFailedStateMissingText =
	"cordmux.dock.vesktop_add_failed_state_missing.text";
// 同上。StateUnreadable
inline constexpr const char *kDockVesktopAddFailedStateUnreadableText =
	"cordmux.dock.vesktop_add_failed_state_unreadable.text";
// 同上。StateWriteFailed
inline constexpr const char *kDockVesktopAddFailedStateWriteText = "cordmux.dock.vesktop_add_failed_state_write.text";
// 同上。BackupWriteFailed (押し直すと直ることがあるので、[Vesktopに追加] が次の操作)
inline constexpr const char *kDockVesktopAddFailedBackupText = "cordmux.dock.vesktop_add_failed_backup.text";
// 同上。NoBackupPlace (バックアップの保存先が起動のときに決まらなかった)。同じ起動の中では直らず、ボタンを置かない
// ので、理由の 1 文に次の操作の 1 文を続ける
inline constexpr const char *kDockVesktopAddFailedNoBackupText = "cordmux.dock.vesktop_add_failed_no_backup.text";
// 同上。BundledDirUnknown。押し直しても直らないので、次の操作まで書く (files_mixed.text と同じ次の操作)
inline constexpr const char *kDockVesktopAddFailedBundledMissingText =
	"cordmux.dock.vesktop_add_failed_bundled_missing.text";
// 同上。WritesDisabled のうち、cordmux の settings.json の allow_state_json_write を手で false にしたとき。ボタンを
// 置かないので、次の操作の 1 文まで書く。押す前の状態のカードも、この起動で書き込まない間
// (Install::vesktop_writes_off) は、本文の 2 つ目にこのテキストを使う (rules_dock.cpp の install_notice)
inline constexpr const char *kDockVesktopAddFailedWritesOffText = "cordmux.dock.vesktop_add_failed_writes_off.text";
// 同上。WritesDisabled のうち、起動のときに settings.json を読めなかったので書き込まないとき (plugin-main.cpp の
// effective_allow_state_json_write。OperationFacts::settings_unreadable_at_startup)。次の操作は、同じ起動の間ずっと
// 表示される settings_unread のカードが伝えるので、ここでは繰り返さない。押す前の状態のカードも、この起動で
// 書き込まない間 (Install::vesktop_writes_off) は、本文の 2 つ目にこのテキストを使う (rules_dock.cpp の install_notice)
inline constexpr const char *kDockVesktopAddFailedSettingsUnreadText =
	"cordmux.dock.vesktop_add_failed_settings_unread.text";
// 同上。PortableVesktop (動いているポータブル版の Vesktop を見たので書かなかった)。設定の場所を OBS の側から知れず、
// 同じ起動の中では変わらないので、ボタンを置かない
inline constexpr const char *kDockVesktopAddFailedPortableText = "cordmux.dock.vesktop_add_failed_portable.text";
// メッセージのレベル 2 の見出し (規則の VesktopRemoveFailed。状態タブの [Vesktopから削除] が失敗した)。本文は
// 理由の 1 文だけで、ボタンは置かない ([Vesktopから削除] は状態タブの「Vesktop」の組にあり、押し直せる)。理由の文は、
// [Vesktopに追加] の失敗と同じ事実のものはそのキーを使う (state_unreadable、state_write、writes_off、settings_unread)
inline constexpr const char *kDockVesktopRemoveFailedHeadline = "cordmux.dock.vesktop_remove_failed.headline";
// 同上の理由。VesktopRemoveFailure::NotRestorable (cordmux を追加する前の値が分からない)
inline constexpr const char *kDockVesktopRemoveFailedNotRestorableText =
	"cordmux.dock.vesktop_remove_failed_not_restorable.text";
// Vesktop の設定が、ほかの Vencord を指しているときの見出し。メッセージのレベル 2 にし、配信中で枠の映像が
// 損なわれているならメッセージのレベル 1 にする。ボタンは add_to_vesktop
inline constexpr const char *kDockVencordElsewhereHeadline = "cordmux.dock.vencord_elsewhere.headline";
// Vesktop の cordmux が上書きされたときの見出し。メッセージのレベル 2 にし、配信中で枠の映像が損なわれているなら
// メッセージのレベル 1 にする。消えたファイルの名前は画面に表示しない
inline constexpr const char *kDockVencordOverwrittenHeadline = "cordmux.dock.vencord_overwritten.headline";
// vencord_overwritten のメッセージのボタン
inline constexpr const char *kDockReinstallButton = "cordmux.dock.reinstall.button";
// vencord_overwritten のメッセージに加える段落 (規則の ReinstallFailed。見出しは vencord_overwritten のものを使う)。
// ファイルの名前は画面に表示しない
inline constexpr const char *kDockReinstallFailedText = "cordmux.dock.reinstall_failed.text";
// メッセージのレベル 2 の見出し (規則の RestartVesktop)
inline constexpr const char *kDockRestartVesktopHeadline = "cordmux.dock.restart_vesktop.headline";
// 影響。何を変えたかを言わない中立の文にする。この規則に来るのは、Vesktop の起動中に [再インストール] で同梱の
// Vencord のファイルをコピーし直したときがほとんどで (Vesktop の設定は起動中に書かない。rules.cpp の RestartVesktop
// の行)、「設定を変更した」とは限らないため
inline constexpr const char *kDockRestartVesktopText = "cordmux.dock.restart_vesktop.text";
// メッセージのレベル 2 の見出し (規則の VersionRestartVesktop)
inline constexpr const char *kDockVersionDiffersHeadline = "cordmux.dock.version_differs.headline";
// version_differs の影響と次の操作。restart_vesktop.headline は句点のない見出しなので、別のキーにする。接続して
// 動いている間にだけ表示するので、何が続くか (現在の接続) を先に伝え、再起動は配信の後などでよいと伝える (隣の
// 上書きのカードの cannot_receive_after_restart と同じ「現在の接続はそのまま続きます」)
inline constexpr const char *kDockVersionDiffersText = "cordmux.dock.version_differs.text";
// メッセージのレベル 2 の見出し (規則の VersionMixedBundle)
inline constexpr const char *kDockFilesMixedHeadline = "cordmux.dock.files_mixed.headline";
// 影響と次の操作。何が続くかを先に伝え、展開し直すのは配信の後などでよいと伝える。バージョンの比較を読むのは
// このメッセージだけで、待ち受け、接続、枠、録画はどれも止まらない。接続していない間にも表示するので、
// version_differs.text の「現在の接続は」では始めない
inline constexpr const char *kDockFilesMixedText = "cordmux.dock.files_mixed.text";
// メッセージのレベル 1 の見出し。切断の理由は見出しに入れず、ログに書く
// %1=切断された時刻 (時と分。QLocale の形)
inline constexpr const char *kDockDisconnectedHeadline = "cordmux.dock.disconnected.headline";
// 切断された時刻が読めないとき (時刻の代わりに「-」を入れないため、別のキーにする)
inline constexpr const char *kDockDisconnectedNoTimeHeadline = "cordmux.dock.disconnected_no_time.headline";
// 影響。応答がないとき、相手が切断したとき、理由が分からないときに使う。再接続するのは Vesktop の側なので、
// 再接続されないときの次の操作 (Vesktop の再起動) を、disconnected_auth と同じ形で続ける
inline constexpr const char *kDockDisconnectedText = "cordmux.dock.disconnected.text";
// 影響と次の操作。接続が切れた後に Vesktop が動いていないと分かったときの、メッセージのレベル 1 の本文 (配信中で枠に
// 人が載っているとき。rules.cpp の ConnectedThenDropped)。語は summary_vesktop_not_running.status の
// 「Vesktopが起動していません」にそろえる
inline constexpr const char *kDockDisconnectedVesktopNotRunningText =
	"cordmux.dock.disconnected_vesktop_not_running.text";
// cordmux の録画の最中に、影響に加える 1 文
inline constexpr const char *kDockDisconnectedRecordingText = "cordmux.dock.disconnected_recording.text";
// トークンが合わずに切断されたときの影響
inline constexpr const char *kDockDisconnectedAuthText = "cordmux.dock.disconnected_auth.text";
// バージョンが食い違って切断されたときの影響
inline constexpr const char *kDockDisconnectedVersionText = "cordmux.dock.disconnected_version.text";
// メッセージのレベル 2 の見出し (規則の ListenPortsExhausted)
inline constexpr const char *kDockPortsInUseHeadline = "cordmux.dock.ports_in_use.headline";
// 影響
// %1=最初のポート番号、%2=最後のポート番号 (桁区切りなし)
inline constexpr const char *kDockPortsInUseText = "cordmux.dock.ports_in_use.text";
// 次の操作。ports_in_use の本文で、cannot_receive.text の影響の後に置く 1 文。待ち受けは 5 秒ごとに開き直すが、
// 確実に直る方法として OBS の再起動を案内する。受け付けの開始に失敗したとき (ListenerFacts::accept_failed) は、
// ポートの文を表示しない
inline constexpr const char *kDockRestartObsText = "cordmux.dock.restart_obs.text";
// メッセージのレベル 2 の見出し (規則の OtherObsHolding)
inline constexpr const char *kDockOtherObsHeadline = "cordmux.dock.other_obs.headline";
// 影響と次の操作
inline constexpr const char *kDockOtherObsText = "cordmux.dock.other_obs.text";
// Vesktop が動いているのに接続されないときの見出し (規則の VesktopRunningNotLinked)。表示するまでの待ちを通す
inline constexpr const char *kDockVesktopNotConnectedHeadline = "cordmux.dock.vesktop_not_connected.headline";
// [移動] と [退出] を押したが通話が変わらなかったときのメッセージ
inline constexpr const char *kDockCallMoveFailedHeadline = "cordmux.dock.call_move_failed.headline";
inline constexpr const char *kDockCallLeaveFailedHeadline = "cordmux.dock.call_leave_failed.headline";
inline constexpr const char *kDockCallActionFailedText = "cordmux.dock.call_action_failed.text";
// 影響と次の操作
inline constexpr const char *kDockVesktopNotConnectedText = "cordmux.dock.vesktop_not_connected.text";
// 接続のための情報のファイル (session.json) を保存できなかったときの見出し (ws::OpenProblem::SessionFileNotSaved)。
// 待ち受けは開いているが、Vesktop は接続できない。メッセージのレベルは rules_dock.cpp が決める
inline constexpr const char *kDockSessionNotSavedHeadline = "cordmux.dock.session_not_saved.headline";
// 原因と影響と次の操作 (vesktop_not_connected.text と同じ形。保存し直すのは 5 秒ごとのポーリングだが、確実に直る
// 方法として次の操作は OBS の再起動)
inline constexpr const char *kDockSessionNotSavedText = "cordmux.dock.session_not_saved.text";
// Discord の内部への依存箇所が見つからないときの見出し (受け取る映像と音声)。依存箇所のメッセージは 1 件にまとめ、
// いちばん重大なものを表示する
inline constexpr const char *kDockContactMediaHeadline = "cordmux.dock.contact_media.headline";
// 同上 (誰が画面共有しているか、視聴を始める操作)
inline constexpr const char *kDockContactSharingHeadline = "cordmux.dock.contact_sharing.headline";
// 同上 (どの映像と音声が誰のものか)
inline constexpr const char *kDockContactSsrcHeadline = "cordmux.dock.contact_ssrc.headline";
// 同上 (通話の参加者の一覧)
inline constexpr const char *kDockContactRosterHeadline = "cordmux.dock.contact_roster.headline";
// 同上 (名前とアバター)
inline constexpr const char *kDockContactProfileHeadline = "cordmux.dock.contact_profile.headline";
// 同上 (Vesktop のアカウントが誰か)
inline constexpr const char *kDockContactSelfHeadline = "cordmux.dock.contact_self.headline";
inline constexpr const char *kDockContactCallHeadline = "cordmux.dock.contact_call.headline";
inline constexpr const char *kDockContactVoiceSelectHeadline = "cordmux.dock.contact_voice_select.headline";
// 依存箇所のメッセージの影響 (どの見出しでも同じテキスト)
inline constexpr const char *kDockContactText = "cordmux.dock.contact.text";
// 依存箇所のメッセージの 2 つ目の段落。Vesktop の画面が表示されないときの戻し方 (Vesktop を終了して
// [Vesktopから削除] を押す) と、cordmux を更新した後にもう一度 [Vesktopに追加] を押すこと。このメッセージは一部の依存箇所だけが
// 見つからないとき (画面は表示される) にも出るので、「表示されないときは」の条件付きで書く。Discord の更新に
// 同梱の Vencord が合わないと、Vesktop の画面が黒いままになり、cordmux の側からは修正できないため
inline constexpr const char *kDockContactNoScreenText = "cordmux.dock.contact_no_screen.text";
// 依存箇所のメッセージのボタン。押すと、設定ウィンドウを [バージョン情報] のページで開いてから始める
inline constexpr const char *kDockExportDiagnosticsButton = "cordmux.dock.export_diagnostics.button";
// メッセージのレベル 2 の見出し (規則の CompositeAbsent)。影響と次の操作は composite_absent.text
inline constexpr const char *kDockCompositeAbsentHeadline = "cordmux.dock.composite_absent.headline";
// 影響と次の操作 (OBS の「ソース」の [+] から cordmux を追加する)
inline constexpr const char *kDockCompositeAbsentText = "cordmux.dock.composite_absent.text";
// メッセージのレベル 3 の行 (規則の RecordingOnly)
// %1=枠に載っていないが、cordmux の録画のために受け取っている人数。1 のときは _one
inline constexpr CountedId kDockRecordingOnlyHeadline = {
	"cordmux.dock.recording_only.headline",
	"cordmux.dock.recording_only.headline_one",
};
// recording_only の行のツールチップ
inline constexpr const char *kDockRecordingOnlyTip = "cordmux.dock.recording_only.tip";
// メッセージのレベル 2 の見出し (規則の DoubleAudio。声が 2 重に配信に入りうるソースを名前で示す)
// %1=Vesktop か Discord の音声をキャプチャしている OBS のソースの名前 (音声ミキサーに表示される名前)
inline constexpr const char *kDockDoubleAudioHeadline = "cordmux.dock.double_audio.headline";
// 影響と次の操作
inline constexpr const char *kDockDoubleAudioText = "cordmux.dock.double_audio.text";
// メッセージのレベル 2 の見出し (規則の BroadcasterNotInCall)
// %1=メインアカウントの名前 (ID の数字ではない)
inline constexpr const char *kDockMainAccountNotInCallHeadline = "cordmux.dock.main_account_not_in_call.headline";
// メインアカウントの名前がまだ分からないとき
inline constexpr const char *kDockMainAccountNotInCallNoNameHeadline =
	"cordmux.dock.main_account_not_in_call_no_name.headline";
// 影響と次の操作
inline constexpr const char *kDockMainAccountNotInCallText = "cordmux.dock.main_account_not_in_call.text";
// メッセージのレベル 1 の見出し (画面共有の映像が届いていない人がいるとき)
// %1=映像が届いていない人数。1 のときは _one
inline constexpr CountedId kDockNoVideoHeadline = {
	"cordmux.dock.no_video.headline",
	"cordmux.dock.no_video.headline_one",
};
// 影響。枠に載っている人の、届いていた映像が止まったとき。録画の対象だけの人には no_video_recording.text を、次の
// 操作には no_video_next.text を使う (両方に当てはまるときは、枠の文、録画の文、次の操作の順に並べる)。最初のフレームが
// まだ届いていない人の枠は空なので、その人には no_video_waiting.text を使う。文の中で対象の人 (映像が途切れた人) を
// 示すのは、no_video_waiting.text と並んでも食い違わないようにするためである
inline constexpr const char *kDockNoVideoText = "cordmux.dock.no_video.text";
// 影響。枠に載っている人で、画面共有していると分かってから、最初のフレームが 1 つも届いていないとき
// (rules_dock.cpp の first_frame_missing。送信側が最初にエンコーダを確保するときに、すべての段階で失敗した人を
// 含む)。その人の枠は空いたままである。見出しの人数に数えた人に、映像が途切れた人もいれば、no_video.text と
// 両方を 1 回ずつ、no_video.text の後に置く。録画の文と次の操作はその後に置く。文の中で対象の人を示すので、
// 単独でも並べても読める。枠に載っていない人には使わない (枠がないため)
inline constexpr const char *kDockNoVideoWaitingText = "cordmux.dock.no_video_waiting.text";
// 影響。録画の対象だけの人 (枠に載っていない人) の映像が届いていないとき
inline constexpr const char *kDockNoVideoRecordingText = "cordmux.dock.no_video_recording.text";
// 画面共有の映像が届かないメッセージの次の操作。影響の文の後に 1 回置き、Vesktop で画面共有の映像が映っているかを
// 確認するよう勧める
inline constexpr const char *kDockNoVideoNextText = "cordmux.dock.no_video_next.text";
// 画面共有の映像が届かないメッセージの次の操作。見出しの人数に数えた人に、段階が unavailable の人 (Vesktop の
// cordmux が映像をエンコードできず、送っていない人。行は row_cant_show) がいるときに使い、画面共有を始め直して
// もらうよう勧める。数えた人に unavailable でない人もいれば、no_video_next.text の後に置く。文の中で対象の人
// (映像を表示できない人) を示すので、単独でも並べても読める。行のテキストを「」で引用しないのは、狭い行では
// 短い形 (row_cant_show_short) になるためである
inline constexpr const char *kDockNoVideoCantShowNextText = "cordmux.dock.no_video_cant_show_next.text";
// メッセージのレベル 2 の見出し (規則の RecordSpaceLow)
// %1=残りの分 (4 桁以上は桁区切りを入れる)。1 のときは _one
inline constexpr CountedId kDockSpaceLowHeadline = {
	"cordmux.dock.space_low.headline",
	"cordmux.dock.space_low.headline_one",
};
// 影響と次の操作。録画が止まるのは空き容量がほぼ 0 のときで、録画ファイルの作成は同じ保存先に書くので、空き容量が尽きて
// 止まった録画は作成できない。そのため、録画ファイルの作成にも空き容量が必要なことと、次の操作を伝える
inline constexpr const char *kDockSpaceLowText = "cordmux.dock.space_low.text";
// メッセージのレベル 2 の見出し (規則の RecordFinalizeShort)
// %1=足りない容量 (1024 の累乗で数え、小数 1 桁と GB か MB)
inline constexpr const char *kDockFinalizeSpaceShortHeadline = "cordmux.dock.finalize_space_short.headline";
// 影響と次の操作
inline constexpr const char *kDockFinalizeSpaceShortText = "cordmux.dock.finalize_space_short.text";
// メッセージのレベル 3 の行 (規則の RecordFinalizeRunning)
inline constexpr const char *kDockFinalizingHeadline = "cordmux.dock.finalizing.headline";
// finalizing の行の 2 行目。現在のファイルの進み具合の割合が分からない間
// %1=終わったファイルの数、%2=全体のファイルの数、%3=経過した分。%2 が 1 のときは _one
inline constexpr CountedId kDockFinalizeProgressStatus = {
	"cordmux.dock.finalize_progress.status",
	"cordmux.dock.finalize_progress.status_one",
};
// 同上。割合が分かるとき
// %1=終わった数、%2=全体の数、%3=現在のファイルの順番、%4=現在のファイルの終わった割合 (整数)、%5=経過した分。
// %2 が 1 のときは _one
inline constexpr CountedId kDockFinalizeProgressFileStatus = {
	"cordmux.dock.finalize_progress_file.status",
	"cordmux.dock.finalize_progress_file.status_one",
};
// 同上。数が決まった時間進まないとき (設定の record_finalize_stall_s)
// %1=終わった数、%2=全体の数、%3=止まっているファイルの順番、%4=最後に進んでからの分。%2 が 1 のときは _one
inline constexpr CountedId kDockFinalizeStalledStatus = {
	"cordmux.dock.finalize_stalled.status",
	"cordmux.dock.finalize_stalled.status_one",
};
// メッセージのレベル 2 の見出し (規則の RecordFinalizePending)
inline constexpr const char *kDockFinalizePendingHeadline = "cordmux.dock.finalize_pending.headline";
// 影響
inline constexpr const char *kDockFinalizePendingText = "cordmux.dock.finalize_pending.text";
// finalize_pending のメッセージのボタン (finalize_failed のメッセージも使う)
inline constexpr const char *kDockResumeFinalizingButton = "cordmux.dock.resume_finalizing.button";
// メッセージのレベル 1 の見出し (規則の RecordFinalizeFailed。録画ファイルの作成のセッションが Failed、つまり作成できなかった
// ファイルが 1 つでもある)。影響は finalize_pending.text (元のファイルは 1 つも削除していない)、次の操作は
// finalize_failed_next.text、ボタンは resume_finalizing
inline constexpr const char *kDockFinalizeFailedHeadline = "cordmux.dock.finalize_failed.headline";
// 同上の次の操作。影響の後に置く。空き容量で止まった録画もこのメッセージになるので、空きを確認してから押し直して
// もらう。空きを見る場所は「次のフォルダー」(すぐ後の場所の行。そのセッションのフォルダー) のドライブにする。
// 保存先を変えた後も、前の保存先のセッションがこのカードに表示されるので、「保存先」と書くと現在の保存先と読める
inline constexpr const char *kDockFinalizeFailedNextText = "cordmux.dock.finalize_failed_next.text";
// 同上の次の操作で、同梱の FFmpeg を起動できなかったとき(RecordingFacts::finalize_ffmpeg_unavailable)。空き容量を
// 見ても直らないので、FFmpeg のファイルが戻るように展開し直してもらう
inline constexpr const char *kDockFinalizeFailedFfmpegNextText = "cordmux.dock.finalize_failed_ffmpeg_next.text";
// 規則の RecordNotReachedSender の見出し
inline constexpr const char *kDockRecordNotReachedHeadline = "cordmux.dock.record_not_reached.headline";
// 影響。表示するのは接続全体の、メッセージのレベル 3 の行のツールチップで、行に人がいないので「この人の」を入れない
inline constexpr const char *kDockRecordNotReachedText = "cordmux.dock.record_not_reached.text";
// 規則の RecordDirUnwritable の見出し (保存先に書き込めず、録画を開始できなかったとき)。録画を始めようとして
// 書き込めなかったときはメッセージのレベル 1、今は始めようとしていないときはメッセージのレベル 2 にする
inline constexpr const char *kDockRecordStartFailedHeadline = "cordmux.dock.record_start_failed.headline";
// 同じメッセージの影響 (書き込めなかった場所が分かれば、場所の行を添える)
inline constexpr const char *kDockSaveFolderUnwritableText = "cordmux.dock.save_folder_unwritable.text";
// 同じメッセージと record_stopped_space のメッセージのボタン。設定ウィンドウの [録画] のページを開く (ページを開くだけ
// なので「...」を付けない)
inline constexpr const char *kDockChangeSaveFolderButton = "cordmux.dock.change_save_folder.button";
// 次の操作。保存先に書き込めず録画を開始できなかったとき (StartFailed) だけ、save_folder_unwritable.text の後に
// 置く (保存先を変えても録画は始まらず、押し直す必要がある)。今は録画を始めていないときのメッセージのレベル 2 には
// 加えない。テキストは record_stopped_space.text の後半と同じ形にする
inline constexpr const char *kDockRecordStartAgainText = "cordmux.dock.record_start_again.text";
// メッセージのレベル 1 の見出し (規則の RecordStoppedSpaceExhausted)
inline constexpr const char *kDockRecordStoppedSpaceHeadline = "cordmux.dock.record_stopped_space.headline";
// 影響と次の操作。ボタンは change_save_folder。空き容量が尽きて止まった録画は作成できないので、元のファイルを
// 削除していないことを、finalize_pending.text と同じテキストで伝える
inline constexpr const char *kDockRecordStoppedSpaceText = "cordmux.dock.record_stopped_space.text";
// 1 行目の短い形 (Notice::headline_short)。record_stopped_space.headline の括弧の前をそのまま使う
inline constexpr const char *kDockRecordStoppedSpaceShortHeadline = "cordmux.dock.record_stopped_space_short.headline";
// ---- 1 行目の短い形 (Notice::headline_short。rules_dock.cpp の short_headline_of が見出しのキーから引く) ----
// 短い形の書き方は docs/ui_guidelines.md の「メッセージ」にある。
// vesktop_not_added の見出しの短い形
inline constexpr const char *kDockVesktopNotAddedShortHeadline = "cordmux.dock.vesktop_not_added_short.headline";
// vesktop_add_failed の見出しの短い形 (メッセージのレベル 1 になることがある)
inline constexpr const char *kDockVesktopAddFailedShortHeadline = "cordmux.dock.vesktop_add_failed_short.headline";
// vesktop_remove_failed の見出しの短い形
inline constexpr const char *kDockVesktopRemoveFailedShortHeadline =
	"cordmux.dock.vesktop_remove_failed_short.headline";
// vencord_elsewhere の見出しの短い形 (メッセージのレベル 1 になることがある)
inline constexpr const char *kDockVencordElsewhereShortHeadline = "cordmux.dock.vencord_elsewhere_short.headline";
// vencord_overwritten の見出しの短い形 (メッセージのレベル 1 になることがある。再インストールの失敗のカードも同じ
// 見出しを使う)
inline constexpr const char *kDockVencordOverwrittenShortHeadline = "cordmux.dock.vencord_overwritten_short.headline";
// version_differs の見出しの短い形。files_mixed の短い形とは別のテキスト
inline constexpr const char *kDockVersionDiffersShortHeadline = "cordmux.dock.version_differs_short.headline";
// files_mixed の見出しの短い形。version_differs の短い形とは別のテキスト
inline constexpr const char *kDockFilesMixedShortHeadline = "cordmux.dock.files_mixed_short.headline";
// vesktop_not_connected の見出しの短い形
inline constexpr const char *kDockVesktopNotConnectedShortHeadline = "cordmux.dock.vesktop_not_connected_short.headline";
// ports_in_use の見出しの短い形
inline constexpr const char *kDockPortsInUseShortHeadline = "cordmux.dock.ports_in_use_short.headline";
// contact_roster の見出しの短い形
inline constexpr const char *kDockContactRosterShortHeadline = "cordmux.dock.contact_roster_short.headline";
// contact_self の見出しの短い形
inline constexpr const char *kDockContactSelfShortHeadline = "cordmux.dock.contact_self_short.headline";
// composite_absent の見出しの短い形 (OBS にも別のソースがあるので、cordmux を省かない)
inline constexpr const char *kDockCompositeAbsentShortHeadline = "cordmux.dock.composite_absent_short.headline";
// double_audio の見出しの短い形 (ソースの名前を省く)
inline constexpr const char *kDockDoubleAudioShortHeadline = "cordmux.dock.double_audio_short.headline";
// main_account_not_in_call の見出し (名前があるときとないときの 2 つ) の短い形
inline constexpr const char *kDockMainAccountNotInCallShortHeadline =
	"cordmux.dock.main_account_not_in_call_short.headline";
// no_video の見出しの短い形 (人数を先頭に置く。英語は全文と同じテキスト)
// %1=映像が届いていない人数。1 のときは _one
inline constexpr CountedId kDockNoVideoShortHeadline = {
	"cordmux.dock.no_video_short.headline",
	"cordmux.dock.no_video_short.headline_one",
};
// finalize_space_short の見出しの短い形 (容量を残す。英語は全文と同じテキスト)
// %1=足りない容量 (1024 の累乗で数え、小数 1 桁と GB か MB)
inline constexpr const char *kDockFinalizeSpaceShortShortHeadline = "cordmux.dock.finalize_space_short_short.headline";
// finalize_pending の見出しの短い形 (日本語は全文と同じテキスト。finalize_failed の短い形とは別のテキスト)
inline constexpr const char *kDockFinalizePendingShortHeadline = "cordmux.dock.finalize_pending_short.headline";
// finalize_failed の見出しの短い形 (finalize_pending の短い形とは別のテキスト)。日本語の全文は先頭の
// 「作成できなかった」の前に助詞がなく、狭い幅で「作成でき...」と逆の意味に読めるので、助詞のある
// 「録画を作成できませんでした」にする。「録画ファイルの作成」は cordmux だけの操作なので、cordmux を書かない「録画」でも
// OBS の録画と取り違えない。「〜に失敗しました」の形にしない (docs/ui_guidelines.md の「エラーと押せない理由」)
inline constexpr const char *kDockFinalizeFailedShortHeadline = "cordmux.dock.finalize_failed_short.headline";
// メッセージのレベル 3 の行 (規則の RecordGap)
// %1=途切れ始めた時刻 (時と分)、%2=途切れた分。%2 が 1 のときは _one
inline constexpr CountedId kDockRecordGapHeadline = {
	"cordmux.dock.record_gap.headline",
	"cordmux.dock.record_gap.headline_one",
};
// メッセージのレベル 2 の見出し (規則の SettingsUnreadAtStartup)
inline constexpr const char *kDockSettingsUnreadHeadline = "cordmux.dock.settings_unread.headline";
// 影響
inline constexpr const char *kDockSettingsUnreadText = "cordmux.dock.settings_unread.text";
// メッセージのレベル 2 の見出し (規則の SettingsUnsaved)
inline constexpr const char *kDockSettingsUnsavedHeadline = "cordmux.dock.settings_unsaved.headline";
// 影響と次の操作
inline constexpr const char *kDockSettingsUnsavedText = "cordmux.dock.settings_unsaved.text";
// 設定ウィンドウを開くボタン
inline constexpr const char *kDockOpenSettingsButton = "cordmux.dock.open_settings.button";
// OBS のソースのプロパティの [cordmuxの設定を開く] (OBS の画面の中なので、cordmux の語を付ける)
inline constexpr const char *kSourceOpenSettingsButton = "cordmux.source.open_settings.button";
// メッセージのレベル 2 の見出し (規則の SeenUnreadAtStartup)
inline constexpr const char *kDockParticipantsUnreadHeadline = "cordmux.dock.participants_unread.headline";
// 影響 (settings_unread と同じ形)
inline constexpr const char *kDockParticipantsUnreadText = "cordmux.dock.participants_unread.text";
// メッセージのレベル 2 の見出し (規則の SeenUnsaved)
inline constexpr const char *kDockParticipantsUnsavedHeadline = "cordmux.dock.participants_unsaved.headline";
// 影響 (参加者の設定はすぐに反映されるので、やり直す [OK] がない)。保存は中身が変わるたびにやり直すので、この後
// どうなるかを 1 文続ける (修正する操作がないときの形)
inline constexpr const char *kDockParticipantsUnsavedText = "cordmux.dock.participants_unsaved.text";
// メッセージのレベル 3 の行 (規則の SeenDiscarded。設定のある過去の参加者を、上限を超えたために削除したとき)
// %1=削除した人数。1 のときは _one
inline constexpr CountedId kDockPastRemovedHeadline = {
	"cordmux.dock.past_removed.headline",
	"cordmux.dock.past_removed.headline_one",
};
// 影響
inline constexpr const char *kDockPastRemovedText = "cordmux.dock.past_removed.text";
// 人の行の状態 (映像を待っている)
inline constexpr const char *kDockRowWaitingStatus = "cordmux.dock.row_waiting.status";
// 状態のアイコンのツールチップとアクセシブルな説明で、状態のテキストに続ける 1 文 (枠に何が映っているか)
inline constexpr const char *kDockRowWaitingTip = "cordmux.dock.row_waiting.tip";
// row_waiting.tip の代わり。枠に載っていないが、cordmux の録画の対象の人の行に使う (枠がないので枠の話をしない。
// 影響のテキストは、メッセージの no_video_recording.text と同じく「映像と声」)
inline constexpr const char *kDockRowWaitingRecordingTip = "cordmux.dock.row_waiting_recording.tip";
// row_not_receiving_short.status と同じ使い方 (映像を待っている状態の短い形)
inline constexpr const char *kDockRowWaitingShortStatus = "cordmux.dock.row_waiting_short.status";
// 人の行の状態 (映像が届いていない)
inline constexpr const char *kDockRowNotReceivingStatus = "cordmux.dock.row_not_receiving.status";
// row_waiting.tip と同じ使い方 (状態のテキストに続ける 1 文)
inline constexpr const char *kDockRowNotReceivingTip = "cordmux.dock.row_not_receiving.tip";
// row_not_receiving.tip の代わり。枠に載っていないが、cordmux の録画の対象の人の行に使う (row_waiting_recording.tip と
// 同じ)
inline constexpr const char *kDockRowNotReceivingRecordingTip = "cordmux.dock.row_not_receiving_recording.tip";
// 人の行の状態の短い形。行には、幅にかかわらず短い形を表示する (全文はツールチップとアクセシブル名に入る)。日本語は
// 5 字までにして、最小の幅の 5.75 em でも省かずに表示できるテキストにする。英語は、語順で先頭が読める状態は全文と同じ
// テキストにする (修正漏れではない)。短い形をもつ状態は dock/name_check.h の status_short_key_for が決める
inline constexpr const char *kDockRowNotReceivingShortStatus = "cordmux.dock.row_not_receiving_short.status";
// 人の行の状態 (画面共有していない)
inline constexpr const char *kDockRowNotSharingStatus = "cordmux.dock.row_not_sharing.status";
// row_waiting.tip と同じ使い方 (状態のテキストに続ける 1 文)
inline constexpr const char *kDockRowNotSharingTip = "cordmux.dock.row_not_sharing.tip";
// row_not_receiving_short.status と同じ使い方 (英語は全文と同じテキスト)
inline constexpr const char *kDockRowNotSharingShortStatus = "cordmux.dock.row_not_sharing_short.status";
// 人の行の状態 (フレームレートを下げている)
inline constexpr const char *kDockRowFpsReducedStatus = "cordmux.dock.row_fps_reduced.status";
// 同上の短い形
inline constexpr const char *kDockRowFpsReducedShortStatus = "cordmux.dock.row_fps_reduced_short.status";
// 同上 (解像度を下げている)
inline constexpr const char *kDockRowResolutionReducedStatus = "cordmux.dock.row_resolution_reduced.status";
// 同上の短い形
inline constexpr const char *kDockRowResolutionReducedShortStatus = "cordmux.dock.row_resolution_reduced_short.status";
// 同上 (フレームレートと解像度の両方を下げている)
inline constexpr const char *kDockRowBothReducedStatus = "cordmux.dock.row_both_reduced.status";
// 同上の短い形
inline constexpr const char *kDockRowBothReducedShortStatus = "cordmux.dock.row_both_reduced_short.status";
// 同上 (映像を表示できない。段階が unavailable)
inline constexpr const char *kDockRowCantShowStatus = "cordmux.dock.row_cant_show.status";
// 同上。画質を下げているが、どの段階か分からないとき
inline constexpr const char *kDockRowQualityReducedStatus = "cordmux.dock.row_quality_reduced.status";
// 同上の短い形
inline constexpr const char *kDockRowQualityReducedShortStatus = "cordmux.dock.row_quality_reduced_short.status";
// 画質を下げている 4 つの状態のツールチップ
inline constexpr const char *kDockRowQualityTip = "cordmux.dock.row_quality.tip";
// 映像を表示できない状態のツールチップの 2 文目 (原因と影響。Vesktop の cordmux はこの人の映像を送らない =
// src-vencord/encode/ladder.ts の 5 つ目の段階)。原因のテキストは row_quality.tip とそろえる
inline constexpr const char *kDockRowCantShowTip = "cordmux.dock.row_cant_show.tip";
// row_not_receiving_short.status と同じ使い方 (英語は全文と同じテキスト)。row_cant_show.status の直後に置くと、
// 上の「同上」が続かなくなるので、ツールチップの後に置く
inline constexpr const char *kDockRowCantShowShortStatus = "cordmux.dock.row_cant_show_short.status";
// Vesktop のアカウント (PersonFact::is_receiver) の行の 2 行目のテキスト
inline constexpr const char *kDockRowReceivingAccountStatus = "cordmux.dock.row_receiving_account.status";
// 人の行の 2 行目 (cordmux での名前を付けた人の、Discord での名前)。アクセシブルな説明にも使う
// %1=Discord での名前
inline constexpr const char *kDockRowDiscordNameStatus = "cordmux.dock.row_discord_name.status";
// 同じ名前の人がいるときの 2 行目
// %1=Discord のユーザー名
inline constexpr const char *kDockRowUsernameStatus = "cordmux.dock.row_username.status";
// Discord での名前とユーザー名を並べるとき
// %1=Discord での名前、%2=ユーザー名
inline constexpr const char *kDockRowDiscordNameUsernameStatus = "cordmux.dock.row_discord_name_username.status";
// 人の行の 2 行目 (使う声を置き換えているとき)
// %1=使っている声の人の名前
inline constexpr const char *kDockRowVoiceStatus = "cordmux.dock.row_voice.status";
// 同上 (使う声に選んだ人が通話に参加していないとき)
// %1=使う声に選んだ人の名前
inline constexpr const char *kDockRowVoiceAbsentStatus = "cordmux.dock.row_voice_absent.status";
// 人の行の解像度 (ドックの幅が広いときだけ表示する)
// %1=幅、%2=高さ、%3=フレームレート (どれも桁区切りなし)
inline constexpr const char *kDockRowResolutionStatus = "cordmux.dock.row_resolution.status";
// 同上。フレームレートを読めないとき
// %1=幅、%2=高さ
inline constexpr const char *kDockRowResolutionSizeStatus = "cordmux.dock.row_resolution_size.status";
// 画面共有のアイコンのアクセシブル名
inline constexpr const char *kDockRowSharingA11y = "cordmux.dock.row_sharing.a11y";
// 画面共有のアイコンのツールチップ
inline constexpr const char *kDockRowSharingTip = "cordmux.dock.row_sharing.tip";
// 行のチェックボックスのアクセシブルな説明 (名前は表示している人の名前)
inline constexpr const char *kDockRowCheckA11y = "cordmux.dock.row_check.a11y";
// 行のチェックボックスのツールチップ
inline constexpr const char *kDockRowCheckTip = "cordmux.dock.row_check.tip";
// 自分の行のチェックが押せない理由 (サブアカウントがなく、自分の枠に映す映像を OBS から選んでいない間)
inline constexpr const char *kDockRowSelfNoVideoTip = "cordmux.dock.row_self_no_video.tip";
// 自分の行の状態 (自分の枠のチェックが付いていて、選んだ OBS のソースかシーンが見つからない)
inline constexpr const char *kDockRowSelfVideoMissingStatus = "cordmux.dock.row_self_video_missing.status";
// row_not_receiving_short.status と同じ使い方 (英語は全文と同じテキスト)
inline constexpr const char *kDockRowSelfVideoMissingShortStatus = "cordmux.dock.row_self_video_missing_short.status";
// row_waiting.tip と同じ使い方 (状態のテキストに続ける 1 文)
inline constexpr const char *kDockRowSelfVideoMissingTip = "cordmux.dock.row_self_video_missing.tip";
// 行の [...] のアクセシブル名
// %1=その行の人の名前
inline constexpr const char *kDockMoreActionsForA11y = "cordmux.dock.more_actions_for.a11y";
// 行の [...] のアクセシブル名。表示している名前が同じ人がほかにいて、ユーザー名が分かる行だけに使う
// %1=その行の人の名前 (表示している名前)、%2=Discord のユーザー名
inline constexpr const char *kDockMoreActionsForUsernameA11y = "cordmux.dock.more_actions_for_username.a11y";
// 下のバーの [...] のアクセシブル名とツールチップ、行の [...] のツールチップ (ツールチップはウィジェットの名前で、
// 文ではない)
inline constexpr const char *kDockMoreActionsA11y = "cordmux.dock.more_actions.a11y";
// 参加者の一覧のアクセシブル名
inline constexpr const char *kDockPeopleListA11y = "cordmux.dock.people_list.a11y";
// 一覧の上の 1 行、一覧のアクセシブルな説明、押せないチェックボックスのツールチップ、枠が埋まったときの
// スクリーンリーダーへの通知に使う
inline constexpr const char *kDockNoFreeSlotText = "cordmux.dock.no_free_slot.text";
inline constexpr const char *kDockRowCheckReplaceTip = "cordmux.dock.row_check_replace.tip";
inline constexpr const char *kDockRowCheckNotSharingTip = "cordmux.dock.row_check_not_sharing.tip";
// 幅が足りないときの、後ろの文を省いた形 (文の区切りをコードで決めないため、別のキーにする)
inline constexpr const char *kDockNoFreeSlotShortText = "cordmux.dock.no_free_slot_short.text";
// 一覧の上の 1 行 (自分の枠を表示していて、載せられる人数の上限に達したとき)
inline constexpr const char *kDockSelfSlotLimitText = "cordmux.dock.self_slot_limit.text";
// 一覧の上の 1 行が入らないときの短い形 (no_free_slot_short と同じ使い方)
inline constexpr const char *kDockSelfSlotLimitShortText = "cordmux.dock.self_slot_limit_short.text";
// 枠が空いたときの、スクリーンリーダーへの控えめな通知 (no_free_slot と対になる)
inline constexpr const char *kDockSlotFreeA11y = "cordmux.dock.slot_free.a11y";
// 一覧が空のときのテキスト (Vesktop と接続していない)
inline constexpr const char *kDockEmptyNotConnectedEmpty = "cordmux.dock.empty_not_connected.empty";
// 同上 (通話に参加していない)
inline constexpr const char *kDockEmptyNotInCallEmpty = "cordmux.dock.empty_not_in_call.empty";
// 同上 (通話にほかの人がいない)
inline constexpr const char *kDockEmptyAloneEmpty = "cordmux.dock.empty_alone.empty";
// アンインストールを始めた後の、空の一覧の 1 文 (語は uninstall_started.text とそろえる)
inline constexpr const char *kDockEmptyUninstalledEmpty = "cordmux.dock.empty_uninstalled.empty";
// 行の [...] のメニューの、チェックの付く項目
inline constexpr const char *kDockRowAutoAddMenu = "cordmux.dock.row_auto_add.menu";
// 同上の説明
inline constexpr const char *kDockRowAutoAddTip = "cordmux.dock.row_auto_add.tip";
// 同上を押せない理由
inline constexpr const char *kDockRowAutoAddDisabledTip = "cordmux.dock.row_auto_add_disabled.tip";
// 行の [...] のメニューの、チェックの付く項目 ([自動で枠に載せる] の次。チェックは、その人を全画面で表示している間)
inline constexpr const char *kDockRowFullScreenMenu = "cordmux.dock.row_full_screen.menu";
// 同上を押せない理由 (画面共有しておらず、合成のソースのタイルにも描いていない人の行)。View の押せない理由として持ち、メニューの項目には
// ツールチップを付けないので表示しない
inline constexpr const char *kDockRowFullScreenUnavailableTip = "cordmux.dock.row_full_screen_unavailable.tip";
// 行の [...] のメニューの項目
inline constexpr const char *kDockRowRenameMenu = "cordmux.dock.row_rename.menu";
// 行の [...] のメニューの、下に階層を持つ項目 (participants.col_voice_from.label と同じテキスト)
inline constexpr const char *kDockRowVoiceFromMenu = "cordmux.dock.row_voice_from.menu";
// 同上のツールチップ
inline constexpr const char *kDockRowVoiceFromTip = "cordmux.dock.row_voice_from.tip";
// この行の人の声をほかの行で使っていて、現在の値が本人の声のときの、押せない「使う声」の項目のテキスト。ツールチップは
// participants.voice_taken.text
// %1=この行の人の名前、%2=その声を使っている行の人の名前
inline constexpr const char *kDockRowVoiceFromLentMenu = "cordmux.dock.row_voice_from_lent.menu";
// 「使う声」の下の、1 つだけ選べる項目 (本人の声)
inline constexpr const char *kDockRowOwnVoiceMenu = "cordmux.dock.row_own_voice.menu";
// 「使う声」の下の押せない項目 (その声をほかの行で使っているとき)
// %1=声の候補の人の名前、%2=その声を使っている行の人の名前
inline constexpr const char *kDockRowVoiceTakenMenu = "cordmux.dock.row_voice_taken.menu";
// 「使う声」の下の押せない項目。候補の人がほかの人の声を使っていて、選ぶと連鎖になるとき
// %1=声の候補の人の名前
inline constexpr const char *kDockRowVoiceUsesOtherMenu = "cordmux.dock.row_voice_uses_other.menu";
// 行のミュートのボタンのアクセシブル名。ボタンは切り替えで、押した状態がミュート中
// %1=その行の人の名前
inline constexpr const char *kDockRowVoiceMuteA11y = "cordmux.dock.row_voice_mute.a11y";
// 声のミュートのボタンのツールチップ (ミュートしていない間と、ミュート中の間)
inline constexpr const char *kDockRowVoiceMuteOnA11y = "cordmux.dock.row_voice_mute_on.a11y";
inline constexpr const char *kDockRowVoiceMuteOffA11y = "cordmux.dock.row_voice_mute_off.a11y";
// 画面共有の音のミュートのボタンのアクセシブル名
// %1=その行の人の名前
inline constexpr const char *kDockRowScreenMuteA11y = "cordmux.dock.row_screen_mute.a11y";
// 画面共有の音のミュートのボタンのツールチップ (ミュートしていない間と、ミュート中の間)
inline constexpr const char *kDockRowScreenMuteOnA11y = "cordmux.dock.row_screen_mute_on.a11y";
inline constexpr const char *kDockRowScreenMuteOffA11y = "cordmux.dock.row_screen_mute_off.a11y";
// 画面共有の音のミュート中のツールチップの 2 行目 (共有が始まるたびにミュートから始まる)
inline constexpr const char *kDockRowScreenMuteStartTip = "cordmux.dock.row_screen_mute_start.tip";
// メインアカウントの声のミュートのボタンのツールチップの 2 行目
inline constexpr const char *kDockRowMainVoiceMuteTip = "cordmux.dock.row_main_voice_mute.tip";
// 音声タブの、声の音量のつまみのアクセシブル名
// %1=その行の人の名前
inline constexpr const char *kDockRowVoiceVolumeA11y = "cordmux.dock.row_voice_volume.a11y";
// 音声タブの、画面共有の音量のつまみのアクセシブル名
// %1=その行の人の名前
inline constexpr const char *kDockRowScreenVolumeA11y = "cordmux.dock.row_screen_volume.a11y";
// つまみの現在の値
// %1=音量 (0 から 200 の整数)
inline constexpr const char *kDockRowVolumeValueLabel = "cordmux.dock.row_volume_value.label";
// 音声タブの、つまみのツールチップ (ダブルクリックで 100% に戻す)
inline constexpr const char *kDockAudioResetTip = "cordmux.dock.audio_reset.tip";
// 音声タブの 1 人の 2 行の見出し (声の行と、画面共有の音声の行)
inline constexpr const char *kDockAudioVoiceKindLabel = "cordmux.dock.audio_voice_kind.label";
inline constexpr const char *kDockAudioScreenKindLabel = "cordmux.dock.audio_screen_kind.label";
// 音声タブの声の行の、つまみの場所の文 (まだ声が届いていない人)
inline constexpr const char *kDockAudioNoVoiceStatus = "cordmux.dock.audio_no_voice.status";
// 音声タブの画面共有の音声の行の、つまみの場所の文 (共有していない人と、共有しているが音声がない人)
inline constexpr const char *kDockAudioNotSharingStatus = "cordmux.dock.audio_not_sharing.status";
inline constexpr const char *kDockAudioNoScreenAudioStatus = "cordmux.dock.audio_no_screen_audio.status";
// 音声タブの、行がないときの文 (合成のソースがない間と、行がまだない間)
inline constexpr const char *kDockAudioNoSourceText = "cordmux.dock.audio_no_source.text";
inline constexpr const char *kDockAudioEmptyEmpty = "cordmux.dock.audio_empty.empty";
// 録画のボタン (待機)
inline constexpr const char *kDockRecordStartButton = "cordmux.dock.record_start.button";
// 録画のボタン (録画中)
inline constexpr const char *kDockRecordStopButton = "cordmux.dock.record_stop.button";
// 録画のボタン (録画中で、経過時間が分かるとき)
// %1=録画の経過時間 (12:34 か 1:02:34)
inline constexpr const char *kDockRecordStopElapsedButton = "cordmux.dock.record_stop_elapsed.button";
// 録画のボタン (開始の処理中が 1 秒を超えたとき)
inline constexpr const char *kDockRecordStartingButton = "cordmux.dock.record_starting.button";
// 録画のボタン (停止の処理中が 1 秒を超えたとき)
inline constexpr const char *kDockRecordStoppingButton = "cordmux.dock.record_stopping.button";
// 録画を始めるボタンのツールチップとアクセシブルな説明 (OBS の録画と何が違うかを、ここで伝える)。停止のボタンには
// 付けない
inline constexpr const char *kDockRecordA11y = "cordmux.dock.record.a11y";
// 録画のボタンを押せない理由 (cordmux を無効にしているとき)。1 行目が無効を伝えている間は、ツールチップと
// アクセシブルな説明だけに入れる
inline constexpr const char *kDockRecordDisabledOffText = "cordmux.dock.record_disabled_off.text";
// ドックの [...] のメニューの項目
inline constexpr const char *kDockMenuSettingsMenu = "cordmux.dock.menu_settings.menu";
// 同上
inline constexpr const char *kDockMenuParticipantsMenu = "cordmux.dock.menu_participants.menu";
// 同上
inline constexpr const char *kDockMenuExportDiagnosticsMenu = "cordmux.dock.menu_export_diagnostics.menu";
// 同上の最後の項目
inline constexpr const char *kDockMenuDisableMenu = "cordmux.dock.menu_disable.menu";
// 同上。無効にしている間
inline constexpr const char *kDockMenuEnableMenu = "cordmux.dock.menu_enable.menu";
// 同上。cordmux の録画の最中
inline constexpr const char *kDockMenuDisableRecordingMenu = "cordmux.dock.menu_disable_recording.menu";

// ---- settings: cordmux の設定ウィンドウ ----
// 設定ウィンドウのタイトル
inline constexpr const char *kSettingsWindowTitle = "cordmux.settings.window.title";
// OBS の [ツール] メニューの項目
inline constexpr const char *kSettingsToolsMenuMenu = "cordmux.settings.tools_menu.menu";
// OBS の [ツール] メニューの 2 つ目の項目。設定ウィンドウの [バージョン情報] で [cordmuxをアンインストール...] を
// 押したのと同じ。テキストは settings.uninstall.button と同じ
inline constexpr const char *kSettingsToolsMenuUninstallMenu = "cordmux.settings.tools_menu_uninstall.menu";
// ページ 1
inline constexpr const char *kSettingsRecordingPage = "cordmux.settings.recording.page";
// ページ 2
inline constexpr const char *kSettingsIncomingVideoPage = "cordmux.settings.incoming_video.page";
// ページ 3
inline constexpr const char *kSettingsAdvancedPage = "cordmux.settings.advanced.page";
// ページ 4
inline constexpr const char *kSettingsAboutPage = "cordmux.settings.about.page";
// 再起動が必要な設定を変えたときに、ページの下に表示する 1 文
inline constexpr const char *kSettingsRestartNeededText = "cordmux.settings.restart_needed.text";
// 数の入力欄の単位 (QSpinBox::setSuffix)
inline constexpr const char *kSettingsUnitMsLabel = "cordmux.settings.unit_ms.label";
// 同上
inline constexpr const char *kSettingsUnitPxLabel = "cordmux.settings.unit_px.label";
// 同上 (% の前には空白を入れない)
inline constexpr const char *kSettingsUnitPercentLabel = "cordmux.settings.unit_percent.label";
// 同上
inline constexpr const char *kSettingsUnitMinutesLabel = "cordmux.settings.unit_minutes.label";
// 表示タブの項目
inline constexpr const char *kLookSlotAssignmentLabel = "cordmux.look.slot_assignment.label";
// 同上のツールチップ
inline constexpr const char *kLookSlotAssignmentTip = "cordmux.look.slot_assignment.tip";
// 同上の選択肢
inline constexpr const char *kLookSlotAssignmentManualOption = "cordmux.look.slot_assignment_manual.option";
// 同上
inline constexpr const char *kLookSlotAssignmentAutoOption = "cordmux.look.slot_assignment_auto.option";
// 表示タブの項目。単位は unit_px
inline constexpr const char *kLookSlotSpacingLabel = "cordmux.look.slot_spacing.label";
// 表示タブの項目 (自分の枠に映す映像)。先頭の選択肢は kLookSelfFrameDiscordOption か kLookNoneOption。保存した
// ソースが OBS に見つからないときは kLookSelfSourceMissingOption の形の選択肢を足す
inline constexpr const char *kLookSelfFrameSourceLabel = "cordmux.look.self_frame_source.label";
// 同上のツールチップ
inline constexpr const char *kLookSelfFrameSourceTip = "cordmux.look.self_frame_source.tip";
// 表示タブの [自分の枠に映す映像] の次の行のチェックボックス (自分の枠に映す映像を、並び順によらず先頭 (左上) に置く)
inline constexpr const char *kLookSelfSlotFirstCheck = "cordmux.look.self_slot_first.check";
// 同上のツールチップ
inline constexpr const char *kLookSelfSlotFirstTip = "cordmux.look.self_slot_first.tip";
// ソースを選ぶ欄の、何も選んでいないときの選択肢 (自分の枠の音声ソースと、メインアカウントのユーザーID が空のときの
// 自分の枠に映す映像)
inline constexpr const char *kLookNoneOption = "cordmux.look.none.option";
// 自分の枠に映す映像の欄の先頭の選択肢 (メインアカウントのユーザーID があるとき。サブアカウントの Discord の画面共有を
// そのまま使う)
inline constexpr const char *kLookSelfFrameDiscordOption = "cordmux.look.self_frame_source_discord.option";
// 自分の枠に映す映像の欄の、OBS のシーンの選択肢 (%1=シーンの名前)
inline constexpr const char *kLookSelfFrameSceneOption = "cordmux.look.self_frame_source_scene.option";
// 表示タブのまとまりの見出し (名前)
inline constexpr const char *kLookHeadingNameHeading = "cordmux.look.heading_name.heading";
// 同上 (輪郭)
inline constexpr const char *kLookHeadingOutlineHeading = "cordmux.look.heading_outline.heading";
// 同上 (アバター)
inline constexpr const char *kLookHeadingAvatarHeading = "cordmux.look.heading_avatar.heading";
// 同上 (背景)
inline constexpr const char *kLookHeadingBackgroundHeading = "cordmux.look.heading_background.heading";
// 同上 (名前ラベル全体)
inline constexpr const char *kLookHeadingWholeHeading = "cordmux.look.heading_whole.heading";
// 同上 (話している人の強調)
inline constexpr const char *kLookHeadingSpeakingHeading = "cordmux.look.heading_speaking.heading";
// 同上 (画面共有している参加者)
inline constexpr const char *kLookHeadingSharingHeading = "cordmux.look.heading_sharing.heading";
// 同上 (画面共有していない参加者)
inline constexpr const char *kLookHeadingNotSharingHeading = "cordmux.look.heading_not_sharing.heading";
// 名前、アバター、背景、画面共有していない参加者のまとまりの [表示] のチェックボックス
inline constexpr const char *kLookShowCheck = "cordmux.look.show.check";
// 名前のまとまりの項目
inline constexpr const char *kLookFontLabel = "cordmux.look.font.label";
// 名前のまとまりの太さ (フォントが持つ太さの選択肢)
inline constexpr const char *kLookFontWeightLabel = "cordmux.look.font_weight.label";
// フォントに標準 (400) の太さがないときに、選択肢へ足す標準
inline constexpr const char *kLookFontWeightNormalOption = "cordmux.look.font_weight_normal.option";
// フォントに太字 (700) の太さがないときに、選択肢へ足す太字
inline constexpr const char *kLookFontWeightBoldOption = "cordmux.look.font_weight_bold.option";
// 名前のまとまりの項目。単位は unit_percent
inline constexpr const char *kLookFontSizeLabel = "cordmux.look.font_size.label";
// 同上のツールチップ
// %1=キャンバスの幅、%2=キャンバスの高さ、%3=現在の割合でのピクセル数
inline constexpr const char *kLookFontSizeTip = "cordmux.look.font_size.tip";
// 名前、輪郭、背景、話している人の強調のまとまりの色の欄
inline constexpr const char *kLookColorLabel = "cordmux.look.color.label";
// 輪郭とアバターのまとまりのサイズの欄。単位は unit_times
inline constexpr const char *kLookSizeLabel = "cordmux.look.size.label";
// 背景と名前ラベル全体のまとまりの不透明度の欄。単位は unit_percent
inline constexpr const char *kLookOpacityLabel = "cordmux.look.opacity.label";
// 名前ラベル全体のまとまりの余白の欄。単位は unit_percent
inline constexpr const char *kLookMarginLabel = "cordmux.look.margin.label";
// 話している人の強調のまとまりの [光] のチェックボックス
inline constexpr const char *kLookGlowCheck = "cordmux.look.glow.check";
// 同上の、光らせる対象の行 (3 つのチェックボックスの行頭)
inline constexpr const char *kLookGlowTargetsLabel = "cordmux.look.glow_targets.label";
// 光らせる対象の枠
inline constexpr const char *kLookGlowTargetFrameCheck = "cordmux.look.glow_target_frame.check";
// 光らせる対象のアバター
inline constexpr const char *kLookGlowTargetAvatarCheck = "cordmux.look.glow_target_avatar.check";
// 光らせる対象の名前
inline constexpr const char *kLookGlowTargetNameCheck = "cordmux.look.glow_target_name.check";
// 話している人の強調のまとまりの光の太さ。単位は unit_times
inline constexpr const char *kLookGlowThicknessLabel = "cordmux.look.glow_thickness.label";
// 同上のぼかし。単位は unit_times
inline constexpr const char *kLookGlowBlurLabel = "cordmux.look.glow_blur.label";
// 同上の、話していない人の暗さ。単位は unit_percent
inline constexpr const char *kLookDimLabel = "cordmux.look.dim.label";
// 同上のフェードインの時間。単位は unit_ms
inline constexpr const char *kLookFadeInLabel = "cordmux.look.fade_in.label";
// 同上の保持時間。単位は unit_ms
inline constexpr const char *kLookHoldLabel = "cordmux.look.hold.label";
// 同上のフェードアウトの時間。単位は unit_ms
inline constexpr const char *kLookFadeOutLabel = "cordmux.look.fade_out.label";
// 画面共有している参加者のまとまりの、名前ラベルの隅の項目
inline constexpr const char *kLookNameLabelPositionLabel = "cordmux.look.name_label_position.label";
// 画面共有している参加者と画面共有していない参加者のまとまりの、アバターの位置の項目
inline constexpr const char *kLookAvatarPositionLabel = "cordmux.look.avatar_position.label";
// アバターの位置の選択肢 (左)
inline constexpr const char *kLookSideLeftOption = "cordmux.look.side_left.option";
// 同上 (右)
inline constexpr const char *kLookSideRightOption = "cordmux.look.side_right.option";
// アバターのまとまりの、アバターの形の項目
inline constexpr const char *kLookAvatarShapeLabel = "cordmux.look.avatar_shape.label";
// アバターの形の選択肢 (丸)
inline constexpr const char *kLookShapeCircleOption = "cordmux.look.shape_circle.option";
// 同上 (角丸)
inline constexpr const char *kLookShapeRoundedOption = "cordmux.look.shape_rounded.option";
// 同上 (四角)
inline constexpr const char *kLookShapeSquareOption = "cordmux.look.shape_square.option";
// 画面共有している参加者のまとまりのチェックボックス
inline constexpr const char *kLookHideWhenSingleCheck = "cordmux.look.hide_when_single.check";
// 「画面共有している参加者」の [表示] のツールチップ ([表示] の文字列は kLookShowCheck)
inline constexpr const char *kLookSlotLabelShowTip = "cordmux.look.slot_label_show.tip";
// 同上のツールチップ (名前ラベルを出さない人を、画面共有していない参加者の並びに出す)
inline constexpr const char *kLookHideWhenSingleTip = "cordmux.look.hide_when_single.tip";
// 画面共有していない参加者のまとまりの、並びを寄せる位置の項目
inline constexpr const char *kLookAnchorLabel = "cordmux.look.anchor.label";
// 画面共有していない参加者のまとまりの、並びの位置をずらす量の項目。横 (offset_x) と縦 (offset_y) の 2 つの欄を 1 行に置く。
// 単位は settings の unit_px
inline constexpr const char *kLookOffsetLabel = "cordmux.look.offset.label";
// 同上の、横にずらす量の欄の前に置く文字
inline constexpr const char *kLookOffsetXLabel = "cordmux.look.offset_x.label";
// 同上の、縦にずらす量の欄の前に置く文字
inline constexpr const char *kLookOffsetYLabel = "cordmux.look.offset_y.label";
// 同上の 2 つの欄のツールチップ (ずらす基準と単位、正の向き)
inline constexpr const char *kLookRosterOffsetTip = "cordmux.look.roster_offset.tip";
// 位置の選択肢 (名前ラベルの隅と、並びを寄せる位置で共通)
inline constexpr const char *kLookPositionTopLeftOption = "cordmux.look.position_top_left.option";
// 同上
inline constexpr const char *kLookPositionTopCenterOption = "cordmux.look.position_top_center.option";
// 同上
inline constexpr const char *kLookPositionTopRightOption = "cordmux.look.position_top_right.option";
// 同上
inline constexpr const char *kLookPositionMiddleLeftOption = "cordmux.look.position_middle_left.option";
// 同上
inline constexpr const char *kLookPositionMiddleRightOption = "cordmux.look.position_middle_right.option";
// 同上
inline constexpr const char *kLookPositionBottomLeftOption = "cordmux.look.position_bottom_left.option";
// 同上
inline constexpr const char *kLookPositionBottomCenterOption = "cordmux.look.position_bottom_center.option";
// 同上
inline constexpr const char *kLookPositionBottomRightOption = "cordmux.look.position_bottom_right.option";
// 画面共有していない参加者のまとまりの、並べる向きの項目
inline constexpr const char *kLookDirectionLabel = "cordmux.look.direction.label";
// 並べる向きの選択肢 (横並び)
inline constexpr const char *kLookDirectionRowOption = "cordmux.look.direction_row.option";
// 同上 (縦並び)
inline constexpr const char *kLookDirectionColumnOption = "cordmux.look.direction_column.option";
// 画面共有していない参加者のまとまりの、並びの間隔の項目。単位は unit_times
inline constexpr const char *kLookGapLabel = "cordmux.look.gap.label";
// 画面共有していない参加者のまとまりの、名前の幅の上限の項目。単位は unit_times
inline constexpr const char *kLookNameMaxWidthLabel = "cordmux.look.name_max_width.label";
// 同上のツールチップ (倍率の基準と、長い名前の扱い)
inline constexpr const char *kLookNameMaxWidthTip = "cordmux.look.name_max_width.tip";
// 画面共有していない参加者のまとまりのチェックボックス
inline constexpr const char *kLookShowBotsCheck = "cordmux.look.show_bots.check";
// 倍率の欄の単位 (数の後ろに付く)
inline constexpr const char *kLookUnitTimesLabel = "cordmux.look.unit_times.label";
// 倍率の欄のツールチップ (フォントサイズに対する倍率)
inline constexpr const char *kLookRatioOfFontSizeTip = "cordmux.look.ratio_of_font_size.tip";
// 余白の欄のツールチップ
inline constexpr const char *kLookRatioOfCanvasHeightTip = "cordmux.look.ratio_of_canvas_height.tip";
// 話していない人の暗さの欄のツールチップ
inline constexpr const char *kLookDimTip = "cordmux.look.dim.tip";
// 話している人の強調のまとまりの、自分の枠の音声ソースの項目。先頭の選択肢は kLookNoneOption
inline constexpr const char *kLookSelfSourceLabel = "cordmux.look.self_source.label";
// 同上のツールチップ
inline constexpr const char *kLookSelfSourceTip = "cordmux.look.self_source.tip";
// 同上の選択肢のうち、保存したソースが OBS に見つからないときにその名前を表示する形。%1 はソースの名前
inline constexpr const char *kLookSelfSourceMissingOption = "cordmux.look.self_source_missing.option";
// 話している人の強調のまとまりの、音声ソースで話し始めを決めるしきい値の項目。単位は unit_db
inline constexpr const char *kLookSelfThresholdLabel = "cordmux.look.self_threshold.label";
// 同上のツールチップ
inline constexpr const char *kLookSelfThresholdTip = "cordmux.look.self_threshold.tip";
// 音量の欄の単位 (数の後ろに付く)
inline constexpr const char *kLookUnitDbLabel = "cordmux.look.unit_db.label";
// 保持時間の欄のツールチップ
inline constexpr const char *kLookHoldTip = "cordmux.look.hold.tip";
// 名前ラベルの隅の選択のツールチップ
inline constexpr const char *kLookNameLabelPositionTip = "cordmux.look.name_label_position.tip";
// 画面共有している参加者のアバターの位置のツールチップ
inline constexpr const char *kLookAvatarSideTip = "cordmux.look.avatar_side.tip";
// 画面共有していない参加者のアバターの位置のツールチップ
inline constexpr const char *kLookRosterAvatarSideTip = "cordmux.look.roster_avatar_side.tip";
// 同じ名前の入力欄 (表示、色、サイズなど) のアクセシブル名
// %1=入力欄の名前、%2=入力欄があるまとまりの見出し
inline constexpr const char *kLookFieldInGroupA11y = "cordmux.look.field_in_group.a11y";
// 上の %2 に入れる、「枠」の「背景」のまとまりの名前。見出しは名前ラベルの「背景」と同じなので、アクセシブル名と色のダイアログの
// タイトルでは、この名前で聞き分ける (「表示 (枠の背景)」)
inline constexpr const char *kLookSlotBackgroundGroupA11y = "cordmux.look.slot_background_group.a11y";
// 色の入力欄のボタン (OBS の Basic.PropertiesWindow.SelectColor と同じテキスト。英語はボタンなので Title Case)
inline constexpr const char *kLookSelectColorButton = "cordmux.look.select_color.button";
// 表示タブの [色を選択] のアクセシブル名 (同じ名前のボタンを聞き分けられるようにする)
// %1=色の欄があるまとまりの見出し (名前、輪郭、背景、話している人の強調)
inline constexpr const char *kLookSelectColorForA11y = "cordmux.look.select_color_for.a11y";
// [録画] のページの項目
inline constexpr const char *kSettingsSaveFolderLabel = "cordmux.settings.save_folder.label";
// 保存先の入力欄のボタン
inline constexpr const char *kSettingsBrowseButton = "cordmux.settings.browse.button";
// 保存先の入力欄と、診断情報のエクスポートの結果の行に添えるボタン
inline constexpr const char *kSettingsOpenFolderButton = "cordmux.settings.open_folder.button";
// 保存先の入力欄の下の補足。録画ごとにこの場所の中へ開始日時の名前のフォルダーを作ること、選んでいないときの既定の場所、
// 入力欄を空にすると既定に戻ること、保存先の値が次に録画を始めたときから反映されること
// (dock_poll_settings.cpp の録画の値の扱い)
inline constexpr const char *kSettingsSaveFolderText = "cordmux.settings.save_folder.text";
// [録画] のページの項目。単位は unit_minutes
inline constexpr const char *kSettingsRemainingTimeWarningLabel = "cordmux.settings.remaining_time_warning.label";
// 同上のツールチップ
inline constexpr const char *kSettingsRemainingTimeWarningTip = "cordmux.settings.remaining_time_warning.tip";
// [録画] のページの項目 (配信の開始と終了に合わせて録画を始め、終える)。文言は OBS の Basic.Settings.General.RecordWhenStreaming に合わせる
inline constexpr const char *kSettingsRecordWithStreamCheck = "cordmux.settings.record_with_stream.check";
// 同上のツールチップ
inline constexpr const char *kSettingsRecordWithStreamTip = "cordmux.settings.record_with_stream.tip";
// [受け取る映像] のページの項目
inline constexpr const char *kSettingsMaxResolutionLabel = "cordmux.settings.max_resolution.label";
// 同上の選択肢
inline constexpr const char *kSettingsMaxResolutionNoneOption = "cordmux.settings.max_resolution_none.option";
// 同上の選択肢
// %1=長い辺のピクセル数 (桁区切りなし)
inline constexpr const char *kSettingsMaxResolutionPxOption = "cordmux.settings.max_resolution_px.option";
// [受け取る映像] のページの項目
inline constexpr const char *kSettingsMaxFrameRateLabel = "cordmux.settings.max_frame_rate.label";
// 同上の選択肢
inline constexpr const char *kSettingsMaxFrameRateOriginalOption = "cordmux.settings.max_frame_rate_original.option";
// 同上の選択肢
inline constexpr const char *kSettingsMaxFrameRateHalfOption = "cordmux.settings.max_frame_rate_half.option";
// [受け取る映像] のページの下の補足 (設定がいつ反映されるか)
inline constexpr const char *kSettingsLimitsApplyText = "cordmux.settings.limits_apply.text";
// [映像の受信] のページの項目 (受信する人)
inline constexpr const char *kSettingsReceiveScopeLabel = "cordmux.settings.receive_scope.label";
// 同上の選択肢 (画面共有している全員。既定ではない)
inline constexpr const char *kSettingsReceiveScopeEveryoneOption = "cordmux.settings.receive_scope_everyone.option";
// 同上の選択肢 (枠に載せた人だけ。既定。録画の対象も枠に載せた人だけになる)
inline constexpr const char *kSettingsReceiveScopeSlotsOption = "cordmux.settings.receive_scope_slots.option";
// 同上のツールチップ
inline constexpr const char *kSettingsReceiveScopeTip = "cordmux.settings.receive_scope.tip";
// [受け取る映像] のページの項目 (送信側のエンコーダの好み)。説明に、動作するタイミングを書く
inline constexpr const char *kSettingsVideoEncoderLabel = "cordmux.settings.video_encoder.label";
// 同上のツールチップ
inline constexpr const char *kSettingsVideoEncoderTip = "cordmux.settings.video_encoder.tip";
// 同上の選択肢
inline constexpr const char *kSettingsVideoEncoderHardwareOption = "cordmux.settings.video_encoder_hardware.option";
// 同上の選択肢
inline constexpr const char *kSettingsVideoEncoderSoftwareOption = "cordmux.settings.video_encoder_software.option";
// [受け取る映像] のページの項目 (送信側のエンコードで、遅延と画質のどちらを優先するか)
inline constexpr const char *kSettingsEncodePriorityLabel = "cordmux.settings.encode_priority.label";
// 同上のツールチップ
inline constexpr const char *kSettingsEncodePriorityTip = "cordmux.settings.encode_priority.tip";
// 同上の選択肢
inline constexpr const char *kSettingsEncodePriorityLatencyOption = "cordmux.settings.encode_priority_latency.option";
// 同上の選択肢
inline constexpr const char *kSettingsEncodePriorityQualityOption = "cordmux.settings.encode_priority_quality.option";
// 状態タブの [映像のバッファ] の欄 (設定ウィンドウにはない)。単位は unit_ms
inline constexpr const char *kStatusVideoBufferLabel = "cordmux.status.video_buffer.label";
// 同上のツールチップ
inline constexpr const char *kStatusVideoBufferTip = "cordmux.status.video_buffer.tip";
// [詳細設定] のページのまとまり (映像と音声)
inline constexpr const char *kSettingsHeadingVideoAudioHeading = "cordmux.settings.heading_video_audio.heading";
// [詳細設定] のページのまとまり (ドック)
inline constexpr const char *kSettingsHeadingDockHeading = "cordmux.settings.heading_dock.heading";
// [映像の受信] のページのまとまり (上限と受信する人)
inline constexpr const char *kSettingsHeadingReceivedVideoHeading = "cordmux.settings.heading_received_video.heading";
// [映像の受信] のページのまとまり (Vesktop 側のエンコードの設定)
inline constexpr const char *kSettingsHeadingVesktopEncodingHeading = "cordmux.settings.heading_vesktop_encoding.heading";
// 「映像と音声」のまとまりの項目
inline constexpr const char *kSettingsVideoDecodingLabel = "cordmux.settings.video_decoding.label";
// 同上の選択肢
inline constexpr const char *kSettingsVideoDecodingSoftwareOption = "cordmux.settings.video_decoding_software.option";
// 同上の選択肢
inline constexpr const char *kSettingsVideoDecodingHardwareOption = "cordmux.settings.video_decoding_hardware.option";
// 「映像と音声」のまとまりの項目。単位は unit_ms
inline constexpr const char *kSettingsScreenAudioOffsetLabel = "cordmux.settings.screen_audio_offset.label";
// 同上のツールチップ
inline constexpr const char *kSettingsScreenAudioOffsetTip = "cordmux.settings.screen_audio_offset.tip";
// [録画] のページの項目
inline constexpr const char *kSettingsAudioFormatLabel = "cordmux.settings.audio_format.label";
// 同上の選択肢
inline constexpr const char *kSettingsAudioFormatFloat32Option = "cordmux.settings.audio_format_float32.option";
// 同上の選択肢
inline constexpr const char *kSettingsAudioFormatInt24Option = "cordmux.settings.audio_format_int24.option";
// 同上のツールチップ
inline constexpr const char *kSettingsAudioFormatTip = "cordmux.settings.audio_format.tip";
// 「ダミー」のまとまりの項目
inline constexpr const char *kLookShowPlaceholdersCheck = "cordmux.look.show_placeholders.check";
// 「ダミー」のまとまりで、「ダミーを表示する」の下に常に表示する補足。オンにしている間は、相手の画面共有も声も
// 配信に出ないことを、押す前に伝える (sources/dummy_feed.h。配信中かどうかを確認するガードはない)。語は
// summary_placeholders.status と同じ
inline constexpr const char *kLookShowPlaceholdersDescText = "cordmux.look.show_placeholders_desc.text";
// 「ドック」のまとまりの項目 (配信中の退出を確認)
inline constexpr const char *kSettingsConfirmWhileStreamingCheck = "cordmux.settings.confirm_while_streaming.check";
// 同上のツールチップ
inline constexpr const char *kSettingsConfirmWhileStreamingTip = "cordmux.settings.confirm_while_streaming.tip";
// [バージョン情報] のページの、まとまりの見出し
inline constexpr const char *kSettingsHeadingVersionsHeading = "cordmux.settings.heading_versions.heading";
// バージョンの一覧のラベル
inline constexpr const char *kSettingsVersionObsPluginLabel = "cordmux.settings.version_obs_plugin.label";
// 同上
inline constexpr const char *kSettingsVersionVesktopPluginLabel = "cordmux.settings.version_vesktop_plugin.label";
// 同上 (製品の名前)
inline constexpr const char *kSettingsVersionVesktopLabel = "cordmux.settings.version_vesktop.label";
// 同上
inline constexpr const char *kSettingsVersionBundledVencordLabel = "cordmux.settings.version_bundled_vencord.label";
// 同上
inline constexpr const char *kSettingsVersionObsLabel = "cordmux.settings.version_obs.label";
// 同上
inline constexpr const char *kSettingsVersionWindowsLabel = "cordmux.settings.version_windows.label";
// 同上
inline constexpr const char *kSettingsVersionGpuLabel = "cordmux.settings.version_gpu.label";
// バージョンの一覧をコピーするボタン
inline constexpr const char *kSettingsCopyButton = "cordmux.settings.copy.button";
// セルの「-」のツールチップとアクセシブル名
inline constexpr const char *kSettingsUnavailableA11y = "cordmux.settings.unavailable.a11y";
// 「名前: 値」の 1 行。ラベルと値を 2 つのウィジェットにすると、列がないので英語では 1 つの
// 句に読めるため、1 つのテキストにする
// %1=起動時に書き直したかの値 (startup_rewritten か startup_not_rewritten のテキスト)
inline constexpr const char *kSettingsStartupVesktopSettingsStatus = "cordmux.settings.startup_vesktop_settings.status";
// 同上の値
inline constexpr const char *kSettingsStartupRewrittenStatus = "cordmux.settings.startup_rewritten.status";
// 同上の値
inline constexpr const char *kSettingsStartupNotRewrittenStatus = "cordmux.settings.startup_not_rewritten.status";
// まとまりの見出し (診断情報)
inline constexpr const char *kSettingsHeadingDiagnosticsHeading = "cordmux.settings.heading_diagnostics.heading";
// 「診断情報」のまとまりのボタン
inline constexpr const char *kSettingsExportDiagnosticsButton = "cordmux.settings.export_diagnostics.button";
// 同上を押してから終わるまでのボタンのテキスト
inline constexpr const char *kSettingsExportDiagnosticsBusyButton = "cordmux.settings.export_diagnostics_busy.button";
// 終わったときにボタンの下に表示する 1 行。場所の行と open_folder を添える
inline constexpr const char *kSettingsExportDoneStatus = "cordmux.settings.export_done.status";
// まとまりの見出し (アンインストール)
inline constexpr const char *kSettingsHeadingUninstallHeading = "cordmux.settings.heading_uninstall.heading";
// [バージョン情報] のページのいちばん下のボタン
inline constexpr const char *kSettingsUninstallButton = "cordmux.settings.uninstall.button";
// 同上を押してから終わるまでのボタンのテキスト
inline constexpr const char *kSettingsUninstallBusyButton = "cordmux.settings.uninstall_busy.button";
// アンインストールを押せない理由 (配信中)
inline constexpr const char *kSettingsUninstallStreamingText = "cordmux.settings.uninstall_streaming.text";
// 同上 (OBS で録画中)
inline constexpr const char *kSettingsUninstallObsRecordingText = "cordmux.settings.uninstall_obs_recording.text";
// 同上 (OBS のリプレイバッファが動いている。obs_frontend_replay_buffer_active)。アンインストールは
// シーンコレクションを切り替えるので、配信と同じく押せなくする。語は OBS の表示に合わせる (日本語
// 「リプレイバッファ」[リプレイバッファ停止]、英語 "replay buffer" [Stop Replay Buffer]。OBS 31.1.1 の
// frontend/data/locale の ReplayBuffer と Basic.Main.StopReplayBuffer)
inline constexpr const char *kSettingsUninstallReplayBufferText = "cordmux.settings.uninstall_replay_buffer.text";
// 同上 (OBS の仮想カメラが動いている。obs_frontend_virtualcam_active)。語は OBS の表示に合わせる (日本語「仮想カメラ」
// [仮想カメラ停止]、英語 "virtual camera" [Stop Virtual Camera]。同じ locale の Basic.VCam.VirtualCamera と
// Basic.Main.StopVirtualCam)
inline constexpr const char *kSettingsUninstallVirtualCameraText = "cordmux.settings.uninstall_virtual_camera.text";
// 同上 (cordmux で録画中)
inline constexpr const char *kSettingsUninstallPerPersonRecordingText =
	"cordmux.settings.uninstall_per_person_recording.text";
// 同上 (Vesktop が動いている)
inline constexpr const char *kSettingsUninstallVesktopRunningText = "cordmux.settings.uninstall_vesktop_running.text";
// 同上。起動した直後の問い合わせの間だけ
inline constexpr const char *kSettingsUninstallCheckingText = "cordmux.settings.uninstall_checking.text";
// アンインストールを始めた後に押せない操作の、すべてに共通の理由。押せるようになる条件はない
inline constexpr const char *kSettingsUninstallStartedText = "cordmux.settings.uninstall_started.text";
// アンインストールが終わった後に [cordmuxをアンインストール...] を押せない理由 (進行中と終わった後で文を分ける)
inline constexpr const char *kSettingsUninstallDoneButtonText = "cordmux.settings.uninstall_done_button.text";

// ---- participants: 参加者の設定ウィンドウ ----
// ウィンドウのタイトル
inline constexpr const char *kParticipantsWindowTitle = "cordmux.participants.window.title";
// 見出し (現在の参加者)
inline constexpr const char *kParticipantsCurrentHeading = "cordmux.participants.current.heading";
// 見出し (過去の参加者)
inline constexpr const char *kParticipantsPastHeading = "cordmux.participants.past.heading";
// 列の見出し
inline constexpr const char *kParticipantsColNameLabel = "cordmux.participants.col_name.label";
// 列の見出し
inline constexpr const char *kParticipantsColAutoAddLabel = "cordmux.participants.col_auto_add.label";
// 同上のツールチップ
inline constexpr const char *kParticipantsColAutoAddTip = "cordmux.participants.col_auto_add.tip";
// 同上を押せない理由
inline constexpr const char *kParticipantsAutoAddDisabledTip = "cordmux.participants.auto_add_disabled.tip";
// 列の見出し
inline constexpr const char *kParticipantsColNameInCordmuxLabel = "cordmux.participants.col_name_in_cordmux.label";
// 同上のツールチップと、名前を変更するダイアログの補足。空の入力欄のプレースホルダーは Discord での名前
inline constexpr const char *kParticipantsNameInCordmuxTip = "cordmux.participants.name_in_cordmux.tip";
// 列の見出し
inline constexpr const char *kParticipantsColVoiceFromLabel = "cordmux.participants.col_voice_from.label";
// 同上のツールチップ
inline constexpr const char *kParticipantsColVoiceFromTip = "cordmux.participants.col_voice_from.tip";
// 列の見出し (チェックを付けた人を、画面共有していない参加者の並びに出さない)
inline constexpr const char *kParticipantsColRosterHideLabel = "cordmux.participants.col_roster_hide.label";
// 同上のツールチップ
inline constexpr const char *kParticipantsColRosterHideTip = "cordmux.participants.col_roster_hide.tip";
// 同上を押せない理由 (設定の「画面共有していない参加者」の [表示] がオフ)
inline constexpr const char *kParticipantsShowOffTip = "cordmux.participants.show_off.tip";
// 同上を押せない理由 (設定の [Botを表示] がオフのときの Bot の行)
inline constexpr const char *kParticipantsShowBotsOffTip = "cordmux.participants.show_bots_off.tip";
// 使う声の列の値 (メニューの row_own_voice とは、英語の大文字の使い方が違う)
inline constexpr const char *kParticipantsOwnVoiceStatus = "cordmux.participants.own_voice.status";
// 使う声で選べない人の理由
// %1=声の候補の人の名前、%2=その声を使っている行の人の名前
inline constexpr const char *kParticipantsVoiceTakenText = "cordmux.participants.voice_taken.text";
// 使う声で選べない人の理由 (候補の人がほかの人の声を使っている)。行のメニューのツールチップと、
// 参加者の設定ウィンドウで使う
// %1=声の候補の人の名前
inline constexpr const char *kParticipantsVoiceUsesOtherText = "cordmux.participants.voice_uses_other.text";
// 過去の参加者の列の見出し (現在の参加者は、この通話に参加した日時を表示する)。値の形は QLocale で決める
inline constexpr const char *kParticipantsColLastSeenLabel = "cordmux.participants.col_last_seen.label";
// 並べ替えの列の見出しのアクセシブルな説明 (並べていない列は、ツールチップの列の説明が届く)
inline constexpr const char *kParticipantsSortedAscendingA11y = "cordmux.participants.sorted_ascending.a11y";
inline constexpr const char *kParticipantsSortedDescendingA11y = "cordmux.participants.sorted_descending.a11y";
// 過去の参加者が 0 人のとき
inline constexpr const char *kParticipantsEmptyPastEmpty = "cordmux.participants.empty_past.empty";
// 「現在の参加者」が 0 行のとき。理由は書かない (Vesktop と接続していない間も、同じテキストで正しいため)
inline constexpr const char *kParticipantsEmptyCurrentEmpty = "cordmux.participants.empty_current.empty";

// ---- dialog: 確認、結果、エラーのダイアログ ----
// アンインストールの確認と結果のダイアログのタイトル
inline constexpr const char *kDialogUninstallTitle = "cordmux.dialog.uninstall.title";
// 確認のダイアログの主文
inline constexpr const char *kDialogUninstallConfirmHeadline = "cordmux.dialog.uninstall_confirm.headline";
// 補足の箇条書きの前置き
inline constexpr const char *kDialogUninstallDeletesText = "cordmux.dialog.uninstall_deletes.text";
// 箇条書きの項目 (settings.window.title とは英語が違う)。削除するのは plugin_config の下の cordmux の全部で、参加者の
// 設定 (seen_people.json) も含む。cordmux の中では 2 つは別のウィンドウの名前なので、両方を書く
// (uninstall_done.text も同じ)
inline constexpr const char *kDialogUninstallDeleteSettingsItemText =
	"cordmux.dialog.uninstall_delete_settings_item.text";
// 箇条書きの項目
inline constexpr const char *kDialogUninstallDeleteScenesItemText = "cordmux.dialog.uninstall_delete_scenes_item.text";
// 補足
inline constexpr const char *kDialogUninstallRestoresText = "cordmux.dialog.uninstall_restores.text";
// 補足
inline constexpr const char *kDialogUninstallKeepsText = "cordmux.dialog.uninstall_keeps.text";
// 補足 (この後に必要な作業)
inline constexpr const char *kDialogUninstallFolderAfterText = "cordmux.dialog.uninstall_folder_after.text";
// 同上の手順書と違う置き方の形 (削除するフォルダーが 1 つに決まらないとき)。完了したダイアログが、ファイルの場所
// (uninstall_files_location) と名前 (uninstall_files) とデータのフォルダー (uninstall_folder) を表示する形と組で使う
// (確認のダイアログの説明と、完了したダイアログの表示をそろえる)
inline constexpr const char *kDialogUninstallFilesAfterText = "cordmux.dialog.uninstall_files_after.text";
// 確認のダイアログの実行のボタン (確認のダイアログの中のボタンなので「...」を付けない)。もう一方は OBS の Cancel
inline constexpr const char *kDialogUninstallButton = "cordmux.dialog.uninstall.button";
// 完了したときの主文
inline constexpr const char *kDialogUninstallDoneHeadline = "cordmux.dialog.uninstall_done.headline";
// 補足
inline constexpr const char *kDialogUninstallDoneText = "cordmux.dialog.uninstall_done.text";
// 場所の行の前置き
inline constexpr const char *kDialogUninstallFolderText = "cordmux.dialog.uninstall_folder.text";
// 手順書と違う置き方の、完了したダイアログの場所の行の前置き。削除するファイルが並ぶフォルダーの場所
inline constexpr const char *kDialogUninstallFilesLocationText = "cordmux.dialog.uninstall_files_location.text";
// 同上の、削除するファイルの名前の箇条書きの前置き。名前は同梱のファイルの一覧のとおりで、言語で変えない (別の
// プラグインの DLL と同じフォルダーに並ぶことがあり、名前から cordmux のものと分からないので、1 つずつ表示する)
inline constexpr const char *kDialogUninstallFilesText = "cordmux.dialog.uninstall_files.text";
// 補足
inline constexpr const char *kDialogUninstallRestartWarningText = "cordmux.dialog.uninstall_restart_warning.text";
// 同上の、手順書と違う置き方の完了したダイアログで、削除するファイルの箇条書きの後に置く形。英語は削除するものが
// 複数 (ファイルとフォルダー) なので them にする。日本語は数を言わないので、上と同じテキスト
inline constexpr const char *kDialogUninstallRestartWarningFilesText = "cordmux.dialog.uninstall_restart_warning_files.text";
// 録画の保存先の場所の行の前置き。場所の行が 2 つ以上あっても、同じ前置きの下に並べる
inline constexpr const char *kDialogRecordingsHereText = "cordmux.dialog.recordings_here.text";
// 結果のダイアログのボタン。もう一方は OBS の OK
inline constexpr const char *kDialogOpenFolderButton = "cordmux.dialog.open_folder.button";
// ファイルの場所の行の右のボタン。エクスプローラーでそのファイルを選んだ状態で開く (テキストは Windows の shell32 の
// 「ファイルの場所を開く」。英語はボタンなので Title Case)
inline constexpr const char *kDialogOpenFileLocationButton = "cordmux.dialog.open_file_location.button";
// 完了したダイアログの「削除するフォルダー:」の行の右のボタン。親のフォルダーを開いて、cordmux のフォルダーを選んだ
// 状態にする (テキストは Windows の shell32 の「フォルダーの場所を開く」。英語はボタンの Title Case で、
// "Open File Location" と同じ形)
inline constexpr const char *kDialogOpenFolderLocationButton = "cordmux.dialog.open_folder_location.button";
// 手順書と違う置き方 (DLL のフォルダーが plugins\<名前> でない) で、削除するフォルダーが 1 つに決まらないときの主文
inline constexpr const char *kDialogUninstallDoneLooseHeadline = "cordmux.dialog.uninstall_done_loose.headline";
// 補足。zip の cordmux フォルダーにあるものと同じ名前のファイルとフォルダーを削除する、という文
inline constexpr const char *kDialogUninstallDoneLooseText = "cordmux.dialog.uninstall_done_loose.text";
// 補足の最後。続けて名前の箇条書きを置く
// 名前の箇条書きが 1 つのときは _one (英語は "This scene collection")
inline constexpr CountedId kDialogUninstallUncheckedText = {
	"cordmux.dialog.uninstall_unchecked.text",
	"cordmux.dialog.uninstall_unchecked.text_one",
};
// 結果のダイアログで、確認できなかったシーンコレクションの名前の箇条書きの後ろに置く次の操作
// 確認できなかったシーンコレクションが 1 つのときは _one (日本語も「この」と「これらの」で分かれる)
inline constexpr CountedId kDialogUninstallUncheckedNextText = {
	"cordmux.dialog.uninstall_unchecked_next.text",
	"cordmux.dialog.uninstall_unchecked_next.text_one",
};
// 途中で止まったときの主文
inline constexpr const char *kDialogUninstallStoppedHeadline = "cordmux.dialog.uninstall_stopped.headline";
// 箇条書きの前置き (3 つのまとまりの 1 つ目)
inline constexpr const char *kDialogNotDeletedText = "cordmux.dialog.not_deleted.text";
// 箇条書きの項目。場所は次の場所の行に表示する
inline constexpr const char *kDialogLeftSettingsFilesItemText = "cordmux.dialog.left_settings_files_item.text";
// 箇条書きの項目
// %1=シーンコレクションの名前
inline constexpr const char *kDialogLeftSourcesItemText = "cordmux.dialog.left_sources_item.text";
// 途中で止まったダイアログで、「削除できなかったもの:」の箇条書き (と場所の行) の直後に置く次の操作。そのまとまりが
// ないダイアログでは表示しない
inline constexpr const char *kDialogNotDeletedNextText = "cordmux.dialog.not_deleted_next.text";
// not_deleted_next.text の代わり。「削除できなかったもの:」に cordmux の設定のフォルダー (plugin_config の下の
// cordmux) が残っているときだけ使う。そのフォルダーには Vesktop を元に戻すのに必要なバックアップがあり、手動で削除
// すると、次のアンインストールで戻せなくなる。ダイアログとドックのカードの両方で使う。「元に戻せなかった設定」の
// まとまりがあるかどうかは条件にしない (そのまとまりと、設定のフォルダーが残ることは、同じアンインストールでは
// 起きない)
inline constexpr const char *kDialogNotDeletedUninstallAgainNextText =
	"cordmux.dialog.not_deleted_uninstall_again_next.text";
// 箇条書きの前置き (3 つのまとまりの 2 つ目)
inline constexpr const char *kDialogNotRestoredText = "cordmux.dialog.not_restored.text";
// 箇条書きの項目。場所は次の場所の行に表示する
inline constexpr const char *kDialogLeftVesktopSettingsItemText = "cordmux.dialog.left_vesktop_settings_item.text";
// 途中で止まったダイアログで、「元に戻せなかった設定:」の箇条書きと場所の行の直後、dont_delete_folder の前に置く
// 次の操作。手動で元に戻す方法は示さない
inline constexpr const char *kDialogNotRestoredNextText = "cordmux.dialog.not_restored_next.text";
// 「元に戻せなかった設定:」のまとまりの注意。直後にプラグインのフォルダーの場所の行を 1 つだけ置くときに使う
// (「次のフォルダー」がその行を指す。設定のフォルダーの行も続くときは dont_delete_folders)
inline constexpr const char *kDialogDontDeleteFolderText = "cordmux.dialog.dont_delete_folder.text";
// 同じ注意の、場所の行を置けないとき (手順書と違う置き方で、削除するフォルダーが 1 つに決まらないとき) の形。指す
// もののない「次の」は書かない。語は、完了したダイアログの手順書と違う置き方の主文 (uninstall_done_loose) の
// 「cordmuxのファイル」とそろえる
inline constexpr const char *kDialogDontDeleteFilesText = "cordmux.dialog.dont_delete_files.text";
// 同じ注意の、場所の行が 2 つ以上のとき (プラグインのフォルダーと cordmux の設定のフォルダー) の形。2 文目は両方に
// 当てはまる理由 (もう一度アンインストールするときに必要) にする。dont_delete_folder の「Vesktopがその場所に公式の
// Vencordを入れます」は、プラグインのフォルダーにしか当てはまらないためである。英語は複数形 (folders、they)
inline constexpr const char *kDialogDontDeleteFoldersText = "cordmux.dialog.dont_delete_folders.text";
// 同じ注意の、プラグインのフォルダーの場所の行を置けないとき (手順書と違う置き方) に、cordmux の設定のフォルダーの場所の
// 行が続く形。数は続く場所の行の数 (1 つなら _one。英語は "the following folder")
inline constexpr CountedId kDialogDontDeleteFilesAndFoldersText = {
	"cordmux.dialog.dont_delete_files_and_folders.text",
	"cordmux.dialog.dont_delete_files_and_folders.text_one",
};
// 箇条書きの前置き (3 つのまとまりの 3 つ目)
inline constexpr const char *kDialogNotCheckedText = "cordmux.dialog.not_checked.text";
// 箇条書きの項目
// %1=シーンコレクションの名前
inline constexpr const char *kDialogLeftCollectionNotOpenedItemText =
	"cordmux.dialog.left_collection_not_opened_item.text";
// 箇条書きの項目
// %1=シーンコレクションの名前
inline constexpr const char *kDialogLeftCollectionDuplicateItemText =
	"cordmux.dialog.left_collection_duplicate_item.text";
// 箇条書きの項目。シーンコレクションのフォルダーを読めず、同じ名前のシーンコレクションがないかを確認できなかった
// (UninstallLeftoverKind::CollectionFolderUnreadable。名前を持たない項目)
inline constexpr const char *kDialogLeftCollectionFolderItemText = "cordmux.dialog.left_collection_folder_item.text";
// 結果のダイアログとカードで、確認できなかったもののうち、名前の分かるシーンコレクションが 1 つもないときの次の操作
// (どのシーンコレクションを開くか分からないので、すべてを開く)
inline constexpr const char *kDialogUninstallUncheckedAllNextText = "cordmux.dialog.uninstall_unchecked_all_next.text";
// ほかの Vencord を使っている人にだけ表示する、確認のダイアログのタイトル
inline constexpr const char *kDialogVesktopAddTitle = "cordmux.dialog.vesktop_add.title";
// 主文
inline constexpr const char *kDialogVesktopSwitchHeadline = "cordmux.dialog.vesktop_switch.headline";
// 補足。続けて場所の行を置く
inline constexpr const char *kDialogVesktopSwitchCurrentText = "cordmux.dialog.vesktop_switch_current.text";
// 切り替えの確認の補足の 1 段落目 (失うもの。段落は、失うもの、変わるもの、残るもの、元に戻す方法の順に並べる)
inline constexpr const char *kDialogVesktopSwitchPluginsText = "cordmux.dialog.vesktop_switch_plugins.text";
// 同じく 2 段落目 (変わるもの)
inline constexpr const char *kDialogVesktopSwitchVersionText = "cordmux.dialog.vesktop_switch_version.text";
// 同じく 3 段落目 (残るもの)
inline constexpr const char *kDialogVesktopSwitchKeptText = "cordmux.dialog.vesktop_switch_kept.text";
// 補足 (元に戻す方法)
inline constexpr const char *kDialogVesktopSwitchRestoreText = "cordmux.dialog.vesktop_switch_restore.text";
// 実行のボタン。もう一方は OBS の Cancel
inline constexpr const char *kDialogSwitchButton = "cordmux.dialog.switch.button";
// 診断情報を保存するダイアログと、エラーのダイアログのタイトル
inline constexpr const char *kDialogExportTitle = "cordmux.dialog.export.title";
// 保存するダイアログのファイルの種類
inline constexpr const char *kDialogExportFilterLabel = "cordmux.dialog.export_filter.label";
// エラーのダイアログの主文
inline constexpr const char *kDialogExportFailedHeadline = "cordmux.dialog.export_failed.headline";
// 補足 (書き込めなかったとき)
inline constexpr const char *kDialogExportUnwritableText = "cordmux.dialog.export_unwritable.text";
// 補足 (zip を作れなかったときと、トークンなどの秘密の値が残っていたとき。理由はログにだけ書く)
inline constexpr const char *kDialogExportRetryText = "cordmux.dialog.export_retry.text";
// 設定を保存できなかったときの、エラーのダイアログのタイトル
inline constexpr const char *kDialogSettingsSaveTitle = "cordmux.dialog.settings_save.title";
// 主文
inline constexpr const char *kDialogSettingsSaveFailedHeadline = "cordmux.dialog.settings_save_failed.headline";
// 補足
inline constexpr const char *kDialogSettingsSaveFailedText = "cordmux.dialog.settings_save_failed.text";
// 保存先を選ぶダイアログのタイトル ([録画] のページから開くので「録画」と短く書く)。英語は入力欄、ボタン、メッセージ、
// 手順書と同じ "Save Folder" を使う。選ぶダイアログのタイトルは動詞で始める (「録画の保存先を選択」/
// "Select Save Folder"。OBS の Basic.Settings.Output.SelectDirectory「録画ディレクトリを選択」/
// "Select Recording Directory" と同じ形)。"Save Folder" だけでは「フォルダーを保存」とも読める
inline constexpr const char *kDialogSaveFolderTitle = "cordmux.dialog.save_folder.title";
// [名前を変更...] で開くダイアログのタイトル (入力欄のラベルは participants.col_name_in_cordmux)
inline constexpr const char *kDialogRenameTitle = "cordmux.dialog.rename.title";

// ---- source: OBS の中のソースの名前と種類の名前、cordmux の録画ファイルの名前の語 ----
// ダミーの名前
// %1=枠の番号
inline constexpr const char *kSourcePlaceholderLabel = "cordmux.source.placeholder.label";
// 合成のソースの種類の名前 (製品名と同じで、日本語と英語で同じ)
inline constexpr const char *kSourceCompositeLabel = "cordmux.source.composite.label";
// 合成のソースのプロパティの、ボタンの上の説明
inline constexpr const char *kSourceCompositeDisplayNoteText = "cordmux.source.composite_display_note.text";
// cordmux の録画ファイルの名前に入れる語
inline constexpr const char *kSourceFileScreenShareLabel = "cordmux.source.file_screen_share.label";
// 同上
inline constexpr const char *kSourceFileVoiceLabel = "cordmux.source.file_voice.label";
// cordmux の録画ファイル名の並びと区切り (並びは名前、種類、日時)
// %1=人の名前 (cordmux での名前を、ファイル名に使える形に修正したもの)、%2=種類の語
// (cordmux.source.file_screen_share.label か cordmux.source.file_voice.label の値)、%3=セッションの日時
// (2000-01-01 21-30-45 の形。OBS の録画の既定の形で、言語で変えない)
inline constexpr const char *kSourceFileNameLabel = "cordmux.source.file_name.label";

// ---- status: ドックの状態タブ ----
// 組の見出し
inline constexpr const char *kStatusNoticesHeading = "cordmux.status.notices.heading";
inline constexpr const char *kStatusRecordingHeading = "cordmux.status.recording.heading";
inline constexpr const char *kStatusReceiveHeading = "cordmux.status.receive.heading";
inline constexpr const char *kStatusHistoryHeading = "cordmux.status.history.heading";
// メッセージがないときの 1 語
inline constexpr const char *kStatusNoticesNoneStatus = "cordmux.status.notices_none.status";
// [cordmuxの録画] の節。録画中、最後の録画がある、この起動でまだ録画していない
inline constexpr const char *kStatusRecordingNowText = "cordmux.status.recording_now.text";
// %1=録画を始めた時刻 (時刻の引数)、%2=録画の長さ (分)
inline constexpr const char *kStatusRecordingLastMinutesStatus = "cordmux.status.recording_last_minutes.status";
// %1=録画を始めた時刻、%2=時間、%3=分
inline constexpr const char *kStatusRecordingLastHoursStatus = "cordmux.status.recording_last_hours.status";
// %1=録画を始めた時刻 (1 分に満たない録画)
inline constexpr const char *kStatusRecordingLastShortStatus = "cordmux.status.recording_last_short.status";
inline constexpr const char *kStatusRecordingNoneStatus = "cordmux.status.recording_none.status";
// [映像の受信] の表の列の見出し
inline constexpr const char *kStatusColNameLabel = "cordmux.status.col_name.label";
inline constexpr const char *kStatusColGapsLabel = "cordmux.status.col_gaps.label";
inline constexpr const char *kStatusColTotalLabel = "cordmux.status.col_total.label";
inline constexpr const char *kStatusColLateLabel = "cordmux.status.col_late.label";
inline constexpr const char *kStatusColMaxLabel = "cordmux.status.col_max.label";
// 表のセル。gaps は %1=途切れの回数、total は %1=合計の秒数、late は %1=遅れたフレームの率 (小数 1 桁)、max は %1=最大の遅れ
// (ms)。数は 4 桁以上で 3 桁ごとにカンマを入れて渡す
inline constexpr const char *kStatusGapsStatus = "cordmux.status.gaps.status";
inline constexpr const char *kStatusTotalStatus = "cordmux.status.total.status";
inline constexpr const char *kStatusLateStatus = "cordmux.status.late.status";
inline constexpr const char *kStatusMaxStatus = "cordmux.status.max.status";
// 表の行のツールチップ (直近 1 分の詳しい値)。tip_late は %1=遅れたフレームの数、%2=全部のフレームの数、%3=率 (小数 1 桁)。
// tip_stutter は %1=続けて遅れたまとまりの数。tip_mean と tip_max は %1=遅れ (ms)
inline constexpr const char *kStatusTipPeriodStatus = "cordmux.status.tip_period.status";
inline constexpr const char *kStatusTipLateStatus = "cordmux.status.tip_late.status";
inline constexpr const char *kStatusTipStutterStatus = "cordmux.status.tip_stutter.status";
inline constexpr const char *kStatusTipMeanStatus = "cordmux.status.tip_mean.status";
inline constexpr const char *kStatusTipMaxStatus = "cordmux.status.tip_max.status";
// 表の下の 1 行の説明
inline constexpr const char *kStatusReceiveNoteText = "cordmux.status.receive_note.text";
// 表の代わりの 1 文 (直近 1 分にフレームが届いた人も、累計のある人もいないとき)
inline constexpr const char *kStatusReceiveEmptyEmpty = "cordmux.status.receive_empty.empty";
// 合成のソースがないシーンコレクションで、表の代わりに置く 1 文
inline constexpr const char *kStatusReceiveNoCompositeText = "cordmux.status.receive_no_composite.text";
inline constexpr const char *kStatusResetButton = "cordmux.status.reset.button";
// 「Vesktop」の組 (Vesktop の設定が同梱の Vencord を指していて、Vesktop が起動していない間だけ表示する)。見出し、
// 状態の 1 文、[Vesktopから削除] (Vesktop の設定を cordmux を追加する前の値に戻す)。ボタンの名前は [Vesktopに追加] と
// 対にする。Vesktop の起動中は組ごと表示しないので、起動中に押せない理由の文は持たない (ledger/actions.cpp の
// RemoveFromVesktop の行)
inline constexpr const char *kStatusVesktopHeading = "cordmux.status.vesktop.heading";
inline constexpr const char *kStatusVesktopAddedText = "cordmux.status.vesktop_added.text";
inline constexpr const char *kStatusRemoveFromVesktopButton = "cordmux.status.remove_from_vesktop.button";
// 履歴。when は %1=時刻の引数。history_notice_* の %1=メッセージの見出し。slot_* の %1=人の名前
inline constexpr const char *kStatusHistoryWhenStatus = "cordmux.status.history_when.status";
inline constexpr const char *kStatusHistoryNoneStatus = "cordmux.status.history_none.status";
inline constexpr const char *kStatusHistoryNoticeAppearedStatus = "cordmux.status.history_notice_appeared.status";
inline constexpr const char *kStatusHistoryNoticeClearedStatus = "cordmux.status.history_notice_cleared.status";
inline constexpr const char *kStatusHistoryConnectedStatus = "cordmux.status.history_connected.status";
inline constexpr const char *kStatusHistoryDisconnectedStatus = "cordmux.status.history_disconnected.status";
inline constexpr const char *kStatusHistorySlotAddedStatus = "cordmux.status.history_slot_added.status";
inline constexpr const char *kStatusHistorySlotRemovedStatus = "cordmux.status.history_slot_removed.status";
inline constexpr const char *kStatusHistoryRecordingStartedStatus = "cordmux.status.history_recording_started.status";
inline constexpr const char *kStatusHistoryRecordingStoppedStatus = "cordmux.status.history_recording_stopped.status";
// 映像が届かなくなって、受信をやり直した人 (%1=人の名前。pipeline/receive_restart.h)
inline constexpr const char *kStatusHistoryReceiveRestartedStatus = "cordmux.status.history_receive_restarted.status";

// ---- plugin: OBS のプラグインの説明 ----
// obs_module_description が返す 1 行
inline constexpr const char *kPluginDescriptionText = "cordmux.plugin.description.text";
// OBS の [設定] の [ホットキー] に並ぶ、cordmux のホットキーの名前 (OBS の画面の中なので cordmux を書く)。4 等分の位置の
// 枠に描いている人を全画面で表示するものが 4 つと、全画面を元に戻すもの
inline constexpr const char *kPluginHotkeyFullScreenTopLeftLabel = "cordmux.plugin.hotkey_full_screen_top_left.label";
inline constexpr const char *kPluginHotkeyFullScreenTopRightLabel = "cordmux.plugin.hotkey_full_screen_top_right.label";
inline constexpr const char *kPluginHotkeyFullScreenBottomLeftLabel =
	"cordmux.plugin.hotkey_full_screen_bottom_left.label";
inline constexpr const char *kPluginHotkeyFullScreenBottomRightLabel =
	"cordmux.plugin.hotkey_full_screen_bottom_right.label";
inline constexpr const char *kPluginHotkeyFullScreenRestoreLabel = "cordmux.plugin.hotkey_full_screen_restore.label";

} // namespace keys

// OBS 自身の ini のキー (obs_frontend_get_locale_string で引く)。[OK] [キャンセル] [適用] [閉じる] は、OBS の
// [設定] と同じテキストにするため、cordmux の ini には持たない。OBS 31.1.1 と 32.2.2 の両方の ini にキーがある
// ことを確認した。`OK` は en-US.ini にだけあるが、OBS は en-US を先に読むので、日本語の画面でも表示される。
namespace obs_keys {
inline constexpr const char *kOk = "OK";
inline constexpr const char *kCancel = "Cancel";
inline constexpr const char *kApply = "Apply";
inline constexpr const char *kClose = "Close";
} // namespace obs_keys

} // namespace cordmux::ledger::text

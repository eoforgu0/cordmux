// SPDX-License-Identifier: Apache-2.0

// actions.h の実装。押せる条件の表を 1 か所のデータとして持つ。表の 1 行は ActionId 1 つ分で、その操作がさせる
// 仕事 (IntentKind) と、押せないときの理由のキーを返す関数を持つ。関数が nullptr を返せば押せる。
// 表は ActionId を 1 つずつ漏れなく持つ (テストが kActionIdCount と比べて確認する)。

#include "ledger/actions.h"

#include <cstddef>
#include <string>

#include "ledger/text_ids.h"

namespace cordmux::ledger {

namespace {

// この user_id の人が Bot のアカウントと分かっているか。現在の通話の人にいなければ偽 (分からない)。
bool person_is_bot(const Observation &obs, const std::string &user_id)
{
	for (const PersonFact &person : obs.people) {
		if (person.user_id == user_id)
			return person.is_bot;
	}
	return false;
}

// この user_id の人が自分の枠の人の行と分かっているか。現在の通話の人にいなければ偽 (分からない)。
bool person_is_self_frame(const Observation &obs, const std::string &user_id)
{
	for (const PersonFact &person : obs.people) {
		if (person.user_id == user_id)
			return person.is_self_frame;
	}
	return false;
}

// 枠の番号が範囲の中か。0 は枠から外す操作の番号なので、範囲に含める。
bool slot_in_range(config::SlotKind kind, int slot)
{
	// 声の枠には上限がない。
	return slot >= 0 && (kind == config::SlotKind::Voice || slot <= config::kScreenSlotCount);
}

// [移動] と [退出] を実行する Vesktop の中の関数 (依存箇所の voice-select) が見つからないと分かっているか。まだ分からない
// 間は押せる (送信側が押された時点で探し直して、見つからなければログに書く)。
bool voice_select_missing(const Observation &obs)
{
	for (const ContactFact &contact : obs.contacts) {
		if (contact.id == "voice-select")
			return contact.state == ContactState::Missing || contact.state == ContactState::Lost;
	}
	return false;
}

// Vesktop の設定 (state.json と Vencord の設定) を書く操作を、Vesktop の状態で押せなくする理由。Vesktop が起動して
// いれば running_text、起動しているかをまだ読めていなければ「確認しています」を返し、起動していないと分かっているときだけ
// nullptr を返す。アンインストールの行 (Vesktop の起動中と、状態を読めていない間) と同じ形である。
// 起動中に書かないのは、Vesktop (v1.6.9 の src/main/settings.ts の loadSettings と SettingsStore) が state.json を
// 起動したときに 1 回だけ読んでメモリに持ち、値が変わるたびにメモリの値の全体で書き直すためである。ウィンドウを
// 動かす、大きさを変える、最小化する、元に戻すだけでも値が変わる (src/main/mainWindow.ts の
// initWindowBoundsListeners)。起動中に cordmux が vencordDir を書くと、再起動の前にウィンドウを動かしただけで、
// Vesktop が元の値で上書きし、cordmux の書いた値が知らされずに消える。Vesktop のほかのバージョンがこの動きを
// 変えても、起動していない間に書けば、Vesktop は起動したときに必ず読むので、どのバージョンでも安全な側になる。
// 状態を読めていない間も押せなくする。書いた値が消えるかを確認できない間は、書かない側にする。
const char *vesktop_busy_reason(const Observation &obs, const char *running_text)
{
	if (obs.install.process == VesktopProcess::Running)
		return running_text;
	if (obs.install.process == VesktopProcess::Unknown)
		return text::keys::kDockVesktopCheckingText;
	return nullptr;
}

struct ActionRule {
	ActionId id;
	IntentKind kind;
	// 押せないときだけ理由の文言 id を返す。nullptr なら押せる。
	const char *(*disabled_reason)(const Observation &, const ActionArgs &);
};

const ActionRule kActionRules[] = {
	// [Vesktopに追加]。押せない理由は、上から順に最初の 1 つ。
	//   Vesktop の設定が既に同梱の Vencord を指している -> reason::kDirMatches (押しても何も変わらない。ボタンを置かない)
	//   Vesktop の起動中                                -> keys::kDockAddVesktopRunningText
	//   Vesktop が起動しているかを読めていない          -> keys::kDockVesktopCheckingText
	// Vesktop の設定 (dir) をまだ読めていない (Unknown) ことは、押せない理由にしない。読めないことを理由に
	// インストールを止めないためで、書いてよいかは install/ の側が決める。Vesktop の状態は、上の
	// vesktop_busy_reason の理由で見る。押した後、書く直前にも Vesktop が起動していないことを確認する
	// (dock/dock_poll_install.cpp の write_vencord_dir)。
	{ActionId::WriteVencordDir, IntentKind::WriteVencordDir,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 if (obs.install.dir == VencordDirState::Matches)
			 return reason::kDirMatches;
		 return vesktop_busy_reason(obs, text::keys::kDockAddVesktopRunningText);
	 }},

	// Vesktop の設定が別の場所を指しているときの [Vesktopに追加]。その状態のときだけ表示する操作なので、
	// ほかの状態では押せない。Vesktop の状態で押せなくするのは WriteVencordDir と同じ (見た目が同じボタンなので、
	// 同じ条件にする)。
	{ActionId::RepairVencordDir, IntentKind::WriteVencordDir,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 if (obs.install.dir != VencordDirState::PointsElsewhere)
			 return reason::kDirNotElsewhere;
		 return vesktop_busy_reason(obs, text::keys::kDockAddVesktopRunningText);
	 }},

	// [再インストール]。同梱の Vencord の 4 つのファイルが配布時のままなら、コピーし直す必要がない。
	{ActionId::ReinstallVencordFiles, IntentKind::ReinstallVencordFiles,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 return obs.install.dir == VencordDirState::HashMismatch ? nullptr : reason::kFilesMatch;
	 }},

	// 診断情報のエクスポート。条件を付けない。診断情報は問題が起きたときに渡すものなので、問題が起きているときほど
	// 押せる必要がある。
	{ActionId::ExportBundle, IntentKind::ExportBundle, nullptr},

	// 「自動で載せる」の切り替え。通話に参加していない人にも切り替えられるので、条件は誰を指しているかだけである。
	{ActionId::SetAlwaysOn, IntentKind::SetAlwaysOn,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return reason::kNoUser;
		 // 載せる人の選び方が「自動」の間は、画面共有を始めた人を全員載せるので、「自動で載せる」は何も
		 // 変えない。押しても何も起きない操作を作らないように、押せなくする。参加者の設定ウィンドウでは、
		 // 同じ内容の別のテキスト (cordmux.participants.auto_add_disabled.tip) に rules_dock.cpp が替える。
		 if (obs.choices.assign_mode == AssignMode::Auto)
			 return text::keys::kDockRowAutoAddDisabledTip;
		 return nullptr;
	 }},

	// 人を枠に載せる、枠から外す。接続していなくても押せる (枠の割り当ては接続が切れても残り、繋ぎ直して同じ人が
	// 画面共有していれば、そのまま映り直す)。
	//
	// ActionArgs::slot が 0 なら枠から外し、1 以上ならその枠に載せる。押せる条件は向きで違う。
	//   - 外す: その人が今枠に載っているときだけ (載っていない人を外しても何も変わらない)。
	//   - 載せる: その人がまだ枠に載っておらず、画面共有していないと分かっていなくて、指した枠が空いているときだけ。
	// 別の人が載っている枠には載せない (上書きしない)。枠は、載っている人が画面共有をやめるか通話を抜けると空く。
	// ほかの人を載せたいときは、同じ一覧で前の人を枠から外す。画面共有していない人は載せられない。
	// ミュートと音量は枠ではなく人に付くので、枠を外しても載せても、前の人の値は残らず、次の人にも引き継がれない
	// (pipeline/pipeline.h の set_person_audio_mute)。
	{ActionId::SetSlot, IntentKind::SetSlot,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return reason::kNoUser;
		 if (!slot_in_range(args.kind, args.slot))
			 return reason::kBadSlot;
		 // 声の枠は、この操作では動かさない (使う声は SetVoiceSource が決める)。画面の枠だけを見る。
		 if (args.kind != config::SlotKind::Screen)
			 return nullptr;
		 // 自分の枠の人の行は、自分の枠に映す映像を OBS から選んでいる間、チェックを SetSelfFrame で動かす。
		 // その人の Discord の画面を枠に載せることは、ここでは受け付けない。外す向きは、前から載っていた枠を
		 // 外せるように通す。
		 if (args.slot != 0 && obs.choices.self_frame_selected && person_is_self_frame(obs, args.user_id))
			 return reason::kSelfFrameRow;
		 const int current = screen_slot_of(obs, args.user_id);
		 if (args.slot == 0)
			 return current != 0 ? nullptr : reason::kNotOnSlot;
		 if (current != 0)
			 return reason::kAlreadyOnSlot;
		 // 画面共有していない人は載せられない (枠を持つのは画面共有している人だけ。画面共有をやめると枠は空く)。
		 // 空きがあるかより先に見る。押せない理由が、空きの有無と別だからである。
		 if (person_known_not_sharing(obs, args.user_id))
			 return text::keys::kDockRowCheckNotSharingTip;
		 // 自分の枠を使っている間 (映像がまだ結び付いていない間も含む) に、Discord の人を載せられるのは 3 人まで
		 // (observation.h の self_frame_limit_reached。自動で載せる経路も同じ上限)。テキストは一覧の上の 1 行と同じ。
		 // 空いている枠があるかより先に見る。4 つ目の枠が空いていても載せられないためである。
		 if (self_frame_limit_reached(obs))
			 return text::keys::kDockSelfSlotLimitText;
		 // 空いている枠が 1 つもないときだけ、その理由を画面に表示する。ほかの枠が空いていれば、そちらに
		 // 載せられる。
		 if (!screen_slot_owner(obs, args.slot).empty())
			 return free_screen_slot(obs) == 0 ? text::keys::kDockNoFreeSlotText : reason::kSlotTaken;
		 return nullptr;
	 }},

	// 使う声を別のアカウントにする、本人の声に戻す。user_id は声を置き換える人、voice_user_id は使う声の
	// アカウントで、空か user_id と同じなら本人の声に戻す。kind と slot は見ない (理由は actions.h の
	// ActionArgs::voice_user_id)。使う声の設定は seen_people.json に残り、user_id の人が通話に参加していなくても
	// 動作し続ける (pipeline/stream_router.h の AssignPolicy) ので、通話に参加しているかを条件にしない。
	// 押せない理由は、上から順に最初の 1 つを返す。
	//   誰に対する操作かを指していない             -> reason::kNoUser
	//   戻そうとしたが、使う声の設定がない         -> reason::kNoVoiceSource (押しても何も変わらない)
	//   今と同じアカウントを選んだ                 -> reason::kSameVoiceSource (押しても何も変わらない)
	//   そのアカウントの声を、別の人が使っている   -> keys::kParticipantsVoiceTakenText (1 人の声を 2 人に
	//                                                 使わない)
	//   そのアカウントが、ほかの人の声を使っている -> keys::kParticipantsVoiceUsesOtherText (連鎖させない)
	//   user_id の人の声を、ほかの人が使っている   -> reason::kVoiceSourceLent (連鎖させない)
	// 1 対 1 にし、連鎖 (A が B の声を使い、B が C の声を使う形) を作らないのは、経路が使う声のアカウントから
	// 声を置き換える人を逆に引くためである。逆に引いた人が 1 人に決まらないと、声をどの枠に入れるかが決まらない
	// (config/allocation.h の usable_voice_links)。ここでは、経路が使わない設定 (手で書き換えて衝突したもの) も
	// 含めて、設定そのもの (observation.h の voice_source_marks()) を見る。新しい衝突を増やさないためである。
	// 戻す向きは設定の有無だけを見るので、衝突した設定も戻せる。連鎖の 2 つの理由を分けるのは、修正する行が違うため
	// である (使う声のアカウントの行を戻すのか、user_id の人の声を使っている人の行を戻すのか)。
	{ActionId::SetVoiceSource, IntentKind::SetVoiceSource,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return reason::kNoUser;
		 const std::string current = voice_source_of(obs, args.user_id);
		 const bool reset = args.voice_user_id.empty() || args.voice_user_id == args.user_id;
		 if (reset)
			 return current.empty() ? reason::kNoVoiceSource : nullptr;
		 if (args.voice_user_id == current)
			 return reason::kSameVoiceSource;
		 // テキストは「「%1」の声は、既に「%2」の行で使っています。」。apply() はキーだけを返し、
		 // %1 と %2 (人の名前) は ledger/rules_dock.cpp が View を作るときに入れる。
		 if (!voice_source_owner_of(obs, args.voice_user_id, args.user_id).empty())
			 return text::keys::kParticipantsVoiceTakenText;
		 // テキストは「「%1」は、ほかの人の声を使っています。」。%1 (使う声のアカウントの人の名前) は
		 // ledger/rules_dock.cpp が View を作るときに入れる。
		 if (!voice_source_of(obs, args.voice_user_id).empty())
			 return text::keys::kParticipantsVoiceUsesOtherText;
		 if (!voice_source_owner_of(obs, args.user_id).empty())
			 return reason::kVoiceSourceLent;
		 return nullptr;
	 }},

	// 配信に表示する名前を上書きする、上書きをやめる。通話に参加していない人にも適用される (参加者の設定ウィンドウに、
	// 過去の参加者の行がある) ので、条件は誰を指しているかと、押して何かが変わるかの 2 つだけである。
	// 何かが変わるかは、渡された名前と現在の上書きを比べて決める。押せないのは、空の名前 (上書きをやめる) を
	// 渡したのにやめる上書きがないときと、今と同じ名前を渡したときである。今と同じ名前のほうも押せなくするのは、
	// どちらも押して何も変わらない点で同じだからである。現在の上書きの値は、一覧の表示と同じく observation.h の
	// name_override_of() で決める。
	{ActionId::SetNameOverride, IntentKind::SetNameOverride,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return reason::kNoUser;
		 const std::string current = name_override_of(obs, args.user_id);
		 if (args.name.empty() && current.empty())
			 return reason::kNoOverride;
		 if (!args.name.empty() && args.name == current)
			 return reason::kSameName;
		 return nullptr;
	 }},

	// 機能の ON/OFF。いつでも押せる。利用者がはっきり指示する操作なので、確認のダイアログも表示しない。
	{ActionId::ToggleFeature, IntentKind::SetFeature, nullptr},

	// cordmux の録画の ON/OFF。OFF は異常として扱わない。押せないのは次の 2 つである。
	//   - 開始の処理中と停止の処理中。理由のテキストはボタンの処理中のテキストそのもの (「開始処理中...」と
	//     「停止処理中...」) で、ボタン自身がそれを表示する。
	//   - 機能を OFF にしている間に、録画を始めようとしたとき。OFF の間は、受信側が送信側に media を false のまま
	//     送るので、送信側は映像を読み出さず、始めても 1 フレームも届かない。OFF にしたときの録画は止めて
	//     録画ファイルを作成するので、押せなくなるのは OFF の後に始め直す操作だけである。
	// 止める向きは押せなくしない。録画を止められなくなると、止まらない録画が残る。保存先に書き込めなかったことでも
	// 押せなくしない。保存先は録画を始めるたびに決め直すので、[保存先を変更] で修正した後や、書き込めるようになった
	// 後に押し直せる必要がある。書き込めなかったことは RecordDirUnwritable のメッセージが表示する。
	{ActionId::ToggleRecording, IntentKind::SetRecording,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (obs.recording.phase == RecordPhase::Starting)
			 return text::keys::kDockRecordStartingButton;
		 if (obs.recording.phase == RecordPhase::Stopping)
			 return text::keys::kDockRecordStoppingButton;
		 if (!args.on)
			 return nullptr;
		 if (obs.choices.feature == FeatureSwitch::Off)
			 return text::keys::kDockRecordDisabledOffText;
		 return nullptr;
	 }},

	// ダミーの表示の ON/OFF。いつでも押せる。
	{ActionId::ToggleDummy, IntentKind::SetDummy, nullptr},

	// [作成を再開]。押せるのは、録画ファイルの作成が終わっていない録画が実際に残っているときだけである。
	// RecordFinalizePending のメッセージと同じフィールド (RecordingFacts::pending_present) を見る。
	// 録画ファイルの作成に失敗したメッセージ (RecordFinalizeFailed) の [作成を再開] も同じ条件である。録画の側は、録画ファイルの作成に
	// 失敗した録画のセッションも作成待ちに数える (録画の中間ファイルは残っていて、再開すればもう一度録画ファイルを作成する)。
	// 失敗したことだけを条件に押せるようにはしない。作成待ちに数えられていないセッションは再開しても対象に
	// ならず、押しても何も起きないためである。
	{ActionId::ResumeFinalize, IntentKind::ResumeFinalize,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 return obs.recording.pending_present ? nullptr : reason::kNoPendingFinalize;
	 }},

	// cordmux のアンインストール。押せない理由は、上から順に最初の 1 つを返す。
	//   アンインストールの最中                      -> keys::kSettingsUninstallStartedText
	//   アンインストールが終わった後                -> keys::kSettingsUninstallDoneButtonText
	//   OBS が配信中                                -> keys::kSettingsUninstallStreamingText
	//   OBS が録画中                                -> keys::kSettingsUninstallObsRecordingText
	//   リプレイバッファの動作中                    -> keys::kSettingsUninstallReplayBufferText
	//   仮想カメラの動作中                          -> keys::kSettingsUninstallVirtualCameraText
	//   cordmux の録画中                            -> keys::kSettingsUninstallPerPersonRecordingText
	//   Vesktop の起動中                            -> keys::kSettingsUninstallVesktopRunningText
	//   配信・OBS の録画・Vesktop の状態を読めていない -> keys::kSettingsUninstallCheckingText
	// OBS の出力の動作中に押せないのは、アンインストールが使用中のソースを消し、シーンコレクションを切り替える
	// ためである。Vesktop の起動中に押せないのは、起動中の Vesktop が state.json をメモリの値で書き直し、
	// アンインストールで元に戻した値が消えるためである (理由は上の vesktop_busy_reason のコメント)。
	// 途中で止まったとき (Stopped) は押し直せる。残りを片付ける次の操作がこのボタンで、
	// kSettingsUninstallDoneButtonText は終わった後のテキストだからである。
	// 状態を読めていない間は押せない。元に戻せない操作なので、確認できない間は押せない側にする。読めた状態で
	// 押せない理由があれば、そちらを先に返す。
	{ActionId::Uninstall, IntentKind::Uninstall,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 if (obs.uninstall.phase == UninstallPhase::Running)
			 return text::keys::kSettingsUninstallStartedText;
		 if (obs.uninstall.phase == UninstallPhase::Done)
			 return text::keys::kSettingsUninstallDoneButtonText;
		 if (obs.uninstall.obs_streaming.value_or(false))
			 return text::keys::kSettingsUninstallStreamingText;
		 if (obs.uninstall.obs_recording.value_or(false))
			 return text::keys::kSettingsUninstallObsRecordingText;
		 // 理由の並びは、OBS のコントロールのボタンの並び (配信、録画、リプレイバッファ、仮想カメラ) に
		 // 合わせる。リプレイバッファと仮想カメラは、読めていない (nullopt) 間も「確認しています」の条件に
		 // 入れない。配信と録画と同じ時点に同じ API (obs_frontend_*_active) で読むので、配信と録画が
		 // 読めていれば、この 2 つも読めている。「確認しています」のテキストはこの 2 つも挙げているが、2 つが
		 // 読めていない間は配信と録画も読めておらず、そのテキストを表示しているので、条件と食い違わない。
		 if (obs.uninstall.obs_replay_buffer.value_or(false))
			 return text::keys::kSettingsUninstallReplayBufferText;
		 if (obs.uninstall.obs_virtual_camera.value_or(false))
			 return text::keys::kSettingsUninstallVirtualCameraText;
		 if (obs.choices.recording == FeatureSwitch::On)
			 return text::keys::kSettingsUninstallPerPersonRecordingText;
		 if (obs.install.process == VesktopProcess::Running)
			 return text::keys::kSettingsUninstallVesktopRunningText;
		 if (!obs.uninstall.obs_streaming.has_value() || !obs.uninstall.obs_recording.has_value() ||
		     obs.install.process == VesktopProcess::Unknown)
			 return text::keys::kSettingsUninstallCheckingText;
		 return nullptr;
	 }},

	// cordmux の設定ウィンドウを開く。いつでも押せる。アンインストールの後も、結果をもう一度見るために開ける。
	// 開くページは args.page ([保存先を変更] は SettingsPage::Recording)。
	{ActionId::OpenSettings, IntentKind::OpenSettings, nullptr},

	// 参加者の設定ウィンドウを開く。いつでも押せる。開くだけでは何も書かず、ウィンドウの中の項目 (SetAlwaysOn
	// など) は、それぞれの行が押せなくする。
	{ActionId::OpenParticipants, IntentKind::OpenParticipants, nullptr},

	// 画面共有していない参加者の並びに出すか (列「通話のみ時ラベル非表示」)。SetAlwaysOn と同じく、設定は seen_people.json に残るので、
	// 通話に参加していない人にも適用される。条件は誰を指しているかである。メインアカウントの行も押せる。メインアカウントも、
	// 自分の枠に名前ラベル付きで映っていない間は、ほかの人と同じく並びに出る (dock/build_observation.h の
	// composite_roster_people) ので、外したい人がこの列で外せる。設定の roster_show が偽の間は誰を並びに出すかが動作せず、
	// roster_show_bots が偽の間は Bot の行が動作しないので、押せなくする。保存した値は変えないので、設定を戻せば前の選択が
	// 動作する。Bot かどうかは、現在の通話の人だけが分かる (過去の参加者と、profiles がまだ届かない人は、Bot と分からないので
	// 押せる)。
	{ActionId::SetRosterShown, IntentKind::SetRosterShown,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return reason::kNoUser;
		 if (!obs.choices.roster_show)
			 return text::keys::kParticipantsShowOffTip;
		 if (!obs.choices.roster_show_bots && person_is_bot(obs, args.user_id))
			 return text::keys::kParticipantsShowBotsOffTip;
		 return nullptr;
	 }},

	// 参加者ごとの声と画面共有の音の、ミュートと音量。値は合成のソースが出す混ぜた音に掛かる (pipeline/mixer_roster.h の
	// MixerLevels)。合成のソースがないシーンコレクションでも、値は表に入り (動作しないだけ)、あとで合成のソースを使う
	// ようになれば動作するので、押せなくしない。通話に参加していない人にも適用される (人の行は一覧に残る)。
	// ミュートの条件は誰を指しているかだけで、今と同じ値でも押せる。音量はさらに範囲 (0 から 200) を見る。今と同じ値を
	// 押せなくしないのは、ドックが見ている値が最大で 1 周期古く、動かした先の値が現在の値と同じに見えて捨てられると、
	// スライダーを戻した値が反映されないためである。
	{ActionId::SetVoiceMute, IntentKind::SetVoiceMute,
	 [](const Observation &, const ActionArgs &args) -> const char * {
		 return args.user_id.empty() ? reason::kNoUser : nullptr;
	 }},
	{ActionId::SetScreenAudioMute, IntentKind::SetScreenAudioMute,
	 [](const Observation &, const ActionArgs &args) -> const char * {
		 return args.user_id.empty() ? reason::kNoUser : nullptr;
	 }},
	{ActionId::SetVoiceVolume, IntentKind::SetVoiceVolume,
	 [](const Observation &, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return reason::kNoUser;
		 return config::person_volume_in_range(args.volume_pct) ? nullptr : reason::kBadVolume;
	 }},
	{ActionId::SetScreenAudioVolume, IntentKind::SetScreenAudioVolume,
	 [](const Observation &, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return reason::kNoUser;
		 return config::person_volume_in_range(args.volume_pct) ? nullptr : reason::kBadVolume;
	 }},

	// 自分の枠のチェックを付け外しする。ActionArgs::on が真なら出す、偽なら消す。自分の枠に映す映像を OBS から選んで
	// いれば、結び付いていない間 (見つからない間) も押せる (値は settings.json に残り、結び付くと動作する)。押せない理由は
	// 上から順に最初の 1 つ。
	//   映像を OBS から選んでいない (サブアカウントなしでは経路がない)   -> keys::kDockRowSelfNoVideoTip
	//   今と同じ状態にする向き                                           -> reason::kSameSelfFrame (押しても何も変わらない)
	//   出す向きで、Discord の人が画面共有の枠 4 つをすべて埋めている   -> keys::kDockNoFreeSlotText
	// 4 人目を押しのけて自分の枠を出すと、配信に出ていた人が突然消える (配置が、後ろの人を外すため)。消す向きは、
	// 枠の埋まりによらず押せる。この Observation は最大 1 周期前の値なので、押した後に自動で 4 人目が載っていたときは、
	// Pipeline が断り、ドックがチェックを外す (pipeline/stream_router.h の accepted_self_frame_in_use)。
	{ActionId::SetSelfFrame, IntentKind::SetSelfFrame,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (!obs.choices.self_frame_selected)
			 return text::keys::kDockRowSelfNoVideoTip;
		 if (args.on == obs.choices.self_frame_on)
			 return reason::kSameSelfFrame;
		 if (args.on && discord_slotted_count(obs) >= config::kScreenSlotCount)
			 return text::keys::kDockNoFreeSlotText;
		 return nullptr;
	 }},

	// [移動]。押せない理由は、上から順に最初の 1 つ。自動では動かないので、ずれているときも押すまで何も起きない。
	//   Vesktop と接続していない                     -> keys::kDockAccountNotConnectedTip
	//   サブアカウントを使う構成でない               -> reason::kNoSubAccount (行そのものがない)
	//   同じ通話に参加している                       -> keys::kDockAccountSameCallTip
	//   Vesktop の中の通話を選ぶ関数が見つからない   -> keys::kDockAccountCallControlMissingTip
	//   メインアカウントの通話が分からない           -> keys::kDockAccountMainCallUnknownTip
	//   参加できるか分からない                       -> keys::kDockAccountMainCallUnknownTip
	//   参加できない                                 -> keys::kDockAccountCannotJoinTip
	// メインアカウントが「どの通話にも参加していない」理由は持たない。Vesktop のアカウントから見えないサーバーの通話に
	// いるときと、どの通話にもいないときを区別できず、後者と断定できないためである (Call::main_presence)。
	{ActionId::MoveToMainCall, IntentKind::MoveToMainCall,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 if (obs.link.state != LinkState::Connected)
			 return text::keys::kDockAccountNotConnectedTip;
		 if (!has_sub_account(obs))
			 return reason::kNoSubAccount;
		 if (obs.call.presence == Presence::InCall && main_account_in_roster(obs))
			 return text::keys::kDockAccountSameCallTip;
		 if (voice_select_missing(obs))
			 return text::keys::kDockAccountCallControlMissingTip;
		 if (obs.call.main_presence == Presence::Unknown || !obs.call.main_call_joinable.has_value())
			 return text::keys::kDockAccountMainCallUnknownTip;
		 return *obs.call.main_call_joinable ? nullptr : text::keys::kDockAccountCannotJoinTip;
	 }},

	// [退出]。押せるのは、サブアカウントが通話に参加しているときだけである。
	{ActionId::LeaveCall, IntentKind::LeaveCall,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 if (obs.link.state != LinkState::Connected)
			 return text::keys::kDockAccountNotConnectedTip;
		 if (!has_sub_account(obs))
			 return reason::kNoSubAccount;
		 if (obs.call.presence != Presence::InCall)
			 return text::keys::kDockAccountSubNoCallTip;
		 return voice_select_missing(obs) ? text::keys::kDockAccountCallControlMissingTip : nullptr;
	 }},

	// メインアカウントを選ぶ。接続していなくても押せる (値は settings.json に残り、接続すると動作する)。今と同じ値は
	// 押しても何も変わらない。Vesktop のアカウントを選ぶのは「メインアカウントを使わない」と同じ構成なので、そちらで
	// 選んでもらう (メニューに Vesktop のアカウントを並べない)。
	{ActionId::SetMainAccount, IntentKind::SetMainAccount,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id == obs.choices.broadcaster_user_id)
			 return reason::kSameMainAccount;
		 if (!args.user_id.empty() && args.user_id == vesktop_account_id(obs))
			 return reason::kMainIsVesktopAccount;
		 return nullptr;
	 }},

	// 枠に載っている人を、1 回で別の人に替える。押せる条件は、外す人が枠に載っていて、載せる人が載っていないことである。
	// 枠の数は変わらないので、空きがあるかは見ない。自分の枠の人の行は、自分の枠に映す映像を OBS から選んでいる間は
	// チェックを自分の枠の付け外しで動かすので、受け付けない (SetSlot と同じ)。
	{ActionId::ReplaceSlot, IntentKind::ReplaceSlot,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty() || args.replace_user_id.empty())
			 return reason::kNoUser;
		 if (obs.choices.self_frame_selected && person_is_self_frame(obs, args.user_id))
			 return reason::kSelfFrameRow;
		 if (screen_slot_of(obs, args.user_id) != 0)
			 return reason::kAlreadyOnSlot;
		 if (person_known_not_sharing(obs, args.user_id))
			 return text::keys::kDockRowCheckNotSharingTip;
		 return screen_slot_of(obs, args.replace_user_id) != 0 ? nullptr : reason::kNotOnSlot;
	 }},

	// 映像途切れの累計をゼロに戻す。累計はメモリだけにあり、何も書かないので、いつでも押せる。
	{ActionId::ResetStallTally, IntentKind::ResetStallTally, nullptr},

	// 全画面で映す人を替える。ActionArgs::user_id が空なら元の配置に戻す。確認はしない (戻せる操作なので)。押せない理由は、
	// 上から順に最初の 1 つ。
	//   戻す向きで、全画面にしていない                     -> reason::kNoFocus (押しても何も変わらない)
	//   cordmux が無効                                     -> keys::kDockSummaryDisabledStatus (無効の間は映像が届かない)
	//   その人を既に全画面にしている                       -> reason::kSameFocus (押しても何も変わらない)
	//   その人を合成のソースのタイルに描いていない         -> keys::kDockRowFullScreenUnavailableTip
	// 描いていない人 (枠に載っていない人、枠が足りずに描けない人) を全画面にしても、その人の映像はないので、押せなくする。
	// 全画面にしている人を別の人に替えるときは、戻さずにそのまま押せる。
	{ActionId::SetFocus, IntentKind::SetFocus,
	 [](const Observation &obs, const ActionArgs &args) -> const char * {
		 if (args.user_id.empty())
			 return obs.choices.focus_user_id.empty() ? reason::kNoFocus : nullptr;
		 if (obs.choices.feature == FeatureSwitch::Off)
			 return text::keys::kDockSummaryDisabledStatus;
		 if (args.user_id == obs.choices.focus_user_id)
			 return reason::kSameFocus;
		 return focus_candidate(obs, args.user_id) ? nullptr : text::keys::kDockRowFullScreenUnavailableTip;
	 }},

	// 状態タブの [Vesktopから削除]。押せない理由は、上から順に最初の 1 つ。
	//   Vesktop の設定が同梱の Vencord を指していない      -> reason::kNotAdded (外すものがない。組ごと表示しない)
	//   Vesktop の起動中か、起動しているかを読めていない   -> reason::kVesktopNotStopped (組ごと表示しない)
	//   この起動で Vesktop の設定を書かない               -> keys::kDockVesktopAddFailedWritesOffText か
	//                                                        keys::kDockVesktopAddFailedSettingsUnreadText
	// 指しているとみなすのは Matches と HashMismatch (ファイルが上書きされていても、Vesktop は同梱の場所を読む。
	// 上書きした Vencord が合わずに Vesktop の画面が表示されないときにも、ここから戻せる必要がある)。
	// 状態タブの「Vesktop」の組は、Vesktop が起動していないと分かっている間だけ表示する (rules_dock.cpp の status_tab_of)。
	// 起動中に押せないボタンと「先にVesktopを終了してください」を出し続けると、配信中に状態タブを見る配信者には、ふだんの
	// 使い方で意味のない指示になるためである。Vesktop の画面が表示されないときは、ContactUnavailable のメッセージが
	// 「Vesktopを終了して、[Vesktopから削除]」を伝え、終了すると組が現れる。そのため、Vesktop の状態で押せない理由は
	// 画面に表示しないマーカーにする (押せないことそのものは、[Vesktopに追加] と同じ vesktop_busy_reason の条件)。
	// 書かない起動の文は、押す前の [Vesktopに追加] のカード (rules_dock.cpp の install_notice) と同じものを使う。
	{ActionId::RemoveFromVesktop, IntentKind::RemoveFromVesktop,
	 [](const Observation &obs, const ActionArgs &) -> const char * {
		 if (obs.install.dir != VencordDirState::Matches && obs.install.dir != VencordDirState::HashMismatch)
			 return reason::kNotAdded;
		 if (vesktop_busy_reason(obs, reason::kVesktopNotStopped) != nullptr)
			 return reason::kVesktopNotStopped;
		 if (obs.install.vesktop_writes_off)
			 return obs.ops.settings_unreadable_at_startup ? text::keys::kDockVesktopAddFailedSettingsUnreadText
								       : text::keys::kDockVesktopAddFailedWritesOffText;
		 return nullptr;
	 }},
};

constexpr std::size_t kActionRuleCount = sizeof(kActionRules) / sizeof(kActionRules[0]);

// アンインストールを始めた後に押せなくする操作か。アンインストールを始めた後は、OBS を終了するまで cordmux は
// 何も書かない。その後も押せる操作を残すと、押しても何も残らないか、アンインストールしたばかりのもの
// ([Vesktopに追加] で作るもの) を作り直すことになる。
// 押せない理由は、最中・終わった後・途中で止まった後のどれでも keys::kSettingsUninstallStartedText
// (「アンインストールを開始した後は使用できません。」) にする。どの状態でも正しい文で、OBS を終了するまで押せる
// ようにはならない。次にすること (OBS を終了してフォルダーを削除する、もう一度アンインストールする) は 1 行目と
// メッセージのレベル 2 のカードが表示するので、ここでは繰り返さない。終わった後だけ
// keys::kSettingsUninstallDoneButtonText にしないのは、そのキーが [cordmuxをアンインストール...] 自身の理由の
// テキストで、ほかの操作には「開始した後は使用できません」がどの状態でも正しいためである。
//
// 押せなくしない操作は次のとおりで、どれも cordmux の残りを増やさない。
//   診断情報のエクスポート     途中で止まったときに渡すもの
//   [作成を再開]             録画したものから録画ファイルを作成する (アンインストールは録画を消さない)
//   録画を止める向き           録画を止められなくしない (ToggleRecording の行と同じ)
//   設定ウィンドウ             開くだけでは何も書かない (ウィンドウの中の項目は、それぞれの行が押せなくする)
//   参加者の設定ウィンドウ     同上
//   全画面を元に戻す向き       メモリの値を戻すだけで、何も書かない (全画面にする向きは、消している途中のソースに
//                              映すことになるので押せなくする)
// アンインストール自身は自分の行で判断する (途中で止まったときは押し直せる)。
// default: を書かない。ActionId を追加したら、ここに case を追加して押せなくするかを決める (case のない値は
// 押せなくする)。
bool blocked_after_uninstall(ActionId id, const ActionArgs &args)
{
	switch (id) {
	case ActionId::ExportBundle:
	case ActionId::ResumeFinalize:
	case ActionId::Uninstall:
	case ActionId::OpenSettings:     // アンインストールの結果をもう一度見るため
	case ActionId::OpenParticipants: // 開くだけでは何も書かない
	case ActionId::ResetStallTally:  // メモリの累計を戻すだけで、何も書かない
		return false;
	case ActionId::ToggleRecording:
		return args.on; // 始める向きだけ押せなくする
	case ActionId::SetFocus:
		return !args.user_id.empty(); // 全画面にする向きだけ押せなくする
	case ActionId::WriteVencordDir:
	case ActionId::RepairVencordDir:
	case ActionId::RemoveFromVesktop: // アンインストールが Vesktop の設定を戻すので、同じものを書かない
	case ActionId::ReinstallVencordFiles:
	case ActionId::SetAlwaysOn:
	case ActionId::SetSlot:
	case ActionId::SetVoiceSource:
	case ActionId::SetNameOverride:
	case ActionId::ToggleFeature:
	case ActionId::ToggleDummy:
	case ActionId::SetRosterShown:
	case ActionId::SetVoiceMute:
	case ActionId::SetScreenAudioMute:
	case ActionId::SetVoiceVolume:
	case ActionId::SetScreenAudioVolume:
	case ActionId::SetSelfFrame:
	case ActionId::MoveToMainCall:
	case ActionId::LeaveCall:
	case ActionId::SetMainAccount:
	case ActionId::ReplaceSlot:
		return true;
	}
	return true;
}

const ActionRule &action_rule(ActionId id)
{
	for (const ActionRule &rule : kActionRules) {
		if (rule.id == id)
			return rule;
	}
	// 表に行がない ActionId はない (テストが kActionIdCount と比べて確認する)。万一見つからないときは、
	// ExportBundle の行を使う。何も壊さず、いつでも押せる操作だからである。
	for (const ActionRule &rule : kActionRules) {
		if (rule.id == ActionId::ExportBundle)
			return rule;
	}
	return kActionRules[0];
}

} // namespace

Intent apply(const Observation &obs, ActionId id, const ActionArgs &args)
{
	const ActionRule &rule = action_rule(id);

	Intent intent;
	intent.kind = rule.kind;
	intent.args = args;

	// アンインストールを始めた後は、行ごとの条件より先に押せなくする (上の blocked_after_uninstall)。
	// 始めたかどうかは、primary の表の Uninstalled の行と同じく、observation.h の uninstall_started() で決める。
	if (uninstall_started(obs) && blocked_after_uninstall(rule.id, args)) {
		intent.disabled = Disabled{text::keys::kSettingsUninstallStartedText};
		return intent;
	}

	const char *reason = rule.disabled_reason != nullptr ? rule.disabled_reason(obs, args) : nullptr;
	if (reason != nullptr)
		intent.disabled = Disabled{reason};
	return intent;
}

IntentKind action_intent_kind(ActionId id)
{
	return action_rule(id).kind;
}

std::size_t action_rule_count()
{
	return kActionRuleCount;
}

ActionId action_rule_id_at(std::size_t index)
{
	return kActionRules[index < kActionRuleCount ? index : kActionRuleCount - 1].id;
}

} // namespace cordmux::ledger

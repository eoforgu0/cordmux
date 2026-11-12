// SPDX-License-Identifier: Apache-2.0

// rules.h の実装。primary と notices の 2 つの表と、値を文字列にする純粋関数を持つ。表の行ごとに、条件と、その
// 条件にした理由をコメントに書く。表が持つのは条件だけで、テキスト・ボタン・メッセージのレベルは rules_dock.cpp の
// decide_dock() が決める。decide_dock() は条件をこの表から読み、同じ条件を 2 か所に持たない。

#include "ledger/rules.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace cordmux::ledger {

namespace {

// 値が分からないときの文字列 (format_fps が返す)。語ではないので ini を通さない。
constexpr const char *kDash = "-";

std::string two_digits(int value)
{
	const std::string s = std::to_string(value);
	return s.size() < 2 ? "0" + s : s;
}

// ---- primary の表。並びが、そのまま優先順位である ----

struct PrimaryRule {
	PrimaryRuleId id;
	bool (*matches)(const Observation &);
};

const PrimaryRule kPrimaryRules[] = {
	// アンインストールを始めた後。表の先頭に置く。アンインストールは vencordDir を元の値に戻し、枠も消すので、
	// 始めた後の Observation は VencordDirElsewhere か VencordDirNotConfigured の条件を満たす。この行がないと、
	// アンインストールしたばかりのものを、もう一度インストールするように画面が勧めてしまう。始めた後に利用者が
	// することは OBS の外にしかないので、最も優先する行にする。
	{PrimaryRuleId::Uninstalled, [](const Observation &obs) { return uninstall_started(obs); }},

	// 機能を OFF にしている。利用者が選んだ状態なので、異常として扱わない。
	{PrimaryRuleId::FeatureOff, [](const Observation &obs) { return obs.choices.feature == FeatureSwitch::Off; }},

	// 別の OBS が cordmux を使っている。
	{PrimaryRuleId::OtherObsHolding,
	 [](const Observation &obs) { return obs.listener.state == ListenerState::BlockedByOtherWriter; }},

	// ポートを 1 つずつずらして 16 回試しても、待ち受けを始められなかった。待ち受けていないことを必ず表示する。
	{PrimaryRuleId::ListenPortsExhausted,
	 [](const Observation &obs) { return obs.listener.state == ListenerState::PortsExhausted; }},

	// Vesktop と接続している。インストールと Vesktop の状態の行より上に置く。接続しているという事実は、
	// インストールの途中の状態のどれよりも新しく、利用者が次にすることを決める前提になるためである。ただし
	// OtherObsHolding と ListenPortsExhausted よりは下に置く。この 2 つは待ち受けそのものを始められていない行で、
	// 同時に成り立てば、接続していることより先に伝える必要がある。接続している間のインストールの食い違いは、
	// notices の InstallMismatch が伝える。
	{PrimaryRuleId::Connected, [](const Observation &obs) { return obs.link.state == LinkState::Connected; }},

	// 待ち受けは始めたが、接続のための情報のファイル (session.json) を保存できなかった。Vesktop はこのファイルを
	// 読めないので接続してこず、待っても接続しない。この位置に置く理由は次のとおりである。
	//   - Connected の下: 万一接続していれば、「Vesktopから接続できません」は事実と違う。
	//   - VencordOverwritten から VesktopRunningNotLinked までの上: どれも Vesktop の側で修正する行で、このファイルが
	//     ない間は、どれを修正しても接続できない。古い session.json が残っていれば、Vesktop は古いトークンで接続して
	//     切られ、ConnectedThenDropped の「Vesktopとの接続が切れました」が先に成り立つ (原因と違うテキストに
	//     なる)。VesktopRunningNotLinked の「Vesktopは起動していますが、接続されていません」も同じ原因で成り立つ
	//     ので、primary の表のこの位置で 1 件にまとめる。
	// notices の表に置かないのは、VesktopNotRunning や VesktopRunningNotLinked の 1 行目、ConnectedThenDropped の
	// メッセージのレベル 1 のメッセージと並び、修正方の違う 2 件が同じ原因を言うことになるためである。
	{PrimaryRuleId::SessionFileNotSaved,
	 [](const Observation &obs) {
		 return obs.listener.state == ListenerState::Listening && obs.listener.session_file_not_saved;
	 }},

	// 同梱の Vencord のファイルが配布時と違う (Vencord の更新で上書きされたときなど)。RestartVesktop と
	// ConnectedThenDropped より先に置く。RestartVesktop を先にすると、ファイルが上書きされたままでも Vesktop の
	// 再起動を勧めることになり、再起動しても直らない。[再インストール] を押すと、cordmux が Vesktop の側を書いた
	// 時刻 (Install::discord_side_written_at_us) が進むので、押した後は RestartVesktop の行に移る。
	{PrimaryRuleId::VencordOverwritten,
	 [](const Observation &obs) { return obs.install.dir == VencordDirState::HashMismatch; }},

	// Vesktop の設定が、同梱の Vencord ではない場所を指している。
	{PrimaryRuleId::VencordDirElsewhere,
	 [](const Observation &obs) { return obs.install.dir == VencordDirState::PointsElsewhere; }},

	// Vesktop の設定がまだ済んでいない (state.json に vencordDir のキーがない)。この行がないと、インストールした
	// 日に次の操作が分からなくなる。キーがない利用者は VencordOverwritten と VencordDirElsewhere の条件を満たさず、
	// Vesktop が起動していれば VesktopRunningNotLinked になって、次の操作が画面のどこにも表示されない。
	// この 2 つの隣に置くのは、どれも Vesktop の設定がまだ整っていない行だからである。
	{PrimaryRuleId::VencordDirNotConfigured,
	 [](const Observation &obs) { return obs.install.dir == VencordDirState::NotConfigured; }},

	// cordmux が Vesktop の側 (state.json の vencordDir か、同梱の Vencord のファイル) を最後に書いた時刻が、
	// Vesktop を起動した時刻より後である (observation.h の restart_needed())。
	// state.json の vencordDir は、Vesktop が起動していない間だけ書く ([Vesktopに追加] は Vesktop の起動中は押せず、
	// 書く直前にも確認する。起動時の書き直しも、起動中は Vesktop の終了を待つ。actions.cpp の vesktop_busy_reason)。
	// そのため vencordDir を書いた後は、Vesktop は書いた後に起動するので、この行は成り立たない (書いた後は
	// VesktopNotRunning の「Vesktopが起動していません」になり、Vesktop を起動すれば接続する)。この行が成り立つのは、
	// Vesktop の起動中に [再インストール] で同梱の Vencord のファイルをコピーし直したときである (コピーした時刻に
	// Install::discord_side_written_at_us を進める。ファイルは Vesktop が起動したときに読むので、再起動が要る)。
	// 書く直前の確認と書き込みの間に Vesktop が起動したとき (ごく短い間) にも成り立つ。そのときの Vesktop は書く前の
	// 値をメモリに持つので、終了して起動し直すのが正しい次の操作であり、同じ文が当てはまる。
	{PrimaryRuleId::RestartVesktop, [](const Observation &obs) { return restart_needed(obs); }},

	// 接続した後に切れた。Vesktop が起動していないと分かっているときは、配信中で枠に人が載っているときだけ
	// 成り立つ。再接続するのは Vesktop の側なので、終了した Vesktop について「自動で再接続します」と表示すると、
	// 起きないことを伝えることになる。また、普通の終わり方 (Vesktop を先に閉じる) でも毎回メッセージのレベル 1 の
	// メッセージを表示することになり、利用者がそれに慣れてしまう。配信していないときと、枠に人が載っていないときは、
	// VesktopNotRunning の平常の「Vesktopが起動していません」にする (何も損なわれておらず、Vesktop を起動すれば
	// 進む)。配信中で枠に人が載っている間は、出力の枠が空くか止まるので、この行のまま、本文を
	// 「Vesktopが起動していないため...」にする (rules_dock.cpp)。Vesktop の状態が Unknown か Running のときは、
	// 接続した後に切れていれば成り立つ。
	// Vesktop が起動していないときに常に VesktopNotRunning にしないのは、配信中に Vesktop がクラッシュしたときに、
	// 配信の枠が空いたことをメッセージのレベル 1 で伝えられなくなるためである。
	{PrimaryRuleId::ConnectedThenDropped,
	 [](const Observation &obs) {
		 if (obs.link.state != LinkState::Disconnected || !obs.link.connected_at_us.has_value())
			 return false;
		 if (obs.install.process != VesktopProcess::NotRunning)
			 return true;
		 return streaming_with_slotted_person(obs);
	 }},

	// Vesktop が起動しているかを、まだ確認していない。起動した直後に普通に起きる状態なので、異常として扱わない。
	{PrimaryRuleId::VesktopChecking,
	 [](const Observation &obs) { return obs.install.process == VesktopProcess::Unknown; }},

	// Vesktop が起動していない。
	{PrimaryRuleId::VesktopNotRunning,
	 [](const Observation &obs) { return obs.install.process == VesktopProcess::NotRunning; }},

	// Vesktop は起動しているが、接続してこない。VesktopProcess の 3 つの値は、VesktopChecking・VesktopNotRunning・
	// この行のどれか 1 つに必ず当てはまる。
	{PrimaryRuleId::VesktopRunningNotLinked,
	 [](const Observation &obs) { return obs.install.process == VesktopProcess::Running; }},
};

constexpr std::size_t kPrimaryRuleCount = sizeof(kPrimaryRules) / sizeof(kPrimaryRules[0]);

const PrimaryRule &primary_rule(PrimaryRuleId id)
{
	for (const PrimaryRule &rule : kPrimaryRules) {
		if (rule.id == id)
			return rule;
	}
	// PrimaryRuleId の値は、どれも表にある。列挙子を追加して表に追加し忘れたときは、VesktopChecking の行を返す
	// (primary_rule_of() と同じ扱いで、primary がない状態にしない)。
	for (const PrimaryRule &rule : kPrimaryRules) {
		if (rule.id == PrimaryRuleId::VesktopChecking)
			return rule;
	}
	return kPrimaryRules[0];
}

// ---- notices の表。条件が成り立った行をすべてメッセージにする ----
//
// 機能を OFF にしている間に伝えない行は、利用者が止めているものに次の操作を勧める行 (CompositeAbsent・
// InstallMismatch) だけである。

// 接続している間も伝えるインストールの行 (primary の表の VencordOverwritten・VencordDirElsewhere・
// VencordDirNotConfigured)。並びは primary の表と同じで、最初に条件が成り立った 1 つを伝える
// (rules_dock.cpp の InstallMismatch)。RestartVesktop は入れない。既に接続している画面で、接続するための
// Vesktop の再起動を勧めることになるためである。
constexpr PrimaryRuleId kInstallRuleIds[] = {PrimaryRuleId::VencordOverwritten, PrimaryRuleId::VencordDirElsewhere,
					     PrimaryRuleId::VencordDirNotConfigured};

// 録画のためだけに受け取っている人の数。接続していて録画が ON のときに、枠に載っていない人のうち、録画の入力に
// フレームが今届いている人を数える。last_record_frame_at_us を見るのは、last_video_frame_at_us が枠への経路の
// あるストリームでしか書かれないためである。参加者ごとに数えるので、枠に載っている人がいても 0 にならない (4 人の
// うち 1 人だけ枠に載せたときに、残りの 3 人を数え落とさない)。見出しの「受け取っています」は現在のことを言う文
// なので、古い時刻は数えない。しきい値は、映像が止まったとみなすしきい値と同じ値を使う。全画面にしている人は、枠に載って
// いなくても配信に映っているので数えない (recording_only_person)。
int recording_only_person_count(const Observation &obs)
{
	if (obs.choices.recording != FeatureSwitch::On || obs.link.state != LinkState::Connected)
		return 0;
	int receiving = 0;
	for (const PersonFact &person : obs.people) {
		if (recording_only_person(obs, person))
			++receiving;
	}
	return receiving;
}

// 録画の状態が送信側に届いているはずの時刻を過ぎたか。録画を ON にした直後と接続した直後は、まだ届いていないのが
// 普通なので、2 つの時刻の遅いほうから待ち時間 (RecordingFacts::sender_grace_us) を数える。
bool sender_recording_grace_passed(const Observation &obs)
{
	if (!obs.recording.enabled_since_us.has_value())
		return false; // いつ ON にしたか分からない間は、届いていないと断定しない
	std::int64_t since = *obs.recording.enabled_since_us;
	if (obs.link.connected_at_us.has_value() && *obs.link.connected_at_us > since)
		since = *obs.link.connected_at_us;
	return obs.now_us - since >= obs.recording.sender_grace_us;
}

struct NoticeRule {
	NoticeRuleId id;
	bool (*fires)(const Observation &);
};

const NoticeRule kNoticeRules[] = {
	// Discord の内部への依存箇所が使えない。どれか 1 つでも Missing か Lost なら成り立つ (見出しの選び方は
	// rules_dock.cpp の contact_notice())。
	{NoticeRuleId::ContactUnavailable,
	 [](const Observation &obs) {
		 for (const ContactFact &contact : obs.contacts) {
			 if (contact.state == ContactState::Missing || contact.state == ContactState::Lost)
				 return true;
		 }
		 return false;
	 }},

	// このシーンコレクションに cordmux のソースがない。接続しているかを条件にしない。ソースがあるかは OBS の側だけで
	// 決まり、接続する前に置けるほうが、次の操作を早く勧められる。調べ終えていない間 (シーンの読み込みの前と、
	// シーンコレクションの切り替えの途中) は成り立たない。機能を OFF にしているときは伝えない (利用者が止めて
	// いるものに、次の操作を勧めない)。アンインストールを始めた後も伝えない (アンインストールしたばかりの
	// cordmux を置き直させない)。
	{NoticeRuleId::CompositeAbsent,
	 [](const Observation &obs) {
		 return obs.choices.composite_checked && !obs.choices.composite_present &&
			obs.choices.feature != FeatureSwitch::Off && !uninstall_started(obs);
	 }},

	// 枠に載せていない人の映像を、録画のためだけに受け取っている。
	{NoticeRuleId::RecordingOnly, [](const Observation &obs) { return recording_only_person_count(obs) > 0; }},

	// 接続している間のインストールの食い違い。primary の表で Connected がインストールの行より上にあるので、
	// 接続している間は、インストールの行が primary にならない。その食い違いを、ここで伝える (接続する前に
	// 起きた問題も、接続した後の Observation に残っていれば伝える)。接続していないときは、同じ行が primary に
	// なるので伝えない。機能を OFF にしているときと、アンインストールを始めた後も伝えない (CompositeAbsent と
	// 同じ理由)。
	{NoticeRuleId::InstallMismatch,
	 [](const Observation &obs) {
		 if (obs.link.state != LinkState::Connected || obs.choices.feature == FeatureSwitch::Off)
			 return false;
		 if (uninstall_started(obs))
			 return false;
		 for (const PrimaryRuleId id : kInstallRuleIds) {
			 if (primary_rule(id).matches(obs))
				 return true;
		 }
		 return false;
	 }},

	// バージョンの 2 行。文字列が一致するかで比べ、どれかが空 (読めていない) なら成り立たない (読めていないことを
	// 食い違いとして扱わない)。2 行は修正する場所が違う別の事実なので、同時に成り立てば両方を伝える。
	// 1 行目: 同梱の cordmux と、Vesktop の中で動いている cordmux が違う。Vesktop を再起動すると直る。
	{NoticeRuleId::VersionRestartVesktop,
	 [](const Observation &obs) {
		 return !obs.versions.bundled_vencord.empty() && !obs.versions.discord_side.empty() &&
			obs.versions.bundled_vencord != obs.versions.discord_side;
	 }},
	// 2 行目: cordmux.dll と同梱の cordmux が違う。配布用の zip の中身が混ざっていて、Vesktop を再起動しても
	// 直らない。
	{NoticeRuleId::VersionMixedBundle,
	 [](const Observation &obs) {
		 return !obs.versions.plugin.empty() && !obs.versions.bundled_vencord.empty() &&
			obs.versions.plugin != obs.versions.bundled_vencord;
	 }},

	// OBS のアプリケーション音声キャプチャが Vesktop か Discord の音声を配信に出していて、通話の声が二重に入る。
	// cordmux の音が配信に出ないときは伝えない。cordmux のソースが配信の出力に出ていないときと、ミュートか
	// 音量 0% にしているときである。cordmux を止めて、アプリケーション音声キャプチャで通話の声を出している
	// 利用者には、声が二重にならないので、どちらかをミュートする次の操作を勧める理由がない。機能を OFF にしている
	// ときも伝えない。送信側が声を送らず、cordmux は声を鳴らさないためである (pipeline/pipeline.h の decide_media)。
	{NoticeRuleId::DoubleAudio,
	 [](const Observation &obs) {
		 return obs.choices.discord_audio_source == DiscordAudioSource::Found && obs.choices.composite_active &&
			obs.choices.composite_audible && obs.choices.feature != FeatureSwitch::Off;
	 }},

	// メインアカウントが通話に参加していない。条件は次の 4 つである。
	//   - broadcaster_user_id が空でない (空ならメインアカウントだけの構成)。
	//   - 通話の参加者の一覧を読めている (読めていないことを、参加していないと扱わない)。
	//   - Vesktop のアカウントが通話に参加している (参加していない間は、対象の通話がないので断定しない)。
	//   - メインアカウントが通話の参加者の一覧にない。
	// 一覧にいるかは PersonFact::in_roster で見る。通話の参加者が分からない間は枠を空けないので、obs.people に
	// いるかで見ると、枠に載っていたメインアカウントは通話を抜けても残り、この行が成り立たない。
	{NoticeRuleId::BroadcasterNotInCall,
	 [](const Observation &obs) {
		 if (obs.choices.broadcaster_user_id.empty())
			 return false;
		 if (!obs.call.roster_count.has_value())
			 return false;
		 if (obs.call.presence != Presence::InCall)
			 return false;
		 for (const PersonFact &person : obs.people) {
			 if (person.user_id == obs.choices.broadcaster_user_id && person.in_roster)
				 return false;
		 }
		 return true;
	 }},

	// 枠か録画の対象の 2 人以上の映像が届かず、対象の中に映像が届いている人が 1 人もいない。全員が同時に止まったときは、
	// 1 人ずつの回線より、全員に共通する所 (配信者の PC かネットワーク、Discord の側など) に原因があることが考えられるので
	// 伝える。1 人だけ止まったときは、その人の行の状態と OBS の映像で配信者がもう気付いているので、メッセージにしない。
	// 回線の弱い人が途切れるたびに表示が出るのを防ぐためである。条件は、接続している、Vesktop のアカウントが通話に
	// 参加している、映像を受け取ると決めている、に上の人数を加えた 4 つである。映像を受け取ると決めていない間は、
	// 送信側が映像を止めているので、届かないのは当然である。
	{NoticeRuleId::AllVideoStoppedHint,
	 [](const Observation &obs) {
		 if (obs.link.state != LinkState::Connected || obs.call.presence != Presence::InCall)
			 return false;
		 if (!video_expected(obs))
			 return false;
		 const NoVideoTally tally = no_video_tally(obs);
		 return tally.count >= 2 && !tally.others_arriving;
	 }},

	// ---- 録画の行。どれも、機能を OFF にしている間も伝える。録画が進んでいるときか、録画したものが残って
	// いるときにしか成り立たず、利用者が止めているものに操作を勧める行ではないためである。ここで伝えないと、
	// 機能を OFF にしたまま起動したときに、録画したものが完成しない。

	// 保存先の残り容量で録画できる時間が少ない。録画中にしか成り立たない (見ているのは、現在の書き込みの速さのままで
	// あと何秒録画できるか)。まだ確認していない間は成り立たない。しきい値は設定で変えられるように、Observation
	// から受け取る。
	{NoticeRuleId::RecordSpaceLow,
	 [](const Observation &obs) {
		 return obs.choices.recording == FeatureSwitch::On && obs.recording.remaining_seconds.has_value() &&
			*obs.recording.remaining_seconds <= obs.recording.space_warn_seconds;
	 }},

	// 録画ファイルの作成に使う空き容量が足りない見込み。録画中かどうかを条件にしない。足りないと困るのは録画ファイルの作成の最中で、
	// 録画を OFF にした後も成り立つためである。録画ログに書いた実際のバイト数から求めた 2 つの数を比べる。
	{NoticeRuleId::RecordFinalizeShort,
	 [](const Observation &obs) {
		 return obs.recording.finalize_need_bytes.has_value() && obs.recording.free_space_bytes.has_value() &&
			*obs.recording.finalize_need_bytes > *obs.recording.free_space_bytes;
	 }},

	// 録画を作成している。
	{NoticeRuleId::RecordFinalizeRunning,
	 [](const Observation &obs) { return obs.recording.finalize == FinalizeState::Running; }},

	// 録画ファイルの作成が終わっていない録画がある。[作成を再開] の押せる条件も、同じフィールドを見る (actions.cpp)。
	{NoticeRuleId::RecordFinalizePending, [](const Observation &obs) { return obs.recording.pending_present; }},

	// 録画の状態が送信側に届いていない。条件は、録画が ON、接続している、待ち時間を過ぎた、送信側が
	// 「録画している」と返していない、の 4 つである。送信側の値がない (nullopt) ときも、届いていないとみなす。
	// そのキーを知らない古い送信側は、キーごと返さないためである。
	{NoticeRuleId::RecordNotReachedSender,
	 [](const Observation &obs) {
		 if (obs.choices.recording != FeatureSwitch::On || obs.link.state != LinkState::Connected)
			 return false;
		 if (!sender_recording_grace_passed(obs))
			 return false;
		 return !obs.recording.sender_recording.value_or(false);
	 }},

	// 保存先に書き込めなかった。値がない (まだ確認していない) ときは成り立たない。確認していないことを、失敗として
	// 扱わないためである。
	{NoticeRuleId::RecordDirUnwritable,
	 [](const Observation &obs) {
		 return obs.recording.save_dir_writable.has_value() && !*obs.recording.save_dir_writable;
	 }},

	// 空き容量が尽きて録画が止まった。録画が ON かどうかを条件にしない (成り立つのは止まった後だけである)。
	// 次に録画を ON にするまでメッセージ続け、録画が途中で止まったことを伝え続ける。
	{NoticeRuleId::RecordStoppedSpaceExhausted,
	 [](const Observation &obs) { return obs.recording.stopped_by_space_exhausted; }},

	// [再インストール] でコピーできなかったファイルがある。機能を OFF にしている間も成り立つ。押した結果として、
	// コピーする側が一覧を空に戻すまでメッセージ続ける。
	{NoticeRuleId::ReinstallFailed,
	 [](const Observation &obs) { return !obs.install.reinstall_failed_files.empty(); }},

	// 録画ファイルの作成に失敗した。録画の側は、録画ファイルを 1 つでも作れない (FFmpeg が失敗したか、録画ファイルの長さが合わない)
	// と、その録画のセッションを Failed にする (record/record_finalize.cpp)。この行がないと、「作成しています...」
	// が成功したときと同じように消え、失敗したことが分からず、再開する操作も表示されない。
	// 次の 2 つを見て、録画ファイルの作成が成功するまでメッセージ続ける。
	//   - この起動で最後に作成したセッションが失敗した (RecordingFacts::finalize が Failed)。
	//   - 録画ログが失敗のまま残っているセッションがある (RecordingFacts::failed_session_dir。新しいセッションを
	//     始めた後や、OBS を再起動した後)。
	// 押し直してそのセッションを作成している間は、録画の側が failed_session_dir にそのセッションを入れないので、
	// この行は成り立たず、「作成しています」の行が伝える。また失敗すれば、この行に戻る。録画が ON かどうかも、
	// 機能の ON/OFF も条件にしない (ほかの録画の行と同じ)。
	// この起動の最後のセッションだけを見ないのは、次のセッションを始めるか再起動すると「録画ファイルの作成が終わっていない」
	// に変わり、失敗したことが画面から消えるためである。finalize が Running の間も
	// failed_session_dir を見る。finalize は最後に録画ファイルの作成を始めたセッションの値で、別のセッションを作成している間も
	// Running になるので、見ないと、失敗したセッションのメッセージがその間だけ消えて、また表示される。
	{NoticeRuleId::RecordFinalizeFailed,
	 [](const Observation &obs) {
		 return obs.recording.finalize == FinalizeState::Failed || !obs.recording.failed_session_dir.empty();
	 }},
};

constexpr std::size_t kNoticeRuleCount = sizeof(kNoticeRules) / sizeof(kNoticeRules[0]);

} // namespace

bool recording_only_person(const Observation &obs, const PersonFact &person)
{
	if (person.slot.has_value() || !person.last_record_frame_at_us.has_value())
		return false;
	if (!obs.choices.focus_user_id.empty() && person.user_id == obs.choices.focus_user_id)
		return false;
	return obs.now_us - *person.last_record_frame_at_us < obs.video_stall_threshold_us;
}

bool video_expected(const Observation &obs)
{
	if (obs.choices.feature == FeatureSwitch::Off)
		return false;
	if (obs.choices.recording == FeatureSwitch::On)
		return true;
	return !(obs.choices.composite_checked && !obs.choices.composite_present);
}

std::int64_t video_wait_from(const Observation &obs, std::int64_t since_us)
{
	if (obs.video_expected_since_us.has_value() && *obs.video_expected_since_us > since_us)
		return *obs.video_expected_since_us;
	return since_us;
}

// その人の映像の、最後のフレームの時刻。枠に載っている人は枠への経路のあるストリームの時刻 (last_video_frame_at_us)、
// 載っていない人は録画の入力の時刻 (last_record_frame_at_us) を使う。
std::optional<std::int64_t> video_time(const PersonFact &person)
{
	return person.slot.has_value() ? person.last_video_frame_at_us : person.last_record_frame_at_us;
}

// 待ち始めの時刻 (since_us) を、その人を視聴の範囲に入れた時刻 (PersonFact::watching_since_us) より前にしない。
// 受け取る範囲が「枠に載せた人だけ」(ReceiveScope::Slots) のときだけ使う。「画面共有している全員」の間は、送信側が
// 最初から視聴しているので、視聴の範囲に入れた時刻を持たない。
static std::int64_t watch_wait_from(const Observation &obs, const PersonFact &person, std::int64_t since_us)
{
	if (obs.choices.receive_scope == ReceiveScope::Slots && person.watching_since_us.has_value() &&
	    *person.watching_since_us > since_us)
		return *person.watching_since_us;
	return since_us;
}

// 届いていた映像が、しきい値 (video_stall_threshold_us) を超えても届かない。1 度も届いていない人は含めない。
// 届いたことのない映像を、止まったと断定しないためである。待ち時間は、映像を受け取ると決めている状態に戻った時刻より前から
// 数えない (video_wait_from())。映像を受け取ると決めていない間かどうかは、呼ぶ側が video_expected() で見る。
bool stalled_video(const Observation &obs, const PersonFact &person)
{
	const std::optional<std::int64_t> at = video_time(person);
	if (!at.has_value())
		return false;
	// 視聴の範囲に入れ直した人 (チェックを外して付け直した人) の最後のフレームは、外す前のものである。視聴を
	// 始める前のフレームから数えると、付けた瞬間に止まった人になるので、視聴を始めた時刻より前から数えない。
	return obs.now_us - watch_wait_from(obs, person, video_wait_from(obs, *at)) >= obs.video_stall_threshold_us;
}

// 映像の受信を始めてから first_frame_wait_threshold_us がたっても、最初のフレームが 1 枚も届かない。受信を始めた時刻は、
// 画面共有していると分かった時刻と、視聴の範囲に入れた時刻 (watch_wait_from。「枠に載せた人だけ」のときだけ) の遅いほう
// である。ずっと前から共有している人のチェックを付けた瞬間に、待ち時間を超えたことにしないためである。
// 画面共有していると分かっている人 (sharing_known) だけを見る。stalled_video() は 1 度も届いていない人を数えない
// ので、その人をここで数える。
bool first_frame_missing(const Observation &obs, const PersonFact &person)
{
	if (!person.sharing_known || !person.sharing || !person.sharing_since_us.has_value())
		return false;
	if (video_time(person).has_value())
		return false;
	const std::int64_t from = watch_wait_from(obs, person, video_wait_from(obs, *person.sharing_since_us));
	return obs.now_us - from >= obs.first_frame_wait_threshold_us;
}

// cordmux の録画の対象か。cordmux の録画が ON で、画面共有していると分かっている人を対象にする。受信側は録画の間、
// 画面共有している全員の映像を録画の入力に通す (枠に載っていない人も録画する)。ただし、受け取る範囲が「枠に載せた人
// だけ」(ReceiveScope::Slots) のときは、枠に載っている人だけを対象にする (送信側が枠に載っていない人の映像を視聴
// せず、録画の入力も枠に載っていない人の画面を通さない。pipeline/receive_scope.h)。
// 録画の入力の時刻 (last_record_frame_at_us) がある人だけにしないのは、1 度もフレームが届いていない人が対象から
// 外れ、行の状態が出なくなるためである。通話の全員にしないのは、画面共有していない人には
// 録画するものがなく、対象にすると「届いていない」が常に成り立つためである。
// 録画の入力が画面共有している全員を通すことは pipeline/ のコメントに書かれているが、実際の通話で 5 人以上の
// ときは確認していない。
bool record_target(const Observation &obs, const PersonFact &person)
{
	if (obs.choices.receive_scope == ReceiveScope::Slots && !person.slot.has_value())
		return false;
	return obs.choices.recording == FeatureSwitch::On && person.sharing_known && person.sharing;
}

// エンコードの段階 (PersonFact::stage) が unavailable か。unavailable は、送信側がその人の映像をエンコードできず、
// 送っていない状態である (docs/wire_format.md の「facts」)。段階がない人と、知らない名前の段階の人は偽にする
// (下げた映像か普通の映像が映っている見込みのため)。displayed()・resolution_of()・映像が届かないメッセージの次の操作
// が、同じこの判定を使う。
bool stage_unavailable(const PersonFact &person)
{
	return person.stage.has_value() && *person.stage == kStageUnavailable;
}

NoVideoTally no_video_tally(const Observation &obs)
{
	NoVideoTally tally;
	for (const PersonFact &person : obs.people) {
		const bool recorded = record_target(obs, person);
		if (!person.slot.has_value() && !recorded)
			continue; // どちらの対象でもない人は数えない (その人の行の状態も表示しない)
		// 届いていた映像が止まった人。画面共有している人だけを数える (画面共有をやめた人の枠と、最後のフレームの
		// 時刻は、しばらく残るので、見ないと、誰かが画面共有をやめるたびに数える)。
		const bool stalled = person.sharing && stalled_video(obs, person);
		// 映像が今届いている人。しきい値の時間内にフレームがある (stalled_video() と同じ時刻と同じしきい値)。
		// 段階が unavailable の人は数えない。送信側が読み出しを止めた後に、しきい値の時間内に残っているフレームは、
		// 届いているのではない。枠に載せていない人の映像は、録画していなければ録画の入力を通らず、届いているかが
		// 分からないので、対象の人だけを見る。
		if (!stalled && !stage_unavailable(person)) {
			const std::optional<std::int64_t> at = video_time(person);
			if (at.has_value() && obs.now_us - *at < obs.video_stall_threshold_us)
				tally.others_arriving = true;
		}
		if (!stalled && !first_frame_missing(obs, person))
			continue;
		++tally.count;
		// 2 つの条件は、1 人の中で同時に成り立たない (stalled_video() は video_time() がある人だけ、
		// first_frame_missing() はない人だけを見る)。
		if (person.slot.has_value()) {
			if (stalled)
				tally.any_slotted_stalled = true;
			else
				tally.any_slotted_waiting = true;
		}
		if (recorded)
			tally.any_recorded = true;
		if (stage_unavailable(person))
			tally.any_cant_show = true;
		else
			tally.any_other = true;
	}
	return tally;
}

std::string format_clock_hms(std::int64_t epoch_us, int utc_offset_sec)
{
	constexpr std::int64_t kUsPerSec = 1000000;
	constexpr std::int64_t kSecPerDay = 86400;

	// 負の時刻も秒の単位に切り下げる。整数の除算は 0 の方向に丸めるので、余りが負なら 1 秒引く。
	std::int64_t seconds = epoch_us / kUsPerSec;
	if (epoch_us % kUsPerSec < 0)
		--seconds;
	seconds += utc_offset_sec;

	std::int64_t sec_of_day = seconds % kSecPerDay;
	if (sec_of_day < 0)
		sec_of_day += kSecPerDay;

	const int hours = static_cast<int>(sec_of_day / 3600);
	const int minutes = static_cast<int>((sec_of_day % 3600) / 60);
	const int secs = static_cast<int>(sec_of_day % 60);
	return two_digits(hours) + ":" + two_digits(minutes) + ":" + two_digits(secs);
}

DateTimeParts format_date_time_parts(std::int64_t epoch_us, int utc_offset_sec)
{
	constexpr std::int64_t kUsPerSec = 1000000;
	constexpr std::int64_t kSecPerDay = 86400;

	// 秒の単位への切り下げ方は format_clock_hms と同じ (整数の除算は 0 の方向に丸めるので、
	// 余りが負なら 1 秒引く)。
	std::int64_t seconds = epoch_us / kUsPerSec;
	if (epoch_us % kUsPerSec < 0)
		--seconds;
	seconds += utc_offset_sec;

	std::int64_t days = seconds / kSecPerDay;
	std::int64_t sec_of_day = seconds % kSecPerDay;
	if (sec_of_day < 0) {
		sec_of_day += kSecPerDay;
		--days;
	}

	// 日数から年月日を求める (Howard Hinnant の civil_from_days)。うるう年の規則を整数の演算だけで表したもので、
	// 表も <ctime> も使わないので、OS の時差の設定に依存しない。3 月を年の初めとして数えるのが要点で、うるう日が
	// 年の最後に来るので、月ごとの場合分けがなくなる。
	const std::int64_t z = days + 719468;                        // 0000-03-01 を原点にする
	const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097; // 400 年を 1 周期として数える
	const std::int64_t doe = z - era * 146097;                   // 周期の中の日 [0, 146096]
	const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // 周期の中の年 [0, 399]
	const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // その年の中の日 [0, 365]
	const std::int64_t mp = (5 * doy + 2) / 153;                                   // 3 月を 0 とする月 [0, 11]
	const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;                         // [1, 31]
	const std::int64_t month = mp + (mp < 10 ? 3 : -9);                            // [1, 12]
	const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);

	DateTimeParts parts;
	parts.year = std::to_string(year);
	parts.month = two_digits(static_cast<int>(month));
	parts.day = two_digits(static_cast<int>(day));
	parts.hour = two_digits(static_cast<int>(sec_of_day / 3600));
	parts.minute = two_digits(static_cast<int>((sec_of_day % 3600) / 60));
	return parts;
}

std::string format_fps(double fps)
{
	// 0 以下と NaN は、分からない値として扱う (fps が届いていないときと同じ)。
	if (!(fps > 0.0))
		return kDash;
	// 正なのに 0 に丸まる値も「0」にしない。画面の「0 fps」は「フレームが 1 枚も届いていない」と読めるが、
	// ここに来た時点でフレームの間隔は分かっていて、1 枚も届いていないわけではない。どちらとも断定できない
	// ので、分からない値として扱う。
	if (fps < 0.05)
		return kDash;
	const double rounded = std::floor(fps + 0.5);
	if (std::fabs(fps - rounded) < 0.05)
		return std::to_string(static_cast<int>(rounded));
	const std::int64_t tenths = static_cast<std::int64_t>(std::floor(fps * 10.0 + 0.5));
	return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

std::size_t primary_rule_count()
{
	return kPrimaryRuleCount;
}

PrimaryRuleId primary_rule_id_at(std::size_t index)
{
	return kPrimaryRules[index < kPrimaryRuleCount ? index : kPrimaryRuleCount - 1].id;
}

bool primary_rule_matches(PrimaryRuleId id, const Observation &obs)
{
	return primary_rule(id).matches(obs);
}

PrimaryRuleId primary_rule_of(const Observation &obs)
{
	for (const PrimaryRule &rule : kPrimaryRules) {
		if (rule.matches(obs))
			return rule.id;
	}
	// VesktopChecking・VesktopNotRunning・VesktopRunningNotLinked の 3 行が VesktopProcess のすべての値を受ける
	// ので、ここには来ない。VesktopProcess に値を追加したときも、primary がちょうど 1 つになるようにする。
	return PrimaryRuleId::VesktopChecking;
}

std::size_t notice_rule_count()
{
	return kNoticeRuleCount;
}

NoticeRuleId notice_rule_id_at(std::size_t index)
{
	return kNoticeRules[index < kNoticeRuleCount ? index : kNoticeRuleCount - 1].id;
}

bool notice_rule_fires(NoticeRuleId id, const Observation &obs)
{
	for (const NoticeRule &rule : kNoticeRules) {
		if (rule.id == id)
			return rule.fires(obs);
	}
	return false;
}

} // namespace cordmux::ledger

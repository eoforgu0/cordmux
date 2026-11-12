// SPDX-License-Identifier: Apache-2.0

// ポーリングのたびに読んだ値から、ledger::Observation を 1 つ作る純粋関数。Pipeline::snapshot()、OsProbe の結果、
// DockPoll が持つ値 (DockFacts) から作り、ledger::decide_dock に渡す。Qt を含む dock_poll.h と分けてあるのは、
// 表示の判断の入力を作る処理も、Qt を使わない単体テストで確認するためである (dock/dock.cmake の
// cordmux_add_dock_observation で align_tests に入る)。libobs にも Qt にも依存しない。時計もファイルも OS も読まず、
// 現在の時刻、時差、利用者が選んだ状態は、すべて DockFacts で受け取る。ほかのプロセスの起動時刻だけは時刻の基準を
// 変換するが (platform::os_process_time_to_epoch_us)、引数だけで決まる計算なので、同じ入力には同じ Observation を返す。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ledger/observation.h"
#include "pipeline/pipeline.h"
#include "platform/os_probe.h"
#include "sources/composite_roster.h"

namespace cordmux::dock {

// Pipeline にも OsProbe の結果にもない、DockPoll が持っている値。どれも、読んで分かった値だけを入れる。画面に表示する
// テキストは、ここで作らない。
struct DockFacts {
	// 現在の時刻 (Unix エポックからのマイクロ秒) と、UTC からの時差 (秒)。
	std::int64_t now_us = 0;
	int utc_offset_sec = 0;

	// フレームが届かない時間がこの長さを超えると、映像が止まったと判断する (settings の video_stall_ms)。
	std::int64_t video_stall_threshold_us = ledger::kDefaultVideoStallUs;

	// 利用者が自分で選んだ状態 (Observation の choices)。ダミーは sources のフラグから取る。
	//
	// 録画のオンとオフは、ここにない。settings にも sources のフラグにもなく、Pipeline のメモリだけが持つ
	// (起動するたびにオフに戻る)。ドックがコピーを持つと、押した直後の 1 回のポーリングの間だけ食い違う値が
	// 増えるので、build_observation が snapshot から直接読む。
	ledger::FeatureSwitch feature = ledger::FeatureSwitch::On;
	ledger::FeatureSwitch dummy = ledger::FeatureSwitch::Unknown;

	// 自分の枠が、合成の配置に入っているか。合成のソースの有無を調べ終えた状態のときだけ判断し、それ以外は
	// false のままにする (observation.h の Choices::self_frame_shown。分からないという 3 つ目の値は作らない)。
	bool self_frame_shown = false;
	// 自分の枠に映す映像を OBS から選んでいるか、選んだ映像が合成のソースに結び付いているか、自分の枠を出す状態か、
	// 自分の Discord の共有の現在の値 (observation.h の Choices の同じ名前のフィールド)。self_frame_shown は bound と on がどちらも true のときだけ true。
	bool self_frame_selected = false;
	bool self_frame_bound = false;
	bool self_frame_on = false;
	ledger::SelfShareState self_frame_share = ledger::SelfShareState::Unknown;

	// メインアカウントの userId (settings の broadcaster_user_id)。空なら、メインアカウントだけの構成である。
	std::string broadcaster_user_id;

	// 枠の並び順 (settings の slot_order)。
	config::SlotOrder slot_order = config::SlotOrder::Name;
	// 自分の枠を先頭に置くか (settings の self_slot_first) と、自分の枠を載せた順の印 (DockPoll が持つ。observation.h の
	// Choices の同じ名前のフィールド)。
	bool self_slot_first = true;
	std::int64_t self_frame_added_mark = 0;

	// 全画面で表示している人、その人の配信に出している名前、全画面にできる人 (ledger/observation.h の Choices の同じ名前の
	// フィールド)。DockPoll が、自分が持つ全画面の人と、合成のソースに最後に渡した配置の入力から入れる。
	std::string focus_user_id;
	std::string focus_name;
	std::vector<std::string> focus_candidates;

	// 接続が切れる前に分かっていた参加者 (DockPoll が、接続している間の Observation::people を覚えておく)。接続していない間だけ
	// Observation::last_known_people にコピーする。
	std::vector<ledger::PersonFact> last_known_people;

	// 接続が切れる前に分かっていた Vesktop のアカウントの userId と名前 (DockPoll が、接続している間に覚える)。
	// Observation::vesktop_user_id と vesktop_name が分からない間だけ使う。
	std::string last_known_vesktop_user_id;
	std::string last_known_vesktop_name;

	// 画面共有していない参加者を名前ラベルに出す設定 (settings の roster_show と roster_show_bots。
	// ledger/observation.h の Choices の同じ名前のフィールド)。
	bool roster_show = true;
	bool roster_show_bots = true;

	// 載せる人の選び方 (settings の assign_mode。既定は手動)。
	ledger::AssignMode assign_mode = ledger::AssignMode::Manual;

	// 映像を受信する人 (settings の receive_scope。既定は枠に載せた人だけ)。
	ledger::ReceiveScope receive_scope = ledger::ReceiveScope::Slots;

	// 合成のソースがあるシーンコレクションか (sources::composite_present。ledger/observation.h の
	// Choices::composite_present)。参加者ごとの音量とミュートは、合成のソースが出す混ぜた音にだけ適用される。
	bool composite_present = false;
	// composite_present を調べ終えているか (ledger/observation.h の Choices::composite_checked)。シーンの読み込みが
	// 終わるまでと、シーンコレクションを切り替えている途中は false にする。読み込みの前に調べると、cordmux を
	// まだ置いていないように見え、起動直後の普通の状態を問題として表示することになる。
	bool composite_checked = false;

	// これまでに見た人の一覧。DockPoll が seen_people.json から読み、ポーリングのたびに、今見えている人を
	// 反映したもの。並べ方も絞り込みも ledger::decide_dock が決めるので、持っているものをそのまま渡す。
	ledger::Seen seen;

	// OBS のアプリケーション音声キャプチャが Vesktop か Discord の音声を配信に出しているかと、cordmux の音が
	// 配信に出る状態か (ledger/observation.h の同じ名前のフィールド)。DockPoll が sources::read_double_audio_state
	// で読む。composite_checked と同じく、シーンの読み込みが終わるまでと、シーンコレクションを切り替えている途中は
	// 読まず、Unknown と false のままにする。
	ledger::DiscordAudioSource discord_audio_source = ledger::DiscordAudioSource::Unknown;
	// Found のときの、そのソースの OBS の中の名前 (ledger/observation.h の同じ名前のフィールド)。Found なら必ず
	// 入れる。
	std::string discord_audio_source_name;
	bool composite_active = false;
	bool composite_audible = false;

	// バージョン (どちらが古いかは、画面では断定しない)。plugin_version には、ws::ListenerConfig::server_version と
	// 同じ文字列を渡す。送信側に hello_ok で伝えるバージョンと、画面に表示するバージョンが
	// 食い違わないようにするためである。送信側のバージョンと Vesktop のバージョンは、送信側が hello で
	// 伝えるもので、PipelineSnapshot::listener の peer_version と peer_vesktop_version からコピーするので、
	// ここにはない。
	std::string plugin_version;
	std::string bundled_vencord_version;

	// 設定ウィンドウの「バージョン情報」のページに表示する、OBS と OS のバージョンと GPU の名前。読むのは
	// dock_poll.cpp で、ここでは受け取るだけである (このファイルは libobs にも OS にも触らない)。読めなければ
	// 空のまま。
	std::string obs_version;
	std::string os_version;
	std::string gpu_name;

	// 同梱した Vencord にある cordmux-build.txt の vencord_commit (表示にだけ使う。ledger::Versions の同じ名前の
	// フィールド)。build_id は上の bundled_vencord_version に入れる。cordmux-build.txt を読めなければ、どちらも空。
	std::string bundled_vencord_commit;
	// 同梱した Vencord にある cordmux-build.txt の vencord_tag (表示にだけ使う。ledger::Versions の同じ名前の
	// フィールド)。読めないときと、前のバージョンの cordmux-build.txt で tag の行がないときは空。
	std::string bundled_vencord_tag;

	// この起動で、vencordDir を書き直したか。書き直すのは obs_module_load で、ドックはその結果を
	// コピーするだけである。
	bool rewrote_at_startup = false;

	// [再インストール] でコピーできなかったファイル (空なら、再インストールしていないか、すべてコピーできた)。
	std::vector<std::string> reinstall_failed_files;

	// data/vencord に前のバージョンの cordmux のファイルがあり、DockPoll がメッセージを表示せずに data/vencord-src から
	// コピーし直した直後か。true の間は、コピーする前の照合の結果 (ハッシュの不一致) を、上書きされたと表示せず、
	// まだ確認していないものとして扱う (コピーし直した後の照合を待っている)。
	bool hide_hash_mismatch = false;

	// Observation の choices のうち、settings からコピーするフィールド。cordmux の有効と無効は、上の feature。
	std::string record_dir;
	ledger::RecordPcm record_output_pcm = ledger::RecordPcm::Float32;
	int limit_max_long_edge = 0;
	int limit_fps_divisor = 1;

	// アンインストールの状態。DockPoll が持つ進み具合と結果と、配信中、録画中などのフィールド
	// (dock/install_flow.h の uninstall_facts_of で作り、配信中などは DockPoll が OBS から読んで入れる)。
	ledger::UninstallFacts uninstall;

	// この起動では、cordmux が Vesktop の設定 (state.json の vencordDir) を書き込まないか。DockPoll
	// (dock_poll.cpp) が !DockPollConfig::allow_state_json_write を入れる。その値は起動時に決まり、同じ起動の中では
	// 変わらない (settings.json で利用者が false にしたときと、起動時に settings.json を読めなかったので
	// false にしたとき。plugin-main.cpp の pipeline::effective_allow_state_json_write)。ledger::Install の同じ
	// 名前のフィールドにコピーする。
	//
	// 押しても同じ起動の中では同じ失敗になると分かっているボタンは、表示しない。この起動では、[Vesktopに追加] は
	// 押す前から、書き込みを止めてあるという結果で終わることが決まっている (dock_poll_install.cpp の
	// write_vencord_dir)。押した後の失敗のカードでだけボタンを消すと、最初の 1 回は押せて、押すと同じ失敗になる。
	bool vesktop_writes_off = false;

	// Vesktop の設定を書く直前に、DockPoll が自分で Vesktop のプロセスを調べて、起動していた (Running) か、調べられ
	// なかった (Unknown) ときの結果。値があるのは、その後に OsProbe の新しい結果がまだ届いていない間だけで、その間は
	// OsProbe の結果の代わりに、この値を ledger::Install::process に入れる (dock_poll_install.cpp の
	// vesktop_free_to_write)。OsProbe の結果は最大で 30 秒 (接続している間は 5 分) 前のものなので、押せるとした
	// 判断のもとの値が古いまま残ると、書かなかったのにボタンが押せるままになり、押せない理由も出ない。
	std::optional<ledger::VesktopProcess> vesktop_process_seen;
};

// 3 つの入力から Observation を 1 つ作る。
ledger::Observation build_observation(const pipeline::PipelineSnapshot &snapshot,
				      const platform::OsProbeResult &probe, const DockFacts &facts);

// 配信に表示するその人の名前。利用者が変えた名前、届いた表示名 (profiles)、最後に見たときの表示名
// (SeenPerson::display_name) の順に探し、どれもなければ空を返す (空は、名前を取得できていないことを表す。そのとき、
// 画面は userId をそのまま表示する)。
//
// 3 か所がこの関数を使う。参加者の一覧の行 (build_observation)、名前ラベル (sources::set_person_decor)、録画に渡す名前
// (dock_poll.cpp)。名前の決め方を 1 か所に持ち、同じ名前を表示する。
std::string effective_display_name(const std::vector<ledger::SeenPerson> &seen_people,
				   const pipeline::ProfilesSnapshot &profiles, const std::string &user_id);

// 自分の枠に載せる人。サブアカウントを使う構成ではメインアカウント (settings の broadcaster_user_id)、メインアカウント
// だけの構成 (欄が空) では Vesktop のアカウント (facts の self_user_id)。どちらも分からなければ空。名前ラベルの値を
// 渡す所と、合成のソースの配置の入力を作る所が、同じ人を使う。
std::string self_frame_user_id(const config::Settings &settings, const pipeline::PipelineSnapshot &snapshot);
// 上と同じ人を、メインアカウントの userId (settings の broadcaster_user_id) を直接渡して決める。
std::string self_frame_user_id(const std::string &broadcaster_user_id, const pipeline::PipelineSnapshot &snapshot);

// 自分の枠の人の user_id と、配信に出している名前 (effective_display_name)。自分の枠の人 (self_frame_user_id) が分からない
// とき (メインアカウントだけの構成で Vesktop との接続が切れている間) は、最後に分かった Vesktop のアカウントにする。行の枠の
// 位置 (Observation の Choices::self_frame_user_id と self_frame_name。ledger/rules_dock.cpp の slot_positions) と、合成の
// ソースの配置の入力 (CompositeInput::self_user_id と self_name。dock_poll_layout.cpp) が、この同じ関数の値を使う。
// 自分の枠の人が通話にいない (PersonFact がない) ときも、両方が同じ名前で並べる。
struct SelfFrameIdentity {
	std::string user_id;
	std::string name;
};
SelfFrameIdentity self_frame_identity(const std::string &broadcaster_user_id, const pipeline::PipelineSnapshot &snapshot,
				      const std::vector<ledger::SeenPerson> &seen_people,
				      const std::string &last_known_vesktop_user_id);

// 画面共有の人と、画面共有していない参加者が声を聞けるか (sources/composite_layout.h の CompositeShare::hears_voice と
// sources/composite_roster.h の CompositeRosterPerson::hears_voice) は、常に真である。声の枠に上限がなく、話した人は
// 全員が声の経路を持つので、声を聞けない人はいない (自分の枠の人は、枠でも並びでも、下の self_frame_hears_voice で決める)。

// 合成のソースで、cordmux が自分の枠の人の声を聞けるか (sources/composite_input.h の CompositeInput::self_hears_voice)。
// 自分の枠の人の声の枠が、いま声の経路を持っているときだけ真 (pipeline::SlotSnapshot の voice_present)。メインアカウント
// だけの構成では、Discord が本人の声を返さないので経路がなく、偽になる (光らせず、暗くもしない)。設定で自分の枠の
// 音声ソースを選んでいる間は、この値ではなく音声ソースの状態で決める (sources/self_speaking_judge.h の SourceVoice)。
bool self_frame_hears_voice(const std::vector<pipeline::SlotSnapshot> &slots, const std::string &self_user_id);

// 合成のソースの名前ラベルを出す人 (sources/composite_input.h の CompositeInput::roster) を決める。並びは問わない (並べる順は
// 合成のソースが決める)。通話の参加者は、画面共有しているか、メインアカウントかに依らず、全員を名前ラベルを出す人にする
// (配信を見る人が、通話で誰が話しているかを分かるように)。その人を、名前ラベル付きで描いている枠に出すか、画面共有して
// いない参加者の並びに出すかは、合成のソースが描くときに決める (sources/composite_label_plan.h の composite_tiles_labeled と
// composite_roster_shows)。枠に名前ラベルを描くかは、キャンバスの大きさ、映像が届いているか、全画面かで変わり、ドックは
// それを持たないので、枠に名前ラベルを描くかを決めるのと同じ所 (同じ関数) で決める。
//   - 入れる人: facts の roster (通話の参加者) にいる人。
//   - 外す人: 受け取りに使うアカウント (サブアカウントの構成。settings の broadcaster_user_id が空でなく、facts の
//     self_user_id と違うときの、Vesktop のアカウント。メインアカウントだけの構成では Vesktop のアカウントが配信する
//     本人なので外さない)、参加者の設定ウィンドウで「通話のみ時ラベル非表示」を付けた人 (SeenPerson::roster_hidden)、
//     ほかの人の行の「使う声」に選ばれている人、Bot のアカウント (profiles の bot) で settings.roster_show_bots が
//     偽のとき。「使う声」に選ばれている人の声は、選んだ人の声の組 (ミキサーの組も選んだ人。pipeline/composite_audio_plan.h の
//     mixer_key_for) で鳴り、選んだ人の名前ラベル (枠でも並びでも) を光らせる。声の人も出すと、同じ声が 2 か所に出たうえ、
//     声の人の名前ラベルは話しても光らない。外した人も、枠に載っていれば枠の名前ラベルは描く (この関数が決めるのは並びだけである)。
//   - 誰も入れないとき: settings.roster_show が偽、facts の roster をまだ読めていない (分からない間に並べない)。
//   - 名前: effective_display_name (cordmux での名前、なければ Discord の表示名)。名前を取得できない人は入れない
//     (userId を名前の代わりに描かない)。
//   - 声を聞けるか (hears_voice): 自分の枠の人 (self_user_id 引数) は self_frame_hears_voice (自分の枠と同じ規則。メイン
//     アカウントだけの構成では Discord が本人の声を返さないので偽で、[自分の枠の音声ソース] を選んでいなければ並びでも
//     光らない)。ほかの人は真。
// 同じ userId は 1 人だけ入れる。
std::vector<sources::CompositeRosterPerson> composite_roster_people(const pipeline::PipelineSnapshot &snapshot,
								    const config::Settings &settings,
								    const std::vector<ledger::SeenPerson> &seen_people,
								    const std::string &self_user_id);

// 名前ラベルと参加者の並びに渡す、その人のアバターの画像。profiles に画像があれば、それを使う。画像がまだ分からない間は、
// 前に渡した画像 (previous。渡していなければ nullptr) を使う。分からない間とは、profiles が届いていない (Vesktop との
// 接続が切れると Pipeline は profiles を捨て、接続し直してから送り直される)、その人が profiles にいない、アバターの
// バージョン (avatar_key) が読めていないか、バージョンに合う画像がまだ届いていない、のどれかである。前の画像を使わないと、接続が切れたときと接続し直した
// 直後に、描き続けている人 (自分の枠の人、最後のフレームを残している人) のアバターが、いったん頭文字に変わる。
// アバターを設定していない人 (avatar_key が空) は空にする。
std::vector<std::uint8_t> decor_avatar(const pipeline::ProfilesSnapshot &profiles, const std::string &user_id,
				       const std::vector<std::uint8_t> *previous);

// 今読めない、Discord の内部への依存箇所の id の集合 (facts の warnings に入っていた id。
// docs/wire_format.md の「facts」)。空の id は入れない。
//
// 判断をこの関数だけに持つのは、同じ集合を、最後に読めた時刻の更新 (update_contact_success) と、診断情報の
// manifest.json の state の両方が使うためである。別々に作ると、片方だけを修正したときに、state と last_ok_at_us が
// 食い違う。
std::set<std::string> unavailable_contact_ids(const std::vector<ledger::ContactFact> &contacts);

// Discord の内部への依存箇所ごとに、最後に読めた時刻を記録する (診断情報のエクスポートで使う)。
//
// 送信側が warnings に入れるのは読めない箇所だけなので、受信側が知っているのは、1 度 warnings に入った id だけである。
// その id が次の facts の warnings から消えたら、そのときは読めていたので、時刻を記録する。warnings に入ったことのない
// 箇所は、この一覧に出てこない (依存箇所の一覧は src-vencord/ledger.ts にあり、C++ には持たない)。
//
// facts が 1 回も届いていない間は、何も記録しない。読めているとも、読めないとも言えないためである。
void update_contact_success(std::map<std::string, std::int64_t> *last_ok_at_us,
			    const std::vector<ledger::ContactFact> &contacts, bool facts_received,
			    std::int64_t now_us);

} // namespace cordmux::dock

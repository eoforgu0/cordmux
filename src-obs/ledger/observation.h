// SPDX-License-Identifier: Apache-2.0

// 表示の規則の入力 Observation と、それを読む小さな関数。ドックは周期ごと (既定 500 ms) に現在の状態から
// Observation を作り直して decide_dock() (ledger/rules.h) に渡す。差分も履歴も持たないので、問題が直れば次の周期で
// メッセージが消える。時計を読むのはドックで、ここは now_us と utc_offset_sec を受け取るだけである。
// 状態を表す列挙は Unknown を持ち、「cordmux.dll がまだ確認していない」と「ない」を分ける。起動した直後の、まだ
// 確認していない状態を問題として表示しないためである (Unknown を持たない列挙には、その理由をコメントに書いてある)。
// 時刻はエポックからのマイクロ秒で、なければ std::optional が値を持たない。libobs にも Qt にも依存しない。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// 画面共有の枠の数 (config::kScreenSlotCount) を読む。枠の数を決めるのは割り当て表なので、ここで別の定数を
// 持つと、枠を増やしたときに「押せる枠の番号」だけが古いままになる。ledger/actions.h も同じ理由でこのヘッダーを
// 読み込む。
#include "config/allocation.h"
#include "config/person_audio.h"
#include "pipeline/arrival_window.h"

namespace cordmux::ledger {

// ---- Discord 側のインストール (Install) ----
// Vesktop が動いているかと、Vesktop の設定が同梱の Vencord を指しているかは、別々に確認する。

// Vesktop のプロセスが動いているか。OS に問い合わせた結果 (platform/ が答える)。
enum class VesktopProcess {
	Unknown,    // まだ問い合わせていないか、答えが返っていない
	NotRunning, // 動いていない
	Running,    // 動いている
};

// Vesktop の設定 (state.json の vencordDir) と、同梱の Vencord の 4 つのファイルを照合した結果。
enum class VencordDirState {
	Unknown,         // まだ読めていない (最初の照合の前か、読み取りに失敗した)
	// state.json は読めたが、vencordDir のキーがまだない (利用者が Vencord の場所を一度も設定していない。
	// インストールした日の状態)。この状態のときにこそ [Vesktopに追加] を表示するので、Unknown と同じ値にしない。
	NotConfigured,
	Matches,         // 同梱の場所を指し、4 つのファイルの中身も配布したときのハッシュと一致する
	HashMismatch,    // 場所は合っているが中身が違う (Force Update で上書きされたときなど)
	PointsElsewhere, // 別の場所を指している
};

struct Install {
	VesktopProcess process = VesktopProcess::Unknown;
	VencordDirState dir = VencordDirState::Unknown;

	// cordmux が最後に Discord 側を書いた時刻 (バックアップ vencord_dir_backup.json の written_at)。state.json の
	// vencordDir を書いた時刻だけでなく、[再インストール] で 4 つのファイルを入れ直した時刻もここを進める。
	// restart_needed() は、今動いている Vesktop が書き換える前の設定とファイルで起動したことを、この時刻で
	// 判断する。入れ直した後にも「Vesktopを再起動してください」を表示するので、入れ直しでもこの時刻を進める
	// 必要がある。書くのは install/ である。
	std::optional<std::int64_t> discord_side_written_at_us;

	// 今動いている Vesktop のプロセスの開始時刻 (動いていなければ持たない)。
	std::optional<std::int64_t> vesktop_started_at_us;

	// 元のファイルと違った同梱のファイルの名前。dir が HashMismatch のときだけ意味を持つ。Vesktop が読む場所から
	// 読めなかったもの (platform::BundleComparison::missing) と、中身が違ったもの (同じく differing) を 1 つに
	// 並べる。並びは照合の順 (kVencordBundleFiles)。ファイル名は訳さないので TextRef::args に入れてよい。
	std::vector<std::string> mismatched_files;

	// この起動で cordmux が state.json の vencordDir を書き直したか。書き直したことは、ログに 1 行と診断情報に
	// 1 つの値として残し、メッセージは表示しない。
	bool rewrote_at_startup = false;

	// [再インストール] でコピーできなかったファイルの名前。空 = 入れ直していないか、全部コピーできた。次に
	// [再インストール] を押すか、照合で一致したら空に戻す (戻すのはコピーする側)。
	std::vector<std::string> reinstall_failed_files;

	// この起動では cordmux が Vesktop の設定 (state.json の vencordDir) を書かないか。settings.json の
	// allow_state_json_write を手で false にしたときと、起動のときに settings.json を読めなかったので false に
	// したときに true になる (どちらかは OperationFacts::settings_unreadable_at_startup で分かる)。起動のときに
	// 決まり、同じ起動の中では変わらない。true の間は、Vesktop に追加されていない種類の状態のカードに
	// [Vesktopに追加] を置かず、理由と次の操作を文で伝える (rules_dock.cpp の install_notice)。ドックが
	// DockFacts の同じ名前のフィールドからコピーする。Install に置くのは、Discord 側のインストールを cordmux が
	// 修正できるかという、Install の状態の読み方を変える事実だからである。OperationFacts は押した操作の結果で、ドックが
	// build_observation() の後にまとめてコピーし直すので、そちらには置かない。VencordDirState の値にもしない。
	// 書くかどうかは照合の結果とは別のことで、どの値とも組み合わさるためである。
	bool vesktop_writes_off = false;
};

// ---- 接続の待ち受け (ListenerFacts。ws/listener.h の OpenResult のコピー) ----

enum class ListenerState {
	Unknown,              // まだ開こうとしていないか、結果が届いていない
	Listening,            // 開いている
	BlockedByOtherWriter, // 接続の情報のファイルを書いたほかの OBS がまだ動いている (2 つ目の OBS)
	PortsExhausted,       // 候補のポートがすべて使用中だった
	Closed,               // 閉じている (cordmux を無効にしたときなど、こちらから閉じた)
};

struct ListenerFacts {
	ListenerState state = ListenerState::Unknown;

	// BlockedByOtherWriter のときだけ意味を持つ (ws/listener.h の OpenResult の同じ名前のフィールド)。
	std::optional<std::int64_t> other_writer_started_at_us;
	std::optional<int> other_writer_pid;

	// PortsExhausted のときだけ意味を持つ。試したポート番号の範囲。
	std::optional<int> port_first;
	std::optional<int> port_last;

	// 待ち受けは開けたが、接続のための情報のファイル (ポート番号とトークン。session.json) を保存できなかった
	// (ws/listener.h の OpenProblem::SessionFileNotSaved を OpenSummary::problem からコピーする)。Vesktop は
	// ファイルを読めないので接続できない。ファイルを書くのは待ち受けを開くときの 1 回だけなので、同じ待ち受けの
	// 間は false に戻らない。state の値にしないのは、待ち受けは開いていて Listening が事実だからである。
	bool session_file_not_saved = false;

	// ポートは開けたが、接続の受け付けを始められなかった (ws/listener.h の OpenProblem::AcceptFailed を
	// OpenSummary::problem からコピーする)。そのときの outcome は PortsExhausted なので、state は
	// PortsExhausted で、port_first と port_last も埋まることがある。しかしポートがすべて使用中だったのではない
	// ので、true の間は「待ち受けを始められない」のメッセージからポート番号の文を除く (rules_dock.cpp の
	// ListenPortsExhausted。起きていない原因を伝えないため)。値を入れるのは dock/build_observation.cpp の
	// listener_of である。ListenerState の値にしないのは、待ち受けていない状態としては PortsExhausted と同じで、
	// primary の表の行も同じだからである。
	bool accept_failed = false;
};

// ---- 接続 (Link) ----

enum class LinkState {
	Unknown,        // まだ分からない
	NeverConnected, // 一度も接続していない
	Connected,      // 接続している
	Disconnected,   // 切断された
};

// 切断された理由。ws/ が持っている名前をそのままコピーする (ドックでの対応を 1 対 1 にするため)。元の名前は、
// ws/control_messages.h の HelloNgReason (BadSecret、BadWire、Malformed) と、ws/listener.h の DisconnectReason
// である。
// Origin と Host の検証で拒んだ接続は、ここに値を持たない。拒んだ接続は 1 度も確立しないので、「接続した後に
// 切断された」理由にはならない (ListenerStats::handshake_rejections は回数で、理由ではない)。
enum class DisconnectReason {
	Unknown,           // 理由が分からない (分からないことを断定しない)
	SecretMismatch,    // トークンが合わない (HelloNgReason::BadSecret)
	WireMismatch,      // ワイヤー形式のバージョンが合わない (HelloNgReason::BadWire)
	MalformedHello,    // 最初のメッセージが壊れていた (HelloNgReason::Malformed)
	HelloTimeout,      // 接続したまま hello を送ってこなかった
	HeartbeatTimeout,  // ハートビートの返事が 2 回続けてなかった
	PeerClosed,        // 相手が切断した
};

struct Link {
	LinkState state = LinkState::Unknown;
	std::optional<std::int64_t> connected_at_us;    // 最後に接続した時刻
	std::optional<std::int64_t> disconnected_at_us; // 最後に切断された時刻
	DisconnectReason reason = DisconnectReason::Unknown;
};

// ---- 利用者が自分で選んだ状態 (Choices) ----
// ここにある状態は利用者が自分で選んだものなので、問題として表示しない。

// 利用者が切り替えるオンとオフ。Unknown は、設定をまだ読んでいないこと。
enum class FeatureSwitch { Unknown, Off, On };

// OBS のアプリケーション音声キャプチャが、Vesktop か Discord の音声を配信に出しているか (声が 2 重に配信に
// 入りうる構成)。配信の出力に出ていて、ミュートでも音量 0% でもなく、取り込む先の実行ファイルが Vesktop か
// Discord のソースがあれば Found (sources/double_audio.h)。デスクトップ音声などのソースは見ない。何を取り込んで
// いるかを設定から読めないので、推測で伝えないためである。
enum class DiscordAudioSource { Unknown, NotFound, Found };

// 録画ファイルに書く PCM の形式 (config::RecordPcmFormat のコピー)。AssignMode と同じ理由で Unknown を
// 持たない (設定の値は起動のときに必ずどちらかに決まる)。
enum class RecordPcm { Float32, Int24 };

// 誰を枠に載せるかの決め方 (config/settings.h の同じ名前の列挙のコピー)。Unknown を持たない。設定の値は起動の
// ときに必ずどちらかに決まる (なければ既定の Manual) ので、読めていない状態がない。
enum class AssignMode { Manual, Auto };

// Vesktop がどの人の映像を受信するか (config/settings.h の ReceiveScope のコピー)。AssignMode と同じ理由で Unknown を
// 持たない。
enum class ReceiveScope { Everyone, Slots };

// 自分の Discord の共有。Unknown は分からない (送信側が未接続、sharing を読めない、自分の人が決まらない)。
// 分からないことを「共有していない」と扱わない。
enum class SelfShareState { Unknown, Off, On };

struct Choices {
	FeatureSwitch feature = FeatureSwitch::Unknown;     // cordmux そのものの有効と無効
	FeatureSwitch recording = FeatureSwitch::Unknown;   // cordmux の録画
	FeatureSwitch dummy = FeatureSwitch::Unknown;       // ダミーの表示
	// 誰を枠に載せるか。既定は Manual。設定の同じ名前の値から来る。
	AssignMode assign_mode = AssignMode::Manual;
	// 映像を受信する人 (設定の receive_scope)。Slots のとき、cordmux の画面共有の録画も枠に載せた人の分だけになる。
	ReceiveScope receive_scope = ReceiveScope::Slots;

	DiscordAudioSource discord_audio_source = DiscordAudioSource::Unknown;
	// Discord の音声をキャプチャしているソースの、OBS の音声ミキサーに表示される名前
	// (cordmux.dock.double_audio.headline の %1)。Found にするときは必ず入れる (ソースの設定を読んで検出するので、
	// そのソースの名前は必ず分かる)。名前が空の Found ではメッセージを表示しない (rules_dock.cpp の DoubleAudio。
	// 名前のない見出しを作らないため)。
	std::string discord_audio_source_name;
	// cordmux の音が配信に出る状態か。cordmux のソースが配信の出力に出ているか (obs_source_active) と、出ている
	// cordmux のソースのうち、ミュートでも音量 0% でもないものが 1 つ以上あるか。どちらかが false なら通話の声は
	// 2 重にならないので、DoubleAudio は成り立たない (rules.cpp)。読めていない間は false のままにする。分からない
	// という 3 つ目の値は作らない。読めていない間は discord_audio_source も Unknown で、どちらにしてもメッセージが
	// 成り立たないためである。
	bool composite_active = false;
	bool composite_audible = false;

	// 自分の枠のソースが、合成の配置に入っているか。意味を持つのは composite_checked が true のときだけで、合成の
	// ソースの有無をまだ調べていない間は false のままである (「表示されていない」と断定する値ではない。「分からない」は
	// composite_checked が持つので、ここに 3 つ目の値を作らない)。配置が、表示する人数に自分を 1 人足すかどうかは、
	// このフィールドで決まる (自分を含めて 4 人まで)。
	bool self_frame_shown = false;
	// 自分の枠に映す映像として、OBS のソースかシーンを選んでいるか (設定の self_frame_source_uuid と
	// self_frame_source_name のどちらかが空でない)。「Discord の画面共有」を選んでいる間は false。自分の行のチェックを
	// 自分の枠の付け外しにするのは、この間だけである (false の間は、サブアカウントがあれば、行のチェックはほかの人と
	// 同じ Discord の共有を枠に載せる意味になり、サブアカウントがなければ押せない)。
	bool self_frame_selected = false;
	// 選んだ映像が合成のソースに結び付いているか (読み込みの前と、見つからない間は false)。
	bool self_frame_bound = false;
	// 自分の枠のチェックの状態 (self_frame_shown は、これと self_frame_bound がどちらも true のとき true)。自分の行の
	// チェックの状態になる。
	bool self_frame_on = false;
	// 自分の Discord の共有の現在の値。
	SelfShareState self_frame_share = SelfShareState::Unknown;

	// メインアカウントの userId (設定の cordmux.settings.main_account_id.label の入力欄)。空ならメインアカウント
	// だけの構成で、メインアカウントが通話に参加していないことを伝えるメッセージは成り立たない (Vesktop のアカウントが
	// メインアカウントなので、メインアカウントと別の通話に参加していることが起きない)。利用者が入力した userId をそのまま
	// 持つ。
	std::string broadcaster_user_id;

	// 画面共有していない参加者を名前ラベルに出す設定 (設定の roster_show と roster_show_bots のコピー)。参加者の設定
	// ウィンドウの列「通話のみ時ラベル非表示」は、roster_show が偽の間は全員が、roster_show_bots が偽の間は Bot の行が、押しても
	// 動作しない。既定は、どちらも今までどおり押せる側 (分からない間に押せなくしない)。
	bool roster_show = true;
	bool roster_show_bots = true;

	// 合成のソース (cordmux) があるシーンコレクションか。参加者ごとの音量とミュートは、合成のソースが出す混ぜた音にだけ
	// 掛かる。ない間は、参加者ごとの値は配信の音に反映されない。
	bool composite_present = false;
	// composite_present を調べ終えているか。シーンの読み込みが終わる前と、シーンコレクションを切り替えている途中は
	// false で、その間の composite_present は「ない」と断定する値ではない。cordmux のソースがないことを伝える
	// 規則 (CompositeAbsent) は、この値が true のときだけ成り立つ (起動直後の普通の状態を、まだ置いていないと
	// 表示しないため)。
	bool composite_checked = false;

	// cordmux そのものの有効と無効は、上の feature が設定の feature_enabled からコピーされる (ここに 2 つ目を
	// 作らない)。録画の残り時間のしきい値は RecordingFacts::space_warn_seconds が持つ。

	// 録画の保存先 (設定の record_dir のまま)。空 = 既定の保存先を使う。実際に使う場所ではない (実際の場所は
	// UninstallFacts::record_dir)。
	std::string record_dir;
	// 録画ファイルに書く音声の形式 (設定の record_output_pcm)。
	RecordPcm record_output_pcm = RecordPcm::Float32;
	// 枠の並び順 (設定の slot_order)。行の枠の位置を、配信の配置と同じ並び順で決める。
	config::SlotOrder slot_order = config::SlotOrder::Name;
	// 自分の枠とメインアカウントの画面共有を、並び順によらず先頭に置くか (設定の self_slot_first)。行の枠の位置を、配信の配置
	// と同じ規則で決める (ledger/slot_cells.h の SlotCellSelf::first)。
	bool self_slot_first = true;
	// 自分の枠を載せた時に、すでに載っていた人の載せた順の番号の最大 (0 は、誰よりも先に載せた)。並び順が載せた順で、
	// 自分の枠を先頭に置かないときに、自分の枠の位置を決める (SlotCellSelf::added_mark)。DockPoll が持つ値で、配信の配置にも
	// 同じ値を渡す。
	std::int64_t self_frame_added_mark = 0;
	// 自分の枠の人の user_id と、配信に出している名前 (dock/build_observation.h の self_frame_identity)。行の枠の位置を、配信の
	// 配置と同じ名前で決める (自分の枠の人が通話にいなくて PersonFact がないときも)。分からなければ空。
	std::string self_frame_user_id;
	std::string self_frame_name;

	// 送信側に送ってもらう画質の上限 (設定の同じ名前のキーのコピー)。limit_max_long_edge は 0 = 制限なし、
	// limit_fps_divisor はフレームレートを割る数で、1 = そのまま、2 = 半分。
	int limit_max_long_edge = 0;
	int limit_fps_divisor = 1;

	// 全画面で表示している人の userId (空 = 全画面でない)。ドックがメモリにだけ持ち、保存しない (OBS を起動し直すと空)。
	// 全画面にしても、どの人をどの枠に載せているかは変わらない。
	std::string focus_user_id;
	// 全画面で表示している人の、配信に出している名前 (1 行目の「全画面: %1」の %1)。ダミー映像の表示の間は、全画面にしている
	// ダミーのタイルの名前。空 = 分からない。
	std::string focus_name;
	// 全画面にできる人の userId。合成のソースのタイルに今描かれている人 (自分の枠を含む) と、描かれていないが画面共有している人
	// (枠に載せていない人)。ドックが、合成のソースに渡す配置の入力と facts の sharing から決める (sources/composite_input.h の
	// composite_focus_tiles と composite_focus_others)。行のメニューの [全画面で表示] を押せるかに使う。
	std::vector<std::string> focus_candidates;
};

// ---- Discord の内部への依存箇所 (ContactFact。依存箇所の一覧は src-vencord/ledger.ts) ----

// 依存箇所 1 つの状態。src-vencord/core/state.ts の ContactStatus ("unseen"、"found"、"missing"、"lost") の
// コピーである。Unknown はそれより 1 つ前の「受信側がその状態をまだ受け取っていない」(facts が 1 度も届いて
// いないか、送信側のバージョンにその依存箇所がない) で、NotSeen ("unseen" = 送信側がまだ探していない) とは
// 別である。
enum class ContactState {
	Unknown,  // 受信側にまだ届いていない
	NotSeen,  // 送信側がまだ探していない
	Found,    // 見つかっている
	Missing,  // 探したが見つからない
	Lost,     // 見つかっていたのに届かなくなった
};

struct ContactFact {
	// 依存箇所の id (`pc-hook` `ssrc-map` `sharing` `watch-stop` `roster` `profile` `self`。
	// src-vencord/ledger.ts の CONTACT_LEDGER)。画面の見出しは id ごとのテキストではなく、何ができなくなるかの
	// テキストで、id から見出しのキーへの対応は rules_dock.cpp の kContactHeadlines が持つ。
	std::string id;
	ContactState state = ContactState::Unknown;
};

// ---- 人 (PersonFact) ----

// 送信側のエンコードの段階の名前。整数にすると送信側の LadderStage の並びに依存するので、文字列で持ち、閉じた
// 列挙にしない。現在の送信側が送るのは `native` `halffps` `downscaled` `software` `unavailable` の 5 つ
// (docs/wire_format.md の「facts」の stages) で、知らない値が来ても、画面はその値をそのまま表示する。
inline constexpr const char *kStageNative = "native";
inline constexpr const char *kStageHalfFps = "halffps";
inline constexpr const char *kStageDownscaled = "downscaled";
inline constexpr const char *kStageSoftware = "software";
// 5 つ目の段階。画質を下げたのではなく、その人の映像を送らない段階なので、知らない名前のときの「画質を
// 下げています」(cordmux.dock.row_quality_reduced.status) の表示にしてはならない。
inline constexpr const char *kStageUnavailable = "unavailable";

// Observation::people には、通話に参加している人が全員入る。枠に載せた人やフレームが届いている人だけでなく、
// 通話の参加者の一覧 (roster) を読めていれば、そこにいる全員が 1 人ずつ並ぶ (dock/build_observation.cpp の
// people_of())。ただし people は「通話に参加している人」より広い。枠は、通話の参加者が分からない間 (Vesktop との
// 接続が切れている間など) は空けないので、載っている人が通話を抜けていても people に残る。そのため、メインアカウントが
// 通話に参加しているかや、参加者の設定ウィンドウの「現在の参加者」は、people にいるかではなく in_roster で決める
// (理由は in_roster のコメント)。
// 参加者ごとの声と画面共有の音の、音量 (%) とミュート。画面共有の音は、共有が始まるたびにミュートから始まる
// (pipeline/mixer_roster.h の MixerLevels)。
struct PersonAudio {
	bool voice_muted = false;
	int voice_volume_pct = config::kPersonVolumeDefaultPct;
	bool screen_muted = false;
	int screen_volume_pct = config::kPersonVolumeDefaultPct;
	// 画面共有の音を持っているか (画面共有していて、その音が届く構成か)。false の間は、画面共有の音の値を画面に出さない。
	bool has_screen_audio = false;
};

inline bool operator==(const PersonAudio &a, const PersonAudio &b)
{
	return a.voice_muted == b.voice_muted && a.voice_volume_pct == b.voice_volume_pct &&
	       a.screen_muted == b.screen_muted && a.screen_volume_pct == b.screen_volume_pct &&
	       a.has_screen_audio == b.has_screen_audio;
}

inline bool operator!=(const PersonAudio &a, const PersonAudio &b)
{
	return !(a == b);
}

struct PersonFact {
	std::string user_id;                      // Discord の userId (桁が落ちないように文字列で持つ)
	std::optional<std::string> display_name;   // 表示名を読めないときは持たない
	std::optional<int> slot;                   // 今載っている画面共有の枠 (1 から 4)。持たない = 載せていない
	std::optional<std::int64_t> last_video_frame_at_us; // 最後にフレームが届いた時刻
	std::optional<int> width;                  // 届いている映像の解像度
	std::optional<int> height;
	std::optional<double> fps;                 // ws/control_messages.h の StreamConfig と同じく double で持つ
	std::optional<std::string> stage;          // 上の 5 つの段階の名前

	// 今画面共有しているか (docs/wire_format.md の「facts」の sharing)。映像が届かないメッセージ
	// (AllVideoStoppedHint。画面共有している人の映像が届かないときに表示し、Vesktop で映像が映っているかを確認する
	// ことなどを勧める) は、このフィールドで共有しているかを見る。共有しているかを見ずに、「枠があるのにフレームが
	// 止まった」だけで判断すると、誰かが普通に画面共有をやめるたびにメッセージが表示される。枠と最後のフレームの
	// 時刻は、しばらく残るためである。何も壊れていないときに表示される警告は、ほかの警告まで読まれなくする。
	// false は「共有していると分かっていない」で、「共有していない」ではない (facts が 1 つも届いていない間も
	// false。メッセージの側が「接続している」を条件に持つ)。
	bool sharing = false;

	// 今、通話の参加者の一覧 (docs/wire_format.md の「facts」の roster) にいるか。メインアカウントが通話に参加して
	// いないことを伝えるメッセージ (BroadcasterNotInCall) は、people にいるかではなく、このフィールドで判断する。
	// 枠に載った人は、通話の参加者が分からない間は、通話を抜けても people に残る。people で判断すると、自動配置で
	// 枠に載っていたメインアカウントが抜けたときに、メッセージが表示されない。メインアカウントは枠に載っていることが
	// 多いので、people にいれば参加しているとみなすと、メッセージが最も必要な場面で表示されない。roster の userId の並びを
	// Call に持たせないのは、規則は people を 1 人ずつ見るので、参加者ごとのフラグなら見る所が 1 つで済むためである
	// (sharing と同じ形)。
	// false は「一覧にいると分かっていない」で、「通話に参加していない」ではない (roster を読めていない間も
	// false。メッセージの側が Call::roster_count を条件に持つ)。
	bool in_roster = false;

	// この人が Vesktop のアカウント (この OBS に接続する送信側が動いている Discord のアカウント) か
	// (docs/wire_format.md の「facts」の self_user_id)。Vesktop のアカウントの声も画面共有も受信側に届かない
	// (WebRTC は自分が送った音声を返さず、送信側は自分の画面共有を視聴しない) ので、使う声のメニューは、
	// Vesktop のアカウントを、声の候補にも、声を置き換える人にもしない (ledger/rules.cpp の
	// voice_source_choices)。名前を is_self にしないのは、Choices::self_frame_shown の「自分」は利用者の自分の
	// 枠で、サブアカウントを使う構成では、メインアカウントと Vesktop のアカウントが別の人だからである。
	// Vesktop のアカウントを people から除かないのは、Vesktop のアカウントも通話に参加している 1 人で、除くと
	// 参加者の人数 (Call::roster_count) と一覧の行の数が食い違うためである。
	// false は「Vesktop のアカウントだと分かっていない」で、「Vesktop のアカウントではない」ではない (送信側が
	// 送らない間や、古いバージョンの送信側と組み合わせた間も false で、そのときは誰も除かない)。
	bool is_receiver = false;

	// 自分の枠の人 (メインアカウント。別アカウント方式ではない構成では Vesktop のアカウント) の行か
	// (dock/build_observation.cpp の self_frame_user_id)。false は「自分の枠の人だと分かっていない」で、
	// 自分の枠の人が決まらない間 (メインアカウントの userId が空で、Vesktop のアカウントも分からない間) は誰も true にしない。
	bool is_self_frame = false;

	// Bot のアカウントか (送信側の profiles の bot)。参加者の設定ウィンドウの列「通話のみ時ラベル非表示」を、Bot の行だけ押せなく
	// するかの判断に使う。false は「Bot だと分かっていない」で、「Bot ではない」ではない (profiles がまだ届いて
	// いない人も false。そのときは押せるままにする)。
	bool is_bot = false;

	// 録画の入力を通ったフレームが最後に届いた時刻。last_video_frame_at_us とは別のフィールドである。
	// last_video_frame_at_us は経路のあるストリームの時刻で、枠に載っていない人のストリームは経路がないので
	// 取り除かれる (pipeline/pipeline.h の RecordOnlyFrameClock)。枠には手動で載せるのが既定なので、「枠に
	// 載っていないのにフレームは届いている」はこのフィールドでしか分からない。持たない = 録画の入力を 1 度も
	// 通っていない (録画がオフの間は誰も持たない)。
	std::optional<std::int64_t> last_record_frame_at_us;

	// sharing を信じてよいか。true = 送信側の facts に sharing のキーが届いていて、sharing の値が事実である。
	// false の間の sharing == false は「分かっていない」なので、「画面共有していない」と表示しない。facts が
	// 届いた (received) だけでは true にしない。sharing のキーがないか壊れた facts でも received は true になる
	// ためである (pipeline/facts_state.h の sharing_valid を正とする)。
	bool sharing_known = false;

	// 画面共有していると分かった時刻 (システム時刻のエポックからのマイクロ秒)。持たない = 共有していると
	// 分かっていない。「最初のフレームが届かない」のしきい値 (Observation::first_frame_wait_threshold_us) は、
	// この時刻から数える。
	std::optional<std::int64_t> sharing_since_us;

	// その人を視聴の範囲に入れた時刻 (sharing_since_us と同じ時計)。持たない = 入れていない。受け取る範囲が「枠に載せた
	// 人だけ」のときだけ、「最初のフレームが届かない」と「届いていた映像が止まった」の待ち始めを、この時刻より前にしない
	// (ずっと前から共有している人のチェックを付けた瞬間に、待ち時間を超えたことにしない)。
	std::optional<std::int64_t> watching_since_us;

	// Discord のユーザー名 (Discord の中で 1 人に 1 つ)。持たない = 届いていない。同じ Discord での名前の人が
	// 2 人以上いるときだけ、行の 2 行目に表示して見分ける。
	std::optional<std::string> username;

	// Discord での名前 (送信側の profiles が送ってきた表示名。cordmux での名前を適用する前の値)。持たない =
	// 届いていない。display_name は cordmux での名前を適用した後の値なので、名前を変えた人の 2 行目
	// (cordmux.dock.row_discord_name.status) と、同じ Discord での名前の人がいるかの判断にはこちらを使う。
	// display_name から cordmux での名前を外して元に戻さないのは、cordmux での名前と同じ表示名を持つ人で、
	// 元の名前が分からなくなるためである。
	std::optional<std::string> discord_name;

	// 参加者ごとの音量とミュート (pipeline/pipeline.h の PersonAudioSnapshot のコピー。合成のソースが出す混ぜた音に掛かる値)。
	// cordmux がまだ何も覚えていない人は、既定の値 (ミュートなし、100%) のままである。
	PersonAudio audio;

	// 枠の映像を今配信に描いているか (pipeline/pipeline.h の visible_screen_slots に入る枠の人)。枠を持っていて、画面共有を
	// やめた人は偽。行の枠の位置 (rules_dock.cpp の slot_positions) は、配信の配置と同じこの条件で人を選ぶ。
	bool slot_visible = false;

	// 枠に載せた順の番号 (config::SlotAssignment::added_order)。枠に載っていない人は 0。
	std::int64_t slot_added_order = 0;

	// この人の声が今 cordmux に届いているか (pipeline/pipeline.h の SlotSnapshot::voice_present。この人の声の枠に、声の
	// ストリームの経路がある)。偽の人の行のマイクは押せない。ミュートしても、配信に出ている声が変わらないためである。
	bool voice_arriving = false;

	// 映像が届かなくなって、受信をやり直した回数 (pipeline/pipeline.h の PipelineSnapshot::receive_restarts。送信側が実際に
	// 始め直した回数と、受信側がデコードの経路を作り直した回数の合計)。OBS を起動してから増えるだけである。状態タブの履歴が、前の周期より増えた人の行を書く。
	std::uint64_t receive_restarts = 0;
};

// ---- 通話 (Call) ----

enum class Presence { Unknown, NotInCall, InCall };

struct Call {
	Presence presence = Presence::Unknown;

	// 通話に参加している人数。持たない = まだ読めていない (読めなかったことを「誰もいない」と扱わない。
	// 送信側が読めなかったときに facts の roster のキーごと省くのと同じ扱いである)。
	std::optional<int> roster_count;

	// Vesktop のアカウントがいる通話の名前。空 = 分からない。画面には「-」と表示する。
	std::string name;

	// メインアカウントが参加している通話 (サブアカウントを使う構成でだけ意味を持つ)。参加者の一覧にメインアカウントがいれば、
	// Vesktop のアカウントと同じ通話なので、この値は見ない (PersonFact::in_roster)。
	// main_presence は、Vesktop のアカウントから見える通話に参加しているか。InCall か Unknown だけを取り、NotInCall には
	// しない。見えないサーバーの通話に参加しているときと、どの通話にもいないときは、Discord の情報に同じに現れ、区別できない
	// ためである (「通話なし」と断定しない)。
	Presence main_presence = Presence::Unknown;
	std::string main_name; // 通話の名前。空 = 分からない
	// サブアカウントがメインアカウントの通話に参加できるか。持たない = 分からない。別のサーバーの通話でも、サブアカウントが
	// そのサーバーにいれば参加できる。
	std::optional<bool> main_call_joinable;
};

// ---- バージョン (Versions) ----

// どのフィールドも、空の文字列は「まだ分からない」。画面には空のまま表示せず、rules.cpp が "-" を埋める。
// 比べるのは次の 3 つで、比べ方は文字列の一致である。どちらが古いかは伝えない。どれかが空なら、食い違いの
// メッセージを表示しない (分からない値で食い違いと判断しないため)。
//   plugin          cordmux.dll のビルドの識別 (`<version>+<短いコミット>`。ローカルのビルドは
//                   `<version>+dev`。ws/listener.h の server_version と同じ値)
//   bundled_vencord 同梱の Vencord のマーカーファイル (元のファイルのフォルダー data/vencord-src の
//                   cordmux-build.txt) の build_id (install/build_mark.h。起動したときと照合のたびに読む)
//   discord_side    Vesktop の cordmux が hello で送ってきたバージョン (送信側の src-vencord/version.ts)
// 表示するメッセージは 2 つある。bundled_vencord と discord_side が違えば VersionRestartVesktop、plugin と
// bundled_vencord が違えば VersionMixedBundle (rules.cpp の notices の表)。
struct Versions {
	std::string plugin;          // cordmux.dll のビルドの識別 (上のコメント)
	std::string discord_side;    // Vesktop の cordmux (hello の version)
	std::string vesktop;         // Vesktop そのもの (hello の省略できるキー vesktop)
	std::string bundled_vencord; // 同梱の Vencord のマーカーファイルの build_id (上のコメント)
	// 同梱の Vencord のマーカーファイルの vencord_commit (ビルドに使った Vencord のコミット。
	// src-vencord/vencord.lock.json)。診断情報に書くだけで、比べない (Vencord のコミットで、cordmux のバージョン
	// ではないため)。空 = マーカーファイルを読めていない。
	std::string bundled_vencord_commit;
	// 同梱の Vencord のマーカーファイルの vencord_tag (Vencord のバージョン。例 v1.15.9)。[バージョン情報] の
	// 「同梱のVencord」に表示する (build_id は cordmux のバージョンで、Vencord のバージョンではないため)。
	// 表示するだけで、比べない (食い違いの判断は bundled_vencord の build_id で行う)。
	// 空 = 印が読めていない。
	std::string bundled_vencord_tag;

	// [バージョン情報] に表示する、cordmux が動いている環境のバージョン。上と同じく、空 = まだ分からない。値を
	// 読むのはドックで、ここは受け取るだけである。FFmpeg のバージョンは、まだどこからも読んでいない。
	std::string obs;     // OBS
	std::string windows; // Windows
	std::string gpu;     // GPU
};

// ---- これまでに見た人 (Seen。参加者の設定のファイル seen_people.json の中身) ----

// seen_people.json の 1 人分 (平たい配列の 1 つの要素で、読み書きの純粋関数は config/seen_people.h にある)。
// ドックはこれをそのまま渡し、一覧の並べ方も表示のしかたも decide_dock() が決める。
struct SeenPerson {
	std::string user_id; // Discord の userId

	// 最後に見たときの表示名。空 = 読めていない (表示名を読めなかったときに記録した行)。そのとき画面には
	// userId をそのまま表示する。
	std::string display_name;

	// 最後にこの人を見た時刻 (エポックからのマイクロ秒)。上限の 200 人を超えたときは、これが古い順に削除する。
	std::int64_t last_seen_at_us = 0;

	// ---- 利用者が付けた設定 (設定のある人は、上限を超えても削除しない) ----

	bool always_on = false;                              // 「自動で載せる」
	std::optional<std::string> name_override;            // cordmux での名前 (配信に表示する名前)
	std::optional<std::string> voice_source_user_id;     // 使う声 (この人の枠で再生する声のアカウント)
	// 参加者の設定ウィンドウの列「通話のみ時ラベル非表示」を付けたか。真なら、合成のソースの画面共有していない参加者の並びに出さない
	// (通話に参加できた Bot など)。音には関係しない。既定は偽 (出す) で、seen_people.json には真のときだけ書く。
	bool roster_hidden = false;
	// 参加者ごとの音量 (%。声と画面共有の音で別。100 が既定)。次に OBS を起動したときも使う。ミュートは覚えない。100 でない値は
	// seen_people.json に書き、利用者の指定として数える (config/seen_people.h)。
	int voice_volume_pct = config::kPersonVolumeDefaultPct;
	int screen_volume_pct = config::kPersonVolumeDefaultPct;
};

// cordmux での名前が付いているか。空の文字列は付いていないものとして扱う。空の名前を渡す操作は「名前を元に
// 戻す」(ledger/actions.h の ActionArgs::name) なので、空の名前が残っていると、名前を変えたと扱いながら元の
// 名前を表示することになる。名前を表示する所と、設定のある人を数える所 (config/seen_people.h) が、この関数を
// 見る。
inline bool override_present(const SeenPerson &person)
{
	return person.name_override.has_value() && !person.name_override->empty();
}

// これまでに見た人の一覧。今通話に参加している人も含めて全員が入る (seen_people.json の中身のまま)。通話に
// 参加している人を現在の参加者の側に分けるのは表示の判断なので decide_dock() が行い、cordmux での名前が
// 付いているかを見る apply() も、この一覧の全体を読む。
struct Seen {
	std::vector<SeenPerson> people;

	// 上限 (200 人) を超えたために削除した、設定のある人の数。設定のない人の削除は一覧のふつうの入れ替わりなので
	// 数えない。0 でなければ、利用者が付けた設定が消えたので、画面で伝える。
	int discarded_with_marks = 0;

	// 今通話に参加している人が、通話の参加者に現れた時刻 (エポックからのマイクロ秒。userId -> 時刻)。DockPoll が OBS の起動中
	// だけ覚え、保存しない。cordmux が Vesktop と接続した時点で既に通話にいた人は、その時点の時刻になる。参加者の設定
	// ウィンドウの「現在の参加者」の最終参加日時の列と、既定の並び (新しい順) に使う。抜けた人はここから消える (抜けた時刻は
	// SeenPerson::last_seen_at_us)。
	std::map<std::string, std::int64_t> call_joined_at_us;
};

// ---- 受け取る映像の事実 (ReceiveFacts) ----

// 1 人の直近 1 分の画面共有の映像の届くまでの遅れ。streams は、画面を共有しているストリームごとの列
// (1 人が 2 本以上共有していれば 2 つ以上)。
struct ArrivalFact {
	std::string user_id;
	std::vector<pipeline::ArrivalWindow> streams;
};

// 状態タブの [映像の受信] の表が出す事実。状態タブが見えている間だけ入る (見えていない間は空)。
struct ReceiveFacts {
	std::vector<ArrivalFact> arrival;
};

// ---- 録画の事実 (RecordingFacts) ----

// 録画ファイルの作成の進み具合。record/record_types.h の record::FinalizeState の名前をそのままコピーし、Unknown を加えた
// (「まだ確認していない」と「まだ始めていない」は別の事実である)。あちらのヘッダーはここから読み込まない。
// 表示の規則は画面に何を表示するかだけを決め、録画の実装の型に依存しないためである (DisconnectReason が
// ws/ の列挙をコピーしているのと同じ形)。
enum class FinalizeState {
	Unknown,    // まだ確認していない
	NotStarted, // まだ録画を止めていない
	Running,    // 作成している最中
	Done,       // 録画ファイルの作成終わった
	Pending,    // 途中で打ち切った (次に起動したときに再開できる)
	Failed,     // 失敗した (元のセグメントファイルは削除していない)
};

// 録画ファイルの作成の方法 (record::FinalizeBranch のコピーに Unknown を加えたもの)。
enum class FinalizeBranch {
	Unknown, // まだ決まっていないか、読めていない
	Remux,   // 再エンコードしない
	ProRes,  // その人のそのファイルだけ、ProRes 422 HQ に 1 回再エンコードする
};

// 空き容量が少ないとメッセージ始める、録画できる残り時間 (秒) の既定値。30 分は目安の値で、測って決めたものでは
// ない。利用者がメッセージの早さを選べるように、設定の record_space_warn_minutes から変えられる。
inline constexpr std::int64_t kDefaultSpaceWarnSeconds = 30 * 60;

// 残り時間がこの秒数以下になったら、空き容量の警告 (RecordSpaceLow) の段階を 1 つ進める。既読にしていても未読に戻して、
// 1 行目にもう一度出す。5 分は目安の値で、測って決めたものではない。
inline constexpr std::int64_t kSpaceUrgentSeconds = 5 * 60;

// 送信側に録画中であることが届いていないと伝える (RecordNotReachedSender) までの待ちの既定値。録画の状態は、
// 値が変わったときと接続した直後に送信側へ送り、送信側は facts で返す。facts は 1 秒に 1 回までしか送られない
// (docs/wire_format.md の「facts」) ので、押した直後や再接続した直後には、必ず「まだ届いていない」状態がある。
// そこを警告にすると、録画を始めるたびに 1 周期だけ警告が表示される。往復の 1 秒にポーリングの周期 (500 ms) と
// 余裕を足して 5 秒にした。目安の値で、測って決めたものではない。ほかのしきい値と同じくフィールドとして持つ。
inline constexpr std::int64_t kDefaultSenderRecordingGraceUs = 5'000'000;

// cordmux の録画の段階 (record::RecordPhase のコピーに Unknown を加えたもの。FinalizeState と同じ形)。録画の
// ボタンの 5 つの状態 (待機、開始の処理中、録画中、停止の処理中、失敗) は、表示の規則がこの段階から決める。
// Unknown は「録画の側から段階をまだコピーしていない」で、Idle とは別である。
enum class RecordPhase {
	Unknown,     // まだコピーしていない
	Idle,        // 録画していない
	Starting,    // 開始の処理中 (保存先を確認している)
	Recording,   // 録画中
	Stopping,    // 停止の処理中 (書き込むスレッドが止まるのを待っている)
	StartFailed, // 開始に失敗した (保存先に書き込めなかった。ボタンは待機の見た目に戻る)
};

// cordmux の録画が途切れていた 1 つの期間 (record::RecordGapSpan のコピー)。start_us はシステム時刻の
// エポックからのマイクロ秒、duration_us はその長さ。何を途切れとみなすかは録画の側 (record/) が決める。
struct RecordGapFact {
	std::int64_t start_us = 0;
	std::int64_t duration_us = 0;
};

struct RecordingFacts {
	// セグメントの数と書いたバイト数。持たない = まだ読めていない。
	std::optional<std::int64_t> segment_count;
	std::optional<std::int64_t> bytes_written;

	// 保存先の空き容量と、現在の消費のままなら何秒録画できるか。
	std::optional<std::uint64_t> free_space_bytes;
	std::optional<std::int64_t> remaining_seconds;

	// 録画ファイルの作成に必要な空き容量の最大値の見積もり。空き容量より大きければ、録画ファイルの作成に足りない見込みを伝える。
	// セグメントのサイズに決まった倍率を掛けては見積もらない。映像によって 40 倍以上ばらつくことを実際に測った
	// ので、録画ログに書いた実際のバイト数から見積もった値が入る。
	std::optional<std::uint64_t> finalize_need_bytes;

	// 録画ファイルの作成の進み具合と方法。
	FinalizeState finalize = FinalizeState::Unknown;
	FinalizeBranch branch = FinalizeBranch::Unknown;
	int finalize_files_total = 0;
	int finalize_files_done = 0;
	// この起動で最後に作成したセッションで、同梱の FFmpeg を起動できなかったファイルがあるか。録画ファイルの作成に失敗した
	// メッセージの次の操作を決める。失敗が録画ログにしか残っていないとき (OBS を起動し直した後など) は、理由が
	// 分からないので偽のままにする。
	bool finalize_ffmpeg_unavailable = false;

	// 録画ファイルの作成の途中のものが残っているか ([作成を再開] を押せる条件)。
	bool pending_present = false;
	int pending_session_count = 0;

	// 録画ファイルの作成に失敗したまま残っている録画のセッションのフォルダー (録画ログが Failed のセッション。録画の側が、
	// 録画ファイルの作成を待つセッションを数えるのと同じ探し方で見つけた、最も新しい 1 つ)。空 = 残っていない (か、まだ
	// 数えていない)。録画ファイルの作成の待ち行列に入れたセッションと、作成している最中のセッションは入れない (押し直して
	// 作成している間は「作成しています」の行が伝える)。finalize はこの起動で最後に作成したセッションのことだけを
	// 伝えるので、新しいセッションを始めたり OBS を再起動したりすると Failed が消える。録画ファイルの作成に失敗したメッセージを、
	// 録画ファイルの作成が成功するまで表示し続けるために、録画ログに残った失敗をここで持つ。録画ファイルの作成に失敗したメッセージ
	// (rules.cpp の RecordFinalizeFailed) が読み、場所の行にもこのフォルダーを表示する。値を入れるのは
	// dock/build_observation.cpp の recording_of (record::RecordingSnapshot の同じ名前のフィールドから)。
	// 失敗したかどうかだけを持たないのは、再起動した後の場所の行に、今録画している別のセッションのフォルダーが
	// 表示されるためである。
	std::string failed_session_dir;

	// 送信側が facts で返した、現在の録画の状態。持たない = まだ届いていない (facts が 1 つも届いていないか、その
	// キーを知らない古いバージョンの送信側)。利用者が選んだ状態 (Choices::recording) と食い違っていれば、画面で
	// 伝える。
	std::optional<bool> sender_recording;

	// 現在のセッションの録画を始めた時刻 (エポックからのマイクロ秒)。持たない = 今は録画していない
	// (record::RecordingSnapshot::started_at_wall_us のコピー。0 は「一度も始めていない」なので、持たない値に
	// する)。RecordNotReachedSender のメッセージがこれを読む。押した直後はまだ届いていないのが当たり前で、そこを
	// 警告にすると、録画を始めるたびに 1 周期だけ警告が表示される (上の kDefaultSenderRecordingGraceUs)。
	std::optional<std::int64_t> enabled_since_us;

	// 録画の側が自分で録画を止めたか。止めるのは空き容量が尽きたときだけなので、理由の列挙ではなく bool で持つ
	// (理由が増えたら列挙にする)。利用者が止めたときと OBS の終了は含まない (止めた側が既に知っている)。次に
	// 録画を始めるまで true のまま残る。止まった理由を画面に表示し続けるためで、false に戻るのは次のセッションを
	// 始めたときである (record/record_types.h の同じ名前のフィールド)。
	bool stopped_by_space_exhausted = false;

	// 保存先に書き込めるか。持たない = まだ確認していない。false のときは RecordDirUnwritable のメッセージを表示する。
	// 録画のボタンは、この値では押せなくしない (理由は ledger/actions.cpp の ToggleRecording の行のコメント)。
	std::optional<bool> save_dir_writable;

	// 空き容量が少ないとメッセージ始めるしきい値 (秒。上の kDefaultSpaceWarnSeconds)。
	std::int64_t space_warn_seconds = kDefaultSpaceWarnSeconds;

	// RecordNotReachedSender をメッセージ始めるまでの待ち (マイクロ秒。上の kDefaultSenderRecordingGraceUs)。
	std::int64_t sender_grace_us = kDefaultSenderRecordingGraceUs;

	// 現在の段階 (上の RecordPhase) と、その段階に入ったシステム時刻。時刻を持たない = まだコピーしていない。
	// 録画のボタンのテキストは、段階に入ってから 1 秒までは前のテキストのままなので、時刻も必要である。
	RecordPhase phase = RecordPhase::Unknown;
	std::optional<std::int64_t> phase_since_us;

	// 現在のセッションか最後のセッションの、録画が途切れた期間 (古い順)。次のセッションを始めると空になる
	// (record::RecordingSnapshot::gaps)。
	std::vector<RecordGapFact> gaps;

	// 録画ファイルの作成の進み具合の詳細。数は実際の処理から取る (経過時間から推測しない)。
	// finalize_current_index は今作成しているファイルの番号 (1 始まり。0 = 作成しているファイルがない)。
	// finalize_permille は現在のファイルの進み具合の千分率 (0 から 1000)。持たない = 割合が分からない (FFmpeg が
	// 進み具合を出す前など)。finalize_last_progress_us は最後に数が進んだシステム時刻。finalize_stalled は、
	// 進み具合が止まったと録画の側が判断したこと (設定の record_finalize_stall_s)。
	int finalize_current_index = 0;
	std::optional<int> finalize_permille;
	std::optional<std::int64_t> finalize_last_progress_us;
	bool finalize_stalled = false;

	// 最後に録画を始めようとして書き込めなかった保存先 (UTF-8 の絶対パス。空 = 書き込めなかった場所はない)。
	// メッセージのレベル 1 のメッセージの場所の行と、[保存先を変更] に使う。
	std::string unwritable_dir;

	// 現在のセッションか最後のセッションのフォルダー (UTF-8 の絶対パス。空 = まだ 1 回も録画していない)。
	// 完成したファイルの場所の行に使う。
	std::string session_dir;
};

// ---- アンインストール (UninstallFacts) ----

// アンインストールの進み具合。Unknown を持たない。アンインストール中を示すフラグはメモリにだけあり、読めない
// 状態がない (起動したときは必ず NotStarted)。
enum class UninstallPhase {
	NotStarted, // まだ始めていない
	Running,    // 始めた後で、終わる前 (アンインストール中のフラグが立っている)
	Done,       // すべての段階を終えた
	Stopped,    // 途中で止まった (leftovers に残っているものがある)
};

// アンインストールが途中で止まったときに残っているものの種類。最初の 4 つは install/removal_table.h の、
// cordmux が削除する行のコピーで、後の 2 つは、ほかのシーンコレクションを順に開いたときに見つかったものである。
// install/ のヘッダーはここから読み込まない (FinalizeState が record/ を読み込まないのと同じ理由)。
enum class UninstallLeftoverKind {
	SettingsFiles,           // obs_module_config_path の下の cordmux のファイル
	LocalDataFolder,         // トークンとポート番号を保存するフォルダー (%LOCALAPPDATA% の下の cordmux)
	Sources,                 // cordmux のソース
	VencordDir,              // state.json の vencordDir を戻せなかった
	CollectionNotOpened,     // 開けなかったシーンコレクション
	CollectionNameDuplicate, // 同じ表示名のシーンコレクションが 2 つ以上ある
	// シーンコレクションのフォルダーを読めず、同じ名前のシーンコレクションがないか確認できなかった (名前を持たない。
	// where は空)。OBS は同じ名前の 2 つ目を一覧に載せないので、読めないと、開けないシーンコレクションがあるか分からない
	CollectionFolderUnreadable,
};

struct UninstallLeftoverFact {
	UninstallLeftoverKind kind = UninstallLeftoverKind::SettingsFiles;
	// どこに残っているか (ファイルの場所か、シーンコレクションの表示名)。利用者のファイルシステムと OBS から来た
	// 文字列なので訳さない (TextRef::args に入れてよい)。
	std::string where;
	// where がファイル 1 つの場所か (アンインストールの処理が、削除できなかったものを見て決めた事実)。false =
	// フォルダーか、シーンコレクションの表示名か、空。ダイアログとカードは、ファイルの場所の行に
	// [ファイルの場所を開く] を置く (rules_dock.cpp の uninstall_result)。
	bool is_file = false;
};

struct UninstallFacts {
	UninstallPhase phase = UninstallPhase::NotStarted;

	// アンインストールを押せない理由を決めるための値 (配信中、録画中、Vesktop が動いている間は押せない)。
	// 持たない = まだ読めていない。Vesktop は Install::process、cordmux の録画は Choices::recording を見る
	// (ここに 2 つ目を作らない)。
	std::optional<bool> obs_streaming; // OBS が配信中か
	std::optional<bool> obs_recording; // OBS が録画中か (cordmux の録画とは別)
	// OBS の仮想カメラとリプレイバッファが動いているか (obs_frontend_virtualcam_active、
	// obs_frontend_replay_buffer_active)。アンインストールはシーンコレクションを順に切り替えてソースを削除する
	// ので、配信と同じく、ほかの人が見ている出力 (仮想カメラの先の会議や Discord) と、保存される出力
	// (リプレイバッファ) に映ってしまう。値を入れるのは、ドックの周期ごとのコピー (dock_poll.cpp) と、押した
	// 時点の取り直し (dock_poll_install.cpp) である。持たない = まだ読めていないが、押せない理由の
	// 「確認しています」には数えない (actions.cpp の Uninstall の行のコメント)。
	std::optional<bool> obs_virtual_camera;
	std::optional<bool> obs_replay_buffer;

	// 完了したときに表示する「削除するフォルダー」(展開した cordmux のフォルダー) と、録画の保存先 (録画した
	// ファイルが残っている場所)。Done のときに意味を持つ。
	std::string folder_to_delete;
	std::string record_dir;

	// Stopped のときに残っているもの。
	std::vector<UninstallLeftoverFact> leftovers;

	// Stopped のときに、次のアンインストールで必要なバックアップ (Vesktop の設定を戻すための元の値) を残した、
	// cordmux の設定のフォルダー (OBS の plugin_config の下の cordmux。UTF-8 の絶対パス)。値を持つのは、Vesktop の
	// 設定を戻せずに止まり、アンインストールの処理がバックアップを確認したときだけで、空 = ないか分からない
	// (install/uninstall.h の UninstallReport::settings_dir)。「元に戻せなかった設定」の注意の場所の行に、
	// プラグインのフォルダーの後に並べる (rules_dock.cpp の uninstall_result)。削除できなかったものではなく、
	// 削除しないでほしいものなので、leftovers とは別のフィールドにする。
	std::string settings_dir;
};

// ---- OBS の配信と録画 (ObsOutputs) ----

// OBS が配信中か、録画中か (cordmux の録画とは別)。持たない = まだ読めていない。配信中の失敗をメッセージの
// レベル 1 にする条件 (streaming_with_slotted_person()) が見る。UninstallFacts::obs_streaming と obs_recording と
// 同じ事実で、build_observation() がそちらからコピーする。
struct ObsOutputs {
	std::optional<bool> streaming;
	std::optional<bool> recording;
};

// ---- ドックが持っている操作の結果 (OperationFacts) ----
//
// 押した操作が失敗したことは、利用者が次の操作をするまで残す。値は DockPoll のメンバー ops_ に書き、ドックが
// 周期ごとに Observation へコピーする。ここには失敗の文を持たない (テキストは規則が ini のキーで選ぶ)。

// [Vesktopに追加] が失敗した理由 (dock/dock_poll_install.cpp の write_vencord_dir が失敗で戻る箇所と 1 対 1)。
// バックアップを読めないとき、[キャンセル] を押したとき、既に同じ場所を指しているときは失敗にしない。
enum class VesktopAddFailure {
	StatePathUnknown,  // Vesktop の設定のファイルの場所が分からない
	StateUnreadable,   // Vesktop の設定のファイルを読めない
	BundledDirUnknown, // 同梱の Vencord の場所が分からない
	Refused,           // 書いてよい形ではなかったので書かなかった
	WritesDisabled,    // 書き込まない設定になっている (設定の allow_state_json_write)
	NoBackupPlace,     // バックアップの保存先がない
	BackupWriteFailed, // バックアップを書き込めない
	StateWriteFailed,  // Vesktop の設定のファイルに書き込めない
	PortableVesktop,   // ポータブル版の Vesktop を見たので書かなかった (設定の場所を OBS の側から知れない)
};

// [Vesktopから削除] が失敗した理由 (dock/dock_poll_install.cpp の remove_from_vesktop が失敗で戻る箇所と対応する。
// install/uninstall.h の VesktopRemoveOutcome をまとめたもの)。書く直前に Vesktop が起動していたときは失敗にしない
// (押せない理由と同じ文をボタンの下に出す。dock/build_observation.h の DockFacts::vesktop_process_seen)。
enum class VesktopRemoveFailure {
	StateUnreadable,  // Vesktop の設定のファイルの場所が分からないか、読めない
	NotRestorable,    // cordmux を追加する前の値が分からない (バックアップがない、読めない、現在の値が想定と違う)
	WritesDisabled,   // 書き込まない設定になっている (設定の allow_state_json_write)
	StateWriteFailed, // Vesktop の設定のファイルに書き込めない
};

// 診断情報のエクスポートの結果の種類。成功だけを持つ (失敗はその場でダイアログに表示する)。列挙にしたのは、
// 結果の種類が増えたときに、フィールドの意味を変えずに追加するためである。
enum class ExportResultKind { Written };

// 診断情報のエクスポートの結果。path は書き込んだ zip の UTF-8 の絶対パス。
struct ExportResultFact {
	ExportResultKind kind = ExportResultKind::Written;
	std::string path;
};

// 押した [移動] か [退出]。送信側は失敗しても何も返さないことがあるので、押した時刻を覚えて、通話が変わったかで結果を見る。
enum class CallActionKind { Move, Leave };

struct CallActionPress {
	CallActionKind kind = CallActionKind::Move;
	std::int64_t pressed_at_us = 0; // 押した時刻 (システム時刻のマイクロ秒)
};

struct OperationFacts {
	// 起動のときに settings.json を読めなかった (その起動では書き込まない)。
	bool settings_unreadable_at_startup = false;
	// 保存できていない設定の変更がある (保存できたら false に戻す)。
	bool settings_unsaved = false;
	// 起動のときに seen_people.json を読めなかった (その起動では書き込まない)。
	bool seen_unreadable_at_startup = false;
	// 保存できていない参加者の設定の変更がある。
	bool seen_unsaved = false;
	// [Vesktopに追加] の失敗。持たない = 失敗していない (成功するか、[キャンセル] を押すと消す)。
	std::optional<VesktopAddFailure> vesktop_add_failure;
	// [Vesktopから削除] の失敗。持たない = 失敗していない ([Vesktopから削除] か [Vesktopに追加] が成功すると消す)。
	std::optional<VesktopRemoveFailure> vesktop_remove_failure;
	// 診断情報のエクスポートの結果。持たない = 結果を表示していない (設定ウィンドウを閉じると消す)。
	std::optional<ExportResultFact> export_result;
	// 押した直後に空きがなくて枠に載せられなかった人の userId。空 = いない。押した結果は、押した所のそば (その人の行) で伝える。
	// 別の人が載せられなかったときは、その人に替わる。その人が載るか、空いている枠ができるか、その人の行のチェックの意味が
	// 変わったと分かるか、接続が切れると消える (slot_refusal_finished と settle_operation_facts。DockPoll)。消えた後に枠がまた
	// 埋まっても、行には出ない。
	std::string last_slot_refused_user_id;
	// 押して、結果をまだ見ていない [移動] か [退出]。持たない = ない。もう一度押すと押し直した時刻に替わり、結果が出るか、
	// 接続が切れると消える (call_action_finished と settle_operation_facts。DockPoll)。
	std::optional<CallActionPress> call_action;
	// 押してから、通話が変わらなければ失敗とみなすまでの時間 (設定の call_action_timeout_ms をマイクロ秒にした値)。
	std::int64_t call_action_timeout_us = 5'000'000;
};

// ---- 設定ウィンドウのページ ----

// 設定ウィンドウの 4 つのページ。並びはウィンドウの左の一覧の上からの順である。このファイルに
// 置くのは、ledger/actions.h の ActionArgs (どのページで開くか) と dock/settings_fields.h (入力欄を置くページ) の
// 両方が同じ型を使い、両方が読み込める場所がここだからである。
enum class SettingsPage {
	Recording, // cordmux の録画 ([録画] ページ)
	Receive,   // 映像の受信
	Advanced,  // 詳細設定
	About,     // cordmux について
};

// ---- 映像が止まったとみなすしきい値 ----

// 接続とハートビートは生きているのに、ある人の映像のフレームが 2 秒以上届かないときは、その人の映像が
// 止まったとみなす (接続の問題ではないので、接続が切れたとは伝えない)。2 秒は目安の値で、測って決めたもの
// ではないので、Observation のフィールドとして持ち、設定の video_stall_ms から変えられるようにしてある。
inline constexpr std::int64_t kDefaultVideoStallUs = 2'000'000;

// 最初のフレームが届かないとみなすまでの待ち (画面共有していると分かった時刻から数える)。設定の
// first_frame_wait_ms (既定 5000) をマイクロ秒にした値で、同じ値をここにも既定値として置く。目安の値で、
// 測って決めたものではない。映像が止まったとみなすしきい値 (kDefaultVideoStallUs) とは別の値である。
// あちらは届いていたものが来なくなったこと、こちらは 1 度も届いていないことで、待つ長さの性質が違う。
inline constexpr std::int64_t kDefaultFirstFrameWaitUs = 5'000'000;

// ドックが周期ごとに作り直す、表示の規則の入力。
struct Observation {
	// 現在の時刻 (システム時刻のエポックからのマイクロ秒)。時計を読むのはドックで、ここから先は純粋関数である。
	std::int64_t now_us = 0;

	// 時刻を利用者の地域の日付で扱うための時差 (秒。今日かどうかの判断などに使う)。OS の設定からドックが埋める。
	// 時差も時計と同じく入力にする。表示の規則の中で localtime を呼ぶと、同じ Observation でも結果が変わり、
	// テストが環境に依存するためである。
	int utc_offset_sec = 0;

	// 映像が止まったとみなすまでの、フレームが届かない時間 (上の kDefaultVideoStallUs)。
	std::int64_t video_stall_threshold_us = kDefaultVideoStallUs;

	Install install;
	ListenerFacts listener;
	Link link;
	Choices choices;
	std::vector<ContactFact> contacts;
	std::vector<PersonFact> people;
	// Vesktop と接続していない間の、接続が切れる前に分かっていた参加者 (dock/build_observation.cpp が DockFacts からコピーする)。
	// 接続している間は空である。参加者の一覧の行を消さずに残すためだけに使い、ほかの規則は読まない。
	std::vector<PersonFact> last_known_people;
	// Vesktop のアカウントの userId と名前 (facts の self_user_id と、その人の表示名)。通話の外でも分かる。接続が切れている間は、
	// 最後に分かった値を使う (last_known_people と同じ考え方)。空 = 分かっていない。
	std::string vesktop_user_id;
	std::string vesktop_name;
	Call call;
	Versions versions;
	Seen seen;

	// 利用者が選んだ状態 (録画のオンとオフ) は Choices::recording が持つ。ここに入るのは、選んだ結果として今
	// 何が起きているかの事実だけで、選択そのものを 2 か所に持たない。
	RecordingFacts recording;

	// アンインストール中を示すフラグは install/ の is_uninstalling() が持ち、ドックがここへコピーする (phase が
	// NotStarted 以外なら、フラグが立っている)。
	UninstallFacts uninstall;

	// 最初のフレームが届かないとみなすしきい値 (上の kDefaultFirstFrameWaitUs。設定の first_frame_wait_ms を
	// ドックがコピーする)。
	std::int64_t first_frame_wait_threshold_us = kDefaultFirstFrameWaitUs;

	// 映像を受け取ると決めていない状態 (video_expected() が false) から、受け取ると決めている状態に戻った時刻。
	// 戻るまでの間は送信側が映像を止めているので、戻った直後に、古い時刻との差だけで映像が止まった、最初のフレームが
	// 届かない、と判断しないために、待ち時間はこの時刻から数える (video_wait_from())。ドックが状態の変わり目を見た
	// ときだけ持つ。見ていない間 (起動してからずっと同じ状態だった) は持たない。
	std::optional<std::int64_t> video_expected_since_us;

	// OBS の配信と録画 (上の ObsOutputs)。
	ObsOutputs obs_outputs;

	// ドックが持っている操作の結果 (上の OperationFacts)。
	OperationFacts ops;

	// 受け取る映像の事実 (上の ReceiveFacts)。
	ReceiveFacts receive;
};

// この userId に今付いている cordmux での名前を返す。なければ空の文字列 (空の名前は override_present() と
// 同じく、付いていないものとして扱う)。cordmux での名前の値そのものを読む関数は、これだけにする。名前の変更を
// 押せるかどうか (actions.cpp) は、名前が付いているかだけでなく、渡された名前が今と同じかも見るので、付いて
// いるかどうかの答えだけでは足りない。
inline std::string name_override_of(const Observation &obs, const std::string &user_id)
{
	for (const SeenPerson &person : obs.seen.people) {
		if (person.user_id != user_id)
			continue;
		return override_present(person) ? *person.name_override : std::string();
	}
	return std::string();
}

// この userId に cordmux での名前が付いているか。付いているかどうかの判断は override_present() が持つ。
inline bool has_name_override(const Observation &obs, const std::string &user_id)
{
	return !name_override_of(obs, user_id).empty();
}

// この画面共有の枠に今載っている人の userId を返す。誰も載っていなければ空。
// people を見るだけで枠の埋まり具合が分かるのは、枠に載っている人が通話の参加者が分からない間に people に残るためである
// (dock/build_observation.cpp の people_of() が、割り当て表に載っている人を全員 people に入れる)。割り当て表
// そのものを Observation に持ち込まなくても、枠の空きは people から決まる。
inline std::string screen_slot_owner(const Observation &obs, int slot)
{
	if (slot < 1 || slot > config::kScreenSlotCount)
		return std::string();
	for (const PersonFact &person : obs.people) {
		if (person.slot.has_value() && *person.slot == slot)
			return person.user_id;
	}
	return std::string();
}

// 今空いている画面共有の枠のうち、いちばん小さい番号を返す。空きが 1 つもなければ 0。人を枠に載せる操作が
// 指す枠と、その操作を押せるかどうかを、この関数が決める。番号の小さい側を選ぶのは、割り当て表の try_assign
// (config/allocation.h) が空いた枠を番号の小さい順に探すのと同じ順にして、画面に表示した番号と実際に載る枠が
// 食い違わないようにするためである。
inline int free_screen_slot(const Observation &obs)
{
	for (int slot = 1; slot <= config::kScreenSlotCount; ++slot) {
		if (screen_slot_owner(obs, slot).empty())
			return slot;
	}
	return 0;
}

// 自分の枠を使っているか (config::self_frame_in_use。自分の枠に映す映像を OBS から選んでいて、自分の行のチェックが付いて
// いる間)。映像が結び付いていない間も含む。自動で載せる経路 (pipeline/stream_router.h) にも、同じ条件の値をドックが渡す。
inline bool self_frame_in_use(const Observation &obs)
{
	return config::self_frame_in_use(obs.choices.self_frame_selected, obs.choices.self_frame_on);
}

// 今、画面共有の枠に載っている人の数を返す。
inline int screen_slotted_count(const Observation &obs)
{
	int count = 0;
	for (const PersonFact &person : obs.people) {
		if (person.slot.has_value())
			++count;
	}
	return count;
}

// 配信中で、枠に載せた人がいるか。配信中に枠の映像が損なわれる失敗を、メッセージのレベル 1 にするために使う。
// OBS が配信中かを読めていない間は false を返す (メッセージのレベル 2 のまま。読めないことを配信中とみなさない)。
// メッセージのレベルを選ぶ rules_dock.cpp と、primary の表の ConnectedThenDropped の行 (Vesktop が起動していなくても、
// 配信中で枠に人が載っていれば、この行が成り立つ) が、同じこの関数を見る。
inline bool streaming_with_slotted_person(const Observation &obs)
{
	if (!obs.obs_outputs.streaming.value_or(false))
		return false;
	return screen_slotted_count(obs) > 0;
}

// 画面共有の枠に載っている Discord の人の数。自分の枠の人の行は数えない (自分の枠を出している間は、その人の画面を
// Discord の枠に載せないため)。自分の枠を出せるかの判断 (actions.cpp の SetSelfFrame) が使う。
inline int discord_slotted_count(const Observation &obs)
{
	int count = 0;
	for (const PersonFact &person : obs.people) {
		if (person.slot.has_value() && !person.is_self_frame)
			++count;
	}
	return count;
}

// 自分の枠を使っていて、枠に載っている人数が、自分の枠を使っている間の上限 (config::screen_slot_capacity。3 人) に
// 達しているか。押せる条件 (actions.cpp の SetSlot) と、一覧の上の 1 行 (rules_dock.cpp の list_reason) がこの関数を見る。
// 自動で載せる経路 (pipeline/stream_router.h の screen_slot_room) も、同じ上限で新しい人を載せない。この Observation は最大
// 1 周期前の値なので、押した後に載せるときも、Pipeline::assign_slot が割り当て表と同じロックの中で同じ上限を見る。
inline bool self_frame_limit_reached(const Observation &obs)
{
	return self_frame_in_use(obs) && screen_slotted_count(obs) >= config::screen_slot_capacity(true);
}

// この人が今載っている画面共有の枠の番号を返す。載っていなければ 0。
inline int screen_slot_of(const Observation &obs, const std::string &user_id)
{
	if (user_id.empty())
		return 0;
	for (const PersonFact &person : obs.people) {
		if (person.user_id == user_id && person.slot.has_value())
			return *person.slot;
	}
	return 0;
}

// この人が画面共有していないと分かっているか。画面共有の事実が届いていて (sharing_known)、共有の一覧にいないときだけ真。
// Vesktop のアカウントが通話にいない間と、接続していない間は、誰の画面共有も見えないので、していないとは分からない。
// 現在の参加者に行がない人も分からない。枠に載せられるのは画面共有している人だけなので、載せる操作の可否に使う
// (actions.cpp の SetSlot と ReplaceSlot)。
inline bool person_known_not_sharing(const Observation &obs, const std::string &user_id)
{
	if (obs.link.state != LinkState::Connected || obs.call.presence == Presence::NotInCall)
		return false;
	for (const PersonFact &person : obs.people) {
		if (person.user_id == user_id)
			return person.sharing_known && !person.sharing;
	}
	return false;
}

// この人が通話の参加者の一覧にいないと分かっているか。一覧を読めていて (Call::roster_count)、そこにいない (in_roster が偽か、
// people に行がない) ときだけ真。一覧を読めていない間は、people に行がなくても分からない (people の行は、一覧のほかに、枠、
// 画面共有の事実、届いたフレームからも作るので、一覧を読めない間は、通話にいる人も行を持たないことがある)。Vesktop の
// アカウントが通話にいない間は、一覧が空なので真になる。person_known_not_sharing と同じく、分からないことを「いない」と扱わない。
inline bool person_known_not_in_roster(const Observation &obs, const std::string &user_id)
{
	if (!obs.call.roster_count.has_value())
		return false;
	for (const PersonFact &person : obs.people) {
		if (person.user_id == user_id)
			return !person.in_roster;
	}
	return true;
}

// この人を全画面にできるか (合成のソースのタイルに今描かれているか、画面共有しているか)。
inline bool focus_candidate(const Observation &obs, const std::string &user_id)
{
	if (user_id.empty())
		return false;
	for (const std::string &candidate : obs.choices.focus_candidates) {
		if (candidate == user_id)
			return true;
	}
	return false;
}

// この userId に「自動で載せる」が付いているか。Seen には今通話に参加している人も入るので、現在の参加者の
// 行もここを見れば値が分かる。Seen に行がなければ付いていない (この設定は seen_people.json にしかない)。
inline bool always_on_for(const Observation &obs, const std::string &user_id)
{
	for (const SeenPerson &person : obs.seen.people) {
		if (person.user_id == user_id)
			return person.always_on;
	}
	return false;
}

// ---- 使う声 ----
//
// 使う声の設定は seen_people.json の voice_source_user_id にしかない。空の値と、本人を指す値は、「置き換えて
// いない」と同じに扱う (override_present() と同じ約束。空の userId を渡す操作は「本人の声に戻す」なので、
// 空の値が残っていても置き換えていない)。
//
// 読む関数は 2 種類あり、使い分けに理由がある。
//   (a) ファイルに書かれた値そのものを読む関数 (voice_source_marks、voice_source_of、voice_source_owner_of)。
//       押せる条件 (actions.cpp) と「本人の声に戻す」の操作が見る。手で書き換えて食い違った値でも、戻す操作は
//       表示しなければならない (戻せない値が残ると、利用者が先に進めなくなる)。
//   (b) 経路に使える組 (usable_voice_sources。config::usable_voice_links で 1 対 1 に絞った組)。画面の
//       「声: %1」が見る。経路 (pipeline/stream_router.h) と話している人の光 (dock/build_observation.cpp) が同じ
//       関数で絞るので、画面に表示する声と実際に流れる声が食い違わない。
// 画面から作れる値は actions.cpp が 1 対 1 に限るので、ふつうは (a) と (b) が一致する。

// seen_people.json の値から「声を置き換える人 -> 使う声の人」の組を作る (ファイルに書かれた値のまま)。同じ
// userId の行が 2 つあれば、先の行を使う (always_on_for と同じ)。
inline std::map<std::string, std::string> voice_source_marks(const std::vector<SeenPerson> &people)
{
	std::map<std::string, std::string> out;
	for (const SeenPerson &person : people) {
		if (person.user_id.empty() || !person.voice_source_user_id.has_value())
			continue;
		const std::string &source = *person.voice_source_user_id;
		if (source.empty() || source == person.user_id)
			continue;
		out.emplace(person.user_id, source);
	}
	return out;
}

// この人に今付いている、使う声の人の userId を返す (ファイルに書かれた値のまま)。なければ空。
inline std::string voice_source_of(const Observation &obs, const std::string &owner)
{
	const std::map<std::string, std::string> marks = voice_source_marks(obs.seen.people);
	const auto found = marks.find(owner);
	return found == marks.end() ? std::string() : found->second;
}

// この人の声を使っている人のうち、except 以外の 1 人の userId を返す (ファイルに書かれた値のまま)。なければ空。
// 「その声はほかの行で使っている」と「この人の声を誰かが使っている」の 2 つの押せない理由 (actions.cpp) が
// 見る。
inline std::string voice_source_owner_of(const Observation &obs, const std::string &source,
					 const std::string &except = std::string())
{
	if (source.empty())
		return std::string();
	for (const auto &[owner, linked] : voice_source_marks(obs.seen.people)) {
		if (linked == source && owner != except)
			return owner;
	}
	return std::string();
}

// 経路に使える組を返す (config::usable_voice_links で 1 対 1 に絞った組)。
inline std::map<std::string, std::string> usable_voice_sources(const Observation &obs)
{
	return config::usable_voice_links(voice_source_marks(obs.seen.people));
}

// Vesktop の再起動が必要な状態か。cordmux が Discord 側を書いた時刻が、今動いている Vesktop の開始時刻より後
// なら、その Vesktop は書き換える前の設定で起動している。primary の表の RestartVesktop の行がこの関数を見る。
inline bool restart_needed(const Observation &obs)
{
	return obs.install.discord_side_written_at_us.has_value() && obs.install.vesktop_started_at_us.has_value() &&
	       *obs.install.discord_side_written_at_us > *obs.install.vesktop_started_at_us;
}

// Vesktop のアカウントの userId。Observation::vesktop_user_id を先に見る (通話の外でも分かる)。なければ、接続している間の人と、
// 接続が切れる前に分かっていた人の順に探す。分からなければ空。
inline std::string vesktop_account_id(const Observation &obs)
{
	if (!obs.vesktop_user_id.empty())
		return obs.vesktop_user_id;
	for (const PersonFact &person : obs.people) {
		if (person.is_receiver)
			return person.user_id;
	}
	for (const PersonFact &person : obs.last_known_people) {
		if (person.is_receiver)
			return person.user_id;
	}
	return std::string();
}

// サブアカウントを使う構成か (メインアカウントの userId が空でなく、Vesktop のアカウントと違う)。
// Vesktop のアカウントが分かっていない間は、メインアカウントの userId だけで決める。
inline bool has_sub_account(const Observation &obs)
{
	const std::string &main = obs.choices.broadcaster_user_id;
	return !main.empty() && main != vesktop_account_id(obs);
}

// メインアカウントが、Vesktop のアカウントと同じ通話の参加者の一覧にいるか。
inline bool main_account_in_roster(const Observation &obs)
{
	for (const PersonFact &person : obs.people) {
		if (person.user_id == obs.choices.broadcaster_user_id)
			return person.in_roster;
	}
	return false;
}

// 押した [移動] か [退出] の結果が出たか。[移動] はメインアカウントが参加者の一覧に入ったとき (Vesktop のアカウントが
// メインアカウントの通話に参加している)、[退出] は Vesktop のアカウントが通話に参加していなくなったとき。押していなければ false。
inline bool call_action_done(const Observation &obs)
{
	if (!obs.ops.call_action.has_value())
		return false;
	switch (obs.ops.call_action->kind) {
	case CallActionKind::Move:
		return main_account_in_roster(obs);
	case CallActionKind::Leave:
		return obs.call.presence == Presence::NotInCall;
	}
	return false;
}

// 押した [移動] か [退出] の結果を見るのをやめてよいか (結果が出た、または接続が切れて見られなくなった)。DockPoll が
// 押した記録を消すかの判断に使う。
inline bool call_action_finished(const Observation &obs)
{
	return obs.ops.call_action.has_value() && (obs.link.state != LinkState::Connected || call_action_done(obs));
}

// 押した [移動] か [退出] で通話が変わらなかったか。押してから call_action_timeout_us たっても結果が出ていないとき。接続している間だけ
// 見る。もう一度押すと、押し直した時刻から数え直す。
inline bool call_action_failed(const Observation &obs)
{
	const std::optional<CallActionPress> &press = obs.ops.call_action;
	if (!press.has_value() || obs.link.state != LinkState::Connected || call_action_done(obs))
		return false;
	return obs.now_us - press->pressed_at_us >= obs.ops.call_action_timeout_us;
}

// 枠のチェックを押して載せられなかったことの返事 (その人の行のそばの「空いている枠がありません。」。
// OperationFacts::last_slot_refused_user_id) を終えてよいか。返事は押した操作に答えるもので、押した後に原因 (空いている枠が
// 1 つもない) が途切れずに続き、その人の行のチェックが押したときと同じ意味の間だけ出す。次のどれかで終える。
//   - その人が枠に載った (どの経路でも。押した操作の結果が出た)
//   - 空いている枠ができた (「空いている枠がありません」が事実でなくなった)
//   - その人が画面共有していないと分かったか、参加者の一覧にいないと分かった (チェックを押せない理由が別のものになった。
//     Vesktop のアカウントが通話を抜けたときも、一覧が空になるので終える)
//   - 接続が切れた (結果を見られない。[移動] と [退出] と同じ)
// 分からない間は終えない。画面共有の事実も参加者の一覧も読めない間は、people にその人の行がないことがあるが、それは通話を
// 抜けたことを表さない (person_known_not_sharing と person_known_not_in_roster)。読めるようになって、まだ枠が埋まっていれば
// 返事を出し続ける。
// 終えた返事は DockPoll が忘れる (settle_operation_facts) ので、後でまた枠が埋まっても出さない。後で埋まったことは、押した操作と
// 関係がない。行に出すか (rules_dock.cpp の refused_note) も、この関数で決める。押していなければ false。
inline bool slot_refusal_finished(const Observation &obs)
{
	const std::string &user_id = obs.ops.last_slot_refused_user_id;
	if (user_id.empty())
		return false;
	if (obs.link.state != LinkState::Connected)
		return true;
	if (screen_slot_of(obs, user_id) != 0 || person_known_not_in_roster(obs, user_id) ||
	    person_known_not_sharing(obs, user_id))
		return true;
	return free_screen_slot(obs) != 0;
}

// 返事を終えた操作の結果 (押した [移動] と [退出]、載せられなかった枠のチェック) を忘れた OperationFacts を返す。DockPoll が
// 周期ごとに、Observation を作った直後に通し、返った値を持ち続ける。忘れないと、後で状態が変わったときに、前に押した操作の
// 返事が出る ([移動] を失敗と誤る、しばらく前に押した人の行に「空いている枠がありません。」が出る)。
inline OperationFacts settle_operation_facts(const Observation &obs)
{
	OperationFacts out = obs.ops;
	if (call_action_finished(obs))
		out.call_action.reset();
	if (slot_refusal_finished(obs))
		out.last_slot_refused_user_id.clear();
	return out;
}

// アンインストールを始めた後か。始めた後は、OBS を終了するまで何も書き込まない。primary の表の Uninstalled の
// 行、notices の表の行を表示しない条件、押せる条件の表の 3 か所が、この関数を見る (同じ判断を 2 か所に書かない
// ため)。途中で止まったとき (Stopped) も始めた後に数える。設定もソースも一部は削除されていて、書き込みを
// 再開すると、残るものが増えるためである。
inline bool uninstall_started(const Observation &obs)
{
	return obs.uninstall.phase != UninstallPhase::NotStarted;
}

} // namespace cordmux::ledger

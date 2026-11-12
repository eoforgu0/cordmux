// SPDX-License-Identifier: Apache-2.0

// 表示の規則のエントリポイント。Observation からドックの View を作る decide_dock() と、その条件を持つ 2 つの
// 表 (primary と notices) を宣言する。同じ Observation を渡せば必ず同じ View を返し、時計もファイルも OS も読まない
// (時刻と時差は Observation から受け取る)。メッセージは現在の状態だけから作り、閉じる操作もないので、状態が直れば次の
// decide_dock() で消える。表が持つのは行ごとの条件だけで、テキスト・ボタン・メッセージのレベルは rules_dock.cpp の
// decide_dock() が決める。表の行数と行ごとの条件も公開する。テストが行ごとの条件を確認し、行を追加してテストを
// 追加し忘れたことを行数の比較で見つけるためである。libobs にも Qt にも依存しない。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ledger/dock_view.h"
#include "ledger/observation.h"

namespace cordmux::ledger {

// primary の表の行。primary は、表を上から見て最初に条件が成り立った 1 行である (decide_dock() はこの行から
// 1 行目かメッセージを作る)。優先順位は rules.cpp の kPrimaryRules の並びで決まり、列挙子の並びとは一致しない。
enum class PrimaryRuleId {
	// アンインストールを始めた後。表の先頭に置く理由は rules.cpp の kPrimaryRules にある。
	Uninstalled,
	FeatureOff,               // 機能を OFF にしている (異常として扱わない)
	OtherObsHolding,          // 別の OBS が cordmux を使っている
	ListenPortsExhausted,     // 待ち受けを始められない
	Connected,                // Vesktop と接続している
	VencordOverwritten,       // 同梱の Vencord のファイルが配布時と違う (Vencord の更新で上書きされたときなど)
	VencordDirElsewhere,      // Vesktop の設定が別の場所を指している
	VencordDirNotConfigured,  // Vesktop の設定に vencordDir がまだない
	RestartVesktop,           // Vesktop の設定を書いた後に、Vesktop が再起動していない
	ConnectedThenDropped,     // 接続した後に切れた
	VesktopChecking,          // Vesktop が起動しているかを、まだ確認していない
	VesktopNotRunning,        // Vesktop が起動していない
	VesktopRunningNotLinked,  // Vesktop は起動しているが、接続してこない
	// 接続のための情報のファイルを保存できなかった。表では Connected の次に置く (理由は rules.cpp の
	// kPrimaryRules にある)。
	SessionFileNotSaved,
};

// notices の表の行。primary と違い、条件が成り立った行をすべてメッセージにする。表の並びは優先順位ではない。
// ledger/notice_tracker.h の stabilize() がメッセージのレベルと表示した時刻で並べ直すので、この並びが残るのは、
// 同じ周期に表示した、メッセージのレベルが同じメッセージの間だけである。1 つの行が 0 個以上のメッセージを作る。
enum class NoticeRuleId {
	ContactUnavailable, // Discord の内部への依存箇所のどれかが見つからないか、使えなくなった
	CompositeAbsent,    // このシーンコレクションに cordmux のソースがない
	RecordingOnly,      // 枠に載せていない人の映像を、録画のためだけに受け取っている
	InstallMismatch,    // 接続している間に、Vesktop の設定か同梱の Vencord のファイルが食い違っている
	// バージョンの食い違い。比べる組ごとに修正する場所が違うので、2 行に分ける。
	VersionRestartVesktop, // 同梱の cordmux と Vesktop の中で動いている cordmux が違う (Vesktop の再起動で直る)
	VersionMixedBundle,    // cordmux.dll と同梱の cordmux が違う (配布用の zip の中身が混ざっている)
	DoubleAudio,        // OBS が Discord の音声を別のソースでも拾っていて、声が二重に入りうる
	BroadcasterNotInCall, // メインアカウントが通話に参加していない
	AllVideoStoppedHint,  // 枠か録画の対象の 2 人以上の映像が届かず、対象の中に届いている人がいない
	RecordSpaceLow,         // 保存先の残り容量で録画できる時間が少ない
	RecordFinalizeShort,    // 録画ファイルの作成に使う空き容量が足りない見込み
	RecordFinalizeRunning,  // 録画を作成している
	RecordFinalizePending,  // 録画ファイルの作成が終わっていない録画がある
	RecordNotReachedSender, // 録画の状態が送信側に届いていない
	RecordDirUnwritable, // 保存先に書き込めなかった
	RecordStoppedSpaceExhausted, // 空き容量が尽きて録画が止まった
	ReinstallFailed, // 再インストールでコピーできなかったファイルがある
	RecordFinalizeFailed, // 録画ファイルの作成に失敗した
};

// Observation からドックの View (ledger/dock_view.h) を作る。時計も OS も読まない純粋関数である。
// 返すのは、メッセージを表示するまでの待ちと消すまでの待ちを通す前の View である。待ちと表示した順の並べ直しは、
// ledger/notice_tracker.h の stabilize() が後から行う (前の状態を持つのは dock/dock_poll.h の DockPoll)。
dockview::DockViewV2 decide_dock(const Observation &obs);

// 参加者タブの一覧の場所にも表示する、最初の設定のメッセージ ([Vesktopに追加] など)。view は ledger::stabilize の後の値。
// 一覧に行がない間だけ返す (行がないので、カードが出ても何も動かない)。メインの行だけが残る構成でも、一覧は空とみなす。
std::vector<dockview::Notice> setup_notices_of(const dockview::DockViewV2 &view);

// ---- decide_dock() だけが使う規則 ----
//
// 操作の失敗 (次に成功すると消える) と、既に起きた損失 (録画の途切れなど) のメッセージ。primary と notices の表には
// 入れない。2 つの表は現在の状態の事実だけで条件が決まる行を持ち、こちらはドックの操作の結果 (Observation::ops)
// と、既に起きた損失を見るためである。decide_dock() は、この規則のメッセージを notices の表のメッセージの後ろに並べる。
enum class AddedRuleId {
	SettingsUnreadAtStartup, // 起動のときに cordmux の設定を読めなかった
	SettingsUnsaved,         // cordmux の設定を保存できていない
	SeenUnreadAtStartup,     // 起動のときに参加者の設定を読めなかった
	SeenUnsaved,             // 参加者の設定を保存できていない
	VesktopAddFailed,        // [Vesktopに追加] が失敗した
	RecordGap,               // cordmux の録画が途切れた (既に起きた損失)
	SeenDiscarded,           // 過去の参加者の設定を上限で消した (既に起きた損失)
	CallActionFailed,        // [移動] か [退出] を押したが、通話が変わらなかった
	VesktopRemoveFailed,     // [Vesktopから削除] が失敗した
};

// AddedRuleId の規則の数と、i 番目 (0 始まり) の規則。範囲の外の i には最後の規則を返す。
std::size_t added_rule_count();
AddedRuleId added_rule_id_at(std::size_t index);

// 規則の英語の名前 (列挙子の名前と同じ文字列)。メッセージを表示したときと消したときに書くログの行と、
// dockview::Notice::rule_name に使う。ログの行は英語で書くので、ini を通さない。列挙にない値には
// "UnknownPrimaryRule" などの名前を返し、nullptr は返さない。
const char *rule_log_name(PrimaryRuleId id);
const char *rule_log_name(NoticeRuleId id);
const char *rule_log_name(AddedRuleId id);

// メッセージのレベルから、ログに書くときのログレベルを決める。Tier::L1 は Error、Tier::L2 は Warning、Tier::L3 は
// Info にする (CODESTYLE.md の「OBS のログ」)。
dockview::LogLevel rule_log_level(dockview::Tier tier);

// ---- 表を読む関数 ----

// primary の表の行数と、i 番目 (0 始まり) の行の id。範囲の外の i には最後の行の id を返す。
std::size_t primary_rule_count();
PrimaryRuleId primary_rule_id_at(std::size_t index);

// その行の条件が、obs の状態で成り立つか。
bool primary_rule_matches(PrimaryRuleId id, const Observation &obs);

// 表を上から見て最初に条件が成り立った行。どの行も成り立たないときは VesktopChecking を返す (VesktopProcess が
// どの値でも VesktopChecking・VesktopNotRunning・VesktopRunningNotLinked のどれかが成り立つので、普通は起きない)。
PrimaryRuleId primary_rule_of(const Observation &obs);

// notices の表の行数と、i 番目 (0 始まり) の行の id。範囲の外の i には最後の行の id を返す。
std::size_t notice_rule_count();
NoticeRuleId notice_rule_id_at(std::size_t index);

// その行の条件が、obs の状態で成り立つか。表にない id には false を返す。
bool notice_rule_fires(NoticeRuleId id, const Observation &obs);

// 今、cordmux が送信側に映像を送らせると決めているか。pipeline/pipeline.h の decide_media が送信側に伝える
// media の値と同じ結論を、Observation の値から出す。機能を OFF にしている間は false。録画が ON の間は、ソースが
// なくても録画のために受け取るので true。それ以外は、現在のシーンコレクションに cordmux のソースがないと分かって
// いる (composite_checked が true で composite_present が false) ときだけ false にする。調べ終えていない間
// (シーンの読み込みの前と切り替えの途中) は、ソースがないと断定せず true のままにする。
// false の間は映像が届かないのが当然なので、映像が届かないメッセージと、人の行の「映像を待っています...」などは出さない。
bool video_expected(const Observation &obs);

// 映像の待ち時間を数え始める時刻。since は、その映像が届くはずだった最後の時刻 (最後のフレームの時刻か、画面共有
// していると分かった時刻) で、受け取ると決めている状態に戻った時刻 (Observation::video_expected_since_us) が
// それより後なら、そちらを返す。
std::int64_t video_wait_from(const Observation &obs, std::int64_t since_us);

// ---- 人の映像の届き方 ----

// その人の映像の、最後のフレームの時刻。
std::optional<std::int64_t> video_time(const PersonFact &person);

// 届いていた映像が、しきい値を超えても届かない。1 度も届いていない人は含めない。
bool stalled_video(const Observation &obs, const PersonFact &person);

// 画面共有していると分かってから待ち時間がたっても、最初のフレームが 1 枚も届かない。
bool first_frame_missing(const Observation &obs, const PersonFact &person);

// cordmux の録画の対象か。
bool record_target(const Observation &obs, const PersonFact &person);

// 録画のためだけに受け取っている人か (メッセージの RecordingOnly と、状態タブのそのメッセージの数)。枠に載っておらず、録画の
// 入力に映像が今届いている人で、全画面にしている人 (枠に載っていなくても配信に映っている) は除く。
bool recording_only_person(const Observation &obs, const PersonFact &person);

// エンコードの段階が unavailable か。
bool stage_unavailable(const PersonFact &person);

// 映像が届かない人を数えた結果。数えるのは、枠に載っているか cordmux の録画の対象で、届いていた映像が止まった人と、最初の
// フレームが届かない人である。notices の表の AllVideoStoppedHint の条件と、そのメッセージの本文の選び方が読む。
struct NoVideoTally {
	int count = 0;
	bool any_slotted_stalled = false; // 数えた人に、枠に載っていて届いていた映像が止まった人がいる
	bool any_slotted_waiting = false; // 数えた人に、枠に載っていて最初のフレームがまだない人がいる
	bool any_recorded = false;        // 数えた人に cordmux の録画の対象の人がいる (枠に載っているかを問わない)
	bool any_cant_show = false;       // 数えた人に、エンコードの段階が unavailable の人がいる
	bool any_other = false;           // 数えた人に、段階が unavailable でない人がいる (段階がない人などを含む)
	bool others_arriving = false;     // 数えた人のほかに、枠か録画の対象で、映像が今届いている人がいる
};

NoVideoTally no_video_tally(const Observation &obs);

// ---- 値を文字列にする純粋関数 ----

// エポックのマイクロ秒と時差 (秒) から `10:32:04` の形の文字列を作る。負の時刻は秒の単位に切り下げる。
// 時計も OS の時差の設定も読まない (<ctime> を使わない)。
std::string format_clock_hms(std::int64_t epoch_us, int utc_offset_sec);

// 受け取っている fps を表示する文字列にする。整数との差が 0.05 未満なら整数 (`30`)、それ以外は小数 1 桁 (`15.5`)。
// 0 以下、NaN、0.05 未満の値は、分からない値として `-` を返す。
std::string format_fps(double fps);

// 日時を年・月・日・時・分の 5 つの文字列にする。並べ方と区切りの文字は呼び出し元が決める。
// format_clock_hms() と同じく、時計も OS の時差の設定も読まない。
struct DateTimeParts {
	std::string year;   // 4 桁以上
	std::string month;  // 2 桁 (01-12)
	std::string day;    // 2 桁 (01-31)
	std::string hour;   // 2 桁 (00-23)
	std::string minute; // 2 桁 (00-59)
};

DateTimeParts format_date_time_parts(std::int64_t epoch_us, int utc_offset_sec);

} // namespace cordmux::ledger

// SPDX-License-Identifier: Apache-2.0

// decide_dock() (rules.h) の実装。Observation から、ドック・設定ウィンドウ・参加者の設定ウィンドウ・アンインストールの
// 結果のダイアログに表示する View (ledger/dock_view.h) を作る。条件は primary と notices の表 (rules.cpp) を使い、
// ここではメッセージのレベル・テキスト・表示する場所を決める。表と違う条件でメッセージを作る行は、その行のコメントに
// 理由を書く。テキストはすべて text::keys のキーで持つ。日時は dockview::time_arg() などで渡し、表示の形は
// 表示する側 (dock/ui/text_resolve.cpp) が決める。値を読めないときは「-」を入れず、キーの別の形 (*_no_time
// など) を選ぶ。時計も OS も読まず、今日かどうかは Observation の now_us と utc_offset_sec で決める。

#include "ledger/rules.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ledger/actions.h"
#include "ledger/audio_tab.h"
#include "ledger/natural_order.h"
#include "ledger/notice_tracker.h"
#include "ledger/plural.h"
#include "ledger/slot_cells.h"
#include "ledger/stream_record.h"
#include "ledger/text_ids.h"

namespace cordmux::ledger {

namespace {

using dockview::DockViewV2;
using dockview::EmptyKind;
using dockview::EmptyList;
using dockview::FirstLine;
using dockview::FirstLineKind;
using dockview::LeftoverGroup;
using dockview::ListReason;
using dockview::LocationKind;
using dockview::LocationLine;
using dockview::LogLevel;
using dockview::RecordButtonState;
using dockview::RecordButtonView;
using dockview::RowState;
using dockview::RowStatus;
using dockview::Settle;
using dockview::StageItem;
using dockview::Tier;
using dockview::UninstallResultKind;
using dockview::UninstallResultView;
using dockview::ViewButton;
using dockview::VoiceChoice;
namespace keys = text::keys;

constexpr std::int64_t kUsPerSec = 1'000'000;
constexpr std::int64_t kSecPerDay = 86'400;

// 録画のボタンを開始か停止の処理中のテキストに替えるまでの時間。1 秒までは前のテキストのままにする。普通は
// ドックの 1 周期 (500 ms) の間に録画の状態が届くので、すぐ替えるとテキストが 2 度ちらつく。
constexpr std::int64_t kRecordLabelHoldUs = 1'000'000;

TextRef ref(const char *id, std::vector<std::string> args = {})
{
	TextRef t;
	t.id = id;
	t.args = std::move(args);
	return t;
}

// 4 桁以上の数を 3 桁ごとにカンマで区切る。年・解像度・ポート番号・ID・バージョンには使わない
// (docs/ui_guidelines.md の「表記」)。
std::string group_digits(long long value)
{
	const bool negative = value < 0;
	unsigned long long magnitude = negative ? 0ULL - static_cast<unsigned long long>(value)
						: static_cast<unsigned long long>(value);
	std::string digits = std::to_string(magnitude);
	std::string out;
	const std::size_t n = digits.size();
	for (std::size_t i = 0; i < n; ++i) {
		out += digits[i];
		const std::size_t rest = n - 1 - i;
		if (rest > 0 && rest % 3 == 0)
			out += ',';
	}
	return negative ? "-" + out : out;
}

std::string count_text(long long value)
{
	return group_digits(value);
}

// 残りの秒を「約 N 分」の N にする。残りを多めに表示しないように、切り捨てる。
long long minutes_floor(std::int64_t seconds)
{
	if (seconds < 0)
		seconds = 0;
	return static_cast<long long>(seconds / 60);
}

// 経過の us を「N 分」の N にする (切り捨て)。
long long minutes_of_us(std::int64_t us)
{
	if (us < 0)
		us = 0;
	return static_cast<long long>(us / (60 * kUsPerSec));
}

// 途切れた長さを「N 分間」の N にする。1 秒の途切れを「0 分間」と表示しないように、切り上げる。
long long minutes_ceil_of_us(std::int64_t us)
{
	if (us <= 0)
		return 0;
	const std::int64_t minute = 60 * kUsPerSec;
	return static_cast<long long>((us + minute - 1) / minute);
}

// バイト数を「1.5 GB」「512.0 MB」の文字列にする。1024 の累乗で、小数 1 桁にする (エクスプローラーと同じ数になる)。
// 小数 1 桁へは切り上げる。この文字列は足りない量にだけ使い、少なめに表示すると、空けた後もまだ足りないことがある。
// 単位は記号で、日本語と英語で同じなので、ini を通さない。
std::string byte_size_1024(std::uint64_t bytes)
{
	constexpr std::uint64_t kMi = 1024ULL * 1024ULL;
	constexpr std::uint64_t kGi = kMi * 1024ULL;
	const bool giga = bytes >= kGi;
	const std::uint64_t unit = giga ? kGi : kMi;
	// 十分の一の単位で切り上げる。
	const std::uint64_t tenths = (bytes * 10ULL + unit - 1ULL) / unit;
	return group_digits(static_cast<long long>(tenths / 10ULL)) + "." + std::to_string(tenths % 10ULL) +
	       (giga ? " GB" : " MB");
}

// 時差を足した時刻の日付を、1970-01-01 からの日数で返す。2 つの時刻が同じ日かを比べるのに使う。時計は読まない。
std::int64_t local_day(std::int64_t epoch_us, int utc_offset_sec)
{
	std::int64_t seconds = epoch_us / kUsPerSec;
	if (epoch_us % kUsPerSec < 0)
		--seconds;
	seconds += utc_offset_sec;
	std::int64_t day = seconds / kSecPerDay;
	if (seconds % kSecPerDay < 0)
		--day;
	return day;
}

// テキストの %N に入れる時刻の引数。今日の時刻なら時刻だけにし、今日でない日の時刻には日付も付ける。
std::string when_arg(const Observation &obs, std::int64_t epoch_us)
{
	if (local_day(epoch_us, obs.utc_offset_sec) == local_day(obs.now_us, obs.utc_offset_sec))
		return dockview::time_arg(epoch_us);
	return dockview::date_time_arg(epoch_us);
}

// 画面に表示する人の名前。cordmux での名前、Discord での名前、userId の順に、最初にあるものを使う
// (PersonFact::display_name は、cordmux での名前の上書きを反映した後の値である)。
std::string person_name(const PersonFact &person)
{
	if (person.display_name.has_value() && !person.display_name->empty())
		return *person.display_name;
	return person.user_id;
}

std::string seen_name(const SeenPerson &person)
{
	if (override_present(person))
		return *person.name_override;
	if (!person.display_name.empty())
		return person.display_name;
	return person.user_id;
}

const PersonFact *find_person(const Observation &obs, const std::string &user_id)
{
	for (const PersonFact &person : obs.people) {
		if (person.user_id == user_id)
			return &person;
	}
	return nullptr;
}

const SeenPerson *find_seen(const Observation &obs, const std::string &user_id)
{
	for (const SeenPerson &person : obs.seen.people) {
		if (person.user_id == user_id)
			return &person;
	}
	return nullptr;
}

// userId から画面に表示する名前。通話に参加していれば現在の名前、参加していなければ過去の参加者の名前、どちらも
// なければ userId にする。
std::string name_of_user(const Observation &obs, const std::string &user_id)
{
	if (const PersonFact *person = find_person(obs, user_id))
		return person_name(*person);
	if (const SeenPerson *seen = find_seen(obs, user_id))
		return seen_name(*seen);
	return user_id;
}

// 使う声のアカウントの名前。通話に参加したり抜けたりしても表示が変わらないように、過去の参加者の一覧の名前を先に
// 使う。一覧になければ通話の名前、それもなければ userId にする。
std::string voice_source_name(const Observation &obs, const std::string &user_id)
{
	if (const SeenPerson *seen = find_seen(obs, user_id))
		return seen_name(*seen);
	if (const PersonFact *person = find_person(obs, user_id))
		return person_name(*person);
	return user_id;
}

bool in_roster(const Observation &obs, const std::string &user_id)
{
	const PersonFact *person = find_person(obs, user_id);
	return person != nullptr && person->in_roster;
}

// 「配信中で、枠に人を載せている」は observation.h の streaming_with_slotted_person() を使う (primary の表の
// ConnectedThenDropped の行と同じ関数)。

// 配信中で、枠の映像が今損なわれているか。損なわれていれば、操作の失敗をメッセージのレベル 1 にする。
// streaming_with_slotted_person() に「Vesktop と接続していない」を加えた条件である。接続している間に
// [Vesktopに追加] や [再インストール] が失敗しても、現在の受け取りは続き、次に Vesktop を起動するまで何も
// 損なわれないので、メッセージのレベル 2 にする (何も損なわれていないときに、メッセージのレベル 1 にしない)。
// VencordOverwritten と VencordDirElsewhere は primary の表で Connected より下にあり、接続していないときだけ
// primary になるので、この条件で同じ意味になる。
bool streaming_with_impaired_frames(const Observation &obs)
{
	return obs.link.state != LinkState::Connected && streaming_with_slotted_person(obs);
}

LogLevel level_of(Tier tier)
{
	return rule_log_level(tier);
}

// 1 行目の短い形 (Notice::headline_short) を、見出しのキーから引く表。1 行目に表示する見出し (メッセージのレベル 1 と
// 2) のうち、ドックの 1 行目の幅 (20 em ほど) に全文が入らず、後ろを省くと問題を言う語 (否定、「上書き」、「違い」)
// が落ちる見出しだけが持つ。日本語は否定が文末にあるので、後ろを省くと逆の意味に読めることがある。keep_args は、
// 見出しの %N を短い形にも渡すか (短い形が同じ %1 を持つときだけ真)。テキストの決め方は text_ids.h の、1 行目の
// 短い形のキーのコメントにある。
// 規則ごとではなく見出しのキーごとの表にするのは、同じ見出しを使う所が複数あるためである。メッセージを作る関数は
// make_notice() の 1 つで、上書きの見出しは VencordOverwritten と ReinstallFailed の 2 つの規則が使い、
// [Vesktopに追加] の失敗のカードは merge_failure_cards() で状態のカードの代わりになる。規則ごとに書くと、同じ見出し
// なのに短い形が付く所と付かない所ができる。
// 短い形を持たない見出しと、その理由は次のとおりである。
//   - 後ろを省いても、前に問題が残る: 別の OBS (「別のOBSでcordmuxが動...」)、空き容量
//     (「保存先の空き容量が少なく...」)、再起動 (命令の文)、アンインストールの完了 (問題ではない)。英語の
//     "Can't ..." "Couldn't ..." "Not ..." で始まる見出し。
//   - 全文が 20 em に入る: voice-select の依存箇所 (「[移動] と [退出] を使えません」)、接続の情報を保存できなかった
//     (「Vesktopから接続できません」)。フォントによっては 1 字
//     はみ出すが、safe_cut が「Vesktopから...」で切る。述語だけの短い形 (「接続できません」) は、狭い幅で切る位置に
//     戻れる助詞がない (次の項目)。
//   - 日本語の短い形の否定の文は、述語の前に助詞を 1 つ残す。述語だけの形 (「追加されていません」) は、15 em や
//     230 px の幅で safe_cut が戻る位置を持たず、Qt の省略が「追加されていま...」(逆の意味) を作る。
//   - 短い自然な文にすると事実が欠ける: Discord の内部への依存箇所の media・sharing・ssrc (「新しい」を落とすと、
//     現在の画面共有まで止まったと読める)、profile (20 em に入る文では内容が欠ける)、call (通話の名前と [移動] の 2 つが
//     使えないことを 1 文で言うので、どちらを落としても事実が欠ける)、参加者の設定の 2 つ
//     (「参加者の設定」だけで 20 em の半分になる)。
//   - cordmux を落とせない (OBS にも同じものがある): cordmux の録画を開始できない (「録画を開始できませんでした」は
//     OBS 自身の録画の失敗と同じテキストになる)、cordmux の設定の 2 つ、cordmux のシーンの作成の失敗。
//   これらの見出しでは、safe_cut の日本語の否定のガードと英語の短縮形のガード (dock/ui/dock_ui_calc.h) が、逆の
//   意味に読める省略を防ぐ。
// 1 行目のテキストを折り返さないのは、1 行目の高さが変わるためである。見出しの全文の語順を変えないのは、日本語の
// 文として不自然になるためである。
struct ShortHeadline {
	const char *headline;
	const char *short_form;
	bool keep_args;
};

const ShortHeadline kShortHeadlines[] = {
	{keys::kDockRecordStoppedSpaceHeadline, keys::kDockRecordStoppedSpaceShortHeadline, false},
	// 短い形は、時刻を落とした見出しのテキストそのもの (後ろの区切りから丸ごと落とす。新しいキーは作らない)。
	// 「接続が切れました」まで落とさないのは、配信中のメッセージのレベル 1 で、OBS の配信の接続が切れたと読める
	// ためである。
	{keys::kDockDisconnectedHeadline, keys::kDockDisconnectedNoTimeHeadline, false},
	{keys::kDockVesktopNotAddedHeadline, keys::kDockVesktopNotAddedShortHeadline, false},
	{keys::kDockVesktopAddFailedHeadline, keys::kDockVesktopAddFailedShortHeadline, false},
	{keys::kDockVesktopRemoveFailedHeadline, keys::kDockVesktopRemoveFailedShortHeadline, false},
	{keys::kDockVencordElsewhereHeadline, keys::kDockVencordElsewhereShortHeadline, false},
	{keys::kDockVencordOverwrittenHeadline, keys::kDockVencordOverwrittenShortHeadline, false},
	{keys::kDockVersionDiffersHeadline, keys::kDockVersionDiffersShortHeadline, false},
	{keys::kDockFilesMixedHeadline, keys::kDockFilesMixedShortHeadline, false},
	{keys::kDockVesktopNotConnectedHeadline, keys::kDockVesktopNotConnectedShortHeadline, false},
	{keys::kDockPortsInUseHeadline, keys::kDockPortsInUseShortHeadline, false},
	{keys::kDockContactRosterHeadline, keys::kDockContactRosterShortHeadline, false},
	{keys::kDockContactSelfHeadline, keys::kDockContactSelfShortHeadline, false},
	{keys::kDockCompositeAbsentHeadline, keys::kDockCompositeAbsentShortHeadline, false},
	{keys::kDockDoubleAudioHeadline, keys::kDockDoubleAudioShortHeadline, false},
	{keys::kDockMainAccountNotInCallHeadline, keys::kDockMainAccountNotInCallShortHeadline, false},
	{keys::kDockMainAccountNotInCallNoNameHeadline, keys::kDockMainAccountNotInCallShortHeadline, false},
	{keys::kDockNoVideoHeadline.other, keys::kDockNoVideoShortHeadline.other, true},
	{keys::kDockNoVideoHeadline.one, keys::kDockNoVideoShortHeadline.one, true},
	{keys::kDockFinalizeSpaceShortHeadline, keys::kDockFinalizeSpaceShortShortHeadline, true},
	{keys::kDockFinalizePendingHeadline, keys::kDockFinalizePendingShortHeadline, false},
	{keys::kDockFinalizeFailedHeadline, keys::kDockFinalizeFailedShortHeadline, false},
};

std::optional<TextRef> short_headline_of(const TextRef &headline)
{
	for (const ShortHeadline &row : kShortHeadlines) {
		if (headline.id == row.headline) {
			TextRef s;
			s.id = row.short_form;
			if (row.keep_args)
				s.args = headline.args;
			return s;
		}
	}
	return std::nullopt;
}

dockview::Notice make_notice(const std::string &rule_name, Tier tier, TextRef headline)
{
	dockview::Notice n;
	n.key = rule_name;
	n.rule_name = rule_name;
	n.tier = tier;
	n.log_level = level_of(tier);
	n.headline_short = short_headline_of(headline);
	n.headline = std::move(headline);
	return n;
}

// ボタンを 1 つ作る。押せるかどうかと押せない理由は apply() が決め、ここでは判断しない。
ViewButton make_button(const Observation &obs, ActionId id, const ActionArgs &args, const char *label_id,
		       std::vector<std::string> label_args = {})
{
	ViewButton b;
	b.id = id;
	b.label = ref(label_id, std::move(label_args));
	b.args = args;
	const Intent intent = apply(obs, id, args);
	if (intent.disabled.has_value())
		b.disabled_reason = ref(intent.disabled->reason_text_id);
	return b;
}

// Vesktop と接続していて、Vesktop のアカウント (受け取りに使うアカウント) が通話に参加していないと分かっている。
// 接続していない間と、通話の参加者をまだ読めていない間は偽 (分からないことを「参加していない」と扱わない)。
// 準備の段階の通話の行、平常の 1 行目、人の行の状態が、この 1 つの判断を見る。
bool receiver_not_in_call(const Observation &obs)
{
	return obs.link.state == LinkState::Connected && obs.call.presence == Presence::NotInCall;
}

// 準備の段階を示す「名前: 値」の行。Vesktop、通話、cordmux のソースの順に、今分かっているところまで表示する。
// 通話の行は、Vesktop と接続していないときは表示しない (Vesktop の行が既に伝えている)。cordmux のソースの行は、
// 調べていない間は表示しない (確認していないことを「追加されていません」と表示しない)。
std::vector<StageItem> stage_items(const Observation &obs)
{
	std::vector<StageItem> out;
	if (obs.link.state == LinkState::Connected)
		out.push_back({ref(keys::kDockStageVesktopConnectedStatus)});
	else if (obs.install.process == VesktopProcess::Unknown && obs.link.state == LinkState::Unknown)
		out.push_back({ref(keys::kDockStageVesktopCheckingStatus)});
	else
		out.push_back({ref(keys::kDockStageVesktopDisconnectedStatus)});

	if (obs.link.state == LinkState::Connected) {
		if (obs.call.presence == Presence::InCall && obs.call.roster_count.has_value()) {
			const int n = *obs.call.roster_count;
			out.push_back({ref(text::pick(keys::kDockStageCallJoinedStatus, n), {count_text(n)})});
		} else if (receiver_not_in_call(obs)) {
			out.push_back({ref(keys::kDockStageCallNotJoinedStatus)});
		}
	}

	if (obs.choices.composite_checked) {
		out.push_back({ref(obs.choices.composite_present ? keys::kDockStageCompositeAddedStatus
								 : keys::kDockStageCompositeNotAddedStatus)});
	}
	return out;
}

// ---- 人の映像の届き方 ----
//
// 最後のフレームの時刻、止まったかなどの判定は rules.h にある。

// 今、枠に映像を表示している人か (1 行目の「表示中」の人数に数えるか)。枠に載っていて、画面共有していて、
// しきい値の時間内にフレームが届いている人を数える。画面共有しているか分からない
// (sharing_known が偽) 間は、フレームが届いていることを画面共有している証拠として数える (古い送信側と接続した
// ときに 0 人にしないため)。
// エンコードの段階が unavailable の人は、フレームの時刻にかかわらず数えない。送信側はこの段階で映像の読み出しを
// 止め、同じストリームを確保し直さない (src-vencord/encode/session.ts) ので、unavailable が届いた時点で、枠は
// 最後に届いた画像のまま止まっていると分かる。しきい値まで待つのは、フレームが届かないのが一時的な間隔なのか、本当に
// 止まったのかを見分けられない間だけで、この事実があれば待つ必要はない。数えると、しきい値 (既定は 2 秒) までの
// 間、同じ View で行は「映像を表示できません」なのに、1 行目の「表示中 (N人)」にその人が含まれる。知らない名前の
// 段階の人は数える (下げた映像が映っている見込みで、quality_of() もそれを「画質を下げています」と表示する)。
// stalled_video() に段階の判定を入れないのは、映像が届かないメッセージの数え方まで変わるためである。行の状態のテキスト
// は、row_status_of() が段階を先に見て「映像を表示できません」にする。
bool displayed(const Observation &obs, const PersonFact &person)
{
	if (!person.slot.has_value())
		return false;
	if (person.sharing_known && !person.sharing)
		return false;
	if (stage_unavailable(person))
		return false;
	if (!person.last_video_frame_at_us.has_value())
		return false;
	return obs.now_us - *person.last_video_frame_at_us < obs.video_stall_threshold_us;
}

// 自分の枠を出している間は、自分も配信の絵に出ているので 1 人として数える (自分の行のチェックの数と、要約の人数を
// 合わせる)。自分の枠の人が Discord の画面共有で枠にも載っているときは、二重に数えない。
int displayed_count(const Observation &obs)
{
	int n = 0;
	bool self_counted = false;
	for (const PersonFact &person : obs.people) {
		if (displayed(obs, person)) {
			++n;
			if (person.is_self_frame)
				self_counted = true;
		}
	}
	if (obs.choices.self_frame_shown && !self_counted)
		++n;
	return n;
}

// ---- 規則の行ごとのメッセージ ----

// Discord の内部への依存箇所の見出し。上の行ほど、使えないときに損なわれるものが重い。id は依存箇所の一覧
// (src-vencord/ledger.ts) の id で、key は見出しのキーである。どの依存箇所でも、配信中にできることがないので
// メッセージのレベル 2 にする。
struct ContactHeadline {
	const char *id;
	const char *key;
};

const ContactHeadline kContactHeadlines[] = {
	{"pc-hook", keys::kDockContactMediaHeadline},
	{"sharing", keys::kDockContactSharingHeadline},
	{"watch-stop", keys::kDockContactSharingHeadline},
	{"ssrc-map", keys::kDockContactSsrcHeadline},
	{"roster", keys::kDockContactRosterHeadline},
	{"profile", keys::kDockContactProfileHeadline},
	{"self", keys::kDockContactSelfHeadline},
	{"call-info", keys::kDockContactCallHeadline},
	{"voice-select", keys::kDockContactVoiceSelectHeadline},
};

void contact_notice(const Observation &obs, std::vector<dockview::Notice> *out)
{
	std::set<std::string> missing;
	for (const ContactFact &contact : obs.contacts) {
		if (contact.state == ContactState::Missing || contact.state == ContactState::Lost)
			missing.insert(contact.id);
	}
	if (missing.empty())
		return;
	// この表にない id (送信側のバージョンが新しく、受信側が知らない依存箇所) は、最も重い見出しで伝える。知らない
	// 依存箇所が使えないときに何が損なわれるかを受信側は判断できないので、伝えずにおくことも、軽い見出しで伝えることも
	// しない。この扱いになるのは送信側のバージョンが違うときだけで、そのときはバージョンの食い違いのメッセージも
	// 表示される。
	const ContactHeadline *best = nullptr;
	for (const ContactHeadline &row : kContactHeadlines) {
		if (missing.count(row.id) != 0) {
			best = &row;
			break;
		}
	}
	bool unknown_id = false;
	for (const std::string &id : missing) {
		bool known = false;
		for (const ContactHeadline &row : kContactHeadlines)
			known = known || id == row.id;
		unknown_id = unknown_id || !known;
	}
	if (unknown_id)
		best = &kContactHeadlines[0];
	dockview::Notice n = make_notice("ContactUnavailable", Tier::L2, ref(best->key));
	n.body.push_back(ref(keys::kDockContactText));
	// Vesktop の画面が表示されないときの戻し方。Discord の更新に同梱の Vencord が合わないと、Vesktop の画面が黒いままに
	// なり、このメッセージが出る。cordmux の側からは修正できないので、Vesktop の設定を cordmux を追加する前の値に戻す操作
	// (状態タブの [Vesktopから削除]) を案内する。一部の依存箇所だけが見つからないとき (画面は表示される) にも同じ
	// メッセージが出るので、文は「表示されないときは」の条件付きにする。依存箇所ごとに分けないのは、画面が表示されて
	// いるかを受信側が知る方法がないためである。ボタンをこのカードに置かないのは、Vesktop を終了すると接続が切れて
	// このカードが消えるためである (dockview::StatusVesktopView のコメント)。
	n.body.push_back(ref(keys::kDockContactNoScreenText));
	n.buttons.push_back(make_button(obs, ActionId::ExportBundle, ActionArgs{}, keys::kDockExportDiagnosticsButton));
	out->push_back(std::move(n));
}

const char *vesktop_add_failure_text(VesktopAddFailure failure, bool settings_unreadable_at_startup);

// インストールの 3 行 (VencordOverwritten・VencordDirElsewhere・VencordDirNotConfigured) のメッセージ。接続している間
// (InstallMismatch) は、現在の接続は続くので、帰結を「次にVesktopを起動したときから、...」のテキストに替える。
dockview::Notice install_notice(const Observation &obs, PrimaryRuleId id, const std::string &rule_name, Tier tier,
				bool connected)
{
	const char *headline = keys::kDockVesktopNotAddedHeadline;
	ActionId action = ActionId::WriteVencordDir;
	const char *button = keys::kDockAddToVesktopButton;
	switch (id) {
	case PrimaryRuleId::VencordOverwritten:
		// 配布時と違っていたファイルの名前 (Install::mismatched_files) は、画面に表示しない。
		headline = keys::kDockVencordOverwrittenHeadline;
		action = ActionId::ReinstallVencordFiles;
		button = keys::kDockReinstallButton;
		break;
	case PrimaryRuleId::VencordDirElsewhere:
		headline = keys::kDockVencordElsewhereHeadline;
		action = ActionId::RepairVencordDir;
		break;
	default:
		break;
	}
	dockview::Notice n = make_notice(rule_name, tier, ref(headline));
	n.body.push_back(ref(connected ? keys::kDockCannotReceiveAfterRestartText : keys::kDockCannotReceiveText));
	// この起動で Vesktop の設定を書かない (Install::vesktop_writes_off) 間は、[Vesktopに追加] を置かず、
	// 押したときの失敗のカードと同じ、理由と次の操作の文を帰結の後に置く。押しても、同じ起動の中では必ず同じ
	// 失敗になるので、ボタンは置かず (押せなくして残すこともしない)、何をすればよいかを文で伝える。文は
	// vesktop_add_failure_text() の WritesDisabled と同じものを使う (settings.json で無効にした起動では、
	// 有効にして再起動するよう伝える。設定を読めなかった起動では、次の操作は同じドックの
	// 「cordmuxの設定を読み込めませんでした」のカードが伝える)。並びは失敗のカードと同じく、帰結、理由と次の操作の
	// 順にする。[再インストール] (VencordOverwritten) は残す。コピーするのは同梱の Vencord のファイルで、
	// Vesktop の設定は書かない (dock/dock_poll_install.cpp の DockPoll::reinstall_vencord_files() は
	// vesktop_writes_off を見ない)。ボタンを押せなくして理由を表示しないのは、押せない理由が、
	// 本文の理由の言い直しになるためである。
	const bool add_button = action == ActionId::WriteVencordDir || action == ActionId::RepairVencordDir;
	if (add_button && obs.install.vesktop_writes_off) {
		if (const char *why =
			    vesktop_add_failure_text(VesktopAddFailure::WritesDisabled, obs.ops.settings_unreadable_at_startup))
			n.body.push_back(ref(why));
	}
	// 準備の段階の行は、止まった段階のカードにだけ表示する。接続している間 (InstallMismatch) は、Vesktop の段階も
	// 通話の段階も動いていて、このカードは止まった段階のメッセージではない (次に起動したときに困るメッセージである)。
	// 段階の行を付けると、ほかのカード (シーン) の段階を言い直すだけになる。
	if (!connected)
		n.stages = stage_items(obs);
	if (add_button && obs.install.vesktop_writes_off)
		return n;
	ViewButton b = make_button(obs, action, ActionArgs{}, button);
	n.buttons.push_back(std::move(b));
	return n;
}

// 切断の帰結のテキスト。切断の理由のうち、修正する場所が違うものだけテキストを分ける。
const char *disconnected_text(DisconnectReason reason)
{
	switch (reason) {
	case DisconnectReason::SecretMismatch:
		return keys::kDockDisconnectedAuthText;
	case DisconnectReason::WireMismatch:
	case DisconnectReason::MalformedHello:
		return keys::kDockDisconnectedVersionText;
	case DisconnectReason::HelloTimeout:
	case DisconnectReason::HeartbeatTimeout:
	case DisconnectReason::PeerClosed:
	case DisconnectReason::Unknown:
		break;
	}
	return keys::kDockDisconnectedText;
}

// 確認できなかったシーンコレクションの名前 (重なりを除き、出てきた順)。1 つの名前が「開けなかった」と
// 「同じ名前が 2 つ以上ある」の両方に出ることがある (install/uninstall.cpp の walk_collections は、
// 現在のシーンコレクションが分からないときは全部を開かずに名前を挙げ、同じ名前のファイルの数え方では別に名前を
// 挙げる)。次の操作の文と、ダイアログのまとまりの見出しの単数と複数は、項目の数ではなく、この名前の数で選ぶ。
// 項目の数で選ぶと、1 つのシーンコレクションに「これらの」(英語では "these") が付く。ダイアログの次の操作の文
// (dock/install_dialog_plan.cpp の collection_count) と同じ数え方である。
std::vector<std::string> unchecked_collection_names(const UninstallFacts &u)
{
	std::vector<std::string> names;
	for (const UninstallLeftoverFact &left : u.leftovers) {
		if (left.kind != UninstallLeftoverKind::CollectionNotOpened &&
		    left.kind != UninstallLeftoverKind::CollectionNameDuplicate)
			continue;
		if (std::find(names.begin(), names.end(), left.where) == names.end())
			names.push_back(left.where);
	}
	return names;
}

long long unchecked_collection_count(const UninstallFacts &u)
{
	return static_cast<long long>(unchecked_collection_names(u).size());
}

// シーンコレクションのフォルダーを読めなかったか。
bool collection_folder_unreadable(const UninstallFacts &u)
{
	for (const UninstallLeftoverFact &left : u.leftovers) {
		if (left.kind == UninstallLeftoverKind::CollectionFolderUnreadable)
			return true;
	}
	return false;
}

// 確認できなかったものの次の操作。名前の分かるシーンコレクションがあれば、それを開く文 (数で単数と複数を選ぶ)。1 つも
// なければ (フォルダーを読めなかっただけ)、すべてを開く文。
TextRef unchecked_next_text(const UninstallFacts &u)
{
	const long long count = unchecked_collection_count(u);
	if (count == 0)
		return ref(keys::kDialogUninstallUncheckedAllNextText);
	return ref(text::pick(keys::kDialogUninstallUncheckedNextText, count));
}

// アンインストールで残ったもの 1 つの場所の行。結果のダイアログと、途中で止まったときのカードが使う (読む側は
// dock/install_dialog_plan.cpp と dock/notice_list.cpp)。ファイルの行 (left.is_file。アンインストールの段階が、
// 消せなかったものを見て決めた事実) は LocationKind::File にして [フォルダーを開く] を付けず、読む側が
// [ファイルの場所を開く] を置く。それ以外はフォルダーの行で、[フォルダーを開く] を付ける。
// 「元に戻せなかった設定」の注意の場所の行に入るファイルの行 (製品では起きない組み合わせ) も、同じ種類のままにする。
// ボタンは、その行のものを開けるかで選ぶためである。注意のテキストの「フォルダー」とは合わないが、行の種類で
// テキストを選び分けるキーを追加するほどではない (製品で、Vesktop の設定を元に戻す段階で止まったときの行は、
// どちらもフォルダーである)。場所の文字列から種類を推測しないのは、拡張子のないファイルも、点を含むフォルダーも
// 作れるためである。
LocationLine leftover_location(const UninstallLeftoverFact &left)
{
	if (!left.is_file)
		return LocationLine{left.where, true};
	LocationLine file{left.where, false};
	file.kind = LocationKind::File;
	return file;
}

std::optional<UninstallResultView> uninstall_result(const Observation &obs);

// アンインストールが途中で止まったときの、ドックのカードの本文。結果のダイアログを閉じた後に残るのはドックだけ
// なので、何が残ったかと、次の操作と、場所をカードにも置く。まとまり・項目・見出し・場所は、ダイアログと同じ
// uninstall_result() の結果から取り、同じことを 2 か所で決めない。カードには箇条書きの記号がないので、まとまりの
// 見出し (「削除できなかったもの:」) の後に項目を 1 段落ずつ並べ、まとまりの直後にその次の操作を置く。
// まとまりは、確認できなかったもの、元に戻せなかった設定、削除できなかったものの順に並べる。確認できなかった
// ものの次の操作は「OBSを終了する前に」で始まり、OBS を終了した後にする作業より先に読んでもらう必要がある
// (ダイアログの並び (dock/install_dialog_plan.cpp の group_rank) と同じ)。
// 場所の行は、ダイアログと同じく、項目の下にその項目の場所を置く (「cordmuxの設定のファイル」の下には設定の
// フォルダーと接続のための情報の保存先を 0 個から 2 個、「Vesktopの設定」の下には Vesktop の設定のファイル)。
// 元に戻せなかった設定の注意 (「この設定を元に戻すまで、次のフォルダーを削除しないでください。...」) の直後には、
// プラグインのフォルダーと、バックアップを残した cordmux の設定のフォルダー (UninstallFacts::settings_dir) と、
// 残りとして挙げた設定のフォルダー (uninstall_result() の caution_locations) を置く。場所の行を置けないときは、
// 注意を「cordmuxのファイルを」のテキストにする。ファイルの残りの場所の行は [ファイルの場所を開く] にする
// (leftover_location())。どれも Notice::body_locations に入れ、その段落の直後に表示する。
// 2 つ目からのまとまりの見出しの位置を Notice::body_group_starts に入れ、読む側がその前に間を空ける。
// 途中で止まったときは必ず残ったものがある (install/uninstall.cpp の run_uninstall は、残ったものがある段階で
// だけ止まる) ので、まとまりのないカードはできない。
// 場所の行をカードの最後に 1 つだけ置かないのは、削除できなかった場所が 2 つあるときに片方しか表示できず、注意が
// 指すプラグインのフォルダーも表示できないためである。まとまりの見出しを置くのは、「cordmuxの設定のファイル」
// だけでは、削除できなかったのか、元に戻せなかったのかが分からないためである。
void stopped_card_body(const Observation &obs, dockview::Notice *n)
{
	const std::optional<UninstallResultView> result = uninstall_result(obs);
	if (!result.has_value())
		return;
	const LeftoverGroup *not_deleted = nullptr;
	const LeftoverGroup *not_restored = nullptr;
	const LeftoverGroup *not_checked = nullptr;
	for (const LeftoverGroup &group : result->groups) {
		if (group.heading.id == keys::kDialogNotDeletedText)
			not_deleted = &group;
		else if (group.heading.id == keys::kDialogNotRestoredText)
			not_restored = &group;
		else if (group.heading.id == keys::kDialogNotCheckedText)
			not_checked = &group;
	}
	// 場所の行を、直前の段落の直後に置く。場所が空の行は置かない (ダイアログの install_dialog_plan.cpp と同じ)。
	const auto put_location = [n](const LocationLine &place) {
		if (place.path.empty() || n->body.empty())
			return;
		dockview::BodyLocation line;
		line.after = n->body.size() - 1;
		line.line = place;
		n->body_locations.push_back(std::move(line));
	};
	const auto put_group = [n, &put_location](const LeftoverGroup &group) {
		if (!n->body.empty())
			n->body_group_starts.push_back(n->body.size());
		n->body.push_back(group.heading);
		for (std::size_t i = 0; i < group.items.size(); ++i) {
			const Label &item = group.items[i];
			if (!item.text.has_value())
				continue;
			n->body.push_back(*item.text);
			// item_locations は空 (どの項目にも場所がない) か、items と同じ長さである
			// (dock_view.h の約束)。
			if (i < group.item_locations.size()) {
				for (const LocationLine &place : group.item_locations[i])
					put_location(place);
			}
		}
	};
	if (not_checked != nullptr) {
		put_group(*not_checked);
		n->body.push_back(unchecked_next_text(obs.uninstall));
	}
	if (not_restored != nullptr) {
		put_group(*not_restored);
		n->body.push_back(ref(keys::kDialogNotRestoredNextText));
		// 注意「...次のフォルダーを削除しないでください。...」の直後に、それが指すフォルダー
		// (終わったときのダイアログの「削除するフォルダー」と同じプラグインのフォルダーと、バックアップを残した
		// cordmux の設定のフォルダー) の場所の行を置く。テキストと場所の行は、uninstall_result() がまとまりの
		// caution と caution_locations に入れたもので、ダイアログと同じ値である (選び方は
		// uninstall_result() のコメント)。場所が分からないときは、「次の」が何も指さないテキストを表示せず、
		// 「cordmuxのファイルを削除しないでください」(dont_delete_files) にする。
		n->body.push_back(not_restored->caution.value_or(ref(keys::kDialogDontDeleteFilesText)));
		for (const LocationLine &place : not_restored->caution_locations)
			put_location(place);
	}
	if (not_deleted != nullptr) {
		put_group(*not_deleted);
		// 次の操作は、ダイアログと同じ値 (LeftoverGroup::next。uninstall_result() が選ぶ)。
		n->body.push_back(not_deleted->next.value_or(ref(keys::kDialogNotDeletedNextText)));
	}
}

// primary の表の 1 行を、1 行目の平常のテキスト (メッセージにしない行。fallback に入れる) か、メッセージ (out に追加する)
// にする。
void primary_to_view(const Observation &obs, PrimaryRuleId id, FirstLine *fallback,
		     std::vector<dockview::Notice> *out)
{
	const std::string name = rule_log_name(id);
	switch (id) {
	case PrimaryRuleId::Uninstalled:
		switch (obs.uninstall.phase) {
		case UninstallPhase::Running:
		case UninstallPhase::NotStarted:
			// 1 行目を処理中のテキストにする。メッセージのレベル 3 の行は作らない。作ると、
			// 同じテキストが 1 行目とメッセージのレベル 3 の行に並ぶ。
			fallback->kind = FirstLineKind::Working;
			fallback->forms = {ref(keys::kDockUninstallingStatus)};
			return;
		case UninstallPhase::Done: {
			const char *main = obs.uninstall.folder_to_delete.empty() ? keys::kDialogUninstallDoneLooseHeadline
										  : keys::kDialogUninstallDoneHeadline;
			// メッセージがないときの 1 行目 (fallback)。下でカードを必ず作るので、1 行目にはカードの見出しが
			// 表示され、普通はこれは表示されない。
			fallback->kind = FirstLineKind::UserChoice;
			fallback->forms = {ref(main)};
			// メッセージのレベル 2 のカードを 1 枚作る。今は何も損なわれていないが、フォルダーを削除する前に
			// OBS を再起動すると、cordmux がもう一度読み込まれる。利用者が何かをする必要があるのは、次に
			// OBS を起動する前である。結果のダイアログを閉じた後に残るのはドックだけなので、
			// 次にすることと場所をツールチップだけに置かない。
			// 見出しは何が起きたかを言い (ダイアログの主文の命令を見出しにしない)、
			// 本文は利用者がする順に並べる。確認できなかったシーンコレクションがあるときは、
			// OBS を終了する前にそれを開いて確認する段落を先に置く (名前は
			// 「開けなかったシーンコレクション: 配信用」の形で 1 行ずつ)。カードは場所の行を最後の段落の
			// 直後に置く (dock/notice_list.cpp) ので、フォルダーを削除する文を最後にして、場所の行と
			// 並べる。確認できなかったシーンコレクションの次の操作 (uninstall_unchecked_next) は、
			// アンインストールの後にシーンコレクションを開いても、cordmux がシーンとソースを作り直さない
			// ことを前提にしている。
			// アンインストールを始めたことを示すフラグ (install::is_uninstalling()。
			// OBS を終了するまでメモリに残る) を、ソースを作る処理が見て、何もしない。段落をダイアログと同じ順
			// (削除の命令が先) にしないのは、場所の行がシーンコレクションの項目の後ろに離れ、
			// どの場所かが読めなくなるためである。残りがないときもカードを作るのは、
			// 1 行目だけだと命令が途中で省略され、全文と場所がツールチップにしか残らないためである。
			const bool loose = obs.uninstall.folder_to_delete.empty();
			dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockUninstallDoneHeadline));
			n.key = name + ":Done";
			// 確認できなかったシーンコレクションは、理由の付いた項目
			// (「開けなかったシーンコレクション: 配信用」) だけを並べ、ダイアログのまとまりの見出し
			// 「次のシーンコレクションは確認できませんでした。」は置かない。カードには箇条書きの記号が
			// ないので、見出しの直後に理由の付いた項目が来ると、同じことを 2 度言う。ダイアログは
			// 箇条書きなので見出しを残す (uninstall_result())。見出しを残して項目の理由を外さないのは、
			// 理由 (開けなかった、名前が重なる) が消えるためである。
			if (!obs.uninstall.leftovers.empty()) {
				for (const UninstallLeftoverFact &left : obs.uninstall.leftovers) {
					if (left.kind == UninstallLeftoverKind::CollectionNotOpened)
						n.body.push_back(ref(keys::kDialogLeftCollectionNotOpenedItemText, {left.where}));
					else if (left.kind == UninstallLeftoverKind::CollectionNameDuplicate)
						n.body.push_back(ref(keys::kDialogLeftCollectionDuplicateItemText, {left.where}));
					else if (left.kind == UninstallLeftoverKind::CollectionFolderUnreadable)
						n.body.push_back(ref(keys::kDialogLeftCollectionFolderItemText));
				}
				// 次の操作の文は、シーンコレクションの数で選ぶ (1 つなら「この」、英語では "this")。名前の分かる
				// ものがなければ、すべてを開く文にする。
				n.body.push_back(unchecked_next_text(obs.uninstall));
			}
			if (loose) {
				n.body.push_back(ref(keys::kDockUninstallDoneLooseText));
				n.body.push_back(ref(keys::kDialogUninstallDoneLooseText));
			} else {
				n.body.push_back(ref(keys::kDockUninstallDoneText));
				n.location = LocationLine{obs.uninstall.folder_to_delete, true};
			}
			out->push_back(std::move(n));
			return;
		}
		case UninstallPhase::Stopped: {
			fallback->kind = FirstLineKind::UserChoice;
			fallback->forms = {ref(keys::kDialogUninstallStoppedHeadline)};
			dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDialogUninstallStoppedHeadline));
			n.key = name + ":Stopped";
			stopped_card_body(obs, &n);
			out->push_back(std::move(n));
			return;
		}
		}
		return;
	case PrimaryRuleId::FeatureOff: {
		// 利用者が選んだ状態。アイコンも色も付けず、有効に戻すボタン ([cordmuxを有効にする]) を同じ行に置く。
		fallback->kind = FirstLineKind::UserChoice;
		fallback->disabled = true;
		fallback->forms = {ref(keys::kDockSummaryDisabledStatus)};
		ActionArgs args;
		args.on = true;
		fallback->button = make_button(obs, ActionId::ToggleFeature, args, keys::kDockEnableButton);
		return;
	}
	case PrimaryRuleId::OtherObsHolding: {
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockOtherObsHeadline));
		n.body.push_back(ref(keys::kDockOtherObsText));
		out->push_back(std::move(n));
		return;
	}
	case PrimaryRuleId::ListenPortsExhausted: {
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockPortsInUseHeadline));
		// 本文は、理由、帰結、次の操作の順にする。理由 (ポートがすべて使用中) は、範囲を読めていて、
		// 受け付けの開始に失敗した (accept_failed) のではないときだけ置く (「-」を入れない)。
		// 受け付けの開始の失敗は、状態が PortsExhausted でも、ポートが使用中だったわけではない。
		// ポート番号は桁を区切らない。帰結は「Discordの画面共有と声を受け取れません。」
		// (cannot_receive) にする。次の操作は OBS の再起動である。待ち受けは 5 秒ごとに開き直す
		// (dock/dock_poll_config.h の maintain_listener) ので、別の OBS が終了したときなどは再起動なしで直るが、
		// メッセージが案内する操作は、確実に直る再起動にする。受け付けの開始に失敗したときに別の理由の文を
		// 追加しないのは、キーが増えるだけで、利用者にできることは同じ再起動だからである。
		if (!obs.listener.accept_failed && obs.listener.port_first.has_value() && obs.listener.port_last.has_value())
			n.body.push_back(ref(keys::kDockPortsInUseText, {std::to_string(*obs.listener.port_first),
									  std::to_string(*obs.listener.port_last)}));
		n.body.push_back(ref(keys::kDockCannotReceiveText));
		n.body.push_back(ref(keys::kDockRestartObsText));
		out->push_back(std::move(n));
		return;
	}
	case PrimaryRuleId::Connected:
		// メッセージにしない行。平常の要約は summary_line() が決める。
		return;
	case PrimaryRuleId::SessionFileNotSaved: {
		// メッセージのレベル 2 にする。Vesktop が接続できず、Discord の画面共有と声が何も届かないが、ファイルを
		// 書くのは OBS を起動したときなので、起きるのは配信の前で、配信中に枠が途中で空くことはない。同じく
		// 何も届かない OtherObsHolding・ListenPortsExhausted・VesktopRunningNotLinked と同じメッセージの
		// レベルである。本文は、原因・帰結・次の操作を 1 つのテキストで言う
		// (cordmux.dock.vesktop_not_connected.text と同じ形)。待ちを通さない。待ち受けを始めたときに 1 回だけ
		// 分かる確かな事実で、推測ではないためである (ポートを開けなかったときと同じ)。準備の段階の行も
		// 持たない。止まったのは cordmux の側の待ち受けで、段階の行 (Vesktop・通話・シーン) を付けると、
		// 「Vesktop: 接続されていません」が見出しの言い直しになる。配信中でもメッセージのレベル 1 にしないのは、
		// 起動したときにしか起きず、配信中に新しく損なわれるものがないためである。
		// [診断情報をエクスポート...] を置かないのは、OBS の再起動という直す方法があるためである。再起動しても
		// 直らないときは、ドックのメニューに同じ項目がある。
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockSessionNotSavedHeadline));
		n.body.push_back(ref(keys::kDockSessionNotSavedText));
		out->push_back(std::move(n));
		return;
	}
	case PrimaryRuleId::VencordOverwritten:
	case PrimaryRuleId::VencordDirElsewhere: {
		const Tier tier = streaming_with_slotted_person(obs) ? Tier::L1 : Tier::L2;
		out->push_back(install_notice(obs, id, name, tier, false));
		return;
	}
	case PrimaryRuleId::VencordDirNotConfigured:
		out->push_back(install_notice(obs, id, name, Tier::L2, false));
		return;
	case PrimaryRuleId::RestartVesktop: {
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockRestartVesktopHeadline));
		n.body.push_back(ref(keys::kDockRestartVesktopText));
		n.stages = stage_items(obs);
		out->push_back(std::move(n));
		return;
	}
	case PrimaryRuleId::ConnectedThenDropped: {
		TextRef headline = obs.link.disconnected_at_us.has_value()
					   ? ref(keys::kDockDisconnectedHeadline, {when_arg(obs, *obs.link.disconnected_at_us)})
					   : ref(keys::kDockDisconnectedNoTimeHeadline);
		dockview::Notice n = make_notice(name, Tier::L1, std::move(headline));
		// Vesktop が起動していないと分かっているとき (この行になるのは、配信中で枠に人が載っているときだけ。
		// rules.cpp の ConnectedThenDropped) は、切れた理由ごとの文ではなく「Vesktopが起動していないため、
		// 画面共有と声は届きません。Vesktopを起動すると、自動で再接続します。」にする。理由ごとの文
		// (トークン・バージョン・普通の切断) は、起動している Vesktop が再接続する前提の次の操作
		// (再起動・再インストール・待つ) で、終了した Vesktop には当てはまらない。理由の文の後に起動を
		// 勧める 1 文を追加しないのは、再起動を勧める文と起動を勧める文が並ぶためである。見出しは替えない。
		// 見出しは切れた時刻を言い、その事実は変わらない。1 行目のテキストが替わると、ログには別のメッセージとして
		// 書かれる。
		n.body.push_back(ref(obs.install.process == VesktopProcess::NotRunning ? keys::kDockDisconnectedVesktopNotRunningText
										     : disconnected_text(obs.link.reason)));
		// cordmux の録画中は、帰結に 1 文 (切れている間の映像と声は録画に入らないこと) を追加する。
		if (obs.choices.recording == FeatureSwitch::On)
			n.body.push_back(ref(keys::kDockDisconnectedRecordingText));
		n.stages = stage_items(obs);
		out->push_back(std::move(n));
		return;
	}
	case PrimaryRuleId::VesktopChecking:
		fallback->kind = FirstLineKind::Working;
		fallback->forms = {ref(keys::kDockSummaryCheckingStatus)};
		return;
	case PrimaryRuleId::VesktopNotRunning:
		// 平常のテキストにする。何も壊れておらず、Vesktop を起動すれば進むためである。
		fallback->kind = FirstLineKind::Summary;
		fallback->forms = {ref(keys::kDockSummaryVesktopNotRunningStatus)};
		return;
	case PrimaryRuleId::VesktopRunningNotLinked: {
		// 推測で決まる状態 (起動の途中は接続していないのが普通) なので、表示するまでの待ちと消すまでの待ちを
		// 通す。
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockVesktopNotConnectedHeadline));
		n.body.push_back(ref(keys::kDockVesktopNotConnectedText));
		n.settle = Settle::Debounced;
		n.stages = stage_items(obs);
		out->push_back(std::move(n));
		return;
	}
	}
}

// 準備の段階の行を持つカードを 1 枚にする。段階の行は「ここまでは動いていて、ここから先が動いていない」を
// 伝えるので、止まった段階のうち最も前の段階のカードにだけ置く。Vesktop の段階のカード (primary の表の行。
// 接続していない間だけメッセージになり、互いに排他) があれば、cordmux のソースの段階のカードは段階の行を持たない
// (持たせると、インストールした日に 2 枚のカードが同じ行を繰り返す)。Vesktop の段階のカードがなければ (接続していて
// cordmux のソースがないとき)、cordmux のソースのカードが持つ。cordmux のソースの段階のカードは、CompositeAbsent
// の 1 つである。
// 段階の行をカードの外の別の行にしないのは、どのメッセージのレベル階かが分からなくなるためである。
void keep_stages_on_one_card(std::vector<dockview::Notice> *notices)
{
	const std::string composite_card = rule_log_name(NoticeRuleId::CompositeAbsent);
	auto is_scene_card = [&](const dockview::Notice &n) { return n.rule_name == composite_card; };
	bool vesktop_card = false;
	for (const dockview::Notice &n : *notices) {
		if (!is_scene_card(n) && !n.stages.empty())
			vesktop_card = true;
	}
	if (!vesktop_card)
		return;
	for (dockview::Notice &n : *notices) {
		if (is_scene_card(n))
			n.stages.clear();
	}
}

// 押した操作の失敗のメッセージ (VesktopAddFailed) と、同じボタンを持つ状態のメッセージ
// (VencordDirElsewhere・VencordDirNotConfigured・InstallMismatch) が並ぶときは、
// 失敗のカード 1 枚にする。2 枚だと、同じボタンと同じ帰結が 2 度並ぶ。失敗のカードを残すのは、見出しが利用者の
// 押した操作の結果を伝え、状態 (まだ追加されていない) も含むためである (失敗の行は、今実際に
// 直っていればメッセージにならない。added_rule_to_view() の条件)。残すカードは状態のカードの位置に置き、準備の
// 段階の行を引き継ぎ、メッセージのレベルは 2 枚のうち重いほうにする (軽いほうにすると、メッセージのレベル 1 のメッセージが
// 2 に下がる)。キーと規則の名前は失敗の行のものにする (ログとスクリーンリーダーへの通知は、失敗を新しいメッセージ
// として扱う)。同じボタンかは、ボタンのテキストのキーで見る。[Vesktopに追加] には WriteVencordDir と
// RepairVencordDir の 2 つの操作があり、どちらが失敗しても同じフラグが立つ (dock/dock_poll.cpp の
// WriteVencordDir の場面の分け方)。
// 状態のカードに「前回の追加はできませんでした」の 1 文を追加しないのは、失敗の見出しのほうが押した結果を先に
// 伝えるためである。2 枚のまま失敗のカードのボタンだけ外さないのは、同じ帰結が 2 度残るためである。
// 同じボタンのテキストは、失敗の行ごとに決める (失敗のカードのボタンから読まない)。押しても同じ失敗になるときは、
// 失敗のカードはボタンを持たないが、そのときも状態のカードの代わりになる。ボタンから読むと、ボタンのない失敗の
// カードの横に、状態のカードがボタン付きで別に表示され、押せば同じ失敗になるボタンが残る。
void merge_failure_cards(std::vector<dockview::Notice> *notices)
{
	struct FailureRule {
		std::string rule;
		const char *label; // 失敗した操作のボタンのテキスト (状態のカードを探すのに使う)
	};
	const FailureRule failure_rules[] = {
		{rule_log_name(AddedRuleId::VesktopAddFailed), keys::kDockAddToVesktopButton},
	};
	auto is_failure = [&](const dockview::Notice &n) {
		for (const FailureRule &f : failure_rules)
			if (n.rule_name == f.rule)
				return true;
		return false;
	};
	for (const FailureRule &f : failure_rules) {
		std::size_t failure = notices->size();
		for (std::size_t i = 0; i < notices->size(); ++i)
			if ((*notices)[i].rule_name == f.rule)
				failure = i;
		if (failure == notices->size())
			continue;
		const std::string label = f.label;
		std::size_t state = notices->size();
		for (std::size_t i = 0; i < notices->size() && state == notices->size(); ++i) {
			const dockview::Notice &n = (*notices)[i];
			if (i == failure || is_failure(n))
				continue;
			for (const ViewButton &b : n.buttons)
				if (b.label.id == label)
					state = i;
		}
		if (state == notices->size())
			continue;
		dockview::Notice merged = (*notices)[failure];
		const dockview::Notice &replaced = (*notices)[state];
		if (merged.stages.empty())
			merged.stages = replaced.stages;
		if (replaced.tier < merged.tier) {
			merged.tier = replaced.tier;
			merged.log_level = level_of(replaced.tier);
		}
		(*notices)[state] = std::move(merged);
		notices->erase(notices->begin() + static_cast<std::ptrdiff_t>(failure));
	}
}

// 準備の段階の行のうち、止まった段階の値を、ドックのどこかの見出しが既に伝えている行を外す。同じ画面に、段階の
// 「シーン: 作成されていません」とメッセージの「cordmuxのシーンがまだ作成されていません」を両方表示しないためである。
// 動いている段階の行 (「Vesktop: 接続済み」「通話: 参加中 (4人)」「シーン: 作成済み (枠4つ)」) は、「ここまでは
// 動いている」を伝えるので残す。既に伝えているとみなす見出しは、同じ段階の状態をテキストそのものが述べるもの
// だけである。
//   - Vesktop の段階の「接続されていません」: 「接続が切れました」(時刻があるものとないもの)、
//     「起動していますが、接続されていません」
//   - cordmux のソースの段階の「追加されていません」: 「cordmuxのソースがまだ追加されていません」
// 見る範囲はドックのすべてのカードである (インストールした日は、cordmux のソースの段階を下のカードの見出しが
// 伝える)。
// install_notice() の見出し (追加されていません、上書きされています、別の Vencord を読む) は、Vesktop の段階を
// 伝えているとみなさない。見出しが伝えるのは原因で、接続の状態そのものではなく、段階の行が「止まったのは接続」を
// 伝えるためである。
void drop_restated_stages(std::vector<dockview::Notice> *notices)
{
	bool vesktop_said = false;
	bool composite_said = false;
	for (const dockview::Notice &n : *notices) {
		const std::string &h = n.headline.id;
		if (h == keys::kDockDisconnectedHeadline || h == keys::kDockDisconnectedNoTimeHeadline ||
		    h == keys::kDockVesktopNotConnectedHeadline)
			vesktop_said = true;
		if (h == keys::kDockCompositeAbsentHeadline)
			composite_said = true;
	}
	for (dockview::Notice &n : *notices) {
		n.stages.erase(std::remove_if(n.stages.begin(), n.stages.end(),
					      [&](const StageItem &s) {
						      return (vesktop_said &&
							      s.text.id == keys::kDockStageVesktopDisconnectedStatus) ||
							     (composite_said &&
							      s.text.id == keys::kDockStageCompositeNotAddedStatus);
					      }),
			       n.stages.end());
	}
}

// 平常の 1 行目の要約。primary が Connected のときだけ使う。FirstLine::forms には、省略しない完全なテキストを
// 長い順に並べる。
void summary_line(const Observation &obs, FirstLine *fallback)
{
	fallback->kind = FirstLineKind::Summary;
	fallback->forms.clear();
	if (obs.call.presence == Presence::InCall && obs.call.roster_count.has_value()) {
		const int n = *obs.call.roster_count;
		const int shown = displayed_count(obs);
		if (shown > 0)
			fallback->forms.push_back(
				ref(text::pick(keys::kDockSummaryCallStatus, n), {count_text(n), count_text(shown)}));
		else
			fallback->forms.push_back(ref(text::pick(keys::kDockSummaryNoneDisplayedStatus, n), {count_text(n)}));
		fallback->forms.push_back(ref(text::pick(keys::kDockSummaryCallShortStatus, n), {count_text(n)}));
		return;
	}
	if (receiver_not_in_call(obs)) {
		fallback->forms.push_back(ref(keys::kDockSummaryConnectedStatus));
		fallback->forms.push_back(ref(keys::kDockSummaryConnectedShortStatus));
		return;
	}
	// 通話の人数をまだ読めていない (facts が届く前)。分かっているところ (接続済み) までを表示する。
	fallback->forms.push_back(ref(keys::kDockSummaryConnectedShortStatus));
}

// notices の表の 1 行からメッセージを作る (条件が成り立った行をすべて)。条件は rules.cpp の表を使う。
void notice_rule_to_view(const Observation &obs, NoticeRuleId id, std::vector<dockview::Notice> *out)
{
	const std::string name = rule_log_name(id);
	// 表の条件が成り立たない行はメッセージにしない (条件を 2 か所に持たない)。例外は、下の行ごとのコメントに書く。
	const bool fired = notice_rule_fires(id, obs);
	switch (id) {
	case NoticeRuleId::ContactUnavailable:
		// contact_notice() は、表と同じ条件 (Missing か Lost) で使えない依存箇所を集め、なければ何も作らない。
		contact_notice(obs, out);
		return;
	case NoticeRuleId::CompositeAbsent: {
		if (!fired)
			return;
		// 帰結は「追加するまで、Discordの画面共有と声はOBSに出ません。」にする (何ができないかを言う)。cordmux を
		// 置く操作はボタンにできない (OBS の「ソース」の [+] は OBS の画面にある) ので、次の一手を文で言う。
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockCompositeAbsentHeadline));
		n.body.push_back(ref(keys::kDockCompositeAbsentText));
		n.stages = stage_items(obs);
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordingOnly: {
		if (!fired)
			return;
		int count = 0;
		for (const PersonFact &person : obs.people) {
			if (recording_only_person(obs, person))
				++count;
		}
		dockview::Notice n = make_notice(name, Tier::L3,
						 ref(text::pick(keys::kDockRecordingOnlyHeadline, count), {count_text(count)}));
		n.tip = ref(keys::kDockRecordingOnlyTip);
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::InstallMismatch: {
		if (!fired)
			return;
		// 接続している間のインストールの食い違い。現在の接続は続くので、メッセージのレベル 2 にする。どの行のメッセージに
		// するかは、primary の表の順で、最初に条件が成り立つ 1 つ (rules.cpp の kInstallRuleIds と同じ並び)。
		for (const PrimaryRuleId install :
		     {PrimaryRuleId::VencordOverwritten, PrimaryRuleId::VencordDirElsewhere,
		      PrimaryRuleId::VencordDirNotConfigured}) {
			if (!primary_rule_matches(install, obs))
				continue;
			out->push_back(install_notice(obs, install, name, Tier::L2, true));
			return;
		}
		return;
	}
	case NoticeRuleId::VersionRestartVesktop: {
		if (!fired)
			return;
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockVersionDiffersHeadline));
		n.body.push_back(ref(keys::kDockVersionDiffersText));
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::VersionMixedBundle: {
		if (!fired)
			return;
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockFilesMixedHeadline));
		n.body.push_back(ref(keys::kDockFilesMixedText));
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::DoubleAudio: {
		// 見出しには、音声ミキサーに表示されているソースの名前を入れる。名前が空ならメッセージにしない
		// (ソースの名前が分かったときだけ Found にする約束なので、普通は起きない。observation.h の
		// discord_audio_source_name)。名前のない見出しのキーを作らないのは、約束のうえでは表示する場面がない
		// ためである。
		if (!fired || obs.choices.discord_audio_source_name.empty())
			return;
		// 配信中は視聴者全員に聞こえていて、OBS のミュートで今修正できるので、メッセージのレベル 1 にする。枠に人が載って
		// いるかは問わない (声は枠に載っていない人の分も出る)。配信していない間はレベル 2 にする。
		const Tier tier = obs.obs_outputs.streaming.value_or(false) ? Tier::L1 : Tier::L2;
		dockview::Notice n =
			make_notice(name, tier, ref(keys::kDockDoubleAudioHeadline, {obs.choices.discord_audio_source_name}));
		n.body.push_back(ref(keys::kDockDoubleAudioText, {obs.choices.discord_audio_source_name}));
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::BroadcasterNotInCall: {
		if (!fired)
			return;
		// 見出しには、メインアカウントの名前を入れる (ID の数字は入れない)。名前が分からないときは、
		// 名前のない形の見出しにする。
		const std::string &id_text = obs.choices.broadcaster_user_id;
		std::string label;
		if (const PersonFact *person = find_person(obs, id_text)) {
			if (person->display_name.has_value() && !person->display_name->empty())
				label = *person->display_name;
		}
		if (label.empty()) {
			if (const SeenPerson *seen = find_seen(obs, id_text)) {
				if (override_present(*seen) || !seen->display_name.empty())
					label = seen_name(*seen);
			}
		}
		dockview::Notice n =
			make_notice(name, Tier::L2,
				    label.empty() ? ref(keys::kDockMainAccountNotInCallNoNameHeadline)
						  : ref(keys::kDockMainAccountNotInCallHeadline, {label}));
		n.body.push_back(ref(keys::kDockMainAccountNotInCallText));
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::AllVideoStoppedHint: {
		// 条件は rules.cpp の表にある (2 人以上の映像が届かず、対象の中に映像が届いている人がいないとき)。1 人だけなら
		// その人の行の状態と OBS の映像で分かるので、メッセージにしない。誰のことかは人の行が表す。
		if (!fired)
			return;
		const NoVideoTally tally = no_video_tally(obs);
		dockview::Notice n = make_notice(
			name, Tier::L2, ref(text::pick(keys::kDockNoVideoHeadline, tally.count), {count_text(tally.count)}));
		// 帰結は、損なわれるものごとに文を追加する。枠に載っている人がいれば枠の文、cordmux の録画の対象の
		// 人がいれば (枠に載っているかを問わない) 「届かない間の映像と声は、cordmuxの録画に入りません。」
		// を追加する。両方なら枠の文を先にする。次の操作の文は、帰結の文の後に置く。
		// 枠の文は、参加者ごとに枠に映っているもので選ぶ。届いていた映像が止まった人の枠には最後の画像が
		// 止まったまま残るので「映像が途切れた人の枠には、最後に届いた映像が止まったまま映っています。」、
		// 最初のフレームがまだない人 (first_frame_missing()。最初の確保でエンコードの段階が尽きた人を含む) の
		// 枠は空いたままなので「映像がまだ一度も届いていない人の枠は、空いたままです。」にする。
		// 両方の人がいれば 2 つの文を 1 回ずつ、止まった人の文を先に置く。どちらの文も誰の枠のことかを自分で
		// 言うので、両方の人がいるときにも誰の枠の話か分かる (見出しの人数は録画の対象だけの人も数えるので、
		// 文では人数を受けない)。1 つの文にまとめないのは、フレームがない人だけのとき (エンコーダがない
		// PC で最もよく起きる) に、空の枠に止まった画像が映っていると伝えてしまうためである。
		// 枠に載っていて録画もしている人がいるときにも、録画の文を追加する。Vesktop との接続が切れたメッセージ
		// (録画しているときは 1 文を追加する) とそろえるためで、枠の止まった画像は配信を見れば分かるが、
		// 録画に入らないことは配信の最中には分からない。録画の対象だけの人に枠の文を追加しないのは、
		// 枠に載っていない人の枠はなく、事実と違うためである。
		if (tally.any_slotted_stalled)
			n.body.push_back(ref(keys::kDockNoVideoText));
		if (tally.any_slotted_waiting)
			n.body.push_back(ref(keys::kDockNoVideoWaitingText));
		if (tally.any_recorded)
			n.body.push_back(ref(keys::kDockNoVideoRecordingText));
		// 次の操作の文は、数えた人のエンコードの段階で選ぶ。段階が unavailable でない人がいれば「Vesktopで、
		// 画面共有の映像が映っているか確認してください。」、unavailable の人がいれば「映像を表示できない人に、
		// 画面共有を始め直してもらってください。」にする。両方なら、Vesktop で確認する文を先にして、
		// どちらも 1 回ずつ置く。全員が unavailable なら、後の 1 文だけにする。
		// unavailable は、送信側がその人の映像をソフトウェアのエンコーダまで試してもエンコードできず、
		// そのストリームを確保し直さない状態である (src-vencord/encode/session.ts)。この状態は、
		// Vesktop で確認しても直らない。同じ View の行は「映像を表示できません」で、ツールチップは、
		// エンコードできなかったことを理由として伝える。新しい画面共有は新しいセッションで最初の段階から確保し
		// 修正するので、画面共有を始め直してもらうと、同じ人の映像をもう一度試せる。見出し・帰結は、段階によって
		// 変えない。枠には止まった画像が残るか (フレームが届いていた人)、空いたままで (フレームがない人)、録画にも
		// 入らないためである。段階の判定は、displayed() と同じ stage_unavailable() を使う。
		if (tally.any_other)
			n.body.push_back(ref(keys::kDockNoVideoNextText));
		if (tally.any_cant_show)
			n.body.push_back(ref(keys::kDockNoVideoCantShowNextText));
		// しきい値 (映像が止まったとみなすしきい値と、最初のフレームの待ち時間) そのものが表示するまでの
		// 待ちになっているので、消すときだけ待つ。
		n.settle = Settle::ClearOnly;
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordSpaceLow: {
		if (!fired)
			return;
		const long long minutes = minutes_floor(*obs.recording.remaining_seconds);
		dockview::Notice n = make_notice(
			name, Tier::L2, ref(text::pick(keys::kDockSpaceLowHeadline, minutes), {count_text(minutes)}));
		n.body.push_back(ref(keys::kDockSpaceLowText));
		// 残りが少なくなったら段階を進める。残り 30 分で既読にした人にも、5 分以下になったら 1 行目にもう一度出す。
		n.read_stage = *obs.recording.remaining_seconds <= kSpaceUrgentSeconds ? 1 : 0;
		// 見積もりは書き込みの速さで変わり、しきい値の前後を行き来することがある。表示するまでの待ちと、
		// 消すまでの待ちを通す。
		n.settle = Settle::Debounced;
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordFinalizeShort: {
		if (!fired)
			return;
		const std::uint64_t need = *obs.recording.finalize_need_bytes - *obs.recording.free_space_bytes;
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockFinalizeSpaceShortHeadline, {byte_size_1024(need)}));
		n.body.push_back(ref(keys::kDockFinalizeSpaceShortText));
		n.settle = Settle::Debounced; // 見積もりなので、上と同じ理由で待ちを通す
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordFinalizeRunning: {
		if (!fired)
			return;
		dockview::Notice n = make_notice(name, Tier::L3, ref(keys::kDockFinalizingHeadline));
		const RecordingFacts &rec = obs.recording;
		const int done = rec.finalize_files_done;
		const int total = rec.finalize_files_total;
		// 経過の分は、録画ファイルの作成を始めた時刻のフィールドがないので、録画の状態が Stopping か Idle になった時刻
		// (phase_since_us) から数える。その時刻は録画を止めた時刻で、録画ファイルの作成はその直後に始まる。
		// 読めないときは 0 分にする。
		const long long elapsed = rec.phase_since_us.has_value() ? minutes_of_us(obs.now_us - *rec.phase_since_us) : 0;
		if (rec.finalize_stalled && rec.finalize_current_index > 0) {
			const long long stalled_for =
				rec.finalize_last_progress_us.has_value()
					? minutes_of_us(obs.now_us - *rec.finalize_last_progress_us)
					: elapsed;
			n.line2 = ref(text::pick(keys::kDockFinalizeStalledStatus, total),
				      {count_text(done), count_text(total), count_text(rec.finalize_current_index),
				       count_text(stalled_for)});
		} else if (rec.finalize_permille.has_value() && rec.finalize_current_index > 0) {
			// 割合は整数にする。千分率を百分率に切り捨てる (進み具合を多めに表示しない)。
			const int percent = std::clamp(*rec.finalize_permille, 0, 1000) / 10;
			n.line2 = ref(text::pick(keys::kDockFinalizeProgressFileStatus, total),
				      {count_text(done), count_text(total), count_text(rec.finalize_current_index),
				       std::to_string(percent), count_text(elapsed)});
		} else {
			n.line2 = ref(text::pick(keys::kDockFinalizeProgressStatus, total),
				      {count_text(done), count_text(total), count_text(elapsed)});
		}
		// 作成したファイルの場所。
		if (!rec.session_dir.empty())
			n.location = LocationLine{rec.session_dir, true};
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordFinalizePending: {
		if (!fired)
			return;
		// 録画ファイルの作成に失敗したメッセージ (RecordFinalizeFailed) の条件が成り立つ間は、メッセージにしない。録画の側は、
		// 失敗したセッションを作成待ちにも数えるので、2 つの条件が同時に成り立つ。しかし帰結の文
		// (finalize_pending.text) とボタン ([作成を再開]) が同じで、その [作成を再開] は作成待ちの
		// セッションを全部やり直す (record/recorder.cpp の resume_pending)。「作成できなかった」は
		// 「終わっていない」を含む。失敗のメッセージを作成待ちのメッセージの 1 文にしないのは、失敗したことが
		// 「終わっていない」の見出しの下に隠れるためである。
		if (notice_rule_fires(NoticeRuleId::RecordFinalizeFailed, obs))
			return;
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockFinalizePendingHeadline));
		n.body.push_back(ref(keys::kDockFinalizePendingText));
		ViewButton b = make_button(obs, ActionId::ResumeFinalize, ActionArgs{}, keys::kDockResumeFinalizingButton);
		n.buttons.push_back(std::move(b));
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordNotReachedSender: {
		if (!fired)
			return;
		dockview::Notice n = make_notice(name, Tier::L3, ref(keys::kDockRecordNotReachedHeadline));
		n.tip = ref(keys::kDockRecordNotReachedText);
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordDirUnwritable: {
		// rules.cpp の表と違う条件で作る。保存先は録画を始める時点で決め、書き込めなかった場所が
		// unwritable_dir に残る。save_dir_writable が偽で場所がないとき (録画の状態をコピーしない古い録画の
		// 側) も、同じメッセージにする。
		const bool unwritable_old =
			obs.recording.save_dir_writable.has_value() && !*obs.recording.save_dir_writable;
		if (obs.recording.unwritable_dir.empty() && !unwritable_old)
			return;
		// 始めようとして書き込めなかったとき (状態が StartFailed) は、今録画がないので、
		// メッセージのレベル 1 にする。それ以外 (書き込めなかった場所が残っているが、今は始めようとしていない) は、
		// 次の録画で困るので、メッセージのレベル 2 にする。
		const bool trying = obs.recording.phase == RecordPhase::StartFailed ||
				    (obs.recording.phase == RecordPhase::Unknown && unwritable_old);
		dockview::Notice n = make_notice(name, trying ? Tier::L1 : Tier::L2, ref(keys::kDockRecordStartFailedHeadline));
		// 録画だけの失敗なので、状態タブを開いたら 1 行目から消す。
		n.read_on_status_tab = trying;
		n.body.push_back(ref(keys::kDockSaveFolderUnwritableText));
		// メッセージのレベル 1 (始めようとして書き込めなかったとき) だけ、次の操作「保存先を変更してから、
		// もう一度録画を開始してください。」を追加する。保存先は押した時点で決める (actions.cpp の
		// ToggleRecording のコメント) ので、[保存先を変更] で修正しても録画は始まらない。
		// 押し直すことを伝えないと、修正したので録画できていると思ったまま録画を失う。テキストは、
		// 空き容量で止まったメッセージ (record_stopped_space.text) の後半と同じ形にする。メッセージのレベル 2 (今は
		// 始めようとしていない) には追加しない。次に始めるときの話で、押し直すものがない。
		if (trying)
			n.body.push_back(ref(keys::kDockRecordStartAgainText));
		if (!obs.recording.unwritable_dir.empty())
			n.location = LocationLine{obs.recording.unwritable_dir, false};
		ActionArgs args;
		args.page = SettingsPage::Recording;
		ViewButton b = make_button(obs, ActionId::OpenSettings, args, keys::kDockChangeSaveFolderButton);
		n.buttons.push_back(std::move(b));
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordStoppedSpaceExhausted: {
		if (!fired)
			return;
		dockview::Notice n = make_notice(name, Tier::L1, ref(keys::kDockRecordStoppedSpaceHeadline));
		// 録画だけの失敗なので、状態タブを開いたら 1 行目から消す。
		n.read_on_status_tab = true;
		// 1 行目に入らないときは、括弧 (理由) を丸ごと落とした短い形にする (括弧の途中で省略しない)。短い形は
		// make_notice() が見出しのキーの表 (kShortHeadlines) から入れる。数を含む括弧の見出し
		// (space_low など) には短い形は必要ない。数の語のガードが括弧を丸ごと落とすためである。
		n.body.push_back(ref(keys::kDockRecordStoppedSpaceText));
		ActionArgs args;
		args.page = SettingsPage::Recording;
		ViewButton b = make_button(obs, ActionId::OpenSettings, args, keys::kDockChangeSaveFolderButton);
		n.buttons.push_back(std::move(b));
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::ReinstallFailed: {
		if (!fired)
			return;
		// 見出しは上書きのメッセージ (「Vesktopのcordmuxが上書きされています」) と同じにし、段落を 1 つ追加する。
		// 同じ見出しのメッセージが既にあれば、そのカードに段落を追加し、カードを 2 枚にしない。
		// コピーできなかったファイルの名前は表示しない。
		// 段落を追加した先のカードのメッセージのレベルは変えない。接続している間 (InstallMismatch。
		// メッセージのレベル 2) は現在の接続が続き、出力は損なわれていない。接続していない間は、
		// VencordOverwritten の行が同じ条件 (配信中で枠に人が載っている) でメッセージのレベル 1 にしている。
		// 下の tier を使うのは、上書きのカードがなく、この行のカードを新しく作るときだけである。
		// 重いほうのメッセージのレベルに上げないのは、接続している間に、不要にメッセージのレベル 1 になるためである。
		const Tier tier = streaming_with_impaired_frames(obs) ? Tier::L1 : Tier::L2;
		for (dockview::Notice &existing : *out) {
			if (existing.headline.id == keys::kDockVencordOverwrittenHeadline) {
				existing.body.push_back(ref(keys::kDockReinstallFailedText));
				return;
			}
		}
		dockview::Notice n = make_notice(name, tier, ref(keys::kDockVencordOverwrittenHeadline));
		n.body.push_back(ref(keys::kDockReinstallFailedText));
		// [再インストール] は押せるときだけ置く。ファイルをまだ照合していないか、一致している間は、押しても何も
		// 変わらない。そのときは本文の文が次の操作を伝える。
		ViewButton b = make_button(obs, ActionId::ReinstallVencordFiles, ActionArgs{}, keys::kDockReinstallButton);
		if (!b.disabled_reason.has_value()) {
			n.buttons.push_back(std::move(b));
		}
		out->push_back(std::move(n));
		return;
	}
	case NoticeRuleId::RecordFinalizeFailed: {
		if (!fired)
			return;
		// メッセージのレベル 2 にする。過去の失敗で配信は損なわれておらず、配信の後に対応すればよいためである。
		// 本文は、帰結、次の操作の順にする。帰結は作成待ちと同じ
		// 「録ったファイルは1つも削除していません。」で、次の操作は「次のフォルダーがあるドライブの空き容量を
		// 確認してから、[作成を再開] を押してください。」である。空き容量が足りずに止まったときも、
		// このメッセージになる。
		// 場所の行は、失敗したセッションのフォルダー (下のコメント)。ボタンは [作成を再開] で、
		// 押せる条件は作成待ちと同じ pending_present である (actions.cpp)。押すたびに録画ファイルの作成を試し、
		// また失敗すれば、このメッセージに戻る。[診断情報をエクスポート...] を置かないのは、
		// 再開という直す方法があるためである。帰結に「完成したファイルはありません」と書かないのは、
		// 1 つでも作れないと Failed になり、ほかの人のファイルは完成していることがあるためである
		// (record/record_finalize.cpp)。
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockFinalizeFailedHeadline));
		n.body.push_back(ref(keys::kDockFinalizePendingText));
		// FFmpeg を起動できなかったのは、この起動で最後に作成したセッションが失敗したときだけ分かる。録画ログに
		// 失敗が残っているだけのときは理由が分からないので、空き容量の確認を言う。
		const bool ffmpeg_unavailable = obs.recording.finalize == FinalizeState::Failed &&
						obs.recording.finalize_ffmpeg_unavailable;
		n.body.push_back(ref(ffmpeg_unavailable ? keys::kDockFinalizeFailedFfmpegNextText
							: keys::kDockFinalizeFailedNextText));
		// 場所の行は、この起動で最後に作成したセッションが失敗したなら、そのセッション (session_dir) にする。
		// そうでなければ、録画ログに失敗が残ったセッション (failed_session_dir) にする。後者のとき、
		// session_dir は今録画している別のセッションのことがある。
		const std::string &failed_dir = obs.recording.finalize == FinalizeState::Failed ? obs.recording.session_dir
												   : obs.recording.failed_session_dir;
		if (!failed_dir.empty())
			n.location = LocationLine{failed_dir, true};
		n.buttons.push_back(make_button(obs, ActionId::ResumeFinalize, ActionArgs{}, keys::kDockResumeFinalizingButton));
		out->push_back(std::move(n));
		return;
	}
	}
}

// [Vesktopに追加] が失敗した理由の 1 文。理由が分かるときは、利用者の語で理由を 1 文で伝える。ボタンを置かない失敗
// (vesktop_add_retry_can_differ() が偽) は、同じキーの 2 文目に次の操作を持つ (押せるボタンがないメッセージは、何を
// すればよいかを文で伝える。bundled_missing.text と同じ形)。理由の文がないときは nullptr を返す。default を
// 書かないのは、列挙子を追加したときに、ここで扱いを決めるためである。
//   - バックアップの保存先がない (NoBackupPlace) と、バックアップを書けない (BackupWriteFailed) は、理由の 1 文が
//     同じでもキーを分ける。保存先がないのは同じ起動の中では直らず、ボタンを置かないので、次の操作 (OBS の
//     再起動) を追加したキーにする。書けないのは押し直すと直ることがあり、ボタンが次の操作になるので、再起動を
//     勧めない。
//   - 書き込みを止めてある (WritesDisabled) は、2 つの場合に起きる。settings.json の allow_state_json_write を手で
//     false にしたときと、起動したときに settings.json を読めなかったので false にしたとき
//     (pipeline/pipeline.h の effective_allow_state_json_write) である。後者で
//     「settings.jsonで無効になっています」と表示すると、利用者は無効にしていないので事実と違う。そこで、
//     読めなかったことを伝える別のキーにする。そのキーは次の操作を伝えない。同じ起動の間ずっと表示している
//     「cordmuxの設定を読み込めませんでした」のカード (SettingsUnreadAtStartup) が「OBSを再起動すると、もう一度
//     読み込みます。」と伝えているためである。
//     この 2 つの文は、押す前の状態のカード (install_notice()) も Install::vesktop_writes_off の間に使い、その
//     カードには [Vesktopに追加] を置かない。そのため、ここに来るのは、ドックが DockFacts::vesktop_writes_off を
//     コピーし忘れたときだけで、そのときも同じテキストになる。
//   - 書いてよい形でなかった (Refused) には理由の文を置かない。分かるのは「書いてよい形でなかった」ことだけで、
//     その中身 (壊れた JSON、形が違う、押している間にほかのツールが書き換えた) は区別できない。どれかの
//     テキストを使うと、別の場合に事実と違うことになる。
// install/vencord_dir.h の RefuseReason を Observation にコピーして理由を分けないのは、理由ごとの次の操作が
// どれも [Vesktopに追加] の押し直しで同じなので、得るものが小さいためである。
const char *vesktop_add_failure_text(VesktopAddFailure failure, bool settings_unreadable_at_startup)
{
	switch (failure) {
	case VesktopAddFailure::StatePathUnknown:
		return keys::kDockVesktopAddFailedStateMissingText;
	case VesktopAddFailure::StateUnreadable:
		return keys::kDockVesktopAddFailedStateUnreadableText;
	case VesktopAddFailure::BundledDirUnknown:
		return keys::kDockVesktopAddFailedBundledMissingText;
	case VesktopAddFailure::Refused:
		return nullptr;
	case VesktopAddFailure::WritesDisabled:
		return settings_unreadable_at_startup ? keys::kDockVesktopAddFailedSettingsUnreadText
						      : keys::kDockVesktopAddFailedWritesOffText;
	case VesktopAddFailure::NoBackupPlace:
		return keys::kDockVesktopAddFailedNoBackupText;
	case VesktopAddFailure::BackupWriteFailed:
		return keys::kDockVesktopAddFailedBackupText;
	case VesktopAddFailure::StateWriteFailed:
		return keys::kDockVesktopAddFailedStateWriteText;
	case VesktopAddFailure::PortableVesktop:
		return keys::kDockVesktopAddFailedPortableText;
	}
	return nullptr;
}

// [Vesktopに追加] をもう一度押せば、別の結果になることがあるか。偽の失敗のカードにはボタンを置かない。押しても、
// 同じ起動の中では同じ失敗になると分かっているボタンは、カードに置かない (押せなくして残すこともしない)。
// default を書かないのは、列挙子を追加したときに、ここで扱いを決めるためである。
// 値は、dock/dock_poll_install.cpp の DockPoll::write_vencord_dir() が失敗する場合ごとに、次の理由で決める。
//   - BundledDirUnknown: 偽。同梱の Vencord の場所 (DockPollConfig::bundled_vencord_dir) は、plugin-main.cpp が
//     起動したときに 1 回だけ決め、DockPoll は cfg_ を書き換えない。修正する方法は OBS を終了してからで、理由の文が
//     それを伝える。
//   - WritesDisabled: 偽。allow_state_json_write も、起動したときの値 (settings.json に手で書く値。起動したときに
//     読めなかったときは false にした値) を cfg_ にコピーしたままで、同じ起動の中では変わらない。押す前から
//     分かるので、状態のカードもボタンを置かない (install_notice())。
//   - NoBackupPlace: 偽。書く計画 (install::plan_write) は、書くときは必ずバックアップを持つので、ここに来るのは
//     バックアップの保存先 (cfg_.backup_json_path。起動したときの obs_module_config_path) が空のときだけで、同じ
//     起動の中では変わらない。
//   - PortableVesktop: 偽。ポータブル版の Vesktop を見たという記録は、同じ起動の中では消えない (OBS を終了するまで)
//     ので、押し直しても同じ失敗になる。
//   - StatePathUnknown: 真。場所は押すたびに、環境変数と Windows の既知のフォルダーに問い合わせ直す (platform の
//     vesktop_state_json_path) ので、次も失敗するかは cordmux には分からない。
//   - StateUnreadable・Refused・StateWriteFailed: 真。Vesktop の設定のファイルは、Vesktop や利用者が書き換えたり、
//     作ったり、ほかのプログラムが閉じたりすることがある (押すたびに読み直す)。
//   - BackupWriteFailed: 真。書き込みの失敗 (空き容量、ほかのプログラムが開いている) は、押し直すと変わることが
//     ある。
// ボタンを押せなくして理由を表示しないのは、押せない理由が、本文の理由の言い直しになるためである。
bool vesktop_add_retry_can_differ(VesktopAddFailure failure)
{
	switch (failure) {
	case VesktopAddFailure::BundledDirUnknown:
	case VesktopAddFailure::WritesDisabled:
	case VesktopAddFailure::NoBackupPlace:
	case VesktopAddFailure::PortableVesktop:
		return false;
	case VesktopAddFailure::StatePathUnknown:
	case VesktopAddFailure::StateUnreadable:
	case VesktopAddFailure::Refused:
	case VesktopAddFailure::BackupWriteFailed:
	case VesktopAddFailure::StateWriteFailed:
		return true;
	}
	return true;
}

// [Vesktopから削除] の失敗の理由の 1 文 (dock/dock_poll_install.cpp の DockPoll::remove_from_vesktop() が失敗する
// 場合ごと)。[Vesktopに追加] の失敗と同じ事実 (設定のファイルを読めない、書けない、この起動では書かない) は、同じ
// キーを使う。次の操作の文は付けない。押し直すのは状態タブの同じ組の [Vesktopから削除] で、書かない起動の理由は
// 次の操作まで同じキーが持つ。cordmux を追加する前の値が分からないとき (NotRestorable) は、cordmux の側で戻す方法が
// ないので、事実だけを伝える。列挙にない値には NotRestorable の文を返す (理由のないカードを作らない)。
const char *vesktop_remove_failure_text(VesktopRemoveFailure failure, bool settings_unreadable_at_startup)
{
	switch (failure) {
	case VesktopRemoveFailure::StateUnreadable:
		return keys::kDockVesktopAddFailedStateUnreadableText;
	case VesktopRemoveFailure::NotRestorable:
		return keys::kDockVesktopRemoveFailedNotRestorableText;
	case VesktopRemoveFailure::WritesDisabled:
		return settings_unreadable_at_startup ? keys::kDockVesktopAddFailedSettingsUnreadText
						      : keys::kDockVesktopAddFailedWritesOffText;
	case VesktopRemoveFailure::StateWriteFailed:
		return keys::kDockVesktopAddFailedStateWriteText;
	}
	return keys::kDockVesktopRemoveFailedNotRestorableText;
}

// decide_dock() だけが使う規則 (rules.h の AddedRuleId) のメッセージ。
void added_rule_to_view(const Observation &obs, AddedRuleId id, std::vector<dockview::Notice> *out)
{
	const std::string name = rule_log_name(id);
	const OperationFacts &ops = obs.ops;
	switch (id) {
	case AddedRuleId::SettingsUnreadAtStartup:
		if (ops.settings_unreadable_at_startup) {
			dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockSettingsUnreadHeadline));
			n.body.push_back(ref(keys::kDockSettingsUnreadText));
			out->push_back(std::move(n));
		}
		return;
	case AddedRuleId::SettingsUnsaved:
		// 設定を読めなかった起動では書かないので、「保存できていません」は表示しない (上の行が伝える)。
		if (ops.settings_unsaved && !ops.settings_unreadable_at_startup) {
			dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockSettingsUnsavedHeadline));
			// 次の操作は設定ウィンドウで [OK] を押し直すことなので、設定ウィンドウを開くボタンを置く
			// (利用者に設定の場所を覚えて行かせない)。開くページは ActionArgs の既定 (先頭のページ) にする。
			// 保存はウィンドウのすべての値を 1 つのファイルに書くので、どのページで [OK] を押しても同じである
			// (dock/dock_poll_settings.cpp の DockPoll::write_settings())。
			n.body.push_back(ref(keys::kDockSettingsUnsavedText));
			n.buttons.push_back(make_button(obs, ActionId::OpenSettings, ActionArgs{}, keys::kDockOpenSettingsButton));
			out->push_back(std::move(n));
		}
		return;
	case AddedRuleId::SeenUnreadAtStartup:
		if (ops.seen_unreadable_at_startup) {
			dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockParticipantsUnreadHeadline));
			n.body.push_back(ref(keys::kDockParticipantsUnreadText));
			out->push_back(std::move(n));
		}
		return;
	case AddedRuleId::SeenUnsaved:
		if (ops.seen_unsaved && !ops.seen_unreadable_at_startup) {
			dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockParticipantsUnsavedHeadline));
			n.body.push_back(ref(keys::kDockParticipantsUnsavedText));
			out->push_back(std::move(n));
		}
		return;
	case AddedRuleId::VesktopAddFailed: {
		// 失敗のフラグが残っていても、今 Vesktop の設定が cordmux を指していればメッセージにしない
		// (今どうなっているかを、実際の状態で伝える)。フラグは成功か [キャンセル] で消えるが、
		// 利用者がほかの方法で修正したこともありうる。
		if (!ops.vesktop_add_failure.has_value() || uninstall_started(obs) || obs.install.dir == VencordDirState::Matches)
			return;
		const Tier tier = streaming_with_impaired_frames(obs) ? Tier::L1 : Tier::L2;
		dockview::Notice n = make_notice(name, tier, ref(keys::kDockVesktopAddFailedHeadline));
		// 帰結は install_notice() と同じに分ける (接続している間は、現在の接続が続くので
		// 「次にVesktopを起動したときから、...」)。このカードは同じボタンの状態のカードの代わりになる
		// (merge_failure_cards()) ので、代わりになったカードの帰結と食い違わないようにする。
		const bool connected = obs.link.state == LinkState::Connected;
		// 並びは、帰結、理由 (ボタンを置かない失敗では、同じキーの次の操作も)、ボタンの順にする。
		// 次の操作をいつも最後に置くためである。理由と次の操作は 1 つのキーにある (bundled_missing.text と同じ
		// 形) ので、帰結を先に置けば、次の操作が帰結の後にも理由の後にも来る。
		// 8 つの失敗の場合で同じ並びにする (ボタンの有無で並びを変えない。ボタンのある場合は、
		// 理由の文がボタンのすぐ上に来る)。理由、帰結の順にしないのは、理由のキーが次の操作を持つ場合に、
		// 「OBSを再起動してください。」の後に「受け取れません。」が来て、次の操作が最後でなくなり、
		// 再起動したから受け取れないとも読めるためである。
		n.body.push_back(ref(connected ? keys::kDockCannotReceiveAfterRestartText : keys::kDockCannotReceiveText));
		if (const char *why = vesktop_add_failure_text(*ops.vesktop_add_failure, ops.settings_unreadable_at_startup))
			n.body.push_back(ref(why));
		// ボタンの操作は、状態のカード (install_notice()) と同じに分ける。ほかの Vencord を読むように
		// 変わっている間は RepairVencordDir (書く場面が FixRequested になる。dock/dock_poll.cpp の
		// WriteVencordDir の場面の分け方)、ほかは WriteVencordDir にする。このカードは同じテキストの状態の
		// カードの代わりになる (merge_failure_cards()) ので、修正する操作を失わないようにする。ただし、
		// 押しても同じ起動の中では同じ失敗になる場合 (vesktop_add_retry_can_differ() が偽) はボタンを置かない。
		// そのときも、カードは状態のカードの代わりのままである (merge_failure_cards() はボタンの有無で
		// 変わらない)。
		if (vesktop_add_retry_can_differ(*ops.vesktop_add_failure)) {
			const ActionId action = obs.install.dir == VencordDirState::PointsElsewhere ? ActionId::RepairVencordDir
												       : ActionId::WriteVencordDir;
			ViewButton b = make_button(obs, action, ActionArgs{}, keys::kDockAddToVesktopButton);
			n.buttons.push_back(std::move(b));
		}
		out->push_back(std::move(n));
		return;
	}
	case AddedRuleId::RecordGap:
		// 途切れ 1 つにつき 1 行 (いつから何分か)。その録画のセッションが終わるまで表示する
		// (次のセッションを始めると、録画の側が空にする)。
		for (const RecordGapFact &gap : obs.recording.gaps) {
			const long long minutes = minutes_ceil_of_us(gap.duration_us);
			dockview::Notice n =
				make_notice(name, Tier::L3,
					    ref(text::pick(keys::kDockRecordGapHeadline, minutes),
						{when_arg(obs, gap.start_us), count_text(minutes)}));
			n.key = name + ":" + std::to_string(gap.start_us);
			out->push_back(std::move(n));
		}
		return;
	case AddedRuleId::CallActionFailed:
		// 押してから時間がたっても通話が変わらない。送信側は失敗を返さないことがあるので、見出しは Vesktop での操作を勧める
		// 文にする。もう一度押すか、結果が出ると消える。
		if (call_action_failed(obs) && !uninstall_started(obs)) {
			const bool move = obs.ops.call_action->kind == CallActionKind::Move;
			dockview::Notice n = make_notice(
				name, Tier::L2, ref(move ? keys::kDockCallMoveFailedHeadline : keys::kDockCallLeaveFailedHeadline));
			n.body.push_back(ref(keys::kDockCallActionFailedText));
			out->push_back(std::move(n));
		}
		return;
	case AddedRuleId::VesktopRemoveFailed: {
		// 失敗のフラグが残っていても、今 Vesktop の設定が同梱の Vencord を指していなければメッセージにしない (外せたなら、
		// 今どうなっているかは [Vesktopに追加] のカードが伝える。VesktopAddFailed と同じく、実際の状態で伝える)。
		// メッセージのレベルはいつも 2 にする。外せなかったときは、Vesktop の設定が押す前のままで、配信の枠の映像は
		// 何も損なわれていない。ボタンは置かない ([Vesktopから削除] は状態タブの「Vesktop」の組にある)。
		const VencordDirState dir = obs.install.dir;
		if (!ops.vesktop_remove_failure.has_value() || uninstall_started(obs) ||
		    (dir != VencordDirState::Matches && dir != VencordDirState::HashMismatch))
			return;
		dockview::Notice n = make_notice(name, Tier::L2, ref(keys::kDockVesktopRemoveFailedHeadline));
		n.body.push_back(
			ref(vesktop_remove_failure_text(*ops.vesktop_remove_failure, ops.settings_unreadable_at_startup)));
		out->push_back(std::move(n));
		return;
	}
	case AddedRuleId::SeenDiscarded:
		if (obs.seen.discarded_with_marks > 0) {
			const int count = obs.seen.discarded_with_marks;
			dockview::Notice n = make_notice(
				name, Tier::L3, ref(text::pick(keys::kDockPastRemovedHeadline, count), {count_text(count)}));
			n.tip = ref(keys::kDockPastRemovedText);
			out->push_back(std::move(n));
		}
		return;
	}
}

// ---- 人の行 ----

// 押せない理由のテキストに、人の名前を入れる (apply() はキーだけを返す)。
void fill_voice_taken_args(const Observation &obs, ViewButton *button)
{
	if (!button->disabled_reason.has_value())
		return;
	if (button->disabled_reason->id != keys::kParticipantsVoiceTakenText || !button->disabled_reason->args.empty())
		return;
	const std::string &source = button->args.voice_user_id;
	const std::string owner = voice_source_owner_of(obs, source, button->args.user_id);
	button->disabled_reason->args = {voice_source_name(obs, source), name_of_user(obs, owner)};
}

// 「使う声」のメニューの項目。surface_window が真なら、参加者の設定ウィンドウのテキスト (cordmux.participants.*)
// を使う。
std::vector<VoiceChoice> voice_choices(const Observation &obs, const PersonFact &person, bool surface_window)
{
	std::vector<VoiceChoice> out;
	const std::string current = voice_source_of(obs, person.user_id);

	// 先頭は「本人の声」。現在の値はチェックで示す (押せない項目と理由で表さない)。
	VoiceChoice own;
	own.label = Label::from_text(ref(surface_window ? keys::kParticipantsOwnVoiceStatus : keys::kDockRowOwnVoiceMenu));
	own.checked = current.empty();
	ActionArgs reset;
	reset.user_id = person.user_id;
	own.action = make_button(obs, ActionId::SetVoiceSource, reset,
				 surface_window ? keys::kParticipantsOwnVoiceStatus : keys::kDockRowOwnVoiceMenu);
	// 「戻す設定がない」は、現在の値が本人の声であること (選んでいる項目) そのものなので、押せない理由にしない。
	if (own.action.disabled_reason.has_value() && own.action.disabled_reason->id == reason::kNoVoiceSource)
		own.action.disabled_reason.reset();
	out.push_back(std::move(own));

	// Vesktop のアカウントは、声を置き換える人にも、使う声のアカウントにもしない。Vesktop のアカウント自身の声と
	// 画面共有は、受信側に届かないためである (observation.h の PersonFact::is_receiver)。
	if (person.is_receiver)
		return out;

	std::vector<const PersonFact *> candidates;
	for (const PersonFact &other : obs.people) {
		if (!other.in_roster || other.user_id.empty() || other.user_id == person.user_id || other.is_receiver)
			continue;
		candidates.push_back(&other);
	}
	std::sort(candidates.begin(), candidates.end(), [](const PersonFact *a, const PersonFact *b) {
		const std::string an = person_name(*a);
		const std::string bn = person_name(*b);
		if (an != bn)
			return natural_less(an, bn);
		return a->user_id < b->user_id;
	});
	for (const PersonFact *other : candidates) {
		VoiceChoice choice;
		const std::string other_name = person_name(*other);
		choice.label = Label::from_literal(other_name);
		choice.checked = current == other->user_id;
		ActionArgs args;
		args.user_id = person.user_id;
		args.voice_user_id = other->user_id;
		choice.action = make_button(obs, ActionId::SetVoiceSource, args, keys::kDockRowVoiceFromMenu);
		choice.action.label = ref(keys::kDockRowVoiceFromMenu);
		// 今選んでいる人はチェックで示す (押せない理由にしない)。
		if (choice.action.disabled_reason.has_value() &&
		    choice.action.disabled_reason->id == reason::kSameVoiceSource)
			choice.action.disabled_reason.reset();
		// この行の人の声が、既にほかの人の声として使われている (連鎖させない)。理由は「「この行の人」の声は、
		// 既に「使っている人」の行で使っています。」で、テキストは 1 対 1 の理由と同じ
		// (participants.voice_taken.text)。lent は理由を書き替える前に保存しておく。書き替えた後は 1 対 1 の
		// 理由と同じ id になり、下のドックの短い理由の条件が成り立って、候補の人の声を使っている人
		// (いないので空) でテキストを作ってしまう (「bob (既に「」の行で使っています)」になる)。
		const bool lent = choice.action.disabled_reason.has_value() &&
				  choice.action.disabled_reason->id == reason::kVoiceSourceLent;
		if (lent) {
			const std::string user = voice_source_owner_of(obs, person.user_id);
			choice.action.disabled_reason =
				ref(keys::kParticipantsVoiceTakenText, {voice_source_name(obs, person.user_id), name_of_user(obs, user)});
		}
		fill_voice_taken_args(obs, &choice.action);
		// 候補の人が、ほかの人の声を使っている (連鎖させない)。テキストは「「%1」は、ほかの人の声を
		// 使っています。」で、%1 は候補の人の名前である (apply() はキーだけを返す)。名前は項目のテキストと同じ
		// person_name() にする (同じ項目の中で、同じ人を別の名前で呼ばない)。
		const bool uses_other = choice.action.disabled_reason.has_value() &&
					choice.action.disabled_reason->id == keys::kParticipantsVoiceUsesOtherText;
		if (uses_other && choice.action.disabled_reason->args.empty())
			choice.action.disabled_reason->args = {other_name};
		// ドックのメニューでは、押せない項目に短い理由を括弧で付ける。ダイアログは理由をツールチップで伝える。
		// lent の項目は名前だけにし、長い理由 (「「carol」の声は、既に「dave」の行で使っています。」) は
		// ツールチップにする。短い理由のキー row_voice_taken.menu は「候補の人の声が別の行で使われている」
		// の形で、lent (この行の人の声が使われている) の事実を伝えられない。row_voice_taken.menu に行の人の
		// 名前を入れないのは、「bob (既に「dave」の行で使っています)」が bob の声のことに読め、
		// 事実と違うためである。
		if (!surface_window && !lent && choice.action.disabled_reason.has_value() &&
		    choice.action.disabled_reason->id == keys::kParticipantsVoiceTakenText) {
			const std::string owner = voice_source_owner_of(obs, other->user_id, person.user_id);
			choice.label = Label::from_text(ref(keys::kDockRowVoiceTakenMenu, {other_name, name_of_user(obs, owner)}));
		}
		if (!surface_window && uses_other)
			choice.label = Label::from_text(ref(keys::kDockRowVoiceUsesOtherMenu, {other_name}));
		out.push_back(std::move(choice));
	}
	return out;
}

// [名前を変更...] のボタン。名前はダイアログで入力するので、args.name は空のままにする。押せるかは apply() が
// 決めるが、空の名前は「上書きをやめる」の意味になるので、押せるかを問い合わせるときは、今と違う空でない名前を
// 渡す (ダイアログを開くことは上書きの有無にかかわらずできるので、押しても何も起きない操作にはならない)。
ViewButton rename_button(const Observation &obs, const std::string &user_id, const char *label)
{
	ActionArgs probe;
	probe.user_id = user_id;
	probe.name = name_override_of(obs, user_id) + std::string(1, dockview::kTimeArgMark);
	ViewButton b = make_button(obs, ActionId::SetNameOverride, probe, label);
	b.args.name.clear();
	return b;
}

ViewButton always_on_button(const Observation &obs, const std::string &user_id, const char *label,
			    bool surface_window)
{
	ActionArgs args;
	args.user_id = user_id;
	args.on = !always_on_for(obs, user_id);
	ViewButton b = make_button(obs, ActionId::SetAlwaysOn, args, label);
	if (surface_window && b.disabled_reason.has_value() && b.disabled_reason->id == keys::kDockRowAutoAddDisabledTip)
		b.disabled_reason = ref(keys::kParticipantsAutoAddDisabledTip);
	return b;
}

// 行のメニューの [全画面で表示]。この行の人を全画面で表示していればチェックを付け、押すと元の配置に戻す (args.user_id が
// 空)。ほかの行では、押すとこの行の人を全画面にする (全画面にしている人がほかにいれば、その人から替える)。押せるかは
// apply() が決める (合成のソースのタイルに今描かれている人だけを押せる)。
void focus_menu_item(const Observation &obs, const std::string &user_id, dockview::RowMenuView *menu)
{
	menu->focus = !user_id.empty() && user_id == obs.choices.focus_user_id;
	ActionArgs args;
	if (!menu->focus)
		args.user_id = user_id;
	menu->focus_toggle = make_button(obs, ActionId::SetFocus, args, keys::kDockRowFullScreenMenu);
}

// 1 行目の「全画面: %1」の %1。ドックが渡す配信に出している名前を使い、空のときだけ行の名前にする。
std::string focus_display_name(const Observation &obs)
{
	if (!obs.choices.focus_name.empty())
		return obs.choices.focus_name;
	if (const PersonFact *person = find_person(obs, obs.choices.focus_user_id); person != nullptr)
		return person_name(*person);
	return obs.choices.focus_user_id;
}

// エンコードの段階から、画質の状態のテキストを決める。段階の名前が分からないときは、決まったテキストにする
// (内部の名前を画面に入れない)。枠に載っているかも、最初のフレームが届いたかも見ない。段階は送信側が伝える
// 事実で、枠に載せていない人の行に「映像を表示できません」が表示されれば、載せる前に「載せても表示できない」と
// 分かり、次の操作が変わる (フレームが届く前でも、configure のストリームから段階がその人に付く)。枠で絞らない
// のは、枠に載せていない録画の対象の人について、映像が届かないメッセージが画面共有を始め直してもらうよう勧めたときに、
// どの人のことかを人の行で見分けられなくなるためである。
std::optional<TextRef> quality_of(const PersonFact &person)
{
	if (!person.stage.has_value())
		return std::nullopt;
	const std::string &stage = *person.stage;
	if (stage.empty() || stage == kStageNative)
		return std::nullopt;
	if (stage == kStageHalfFps)
		return ref(keys::kDockRowFpsReducedStatus);
	if (stage == kStageDownscaled)
		return ref(keys::kDockRowResolutionReducedStatus);
	// software の段階は 1280x720 と 30 FPS に固定するフォールバックで、解像度とフレームレートの両方が下がる。
	if (stage == kStageSoftware)
		return ref(keys::kDockRowBothReducedStatus);
	if (stage == kStageUnavailable)
		return ref(keys::kDockRowCantShowStatus);
	return ref(keys::kDockRowQualityReducedStatus);
}

// 解像度とフレームレート (桁を区切らない)。フレームレートを読めないときは、大きさだけにする。
// 映像を表示できない段階 (unavailable。送信側がその人の映像を送らない) の間は表示しない。送っていない映像の大きさを
// 表示することになるためである。送信側はその間 configure の 3 つのフィールドを省くが、段階は facts で、大きさは
// configure で別々に届くので、2 つのメッセージの間は古い大きさが残ることがある。ここでも判定して、届く順に
// よらないようにする (送信側が省くのに任せると、その間、「映像を表示できません」と「1280x720 (30 FPS)」が同じ行の
// ツールチップと accessibleDescription に並ぶ)。
// 最初のフレームが届くまでも表示しない。フレームが 1 枚もない間の大きさは configure のストリームの値 (送信側が
// 段階を決めた時点の値。dock/build_observation.cpp が付ける) で、まだ届いていない映像の大きさになる。枠に載せて
// いる人の行では「映像を待っています...」の横に並び、行の中で食い違って読める。「届いた」は video_time() (枠に
// 載せている人は枠への経路のあるストリームのフレーム、載せていない人は録画の入力のフレーム) で見て、stalled_video()
// と first_frame_missing() と同じ判定にする。そのため、枠に載せていない人の行には、cordmux の録画が録画の入力に
// フレームを通している間だけ大きさを表示する (届いている映像の大きさなので事実である)。録画していない、枠に
// 載せていない人には表示しない。
// 枠に載せていない人を一律に除かないのは、録画の入力に届いている映像の大きさという事実まで落とすためである。
// last_video_frame_at_us だけで「届いた」を見ないのは、行と映像が届かないメッセージで「フレームが届いた」の定義が
// 2 つになるためである。
std::optional<TextRef> resolution_of(const PersonFact &person)
{
	if (stage_unavailable(person))
		return std::nullopt;
	if (!video_time(person).has_value())
		return std::nullopt;
	if (!person.width.has_value() || !person.height.has_value())
		return std::nullopt;
	const std::string w = std::to_string(*person.width);
	const std::string h = std::to_string(*person.height);
	if (person.fps.has_value()) {
		const std::string fps = format_fps(*person.fps);
		if (fps != "-")
			return ref(keys::kDockRowResolutionStatus, {w, h, fps});
	}
	return ref(keys::kDockRowResolutionSizeStatus, {w, h});
}

// 自分の枠のチェックが付いていて、OBS から選んだ映像が見つからないか。self_frame_bound は、シーンの読み込みの前と、
// cordmux のソースがない間も false なので、cordmux のソースがあると分かっている間だけ見る (ない間は、ソースがないことを
// 伝える規則が伝える)。cordmux を無効にしている間も表示しない。
bool self_frame_video_missing(const Observation &obs, const PersonFact &person)
{
	const Choices &c = obs.choices;
	return person.is_self_frame && c.feature != FeatureSwitch::Off && c.self_frame_selected && c.self_frame_on &&
	       !c.self_frame_bound && c.composite_checked && c.composite_present;
}

// 人の行の状態のテキスト。(自分の枠の映像が見つからない)、画面共有していない、
// (映像を表示できない段階)、まだ届いていない、届かなくなった、の順に見て、最初の 1 つにする。枠に載せていない人には
// 表示しない (cordmux の録画の対象の人と、自分の枠の映像が見つからない人は例外。下のコメント)。
// 映像を表示できない段階 (stage_unavailable()) の間は、まだ届いていない・届かなくなったのテキストを表示せず、行の
// 状態を空にする。状態のテキストがないときに表示される画質のテキスト (quality_of() の「映像を表示できません」) が、
// しきい値を超える前と同じく行に残るようにするためである。段階は原因で、フレームが届かないのはその結果である
// (送信側はこの段階で映像の読み出しを止め、同じストリームを確保し直さない。src-vencord/encode/session.ts)。映像が
// 届かないメッセージ (AllVideoStoppedHint) は、しきい値を超えた unavailable の人について「映像を表示できない人に、
// 画面共有を始め直してもらってください。」と伝えるので、行が「映像が届いていません」に替わると、メッセージが指す人を、
// 行に表示されたテキストで見分けられない (行・ツールチップ・メッセージが同じ原因を伝えるようにする)。
// 「まだ届いていない」より先に見る。最初の確保で段階が尽きた人には、フレームが 1 枚も届かない (session.ts の
// start()) ので、段階がその人に付いていれば、行は「映像を待っています...」ではなく「映像を表示できません」になる。
// 画面共有していないより後に見る。画面共有をやめた人の段階は前の画面共有のときの値で、現在の枠の事実として
// 画面共有していないが先である。
// RowState に「表示できない」を追加しないのは、しきい値を超える前の表示 (画質のアイコンとテキスト) と別の形になり、
// ドックのアイコンの選び方 (mark_for) とログの名前にも新しい場合が必要になり、同じ事実を 2 つの形で表示することになる
// ためである。
// しきい値の時間内に段階が後から unavailable に替わったときは、表示していた「映像が届いていません」は消すまでの待ち
// (Settle::ClearOnly) の間だけ残り、その後「映像を表示できません」になる (notice_tracker.h の、行の状態の待ち)。
// 枠に載せていない人のうち、cordmux の録画の対象の人 (record_target()) は例外である。映像が届かないメッセージ
// (AllVideoStoppedHint) は、枠に載っているか録画の対象の人を数えて人数だけを伝え、誰のことかは人の行に任せる。
// そのため、録画の対象だけの人の行に何も表示しないと、その人の映像が止まったときに、どこで起きているかが画面に
// なくなる。その人には、まだ届いていないと届かなくなっただけを表示する (画面共有していないは、record_target() が
// 画面共有している人だけなので当てはまらない)。ツールチップの 1 文は、枠のことを
// 言わない録画の文 (row_waiting_recording.tip と row_not_receiving_recording.tip。RowStatus::tip) にする。
// 届いたかは video_time() (載せていない人は録画の入力のフレーム) で見て、メッセージの数え方と同じ判定にする。
// 録画の対象でない人に表示しないのは、録画していない人の映像は録画の入力を通らず、届いているかが分からないため
// である。
// 自分の枠のチェックが付いていて、選んだ映像が見つからない人 (self_frame_video_missing()) も、枠に載っていないので
// 例外である。この状態を表示しないと、チェックが付いているのに何も映らない理由が画面にない。
std::optional<RowStatus> row_status_of(const Observation &obs, const PersonFact &person)
{
	RowStatus s;
	if (self_frame_video_missing(obs, person)) {
		s.state = RowState::NoVideo;
		s.text = ref(keys::kDockRowSelfVideoMissingStatus);
		s.tip = ref(keys::kDockRowSelfVideoMissingTip);
		s.settle = Settle::Debounced; // 映像を選んだ直後の周期に、結び付く前の値で一瞬表示しないように待つ
		return s;
	}
	const bool slotted = person.slot.has_value();
	if (!slotted && !record_target(obs, person))
		return std::nullopt;
	// 「画面共有していません」は、画面共有の事実が届いていて、していないと分かっているときだけにする。
	// Vesktop のアカウントが通話に参加していない間は、誰の画面共有も見えないので、していないと分からない。行の状態を空にする。
	if (person.sharing_known && !person.sharing) {
		if (receiver_not_in_call(obs))
			return std::nullopt;
		s.state = RowState::NotSharing;
		s.text = ref(keys::kDockRowNotSharingStatus);
		s.tip = ref(keys::kDockRowNotSharingTip);
		return s;
	}
	// 映像を表示できない段階では、行の状態を空にして、画質のテキストの「映像を表示できません」を表示する
	// (上のコメント)。
	if (stage_unavailable(person))
		return std::nullopt;
	// 映像を受け取ると決めていない間は、届かないのが当然なので、まだ届いていない・届かなくなったを表示しない
	// (映像が届かないメッセージと同じ判断。video_expected())。
	if (!video_expected(obs))
		return std::nullopt;
	if (!video_time(person).has_value()) {
		s.state = RowState::Waiting;
		s.text = ref(keys::kDockRowWaitingStatus);
		s.tip = ref(slotted ? keys::kDockRowWaitingTip : keys::kDockRowWaitingRecordingTip);
		return s;
	}
	if (stalled_video(obs, person)) {
		s.state = RowState::NoVideo;
		s.text = ref(keys::kDockRowNotReceivingStatus);
		s.tip = ref(slotted ? keys::kDockRowNotReceivingTip : keys::kDockRowNotReceivingRecordingTip);
		s.settle = Settle::ClearOnly; // しきい値そのものが表示するまでの待ちになる。消すときは待つ
		return s;
	}
	return std::nullopt;
}

dockview::PersonRow make_person_row(const Observation &obs, const PersonFact &person,
				    const std::map<std::string, std::string> &voice_links,
				    const std::map<std::string, int> &discord_name_counts,
				    const std::map<std::string, int> &visible_name_counts)
{
	dockview::PersonRow row;
	row.user_id = person.user_id;
	const std::string visible = person_name(person);
	row.name = Label::from_literal(visible);
	row.checkable = !person.is_receiver;
	row.checked = person.slot.has_value();
	row.slot = person.slot;

	ActionArgs slot_args;
	slot_args.user_id = person.user_id;
	slot_args.kind = config::SlotKind::Screen;
	if (person.slot.has_value()) {
		slot_args.slot = 0;
	} else {
		const int free_slot = free_screen_slot(obs);
		slot_args.slot = free_slot != 0 ? free_slot : 1;
	}
	// チェックのウィジェットはテキストを持たないので、ボタンのテキストはスクリーンリーダー向けのテキスト
	// (row_check.a11y。「枠に載せる」) にする。
	row.check_action = make_button(obs, ActionId::SetSlot, slot_args, keys::kDockRowCheckA11y);
	// 自分の枠の人の行は、自分の枠に映す映像を OBS から選んでいる間、チェックが自分の枠の付け外しになる。選んでいない
	// 間も、サブアカウントがなければ (その人が Vesktop のアカウントなら) チェックを出し、押せない理由を付ける。
	// Discord の画面共有を枠に載せる経路がないためである。サブアカウントがあれば、ほかの人と同じ行のままにする。
	// チェックの状態は自分の枠を出すか。押したときの向きは現在の逆で、出す向きは Discord の人で枠が埋まっていると押せない
	// (理由は actions.cpp の SetSelfFrame)。Vesktop のアカウントの行 (checkable が偽) もチェックだけは出す。
	row.self_frame_check = person.is_self_frame && (obs.choices.self_frame_selected || person.is_receiver);
	if (row.self_frame_check) {
		ActionArgs self_args;
		self_args.user_id = person.user_id;
		self_args.on = !obs.choices.self_frame_on;
		// 映像を選んでいない間は枠に載らないので、残っている値があってもチェックを付けて見せない。
		row.checked = obs.choices.self_frame_selected && obs.choices.self_frame_on;
		row.check_action = make_button(obs, ActionId::SetSelfFrame, self_args, keys::kDockRowCheckA11y);
	}
	row.check_enabled = (row.checkable || row.self_frame_check) && !row.check_action.disabled_reason.has_value();

	row.sharing = person.sharing_known && person.sharing;
	row.status = row_status_of(obs, person);
	// 映像を受け取ると決めていない間は、送信側が映像を止めている。前に受け取っていたときの段階 (映像を表示できない
	// など) を、現在の事実として表示しない。
	// Vesktop のアカウントが通話に参加していない間の、画面共有していないと見える人 (row_status_of() が状態を空にした人) の
	// 段階は、前の画面共有のときの値で、現在の事実か分からないので、画質のテキストも表示しない。
	const bool sharing_unobservable = receiver_not_in_call(obs) && person.sharing_known && !person.sharing;
	row.quality = video_expected(obs) && !sharing_unobservable ? quality_of(person) : std::nullopt;
	row.resolution = resolution_of(person);

	// 2 行目。部分ごとに分けて持ち、区切りの文字でつながない (つなぐのは表示する側)。
	if (person.is_receiver) {
		// 「サブアカウント」は、メインアカウントと別のアカウントで Vesktop を使う構成の Vesktop のアカウントだけに付ける。
		// メインアカウントだけの構成 (メインアカウントの userId が空か、Vesktop のアカウントと同じ) では、その人が
		// メインアカウントである。
		if (!obs.choices.broadcaster_user_id.empty() && person.user_id != obs.choices.broadcaster_user_id)
			row.line2_parts.push_back(ref(keys::kDockRowReceivingAccountStatus));
	} else {
		const auto linked = voice_links.find(person.user_id);
		if (linked != voice_links.end()) {
			const std::string source_name = voice_source_name(obs, linked->second);
			// 「声: %1 (通話に参加していません)」の形は、通話の参加者の一覧を読めていて、そこにいないときだけにする
			// (読めていないことを、参加していないと扱わない)。
			const bool absent = obs.call.roster_count.has_value() && !in_roster(obs, linked->second);
			row.line2_parts.push_back(
				ref(absent ? keys::kDockRowVoiceAbsentStatus : keys::kDockRowVoiceStatus, {source_name}));
		}
		if (has_name_override(obs, person.user_id) && person.discord_name.has_value())
			row.line2_parts.push_back(ref(keys::kDockRowDiscordNameStatus, {*person.discord_name}));
	}
	if (person.discord_name.has_value()) {
		const auto found = discord_name_counts.find(*person.discord_name);
		row.same_discord_name_exists = found != discord_name_counts.end() && found->second > 1;
	}
	{
		const auto found = visible_name_counts.find(visible);
		row.same_visible_name_exists = found != visible_name_counts.end() && found->second > 1;
	}
	if (!person.is_receiver && row.same_discord_name_exists && person.username.has_value())
		row.line2_parts.push_back(ref(keys::kDockRowUsernameStatus, {*person.username}));
	// アクセシブル名に使うユーザー名 (2 行目に表示するかとは別。dock_view.h の PersonRow::username)。Vesktop の
	// アカウントの行はメニュー ([...]) を作らないので、持たせない。
	if (!person.is_receiver)
		row.username = person.username;

	// サブアカウントがない構成の自分の行 (Vesktop のアカウントの行) は、メニューに [自動で載せる] と [全画面で表示] だけを
	// 出す。名前の変更と使う声と音量は、受信側に届かない人には適用されない。自分の枠は全画面にできる。
	if (person.is_receiver && row.self_frame_check) {
		row.menu.always_on = always_on_for(obs, person.user_id);
		row.menu.always_on_toggle = always_on_button(obs, person.user_id, keys::kDockRowAutoAddMenu, false);
		focus_menu_item(obs, person.user_id, &row.menu);
		row.menu.auto_add_only = true;
	}
	if (!person.is_receiver) {
		row.menu.always_on = always_on_for(obs, person.user_id);
		row.menu.always_on_toggle = always_on_button(obs, person.user_id, keys::kDockRowAutoAddMenu, false);
		focus_menu_item(obs, person.user_id, &row.menu);
		row.menu.rename = rename_button(obs, person.user_id, keys::kDockRowRenameMenu);
		row.menu.name_override = name_override_of(obs, person.user_id);
		row.menu.voices = voice_choices(obs, person, false);
		// この行の人の声をほかの行の人が使っていて、現在の値が本人の声のときは、「使う声」の項目そのものを
		// 押せなくし、括弧の理由を 1 回だけ表示する。理由は候補の全員で同じ
		// (どの候補を選んでも連鎖になる。actions.cpp の SetVoiceSource の kVoiceSourceLent と同じ事実) なので、
		// 候補ごとに並べない。現在の値が本人の声でない (手で書き換えて衝突した設定) ときは押せなくしない。
		// 「本人の声」に戻す操作を残すためである (戻す向きは設定の有無だけを見る。actions.cpp)。
		// そのときの候補は、名前だけとツールチップにする (voice_choices() の lent の場合)。候補を、
		// 理由の見えない名前だけの押せない項目にしないのは、なぜ選べないのかが分からなくなるためである。
		// 声の名前は、使っている人の行の 2 行目「声: carol」(row_voice.status) と同じ voice_source_name()
		// にする (同じ声を画面の 2 か所で違う名前で呼ばない)。
		const std::string borrower = voice_source_owner_of(obs, person.user_id);
		if (!borrower.empty() && voice_source_of(obs, person.user_id).empty()) {
			const std::string voice_name = voice_source_name(obs, person.user_id);
			const std::string borrower_name = name_of_user(obs, borrower);
			row.menu.voices_disabled_label = ref(keys::kDockRowVoiceFromLentMenu, {voice_name, borrower_name});
			row.menu.voices_disabled_reason = ref(keys::kParticipantsVoiceTakenText, {voice_name, borrower_name});
		}
	}

	// 音量とミュートの値。Vesktop のアカウントの行は値を持たない (声も画面共有も受信側に届かない)。
	if (!person.is_receiver) {
		row.audio.applies = obs.choices.composite_present;
		row.audio.is_main_account =
			!obs.choices.broadcaster_user_id.empty() && person.user_id == obs.choices.broadcaster_user_id;
		row.audio.voice_muted = person.audio.voice_muted;
		row.audio.voice_volume_pct = person.audio.voice_volume_pct;
		row.audio.has_screen_audio = person.audio.has_screen_audio;
		row.audio.screen_muted = person.audio.screen_muted;
		row.audio.screen_volume_pct = person.audio.screen_volume_pct;
	}

	// 押した直後に空いている枠がなくなって、載せられなかった。押した操作への返事なので、押した後に原因 (空いている枠がない) が
	// 途切れずに続く間だけ表示する (observation.h の slot_refusal_finished。枠が空いたら終え、後でまた埋まっても出さない)。
	// 短い形にするのは、同じ理由の全文を一覧の上の 1 行 (list_reason) が伝えるためで、押した所のそばには結果だけを置く。
	if (!obs.ops.last_slot_refused_user_id.empty() && obs.ops.last_slot_refused_user_id == person.user_id &&
	    !slot_refusal_finished(obs))
		row.refused_note = ref(keys::kDockNoFreeSlotShortText);
	return row;
}

// ---- 参加者タブの一覧 ----

dockview::SlotPosition position_of_cell(int cell)
{
	switch (cell) {
	case 0:
		return dockview::SlotPosition::TopLeft;
	case 1:
		return dockview::SlotPosition::TopRight;
	case 2:
		return dockview::SlotPosition::BottomLeft;
	case 3:
		return dockview::SlotPosition::BottomRight;
	default:
		break;
	}
	return dockview::SlotPosition::None;
}

// userId ごとの枠の位置。配信の配置 (dock/dock_poll_layout.cpp の push_composite_input) と同じ人と同じ名前を、同じ関数
// (ledger/slot_cells.h の plan_slot_cells) に渡すので、行に表示する位置と配信の位置は必ず一致する。ダミー映像を表示している
// 間は、配信に映っているのはダミーなので、誰の行にも位置を表示しない。
std::map<std::string, dockview::SlotPosition> slot_positions(const Observation &obs)
{
	std::map<std::string, dockview::SlotPosition> out;
	if (obs.choices.dummy == FeatureSwitch::On)
		return out;
	std::vector<SlotCellShare> shares;
	for (const PersonFact &person : obs.people) {
		if (!person.slot.has_value() || !person.slot_visible)
			continue;
		SlotCellShare share;
		share.slot = *person.slot;
		share.name = person.display_name.value_or(std::string());
		share.user_id = person.user_id;
		share.added_order = person.slot_added_order;
		share.is_main = !obs.choices.broadcaster_user_id.empty() && person.user_id == obs.choices.broadcaster_user_id;
		shares.push_back(std::move(share));
	}
	// 自分の枠の置き方。名前と user_id は、配信の配置 (CompositeInput::self_name と self_user_id) と同じ関数で決めた値
	// (dock/build_observation.h の self_frame_identity) を使う。自分の枠の人の PersonFact から取ると、その人が通話にいない
	// ときに名前が空になり、配信と違う位置になる。
	SlotCellSelf self;
	self.shown = obs.choices.self_frame_shown;
	self.first = obs.choices.self_slot_first;
	self.added_mark = obs.choices.self_frame_added_mark;
	self.name = obs.choices.self_frame_name;
	self.user_id = obs.choices.self_frame_user_id;
	const SlotCellPlan plan = plan_slot_cells(shares, self, obs.choices.slot_order, kFullWhenSingle);
	for (const SlotCell &cell : plan.cells) {
		const dockview::SlotPosition position = plan.full ? dockview::SlotPosition::Full : position_of_cell(cell.cell);
		if (!cell.self) {
			out[shares[static_cast<std::size_t>(cell.share)].user_id] = position;
			continue;
		}
		for (const PersonFact &person : obs.people) {
			if (person.is_self_frame)
				out[person.user_id] = position;
		}
	}
	return out;
}

dockview::MicState mic_state_of(const PersonFact &person)
{
	if (person.audio.voice_muted)
		return dockview::MicState::Muted;
	return person.voice_arriving ? dockview::MicState::Normal : dockview::MicState::NoVoice;
}

dockview::SpeakerState speaker_state_of(const PersonFact &person)
{
	if (!person.audio.has_screen_audio)
		return dockview::SpeakerState::Hidden;
	return person.audio.screen_muted ? dockview::SpeakerState::Muted : dockview::SpeakerState::Normal;
}

// 枠が埋まっているために押せないチェックか。自分の枠の付け外しのチェックは、誰かと入れ替えるものではないので含めない。
bool check_blocked_by_full_slots(const dockview::PersonRow &row)
{
	if (row.self_frame_check || row.checked || !row.check_action.disabled_reason.has_value())
		return false;
	const std::string &reason_id = row.check_action.disabled_reason->id;
	return reason_id == keys::kDockNoFreeSlotText || reason_id == keys::kDockSelfSlotLimitText;
}

// 「入れ替える相手」のメニューの項目。枠に載っている人を、位置の順に並べる (映っていない人は、後ろに枠の番号の順に並べる)。
std::vector<dockview::ReplaceChoice> replace_choices_for(const Observation &obs, const std::string &user_id,
							 const std::map<std::string, dockview::SlotPosition> &positions)
{
	std::vector<const PersonFact *> slotted;
	for (const PersonFact &person : obs.people) {
		if (person.slot.has_value())
			slotted.push_back(&person);
	}
	const auto position_of = [&positions](const PersonFact &person) {
		const auto found = positions.find(person.user_id);
		return found == positions.end() ? dockview::SlotPosition::None : found->second;
	};
	std::sort(slotted.begin(), slotted.end(), [&position_of](const PersonFact *a, const PersonFact *b) {
		const dockview::SlotPosition pa = position_of(*a);
		const dockview::SlotPosition pb = position_of(*b);
		const bool shown_a = pa != dockview::SlotPosition::None;
		const bool shown_b = pb != dockview::SlotPosition::None;
		if (shown_a != shown_b)
			return shown_a;
		if (pa != pb)
			return static_cast<int>(pa) < static_cast<int>(pb);
		return *a->slot < *b->slot;
	});
	std::vector<dockview::ReplaceChoice> out;
	for (const PersonFact *person : slotted) {
		dockview::ReplaceChoice choice;
		choice.name = Label::from_literal(person_name(*person));
		choice.position = position_of(*person);
		ActionArgs args;
		args.user_id = user_id;
		args.replace_user_id = person->user_id;
		args.slot = *person->slot;
		choice.action = make_button(obs, ActionId::ReplaceSlot, args, keys::kDockReplaceWithMenu);
		out.push_back(std::move(choice));
	}
	return out;
}

// 参加者タブの一覧に行を出す人か。通話に参加している人、Vesktop のアカウント、自分の枠の人だけを出す。枠を持っていても
// 通話にいない人 (参加者が分からない間に、枠を持ったまま残っている人) は、行にしない。
bool listed_in_people_list(const PersonFact &person)
{
	return person.in_roster || person.is_receiver || person.is_self_frame;
}

// 参加者タブの一覧。接続している間は通話の人、接続していない間は切れる前に分かっていた人を、行にする。
// 接続していない間の行は off にして、行の状態を持たず、どのボタンも押せなくする (切れる前の組のまま残すので、行は動かない)。
// 自分の枠の付け外しも押せなくする。行の見た目を 1 つにして、薄い行に押せるものを混ぜないためである。
dockview::PeopleListView people_list_of(const Observation &obs, const std::map<std::string, std::string> &voice_links)
{
	dockview::PeopleListView list;
	const bool connected = obs.link.state == LinkState::Connected;
	std::vector<const PersonFact *> sources;
	if (!uninstall_started(obs)) {
		std::set<std::string> listed;
		if (!connected) {
			for (const PersonFact &person : obs.last_known_people) {
				if (listed_in_people_list(person) && listed.insert(person.user_id).second)
					sources.push_back(&person);
			}
		}
		for (const PersonFact &person : obs.people) {
			if (listed_in_people_list(person) && listed.insert(person.user_id).second)
				sources.push_back(&person);
		}
	}
	std::sort(sources.begin(), sources.end(), [](const PersonFact *a, const PersonFact *b) {
		const std::string an = person_name(*a);
		const std::string bn = person_name(*b);
		if (an != bn)
			return natural_less(an, bn);
		return a->user_id < b->user_id;
	});
	std::map<std::string, int> discord_name_counts;
	std::map<std::string, int> visible_name_counts;
	for (const PersonFact *person : sources) {
		if (person->discord_name.has_value())
			++discord_name_counts[*person->discord_name];
		++visible_name_counts[person_name(*person)];
	}

	const bool sub_account = has_sub_account(obs);
	const std::map<std::string, dockview::SlotPosition> positions = slot_positions(obs);
	for (const PersonFact *person : sources) {
		if (person->is_receiver && sub_account)
			continue;
		dockview::PersonRow row = make_person_row(obs, *person, voice_links, discord_name_counts, visible_name_counts);
		const auto position = positions.find(person->user_id);
		if (position != positions.end())
			row.position = position->second;
		row.is_main = sub_account ? person->user_id == obs.choices.broadcaster_user_id : person->is_receiver;
		row.auto_add = always_on_for(obs, person->user_id);
		row.mic = mic_state_of(*person);
		row.speaker = speaker_state_of(*person);
		row.check_replaces = check_blocked_by_full_slots(row);
		if (row.check_replaces) {
			row.replace_choices = replace_choices_for(obs, person->user_id, positions);
			// 薄いチェックは押すと入れ替える相手を選べるので、ツールチップも入れ替えを案内する文にする
			// (押せない理由の「誰かを枠から外すと載せられます」は、押す先を言わない)。
			row.check_action.disabled_reason = ref(keys::kDockRowCheckReplaceTip);
		}
		row.off = !connected;
		if (row.off) {
			row.status.reset();
			row.quality.reset();
			row.check_enabled = false;
			row.check_replaces = false;
			row.replace_choices.clear();
			// 覚えている行の枠の値は切れる前のものである。枠を空にした後もチェックが付いて見えないように、現在の枠に合わせる。
			if (!row.self_frame_check) {
				const PersonFact *now = find_person(obs, person->user_id);
				row.slot = now != nullptr ? now->slot : std::nullopt;
				row.checked = row.slot.has_value();
			}
		}
		if (row.is_main)
			list.pinned = std::move(row);
		else if (row.sharing)
			list.sharing.push_back(std::move(row));
		else
			list.in_call.push_back(std::move(row));
	}

	// 自分の枠を使っている間は、映像がまだ結び付いていなくても自分の枠を 1 つと数える。Discord の人を載せられる人数
	// (self_frame_limit_reached) と同じ条件にして、「枠3/4」なのに 4 人目を載せられない、という表示にしない。
	list.slots_used = std::min(self_frame_in_use(obs) ? discord_slotted_count(obs) + 1 : screen_slotted_count(obs),
				   list.slots_total);
	list.slots_label = ref(keys::kDockSlotsUsedStatus, {std::to_string(list.slots_used), std::to_string(list.slots_total)});
	list.sharing_heading = ref(keys::kDockGroupSharingHeading, {count_text(static_cast<long long>(list.sharing.size()))});
	list.in_call_heading = ref(keys::kDockGroupInCallHeading, {count_text(static_cast<long long>(list.in_call.size()))});
	return list;
}

// ---- 上部のメインとサブ ----

// 値を読めないときの表示。
Label dash_label()
{
	return Label::from_literal("-");
}

Label name_or_dash(const std::string &name)
{
	return name.empty() ? dash_label() : Label::from_literal(name);
}

// 上部の行に表示するアカウントの名前。名前が分からないときは userId を出さず「-」にする。
std::string account_name(const Observation &obs, const std::string &user_id)
{
	if (user_id.empty())
		return std::string();
	if (const PersonFact *person = find_person(obs, user_id); person != nullptr && person->display_name.has_value())
		return *person->display_name;
	if (user_id == obs.vesktop_user_id && !obs.vesktop_name.empty())
		return obs.vesktop_name;
	for (const PersonFact &person : obs.last_known_people) {
		if (person.user_id == user_id && person.display_name.has_value())
			return *person.display_name;
	}
	if (const SeenPerson *seen = find_seen(obs, user_id);
	    seen != nullptr && (override_present(*seen) || !seen->display_name.empty()))
		return seen_name(*seen);
	return std::string();
}

dockview::AccountRow account_row(const std::string &name, dockview::AccountCallState state, const std::string &call_name)
{
	dockview::AccountRow row;
	row.name = name_or_dash(name);
	row.state = state;
	switch (state) {
	case dockview::AccountCallState::NotConnected:
		row.call = Label::from_text(ref(keys::kDockAccountNotConnectedStatus));
		break;
	case dockview::AccountCallState::NoCall:
		row.call = Label::from_text(ref(keys::kDockAccountNoCallStatus));
		break;
	case dockview::AccountCallState::Unknown:
		row.call = dash_label();
		break;
	case dockview::AccountCallState::SameCall:
	case dockview::AccountCallState::OtherCall:
		row.call = name_or_dash(call_name);
		break;
	}
	return row;
}

dockview::AccountsView accounts_of(const Observation &obs)
{
	using dockview::AccountCallState;
	dockview::AccountsView view;
	const bool connected = obs.link.state == LinkState::Connected;
	const bool sub_account = has_sub_account(obs);
	const std::string vesktop_id = vesktop_account_id(obs);
	const auto presence_state = [](Presence presence) {
		switch (presence) {
		case Presence::InCall:
			return AccountCallState::SameCall;
		case Presence::NotInCall:
			return AccountCallState::NoCall;
		case Presence::Unknown:
			break;
		}
		return AccountCallState::Unknown;
	};

	if (!sub_account) {
		const AccountCallState state = connected ? presence_state(obs.call.presence) : AccountCallState::NotConnected;
		view.main.row = account_row(account_name(obs, vesktop_id), state, obs.call.name);
	} else {
		// 参加者の一覧をまだ読めていない間 (roster_count が値を持たない間) は、メインアカウントがサブアカウントと同じ通話に
		// いるかを決められない。その間は、サブアカウントの行を「別の通話」(注意の色) にしない。
		const bool roster_read = obs.call.roster_count.has_value();
		const bool same_call = obs.call.presence == Presence::InCall && main_account_in_roster(obs);
		AccountCallState main_state = AccountCallState::NotConnected;
		if (connected && same_call) {
			main_state = AccountCallState::SameCall;
		} else if (connected) {
			// メインアカウントの通話は、見える通話に参加している (InCall) か、分からない (Unknown) かのどちらかである。
			// どの通話にもいないとは断定しない (Call::main_presence)。
			main_state = obs.call.main_presence == Presence::InCall ? AccountCallState::OtherCall
										  : AccountCallState::Unknown;
		}
		view.main.row = account_row(account_name(obs, obs.choices.broadcaster_user_id), main_state,
					    same_call ? obs.call.name : obs.call.main_name);

		AccountCallState sub_state = AccountCallState::NotConnected;
		if (connected) {
			sub_state = presence_state(obs.call.presence);
			if (sub_state == AccountCallState::SameCall && !same_call && roster_read)
				sub_state = AccountCallState::OtherCall;
		}
		dockview::SubAccountView sub;
		sub.row = account_row(account_name(obs, vesktop_id), sub_state, obs.call.name);
		sub.row.call_warn = sub_state == AccountCallState::OtherCall;
		sub.move = make_button(obs, ActionId::MoveToMainCall, ActionArgs{}, keys::kDockAccountMoveButton);
		sub.leave = make_button(obs, ActionId::LeaveCall, ActionArgs{}, keys::kDockAccountLeaveButton);
		view.sub = std::move(sub);
	}

	// [変更]。押せるかは apply() に問い合わせる。押すとメニューを開くだけなので、今と違う値を渡して問い合わせる
	// (rename_button() と同じ)。メニューの項目は、選んだ人を args.user_id に持つ。
	ActionArgs probe;
	probe.user_id = obs.choices.broadcaster_user_id + std::string(1, dockview::kTimeArgMark);
	view.main.change = make_button(obs, ActionId::SetMainAccount, probe, keys::kDockAccountChangeButton);
	view.main.change.args.user_id.clear();

	const auto choice_of = [&obs](const std::string &user_id, const Label &label, bool in_call) {
		dockview::MainAccountChoice choice;
		choice.label = label;
		choice.checked = user_id == obs.choices.broadcaster_user_id;
		choice.in_call = in_call;
		ActionArgs args;
		args.user_id = user_id;
		choice.action = make_button(obs, ActionId::SetMainAccount, args, keys::kDockAccountChangeButton);
		// 現在の値はチェックで示す (押せない理由にしない)。
		if (choice.action.disabled_reason.has_value() && choice.action.disabled_reason->id == reason::kSameMainAccount)
			choice.action.disabled_reason.reset();
		return choice;
	};
	std::set<std::string> listed;
	if (!vesktop_id.empty())
		listed.insert(vesktop_id);
	std::vector<dockview::MainAccountChoice> current;
	std::vector<dockview::MainAccountChoice> past;
	for (const PersonFact &person : obs.people) {
		if (!person.in_roster || person.user_id.empty() || !listed.insert(person.user_id).second)
			continue;
		current.push_back(choice_of(person.user_id, Label::from_literal(person_name(person)), true));
	}
	for (const SeenPerson &seen : obs.seen.people) {
		if (seen.user_id.empty() || !listed.insert(seen.user_id).second)
			continue;
		past.push_back(choice_of(seen.user_id, Label::from_literal(seen_name(seen)), false));
	}
	const auto by_name = [](const dockview::MainAccountChoice &a, const dockview::MainAccountChoice &b) {
		if (a.label.literal != b.label.literal)
			return natural_less(a.label.literal, b.label.literal);
		return a.action.args.user_id < b.action.args.user_id;
	};
	std::sort(current.begin(), current.end(), by_name);
	std::sort(past.begin(), past.end(), by_name);
	view.main.choices = std::move(current);
	view.main.choices.insert(view.main.choices.end(), past.begin(), past.end());
	view.main.no_main = choice_of(std::string(), Label::from_text(ref(keys::kDockAccountNoMainMenu)), false);
	view.main.no_main.checked = !sub_account;
	return view;
}

// ---- ダイアログの View ----

std::vector<dockview::ParticipantRow> participant_rows(const Observation &obs)
{
	std::vector<dockview::ParticipantRow> current;
	std::vector<dockview::ParticipantRow> past;
	std::set<std::string> listed;

	const auto fill = [&](dockview::ParticipantRow &row, const std::string &user_id) {
		row.always_on = always_on_for(obs, user_id);
		row.always_on_toggle = always_on_button(obs, user_id, keys::kParticipantsColAutoAddLabel, true);
		row.name_override = name_override_of(obs, user_id);
		row.rename = rename_button(obs, user_id, keys::kParticipantsColNameInCordmuxLabel);
		// 画面共有していない参加者の並びに出すか。行がない人 (まだ seen_people.json に書いていない人) は出す。
		const SeenPerson *seen = find_seen(obs, user_id);
		row.roster_shown = seen == nullptr || !seen->roster_hidden;
		ActionArgs args;
		args.user_id = user_id;
		args.on = !row.roster_shown;
		row.roster_toggle = make_button(obs, ActionId::SetRosterShown, args, keys::kParticipantsColRosterHideLabel);
	};

	// 現在の参加者 (通話の参加者の一覧にいる人)。Vesktop のアカウントの行は作らない (どの項目も動作しない)。
	for (const PersonFact &person : obs.people) {
		if (!person.in_roster || person.is_receiver || person.user_id.empty())
			continue;
		dockview::ParticipantRow row;
		row.user_id = person.user_id;
		row.in_call = true;
		if (const auto joined = obs.seen.call_joined_at_us.find(person.user_id); joined != obs.seen.call_joined_at_us.end())
			row.last_seen_at_us = joined->second;
		// 名前の列は Discord での名前にする (cordmux での名前は別の列)。読めなければ過去の参加者の名前、
		// それもなければ userId にする。
		std::string name;
		if (person.discord_name.has_value())
			name = *person.discord_name;
		else if (const SeenPerson *seen = find_seen(obs, person.user_id); seen != nullptr && !seen->display_name.empty())
			name = seen->display_name;
		else
			name = person.user_id;
		row.name = Label::from_literal(name);
		fill(row, person.user_id);
		row.voices = voice_choices(obs, person, true);
		listed.insert(person.user_id);
		current.push_back(std::move(row));
	}
	// 過去の参加者 (seen_people.json にいて、通話に参加していない人)。
	for (const SeenPerson &seen : obs.seen.people) {
		if (seen.user_id.empty() || listed.count(seen.user_id) != 0)
			continue;
		if (const PersonFact *person = find_person(obs, seen.user_id); person != nullptr && person->is_receiver)
			continue;
		dockview::ParticipantRow row;
		row.user_id = seen.user_id;
		row.in_call = false;
		row.name = Label::from_literal(seen.display_name.empty() ? seen.user_id : seen.display_name);
		if (seen.last_seen_at_us > 0)
			row.last_seen_at_us = seen.last_seen_at_us;
		fill(row, seen.user_id);
		PersonFact stand_in;
		stand_in.user_id = seen.user_id;
		row.voices = voice_choices(obs, stand_in, true);
		listed.insert(seen.user_id);
		past.push_back(std::move(row));
	}
	// 既定の並びは、最終参加日時の新しい順 (最近入った人について操作することが多いため)。同じ時刻 (cordmux が接続した時点で
	// 既に通話にいた人は同じ時刻になる) は名前の自然順。参加者の設定ウィンドウは、見出しで並べ替えても同じ比べ方を使う。
	const auto by_recent = [](const dockview::ParticipantRow &a, const dockview::ParticipantRow &b) {
		const std::int64_t at = a.last_seen_at_us.value_or(0);
		const std::int64_t bt = b.last_seen_at_us.value_or(0);
		if (at != bt)
			return at > bt;
		const std::string &an = a.name.literal;
		const std::string &bn = b.name.literal;
		if (an != bn)
			return natural_less(an, bn);
		return a.user_id < b.user_id;
	};
	std::sort(current.begin(), current.end(), by_recent);
	std::sort(past.begin(), past.end(), by_recent);
	for (dockview::ParticipantRow &row : past)
		current.push_back(std::move(row));
	return current;
}

dockview::AboutPageView about_page(const Observation &obs)
{
	dockview::AboutPageView about;
	// 読めない値は「-」にする (「-」は値を読めないことだけを表す)。グリッドのセルの「-」のツールチップと
	// アクセシブル名は、ダイアログの側で付ける。
	const auto version = [&about](const char *label, const std::string &value) {
		about.versions.push_back({ref(label), Label::from_literal(value.empty() ? std::string("-") : value)});
	};
	version(keys::kSettingsVersionObsPluginLabel, obs.versions.plugin);
	version(keys::kSettingsVersionVesktopPluginLabel, obs.versions.discord_side);
	version(keys::kSettingsVersionVesktopLabel, obs.versions.vesktop);
	// 「同梱のVencord」は本家のバージョンの文字列 (印の vencord_tag)。build_id は cordmux のバージョンで、上の「cordmux (OBS)」と同じ値に
	// なるため Vencord のバージョンとして出さない。印が読めないときは「-」。
	// tag がないときに build_id を出すと、Vencord のバージョンでない値を同じ札で出すことになる。
	version(keys::kSettingsVersionBundledVencordLabel, obs.versions.bundled_vencord_tag);
	version(keys::kSettingsVersionObsLabel, obs.versions.obs);
	version(keys::kSettingsVersionWindowsLabel, obs.versions.windows);
	version(keys::kSettingsVersionGpuLabel, obs.versions.gpu);

	about.vesktop_rewrote_at_startup = ref(obs.install.rewrote_at_startup ? keys::kSettingsStartupRewrittenStatus
									     : keys::kSettingsStartupNotRewrittenStatus);
	if (obs.ops.export_result.has_value()) {
		dockview::ExportResultView result;
		result.text = ref(keys::kSettingsExportDoneStatus);
		// 場所は作った zip のファイルなので、ファイルの行にする (読む側が [ファイルの場所を開く] で、
		// zip を選んだ状態でフォルダーを開く)。open_folder は、File の行では偽にする (dock_view.h の約束)。
		// フォルダーの行にしないのは、開いたフォルダーに前の zip が並び、どれが現在のファイルかを利用者が
		// 探すことになるためである。
		LocationLine file{obs.ops.export_result->path, false};
		file.kind = LocationKind::File;
		result.location = std::move(file);
		about.export_result = std::move(result);
	}
	about.export_button = make_button(obs, ActionId::ExportBundle, ActionArgs{}, keys::kSettingsExportDiagnosticsButton);
	// アンインストールの最中は、押したボタンのテキストを処理中のテキスト (「アンインストール中...」) にする。OBS の
	// 録画のボタンの「停止処理中...」と同じく、押した所の近くで進み具合を伝える。押せない理由 (uninstall_started())
	// は残す。設定ウィンドウは、理由の有無で押せるかを決めるためである (dock/settings_window.cpp)。
	about.uninstall_button =
		make_button(obs, ActionId::Uninstall, ActionArgs{},
			    obs.uninstall.phase == UninstallPhase::Running ? keys::kSettingsUninstallBusyButton
									   : keys::kSettingsUninstallButton);
	about.settings_locked = uninstall_started(obs);
	return about;
}

// 状態タブの [映像の受信] の表のうち、直近 1 分の列を持つ行。行は、直近 1 分にフレームが届いた人だけ。名前は、cordmux での名前、
// Discord での名前、userId の順で、最初にあるものを使う。合成のソースがないシーンコレクションでは行を作らない (バッファが動作
// しない)。累計と録画の節と履歴は、前の周期の状態が要るので settle_status() (ledger/status_tab.h) が埋める。
dockview::StatusTabView status_tab_of(const Observation &obs)
{
	dockview::StatusTabView view;
	view.composite_present = obs.choices.composite_present;
	view.reset = make_button(obs, ActionId::ResetStallTally, ActionArgs{}, keys::kStatusResetButton);
	// 「Vesktop」の組。Vesktop の設定が同梱の Vencord を指していて、Vesktop が起動していないと分かっている間だけ作る
	// (dockview::StatusVesktopView のコメント)。指していない間は、[Vesktopに追加] のカードが状態を伝え、外すものもないので、
	// 組ごと作らない ([Vesktopに追加] が追加した後はボタンごと表示しないのと同じ)。Vesktop の起動中と、起動しているかを
	// 読めていない間も作らない。配信中に状態タブを見る配信者に、押せないボタンと「先にVesktopを終了してください」を出し
	// 続けることになるためである (ledger/actions.cpp の RemoveFromVesktop の行)。アンインストールを始めた後も作らない。
	// アンインストールが Vesktop の設定を戻すので、ここから戻す操作を残さない。
	const bool vesktop_added =
		obs.install.dir == VencordDirState::Matches || obs.install.dir == VencordDirState::HashMismatch;
	if (vesktop_added && obs.install.process == VesktopProcess::NotRunning && !uninstall_started(obs)) {
		dockview::StatusVesktopView vesktop;
		vesktop.text = ref(keys::kStatusVesktopAddedText);
		vesktop.remove =
			make_button(obs, ActionId::RemoveFromVesktop, ActionArgs{}, keys::kStatusRemoveFromVesktopButton);
		view.vesktop = std::move(vesktop);
	}
	if (!view.composite_present)
		return view;
	for (const ArrivalFact &fact : obs.receive.arrival) {
		if (fact.user_id.empty() || pipeline::frame_count(fact.streams) == 0)
			continue;
		dockview::StatusReceiveRow row;
		row.user_id = fact.user_id;
		std::string name;
		const PersonFact *person = find_person(obs, fact.user_id);
		if (person != nullptr && person->display_name.has_value() && !person->display_name->empty())
			name = *person->display_name;
		else if (const SeenPerson *seen = find_seen(obs, fact.user_id); seen != nullptr)
			name = seen_name(*seen);
		else
			name = fact.user_id;
		row.name = Label::from_literal(name);
		row.streams = fact.streams;
		view.rows.push_back(std::move(row));
	}
	std::sort(view.rows.begin(), view.rows.end(),
		  [](const dockview::StatusReceiveRow &a, const dockview::StatusReceiveRow &b) {
			  if (a.name.literal != b.name.literal)
				  return natural_less(a.name.literal, b.name.literal);
			  return a.user_id < b.user_id;
		  });
	return view;
}

// アンインストールの結果のダイアログの View。ここで決めるのは、主文・cordmux がしたこと・削除するフォルダー・
// 録画の場所・残ったもののまとまりで、前置きのテキストと後ろの文は、ダイアログの側 (dock/install_dialog_plan.cpp) が
// 追加する。アンインストールが完了も停止もしていないとき (NotStarted と Running) は nullopt を返す。
std::optional<UninstallResultView> uninstall_result(const Observation &obs)
{
	const UninstallFacts &u = obs.uninstall;
	if (u.phase != UninstallPhase::Done && u.phase != UninstallPhase::Stopped)
		return std::nullopt;
	UninstallResultView v;
	v.loose = u.folder_to_delete.empty();
	if (!u.record_dir.empty())
		v.record_dir = LocationLine{u.record_dir, true};

	if (u.phase == UninstallPhase::Done) {
		v.kind = u.leftovers.empty() ? UninstallResultKind::Done : UninstallResultKind::DoneUnconfirmed;
		v.main = ref(v.loose ? keys::kDialogUninstallDoneLooseHeadline : keys::kDialogUninstallDoneHeadline);
		// cordmux がしたこと。手順書と違う置き方でも同じにする (設定とシーンを消し、Vesktop の設定を元に戻すのは、
		// インストールの仕方によらない)。ダイアログの並び (主文、cordmux がしたこと、削除するもの) は
		// dock/install_dialog_plan.cpp が決める。
		v.notes.push_back(ref(keys::kDialogUninstallDoneText));
		if (!v.loose)
			v.folder_to_delete = LocationLine{u.folder_to_delete, true};
		if (!u.leftovers.empty()) {
			// 項目は名前だけなので、「開けなかった」と「同じ名前が 2 つ以上ある」の両方に出た
			// 名前は 1 つにする (同じ名前の項目を 2 つ並べない)。見出しは名前の数で選ぶ (1 つなら _one。
			// 英語では "This scene collection couldn't be checked.")。次の操作の文は、
			// ダイアログの側が同じ数で選ぶ。
			// 名前の分かるシーンコレクションがなく、フォルダーを読めなかっただけのときは、見出しを
			// 「確認できなかったもの:」(途中で止まったときと同じ) にする (項目はシーンコレクションの名前ではない)。
			LeftoverGroup group;
			const std::vector<std::string> names = unchecked_collection_names(u);
			group.heading = names.empty() ? ref(keys::kDialogNotCheckedText)
						      : ref(text::pick(keys::kDialogUninstallUncheckedText,
								       static_cast<long long>(names.size())));
			for (const std::string &collection : names)
				group.items.push_back(Label::from_literal(collection));
			if (collection_folder_unreadable(u))
				group.items.push_back(Label::from_text(ref(keys::kDialogLeftCollectionFolderItemText)));
			v.groups.push_back(std::move(group));
		}
		return v;
	}

	v.kind = UninstallResultKind::Stopped;
	v.main = ref(keys::kDialogUninstallStoppedHeadline);
	// 元に戻せなかった設定の注意「この設定を元に戻すまで、次のフォルダーを削除しないでください。...」が指すフォルダー
	// (終わったときのダイアログの「削除するフォルダー」と同じプラグインのフォルダー)。ダイアログ
	// (dock/install_dialog_plan.cpp) とドックのカード (stopped_card_body()) が同じ値を読む。Observation に場所が
	// ないときは持たない。注意を表示するときだけでなく、いつも入れる。どのまとまりで使うかは読む側の並べ方で、値は
	// 事実として 1 つ置けば足りるためである。
	if (!u.folder_to_delete.empty())
		v.folder_to_delete = LocationLine{u.folder_to_delete, true};
	// 3 つのまとまり (削除できなかったもの、元に戻せなかった設定、確認できなかったもの)。空のまとまりは入れない。
	// この順で入れ、画面の並び (確認、設定、削除) は読む側が見出しで決める (dock_view.h の LeftoverGroup)。
	LeftoverGroup not_deleted;
	not_deleted.heading = ref(keys::kDialogNotDeletedText);
	LeftoverGroup not_restored;
	not_restored.heading = ref(keys::kDialogNotRestoredText);
	LeftoverGroup not_checked;
	not_checked.heading = ref(keys::kDialogNotCheckedText);

	// 元に戻すためのバックアップが残る cordmux の設定のフォルダーを、手で削除するものとして表示しない。
	// バックアップの vencord_dir_backup.json は OBS の plugin_config の cordmux のフォルダー (Observation の
	// SettingsFiles) にあり、設定を消す段階はバックアップを残して消し (install/uninstall.cpp の clear_folder の
	// keep_path)、Vesktop の設定を戻す段階は、戻せなかったときにバックアップを残す (finish_backup を呼ばない)。
	// 手で消すと、次のアンインストールの Vesktop の設定を戻す段階にバックアップがなく、Vesktop が同梱の場所を
	// 指したまま戻せなくなる。そこで、ダイアログとカードのどこでも、plugin_config の cordmux のフォルダーを
	// 削除するよう勧めない。
	//   (1) 「元に戻せなかった設定」のまとまりがあるとき: バックアップを残した設定のフォルダー (u.settings_dir。
	//       Vesktop の設定を戻す段階がバックアップを見たときだけ値を持つ) と、残りとして挙げた設定のフォルダーの
	//       場所を、そのまとまりの注意 (「この設定を元に戻すまで、次のフォルダーを削除しないでください。」) の
	//       場所の行に、プラグインのフォルダーの後に並べ、「削除できなかったもの」からは外す
	//       (同じ場所を 2 つのまとまりに表示しない)。接続のための情報の保存先 (LocalDataFolder) はバックアップを
	//       持たず、手で消してよいので、「削除できなかったもの」に残す。製品で起きるのは Vesktop の設定を戻す段階で
	//       止まったときで、場所の行はプラグインのフォルダーと u.settings_dir の 2 つになる。
	//       「削除しないでください」の並びに 1 つだけ挙げると、挙げなかったほうは消してよいと読めるので、
	//       両方を挙げる。消すと、アンインストールで元に戻す方法が二度となくなる。
	//   (2) それ以外で、設定のフォルダーが「削除できなかったもの」に残るとき: そのまとまりの次の操作を
	//       「もう一度アンインストールしてください。」にする (下の next)。製品で起きるのは、設定を消す段階で
	//       止まったときである (止まった段階の残りだけを持つので、設定のフォルダーの残りと
	//       「元に戻せなかった設定」が同時にあることはない。install/uninstall.cpp の run_uninstall)。
	// u.settings_dir の場所を、その下にある残りの場所 (例 ...\cordmux\settings.json) と比べて 1 つにまとめることは
	// しない。ledger は場所の文字列の比べ方 (区切り、大文字と小文字) を持たず (platform を読まない)、製品では
	// u.settings_dir と設定のフォルダーの残りが同時にあることはない (前者は Vesktop の設定を戻す段階、後者は設定を
	// 消す段階で止まったときに残る)。同じ文字列の場所だけを 1 つにする。
	// 「削除できなかったもの」に残したまま次の操作の文だけを変えないのは、同じ場所が注意の
	// 「削除しないでください」と「削除できなかったもの」の両方に表示されるためである。設定のフォルダー用の注意を
	// 別の段落で追加しないのは、プラグインのフォルダーの注意と同じこと (戻すまで消さない) を 2 回言うためである。
	// (2) のときに注意を追加しないのは、設定を消す段階で止まったときは、バックアップがあるか (cordmux を
	// Vesktop に追加したことがあるか) がアンインストールの結果 (UninstallFacts) になく、ないときに
	// 「戻すのに必要です」は事実と違うためである。次の操作を「もう一度アンインストール」だけにすれば、手で消す
	// 方法は伝えない。
	bool restore_pending = false;
	for (const UninstallLeftoverFact &left : u.leftovers)
		restore_pending = restore_pending || left.kind == UninstallLeftoverKind::VencordDir;
	std::vector<LocationLine> settings_dir_places;
	const auto add_settings_place = [&settings_dir_places](const LocationLine &line) {
		for (const LocationLine &have : settings_dir_places) {
			if (have.path == line.path)
				return;
		}
		settings_dir_places.push_back(line);
	};
	if (!u.settings_dir.empty())
		add_settings_place(LocationLine{u.settings_dir, true});
	for (const UninstallLeftoverFact &left : u.leftovers) {
		if (left.kind == UninstallLeftoverKind::SettingsFiles && !left.where.empty())
			add_settings_place(leftover_location(left));
	}
	const bool settings_in_caution = restore_pending && !settings_dir_places.empty();

	// cordmux の設定のファイルと、接続のための情報の保存先は 1 つの項目にまとめ、場所の行を並べる (ファイルの名前は
	// 表示しない)。(1) のときは設定のフォルダーを注意に移すので、保存先だけが残る。
	std::vector<LocationLine> settings_places;
	bool settings_item = false;
	bool settings_files_left = false; // 「削除できなかったもの」に設定のフォルダーの残りがある
	for (const UninstallLeftoverFact &left : u.leftovers) {
		const bool settings = left.kind == UninstallLeftoverKind::SettingsFiles;
		if (!settings && left.kind != UninstallLeftoverKind::LocalDataFolder)
			continue;
		if (settings && settings_in_caution)
			continue;
		settings_item = true;
		settings_files_left = settings_files_left || settings;
		if (!left.where.empty())
			settings_places.push_back(leftover_location(left));
	}
	if (settings_item) {
		not_deleted.items.push_back(Label::from_text(ref(keys::kDialogLeftSettingsFilesItemText)));
		not_deleted.item_locations.push_back(settings_places);
	}
	for (const UninstallLeftoverFact &left : u.leftovers) {
		const char *item = nullptr;
		LeftoverGroup *group = nullptr;
		std::vector<LocationLine> places;
		switch (left.kind) {
		case UninstallLeftoverKind::SettingsFiles:
		case UninstallLeftoverKind::LocalDataFolder:
			continue;
		case UninstallLeftoverKind::Sources:
			item = keys::kDialogLeftSourcesItemText;
			group = &not_deleted;
			break;
		case UninstallLeftoverKind::VencordDir:
			// Vesktop の設定のファイルの場所。ファイルの行 (LocationKind::File) なので
			// [フォルダーを開く] は付けず (open_folder は false)、読む側が kind を見て
			// [ファイルの場所を開く] を置く。
			not_restored.items.push_back(Label::from_text(ref(keys::kDialogLeftVesktopSettingsItemText)));
			if (!left.where.empty()) {
				LocationLine file{left.where, false};
				file.kind = LocationKind::File;
				places.push_back(std::move(file));
			}
			not_restored.item_locations.push_back(places);
			continue;
		case UninstallLeftoverKind::CollectionNotOpened:
			item = keys::kDialogLeftCollectionNotOpenedItemText;
			group = &not_checked;
			break;
		case UninstallLeftoverKind::CollectionNameDuplicate:
			item = keys::kDialogLeftCollectionDuplicateItemText;
			group = &not_checked;
			break;
		case UninstallLeftoverKind::CollectionFolderUnreadable:
			item = keys::kDialogLeftCollectionFolderItemText;
			group = &not_checked;
			break;
		}
		group->items.push_back(Label::from_text(left.where.empty() ? ref(item) : ref(item, {left.where})));
		group->item_locations.push_back({});
	}
	// 「削除できなかったもの」の次の操作 (dock_view.h の LeftoverGroup::next。上の (2))。設定のフォルダーの残りが
	// このまとまりにあるときだけ、手で削除する方法を伝えない。シーン・ソース・名前ラベル・接続のための情報の
	// 保存先の残りは手で消してよいので、それだけのときは「もう一度アンインストールするか、手で削除してください。」
	// のままにする。条件は項目のテキストではなく、残ったものの種類で見る (項目「cordmuxの設定のファイル」は、
	// 保存先だけが残ったときにも表示される)。いつも「もう一度アンインストール」だけにしないのは、手で消してよい
	// 残りを消す方法まで伝えなくなるためである。
	if (!not_deleted.items.empty())
		not_deleted.next = ref(settings_files_left ? keys::kDialogNotDeletedUninstallAgainNextText
							   : keys::kDialogNotDeletedNextText);
	// 「元に戻せなかった設定」の注意と場所の行 (dock_view.h の LeftoverGroup::caution。上の (1))。テキストは場所の
	// 行の中身で選ぶ。
	//   プラグインのフォルダーだけ: dont_delete_folder (2 文目の「Vesktopがその場所に公式のVencordを入れます」は、
	//     このフォルダーに当てはまる)。
	//   プラグインのフォルダーと設定のフォルダー (u.settings_dir と、残りとして挙げた設定のフォルダー。製品では、
	//     Vesktop の設定を戻す段階で止まったときの u.settings_dir): dont_delete_folders (2 文目は、両方に当てはまる
	//     「もう一度アンインストールするときに必要です」。設定のフォルダーに公式の Vencord は入らない)。
	//     プラグインのフォルダーを先に並べる (終わったときのダイアログの「削除するフォルダー」と同じ所を先に
	//     見せる)。
	//   プラグインのフォルダーが分からない (手順書と違う置き方など): 設定のフォルダーがなければ dont_delete_files
	//     (場所の行なし)、あれば dont_delete_files_and_folders (数は続く場所の行の数。英語の folder と folders)。
	// ダイアログとカードが、それぞれ「場所の行を置けるか」でテキストを選ばないのは、同じ条件を 2 か所に持つことに
	// なるためである。2 文目を落とさないのは、なぜ消してはいけないかがないと、残ったものを片付けたい人に伝わらない
	// ためである。
	if (!not_restored.items.empty()) {
		const bool has_folder = v.folder_to_delete.has_value() && !v.folder_to_delete->path.empty();
		if (has_folder)
			not_restored.caution_locations.push_back(*v.folder_to_delete);
		if (settings_in_caution) {
			not_restored.caution_locations.insert(not_restored.caution_locations.end(),
							     settings_dir_places.begin(), settings_dir_places.end());
		}
		if (has_folder) {
			not_restored.caution = ref(settings_in_caution ? keys::kDialogDontDeleteFoldersText
								       : keys::kDialogDontDeleteFolderText);
		} else if (settings_in_caution) {
			not_restored.caution = ref(text::pick(keys::kDialogDontDeleteFilesAndFoldersText,
							      static_cast<long long>(settings_dir_places.size())));
		} else {
			not_restored.caution = ref(keys::kDialogDontDeleteFilesText);
		}
	}
	// どの項目も場所の行を持たないまとまりは、item_locations を空にする (空は場所の行がないことを表す)。
	for (LeftoverGroup *group : {&not_deleted, &not_restored, &not_checked}) {
		bool any = false;
		for (const std::vector<LocationLine> &places : group->item_locations)
			any = any || !places.empty();
		if (!any)
			group->item_locations.clear();
		if (!group->items.empty())
			v.groups.push_back(std::move(*group));
	}
	return v;
}

// ---- 録画のボタンとメニュー ----

// 録画を終えるボタンのテキスト。録画を始めた時刻が分かれば、ボタンの中に経過時間を出す (配信を見ながら、録画が進んでいることと
// 長さが分かる)。
TextRef record_stop_label(const Observation &obs)
{
	if (!obs.recording.enabled_since_us.has_value())
		return ref(keys::kDockRecordStopButton);
	return ref(keys::kDockRecordStopElapsedButton,
		   {record_elapsed_text(obs.now_us - *obs.recording.enabled_since_us)});
}

RecordButtonView record_button(const Observation &obs)
{
	RecordButtonView rb;
	const RecordingFacts &rec = obs.recording;
	switch (rec.phase) {
	case RecordPhase::Unknown:
		// 録画の状態をコピーしない古い録画の側。ON か OFF かから決める。
		rb.state = obs.choices.recording == FeatureSwitch::On ? RecordButtonState::Recording
								      : RecordButtonState::Idle;
		if (rb.state == RecordButtonState::Idle && rec.stopped_by_space_exhausted)
			rb.state = RecordButtonState::Failed;
		break;
	case RecordPhase::Idle:
		rb.state = rec.stopped_by_space_exhausted ? RecordButtonState::Failed : RecordButtonState::Idle;
		break;
	case RecordPhase::Starting:
		rb.state = RecordButtonState::Starting;
		break;
	case RecordPhase::Recording:
		rb.state = RecordButtonState::Recording;
		break;
	case RecordPhase::Stopping:
		rb.state = RecordButtonState::Stopping;
		break;
	case RecordPhase::StartFailed:
		rb.state = RecordButtonState::Failed;
		break;
	}
	// 処理に入ってから 1 秒までは前のテキストにする (kRecordLabelHoldUs)。入った時刻が分からなければ、処理中の
	// テキストにする (見た目と実際が食い違わない側にする)。
	const bool within_hold = rec.phase_since_us.has_value() && obs.now_us - *rec.phase_since_us < kRecordLabelHoldUs;
	switch (rb.state) {
	case RecordButtonState::Idle:
	case RecordButtonState::Failed:
		rb.label = ref(keys::kDockRecordStartButton);
		break;
	case RecordButtonState::Starting:
		rb.label = ref(within_hold ? keys::kDockRecordStartButton : keys::kDockRecordStartingButton);
		break;
	case RecordButtonState::Recording:
		rb.label = record_stop_label(obs);
		break;
	case RecordButtonState::Stopping:
		rb.label = within_hold ? record_stop_label(obs) : ref(keys::kDockRecordStoppingButton);
		break;
	}
	// 名前は経過時間を除いたテキストにする (RecordButtonView::name)。
	rb.name = rb.label.id == keys::kDockRecordStopElapsedButton ? ref(keys::kDockRecordStopButton) : rb.label;
	rb.pressed_look = rb.state == RecordButtonState::Recording || rb.state == RecordButtonState::Stopping;
	ActionArgs args;
	args.on = rb.state == RecordButtonState::Idle || rb.state == RecordButtonState::Failed;
	rb.action = make_button(obs, ActionId::ToggleRecording, args, rb.label.id.c_str());
	rb.action.label = rb.label;
	rb.width_labels = {ref(keys::kDockRecordStartButton), ref(keys::kDockRecordStopButton),
			   ref(keys::kDockRecordStartingButton), ref(keys::kDockRecordStoppingButton),
			   ref(keys::kDockRecordStopElapsedButton, {kRecordElapsedWidest})};
	return rb;
}

dockview::DockMenuView dock_menu(const Observation &obs, const RecordButtonView &record)
{
	dockview::DockMenuView m;
	ActionArgs settings;
	settings.page = SettingsPage::Recording;
	m.open_settings = make_button(obs, ActionId::OpenSettings, settings, keys::kDockMenuSettingsMenu);
	m.open_participants = make_button(obs, ActionId::OpenParticipants, ActionArgs{}, keys::kDockMenuParticipantsMenu);
	m.export_bundle = make_button(obs, ActionId::ExportBundle, ActionArgs{}, keys::kDockMenuExportDiagnosticsMenu);
	ActionArgs feature;
	feature.on = obs.choices.feature == FeatureSwitch::Off;
	const char *feature_label = keys::kDockMenuDisableMenu;
	if (feature.on)
		feature_label = keys::kDockMenuEnableMenu;
	else if (record.state == RecordButtonState::Recording || record.state == RecordButtonState::Starting ||
		 record.state == RecordButtonState::Stopping)
		feature_label = keys::kDockMenuDisableRecordingMenu; // 録画も止まることをテキストで伝える
	m.feature_toggle = make_button(obs, ActionId::ToggleFeature, feature, feature_label);
	return m;
}

// 表示タブ。ダミー映像の切り替えは、押した後の値を args.on に持つ。
dockview::LookTabView look_tab(const Observation &obs)
{
	dockview::LookTabView v;
	v.dummy_on = obs.choices.dummy == FeatureSwitch::On;
	ActionArgs args;
	args.on = !v.dummy_on;
	v.dummy_toggle = make_button(obs, ActionId::ToggleDummy, args, keys::kLookShowPlaceholdersCheck);
	v.has_main_account = !obs.choices.broadcaster_user_id.empty();
	return v;
}

} // namespace

const char *rule_log_name(PrimaryRuleId id)
{
	switch (id) {
	case PrimaryRuleId::Uninstalled:
		return "Uninstalled";
	case PrimaryRuleId::FeatureOff:
		return "FeatureOff";
	case PrimaryRuleId::OtherObsHolding:
		return "OtherObsHolding";
	case PrimaryRuleId::ListenPortsExhausted:
		return "ListenPortsExhausted";
	case PrimaryRuleId::Connected:
		return "Connected";
	case PrimaryRuleId::SessionFileNotSaved:
		return "SessionFileNotSaved";
	case PrimaryRuleId::VencordOverwritten:
		return "VencordOverwritten";
	case PrimaryRuleId::VencordDirElsewhere:
		return "VencordDirElsewhere";
	case PrimaryRuleId::VencordDirNotConfigured:
		return "VencordDirNotConfigured";
	case PrimaryRuleId::RestartVesktop:
		return "RestartVesktop";
	case PrimaryRuleId::ConnectedThenDropped:
		return "ConnectedThenDropped";
	case PrimaryRuleId::VesktopChecking:
		return "VesktopChecking";
	case PrimaryRuleId::VesktopNotRunning:
		return "VesktopNotRunning";
	case PrimaryRuleId::VesktopRunningNotLinked:
		return "VesktopRunningNotLinked";
	}
	return "UnknownPrimaryRule";
}

const char *rule_log_name(NoticeRuleId id)
{
	switch (id) {
	case NoticeRuleId::ContactUnavailable:
		return "ContactUnavailable";
	case NoticeRuleId::CompositeAbsent:
		return "CompositeAbsent";
	case NoticeRuleId::RecordingOnly:
		return "RecordingOnly";
	case NoticeRuleId::InstallMismatch:
		return "InstallMismatch";
	case NoticeRuleId::VersionRestartVesktop:
		return "VersionRestartVesktop";
	case NoticeRuleId::VersionMixedBundle:
		return "VersionMixedBundle";
	case NoticeRuleId::DoubleAudio:
		return "DoubleAudio";
	case NoticeRuleId::BroadcasterNotInCall:
		return "BroadcasterNotInCall";
	case NoticeRuleId::AllVideoStoppedHint:
		return "AllVideoStoppedHint";
	case NoticeRuleId::RecordSpaceLow:
		return "RecordSpaceLow";
	case NoticeRuleId::RecordFinalizeShort:
		return "RecordFinalizeShort";
	case NoticeRuleId::RecordFinalizeRunning:
		return "RecordFinalizeRunning";
	case NoticeRuleId::RecordFinalizePending:
		return "RecordFinalizePending";
	case NoticeRuleId::RecordNotReachedSender:
		return "RecordNotReachedSender";
	case NoticeRuleId::RecordDirUnwritable:
		return "RecordDirUnwritable";
	case NoticeRuleId::RecordStoppedSpaceExhausted:
		return "RecordStoppedSpaceExhausted";
	case NoticeRuleId::ReinstallFailed:
		return "ReinstallFailed";
	case NoticeRuleId::RecordFinalizeFailed:
		return "RecordFinalizeFailed";
	}
	return "UnknownNoticeRule";
}

const char *rule_log_name(AddedRuleId id)
{
	switch (id) {
	case AddedRuleId::SettingsUnreadAtStartup:
		return "SettingsUnreadAtStartup";
	case AddedRuleId::SettingsUnsaved:
		return "SettingsUnsaved";
	case AddedRuleId::SeenUnreadAtStartup:
		return "SeenUnreadAtStartup";
	case AddedRuleId::SeenUnsaved:
		return "SeenUnsaved";
	case AddedRuleId::VesktopAddFailed:
		return "VesktopAddFailed";
	case AddedRuleId::RecordGap:
		return "RecordGap";
	case AddedRuleId::SeenDiscarded:
		return "SeenDiscarded";
	case AddedRuleId::CallActionFailed:
		return "CallActionFailed";
	case AddedRuleId::VesktopRemoveFailed:
		return "VesktopRemoveFailed";
	}
	return "UnknownAddedRule";
}

dockview::LogLevel rule_log_level(dockview::Tier tier)
{
	switch (tier) {
	case Tier::L1:
		return LogLevel::Error;
	case Tier::L2:
		return LogLevel::Warning;
	case Tier::L3:
		break;
	}
	return LogLevel::Info;
}

namespace {

constexpr AddedRuleId kAddedRules[] = {
	AddedRuleId::SettingsUnreadAtStartup, AddedRuleId::SettingsUnsaved, AddedRuleId::SeenUnreadAtStartup,
	AddedRuleId::SeenUnsaved,             AddedRuleId::VesktopAddFailed, AddedRuleId::RecordGap,
	AddedRuleId::SeenDiscarded,           AddedRuleId::CallActionFailed, AddedRuleId::VesktopRemoveFailed,
};

} // namespace

std::size_t added_rule_count()
{
	return sizeof(kAddedRules) / sizeof(kAddedRules[0]);
}

AddedRuleId added_rule_id_at(std::size_t index)
{
	const std::size_t n = added_rule_count();
	return kAddedRules[index < n ? index : n - 1];
}

std::vector<dockview::Notice> setup_notices_of(const dockview::DockViewV2 &view)
{
	std::vector<dockview::Notice> out;
	if (!view.people_list.sharing.empty() || !view.people_list.in_call.empty())
		return out;
	// 利用者が最初の設定で順に進める物だけを選ぶ。録画や音声のメッセージは、一覧が空でも状態タブだけに置く。
	const char *const names[] = {
		rule_log_name(PrimaryRuleId::VencordDirNotConfigured), rule_log_name(PrimaryRuleId::VencordDirElsewhere),
		rule_log_name(PrimaryRuleId::VencordOverwritten),      rule_log_name(PrimaryRuleId::RestartVesktop),
		rule_log_name(PrimaryRuleId::VesktopRunningNotLinked), rule_log_name(NoticeRuleId::ReinstallFailed),
		rule_log_name(AddedRuleId::VesktopAddFailed),
	};
	for (const dockview::Notice &notice : view.notices) {
		for (const char *name : names) {
			if (notice.rule_name == name) {
				out.push_back(notice);
				break;
			}
		}
	}
	return out;
}

dockview::DockViewV2 decide_dock(const Observation &obs)
{
	DockViewV2 view;

	// メッセージがないときの 1 行目と、primary の表の行のメッセージ。
	FirstLine fallback;
	const PrimaryRuleId primary = primary_rule_of(obs);
	if (primary == PrimaryRuleId::Connected)
		summary_line(obs, &fallback);
	else
		primary_to_view(obs, primary, &fallback, &view.notices);
	// レベル 2 をすべて既読にした後の 1 行目。fallback と同じだが、primary のメッセージを既読にしたときは「確認中...」に
	// せず、接続していないことを伝える。
	FirstLine read_line = fallback;
	if (fallback.forms.empty()) {
		// primary がメッセージになる行 (メッセージのレベル 1 か 2)。メッセージを表示するまでの待ち (Debounced) の間は、
		// 1 行目を「確認中...」にする (今分かっている範囲で、事実と違うことを表示しない)。
		fallback.kind = FirstLineKind::Working;
		fallback.forms = {ref(keys::kDockSummaryCheckingStatus)};
		read_line.kind = FirstLineKind::Summary;
		read_line.forms = {ref(obs.link.state == LinkState::Connected ? keys::kDockStageVesktopConnectedStatus
									      : keys::kDockStageVesktopDisconnectedStatus)};
	}
	// 1 人を全画面で表示している間と、ダミーを表示している間は、要約の代わりに、その状態を平常のテキストで伝える。
	// 全画面はダミーより優先し、無効と処理中のテキストは、どちらよりも優先する。全画面の行には、元の配置に戻すボタン
	// ([元に戻す]) を右端に置く (無効の行の [有効にする] と同じ作り)。全画面の行は未読のレベル 2 より優先しないので、
	// disabled は立てない。
	const auto show_user_choice = [&obs, primary](FirstLine *line) {
		if (line->kind != FirstLineKind::Summary && primary != PrimaryRuleId::VesktopChecking)
			return;
		if (!obs.choices.focus_user_id.empty()) {
			line->kind = FirstLineKind::UserChoice;
			line->forms = {ref(keys::kDockSummaryFullScreenStatus, {focus_display_name(obs)})};
			line->button = make_button(obs, ActionId::SetFocus, ActionArgs{}, keys::kDockFullScreenRestoreButton);
			return;
		}
		if (obs.choices.dummy != FeatureSwitch::On)
			return;
		line->kind = FirstLineKind::UserChoice;
		line->forms = {ref(keys::kDockSummaryPlaceholdersStatus), ref(keys::kDockSummaryPlaceholdersShortStatus)};
		line->button.reset();
	};
	show_user_choice(&fallback);
	show_user_choice(&read_line);
	view.fallback_line = fallback;
	view.read_line = read_line;

	// notices の表のメッセージ (表の順。並べ直しは stabilize() が行う)。
	for (std::size_t i = 0; i < notice_rule_count(); ++i)
		notice_rule_to_view(obs, notice_rule_id_at(i), &view.notices);
	for (const AddedRuleId id : kAddedRules)
		added_rule_to_view(obs, id, &view.notices);
	// 同じボタンのカードを 1 枚にしてから、準備の段階の行を持つカードを決め、その後に、見出しが既に伝えている
	// 段階の行を外す (1 枚にすると表示される見出しが変わるので、外すかの判断はその後にする)。
	merge_failure_cards(&view.notices);
	keep_stages_on_one_card(&view.notices);
	drop_restated_stages(&view.notices);
	mark_primary_button(&view.notices);

	// 人の行。表示する名前の自然順に並べる。同じ名前は userId の順にして、同じ Observation なら同じ並びにする。
	const std::map<std::string, std::string> voice_links = usable_voice_sources(obs);
	std::map<std::string, int> discord_name_counts;
	std::map<std::string, int> visible_name_counts;
	for (const PersonFact &person : obs.people) {
		if (person.discord_name.has_value())
			++discord_name_counts[*person.discord_name];
		++visible_name_counts[person_name(person)];
	}
	// アンインストールを始めた後は、人の行を作らない。アンインストールの最初の段階で待ち受けを閉じるので、その後に
	// 残る行は前の接続のときのコピーで、どの操作も押せない (なくなった事実を表示し続けない)。空の一覧の 1 文
	// (EmptyKind::Uninstalled) がその理由を伝えるので、その文が事実になるように、ここで行を作らない (待ち受けを
	// 閉じるほかのコンポーネントの動作に頼らない)。行を作り、行が 0 のときだけ Uninstalled の文を表示する形に
	// しないのは、行が残る間は文と食い違い、文が事実かどうかが、待ち受けを閉じる段階と pipeline/ の
	// facts の捨て方によって変わるためである。
	std::vector<const PersonFact *> people;
	if (!uninstall_started(obs)) {
		for (const PersonFact &person : obs.people)
			people.push_back(&person);
	}
	std::sort(people.begin(), people.end(), [](const PersonFact *a, const PersonFact *b) {
		const std::string an = person_name(*a);
		const std::string bn = person_name(*b);
		if (an != bn)
			return natural_less(an, bn);
		return a->user_id < b->user_id;
	});
	bool any_unchecked = false;
	for (const PersonFact *person : people) {
		view.people.push_back(make_person_row(obs, *person, voice_links, discord_name_counts, visible_name_counts));
		any_unchecked = any_unchecked || (!person->slot.has_value() && !person->is_receiver) ||
				(person->is_self_frame && obs.choices.self_frame_selected && !obs.choices.self_frame_on);
	}

	// 一覧の上の 1 行。枠に載せていない人がいて、その人たちを載せられないときだけ作る。
	view.list_full = free_screen_slot(obs) == 0;
	if (any_unchecked && !uninstall_started(obs)) {
		if (self_frame_limit_reached(obs)) {
			ListReason reason;
			reason.text = ref(keys::kDockSelfSlotLimitText);
			// 幅が狭いときの短い形「枠に載せられるのは3人までです。」(一覧の上の 1 行は、
			// 事実だけの短い形を持つ。空いている枠がないときの no_free_slot と同じ形で、
			// dock/people_list.cpp の apply_reason が ElideLabel の forms に入れる)。
			// 全文の先頭の「自分の枠を使っている間は、」は、20 em の幅では、唯一の事実「3人まで」
			// を押し出す。
			reason.short_text = ref(keys::kDockSelfSlotLimitShortText);
			view.list_reason = reason;
		} else if (view.list_full) {
			ListReason reason;
			reason.text = ref(keys::kDockNoFreeSlotText);
			reason.short_text = ref(keys::kDockNoFreeSlotShortText);
			view.list_reason = reason;
		}
	}

	view.people_list = people_list_of(obs, voice_links);

	// 空の一覧。1 行目が理由を伝える 2 つ (接続していない、通話に参加していない) は、条件を言う 1 文にする。
	// 行があるかは参加者タブの一覧で決める (声の枠だけを持つ人は一覧に出さないので、空の理由を隠さない)。
	// Vesktop のアカウントの行だけのときは、行がないのと同じに扱う。接続が切れても行を残す間は、一覧は空ではない。
	const dockview::PeopleListView &list = view.people_list;
	const bool pinned_listed = list.pinned.has_value() && list.pinned->user_id != vesktop_account_id(obs);
	if (!pinned_listed && list.sharing.empty() && list.in_call.empty()) {
		EmptyList empty;
		if (uninstall_started(obs)) {
			// 接続や通話の状態によらない (待ち受けを閉じた後なので、「接続されると表示されます」
			// は事実と違う)。
			empty.kind = EmptyKind::Uninstalled;
			empty.text = ref(keys::kDockEmptyUninstalledEmpty);
		} else if (obs.link.state != LinkState::Connected) {
			empty.kind = EmptyKind::NotConnected;
			empty.text = ref(keys::kDockEmptyNotConnectedEmpty);
		} else if (obs.call.presence != Presence::InCall) {
			empty.kind = EmptyKind::NotInCall;
			empty.text = ref(keys::kDockEmptyNotInCallEmpty);
		} else {
			empty.kind = EmptyKind::Alone;
			empty.text = ref(keys::kDockEmptyAloneEmpty);
		}
		view.empty = empty;
	}
	view.audio_tab = audio_tab_of(view.people_list, obs.choices.composite_present);
	view.look_tab = look_tab(obs);
	view.accounts = accounts_of(obs);

	view.record = record_button(obs);
	view.menu = dock_menu(obs, view.record);
	view.participants = participant_rows(obs);
	view.about = about_page(obs);
	view.status_tab = status_tab_of(obs);
	view.uninstall_result = uninstall_result(obs);

	// 待ちを通す前の 1 行目。DockPoll は、stabilize() が決め直した 1 行目を使う。
	view.first_line = first_line_of(view.notices, view.fallback_line);
	return view;
}

} // namespace cordmux::ledger

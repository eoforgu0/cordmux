// SPDX-License-Identifier: Apache-2.0

// ポーリングのたびに読んだ値から、ledger::Observation を作る (build_observation.h の実装)。OsProbe の結果
// (platform/os_probe.h) と接続の統計 (ws::ListenerStats) を、Observation の各フィールドにコピーする。

#include "dock/build_observation.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dock/install_flow.h"
#include "platform/platform.h"

namespace cordmux::dock {

// 配信に表示するその人の名前 (約束は build_observation.h の宣言にある)。
std::string effective_display_name(const std::vector<ledger::SeenPerson> &seen_people,
				   const pipeline::ProfilesSnapshot &profiles, const std::string &user_id)
{
	if (user_id.empty())
		return {};
	const ledger::SeenPerson *seen = nullptr;
	for (const ledger::SeenPerson &person : seen_people) {
		if (person.user_id != user_id)
			continue;
		if (ledger::override_present(person))
			return *person.name_override;
		seen = &person;
		break;
	}
	const std::optional<std::string> name = profiles.display_name_of(user_id);
	if (name.has_value() && !name->empty())
		return *name;
	// 通話から抜けた人など、profiles に残っていない人は、最後に見たときの表示名を使う。
	return seen != nullptr ? seen->display_name : std::string();
}

std::set<std::string> unavailable_contact_ids(const std::vector<ledger::ContactFact> &contacts)
{
	std::set<std::string> out;
	for (const ledger::ContactFact &contact : contacts) {
		if (contact.id.empty())
			continue;
		out.insert(contact.id);
	}
	return out;
}

void update_contact_success(std::map<std::string, std::int64_t> *last_ok_at_us,
			    const std::vector<ledger::ContactFact> &contacts, bool facts_received,
			    std::int64_t now_us)
{
	if (last_ok_at_us == nullptr || !facts_received)
		return;

	const std::set<std::string> unavailable = unavailable_contact_ids(contacts);
	// 今読めない箇所も一覧に入れる (1 度も読めていないことを 0 で表す)。
	for (const std::string &id : unavailable)
		last_ok_at_us->emplace(id, 0);
	for (auto &entry : *last_ok_at_us) {
		if (unavailable.count(entry.first) == 0)
			entry.second = now_us;
	}
}

std::string self_frame_user_id(const std::string &broadcaster_user_id, const pipeline::PipelineSnapshot &snapshot)
{
	if (!broadcaster_user_id.empty())
		return broadcaster_user_id;
	return snapshot.facts.self_user_id.value_or(std::string());
}

std::string self_frame_user_id(const config::Settings &settings, const pipeline::PipelineSnapshot &snapshot)
{
	return self_frame_user_id(settings.broadcaster_user_id, snapshot);
}

SelfFrameIdentity self_frame_identity(const std::string &broadcaster_user_id, const pipeline::PipelineSnapshot &snapshot,
				      const std::vector<ledger::SeenPerson> &seen_people,
				      const std::string &last_known_vesktop_user_id)
{
	SelfFrameIdentity out;
	const std::string owner = self_frame_user_id(broadcaster_user_id, snapshot);
	out.user_id = !owner.empty() ? owner : last_known_vesktop_user_id;
	if (!out.user_id.empty())
		out.name = effective_display_name(seen_people, snapshot.profiles, out.user_id);
	return out;
}

bool self_frame_hears_voice(const std::vector<pipeline::SlotSnapshot> &slots, const std::string &self_user_id)
{
	if (self_user_id.empty())
		return false;
	for (const pipeline::SlotSnapshot &row : slots) {
		if (row.kind == config::SlotKind::Voice && row.owner_user_id == self_user_id && row.voice_present)
			return true;
	}
	return false;
}

std::vector<sources::CompositeRosterPerson> composite_roster_people(const pipeline::PipelineSnapshot &snapshot,
								    const config::Settings &settings,
								    const std::vector<ledger::SeenPerson> &seen_people,
								    const std::string &self_user_id)
{
	std::vector<sources::CompositeRosterPerson> out;
	if (!settings.roster_show || !snapshot.facts.roster.has_value())
		return out;

	std::set<std::string> hidden;
	for (const ledger::SeenPerson &person : seen_people) {
		if (person.roster_hidden)
			hidden.insert(person.user_id);
	}
	// ほかの人の行の「使う声」に選ばれている人。組み合わせは声の経路と同じ関数 (config::usable_voice_links) で絞る。
	std::set<std::string> lent;
	for (const auto &[owner, voice] : config::usable_voice_links(ledger::voice_source_marks(seen_people))) {
		if (voice != owner)
			lent.insert(voice);
	}
	// 受け取りに使うアカウント (サブアカウントの構成で、メインアカウントと別の Vesktop のアカウント)。メインアカウントだけの
	// 構成 (欄が空か、Vesktop のアカウントと同じ) では、Vesktop のアカウントは配信する本人なので外さない。
	const std::string vesktop = snapshot.facts.self_user_id.value_or(std::string());
	const bool sub_account = !settings.broadcaster_user_id.empty() && settings.broadcaster_user_id != vesktop;
	const std::string receiver = sub_account ? vesktop : std::string();

	std::set<std::string> added;
	for (const std::string &user_id : snapshot.facts.roster->user_ids) {
		if (user_id.empty() || added.count(user_id) != 0)
			continue;
		if (!receiver.empty() && user_id == receiver)
			continue; // 受け取りに使うアカウント
		if (hidden.count(user_id) != 0)
			continue; // 「通話のみ時ラベル非表示」を付けた人
		if (lent.count(user_id) != 0)
			continue; // 声をほかの人の行で使っている人 (その声は、選んだ人の名前ラベルを光らせる)
		if (!settings.roster_show_bots && snapshot.profiles.bot_of(user_id))
			continue;
		sources::CompositeRosterPerson person;
		person.name = effective_display_name(seen_people, snapshot.profiles, user_id);
		if (person.name.empty())
			continue; // 名前を取得できない人は描かない
		person.user_id = user_id;
		// 自分の枠の人の声を聞けるかは、自分の枠と同じ規則で決める (メインアカウントだけの構成では Discord が本人の声を
		// 返さないので聞けない)。ほかの人は、話せば声の経路を持つので聞ける。
		if (!self_user_id.empty() && user_id == self_user_id)
			person.hears_voice = self_frame_hears_voice(snapshot.slots, self_user_id);
		added.insert(user_id);
		out.push_back(std::move(person));
	}
	return out;
}

std::vector<std::uint8_t> decor_avatar(const pipeline::ProfilesSnapshot &profiles, const std::string &user_id,
				       const std::vector<std::uint8_t> *previous)
{
	// 表にある画像は、いつも profiles のアバターのバージョンに合っている (pipeline/profiles_state.h の apply_decoded_avatar)。
	const auto image = profiles.avatars.find(user_id);
	if (image != profiles.avatars.end() && image->second.png)
		return *image->second.png;
	const auto found = profiles.people.find(user_id);
	if (profiles.received && found != profiles.people.end() && found->second.avatar_key.has_value() &&
	    found->second.avatar_key->empty())
		return {}; // アバターを設定していない人
	// 画像がまだ分からない。
	return previous ? *previous : std::vector<std::uint8_t>();
}

namespace {

// ---- Observation の install (Vesktop へのインストール。platform/os_probe.h の OsProbeResult から) ----

ledger::VencordDirState dir_state_of(const platform::OsProbeResult &probe, bool hide_hash_mismatch)
{
	// state.json を読めていないのは、まだ分からないことであって、ほかの場所を指していることでも、
	// 設定していないことでもない。
	if (!probe.state_json_read)
		return ledger::VencordDirState::Unknown;
	// 読めたうえで vencordDir のキーがないのは、読めなかったのとは別のことである (利用者がまだ 1 度も Vencord の
	// 場所を設定していない。インストールした日)。これを Unknown にすると、その日の画面から [Vesktopに追加] が消え、
	// 利用者が先に進めなくなる (ledger/rules.cpp)。
	if (!probe.vencord_dir.has_value())
		return ledger::VencordDirState::NotConfigured;
	if (!probe.points_at_bundle)
		return ledger::VencordDirState::PointsElsewhere;
	switch (probe.bundle_hash.state) {
	case platform::BundleState::Matches:
		return ledger::VencordDirState::Matches;
	case platform::BundleState::Mismatch:
		// 前のバージョンのファイルをコピーし直した直後は、コピーする前の照合の結果で、上書きされていると
		// 表示しない (DockFacts::hide_hash_mismatch)。
		if (hide_hash_mismatch)
			break;
		return ledger::VencordDirState::HashMismatch;
	case platform::BundleState::Unknown:
		break;
	}
	// data/vencord-src を読めず、照合できなかったときは、違うとは表示しない (platform/bundle_check.h の
	// BundleState::Unknown)。
	return ledger::VencordDirState::Unknown;
}

ledger::Install install_of(const platform::OsProbeResult &probe, const DockFacts &facts)
{
	ledger::Install out;
	if (probe.probed) {
		out.process = probe.vesktop_process ? ledger::VesktopProcess::Running
						    : ledger::VesktopProcess::NotRunning;
		out.dir = dir_state_of(probe, facts.hide_hash_mismatch);
		// Vesktop が実行中であることは分かっていて、開始時刻だけを読めないことがある (platform.h の
		// find_vesktop_process。0 は分からないことを表す)。
		if (probe.vesktop_process && probe.vesktop_started_at_us > 0)
			out.vesktop_started_at_us = probe.vesktop_started_at_us;
	}
	// 書く直前に DockPoll が見た Vesktop の状態 (DockFacts::vesktop_process_seen)。OsProbe の結果より新しいので、
	// こちらを使う。起動の時刻は OsProbe の値のままにする (再起動を勧めるかは、OsProbe が読んだ時刻で決める)。
	if (facts.vesktop_process_seen.has_value())
		out.process = *facts.vesktop_process_seen;
	// バックアップから読んだ、cordmux が最後に Vesktop の設定を書いた時刻。問い合わせが 1 度も終わっていなければ、
	// 値は入っていない。
	out.discord_side_written_at_us = probe.state_json_written_at_us;

	// 違ったファイルの名前。HashMismatch のときだけ入れる (ledger/observation.h の Install::mismatched_files の
	// 約束)。上書きされたと表示しないときに、名前だけが残らないようにするためである。順は照合の順にする
	// (dock/install_flow.h の mismatched_file_names)。
	if (out.dir == ledger::VencordDirState::HashMismatch)
		out.mismatched_files = mismatched_file_names(probe.bundle_hash);

	// 起動時の書き直しと、[再インストール] でコピーできなかったファイルは、DockPoll が持つ情報である。
	out.rewrote_at_startup = facts.rewrote_at_startup;
	out.reinstall_failed_files = facts.reinstall_failed_files;
	// この起動で Vesktop の設定を書き込まないか。値は起動時の設定で決まり、照合の結果によらないので、問い合わせが
	// 終わる前からコピーする。問い合わせの後にコピーすると、問い合わせが終わるまでのポーリングの間だけ、ボタンが
	// 表示されてから消える。
	out.vesktop_writes_off = facts.vesktop_writes_off;
	return out;
}

// ---- Observation の listener (待ち受け。ws/listener.h の OpenSummary から) ----

ledger::ListenerFacts listener_of(const ws::ListenerStats &stats)
{
	ledger::ListenerFacts out;
	if (!stats.open.attempted)
		return out; // Unknown のまま (まだ待ち受けを始めようとしていない)

	switch (stats.open.outcome) {
	case ws::OpenOutcome::Opened:
	case ws::OpenOutcome::AlreadyOpenedBefore:
		// 待ち受けを始められたことと、待ち受けていることは別である (ws/listener.h の listening)。
		out.state = stats.listening ? ledger::ListenerState::Listening : ledger::ListenerState::Closed;
		// 待ち受けを始めたが、トークンファイル (session.json) を保存できなかった。待ち受けの状態は
		// Listening のまま (事実のとおり) にし、表示の規則は、待ち受け中のときだけこのフラグを読む。
		// AcceptFailed は outcome が PortsExhausted なので、ここには来ない (下の PortsExhausted の場合で扱う)。
		out.session_file_not_saved = stats.open.problem == ws::OpenProblem::SessionFileNotSaved;
		break;
	case ws::OpenOutcome::BlockedByOtherWriter:
		out.state = ledger::ListenerState::BlockedByOtherWriter;
		if (stats.open.other_writer_pid != 0)
			out.other_writer_pid = static_cast<int>(stats.open.other_writer_pid);
		{
			// session.json の started_at は、pid の再利用を見分けるための実装に依存した値で、画面に表示する
			// 時刻ではない (docs/wire_format.md の「トークンファイル」)。時刻の基準を変換してから渡す
			// (platform.h の os_process_time_to_epoch_us)。
			const std::int64_t started_at_us =
				platform::os_process_time_to_epoch_us(stats.open.other_writer_started_at);
			if (started_at_us > 0)
				out.other_writer_started_at_us = started_at_us;
		}
		break;
	case ws::OpenOutcome::PortsExhausted:
		out.state = ledger::ListenerState::PortsExhausted;
		// 実際に bind を試したときだけ、ポート番号が入る (試していない範囲を、試したと表示しない)。
		if (stats.open.port_first != 0)
			out.port_first = static_cast<int>(stats.open.port_first);
		if (stats.open.port_last != 0)
			out.port_last = static_cast<int>(stats.open.port_last);
		// ポートは開けたが、受け付けを始められなかったとき (OpenProblem::AcceptFailed)。outcome は同じ
		// PortsExhausted なので、フラグを別にコピーし、すべて使用中という文を表示しないようにする
		// (ledger/observation.h の accept_failed)。
		out.accept_failed = stats.open.problem == ws::OpenProblem::AcceptFailed;
		break;
	}
	return out;
}

// ---- Observation の link (接続。ws の DisconnectReason と 1 対 1) ----

ledger::DisconnectReason reason_of(ws::DisconnectReason reason)
{
	switch (reason) {
	case ws::DisconnectReason::SecretMismatch:
		return ledger::DisconnectReason::SecretMismatch;
	case ws::DisconnectReason::WireMismatch:
		return ledger::DisconnectReason::WireMismatch;
	case ws::DisconnectReason::MalformedHello:
		return ledger::DisconnectReason::MalformedHello;
	case ws::DisconnectReason::HelloTimeout:
		return ledger::DisconnectReason::HelloTimeout;
	case ws::DisconnectReason::HeartbeatTimeout:
		return ledger::DisconnectReason::HeartbeatTimeout;
	case ws::DisconnectReason::PeerClosed:
		return ledger::DisconnectReason::PeerClosed;
	case ws::DisconnectReason::LocalClose:
	case ws::DisconnectReason::Unknown:
		break;
	}
	// LocalClose (cordmux から閉じた) に当たる値は ledger にないので (ws/listener.h の DisconnectReason)、
	// Unknown を返す。
	return ledger::DisconnectReason::Unknown;
}

ledger::Link link_of(const ws::ListenerStats &stats)
{
	ledger::Link out;
	// 接続した時刻は、hello の成否より前の、クライアントを受け付けた時刻である (ws/listener.cpp の
	// connection_opened_at_us)。トークンが合わずに切れた接続も、接続したが切れたという表示になるので、
	// hello が通った時刻で判断すると、トークンが合わないことを 1 度も表示できない。
	if (stats.connection_opened_at_us > 0)
		out.connected_at_us = stats.connection_opened_at_us;
	if (stats.connection_closed_at_us > 0)
		out.disconnected_at_us = stats.connection_closed_at_us;

	if (stats.client_connected) {
		out.state = ledger::LinkState::Connected;
		return out;
	}
	if (stats.connection_closed_at_us > 0) {
		out.state = ledger::LinkState::Disconnected;
		out.reason = reason_of(stats.last_disconnect_reason);
		return out;
	}
	if (stats.open.attempted) {
		// 待ち受けを始めた結果は出ているが、まだ 1 度もクライアントが接続していない。
		out.state = ledger::LinkState::NeverConnected;
		return out;
	}
	return out; // Unknown
}

// ---- Observation の people (人) ----

// steady_clock で測った最後のフレームの時刻を、画面が使うシステム時刻の基準に変換する。経過時間は steady_clock で測る
// (システム時刻の変更で変わらない。pipeline.h の StreamFrameClock)。測った経過時間を、現在のシステム時刻
// (facts.now_us) から引く。
//
// 2 つの表 (last_frames と record_only_frames) が同じ変換をするので、フィールドの名前ではなく 2 つの値で受け取り、
// 同じ計算を 2 か所に持たない。
std::optional<std::int64_t> frame_time_us(const pipeline::PipelineSnapshot &snapshot, const DockFacts &facts,
					  std::int64_t last_steady_us, std::int64_t last_wall_us)
{
	if (last_steady_us > 0 && snapshot.taken_at_steady_us > 0) {
		std::int64_t elapsed = snapshot.taken_at_steady_us - last_steady_us;
		if (elapsed < 0)
			elapsed = 0;
		return facts.now_us - elapsed;
	}
	// steady_clock の値がないスナップショット (テストで作ったものなど) では、システム時刻の値をそのまま使う。
	if (last_wall_us > 0)
		return last_wall_us;
	return std::nullopt;
}

// 人の一覧を作るためのクラス。user_id ごとに 1 行だけ作り、最初に作った行に値を追加していく。
class PeopleBuilder {
public:
	// user_id の行を用意して返す。空の user_id では行を作らない (nullptr を返す)。
	ledger::PersonFact *ensure(const std::string &user_id)
	{
		if (user_id.empty())
			return nullptr;
		const auto found = index_.find(user_id);
		if (found != index_.end())
			return &rows_[found->second];
		index_.emplace(user_id, rows_.size());
		ledger::PersonFact row;
		row.user_id = user_id;
		rows_.push_back(std::move(row));
		return &rows_.back();
	}

	// user_id の行があれば返す。行は作らない (なければ nullptr を返す)。
	ledger::PersonFact *find(const std::string &user_id)
	{
		const auto found = index_.find(user_id);
		return found != index_.end() ? &rows_[found->second] : nullptr;
	}

	std::vector<ledger::PersonFact> take() { return std::move(rows_); }

private:
	std::vector<ledger::PersonFact> rows_;
	std::map<std::string, std::size_t> index_;
};

std::vector<ledger::PersonFact> people_of(const pipeline::PipelineSnapshot &snapshot, const DockFacts &facts)
{
	PeopleBuilder builder;

	// (1) 枠に載せた人。画面共有 1..4、声 1..8 の順 (snapshot.slots の順)。slot に入れるのは画面共有の枠だけである
	//     (ledger::PersonFact::slot の宣言)。声の枠だけの人も一覧には入れるが、枠の番号は持たない。
	for (const pipeline::SlotSnapshot &row : snapshot.slots) {
		ledger::PersonFact *person = builder.ensure(row.owner_user_id);
		if (person == nullptr)
			continue;
		if (row.kind == config::SlotKind::Screen && !person->slot.has_value()) {
			person->slot = row.slot;
			person->slot_added_order = row.added_order;
			person->slot_visible = row.video_allowed && row.share_present;
		}
		// 声が今届いているか。使う声を置き換えている枠でも、配信に出る声は枠に載っている人の行で操作する。
		if (row.kind == config::SlotKind::Voice && row.voice_present)
			person->voice_arriving = true;
	}

	// (2) 通話に参加している人 (roster)。枠に載せていない人も、ここで一覧に入る。通話に参加していることを表す
	//     in_roster は、ここでだけ true にする。(1) の枠に載っている人は、通話の参加者が分からない間は通話から抜けても people に残るので、
	//     people にいることは、通話に参加していることを表さない (observation.h の PersonFact::in_roster)。
	if (snapshot.facts.roster.has_value()) {
		for (const std::string &user_id : snapshot.facts.roster->user_ids) {
			ledger::PersonFact *person = builder.ensure(user_id);
			if (person != nullptr)
				person->in_roster = true;
		}
	}

	// (3) 今画面共有している人 (facts の sharing)。これがないと、画面共有している人についての案内が成り立たない
	//     (observation.h の PersonFact::sharing)。枠にもフレームにも現れていない人 (画面共有を始めた直後) も、
	//     ここで一覧に入る。
	for (const std::string &user_id : snapshot.facts.sharing.user_ids) {
		ledger::PersonFact *person = builder.ensure(user_id);
		if (person != nullptr)
			person->sharing = true;
	}
	// 画面共有しているかを信じてよいか (sharing_known) と、画面共有していると分かった時刻は、後の (4) で作る行にも
	// 付ける必要があるので、すべての行を作った後 (下の (7)) で付ける。

	// (4) フレームが届いている人と、そのエンコードの段階 (facts の stages)。stages は stream_id ごとに来るので、
	//     ストリームごとの最後のフレームの時刻 (last_frames) を通して、人に結び付ける。フレームがまだ
	//     1 枚も届いていない人の段階は、下の (4a) で付ける。
	std::map<std::uint32_t, std::string> stage_of_stream;
	if (snapshot.facts.stages.has_value()) {
		for (const pipeline::StreamStage &entry : *snapshot.facts.stages)
			stage_of_stream[entry.stream_id] = entry.stage;
	}

	for (const auto &entry : snapshot.last_frames) {
		const pipeline::StreamFrameClock &clock = entry.second;
		ledger::PersonFact *person = builder.ensure(clock.user_id);
		if (person == nullptr)
			continue;
		const std::optional<std::int64_t> at_us =
			frame_time_us(snapshot, facts, clock.last_video_frame_steady_us, clock.last_video_frame_wall_us);
		if (!at_us.has_value())
			continue;
		// 同じ人に複数のストリームがあれば、いちばん新しいフレームを使う (そのストリームの段階を表示する)。
		if (person->last_video_frame_at_us.has_value() && *person->last_video_frame_at_us >= *at_us)
			continue;
		person->last_video_frame_at_us = at_us;
		const auto stage = stage_of_stream.find(clock.stream_id);
		if (stage != stage_of_stream.end())
			person->stage = stage->second;
		else
			person->stage.reset();

		// 届いている解像度とフレームレート。段階と同じストリームから取るので、1080p なのに下げている、
		// のような食い違いが画面に出ない (どちらも、今フレームが来ているストリームのことである)。
		// 0 の値は持たない。最初の段階が決まるまでと unavailable の間は、送信側が省くので、0x0 とは表示しない
		// (docs/wire_format.md の「メッセージの種類」)。
		person->width.reset();
		person->height.reset();
		person->fps.reset();
		const auto format = snapshot.stream_formats.find(clock.stream_id);
		if (format != snapshot.stream_formats.end()) {
			if (format->second.width > 0)
				person->width = format->second.width;
			if (format->second.height > 0)
				person->height = format->second.height;
			if (format->second.fps > 0.0)
				person->fps = format->second.fps;
		}
	}

	// (4a) フレームがまだ 1 枚も届いていない人の段階。上の (4) はフレームの時刻を通してしかストリームを人に
	//      結び付けないので、最初のエンコーダの確保で段階が尽きたストリーム (unavailable) の人には、
	//      段階が付かない。送信側はそのとき読み出しを始めないので (src-vencord/encode/session.ts の start())、
	//      フレームは 1 枚も来ない。行は「映像を待っています...」のままになり、メッセージの次の操作も段階で選べない。
	//      これがいちばん起こりやすいのは、最初からエンコーダを使えない PC である。
	//
	// 結び付ける先は、configure のストリームの表 (stream_formats の user_id) である。送信側は、段階が決まる前と
	// unavailable の間もストリームを configure に入れ (省くのは解像度だけ。src-vencord/index.ts の
	// buildConfigureStreams)、段階が変わるたびに facts と configure を送り直す (同じファイルの videoSink の
	// onStage)。ストリームと人の組み合わせは、ワイヤー形式の約束である (docs/wire_format.md の
	// 「メッセージの種類」)。受信側は経路 (route) のないストリームも持ち、接続が切れたら last_frames と一緒に
	// 捨てる (pipeline.cpp) ので、前の接続の番号で、別の人に結び付けることはない。
	//
	// 付けるのは、フレームの時刻を持たない人だけである。フレームが届いている人は (4) のとおり、フレームが来ている
	// ストリームの段階を表示する (報告のないストリームに段階を付けない約束も変えない)。行は作らない ((1) から
	// (3) で作った行にだけ付ける)。1 人に画面共有のストリームが 2 つ以上あれば、stream_id の大きいほう (後から
	// 確保したストリーム。送信側は接続の中で番号を 1 つずつ増やして割り当てる。index.ts の nextStreamId) を使う。
	// 解像度も同じストリームから取る ((4) と同じ理由。unavailable の間は送信側が省くので付かない)。
	//
	// 次の方法は取らない。
	//   - 送信側が facts の stages で unavailable を送るときに、そのストリームの人の userId も入れる: 同じ
	//     組み合わせを configure が既に送っているので、ワイヤー形式のフィールドを増やすだけになる。
	//   - (4) の結び付け方ごと configure に替える: 同じ人に古いストリームが残る間、フレームが来ているストリームと
	//     段階のストリームがずれることがある。
	//   - 枠に載っている人だけに付ける: 枠に載っていない、cordmux の録画の対象の人も、映像が届かない人のメッセージ
	//     (rules_dock.cpp の AllVideoStoppedHint) で数えられる。このメッセージは次の操作を段階によって選ぶので、
	//     その人の次の操作を選べないまま残る。
	std::map<std::string, std::uint32_t> configured_screen_of;
	for (const auto &entry : snapshot.stream_formats) {
		const pipeline::StreamFormat &format = entry.second;
		if (!format.screen || format.user_id.empty())
			continue;
		std::uint32_t &chosen = configured_screen_of[format.user_id];
		if (entry.first > chosen)
			chosen = entry.first;
	}
	for (const auto &entry : configured_screen_of) {
		ledger::PersonFact *person = builder.find(entry.first);
		if (person == nullptr || person->last_video_frame_at_us.has_value())
			continue;
		const auto stage = stage_of_stream.find(entry.second);
		if (stage == stage_of_stream.end())
			continue;
		person->stage = stage->second;
		const pipeline::StreamFormat &format = snapshot.stream_formats.at(entry.second);
		person->width.reset();
		person->height.reset();
		person->fps.reset();
		if (format.width > 0)
			person->width = format.width;
		if (format.height > 0)
			person->height = format.height;
		if (format.fps > 0.0)
			person->fps = format.fps;
	}

	// (4b) 録画の入力を通ったフレーム。経路の有無を条件にしない別の表なので、枠に載っていない人も、ここで一覧に
	//      入る (録画のためだけに受け取り続けている、という表示は、このフィールドでしか成り立たない。pipeline.h の
	//      RecordOnlyFrameClock)。
	//
	// 声のストリームは数えない。このフィールドを読む判断 (ledger/rules.cpp の RecordingOnly) は、
	// 枠に載っていないことを PersonFact::slot で見ており、slot は画面共有の枠だけである (上の (1))。声の枠だけの
	// 人は slot を持たないので、声のストリームをここに入れると、配信に声が出ている人を、録画のためだけに
	// 受け取っていると表示することになる。この表と対になる last_frames も映像の時刻なので、映像にそろえる。
	// 行そのものは、声のストリームでも作る (何かが届いている人は一覧に入る。上の (4) と同じ約束)。
	// (4b') 受信をやり直した回数 (pipeline/receive_restart.h)。いま一覧にいる人にだけ入れる (抜けた人の行は作らない)。
	for (const auto &[user_id, count] : snapshot.receive_restarts) {
		if (ledger::PersonFact *person = builder.find(user_id))
			person->receive_restarts = count;
	}

	for (const auto &entry : snapshot.record_only_frames) {
		const pipeline::RecordOnlyFrameClock &clock = entry.second;
		ledger::PersonFact *person = builder.ensure(clock.user_id);
		if (person == nullptr)
			continue;
		if (!clock.screen)
			continue;
		const std::optional<std::int64_t> at_us =
			frame_time_us(snapshot, facts, clock.last_frame_steady_us, clock.last_frame_wall_us);
		if (!at_us.has_value())
			continue;
		if (person->last_record_frame_at_us.has_value() && *person->last_record_frame_at_us >= *at_us)
			continue;
		person->last_record_frame_at_us = at_us;
	}

	// (5) 表示名 (profiles と、利用者が変えた名前)。2 か所に渡す名前の 1 つ目である (PersonFact.display_name、
	//     名前ラベル。名前ラベルに渡すのは dock/dock_poll_layout.cpp の distribute_person_decor で、同じ
	//     effective_display_name を呼ぶ)。名前を取得できない人には入れない (画面は userId をそのまま表示する)。
	//
	// (6) Vesktop のアカウント (facts の self_user_id)。行は作らず、既にある行にフラグを付けるだけである。
	//     Vesktop のアカウントであることは、通話に参加していることを表さないので、roster にも枠にもいない
	//     Vesktop のアカウントの行をここで作ると、通話に参加していない人が参加者の一覧に入る (observation.h の
	//     PersonFact::is_receiver)。
	const std::optional<std::string> &receiver = snapshot.facts.self_user_id;
	// 自分の枠の人 (PersonFact::is_self_frame)。メインアカウントの userId が空でなければその人、空なら Vesktop のアカウント。
	const std::string self_frame_owner = self_frame_user_id(facts.broadcaster_user_id, snapshot);
	std::vector<ledger::PersonFact> rows = builder.take();
	for (ledger::PersonFact &person : rows) {
		const std::string name = effective_display_name(facts.seen.people, snapshot.profiles, person.user_id);
		if (!name.empty())
			person.display_name = name;
		person.is_receiver = receiver.has_value() && person.user_id == *receiver;
		person.is_self_frame = !self_frame_owner.empty() && person.user_id == self_frame_owner;
		person.is_bot = snapshot.profiles.bot_of(person.user_id);

		// (7) 次の 4 つのフィールド。
		//   - sharing_known: sharing_valid が true の facts のときだけ true にする。sharing のキーがないか
		//     壊れていた facts では、集合が空でも、誰も画面共有していないと分かったことにならない
		//     (facts_state.h の sharing_valid)。
		//   - sharing_since_us: 画面共有の集合にいる人だけが持つ (facts_state.h の carry_sharing_since が入れた
		//     時刻)。
		//   - discord_name と username: 送信側の profiles の値のまま (利用者が変えた名前は使わない)。
		person.sharing_known = snapshot.facts.received && snapshot.facts.sharing_valid;
		if (person.sharing_known && person.sharing) {
			const auto since = snapshot.facts.sharing_since_us.find(person.user_id);
			if (since != snapshot.facts.sharing_since_us.end())
				person.sharing_since_us = since->second;
		}
		const auto watching = snapshot.watching_since_us.find(person.user_id);
		if (watching != snapshot.watching_since_us.end())
			person.watching_since_us = watching->second;
		const std::optional<std::string> discord_name = snapshot.profiles.display_name_of(person.user_id);
		if (discord_name.has_value() && !discord_name->empty())
			person.discord_name = discord_name;
		const std::optional<std::string> username = snapshot.profiles.username_of(person.user_id);
		if (username.has_value() && !username->empty())
			person.username = username;

		// (8) 参加者ごとの音量とミュート (Pipeline の表のコピー)。表に入っていない人は、既定の値 (ミュートなし、100%) の
		//     ままにする。
		const auto audio = snapshot.person_audio.find(person.user_id);
		if (audio != snapshot.person_audio.end()) {
			person.audio.voice_muted = audio->second.voice_muted;
			person.audio.voice_volume_pct = audio->second.voice_volume_pct;
			person.audio.screen_muted = audio->second.screen_muted;
			person.audio.screen_volume_pct = audio->second.screen_volume_pct;
			person.audio.has_screen_audio = audio->second.has_screen_audio;
		}
	}
	return rows;
}

// ---- Observation の call (通話) ----

ledger::Call call_of(const pipeline::PipelineSnapshot &snapshot, const std::string &broadcaster_user_id)
{
	ledger::Call out;
	// facts が 1 回も届いていないか、roster のキーが省かれている間は、まだ分からない (docs/wire_format.md の
	// 「facts」)。
	if (!snapshot.facts.received || !snapshot.facts.roster.has_value())
		return out;
	// 人数は user_ids の数で数える (送信側の count は使わない。facts_state.h の UserIdSet)。
	const int count = static_cast<int>(snapshot.facts.roster->user_ids.size());
	out.roster_count = count;
	out.presence = count > 0 ? ledger::Presence::InCall : ledger::Presence::NotInCall;
	// 通話の名前 (キーがなければ空のまま。画面には「-」と表示する)。
	out.name = snapshot.facts.call_name.value_or(std::string());
	// メインアカウントがいる通話。送信側は見える通話に参加しているときだけ送るので、キーがあれば InCall で、なければ分からない
	// (NotInCall にしない)。送信側が読んだときのメインアカウントが、現在の設定のメインアカウントと違うときは、設定を変えた
	// 直後の古い値なので使わない。
	const auto &main_call = snapshot.facts.main_call;
	if (main_call.has_value() && !broadcaster_user_id.empty() && main_call->user_id == broadcaster_user_id) {
		out.main_presence = ledger::Presence::InCall;
		out.main_name = main_call->name.value_or(std::string());
		out.main_call_joinable = main_call->joinable;
	}
	return out;
}

// ---- Observation の recording (録画) ----

// 録画ファイルの作成の進み具合を変換する (ledger の列挙は、record の列挙に Unknown を加えたもの。observation.h)。
// default: を書かないのは、record の側に値が増えたときに、この switch が取りこぼしたことに、コンパイラの警告で
// 気付けるようにするためである。default: で Unknown にすると、新しい状態が画面から消える。
ledger::FinalizeState finalize_state_of(record::FinalizeState state)
{
	switch (state) {
	case record::FinalizeState::NotStarted:
		return ledger::FinalizeState::NotStarted;
	case record::FinalizeState::Running:
		return ledger::FinalizeState::Running;
	case record::FinalizeState::Done:
		return ledger::FinalizeState::Done;
	case record::FinalizeState::Pending:
		return ledger::FinalizeState::Pending;
	case record::FinalizeState::Failed:
		return ledger::FinalizeState::Failed;
	}
	return ledger::FinalizeState::Unknown;
}

ledger::FinalizeBranch finalize_branch_of(const std::optional<record::FinalizeBranch> &branch)
{
	// まだ決まっていなければ Unknown にする (record の側は、分からないことを列挙の値にせず、std::optional で表す。
	// record/record_types.h)。
	if (!branch.has_value())
		return ledger::FinalizeBranch::Unknown;
	switch (*branch) {
	case record::FinalizeBranch::Remux:
		return ledger::FinalizeBranch::Remux;
	case record::FinalizeBranch::ProRes:
		return ledger::FinalizeBranch::ProRes;
	}
	return ledger::FinalizeBranch::Unknown;
}

// 録画の段階を変換する (ledger の列挙は、record の列挙に Unknown を加えたもの)。default: は書かない (上の
// finalize_state_of と同じ理由)。
ledger::RecordPhase record_phase_of(record::RecordPhase phase)
{
	switch (phase) {
	case record::RecordPhase::Idle:
		return ledger::RecordPhase::Idle;
	case record::RecordPhase::Starting:
		return ledger::RecordPhase::Starting;
	case record::RecordPhase::Recording:
		return ledger::RecordPhase::Recording;
	case record::RecordPhase::Stopping:
		return ledger::RecordPhase::Stopping;
	case record::RecordPhase::StartFailed:
		return ledger::RecordPhase::StartFailed;
	}
	return ledger::RecordPhase::Unknown;
}

ledger::RecordingFacts recording_of(const pipeline::PipelineSnapshot &snapshot,
				    const platform::OsProbeResult &probe)
{
	const record::RecordingSnapshot &src = snapshot.recording;
	ledger::RecordingFacts out;

	// セグメントの数は、閉じたセグメントと、今開いているセグメントの合計である (画面に表示するのは、
	// この録画でいくつ書いたかで、閉じたかどうかは、利用者には関係ない)。
	out.segment_count = src.segment_count + src.open_segment_count;
	out.bytes_written = src.bytes_written;
	out.free_space_bytes = src.free_space_bytes;
	out.remaining_seconds = src.remaining_seconds;
	out.finalize_need_bytes = src.finalize_need_bytes;

	out.finalize = finalize_state_of(src.finalize.state);
	out.branch = finalize_branch_of(src.finalize.branch);
	out.finalize_files_total = src.finalize.files_total;
	out.finalize_files_done = src.finalize.files_done;
	out.finalize_ffmpeg_unavailable = src.finalize.ffmpeg_unavailable;

	out.pending_present = src.pending_present;
	out.pending_session_count = src.pending_session_count;
	// 録画ログに残った、録画ファイルの作成に失敗した録画 (作成が成功するまで、失敗のメッセージを残す)。
	out.failed_session_dir = src.failed_session_dir;

	out.sender_recording = snapshot.sender_recording;

	// いつオンにしたか (待ち時間の判断が読む)。録画していない間は持たない。0 は 1 度もオンにしていないことを表し、
	// 時刻ではない (record/record_types.h)。
	if (src.enabled && src.started_at_wall_us > 0)
		out.enabled_since_us = src.started_at_wall_us;

	// 残り容量が少ないと判断するしきい値は、settings から取る。分を秒にする。
	out.space_warn_seconds = static_cast<std::int64_t>(snapshot.settings.record_space_warn_minutes) * 60;

	// 保存先に書き込めたか (持たないときは、まだ確認していない)。false になるのは、録画をオンにしてフォルダーを
	// 作れなかったときで、そのとき [cordmuxの録画を開始] は押せなくなり、理由がボタンの近くに表示される
	// (ledger/actions.cpp の ToggleRecording)。
	out.save_dir_writable = src.save_dir_writable;

	// 録画が自分で止まったとき (止まるのは、空き容量がなくなったときだけ)。画面に表示する語は 1 つなので、
	// 理由があるかを、そのまま bool にする (ledger は record の列挙を持ち込まない。observation.h の
	// FinalizeState と同じ)。
	out.stopped_by_space_exhausted =
		src.self_stopped_reason.has_value() &&
		*src.self_stopped_reason == record::StopReason::SpaceExhausted;

	// 空き容量には 2 つの取得元がある。録画中は録画の側の値が新しいので、それがあればそのまま使い、ないとき (録画を
	// 始める前や、まだ 1 度も確認していないとき) だけ、OsProbe の結果で埋める。どちらも、持たないときは
	// 読めていないことを表す。
	if (!out.free_space_bytes.has_value())
		out.free_space_bytes = probe.free_space_bytes;

	// ---- 録画の段階、途切れ、録画ファイルの作成の進み具合 ----
	// 段階と、その段階に入った時刻。時刻の 0 はまだ入っていないことを表すので、持たない。
	out.phase = record_phase_of(src.phase);
	if (src.phase_since_wall_us > 0)
		out.phase_since_us = src.phase_since_wall_us;
	// 途切れ (数えているのは、録画中に Vesktop との接続が切れていた間だけ)。
	out.gaps.reserve(src.gaps.size());
	for (const record::RecordGapSpan &gap : src.gaps) {
		ledger::RecordGapFact fact;
		fact.start_us = gap.start_wall_us;
		fact.duration_us = gap.duration_us;
		out.gaps.push_back(fact);
	}
	// 録画ファイルの作成の進み具合の細かい値。数は、録画の側が実際の作業から数えたものを、そのままコピーする。
	out.finalize_current_index = src.finalize.current_index;
	out.finalize_permille = src.finalize.current_permille;
	if (src.finalize.last_progress_wall_us > 0)
		out.finalize_last_progress_us = src.finalize.last_progress_wall_us;
	out.finalize_stalled = src.finalize.stalled;
	out.unwritable_dir = src.unwritable_dir;
	out.session_dir = src.session_dir;
	return out;
}

} // namespace

ledger::Observation build_observation(const pipeline::PipelineSnapshot &snapshot,
				      const platform::OsProbeResult &probe, const DockFacts &facts)
{
	ledger::Observation obs;
	obs.now_us = facts.now_us;
	obs.utc_offset_sec = facts.utc_offset_sec;
	obs.video_stall_threshold_us = facts.video_stall_threshold_us;

	obs.install = install_of(probe, facts);
	obs.listener = listener_of(snapshot.listener);
	obs.link = link_of(snapshot.listener);

	obs.choices.feature = facts.feature;
	// 録画のオンとオフは、受信側のメモリだけが持つ (settings にも書かず、起動するたびにオフに戻る)。それを持つのは
	// Pipeline なので、ドックはコピーを持たずにここで読む (同じ選択を 2 か所に持たないために、DockFacts に録画の
	// フィールドはない)。Unknown にはならない。自分のメモリの値なので、まだ読めていない状態がない。
	obs.choices.recording = snapshot.recording.enabled ? ledger::FeatureSwitch::On : ledger::FeatureSwitch::Off;
	obs.choices.dummy = facts.dummy;
	obs.choices.discord_audio_source = facts.discord_audio_source;
	obs.choices.discord_audio_source_name = facts.discord_audio_source_name;
	obs.choices.composite_active = facts.composite_active;
	obs.choices.composite_audible = facts.composite_audible;
	// 次の 3 つ (自分の枠、メインアカウント、載せる人の選び方) は、どれも DockPoll が持つ情報で、Pipeline も OsProbe も
	// 知らない。
	obs.choices.self_frame_shown = facts.self_frame_shown;
	obs.choices.self_frame_selected = facts.self_frame_selected;
	obs.choices.self_frame_bound = facts.self_frame_bound;
	obs.choices.self_frame_on = facts.self_frame_on;
	obs.choices.self_frame_share = facts.self_frame_share;
	obs.choices.broadcaster_user_id = facts.broadcaster_user_id;
	obs.choices.roster_show = facts.roster_show;
	obs.choices.roster_show_bots = facts.roster_show_bots;
	obs.choices.assign_mode = facts.assign_mode;
	obs.choices.receive_scope = facts.receive_scope;
	obs.choices.composite_present = facts.composite_present;
	obs.choices.composite_checked = facts.composite_checked;
	// settings からコピーする 4 つ。どれも利用者の選択そのものである。
	obs.choices.record_dir = facts.record_dir;
	obs.choices.record_output_pcm = facts.record_output_pcm;
	obs.choices.limit_max_long_edge = facts.limit_max_long_edge;
	obs.choices.limit_fps_divisor = facts.limit_fps_divisor;
	obs.choices.slot_order = facts.slot_order;
	obs.choices.self_slot_first = facts.self_slot_first;
	obs.choices.self_frame_added_mark = facts.self_frame_added_mark;
	{
		const SelfFrameIdentity self = self_frame_identity(facts.broadcaster_user_id, snapshot, facts.seen.people,
								   facts.last_known_vesktop_user_id);
		obs.choices.self_frame_user_id = self.user_id;
		obs.choices.self_frame_name = self.name;
	}
	// 全画面は DockPoll がメモリにだけ持つ (保存しない)。
	obs.choices.focus_user_id = facts.focus_user_id;
	obs.choices.focus_name = facts.focus_name;
	obs.choices.focus_candidates = facts.focus_candidates;

	// Discord の内部への依存箇所 (contacts)。送信側が送るのは、missing と lost を混ぜた id の一覧 (facts の
	// warnings) なので、受信側でその 2 つを分けられない。どちらも同じ「読めません」の警告になるので、
	// Missing にそろえる (ledger/rules.cpp の ContactUnavailable は、Missing と Lost を同じに扱う)。
	obs.contacts.reserve(snapshot.facts.warnings.size());
	for (const std::string &id : snapshot.facts.warnings) {
		ledger::ContactFact contact;
		contact.id = id;
		contact.state = ledger::ContactState::Missing;
		obs.contacts.push_back(std::move(contact));
	}

	obs.people = people_of(snapshot, facts);
	// 接続している間は people がそのまま最新なので、切れる前の値を持ち込まない。
	if (obs.link.state != ledger::LinkState::Connected)
		obs.last_known_people = facts.last_known_people;
	// Vesktop のアカウントは、通話の外でも facts の self_user_id で分かる。分からない間 (接続が切れている間など) は、
	// 最後に分かった値を使う。
	obs.vesktop_user_id = snapshot.facts.self_user_id.value_or(std::string());
	if (!obs.vesktop_user_id.empty())
		obs.vesktop_name = effective_display_name(facts.seen.people, snapshot.profiles, obs.vesktop_user_id);
	if (obs.vesktop_user_id.empty()) {
		obs.vesktop_user_id = facts.last_known_vesktop_user_id;
		obs.vesktop_name = facts.last_known_vesktop_name;
	} else if (obs.vesktop_name.empty() && obs.vesktop_user_id == facts.last_known_vesktop_user_id) {
		obs.vesktop_name = facts.last_known_vesktop_name;
	}
	obs.call = call_of(snapshot, facts.broadcaster_user_id);
	// 録画。Pipeline が持っている値を、そのままコピーする (どれを画面に表示するか、どこからが少ないかは
	// ledger::decide_dock が決める)。
	obs.recording = recording_of(snapshot, probe);
	// これまでに見た人の一覧。ドックは一覧を作り替えない。並べ方も、今通話に参加している人を外すことも、
	// ledger::decide_dock が決める。
	obs.seen = facts.seen;

	obs.versions.plugin = facts.plugin_version;
	// 送信側が hello で伝えたバージョン。切れたら空に戻る (ws/listener.h の peer_version)。
	obs.versions.discord_side = snapshot.listener.peer_version;
	// Vesktop のバージョンも、送信側が hello の任意のキー `vesktop` で伝える (docs/wire_format.md の
	// 「メッセージの種類」)。伝えなければ空で、"-" は ledger::decide_dock が入れる。切れたら空に戻る。
	obs.versions.vesktop = snapshot.listener.peer_vesktop_version;
	// 同梱の Vencord のバージョンは、data/vencord-src にある cordmux-build.txt の build_id と
	// vencord_commit である。読めなければ空。
	obs.versions.bundled_vencord = facts.bundled_vencord_version;
	obs.versions.bundled_vencord_commit = facts.bundled_vencord_commit;
	// Vencord のリポジトリでのバージョンのタグ。表示にだけ使い、比べるのは上の build_id のまま。
	obs.versions.bundled_vencord_tag = facts.bundled_vencord_tag;
	// 設定ウィンドウの「バージョン情報」のページに表示する、OBS と OS のバージョンと GPU の名前。読めていなければ
	// 空のまま渡し、"-" は ledger::decide_dock が入れる (observation.h の Versions)。
	obs.versions.obs = facts.obs_version;
	obs.versions.windows = facts.os_version;
	obs.versions.gpu = facts.gpu_name;

	// アンインストールの状態。DockPoll が作ったものを、そのままコピーする。
	obs.uninstall = facts.uninstall;

	// OBS 自身の配信と録画の状態 (obs_outputs)。値を持つのは uninstall の 2 つのフィールドで、ここではそれを
	// コピーして、2 か所の値を同じにする。
	obs.obs_outputs.streaming = facts.uninstall.obs_streaming;
	obs.obs_outputs.recording = facts.uninstall.obs_recording;

	// 参加者ごとの届くまでの遅れ (状態タブが見えている間だけ、snapshot に入っている)。
	obs.receive.arrival.reserve(snapshot.arrival_by_user.size());
	for (const auto &[user_id, streams] : snapshot.arrival_by_user)
		obs.receive.arrival.push_back(ledger::ArrivalFact{user_id, streams});

	return obs;
}

} // namespace cordmux::dock

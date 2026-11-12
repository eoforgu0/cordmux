// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、診断情報のエクスポート。宣言は dock_poll.h と dock_poll_export.h にある。

#include "dock/dock_poll.h"

#include <obs-module.h>

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QMetaObject>
#include <QtGlobal>

#include <memory>
#include <set>
#include <string>
#include <utility>

#include "bundle/export_bundle.h"
#include "diag/diag_log.h"
#include "dock/dock_poll_internal.h"
#include "dock/settings_window.h"
#include "dock/ui/message_dialog.h"
#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"
#include "platform/bundle_check.h"
#include "platform/platform.h"

namespace cordmux::dock {

using detail::wall_now_us;

namespace {

// 診断情報の manifest.json に書く値。画面に表示しないので ini を使わない。manifest.json は人とツールが読むデータの
// ファイルで、キーも値も英語にする。
const char *open_outcome_name(ws::OpenOutcome outcome)
{
	switch (outcome) {
	case ws::OpenOutcome::Opened:
		return "opened";
	case ws::OpenOutcome::AlreadyOpenedBefore:
		return "already_opened_before";
	case ws::OpenOutcome::BlockedByOtherWriter:
		return "blocked_by_other_writer";
	case ws::OpenOutcome::PortsExhausted:
		return "ports_exhausted";
	}
	return "unknown";
}

// 待ち受けを始めたときに起きた問題。値は open_outcome_name と同じく英語の snake_case にする (画面には表示しない)。
// 待ち受けを始めようとしていないときと、問題がなかったときは "none" で、listener_attempted と outcome と合わせて読む。
const char *open_problem_name(ws::OpenProblem problem)
{
	switch (problem) {
	case ws::OpenProblem::None:
		return "none";
	case ws::OpenProblem::SessionFileNotSaved:
		return "session_file_not_saved";
	case ws::OpenProblem::AcceptFailed:
		return "accept_failed";
	}
	return "unknown";
}

const char *disconnect_reason_name(ws::DisconnectReason reason)
{
	switch (reason) {
	case ws::DisconnectReason::SecretMismatch:
		return "secret_mismatch";
	case ws::DisconnectReason::WireMismatch:
		return "wire_mismatch";
	case ws::DisconnectReason::MalformedHello:
		return "malformed_hello";
	case ws::DisconnectReason::HelloTimeout:
		return "hello_timeout";
	case ws::DisconnectReason::HeartbeatTimeout:
		return "heartbeat_timeout";
	case ws::DisconnectReason::PeerClosed:
		return "peer_closed";
	case ws::DisconnectReason::LocalClose:
		return "local_close";
	case ws::DisconnectReason::Unknown:
		break;
	}
	return "unknown";
}

const char *bundle_state_name(platform::BundleState state)
{
	switch (state) {
	case platform::BundleState::Matches:
		return "matches";
	case platform::BundleState::Mismatch:
		return "mismatch";
	case platform::BundleState::Unknown:
		break;
	}
	return "unknown";
}

} // namespace

void DockPoll::export_bundle_now(const pipeline::PipelineSnapshot &snapshot, const platform::OsProbeResult &probe)
{
	// 診断情報に入れるデータを集め、ワーカースレッドで zip の作成を始める (流れは dock_poll_export.h の
	// DockPollExportPart)。呼び出すのは DockPollExportPart::begin で、ダイアログの親を保存してから呼ぶ。
	// 処理中なら何もしない。
	if (export_state_.phase != DockPollExportState::Phase::Idle)
		return;
	const QDateTime now = QDateTime::currentDateTime();

	bundle::ExportRequest request;
	bundle::ManifestInput &manifest = request.materials.manifest;
	manifest.taken_at_us = wall_now_us();
	manifest.utc_offset_sec = now.offsetFromUtc();

	// バージョンの一覧。読めなかったものは空のまま入れ、推測した値で埋めない。Windows のバージョン、GPU の名前と
	// ドライバーのバージョン、FFmpeg のバージョンは入れていない。
	manifest.versions.push_back({"plugin", last_observation_.versions.plugin});
	manifest.versions.push_back({"discord_side", last_observation_.versions.discord_side});
	manifest.versions.push_back({"vesktop", last_observation_.versions.vesktop});
	manifest.versions.push_back({"bundled_vencord", last_observation_.versions.bundled_vencord});
	// 同梱の Vencord のコミット (cordmux-build.txt の vencord_commit)。
	manifest.versions.push_back({"bundled_vencord_commit", last_observation_.versions.bundled_vencord_commit});
	// 同梱の Vencord のバージョンのタグ (cordmux-build.txt の vencord_tag)。設定ウィンドウの「バージョン情報」の
	// ページに表示する値と同じ。
	manifest.versions.push_back({"bundled_vencord_tag", last_observation_.versions.bundled_vencord_tag});
	const char *obs_version = obs_get_version_string();
	manifest.versions.push_back({"obs", obs_version != nullptr ? obs_version : ""});
	manifest.versions.push_back({"qt_runtime", qVersion()});

	// Discord の内部への依存箇所ごとの結果と、最後に読めた時刻。使えないかの判断は、update_contact_success と同じ
	// unavailable_contact_ids から取る。別々に判断すると、片方だけを修正したときに state と last_ok_at_us が
	// 食い違う。
	const std::set<std::string> unavailable = unavailable_contact_ids(last_observation_.contacts);
	for (const auto &entry : contact_last_ok_) {
		bundle::ManifestContact row;
		row.id = entry.first;
		row.state = unavailable.count(entry.first) > 0 ? "unavailable" : "ok";
		row.last_ok_at_us = entry.second;
		manifest.contacts.push_back(std::move(row));
	}

	// ダミーを表示していた時間帯。実際の通話の映像と区別できるように入れる。
	manifest.dummy_intervals = snapshot.dummy_intervals;

	// cordmux.dll が自分だけで取得できる情報。Vesktop と接続できていないときに、診断に使える情報はこれだけになる。
	bundle::ManifestSelfFacts &facts = manifest.facts;
	facts.listener_attempted = snapshot.listener.open.attempted;
	facts.listener_outcome = open_outcome_name(snapshot.listener.open.outcome);
	// 待ち受けを始めたときに起きた問題も入れる。outcome だけでは、トークンファイル (session.json) を保存できず、
	// Vesktop が接続できないときも、待ち受け中と同じに見える。この問題はログにも書くが、OBS のログの行は
	// zip に入らず、plugin.log のリングバッファーでは起動直後の行が先に押し出されるので、ログだけでは読めないことが
	// ある。画面の表示 (build_observation.cpp の ListenerFacts) と同じ OpenSummary::problem から取る。
	facts.listener_open_problem = open_problem_name(snapshot.listener.open.problem);
	facts.listening = snapshot.listener.listening;
	facts.listening_port = static_cast<int>(snapshot.listener.open.port);
	facts.connection_opened_at_us = snapshot.listener.connection_opened_at_us;
	facts.hello_ok_at_us = snapshot.listener.hello_ok_at_us;
	facts.connection_closed_at_us = snapshot.listener.connection_closed_at_us;
	facts.last_disconnect_reason = disconnect_reason_name(snapshot.listener.last_disconnect_reason);
	facts.connections_accepted = snapshot.listener.connections_accepted;
	facts.secret_mismatches = snapshot.listener.hello_secret_mismatches;
	facts.handshake_rejections = snapshot.listener.handshake_rejections;
	facts.heartbeat_timeouts = snapshot.listener.heartbeat_timeouts;
	facts.hello_timeouts = snapshot.listener.hello_timeouts;
	facts.rejected_clients = snapshot.listener.rejected_clients;
	facts.state_json_written_at_us = probe.state_json_written_at_us.value_or(0);
	facts.bundle_hash_state = bundle_state_name(probe.bundle_hash.state);
	facts.bundle_hash_checked_at_us = probe.probed ? probe.probed_at_us : 0;
	facts.bundle_hash_missing = probe.bundle_hash.missing;
	facts.bundle_hash_differing = probe.bundle_hash.differing;
	facts.vesktop_running = probe.vesktop_process;
	facts.vesktop_started_at_us = probe.vesktop_started_at_us;
	facts.plugin_log_dropped = snapshot.plugin_log_dropped;
	facts.plugin_stats_log_dropped = snapshot.plugin_stats_log_dropped;
	facts.discord_log_dropped = snapshot.js_log_dropped;
	facts.discord_stats_log_dropped = snapshot.js_stats_log_dropped;
	facts.exceptions_caught = snapshot.exceptions_caught;
	facts.facts_unreadable_keys = snapshot.facts.unreadable_keys;
	facts.avatar_decode_failures = snapshot.profiles.avatar_decode_failures;
	facts.hw_fallback_streams = snapshot.hw_fallback_streams;

	// 4 つのログ (出来事の行と統計の行を、cordmux.dll と送信側のそれぞれで) と設定。設定は、DockPoll が持っている現在の値を入れる。settings.json のファイルより
	// 新しいことがあるためである。
	request.materials.plugin_log = snapshot.plugin_log;
	request.materials.plugin_log_dropped = snapshot.plugin_log_dropped;
	request.materials.discord_log = snapshot.js_log;
	request.materials.discord_log_dropped = snapshot.js_log_dropped;
	request.materials.plugin_stats_log = snapshot.plugin_stats_log;
	request.materials.plugin_stats_log_dropped = snapshot.plugin_stats_log_dropped;
	request.materials.discord_stats_log = snapshot.js_stats_log;
	request.materials.discord_stats_log_dropped = snapshot.js_stats_log_dropped;
	request.materials.settings_json = config::settings_to_json(settings_);
	request.materials.user_profile_dir = platform::user_profile_dir();

	// ファイル名は cordmux-diagnostics-<YYYYMMDD-HHMMSS>.zip にする。開発者が読むファイルなので、言語によって名前を
	// 変えず、英字だけにする。
	request.suggested_name = "cordmux-diagnostics-" + now.toString("yyyyMMdd-HHmmss").toStdString() + ".zip";
	request.stamp.year = now.date().year();
	request.stamp.month = now.date().month();
	request.stamp.day = now.date().day();
	request.stamp.hour = now.time().hour();
	request.stamp.minute = now.time().minute();
	request.stamp.second = now.time().second();

	// トークンを消す関数と、残っていないかを確認する関数は、plugin-main.cpp から渡された関数である (トークンは
	// DockPoll にはない)。どちらも自分でロックを取るので、ワーカースレッドから呼んでよい。
	request.redact = cfg_.redact;
	request.reveals = cfg_.reveals_secret;

	// トークンを消す処理、残っていないかの確認、zip の作成は、UI スレッドを止めないようにワーカースレッドで行う。
	// 結果は receiver に QueuedConnection で返す。
	DockPollExportState &state = export_state_;
	if (state.worker.joinable())
		state.worker.join(); // 前のスレッドは終わっている (Idle に戻るのは結果を受け取った後)
	state.zip_name = request.suggested_name;
	state.phase = DockPollExportState::Phase::Building;
	auto shared_request = std::make_shared<bundle::ExportRequest>(std::move(request));
	QObject *receiver = state.receiver.get();
	DockPoll *self = this;
	state.worker = std::thread([shared_request, receiver, self]() {
		auto prepared = std::make_shared<bundle::PreparedBundle>(bundle::prepare_bundle(*shared_request));
		QMetaObject::invokeMethod(
			receiver, [self, prepared]() { DockPollExportPart::on_prepared(*self, *prepared); },
			Qt::QueuedConnection);
	});
}

// ---- DockPollExportState と DockPollExportPart の実装 (dock_poll_export.h) ----

DockPollExportState::DockPollExportState() : receiver(std::make_unique<QObject>()) {}

DockPollExportState::~DockPollExportState()
{
	// 先にワーカースレッドを終わらせる。トークンを消す関数は plugin-main.cpp の関数なので、DLL がアンロードされた
	// 後に実行されてはならない。その後で receiver を破棄するので、まだ実行されていない呼び出しは receiver と一緒に
	// 捨てられる。
	if (worker.joinable())
		worker.join();
}

namespace {

// エラーのダイアログと保存のダイアログの親は、エクスポートを始めた設定ウィンドウにする。閉じていればドックにする。
QWidget *dialog_owner(DockPollExportState &state, QWidget *fallback)
{
	return state.owner ? state.owner.data() : fallback;
}

// エラーのダイアログを表示する。設定ウィンドウから始めて、そのウィンドウがまだ開いていれば、操作を止めるのは
// 設定ウィンドウだけにする (OwnerOnly)。配信の間に、OBS のメインウィンドウとドックの操作を止めないためである。
// 設定ウィンドウを閉じた後でドックが親になったときは、ドックから開いたダイアログと同じく、親とその親を止める。
// 呼んだ後は、DockPoll が持つものだけを触る。OwnerOnly の間はメインウィンドウとドックを操作できるので、設定ウィンドウは
// 破棄されていることがある (run_message_dialog の約束)。
void show_export_failed(DockPollExportState &state, QWidget *fallback, bundle::ExportOutcome outcome)
{
	ui::MessageSpec spec = export_failed_spec(outcome);
	if (state.owner)
		spec.blocking = ui::MessageBlocking::OwnerOnly;
	ui::run_message_dialog(dialog_owner(state, fallback), spec);
}

void set_window_busy(QPointer<QWidget> &window, bool busy)
{
	if (auto *settings = static_cast<SettingsWindow *>(window.data()))
		settings->set_export_busy(busy);
}

} // namespace

bool DockPollExportPart::busy(const DockPoll &self)
{
	return self.export_state_.phase != DockPollExportState::Phase::Idle;
}

void DockPollExportPart::begin(DockPoll &self, QWidget *owner)
{
	DockPollExportState &state = self.export_state_;
	if (state.phase != DockPollExportState::Phase::Idle)
		return; // 処理中。ボタンは押せなくなっているが、ドックのメニューからもう一度呼ばれることがある
	state.owner = owner;
	self.export_bundle_now(self.pipeline_.snapshot(), self.probe_.latest());
}

void DockPollExportPart::on_prepared(DockPoll &self, const bundle::PreparedBundle &prepared)
{
	DockPollExportState &state = self.export_state_;
	QWidget *owner = dialog_owner(state, self.widget_.data());

	if (prepared.outcome != bundle::ExportOutcome::Written) {
		// トークンが残っていたか、zip を作れなかった。書き込まないことが決まっているので、保存のダイアログは
		// 開かない。理由はログにだけ書き、ダイアログには次の操作だけを表示する。トークンが残っていたことは、
		// 必ずログに 1 行書く。
		if (prepared.outcome == bundle::ExportOutcome::SecretFound)
			diag::logf(LOG_WARNING, "Didn't export diagnostics: %s", prepared.error.c_str());
		else {
			diag::logf(LOG_WARNING, "Couldn't export diagnostics: the file couldn't be built");
			// 理由の文字列 (bundle/zip_writer.h が日本語で作る) は LOG_DEBUG で書く。LOG_DEBUG の行は、
			// 診断情報の plugin.log には入るが、OBS のログには --verbose で起動したときだけ書かれる。
			diag::logf(LOG_DEBUG, "Diagnostics build error: %s", prepared.error.c_str());
		}
		state.phase = DockPollExportState::Phase::Idle;
		set_window_busy(self.settings_state_.window, false);
		show_export_failed(state, self.widget_.data(), prepared.outcome);
		return;
	}

	// トークンが残っていたので、説明の文に置き換えたファイル (bundle/export_bundle.h)。利用者にできることは
	// ないので、ダイアログは表示せず、ログにだけ書く。置き換えたことは、zip の中のそのファイルにも 1 行残る。
	for (const std::string &name : prepared.left_out)
		diag::logf(LOG_WARNING, "Diagnostics left out %s: a connection secret remained after redaction",
			   name.c_str());

	// 保存先を選んでもらう (親は設定ウィンドウ)。Windows の標準の保存のダイアログを、親を渡して開く。
	// OBS 自身の保存のダイアログも同じ静的関数を使い、設定ウィンドウの [参照...] とも同じにする。ダイアログを
	// 開いている間に何が操作できなくなるかは、settings_window.cpp の choose_record_dir にある。
	state.phase = DockPollExportState::Phase::Asking;
	const QString start = QDir(QString::fromStdString(bundle::default_export_directory()))
				      .filePath(QString::fromStdString(state.zip_name));
	const QString chosen = QFileDialog::getSaveFileName(owner, ui::resolve_key(ledger::text::keys::kDialogExportTitle),
							    start,
							    ui::resolve_key(ledger::text::keys::kDialogExportFilterLabel));
	if (chosen.isEmpty()) {
		// 利用者がキャンセルした。何も書き込んでいないので、何も表示しない。
		diag::logf(LOG_INFO, "Diagnostics export was canceled");
		state.phase = DockPollExportState::Phase::Idle;
		set_window_busy(self.settings_state_.window, false);
		return;
	}

	// 書き込みもワーカースレッドで行い、遅いドライブで UI スレッドを止めない。
	state.phase = DockPollExportState::Phase::Writing;
	if (state.worker.joinable())
		state.worker.join();
	auto zip = std::make_shared<std::string>(prepared.zip);
	const std::string path = QDir::toNativeSeparators(chosen).toStdString();
	QObject *receiver = state.receiver.get();
	DockPoll *self_ptr = &self;
	state.worker = std::thread([zip, path, receiver, self_ptr]() {
		auto result = std::make_shared<bundle::ExportResult>(bundle::write_bundle(path, *zip));
		QMetaObject::invokeMethod(
			receiver, [self_ptr, result]() { DockPollExportPart::on_written(*self_ptr, *result); },
			Qt::QueuedConnection);
	});
}

void DockPollExportPart::on_written(DockPoll &self, const bundle::ExportResult &result)
{
	DockPollExportState &state = self.export_state_;
	state.phase = DockPollExportState::Phase::Idle;
	set_window_busy(self.settings_state_.window, false);

	if (result.outcome != bundle::ExportOutcome::Written) {
		diag::logf(LOG_WARNING, "Couldn't export diagnostics: %s", result.error.c_str());
		show_export_failed(state, self.widget_.data(), result.outcome);
		return;
	}

	// 書き込めた。結果は、設定ウィンドウを閉じるまでボタンの下に表示する。保存した場所は画面にコピーできる形で
	// 表示し、ログには書かない。ログに書くと、利用者の PC のフォルダーの名前が、次に渡す診断情報の zip にも
	// 入るためである。
	diag::logf(LOG_INFO, "Diagnostics exported");
	ledger::ExportResultFact fact;
	fact.kind = ledger::ExportResultKind::Written;
	fact.path = result.path;
	self.ops_.export_result = fact;
	// すぐに tick() を 1 回呼び、ポーリングの間隔を長くしていても、結果がすぐに表示されるようにする。呼ぶのは
	// ポーリングを実行している間だけで、OBS を閉じる途中の DockPoll::stop() の後は呼ばない。
	if (self.timer_)
		self.tick();
}

} // namespace cordmux::dock

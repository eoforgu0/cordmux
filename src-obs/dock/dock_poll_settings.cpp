// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、設定 (settings.json) とこれまでに見た人の一覧 (seen_people.json) の読み書きと、
// 設定ウィンドウと参加者の設定ウィンドウを扱うもの。宣言は dock_poll.h と dock_poll_settings.h にある。

#include "dock/dock_poll.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QTimer>

#include <set>
#include <string>
#include <utility>

#include "config/seen_people.h"
#include "diag/diag_log.h"
#include "dock/dock_poll_internal.h"
#include "dock/participants_window.h"
#include "dock/settings_fields.h"
#include "dock/settings_window.h"
#include "platform/platform.h"
#include "sources/self_frame_source.h"
#include "sources/self_speaking_source.h"

namespace cordmux::dock {

using detail::wall_now_us;

void DockPoll::on_settings_changed(const config::Settings &settings)
{
	// アンインストールを始めた後は、設定を書かない。消した設定フォルダーを、書き込みで作り直さないためである。
	// 設定ウィンドウでも設定を変えられなくしてある (ledger::dockview::AboutPageView::settings_locked) ので、
	// ここに来るのは、それが画面に反映されるまでの間だけである。
	if (writes_stopped()) {
		diag::logf(LOG_INFO, "Ignored a settings change: cordmux has been uninstalled");
		return;
	}

	// 設定ウィンドウの項目だけをコピーする。どの項目をコピーするかは dock/settings_fields.h の表が決める。表に
	// settings のすべてのキーがあるかは、tests/test_dock_settings_fields.cpp で確認する。オンとオフを
	// 切り替える 2 つ (ダミー、cordmux の有効と無効) は ActionId で変えるので、表でもコピーしない
	// 側にある。
	//
	// 載せる人の選び方を「自動」から「手動」に戻しても、今枠に載っている人はそのまま残す。「自動」の間もドックは
	// 枠に載っている人の行にチェックを付けて表示しているので、「手動」の「チェックを付けた人を載せる」をそのまま
	// 満たす。「手動」の割り当ては枠に載っている人のストリームを保つので、ここで枠を空けなければ誰も消えず、
	// 次に画面共有を始めた人は載らない。ここでは何もしない。選んでいない人の枠をここで空けると、[適用] を
	// 押した瞬間に配信から人が消えるので、空けない。

	// OBS を再起動しなくても反映される項目 (画質の上限と、送信側のエンコードの設定) が変わったかを、コピーする前に
	// 調べておく。
	const bool limits_changed = settings_.limit_max_long_edge != settings.limit_max_long_edge ||
				    settings_.limit_fps_divisor != settings.limit_fps_divisor ||
				    settings_.encoder_preference != settings.encoder_preference ||
				    settings_.encode_priority != settings.encode_priority;

	const bool was_selected = self_frame_selected();
	const bool assign_mode_changed = settings_.assign_mode != settings.assign_mode;

	apply_settings_page_fields(settings, settings_);

	// 自分の枠に映す映像を「Discord の画面共有」と OBS のソースかシーンの間で替えたら、メインアカウントのチェックを
	// 引き継ぐ。載せる人の選び方を替えたら、外した人を忘れる (ほかの人の StreamRouter と同じ)。
	if (was_selected != self_frame_selected())
		carry_self_frame_check(self_frame_selected());
	if (assign_mode_changed)
		self_frame_state_.excluded = false;

	write_settings();
	push_assign_policy();

	// 画質の上限とエンコードの設定は、OBS を再起動しなくても反映される (notify_runtime_settings から呼ばれる
	// Pipeline::set_limits が、送信側に limits を送り直す)。
	if (limits_changed)
		notify_runtime_settings();

	// すぐに反映できる設定は、ここで反映する。デコードの種類 (decoder) と、音声のずれの補正の 2 つ
	// (screen_audio_offset_ms と voice_audio_delay_ms) は、受信側が起動時にコピーした設定で動いているので、ここでは
	// 反映されない。設定ウィンドウは、画面の音の補正の値を変えた時点で、再起動が必要なことを表示する
	// (声の遅延は設定のファイルでだけ変える)。
	if (timer_)
		timer_->setInterval(settings_.dock_poll_ms > 0 ? settings_.dock_poll_ms : 200);

	// 録画の設定 (保存先、音声の形式、残り時間の警告) は、次に録画を始めたときから反映される。保存先は録画を始めた
	// 時点で決めるので、録画ファイルの作成を待っている前の録画には影響しない。再起動が必要ないことは、
	// dock/settings_fields.cpp の表の restart_required で決めている。
	pipeline_.update_record_settings(settings_);

	// 名前ラベルの見た目と、接続が切れたときに最後のフレームを表示し続ける時間などを、sources と Pipeline に
	// 渡し直す (DockPollLayoutPart::push_runtime_values)。
	DockPollLayoutPart::on_settings_changed(*this);
}

void DockPoll::on_look_changed(const config::Settings &from_tab)
{
	// アンインストールを始めた後は、設定を書かない (on_settings_changed と同じ)。
	if (writes_stopped()) {
		diag::logf(LOG_INFO, "Ignored a settings change: cordmux has been uninstalled");
		return;
	}

	// 表示タブの項目だけをコピーする。範囲は clamp_settings で収めてから、表示タブの項目だけを settings_ に戻す。設定ウィンドウの
	// 値と、ActionId を通る値は、ここでは書き換えない。
	config::Settings next = settings_;
	apply_look_tab_fields(from_tab, next);
	config::clamp_settings(next);

	const bool was_selected = self_frame_selected();
	const bool assign_mode_changed = settings_.assign_mode != next.assign_mode;
	apply_look_tab_fields(next, settings_);

	// 自分の枠に映す映像を替えたら、メインアカウントのチェックを引き継ぐ。載せる人の選び方を替えたら、外した人を忘れる
	// (on_settings_changed と同じ)。
	if (was_selected != self_frame_selected())
		carry_self_frame_check(self_frame_selected());
	if (assign_mode_changed)
		self_frame_state_.excluded = false;

	push_assign_policy();
	// 名前ラベルの見た目と、枠の並べ方 (間隔、並び順) を、次の周期を待たずに渡す。
	DockPollLayoutPart::on_settings_changed(*this);
	push_layout_now(pipeline_.snapshot(), self_frame_shown_now());

	if (look_save_)
		look_save_->touch();
	else
		write_settings();
}

void DockPoll::on_buffer_changed(int ms)
{
	// 映像のバッファだけを変える。表示タブの値は現在の設定のまま。
	config::Settings from = settings_;
	from.display_buffer_ms = ms;
	on_look_changed(from);
}

void DockPoll::write_settings()
{
	// アンインストールを始めた後は、OBS を閉じるまで書かない。消した設定フォルダーを、
	// 書き込みで作り直さないためである。write_seen_people も同じ。
	if (writes_stopped())
		return;
	if (cfg_.settings_json_path.empty()) {
		diag::logf(LOG_WARNING, "Couldn't save settings: the cordmux settings folder is unknown");
		// 保存できていない変更がある。メッセージのレベル 2 と、設定ウィンドウのエラーのダイアログで表示する。
		ops_.settings_unsaved = true;
		return;
	}
	// 読めなかった settings.json には一切書き込まない (load_seen_people と同じ扱い)。DockPoll が持っているのは
	// 既定値なので、書き込むと、利用者が書いた値がすべて既定値に置き換わる。config/settings.h は、形式の違う値が
	// 1 つあるとすべてのキーを既定値に戻すので、書き間違い 1 つで設定がすべて消えることになる。
	//
	// ここではログに書かない。この関数は、利用者が設定を 1 つ変えるたびにも、枠を手で動かすたびにも呼ばれる。
	// 同じ行をリングバッファー (200 行) に書き続けると、問題が起きたときに読みたいほかの行が押し出される。
	// ログは起動時に 1 回だけ書いてある (コンストラクター)。
	//
	// 読めなかった起動では、ops_.settings_unsaved を true にしない。起動時から、メッセージのレベル 2
	// (SettingsUnreadAtStartup) が、この起動で変えた値は保存されないことを表示している。true にすると、
	// 同じことをメッセージとエラーのダイアログで 2 回表示し、読めないファイルには適用されない「もう一度お試しください」を
	// 勧めることになる。
	if (cfg_.settings_file_unreadable)
		return;
	const std::string text = config::settings_to_json(settings_);
	if (text == settings_written_json_) {
		ops_.settings_unsaved = false; // 書き込む差分がないので、保存できていない変更はない
		return; // 中身が変わったときだけ書く
	}
	if (!platform::write_file_bytes(cfg_.settings_json_path, text)) {
		diag::logf(LOG_WARNING,
			   "Couldn't save settings to settings.json; the previous values return next start");
		// メッセージのレベル 2 で表示する。設定ウィンドウから変えたときは、エラーのダイアログも表示する。
		ops_.settings_unsaved = true;
		return;
	}
	settings_written_json_ = text;
	ops_.settings_unsaved = false;
}

void DockPoll::load_seen_people()
{
	if (cfg_.seen_people_json_path.empty())
		return;

	std::string text;
	if (!platform::read_file_bytes(cfg_.seen_people_json_path, text, cfg_.max_json_bytes)) {
		// 初めての起動では、まだファイルがないのが普通で、エラーではない。
		return;
	}

	std::string error;
	if (!config::seen_people_from_json(text, &seen_people_, &error)) {
		// 読めなかったファイルには一切書き込まない。空の一覧で上書きすると、利用者が設定した「自動で載せる」、
		// 名前の変更、「使う声」がその場で消える。
		seen_people_.clear();
		seen_file_unreadable_ = true;
		diag::logf(LOG_WARNING,
			   "Couldn't read seen_people.json, so Participant Settings aren't saved this session");
		// 読めなかった理由 (config/seen_people.h が作る日本語の文字列) は LOG_DEBUG で書く。LOG_DEBUG の行は、
		// 診断情報の plugin.log には入るが、OBS のログには --verbose で起動したときだけ書かれる。
		diag::logf(LOG_DEBUG, "Read error in seen_people.json: %s", error.c_str());
		return;
	}
	seen_written_json_ = text;
}

void DockPoll::write_seen_people()
{
	if (cfg_.seen_people_json_path.empty() || seen_file_unreadable_)
		return;
	// アンインストールを始めた後は、OBS を閉じるまで書かない。終了時の stop() もここを通るので、消した
	// seen_people.json を、OBS を閉じるときに作り直さないようにする。
	if (writes_stopped())
		return;

	// 今見えている人の最後に見た時刻を今にしてから、一覧の上限を適用する。入った時刻のままにすると、通話に長くいる
	// 人の行が、さっき抜けた人の行より古くなる。上限ちょうどの一覧では、その人の行だけが最も古い行として削除され、
	// 次のポーリングで追加されてまた削除される、を繰り返す。
	//
	// 時刻を更新するのは書き込むときだけなので、周期ごとの書き込みは起きない。update_seen_people が時刻を
	// 更新するのは入ったときと抜けたときだけで、この関数は中身が変わったときにしか呼ばれない。
	config::touch_seen_people(seen_people_, last_in_call_, wall_now_us());

	// 上限を適用してから書く。参加者の設定 (「自動で載せる」、名前、「使う声」など) がある行を削除したときは、
	// その数を画面に表示する。
	const config::SeenTrimResult trim = config::trim_seen_people(&seen_people_, settings_.seen_people_max);
	if (trim.discarded_with_marks > 0) {
		seen_discarded_with_marks_ += trim.discarded_with_marks;
		diag::logf(LOG_INFO, "Participant list is full; removed %d people who had Participant Settings",
		           trim.discarded_with_marks);
		// 削除した行の設定を、Pipeline が持つコピーからも消す。「使う声」を設定した行を削除しても Pipeline の
		// コピーが古いままだと、その人の声は "voice_replaced" のまま出力されない。画面には「使う声」の表示も、
		// 元に戻す操作もないので、利用者が元に戻せなくなる。「自動で載せる」も同じコピーに入っているので、
		// 一緒に正しくなる。
		// Pipeline に渡すのは、ポーリングを実行している間 (timer_ がある間) だけにする。stop() は timer_ を
		// 外してからここを通るので、終了の途中の Pipeline に configure を適用し直させない。起動時は、
		// load_seen_people の直後に start() が push_assign_policy を呼ぶので、ここで渡さなくても漏れない。
		// 音量も同じで、捨てた行の人の音量は 100% に戻す。
		if (timer_) {
			push_assign_policy();
			push_person_volumes();
		}
	}

	const std::string text = config::seen_people_to_json(seen_people_);
	if (text == seen_written_json_) {
		ops_.seen_unsaved = false;
		return; // 中身が変わったときだけ書く
	}
	if (!platform::write_file_bytes(cfg_.seen_people_json_path, text)) {
		diag::logf(LOG_WARNING, "Couldn't save Participant Settings to seen_people.json");
		// 参加者の設定はすぐに反映されるので、保存できないことはメッセージのレベル 2 で表示する。
		ops_.seen_unsaved = true;
		return;
	}
	seen_written_json_ = text;
	ops_.seen_unsaved = false;
}

void DockPoll::update_seen_people(const pipeline::PipelineSnapshot &snapshot, std::int64_t now_us)
{
	// 今見えている人は、通話に参加している人 (roster) と、枠に載っている人である。roster を読めないときも枠の人は
	// 見えているので、両方から集める。roster を読めないことを、誰もいないことと扱わないためである。
	std::set<std::string> in_call;
	if (snapshot.facts.roster.has_value()) {
		for (const std::string &user_id : snapshot.facts.roster->user_ids) {
			if (!user_id.empty())
				in_call.insert(user_id);
		}
	}
	for (const pipeline::SlotSnapshot &row : snapshot.slots) {
		if (!row.owner_user_id.empty())
			in_call.insert(row.owner_user_id);
	}

	bool changed = false;

	// 時刻を更新するのは、入ったときと抜けたときだけにする。ポーリングのたびに全員の時刻を更新すると、
	// 中身が変わったときだけ書く処理が、周期ごとの書き込みになる。一覧に表示するのは通話に参加していない
	// 人なので、意味があるのは抜けた時刻である。
	for (const std::string &user_id : in_call) {
		const bool was_here = last_in_call_.count(user_id) > 0;
		ledger::SeenPerson &person = config::ensure_seen_person(seen_people_, user_id);
		if (!was_here) {
			person.last_seen_at_us = now_us;
			call_joined_at_us_[user_id] = now_us; // 接続した時点で既にいた人も、最初に見えた時刻になる
			changed = true;
		}
		// 表示名は変わることがあるので、届いている間はコピーを更新する。利用者が変えた名前 (name_override) は
		// 別のフィールドに持ち、ここでは書き換えない。別に持つので、利用者は変えた名前を元に戻せる。
		const std::optional<std::string> name = snapshot.profiles.display_name_of(user_id);
		if (name.has_value() && !name->empty() && person.display_name != *name) {
			person.display_name = *name;
			changed = true;
		}
	}

	for (const std::string &user_id : last_in_call_) {
		if (in_call.count(user_id) > 0)
			continue;
		config::ensure_seen_person(seen_people_, user_id).last_seen_at_us = now_us;
		call_joined_at_us_.erase(user_id);
		changed = true;
	}

	last_in_call_ = std::move(in_call);
	if (changed)
		write_seen_people();
}


// ---- DockPollSettingsPart の実装 (dock_poll_settings.h): 設定ウィンドウと参加者の設定ウィンドウ ----

namespace {

// ウィンドウの親は OBS のメインウィンドウにする。初めて開くときにメインウィンドウの中央に表示され、フローティングの
// ドックやメインウィンドウの後ろに隠れない。
QWidget *obs_main_window()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

SettingsWindow *settings_window_of(DockPollSettingsState &state)
{
	return static_cast<SettingsWindow *>(state.window.data());
}

ParticipantsWindow *participants_window_of(DockPollSettingsState &state)
{
	return static_cast<ParticipantsWindow *>(state.participants_window.data());
}

void bring_to_front(QWidget *window)
{
	window->show();
	if (window->isMinimized())
		window->showNormal();
	window->raise();
	window->activateWindow();
}

} // namespace

DockPollSettingsState::~DockPollSettingsState()
{
	delete window.data();
	delete participants_window.data();
}

void DockPollSettingsPart::open_settings(DockPoll &self, ledger::SettingsPage page, bool start_export)
{
	DockPollSettingsState &state = self.settings_state_;
	// ウィンドウを開いたウィジェット (ドックのメニューの [...]、メッセージのレベル 1 のボタン)。ウィンドウがフォーカスを持ったまま閉じたら、フォーカスをここに戻す。メニューから選んだときも
	// メニューは閉じた後なので、フォーカスはメニューを開いたウィジェットにある (確認のダイアログの
	// run_message_dialog がフォーカスを覚えるのと同じタイミング)。
	QWidget *opener = QApplication::focusWidget();
	SettingsWindow *window = settings_window_of(state);
	if (window == nullptr) {
		SettingsWindowInput in;
		in.current = self.settings_;
		in.effective_at_start = state.effective_at_start;
		in.page = page;
		if (state.last_about.has_value())
			in.about = *state.last_about;
		// 設定ウィンドウの [録画] のページで、編集中の保存先から、今録画を始めたら書き込む場所を表示するために
		// 使う (Pipeline::preview_record_base_dir。問い合わせの時間には上限がある)。
		in.record_dir_now = [&self](const std::string &record_dir) {
			return self.pipeline_.preview_record_base_dir(record_dir);
		};
		in.on_apply = [&self](const config::Settings &settings) {
			return DockPollSettingsPart::apply_from_window(self, settings);
		};
		in.on_export = [&self](QWidget *owner) { DockPollExportPart::begin(self, owner); };
		// エクスポートの結果の zip の行の [ファイルの場所を開く]。開けなかったときに、ほかのメッセージを重ねて
		// 表示しないので、戻り値は見ない (platform.h の open_file_location の約束)。
		in.reveal_file = [](const std::string &path) { platform::open_file_location(path); };
		in.on_uninstall = [&self](QWidget *owner) {
			// 確認と結果のダイアログの親は、このウィンドウにする。アンインストールが終わると押したボタンは
			// 押せなくなるので、ダイアログを閉じた後のフォーカスは、「バージョン情報」のページの最初の
			// 項目に移す。
			auto *settings = static_cast<SettingsWindow *>(owner);
			DockPollInstallPart::uninstall(self, owner, settings != nullptr ? settings->about_focus_fallback() : nullptr);
		};
		in.on_closed = [&self](const std::string &geometry) {
			// ウィンドウの大きさと位置を、設定に保存する (設定ウィンドウには表示しないキー)。
			self.settings_.settings_window_geometry = geometry;
			self.write_settings();
			// エクスポートの結果は、設定ウィンドウを閉じるまで表示する。
			self.ops_.export_result.reset();
			// 閉じたウィンドウへのポインターを外す。ウィンドウが破棄されるまでの間に、refresh_windows が
			// 閉じたウィンドウを書き換えないようにする。
			self.settings_state_.window.clear();
			DockPollSettingsPart::on_window_closed(self);
		};
		in.geometry = self.settings_.settings_window_geometry;
		in.export_busy = DockPollExportPart::busy(self);
		window = make_settings_window(obs_main_window(), in);
		window->setAttribute(Qt::WA_DeleteOnClose, true);
		state.window = window;
	} else {
		window->set_page(page);
	}
	window->set_return_focus(opener);
	bring_to_front(window);
	if (start_export)
		window->start_export();
}

void DockPollSettingsPart::open_settings_to_uninstall(DockPoll &self)
{
	open_settings(self, ledger::SettingsPage::About, false);
	// イベントループの次の周回で始める。[ツール] メニューの項目の呼び出しを先に返し、ウィンドウを表示して
	// レイアウトが決まった後に、スクロールの位置を決めて、確認のダイアログを設定ウィンドウの上に開く。ここで直接
	// start_uninstall を呼ぶと、メニューの呼び出しの中で確認のダイアログのイベントループに入り、初めて開いた
	// ウィンドウはレイアウトの前なので、スクロールの範囲が決まっていない。
	// 待つ間にウィンドウが閉じられたら何もしない。state.window は設定ウィンドウだけを指す。
	const QPointer<QWidget> window = self.settings_state_.window;
	if (window.isNull())
		return;
	QTimer::singleShot(0, window.data(), [window]() {
		if (!window.isNull())
			static_cast<SettingsWindow *>(window.data())->start_uninstall();
	});
}

void DockPollSettingsPart::open_participants(DockPoll &self)
{
	DockPollSettingsState &state = self.settings_state_;
	QWidget *opener = QApplication::focusWidget(); // 閉じた後のフォーカスの戻し先 (open_settings と同じ)
	ParticipantsWindow *window = participants_window_of(state);
	if (window == nullptr) {
		// ウィンドウの操作は、ドックの行のメニューと同じ ActionId を使い、押せるかと何をするかは
		// ledger::apply が決める。押した後はすぐに tick() を 1 回呼び、次のポーリングを待たずに、ウィンドウと
		// ドックの両方に変更後の値を表示する。
		window = make_participants_window(obs_main_window(), state.last_participants,
						  [&self](ledger::ActionId id, const ledger::ActionArgs &args) {
							  self.run_action(id, args);
							  if (self.timer_)
								  self.tick();
						  });
		window->restore_window_geometry(self.settings_.participants_window_geometry);
		window->set_on_closed([&self](const std::string &geometry) {
			self.settings_.participants_window_geometry = geometry;
			self.write_settings();
			self.settings_state_.participants_window.clear();
			DockPollSettingsPart::on_window_closed(self);
		});
		window->setAttribute(Qt::WA_DeleteOnClose, true);
		state.participants_window = window;
	}
	window->set_return_focus(opener);
	bring_to_front(window);
}

bool DockPollSettingsPart::apply_from_window(DockPoll &self, const config::Settings &settings)
{
	// ウィンドウから来る値を、最後に範囲に収める。ウィンドウの入力欄も範囲を制限しているので、
	// 普通は何も変わらない。
	config::Settings next = settings;
	config::clamp_settings(next);
	self.on_settings_changed(next);
	// アンインストールを始めた後は、ウィンドウの値を何も反映しない。on_settings_changed は自分で値を捨ててログに
	// 1 行書く。設定ウィンドウの [OK] も、アンインストールの後は反映せずに閉じる (settings_window.cpp) が、ウィンドウが
	// それを知るのは次のポーリングの描き直しなので、その間に押された [OK] と [適用] をここで止める。
	// 書き込まないのは決めたとおりの動作で、保存の失敗ではないので true を返す。false を返すと、アンインストールの
	// 後に「設定を保存できませんでした」のダイアログが表示され、ウィンドウも閉じない。
	if (self.writes_stopped())
		return true;
	// 保存できたかは、write_settings が ops_ に書いた値で判断する。すぐに tick() を 1 回呼び、メッセージのレベル 2 も
	// すぐに表示する。呼ぶのはポーリングを実行している間だけで、OBS を閉じる途中の DockPoll::stop() の後は
	// 呼ばない。
	if (self.timer_)
		self.tick();
	return !self.ops_.settings_unsaved;
}

void DockPollSettingsPart::on_theme_changed(DockPoll &self)
{
	// OBS のテーマ、[フォントサイズ]、[密度] が変わった (OBS_FRONTEND_EVENT_THEME_CHANGED)。開いているウィンドウの
	// アイコンとフォントサイズを読み直す。
	if (SettingsWindow *window = settings_window_of(self.settings_state_))
		window->refresh_theme();
	if (ParticipantsWindow *window = participants_window_of(self.settings_state_))
		window->refresh_theme();
}

void DockPollSettingsPart::on_window_closed(DockPoll &self)
{
	// どちらかのウィンドウが閉じた。ポインターを外すことと、閉じた後の処理 (大きさと位置の保存、エクスポートの
	// 結果の削除) は、ウィンドウごとの on_closed が先に済ませている。ここでは、エクスポートの結果を消したことを次の
	// ポーリングを待たずに反映するため、すぐに tick() を 1 回呼ぶ。呼ぶのはポーリングを実行している間だけで、
	// OBS を閉じる途中の DockPoll::stop() の後は呼ばない。
	if (self.timer_)
		self.tick();
}

void DockPollSettingsPart::refresh_windows(DockPoll &self, const ledger::dockview::DockViewV2 &view)
{
	// 開いているウィンドウを、ドックと同じ View で書き換える。ウィンドウを開いたときに使うため、コピーも持つ。
	DockPollSettingsState &state = self.settings_state_;
	state.last_about = view.about;
	state.last_participants = view.participants;
	if (SettingsWindow *window = settings_window_of(state)) {
		if (window->isVisible()) {
			window->update_about(view.about);
		}
	}
	if (ParticipantsWindow *window = participants_window_of(state)) {
		if (window->isVisible())
			window->update_rows(view.participants);
	}
}

} // namespace cordmux::dock

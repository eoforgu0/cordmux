// SPDX-License-Identifier: Apache-2.0

// dock/install_flow.h と install_dialog_plan.h の単体テスト。[Vesktopに追加] でほかの Vencord から
// 切り替える確認のダイアログを表示する条件、[再インストール] のコピー、アンインストールの結果の変換と残っている
// ものの並び、確認のダイアログと結果のダイアログの中身の並びを確認する。Qt を起動しない (どちらのヘッダーも
// Qt にも libobs にも依存しない)。

#include "doctest.h"

#include "dock/install_dialog_plan.h"
#include "dock/install_flow.h"
#include "install/removal_table.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace cordmux::dock;
namespace install = cordmux::install;
namespace ledger = cordmux::ledger;
namespace platform = cordmux::platform;

TEST_CASE("ほかの Vencord から切り替える確認のダイアログは、vencordDir のキーがあり、同梱の場所でも cordmux が最後に書き込んだ場所でもないときだけ表示する")
{
	const std::string bundled = "C:/obs/plugins/cordmux/data/vencord";

	// キーがない (Vesktop の既定の Vencord を使っている) ときは表示しない。ダイアログの
	// 「いまVesktopは別のVencordを使っています。」が事実でないため。
	CHECK_FALSE(other_vencord_dialog_needed(std::nullopt, bundled, std::nullopt));

	// ほかの Vencord を指している (自分でビルドした Vencord など) ときは表示する。
	CHECK(other_vencord_dialog_needed(std::string("C:/Users/u/Vencord/dist"), bundled, std::nullopt));
	CHECK(other_vencord_dialog_needed(std::string("C:/Users/u/Vencord/dist"), bundled,
					  std::string("D:/old/cordmux/data/vencord")));

	// 同梱の場所 (すでに cordmux を指している) なら表示しない。比べ方は OsProbe の照合と同じ same_dir_path で、
	// 区切り文字、大文字と小文字、末尾の区切り文字が違うだけでは別の場所にしない。
	CHECK_FALSE(other_vencord_dialog_needed(bundled, bundled, std::nullopt));
	CHECK_FALSE(other_vencord_dialog_needed(std::string("c:/OBS/plugins/cordmux/data/vencord/"), bundled,
						std::nullopt));
	CHECK(platform::same_dir_path("c:/OBS/plugins/cordmux/data/vencord/", bundled));

	// cordmux が最後に書き込んだ場所 (フォルダーを移す前の cordmux) なら表示しない。ダイアログの
	// 「切り替えると、現在のVencordに自分で組み込んだプラグインは使えなくなります。」が事実でないため。
	CHECK_FALSE(other_vencord_dialog_needed(std::string("D:/old/cordmux/data/vencord"), bundled,
						std::string("D:/old/cordmux/data/vencord")));

	// 値が空の文字列でも、キーがあるものとして扱う (利用者が空にした値も利用者の値である)。
	CHECK(other_vencord_dialog_needed(std::string(""), bundled, std::nullopt));
}

TEST_CASE("[再インストール] のコピーは、コピーできなかったファイルの名前を並びの順に返し、残りのファイルもコピーし続ける")
{
	const std::vector<install::CopyStep> steps = {
		{"vencordDesktopMain.js", "src/vencordDesktopMain.js", "dst/vencordDesktopMain.js"},
		{"vencordDesktopRenderer.js", "src/vencordDesktopRenderer.js", "dst/vencordDesktopRenderer.js"},
		{"cordmux-build.txt", "src/cordmux-build.txt", "dst/cordmux-build.txt"},
	};

	// すべてコピーできたら空。
	std::vector<std::string> copied;
	const std::vector<std::string> none = run_copy_steps(
		steps,
		[&copied](const std::string &from, const std::string &to) {
			copied.push_back(from + ">" + to);
			return true;
		},
		"src");
	CHECK(none.empty());
	REQUIRE(copied.size() == 3);
	CHECK(copied[0] == "src/vencordDesktopMain.js>dst/vencordDesktopMain.js");
	CHECK(copied[2] == "src/cordmux-build.txt>dst/cordmux-build.txt"); // 並びのまま (マーカーのファイルは最後)

	// 1 つコピーできなければ、その名前だけを返す。残りのファイルもコピーし続ける (コピーできるものまで古いまま
	// 残さない)。
	std::size_t calls = 0;
	const std::vector<std::string> one = run_copy_steps(
		steps,
		[&calls](const std::string &from, const std::string &) {
			++calls;
			return from != "src/vencordDesktopRenderer.js";
		},
		"src");
	REQUIRE(one.size() == 1);
	CHECK(one[0] == "vencordDesktopRenderer.js");
	CHECK(calls == 3);

	// 空の計画は、コピーするものがないのではなく、コピー元のファイルを数えられなかったことを表すので、
	// コピー元の場所を返す。
	const std::vector<std::string> empty_plan =
		run_copy_steps({}, [](const std::string &, const std::string &) { return true; }, "C:/x/vencord-src");
	REQUIRE(empty_plan.size() == 1);
	CHECK(empty_plan[0] == "C:/x/vencord-src");

	// コピーする関数がなければ、すべての名前を返す (何も伝えずに成功として扱わない)。
	const std::vector<std::string> no_copy = run_copy_steps(steps, CopyFileFn{}, "src");
	CHECK(no_copy.size() == 3);
}

TEST_CASE("コピーし直した後の問い合わせは、自動のコピーし直しでコピーできなかったときだけ行わない (行うと、コピーし直しと問い合わせが止まらずに続く)")
{
	CHECK(probe_after_reinstall(true, false));  // 自動でコピーできた: 照合の一致を早く画面に表示する
	CHECK(probe_after_reinstall(true, true));   // [再インストール] でコピーできた
	CHECK(probe_after_reinstall(false, true));  // [再インストール] でコピーできなかった: 現在の状態を画面に反映する
	CHECK(!probe_after_reinstall(false, false)); // 自動でコピーできなかった: 次は定期的な問い合わせで確認する
}

TEST_CASE("照合の結果の変換では、違ったファイルを照合の順に並べ、照合できなかったときは nullopt にする")
{
	platform::BundleComparison comparison;
	comparison.state = platform::BundleState::Mismatch;
	// data/vencord から読めなかったものと、中身が違ったものを、照合の順 (kVencordBundleFiles) で 1 つに並べる。
	comparison.missing = {"vencordDesktopRenderer.css"};
	comparison.differing = {"vencordDesktopRenderer.js", "vencordDesktopMain.js"};
	const std::vector<std::string> names = mismatched_file_names(comparison);
	REQUIRE(names.size() == 3);
	CHECK(names[0] == "vencordDesktopMain.js");
	CHECK(names[1] == "vencordDesktopRenderer.js");
	CHECK(names[2] == "vencordDesktopRenderer.css");

	// 照合する 4 つのファイル以外の名前も捨てずに、後ろに付ける。
	platform::BundleComparison extra;
	extra.differing = {"unexpected.js"};
	const std::vector<std::string> extra_names = mismatched_file_names(extra);
	REQUIRE(extra_names.size() == 1);
	CHECK(extra_names[0] == "unexpected.js");

	CHECK(hashes_match_of(platform::BundleState::Matches) == std::optional<bool>(true));
	CHECK(hashes_match_of(platform::BundleState::Mismatch) == std::optional<bool>(false));
	CHECK_FALSE(hashes_match_of(platform::BundleState::Unknown).has_value());
}

TEST_CASE("消す対象の表のうち cordmux が消す行は、どれも画面の「残っているもの」の種類を持つ")
{
	// 表を 1 行ずつ確認する (cordmux が消す行を追加したら、画面の種類も追加する必要がある)。
	std::set<int> kinds;
	std::size_t product_rows = 0;
	for (std::size_t i = 0; i < install::kRemovalTableRows; ++i) {
		const install::RemovalRow &row = install::kRemovalTable[i];
		CAPTURE(static_cast<int>(row.item));
		const std::optional<ledger::UninstallLeftoverKind> kind = leftover_kind_of(row.item);
		if (row.remover == install::Remover::Product) {
			++product_rows;
			REQUIRE(kind.has_value());
			CHECK(kinds.insert(static_cast<int>(*kind)).second); // 行ごとに別の種類
		} else {
			// 利用者が消す行と消さない行は、アンインストールが残すものではない。
			CHECK_FALSE(kind.has_value());
		}
	}
	CHECK(product_rows == 4);
}

TEST_CASE("アンインストールの結果は Observation の uninstall にコピーされ、残っているものは表の行、開けなかったシーンコレクション、同じ名前のシーンコレクションの順に並ぶ")
{
	// まだ押していない。
	const ledger::UninstallFacts none = uninstall_facts_of(ledger::UninstallPhase::NotStarted, std::nullopt);
	CHECK(none.phase == ledger::UninstallPhase::NotStarted);
	CHECK(none.leftovers.empty());
	CHECK(none.folder_to_delete.empty());

	// アンインストールが終わった。
	install::UninstallReport done;
	done.reached = install::UninstallStep::ShowFolder;
	done.completed = true;
	done.folder_to_delete = "C:/obs/plugins/cordmux";
	done.record_dir = "D:/rec";
	CHECK(phase_after(done) == ledger::UninstallPhase::Done);
	const ledger::UninstallFacts done_facts = uninstall_facts_of(phase_after(done), done);
	CHECK(done_facts.phase == ledger::UninstallPhase::Done);
	CHECK(done_facts.folder_to_delete == "C:/obs/plugins/cordmux");
	CHECK(done_facts.record_dir == "D:/rec");
	CHECK(done_facts.leftovers.empty());

	// 途中で止まった。
	install::UninstallReport stopped;
	stopped.reached = install::UninstallStep::SettingsAndSecret;
	stopped.completed = false;
	stopped.leftovers.push_back({install::RemovalItem::CordmuxSources, "coll-a"});
	stopped.leftovers.push_back({install::RemovalItem::VencordDirValue, "C:/state.json"});
	stopped.collections_not_opened = {"coll-b"};
	stopped.collections_duplicate_names = {"coll-c"};
	stopped.folder_to_delete = "C:/obs/plugins/cordmux"; // 途中で止まったときも入る (install/uninstall.h)
	CHECK(phase_after(stopped) == ledger::UninstallPhase::Stopped);
	const ledger::UninstallFacts stopped_facts = uninstall_facts_of(phase_after(stopped), stopped);
	REQUIRE(stopped_facts.leftovers.size() == 4);
	CHECK(stopped_facts.leftovers[0].kind == ledger::UninstallLeftoverKind::Sources);
	CHECK(stopped_facts.leftovers[0].where == "coll-a");
	CHECK(stopped_facts.leftovers[1].kind == ledger::UninstallLeftoverKind::VencordDir);
	CHECK(stopped_facts.leftovers[2].kind == ledger::UninstallLeftoverKind::CollectionNotOpened);
	CHECK(stopped_facts.leftovers[2].where == "coll-b");
	CHECK(stopped_facts.leftovers[3].kind == ledger::UninstallLeftoverKind::CollectionNameDuplicate);
	CHECK(stopped_facts.leftovers[3].where == "coll-c");
	CHECK(stopped_facts.folder_to_delete == "C:/obs/plugins/cordmux");

	// シーンコレクションのフォルダーを読めなかったときは、名前を持たない項目 1 つになる (最後に並ぶ)。
	stopped.collection_folder_unreadable = true;
	const ledger::UninstallFacts unreadable_facts = uninstall_facts_of(phase_after(stopped), stopped);
	REQUIRE(unreadable_facts.leftovers.size() == 5);
	CHECK(unreadable_facts.leftovers[4].kind == ledger::UninstallLeftoverKind::CollectionFolderUnreadable);
	CHECK(unreadable_facts.leftovers[4].where.empty());
	stopped.collection_folder_unreadable = false;
	// バックアップを残した設定のフォルダーは、なければ空のまま (段階 (3) がバックアップを見たときだけ値を持つ)。
	CHECK(stopped_facts.settings_dir.empty());
	CHECK_FALSE(stopped_facts.leftovers[0].is_file);

	// 段階 (3) で止まり、バックアップを残したとき: 設定のフォルダーと、残っているものがファイルかどうかが入る。
	install::UninstallReport not_restored;
	not_restored.reached = install::UninstallStep::SourcesAndScenes;
	not_restored.leftovers.push_back({install::RemovalItem::VencordDirValue, "C:/state.json", true});
	not_restored.settings_dir = "C:/obs/plugin_config/cordmux";
	const ledger::UninstallFacts not_restored_facts = uninstall_facts_of(phase_after(not_restored), not_restored);
	CHECK(not_restored_facts.settings_dir == "C:/obs/plugin_config/cordmux");
	REQUIRE(not_restored_facts.leftovers.size() == 1);
	CHECK(not_restored_facts.leftovers[0].is_file);

	// 種類を持たない行が混ざっていても捨てない (アンインストールの側の誤りを隠さない)。
	install::UninstallReport odd;
	odd.leftovers.push_back({install::RemovalItem::ObsLogs, "C:/obs/logs"});
	const ledger::UninstallFacts odd_facts = uninstall_facts_of(ledger::UninstallPhase::Stopped, odd);
	REQUIRE(odd_facts.leftovers.size() == 1);
	CHECK(odd_facts.leftovers[0].where == "C:/obs/logs");

	// 配信中と録画中の 2 つのフィールドは呼び出し元が入れる (ここでは読めていないまま)。
	CHECK_FALSE(stopped_facts.obs_streaming.has_value());
	CHECK_FALSE(stopped_facts.obs_recording.has_value());
}

// アンインストールに渡す入力は、DockPollConfig から 1 か所で作る。フィールドごとに違う値を入れ、どのフィールドも
// コピーされることを確認する。allow_state_json_write と bundled_vencord_dir は、UninstallContext の既定値
// (false と空) と違う値にする。既定値と同じ値では、渡し忘れても見つけられない。製品で渡し忘れると、アンインストールの
// 段階 (3) が毎回 Vesktop の設定 (vencordDir) を元に戻せずに止まり (install/uninstall.cpp の
// restore_vencord_dir)、同梱の場所を指す値かどうかを判定できない。
TEST_CASE("アンインストールに渡す入力は、すべてのフィールドがドックの設定からコピーされる")
{
	DockPollConfig cfg;
	cfg.backup_json_path = "C:/cfg/cordmux/vencord_dir_backup.json";
	cfg.max_json_bytes = 12345;
	cfg.bundled_vencord_dir = "C:/obs/plugins/cordmux/data/vencord";
	cfg.allow_state_json_write = true;
	int stop_calls = 0;
	cfg.stop_listening = [&stop_calls]() { ++stop_calls; };
	// コピーしてはいけないフィールドにも目印の値を入れる (ほかのフィールドと取り違えていないことを確認する)。
	cfg.dll_dir = "C:/obs/plugins/cordmux";
	cfg.settings_json_path = "C:/cfg/cordmux/settings.json";

	const install::UninstallContext context =
		uninstall_context_of(cfg, "C:/cfg/cordmux", "D:/record/cordmux");

	CHECK(context.module_config_dir == "C:/cfg/cordmux");
	CHECK(context.backup_json_path == "C:/cfg/cordmux/vencord_dir_backup.json");
	CHECK(context.record_dir == "D:/record/cordmux");
	CHECK(context.max_json_bytes == 12345);
	CHECK(context.bundled_vencord_dir == "C:/obs/plugins/cordmux/data/vencord");
	CHECK(context.allow_state_json_write);
	REQUIRE(context.stop_listening);
	context.stop_listening();
	CHECK(stop_calls == 1);

	// 設定で書き込みを止めたとき (test/tools/obs-run/seed-config.py の --real-profile) は、その値のまま渡る。
	cfg.allow_state_json_write = false;
	CHECK_FALSE(uninstall_context_of(cfg, "C:/cfg/cordmux", "D:/record/cordmux").allow_state_json_write);

	// 待ち受けを閉じる関数が空なら、空のまま渡る (アンインストールの側は、空なら呼ばない)。
	cfg.stop_listening = nullptr;
	CHECK_FALSE(uninstall_context_of(cfg, "C:/cfg/cordmux", "D:/record/cordmux").stop_listening);
}

TEST_CASE("アンインストールの確認のダイアログを表示できるかは、設定ウィンドウのボタンを押せるかと同じ判定になる")
{
	// 押せる状態: 配信も録画もしておらず、Vesktop が起動していないと分かっている。
	ledger::Observation ready;
	ready.uninstall.obs_streaming = false;
	ready.uninstall.obs_recording = false;
	ready.install.process = ledger::VesktopProcess::NotRunning;

	struct Case {
		const char *name;
		ledger::Observation obs;
		bool can_start;
	};
	std::vector<Case> cases;
	cases.push_back({"押せる", ready, true});
	{
		ledger::Observation o = ready;
		o.uninstall.obs_streaming = true;
		cases.push_back({"配信中", o, false});
	}
	{
		ledger::Observation o = ready;
		o.uninstall.obs_recording = true;
		cases.push_back({"OBS の録画中", o, false});
	}
	{
		ledger::Observation o = ready;
		o.choices.recording = ledger::FeatureSwitch::On;
		cases.push_back({"cordmux の録画中", o, false});
	}
	{
		ledger::Observation o = ready;
		o.install.process = ledger::VesktopProcess::Running;
		cases.push_back({"Vesktop の起動中", o, false});
	}
	{
		ledger::Observation o = ready;
		o.uninstall.obs_streaming.reset();
		cases.push_back({"状態の確認中 (配信が読めていない)", o, false});
	}
	{
		ledger::Observation o = ready;
		o.install.process = ledger::VesktopProcess::Unknown;
		cases.push_back({"状態の確認中 (Vesktop が読めていない)", o, false});
	}
	{
		ledger::Observation o = ready;
		o.uninstall.phase = ledger::UninstallPhase::Running;
		cases.push_back({"アンインストールの最中", o, false});
	}
	{
		ledger::Observation o = ready;
		o.uninstall.phase = ledger::UninstallPhase::Done;
		cases.push_back({"アンインストールの後", o, false});
	}
	{
		// 途中で止まった後は、もう一度押せる。残ったものを片付ける次の操作が、このボタンを押すことだからである
		// (ledger/actions.cpp)。
		ledger::Observation o = ready;
		o.uninstall.phase = ledger::UninstallPhase::Stopped;
		cases.push_back({"途中で止まった後", o, true});
	}
	for (const Case &c : cases) {
		CAPTURE(c.name);
		CHECK(uninstall_can_start(c.obs) == c.can_start);
		// 設定ウィンドウの「バージョン情報」のページのボタンと、[ツール] メニューの項目が読む、View の
		// 押せない理由と同じ答えになる。
		const ledger::dockview::DockViewV2 view = ledger::decide_dock(c.obs);
		CHECK(uninstall_can_start(c.obs) == !view.about.uninstall_button.disabled_reason.has_value());
	}
}

// ---- ダイアログの中身の並び (dock/install_dialog_plan.h) ----

namespace {

namespace keys = ledger::text::keys;
namespace dockview = ledger::dockview;

// ブロックの並びを「種類:キーかテキスト」の文字列の一覧にする (並びの全体を 1 回で比べるため)。段落は「P:」、
// 箇条書きは「B:」、場所の行は「L:見出しのキー:場所:open または -」で始まる。末尾には、箇条書きの
// 項目の下の行 (under_bullet) なら「:under」、ファイルの行 (file) なら「:file」、親フォルダーの中で選択して見せる
// フォルダーの行 (folder_location) なら「:folder」、2 つ目からのグループの始まり (group_start) なら「:group」を付ける。
std::vector<std::string> block_names(const DialogPlan &plan)
{
	std::vector<std::string> out;
	for (const DialogBlock &b : plan.blocks) {
		const std::string text = b.text.is_text() ? b.text.text->id : b.text.literal;
		switch (b.kind) {
		case DialogBlock::Kind::Paragraph:
			out.push_back("P:" + text + (b.group_start ? ":group" : ""));
			break;
		case DialogBlock::Kind::Bullet:
			out.push_back("B:" + text + (b.group_start ? ":group" : ""));
			break;
		case DialogBlock::Kind::Location:
			out.push_back("L:" + text + ":" + b.path + ":" + (b.open_folder ? "open" : "-") +
				      (b.under_bullet ? ":under" : "") + (b.file ? ":file" : "") +
				      (b.folder_location ? ":folder" : "") + (b.group_start ? ":group" : ""));
			break;
		}
	}
	return out;
}

std::vector<DialogButton::Kind> button_kinds(const DialogPlan &plan)
{
	std::vector<DialogButton::Kind> out;
	for (const DialogButton &b : plan.buttons)
		out.push_back(b.kind);
	return out;
}

// 既定のボタンと Esc のボタンの番号 (なければ -1)。
int default_index(const DialogPlan &plan)
{
	for (std::size_t i = 0; i < plan.buttons.size(); ++i)
		if (plan.buttons[i].is_default)
			return static_cast<int>(i);
	return -1;
}

int escape_index(const DialogPlan &plan)
{
	for (std::size_t i = 0; i < plan.buttons.size(); ++i)
		if (plan.buttons[i].is_escape)
			return static_cast<int>(i);
	return -1;
}

std::string p(const char *key)
{
	return std::string("P:") + key;
}

std::string b(const char *key)
{
	return std::string("B:") + key;
}

// 2 つ目からのグループの見出し (前をまとまりの間の余白にするフラグ付き。DialogBlock::group_start)。
std::string g(const char *key)
{
	return p(key) + ":group";
}

dockview::UninstallResultView done_view()
{
	dockview::UninstallResultView v;
	v.kind = dockview::UninstallResultKind::Done;
	v.main = ledger::TextRef{keys::kDialogUninstallDoneHeadline, {}};
	v.notes = {ledger::TextRef{keys::kDialogUninstallDoneText, {}}};
	v.folder_to_delete = dockview::LocationLine{"C:/ProgramData/obs-studio/plugins/cordmux", true};
	v.record_dir = dockview::LocationLine{"D:/Videos/cordmux", true};
	return v;
}

} // namespace

TEST_CASE("アンインストールの確認のダイアログは、削除するもの、元に戻すもの、残るもの、この後の作業の順に並び、既定と Esc は [キャンセル] になる")
{
	const DialogPlan plan = uninstall_confirm_plan();
	CHECK(plan.title.id == keys::kDialogUninstallTitle);
	CHECK(plan.main.id == keys::kDialogUninstallConfirmHeadline);
	CHECK(plan.icon == DialogIcon::Warning); // 元に戻せないものを含む確認
	const std::vector<std::string> expected = {
		p(keys::kDialogUninstallDeletesText),           b(keys::kDialogUninstallDeleteSettingsItemText),
		b(keys::kDialogUninstallDeleteScenesItemText),  p(keys::kDialogUninstallRestoresText),
		p(keys::kDialogUninstallKeepsText),             p(keys::kDialogUninstallFolderAfterText),
	};
	CHECK(block_names(plan) == expected);
	REQUIRE(plan.buttons.size() == 2);
	CHECK(plan.buttons[0].kind == DialogButton::Kind::Confirm);
	CHECK(plan.buttons[0].text.id == keys::kDialogUninstallButton);
	CHECK(plan.buttons[1].kind == DialogButton::Kind::Cancel);
	CHECK(default_index(plan) == 1);
	CHECK(escape_index(plan) == 1);
}

TEST_CASE("切り替えの確認のダイアログは、現在の状態を伝える文の下に場所の行と [フォルダーを開く] を置き、失うもの、変わるもの、残るもの、戻し方を 1 文ずつ並べ、既定と Esc は [キャンセル] になる")
{
	const DialogPlan plan = switch_confirm_plan("C:/Users/u/Vencord/dist");
	CHECK(plan.title.id == keys::kDialogVesktopAddTitle);
	CHECK(plan.main.id == keys::kDialogVesktopSwitchHeadline);
	CHECK(plan.icon == DialogIcon::Warning);
	const std::vector<std::string> expected = {
		std::string("L:") + keys::kDialogVesktopSwitchCurrentText + ":C:\\Users\\u\\Vencord\\dist:open",
		p(keys::kDialogVesktopSwitchPluginsText),
		p(keys::kDialogVesktopSwitchVersionText),
		p(keys::kDialogVesktopSwitchKeptText),
		p(keys::kDialogVesktopSwitchRestoreText),
	};
	CHECK(block_names(plan) == expected);
	CHECK(plan.open_folder_text.id == keys::kDialogOpenFolderButton);
	REQUIRE(plan.buttons.size() == 2);
	CHECK(plan.buttons[0].kind == DialogButton::Kind::Confirm);
	CHECK(plan.buttons[0].text.id == keys::kDialogSwitchButton);
	CHECK(default_index(plan) == 1);
	CHECK(escape_index(plan) == 1);
}

TEST_CASE("アンインストールが終わったときの結果のダイアログは、したこと、削除するフォルダー、削除する前に OBS を起動したときの注意、録画の場所の順に並び、場所を開くボタンは場所の行に、右下には [OK] だけを置く")
{
	dockview::UninstallResultView v = done_view();
	const DialogPlan plan = uninstall_result_plan(v);
	CHECK(plan.icon == DialogIcon::Information);
	CHECK(plan.main.id == keys::kDialogUninstallDoneHeadline);
	CHECK(plan.open_folder_text.id == keys::kDialogOpenFolderButton);
	// 「削除するフォルダー:」の行の右のボタンのテキスト。空なら、ダイアログ (dock/ui/message_dialog.cpp) は
	// その行にボタンを置かない。
	CHECK(plan.open_folder_location_text.id == keys::kDialogOpenFolderLocationButton);
	const std::vector<std::string> expected = {
		p(keys::kDialogUninstallDoneText),
		// 主文が求める操作 (フォルダーの削除) に使うボタンは、削除するフォルダーの行の右に置く。
		// この行だけは、親フォルダーを開いてそのフォルダーを選択する [フォルダーの場所を開く] にする (利用者は
		// フォルダーを丸ごと削除するため)。
		std::string("L:") + keys::kDialogUninstallFolderText + ":C:\\ProgramData\\obs-studio\\plugins\\cordmux:-:folder",
		p(keys::kDialogUninstallRestartWarningText),
		// 録画の場所は見出しの下に置き、[フォルダーを開く] を付ける。
		std::string("L:") + keys::kDialogRecordingsHereText + ":D:\\Videos\\cordmux:open",
	};
	CHECK(block_names(plan) == expected);
	// 押してもダイアログを閉じないボタンは、閉じるボタンの列に置かない。
	const std::vector<DialogButton::Kind> buttons = {DialogButton::Kind::Ok};
	CHECK(button_kinds(plan) == buttons);
	CHECK(default_index(plan) == 0); // [OK]
	CHECK(escape_index(plan) == 0);

	// ファイルの行 (LocationKind::File) は、open_folder が true でも [フォルダーを開く] を置かず、
	// ファイルの場所の行にする。
	dockview::UninstallResultView file_kind = done_view();
	dockview::LocationLine file_row{"D:/Videos/cordmux/list.txt", true};
	file_row.kind = dockview::LocationKind::File;
	file_kind.record_dir = file_row;
	const std::vector<std::string> file_names = block_names(uninstall_result_plan(file_kind));
	CHECK(file_names.back() ==
	      std::string("L:") + keys::kDialogRecordingsHereText + ":D:\\Videos\\cordmux\\list.txt:-:file");

	// フォルダーかファイルかは ledger が決めたとおりにする (open_folder が false の場所の行にはボタンを置かない)。
	dockview::UninstallResultView file_line = done_view();
	file_line.record_dir = dockview::LocationLine{"D:/Videos/cordmux", false};
	const std::vector<std::string> names = block_names(uninstall_result_plan(file_line));
	CHECK(names.back() == std::string("L:") + keys::kDialogRecordingsHereText + ":D:\\Videos\\cordmux:-");
}

TEST_CASE("cordmux の録画が残っていないときは、結果のダイアログに録画の場所の見出しも表示しない")
{
	const auto has_recordings_heading = [](const std::vector<std::string> &names) {
		return std::any_of(names.begin(), names.end(), [](const std::string &n) {
			return n.find(keys::kDialogRecordingsHereText) != std::string::npos;
		});
	};

	// 録画の場所が 1 つもないときは、削除する前に OBS を起動したときの注意で終わる。
	dockview::UninstallResultView none = done_view();
	none.record_dir.reset();
	const std::vector<std::string> none_names = block_names(uninstall_result_plan(none));
	CHECK_FALSE(has_recordings_heading(none_names));
	REQUIRE_FALSE(none_names.empty());
	CHECK(none_names.back() == p(keys::kDialogUninstallRestartWarningText));

	// 場所が空の行だけが来ても、見出しを表示しない (見出しだけが残るダイアログにしない)。
	dockview::UninstallResultView empty_only = done_view();
	empty_only.record_dir = dockview::LocationLine{"", true};
	CHECK(block_names(uninstall_result_plan(empty_only)) == none_names);

	// 手順書と違う置き方も同じ: 名前を伝える 1 文で終わる。
	dockview::UninstallResultView loose = none;
	loose.loose = true;
	loose.folder_to_delete.reset();
	const std::vector<std::string> loose_names = block_names(uninstall_result_plan(loose));
	CHECK_FALSE(has_recordings_heading(loose_names));
	REQUIRE_FALSE(loose_names.empty());
	CHECK(loose_names.back() == p(keys::kDialogUninstallDoneLooseText));

	// アンインストールの結果からダイアログまでを通す: 録画のフォルダーがないか空で (record_dir_if_has_files が
	// 空を返した) 終わった結果を Observation の uninstall にコピーし、ledger の View をそのまま描いても、見出しは
	// 表示しない。
	install::UninstallReport report;
	report.completed = true;
	report.reached = install::UninstallStep::ShowFolder;
	report.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	ledger::Observation obs;
	obs.uninstall = uninstall_facts_of(phase_after(report), report);
	const std::optional<dockview::UninstallResultView> view = ledger::decide_dock(obs).uninstall_result;
	REQUIRE(view.has_value());
	CHECK_FALSE(view->record_dir.has_value());
	CHECK_FALSE(has_recordings_heading(block_names(uninstall_result_plan(*view))));
}

TEST_CASE("確認できなかったシーンコレクションがあって終わったときの結果のダイアログは、したことの直後に名前の箇条書きと次の操作を並べ、その後に削除するフォルダーを置き、アイコンは警告になる")
{
	dockview::UninstallResultView v = done_view();
	v.kind = dockview::UninstallResultKind::DoneUnconfirmed;
	dockview::LeftoverGroup unchecked;
	unchecked.heading = ledger::TextRef{keys::kDialogUninstallUncheckedText.other, {}};
	unchecked.items = {ledger::Label::from_literal("配信用"), ledger::Label::from_literal("練習")};
	v.groups.push_back(unchecked);
	const DialogPlan plan = uninstall_result_plan(v);
	CHECK(plan.icon == DialogIcon::Warning); // 利用者が確認する作業が残るので警告にする
	// 上から読む順を作業の順 (OBS を終了する前にシーンコレクションを開く、OBS を終了する、フォルダーを削除する) に
	// そろえる。グループの外の場所の行 (削除するフォルダー、録画の場所) では、under_bullet を true にしない。
	const std::vector<std::string> expected = {
		p(keys::kDialogUninstallDoneText),
		p(keys::kDialogUninstallUncheckedText.other),
		"B:配信用",
		"B:練習",
		p(keys::kDialogUninstallUncheckedNextText.other),
		std::string("L:") + keys::kDialogUninstallFolderText + ":C:\\ProgramData\\obs-studio\\plugins\\cordmux:-:folder",
		p(keys::kDialogUninstallRestartWarningText),
		std::string("L:") + keys::kDialogRecordingsHereText + ":D:\\Videos\\cordmux:open",
	};
	CHECK(block_names(plan) == expected);
	const std::vector<DialogButton::Kind> buttons = {DialogButton::Kind::Ok};
	CHECK(button_kinds(plan) == buttons);

	// 確認できなかったものがなく終わったときは、アイコンを情報にし、次の操作も表示しない。
	const DialogPlan done = uninstall_result_plan(done_view());
	CHECK(done.icon == DialogIcon::Information);
	const std::vector<std::string> done_names = block_names(done);
	CHECK(std::find(done_names.begin(), done_names.end(), p(keys::kDialogUninstallUncheckedNextText.other)) ==
	      done_names.end());
	CHECK(std::find(done_names.begin(), done_names.end(), p(keys::kDialogUninstallUncheckedNextText.one)) ==
	      done_names.end());

	// シーンコレクションが 1 つのときは、次の操作のテキストを _one の形にする (en は "this scene collection"、
	// ja は「この」)。見出しは ledger が数で選ぶ。_one の見出しでも、同じグループとして次の操作を置く
	// (片方の形だけを見ない)。
	dockview::UninstallResultView one = done_view();
	one.kind = dockview::UninstallResultKind::DoneUnconfirmed;
	dockview::LeftoverGroup one_group;
	one_group.heading = ledger::TextRef{keys::kDialogUninstallUncheckedText.one, {}};
	one_group.items = {ledger::Label::from_literal("配信用")};
	one.groups.push_back(one_group);
	const std::vector<std::string> one_names = block_names(uninstall_result_plan(one));
	CHECK(std::count(one_names.begin(), one_names.end(), p(keys::kDialogUninstallUncheckedNextText.one)) == 1);
	CHECK(std::count(one_names.begin(), one_names.end(), p(keys::kDialogUninstallUncheckedNextText.other)) == 0);
}

TEST_CASE("確認できなかったシーンコレクションの次の操作のテキストは、項目の数ではなくシーンコレクションの数で選ぶ")
{
	// 途中で止まったとき: 1 つのシーンコレクションが「開けなかった」と「同じ名前が 2 つ以上」の両方に出るときは、
	// 1 つと数えて _one にする。
	dockview::UninstallResultView v;
	v.kind = dockview::UninstallResultKind::Stopped;
	v.main = ledger::TextRef{keys::kDialogUninstallStoppedHeadline, {}};
	dockview::LeftoverGroup not_checked;
	not_checked.heading = ledger::TextRef{keys::kDialogNotCheckedText, {}};
	not_checked.items = {
		ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftCollectionNotOpenedItemText, {"配信用"}}),
		ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftCollectionDuplicateItemText, {"配信用"}})};
	v.groups = {not_checked};
	const std::vector<std::string> same = {
		p(keys::kDialogNotCheckedText),
		b(keys::kDialogLeftCollectionNotOpenedItemText),
		b(keys::kDialogLeftCollectionDuplicateItemText),
		p(keys::kDialogUninstallUncheckedNextText.one),
	};
	CHECK(block_names(uninstall_result_plan(v)) == same);

	// 名前が 2 つなら元のキー (ja は「これらの」、en は "these")。
	v.groups[0].items[1] =
		ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftCollectionDuplicateItemText, {"練習"}});
	const std::vector<std::string> names = block_names(uninstall_result_plan(v));
	REQUIRE(!names.empty());
	CHECK(names.back() == p(keys::kDialogUninstallUncheckedNextText.other));

	// 終わったときの結果のダイアログでも、同じ名前の項目は 1 つと数える。
	dockview::UninstallResultView done = done_view();
	done.kind = dockview::UninstallResultKind::DoneUnconfirmed;
	dockview::LeftoverGroup unchecked;
	unchecked.heading = ledger::TextRef{keys::kDialogUninstallUncheckedText.other, {}};
	unchecked.items = {ledger::Label::from_literal("配信用"), ledger::Label::from_literal("配信用")};
	done.groups = {unchecked};
	const std::vector<std::string> done_names = block_names(uninstall_result_plan(done));
	CHECK(std::count(done_names.begin(), done_names.end(), p(keys::kDialogUninstallUncheckedNextText.one)) == 1);
}

TEST_CASE("ledger の、終わったが確認できなかったものがある View をそのまま描くと、名前の箇条書きの後、削除するフォルダーより前に次の操作が並び、アイコンは警告になる")
{
	ledger::Observation obs;
	obs.uninstall.phase = ledger::UninstallPhase::Done;
	obs.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	obs.uninstall.leftovers = {{ledger::UninstallLeftoverKind::CollectionNotOpened, "配信用"}};
	const std::optional<dockview::UninstallResultView> view = ledger::decide_dock(obs).uninstall_result;
	REQUIRE(view.has_value());
	CHECK(view->kind == dockview::UninstallResultKind::DoneUnconfirmed);
	const DialogPlan plan = uninstall_result_plan(*view);
	CHECK(plan.icon == DialogIcon::Warning);
	const std::vector<std::string> names = block_names(plan);
	// 見出しのキーは ledger が数で選ぶ (1 件なら _one)。どちらのキーでも、同じグループとして扱う。
	auto heading = std::find(names.begin(), names.end(), p(keys::kDialogUninstallUncheckedText.one));
	if (heading == names.end())
		heading = std::find(names.begin(), names.end(), p(keys::kDialogUninstallUncheckedText.other));
	REQUIRE(heading != names.end());
	REQUIRE(std::distance(heading, names.end()) >= 3);
	CHECK(*std::next(heading, 1) == "B:配信用");
	const auto next_step = std::next(heading, 2);
	CHECK(*next_step == p(keys::kDialogUninstallUncheckedNextText.one)); // 1 つのシーンコレクション
	// 次の操作は、削除するフォルダーの行より前に置く。
	const std::string folder_prefix = std::string("L:") + keys::kDialogUninstallFolderText + ":";
	const auto folder = std::find_if(names.begin(), names.end(),
					 [&](const std::string &s) { return s.rfind(folder_prefix, 0) == 0; });
	REQUIRE(folder != names.end());
	CHECK(std::distance(names.begin(), next_step) < std::distance(names.begin(), folder));
}

TEST_CASE("手順書と違う置き方で終わったときの結果のダイアログは、場所の行の代わりに名前を伝える 1 文を表示し、ボタンは [OK] だけになる")
{
	dockview::UninstallResultView v = done_view();
	v.loose = true;
	v.folder_to_delete.reset();
	v.main = ledger::TextRef{keys::kDialogUninstallDoneLooseHeadline, {}};
	const DialogPlan plan = uninstall_result_plan(v);
	CHECK(plan.icon == DialogIcon::Information);
	const std::vector<std::string> expected = {
		p(keys::kDialogUninstallDoneText),
		p(keys::kDialogUninstallDoneLooseText),
		std::string("L:") + keys::kDialogRecordingsHereText + ":D:\\Videos\\cordmux:open",
	};
	CHECK(block_names(plan) == expected);
	const std::vector<DialogButton::Kind> buttons = {DialogButton::Kind::Ok};
	CHECK(button_kinds(plan) == buttons);

	// loose が false でも、削除するフォルダーがなければ同じ形 (場所のない「削除するフォルダー:」を表示しない)。
	dockview::UninstallResultView no_folder = done_view();
	no_folder.folder_to_delete.reset();
	CHECK(uninstall_result_is_loose(no_folder));
	CHECK(button_kinds(uninstall_result_plan(no_folder)) == buttons);
}

TEST_CASE("手順書と違う置き方で場所が分かるときの結果のダイアログは、ファイルの場所の行、ファイルの名前の箇条書き、削除するフォルダー、削除する前に OBS を起動したときの注意、録画の場所の順に並ぶ")
{
	dockview::UninstallResultView v = done_view();
	v.loose = true;
	v.folder_to_delete.reset();
	v.main = ledger::TextRef{keys::kDialogUninstallDoneLooseHeadline, {}};
	LooseInstallFiles loose;
	loose.files_dir = "C:/obs/plugins/cordmux/bin/64bit";
	loose.file_names = {"avcodec-61.dll", "cordmux.dll", "ffmpeg.exe"};
	loose.data_dir = "C:/obs/plugins/cordmux/data";
	const DialogPlan plan = uninstall_result_plan(v, &loose);
	CHECK(plan.icon == DialogIcon::Information);
	const std::vector<std::string> expected = {
		p(keys::kDialogUninstallDoneText),
		// ファイルの場所の行には、中を開く [フォルダーを開く] を置く (削除するのは中のファイル)。
		std::string("L:") + keys::kDialogUninstallFilesLocationText + ":C:\\obs\\plugins\\cordmux\\bin\\64bit:open",
		p(keys::kDialogUninstallFilesText),
		"B:avcodec-61.dll",
		"B:cordmux.dll",
		"B:ffmpeg.exe",
		// データのフォルダーは丸ごと削除するので、親フォルダーを開いて選択する。
		std::string("L:") + keys::kDialogUninstallFolderText + ":C:\\obs\\plugins\\cordmux\\data:-:folder",
		p(keys::kDialogUninstallRestartWarningFilesText), // 削除するものが複数 (en は them)
		std::string("L:") + keys::kDialogRecordingsHereText + ":D:\\Videos\\cordmux:open",
	};
	CHECK(block_names(plan) == expected);
	// 名前は ini のテキストではなく、同梱のファイルの名前そのもの (言語によって変えない)。
	for (const DialogBlock &block : plan.blocks) {
		if (block.kind == DialogBlock::Kind::Bullet)
			CHECK_FALSE(block.text.is_text());
	}
	// 名前を伝える 1 文は表示しない (場所と名前をダイアログが示す)。
	const std::vector<std::string> names = block_names(plan);
	CHECK(std::find(names.begin(), names.end(), p(keys::kDialogUninstallDoneLooseText)) == names.end());

	// データのフォルダーが分からなかったときは、その行だけを表示しない (ほかは同じ)。
	LooseInstallFiles no_data = loose;
	no_data.data_dir.clear();
	const std::vector<std::string> no_data_names = block_names(uninstall_result_plan(v, &no_data));
	CHECK(std::none_of(no_data_names.begin(), no_data_names.end(), [](const std::string &n) {
		return n.rfind(std::string("L:") + keys::kDialogUninstallFolderText, 0) == 0;
	}));
	CHECK(std::find(no_data_names.begin(), no_data_names.end(), "B:cordmux.dll") != no_data_names.end());

	// 場所が分からなかったとき (loose がない、files_dir が空、名前がない) は、名前を伝える 1 文に戻る。
	LooseInstallFiles no_dir = loose;
	no_dir.files_dir.clear();
	LooseInstallFiles no_names = loose;
	no_names.file_names.clear();
	const std::vector<const LooseInstallFiles *> bad_ones = {nullptr, &no_dir, &no_names};
	for (const LooseInstallFiles *bad : bad_ones) {
		const std::vector<std::string> fallback = block_names(uninstall_result_plan(v, bad));
		CHECK(std::find(fallback.begin(), fallback.end(), p(keys::kDialogUninstallDoneLooseText)) != fallback.end());
		CHECK(std::find(fallback.begin(), fallback.end(), p(keys::kDialogUninstallFilesText)) == fallback.end());
	}

	// 手順書と違う置き方でないとき (削除するフォルダーが 1 つに決まる) は、loose を渡されても使わない。
	const std::vector<std::string> normal = block_names(uninstall_result_plan(done_view(), &loose));
	CHECK(std::find(normal.begin(), normal.end(), p(keys::kDialogUninstallFilesText)) == normal.end());
	// 途中で止まったときも使わない (削除するものを示すのは、終わったときの結果のダイアログだけ)。
	dockview::UninstallResultView stopped = v;
	stopped.kind = dockview::UninstallResultKind::Stopped;
	const std::vector<std::string> stopped_names = block_names(uninstall_result_plan(stopped, &loose));
	CHECK(std::find(stopped_names.begin(), stopped_names.end(), p(keys::kDialogUninstallFilesText)) ==
	      stopped_names.end());
}

TEST_CASE("手順書と違う置き方のアンインストールの確認のダイアログは、最後にファイルとフォルダーの場所を表示することを伝える")
{
	const std::vector<std::string> loose = block_names(uninstall_confirm_plan(true));
	REQUIRE_FALSE(loose.empty());
	CHECK(loose.back() == p(keys::kDialogUninstallFilesAfterText));
	CHECK(std::find(loose.begin(), loose.end(), p(keys::kDialogUninstallFolderAfterText)) == loose.end());
	// 手順書と違う置き方でないときは、フォルダーの場所を表示すると伝える (引数の既定も手順書と違う置き方でない)。
	CHECK(block_names(uninstall_confirm_plan(false)).back() == p(keys::kDialogUninstallFolderAfterText));
	CHECK(block_names(uninstall_confirm_plan()) == block_names(uninstall_confirm_plan(false)));
}

TEST_CASE("手順書と違う置き方で削除してもらうファイルの名前は、名前の順に並べて重複を除き、データのフォルダーは同梱の Vencord のフォルダーの親にする")
{
	const std::vector<std::string> runtime = {"zlib.dll", "avcodec-61.dll", "ffmpeg.exe", "", "AVCODEC-61.DLL"};
	const std::optional<LooseInstallFiles> got = loose_install_files_of(
		"C:\\obs\\plugins\\cordmux\\bin\\64bit", "cordmux.dll", runtime, "C:/obs/plugins/cordmux/data/vencord");
	REQUIRE(got.has_value());
	CHECK(got->files_dir == "C:\\obs\\plugins\\cordmux\\bin\\64bit");
	// エクスプローラーの既定の並び (名前の順) と同じ。空の名前と、大文字と小文字だけが違う 2 つ目は入れない。
	const std::vector<std::string> names = {"avcodec-61.dll", "cordmux.dll", "ffmpeg.exe", "zlib.dll"};
	CHECK(got->file_names == names);
	CHECK(got->data_dir == "C:/obs/plugins/cordmux/data");

	// 区切り文字が \ でも、末尾に区切り文字があっても、同じ親になる。
	CHECK(loose_install_files_of("D:/x", "cordmux.dll", {}, "D:\\obs\\plugins\\cordmux\\data\\vencord\\")->data_dir ==
	      "D:\\obs\\plugins\\cordmux\\data");
	// パスの最後の部分が vencord でない (約束の外)、親がドライブのルート、空のときはデータのフォルダーを空にする。
	CHECK(loose_install_files_of("D:/x", "cordmux.dll", {}, "D:/obs/data/other")->data_dir.empty());
	CHECK(loose_install_files_of("D:/x", "cordmux.dll", {}, "D:/vencord")->data_dir.empty());
	CHECK(loose_install_files_of("D:/x", "cordmux.dll", {}, "")->data_dir.empty());
	// DLL のフォルダーが分からないときと、名前が 1 つもないときは作らない (ダイアログは名前を伝える 1 文に戻る)。
	CHECK_FALSE(loose_install_files_of("", "cordmux.dll", runtime, "D:/a/vencord").has_value());
	CHECK_FALSE(loose_install_files_of("D:/x", "", {}, "D:/a/vencord").has_value());
}

TEST_CASE("途中で止まったときの結果のダイアログは、グループごとに箇条書きと次の操作を並べ、元に戻せなかった設定の後に削除しないでほしいフォルダーを示し、ボタンは [OK] だけになる")
{
	dockview::UninstallResultView v;
	v.kind = dockview::UninstallResultKind::Stopped;
	v.main = ledger::TextRef{keys::kDialogUninstallStoppedHeadline, {}};
	dockview::LeftoverGroup not_deleted;
	not_deleted.heading = ledger::TextRef{keys::kDialogNotDeletedText, {}};
	not_deleted.items = {ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftSettingsFilesItemText, {}})};
	// 次の操作は、ledger が選んでグループの next に入れる (ここは設定のフォルダーが残っているとき)。この View は
	// グループの caution を持たない (約束の外) ので、注意は caution がないときの選び方 (場所の行を置けるか) で
	// 決まる。
	not_deleted.next = ledger::TextRef{keys::kDialogNotDeletedUninstallAgainNextText, {}};
	dockview::LeftoverGroup not_restored;
	not_restored.heading = ledger::TextRef{keys::kDialogNotRestoredText, {}};
	not_restored.items = {ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftVesktopSettingsItemText, {}})};
	dockview::LeftoverGroup not_checked;
	not_checked.heading = ledger::TextRef{keys::kDialogNotCheckedText, {}};
	not_checked.items = {
		ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftCollectionNotOpenedItemText, {"配信用"}})};
	dockview::LeftoverGroup empty_group; // 空のグループは表示しない
	empty_group.heading = ledger::TextRef{keys::kDialogNotCheckedText, {}};
	v.groups = {not_deleted, not_restored, not_checked, empty_group};
	// 途中で止まったときは、場所のフィールドに値があっても「削除するフォルダー:」と録画の場所を表示しない
	// (フォルダーを先に削除させない)。プラグインのフォルダーは、「元に戻せなかった設定」の注意を見出しにした
	// 場所の行でだけ表示する (注意のテキストは dont_delete_folder で、注意と場所の行の間は小さい余白にする)。
	v.folder_to_delete = dockview::LocationLine{"C:/obs/plugins/cordmux", true};
	v.record_dir = dockview::LocationLine{"D:/Videos/cordmux", true};

	const DialogPlan plan = uninstall_result_plan(v);
	CHECK(plan.icon == DialogIcon::Warning);
	// グループの並びは確認できなかったもの、元に戻せなかった設定、削除できなかったものの順 (ドックの途中で止まった
	// ときのカードと同じ)。ledger は、削除、設定、確認の順で渡す (上の v.groups)。
	const std::vector<std::string> expected = {
		p(keys::kDialogNotCheckedText),
		b(keys::kDialogLeftCollectionNotOpenedItemText),
		p(keys::kDialogUninstallUncheckedNextText.one),
		// 2 つ目と 3 つ目のグループの見出しの前は、まとまりの間の余白にする。1 つ目のグループの見出しには
		// フラグを立てない。
		g(keys::kDialogNotRestoredText),
		b(keys::kDialogLeftVesktopSettingsItemText),
		p(keys::kDialogNotRestoredNextText),
		std::string("L:") + keys::kDialogDontDeleteFolderText + ":C:\\obs\\plugins\\cordmux:open",
		g(keys::kDialogNotDeletedText),
		b(keys::kDialogLeftSettingsFilesItemText),
		p(keys::kDialogNotDeletedUninstallAgainNextText),
	};
	// 並びの全体を比べるので、グループをまたいで残ったものをまとめる 1 文がないことも確認できる。
	CHECK(block_names(plan) == expected);
	// グループを並べ替えるのは、削除しないでほしいフォルダーの注意を、「削除できなかったもの」の次の操作より先に
	// 読ませるためである。
	const auto pos = [&](const std::string &s) { return std::find(expected.begin(), expected.end(), s); };
	CHECK(pos(std::string("L:") + keys::kDialogDontDeleteFolderText + ":C:\\obs\\plugins\\cordmux:open") <
	      pos(p(keys::kDialogNotDeletedUninstallAgainNextText)));
	// ledger が渡す順によらない (この順で渡しても、逆の順や混ざった順で渡しても、同じダイアログになる)。
	for (const std::vector<dockview::LeftoverGroup> &order :
	     {std::vector<dockview::LeftoverGroup>{not_checked, not_restored, not_deleted},
	      std::vector<dockview::LeftoverGroup>{not_restored, empty_group, not_deleted, not_checked}}) {
		dockview::UninstallResultView reordered = v;
		reordered.groups = order;
		CHECK(block_names(uninstall_result_plan(reordered)) == expected);
	}
	// グループが 2 つだけのときも同じ並び (設定、削除の順)。1 つ目に表示するグループの見出しにはフラグを立てない。
	dockview::UninstallResultView restore_and_delete = v;
	restore_and_delete.groups = {not_deleted, not_restored};
	const std::vector<std::string> restore_and_delete_expected = {
		p(keys::kDialogNotRestoredText),
		b(keys::kDialogLeftVesktopSettingsItemText),
		p(keys::kDialogNotRestoredNextText),
		std::string("L:") + keys::kDialogDontDeleteFolderText + ":C:\\obs\\plugins\\cordmux:open",
		g(keys::kDialogNotDeletedText),
		b(keys::kDialogLeftSettingsFilesItemText),
		p(keys::kDialogNotDeletedUninstallAgainNextText),
	};
	CHECK(block_names(uninstall_result_plan(restore_and_delete)) == restore_and_delete_expected);
	// 場所の行を置けるときは、場所がないときのテキスト (dont_delete_files) を表示しない (同じ注意を繰り返さない)。
	CHECK(std::find(expected.begin(), expected.end(), p(keys::kDialogDontDeleteFilesText)) == expected.end());

	// グループが 1 つだけ (元に戻せなかった設定だけ) のときは、そのグループの次の操作だけを表示する。削除する操作も
	// 確認する操作も表示しない。
	dockview::UninstallResultView only_restore = v;
	only_restore.groups = {not_restored};
	const std::vector<std::string> only_restore_expected = {
		p(keys::kDialogNotRestoredText),
		b(keys::kDialogLeftVesktopSettingsItemText),
		p(keys::kDialogNotRestoredNextText),
		std::string("L:") + keys::kDialogDontDeleteFolderText + ":C:\\obs\\plugins\\cordmux:open",
	};
	CHECK(block_names(uninstall_result_plan(only_restore)) == only_restore_expected);

	// 「元に戻せなかった設定」のグループがないときは、フォルダーの場所が来ても表示しない (削除しないでほしいという
	// 注意がないので、場所だけを表示すると、削除するフォルダーと読める)。
	dockview::UninstallResultView only_delete = v;
	only_delete.groups = {not_deleted};
	// 設定を元に戻せたときなので、ledger は not_deleted_next を選ぶ (ダイアログはグループの next を読むだけ)。
	only_delete.groups[0].next = ledger::TextRef{keys::kDialogNotDeletedNextText, {}};
	const std::vector<std::string> only_delete_expected = {
		p(keys::kDialogNotDeletedText),
		b(keys::kDialogLeftSettingsFilesItemText),
		p(keys::kDialogNotDeletedNextText),
	};
	CHECK(block_names(uninstall_result_plan(only_delete)) == only_delete_expected);

	// 手順書と違う置き方 (削除するフォルダーが 1 つに決まらず、場所がない) では場所の行を表示せず、注意は
	// dont_delete_folder ではなく dont_delete_files の段落にする (何も指さない「次の」を表示しない)。
	dockview::UninstallResultView loose_restore = only_restore;
	loose_restore.folder_to_delete.reset();
	loose_restore.loose = true;
	const std::vector<std::string> loose_restore_expected = {
		p(keys::kDialogNotRestoredText),
		b(keys::kDialogLeftVesktopSettingsItemText),
		p(keys::kDialogNotRestoredNextText),
		p(keys::kDialogDontDeleteFilesText),
	};
	CHECK(block_names(uninstall_result_plan(loose_restore)) == loose_restore_expected);
	// loose が false でも、場所が来なければ同じ形にする (場所の行を表示するかと同じ
	// uninstall_result_is_loose で判定する)。
	dockview::UninstallResultView no_place_restore = only_restore;
	no_place_restore.folder_to_delete = dockview::LocationLine{"", true};
	CHECK(block_names(uninstall_result_plan(no_place_restore)) == loose_restore_expected);
	// loose が true なら、約束の外で場所が来ても dont_delete_folder の形にしない。場所の行とテキストを、
	// 同じ判定で選ぶ。
	dockview::UninstallResultView loose_with_place = only_restore;
	loose_with_place.loose = true;
	CHECK(block_names(uninstall_result_plan(loose_with_place)) == loose_restore_expected);
	const std::vector<DialogButton::Kind> buttons = {DialogButton::Kind::Ok};
	CHECK(button_kinds(plan) == buttons);
	CHECK(default_index(plan) == 0);
	CHECK(escape_index(plan) == 0);
}

TEST_CASE("途中で止まったときの結果のダイアログは、項目の下にその項目の場所の行を見出しなしで並べ、フォルダーの行にだけ [フォルダーを開く] を置く")
{
	dockview::UninstallResultView v;
	v.kind = dockview::UninstallResultKind::Stopped;
	v.main = ledger::TextRef{keys::kDialogUninstallStoppedHeadline, {}};
	// 削除できなかったもの: 設定のファイル (場所は 2 つ。設定のフォルダーと、名前を表示しない保存先) と、
	// ソース (場所なし)。
	dockview::LeftoverGroup not_deleted;
	not_deleted.heading = ledger::TextRef{keys::kDialogNotDeletedText, {}};
	not_deleted.items = {ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftSettingsFilesItemText, {}}),
			     ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftSourcesItemText, {"配信用"}})};
	not_deleted.item_locations = {{dockview::LocationLine{"C:/u/obs-studio/plugin_config/cordmux", true},
				       dockview::LocationLine{"C:/u/AppData/Local/cordmux", true}},
				      {}};
	not_deleted.next = ledger::TextRef{keys::kDialogNotDeletedUninstallAgainNextText, {}}; // ledger が選んだ値
	// 元に戻せなかった設定: Vesktop の設定のファイル (ファイルなので、ledger が open_folder を false にする)。
	dockview::LeftoverGroup not_restored;
	not_restored.heading = ledger::TextRef{keys::kDialogNotRestoredText, {}};
	not_restored.items = {ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftVesktopSettingsItemText, {}})};
	not_restored.item_locations = {{dockview::LocationLine{"C:/u/vesktop/settings.json", false}}};
	// 確認できなかったもの: どの項目にも場所がない (item_locations は空)。
	dockview::LeftoverGroup not_checked;
	not_checked.heading = ledger::TextRef{keys::kDialogNotCheckedText, {}};
	not_checked.items = {
		ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftCollectionNotOpenedItemText, {"配信用"}})};
	v.groups = {not_deleted, not_restored, not_checked};

	const DialogPlan plan = uninstall_result_plan(v);
	// グループの並びは、確認、設定、削除の順。
	const std::vector<std::string> expected = {
		p(keys::kDialogNotCheckedText),
		b(keys::kDialogLeftCollectionNotOpenedItemText),
		p(keys::kDialogUninstallUncheckedNextText.one),
		g(keys::kDialogNotRestoredText),
		b(keys::kDialogLeftVesktopSettingsItemText),
		"L::C:\\u\\vesktop\\settings.json:-:under",
		p(keys::kDialogNotRestoredNextText),
		// この View は削除するフォルダーの場所を持たない (手順書と違う置き方) ので、注意は場所がないときの
		// テキストになる。
		p(keys::kDialogDontDeleteFilesText),
		g(keys::kDialogNotDeletedText),
		b(keys::kDialogLeftSettingsFilesItemText),
		// 項目の下の場所の行は、箇条書きのテキストの位置にそろえる (under_bullet)。
		"L::C:\\u\\obs-studio\\plugin_config\\cordmux:open:under",
		"L::C:\\u\\AppData\\Local\\cordmux:open:under",
		b(keys::kDialogLeftSourcesItemText),
		p(keys::kDialogNotDeletedUninstallAgainNextText),
	};
	CHECK(block_names(plan) == expected);
	CHECK(plan.open_folder_text.id == keys::kDialogOpenFolderButton);
	// ファイルの場所の行 (Vesktop の設定) の右に置く [ファイルの場所を開く] のテキスト。
	CHECK(plan.open_file_location_text.id == keys::kDialogOpenFileLocationButton);
	const std::vector<DialogButton::Kind> buttons = {DialogButton::Kind::Ok};
	CHECK(button_kinds(plan) == buttons);

	// item_locations が items より短い View (約束の外) でも、ある分だけを描き、クラッシュしない。
	// 空の場所は描かない。
	dockview::UninstallResultView short_view = v;
	short_view.groups = {not_deleted};
	short_view.groups[0].item_locations = {{dockview::LocationLine{"", true}}};
	short_view.groups[0].next = ledger::TextRef{keys::kDialogNotDeletedNextText, {}}; // 設定を元に戻せたときの値
	const std::vector<std::string> short_expected = {
		p(keys::kDialogNotDeletedText),
		b(keys::kDialogLeftSettingsFilesItemText),
		b(keys::kDialogLeftSourcesItemText),
		p(keys::kDialogNotDeletedNextText),
	};
	CHECK(block_names(uninstall_result_plan(short_view)) == short_expected);
}

TEST_CASE("途中で止まったときの結果のダイアログは、削除できなかったものの次の操作に ledger がグループの next に入れた値を使い、なければ手で削除する道も示すテキストにする")
{
	// 次の操作をどちらにするかは ledger の uninstall_result が決める。not_deleted_uninstall_again_next にするのは、
	// 設定を元に戻せず、設定のファイルが残ったときだけである (テストは test_ledger_dock_rules.cpp)。ダイアログは
	// 同じ条件を持たず、グループの next を読む (ドックの途中で止まったときのカードと同じ値)。ledger から描いて
	// 同じになることは、下にある、結果のダイアログとドックのカードを比べるテストが確認する。
	dockview::UninstallResultView v;
	v.kind = dockview::UninstallResultKind::Stopped;
	v.main = ledger::TextRef{keys::kDialogUninstallStoppedHeadline, {}};
	v.folder_to_delete = dockview::LocationLine{"C:/obs/plugins/cordmux", true};
	dockview::LeftoverGroup not_restored;
	not_restored.heading = ledger::TextRef{keys::kDialogNotRestoredText, {}};
	not_restored.items = {ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftVesktopSettingsItemText, {}})};
	dockview::LeftoverGroup not_deleted;
	not_deleted.heading = ledger::TextRef{keys::kDialogNotDeletedText, {}};
	not_deleted.items = {ledger::Label::from_text(ledger::TextRef{keys::kDialogLeftSettingsFilesItemText, {}})};
	const auto has = [](const DialogPlan &plan, const std::string &name) {
		const std::vector<std::string> names = block_names(plan);
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	const std::string again = p(keys::kDialogNotDeletedUninstallAgainNextText);
	const std::string by_hand = p(keys::kDialogNotDeletedNextText);

	// next が not_deleted_uninstall_again_next のとき: 手で削除する道を示さない。
	dockview::UninstallResultView chosen_again = v;
	chosen_again.groups = {not_deleted, not_restored};
	chosen_again.groups[0].next = ledger::TextRef{keys::kDialogNotDeletedUninstallAgainNextText, {}};
	CHECK(has(uninstall_result_plan(chosen_again), again));
	CHECK_FALSE(has(uninstall_result_plan(chosen_again), by_hand));
	// next が not_deleted_next のとき: グループが同じでも、ダイアログは条件を決め直さない (選ぶのは ledger だけ)。
	dockview::UninstallResultView chosen_by_hand = chosen_again;
	chosen_by_hand.groups[0].next = ledger::TextRef{keys::kDialogNotDeletedNextText, {}};
	CHECK(has(uninstall_result_plan(chosen_by_hand), by_hand));
	CHECK_FALSE(has(uninstall_result_plan(chosen_by_hand), again));
	// next のない View (約束の外): not_deleted_next にする。
	dockview::UninstallResultView no_next = v;
	no_next.groups = {not_deleted, not_restored};
	CHECK(has(uninstall_result_plan(no_next), by_hand));
	CHECK_FALSE(has(uninstall_result_plan(no_next), again));
}

TEST_CASE("ledger の、途中で止まったときの View をそのまま描くと、設定のファイルの項目の下にその場所の行が表示される")
{
	ledger::Observation obs;
	obs.uninstall.phase = ledger::UninstallPhase::Stopped;
	obs.uninstall.leftovers = {{ledger::UninstallLeftoverKind::SettingsFiles, "C:/cfg/cordmux"},
				   {ledger::UninstallLeftoverKind::Sources, "配信用"},
				   {ledger::UninstallLeftoverKind::VencordDir, "C:/u/vesktop/settings.json"}};
	const std::optional<dockview::UninstallResultView> view = ledger::decide_dock(obs).uninstall_result;
	REQUIRE(view.has_value());
	const std::vector<std::string> names = block_names(uninstall_result_plan(*view));
	const auto at = [&](const std::string &s) { return std::find(names.begin(), names.end(), s); };
	const auto vesktop_item = at(b(keys::kDialogLeftVesktopSettingsItemText));
	REQUIRE(vesktop_item != names.end());
	REQUIRE(std::next(vesktop_item) != names.end());
	// ledger は Vesktop の設定をファイルの行 (LocationKind::File) にする。ダイアログはファイルの場所の行にする。
	CHECK(*std::next(vesktop_item) == "L::C:\\u\\vesktop\\settings.json:-:under:file");
	// 設定を元に戻せなかったときの設定のフォルダー (元に戻すためのバックアップが残る) は、削除しないでほしいという
	// 注意の場所の行になる。削除するフォルダーの場所が観測されていない (手順書と違う置き方) ので、注意は
	// dont_delete_files_and_folders で、続く場所の行が 1 つなので _one の形になる。注意の後にその場所の行が続き、
	// プラグインのフォルダーの注意 (dont_delete_folder) は表示しない。
	const auto restore_next = at(p(keys::kDialogNotRestoredNextText));
	REQUIRE(restore_next != names.end());
	REQUIRE(std::next(restore_next) != names.end());
	CHECK(*std::next(restore_next) ==
	      std::string("L:") + keys::kDialogDontDeleteFilesAndFoldersText.one + ":C:\\cfg\\cordmux:open");
	CHECK(std::none_of(names.begin(), names.end(), [](const std::string &n) {
		return n.find(std::string(keys::kDialogDontDeleteFolderText) + ":") != std::string::npos ||
		       n == p(keys::kDialogDontDeleteFilesText);
	}));
	// 「削除できなかったもの」には、設定のファイルの項目もその場所も表示しない (同じ場所を、削除しないでほしい
	// という注意と「削除できなかったもの」の両方に表示しない)。残ったシーンは手で削除してよいので、そのグループの
	// 次の操作は not_deleted_next になる。
	CHECK(at(b(keys::kDialogLeftSettingsFilesItemText)) == names.end());
	CHECK(std::count_if(names.begin(), names.end(), [](const std::string &n) {
		      return n.find("C:\\cfg\\cordmux") != std::string::npos;
	      }) == 1);
	CHECK(at(p(keys::kDialogNotDeletedNextText)) != names.end());
	// ダイアログで 2 つ目に表示するグループ (削除できなかったもの。並びは設定、削除の順) の見出しにだけ、グループの
	// 始まりのフラグが立つ。ledger の View は削除、設定の順に並べるので、ダイアログが並べ替えていることもここで
	// 確認できる。
	CHECK(at(p(keys::kDialogNotRestoredText)) != names.end());
	CHECK(at(g(keys::kDialogNotDeletedText)) != names.end());
	CHECK(at(p(keys::kDialogNotRestoredText)) < at(g(keys::kDialogNotDeletedText)));

	// 段階 (1) で止まったとき (製品で起きる形。バックアップは残っている): 設定のファイルの項目の下に場所の行が
	// 並び、次の操作は not_deleted_uninstall_again_next だけで、手で削除する道を示さない。アンインストールの段階が
	// ファイルとして返した残りはファイルの場所の行 ([ファイルの場所を開く]) に、フォルダーはフォルダーの行になる。
	install::UninstallReport step1_report;
	step1_report.reached = install::UninstallStep::MarkUninstalling;
	step1_report.leftovers.push_back({install::RemovalItem::ModuleConfigFiles, "C:/cfg/cordmux/settings.json", true});
	step1_report.leftovers.push_back({install::RemovalItem::LocalDataFolder, "C:/u/local/cordmux", false});
	step1_report.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	ledger::Observation step1;
	step1.uninstall = uninstall_facts_of(phase_after(step1_report), step1_report);
	const std::optional<dockview::UninstallResultView> step1_view = ledger::decide_dock(step1).uninstall_result;
	REQUIRE(step1_view.has_value());
	CHECK(block_names(uninstall_result_plan(*step1_view)) ==
	      std::vector<std::string>{p(keys::kDialogNotDeletedText), b(keys::kDialogLeftSettingsFilesItemText),
				       "L::C:\\cfg\\cordmux\\settings.json:-:under:file", "L::C:\\u\\local\\cordmux:open:under",
				       p(keys::kDialogNotDeletedUninstallAgainNextText)});

	// 段階 (3) で止まり、バックアップを残したとき (製品で起きる形): 注意 (dont_delete_folders) の後に、プラグインの
	// フォルダー、plugin_config の cordmux の順に場所の行が続く。「削除できなかったもの」のグループはない。
	install::UninstallReport step3_report;
	step3_report.reached = install::UninstallStep::SourcesAndScenes;
	step3_report.leftovers.push_back({install::RemovalItem::VencordDirValue, "C:/u/vesktop/state.json", true});
	step3_report.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	step3_report.settings_dir = "C:/u/obs-studio/plugin_config/cordmux";
	ledger::Observation step3;
	step3.uninstall = uninstall_facts_of(phase_after(step3_report), step3_report);
	const std::optional<dockview::UninstallResultView> step3_view = ledger::decide_dock(step3).uninstall_result;
	REQUIRE(step3_view.has_value());
	CHECK(block_names(uninstall_result_plan(*step3_view)) ==
	      std::vector<std::string>{p(keys::kDialogNotRestoredText), b(keys::kDialogLeftVesktopSettingsItemText),
				       "L::C:\\u\\vesktop\\state.json:-:under:file", p(keys::kDialogNotRestoredNextText),
				       std::string("L:") + keys::kDialogDontDeleteFoldersText +
					       ":C:\\ProgramData\\obs-studio\\plugins\\cordmux:open",
				       "L::C:\\u\\obs-studio\\plugin_config\\cordmux:open"});
	// 手順書と違う置き方 (プラグインのフォルダーが分からない): 注意は dont_delete_files_and_folders で、場所の行は
	// 設定のフォルダーの 1 つだけ (_one の形)。
	install::UninstallReport step3_loose = step3_report;
	step3_loose.folder_to_delete.clear();
	ledger::Observation loose3;
	loose3.uninstall = uninstall_facts_of(phase_after(step3_loose), step3_loose);
	const std::optional<dockview::UninstallResultView> loose3_view = ledger::decide_dock(loose3).uninstall_result;
	REQUIRE(loose3_view.has_value());
	const std::vector<std::string> loose3_names = block_names(uninstall_result_plan(*loose3_view));
	REQUIRE_FALSE(loose3_names.empty());
	CHECK(loose3_names.back() == std::string("L:") + keys::kDialogDontDeleteFilesAndFoldersText.one +
						":C:\\u\\obs-studio\\plugin_config\\cordmux:open");

	// 途中で止まったときのアンインストールの結果 (install::run_uninstall は、止まったときもプラグインのフォルダーを
	// 入れる) を Observation の uninstall にコピーし、ledger の View をそのまま描くと、削除しないでほしいという
	// 注意の直後に、そのフォルダーの場所の行が並ぶ。
	install::UninstallReport report;
	report.reached = install::UninstallStep::SourcesAndScenes;
	report.leftovers.push_back({install::RemovalItem::VencordDirValue, "C:/u/vesktop/settings.json"});
	report.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	ledger::Observation stopped;
	stopped.uninstall = uninstall_facts_of(phase_after(report), report);
	const std::optional<dockview::UninstallResultView> stopped_view = ledger::decide_dock(stopped).uninstall_result;
	REQUIRE(stopped_view.has_value());
	const std::vector<std::string> stopped_names = block_names(uninstall_result_plan(*stopped_view));
	const auto note = std::find(stopped_names.begin(), stopped_names.end(), p(keys::kDialogNotRestoredNextText));
	REQUIRE(note != stopped_names.end());
	REQUIRE(std::next(note) != stopped_names.end());
	CHECK(*std::next(note) == std::string("L:") + keys::kDialogDontDeleteFolderText +
					  ":C:\\ProgramData\\obs-studio\\plugins\\cordmux:open");
	// 途中で止まったときは、「削除するフォルダー:」を表示しない。
	CHECK(std::none_of(stopped_names.begin(), stopped_names.end(), [](const std::string &n) {
		return n.rfind(std::string("L:") + keys::kDialogUninstallFolderText, 0) == 0;
	}));
}

TEST_CASE("途中で止まったときの結果のダイアログとドックのカードは、同じ観測から、グループ、項目、次の操作、注意のテキストを同じ順に並べ、グループの始まりも同じ位置になる")
{
	// ダイアログ (install_dialog_plan) とカード (ledger の stopped_card_body) は、同じ uninstall_result から
	// 別々に並べる。並べる所が 2 か所あるので、どちらかだけが並びを変えたら失敗するようにする。場所があるときと
	// ないとき (手順書と違う置き方) の両方で確認する。
	for (const bool with_folder : {true, false}) {
		CAPTURE(with_folder);
		ledger::Observation obs;
		obs.uninstall.phase = ledger::UninstallPhase::Stopped;
		if (with_folder)
			obs.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
		obs.uninstall.leftovers = {{ledger::UninstallLeftoverKind::SettingsFiles, "C:/cfg/cordmux"},
					   {ledger::UninstallLeftoverKind::Sources, "配信用"},
					   {ledger::UninstallLeftoverKind::VencordDir, "C:/u/vesktop/settings.json"},
					   {ledger::UninstallLeftoverKind::CollectionNotOpened, "予備"},
					   {ledger::UninstallLeftoverKind::CollectionNameDuplicate, "配信用"}};
		const dockview::DockViewV2 view = ledger::decide_dock(obs);
		REQUIRE(view.uninstall_result.has_value());
		const auto card = std::find_if(view.notices.begin(), view.notices.end(),
					       [](const dockview::Notice &n) { return n.key == "Uninstalled:Stopped"; });
		REQUIRE(card != view.notices.end());

		// ダイアログのブロックのうちテキストを持つもの (見出しのない場所の行はテキストを持たない。カードでは
		// body_locations に入る) を、カードの段落と比べる。
		const DialogPlan plan = uninstall_result_plan(*view.uninstall_result);
		std::vector<std::string> window_ids;
		std::vector<std::size_t> window_group_starts;
		for (const DialogBlock &block : plan.blocks) {
			if (!block.text.is_text() || block.text.text->id.empty())
				continue;
			if (block.group_start)
				window_group_starts.push_back(window_ids.size());
			window_ids.push_back(block.text.text->id);
		}
		std::vector<std::string> card_ids;
		for (const ledger::TextRef &t : card->body)
			card_ids.push_back(t.id);
		CHECK(window_ids == card_ids);
		CHECK(window_group_starts == card->body_group_starts);
		// 場所の行も同じ並びになる (削除しないでほしいという注意の下の場所の行が、ダイアログとカードで同じ)。
		// ダイアログの場所の行は、テキストの有無によらずすべてを、カードは body_locations の順に比べる。
		std::vector<std::string> window_places;
		for (const DialogBlock &block : plan.blocks) {
			if (block.kind == DialogBlock::Kind::Location)
				window_places.push_back(block.path);
		}
		std::vector<std::string> card_places;
		for (const dockview::BodyLocation &line : card->body_locations)
			card_places.push_back(windows_path(line.line.path));
		CHECK(window_places == card_places);
		// 元に戻すためのバックアップが残る設定のフォルダーは、注意の場所の行に 1 回だけ表示する
		// (「削除できなかったもの」には表示しない)。
		CHECK(std::count(window_places.begin(), window_places.end(), std::string("C:\\cfg\\cordmux")) == 1);
		CHECK(std::find(window_ids.begin(), window_ids.end(), std::string(keys::kDialogLeftSettingsFilesItemText)) ==
		      window_ids.end());
		// 並びの先頭は「確認できなかったもの」。
		REQUIRE_FALSE(window_ids.empty());
		CHECK(window_ids.front() == keys::kDialogNotCheckedText);
	}
}

TEST_CASE("ledger が主文を入れなかったときは、結果のダイアログが種類と形から主文を選ぶ (主文のないダイアログを表示しない)")
{
	dockview::UninstallResultView done = done_view();
	done.main = ledger::TextRef{};
	CHECK(uninstall_result_plan(done).main.id == keys::kDialogUninstallDoneHeadline);

	dockview::UninstallResultView loose = done_view();
	loose.main = ledger::TextRef{};
	loose.loose = true;
	CHECK(uninstall_result_plan(loose).main.id == keys::kDialogUninstallDoneLooseHeadline);

	dockview::UninstallResultView stopped;
	stopped.kind = dockview::UninstallResultKind::Stopped;
	CHECK(uninstall_result_plan(stopped).main.id == keys::kDialogUninstallStoppedHeadline);
}

TEST_CASE("場所は Windows の区切り文字で表示する")
{
	CHECK(windows_path("C:/a/b\\c/") == "C:\\a\\b\\c\\");
	CHECK(windows_path("") == "");
}

TEST_CASE("[Vesktopから削除] の結果は、画面に出す理由の文ごとに失敗にまとめ、成功と Vesktop の起動中は失敗にしない")
{
	using O = install::VesktopRemoveOutcome;
	using F = ledger::VesktopRemoveFailure;
	// 成功と、書く直前に Vesktop が起動していた (か、調べられなかった) ときは失敗にしない (後者は、押せない理由を
	// ボタンの下に出す)。
	for (const O ok : {O::Restored, O::AlreadyRestored, O::VesktopRunning, O::VesktopUnknown})
		CHECK_FALSE(remove_failure_of(ok).has_value());
	CHECK(remove_failure_of(O::StatePathUnknown) == std::optional<F>(F::StateUnreadable));
	CHECK(remove_failure_of(O::StateUnreadable) == std::optional<F>(F::StateUnreadable));
	CHECK(remove_failure_of(O::NoBackup) == std::optional<F>(F::NotRestorable));
	CHECK(remove_failure_of(O::BackupInvalid) == std::optional<F>(F::NotRestorable));
	CHECK(remove_failure_of(O::Refused) == std::optional<F>(F::NotRestorable));
	CHECK(remove_failure_of(O::WritesDisabled) == std::optional<F>(F::WritesDisabled));
	CHECK(remove_failure_of(O::StateWriteFailed) == std::optional<F>(F::StateWriteFailed));
}

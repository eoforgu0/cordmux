// SPDX-License-Identifier: Apache-2.0

// インストールとアンインストールのダイアログの中身を決める (install_dialog_plan.h の実装)。フォルダーの場所の行には、
// 行の右に [フォルダーを開く] を置き、右下のボタンの列には置かない。アンインストールが
// 終わったときの「削除するフォルダー:」の行だけは、[フォルダーの場所を開く] にする (install_dialog_plan.h の
// DialogBlock::folder_location)。

#include "dock/install_dialog_plan.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ledger/plural.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace keys = ledger::text::keys;
using ledger::dockview::UninstallResultKind;
using ledger::dockview::UninstallResultView;

namespace {

ledger::TextRef ref(const char *key)
{
	return ledger::TextRef{key, {}};
}

DialogBlock paragraph(ledger::Label text)
{
	DialogBlock b;
	b.kind = DialogBlock::Kind::Paragraph;
	b.text = std::move(text);
	return b;
}

DialogBlock paragraph(const char *key)
{
	return paragraph(ledger::Label::from_text(ref(key)));
}

DialogBlock bullet(ledger::Label text)
{
	DialogBlock b;
	b.kind = DialogBlock::Kind::Bullet;
	b.text = std::move(text);
	return b;
}

DialogBlock bullet(const char *key)
{
	return bullet(ledger::Label::from_text(ref(key)));
}

// heading が nullptr なら、場所の前に何も置かない。
DialogBlock location(const char *heading, const std::string &path, bool open_folder)
{
	DialogBlock b;
	b.kind = DialogBlock::Kind::Location;
	if (heading != nullptr)
		b.text = ledger::Label::from_text(ref(heading));
	b.path = windows_path(path);
	b.open_folder = open_folder;
	return b;
}

// ledger が作った場所の行。[フォルダーを開く] を置くか (フォルダーかファイルか) は、ledger が決めたとおりにする。
DialogBlock location(const char *heading, const ledger::dockview::LocationLine &line)
{
	const bool file = line.kind == ledger::dockview::LocationKind::File;
	DialogBlock b = location(heading, line.path, file ? false : line.open_folder);
	b.file = file;
	return b;
}

// アンインストールが終わったときの「削除するフォルダー:」の行 (普通のインストールではプラグインのフォルダー、
// 手順書と違う置き方ではデータのフォルダー)。行の右には、親を開いてそのフォルダーを選択する
// [フォルダーの場所を開く] を置く (install_dialog_plan.h の DialogBlock::folder_location)。
DialogBlock folder_to_delete_line(const std::string &path)
{
	DialogBlock b = location(keys::kDialogUninstallFolderText, path, false);
	b.folder_location = true;
	return b;
}

DialogButton confirm_button(const char *key)
{
	DialogButton b;
	b.kind = DialogButton::Kind::Confirm;
	b.text = ref(key);
	return b;
}

// 元に戻せない操作と、ほかのアプリの設定を変える操作の確認では、既定のボタン (Enter) と Esc を [キャンセル] にする。
DialogButton cancel_button()
{
	DialogButton b;
	b.kind = DialogButton::Kind::Cancel;
	b.is_default = true;
	b.is_escape = true;
	return b;
}

// ledger が主文を入れなかったとき (読めない View) の主文。結果の種類と形から 1 つに決まる。主文のないダイアログを
// 表示しないためである。
const char *fallback_main_key(const UninstallResultView &v)
{
	if (v.kind == UninstallResultKind::Stopped)
		return keys::kDialogUninstallStoppedHeadline;
	return uninstall_result_is_loose(v) ? keys::kDialogUninstallDoneLooseHeadline
					       : keys::kDialogUninstallDoneHeadline;
}

// グループの項目が指すシーンコレクションの数。確認できなかったシーンコレクションについての次の操作を、
// 数によって選ぶために使う。終わったときのグループの項目は名前そのもの (literal) で、
// 途中で止まったときの「確認できなかったもの」の項目は理由付きのテキストで、名前は 1 つ目の引数である。
// 同じ名前は 1 つと数える。1 つのシーンコレクションが、開けなかったことと、同じ名前が 2 つ以上あることの両方で
// 表示されても、開いて確認するのは 1 つなので、「この」(en は "this") にする。名前を持たない項目 (約束の外) は、
// 項目ごとに別のものと数える (少なく数えて「この」と言い切らない)。項目の数 (group.items.size()) で数えると、
// 1 つのシーンコレクションに「これらの」が付いてしまう。
long long collection_count(const ledger::dockview::LeftoverGroup &group)
{
	std::vector<std::string> names;
	long long unnamed = 0;
	for (const ledger::Label &item : group.items) {
		// フォルダーを読めなかった項目は、シーンコレクションを指さない (数えない)。
		if (item.is_text() && item.text->id == keys::kDialogLeftCollectionFolderItemText)
			continue;
		std::string name;
		if (!item.is_text())
			name = item.literal;
		else if (!item.text->args.empty())
			name = item.text->args.front();
		if (name.empty()) {
			++unnamed;
			continue;
		}
		if (std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
	}
	return static_cast<long long>(names.size()) + unnamed;
}

// グループの見出しが、確認できなかったシーンコレクションのグループか。終わったときの見出しは ledger が数によって
// 選ぶので、両方の形を見る。片方だけを見ると、ledger が _one を選んだダイアログで、次の操作が表示されなくなる。
// 途中で止まったときの「確認できなかったもの:」も、同じ次の操作を表示する。
bool unchecked_group(const ledger::TextRef &heading)
{
	return heading.id == keys::kDialogUninstallUncheckedText.other ||
	       heading.id == keys::kDialogUninstallUncheckedText.one || heading.id == keys::kDialogNotCheckedText;
}

// 結果のダイアログで、グループを並べる順位 (小さいほど上)。確認できなかったもの、元に戻せなかった設定、
// 削除できなかったものの順にする。ドックの止まったときのカード (ledger の stopped_card_body) と同じ順である。
// OBS を終了する前にする作業を、OBS を終了した後にする作業 (フォルダーの削除) より先に読ませ、上から読む順を作業の
// 順にするためである。確認できなかったものの次の操作は、OBS を終了する前にシーンコレクションを開くことで、
// OBS の中の作業である。設定を元に戻すのも、もう一度アンインストールすることで、OBS の中の作業である。
// 削除できなかったものの次の操作は手での削除で、OBS を閉じた後でもできる。この順なら、
// 設定のグループの「次のフォルダーを削除しないでください」が、削除のグループの「手で削除してください」より先に読まれる
// (逆だと、手で削除するものを読んだ後で、削除してはいけないものを知ることになる)。ダイアログを閉じた後に残るのは
// ドックのカードだけなので、カードとダイアログで、同じ情報の並びを違えない。
// 知らない見出しのグループ (約束の外) は、最後に来た順のまま並べる (表示せずに捨てない)。並べる処理をここに置くのは、
// ドックのカード (stopped_card_body) も ledger の groups の順を使わず、見出しで判断して並べているのと同じ
// 形にするためである。ledger/dock_view.h の LeftoverGroup のコメントは、groups の順を削除、設定、確認と約束しているが、
// それが変わっても、並べ替えは安定なので、ダイアログの並びは変わらない。カードと同じ並びになることは、
// tests/test_dock_install_flow.cpp で確認する。
int group_rank(const ledger::TextRef &heading)
{
	if (unchecked_group(heading))
		return 0;
	if (heading.id == keys::kDialogNotRestoredText)
		return 1;
	if (heading.id == keys::kDialogNotDeletedText)
		return 2;
	return 3;
}

} // namespace

std::string windows_path(const std::string &path)
{
	std::string out = path;
	std::replace(out.begin(), out.end(), '/', '\\');
	return out;
}

bool uninstall_result_is_loose(const UninstallResultView &v)
{
	// ledger は、削除するフォルダーが 1 つに決まらないときに loose を true にし、folder_to_delete を入れない。
	// どちらか片方でも当てはまれば場所の行を表示できないので、同じ形として扱う (場所のない「削除するフォルダー:」を
	// 表示しない)。
	return v.loose || !v.folder_to_delete.has_value() || v.folder_to_delete->path.empty();
}

DialogPlan uninstall_confirm_plan(bool loose)
{
	DialogPlan p;
	p.title = ref(keys::kDialogUninstallTitle);
	p.icon = DialogIcon::Warning;
	p.main = ref(keys::kDialogUninstallConfirmHeadline);
	// 消えるもの、元に戻すもの、残るもの、この後に必要な作業の順にする。削除と復元を、1 つの箇条書きの下に
	// 混ぜない。
	p.blocks.push_back(paragraph(keys::kDialogUninstallDeletesText));
	p.blocks.push_back(bullet(keys::kDialogUninstallDeleteSettingsItemText));
	p.blocks.push_back(bullet(keys::kDialogUninstallDeleteScenesItemText));
	p.blocks.push_back(paragraph(keys::kDialogUninstallRestoresText));
	p.blocks.push_back(paragraph(keys::kDialogUninstallKeepsText));
	// 最後の文は、アンインストールが終わったときのダイアログが表示するものと同じものを伝える (手順書と違う置き方
	// では、ファイルとフォルダーの場所)。
	p.blocks.push_back(paragraph(loose ? keys::kDialogUninstallFilesAfterText : keys::kDialogUninstallFolderAfterText));
	p.buttons.push_back(confirm_button(keys::kDialogUninstallButton));
	p.buttons.push_back(cancel_button());
	p.open_folder_text = ref(keys::kDialogOpenFolderButton);
	return p;
}

DialogPlan switch_confirm_plan(const std::string &current_vencord_dir)
{
	DialogPlan p;
	p.title = ref(keys::kDialogVesktopAddTitle);
	p.icon = DialogIcon::Warning;
	p.main = ref(keys::kDialogVesktopSwitchHeadline);
	// 現在の状態の 1 文を前に置き、今使っている Vencord の場所を、コピーできる 1 行で表示する。フォルダーなので、
	// 行の右に [フォルダーを開く] を置く。切り替える前に、自分で何を入れていたかを開いて確認できる。
	p.blocks.push_back(location(keys::kDialogVesktopSwitchCurrentText, current_vencord_dir, true));
	// 切り替えで起きることを 1 文ずつ別の段落にし、失うもの、変わるもの、残るもの、アンインストールで元に戻すことの
	// 順にする。判断に必要な、失うものを先頭にして埋もれさせないためである。同じ種類の項目の並びではないので、
	// 箇条書きにはしない。
	p.blocks.push_back(paragraph(keys::kDialogVesktopSwitchPluginsText));
	p.blocks.push_back(paragraph(keys::kDialogVesktopSwitchVersionText));
	p.blocks.push_back(paragraph(keys::kDialogVesktopSwitchKeptText));
	p.blocks.push_back(paragraph(keys::kDialogVesktopSwitchRestoreText));
	p.buttons.push_back(confirm_button(keys::kDialogSwitchButton));
	p.buttons.push_back(cancel_button());
	p.open_folder_text = ref(keys::kDialogOpenFolderButton);
	return p;
}

// 並びは次のとおり。
//   - 終わったとき: 主文、cordmux がしたこと (ledger の notes)、確認できなかったシーンコレクション (ledger の groups。
//     あるときだけ)、その次の操作、削除するフォルダーと場所の行、削除する前に OBS を起動したときのこと、
//     cordmux の録画の場所 (あって空でないときだけ)
//   - 手順書と違う置き方で終わったとき: 主文、cordmux がしたこと、削除するファイルの場所の行、削除するファイルの名前の
//     箇条書き、削除するフォルダー (データのフォルダー) と場所の行、削除する前に OBS を起動したときのこと、
//     cordmux の録画の場所。場所を読めなかったときだけ、削除するファイルは zip の cordmux フォルダーにあるものと同じ名前という 1 文にする
//   - 途中で止まったとき: 主文、表示するグループ (確認できなかったもの、元に戻せなかった設定、削除できなかったもの) を
//     group_rank の順に並べる。グループごとに、見出し、項目 (項目の下にその場所の行)、そのグループの次の操作を置く
DialogPlan uninstall_result_plan(const UninstallResultView &v, const LooseInstallFiles *loose)
{
	DialogPlan p;
	p.title = ref(keys::kDialogUninstallTitle);
	const bool stopped = v.kind == UninstallResultKind::Stopped;
	// 途中で止まったときと、終わったが確認できなかったものがあるときは警告、終わったときは情報のアイコンにする。
	// 確認できなかったものがあるときは、残っているかもしれないものを利用者が確認する作業が残り、ドックでもメッセージの
	// レベル 2 (警告) で表示するので、ダイアログとドックで同じ情報の重さを違えない。
	p.icon = stopped || v.kind == UninstallResultKind::DoneUnconfirmed ? DialogIcon::Warning
									   : DialogIcon::Information;
	p.main = v.main.id.empty() ? ref(fallback_main_key(v)) : v.main;
	p.open_folder_text = ref(keys::kDialogOpenFolderButton);
	p.open_file_location_text = ref(keys::kDialogOpenFileLocationButton);
	// 「削除するフォルダー:」の行の右の [フォルダーの場所を開く]。
	p.open_folder_location_text = ref(keys::kDialogOpenFolderLocationButton);

	// cordmux がしたこと (ledger が選ぶ)。
	for (const ledger::TextRef &note : v.notes)
		p.blocks.push_back(paragraph(ledger::Label::from_text(note)));

	// 残ったもののグループ (途中で止まったときは 3 つ、終わったときは確認できなかったシーンコレクションの 1 つ)。
	// 見出しと箇条書きは ledger が選ぶ。cordmux がしたことの直後に置く。終わったが確認できなかったダイアログでは、
	// 次の操作「OBSを終了する前に...」が、削除するフォルダーの行と、主文の「OBSを終了してから」より先に読まれ、
	// 上から読む順が作業の順になる。主文を「シーンコレクションを確認し、OBSを終了してから...」にしないのは、
	// キーが増え、大きい主文が 2 行になり、残っている場合だけの作業を主な指示に入れることになるためである。
	// グループの順は group_rank の順 (確認、設定、削除) にし、同じ順位のグループは来た順のまま
	// (stable_sort) にする。
	std::vector<const ledger::dockview::LeftoverGroup *> ordered;
	ordered.reserve(v.groups.size());
	for (const ledger::dockview::LeftoverGroup &group : v.groups)
		ordered.push_back(&group);
	std::stable_sort(ordered.begin(), ordered.end(),
			 [](const ledger::dockview::LeftoverGroup *a, const ledger::dockview::LeftoverGroup *b) {
				 return group_rank(a->heading) < group_rank(b->heading);
			 });
	bool first_group = true;
	for (const ledger::dockview::LeftoverGroup *group_ptr : ordered) {
		const ledger::dockview::LeftoverGroup &group = *group_ptr;
		if (group.items.empty())
			continue;
		// 2 つ目からのグループの見出しの前は、まとまりの間の余白にする (install_dialog_plan.h の
		// DialogBlock::group_start)。
		DialogBlock heading = paragraph(ledger::Label::from_text(group.heading));
		heading.group_start = !first_group;
		first_group = false;
		p.blocks.push_back(std::move(heading));
		for (std::size_t i = 0; i < group.items.size(); ++i) {
			p.blocks.push_back(bullet(group.items[i]));
			// 項目の下に、その項目の場所の行を、前に何も置かずに並べる。item_locations は空 (どの項目にも
			// 場所がない) か、items と同じ長さである (ledger/dock_view.h の約束)。長さが足りない
			// View でも、ある分だけ描画し、クラッシュしない。
			if (i >= group.item_locations.size())
				continue;
			for (const ledger::dockview::LocationLine &place : group.item_locations[i]) {
				if (place.path.empty())
					continue;
				DialogBlock line = location(nullptr, place);
				line.under_bullet = true; // 箇条書きのテキストの位置にそろえる
				p.blocks.push_back(std::move(line));
			}
		}
		// 次の操作はグループごとに、そのグループの箇条書き (と場所の行) の直後に置く。表示していないグループの
		// 操作は表示しない (ledger は空のグループを入れないので、途中で止まったダイアログは、グループが
		// 1 つだけでも表示できる)。グループをまたぐ 1 文を最後に置かないのは、設定 (元に戻すもの) と、
		// 確認できなかったもの (確認するもの) にも「手で削除」を勧めることになり、
		// 直前の「フォルダーを削除しないでください」の後で、残ったものにそのフォルダーが入るとも
		// 読めるためである。
		if (group.heading.id == keys::kDialogNotDeletedText) {
			// 元に戻せなかった設定のバックアップが残るフォルダーを、手で削除させない。どちらの次の操作を
			// 表示するかは、ledger の uninstall_result がグループの next に入れたものを使う (ドックの
			// 止まったときのカードと同じ値。条件は ledger/dock_view.h の LeftoverGroup::next)。next がない
			// View (終わったときのダイアログなど) では、現在の次の操作を表示する。ダイアログでグループを見て
			// 同じ条件を判断し直さないのは、条件が 2 か所に分かれるためである。
			p.blocks.push_back(group.next.has_value() ? paragraph(ledger::Label::from_text(*group.next))
								  : paragraph(keys::kDialogNotDeletedNextText));
		} else if (group.heading.id == keys::kDialogNotRestoredText) {
			// 元に戻す操作 (手で戻す方法は示さない) の後に、元に戻すまでフォルダーを
			// 削除しないでほしいことを置く。先にフォルダーを削除すると、Vesktop が公式の Vencord をそこに
			// 入れる。
			p.blocks.push_back(paragraph(keys::kDialogNotRestoredNextText));
			// 注意が指すプラグインのフォルダー (終わったときの「削除するフォルダー」と同じ所) を、
			// 注意を前に置いた場所の行で表示する ([フォルダーを開く] 付き)。
			// 同じダイアログの「削除できなかったもの」に、名前が cordmux のフォルダーが 2 つ並び、
			// そのグループの次の操作は「手で削除」なので、削除してはいけない 1 つの場所を見せる。
			// テキストは「次のフォルダーを削除しないでください」で、前のテキストと場所の行の間は小さい余白
			// (0.25 em) にする (終わったときの「削除するフォルダー:」と同じ)。手順書と違う置き方 (削除する
			// フォルダーが 1 つに決まらない) では場所の行を置けないので、「次の」が何も指さないテキストは
			// 表示せず、「cordmuxのファイルを削除しないでください」の段落にする (dont_delete_files)。
			// どちらにするかは、場所の行を表示するかと同じ uninstall_result_is_loose で決める (ドックの
			// 止まったときのカードでは ledger の stopped_card_body)。
			// 「cordmux本体のフォルダー」というテキストにしないのは、「本体」は画面のどこにも表示しない
			// 語で、場所の行ほど確かに指せないためである。注意を消すと、Vesktop が公式の Vencord を入れ、
			// 設定が戻らなくなる。注意を場所の行の後に置くと、場所を見る前に何の場所かが分からない。
			// テキストと場所の行は、ledger の uninstall_result がグループの caution と caution_locations に
			// 入れたものを使う (ドックの止まったときのカードと同じ値)。元に戻すためのバックアップが残る
			// cordmux の設定のフォルダーが分かるとき (Vesktop の設定を戻す手順で止まったときの
			// UninstallFacts::settings_dir、または残っているものとして表示されたとき) は、プラグインの
			// フォルダーの後にその場所の行も続き、テキストは 2 つ以上を指す形 (dont_delete_folders か
			// dont_delete_files_and_folders) になる。1 つ目の場所の行に注意を前に置き、2 つ目からは前に
			// 何も置かずに続ける (録画の場所の行と同じ並べ方)。caution のない View (約束の外) では、
			// 前の選び方にする。ダイアログが場所の行の数と中身からテキストを選び直さないのは、条件が
			// ledger とダイアログの 2 か所に分かれるためである。
			if (group.caution.has_value()) {
				bool first_place = true;
				for (const ledger::dockview::LocationLine &place : group.caution_locations) {
					if (place.path.empty())
						continue;
					DialogBlock line = location(nullptr, place);
					if (first_place)
						line.text = ledger::Label::from_text(*group.caution);
					first_place = false;
					p.blocks.push_back(std::move(line));
				}
				if (first_place)
					p.blocks.push_back(paragraph(ledger::Label::from_text(*group.caution)));
			} else if (!uninstall_result_is_loose(v)) {
				p.blocks.push_back(location(keys::kDialogDontDeleteFolderText, *v.folder_to_delete));
			} else {
				p.blocks.push_back(paragraph(keys::kDialogDontDeleteFilesText));
			}
		} else if (unchecked_group(group.heading)) {
			// 確認できなかったシーンコレクションを開いて確認する、次の操作。テキストはシーンコレクションの
			// 数で選ぶ (1 つなら ja は「この」、en は "this")。
			// 名前の分かるシーンコレクションがなければ、すべてを開く文にする。
			const long long count = collection_count(group);
			p.blocks.push_back(paragraph(count == 0 ? keys::kDialogUninstallUncheckedAllNextText
								 : ledger::text::pick(keys::kDialogUninstallUncheckedNextText, count)));
		}
	}

	const bool is_loose = uninstall_result_is_loose(v);
	if (!stopped) {
		if (is_loose && loose != nullptr && !loose->files_dir.empty() && !loose->file_names.empty()) {
			// 削除するファイルの場所の行、名前の箇条書き、データのフォルダーの「削除するフォルダー:」の行、
			// 削除する前に OBS を起動したときのことの順にする。ファイルの場所の行は、中を開く
			// [フォルダーを開く] にする。削除するのはフォルダーの中のファイルで、
			// フォルダーそのものではないためである。
			p.blocks.push_back(location(keys::kDialogUninstallFilesLocationText, loose->files_dir, true));
			p.blocks.push_back(paragraph(keys::kDialogUninstallFilesText));
			for (const std::string &name : loose->file_names)
				p.blocks.push_back(bullet(ledger::Label::from_literal(name)));
			if (!loose->data_dir.empty())
				p.blocks.push_back(folder_to_delete_line(loose->data_dir));
			// 削除するものは、箇条書きのファイルとフォルダーの複数である (en の them)。
			// 直前の「削除するフォルダー:」の 1 つだけを指すと読ませないためである。OBS が読み込むのは、
			// 箇条書きの cordmux.dll のほうである。
			p.blocks.push_back(paragraph(keys::kDialogUninstallRestartWarningFilesText));
		} else if (is_loose) {
			// 場所を読めなかったときは、削除するファイルの名前を、zip の cordmux フォルダーと同じだと伝える。
			p.blocks.push_back(paragraph(keys::kDialogUninstallDoneLooseText));
		} else {
			// 削除するフォルダーの場所の行。主文が勧める操作のボタン ([フォルダーの場所を開く]) は、
			// この行の右に置く。
			p.blocks.push_back(folder_to_delete_line(v.folder_to_delete->path));
			p.blocks.push_back(paragraph(keys::kDialogUninstallRestartWarningText));
		}
		// cordmux の録画の場所。フォルダーが在り空でないときだけ値を持つ (install/uninstall.h の
		// record_dir_if_has_files)。[フォルダーを開く] を付ける。
		if (v.record_dir.has_value() && !v.record_dir->path.empty())
			p.blocks.push_back(location(keys::kDialogRecordingsHereText, *v.record_dir));
	}

	// 右下は、閉じる [OK] だけにする。既定のボタンで、Esc と [x] も [OK] になる。次の操作の [フォルダーを開く] は、
	// 場所の行の右にある。
	DialogButton ok;
	ok.kind = DialogButton::Kind::Ok;
	ok.is_default = true;
	ok.is_escape = true;
	p.buttons.push_back(ok);
	return p;
}

} // namespace cordmux::dock

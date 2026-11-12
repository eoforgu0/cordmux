// SPDX-License-Identifier: Apache-2.0

// インストールとアンインストールのダイアログの中身 (Qt を含まない側)。何をどの順に表示するか、どのボタンを置くか、
// どのアイコンにするかをここで決め、dock/install_dialogs.* はそれを ui::MessageSpec に変換するだけである。
// Qt を使わない単体テストで確認できるように、Qt にも libobs にも依存しない (align_tests に入る)。
// テキストは ini のキー (ledger::TextRef) か、ファイルシステムから来た文字列 (場所) で持ち、画面のテキストにはしない
// (テキストにするのは install_dialogs.cpp の ui::resolve)。OBS 自身のテキスト ([OK] と [キャンセル]) は、
// 種類だけを持つ。

#pragma once

#include <string>
#include <vector>

#include "dock/install_flow.h"
#include "ledger/dock_view.h"
#include "ledger/view.h"

namespace cordmux::dock {

enum class DialogIcon { Warning, Information };

// ダイアログの本文の 1 つの要素 (渡した順に並べる。ui::MessageItem と 1 対 1 に対応する)。
struct DialogBlock {
	enum class Kind { Paragraph, Bullet, Location };
	Kind kind = Kind::Paragraph;
	// Paragraph と Bullet のテキスト。Location では前に置くテキスト (キーもテキストも空なら、前に何も置かない)。
	ledger::Label text;
	std::string path;         // Location のパス (Windows の区切り文字)
	bool open_folder = false; // Location の行の右に [フォルダーを開く] を置くか
	// Location の行が、直前の箇条書きの項目の場所か (残ったもののグループで、項目の下に置く行。ledger の
	// item_locations)。true なら、dock/ui が場所の行を箇条書きのテキストの位置にそろえ、項目との間を小さくする
	// (ui::MessageItem::location の under_bullet)。グループの外の場所の行 (録画の場所の 2 行目から) では
	// true にしない。
	bool under_bullet = false;
	// Location の場所が 1 つのファイルか (ledger の LocationLine::kind == File)。true なら open_folder は使わず、
	// ui::MessageItem::file_location にする (行の右に [ファイルの場所を開く] を置く)。
	bool file = false;
	// この要素が、ダイアログの 2 つ目からのグループの始まり (途中で止まったときの結果のダイアログで、
	// グループの見出しの段落) か。true なら、dock/ui がこの要素の前を、まとまりの間の余白 (0.75 em。
	// ui::MessageItem::group_start) にし、グループの中の余白 (0.25 em と 0.5 em) より広くする。
	// ドックの止まったときのカードで、グループの間を余白で見せる (ledger の body_group_starts) のと同じ
	// 見せ方にする。1 つ目のグループの見出しでは true にしない (前の要素は cordmux がしたことか主文で、
	// グループの境ではない)。
	// グループの見出しを太字にしないのは、カードは余白で見せるので、同じ内容に見せ方が 2 つできるためである。
	// DialogPlan にグループの始まりの番号の一覧を持たせないのは、blocks は渡した順の 1 つの一覧なので、
	// 要素ごとのフラグのほうが順を取り違えないためである。
	bool group_start = false;
	// Location の場所が、中を開くのではなく、親フォルダーの中で選択して見せるフォルダーか (アンインストールが
	// 終わったときの「削除するフォルダー:」の行だけ)。true なら open_folder と file は見ず、
	// ui::MessageItem::folder_location_in_parent にする (行の右に [フォルダーの場所を開く] を置く。テキストは
	// DialogPlan::open_folder_location_text、開く関数は MessageSpec::reveal_file)。
	// 利用者はそのフォルダーを丸ごと削除するので、親を開いて選択した状態にする。中を開くと、削除するものが見えず、
	// 1 つ上に戻る操作が必要で、中ですべてを選択して削除すると、空のフォルダーが残る。ボタンのテキストは、
	// エクスプローラーと同じく、ファイルなら「ファイルの場所を開く」、フォルダーなら「フォルダーの場所を開く」
	// にする。ほかの場所の行 (録画の場所、途中で止まったときの削除しないでほしいフォルダー、切り替えの確認) は、
	// 見て確認する場所で削除するものではないので、中を開く [フォルダーを開く] のままにする。同じダイアログの録画の
	// 場所の行と同じテキストで違う動作をしないように、テキストも変える。
	bool folder_location = false;
};

// ダイアログの右下のボタン (渡した順に並べる)。
struct DialogButton {
	enum class Kind {
		Confirm,    // 主文に答える動詞 ([アンインストール] か [切り替え])。テキストは text
		Cancel,     // OBS の [キャンセル]
		Ok,         // OBS の [OK]
	};
	// 押しても閉じない [フォルダーを開く] は、ボタンの列に置かず、フォルダーの場所の行ごとに、その行の右に置く
	// (ファイルの場所の行には [ファイルの場所を開く] を置く)。Windows のダイアログボックスのガイドラインは、
	// [適用] 以外のコミットボタンはダイアログを閉じるとし、コミットボタンとほかのボタンを分けるとしているので、
	// 閉じないボタンを閉じるボタンの列に混ぜない。場所の行が 2 つあるダイアログでも、どちらを開くかが分かる。
	// https://learn.microsoft.com/en-us/windows/win32/uxguide/win-dialog-box
	Kind kind = Kind::Ok;
	ledger::TextRef text; // Confirm のテキスト (Cancel と Ok は OBS のテキストなので空)
	bool is_default = false;
	bool is_escape = false;
};

struct DialogPlan {
	ledger::TextRef title;
	DialogIcon icon = DialogIcon::Warning;
	ledger::TextRef main;
	std::vector<DialogBlock> blocks;
	std::vector<DialogButton> buttons;
	// 場所の行の右に置く [フォルダーを開く] のテキスト (open_folder が true の行がなければ使わない)。
	ledger::TextRef open_folder_text;
	// ファイルの場所の行の右に置く [ファイルの場所を開く] のテキスト (file が true の行がなければ使わない)。
	// 結果のダイアログだけで使い、2 つの確認のダイアログには、ファイルの場所の行がない。
	ledger::TextRef open_file_location_text;
	// 「削除するフォルダー:」の行の右に置く [フォルダーの場所を開く] のテキスト (folder_location が true の行が
	// なければ使わない)。アンインストールが終わったときのダイアログだけで使う。
	ledger::TextRef open_folder_location_text;
};

// パスの区切り文字を、Windows の区切り文字 (\) にする。エクスプローラーのアドレスバーに貼り付けられる
// 形にするためである。cordmux が "/" でつないだパスと、OS から来たパスが混ざるので、表示する直前に 1 か所でそろえる。
std::string windows_path(const std::string &path);

// アンインストールの確認のダイアログ。ボタンは [アンインストール] (0 番) と [キャンセル] (1 番。既定のボタンで、
// Esc でも選ばれる)。loose は、削除するフォルダーが 1 つに決まらないインストール (DLL のフォルダーが plugins\<名前> でない置き方) か。
// true なら、最後の文を、ファイルとフォルダーの場所を表示するという文にする (アンインストールが終わったときの
// ダイアログが表示するものとそろえる)。呼び出し元は、結果のダイアログと同じ判断 (install::real_folder_to_delete() が
// 空か) で選ぶ。
DialogPlan uninstall_confirm_plan(bool loose = false);

// ほかの Vencord から切り替える確認のダイアログ。ボタンは [切り替え] (0 番) と [キャンセル] (1 番。既定のボタンで、
// Esc でも選ばれる)。
DialogPlan switch_confirm_plan(const std::string &current_vencord_dir);

// アンインストールの結果のダイアログ。v は ledger の decide_dock が返す DockViewV2::uninstall_result。残ったものの
// グループの項目の下には、LeftoverGroup::item_locations の場所の行を、前に何も置かずに並べる。
// loose は、手順書と違う置き方でアンインストールが終わったときに表示する、利用者に削除してもらうファイルの場所と名前、
// データのフォルダー (dock/install_flow.h の LooseInstallFiles)。そのときだけ使い、nullptr なら、zip の cordmux フォルダーと同じ名前だという
// 1 文にする。表示する順は、ファイルの場所の行、ファイルの名前の箇条書き、データのフォルダーの「削除するフォルダー:」の
// 行、削除する前に OBS を起動したときのことである。名前は同梱のファイルの一覧
// (cmake/cordmux_ffmpeg_runtime.cmake) のままで、言語によって変えない。
// loose を View の外から渡すのは、ledger の View (UninstallResultView) がこの情報を持たないためである。この情報だけは、
// ドック、設定ウィンドウ、結果のダイアログが同じ View から表示する、という形の例外になり、ドックのアンインストールが
// 終わったときのカード (手順書と違う置き方では名前だけを伝える 1 文) は、ダイアログと同じ場所を表示しない。
// その 1 文だけにしないのは、zip と手順書を削除した利用者が作業を進められず、取り違えて OBS の
// DLL を削除するおそれがあるためである。
DialogPlan uninstall_result_plan(const ledger::dockview::UninstallResultView &v,
				 const LooseInstallFiles *loose = nullptr);

// 結果のダイアログが、削除するフォルダーの場所の行を表示できない形 (手順書と違う置き方) か。
bool uninstall_result_is_loose(const ledger::dockview::UninstallResultView &v);

} // namespace cordmux::dock

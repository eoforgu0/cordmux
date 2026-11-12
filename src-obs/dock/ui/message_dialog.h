// SPDX-License-Identifier: Apache-2.0

// 確認・結果・失敗のダイアログ (make_message_dialog、run_message_dialog) と、操作を始めた範囲だけを止めて
// ダイアログを表示する仕組み (run_owner_only_dialog)。
// QMessageBox は使わず、同じ形 (アイコン、主文、補足、ボタン) の QDialog で作る。QMessageBox は表示するときに
// QAccessible::Alert のイベントを出し、Qt はそのイベントで Windows の音を鳴らすため (OBS がデスクトップの音声を
// 取り込んでいると、その音が配信に入る)。主文と補足は、ダイアログのアクセシブルな説明 (accessibleDescription)
// に入れる。テキストはすべて呼び出し元が ini から読んで渡す (このファイルは ini のキーを持たない)。

#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "dock/ui/dock_style.h"

namespace cordmux::dock::ui {

enum class MessageIcon { Warning, Information };
enum class MessageButtonRole { Accept, Reject, Action };

// ダイアログが開いている間に止める範囲。
enum class MessageBlocking {
	// 親のウィンドウとその祖先のウィンドウを止める (Qt::WindowModal)。製品のダイアログはどれも OwnerOnly を
	// 指定する。この値は、OwnerOnly と動作を比べるために残している。OwnerOnly を指定しても、make_message_dialog の
	// owner が nullptr なら Qt::WindowModal で表示する。
	OwnerAndParents,
	// 操作を始めた範囲だけを止める。ダイアログは親のウィンドウの子のウィンドウ (いつも親の上に表示し、
	// タスクバーに別のボタンを持たない) として、モーダルにしないで表示し、開いている間は止める範囲
	// (owner_hold_region) への入力だけを捨てる。止める範囲は、操作を始めたウィジェットがドックの中ならその
	// ドック (QDockWidget。メインウィンドウに入っていてもフローティングでも)、ほかはそのウィンドウ。範囲を
	// 操作しようとすると、ダイアログを前面に出す。範囲は setEnabled(false) にしない (灰色にしない)。範囲の外 (OBS の
	// メインウィンドウ、ほかのドック) は使えるままにする (配信中に OBS の操作を止めないため)。タイトルバーの [x] と
	// Alt+F4 で範囲を閉じる操作も捨てる (コードからの close は通すので、OBS の終了処理は止めない)。どの入力を
	// 捨てるかは dock_ui_calc.h の owner_hold_action が決める。
	// 次の方法は使わない。
	//   - Qt::WindowModal: Qt は親とその祖先のウィンドウをすべて止める。設定ウィンドウの親は OBS の
	//     メインウィンドウなので、配信中にメインウィンドウとドックが止まる。
	//   - 設定ウィンドウを親のないウィンドウにする: OBS のメインウィンドウをクリックすると、設定ウィンドウが
	//     その後ろに隠れ、タスクバーに別のボタンが表示される。
	//   - Windows の API (EnableWindow) で親のウィンドウへの入力を止める: 入力を止めたウィンドウをクリックすると、
	//     Windows が音を鳴らす。
	//   - 親を setEnabled(false) にする: 中のウィジェットがすべて灰色になる。
	//   - ドックから始めたダイアログの範囲を、ドックのウィンドウ (window()) にする: メインウィンドウに入った
	//     ドックの window() はメインウィンドウそのものなので、メインウィンドウ全体を止めてしまう。
	OwnerOnly,
};

struct MessageButton {
	QString text;
	MessageButtonRole role = MessageButtonRole::Action;
	bool is_default = false;
	bool is_escape = false;
	// 空でなければ、押すとこのフォルダーをエクスプローラーで開き、ダイアログは閉じない ([フォルダーを開く])。
	// 開き方は ui::open_folder (存在しなければ、存在する親を開く。open_location.h)。
	QString open_path;
};

// コピーできる 1 行のパス。heading は見出しのテキスト (「削除するフォルダー:」)。open_folder が true で、
// MessageSpec::open_folder_text が空でなければ、行の右に [フォルダーを開く] を置く。ボタンを置く行は、パスの
// 幅をボタンと間隔の分だけ狭めて測る (ダイアログの幅が 45 em の上限を超えないように)。ボタンのアクセシブルな
// 説明は開くパスにする (同じ名前のボタンが並んでも聞き分けられるように)。
struct MessageLocation {
	QString heading;
	QString path;
	bool open_folder = true;
};

// 見出しと箇条書き (「削除できなかったもの:」など)。2 つ目からのグループの前は、まとまりの間隔にする
// (MessageItem::group_start と同じ)。
struct MessageGroup {
	QString heading;
	QStringList items;
};

// 補足を渡した順に並べるための 1 つのブロック (文、箇条書き、場所の行を交互に並べられる)。
struct MessageItem {
	enum class Kind { Text, Bullet, Location };
	Kind kind = Kind::Text;
	QString text; // Text と Bullet のテキスト。Location では見出しのテキスト (空なら見出しを表示しない)
	QString path; // Location のパス
	bool open_folder = false; // Location の行の右に [フォルダーを開く] を置くか
	// Location の行が、直前の箇条書きの項目のパスか (アンインストールが途中で止まったときの「- cordmuxの設定の
	// ファイル」の下の場所の行など)。true なら、行を箇条書きのテキストの列 (記号の右) にそろえ、箇条書きとの間を
	// 小さい余白 (0.25 em) にする。
	bool under_bullet = false;
	// Location のパスが 1 つのファイルか (ledger::dockview::LocationKind::File)。true なら open_folder は見ず、
	// MessageSpec::open_file_location_text と reveal_file が両方あるときだけ、行の右に [ファイルの場所を開く]
	// (エクスプローラーでそのファイルを選択した状態で開く) を置く。
	bool file = false;
	// このブロックが、ダイアログの 2 つ目からのグループの始まり (アンインストールが途中で止まったときの結果の
	// ダイアログの「元に戻せなかった設定:」などのグループの見出し) か。true なら、このブロックの前 (Location なら
	// 見出しの前) をまとまりの間隔 (0.75 em = DockMetrics::group_gap_px) にし、グループの中の間隔 (0.25 em と
	// 0.5 em) より広くする。同じ間隔だと、2 つ目のグループが前のグループの続きの説明に見えるため。ダイアログの
	// 最初のブロックでは見ない。グループの見出しを太字にする方法は使わない。ドックのカードはグループを間隔で
	// 分けて見せるので、同じ構成の内容に 2 つの見せ方ができるため。
	bool group_start = false;
	// Location のパスが、中を開くのではなく、親のフォルダーの中で選択して表示するフォルダーか (結果の
	// ダイアログの「削除するフォルダー:」の行だけ)。利用者はそのフォルダーを丸ごと削除するので、中を開くと
	// 削除するものが見えず、1 つ上へ戻る操作が必要になる。中ですべてを選んで削除すると、空のフォルダーが残る。
	// true なら open_folder と file は見ず、MessageSpec::open_folder_location_text と reveal_file が両方ある
	// ときだけ、行の右に [フォルダーの場所を開く] (エクスプローラーで親のフォルダーを開き、そのフォルダーを
	// 選択した状態にする) を置く。開く関数は reveal_file (platform::open_file_location は、存在するフォルダーも
	// 親を開いて選択する。存在しなければ、存在するいちばん近い親を開く)。テキストは Windows のエクスプローラーの
	// 右クリックのメニューの項目と同じ組 (ファイルは「ファイルの場所を開く」、フォルダーは「フォルダーの場所を
	// 開く」) で、ボタンのテキストが動作を表す。
	// テキストを [フォルダーを開く] のまま動作だけを変える方法は使わない。同じダイアログの録画の場所の行の
	// [フォルダーを開く] は中を開くので、同じテキストのボタンが、行によって違う動作をするため。
	bool folder_location = false;

	static MessageItem paragraph(const QString &t)
	{
		return MessageItem{Kind::Text, t, QString(), false, false, false, false, false};
	}
	static MessageItem bullet(const QString &t)
	{
		return MessageItem{Kind::Bullet, t, QString(), false, false, false, false, false};
	}
	static MessageItem location(const QString &heading, const QString &p, bool open = false,
				    bool under_bullet = false)
	{
		return MessageItem{Kind::Location, heading, p, open, under_bullet, false, false, false};
	}
	// ファイルの場所の行。
	static MessageItem file_location(const QString &heading, const QString &p, bool under_bullet = false)
	{
		return MessageItem{Kind::Location, heading, p, false, under_bullet, true, false, false};
	}
	// 親のフォルダーの中で選択して表示するフォルダーの場所の行。
	static MessageItem folder_location_in_parent(const QString &heading, const QString &p,
						     bool under_bullet = false)
	{
		return MessageItem{Kind::Location, heading, p, false, under_bullet, false, false, true};
	}
};

// ダイアログの内容。ダイアログの形は次のとおり。
//   - 親は操作を始めたウィンドウ (ドックから始めたならドックのあるウィンドウ、設定ウィンドウから始めたなら
//     設定ウィンドウ) にし、親の中央に表示する。開いている間に止める範囲は blocking で選ぶ。
//   - タイトルは操作の名前。左上に Windows の標準のアイコン (QStyle::standardIcon の警告か情報) を置く。
//     疑問符のアイコンは使わない。
//   - 主文は 1 文で、補足より大きいフォントにする (DockStyle::dialog_main_scale)。
//   - 補足は本文の大きさ。3 つ以上並ぶものは 1 行ずつの箇条書きにする。パスは文の中の括弧に入れず、1 行に
//     独立させ、選択してコピーできる形にする。
//   - ボタンは右下に、渡した順に並べる。フォーカスは既定のボタンに置く。Esc とタイトルバーの [x] は、Esc の
//     ボタンを押したのと同じにする。
//   - 本文は 1 行 40 em 前後で折り返す (ダイアログの幅の上限は 45 em)。最後の行が 2 文字以下にならず、列の
//     4 分の 1 より短くならない幅にする (値は DockStyle)。すべてのラベルで 1 つの列の幅を探し、その幅で
//     合わないラベルだけが、ラベルの中でテキストを折り返す幅を狭める。ラベルは dock/ui/wrap_label.h の
//     WrapLabel。
//   - ブロックの間隔は、見出しの段落とその箇条書き、箇条書きどうし、場所の行の見出しと場所の行の間が 0.25 em、
//     ほかが 0.5 em。2 つ目からのグループの始まり (MessageItem::group_start と、groups の 2 つ目から) の前は
//     0.75 em。
//   - 箇条書きの記号はラベルの左の余白に描く (アクセシブル名にも説明にも記号を入れない)。
//   - ダイアログの高さは中身から決まり、利用者は大きさを変えられない。中身の高さが、表示する画面の作業領域
//     (タスクバーを除く) からダイアログの枠を引いた高さを超えるときだけ、主文とボタンの間の本文 (補足、
//     箇条書き、場所の行、コピーできる入力欄) をダイアログの中で縦にスクロールする (dock_ui_calc.h の
//     dialog_height_fit)。アイコン、主文、ボタンはいつも見える。スクロールする間だけ、スクロール領域
//     (objectName は cordmuxMessageScroll。背景も枠もない) がタブストップになり (本文の最初のタブストップの
//     直前。ほかのタブストップの順は変えない)、上下の矢印キーと PageUp・PageDown でスクロールする。
//     スクロール領域のアクセシブル名は、ダイアログのタイトルにする。スクロールしないダイアログは、見た目も
//     Tab の順も変わらない (本文は余白 0 のコンテナー cordmuxMessageBody に入るだけ)。ダイアログが別の画面へ
//     移ったときと、作業領域が変わったときに合わせ直す (位置は動かさない)。
//   - 「今後表示しない」は置かない。閉じたら、開く前にフォーカスのあったウィジェットへフォーカスを戻す
//     (削除されたか押せなければ focus_fallback へ移す)。
struct MessageSpec {
	QString title; // 操作の名前
	// ダイアログの ID。空でなければ動的なプロパティ dialogId に入れる。ドックを操作するテスト
	// (tests/dock_drive/) とプレビューツールが、run_message_dialog で開いたダイアログを見分けるために使う。
	QString dialog_id;
	// 開いている間に止める範囲。既定は OwnerOnly (呼び出し元が指定し忘れても、メインウィンドウを止めないため)。
	MessageBlocking blocking = MessageBlocking::OwnerOnly;
	MessageIcon icon = MessageIcon::Warning;
	// 1 文 (大きいフォント)。句点を付けない (docs/ui_guidelines.md の「UI 要素ごとの書き方」)
	QString main;
	QString supplement; // 補足の最初の段落
	// 補足の続き。渡した順に並べる (文、箇条書き、場所の行)。
	std::vector<MessageItem> items;
	// 次の 3 つは、items の後にこの順で並ぶ (並べる順を指定しなくてよい簡単なダイアログのため)。
	QStringList bullets;
	std::vector<MessageGroup> groups;
	std::vector<MessageLocation> locations;
	QStringList copy_lines;              // 空でなければ、コピーできる入力欄と [コピー] を置く
	QString open_folder_text, copy_text; // [フォルダーを開く] と [コピー] のテキスト (呼び出し元が ini から読む)
	// [ファイルの場所を開く] のテキスト (呼び出し元が ini のキー cordmux.dialog.open_file_location.button から
	// 読む)。押したときにエクスプローラーでそのファイルを選択して開く関数は reveal_file (Windows の API は
	// platform/ に閉じ込めるので、呼び出し元が渡す)。どちらかが空なら、ファイルの行にボタンを置かない。
	QString open_file_location_text;
	// [フォルダーの場所を開く] のテキスト (ini のキー cordmux.dialog.open_folder_location.button)。
	// MessageItem::folder_location の行の右に置く。押したときの関数は reveal_file と同じ。どちらかが空なら、
	// その行にボタンを置かない ([フォルダーを開く] で代えると、テキストと動作が食い違うため)。
	QString open_folder_location_text;
	// パスを親のフォルダーの中で選択した状態で開く関数。ファイルの場所の行とフォルダーの場所の行
	// (folder_location) の両方が使う (製品は platform::open_file_location を渡す。存在するファイルも
	// フォルダーも、親を開いて選択する)。
	std::function<void(const QString &)> reveal_file;
	std::vector<MessageButton> buttons;  // 右下。元に戻せない操作の確認では、既定と Esc を [キャンセル] に
	// 閉じた後、開く前にフォーカスのあったウィジェットが削除されたか押せないときに、フォーカスを移す先
	// (同じウィンドウの中の、削除されないウィジェット)。
	QPointer<QWidget> focus_fallback;
};

// ダイアログを開かずに作る (プレビューツールとテストが描くため)。owner のウィンドウを親にするので、ダイアログは
// そのウィンドウと一緒に削除される。owner が nullptr なら、呼び出し元が削除する。押したボタンの番号は
// message_dialog_choice で読む。
QDialog *make_message_dialog(QWidget *owner, const MessageSpec &spec, const DockStyle &style = DockStyle());
// 押されたボタンの番号 (まだ押されていないときと、make_message_dialog のダイアログでないときは -1)。
int message_dialog_choice(const QDialog *dialog);

// ダイアログを表示する画面の作業領域の高さ available_px と、ダイアログの枠 (タイトルバーと上下の枠) の高さ
// frame_px (どちらも論理 px) を固定し、その大きさでダイアログの高さを合わせる。テストとプレビューツールが、仮の
// 画面の大きさでダイアログを描くために使う。固定した値は、ダイアログを表示した後 (showEvent) と画面を移った後も
// 使う。available_px が 0 以下なら固定をやめ、次に合わせるとき (表示するときか、画面が変わったとき) から、
// ダイアログのある画面から読む。戻り値は、本文をスクロールする形になったか。make_message_dialog のダイアログで
// なければ、何もせずに false を返す。製品は呼ばない (ダイアログが自分で、表示する画面の
// QScreen::availableGeometry の高さと、枠の高さ (QWindow::frameMargins の上と下の和。測れなければ 40 px) で
// 合わせる)。
bool set_message_dialog_work_area(QDialog *dialog, int available_px, int frame_px);
// 本文をスクロールする形か (make_message_dialog のダイアログでなければ false)。
bool message_dialog_scrolls(const QDialog *dialog);

// spec.blocking の範囲を止めてダイアログを表示し、押したボタンの番号を返す (Esc と [x] は Esc のボタンの番号。
// Esc のボタンがなければ -1)。閉じたら、開く前のフォーカスに戻す。ダイアログは戻る前に削除する。
// OwnerOnly では run_owner_only_dialog と同じ形で待ち、開いている間も、止める範囲の外 (OBS のメインウィンドウ
// など) は使えるままである。そのため、ドックの操作や OBS の終了で親のウィンドウが削除されると、ダイアログも
// 一緒に削除され、-1 を返す。呼び出し元は、戻った後に自分 (owner) がまだ存在するかを QPointer で確認してから、
// 自分のメンバーに触る。
int run_message_dialog(QWidget *owner, const MessageSpec &spec, const DockStyle &style = DockStyle());

// OwnerOnly のときに止める範囲。owner かその祖先 (同じウィンドウの中) に QDockWidget があれば、いちばん近い
// その QDockWidget、なければ owner のウィンドウ (window())。owner が nullptr なら nullptr を返す。
QWidget *owner_hold_region(QWidget *owner);

// 呼び出し元が作ったダイアログ dialog (QColorDialog など) を、OwnerOnly と同じ形で表示し、閉じるまで待つ。
// 止めるのは owner_hold_region(owner) の範囲だけで、ダイアログは Qt のモーダルにしない (windowModality は
// NonModal、動的なプロパティ messageBlocking は "ownerOnly")。dialog に親がなければ owner のウィンドウを親に
// する (いつも親の上に表示し、タスクバーに別のボタンを持たない)。戻り値は閉じたときの QDialog::result()。
// 開いている間に dialog が削除されたら (親のウィンドウが削除されて一緒に削除されたとき) -1 を返す。owner が
// nullptr なら、止める範囲を決められないので、Qt の既定どおり exec で待つ。dialog は削除しない (選んだ値を
// 呼び出し元が読むため。削除するのは呼び出し元)。閉じた後のフォーカスは、run_message_dialog と同じく、開く前に
// フォーカスのあったウィジェットへ戻す (削除されたか押せなければ戻さない)。呼び出し元は、戻った後に自分 (owner)
// と dialog がまだ存在するかを QPointer で確認してから触る (開いている間も範囲の外は使えるので、OBS の終了や
// ドックの操作で削除されることがある)。
// QColorDialog::getColor と QFileDialog の静的な関数は使わない。中で exec を呼び、アプリケーション全体か、親と
// その祖先のウィンドウをすべて止めるので、配信中に OBS のメインウィンドウが止まるため。
int run_owner_only_dialog(QDialog *dialog, QWidget *owner);

// widget (ウィンドウそのものか、その中のウィジェット) が、OwnerOnly のダイアログが止めている範囲の中にあれば、
// そのダイアログを返す (重なっていれば、後から開いたダイアログ)。範囲の外なら nullptr を返す。たとえば
// ドックから始めたダイアログでは、ドックの中身、その QDockWidget、ドックの中のウィジェットはダイアログを返し、
// ドックがメインウィンドウに入っていても、OBS のメインウィンドウそのものとメインウィンドウのほかのウィジェットは
// nullptr を返す。設定ウィンドウから始めたダイアログでは、設定ウィンドウとその中のウィジェットはダイアログを
// 返し、メインウィンドウとドックは nullptr を返す。止め方の名前 ("ownerOnly" か "ownerAndParents") は、
// ダイアログの動的なプロパティ messageBlocking にも入っている。
QDialog *message_dialog_holding(const QWidget *widget);

} // namespace cordmux::dock::ui

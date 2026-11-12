// SPDX-License-Identifier: Apache-2.0

// 設定ウィンドウ (「cordmuxの設定」)。形は OBS の [設定] と同じで、左にアイコンと名前のページの一覧、右に
// スクロール領域の中の入力欄、下に [OK]、[キャンセル]、[適用] を置く。ページは [録画]、[映像の受信]、[詳細設定]、
// [バージョン情報] の 4 つである。配信の画面を見ながら動かす値 (枠、名前ラベル、話している人の強調、ダミー映像) は、ドックの
// 表示タブにある。配信中にもドックを使えるように、
// モーダルにしない。テキストは ini のキー (ledger/text_ids.h の text::keys) から読み、[OK]、[キャンセル]、
// [適用] は OBS の ini (obs_frontend_get_locale_string) から読む。libobs から使うのは obs_module_text と
// obs_frontend_get_locale_string の 2 つだけなので、プレビューツールでも作れる (make_settings_window)。

#pragma once

#include <QDialog>
#include <QPointer>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bundle/export_bundle.h"
#include "config/settings.h"
#include "dock/ui/message_dialog.h"
#include "ledger/dock_view.h"
#include "ledger/observation.h"

class QAbstractButton;
class QCheckBox;
class QDialogButtonBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace cordmux::dock {

namespace ui {
class FocusRing;
class WrapLabel;
} // namespace ui

// ウィンドウを開くときに渡す値。
struct SettingsWindowInput {
	config::Settings current;            // ウィンドウで編集する値の元 (今使っている値)
	config::Settings effective_at_start; // 再起動を求める文で比べる元 (この起動で使っている値)
	ledger::SettingsPage page = ledger::SettingsPage::Recording;
	ledger::dockview::AboutPageView about;
	// ウィンドウで編集中の保存先の値から、今録画を始めたら書く場所を返す (Pipeline::preview_record_base_dir)。
	// 関数がないか空を返したときは、[録画] のページの [フォルダーを開く] を押せなくする。
	std::function<std::string(const std::string &record_dir_setting)> record_dir_now;
	// [適用] と [OK] で呼び出す。保存できたら true を返す。false ならウィンドウを閉じず、失敗のダイアログを開く。
	std::function<bool(const config::Settings &)> on_apply;
	// 「cordmux について」のページのボタンで呼び出す。owner はこのウィンドウ (確認、結果、失敗のダイアログの親)。
	std::function<void(QWidget *owner)> on_export;
	std::function<void(QWidget *owner)> on_uninstall;
	// ウィンドウを閉じたときに 1 回呼び出す (どの閉じ方でも)。geometry は QWidget::saveGeometry の base64 で、
	// settings_window_geometry に保存する値。
	std::function<void(const std::string &geometry)> on_closed;
	// 前に閉じたときの大きさと位置 (settings_window_geometry)。空なら中身から決めた大きさで、親ウィンドウの中央に表示する。
	std::string geometry;
	// エクスポートの処理中か (開いた時点で前の処理が続いているとき)。
	bool export_busy = false;
	// ファイルの場所の行の [ファイルの場所を開く] で呼び出す。エクスプローラーで、そのファイルを選んだ状態で開く
	// (platform::open_file_location)。Windows の API は platform/ の中だけで使い (windows_api_confinement.py がチェック
	// する)、プレビューツールは platform/ をビルドしないので、呼び出し元が渡す。空なら、ファイルのあるフォルダーを開く
	// (押しても何も起きない形にしない)。
	std::function<void(const std::string &path)> reveal_file;
};

// 値の反映:
//   - 入力欄を変えても、すぐには反映しない。[適用] か [OK] で、ウィンドウの中の設定を呼び出し元
//     (SettingsWindowInput::on_apply) に渡す。呼び出し元は、ウィンドウが入力欄を持つ項目 (settings_fields.h の
//     SettingsOwner::SettingsWindow の行) だけをコピーする。ドックの表示タブの値とメインアカウントは、ウィンドウを開いた後に
//     変わっていても、古い値で上書きしない。保存できなかったらウィンドウを閉じず、このウィンドウを親にした失敗の
//     ダイアログを開く。
//   - [キャンセル]、Esc、タイトルバーの [x] は、ウィンドウの中で変えてまだ反映していない値を捨てて閉じる。ウィンドウの
//     中から実行した操作 (診断情報のエクスポート、アンインストール、[フォルダーを開く]、[ファイルの場所を開く]) は
//     戻さない。
//   - 再起動が必要な入力欄 (settings_fields.h の restart_required) の値が、この起動で使っている値と違う間だけ、その
//     ページの下に再起動を求める 1 文を表示する (元の値に戻せば消える。比べる元は起動のときの値なので、[適用] の後も
//     表示したまま)。
//   - このウィンドウの値はほかの場所から変えられないので、開いている間に外から書き換えない。外から書き換えるのは
//     「cordmux について」のページ (バージョン、エクスポートの結果、アンインストールのボタン) だけ。
// 入力欄の objectName は `cordmuxField_<json_key>` にする。1 つの入力欄が 2 つ以上のキーを持つときは、キーを「+」で
// つないだ名前にする (`<キー>+<キー>`)。どの入力欄も動的なプロパティ `restartRequired`
// (bool) を持つ。プレビューツールとドックを操作するテストが、この形で入力欄を探す。
class SettingsWindow : public QDialog {
public:
	SettingsWindow(QWidget *parent, SettingsWindowInput input);
	~SettingsWindow() override;

	SettingsWindow(const SettingsWindow &) = delete;
	SettingsWindow &operator=(const SettingsWindow &) = delete;

	// 表示するページを替える (開いているウィンドウを、ドックのメニューやメッセージのボタンから開き直したとき)。
	void set_page(ledger::SettingsPage page);
	ledger::SettingsPage page() const;

	// 「cordmux について」のページを、ドックと同じ周期の View で書き換える。変わったウィジェットだけを書き換える。
	void update_about(const ledger::dockview::AboutPageView &about);

	// [診断情報をエクスポート...] を押したのと同じ処理をする (ドックのメニューから来たとき)。
	void start_export();
	// エクスポートの処理中の見た目にする (ボタンのテキストを「エクスポート中...」にして押せなくする)。
	void set_export_busy(bool busy);

	// OBS の [ツール] の「cordmuxをアンインストール...」から来たときに呼び出す。「cordmux について」のページに移り、
	// ページのいちばん下 ([cordmuxをアンインストール...] とその下の押せない理由) が見える所までスクロールする。押せる
	// なら、ページのボタンを押したのと同じこと (ボタンにフォーカスを置いて on_uninstall) をする。押せないなら確認の
	// ダイアログを開かない (理由はボタンの下に見えている)。押せるかの判定は、ページのボタンと同じ
	// windows_calc::uninstall_entry。ウィンドウを表示した後に呼び出す (スクロールの量は、置いた後の高さで決まる)。
	void start_uninstall();

	// OBS のテーマ、[フォントサイズ]、[密度] が変わったとき (THEME_CHANGED) に呼び出す。アイコンと大きさの値を読み込み直す。
	void refresh_theme();

	// 「cordmux について」のページの最初のウィジェット ([コピー])。このウィンドウから始めたアンインストールの確認と結果の
	// ダイアログを閉じた後、押したボタンが押せなくなっていたときのフォーカスの移し先 (DockPollInstallPart::uninstall の
	// focus_fallback)。
	QWidget *about_focus_fallback() const;

	// ウィンドウを開いたウィジェット (ドックのメニューの [...] など)。ウィンドウがフォーカスを持ったまま閉じたら、
	// フォーカスをここに戻す。ウィンドウを開き直すたびに、呼び出し元が渡し直す。
	void set_return_focus(QWidget *opener);

	// ウィンドウの中の現在の値 (テストとプレビューツールが読む)。
	const config::Settings &edited() const { return edited_; }

protected:
	void done(int result) override;
	bool event(QEvent *event) override;
	// Tab と Shift+Tab でのフォーカスの移動。移った先がページのスクロール領域の中なら、見える所までスクロールする。
	bool focusNextPrevChild(bool next) override;

private:
	struct Field; // 入力欄 1 つ (ウィジェットと、ウィンドウの値とウィジェットの間で値をコピーする関数)

	void build();
	QWidget *build_recording_page();
	QWidget *build_receive_page();
	QWidget *build_advanced_page();
	QWidget *build_about_page();
	QWidget *wrap_page(QWidget *content, ledger::SettingsPage page);
	QLabel *make_restart_note(ledger::SettingsPage page);

	// 入力欄を登録する (objectName と restartRequired を付け、値をコピーする関数を保存する)。
	void register_field(QWidget *widget, const std::string &keys, std::function<void()> load,
			    std::function<void()> store);
	// ウィジェットに値を入れる (ウィンドウを作ったとき)。
	void load_fields();
	// ウィジェットの値をウィンドウの設定にコピーし、[適用] と、再起動を求める文と、反映されない入力欄の見た目を更新する。
	void on_field_changed();
	void update_restart_notes();
	void update_apply_enabled();
	bool dirty() const;
	bool apply();

	void choose_record_dir();
	void update_record_location();
	void copy_versions();
	void update_lock();
	void refresh_page_icons();
	void refresh_focus_ring();
	// 説明の文 (入力欄の下の補足) の色を、テーマから決め直す。
	void refresh_notes();
	// form に説明の文の行を足す (入力欄の列にそろえる。色は refresh_notes)。
	void add_description(QFormLayout *form, const QString &text);
	QSize initial_size() const;
	QSize minimum_window_size() const;
	// ページのスクロール領域 (index 番目のページ。なければ nullptr)。
	QScrollArea *page_scroll(int index) const;
	// ページ (スクロール領域) の中身 (index 番目のページ)。
	QWidget *page_content(int index) const;
	// 右に表示するページを index 番目に替える (項目の一覧で選んだときと、set_page から呼び出す)。
	void show_stack_page(int index);
	// 「cordmux について」のページのスクロール領域 (なければ nullptr)。
	QScrollArea *about_scroll() const;
	// 「cordmux について」のページをいちばん下までスクロールする (start_uninstall)。
	void show_about_bottom();
	// スクロール領域 scroll の見えている範囲に、ウィジェット w の全体と、フォーカスリングの縦の範囲が入っているか。
	bool mark_shown_in(const QScrollArea *scroll, const QWidget *w) const;
	// mark_shown_in が false なら、ウィジェットとリングの縦の範囲が全部入るまでスクロールする (focusNextPrevChild の
	// コメント)。
	void reveal_mark(QScrollArea *scroll, const QWidget *w);
	// ウィジェット w が見えているか。「cordmux について」のスクロール領域の中のウィジェットなら mark_shown_in で判定し、
	// スクロール領域の外のウィジェット (項目の一覧、[OK] など) は表示していれば true。ドックを操作するテストの
	// shown_in_viewport (ウィジェットの全体) よりリングの幅だけ厳しく、ここで true なら shown_in_viewport も true。
	bool shown_in_about_view(QWidget *w) const;
	// アンインストールを断られたときと押せないときに、フォーカスを置き直す (update_about のコメント)。scroll が true
	// なら、先にいちばん下 (ボタンと理由) までスクロールする。preferred は押す前にフォーカスがあったウィジェット
	// (なければ nullptr)。
	void settle_focus_after_refusal(QWidget *preferred, bool scroll);
	// ページの [cordmuxをアンインストール...] が押されたとき (start_uninstall で押せるときもここを通る)。focus_before は
	// 押す前にウィンドウでフォーカスがあったウィジェット ([ツール] から来たときは、ボタンにフォーカスを置く前のもの)。
	void press_uninstall(QWidget *focus_before);

	SettingsWindowInput input_;
	config::Settings applied_;      // 最後に反映した値 ([適用] を押せるかを決めるときに比べる元)
	config::Settings edited_;       // ウィンドウの中の現在の値
	bool loading_ = false; // ウィジェットに値を入れている間は on_field_changed を実行しない
	bool closed_reported_ = false;
	bool export_busy_ = false;
	bool save_failed_ = false; // 最後の反映で保存できなかった ([OK] と [適用] でもう一度保存を試せるように)

	std::vector<Field *> fields_;

	QListWidget *page_list_ = nullptr;
	QStackedWidget *stack_ = nullptr;
	QDialogButtonBox *buttons_ = nullptr;
	QPushButton *apply_button_ = nullptr;
	std::map<ledger::SettingsPage, QLabel *> restart_notes_;
	std::vector<QWidget *> locked_pages_; // アンインストールの後に操作できなくするページ (「cordmux について」以外)
	ui::FocusRing *ring_ = nullptr;       // フォーカスリング
	int ring_reach_px_ = 3;               // リングが押せる範囲の外にはみ出す幅 (太さ + 空き。refresh_focus_ring で読む)
	QPointer<QWidget> return_focus_;      // 閉じた後にフォーカスを戻すウィジェット (set_return_focus)

	// [録画] のページ
	QLineEdit *record_dir_edit_ = nullptr;
	QPushButton *record_open_folder_ = nullptr;
	std::string record_location_path_;
	// 保存先を選んでいないときに録画ごとのフォルダーを作る場所 (record_dir_now("") の最後の答え)。入力欄の文字がこれと
	// 同じなら、選んでいないものとして扱う。OBS の録画の保存先が変わったとき、まだこの場所のままの入力欄を新しい場所に
	// 替えるためにも、前の値を持っておく。
	std::string record_default_path_;

	// 説明の文 (項目の名前でも値でもない、入力欄の下の補足) と、それを置いたフォーム。
	struct Note {
		QFormLayout *form = nullptr;
		QWidget *label = nullptr;
	};
	std::vector<Note> notes_;

	// 「cordmux について」のページ
	ledger::dockview::AboutPageView about_;
	bool about_shown_ = false;
	QWidget *versions_box_ = nullptr;
	QPushButton *copy_button_ = nullptr;
	QWidget *vesktop_box_ = nullptr; // 「診断情報」のまとまりの「起動時のVesktopの設定」の行
	QPushButton *export_button_ = nullptr;
	// エクスポートを始めたときに、終わった後でフォーカスをボタンに戻すかと、始めた時点でウィンドウのフォーカスがあった
	// ウィジェット (押せなくしたボタンから Qt がフォーカスを移した先。終わったときにフォーカスがまだそこにあれば、利用者は
	// フォーカスを動かしていない)。
	bool export_return_focus_ = false;
	QPointer<QWidget> export_focus_mark_;
	ui::WrapLabel *export_result_text_ = nullptr;
	QWidget *export_location_row_ = nullptr;
	ui::WrapLabel *export_location_ = nullptr; // 区切りの直後で折り返す、選べるラベル
	std::string export_location_path_;
	// 場所の行の右のボタン。場所がファイルなら [ファイルの場所を開く]、フォルダーなら [フォルダーを開く]。
	QPushButton *export_open_button_ = nullptr;
	bool export_location_is_file_ = false;
	QPushButton *uninstall_button_ = nullptr;
	ui::WrapLabel *uninstall_reason_ = nullptr;
	// アンインストールを押してから、押した関数から返った後の最初の周期までの状態 (ページのボタンと [ツール] の両方。
	// update_about の終わりで使う)。押した関数はアンインストールを始めると、その呼び出しの中で周期を進める
	// (dock_poll_install.cpp の after_paint) ので、段階が進んだかは、押している間の周期ごとに見る。段階はウィンドウに見える
	// 形で判断する (入力欄を操作できなくしたかと、ボタンのテキスト。アンインストールの最中のテキストは
	// 「アンインストール中...」で、rules_dock.cpp の about のコメントにある)。
	struct UninstallPress {
		bool active = false;      // 押した (押した関数から返った後の最初の周期まで)
		bool returned = false;    // 押した関数から返った
		bool stage_moved = false; // 押した後のどこかの周期で、段階が押す前と違った (アンインストールを始めた)
		bool locked_at_press = false;
		ledger::TextRef label_at_press;
		QPointer<QWidget> focus_before; // 押す前にウィンドウでフォーカスがあったウィジェット
	};
	UninstallPress uninstall_press_;
	// アンインストールを始めて押せなくなったボタンから [コピー] にフォーカスを移したときの、始める前のボタンのテキスト。
	// テキストがそれに戻った周期 (アンインストールが済んだ周期) で [コピー] までスクロールする (update_about のコメント)。
	std::optional<ledger::TextRef> reveal_copy_after_label_;
};

// ウィンドウを閉じた後、閉じたウィンドウがフォーカスを持っていたなら、フォーカスを開いたウィジェット (opener) に戻す。
// Windows は親ウィンドウを持つウィンドウを閉じると、親ウィンドウ (OBS のメインウィンドウ) を前面にするので、その後
// (イベントループの次の周回) に、開いたウィジェットのウィンドウを前面にし、そのウィジェットにフォーカスを置く。
// was_active は、閉じる直前にウィンドウがアクティブだったか (利用者が別のウィンドウに移っていたらフォーカスを奪わない)。
// owner は閉じたウィンドウの親ウィンドウ (次の周回で、それ以外のウィンドウが前面に出ていたら奪わない)。
// 設定ウィンドウと参加者の設定ウィンドウの done から呼び出す。
void return_focus_after_close(bool was_active, const QPointer<QWidget> &opener, const QPointer<QWidget> &owner);

// ウィンドウを作り、開かずに返す (プレビューツールとテストが描くため。製品は作った後に show する)。
SettingsWindow *make_settings_window(QWidget *parent, const SettingsWindowInput &in);

// 失敗のダイアログの中身。タイトルは操作の名前、主文は「〜できませんでした」、補足は理由と次の操作。書けなかったとき
// (WriteFailed) は理由を伝え、zip を作れなかったときと秘密の値が残っていたとき (ZipFailed、SecretFound) は、理由を
// 画面に表示せずに次の操作だけを伝える (利用者にできることがなく、秘密の値があることを画面に表示しないため。理由は
// ログにだけ書く)。
ui::MessageSpec export_failed_spec(bundle::ExportOutcome outcome);
ui::MessageSpec settings_save_failed_spec();

// OBS の ini のテキスト ([OK] など。ledger/text_ids.h の obs_keys)。読めなければキーそのものを返す。
QString obs_frontend_text(const char *key);

} // namespace cordmux::dock

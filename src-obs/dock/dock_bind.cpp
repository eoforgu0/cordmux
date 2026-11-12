// SPDX-License-Identifier: Apache-2.0

// ドックを OBS に追加し、初めての起動のときに表示する (dock_bind.h の実装)。

#include "dock/dock_bind.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QDockWidget>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <string>

#include "diag/diag_log.h"
#include "dock/ui/dock_metrics.h"
#include "ledger/text_ids.h"
#include "platform/platform.h"

namespace cordmux::dock {

namespace {

// OBS がドックの表示状態とレイアウトを保存するときのキー。画面に表示しない識別子なので ini には置かない。変えると、
// 利用者が決めたドックの配置が失われる。
const char *const kDockId = "cordmux";

// ドックのタイトルは画面に表示するので、ini のキー cordmux.dock.window.title (ledger/text_ids.h の
// kDockWindowTitle) から取る。

// 初めての起動でドックを表示したことを示すマーカーのファイル。中身は読まず、ファイルがあるかだけを見る。
const char *const kShownOnceMarker = "dock_shown_once";

// 初めての起動でフローティングとして表示するときの大きさ。中身が収まる大きさの目安である。em は OBS の本文の
// フォントサイズなので、OBS の [フォントサイズ] に合わせて変わる。ウィジェットの最小の大きさより小さくはしない。中身の
// sizeHint に任せないのは、QScrollArea が中身の大きさを sizeHint に含めず、一覧が 1、2 行しか見えない
// 大きさになることがあるためである。
constexpr double kInitialWidthEm = 26.0;
constexpr double kInitialHeightEm = 30.0;

// ドックの中身のウィジェットから、親の QDockWidget を取得する。親がないか QDockWidget でなければ nullptr を返す。
QDockWidget *dock_of(QWidget *widget)
{
	if (widget == nullptr)
		return nullptr;
	return qobject_cast<QDockWidget *>(widget->parentWidget());
}

} // namespace

std::string module_config_file_path(const char *name)
{
	// 先に設定フォルダーを作る。obs_module_config_path("") は末尾に区切り文字の付いたパスを返すので、
	// 区切り文字を除いてから os_mkdirs に渡す。os_mkdirs の実装には、末尾の区切り文字を
	// 想定していないものがあるためである。
	char *dir = obs_module_config_path("");
	if (dir != nullptr) {
		std::string dir_path(dir);
		bfree(dir);
		while (!dir_path.empty() && (dir_path.back() == '/' || dir_path.back() == '\\'))
			dir_path.pop_back();
		if (!dir_path.empty())
			os_mkdirs(dir_path.c_str());
	}

	char *path = obs_module_config_path(name);
	if (path == nullptr)
		return {};
	// ポータブル版の OBS では、libobs が相対パスを返す。画面に出すときと同じ場所を指すように、絶対パスにして返す
	// (platform::absolute_path)。
	std::string out = platform::absolute_path(path);
	bfree(path);
	return out;
}

std::string module_config_dir()
{
	char *dir = obs_module_config_path("");
	if (dir == nullptr)
		return {};
	// ポータブル版の OBS では相対パスになる。アンインストールのダイアログは、この場所を利用者に見せるので、絶対パスにする
	// (platform::absolute_path)。
	std::string dir_path = platform::absolute_path(dir);
	bfree(dir);
	// module_config_file_path と同じく、末尾の区切り文字を除く。呼び出し元が区切り文字を付けてから
	// ファイル名をつなぐ。
	while (!dir_path.empty() && (dir_path.back() == '/' || dir_path.back() == '\\'))
		dir_path.pop_back();
	return dir_path;
}

bool add_dock(QWidget *widget)
{
	if (widget == nullptr)
		return false;
	return obs_frontend_add_dock_by_id(kDockId, obs_module_text(ledger::text::keys::kDockWindowTitle), widget);
}

void show_dock_once_after_loading(QWidget *widget)
{
	if (widget == nullptr)
		return;

	const std::string marker = module_config_file_path(kShownOnceMarker);
	if (marker.empty()) {
		diag::logf(LOG_WARNING, "Didn't show the dock on first start: the cordmux settings folder is unknown");
		return;
	}
	if (os_file_exists(marker.c_str()))
		return; // 2 回目からの起動。表示状態は OBS が保存している

	// ウィジェットは OBS が破棄するので、表示する前に破棄されていたら何もしない。
	QPointer<QWidget> guard(widget);
	QTimer::singleShot(0, widget, [guard, marker] {
		if (guard.isNull())
			return;
		QDockWidget *dock = dock_of(guard.data());
		if (dock == nullptr) {
			// マーカーを書かずに戻り、次の起動でもう一度試す。
			diag::logf(LOG_WARNING,
				   "Didn't show the dock on first start: its dock window wasn't found (trying again "
				   "next start)");
			return;
		}
		dock->setVisible(true);
		dock->raise();
		// 中身が収まる大きさにして、OBS のメインウィンドウの中央に表示する。ここに来るのは初めての
		// 起動のときだけで、2 回目からの起動では、OBS が保存した位置と大きさになる。
		const double em = cordmux::dock::ui::base_point_size(guard.data()) * guard->logicalDpiY() / 72.0;
		const QSize min = guard->minimumSize();
		const int w = std::max(min.width(), static_cast<int>(std::lround(kInitialWidthEm * em)));
		const int h = std::max(min.height(), static_cast<int>(std::lround(kInitialHeightEm * em)));
		dock->resize(w, h);
		if (dock->isFloating()) {
			if (auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window())) {
				const QRect frame = main_window->frameGeometry();
				dock->move(frame.center() - QPoint(dock->width() / 2, dock->height() / 2));
			}
		}

		// パスは libobs が返した UTF-8 の文字列である。narrow 文字列を受け取る API に渡すと、
		// アクティブコードページの文字列として解釈されるので、書き込みも platform/ の関数で行う。こうしないと、
		// ユーザーフォルダーの名前に ASCII 以外の文字があるときに、フォルダーは作れてもマーカーのファイルを
		// 書けない。
		if (!platform::write_file_bytes(marker, "shown\n")) {
			diag::logf(LOG_WARNING,
				   "Couldn't save that the dock was shown, so it will be shown again next start");
			return;
		}
		diag::logf(LOG_INFO, "Showed the dock as a floating window on first start");
	});
}

} // namespace cordmux::dock

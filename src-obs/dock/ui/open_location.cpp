// SPDX-License-Identifier: Apache-2.0

// open_location.h の実装。

#include "dock/ui/open_location.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <string>

#include "dock/ui/dock_ui_calc.h"

namespace cordmux::dock::ui {

QString folder_to_open(const QString &path)
{
	if (path.trimmed().isEmpty())
		return QString();
	// 相対パスは OBS の作業フォルダーの下を指してしまうので開かない (platform::resolve_file_location と同じ)。
	if (QDir::isRelativePath(path))
		return QString();
	// 親をたどる規則は dock_ui_calc.h の nearest_folder (文字列でたどるので、単体テストで確認できる)。ここは
	// 存在するかを調べるだけ。
	const QString clean = QDir::cleanPath(QDir(path).absolutePath());
	const std::u16string found = calc::nearest_folder(clean.toStdU16String(), [](const std::u16string &p) {
		const QFileInfo info(QString::fromStdU16String(p));
		if (!info.exists())
			return calc::PathKind::Missing;
		return info.isDir() ? calc::PathKind::Folder : calc::PathKind::File;
	});
	return found.empty() ? QString() : QDir::toNativeSeparators(QString::fromStdU16String(found));
}

bool open_folder(const QString &path)
{
	const QString folder = folder_to_open(path);
	if (folder.isEmpty())
		return false;
	return QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

} // namespace cordmux::dock::ui

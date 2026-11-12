// SPDX-License-Identifier: Apache-2.0

// bundle/export_bundle.h の実装。

#include "bundle/export_bundle.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>

#include <vector>

namespace cordmux::bundle {

PreparedBundle prepare_bundle(const ExportRequest &request)
{
	PreparedBundle prepared;

	// 1 段目。トークンを伏せる。中身を作るのは bundle/bundle_files.h である。
	std::vector<ZipEntry> entries = build_bundle_entries(request.materials, request.redact);

	// 2 段目。ファイルごとに確認し、トークンを検出したファイルだけを 1 行の説明に置き換える
	// (leave_out_revealing_entries)。置き換えた名前は、呼び出し元がログに書く。
	prepared.left_out = leave_out_revealing_entries(entries, request.reveals);

	// 置き換えた後の全文を、もう一度確認する。検出したら何も書かず、保存先も尋ねない (呼び出し元が outcome を見て、
	// 保存のダイアログを表示しない)。理由はログにだけ書く (秘密の値があったことを、画面に表示しない)。
	if (bundle_reveals_secret(entries, request.reveals)) {
		prepared.outcome = ExportOutcome::SecretFound;
		prepared.error = "a connection secret remained in the bundle; nothing was written";
		return prepared;
	}

	std::string zip_error;
	if (!build_zip_stored(entries, request.stamp, &prepared.zip, &zip_error)) {
		prepared.outcome = ExportOutcome::ZipFailed;
		prepared.zip.clear();
		prepared.error = zip_error;
		return prepared;
	}

	prepared.outcome = ExportOutcome::Written;
	return prepared;
}

ExportResult write_bundle(const std::string &path, const std::string &zip)
{
	ExportResult result;
	const QString chosen = QString::fromUtf8(path.c_str());

	// QFile で書く (std::ofstream は使わない)。利用者が選ぶ場所には日本語が入ることがあり、char の場所を取る
	// std::ofstream はその場所を開けない。QFile は UTF-16 の場所で開く。QFile はウィジェットではないので、どのスレッドでも
	// 使える。
	QFile file(chosen);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		result.outcome = ExportOutcome::WriteFailed;
		result.error = file.errorString().toStdString();
		return result;
	}
	const qint64 written = file.write(zip.data(), static_cast<qint64>(zip.size()));
	const bool flushed = file.flush();
	file.close();
	if (written != static_cast<qint64>(zip.size()) || !flushed) {
		// 途中まで書けたファイルは残さない (途中までの zip を診断情報として渡さないため)。
		result.outcome = ExportOutcome::WriteFailed;
		result.error = "the write stopped part way";
		QFile::remove(chosen);
		return result;
	}

	result.outcome = ExportOutcome::Written;
	result.path = path;
	return result;
}

std::string default_export_directory()
{
	// デスクトップ、なければドキュメント、なければホームのフォルダー。OBS の作業フォルダーを既定にすると、問題が
	// 起きたときに渡すファイルが、プログラムのフォルダーの中に保存されて見つけにくい。
	QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
	if (dir.isEmpty())
		dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	if (dir.isEmpty())
		dir = QDir::homePath();
	return QDir::toNativeSeparators(dir).toStdString();
}

} // namespace cordmux::bundle

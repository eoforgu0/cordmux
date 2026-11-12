// SPDX-License-Identifier: Apache-2.0

// decor_test_support.h の実装。

#include "decor/decor_test_support.h"

#include "doctest.h"

#include <QColor>
#include <QDir>
#include <QPainter>
#include <QString>

#include <algorithm>

#include "decor/decor_test_env.h"

namespace cordmux::tests {

namespace {

QImage as_argb32(const QImage &image)
{
	return image.convertToFormat(QImage::Format_ARGB32);
}

// ピクセルが 1 つでも違う場所を赤く塗った差分の画像。
QImage make_difference(const QImage &expected, const QImage &actual)
{
	int width = std::max(expected.width(), actual.width());
	int height = std::max(expected.height(), actual.height());
	QImage diff(width, height, QImage::Format_ARGB32);
	diff.fill(QColor(40, 40, 40));
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			bool inside = x < expected.width() && y < expected.height() && x < actual.width() &&
				      y < actual.height();
			bool same = inside && expected.pixel(x, y) == actual.pixel(x, y);
			if (!same)
				diff.setPixel(x, y, qRgb(255, 0, 0));
		}
	}
	return diff;
}

// 失敗したときの 3 つの画像 (期待、実際、差分) を、実行ファイルのフォルダーの下に書き出す。一時フォルダーには
// 置かない (後から人が開いて見るものなので、消える場所に置かない)。
void write_failure_images(const QString &name, const QImage &expected, const QImage &actual)
{
	QString dir = decor_failure_dir();
	QDir().mkpath(dir);
	if (!expected.isNull())
		expected.save(dir + QStringLiteral("/") + name + QStringLiteral(".expected.png"));
	actual.save(dir + QStringLiteral("/") + name + QStringLiteral(".actual.png"));
	if (!expected.isNull())
		make_difference(expected, actual).save(dir + QStringLiteral("/") + name + QStringLiteral(".diff.png"));
	MESSAGE("snapshot failure images: " << (dir + QStringLiteral("/") + name).toStdString());
}

} // namespace

QImage test_avatar()
{
	QImage avatar(64, 64, QImage::Format_ARGB32_Premultiplied);
	avatar.fill(Qt::transparent);
	QPainter painter(&avatar);
	painter.fillRect(QRect(0, 0, 32, 32), QColor(220, 70, 70));
	painter.fillRect(QRect(32, 0, 32, 32), QColor(70, 220, 70));
	painter.fillRect(QRect(0, 32, 32, 32), QColor(70, 70, 220));
	painter.fillRect(QRect(32, 32, 32, 32), QColor(230, 230, 230));
	return avatar;
}

bool snapshot_matches(const QImage &actual_in, const char *name)
{
	QString file = QStringLiteral(CORDMUX_DECOR_SNAPSHOT_DIR) + QStringLiteral("/") +
		       QString::fromUtf8(name) + QStringLiteral(".png");
	QImage actual = as_argb32(actual_in);

	if (decor_snapshot_update()) {
		QDir().mkpath(QStringLiteral(CORDMUX_DECOR_SNAPSHOT_DIR));
		bool saved = actual.save(file);
		MESSAGE("snapshot updated: " << file.toStdString());
		return saved;
	}

	QImage expected;
	if (!expected.load(file)) {
		write_failure_images(QString::fromUtf8(name), QImage(), actual);
		MESSAGE("snapshot missing: " << file.toStdString());
		return false;
	}
	expected = as_argb32(expected);
	if (expected == actual)
		return true;

	write_failure_images(QString::fromUtf8(name), expected, actual);
	return false;
}

QRect ink_bounds(const QImage &image)
{
	int left = image.width();
	int top = image.height();
	int right = -1;
	int bottom = -1;
	for (int y = 0; y < image.height(); ++y) {
		for (int x = 0; x < image.width(); ++x) {
			if (qAlpha(image.pixel(x, y)) == 0)
				continue;
			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x);
			bottom = std::max(bottom, y);
		}
	}
	if (right < 0)
		return QRect();
	return QRect(left, top, right - left + 1, bottom - top + 1);
}

} // namespace cordmux::tests

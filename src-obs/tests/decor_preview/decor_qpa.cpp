// SPDX-License-Identifier: Apache-2.0

// decor_qpa.h の実装 (プラットフォームプラグイン cordmuxdecorpreview)。この翻訳単位だけを、静的な Qt のプラグインと
// してビルドする (QT_STATICPLUGIN)。登録は main.cpp の Q_IMPORT_PLUGIN(CordmuxDecorPreviewIntegrationPlugin) が行う。

#define QT_STATICPLUGIN

#include "decor_qpa.h"

#include <QFont>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformintegrationplugin.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformtheme.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

#include <QtGui/private/qwindowsdirectwritefontdatabase_p.h>
#include <QtGui/private/qwindowsfontdatabase_p.h>
#include <QtGui/private/qwindowsguieventdispatcher_p.h>

#include <memory>
#include <string>

namespace {

std::string &system_font_family()
{
	static std::string family = "Yu Gothic UI";
	return family;
}

// 画面 (QMinimalScreen と同じ形)。1920 x 1080、拡大率 100% (96 dpi)。名前ラベルはピクセルで大きさを決めるので、
// 画面の DPI は文字の大きさに反映されない。それでも、Qt の既定の値を OBS でよく使われる画面の値にそろえておく。
class DecorScreen : public QPlatformScreen {
public:
	QRect geometry() const override { return QRect(0, 0, 1920, 1080); }
	int depth() const override { return 32; }
	QImage::Format format() const override { return QImage::Format_ARGB32_Premultiplied; }
	QDpi logicalDpi() const override { return QDpi(96.0, 96.0); }
	QSizeF physicalSize() const override { return QSizeF(1920.0 * 25.4 / 96.0, 1080.0 * 25.4 / 96.0); }
};

// バッキングストア (QMinimalBackingStore と同じく、メモリの上の QImage に描画するだけ)。このツールはウィンドウを
// 作らないので使われないが、QPlatformIntegration::createPlatformBackingStore は純粋仮想関数なので実装が必要である。
class DecorBackingStore : public QPlatformBackingStore {
public:
	explicit DecorBackingStore(QWindow *window) : QPlatformBackingStore(window) {}
	QPaintDevice *paintDevice() override { return &image_; }
	void flush(QWindow *, const QRegion &, const QPoint &) override {}
	void resize(const QSize &size, const QRegion &) override
	{
		if (image_.size() != size)
			image_ = QImage(size, QImage::Format_ARGB32_Premultiplied);
	}

private:
	QImage image_;
};

// テーマ (QWindowsTheme::refreshFonts のうち、アプリの既定のフォントだけ)。
class DecorTheme : public QPlatformTheme {
public:
	DecorTheme() : system_font_(QString::fromStdString(system_font_family()), 9) {}
	const QFont *font(Font type) const override { return type == SystemFont ? &system_font_ : nullptr; }

private:
	QFont system_font_;
};

class DecorIntegration : public QPlatformIntegration {
public:
	DecorIntegration()
	{
		screen_ = new DecorScreen();
		QWindowSystemInterface::handleScreenAdded(screen_);
	}
	~DecorIntegration() override { QWindowSystemInterface::handleScreenRemoved(screen_); }

	bool hasCapability(Capability cap) const override
	{
		if (cap == ThreadedPixmaps)
			return true;
		return QPlatformIntegration::hasCapability(cap);
	}

	// minimal と同じく、ウィンドウは素の QPlatformWindow にする (OS のウィンドウを作らない)。
	QPlatformWindow *createPlatformWindow(QWindow *window) const override { return new QPlatformWindow(window); }
	QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override
	{
		return new DecorBackingStore(window);
	}
	QAbstractEventDispatcher *createEventDispatcher() const override { return new QWindowsGuiEventDispatcher; }

	// QWindowsIntegration::fontDatabase の既定 (DirectWrite 版)。
	QPlatformFontDatabase *fontDatabase() const override
	{
		if (!font_database_)
			font_database_ = std::make_unique<QWindowsDirectWriteFontDatabase>();
		return font_database_.get();
	}

	// 文字を混ぜるときのガンマは、QWindowsIntegration::styleHint と同じく OS の値 (ClearType のコントラスト) にする。
	// Qt の既定 (1.7) のままだと、OBS の中で描いた名前の文字より縁が明るくなる。
	QVariant styleHint(StyleHint hint) const override
	{
		if (hint == FontSmoothingGamma)
			return QWindowsFontDatabase::fontSmoothingGamma();
		return QPlatformIntegration::styleHint(hint);
	}

	QPlatformTheme *createPlatformTheme(const QString &) const override { return new DecorTheme(); }
	QStringList themeNames() const override { return QStringList{QStringLiteral("cordmuxdecorpreview")}; }

private:
	DecorScreen *screen_ = nullptr;
	mutable std::unique_ptr<QPlatformFontDatabase> font_database_;
};

} // namespace

namespace cordmux::decor_preview {

void set_system_font_family(const char *family)
{
	system_font_family() = family ? family : "";
}

} // namespace cordmux::decor_preview

class CordmuxDecorPreviewIntegrationPlugin : public QPlatformIntegrationPlugin {
	Q_OBJECT
	Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "decor_qpa.json")

public:
	QPlatformIntegration *create(const QString &system, const QStringList &) override
	{
		if (system.compare(QLatin1String(cordmux::decor_preview::kDecorPlatformName), Qt::CaseInsensitive) == 0)
			return new DecorIntegration();
		return nullptr;
	}
};

#include "decor_qpa.moc"

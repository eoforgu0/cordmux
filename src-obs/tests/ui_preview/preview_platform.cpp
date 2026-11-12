// SPDX-License-Identifier: Apache-2.0

// preview_platform.h の実装 (プラットフォームプラグイン cordmuxpreview)。この翻訳単位だけを、静的な Qt のプラグインと
// してビルドする (QT_STATICPLUGIN)。登録は main.cpp の Q_IMPORT_PLUGIN(CordmuxPreviewIntegrationPlugin) が行う。

#define QT_STATICPLUGIN

#include "preview_platform.h"

#include <QAccessible>
#include <QFont>
#include <QImage>
#include <QPalette>
#include <QPixmap>
#include <QSize>
#include <QVariant>

#include <qpa/qplatformaccessibility.h>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformintegrationplugin.h>
#include <qpa/qplatformnativeinterface.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformtheme.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

#include <QtGui/private/qwindowsdirectwritefontdatabase_p.h>
#include <QtGui/private/qwindowsguieventdispatcher_p.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>

#include <memory>

#include "render.h"

namespace {

// ---- 画面 (QMinimalScreen と同じ形。大きさだけを、OBS でよく使われる画面の大きさにする) ----
//
// 1920 x 1080 で、下にタスクバー 40 px。拡大率 100% (96 dpi)。QMessageBox は、この幅から自分の幅の上限を決める
// (QMessageBoxPrivate::updateSize。画面が 1024 px を超えれば上限は 500 px)。メニューは、この中に収まるように
// 表示する位置を決める。
class PreviewScreen : public QPlatformScreen {
public:
	QRect geometry() const override { return QRect(0, 0, 1920, 1080); }
	QRect availableGeometry() const override { return QRect(0, 0, 1920, 1040); }
	int depth() const override { return 32; }
	QImage::Format format() const override { return QImage::Format_ARGB32_Premultiplied; }
	QDpi logicalDpi() const override { return QDpi(96.0, 96.0); }
	QSizeF physicalSize() const override { return QSizeF(1920.0 * 25.4 / 96.0, 1080.0 * 25.4 / 96.0); }
};

// ---- バッキングストア (QMinimalBackingStore と同じく、メモリの上の QImage に描画するだけで、どこにも表示しない) ----
class PreviewBackingStore : public QPlatformBackingStore {
public:
	explicit PreviewBackingStore(QWindow *window) : QPlatformBackingStore(window) {}
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

// ---- テーマ (QWindowsTheme のうち、描画される画像に反映されるところだけを移植した) ----
class PreviewTheme : public QPlatformTheme {
public:
	PreviewTheme()
	{
		palette_ = cordmux::ui_preview::windows_light_base_palette();
		// QWindowsTheme::refreshFonts と同じく、アプリの既定のフォントはメッセージのフォント (lfMessageFont)。
		// 日本語の Windows 10 の既定値は Yu Gothic UI の 9 pt (SPI_GETNONCLIENTMETRICS が返す値)。プレビューが
		// PC の設定で変わらないように固定する。Yami の QSS はフォントの名前と大きさを上書きするので、動作するのは
		// QSS が決めない性質 (太さなど) だけである。
		system_font_ = QFont(QStringLiteral("Yu Gothic UI"), 9);
	}

	const QPalette *palette(Palette type) const override
	{
		return type == SystemPalette ? &palette_ : nullptr;
	}

	const QFont *font(Font type) const override { return type == SystemFont ? &system_font_ : nullptr; }

	QVariant themeHint(ThemeHint hint) const override
	{
		switch (hint) {
		case UseFullScreenForPopupMenu:
			return QVariant(true);
		case DialogButtonBoxLayout:
			return QVariant(int(0)); // QPlatformDialogHelper::WinLayout
		case StyleNames:
			// Windows 10 で QWindowsTheme::styleNames() が返す並び (Windows 11 なら先頭に windows11)。
			return QVariant(QStringList{QStringLiteral("windowsvista"), QStringLiteral("windows")});
		case IconPixmapSizes:
			// QWindowsTheme::refreshIconPixmapSizes の 96 dpi での値 (小 16、大 32、特大 48、最大 256)。
			return QVariant::fromValue(QList<QSize>{QSize(16, 16), QSize(32, 32), QSize(48, 48), QSize(256, 256)});
		case ContextMenuOnMouseRelease:
			return QVariant(true);
		default:
			break;
		}
		return QPlatformTheme::themeHint(hint);
	}

	// QWindowsTheme::standardPixmap のうち、ダイアログ (QMessageBox) のアイコンの 4 つだけ。
	// 同じ順に探す (シェルのストックアイコン、LoadIcon の順)。
	QPixmap standardPixmap(StandardPixmap sp, const QSizeF &size) const override
	{
		SHSTOCKICONID stock_id = SIID_INVALID;
		LPCTSTR icon_name = nullptr;
		switch (sp) {
		case MessageBoxInformation:
			stock_id = SIID_INFO;
			icon_name = IDI_INFORMATION;
			break;
		case MessageBoxWarning:
			stock_id = SIID_WARNING;
			icon_name = IDI_WARNING;
			break;
		case MessageBoxCritical:
			stock_id = SIID_ERROR;
			icon_name = IDI_ERROR;
			break;
		case MessageBoxQuestion:
			stock_id = SIID_HELP;
			icon_name = IDI_QUESTION;
			break;
		default:
			return QPlatformTheme::standardPixmap(sp, size);
		}
		SHSTOCKICONINFO info;
		ZeroMemory(&info, sizeof(info));
		info.cbSize = sizeof(info);
		if (SHGetStockIconInfo(stock_id, SHGSI_ICONLOCATION, &info) == S_OK) {
			HICON icon = nullptr;
			const UINT icon_size = static_cast<UINT>(size.width());
			if (SHDefExtractIconW(info.szPath, info.iIcon, 0, &icon, nullptr, icon_size) == S_OK) {
				const QPixmap pixmap = QPixmap::fromImage(QImage::fromHICON(icon));
				DestroyIcon(icon);
				if (!pixmap.isNull())
					return pixmap;
			}
		}
		if (icon_name != nullptr) {
			HICON icon = LoadIcon(nullptr, icon_name);
			const QPixmap pixmap = QPixmap::fromImage(QImage::fromHICON(icon));
			DestroyIcon(icon);
			if (!pixmap.isNull())
				return pixmap;
		}
		return QPlatformTheme::standardPixmap(sp, size);
	}

private:
	QPalette palette_;
	QFont system_font_;
};

// ---- アクセシビリティ ----
//
// Qt の素の QPlatformAccessibility は無効のまま (isActive() が false)。QAccessible::setActive(true) は
// ActivationObserver に通知するだけで、プラグインの m_active は変えない (Qt 6.8.3 の qaccessible.cpp。windows
// プラグインは、スクリーンリーダーがウィンドウに問い合わせてきたときに自分で setActive する)。このプラグインには
// 問い合わせてくる相手がいないので、テストの exe が QAccessible::setActive(true) を呼び出したら、その通知を受けて
// プラグイン自身を有効にする。setActive は同じ通知をもう一度出すので、値が同じなら何もしない (通知の繰り返しを
// 止める)。
class PreviewAccessibility : public QPlatformAccessibility, public QAccessible::ActivationObserver {
public:
	PreviewAccessibility() { QAccessible::installActivationObserver(this); }
	~PreviewAccessibility() override { QAccessible::removeActivationObserver(this); }

	void accessibilityActiveChanged(bool active) override
	{
		if (isActive() != active)
			setActive(active);
	}
};

// ---- プラグイン本体 ----
class PreviewIntegration : public QPlatformIntegration {
public:
	PreviewIntegration()
	{
		// QWindowsContext のコンストラクターと同じく、OLE (COM) を初期化する。シェルのアイコンを読む関数
		// (SHGetStockIconInfo と SHDefExtractIcon) は、これがないと使えない。
		ole_initialized_ = SUCCEEDED(OleInitialize(nullptr));
		screen_ = new PreviewScreen();
		QWindowSystemInterface::handleScreenAdded(screen_);
		// アクセシビリティは先に作る (accessibility() が呼ばれる前の QAccessible::setActive の通知も
		// 受けるため)。
		accessibility_ = std::make_unique<PreviewAccessibility>();
	}
	~PreviewIntegration() override
	{
		QWindowSystemInterface::handleScreenRemoved(screen_);
		if (ole_initialized_)
			OleUninitialize();
	}

	bool hasCapability(Capability cap) const override
	{
		switch (cap) {
		case ThreadedPixmaps:
		case MultipleWindows:
			return true;
		case RhiBasedRendering:
			return false;
		default:
			return QPlatformIntegration::hasCapability(cap);
		}
	}

	// minimal と同じく、ウィンドウは素の QPlatformWindow にする (OS のウィンドウを作らない)。
	QPlatformWindow *createPlatformWindow(QWindow *window) const override
	{
		auto *w = new QPlatformWindow(window);
		w->requestActivateWindow();
		return w;
	}

	QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override
	{
		return new PreviewBackingStore(window);
	}

	QAbstractEventDispatcher *createEventDispatcher() const override { return new QWindowsGuiEventDispatcher; }

	// minimal と同じ素の実装 (どのリソースを問い合わせられてもないと答える)。これがないと、
	// QMessageBox::showEvent がウィンドウの [閉じる] の項目を探すところで止まる (qt_getWindowsSystemMenu が、
	// この実装の答えを前提にしている)。
	QPlatformNativeInterface *nativeInterface() const override
	{
		if (!native_interface_)
			native_interface_ = std::make_unique<QPlatformNativeInterface>();
		return native_interface_.get();
	}

	// QWindowsIntegration::fontDatabase の既定 (DirectWrite 版)。
	QPlatformFontDatabase *fontDatabase() const override
	{
		if (!font_database_)
			font_database_ = std::make_unique<QWindowsDirectWriteFontDatabase>();
		return font_database_.get();
	}

	// アクセシビリティ。Qt の素の QPlatformAccessibility (何もしない基底で、既定は isActive() が false) に、
	// QAccessible::setActive の通知で有効になる処理だけを追加したもの (上の PreviewAccessibility) を 1 つ持って
	// 返す。minimal と同じく、OS のスクリーンリーダーには何も渡さない。QAccessible::updateAccessibility は、
	// プラグインが QPlatformAccessibility を返さないと isActive() が false のままなので、イベントを
	// installUpdateHandler のハンドラーに渡す前に捨てる。テストの exe (dock_drive) が
	// QAccessible::setActive(true) を呼び出し、ハンドラーでイベントを数えるには、プラグインがこれを返す必要が
	// ある。ui_preview は setActive を呼び出さないので、画像には反映されない。dock_drive の側で、このプラグインを
	// 包んですべての仮想関数を中継する別のプラグインを作ると、中継し忘れたときに 2 つの動作が食い違うので、
	// このプラグインに持たせる。
	QPlatformAccessibility *accessibility() const override
	{
		if (!accessibility_)
			accessibility_ = std::make_unique<PreviewAccessibility>();
		return accessibility_.get();
	}

	QPlatformTheme *createPlatformTheme(const QString &) const override { return new PreviewTheme(); }
	QStringList themeNames() const override { return QStringList{QStringLiteral("cordmuxpreview")}; }

	QVariant styleHint(StyleHint hint) const override
	{
		// QWindowsIntegration::styleHint と同じく、ClearType の文字のガンマは ClearType のコントラスト
		// (SPI_GETFONTSMOOTHINGCONTRAST) / 1000。コントラストは PC の設定で変わり、Microsoft のドキュメントの
		// 既定値は 1400 である。プレビューが PC の設定で変わらないように、読まずに 1.2 (コントラスト 1200) に
		// 固定する。既定値のままの PC の OBS と描画が同じになるかは確認していない。
		if (hint == FontSmoothingGamma)
			return QVariant(qreal(1.2));
		return QPlatformIntegration::styleHint(hint);
	}

private:
	PreviewScreen *screen_ = nullptr;
	bool ole_initialized_ = false;
	mutable std::unique_ptr<QPlatformFontDatabase> font_database_;
	mutable std::unique_ptr<QPlatformNativeInterface> native_interface_;
	mutable std::unique_ptr<QPlatformAccessibility> accessibility_;
};

} // namespace

class CordmuxPreviewIntegrationPlugin : public QPlatformIntegrationPlugin {
	Q_OBJECT
	Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "preview_platform.json")

public:
	QPlatformIntegration *create(const QString &system, const QStringList &) override
	{
		if (system.compare(QLatin1String(cordmux::ui_preview::kPreviewPlatformName), Qt::CaseInsensitive) == 0)
			return new PreviewIntegration();
		return nullptr;
	}
};

#include "preview_platform.moc"

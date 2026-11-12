// SPDX-License-Identifier: Apache-2.0

// プレビューツール (ui_preview) とドックを操作するテスト (dock_drive) が使う、OBS と同じ型の名前を持つクラス。
// OBS のテーマは型の名前でプロパティを設定する。cordmux は OBS の画面と同じアイコン (画面共有のアイコンは
// メインウィンドウの desktopCapIcon、設定ウィンドウのアイコンは OBS の設定のアイコン) を、テーマが OBS のウィジェットに
// 設定したプロパティから読む。プレビューでも同じ方法で読むため、同じ型の名前とプロパティの名前を持つ型を作る (中身は
// 何もしない)。

#pragma once

#include <QDialog>
#include <QIcon>
#include <QMainWindow>

// OBS 31.1.1 の frontend/widgets/OBSBasic.hpp と同じ Q_PROPERTY (desktopCapIcon)。Yami.obt が
// qproperty-desktopCapIcon で設定する。ドックを OBS の既定のレイアウトに入れて描画するときのメインウィンドウにも使う
// (OBS のメインウィンドウの型の名前で選ぶテーマの規則が動作するように)。
class OBSBasic : public QMainWindow {
	Q_OBJECT
	Q_PROPERTY(QIcon desktopCapIcon READ desktopCapIcon WRITE setDesktopCapIcon DESIGNABLE true)

public:
	explicit OBSBasic(QWidget *parent = nullptr) : QMainWindow(parent) {}
	QIcon desktopCapIcon() const { return desktop_cap_icon_; }
	void setDesktopCapIcon(const QIcon &icon) { desktop_cap_icon_ = icon; }

private:
	QIcon desktop_cap_icon_;
};

// OBS 31.1.1 の frontend/settings/OBSBasicSettings.hpp と同じ Q_PROPERTY (generalIcon など)。Yami.obt が設定する。
class OBSBasicSettings : public QDialog {
	Q_OBJECT
	Q_PROPERTY(QIcon generalIcon READ generalIcon WRITE setGeneralIcon DESIGNABLE true)
	Q_PROPERTY(QIcon appearanceIcon READ appearanceIcon WRITE setAppearanceIcon DESIGNABLE true)
	Q_PROPERTY(QIcon outputIcon READ outputIcon WRITE setOutputIcon DESIGNABLE true)
	Q_PROPERTY(QIcon videoIcon READ videoIcon WRITE setVideoIcon DESIGNABLE true)
	Q_PROPERTY(QIcon advancedIcon READ advancedIcon WRITE setAdvancedIcon DESIGNABLE true)

public:
	explicit OBSBasicSettings(QWidget *parent = nullptr) : QDialog(parent) {}
	QIcon generalIcon() const { return general_; }
	void setGeneralIcon(const QIcon &icon) { general_ = icon; }
	QIcon appearanceIcon() const { return appearance_; }
	void setAppearanceIcon(const QIcon &icon) { appearance_ = icon; }
	QIcon outputIcon() const { return output_; }
	void setOutputIcon(const QIcon &icon) { output_ = icon; }
	QIcon videoIcon() const { return video_; }
	void setVideoIcon(const QIcon &icon) { video_ = icon; }
	QIcon advancedIcon() const { return advanced_; }
	void setAdvancedIcon(const QIcon &icon) { advanced_ = icon; }

private:
	QIcon general_;
	QIcon appearance_;
	QIcon output_;
	QIcon video_;
	QIcon advanced_;
};

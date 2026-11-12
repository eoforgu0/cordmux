// SPDX-License-Identifier: Apache-2.0

// OBS がドックのウィジェットを入れるコンテナー (OBSDock) と同じ型の名前を持つ QDockWidget。OBS 31.1.1 の
// obs_frontend_add_dock_by_id は、プラグインのウィジェットを OBSDock (frontend/docks/OBSDock.hpp。QDockWidget を
// 継承して Q_OBJECT を持つ) に入れる。Yami のテーマは `OBSDock > QWidget { background: ...; border: ... }`、
// `OBSDock QLabel`、`OBSDock QScrollBar` のようにこの型の名前でドックの中身を選ぶので、同じ名前の型に入れないと、
// ドックの背景の色・枠線・スクロールバーが OBS と違って描画される。OBSDock が追加するのは閉じたときの案内の
// ダイアログだけで描画には反映されないので、ここでは何も追加しない。

#pragma once

#include <QDockWidget>

class OBSDock : public QDockWidget {
	Q_OBJECT

public:
	explicit OBSDock(QWidget *parent = nullptr) : QDockWidget(parent) {}
};

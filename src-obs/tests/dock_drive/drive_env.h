// SPDX-License-Identifier: Apache-2.0

// ドックを操作するテストの環境を用意する。プレビュー用のプラットフォームプラグイン (cordmuxpreview) の上で、OBS の
// テーマ、[フォントサイズ] と [密度]、cordmux と OBS の ini、OBS のメインウィンドウと同じ型名のウィジェットを用意する。
// 手順はプレビューツールの関数 (tests/ui_preview/render.h の apply_obs_theme と tests/ui_preview/locale_stub.h) を呼ぶ
// だけで、同じ手順を 2 か所に書かない。
// 既定の組み合わせは、表示の倍率 100% (96 dpi)、1920 x 1080、テーマ Yami Classic、[フォントサイズ] 10、[密度] -2
// (クラシック) で、OBS の既定の組み合わせとは違う。押せる範囲と行の高さは組み合わせで変わるので、ほかの組み合わせは
// --theme などで指定して確認する。

#pragma once

#include <QString>

#include <memory>

class QApplication;
class QWidget;

namespace cordmux::dock_drive {

struct EnvOptions {
	QString obs_source;                     // OBS のソースツリー (テーマ、フォント、OBS の ini を読む)
	QString locale_dir;                     // cordmux の ini のフォルダー
	QString theme = QStringLiteral("yami-classic"); // yami / yami-light / yami-classic / system / テーマの id
	QString lang = QStringLiteral("ja-JP");
	int font_scale = 10; // OBS の [外観] の [フォントサイズ]
	int density = -2;    // OBS の [外観] の [密度] の番号 (-2 クラシック、-3 コンパクト、-4 通常、-5 快適。1 = OBS 31 の既定 = 通常)
};

// テーマの短い名前を OBS のテーマの id にする (tests/ui_preview/main.cpp と同じ対応)。ほかの名前はそのまま返す。
QString theme_id_of(const QString &name);

// テーマ、ini、メインウィンドウを用意する。失敗したら false を返し、error に理由を書く。QApplication を作った後に
// 1 回だけ呼ぶ。
bool setup_env(QApplication &app, const EnvOptions &options, QString *error);

// テーマだけを適用し直す。失敗したら false を返し、error に理由を書く。
bool reapply_theme(QApplication &app, const EnvOptions &options, QString *error);

// OBS のメインウィンドウと同じ型名のウィジェット (obs_frontend_get_main_window が返すもの)。表示してあるが、
// プレビュー用のプラットフォームプラグインの上なので、OS のウィンドウはない。setup_env から teardown_env までの
// 間だけ有効。
QWidget *main_window();

// メインウィンドウを外して破棄する。
void teardown_env();

} // namespace cordmux::dock_drive

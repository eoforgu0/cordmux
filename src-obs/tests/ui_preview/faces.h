// SPDX-License-Identifier: Apache-2.0

// プレビューツールの描画の対象 (--face)。製品のウィジェットをウィンドウを表示せずに作り、場面ごとに画像と
// ウィジェットの一覧 (widgets.tsv) を書く。widgets.tsv は描画したすべてのウィジェット (メニューならすべての項目) を
// 1 行ずつ書き、列は faces.cpp の kWidgetsHeader。読む側は見出しの名前で列を引くので、列は後ろにだけ
// 追加する。metrics.tsv (ドックだけ) は、場面と大きさごとの 1 行目の高さ・横のスクロールの要否・最小幅などで、
// 後ろに measure.h の m1 から m8 が続く。targets.tsv、names.tsv、contrast.tsv (出力先ごとに 1 つ) は、押せるものと
// WCAG 2.5.8、名前の見え方、テキストのコントラスト (measure.h)。

#pragma once

#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

#include "dock/ui/dock_style.h"
#include "render.h"

class QApplication;

namespace cordmux::ui_preview {

struct FaceOptions {
	// 描画の対象。次のどれか (引数の形は face_names() の一覧)。
	//   dock               ドック (場面ごと、幅ごと)。場面は fixtures.h の dock_fixtures()
	//   settings:<page>    設定ウィンドウ (<page> は general、name-label、recording、receive、advanced、about、all)
	//   participants       参加者の設定ウィンドウ
	//   dialog:<id>        確認・結果・失敗のダイアログと、「名前を変更」のダイアログ
	//   menu:dock          ドックのメニューを開いた画像
	//   menu:row           参加者の行のメニューを開いた画像 (menu:row-voice は「使う声」のサブメニューも開く)
	//   focus              キーボードでフォーカスを移した画像 (行のチェックボックス・[...]・録画のボタン・
	//                      ドックの [...]。--tab で開いたタブのつまみも)
	//   timeline           Observation の並びを stabilize に順に通した画像 (表示までの待ち・表示の順・並びの保留)
	//   theme-switch       テーマ・フォントサイズ・密度を切り替えて製品に通知した画像と、作り直した画像との比較
	//   probe:menu-check   ただの QMenu のチェックマークを、QSS なしと OBS のテーマで描画して比べる
	//   probe:obs-layout   OBS の既定のレイアウトに cordmux のドックを入れたときの大きさ
	//   probe:screen-fit   画面に収まるかの集計 (screen-fit.tsv) が失敗を出せるかのセルフテスト
	// 描画する関数がビルドにない対象 (ウィンドウを作るソースの .cmake がないなど) は、ないと出力して 0 で終了する。
	QString face;
	QString out_dir; // 書き出す出力先
	QStringList themes{QStringLiteral("yami"), QStringLiteral("yami-light")};
	QStringList langs{QStringLiteral("ja-JP"), QStringLiteral("en-US")};
	QList<double> widths_em{15.0, 20.0, 26.0, 32.0, 45.0}; // ドックの幅 (em。OBS の本文の文字の大きさの倍数)
	QList<QSize> sizes;    // さらに px で指定する大きさ (例 230x165)
	int height = 600;      // 幅で指定するときの高さ (0 以下なら、スクロールが不要になる高さまで伸ばす)
	QString only;          // 場面の名前にこの文字列を含むものだけ
	QString locale_dir;    // cordmux の ini のフォルダー (--ini-dir)
	QString obs_source;    // OBS のソースのフォルダー (OBS 自身の ini とテーマ)
	bool platform_defaults = true;
	dock::ui::DockStyle style; // 見た目の値 (--first-line-scale など)
	Appearance appearance;     // OBS の [外観] (--font-size、--density)
	double dpr = 1.0;          // 画像の拡大率 (--dpr)
	// 描画する画面 (--screen。例 "1366x768@125"。空なら既定の 1920x1080 の 100%)。ウィンドウ (dialog・settings・
	// participants) は、画面が指定されていて本文をスクロールする形なら、いちばん下までスクロールした画像も描画する
	// (名前の先頭は bottom-)。画面に収まるかの集計 (screen-fit.tsv) は、画面の指定に関わらず書く。
	QString screen_tag;
	QString obs_tag;           // OBS のバージョンを表す文字列 (--obs 32 のとき "obs32")
	bool at_min_width = false; // 製品が計算した最小幅でも描画する (--at-min-width。大きさの名前は min)
	// 行の状態のテキストを表示する、いちばん狭いドックの幅でも描画する (--at-status-floor。大きさの名前は
	// floor)。製品の境目 (PeopleList::status_text_min_width_px) を行の幅で満たす最小の幅で、状態のテキストの
	// QLabel は下限の 5.75 em (rows::kStatusTextKeepEm) になる。この幅でも、短い形を省略せずに表示するかを
	// 確認する。状態のテキストのない場面は描画しない。
	bool at_status_floor = false;
	// ドックで開くタブ (--tab people|audio|display|status)。持たないときは既定の参加者タブ。
	std::optional<config::DockTab> tab;
};

// 出力先のフォルダーの名前のテーマの部分
// (<テーマ>[-<OBS のバージョン>][-f<フォントサイズ>-d<密度>][-x<拡大率の百分率>])。既定の設定 (OBS 31.1.1、
// フォントサイズ 10、[通常]、100%) では、テーマの名前だけにする。
QString theme_dir_name(const QString &theme, const FaceOptions &options);

// 描画の対象を描画する。描画できたら 0 (ビルドにない対象も 0)、描画できないものがあったら 1 を返す。
int run_face(QApplication &app, const ObsPaths &paths, const FaceOptions &options);

// 描画の対象の名前の一覧 (--list-faces と使い方の表示に使う)。
QStringList face_names();

} // namespace cordmux::ui_preview

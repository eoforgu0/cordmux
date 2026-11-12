// SPDX-License-Identifier: Apache-2.0

// 状態のアイコン。メッセージのレベルのアイコンと参加者の行の状態のアイコンは、cordmux が 16 x 16 のグリッドで同じ描き方
// (形を塗り、中の記号を抜く) で描き、渡した 1 色で塗る (色の付いたアイコンにしない)。使う側は、テーマの本文の色
// (ThemeRead::text) を渡し、メッセージのレベル 1 のパネルの上だけは白か黒 (L1Surface::foreground) を渡す。
// QPainter で描き、Qt6::Svg はリンクしない (cordmux.dll の依存を増やさないため)。画像はウィジェットの画面の倍率
// (devicePixelRatio) で描く (拡大率 125% や 150% でぼけないように)。テーマを替えたときは、使う側が新しい
// テーマの色を読み、MarkLabel::set_painter を呼び直す。

#pragma once

#include <QColor>
#include <QIcon>
#include <QLabel>
#include <QPixmap>

#include <functional>

#include "dock/ui/dock_style.h"
#include "dock/ui/theme_read.h"

class QStyle;

namespace cordmux::dock::ui {

// メッセージのレベルのアイコンの形 (ledger の Tier とは別の型。dock/ui/ は ledger の View の型に依存しない)。
// TierMark::L1 は円に x、TierMark::L2 は三角形に ! (OBS の alert.svg と同じ形)、TierMark::L3 は円に i。
enum class TierMark { L1, L2, L3 };

// 参加者の行の状態のアイコン (塗った円の中の記号)。円の形にするのは、画面共有のアイコンの形と外形が紛れないため。
// シーンコレクションを切り替える間の保留には、アイコンを用意しない。保留で止めるのは音声だけで、一時停止の形は
// 「止まった映像が配信に残っている」と読まれるため。
enum class RowMark {
	Waiting,    // まだ届いていない (時計の針)
	NoVideo,    // 届かなくなった (斜めの線)
	NotSharing, // 画面共有していない (横の線)
	Quality,    // 画質を下げている (下向きの矢印)
};

// 参加者の行のミュートのボタンの図。声はマイク、画面共有の音はスピーカーの形で、ミュート中は斜線を重ねる。斜線の色は
// slash で渡す (無効な色なら color で描く)。色がなくても斜線の形で分かる。画面の形は映像と間違えられるので、声と画面共有の
// 音は別の形にする。OBS のミュートの図 (mute.svg) はスピーカーだけで、色が赤に固定されているので使わない。
enum class AudioMark { Voice, Screen };
QPixmap audio_mark_pixmap(AudioMark mark, bool muted, int px, const QColor &color, qreal dpr,
			  const QColor &slash = QColor());

// 参加者の行の画面のアイコン。画面の輪郭の中の四角で、枠の位置を示す。None は四角なし (枠に載っていない共有中の人)。
// テーマの desktopCapIcon は中に四角を描く余白を持たないので、この図は cordmux が描く。
enum class SlotMark { None, TopLeft, TopRight, BottomLeft, BottomRight, Full };
QPixmap slot_mark_pixmap(SlotMark mark, int px, const QColor &color, qreal dpr);

// 山形のアイコンの向き。Right はドックの 1 行目の右端に置き、押すと状態タブへ移ることを示す。
enum class Chevron { Up, Down, Right };

// cordmux が描く 1 色の、メッセージのレベルのアイコン。
QPixmap tier_mark_pixmap(TierMark mark, int px, const QColor &color, qreal dpr);
// Windows の標準のアイコン (DockStyle::tier_icon_set が WindowsStandard のとき。見た目を比べるためだけに使う)。
// ダイアログのアイコンには使わない。style が nullptr なら、アプリケーションのスタイルを使う。
QPixmap standard_tier_mark_pixmap(TierMark mark, int px, qreal dpr, QStyle *style = nullptr);
// DockStyle::tier_icon_set に従って、上の 2 つのどちらかを返す。
QPixmap tier_pixmap(TierMark mark, int px, const QColor &color, qreal dpr, TierIconSet set);

QPixmap row_mark_pixmap(RowMark mark, int px, const QColor &color, qreal dpr);

// [...] のアイコン。テーマがアイコンを持てば空の QIcon を返す (クラス btn-tool icon-dots-vert をボタンに付ければ、
// テーマがアイコンを設定する)。持たなければ、cordmux が 3 つの点を color で描いた QIcon (いくつかの倍率の画像を
// 持つ) を返す。
QIcon fallback_dots_icon(const ThemeRead &theme, int px, const QColor &color);

// 山形のアイコン。16 x 16 のグリッドで、メッセージのレベル 1 のアイコンの x と同じ太さ (2.0) の、端の丸い線 2 本で描く
// (同じパネルの上で、メッセージのレベル 1 のアイコンの右に並ぶので、線の太さをそろえる)。塗った円は持たない (状態では
// なく押すもののアイコンなので、[...] の 3 つの点と同じく線だけの形にする)。色は使う側が渡す (パネルの上なので白か
// 黒。L1Surface::foreground)。テーマからは取らない (OBS のテーマに、上向きと下向きの山形のクラスがない)。
// テキストの「^」「v」は使わない。フォントによって形が変わるため。
QPixmap chevron_pixmap(Chevron dir, int px, const QColor &color, qreal dpr);

// アイコンを表示するラベル。大きさはアイコンの大きさに固定し (アイコンを置くウィジェットは、アイコンの大きさを
// 下回らないようにする)、ウィジェットが別の倍率の画面へ移ったとき (QEvent::DevicePixelRatioChange) は描き直す。
// アクセシブル名は使う側が付ける。
// 画像はラベルの矩形の中央に自分で描く。QLabel の描き方では、スタイルシートの padding で中身の矩形が狭まり、Yami の
// `.frame-notice QLabel { padding: 4px 0 }` の中で、16 px のアイコンが 8 px の高さに切れるため。QLabel を継承するのは、
// プレビューツールとスクリーンリーダーがラベルとして扱う形を変えないため。
class MarkLabel : public QLabel {
public:
	using Painter = std::function<QPixmap(qreal dpr)>;

	explicit MarkLabel(QWidget *parent = nullptr);
	// px の大きさのアイコンを painter で描く。painter が空なら何も表示しない (幅だけを確保する列に使う)。
	void set_painter(int px, Painter painter);
	// 現在の倍率で描き直す (新しいテーマの色を読んだ後など)。
	void refresh();
	int mark_px() const { return px_; }

protected:
	bool event(QEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	int px_ = 16;
	Painter painter_;
};

} // namespace cordmux::dock::ui

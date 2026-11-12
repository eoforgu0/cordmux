// SPDX-License-Identifier: Apache-2.0

// status_icons.h の実装。

#include "dock/ui/status_icons.h"

#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStyle>
#include <QStyleOption>

#include <utility>

namespace cordmux::dock::ui {

namespace {

// QIcon に持たせる画像の倍率の組 (Windows の拡大率の段階。QIcon がいちばん近いものを選ぶ)。
constexpr qreal kIconRatios[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};

// 16 x 16 のグリッドで描くための、背景が透明な画像。
QPixmap canvas(int px, qreal dpr)
{
	const qreal r = dpr > 0.0 ? dpr : 1.0;
	QPixmap pm(QSize(qRound(px * r), qRound(px * r)));
	pm.setDevicePixelRatio(r);
	pm.fill(Qt::transparent);
	return pm;
}

void begin16(QPainter &p, int px)
{
	p.setRenderHint(QPainter::Antialiasing, true);
	p.scale(px / 16.0, px / 16.0);
}

// 抜く (透明にする) ための合成モード。
void knock_mode(QPainter &p)
{
	p.setCompositionMode(QPainter::CompositionMode_Clear);
}

// OBS の alert.svg (themes/Dark/alert.svg。16 x 16 のグリッド) の外形のパスをコピーした三角形。OBS のテーマの
// ファイルは読まない。OBS は明暗で Dark と Light のファイルを選ぶので利用者のテーマのアイコンのフォルダーに届かず、
// 塗りの色も固定されていて、ほかのアイコンと色がそろわないため。
QPainterPath alert_triangle()
{
	QPainterPath path;
	path.moveTo(7.90625, 0.105469);
	path.cubicTo(7.378906, 0.078125, 6.867188, 0.386719, 6.46875, 1.074219);
	path.lineTo(0.21875, 12.667969);
	path.cubicTo(-0.316406, 13.632812, 0.265625, 15.011719, 1.3125, 15.011719);
	path.lineTo(14.46875, 15.011719);
	path.cubicTo(15.449219, 15.011719, 16.371094, 13.851562, 15.6875, 12.667969);
	path.lineTo(9.375, 1.136719);
	path.cubicTo(8.976562, 0.488281, 8.433594, 0.132812, 7.90625, 0.105469);
	path.closeSubpath();
	return path;
}

} // namespace

QPixmap tier_mark_pixmap(TierMark mark, int px, const QColor &color, qreal dpr)
{
	QPixmap pm = canvas(px, dpr);
	QPainter p(&pm);
	begin16(p, px);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	if (mark == TierMark::L2) {
		// 三角形を塗り、! の縦線と点を抜く (alert.svg の中の 2 つのパスと同じ位置)。
		p.drawPath(alert_triangle());
		knock_mode(p);
		p.setBrush(Qt::black);
		p.drawRoundedRect(QRectF(6.97, 4.04, 2.06, 5.94), 1.0, 1.0);
		p.drawEllipse(QPointF(8.0, 12.01), 1.0, 1.0);
		return pm;
	}
	p.drawEllipse(QRectF(0.25, 0.25, 15.5, 15.5));
	knock_mode(p);
	if (mark == TierMark::L1) {
		QPen pen(Qt::black, 2.0, Qt::SolidLine, Qt::RoundCap);
		p.setPen(pen);
		p.drawLine(QPointF(5.2, 5.2), QPointF(10.8, 10.8));
		p.drawLine(QPointF(10.8, 5.2), QPointF(5.2, 10.8));
	} else {
		p.setBrush(Qt::black);
		p.drawEllipse(QPointF(8.0, 4.6), 1.2, 1.2);
		p.drawRoundedRect(QRectF(7.0, 6.8, 2.0, 5.6), 0.8, 0.8);
	}
	return pm;
}

QPixmap standard_tier_mark_pixmap(TierMark mark, int px, qreal dpr, QStyle *style)
{
	QStyle *s = style != nullptr ? style : QApplication::style();
	QStyle::StandardPixmap sp = QStyle::SP_MessageBoxInformation;
	if (mark == TierMark::L1)
		sp = QStyle::SP_MessageBoxCritical;
	else if (mark == TierMark::L2)
		sp = QStyle::SP_MessageBoxWarning;
	return s->standardIcon(sp).pixmap(QSize(px, px), dpr > 0.0 ? dpr : 1.0);
}

QPixmap tier_pixmap(TierMark mark, int px, const QColor &color, qreal dpr, TierIconSet set)
{
	if (set == TierIconSet::WindowsStandard)
		return standard_tier_mark_pixmap(mark, px, dpr);
	return tier_mark_pixmap(mark, px, color, dpr);
}

QPixmap row_mark_pixmap(RowMark mark, int px, const QColor &color, qreal dpr)
{
	QPixmap pm = canvas(px, dpr);
	QPainter p(&pm);
	begin16(p, px);
	// 円の中の記号。円を塗り、記号を抜く。
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawEllipse(QRectF(0.25, 0.25, 15.5, 15.5));
	knock_mode(p);
	const QPen pen(Qt::black, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	switch (mark) {
	case RowMark::Waiting: // 時計の針
		p.setPen(pen);
		p.drawLine(QPointF(8.0, 8.0), QPointF(8.0, 4.0));
		p.drawLine(QPointF(8.0, 8.0), QPointF(11.0, 9.8));
		break;
	case RowMark::NoVideo: // 斜めの線 (届かない)
		p.setPen(pen);
		p.drawLine(QPointF(4.8, 11.2), QPointF(11.2, 4.8));
		break;
	case RowMark::NotSharing: // 横の線 (共有していない)
		p.setPen(pen);
		p.drawLine(QPointF(4.5, 8.0), QPointF(11.5, 8.0));
		break;
	case RowMark::Quality: // 下向きの矢印
		p.setPen(pen);
		p.drawLine(QPointF(8.0, 4.0), QPointF(8.0, 11.5));
		p.drawLine(QPointF(5.0, 8.8), QPointF(8.0, 11.8));
		p.drawLine(QPointF(11.0, 8.8), QPointF(8.0, 11.8));
		break;
	}
	return pm;
}

QPixmap slot_mark_pixmap(SlotMark mark, int px, const QColor &color, qreal dpr)
{
	QPixmap pm = canvas(px, dpr);
	QPainter p(&pm);
	begin16(p, px);
	p.setPen(QPen(color, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);
	// 画面の輪郭と台。台がないと、四角なしの形が空のチェックボックスに見える。
	p.drawRoundedRect(QRectF(0.75, 1.25, 14.5, 10.5), 1.0, 1.0);
	p.drawLine(QPointF(4.5, 14.5), QPointF(11.5, 14.5));
	p.drawLine(QPointF(8.0, 11.75), QPointF(8.0, 14.5));
	QRectF cell;
	switch (mark) {
	case SlotMark::None:
		return pm;
	case SlotMark::TopLeft:
		cell = QRectF(2.25, 2.75, 5.5, 3.5);
		break;
	case SlotMark::TopRight:
		cell = QRectF(8.25, 2.75, 5.5, 3.5);
		break;
	case SlotMark::BottomLeft:
		cell = QRectF(2.25, 6.75, 5.5, 3.5);
		break;
	case SlotMark::BottomRight:
		cell = QRectF(8.25, 6.75, 5.5, 3.5);
		break;
	case SlotMark::Full:
		cell = QRectF(2.25, 2.75, 11.5, 7.5);
		break;
	}
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawRect(cell);
	return pm;
}

QPixmap audio_mark_pixmap(AudioMark mark, bool muted, int px, const QColor &color, qreal dpr, const QColor &slash)
{
	QPixmap pm = canvas(px, dpr);
	QPainter p(&pm);
	begin16(p, px);
	// 画面共有のアイコンと同じ、1.5 の輪郭線の描き方にそろえる。
	p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);
	if (mark == AudioMark::Voice) {
		// マイク: カプセル、受けの弧、支柱と台。
		p.drawRoundedRect(QRectF(5.5, 1.5, 5.0, 8.0), 2.5, 2.5);
		QPainterPath cup;
		cup.moveTo(3.25, 7.0);
		cup.arcTo(QRectF(3.25, 3.5, 9.5, 9.0), 180.0, 180.0);
		p.drawPath(cup);
		p.drawLine(QPointF(8.0, 12.5), QPointF(8.0, 14.5));
		p.drawLine(QPointF(5.5, 14.5), QPointF(10.5, 14.5));
	} else {
		// スピーカー: 本体と、音の広がり 2 本。
		QPainterPath body;
		body.moveTo(1.75, 6.0);
		body.lineTo(4.5, 6.0);
		body.lineTo(8.0, 2.75);
		body.lineTo(8.0, 13.25);
		body.lineTo(4.5, 10.0);
		body.lineTo(1.75, 10.0);
		body.closeSubpath();
		p.drawPath(body);
		p.drawArc(QRectF(5.0, 5.0, 6.0, 6.0), -50 * 16, 100 * 16);
		p.drawArc(QRectF(3.0, 3.0, 10.0, 10.0), -50 * 16, 100 * 16);
	}
	if (muted) {
		// 斜線。まわりを抜いて、図の線と重なって潰れないようにする。
		p.save();
		knock_mode(p);
		p.setPen(QPen(Qt::black, 4.0, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(2.5, 2.5), QPointF(13.5, 13.5));
		p.restore();
		p.setPen(QPen(slash.isValid() ? slash : color, 1.5, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(2.5, 2.5), QPointF(13.5, 13.5));
	}
	return pm;
}

QIcon fallback_dots_icon(const ThemeRead &theme, int px, const QColor &color)
{
	if (theme.has_dots)
		return QIcon();
	QIcon icon;
	// 画面の拡大率ごとに描いた画像を持たせる (QIcon がいちばん近いものを選ぶので、ボタンが別の倍率の画面へ移っても
	// ぼけない)。
	for (const qreal dpr : kIconRatios) {
		QPixmap pm = canvas(px, dpr);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		const qreal s = px / 16.0;
		for (int i = 0; i < 3; ++i)
			p.drawEllipse(QPointF(8.0 * s, (2.0 + 6.0 * i) * s), 1.8 * s, 1.8 * s);
		p.end();
		icon.addPixmap(pm);
	}
	return icon;
}

QPixmap chevron_pixmap(Chevron dir, int px, const QColor &color, qreal dpr)
{
	QPixmap pm = canvas(px, dpr);
	QPainter p(&pm);
	begin16(p, px);
	p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);
	// 幅 9、高さ 5 の山形を、グリッドの中央に描く (横 3.5 から 12.5、縦 5.5 から 10.5。右向きは縦横を入れ替える)。
	QPainterPath path;
	if (dir == Chevron::Right) {
		path.moveTo(5.5, 3.5);
		path.lineTo(10.5, 8.0);
		path.lineTo(5.5, 12.5);
	} else {
		const qreal tip = dir == Chevron::Up ? 5.5 : 10.5;
		const qreal ends = dir == Chevron::Up ? 10.5 : 5.5;
		path.moveTo(3.5, ends);
		path.lineTo(8.0, tip);
		path.lineTo(12.5, ends);
	}
	p.drawPath(path);
	return pm;
}

MarkLabel::MarkLabel(QWidget *parent) : QLabel(parent)
{
	setAlignment(Qt::AlignCenter);
	setFixedSize(px_, px_);
}

void MarkLabel::set_painter(int px, Painter painter)
{
	px_ = px > 0 ? px : 16;
	painter_ = std::move(painter);
	setFixedSize(px_, px_);
	refresh();
}

void MarkLabel::refresh()
{
	if (!painter_) {
		clear();
		return;
	}
	setPixmap(painter_(devicePixelRatioF()));
}

void MarkLabel::paintEvent(QPaintEvent *e)
{
	// テーマがラベルに付ける背景と枠線 (QSS の background、border) は、QFrame の描き方のまま描く。中身の画像だけを
	// 自分で置く。
	QFrame::paintEvent(e);
	QPixmap pm = pixmap();
	if (pm.isNull())
		return;
	// setEnabled(false) のウィジェットの中では、QLabel と同じく、スタイルが作る QIcon::Disabled の画像にする
	// (QStyle::drawItemPixmap と同じ処理)。
	if (!isEnabled()) {
		QStyleOption option;
		option.initFrom(this);
		pm = style()->generatedIconPixmap(QIcon::Disabled, pm, &option);
	}
	const qreal dpr = pm.devicePixelRatio() > 0.0 ? pm.devicePixelRatio() : 1.0;
	const QSizeF logical = QSizeF(pm.size()) / dpr;
	const QPointF at((width() - logical.width()) / 2.0, (height() - logical.height()) / 2.0);
	QPainter p(this);
	p.drawPixmap(at, pm);
}

bool MarkLabel::event(QEvent *e)
{
	if (e->type() == QEvent::DevicePixelRatioChange)
		refresh();
	return QLabel::event(e);
}

} // namespace cordmux::dock::ui

// SPDX-License-Identifier: Apache-2.0

// scen_common.h の実装。

#include "scen_common.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

#include "dock/accounts_bar.h"
#include "dock/bottom_bar.h"
#include "dock/dock_tabs.h"
#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/ui/focus_ring.h"
#include "query.h"

namespace cordmux::dock_drive {

namespace {

double channel(double v)
{
	v /= 255.0;
	return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

} // namespace

Frame frame_of(dock::DockWidget *widget)
{
	Frame f;
	f.first_line = global_rect(descendant_of<dock::FirstLineBar>(widget));
	f.accounts = global_rect(descendant_of<dock::AccountsBar>(widget));
	f.tab_bar = global_rect(descendant_of<dock::DockTabBar>(widget));
	f.bottom = global_rect(descendant_of<dock::BottomBar>(widget));
	return f;
}

QString frame_text(const Frame &f)
{
	auto r = [](const QRect &q) { return QStringLiteral("(%1,%2 %3x%4)").arg(q.x()).arg(q.y()).arg(q.width()).arg(q.height()); };
	return r(f.first_line) + r(f.accounts) + r(f.tab_bar) + r(f.bottom);
}

double relative_luminance(const QColor &c)
{
	return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue());
}

double contrast_ratio(const QColor &a, const QColor &b)
{
	const double la = relative_luminance(a);
	const double lb = relative_luminance(b);
	return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QImage grab_window_image(QWidget *top)
{
	QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
	QImage image(top->size(), QImage::Format_RGB32);
	image.fill(QColor(255, 0, 255)); // 描かれていない所があれば目立つ色
	top->render(&image);
	return image;
}

QStringList click_focus_controls(QWidget *root)
{
	QStringList names;
	const auto add = [&names](QWidget *w) {
		if (w->focusPolicy() & Qt::ClickFocus)
			names << (w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName());
	};
	for (QAbstractButton *b : root->findChildren<QAbstractButton *>())
		add(b);
	for (QComboBox *c : root->findChildren<QComboBox *>())
		add(c);
	return names;
}

void pump(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

RingMeasure measure_ring(QWidget *top, QWidget *focused)
{
	RingMeasure m;
	if (top == nullptr || focused == nullptr)
		return m;
	const QRect hit = dock::ui::focus_hit_rect_of(focused);
	m.hit = QRect(focused->mapTo(top, hit.topLeft()), hit.size());
	// フォーカスリングのウィジェットは、ウィンドウの中に 1 つある (dock/ui/focus_ring.h)。
	// focused を追っているものを探す。
	dock::ui::FocusRing *ring = nullptr;
	for (QWidget *w : top->findChildren<QWidget *>(QStringLiteral("cordmuxFocusRing"))) {
		if (w->window() != top->window())
			continue; // 子のウィンドウ (このウィンドウを親にした確認のダイアログなど) のリングは数えない
		if (auto *r = dynamic_cast<dock::ui::FocusRing *>(w)) {
			m.ring_widget_found = true;
			if (r->target() == focused)
				ring = r;
		}
	}
	if (ring == nullptr)
		return m;
	m.ring_visible = ring->isVisible();
	m.ring_rect = QRect(ring->mapTo(top, QPoint(0, 0)), ring->size());
	m.with_ring = grab_window_image(top);
	if (!m.ring_visible)
		return m;
	ring->hide();
	const QImage without = grab_window_image(top);
	ring->show();
	QCoreApplication::processEvents();

	// 違うピクセル (リング) と、その下の背景 (リングのない画像の同じピクセル) のコントラスト比。
	struct Px {
		int x;
		int y;
		int diff;
	};
	std::vector<Px> pixels;
	int max_diff = 0;
	for (int y = 0; y < m.with_ring.height(); ++y) {
		const QRgb *a = reinterpret_cast<const QRgb *>(m.with_ring.constScanLine(y));
		const QRgb *b = reinterpret_cast<const QRgb *>(without.constScanLine(y));
		for (int x = 0; x < m.with_ring.width(); ++x) {
			const int d = std::abs(qRed(a[x]) - qRed(b[x])) + std::abs(qGreen(a[x]) - qGreen(b[x])) +
				      std::abs(qBlue(a[x]) - qBlue(b[x]));
			if (d == 0)
				continue;
			pixels.push_back({x, y, d});
			max_diff = std::max(max_diff, d);
			m.bbox = m.bbox.united(QRect(x, y, 1, 1));
		}
	}
	m.drawn = !pixels.empty();
	{
		std::vector<QPoint> at;
		at.reserve(pixels.size());
		for (const Px &p : pixels)
			at.emplace_back(p.x, p.y);
		count_ring_sides(at, m.hit, m.ring_rect, m.side_len, m.side_cover);
	}
	// リングの芯 (差が最大の半分以上で、線の端の中間の色を除いたピクセル) のコントラスト比。
	std::vector<double> ratios;
	for (const Px &p : pixels) {
		if (p.diff * 2 < max_diff)
			continue;
		ratios.push_back(contrast_ratio(QColor(m.with_ring.pixel(p.x, p.y)), QColor(without.pixel(p.x, p.y))));
	}
	m.core_pixels = static_cast<int>(ratios.size());
	if (!ratios.empty()) {
		std::sort(ratios.begin(), ratios.end());
		m.contrast_p05 = ratios[ratios.size() / 20];
		m.contrast_median = ratios[ratios.size() / 2];
	}
	return m;
}

void count_ring_sides(const std::vector<QPoint> &pixels, const QRect &hit, const QRect &ring_rect, int side_len[4],
		      int side_cover[4])
{
	// 辺ごとに、押せる範囲の辺の外側から、フォーカスリングの矩形までの間 (リングの太さと余白) を見る。長さは
	// 押せる範囲の辺の長さで、角は数えない。リングのピクセルの外接矩形 (bbox) を押せる範囲と比べるだけでは、
	// 表示範囲の外で切れた辺を見落とす (上下の 2 辺だけのリングでも、範囲を囲んでいるとみなしてしまう)。
	// 辺ごとに描かれた位置を数えれば、切れた辺は 0 になる。
	for (int i = 0; i < 4; ++i) {
		side_len[i] = 0;
		side_cover[i] = 0;
	}
	if (!hit.isValid() || !ring_rect.isValid())
		return;
	side_len[0] = side_len[2] = hit.height();
	side_len[1] = side_len[3] = hit.width();
	std::vector<char> seen[4];
	for (int i = 0; i < 4; ++i)
		seen[i].assign(static_cast<size_t>(side_len[i]), 0);
	auto mark = [&](int side, int at) {
		char &c = seen[side][static_cast<size_t>(at)];
		if (c == 0) {
			c = 1;
			++side_cover[side];
		}
	};
	for (const QPoint &p : pixels) {
		const bool in_rows = p.y() >= hit.top() && p.y() <= hit.bottom();
		const bool in_cols = p.x() >= hit.left() && p.x() <= hit.right();
		if (in_rows && p.x() >= ring_rect.left() && p.x() < hit.left())
			mark(0, p.y() - hit.top());
		if (in_cols && p.y() >= ring_rect.top() && p.y() < hit.top())
			mark(1, p.x() - hit.left());
		if (in_rows && p.x() > hit.right() && p.x() <= ring_rect.right())
			mark(2, p.y() - hit.top());
		if (in_cols && p.y() > hit.bottom() && p.y() <= ring_rect.bottom())
			mark(3, p.x() - hit.left());
	}
}

int ring_sides_drawn(const int side_len[4], const int side_cover[4])
{
	int n = 0;
	for (int i = 0; i < 4; ++i)
		if (side_len[i] > 0 && side_cover[i] >= side_len[i])
			++n;
	return n;
}

} // namespace cordmux::dock_drive

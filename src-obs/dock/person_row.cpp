// SPDX-License-Identifier: Apache-2.0

// 参加者の一覧の 1 行 (person_row.h) を実装する。

#include "dock/person_row.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOptionButton>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <chrono>

#include "dock/ui/elide_label.h"
#include "dock/ui/set_class.h"
#include "dock/ui/status_icons.h"
#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

namespace keys = ledger::text::keys;

// 行に [...] を置くか。checkable の行に加えて、[自動で載せる] だけのメニューを持つ行 (サブアカウントがない構成の
// 自分の行) も置く。
bool shows_menu(const ledger::dockview::PersonRow &view)
{
	return view.checkable || view.menu.auto_add_only;
}

QString join_lines(const std::vector<QString> &lines)
{
	// 別々のテキスト (それぞれが ini の 1 つの文字列か、Discord から来た名前) を改行で並べる。1 行の中で文字列を連結しない。
	// まったく同じテキストは 2 回並べない。押せない理由の文が、押した直後に載せられなかったことを言う 1 行を含むときの
	// 扱いは refresh_accessibility が決める (ここで省くのは、テキストが完全に一致するときだけ)。
	QString out;
	std::vector<QString> seen;
	for (const QString &line : lines) {
		if (line.isEmpty() || std::find(seen.begin(), seen.end(), line) != seen.end())
			continue;
		seen.push_back(line);
		if (!out.isEmpty())
			out += QLatin1Char('\n');
		out += line;
	}
	return out;
}

// 行の状態のアイコン。どの状態にもアイコンがある。default を置かないのは、列挙子が増えたときに、アイコンを決めていない
// 状態をコンパイラーの警告 (-Wswitch) で見つけるためである。
std::optional<ui::RowMark> mark_for(ledger::dockview::RowState state)
{
	using ledger::dockview::RowState;
	switch (state) {
	case RowState::NotSharing:
		return ui::RowMark::NotSharing;
	case RowState::Waiting:
		return ui::RowMark::Waiting;
	case RowState::NoVideo:
		return ui::RowMark::NoVideo;
	}
	return std::nullopt;
}

// 行の状態のテキストに続ける「枠に何が映っているか」の 1 文。状態のテキストのキーから引く。状態の種類 (RowState) は
// テキストの選び方と 1 対 1 で対応しない (画面共有していない人と、まだ届いていない人と、届かなくなった人は、状態の種類とテキストが別である)。画質を
// 下げている 4 つの状態には追加しない (row_quality.tip が、下げた映像が映っていることを言う)。cordmux が確認できる
// のは自分の枠までなので、テキストは「配信」ではなく「枠」と書く。
const char *slot_tip_key_for(const std::string &status_key)
{
	if (status_key == keys::kDockRowWaitingStatus)
		return keys::kDockRowWaitingTip;
	if (status_key == keys::kDockRowNotReceivingStatus)
		return keys::kDockRowNotReceivingTip;
	if (status_key == keys::kDockRowNotSharingStatus)
		return keys::kDockRowNotSharingTip;
	return nullptr;
}

// 画質の状態のテキストに続ける、原因と影響の文。下げた映像を送っている 4 つの状態は row_quality.tip (「送る映像を
// 下げています」)。映像を表示できない状態 (送信側がこの人の映像を送らない。src-vencord/encode/ladder.ts の
// Unavailable) は row_cant_show.tip (「この人の映像を送っていません」。原因のテキストは row_quality.tip とそろえてある)。
// 1 文を追加しないのは画質を下げている状態だけで、映像を表示できない状態では下げた映像も映らないので追加する。
// 追加しないと、なぜ映らないかがツールチップでもスクリーンリーダーでも分からない。row_quality.tip を付けると、「下げています」と
// 「表示できません」が並んで、事実と違う文になる。
const char *quality_tip_key_for(const std::string &quality_key)
{
	if (quality_key == keys::kDockRowCantShowStatus)
		return keys::kDockRowCantShowTip;
	return keys::kDockRowQualityTip;
}

bool same_theme(const ui::ThemeRead &a, const ui::ThemeRead &b)
{
	return a.text == b.text && a.muted == b.muted && a.danger == b.danger && a.warning == b.warning &&
	       a.success == b.success && a.base_bg == b.base_bg &&
	       a.window_bg == b.window_bg && a.notice_bg == b.notice_bg && a.has_muted == b.has_muted &&
	       a.has_danger == b.has_danger && a.has_notice == b.has_notice &&
	       a.desktop_cap.cacheKey() == b.desktop_cap.cacheKey() && a.has_desktop_cap == b.has_desktop_cap &&
	       a.dots.cacheKey() == b.dots.cacheKey() && a.has_dots == b.has_dots && a.icon_px == b.icon_px &&
	       a.dark == b.dark;
}

bool same_style(const ui::DockStyle &a, const ui::DockStyle &b)
{
	return a.first_line_scale == b.first_line_scale && a.l1_border_px == b.l1_border_px &&
	       a.tier_icon_set == b.tier_icon_set && a.spacing_small_em == b.spacing_small_em &&
	       a.spacing_large_em == b.spacing_large_em && a.group_gap_em == b.group_gap_em &&
	       a.row_min_height_px == b.row_min_height_px && a.name_floor_em == b.name_floor_em;
}

// テキストの最初と最後の 1 字 (書記素。絵文字や結合文字を分けない)。
QString first_grapheme(const QString &text)
{
	if (text.isEmpty())
		return {};
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	const qsizetype next = finder.toNextBoundary();
	return text.left(next > 0 ? next : 1);
}

QString last_grapheme(const QString &text)
{
	if (text.isEmpty())
		return {};
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	finder.toEnd();
	const qsizetype prev = finder.toPreviousBoundary();
	return text.mid(prev >= 0 ? prev : text.size() - 1);
}

// 2 行目の部分を省いても中身が読める最小の幅 (name_check.h の PartIn::head_px)。
//   - ユーザー名 (真ん中を省く): 最初の 1 字 + 省略記号 + 最後の 1 字 (見分けに必要な末尾を残す)。
//   - ほか (末尾を省く): 「声:」のような語 + 値の最初の 1 字 + 省略記号 (「声: a...」)。語だけの「声: ...」にしない。
//     値がテキストの中に見つからなければ、テキストの最初の 1 字 + 省略記号。
// 省略記号は U+2026 (Qt の elidedText が、フォントにあれば使う文字)。先頭を測った幅で Qt が実際に先頭を残すかを
// 確認し、残さなければ 1 px ずつ (3 px まで) 広げる (文字の組み合わせのカーニングで 1-2 px ずれることがある)。
// 一律に 2 px 足すと、最小幅で声のラベルが必要より広くなり、隣のユーザー名が 1 字減る。
int line2_head_px(const QFontMetrics &fm, const NameBlock::Part &part)
{
	const QString ellipsis(QChar(0x2026));
	const QString text = part.short_text.isEmpty() ? part.full : part.short_text;
	if (text.isEmpty())
		return 0;
	QString head;
	if (part.kind == rows::PartKind::Username) {
		head = first_grapheme(text) + ellipsis + last_grapheme(text);
	} else {
		qsizetype pos = part.value.isEmpty() ? -1 : text.indexOf(part.value);
		if (pos < 0)
			pos = 0;
		head = text.left(pos) + first_grapheme(text.mid(pos)) + ellipsis;
	}
	const Qt::TextElideMode mode = part.kind == rows::PartKind::Username ? Qt::ElideMiddle : Qt::ElideRight;
	const int base = fm.horizontalAdvance(head);
	for (int extra = 0; extra <= 3; ++extra) {
		const QString shown = fm.elidedText(text, mode, base + extra);
		if (shown.size() >= head.size() && fm.horizontalAdvance(shown) <= base + extra)
			return base + extra;
	}
	return base + 3;
}

} // namespace

bool operator==(const RowEnv &a, const RowEnv &b)
{
	return a.metrics == b.metrics && same_theme(a.theme, b.theme) && same_style(a.style, b.style) &&
	       a.muted_line2 == b.muted_line2 && a.disabled_alpha == b.disabled_alpha &&
	       a.enabled_text == b.enabled_text &&
	       a.text_indent_px == b.text_indent_px && a.status_keep_em == b.status_keep_em;
}

// ---- NameBlock ----

QLabel *NameBlock::make_tag(const char *key, const char *tip_key)
{
	auto *tag = new QLabel(ui::resolve_key(key), owner_);
	tag->setObjectName(QStringLiteral("cordmuxRowTag"));
	tag->setTextFormat(Qt::PlainText);
	// ツールチップのあるタグだけマウスを受ける。クリックと右クリックは、QLabel が使わずに行へ渡す (テキストの選択を
	// 許していないため)。ツールチップのないタグは、マウスを受けない。
	if (tip_key != nullptr)
		tag->setToolTip(ui::resolve_key(tip_key));
	else
		tag->setAttribute(Qt::WA_TransparentForMouseEvents);
	tag->hide();
	return tag;
}

NameBlock::NameBlock(QWidget *owner) : owner_(owner)
{
	name_ = new ui::ElideLabel(owner);
	name_->setObjectName(QStringLiteral("cordmuxRowName"));
	// 名前は Qt の末尾の省略にし、数の語を途中で切らない処理 (set_keep_numbers) は使わない。この処理を使うのは、件数と
	// 時刻のテキストだけである。
	name_->set_keep_numbers(false);
	name_->setAttribute(Qt::WA_TransparentForMouseEvents);
	name_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	main_tag_ = make_tag(keys::kDockAccountMainLabel);
	// [自動] のタグのツールチップは、行のメニューの [自動で枠に載せる] と同じ文言にする (何の自動かを示す)。
	auto_tag_ = make_tag(keys::kDockRowBadgeAutoLabel, keys::kDockRowAutoAddMenu);
}

void NameBlock::set_name(const QString &full)
{
	name_->set_full_text(full);
	place_name_and_tags(owner_->width() - left_);
}

void NameBlock::set_tags(bool main_tag, bool auto_tag)
{
	if (want_main_tag_ == main_tag && want_auto_tag_ == auto_tag)
		return;
	want_main_tag_ = main_tag;
	want_auto_tag_ = auto_tag;
	place_name_and_tags(owner_->width() - left_);
}

void NameBlock::place_name_and_tags(int width)
{
	const int gap = env_.metrics.small_px;
	int tags_px = 0;
	for (QLabel *tag : {main_tag_, auto_tag_}) {
		const bool wanted = tag == main_tag_ ? want_main_tag_ : want_auto_tag_;
		if (wanted)
			tags_px += gap + tag->sizeHint().width();
	}
	const int natural = name_->fontMetrics().horizontalAdvance(name_->full_text()) + 2;
	// 名前に下限の幅を残せないときはタグを隠し、名前の幅を優先する。
	const bool tags_fit = tags_px > 0 && width - tags_px >= std::min(natural, env_.metrics.name_floor_px);
	const int name_px = tags_fit ? std::min(natural, std::max(0, width - tags_px)) : std::max(0, width);
	name_->setGeometry(left_, 0, name_px, line1_h_);
	int x = left_ + name_px;
	for (QLabel *tag : {main_tag_, auto_tag_}) {
		const bool wanted = tag == main_tag_ ? want_main_tag_ : want_auto_tag_;
		const bool shown = wanted && tags_fit;
		if (shown) {
			const QSize hint = tag->sizeHint();
			tag->setGeometry(x + gap, std::max(0, (line1_h_ - hint.height()) / 2), hint.width(), hint.height());
			x += gap + hint.width();
		}
		if (tag->isHidden() == shown)
			tag->setVisible(shown);
	}
}

void NameBlock::set_name_mode(Qt::TextElideMode mode)
{
	name_->set_elide_mode(mode);
}

void NameBlock::set_line2(const std::vector<Part> &parts)
{
	const bool same = parts.size() == parts_.size() &&
			  std::equal(parts.begin(), parts.end(), parts_.begin(), [](const Part &a, const Part &b) {
				  return a.kind == b.kind && a.full == b.full && a.short_text == b.short_text &&
					 a.value == b.value;
			  });
	if (same)
		return;
	parts_ = parts;
	while (labels_.size() > parts_.size()) {
		delete labels_.back();
		labels_.pop_back();
	}
	while (labels_.size() < parts_.size()) {
		auto *label = new QLabel(owner_);
		label->setObjectName(QStringLiteral("cordmuxRowLine2"));
		label->setTextFormat(Qt::PlainText);
		label->setWordWrap(false);
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		ui::set_class(label, env_.muted_line2 ? QStringLiteral("text-muted") : QString());
		label->show();
		labels_.push_back(label);
	}
	set_dimmed(dimmed_);
	fit_line2(owner_->width() - left_);
}

void NameBlock::set_env(const RowEnv &env)
{
	env_ = env;
	for (QLabel *label : labels_)
		ui::set_class(label, env_.muted_line2 ? QStringLiteral("text-muted") : QString());
	// タグはテキストと同じ大きさのまま、細い枠線で囲む。色は補足のテキストの色で、状態の色にしない。
	const QColor line = env_.muted_line2 ? env_.theme.muted : env_.theme.text;
	for (QLabel *tag : {main_tag_, auto_tag_}) {
		ui::set_class(tag, env_.muted_line2 ? QStringLiteral("text-muted") : QString());
		ui::set_own_style(tag, "border",
				  QStringLiteral("border: 1px solid %1; border-radius: 2px; padding: 0px 2px;")
					  .arg(line.isValid() ? line.name(QColor::HexRgb) : QStringLiteral("transparent")));
	}
	set_dimmed(dimmed_);
}

void NameBlock::set_dimmed(bool dimmed)
{
	dimmed_ = dimmed;
	// 押せない人の名前は、名前の中の絵文字も含めて同じ薄さにする。カラーの絵文字はテーマの押せないテキストの色で
	// 塗れないので、ラベルを押せるときの色で描き、ラベルごと不透明度を下げて、押せないテキストの色に合わせる。
	const bool apply = dimmed && env_.disabled_alpha < 0.999;
	auto dim = [&](QLabel *label, const QColor &color) {
		if (apply && color.isValid()) {
			ui::set_own_style(label, "color", QStringLiteral("color: %1;").arg(color.name(QColor::HexRgb)));
			auto *effect = qobject_cast<QGraphicsOpacityEffect *>(label->graphicsEffect());
			if (effect == nullptr) {
				effect = new QGraphicsOpacityEffect(label);
				label->setGraphicsEffect(effect);
			}
			effect->setOpacity(env_.disabled_alpha);
		} else {
			ui::set_own_style(label, "color", QString());
			if (label->graphicsEffect() != nullptr)
				label->setGraphicsEffect(nullptr);
		}
	};
	dim(name_, env_.enabled_text);
	// 2 行目も名前と同じ本文の色で描き、同じ不透明度だけを掛ける (ラベル全体を同じ薄さにする)。補足であることは、
	// 位置と量で表す。押せるときと同じ薄い色のテキストに不透明度を重ねると、薄さが 2 重になり、Yami で背景との
	// コントラスト比が 2.39 (同じ行の名前は 4.92) まで下がる。
	for (QLabel *label : labels_)
		dim(label, env_.enabled_text);
	// タグも名前と同じ薄さにする。不透明度は枠線にも掛かる。
	dim(main_tag_, env_.enabled_text);
	dim(auto_tag_, env_.enabled_text);
}

int NameBlock::line2_height() const
{
	int h = 0;
	for (const QLabel *label : labels_)
		h = std::max(h, label->sizeHint().height());
	if (h == 0)
		h = name_->fontMetrics().height();
	return h;
}

int NameBlock::height_for(int line1_h) const
{
	return line1_h + (parts_.empty() ? 0 : line2_height());
}

void NameBlock::place(int left, int width, int line1_h)
{
	left_ = left;
	line1_h_ = line1_h;
	place_name_and_tags(width - left);
	fit_line2(width - left);
}

void NameBlock::fit_line2(int width)
{
	line2_shortened_ = false;
	if (parts_.empty())
		return;
	const int gap = env_.metrics.large_px;
	std::vector<rows::PartIn> ins;
	ins.reserve(parts_.size());
	for (std::size_t i = 0; i < parts_.size(); ++i) {
		const QFontMetrics fm = labels_[i]->fontMetrics();
		rows::PartIn in;
		in.kind = parts_[i].kind;
		in.full_px = fm.horizontalAdvance(parts_[i].full);
		in.short_px = parts_[i].short_text.isEmpty() ? -1 : fm.horizontalAdvance(parts_[i].short_text);
		in.head_px = line2_head_px(fm, parts_[i]);
		ins.push_back(in);
	}
	const std::vector<rows::PartOut> outs = rows::fit_line2(ins, std::max(0, width), gap);
	const int h = line2_height();
	int x = left_;
	for (std::size_t i = 0; i < parts_.size(); ++i) {
		QLabel *label = labels_[i];
		const rows::PartOut &o = outs[i];
		if (!o.shown) {
			label->hide();
			line2_shortened_ = true;
			continue;
		}
		const QString text = o.use_short ? parts_[i].short_text : parts_[i].full;
		QString shown = text;
		if (o.elide != rows::ElideMode::None) {
			const Qt::TextElideMode mode = o.elide == rows::ElideMode::Middle ? Qt::ElideMiddle : Qt::ElideRight;
			const QFontMetrics fm = label->fontMetrics();
			// Qt が省いたテキストは、全体の幅で測ると 1-2 px はみ出すことがある (文字の組み合わせのカーニング)。
			// はみ出さなくなるまで幅を狭めて省き直し、右端が切れただけのラベルを作らない。
			int room = o.width_px;
			shown = fm.elidedText(text, mode, room);
			while (room > 0 && fm.horizontalAdvance(shown) > o.width_px) {
				--room;
				shown = fm.elidedText(text, mode, room);
			}
		}
		if (o.use_short || shown != text)
			line2_shortened_ = true;
		if (label->text() != shown)
			label->setText(shown);
		label->setGeometry(x, line1_h_, o.width_px, h);
		label->show();
		x += o.width_px + gap;
	}
}

QString NameBlock::full_name() const
{
	return name_->full_text();
}

QString NameBlock::shown_name() const
{
	return name_->text();
}

int NameBlock::name_width() const
{
	return name_->contentsRect().width();
}

std::vector<QString> NameBlock::line2_full() const
{
	std::vector<QString> out;
	out.reserve(parts_.size());
	for (const Part &p : parts_)
		out.push_back(p.full);
	return out;
}

int NameBlock::text_right() const
{
	int right = name_->x() + name_->fontMetrics().horizontalAdvance(name_->text());
	for (const QLabel *label : labels_) {
		if (label->isHidden())
			continue;
		right = std::max(right, label->x() + label->fontMetrics().horizontalAdvance(label->text()));
	}
	return right;
}

// ---- RowCheck ----

RowCheck::RowCheck(QWidget *parent) : QCheckBox(parent), names_(this)
{
	setObjectName(QStringLiteral("cordmuxRowCheck"));
	setText(QString());
	// クリックではフォーカスを取らず、Tab でだけ取る。OBS が前面のまま Space を押して、同じ操作が働くのを防ぐ。
	setFocusPolicy(Qt::TabFocus);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void RowCheck::set_line1_height(int h)
{
	if (line1_h_ == h)
		return;
	line1_h_ = h;
	updateGeometry();
	names_.place(name_left(), width(), line1_h_);
	update();
}

void RowCheck::set_replaces(bool replaces)
{
	if (replaces_ == replaces)
		return;
	replaces_ = replaces;
	update();
}

void RowCheck::nextCheckState()
{
	if (!replaces_)
		QCheckBox::nextCheckState();
}

void RowCheck::set_avatar(const QPixmap &image, const QString &letter, const QColor &color)
{
	avatar_image_ = image;
	avatar_letter_ = letter;
	avatar_color_ = color;
	update();
}

void RowCheck::set_avatar_shown(bool shown)
{
	if (avatar_shown_ == shown)
		return;
	avatar_shown_ = shown;
	updateGeometry();
	names_.place(name_left(), width(), line1_h_);
	update();
}

int RowCheck::name_left() const
{
	if (!avatar_shown_)
		return text_left();
	return text_left() + rows::kRowAvatarPx + env_.metrics.small_px;
}

void RowCheck::set_env(const RowEnv &env)
{
	env_ = env;
	names_.set_env(env);
	updateGeometry();
}

QRect RowCheck::indicator_rect() const
{
	QStyleOptionButton option;
	initStyleOption(&option);
	option.text.clear();
	option.rect = QRect(0, 0, std::max(width(), 1), std::max(line1_h_, 1));
	QRect r = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
	// チェックボックスのインジケーターは 1 行目の高さの中央に置く (2 行目があっても上の行にそろえる)。
	r.moveTop((std::max(line1_h_, 1) - r.height()) / 2);
	return r;
}

int RowCheck::text_left() const
{
	QStyleOptionButton option;
	initStyleOption(&option);
	option.text.clear();
	option.rect = QRect(0, 0, 400, std::max(line1_h_, 1));
	return style()->subElementRect(QStyle::SE_CheckBoxContents, &option, this).left();
}

QRect RowCheck::focus_hit_rect() const
{
	// チェックボックスのインジケーターの左端から、見えているテキスト (名前と 2 行目) の右端まで。高さは行の全部。
	const QRect ind = indicator_rect();
	const int right = std::min(std::max(names_.text_right(), ind.right() + 1), width());
	return QRect(QPoint(ind.left(), 0), QPoint(right - 1, height() - 1));
}

QRect RowCheck::qt_click_rect() const
{
	QStyleOptionButton option;
	initStyleOption(&option);
	option.rect = QRect(0, 0, width(), height());
	return style()->subElementRect(QStyle::SE_CheckBoxClickRect, &option, this);
}

QSize RowCheck::sizeHint() const
{
	const int h = std::max(names_.height_for(line1_h_), env_.metrics.row_min_height_px);
	return QSize(name_left() + env_.metrics.name_floor_px, h);
}

QSize RowCheck::minimumSizeHint() const
{
	return sizeHint();
}

void RowCheck::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	QStyleOptionButton option;
	initStyleOption(&option);
	option.text.clear();
	option.rect = indicator_rect();
	painter.save();
	// 枠が埋まっている間のチェックは、押せないチェックと同じ薄さにする。押すと入れ替える相手を選べるので、押せる状態のまま描く。
	if (replaces_)
		painter.setOpacity(std::min(env_.disabled_alpha, 0.5));
	style()->drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter, this);
	painter.restore();
	if (!avatar_shown_)
		return;
	const int px = rows::kRowAvatarPx;
	const QRect area(text_left(), std::max(0, (std::max(line1_h_, px) - px) / 2), px, px);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	if (!isEnabled())
		painter.setOpacity(env_.disabled_alpha);
	if (!avatar_image_.isNull()) {
		QPainterPath circle;
		circle.addEllipse(QRectF(area));
		painter.setClipPath(circle);
		painter.drawPixmap(area, avatar_image_);
		painter.setClipping(false);
		return;
	}
	painter.setPen(Qt::NoPen);
	painter.setBrush(avatar_color_);
	painter.drawEllipse(area);
	painter.setPen(ui::black_or_white_on(avatar_color_));
	QFont letter_font = font();
	letter_font.setPixelSize(std::max(8, px * 11 / 20));
	painter.setFont(letter_font);
	painter.drawText(area, Qt::AlignCenter, avatar_letter_);
}

bool RowCheck::hitButton(const QPoint &pos) const
{
	return focus_hit_rect().contains(pos);
}

void RowCheck::resizeEvent(QResizeEvent *event)
{
	QCheckBox::resizeEvent(event);
	names_.place(name_left(), width(), line1_h_);
	if (on_geometry_changed)
		on_geometry_changed();
}

void RowCheck::mousePressEvent(QMouseEvent *event)
{
	if (press_blocked_ && press_blocked_()) {
		event->accept();
		return;
	}
	QCheckBox::mousePressEvent(event);
}

void RowCheck::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (press_blocked_ && press_blocked_()) {
		event->accept();
		return;
	}
	QCheckBox::mouseDoubleClickEvent(event);
}

void RowCheck::changeEvent(QEvent *event)
{
	QCheckBox::changeEvent(event);
	switch (event->type()) {
	case QEvent::EnabledChange:
		names_.set_dimmed(!isEnabled());
		break;
	case QEvent::FontChange:
	case QEvent::StyleChange:
		updateGeometry();
		names_.place(name_left(), width(), line1_h_);
		break;
	default:
		break;
	}
}

// ---- DotsButton ----

DotsButton::DotsButton(QWidget *parent) : QPushButton(parent)
{
	setObjectName(QStringLiteral("cordmuxRowMenuButton"));
	setFocusPolicy(Qt::TabFocus);
	setAutoDefault(false);
	ui::set_class(this, QStringLiteral("btn-tool icon-dots-vert"));
}

void DotsButton::set_env(const RowEnv &env)
{
	// テーマがアイコンを持たないときは、同じ意味の cordmux のアイコン (3 つの点) を本文の色で描く。
	// テーマがアイコンを持つときは、テーマがクラス (icon-dots-vert) で入れたアイコンをそのまま使う。cordmux の
	// アイコンを入れたのはこのウィジェット自身なので、テーマがアイコンを持つように変わったときだけ外し、polish し直して
	// テーマのアイコンを入れ直させる。
	if (!env.theme.has_dots) {
		setIcon(ui::fallback_dots_icon(env.theme, env.metrics.icon_px, env.theme.text));
		setIconSize(QSize(env.metrics.icon_px, env.metrics.icon_px));
		fallback_icon_ = true;
	} else if (fallback_icon_) {
		setIcon(QIcon());
		fallback_icon_ = false;
		style()->unpolish(this);
		style()->polish(this);
	}
}

void DotsButton::keyPressEvent(QKeyEvent *event)
{
	// Enter でもメニューを開く。QPushButton は、ダイアログの既定のボタンでないと Enter で押されない。
	if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
	    (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
		event->accept();
		click();
		return;
	}
	QPushButton::keyPressEvent(event);
}

void DotsButton::mousePressEvent(QMouseEvent *event)
{
	if (press_blocked_ && press_blocked_()) {
		event->accept();
		return;
	}
	QPushButton::mousePressEvent(event);
}

void DotsButton::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (press_blocked_ && press_blocked_()) {
		event->accept();
		return;
	}
	QPushButton::mouseDoubleClickEvent(event);
}

// ---- MuteButton ----

int mute_button_side(const QSize &dots_hint, int icon_px)
{
	return std::max(dots_hint.height(), icon_px + 4);
}

MuteButton::MuteButton(ui::AudioMark mark, QWidget *parent) : QAbstractButton(parent), mark_(mark)
{
	setCheckable(true);
	setFocusPolicy(Qt::TabFocus);
	setAttribute(Qt::WA_Hover, true);
}

void MuteButton::set_env(const RowEnv &env, int side_px)
{
	env_ = env;
	if (side_px_ != side_px) {
		side_px_ = side_px;
		updateGeometry();
	}
	update();
}

void MuteButton::set_muted(bool muted)
{
	if (isChecked() == muted)
		return;
	// 状態は View の値だけが決める (nextCheckState は何もしない)。
	setChecked(muted);
	update();
}

void MuteButton::set_speaking(bool speaking)
{
	if (speaking_ == speaking)
		return;
	speaking_ = speaking;
	update();
}

QSize MuteButton::sizeHint() const
{
	return QSize(side_px_, side_px_);
}

void MuteButton::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QColor text = env_.theme.text;
	const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	if (isEnabled()) {
		// 押せるボタンには背景を付けて、押せないアイコンと区別する (色はテーマの本文の色を薄くしたもの)。
		QColor fill = text;
		fill.setAlpha(isDown() ? 72 : underMouse() ? 48 : 24);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(box, 3.0, 3.0);
	} else {
		p.setOpacity(0.4);
	}
	const bool speaking = speaking_ && isEnabled() && !isChecked();
	// テーマが成功の色を持たないときは、本文の色の枠線で話していることを示す。
	const QColor speak = env_.theme.success.isValid() ? env_.theme.success : text;
	if (speaking) {
		p.setPen(QPen(speak, 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 3.0, 3.0);
	}
	QColor color = isChecked() || !env_.muted_line2 ? text : env_.theme.muted;
	if (speaking)
		color = speak;
	const int px = env_.metrics.icon_px;
	const QPixmap pm = ui::audio_mark_pixmap(mark_, isChecked(), px, color, devicePixelRatioF(), env_.theme.danger);
	p.drawPixmap(QPointF((width() - px) / 2.0, (height() - px) / 2.0), pm);
}

void MuteButton::keyPressEvent(QKeyEvent *event)
{
	// Enter でも切り替える (Space は QAbstractButton が受ける)。
	if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
	    (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
		event->accept();
		click();
		return;
	}
	QAbstractButton::keyPressEvent(event);
}

void MuteButton::mousePressEvent(QMouseEvent *event)
{
	if (press_blocked_ && press_blocked_()) {
		event->accept();
		return;
	}
	QAbstractButton::mousePressEvent(event);
}

void MuteButton::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (press_blocked_ && press_blocked_()) {
		event->accept();
		return;
	}
	QAbstractButton::mouseDoubleClickEvent(event);
}

// ---- PersonRowWidget ----

std::int64_t PersonRowWidget::now_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

PersonRowWidget::PersonRowWidget(const std::string &user_id, const RowEnv &env, QWidget *parent)
	: QWidget(parent),
	  user_id_(user_id),
	  env_(env)
{
	setObjectName(QStringLiteral("cordmuxRow"));
	setProperty("userId", QString::fromStdString(user_id_));
	layout_ = new QHBoxLayout(this);
	layout_->setContentsMargins(0, 0, 0, 0);
	// 名前のラベルと、その右のまとまり (状態のアイコン、状態のテキスト、解像度、共有の列、[...]) の間は大きい余白
	// (0.5 em) にする。名前と状態は別の事実なので、別の場所に置く。まとまりの中のウィジェットの間は小さい余白のままに
	// し、状態のアイコンを自分のテキストの側に寄せる。全部の間を小さい余白にすると、名前がラベルいっぱいのときに状態の
	// アイコンが名前と自分のテキストの真ん中に来て、名前の末尾とアイコンが 1 つのまとまりに見える。
	layout_->setSpacing(env_.metrics.large_px);

	tail_ = new QWidget(this);
	tail_->setProperty("userId", QString::fromStdString(user_id_));
	tail_layout_ = new QHBoxLayout(tail_);
	tail_layout_->setContentsMargins(0, 0, 0, 0);
	tail_layout_->setSpacing(env_.metrics.small_px);

	status_mark_ = new ui::MarkLabel(tail_);
	status_mark_->setObjectName(QStringLiteral("cordmuxRowStatus"));
	status_text_ = new ui::ElideLabel(tail_);
	status_text_->setObjectName(QStringLiteral("cordmuxRowStatusText"));
	// 状態のテキストは全文の幅を取るが、入らないときは名前の全文を先に残して縮む。幅の上限は fit_tail が
	// split_name_status で決める。折り返さず、1 行で末尾を省く。
	status_text_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	resolution_ = new QLabel(tail_);
	resolution_->setObjectName(QStringLiteral("cordmuxRowResolution"));
	resolution_->setTextFormat(Qt::PlainText);
	resolution_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	share_ = new ui::MarkLabel(tail_);
	share_->setObjectName(QStringLiteral("cordmuxRowShare"));
	// 共有していない行もアイコンの幅を確保し、アイコンをどの行でも同じ横の位置に置く。
	QSizePolicy keep = share_->sizePolicy();
	keep.setRetainSizeWhenHidden(true);
	share_->setSizePolicy(keep);
	voice_mute_ = new MuteButton(ui::AudioMark::Voice, tail_);
	voice_mute_->setObjectName(QStringLiteral("cordmuxRowVoiceMute"));
	screen_mute_ = new MuteButton(ui::AudioMark::Screen, tail_);
	screen_mute_->setObjectName(QStringLiteral("cordmuxRowScreenMute"));
	// ボタンは、隠している間も場所を空ける。行の間で画面のアイコンと [...] の位置がそろい、合成のソースの有無と画面共有の音の有無で
	// 位置が動かない。
	for (MuteButton *mute : {voice_mute_, screen_mute_}) {
		QSizePolicy mute_keep = mute->sizePolicy();
		mute_keep.setRetainSizeWhenHidden(true);
		mute->setSizePolicy(mute_keep);
	}
	dots_ = new DotsButton(tail_);
	for (QWidget *w : {static_cast<QWidget *>(status_mark_), static_cast<QWidget *>(status_text_),
			   static_cast<QWidget *>(resolution_)}) {
		w->setProperty("userId", QString::fromStdString(user_id_));
		tail_layout_->addWidget(w, 0, Qt::AlignVCenter);
	}
	// テキスト (状態のテキストと解像度) と共有の列の間は、別の事実を別の場所に置くために大きい余白 (0.5 em) にする。
	// レイアウトの spacing (小さい余白) に足す幅で、テキストを表示していない行では 0 (fit_tail)。空の項目なので、
	// レイアウトはこの前後に spacing を足さない。
	share_gap_ = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
	tail_layout_->addItem(share_gap_);
	// 画面のアイコンの右に、ミュートのボタンを [声][画面共有の音] の順で置く。
	voice_mute_->setProperty("userId", QString::fromStdString(user_id_));
	screen_mute_->setProperty("userId", QString::fromStdString(user_id_));
	share_->setProperty("userId", QString::fromStdString(user_id_));
	tail_layout_->addWidget(share_, 0, Qt::AlignVCenter);
	tail_layout_->addWidget(voice_mute_, 0, Qt::AlignVCenter);
	tail_layout_->addWidget(screen_mute_, 0, Qt::AlignVCenter);
	// スピーカーのボタンと [...] の間は、[...] のフォーカスリング (空き + 線) の外に背景の色が 1 px 以上残る幅にする
	// (name_check.h の dots_gap_px)。レイアウトの spacing (小さい余白) に足す幅で、[...] を表示しない行では 0 (fit_tail)。
	dots_gap_ = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
	tail_layout_->addItem(dots_gap_);
	dots_->setProperty("userId", QString::fromStdString(user_id_));
	tail_layout_->addWidget(dots_, 0, Qt::AlignVCenter);
	tail_layout_->setStretchFactor(status_text_, 0);
	status_mark_->hide();
	status_text_->hide();
	resolution_->hide();
	share_->hide();
	voice_mute_->hide();
	screen_mute_->hide();

	// 押した時点の View の値を反転した操作を渡す。コンポーネントの表示は変えず、次の周期の View で変わる。
	voice_mute_->set_press_guard([this] { return press_blocked_now(); });
	screen_mute_->set_press_guard([this] { return press_blocked_now(); });
	QObject::connect(voice_mute_, &QAbstractButton::clicked, this, [this] {
		if (!on_audio_action || departed_ || !has_view_)
			return;
		ledger::ActionArgs args;
		args.user_id = user_id_;
		args.on = !view_.audio.voice_muted;
		on_audio_action(ledger::ActionId::SetVoiceMute, args);
	});
	QObject::connect(screen_mute_, &QAbstractButton::clicked, this, [this] {
		if (!on_audio_action || departed_ || !has_view_)
			return;
		ledger::ActionArgs args;
		args.user_id = user_id_;
		args.on = !view_.audio.screen_muted;
		on_audio_action(ledger::ActionId::SetScreenAudioMute, args);
	});

	dots_->set_press_guard([this] { return press_blocked_now(); });
	QObject::connect(dots_, &QPushButton::clicked, this, [this] {
		if (on_menu_requested)
			on_menu_requested(dots_, dots_->mapToGlobal(QPoint(0, dots_->height())), false);
	});

	build_name_widget();
	layout_->addWidget(tail_, 0, Qt::AlignTop);
	set_env(env);
	changed_at_ms_ = now_ms();
}

void PersonRowWidget::build_name_widget()
{
	check_ = new RowCheck(this);
	check_->set_press_guard([this] { return press_blocked_now(); });
	check_->on_geometry_changed = [this] {
		if (on_geometry_changed)
			on_geometry_changed();
	};
	QObject::connect(check_, &QAbstractButton::clicked, this, [this] {
		if (on_check_clicked)
			on_check_clicked();
	});
	// 押せないチェックの上の右クリックでも、行のメニューを開く (eventFilter)。
	check_->installEventFilter(this);
	check_->setProperty("userId", QString::fromStdString(user_id_));
	layout_->insertWidget(0, check_, 1, Qt::AlignTop);
	check_->set_env(env_);
	if (has_view_)
		update_texts();
}

void PersonRowWidget::set_env(const RowEnv &env)
{
	env_ = env;
	layout_->setSpacing(env_.metrics.large_px);
	tail_layout_->setSpacing(env_.metrics.small_px);
	if (check_ != nullptr)
		check_->set_env(env_);
	dots_->set_env(env_);
	{
		const int side = mute_button_side(dots_->sizeHint(), env_.metrics.icon_px);
		voice_mute_->set_env(env_, side);
		screen_mute_->set_env(env_, side);
	}
	ui::set_class(resolution_, env_.muted_line2 ? QStringLiteral("text-muted") : QString());
	update_share_mark();
	if (has_view_)
		update_tail();
	relayout_heights();
	fit_tail();
}

bool PersonRowWidget::shape_differs(const ledger::dockview::PersonRow &view) const
{
	if (!has_view_)
		return true;
	return view.line2_parts.empty() != view_.line2_parts.empty();
}

void PersonRowWidget::apply(const ledger::dockview::PersonRow &view, bool allow_shape_change)
{
	ledger::dockview::PersonRow effective = view;
	if (has_view_ && !allow_shape_change && shape_differs(view)) {
		// 並びの保留の間は、行の高さを変える書き換えを後に残す。2 行目の中身の書き換えは、2 行目の有無が同じなら
		// その場で行う。
		if (view.line2_parts.empty() != view_.line2_parts.empty())
			effective.line2_parts = view_.line2_parts;
		shape_pending_ = true;
	} else {
		shape_pending_ = false;
	}
	// チェックは押すと自分で状態を変える。押した後に View が変わらなくても (apply() が断ったときなど)、毎回 View の値に
	// 戻す。チェックは枠に載せる指定そのものなので、画面だけが変わって何も起きない状態を残さない。
	if (check_ != nullptr && check_->isChecked() != effective.checked) {
		const QSignalBlocker block(check_);
		check_->setChecked(effective.checked);
	}
	if (has_view_ && effective == view_)
		return;
	view_ = effective;
	has_view_ = true;
	update_texts();
}

void PersonRowWidget::update_texts()
{
	const QString name = ui::resolve_label(view_.name);
	std::vector<NameBlock::Part> parts;
	parts.reserve(view_.line2_parts.size());
	for (const ledger::TextRef &ref : view_.line2_parts) {
		NameBlock::Part part;
		part.kind = rows::part_kind_of(ref.id);
		part.full = ui::resolve(ref);
		// ユーザー名は、語を落として値だけにした短い形を持つ。値は Discord から来たテキストのまま (翻訳しない)。
		if (part.kind == rows::PartKind::Username && !ref.args.empty())
			part.short_text = QString::fromStdString(ref.args.front());
		if (!ref.args.empty())
			part.value = QString::fromStdString(ref.args.front());
		parts.push_back(part);
	}
	NameBlock &names = check_->names();
	names.set_name(name);
	names.set_tags(view_.is_main, view_.auto_add);
	names.set_line2(parts);
	if (check_ != nullptr) {
		const QSignalBlocker block(check_);
		check_->setChecked(view_.checked);
		check_->set_replaces(view_.check_replaces);
		set_check_enabled((view_.check_enabled || view_.check_replaces) && !departed_);
		update_avatar_fallback();
		check_->updateGeometry();
		check_->names().place(check_->name_left(), check_->width(), line1_height());
	}
	update_tail();
	relayout_heights();
}

void PersonRowWidget::update_tail()
{
	QString status;
	// 状態のテキストのキー (短い形を引く元)。載せられなかったことを言うテキストは短い形を持たないので、空のままにする。
	std::string status_key;
	std::optional<ui::RowMark> mark;
	if (view_.status) {
		status = ui::resolve(view_.status->text);
		status_key = view_.status->text.id;
		mark = mark_for(view_.status->state);
	} else if (view_.quality) {
		status = ui::resolve(*view_.quality);
		status_key = view_.quality->id;
		mark = ui::RowMark::Quality;
	} else if (view_.refused_note) {
		// 押した直後に空いている枠がなくなり、載せられなかった。行の高さを変えずに伝えるため、状態のテキストの場所に
		// 表示する。
		status = ui::resolve(*view_.refused_note);
	}
	if (mark) {
		const ui::RowMark m = *mark;
		const QColor color = env_.theme.text;
		const int px = env_.metrics.icon_px;
		status_mark_->set_painter(px, [m, px, color](qreal dpr) { return ui::row_mark_pixmap(m, px, color, dpr); });
		status_mark_->show();
	} else {
		status_mark_->set_painter(env_.metrics.icon_px, nullptr);
		status_mark_->hide();
	}
	// 状態は、幅にかかわらず短い形 (「映像待ち」「映像途切れ」など) を表示する。狭い所で全文を省くと、日本語は
	// 「映像が届いて...」のように逆の意味の先頭が残る。全文は、ツールチップの 1 行目とアクセシブル名に入る (refresh_accessibility)。
	// 短い形が全文と同じテキストになる言語 (英語の一部) と、短い形を持たない状態は、全文を表示する。
	// 幅の分け方 (fit_tail の split_name_status) は、表示するテキスト (短い形) の幅で数える。
	const char *short_key = status_key.empty() ? nullptr : rows::status_short_key_for(status_key);
	const QString short_form = short_key != nullptr ? ui::resolve_key(short_key) : QString();
	status_text_->set_full_text(!short_form.isEmpty() ? short_form : status);
	status_shown_short_ = !short_form.isEmpty() && short_form != status;
	status_text_->setVisible(!status.isEmpty() && rules_.status_text);

	const QString resolution = view_.resolution ? ui::resolve(*view_.resolution) : QString();
	if (resolution_->text() != resolution)
		resolution_->setText(resolution);
	const bool res_on = !resolution.isEmpty() && rules_.resolution && !resolution_hidden_by_fit_;
	if (resolution_->isHidden() == res_on)
		resolution_->setVisible(res_on);

	update_share_mark();
	share_->setVisible(view_.sharing || view_.position != ledger::dockview::SlotPosition::None);
	update_audio_buttons();
	// Vesktop のアカウントの行には、[...] も表示しない (名前と薄い色のテキストだけにする)。ただし、自分の行の
	// [自動で載せる] だけのメニューは置く。
	dots_->setVisible(shows_menu(view_));
	dots_->setEnabled(!departed_ && !view_.off);
	fit_tail();
	refresh_accessibility();
}

bool PersonRowWidget::audio_buttons_shown() const
{
	// 動作しないコンポーネントを置かない: 合成のソースがない間と、Vesktop のアカウントの行には置かない。
	return view_.audio.applies && view_.checkable;
}

void PersonRowWidget::set_check_enabled(bool enabled)
{
	// Qt は押せなくなったコンポーネントの焦点を Tab の順の次へ移す。次はミュートのボタンなので、続けて押した Space が配信の音を
	// 切り替えてしまう。同じ人の [...] なら、Space はメニューを開くだけである。
	if (!enabled && check_->hasFocus() && dots_->isEnabled() && !dots_->isHidden())
		dots_->setFocus(Qt::OtherFocusReason);
	check_->setEnabled(enabled);
}

void PersonRowWidget::update_audio_buttons()
{
	const bool on = audio_buttons_shown();
	voice_mute_->setVisible(on);
	screen_mute_->setVisible(on && view_.speaker != ledger::dockview::SpeakerState::Hidden);
	voice_mute_->set_muted(view_.audio.voice_muted);
	screen_mute_->set_muted(view_.audio.screen_muted);
	voice_mute_->setEnabled(!departed_ && !view_.off && view_.mic != ledger::dockview::MicState::NoVoice);
	screen_mute_->setEnabled(!departed_ && !view_.off);
}

void PersonRowWidget::update_share_mark()
{
	ui::SlotMark mark = ui::SlotMark::None;
	switch (view_.position) {
	case ledger::dockview::SlotPosition::None:
		break;
	case ledger::dockview::SlotPosition::TopLeft:
		mark = ui::SlotMark::TopLeft;
		break;
	case ledger::dockview::SlotPosition::TopRight:
		mark = ui::SlotMark::TopRight;
		break;
	case ledger::dockview::SlotPosition::BottomLeft:
		mark = ui::SlotMark::BottomLeft;
		break;
	case ledger::dockview::SlotPosition::BottomRight:
		mark = ui::SlotMark::BottomRight;
		break;
	case ledger::dockview::SlotPosition::Full:
		mark = ui::SlotMark::Full;
		break;
	}
	QColor color = env_.theme.text;
	if (view_.off || departed_)
		color.setAlphaF(env_.disabled_alpha);
	const int px = env_.metrics.icon_px;
	share_->set_painter(px, [mark, px, color](qreal dpr) { return ui::slot_mark_pixmap(mark, px, color, dpr); });
}

void PersonRowWidget::set_speaking(bool speaking)
{
	voice_mute_->set_speaking(speaking);
}

void PersonRowWidget::set_avatar(const QPixmap &image)
{
	avatar_image_ = image;
	update_avatar_fallback();
}

void PersonRowWidget::update_avatar_fallback()
{
	if (check_ == nullptr)
		return;
	// 画像がないときの色は user_id から決める。名前を変えても同じ人は同じ色になり、起動のたびに変わらない。
	const QColor color = QColor::fromHsl(static_cast<int>(qHash(QString::fromStdString(user_id_)) % 360U), 110, 110);
	check_->set_avatar(avatar_image_, first_grapheme(ui::resolve_label(view_.name)).toUpper(), color);
}

std::vector<QString> PersonRowWidget::state_lines() const
{
	// テキストは行ごとに分けて並べる。状態のテキストは句点のない ini の値で、句読点の形は言語で違うので、コードで
	// 「。」を付けて 1 つの文につながない。句点を付けると、日本語の「。」と英語の「. 」を、コードが言語ごとに持つことに
	// なる。2 文目のキーに状態のテキストごと入れると、同じテキストを 2 か所に書き、片方だけが直ることになる。
	std::vector<QString> lines;
	if (view_.status) {
		lines.push_back(ui::resolve(view_.status->text));
		// 2 文目は、View が選んだ RowStatus::tip を先に使う (枠に載せていない録画の対象の人には、枠ではなく録画の文に
		// なる。選ぶのは ledger/rules_dock.cpp の row_status_of だけ)。空のときだけ、状態のテキストのキーから引く。
		if (!view_.status->tip.id.empty())
			lines.push_back(ui::resolve(view_.status->tip));
		else if (const char *key = slot_tip_key_for(view_.status->text.id))
			lines.push_back(ui::resolve_key(key));
	} else if (view_.quality) {
		lines.push_back(ui::resolve(*view_.quality));
		if (const char *key = quality_tip_key_for(view_.quality->id))
			lines.push_back(ui::resolve_key(key));
	}
	return lines;
}

void PersonRowWidget::refresh_accessibility()
{
	const QString name = ui::resolve_label(view_.name);
	const NameBlock &names = check_->names();
	const std::vector<QString> line2 = names.line2_full();

	// 行の状態のテキストと、それに続ける 1 文 (状態のアイコンのツールチップの行)。下の状態のアイコンと、行のチェックの
	// accessibleDescription が同じものを使う。
	const std::vector<QString> state = state_lines();

	std::vector<QString> desc;
	std::vector<QString> tip;
	if (names.shown_name() != names.full_name())
		tip.push_back(name);
	if (names.line2_shortened())
		tip.insert(tip.end(), line2.begin(), line2.end());
	if (check_ != nullptr) {
		// アクセシブル名は見えている人の名前、accessibleDescription は「枠に載せる」+ 2 行目 + 状態の 2 文 + 押せない理由。
		desc.push_back(ui::resolve_key(keys::kDockRowCheckA11y));
		desc.insert(desc.end(), line2.begin(), line2.end());
		// 行に状態があるときは、状態のアイコンのツールチップと同じ行を説明に追加する。Tab で移動する人はチェックに
		// 止まり、状態のアイコンとテキスト (QLabel) には止まらないので、ここにないと、状態と「枠に何が映っているか」を
		// スクリーンリーダーで聞けない。幅のせいで状態のテキストのラベルが隠れていても追加する。隠れているときに追加
		// しないと、同じ人の同じ状態が、ドックの幅によって聞こえたり聞こえなかったりする。チェックのツールチップには
		// 追加しない (状態のアイコンとテキストが、自分のツールチップで同じ 2 文を表示する)。
		desc.insert(desc.end(), state.begin(), state.end());
		tip.push_back(ui::resolve_key(keys::kDockRowCheckTip));
		QString reason;
		if ((!check_->isEnabled() || view_.check_replaces) && view_.check_action.disabled_reason) {
			reason = ui::resolve(*view_.check_action.disabled_reason);
			desc.push_back(reason);
			tip.push_back(reason);
		}
		// 押した直後に載せられなかったことを言う 1 行 (refused_note「空いている枠がありません。」) は、押せない理由の
		// 文がそのテキストを含むとき (空いている枠が 0 の間は必ず含む) は追加しない。同じ注意を、同じ説明の中で 2 回
		// 言わない。行に見えているテキスト (状態のテキストの場所の短い形) は、押した所に表示する結果なので残す。
		// join_lines で「前の行に含まれる行」を全部落とすと、名前とユーザー名のように、別の事実のテキストがたまたま
		// 含まれる行まで落ちる。
		if (view_.refused_note) {
			const QString note = ui::resolve(*view_.refused_note);
			if (reason.isEmpty() || !reason.contains(note)) {
				desc.push_back(note);
				tip.push_back(note);
			}
		}
		if (check_->accessibleName() != name)
			check_->setAccessibleName(name);
		const QString d = join_lines(desc);
		if (check_->accessibleDescription() != d)
			check_->setAccessibleDescription(d);
		const QString t = join_lines(tip);
		if (check_->toolTip() != t)
			check_->setToolTip(t);
	}

	// 行の状態のアイコン。状態のテキストをアクセシブル名にし、「枠に何が映っているか」の 1 文、解像度、状態があるときの
	// 画質を accessibleDescription にする。ツールチップは状態のテキスト + 1 文。行の形は state_lines のコメントにある。
	if (!state.empty()) {
		// 名前は状態のテキスト、説明は状態のテキストの後の行 (1 文か、画質の状態の理由の文) と解像度、ツールチップは
		// 状態の行の全部にする。画質の状態の理由の文 (row_quality.tip) も説明に入れ、アイコンのツールチップの 2 行目を
		// スクリーンリーダーでも聞けるようにする。
		std::vector<QString> mark_desc(state.begin() + 1, state.end());
		// 状態と、画質を下げていることが同時にあるとき、画質は accessibleDescription にだけ入れる。ツールチップは状態の
		// テキスト + 1 文の 2 行までにする (状態のある行の枠は、下げた映像ではなく、待っている、止まっている、空の
		// どれかを映している)。ツールチップにも画質を追加すると 4 行になる。
		if (view_.status && view_.quality)
			mark_desc.push_back(ui::resolve(*view_.quality));
		if (view_.resolution)
			mark_desc.push_back(ui::resolve(*view_.resolution));
		status_mark_->setAccessibleName(state.front());
		status_mark_->setAccessibleDescription(join_lines(mark_desc));
		status_mark_->setToolTip(join_lines(state));
		// 状態のテキストのラベル。短い形を表示しているときは、全文を 1 行目にしたアイコンと同じ行をツールチップにし、全文を
		// アクセシブル名にする。全文を表示しているときは、テキストの後に続ける行があるときだけ、アイコンと同じ行をツール
		// チップにする。続ける行がない状態 (slot_tip_key_for が nullptr を返すテキスト) では、見えているテキストの
		// 繰り返しになるので、ラベルのツールチップは付けない (省いたときは、ラベルが全文をツールチップに表示する)。
		status_text_->set_base_tool_tip(state.size() >= 2 || status_shown_short_ ? join_lines(state) : QString());
		status_text_->set_base_accessible_name(status_shown_short_ ? state.front() : QString());
	} else {
		status_text_->set_base_tool_tip(QString());
		status_text_->set_base_accessible_name(QString());
	}

	share_->setAccessibleName(ui::resolve_key(keys::kDockRowSharingA11y));
	{
		const char *tip_key = keys::kDockRowSharingTip;
		switch (view_.position) {
		case ledger::dockview::SlotPosition::None:
			break;
		case ledger::dockview::SlotPosition::TopLeft:
			tip_key = keys::kDockSlotTopLeftTip;
			break;
		case ledger::dockview::SlotPosition::TopRight:
			tip_key = keys::kDockSlotTopRightTip;
			break;
		case ledger::dockview::SlotPosition::BottomLeft:
			tip_key = keys::kDockSlotBottomLeftTip;
			break;
		case ledger::dockview::SlotPosition::BottomRight:
			tip_key = keys::kDockSlotBottomRightTip;
			break;
		case ledger::dockview::SlotPosition::Full:
			tip_key = keys::kDockSlotFullTip;
			break;
		}
		share_->setToolTip(ui::resolve_key(tip_key));
	}

	// ミュートのボタン。アクセシブル名は「声をミュート (alice)」で、押した状態がミュート中 (OBS の音声ミキサーの
	// ミュートのボタンと同じ形)。ツールチップは今押せる操作 (ミュートしていないときは「声をミュート」、ミュート中は
	// 「声のミュートを解除」)。メインアカウントの声のボタンと、画面共有の音のミュート中は、2 行目で補足する。
	{
		ledger::TextRef voice_name;
		voice_name.id = keys::kDockRowVoiceMuteA11y;
		voice_name.args.push_back(name.toStdString());
		ledger::TextRef screen_name;
		screen_name.id = keys::kDockRowScreenMuteA11y;
		screen_name.args.push_back(name.toStdString());
		voice_mute_->setAccessibleName(ui::resolve(voice_name));
		screen_mute_->setAccessibleName(ui::resolve(screen_name));

		const QString voice_action = ui::resolve_key(view_.audio.voice_muted ? keys::kDockRowVoiceMuteOffA11y
										   : keys::kDockRowVoiceMuteOnA11y);
		const QString voice_note =
			view_.audio.is_main_account ? ui::resolve_key(keys::kDockRowMainVoiceMuteTip) : QString();
		const QString screen_action = ui::resolve_key(view_.audio.screen_muted ? keys::kDockRowScreenMuteOffA11y
										       : keys::kDockRowScreenMuteOnA11y);
		const QString screen_note =
			view_.audio.screen_muted ? ui::resolve_key(keys::kDockRowScreenMuteStartTip) : QString();
		voice_mute_->setToolTip(view_.mic == ledger::dockview::MicState::NoVoice
						? ui::resolve_key(keys::kDockRowNoVoiceTip)
						: join_lines({voice_action, voice_note}));
		screen_mute_->setToolTip(join_lines({screen_action, screen_note}));
		voice_mute_->setAccessibleDescription(voice_note);
		screen_mute_->setAccessibleDescription(screen_note);
	}

	// 行の [...]。アクセシブル名は「その他の操作 (alice)」、ツールチップは「その他の操作」にする。見えている名前が
	// 同じ人がほかにいて、ユーザー名が分かる行は「その他の操作 (alice、ユーザー名: alice_1)」にし、[...] の名前を
	// 参加者ごとに違える (ユーザー名は Discord の中で 1 人に 1 つ)。ユーザー名は View の PersonRow::username から読む
	// (2 行目に表示するかとは別に ledger が入れる)。2 行目のユーザー名の部分から読むと、2 行目にユーザー名が表示
	// されるのは Discord での名前も同じときだけなので、見えている名前だけが同じ 2 人の [...] の名前が同じになる。
	ledger::TextRef more;
	const std::string username =
		view_.same_visible_name_exists && view_.username.has_value() ? *view_.username : std::string();
	more.id = username.empty() ? keys::kDockMoreActionsForA11y : keys::kDockMoreActionsForUsernameA11y;
	more.args.push_back(name.toStdString());
	if (!username.empty())
		more.args.push_back(username);
	dots_->setAccessibleName(ui::resolve(more));
	dots_->setToolTip(ui::resolve_key(keys::kDockMoreActionsA11y));
}

int PersonRowWidget::line1_height() const
{
	int h = 0;
	const NameBlock &names = check_->names();
	h = std::max(h, names.name_label()->fontMetrics().height());
	h = std::max(h, env_.metrics.icon_px);
	h = std::max(h, dots_->sizeHint().height());
	h = std::max(h, voice_mute_->sizeHint().height());
	h = std::max(h, rows::kRowAvatarPx);
	h = std::max(h, status_text_->fontMetrics().height());
	if (check_ != nullptr) {
		QStyleOptionButton option;
		option.initFrom(check_);
		h = std::max(h, check_->style()->pixelMetric(QStyle::PM_IndicatorHeight, &option, check_));
	}
	return h;
}

void PersonRowWidget::relayout_heights()
{
	const int line1 = line1_height();
	if (check_ != nullptr)
		check_->set_line1_height(line1);
	if (tail_->minimumHeight() != line1 || tail_->maximumHeight() != line1)
		tail_->setFixedHeight(line1);
}

void PersonRowWidget::set_departed(bool departed)
{
	if (departed_ == departed)
		return;
	departed_ = departed;
	if (check_ != nullptr)
		check_->setEnabled((view_.check_enabled || view_.check_replaces) && !departed_);
	dots_->setEnabled(!departed_ && !view_.off);
	update_audio_buttons();
	update_share_mark();
	refresh_accessibility();
}

void PersonRowWidget::set_width_rules(const rows::WidthRules &rules, bool resolution_allowed)
{
	rules_ = rules;
	if (check_ != nullptr)
		check_->set_avatar_shown(rules_.avatar);
	resolution_hidden_by_fit_ = !resolution_allowed;
	const bool status_on = !status_text_->full_text().isEmpty() && rules_.status_text;
	if (status_text_->isHidden() == status_on)
		status_text_->setVisible(status_on);
	const bool res_on = !resolution_->text().isEmpty() && rules_.resolution && resolution_allowed;
	if (resolution_->isHidden() == res_on)
		resolution_->setVisible(res_on);
	fit_tail();
}

bool PersonRowWidget::resolution_shown() const
{
	return !resolution_->isHidden();
}

bool PersonRowWidget::resolution_would_fit(const rows::WidthRules &rules) const
{
	if (resolution_->text().isEmpty())
		return true;
	// 解像度は、名前の全文 (下限より短ければ下限) と行の状態のテキストの全文を削らずに 1 行に入るときだけ表示する。
	// 解像度はひと目で見る情報ではないので、狭いときに最初に消える。ウィジェットを表示したり隠したりして測らず、
	// sizeHint の幅の足し算で決める (表示と非表示で幅が変わり、判定が行き来しないように)。名前を下限まで縮めて
	// 解像度を表示すると、幅を広げたときに名前が狭くなる。
	const NameBlock &names = check_->names();
	const bool status_on = !status_text_->full_text().isEmpty() && rules.status_text;
	const int status_natural = status_on ? status_text_->sizeHint().width() : 0;
	const int name_natural = std::max(names.name_label()->sizeHint().width(), env_.metrics.name_floor_px);
	const int need = name_left() + name_natural + layout_->spacing() + tail_width(status_on, status_natural, true);
	return need <= width();
}

int PersonRowWidget::name_left() const
{
	return check_ != nullptr ? check_->name_left() : env_.text_indent_px;
}

int PersonRowWidget::share_gap_extra() const
{
	return std::max(0, env_.metrics.large_px - env_.metrics.small_px);
}

int PersonRowWidget::dots_gap_extra() const
{
	const int gap = rows::dots_gap_px(env_.metrics.large_px, env_.metrics.focus_ring_px, env_.metrics.focus_ring_gap_px);
	return std::max(0, gap - env_.metrics.small_px);
}

int PersonRowWidget::tail_width(bool status_text, int status_text_px, bool resolution) const
{
	// tail_layout_ と同じ数え方をする。表示しているウィジェットの幅と、その間の spacing (小さい余白) を足し、テキストの
	// 後の画面のアイコンの前は大きい余白にする (小さい余白に足す)。状態のアイコンは View に状態か画質があるときだけ、画面の
	// アイコンとミュートのボタンはどの行でも、[...] はメニューを持つ行 (shows_menu) だけ幅を取る。
	const int spacing = tail_layout_->spacing();
	int total = 0;
	int count = 0;
	auto add = [&](int w) {
		total += w;
		++count;
	};
	// update_tail が状態のアイコンを表示する条件と同じ (変えるときは両方を変える)
	if ((view_.status && mark_for(view_.status->state)) || view_.quality)
		add(env_.metrics.icon_px);
	if (status_text)
		add(status_text_px);
	if (resolution)
		add(resolution_->sizeHint().width());
	add(env_.metrics.icon_px);
	// ミュートのボタンは、隠している間も場所を空けるので、持たない間も数える。
	add(voice_mute_->sizeHint().width());
	add(screen_mute_->sizeHint().width());
	if (shows_menu(view_))
		add(dots_->sizeHint().width());
	if (count > 1)
		total += spacing * (count - 1);
	if (status_text || resolution)
		total += share_gap_extra();
	if (shows_menu(view_)) // fit_tail が共有のアイコンと [...] の間を広げる条件と同じ
		total += dots_gap_extra();
	return total;
}

void PersonRowWidget::fit_tail()
{
	const bool status_on = !status_text_->isHidden();
	const bool res_on = !resolution_->isHidden();
	// テキストと共有の列の間は大きい余白にする。アイコンどうし (状態のアイコンと共有のアイコン) の間は小さい余白の
	// ままにする。アイコンの画像は自分の矩形の中に余白を持ち、テキストのように端まで描かないためである。状態の
	// テキストを表示する境目より狭い幅ではテキストを表示しないので、最小幅も変わらない。
	const int extra = status_on || res_on ? share_gap_extra() : 0;
	if (share_gap_->sizeHint().width() != extra) {
		share_gap_->changeSize(extra, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
		tail_layout_->invalidate();
	}
	const int dots_extra = shows_menu(view_) ? dots_gap_extra() : 0;
	if (dots_gap_->sizeHint().width() != dots_extra) {
		dots_gap_->changeSize(dots_extra, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
		tail_layout_->invalidate();
	}
	// 名前と状態のテキストの幅 (名前の全文を先に残し、状態のテキストは下限まで先に縮む)。行の幅だけから決める。
	// チェックのウィジェットの幅はこの計算の結果なので使わない。使うと、計算し直すたびに結果が行き来する。
	int cap = QWIDGETSIZE_MAX;
	if (status_on && width() > 0) {
		const NameBlock &names = check_->names();
		const int status_natural = status_text_->sizeHint().width();
		const int name_natural = names.name_label()->sizeHint().width();
		const int text_px = width() - name_left() - layout_->spacing() - tail_width(true, 0, res_on);
		const int keep = static_cast<int>(env_.status_keep_em * env_.metrics.em_px + 0.5);
		const rows::NameStatusSplit split =
			rows::split_name_status(text_px, name_natural, status_natural,
						check_ != nullptr && check_->avatar_shown()
							? rows::name_floor_with_avatar(env_.metrics.name_floor_px)
							: env_.metrics.name_floor_px,
						keep);
		if (split.status_px < status_natural)
			cap = split.status_px;
	}
	if (status_text_->maximumWidth() != cap)
		status_text_->setMaximumWidth(cap);
}

void PersonRowWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	fit_tail();
}

void PersonRowWidget::activate_layouts()
{
	tail_layout_->activate();
	layout_->activate();
}

QString PersonRowWidget::full_name() const
{
	return check_->names().full_name();
}

int PersonRowWidget::name_width() const
{
	return check_->names().name_width();
}

QFontMetrics PersonRowWidget::name_font_metrics() const
{
	return check_->names().name_label()->fontMetrics();
}

QString PersonRowWidget::shown_name() const
{
	return check_->names().shown_name();
}

void PersonRowWidget::set_name_mode(Qt::TextElideMode mode)
{
	name_mode_ = mode;
	if (check_ != nullptr)
		check_->names().set_name_mode(mode);
	refresh_accessibility();
}

bool PersonRowWidget::press_blocked_now() const
{
	return rows::press_blocked(changed_at_ms_, now_ms(), QGuiApplication::styleHints()->mouseDoubleClickInterval());
}

QWidget *PersonRowWidget::focus_target() const
{
	if (check_ != nullptr)
		return check_;
	return dots_->isVisibleTo(const_cast<PersonRowWidget *>(this)) ? dots_ : nullptr;
}

bool PersonRowWidget::eventFilter(QObject *watched, QEvent *event)
{
	// 押せないチェック (枠がすべて使用中で、載せていない人) の上の右クリック。Qt は、押せないウィジェットへの
	// コンテキストメニューのイベントを、そのウィジェットにも親にも渡さないことがあるので、ウィジェットに届く前に
	// 受け取って行のメニューを開く。行のどこを右クリックしても行のメニューを開くようにし、押せないチェックの上も
	// 例外にしない。押せるチェックは、Qt の通常の処理 (親であるこのウィジェットの contextMenuEvent) で開くので触らない。
	if (check_ != nullptr && watched == check_ && event->type() == QEvent::ContextMenu && !check_->isEnabled()) {
		contextMenuEvent(static_cast<QContextMenuEvent *>(event));
		return event->isAccepted();
	}
	return QWidget::eventFilter(watched, event);
}

void PersonRowWidget::contextMenuEvent(QContextMenuEvent *event)
{
	// 右クリック、Menu キー、Shift+F10。メニューを持たない行 (Vesktop のアカウントの行) と、抜けた人の行では開かない。
	if (!shows_menu(view_) || departed_ || view_.off || !on_menu_requested) {
		event->ignore();
		return;
	}
	event->accept();
	const bool keyboard = event->reason() == QContextMenuEvent::Keyboard;
	on_menu_requested(keyboard ? static_cast<QWidget *>(dots_) : static_cast<QWidget *>(this), event->globalPos(),
			  keyboard);
}

} // namespace cordmux::dock

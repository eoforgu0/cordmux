// SPDX-License-Identifier: Apache-2.0

// composite_scene.h の実装。

#include "composite_scene.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>

#include <algorithm>
#include <cmath>

#include "sources/composite_draw_cpu.h"
#include "sources/composite_draw_plan.h"

namespace cordmux::decor_preview {

namespace sources = cordmux::sources;

const std::vector<ScenePerson> &scene_people()
{
	static const std::vector<ScenePerson> people = {
		{"mika", "Mika", true, 200},
		{"noa", "Noa Kimura", true, 20},
		{"taro", "\xE3\x81\x9F\xE3\x82\x8D\xE3\x81\x86", false, 0}, // たろう。アバターの画像がない人
		{"alexander", "Alexander Featherstonehaugh-Montgomery", true, 120},
		{"sakura", "\xE3\x81\x95\xE3\x81\x8F\xE3\x82\x89", true, 320}, // さくら
		{"ken", "Ken", false, 0},
		// しろくまとペンギンのこおりのおしろ。省略される長い日本語の名前
		{"long_ja",
		 "\xE3\x81\x97\xE3\x82\x8D\xE3\x81\x8F\xE3\x81\xBE\xE3\x81\xA8\xE3\x83\x9A\xE3\x83\xB3\xE3\x82\xAE\xE3\x83\xB3"
		 "\xE3\x81\xAE\xE3\x81\x93\xE3\x81\x8A\xE3\x82\x8A\xE3\x81\xAE\xE3\x81\x8A\xE3\x81\x97\xE3\x82\x8D",
		 true, 40},
		// ここから後ろは、画面共有していない参加者を 8 人まで出すための架空の人。短い名前と、アバターの画像がない
		// 日本語の名前、長い英語の名前、アバターの画像がない長い日本語の名前を混ぜてある。
		{"rin", "Rin", true, 270},
		{"sota", "\xE3\x81\x9D\xE3\x81\x86\xE3\x81\x9F", false, 0}, // そうた
		{"olivia", "Olivia Montgomery-Hargreaves", true, 160},
		{"yuzu", "\xE3\x82\x86\xE3\x81\x9A", true, 70}, // ゆず
		// ふわふわのくもにのっておしろへむかうひと
		{"kumo",
		 "\xE3\x81\xB5\xE3\x82\x8F\xE3\x81\xB5\xE3\x82\x8F\xE3\x81\xAE\xE3\x81\x8F\xE3\x82\x82\xE3\x81\xAB\xE3\x81\xAE"
		 "\xE3\x81\xA3\xE3\x81\xA6\xE3\x81\x8A\xE3\x81\x97\xE3\x82\x8D\xE3\x81\xB8\xE3\x82\x80\xE3\x81\x8B\xE3\x81\x86"
		 "\xE3\x81\xB2\xE3\x81\xA8",
		 false, 0},
	};
	return people;
}

// 明るい所、暗い所、細かい模様が混ざるように 4 種類を使い分ける。名前ラベルは各枠の隅に載るので、隅の明るさが
// ばらけるようにしてある。
QImage make_scene_background(int kind, int w, int h)
{
	QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
	QPainter p(&image);
	p.setPen(Qt::NoPen);
	switch (kind % kBackgroundKindCount) {
	case kDarkGame: {
		QLinearGradient g(0, 0, 0, h);
		g.setColorAt(0.0, QColor(18, 22, 48));
		g.setColorAt(1.0, QColor(58, 28, 70));
		p.fillRect(QRect(0, 0, w, h), g);
		p.setBrush(QColor(230, 200, 60));
		p.drawRect(QRectF(w * 0.05, h * 0.06, w * 0.30, h * 0.04));
		p.setBrush(QColor(210, 60, 60));
		p.drawRect(QRectF(w * 0.05, h * 0.12, w * 0.18, h * 0.03));
		p.setBrush(QColor(90, 200, 255));
		p.drawEllipse(QRectF(w * 0.62, h * 0.30, w * 0.22, w * 0.22));
		break;
	}
	case kBrightSky: {
		QLinearGradient g(0, 0, 0, h);
		g.setColorAt(0.0, QColor(110, 170, 235));
		g.setColorAt(1.0, QColor(225, 240, 252));
		p.fillRect(QRect(0, 0, w, h), g);
		p.setBrush(QColor(255, 255, 255));
		p.drawEllipse(QRectF(w * 0.05, h * 0.62, w * 0.40, h * 0.30));
		p.drawEllipse(QRectF(w * 0.30, h * 0.70, w * 0.45, h * 0.28));
		break;
	}
	case kStripes: {
		const int stripe = std::max(8, w / 48);
		for (int x = 0, i = 0; x < w; x += stripe, ++i)
			p.fillRect(QRect(x, 0, stripe, h), (i % 2) ? QColor(240, 140, 40) : QColor(30, 120, 130));
		break;
	}
	default: {
		QLinearGradient g(0, 0, 0, h);
		g.setColorAt(0.0, QColor(70, 140, 70));
		g.setColorAt(0.7, QColor(30, 90, 40));
		g.setColorAt(0.7001, QColor(245, 247, 250));
		g.setColorAt(1.0, QColor(215, 225, 235));
		p.fillRect(QRect(0, 0, w, h), g);
		break;
	}
	}
	return image;
}

QImage make_scene_avatar(int hue)
{
	QImage image(128, 128, QImage::Format_ARGB32_Premultiplied);
	QPainter p(&image);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	p.fillRect(QRect(0, 0, 128, 128), QColor::fromHsv(hue, 150, 215));
	p.setBrush(QColor::fromHsv(hue, 60, 250));
	p.drawEllipse(QRectF(40, 22, 48, 48));
	p.drawEllipse(QRectF(14, 80, 100, 90));
	return image;
}

ScenePlan plan_scene(const SceneRequest &request)
{
	ScenePlan plan;
	plan.canvas_w = request.canvas_w;
	plan.canvas_h = request.canvas_h;
	plan.look = sources::composite_label_look_from_settings(request.settings);
	plan.params = sources::decor_label_params_from_settings(request.settings);
	plan.name_px = sources::composite_name_px(request.canvas_h, plan.params.em_ratio);
	plan.margin_px = sources::composite_margin_px(request.canvas_h, plan.look.margin_ratio);
	plan.parts = sources::composite_speaking_parts(plan.look);
	plan.paint_look = plan.look;
	if (!plan.parts.label_glow) {
		plan.paint_look.glow_avatar = false;
		plan.paint_look.glow_name = false;
	}
	plan.dim_pct = plan.params.dim_pct;
	plan.roster_params = sources::composite_roster_params_from_settings(request.settings);
	plan.roster_cap_w = sources::composite_roster_cap_width(request.canvas_w, plan.margin_px);
	plan.roster_paint_look = plan.paint_look;
	plan.roster_paint_look.avatar_side = plan.roster_params.avatar_side;

	const std::vector<ScenePerson> &people = scene_people();
	const std::size_t seat_count = request.seats.size();
	if (seat_count == 0 || seat_count > static_cast<std::size_t>(sources::kCompositeCellCount))
		return plan;
	for (const SceneSeat &seat : request.seats) {
		if (seat.person < 0 || seat.person >= static_cast<int>(people.size()))
			return plan;
	}

	// 製品の配置。自分の枠を出していて、画面共有の枠は表示名の順に並ぶ。user_id の代わりに枠の番号を渡す (同じ
	// 名前のときの並びの決め手にだけ使われる)。
	sources::CompositeLayoutRequest layout_request;
	layout_request.canvas_w = request.canvas_w;
	layout_request.canvas_h = request.canvas_h;
	layout_request.self_shown = true;
	for (std::size_t slot = 1; slot < seat_count; ++slot)
		layout_request.shares.push_back({static_cast<int>(slot),
						 people[static_cast<std::size_t>(request.seats[slot].person)].name,
						 std::to_string(slot)});
	plan.layout = sources::plan_composite_layout(layout_request);
	plan.hide = sources::composite_hides_label(plan.look, request.canvas_w, request.canvas_h, plan.layout.tiles) ||
		    !(plan.name_px > 0.0);

	// 名前ラベルを描くタイル。製品と同じ関数で決める (このツールの席は、どれも映像を描いている)。名前ラベルを描かないタイルの
	// 席の人 ([1人のときは非表示] で隠す全面の 1 枚の人と、名前ラベルを枠に描かない設定のときの全員) は、製品と同じく画面共有
	// していない参加者の並びに描く
	// (sources/composite_label_plan.h の composite_roster_shows)。
	const std::vector<bool> tile_labeled = sources::composite_tiles_labeled(
		plan.look, request.canvas_w, request.canvas_h, plan.layout.tiles,
		std::vector<bool>(plan.layout.tiles.size(), true));
	plan.tile_labeled = tile_labeled;

	// 画面共有していない参加者。並べる順は画面共有の枠と同じ比べ方 (user_id の代わりに人の id を渡す)。出さない形
	// (roster_show が偽、名前の大きさが 0) では空にする。member は SceneRequest::roster の添字 (席の人は -1)、track は
	// 話している明るさを読む行 (席の人はその席、並びの人は kCompositeCellCount + member)。
	struct RosterInput {
		int member = -1;
		int person = 0;
		bool tracked = true;
		int track = 0;
	};
	std::vector<RosterInput> roster_input;
	std::vector<std::string> labeled_names; // 名前ラベル付きで描くタイルの人の名前
	if (plan.roster_params.show && plan.name_px > 0.0) {
		for (std::size_t t = 0; t < plan.layout.tiles.size(); ++t) {
			const std::size_t seat = plan.layout.tiles[t].self ? 0u : static_cast<std::size_t>(plan.layout.tiles[t].slot);
			if (!tile_labeled[t])
				roster_input.push_back({-1, request.seats[seat].person, true, static_cast<int>(seat)});
		}
		for (std::size_t i = 0; i < request.roster.size() && roster_input.size() < kMaxRosterMembers; ++i) {
			const int person = request.roster[i].person;
			if (person >= 0 && person < static_cast<int>(people.size()))
				roster_input.push_back({static_cast<int>(i), person, request.roster[i].tracked,
							sources::kCompositeCellCount + static_cast<int>(i)});
		}
	}
	for (std::size_t t = 0; t < plan.layout.tiles.size(); ++t) {
		const std::size_t seat = plan.layout.tiles[t].self ? 0u : static_cast<std::size_t>(plan.layout.tiles[t].slot);
		if (tile_labeled[t])
			labeled_names.push_back(people[static_cast<std::size_t>(request.seats[seat].person)].name);
	}
	std::vector<sources::CompositeRosterPerson> roster_keys;
	for (const RosterInput &member : roster_input) {
		const ScenePerson &person = people[static_cast<std::size_t>(member.person)];
		roster_keys.push_back({person.name, person.id});
	}
	const std::vector<std::size_t> roster_order = sources::composite_roster_order(roster_keys);

	// ほかの人と見分けるための名前は、画面共有の人と画面共有していない参加者の全員 (その人自身を除く)。
	std::vector<std::string> roster_names;
	for (const std::size_t o : roster_order)
		roster_names.push_back(roster_keys[o].name);
	std::vector<std::string> seat_names;
	for (const SceneSeat &seat : request.seats)
		seat_names.push_back(people[static_cast<std::size_t>(seat.person)].name);

	for (const sources::CompositeTile &tile : plan.layout.tiles) {
		const std::size_t seat = tile.self ? 0u : static_cast<std::size_t>(tile.slot);
		const int person_index = request.seats[seat].person;
		const ScenePerson &person = people[static_cast<std::size_t>(person_index)];
		sources::LabelContent content;
		content.name = person.name;
		content.corner = sources::decor_corner_of(request.settings.label_corner);
		if (person.has_avatar)
			content.avatar = make_scene_avatar(person.hue);
		for (std::size_t other = 0; other < seat_count; ++other)
			if (other != seat)
				content.other_visible_names.push_back(seat_names[other]);
		for (const std::string &name : roster_names)
			content.other_visible_names.push_back(name);
		plan.tile_seat.push_back(static_cast<int>(seat));
		plan.tile_person.push_back(person_index);
		plan.tile_owner.push_back(std::to_string(person_index));
		// 調整ツールでは、自分の枠の人 (席 0) も、ほかの人と同じく話していなければ暗くする (ツールでは自分の枠も
		// 光らせられるため)。製品の自分の枠の人は、自分の声の経路があるとき (サブアカウントの構成で、合成のソースが
		// あるとき) だけ暗くする (dock/build_observation.h の self_frame_hears_voice)。
		plan.tile_hears_voice.push_back(true);
		plan.tile_content.push_back(content);
	}

	// 並びの見分けに使う名前は、見えている名前の全部 (名前ラベル付きのタイルの人と、並びの人) から本人を除いたもの (製品の
	// composite_roster_others)。
	std::vector<std::string> visible_names = labeled_names;
	visible_names.insert(visible_names.end(), roster_names.begin(), roster_names.end());
	for (std::size_t position = 0; position < roster_order.size(); ++position) {
		const RosterInput &entry = roster_input[roster_order[position]];
		const ScenePerson &person = people[static_cast<std::size_t>(entry.person)];
		sources::LabelContent content;
		content.name = person.name;
		content.corner = sources::DecorCorner::TopLeft;
		if (person.has_avatar)
			content.avatar = make_scene_avatar(person.hue);
		content.other_visible_names = sources::composite_roster_others(visible_names, person.name);
		plan.roster_member.push_back(entry.member);
		plan.roster_track.push_back(entry.track);
		plan.roster_person.push_back(entry.person);
		plan.roster_owner.push_back(std::string("roster:") + person.id);
		const bool last_unheard = request.roster_last_unheard && position + 1 == roster_order.size();
		plan.roster_hears_voice.push_back(entry.tracked && !last_unheard);
		plan.roster_content.push_back(content);
	}
	return plan;
}

sources::CompositeLabel paint_scene_label(const ScenePlan &plan, std::size_t tile)
{
	if (tile >= plan.layout.tiles.size())
		return sources::CompositeLabel();
	const sources::PixelRect &cell = plan.layout.tiles[tile].rect;
	return sources::paint_composite_label(plan.tile_content[tile], plan.paint_look, plan.name_px, plan.margin_px,
					      cell.w, cell.h);
}

QImage paint_scene_frame(const ScenePlan &plan, std::size_t tile)
{
	if (tile >= plan.layout.tiles.size() || !plan.parts.frame_glow)
		return QImage();
	const sources::PixelRect &cell = plan.layout.tiles[tile].rect;
	return sources::paint_frame_glow(cell.w, cell.h, plan.look.glow_argb, plan.look.glow_blur_ratio * plan.name_px,
					 plan.look.glow_spread_ratio * plan.name_px);
}

sources::CompositeLabel paint_scene_roster_label(const ScenePlan &plan, std::size_t r)
{
	if (r >= plan.roster_content.size())
		return sources::CompositeLabel();
	return sources::paint_composite_label(plan.roster_content[r], plan.roster_paint_look, plan.name_px, 0.0,
					      plan.roster_cap_w, plan.canvas_h);
}

SceneRosterLayout layout_scene_roster(const ScenePlan &plan, const std::vector<SceneRosterImages> &roster)
{
	SceneRosterLayout out;
	sources::CompositeRosterRequest request;
	request.canvas_w = plan.canvas_w;
	request.canvas_h = plan.canvas_h;
	request.margin_px = static_cast<int>(std::lround(plan.margin_px));
	request.gap_px = sources::composite_roster_gap_px(plan.roster_params.gap_ratio, plan.name_px);
	request.anchor = plan.roster_params.anchor;
	request.direction = plan.roster_params.direction;
	request.avatar_side = plan.roster_params.avatar_side;
	request.offset_x_px = plan.roster_params.offset_x_px;
	request.offset_y_px = plan.roster_params.offset_y_px;
	const std::size_t count = std::min(plan.roster_content.size(), roster.size());
	for (std::size_t r = 0; r < count; ++r) {
		// 画像がない人は並べない。描き直しを待つ間は、描く人が同じ前の画像を置く。
		const SceneRosterImages &shown = roster[r];
		sources::CompositeRosterBox box;
		if (!shown.label.label.isNull() && (shown.label_current || shown.label_owner == plan.roster_owner[r])) {
			box.w = shown.label.box_w;
			box.h = shown.label.box_h;
		}
		request.boxes.push_back(box);
	}
	const sources::CompositeRosterPlan plan_out = sources::plan_composite_roster(request);
	out.placed = plan_out.placed;
	out.not_placed = plan_out.not_placed;
	return out;
}

void draw_scene(QImage *canvas, const ScenePlan &plan, const std::vector<SceneTileImages> &tiles,
		const std::vector<SceneRosterImages> &roster)
{
	draw_scene_region(canvas, 0, 0, plan, tiles, roster);
}

void draw_scene_region(QImage *target, int origin_x, int origin_y, const ScenePlan &plan,
		       const std::vector<SceneTileImages> &tiles, const std::vector<SceneRosterImages> &roster)
{
	if (!target || target->isNull())
		return;
	sources::CompositeDrawRequest request;
	request.canvas_w = plan.canvas_w;
	request.canvas_h = plan.canvas_h;
	request.hide = plan.hide;
	request.parts = plan.parts;

	const auto image_of = [](const QImage &image, int x, int y) {
		sources::CompositeDrawImage drawn;
		drawn.present = !image.isNull();
		drawn.x = x;
		drawn.y = y;
		drawn.w = image.width();
		drawn.h = image.height();
		return drawn;
	};
	std::vector<sources::CompositeTileImages> images;
	const std::size_t count = std::min(plan.layout.tiles.size(), tiles.size());
	for (std::size_t index = 0; index < count; ++index) {
		const SceneTileImages &shown = tiles[index];
		sources::CompositeDrawTile draw_tile;
		draw_tile.cell = plan.layout.tiles[index].rect;
		draw_tile.has_video = true;
		draw_tile.level = std::clamp(shown.level, 0.0, 1.0);
		// 名前ラベルを描かないタイル (製品の composite_tiles_labeled が偽) には、名前ラベルとその光を置かず、枠の光だけを置く
		// (製品の合成のソースが、そのタイルの名前ラベルの画像を持たないのと同じ)。
		const bool labeled = index < plan.tile_labeled.size() && plan.tile_labeled[index];
		if (labeled) {
			draw_tile.label = image_of(shown.label.label, shown.label.label_x, shown.label.label_y);
			draw_tile.glow = image_of(shown.label.glow, shown.label.glow_x, shown.label.glow_y);
		}
		draw_tile.label_current = shown.label_current;
		draw_tile.label_owner = shown.label_owner;
		draw_tile.owner = plan.tile_owner[index];
		draw_tile.frame = image_of(shown.frame, 0, 0);
		draw_tile.frame_current = shown.frame_current;
		draw_tile.label_dim = shown.dim;
		request.tiles.push_back(draw_tile);

		sources::CompositeTileImages tile_images;
		if (labeled) {
			tile_images.label = shown.label.label;
			tile_images.glow = shown.label.glow;
		}
		tile_images.frame = shown.frame;
		images.push_back(tile_images);
	}
	// 画面共有していない参加者。置いた人の名前ラベルと光を、位置 (名前ラベルの矩形の左上) に合わせてキャンバスの座標に
	// する。画像の位置は、名前ラベルを描いたセルの左上から見た位置なので、矩形の左上 (box_x、box_y) との差だけずらす。
	std::vector<sources::CompositeTileImages> roster_images;
	if (!plan.roster_content.empty() && !roster.empty()) {
		for (const sources::CompositeRosterPlaced &placed : layout_scene_roster(plan, roster).placed) {
			const SceneRosterImages &shown = roster[placed.index];
			const int dx = placed.x - shown.label.box_x;
			const int dy = placed.y - shown.label.box_y;
			sources::CompositeDrawRosterEntry entry;
			entry.label = image_of(shown.label.label, shown.label.label_x + dx, shown.label.label_y + dy);
			entry.glow = image_of(shown.label.glow, shown.label.glow_x + dx, shown.label.glow_y + dy);
			entry.level = std::clamp(shown.level, 0.0, 1.0);
			entry.dim = shown.dim;
			request.roster.push_back(entry);

			sources::CompositeTileImages entry_images;
			entry_images.label = shown.label.label;
			entry_images.glow = shown.label.glow;
			roster_images.push_back(entry_images);
		}
	}
	// 切り抜きを target の範囲に狭めてから、整数のピクセルだけずらす。draw_composite_plan はどのピクセルも、そのピクセルに
	// かかる置く物だけから求めるので、範囲の中の値は変わらない。
	const sources::PixelRect area{origin_x, origin_y, target->width(), target->height()};
	std::vector<sources::CompositeDraw> draws;
	for (sources::CompositeDraw draw : sources::plan_composite_draws(request)) {
		draw.clip = sources::draw_plan_detail::intersect(draw.clip, area);
		if (draw.clip.w <= 0 || draw.clip.h <= 0)
			continue;
		draw.clip.x -= origin_x;
		draw.clip.y -= origin_y;
		draw.image.x -= origin_x;
		draw.image.y -= origin_y;
		draws.push_back(draw);
	}
	sources::draw_composite_plan(target, draws, images, roster_images);
}

} // namespace cordmux::decor_preview

// SPDX-License-Identifier: Apache-2.0

// tuner_renderer.h の実装。

#include "tuner_renderer.h"

#include <algorithm>
#include <utility>

#include "sources/decor_params.h"

namespace cordmux::label_tuner {

namespace sources = cordmux::sources;
namespace decor_preview = cordmux::decor_preview;

namespace {

// 保存先の番号。
constexpr std::size_t kLabelSlot = 0;
constexpr std::size_t kFrameSlot = 1;

} // namespace

TunerRenderer::TunerRenderer(bool threads) : threads_(threads)
{
	const auto paint = [](const PaintInput &input) -> PaintOutput {
		if (input.roster)
			return decor_preview::paint_scene_roster_label(*input.plan, input.tile);
		if (input.frame)
			return decor_preview::paint_scene_frame(*input.plan, input.tile);
		return decor_preview::paint_scene_label(*input.plan, input.tile);
	};
	for (auto &handoff : handoffs_)
		handoff = std::make_unique<Handoff>(2, paint, threads);
	roster_handoff_ = std::make_unique<Handoff>(decor_preview::kMaxRosterMembers, paint, threads);
	plan_ = std::make_shared<const decor_preview::ScenePlan>();
}

// 受け渡しを先に止める (描いている最中の仕事を描き終えるまで待つ)。仕事は場面の計画を自分で持っているので、順は
// どちらでもよい。
TunerRenderer::~TunerRenderer()
{
	for (auto &handoff : handoffs_)
		handoff->stop();
	roster_handoff_->stop();
}

void TunerRenderer::set_request(const decor_preview::SceneRequest &request)
{
	// 描き直す鍵の 2 つの version は、合成のソースと同じ分け方で進める。見た目の値が変わったら look_version、人と隅
	// (と、そこから決まるほかの人の名前) が変わったら version。名前の大きさ、余白、セルの大きさは鍵にそのまま入る。
	const sources::CompositeLabelLook look = sources::composite_label_look_from_settings(request.settings);
	bool seats_changed = request.seats.size() != request_.seats.size() ||
			     request.settings.label_corner != request_.settings.label_corner;
	for (std::size_t i = 0; !seats_changed && i < request.seats.size(); ++i)
		seats_changed = request.seats[i].person != request_.seats[i].person;
	auto next = std::make_shared<const decor_preview::ScenePlan>(decor_preview::plan_scene(request));
	// 画面共有していない参加者の顔ぶれと並びが変わったとき (ほかの人の名前が変わるので、名前の省略の仕方が変わる)。
	const bool roster_changed = next->roster_owner != plan_->roster_owner;
	// 並びの名前ラベルは、アバターの位置だけが画面共有の人の名前ラベルと別の値なので、そちらが変わっても描き直す。
	if (look != plan_->look || next->roster_paint_look != plan_->roster_paint_look)
		++look_version_;
	if (seats_changed || roster_changed)
		++version_;
	request_ = request;
	plan_ = std::move(next);
}

sources::CompositeLabelKey TunerRenderer::roster_key_of(std::size_t r) const
{
	const decor_preview::ScenePlan &plan = *plan_;
	return sources::make_composite_label_key(version_, look_version_, plan.roster_owner[r],
						 sources::PixelRect{0, 0, plan.roster_cap_w, plan.canvas_h}, plan.name_px,
						 0.0);
}

sources::CompositeLabelKey TunerRenderer::label_key_of(std::size_t tile) const
{
	const decor_preview::ScenePlan &plan = *plan_;
	return sources::make_composite_label_key(version_, look_version_, plan.tile_owner[tile],
						 plan.layout.tiles[tile].rect, plan.name_px, plan.margin_px);
}

sources::CompositeFrameGlowKey TunerRenderer::frame_key_of(std::size_t tile) const
{
	const decor_preview::ScenePlan &plan = *plan_;
	return sources::make_composite_frame_glow_key(plan.layout.tiles[tile].rect, plan.look, plan.name_px);
}

bool TunerRenderer::poll()
{
	const decor_preview::ScenePlan &plan = *plan_;
	bool changed = false;
	// 合成のソースと同じく、名前ラベルと光を置かない場面では、画面共有の枠の物を描き直さない。画面共有していない
	// 参加者は、この設定には従わない。
	const std::size_t count = plan.hide ? 0u : std::min(plan.layout.tiles.size(), tiles_.size());
	for (std::size_t index = 0; index < count; ++index) {
		Handoff &handoff = *handoffs_[index];
		TileCache &cache = tiles_[index];

		const sources::CompositeLabelKey label_key = label_key_of(index);
		if (auto done = handoff.take(kLabelSlot, PaintKey(label_key))) {
			cache.label = std::get<sources::CompositeLabel>(std::move(done->output));
			cache.label_key = std::get<sources::CompositeLabelKey>(done->key);
			cache.label_shown = true;
			last_paint_ms_ = static_cast<double>(done->paint_ns) / 1e6;
			changed = true;
		}
		if (!cache.label_shown || cache.label_key != label_key)
			handoff.submit(kLabelSlot, PaintKey(label_key),
				       [&]() { return PaintInput{plan_, index, false}; });

		if (plan.parts.frame_glow) {
			const sources::CompositeFrameGlowKey frame_key = frame_key_of(index);
			if (auto done = handoff.take(kFrameSlot, PaintKey(frame_key))) {
				cache.frame = std::get<QImage>(std::move(done->output));
				cache.frame_key = std::get<sources::CompositeFrameGlowKey>(done->key);
				cache.frame_shown = true;
				changed = true;
			}
			if (!cache.frame_shown || cache.frame_key != frame_key)
				handoff.submit(kFrameSlot, PaintKey(frame_key),
					       [&]() { return PaintInput{plan_, index, true}; });
		} else if (cache.frame_shown) {
			// 枠を光らせない形に替わったら、セルの大きさの画像を持ち続けない (合成のソースと同じ)。
			cache.frame = QImage();
			cache.frame_shown = false;
			changed = true;
		}
	}

	// 画面共有していない参加者。参加者ごとに保存先を持ち、1 つのスレッドで描く。
	const std::size_t roster_count = std::min(plan.roster_owner.size(), roster_.size());
	for (std::size_t r = 0; r < roster_.size(); ++r) {
		RosterCache &cache = roster_[r];
		if (r >= roster_count) {
			// 並びの人数が減ったら、使わない画像を持ち続けない。
			if (cache.shown) {
				cache.label = sources::CompositeLabel();
				cache.shown = false;
				changed = true;
			}
			continue;
		}
		const sources::CompositeLabelKey key = roster_key_of(r);
		if (auto done = roster_handoff_->take(r, PaintKey(key))) {
			cache.label = std::get<sources::CompositeLabel>(std::move(done->output));
			cache.key = std::get<sources::CompositeLabelKey>(done->key);
			cache.shown = true;
			last_paint_ms_ = static_cast<double>(done->paint_ns) / 1e6;
			changed = true;
		}
		if (!cache.shown || cache.key != key)
			roster_handoff_->submit(r, PaintKey(key), [&]() { return PaintInput{plan_, r, false, true}; });
	}
	return changed;
}

void TunerRenderer::paint_pending_now()
{
	if (threads_)
		return;
	for (auto &handoff : handoffs_)
		while (handoff->run_one()) {
		}
	while (roster_handoff_->run_one()) {
	}
}

int TunerRenderer::pending_count() const
{
	const decor_preview::ScenePlan &plan = *plan_;
	int pending = 0;
	const std::size_t count = plan.hide ? 0u : std::min(plan.layout.tiles.size(), tiles_.size());
	for (std::size_t index = 0; index < count; ++index) {
		const TileCache &cache = tiles_[index];
		if (!cache.label_shown || cache.label_key != label_key_of(index))
			++pending;
		if (plan.parts.frame_glow && (!cache.frame_shown || cache.frame_key != frame_key_of(index)))
			++pending;
	}
	const std::size_t roster_count = std::min(plan.roster_owner.size(), roster_.size());
	for (std::size_t r = 0; r < roster_count; ++r)
		if (!roster_[r].shown || roster_[r].key != roster_key_of(r))
			++pending;
	return pending;
}

bool TunerRenderer::all_current() const
{
	return pending_count() == 0;
}

std::vector<decor_preview::SceneRosterImages> TunerRenderer::shown_roster(const std::vector<double> &levels,
									const std::vector<double> &dims) const
{
	const decor_preview::ScenePlan &plan = *plan_;
	std::vector<decor_preview::SceneRosterImages> shown;
	const std::size_t count = std::min(plan.roster_owner.size(), roster_.size());
	for (std::size_t r = 0; r < count; ++r) {
		const RosterCache &cache = roster_[r];
		decor_preview::SceneRosterImages images;
		if (cache.shown) {
			images.label = cache.label;
			images.label_owner = cache.key.owner;
		}
		images.label_current = cache.shown && cache.key == roster_key_of(r);
		images.level = r < levels.size() ? levels[r] : 0.0;
		images.dim = r < dims.size() ? dims[r] : 1.0;
		shown.push_back(images);
	}
	return shown;
}

decor_preview::SceneRosterLayout TunerRenderer::roster_layout() const
{
	return decor_preview::layout_scene_roster(*plan_, shown_roster({}, {}));
}

int TunerRenderer::roster_not_placed() const
{
	return roster_layout().not_placed;
}

QRect TunerRenderer::roster_label_rect(std::size_t r) const
{
	const std::vector<decor_preview::SceneRosterImages> shown = shown_roster({}, {});
	for (const sources::CompositeRosterPlaced &placed : decor_preview::layout_scene_roster(*plan_, shown).placed) {
		if (placed.index != r)
			continue;
		const sources::CompositeLabel &label = shown[r].label;
		if (label.label.isNull())
			return QRect();
		return QRect(label.label_x + placed.x - label.box_x, label.label_y + placed.y - label.box_y,
			     label.label.width(), label.label.height());
	}
	return QRect();
}

QRect TunerRenderer::roster_label_area(std::size_t r) const
{
	const std::vector<decor_preview::SceneRosterImages> shown = shown_roster({}, {});
	for (const sources::CompositeRosterPlaced &placed : decor_preview::layout_scene_roster(*plan_, shown).placed) {
		if (placed.index != r)
			continue;
		const sources::CompositeLabel &label = shown[r].label;
		const int dx = placed.x - label.box_x;
		const int dy = placed.y - label.box_y;
		QRect area;
		if (!label.label.isNull())
			area |= QRect(label.label_x + dx, label.label_y + dy, label.label.width(), label.label.height());
		if (!label.glow.isNull())
			area |= QRect(label.glow_x + dx, label.glow_y + dy, label.glow.width(), label.glow.height());
		return area;
	}
	return QRect();
}

const sources::CompositeLabel *TunerRenderer::shown_roster_label(std::size_t r) const
{
	if (r >= plan_->roster_owner.size() || r >= roster_.size() || !roster_[r].shown)
		return nullptr;
	return &roster_[r].label;
}

std::vector<decor_preview::SceneTileImages> TunerRenderer::shown_images(const std::vector<double> &levels,
									const std::vector<double> &dims) const
{
	const decor_preview::ScenePlan &plan = *plan_;
	std::vector<decor_preview::SceneTileImages> shown;
	const std::size_t count = std::min(plan.layout.tiles.size(), tiles_.size());
	for (std::size_t index = 0; index < count; ++index) {
		const TileCache &cache = tiles_[index];
		decor_preview::SceneTileImages images;
		if (cache.label_shown) {
			images.label = cache.label;
			images.label_owner = cache.label_key.owner;
		}
		images.label_current = cache.label_shown && cache.label_key == label_key_of(index);
		if (cache.frame_shown)
			images.frame = cache.frame;
		images.frame_current = cache.frame_shown && cache.frame_key == frame_key_of(index);
		images.level = index < levels.size() ? levels[index] : 0.0;
		images.dim = index < dims.size() ? dims[index] : 1.0;
		shown.push_back(images);
	}
	return shown;
}

QRect TunerRenderer::label_area(std::size_t tile) const
{
	const decor_preview::ScenePlan &plan = *plan_;
	if (tile >= plan.layout.tiles.size() || tile >= tiles_.size() || !tiles_[tile].label_shown)
		return QRect();
	const sources::PixelRect &cell = plan.layout.tiles[tile].rect;
	const sources::CompositeLabel &label = tiles_[tile].label;
	QRect area;
	if (!label.label.isNull())
		area |= QRect(cell.x + label.label_x, cell.y + label.label_y, label.label.width(),
			      label.label.height());
	if (!label.glow.isNull())
		area |= QRect(cell.x + label.glow_x, cell.y + label.glow_y, label.glow.width(), label.glow.height());
	return area;
}

QRect TunerRenderer::label_rect(std::size_t tile) const
{
	const decor_preview::ScenePlan &plan = *plan_;
	if (tile >= plan.layout.tiles.size() || tile >= tiles_.size() || !tiles_[tile].label_shown ||
	    tiles_[tile].label.label.isNull())
		return QRect();
	const sources::PixelRect &cell = plan.layout.tiles[tile].rect;
	const sources::CompositeLabel &label = tiles_[tile].label;
	return QRect(cell.x + label.label_x, cell.y + label.label_y, label.label.width(), label.label.height());
}

const sources::CompositeLabel *TunerRenderer::shown_label(std::size_t tile) const
{
	if (tile >= plan_->layout.tiles.size() || tile >= tiles_.size() || !tiles_[tile].label_shown)
		return nullptr;
	return &tiles_[tile].label;
}

} // namespace cordmux::label_tuner

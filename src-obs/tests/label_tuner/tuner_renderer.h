// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールが、場面 (tests/decor_preview/composite_scene.h) の名前ラベルと光を描き直し、背景に重ねる。
// 描き直しと置き方は合成のソース (sources/composite_decor.cpp) と同じにする。重い描画は描き直しのスレッドが行い
// (sources/composite_paint_handoff.h。保存先ごとに最後の仕事だけを描く)、描き直す鍵は製品の鍵
// (sources/composite_label_plan.h) を使い、描き終えるまでは前の画像を製品と同じ規則で置く。Qt Widgets には依存しない。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include <QImage>
#include <QRect>

#include "composite_scene.h"
#include "sources/composite_label.h"
#include "sources/composite_label_plan.h"
#include "sources/composite_layout.h"
#include "sources/composite_paint_handoff.h"

namespace cordmux::label_tuner {

class TunerRenderer {
public:
	// threads が真なら、タイルごとに描き直しのスレッドを 1 つ持つ (4 つまで同時に描く)。偽なら描き直しのスレッドを
	// 作らず、paint_pending_now がその場で描く (--shot で、描き終えるのを待たずに同じ結果を得るため)。
	explicit TunerRenderer(bool threads);
	~TunerRenderer();
	TunerRenderer(const TunerRenderer &) = delete;
	TunerRenderer &operator=(const TunerRenderer &) = delete;

	// 場面を変える。名前ラベルの見た目か、人と隅が変わったら、描き直す鍵の version を進める。描き直しは poll が頼む。
	void set_request(const decor_preview::SceneRequest &request);
	const decor_preview::SceneRequest &request() const { return request_; }
	const decor_preview::ScenePlan &plan() const { return *plan_; }

	// 描き終えた画像を受け取り、現在の場面で描けていない画像の描き直しを頼む。受け取った画像があれば true。
	// 画面のスレッドから、描くたびに呼ぶ。
	bool poll();

	// threads が偽のとき、頼んである描き直しを全部この場で描く。真のときは何もしない。
	void paint_pending_now();

	// 現在の場面のどの画像も、現在の鍵で描いたものか (描き直しを待っている画像がないか)。名前ラベルと光を置かない
	// 場面 (plan().hide) では真。
	bool all_current() const;

	// 描き直しを待っている画像の数。
	int pending_count() const;

	// 直近の 1 回の描き直しにかかった時間 (ms)。まだ描いていなければ 0。
	double last_paint_ms() const { return last_paint_ms_; }

	// 現在の場面の計画 (描き直しの仕事と、重ねる仕事が持つ)。
	std::shared_ptr<const decor_preview::ScenePlan> plan_ptr() const { return plan_; }

	// いま置いている画像と、明るさ。levels と dims はタイルの並びの、光の明るさと名前ラベルの色の倍率 (足りないタイルは
	// 明るさ 0、倍率 1)。置くかどうか (描き終えていない間に前の画像を置くか) は、重ねるときに合成のソースと同じ計画が
	// 決める。
	std::vector<decor_preview::SceneTileImages> shown_images(const std::vector<double> &levels,
								 const std::vector<double> &dims) const;

	// 画面共有していない参加者の、いま置いている画像と、明るさと倍率 (計画の並び)。
	std::vector<decor_preview::SceneRosterImages> shown_roster(const std::vector<double> &levels,
								  const std::vector<double> &dims) const;

	// タイル tile にいま置いている名前ラベルとその光の範囲 (キャンバスのピクセル)。置いていなければ空の矩形。
	QRect label_area(std::size_t tile) const;

	// タイル tile にいま置いている名前ラベルの画像の矩形 (キャンバスのピクセル。光を含めない)。置いていなければ空。
	QRect label_rect(std::size_t tile) const;

	// タイル tile にいま置いている名前ラベルの描画の結果 (省略したか、頭文字の円か など)。置いていなければ nullptr。
	const sources::CompositeLabel *shown_label(std::size_t tile) const;

	// 画面共有していない参加者 r (計画の並び) の、名前ラベルとその光の範囲、名前ラベルの画像の矩形 (どちらもキャンバスの
	// ピクセル)。置いていない (画像がない、入りきらない) ときは空の矩形。
	QRect roster_label_area(std::size_t r) const;
	QRect roster_label_rect(std::size_t r) const;

	// 画面共有していない参加者 r にいま置いている名前ラベルの描画の結果。画像がなければ nullptr。
	const sources::CompositeLabel *shown_roster_label(std::size_t r) const;

	// 現在の置き方で入りきらずに置かなかった人の数。
	int roster_not_placed() const;

private:
	using PaintKey = std::variant<sources::CompositeLabelKey, sources::CompositeFrameGlowKey>;
	struct PaintInput {
		std::shared_ptr<const decor_preview::ScenePlan> plan;
		std::size_t tile = 0; // roster が真なら、画面共有していない参加者の添字
		bool frame = false;
		bool roster = false;
	};
	using PaintOutput = std::variant<sources::CompositeLabel, QImage>;
	using Handoff = sources::PaintHandoff<PaintKey, PaintInput, PaintOutput>;

	// タイル (並びの位置) 1 つ分の、置いている画像とその鍵。
	struct TileCache {
		bool label_shown = false;
		sources::CompositeLabelKey label_key;
		sources::CompositeLabel label;
		bool frame_shown = false;
		sources::CompositeFrameGlowKey frame_key;
		QImage frame;
	};

	// 画面共有していない参加者 (計画の並びの位置) 1 人分の、置いている画像とその鍵。
	struct RosterCache {
		bool shown = false;
		sources::CompositeLabelKey key;
		sources::CompositeLabel label;
	};

	sources::CompositeLabelKey label_key_of(std::size_t tile) const;
	sources::CompositeFrameGlowKey frame_key_of(std::size_t tile) const;
	sources::CompositeLabelKey roster_key_of(std::size_t r) const;
	// 画面共有していない参加者を置く位置 (いま置いている画像から決める)。
	decor_preview::SceneRosterLayout roster_layout() const;

	bool threads_ = true;
	decor_preview::SceneRequest request_;
	std::shared_ptr<const decor_preview::ScenePlan> plan_;
	std::uint64_t version_ = 1;
	std::uint64_t look_version_ = 1;
	std::array<TileCache, sources::kCompositeCellCount> tiles_;
	// タイルごとの受け渡し。保存先 0 は名前ラベル、1 は枠の光。
	std::array<std::unique_ptr<Handoff>, sources::kCompositeCellCount> handoffs_;
	// 画面共有していない参加者の受け渡し。1 つのスレッドで、保存先が 1 人ずつ。
	std::array<RosterCache, decor_preview::kMaxRosterMembers> roster_;
	std::unique_ptr<Handoff> roster_handoff_;
	double last_paint_ms_ = 0.0;
};

} // namespace cordmux::label_tuner

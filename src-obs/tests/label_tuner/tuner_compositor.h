// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールで、背景に名前ラベルと光を重ねる仕事を、画面のスレッドの外で行う。重ねるのは
// tests/decor_preview/composite_scene.h の draw_scene_region (合成のソースの置く物の計画と、CPU で重ねる関数) で、
// 頼まれた範囲だけを、行を分けて同時に重ねる。光が大きいと、キャンバス全体を重ねるのに 0.3 秒ほどかかり、画面の
// スレッドで重ねるとつまみが動かなくなるためである。待っている仕事は最後の 1 つだけを持つ (古い明るさの重ねを積まない)。

#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <QImage>
#include <QRect>

#include "composite_scene.h"

namespace cordmux::label_tuner {

struct ComposeJob {
	std::uint64_t serial = 0;
	QImage background; // キャンバス全体の背景 (QImage::Format_ARGB32_Premultiplied)
	std::shared_ptr<const decor_preview::ScenePlan> plan;
	std::vector<decor_preview::SceneTileImages> tiles;
	std::vector<decor_preview::SceneRosterImages> roster; // 画面共有していない参加者
	QRect region; // 重ねる範囲 (キャンバスのピクセル)。キャンバスの中にあること
};

struct ComposeResult {
	std::uint64_t serial = 0;
	QRect region;
	QImage image; // region の大きさの、重ねた画像
	double compose_ms = 0.0;
};

// job の範囲を、行を bands に分けて同時に重ねる。どのピクセルも、キャンバス全体を draw_scene で重ねたときの同じピクセルと
// 同じ値になる。
QImage compose_region(const ComposeJob &job, int bands);

class ComposeWorker {
public:
	// threaded が偽ならスレッドを作らず、run_pending がその場で重ねる (--shot)。
	ComposeWorker(bool threaded, int bands);
	~ComposeWorker();
	ComposeWorker(const ComposeWorker &) = delete;
	ComposeWorker &operator=(const ComposeWorker &) = delete;

	// 仕事を置く。待っている仕事があれば置き換える。
	void submit(ComposeJob job);
	// 重ね終えた最後の画像を受け取る。なければ空。
	std::optional<ComposeResult> take();
	// threaded が偽のとき、待っている仕事をその場で重ねる。
	void run_pending();

private:
	void loop();
	ComposeResult run(const ComposeJob &job) const;

	const int bands_;
	std::mutex mu_;
	std::condition_variable cv_;
	std::optional<ComposeJob> pending_;
	std::optional<ComposeResult> done_;
	bool stopped_ = false;
	std::thread thread_;
};

} // namespace cordmux::label_tuner

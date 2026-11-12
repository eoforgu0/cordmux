// SPDX-License-Identifier: Apache-2.0

// tuner_compositor.h の実装。

#include "tuner_compositor.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <future>
#include <utility>

namespace cordmux::label_tuner {

QImage compose_region(const ComposeJob &job, int bands)
{
	if (!job.plan || job.background.isNull() || job.region.isEmpty())
		return QImage();
	const QRect region = job.region.intersected(job.background.rect());
	const int rows = region.height();
	const int count = std::clamp(bands, 1, std::max(1, rows));
	// 帯ごとに別の画像を作って重ね、最後に 1 枚へコピーする (同じ QImage に複数のスレッドから書かない)。
	std::vector<QImage> parts(static_cast<std::size_t>(count));
	std::vector<int> tops(static_cast<std::size_t>(count));
	std::vector<std::future<void>> running;
	for (int b = 0; b < count; ++b) {
		const int y0 = rows * b / count;
		const int y1 = rows * (b + 1) / count;
		const auto index = static_cast<std::size_t>(b);
		tops[index] = y0;
		parts[index] = job.background.copy(QRect(region.x(), region.y() + y0, region.width(), y1 - y0));
		QImage *part = &parts[index];
		const int origin_y = region.y() + y0;
		const auto work = [&job, part, region, origin_y]() {
			decor_preview::draw_scene_region(part, region.x(), origin_y, *job.plan, job.tiles, job.roster);
		};
		if (b + 1 == count)
			work(); // 最後の帯は呼んだスレッドで重ねる
		else
			running.push_back(std::async(std::launch::async, work));
	}
	for (std::future<void> &f : running)
		f.get();
	if (count == 1)
		return parts.front();
	QImage out(region.size(), QImage::Format_ARGB32_Premultiplied);
	const auto row_bytes = static_cast<std::size_t>(out.bytesPerLine());
	for (std::size_t b = 0; b < parts.size(); ++b)
		for (int y = 0; y < parts[b].height(); ++y)
			std::memcpy(out.scanLine(tops[b] + y), parts[b].constScanLine(y), row_bytes);
	return out;
}

ComposeWorker::ComposeWorker(bool threaded, int bands) : bands_(bands)
{
	if (threaded)
		thread_ = std::thread([this]() { loop(); });
}

ComposeWorker::~ComposeWorker()
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopped_ = true;
		pending_.reset();
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void ComposeWorker::submit(ComposeJob job)
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		pending_ = std::move(job);
	}
	cv_.notify_one();
}

std::optional<ComposeResult> ComposeWorker::take()
{
	std::lock_guard<std::mutex> lock(mu_);
	return std::exchange(done_, std::nullopt);
}

void ComposeWorker::run_pending()
{
	std::optional<ComposeJob> job;
	{
		std::lock_guard<std::mutex> lock(mu_);
		job = std::exchange(pending_, std::nullopt);
	}
	if (!job)
		return;
	ComposeResult result = run(*job);
	std::lock_guard<std::mutex> lock(mu_);
	done_ = std::move(result);
}

ComposeResult ComposeWorker::run(const ComposeJob &job) const
{
	const auto started = std::chrono::steady_clock::now();
	ComposeResult result;
	result.serial = job.serial;
	result.region = job.region;
	// 重ねられなければ (メモリーが足りないなど) 空の画像を返し、表示は前の画像のままにする。
	try {
		result.image = compose_region(job, bands_);
	} catch (const std::exception &) {
		result.image = QImage();
	}
	result.compose_ms =
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	return result;
}

void ComposeWorker::loop()
{
	for (;;) {
		std::optional<ComposeJob> job;
		{
			std::unique_lock<std::mutex> lock(mu_);
			cv_.wait(lock, [this]() { return stopped_ || pending_.has_value(); });
			if (stopped_)
				return;
			job = std::exchange(pending_, std::nullopt);
		}
		ComposeResult result = run(*job);
		std::lock_guard<std::mutex> lock(mu_);
		if (!stopped_)
			done_ = std::move(result);
	}
}

} // namespace cordmux::label_tuner

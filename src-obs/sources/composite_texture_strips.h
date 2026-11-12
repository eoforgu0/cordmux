// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux が、大きな画像を横長の帯に分けてテクスチャにするための純粋関数と型。帯の分け方
// (plan_composite_texture_strips)、切り抜きと帯の重なり (for_each_composite_strip_piece)、描き直しのスレッドが
// グラフィックスのコンテキストに入る間合い (CompositeUploadTurn) を決める。libobs にも Qt にも依存しないので、
// テスト (align_tests) がヘッダーを直接読む。テクスチャを作って置く処理は sources/composite_decor.cpp にある。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace cordmux::sources {

// 帯 1 つのテクスチャの大きさの上限 (バイト。幅 x 4 バイト x 行数で、上下の余分の行も数える)。描き直しのスレッドは
// テクスチャを 1 つ作る間グラフィックスのコンテキストを持ち、その間は描画のスレッドがフレームを描き始められない。
// 1 回に待たせる時間は 5 ms 以下にする (60 fps の 1 フレームは 16.7 ms で、フレームを遅らせないため)。OBS の中で
// 測ると、6.5 MB の光の画像 1 つに 11-24 ms、2 MB の画像に 4 ms 未満かかった。1 MB あたり 2-4 ms で、最初の確保は
// 遅い。512 KB なら長くても 2 ms ほどで、5 ms の線に 2 倍の余裕がある。
constexpr std::uint64_t kCompositeStripMaxBytes = 512u * 1024u;

// 帯 1 つ。画像の top 行から rows 行を置く。テクスチャは、その上の pad_top 行と下の pad_bottom 行 (どちらも 0 か 1)
// も持つ。余分の行は置かないが、合成のソースがシーンで拡大縮小されて Linear で読むとき、帯の境で隣の行を読むのに
// 必要である。これで、帯に分けても 1 枚のテクスチャで置いたときと同じに見える (境に筋が出ない)。画像の上の端と下の端の帯は、
// 端の側に余分の行を持たない (そこは Clamp で読むので、1 枚のときと同じになる)。
struct CompositeTextureStrip {
	int top = 0;
	int rows = 0;
	int pad_top = 0;
	int pad_bottom = 0;

	// テクスチャの最初の行の、画像の中の位置。
	int texture_top() const { return top - pad_top; }
	// テクスチャの行数。
	int texture_rows() const { return pad_top + rows + pad_bottom; }
};

// 幅 width、高さ height の画像 (1 ピクセル 4 バイト) を分ける帯を、上から順に返す。どの行もちょうど 1 つの帯で
// 置かれ、余分の行は画像の外に出ない。画像の大きさが max_bytes 以下なら、余分の行のない帯 1 つ (1 枚のテクスチャ) に
// する。それより大きければ、帯のテクスチャ (余分の行を含む) が max_bytes 以下になるように分ける。幅が広くて、余分の行を
// 含めると置く行が 1 行でも上限を超えるときは、置く行を 1 行にする (その帯は上限を超える)。幅か高さが 0 以下なら空を返す。
inline std::vector<CompositeTextureStrip> plan_composite_texture_strips(int width, int height, std::uint64_t max_bytes)
{
	std::vector<CompositeTextureStrip> strips;
	if (width <= 0 || height <= 0)
		return strips;
	const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4u;
	const int limit_rows =
		static_cast<int>(std::min<std::uint64_t>(max_bytes / row_bytes, static_cast<std::uint64_t>(height)));
	if (limit_rows >= height) {
		strips.push_back({0, height, 0, 0});
		return strips;
	}
	for (int top = 0; top < height;) {
		CompositeTextureStrip strip;
		strip.top = top;
		strip.pad_top = top > 0 ? 1 : 0;
		const int left = height - top;
		// 下に余分の行を持たずに置ける行数。残りが全部入れば、それが最後の帯になる。
		const int room = limit_rows - strip.pad_top;
		if (room >= left) {
			strip.rows = left;
		} else {
			strip.rows = std::max(1, room - 1);
			strip.pad_bottom = top + strip.rows < height ? 1 : 0;
		}
		strips.push_back(strip);
		top += strip.rows;
	}
	return strips;
}

// 画像の行 first から count 行を置くために、帯ごとに置く部分を上から順に fn(index, texture_row, image_row, rows) で
// 渡す。index は帯の番号、texture_row はその帯のテクスチャの中で読み始める行、image_row は置き始める画像の行、rows は
// 行数である。帯の置く行と重なる部分だけを渡し、余分の行は置かない。
template <typename Fn>
void for_each_composite_strip_piece(const std::vector<CompositeTextureStrip> &strips, int first, int count, Fn &&fn)
{
	for (std::size_t index = 0; index < strips.size(); ++index) {
		const CompositeTextureStrip &strip = strips[index];
		const int top = std::max(first, strip.top);
		const int bottom = std::min(first + count, strip.top + strip.rows);
		if (bottom > top)
			fn(index, top - strip.texture_top(), top, bottom - top);
	}
}

// 描き直しのスレッドが待つ長さの上限 (ns)。60 fps では 6 フレーム、24 fps でも 2 フレームより長い。上限が 1 フレーム
// より短くなる 10 fps 以下では、1 フレームの時間 (100 ms 以上) に比べて帯 1 つを作る時間 (2 ms ほど) が小さいので、
// 上限で待ちをやめて入っても、描画のスレッドを待たせる時間は 1 フレームの時間に比べて小さい。
constexpr std::uint64_t kCompositeUploadWaitLimitNs = 100'000'000u;

// CompositeUploadTurn が読む時計。製品では libobs の関数を渡し、テストは差し替える。
//   frame_time  描画のスレッドの現在のフレームの時刻 (obs_get_video_frame_time)
//   now_ns      現在の時刻 (os_gettime_ns)
//   sleep       短く眠る (os_sleep_ms(1))
//   stopping    描き直しのスレッドを止めるときに真
struct CompositeUploadClock {
	std::function<std::uint64_t()> frame_time;
	std::function<std::uint64_t()> now_ns;
	std::function<void()> sleep;
	std::function<bool()> stopping;
};

// 描き直しのスレッドが、帯のテクスチャを 1 つ作るたびにグラフィックスのコンテキストに入る間合い。描き直しのスレッドから
// だけ使う。
//
// 描画のスレッドはフレームを描く間だけコンテキストを持つ (OBS 31.1.1 の libobs/obs-video.c の
// obs_graphics_thread_loop)。描き直しのスレッドが続けて入ると、ミューテックスを待っている描画のスレッドが間に入れない
// ことがある。そこで、帯を 1 つ作ってコンテキストを出たら、描画のスレッドが次のフレームに進むまで次の帯を作らない。
// 描画のスレッドを待たせるのは、1 フレームに帯 1 つ分までになる。その代わり、名前ラベルが出るまでが帯の数 x 1 フレーム
// だけ遅れる。配信のフレームを遅らせないことを優先するので、この遅れを縮めるために待ちを外さない。
//
// 進んだことは、obs_get_video_frame_time が返す video_time が変わったことで知る。video_time は、obs_graphics_thread_loop
// の末尾の video_sleep が、次のフレームの時刻まで眠った後に毎回書く。ソースが見えているかや、出力があるかに関係なく
// 進む。書くのはコンテキストを放した後なので、変わったときには前のフレームを描き終えている。OBS のタスク
// (obs_queue_task の OBS_TASK_GRAPHICS) でフレームの終わりを伝える方法は採らない。待ちをやめた後や描画のスレッドが
// 止まった後にもタスクが残り、壊した物を指すことがあるためである。
//
// 待つ長さには上限 (limit_ns) を付ける。OBS を終えるときは、描画のスレッドを止めてから (obs.c の obs_shutdown の
// stop_video) モジュールを閉じる (free_module) ので、video_time が進まないまま描き直しのスレッドを止めることがある。
// stopping が真になったら、待ちをやめてすぐ返す。待つ間は短く眠って見に行く (回り続けない)。
class CompositeUploadTurn {
public:
	explicit CompositeUploadTurn(CompositeUploadClock clock, std::uint64_t limit_ns = kCompositeUploadWaitLimitNs)
		: clock_(std::move(clock)), limit_ns_(limit_ns)
	{
	}

	// コンテキストに入る前に呼ぶ。前にコンテキストを出てから描画のスレッドが次のフレームに進むまで待つ (初めて入る
	// ときと、すでに進んでいるときは待たない)。上限まで待っても進まなければ true を返す。止めるときは待ちをやめて
	// false を返す (残りの帯を作らない)。
	bool wait()
	{
		if (clock_.stopping())
			return false;
		if (!has_left_)
			return true;
		const std::uint64_t started = clock_.now_ns();
		while (clock_.frame_time() == left_frame_) {
			if (clock_.now_ns() - started >= limit_ns_)
				return true;
			clock_.sleep();
			if (clock_.stopping())
				return false;
		}
		return true;
	}

	// コンテキストを出た直後に呼ぶ。
	void left()
	{
		has_left_ = true;
		left_frame_ = clock_.frame_time();
	}

private:
	CompositeUploadClock clock_;
	std::uint64_t limit_ns_ = kCompositeUploadWaitLimitNs;
	bool has_left_ = false;
	std::uint64_t left_frame_ = 0;
};

} // namespace cordmux::sources

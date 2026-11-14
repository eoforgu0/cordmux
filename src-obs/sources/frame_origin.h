// SPDX-License-Identifier: Apache-2.0

// 合成のソースの子 1 つの、テクスチャに入っているフレームがダミー表示のフレームか実際の画面共有のフレームかを決める記録。
// 合成のソースが 1 人の映像を全面に描いたときに、それがダミーかを、ダミー表示のフラグではなく描いたテクスチャの中身で
// 決めるために使う (sources/composite_source.cpp)。libobs にも Qt にも依存しない。ロックは持たない (呼び出し側が守る)。
//
// libobs の非同期の映像のソースは、tick (async_tick) で表示するフレームを選び (このとき子の測定のフィルタの filter_video が
// 呼ばれる)、そのフレームの描画で初めてテクスチャに入れる (obs-source.c の obs_source_update_async_video)。描かれなかった
// フレームで選ばれたフレームは、次の tick で捨てられ、テクスチャに入らない。そこで:
//   - FrameOriginLog: 子に渡したフレームのタイムスタンプとダミーかを覚え、選ばれたフレームを引く。
//   - TileTextureOrigin: tick ごとに「選ばれた」を消し、選ばれたら覚え、合成のソースが子を描いたときに、そのフレームを
//     テクスチャの元として確定する。選ばれなかったフレームの描画では、テクスチャの元は前のまま。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cordmux::sources {

class FrameOriginLog {
public:
	// 覚えるフレームの数。合成のソースの子は OBS の緩衝なしで受け、OBS は tick ごとに最新の 1 つを表示に選ぶので、tick の
	// 間に渡したフレームの数より多ければよい (60 fps の 1 秒分の余裕を持たせる)。
	static constexpr std::size_t kCapacity = 64;

	// 子にフレームを渡す前に呼ぶ (OBS が表示に選ぶ前に記録があるように)。timestamp は渡すフレームのタイムスタンプ。
	void note_output(std::uint64_t timestamp, bool dummy)
	{
		entries_[next_] = Entry{timestamp, dummy, true};
		next_ = (next_ + 1) % kCapacity;
	}

	// timestamp のフレームがダミーか。記録にあれば真を返して *dummy に入れる。なければ (古すぎる、またはこの記録を通さずに
	// 渡したフレーム) 偽。同じタイムスタンプが 2 つあれば、新しいほうで決める。
	bool lookup(std::uint64_t timestamp, bool *dummy) const
	{
		for (std::size_t i = 0; i < kCapacity; ++i) {
			const Entry &e = entries_[(next_ + kCapacity - 1 - i) % kCapacity];
			if (e.used && e.timestamp == timestamp) {
				*dummy = e.dummy;
				return true;
			}
		}
		return false;
	}

private:
	struct Entry {
		std::uint64_t timestamp = 0;
		bool dummy = false;
		bool used = false;
	};
	std::array<Entry, kCapacity> entries_{};
	std::size_t next_ = 0;
};

class TileTextureOrigin {
public:
	// OBS の tick の始め (合成のソースの tick のコールバック。ソースの tick より前)。前の tick で選ばれて描かれなかったフレームは、
	// この tick で捨てられるので忘れる。
	void on_tick() { selected_ = false; }

	// OBS がこの tick で表示にフレームを選んだ (子の測定のフィルタの filter_video)。dummy はそのフレームがダミーか。
	void on_selected(bool dummy)
	{
		selected_ = true;
		selected_dummy_ = dummy;
	}

	// 合成のソースが子を描いた。この tick で選ばれたフレームがあれば、それがテクスチャに入ったので確定する。同じフレームの
	// 2 回目からの描画 (プレビューなど) は、テクスチャの元を変えない。
	void on_drawn()
	{
		if (!selected_)
			return;
		texture_dummy_ = selected_dummy_;
		selected_ = false;
	}

	// テクスチャに入っているフレームがダミーか。まだ何も描いていなければ偽。
	bool texture_dummy() const { return texture_dummy_; }

private:
	bool selected_ = false;
	bool selected_dummy_ = false;
	bool texture_dummy_ = false;
};

} // namespace cordmux::sources

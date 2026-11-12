// SPDX-License-Identifier: Apache-2.0

// 画面共有のデコードしたフレームを OBS に渡すかの判断 (純粋関数)。渡す先は合成のソース (sources/composite_source.h)
// の子だけである。libobs にも asio にも依存しないので、単体テストから直接使える。

#pragma once

namespace cordmux::pipeline {

// フレームを渡してよいか。
//   composite_present  合成のソースが現在のシーンコレクションにあるか
//   slot               フレームの経路の画面の枠の番号 (枠から外した人の経路は 0)
// 合成のソースがない間と、枠の外の経路では渡さない (呼び出し元が捨てて数える)。
//
// 渡すときは、表示の緩衝 D が 0 でも常に待ち行列 (FramePacer) を通し、渡し先の子を OBS の緩衝なし
// (async_unbuffered) にする。D が 0 のときに OBS の緩衝を使うと、映像は OBS の緩衝の深さ (偶然の深さ) で遅れ、
// 音は cordmux が決めた時刻で出て、ずれる。待ち行列を通らない渡し方を混ぜないので、待ち行列にある古いフレームが
// 新しいフレームの後に表示されることもない。
inline bool frame_deliverable(bool composite_present, int slot)
{
	return composite_present && slot >= 1;
}

} // namespace cordmux::pipeline

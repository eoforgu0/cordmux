// SPDX-License-Identifier: Apache-2.0

// 画面共有のデコーダーがハードウェアデコードを使えずソフトウェアデコードに切り替えたことを、ストリームごとに
// 1 度だけ伝えるための純粋な型。切り替えは設定の [デコーダー] の値が動作しないことを意味するが、画面には出さず、
// OBS のログと診断情報に残す。libobs にも Qt にも依存しない。ロックは持たず、1 つのスレッドだけが使う。

#pragma once

namespace cordmux::pipeline {

class HwFallbackNote {
public:
	// デコーダーの現在の hw_fallback (decode::DecoderStats) を渡す。切り替えたことを初めて見たときだけ true を返す
	// (呼び出し元はそのときだけログに 1 行書く)。
	bool observe(bool hw_fallback)
	{
		if (!hw_fallback || noted_)
			return false;
		noted_ = true;
		return true;
	}

private:
	bool noted_ = false;
};

} // namespace cordmux::pipeline

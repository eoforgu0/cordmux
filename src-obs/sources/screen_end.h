// SPDX-License-Identifier: Apache-2.0

// 画面共有の枠の映像が終わる事実ごとに、枠を空けるタイミングと、並べ直すかどうかを決める。
// 呼び出し元 (pipeline.cpp の configure と切断の処理、dock_poll_layout.cpp の、参加者の行のチェックを外したときの
// 処理) は、事実をこの列挙で指定し、返った動作だけを行う。呼び出し元ごとに書くと動作が食い違うので、決定をここに
// まとめる。
// libobs にも Qt にも依存しない。

#pragma once

namespace cordmux::sources {

// 枠の映像が終わることがある事実。
enum class ScreenEndEvent {
	ShareEnded,      // 送信側が画面共有の終了を伝えた (configure からその人の画面共有のストリームが消えた)
	LeftCall,        // その人が通話から抜けた (同じく configure からストリームが消える)
	RemovedFromSlot, // 利用者がドックで枠から外した
	LinkLost,        // Vesktop との接続が切れたか、ハートビートが続けて返らなかった
	FramesStopped,   // 接続は生きているが、フレームだけが来ない (静止した画面の共有と止まった共有を見分けられない)
	DummyStopped,    // ダミー表示を止めた
	// フレームは届いているのにデコード済みのフレームが出ないので、デコードの経路を作り直した
	// (pipeline/receive_restart.h の plan_decode_restart)。
	ReceiveRestarted,
};

// その事実で枠に行うこと。
struct ScreenEndAction {
	bool clear_now = false;       // その場ですぐ空ける (clear_screen_now)
	bool hold_then_clear = false; // 設定の長さ (link_lost_hold_ms) だけ最後のフレームを表示してから空ける
	bool relayout = false;        // 同じ処理で自動配置を並べ直す
	// 名前ラベルに描く人 (set_decor_owner) を外す。空いた枠に前の人の名前ラベルを残さないためで、次の人の最初の
	// フレームで新しい人が入る。外すのは、その人がこの枠に戻らないと分かる事実のときだけである。接続の途切れと
	// ダミーでは同じ人が戻るので外さない。
	bool clear_owner = false;
};

constexpr ScreenEndAction screen_end_action(ScreenEndEvent event)
{
	switch (event) {
	case ScreenEndEvent::ShareEnded:
	case ScreenEndEvent::LeftCall:
	case ScreenEndEvent::RemovedFromSlot:
		// 終わったことが分かっているので、最後のフレームを残す理由がない。空いた場所を残さないように並べ直し、
		// 名前ラベルに描く人も外す。
		return ScreenEndAction{true, false, true, true};
	case ScreenEndEvent::LinkLost:
		// 接続の途切れからは戻ることがある (割り当て表は残る)。並べ直すと、戻ったときにもう一度配置が変わる。
		// 戻る人なので、名前ラベルに描く人も残す。
		return ScreenEndAction{false, true, false, false};
	case ScreenEndEvent::FramesStopped:
		// 空けると、静止した文書の共有まで空けてしまうので、何もしない。フレームが来ないことはメッセージで扱う。
		return ScreenEndAction{false, false, false, false};
	case ScreenEndEvent::ReceiveRestarted:
		// 同じ人の映像が、新しいキーフレームから戻ることを待っている。空けると、合成のソースの子の大きさが 0 になり、
		// その人の名前ラベルが枠から画面共有していない参加者の並びへ移り、映像が戻るとまた枠に戻る。戻るまでは、
		// 届かなくなったとき (FramesStopped) と同じく最後のフレームと名前ラベルを残し、並べ直さない。
		return ScreenEndAction{false, false, false, false};
	case ScreenEndEvent::DummyStopped:
		// ダミーの画像を残さないように空ける。ダミーの間も同じ枠の配置で並べているので、並べ直さない。
		// 名前ラベルに描く人はダミーの間も実際の人のままなので外さない (ダミーを止めた後の最初のフレームで、
		// その人の名前が表示される)。
		return ScreenEndAction{true, false, false, false};
	}
	return ScreenEndAction{};
}

} // namespace cordmux::sources

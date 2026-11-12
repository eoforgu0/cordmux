// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、Vesktop への追加、同梱の Vencord の再インストール、照合の結果の読み直し、
// アンインストールを扱うものの状態 (DockPollInstallState) と関数 (DockPollInstallPart) を宣言する。実装は
// dock_poll_install.cpp にある。DockPoll のメンバー関数 (write_vencord_dir など) の宣言は dock_poll.h にある。
// dock_poll.h を include しない (dock_poll.h がこのヘッダーを include する)。

#pragma once

class QWidget;

namespace cordmux::dock {

class DockPoll;

// インストールに関わる処理の状態。DockPoll がメンバー install_state_ として持つ。
//
// 時間がかかることのある操作は、押したときのイベントの中では処理せず、QTimer::singleShot(0, ...) でイベントループの
// 次の周回に送る。押したボタンの見た目と処理中の表示を、先に描画するためである。処理の間だけ下のフィールドを
// true にし、終えたら false に戻してから DockPoll::tick() を呼ぶ。ドック (dock_poll.cpp) と設定ウィンドウは、
// このフィールドを見て、押したボタンを押せなくし、処理中のテキストを表示してよい。true の間は、同じ操作が
// もう 1 度押されても実行しない。
struct DockPollInstallState {
	bool adding_to_vesktop = false; // [Vesktopに追加] で書き込んでいる
	bool uninstalling = false;      // 確認のダイアログで [アンインストール] を選んでから、結果を表示するまで
	// アンインストールの確認のダイアログが開いている。処理中の表示には使わず、2 つ目の確認のダイアログを
	// 開かないために使う。確認のダイアログは親ウィンドウの操作だけを止めるので、設定ウィンドウから開いている間は
	// ドックのボタンを、フローティングのドックから開いている間は設定ウィンドウのボタンを押せる。
	bool uninstall_confirm_open = false;
};

// インストールに関わる処理。static の関数だけを持ち、第 1 引数に DockPoll を取る。
// DockPoll はこの型を friend にしている。
struct DockPollInstallPart {
	// owner を親にして確認のダイアログを開き、[アンインストール] が選ばれたらアンインストールして、
	// 結果のダイアログを開く。owner は、ドックから呼ぶときはドック、設定ウィンドウから呼ぶときは
	// 設定ウィンドウにする。focus_fallback は、ダイアログを閉じた後に、開く前のウィジェットにフォーカスを戻せない
	// (破棄されたか、押せなくなった) ときの移し先である。設定ウィンドウなら、そのページの最初の項目を渡す。なければ
	// nullptr。
	static void uninstall(DockPoll &self, QWidget *owner, QWidget *focus_fallback = nullptr);
};

} // namespace cordmux::dock

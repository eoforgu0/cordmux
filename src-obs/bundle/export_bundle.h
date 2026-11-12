// SPDX-License-Identifier: Apache-2.0

// 診断情報の zip を作り (prepare_bundle)、ファイルに書く (write_bundle)。ダイアログは表示しない。保存と失敗の
// ダイアログは、設定ウィンドウを親ウィンドウにして、呼び出し元 (dock/dock_poll_export.cpp) が表示する。どちらの関数も
// Qt のウィジェットに触らない (QFile と QDir はウィジェットではない) ので、UI のスレッドの外で呼んでよい。呼び出し元は
// ワーカースレッドで呼び、押されたボタンを先に cordmux.settings.export_diagnostics_busy.button の表示にする。
// このヘッダーは Qt のヘッダーを読み込まない (Qt を使わない単体テストが、このヘッダーを読むため)。

#pragma once

#include <string>
#include <vector>

#include "bundle/bundle_files.h"
#include "bundle/zip_writer.h"

namespace cordmux::bundle {

enum class ExportOutcome {
	Cancelled,   // 利用者が保存のダイアログを閉じた (何も書いていない)
	SecretFound, // 検出した中身を置き換えた後も、全文でトークンを検出した (書かない)
	ZipFailed,   // zip を作れなかった (ファイルの名前か大きさの問題)
	WriteFailed, // 選ばれた場所に書けなかった
	Written,     // 書けた
};

struct ExportRequest {
	BundleMaterials materials;

	// zip の中のファイルに書く日時と、保存のダイアログに表示する既定のファイル名
	// (cordmux-diagnostics-<YYYYMMDD-HHMMSS>.zip)。どちらも呼び出し元が作る (このファイルの関数は時計を読まない)。
	ZipDateTime stamp;
	std::string suggested_name;

	// 伏せる関数と確認する関数 (bundle/bundle_files.h の RedactFn と RevealFn)。トークンを知っているのは呼び出し元
	// だけで、トークンはこのファイルの関数にも zip のデータにも渡らない。ワーカースレッドから呼ばれるので、関数は自分で
	// ロックを取ること (plugin-main.cpp の 2 つは g_session_guard_mutex を取る)。
	RedactFn redact;
	RevealFn reveals;
};

// prepare_bundle の結果。outcome が Written なら zip に中身があり、保存先を尋ねてよい (名前は Written だが、まだ書いて
// いない。結果の種類を 1 つの列挙型にそろえるため)。SecretFound と ZipFailed なら、保存先を尋ねない (書かないと決まって
// いるファイルの保存先を、利用者に選ばせない)。
struct PreparedBundle {
	ExportOutcome outcome = ExportOutcome::ZipFailed;
	std::string zip;                   // 作った zip の中身
	std::string error;                 // 作れなかった理由 (ログに書く。画面には ini の文字列を表示する)
	std::vector<std::string> left_out; // トークンを検出して、1 行の説明に置き換えたファイルの名前 (呼び出し元がログに書く)
};

// 置き換えたファイルに入れる 1 行 (英語。zip の中は人とツールが読むデータなので、ini の文字列を使わない。
// bundle/bundle_manifest.h と同じ扱い)。
inline std::string left_out_note(const std::string &name)
{
	return name + " was left out of this bundle because a connection secret remained in it after redaction.\n";
}

// トークンを検出したファイルだけを left_out_note に置き換え、置き換えたファイルの名前を並びの順に返す。検出するかは、
// ファイルごとに bundle_reveals_secret と同じ形 (名前と中身) で確認する。reveals が空なら何もしない。
// 検出したファイルだけを置き換えるのは、何も書かずに失敗にすると、もう一度試しても同じリングバッファーから同じ中身を
// 作るので、同じ結果になるためである。行だけを消す方法は使わない (ログの行の区切りをまたいで検出すると、消す行を
// 決められない)。置き換えた後の全文は、呼び出し元 (prepare_bundle) がもう一度確認する (ファイルをまたいで検出する
// 場合を見逃さないため)。
inline std::vector<std::string> leave_out_revealing_entries(std::vector<ZipEntry> &entries, const RevealFn &reveals)
{
	std::vector<std::string> left_out;
	if (!reveals)
		return left_out;
	for (ZipEntry &entry : entries) {
		if (!bundle_reveals_secret({entry}, reveals))
			continue;
		entry.content = left_out_note(entry.name);
		left_out.push_back(entry.name);
	}
	return left_out;
}

struct ExportResult {
	ExportOutcome outcome = ExportOutcome::Cancelled;
	std::string path;  // 書けたときだけ入る (利用者が選んだ場所)
	std::string error; // 書けなかったときの理由 (ログに書く。画面には ini の文字列を表示する)
};

// トークンを伏せ、残っていないかを確認し、zip を作る。どのスレッドから呼んでもよい (ダイアログを表示せず、Qt の
// ウィジェットに触らない)。
PreparedBundle prepare_bundle(const ExportRequest &request);

// 作った zip を path (UTF-8 の絶対パス) に書く。どのスレッドから呼んでもよい。途中まで書けたファイルは残さない
// (途中までの zip を診断情報として渡さないため)。書けたら outcome は Written で path が入り、書けなければ WriteFailed で
// error が入る。
ExportResult write_bundle(const std::string &path, const std::string &zip);

// 保存のダイアログで最初に開くフォルダー。デスクトップ、なければドキュメント、なければホームのフォルダー。
std::string default_export_directory();

} // namespace cordmux::bundle

// SPDX-License-Identifier: Apache-2.0

// [再インストール] (cordmux.dock.reinstall.button) で、data/vencord-src のファイルを data/vencord へコピーする
// 手順を決める純粋関数 (plan_reinstall)。前のバージョンのファイルが data/vencord に残っていたときのコピーにも
// 使う。この関数はファイルに触らない。libobs にも Qt にも依存しない。

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cordmux::install {

// コピーする 1 つのファイル。from と to はパス (UTF-8)、name はファイル名 (コピーできなかったときに表示する)。
struct CopyStep {
	std::string name;
	std::string from;
	std::string to;
};

// original_dir (data/vencord-src) にある original_file_names を、shown_dir (data/vencord) へコピーする手順
// (CopyStep の並び) を返す。名前はフォルダーを含まない名前にする (platform::list_directory が返す形。呼び出し元が
// フォルダーを除いて渡す)。空の一覧なら空の並びを返す。パスは platform/bundle_check.h の join_path でつなぐ
// (Vesktop の設定を調べる側が、4 つのファイルのパスを作るときと同じ関数)。
//
// マーカーのファイル (install/build_mark.h の kBuildMarkFileName) を必ず最後に置き、それ以外は渡された順の
// ままにする (list_directory は名前の順で返す)。途中で止まったときに data/vencord のマーカーが古いまま残るので、
// 次に調べたときに、前のバージョンのファイル (ShownBundleVerdict::StaleOwnBundle) か上書き (Overwritten) と
// して、もう一度見つかる。マーカーが先に新しくなると、一部だけコピーしたファイルを一致 (Matches) と判定する
// おそれがある。マーカーは名前の完全一致で見分ける (書くのは CI だけで、名前は 1 か所にしかない)。
// data/vencord-src にマーカーがない (ローカルでビルドした) ときは、マーカーをコピーしない。
//
// data/vencord にだけあるファイルは消さない (手順はコピーだけ)。Vesktop が読むのは data/vencord-src にある名前
// (4 つのファイルと package.json) なので、余分なファイルが残っても、読まれる中身は data/vencord-src と同じに
// なる。消す手順を足すと、data/vencord に誰かが置いたファイルを、cordmux が消してしまう。
//
// コピーは、呼び出し元が platform::copy_file_replace で 1 つずつ行い、コピーできなかったファイルを
// ledger/observation.h の Install::reinstall_failed_files に入れる。コピーし終えたら、install/vencord_dir.h の
// advance_written_at でバックアップの written_at を進める。
std::vector<CopyStep> plan_reinstall(std::string_view original_dir, std::string_view shown_dir,
				     const std::vector<std::string> &original_file_names);

} // namespace cordmux::install

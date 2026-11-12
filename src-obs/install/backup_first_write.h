// SPDX-License-Identifier: Apache-2.0

// バックアップと state.json を書く手順。起動時の書き直し (pipeline/pipeline.h) と、[Vesktopに追加]
// (dock/dock_poll_install.cpp) の両方が呼ぶ。何を書くかは plan_write (install/vencord_dir.h) が決め、ここが持つのは
// 書く順と、失敗したときの戻し方だけである。ファイルの読み書きは関数で受け取る (テストは偽のファイルを渡す)。
// libobs にも Qt にも依存しない。

#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace cordmux::install {

struct BackupFirstOps {
	std::function<bool(const std::string &, std::string &, std::size_t)> read_file_bytes;
	std::function<bool(const std::string &, const std::string &)> write_file_atomic;
	// なくてもよい。ないときは、書く前にバックアップを読めなかった場合、戻せないものとして扱う。
	std::function<bool(const std::string &)> delete_file;
	// ファイルがないと確認できたときだけ true を返す。読めないだけのファイルや、確認できないときは false。
	// なくてもよい。ないときは、ファイルがあるか分からないものとして扱う。
	std::function<bool(const std::string &)> file_known_absent;
};

enum class BackupFirstOutcome {
	BackupWriteFailed, // バックアップを書けなかった (state.json には触っていない)
	StateWriteFailed,  // バックアップは書けたが state.json を書けなかった
	Wrote,             // 両方書けた
};

struct BackupFirstResult {
	BackupFirstOutcome outcome = BackupFirstOutcome::BackupWriteFailed;
	bool backup_restored = false; // StateWriteFailed のときだけ: バックアップを書く前の状態に戻せたか
};

// バックアップを先に書き、書けたときだけ state.json を書く。順が逆だと、バックアップを書けなかったときに、元の値を
// 持つファイルが 1 つも残らない状態になる。どちらも一時ファイルに書いてから置き換える (write_file_atomic) ので、
// 書けなかったファイルは元のままである。
// state.json を書けなかったら、バックアップを書く前の状態に戻す。書く前の中身を読めていればそのまま書き戻し、
// ファイルがなかったと確認できていれば、書いたバックアップを消す。どちらもできなければ戻せない
// (backup_restored が false)。バックアップだけが進んだまま残ると、バックアップの previous と state.json の現在の値が
// 食い違い、次に書くときに利用者の元の値を取り違える (plan_write の分岐 3 のコメントを参照)。
inline BackupFirstResult write_backup_then_state(const BackupFirstOps &ops, std::size_t max_bytes,
						 const std::string &backup_path, const std::string &new_backup_text,
						 const std::string &state_path, const std::string &new_state_text)
{
	BackupFirstResult out;
	if (!ops.write_file_atomic)
		return out;

	std::string before_text;
	const bool before_read = ops.read_file_bytes && ops.read_file_bytes(backup_path, before_text, max_bytes);
	const bool before_absent = !before_read && ops.file_known_absent && ops.file_known_absent(backup_path);

	if (!ops.write_file_atomic(backup_path, new_backup_text))
		return out;
	if (ops.write_file_atomic(state_path, new_state_text)) {
		out.outcome = BackupFirstOutcome::Wrote;
		return out;
	}

	out.outcome = BackupFirstOutcome::StateWriteFailed;
	if (before_read)
		out.backup_restored = ops.write_file_atomic(backup_path, before_text);
	else if (before_absent)
		out.backup_restored = ops.delete_file && ops.delete_file(backup_path);
	return out;
}

} // namespace cordmux::install

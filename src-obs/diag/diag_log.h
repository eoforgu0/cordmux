// SPDX-License-Identifier: Apache-2.0

// cordmux.dll がログに書く行を、登録された 1 つの出力先に渡す。ws/ を除く cordmux.dll のコードは、ログの行を
// すべて diag::logf で書く (ws/ は diag/ に依存せず、ws::ListenerConfig::log で書く)。blog() を直接呼ぶと、その行は
// トークンとポート番号のチェックを通らず、診断情報の plugin.log にも入らない。そうなると、診断情報を受け取った人は、
// 枠を作れなかった理由などを読めない。
// 出力先は plugin-main.cpp が登録する。blog() を呼び、チェックをし、リングバッファーに積むのはその出力先である
// (トークンを知っているのが plugin-main.cpp だけのため)。このファイルは libobs にも Qt にも依存しない。
// どのスレッドから呼んでもよい。

#pragma once

#include <cstddef>
#include <string_view>

namespace cordmux::diag {

// 行の種類。出来事の行 (Event) と、数秒ごとの統計の行 (Stats) を分けて保つ (診断情報の plugin.log と
// plugin-stats.log)。統計の行は数秒ごとに何行も出るので、同じ保持に入れると、出来事の行が数分で押し出される。
// 出来事の行は、状態が変わったときの行 (接続、切断、メッセージの表示と解消、枠の割り当て、受信のやり直しなど) で、
// 統計の行は、周期の統計 (Stats ...) と、届くたびに書く数の行 (Facts、Profiles、Avatar、合成のソースの絵の用意) である。
enum class LineKind {
	Event,
	Stats,
};

// 組み立てた 1 行を受け取る出力先。level は libobs のログレベル (LOG_INFO、LOG_WARNING など) をそのまま渡す。
// int で受けるのは、このファイルに libobs のヘッダーを持ち込まないためである。
// std::function でなく関数ポインタにするのは、登録の切り替えを std::atomic 1 つで済ませるためである
// (ws/log_sink.h の LogSink は、呼び出し元ごとにコールバックを渡すので std::function にしている)。
using LogSink = void (*)(int level, LineKind kind, std::string_view line);

// 出力先を登録する。nullptr で登録を解除する。登録していない間 (obs_module_load が登録する前、obs_module_unload が
// 解除した後、単体テスト) は、logf に渡した行を捨てる。ほかのスレッドの logf と同時に呼んでよく、その瞬間に
// 組み立てていた行は、前の出力先か新しい出力先のどちらかに届く。
void set_log_sink(LogSink sink);

// いま登録している出力先。登録していなければ nullptr。
LogSink log_sink();

// 1 行を組み立てて出力先に渡す。書式は std::vsnprintf と同じ。level は libobs のログレベルである。
// LOG_DEBUG の行は、OBS を --verbose で起動したときだけ OBS のログのファイルに書かれる (OBS 31.1.1 の
// frontend/obs-main.cpp の do_log)。診断情報の plugin.log には、ほかのレベルの行と同じく入る。--verbose なしの
// OBS のログに残したい行は LOG_INFO 以上にする。
// kMaxLineBytes に収まらない分は捨てる。捨てたことを示す文字は行に付けない。付けると、付けた文字と本文を区別
// できなくなる。
void logf(int level, const char *format, ...);

// 統計の行 (LineKind::Stats) を書く。ほかは logf と同じ。logf と同じ名前にするのは、test/lint/log_lines.py が
// logf の呼び出しの文字列を読んで、OBS のログの行の形を確認するためである。
void logf(LineKind kind, int level, const char *format, ...);

// 1 行を組み立てるバッファーの大きさ (バイト。終端の NUL を含む)。OBS 31.1.1 の do_log は 1 行を 8192 バイトの
// バッファーに組み立てるので、先頭に [cordmux] を付けても OBS の側では切れない。
constexpr std::size_t kMaxLineBytes = 4096;

} // namespace cordmux::diag

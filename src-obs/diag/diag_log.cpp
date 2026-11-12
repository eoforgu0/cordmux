// SPDX-License-Identifier: Apache-2.0

// diag/diag_log.h の実装。

#include "diag/diag_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace cordmux::diag {

namespace {

// 登録している出力先。関数ポインタなので、std::atomic 1 つで切り替えられる。
std::atomic<LogSink> g_sink{nullptr};

} // namespace

void set_log_sink(LogSink sink)
{
	g_sink.store(sink, std::memory_order_release);
}

LogSink log_sink()
{
	return g_sink.load(std::memory_order_acquire);
}

namespace {

void vlogf(LineKind kind, int level, const char *format, va_list args)
{
	const LogSink sink = g_sink.load(std::memory_order_acquire);
	if (sink == nullptr || format == nullptr)
		return; // 出力先がない間は、行を組み立てない

	char buffer[kMaxLineBytes];
	const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
	if (written < 0)
		return; // 書式が不正で、行を作れない

	// vsnprintf は収まらない分を捨てて、終端の NUL を必ず書く。written は切らなかった場合の長さなので、
	// 実際の長さはバッファーの大きさで抑える。
	std::size_t length = static_cast<std::size_t>(written);
	if (length >= sizeof(buffer))
		length = sizeof(buffer) - 1;

	sink(level, kind, std::string_view(buffer, length));
}

} // namespace

void logf(int level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vlogf(LineKind::Event, level, format, args);
	va_end(args);
}

void logf(LineKind kind, int level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vlogf(kind, level, format, args);
	va_end(args);
}

} // namespace cordmux::diag

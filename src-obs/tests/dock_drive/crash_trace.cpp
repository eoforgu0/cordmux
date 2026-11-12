// SPDX-License-Identifier: Apache-2.0

// crash_trace.h の実装。Windows でだけ動き、ほかの OS では何もしない。

#include "crash_trace.h"

#ifdef _WIN32

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <cstdio>
#include <cstring>

namespace cordmux::dock_drive {

namespace {

// 記録のリングバッファー。1 つのスレッドからだけ書き、クラッシュしたときに読むだけなので、ロックを取らない。
constexpr int kNotes = 512;
constexpr int kNoteLen = 160;
char g_notes[kNotes][kNoteLen] = {};
int g_next_note = 0;
long long g_note_count = 0;

void print_notes()
{
	std::fprintf(stderr, "dock_drive: last notes (old -> new):\n");
	const int n = g_note_count < kNotes ? static_cast<int>(g_note_count) : kNotes;
	for (int i = 0; i < n; ++i) {
		const int at = (g_next_note - n + i + kNotes) % kNotes;
		std::fprintf(stderr, "dock_drive:   . %s\n", g_notes[at]);
	}
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS *info)
{
	HANDLE process = GetCurrentProcess();
	HANDLE thread = GetCurrentThread();
	std::fprintf(stderr, "dock_drive: crashed: code=0x%08lx address=%p\n",
		     static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), info->ExceptionRecord->ExceptionAddress);
	if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
		std::fprintf(stderr, "dock_drive: access violation: %s at %p\n",
			     info->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : "write",
			     reinterpret_cast<void *>(info->ExceptionRecord->ExceptionInformation[1]));

	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	SymInitialize(process, nullptr, TRUE);

	CONTEXT context = *info->ContextRecord;
	STACKFRAME64 frame = {};
	frame.AddrPC.Offset = context.Rip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = context.Rbp;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Offset = context.Rsp;
	frame.AddrStack.Mode = AddrModeFlat;

	alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
	auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
	for (int depth = 0; depth < 48; ++depth) {
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
				 SymGetModuleBase64, nullptr))
			break;
		const DWORD64 pc = frame.AddrPC.Offset;
		if (pc == 0)
			break;
		char module_name[MAX_PATH] = "?";
		const DWORD64 module_base = SymGetModuleBase64(process, pc);
		if (module_base != 0)
			GetModuleFileNameA(reinterpret_cast<HMODULE>(module_base), module_name, MAX_PATH);
		const char *short_module = module_name;
		for (const char *p = module_name; *p != '\0'; ++p) {
			if (*p == '\\' || *p == '/')
				short_module = p + 1;
		}
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 511;
		DWORD64 displacement = 0;
		const bool named = SymFromAddr(process, pc, &displacement, symbol) != FALSE;
		IMAGEHLP_LINE64 line = {};
		line.SizeOfStruct = sizeof(line);
		DWORD line_displacement = 0;
		const bool lined = SymGetLineFromAddr64(process, pc, &line_displacement, &line) != FALSE;
		std::fprintf(stderr, "dock_drive:   #%02d %s!%s+0x%llx", depth, short_module, named ? symbol->Name : "?",
			     static_cast<unsigned long long>(named ? displacement : pc - module_base));
		if (lined)
			std::fprintf(stderr, " (%s:%lu)", line.FileName, static_cast<unsigned long>(line.LineNumber));
		std::fprintf(stderr, "\n");
	}
	print_notes();
	std::fflush(stderr);
	std::fflush(stdout);
	return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void install_crash_trace()
{
	SetUnhandledExceptionFilter(&on_unhandled);
}

void crash_note(const char *text)
{
	char *slot = g_notes[g_next_note];
	strncpy_s(slot, kNoteLen, text, _TRUNCATE);
	g_next_note = (g_next_note + 1) % kNotes;
	++g_note_count;
}

} // namespace cordmux::dock_drive

#else

namespace cordmux::dock_drive {

void install_crash_trace() {}
void crash_note(const char *) {}

} // namespace cordmux::dock_drive

#endif

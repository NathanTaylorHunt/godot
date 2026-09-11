/**************************************************************************/
/*  crash_handler_windows_minidump.cpp                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// A process-wide crash minidump, present in release templates too.
//
// The SEH backtrace in crash_handler_windows_seh.cpp is compiled only under
// DEBUG_ENABLED, and even there it is a __try around main(), so a crash on the
// rendering or on a worker thread escapes it. An unhandled-exception filter is
// per-process and per-thread-agnostic, so it is the only layer that sees those.
// It deliberately performs no engine work: the directory is resolved while the
// process is healthy and the crash path touches Win32 alone.

#include "crash_handler_windows.h"

#include "core/os/os.h"
#include "core/string/ustring.h"

#include <dbghelp.h>
#include <stdio.h>

namespace {

// Resolved at startup by crash_handler_windows_set_dump_directory(), so the
// filter never asks ProjectSettings for anything on a crashing process.
WCHAR dump_directory[MAX_PATH] = { 0 };

LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
LONG filter_entered = 0;

// Retention scans and the dump path are static, not stack, buffers: the
// faulting thread's stack is the one resource a crash is most likely to have
// exhausted.
constexpr int DUMPS_KEPT = 3;
constexpr int RETENTION_SCAN_MAX = 64;
WCHAR retention_names[RETENTION_SCAN_MAX][MAX_PATH];
WCHAR dump_path[MAX_PATH];
WCHAR summary_path[MAX_PATH];
WCHAR retention_path[MAX_PATH];

// Thread stacks, module list, unloaded modules and handles, plus the address
// space map and the memory the stacks point at. Full memory is deliberately
// not requested: it turns a 100 MB dump into a multi-gigabyte one without
// making the faulting stack any more readable.
constexpr MINIDUMP_TYPE DUMP_TYPE = (MINIDUMP_TYPE)(
		MiniDumpWithThreadInfo |
		MiniDumpWithUnloadedModules |
		MiniDumpWithHandleData |
		MiniDumpWithFullMemoryInfo |
		MiniDumpWithProcessThreadData |
		MiniDumpWithIndirectlyReferencedMemory |
		MiniDumpWithDataSegs);

void make_directory_tree(const WCHAR *p_path) {
	WCHAR partial[MAX_PATH];
	size_t length = wcsnlen(p_path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		return;
	}
	wcsncpy_s(partial, MAX_PATH, p_path, length);
	for (size_t i = 0; i < length; i++) {
		if (i > 0 && (partial[i] == L'\\' || partial[i] == L'/')) {
			WCHAR separator = partial[i];
			partial[i] = 0;
			CreateDirectoryW(partial, nullptr);
			partial[i] = separator;
		}
	}
	CreateDirectoryW(partial, nullptr);
}

// Names are "crash-<sortable timestamp>-pid<n>.dmp", so lexical order is
// chronological order and retention needs no file times.
void prune_old_dumps(int p_keep) {
	if (dump_directory[0] == 0) {
		return;
	}
	if (_snwprintf_s(retention_path, MAX_PATH, _TRUNCATE, L"%s\\crash-*.dmp", dump_directory) < 0) {
		return;
	}

	WIN32_FIND_DATAW found;
	HANDLE search = FindFirstFileW(retention_path, &found);
	if (search == INVALID_HANDLE_VALUE) {
		return;
	}
	int count = 0;
	do {
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			continue;
		}
		if (count < RETENTION_SCAN_MAX) {
			wcsncpy_s(retention_names[count], MAX_PATH, found.cFileName, _TRUNCATE);
			count++;
		}
	} while (FindNextFileW(search, &found));
	FindClose(search);

	// Insertion sort, oldest first. The list is at most RETENTION_SCAN_MAX long
	// and this runs on a dying process, so the simplest correct sort is right.
	for (int i = 1; i < count; i++) {
		WCHAR pivot[MAX_PATH];
		wcsncpy_s(pivot, MAX_PATH, retention_names[i], _TRUNCATE);
		int j = i - 1;
		while (j >= 0 && wcscmp(retention_names[j], pivot) > 0) {
			wcsncpy_s(retention_names[j + 1], MAX_PATH, retention_names[j], _TRUNCATE);
			j--;
		}
		wcsncpy_s(retention_names[j + 1], MAX_PATH, pivot, _TRUNCATE);
	}

	for (int i = 0; i < count - p_keep; i++) {
		if (_snwprintf_s(retention_path, MAX_PATH, _TRUNCATE, L"%s\\%s", dump_directory, retention_names[i]) < 0) {
			continue;
		}
		DeleteFileW(retention_path);
		if (_snwprintf_s(retention_path, MAX_PATH, _TRUNCATE, L"%s\\%s.txt", dump_directory, retention_names[i]) >= 0) {
			DeleteFileW(retention_path);
		}
	}
}

// A few lines anyone can read without a debugger, written before the dump is
// attempted so that a failed MiniDumpWriteDump still leaves the exception code
// and the reason it failed.
void write_summary(const WCHAR *p_path, EXCEPTION_POINTERS *p_exception, DWORD p_dump_error) {
	HANDLE file = CreateFileW(p_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	char text[512];
	const EXCEPTION_RECORD *record = p_exception ? p_exception->ExceptionRecord : nullptr;
	int length = _snprintf_s(text, sizeof(text), _TRUNCATE,
			"process %lu crashed\r\n"
			"thread %lu\r\n"
			"exception 0x%08lX at 0x%p\r\n"
			"minidump %s\r\n",
			GetCurrentProcessId(), GetCurrentThreadId(),
			record ? record->ExceptionCode : 0,
			record ? record->ExceptionAddress : nullptr,
			p_dump_error == 0 ? "written" : "failed");
	if (length > 0) {
		DWORD unused = 0;
		WriteFile(file, text, (DWORD)length, &unused, nullptr);
	}
	CloseHandle(file);
}

bool write_minidump(EXCEPTION_POINTERS *p_exception) {
	if (dump_directory[0] == 0) {
		return false;
	}

	SYSTEMTIME now;
	GetLocalTime(&now);
	if (_snwprintf_s(dump_path, MAX_PATH, _TRUNCATE,
				L"%s\\crash-%04d%02d%02d-%02d%02d%02d-pid%lu.dmp",
				dump_directory, now.wYear, now.wMonth, now.wDay,
				now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId()) < 0) {
		return false;
	}

	if (_snwprintf_s(summary_path, MAX_PATH, _TRUNCATE, L"%s.txt", dump_path) < 0) {
		summary_path[0] = 0;
	} else {
		write_summary(summary_path, p_exception, ERROR_IO_PENDING);
	}

	HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}

	MINIDUMP_EXCEPTION_INFORMATION exception_information;
	exception_information.ThreadId = GetCurrentThreadId();
	exception_information.ExceptionPointers = p_exception;
	exception_information.ClientPointers = FALSE;

	BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
			DUMP_TYPE, p_exception ? &exception_information : nullptr, nullptr, nullptr);
	CloseHandle(file);

	if (!written) {
		DWORD error = GetLastError();
		DeleteFileW(dump_path);
		if (summary_path[0] != 0) {
			write_summary(summary_path, p_exception, error == 0 ? ERROR_INVALID_FUNCTION : error);
		}
		return false;
	}
	if (summary_path[0] != 0) {
		write_summary(summary_path, p_exception, 0);
	}

	// Keeping the dump just written, so the fourth crash evicts the first.
	prune_old_dumps(DUMPS_KEPT);

	// stderr, not print_error: the engine's logger may be exactly what is broken.
	fwprintf(stderr, L"Godot: crash minidump written to %s\n", dump_path);
	fflush(stderr);
	return true;
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *p_exception) {
	// A second crash while writing the first dump must not recurse.
	if (InterlockedCompareExchange(&filter_entered, 1, 0) == 0) {
		OS *os = OS::get_singleton();
		if (!(os && os->is_disable_crash_handler()) && !IsDebuggerPresent()) {
			write_minidump(p_exception);
		}
	}

	// Whoever was installed before us decides how the process dies: the
	// headless/no-dialogs filter exits quietly, and with no earlier filter
	// Windows Error Reporting still gets its turn.
	if (previous_filter) {
		return previous_filter(p_exception);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

// Called once at OS::initialize() and again once the main loop starts. The
// second call matters: the .NET runtime installs its own top-level filter while
// it loads, which would otherwise displace this one for the rest of the
// process. Re-arming puts this handler back in front and keeps the runtime's as
// the one it defers to.
void crash_handler_windows_install_unhandled_filter() {
	LPTOP_LEVEL_EXCEPTION_FILTER prior = SetUnhandledExceptionFilter(&unhandled_exception_filter);
	if (prior != &unhandled_exception_filter) {
		previous_filter = prior;
	}
}

void crash_handler_windows_set_dump_directory(const String &p_directory) {
	if (p_directory.is_empty()) {
		return;
	}
	const String native = p_directory.replace_char('/', '\\');
	const Char16String utf16 = native.utf16();
	if (utf16.length() <= 0 || utf16.length() >= MAX_PATH) {
		return;
	}
	wcsncpy_s(dump_directory, MAX_PATH, (const WCHAR *)utf16.get_data(), _TRUNCATE);
	make_directory_tree(dump_directory);
}

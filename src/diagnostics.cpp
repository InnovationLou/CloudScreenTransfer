/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "diagnostics.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <exception>
#include <iterator>

namespace cst::diagnostics {
namespace {
wchar_t logPath[32768]{};
thread_local const char *currentStage = "process_start";

HANDLE openLog() noexcept {
    return logPath[0] ? CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                      : INVALID_HANDLE_VALUE;
}
LONG WINAPI unhandled(EXCEPTION_POINTERS *pointers) {
    char line[512]{};
    auto *record = pointers ? pointers->ExceptionRecord : nullptr;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "unhandled_exception code=0x%08lX address=%p stage=%s\r\n",
                record ? record->ExceptionCode : 0, record ? record->ExceptionAddress : nullptr, stage());
    append(line);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void stage(const char *name) noexcept {
    currentStage = name ? name : "unknown";
}
const char *stage() noexcept {
    return currentStage;
}

void append(const char *message) noexcept {
    HANDLE file = openLog();
    if (file == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char line[4608]{};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu thread=%lu %s",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                time.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), message ? message : "");
    DWORD written = 0;
    // Append a complete record in one write, so concurrent worker failures
    // cannot separate one record's prefix from its exception details.
    WriteFile(file, line, DWORD(std::strlen(line)), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void exception(const char *type, const char *message) noexcept {
    char line[4096]{};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "cpp_exception stage=%s type=%s message=%s\r\n", stage(),
                type ? type : "unknown", message ? message : "unknown");
    append(line);
}

void install() noexcept {
    DWORD length = GetModuleFileNameW(nullptr, logPath, DWORD(std::size(logPath)));
    if (length && length < std::size(logPath)) {
        auto *separator = std::wcsrchr(logPath, L'\\');
        if (separator)
            wcscpy_s(separator + 1, std::size(logPath) - size_t(separator + 1 - logPath),
                     L"CloudScreenTransfer.log");
        else
            logPath[0] = L'\0';
    } else
        logPath[0] = L'\0';
    HANDLE probe = openLog();
    if (probe == INVALID_HANDLE_VALUE) {
        DWORD size = GetTempPathW(DWORD(std::size(logPath)), logPath);
        if (size && size < std::size(logPath))
            swprintf_s(logPath + size, std::size(logPath) - size, L"CloudScreenTransfer-%lu.log",
                       GetCurrentProcessId());
        else
            logPath[0] = L'\0';
    } else
        CloseHandle(probe);
    SetUnhandledExceptionFilter(unhandled);
    std::set_terminate([] {
        append("std::terminate reached\r\n");
        std::abort();
    });
    append("CloudScreenTransfer 0.2.0 process_start\r\n");
}
} // namespace cst::diagnostics

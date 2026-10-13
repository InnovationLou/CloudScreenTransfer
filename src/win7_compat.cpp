/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */

#if !defined(_MSC_VER) || !defined(_M_X64)
#error WIN7_COMPAT supports MSVC x64 builds only.
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Current MSVC static libraries (including prebuilt dependencies) call this
// Win8 API through its import pointer. Resolve that pointer in the executable
// before Kernel32.lib can add an unavailable entry to the PE import table.
// Both APIs have the same signature and UTC epoch. The Win7 API has lower
// wall-clock precision; frame pacing still uses steady_clock / QPC.
namespace {
void WINAPI get_system_time_win7(LPFILETIME time) {
    GetSystemTimeAsFileTime(time);
}
} // namespace

// Use the address of a local function: taking the address of a dllimport
// function directly needs dynamic initialization in MSVC. This pointer must
// already be valid before any CRT or application global constructors run.
extern "C" {
void(WINAPI *__imp_GetSystemTimePreciseAsFileTime)(LPFILETIME) = &get_system_time_win7;
}

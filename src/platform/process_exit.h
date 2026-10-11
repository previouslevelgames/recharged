#pragma once

#include <cstdlib>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned int exit_code);
#endif

namespace mscharged::platform
{
// Ends the process at once, without static destructors or atexit handlers.
// Surviving original game statics (the StringBlock allocator among them) must
// not run against already retired game arenas. On Windows std::_Exit is not
// enough: it still calls ExitProcess, which sends DLL_PROCESS_DETACH to the
// game module and so runs its static destructors. Flush logs first.
[[noreturn]] inline void TerminateProcessNow(int code)
{
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), static_cast<unsigned int>(code));
#endif
    std::_Exit(code);
}
}

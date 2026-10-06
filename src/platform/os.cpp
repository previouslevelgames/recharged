#include <dolphin/os.h>
#ifdef AURORA_ENABLE_GX
#include <dolphin/gx/GXManage.h>
#endif
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <thread>

// Only interfaces with a verified equivalent are forwarded to Aurora.
extern "C" void* OSGetMEM1ArenaLo() { return OSGetArenaLo(); }
extern "C" void* OSGetMEM1ArenaHi() { return OSGetArenaHi(); }
extern "C" void* OSAllocFromMEM1ArenaLo(std::uint32_t size, std::uint32_t alignment)
{ return OSAllocFromArenaLo(size, alignment); }
extern "C" std::uint32_t ChargedGetBusClock() { return __OSBusClock; }
extern "C" void OSYieldThread()
{
#ifdef AURORA_ENABLE_GX
    // Workers latch genuine PE completion; only the unmasked GX owner delivers.
    AuroraServiceGXDrawDone();
#endif
    std::this_thread::yield();
}

// Aurora leaves these reports to the host application.
extern "C" void OSVReport(const char* message, va_list arguments)
{ std::vfprintf(stderr, message, arguments); }
extern "C" void OSReport(const char* message, ...)
{
    va_list arguments;
    va_start(arguments, message);
    OSVReport(message, arguments);
    va_end(arguments);
}

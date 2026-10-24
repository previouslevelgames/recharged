#include "platform/rollback_platform.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#if !defined(__APPLE__)
#include <link.h>
#endif
#endif

#include <algorithm>

#if defined(_WIN32)
// The bounds of the original game's writable data on PE, where no linker script
// gathers it: tools/rollback_sections.py renames the sections of its objects to
// .rbkd$m and .rbkb$m, and the linker puts the "$a" and "$z" sections, these,
// before and after them (cmake/rollback_sections.ld on ELF).
#pragma clang section data = ".rbkd$a" bss = ".rbkb$a"
extern "C"
{
char __rbk_data_start[1] = {1};
char __rbk_bss_start[1];
}
#pragma clang section data = ".rbkd$z" bss = ".rbkb$z"
extern "C"
{
char __rbk_data_end[1] = {1};
char __rbk_bss_end[1];
}
#pragma clang section data = "" bss = ""
#endif

namespace mscharged::platform
{
#if defined(_WIN32)
int RollbackCallStack(void** frames, int capacity)
{
    return RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::max(capacity, 0)), frames, nullptr);
}

std::uintptr_t RollbackModuleBase(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module))
        return 0;
    return reinterpret_cast<std::uintptr_t>(module);
}

int RollbackWritableSegments(std::uintptr_t moduleBase, std::uintptr_t (*segments)[2], int capacity)
{
    if (!moduleBase) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    int count = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && count < capacity; ++i, ++section)
    {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const std::uintptr_t start = moduleBase + section->VirtualAddress;
        segments[count][0] = start;
        segments[count][1] = start + section->Misc.VirtualSize;
        ++count;
    }
    return count;
}

void* RollbackMapPages(std::size_t bytes)
{
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void RollbackUnmapPages(void* pages, std::size_t)
{
    if (pages) VirtualFree(pages, 0, MEM_RELEASE);
}

namespace
{
DWORD WINAPI RunThread(LPVOID body)
{
    reinterpret_cast<void (*)()>(body)();
    return 0;
}
} // namespace

bool RollbackStartThread(void (*body)())
{
    HANDLE thread = CreateThread(nullptr, 0, RunThread, reinterpret_cast<LPVOID>(body), 0, nullptr);
    if (!thread) return false;
    CloseHandle(thread);
    return true;
}

void RollbackSleepMs(unsigned milliseconds)
{
    Sleep(milliseconds);
}
#else
int RollbackCallStack(void** frames, int capacity)
{
    void* all[130];
    const int depth = backtrace(all, std::min(capacity + 1, 130));
    const int count = std::max(depth - 1, 0); // without this function's own
    std::copy(all + 1, all + 1 + count, frames);
    return count;
}

std::uintptr_t RollbackModuleBase(const void* address)
{
    Dl_info info{};
    if (!address || !dladdr(address, &info)) return 0;
    return reinterpret_cast<std::uintptr_t>(info.dli_fbase);
}

#if defined(__APPLE__)
// Not yet (the module globals' comparison of RECHARGED_ROLLBACK_TEST_COMPARE).
int RollbackWritableSegments(std::uintptr_t, std::uintptr_t (*)[2], int)
{
    return 0;
}
#else
namespace
{
struct SegmentSearch
{
    std::uintptr_t base;
    std::uintptr_t (*segments)[2];
    int capacity, count;
};

int AddWritableSegments(dl_phdr_info* info, std::size_t, void* data)
{
    auto& search = *static_cast<SegmentSearch*>(data);
    if (info->dlpi_addr != search.base) return 0;
    for (int i = 0; i < info->dlpi_phnum && search.count < search.capacity; ++i)
    {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || !(header.p_flags & PF_W)) continue;
        const std::uintptr_t start = info->dlpi_addr + header.p_vaddr;
        search.segments[search.count][0] = start;
        search.segments[search.count][1] = start + header.p_memsz;
        ++search.count;
    }
    return 1;
}
} // namespace

int RollbackWritableSegments(std::uintptr_t moduleBase, std::uintptr_t (*segments)[2], int capacity)
{
    SegmentSearch search{moduleBase, segments, capacity, 0};
    dl_iterate_phdr(AddWritableSegments, &search);
    return search.count;
}
#endif

void* RollbackMapPages(std::size_t bytes)
{
    void* pages = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return pages == MAP_FAILED ? nullptr : pages;
}

void RollbackUnmapPages(void* pages, std::size_t bytes)
{
    if (pages) munmap(pages, bytes);
}

namespace
{
void* RunThread(void* body)
{
    reinterpret_cast<void (*)()>(body)();
    return nullptr;
}
} // namespace

// A pthread: std::thread would allocate its state with the game's operator new.
bool RollbackStartThread(void (*body)())
{
    pthread_t thread;
    if (pthread_create(&thread, nullptr, RunThread, reinterpret_cast<void*>(body)) != 0) return false;
    pthread_detach(thread);
    return true;
}

void RollbackSleepMs(unsigned milliseconds)
{
    timespec pause{static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1'000'000};
    nanosleep(&pause, nullptr);
}
#endif
} // namespace mscharged::platform

#if defined(MSCHARGED_GAME_MODULE)
#error Native HBM memory/debug services use the host CRT, not game operators
#endif

#include "platform/hbm_debug_abi.h"
#include "platform/thread.h"
#include <dolphin/os.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <unwind.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <link.h>
#endif
#endif

namespace {
bool Contains(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t point) {
    return begin && end > begin && point >= begin && point < end;
}

bool InLiveSDKBank(std::uintptr_t address) {
    // Read current owner facts, not cached permissions. Native SDK shutdown
    // clears the actual base/arena pointers after its accesses are drained.
    // Arena high-water marks can survive the memory retirement itself. A cold
    // or retired SDK therefore uses the fresh host mapping query below.
    if (!OSBaseAddress) return false;
    if (Contains(OSBaseAddress, reinterpret_cast<std::uintptr_t>(OSGetArenaHi()), address))
        return true;
    const auto mem2_end = reinterpret_cast<std::uintptr_t>(OSGetMEM2ArenaHi());
    if (mem2_end) {
        const auto mem2_begin = reinterpret_cast<std::uintptr_t>(OSPhysicalToCached(0x10000000));
        if (Contains(mem2_begin, mem2_end, address)) return true;
    }
    return false;
}

bool InActiveCurrentStack(std::uintptr_t address) {
    // Bounds belong to this native thread and expire with it. Never accept the
    // uncommitted reserve below the actual active frame. An alternate stack
    // outside these bounds uses the current OS mapping query instead. As for
    // the live SDK banks, the caller must retain this thread's active readable
    // stack; these bounds never grant permission to alter or retire it.
    const auto marker = reinterpret_cast<std::uintptr_t>(&address);
    static thread_local const auto limits = mscharged::CurrentThreadStackLimits();
    return marker >= limits.low && marker < limits.high &&
        address >= marker && address < limits.high;
}

bool ReadableMapping(std::uintptr_t address) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info))
        return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (info.RegionSize > std::numeric_limits<std::uintptr_t>::max() - begin ||
        !Contains(begin, begin + info.RegionSize, address) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto access = info.Protect & 0xff;
    return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
        access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
#elif defined(__APPLE__)
    mach_vm_address_t begin = address;
    mach_vm_size_t size{};
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto result = mach_vm_region(mach_task_self(), &begin, &size,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return result == KERN_SUCCESS && (info.protection & VM_PROT_READ) != 0 &&
        address >= begin && address - begin < size;
#elif defined(__linux__)
    // A fresh mapping query is needed for unowned CRT/foreign regions and for
    // permission/unmap changes. No positive page result is cached.
    FILE* file = std::fopen("/proc/self/maps", "r");
    if (!file) throw std::runtime_error("Native HBM address mapping query is unavailable");
    bool found = false;
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        std::uintptr_t begin{}, end{};
        char permissions[5]{};
        if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &begin, &end, permissions) == 3 &&
            Contains(begin, end, address)) {
            found = permissions[0] == 'r';
            break;
        }
    }
    const bool error = std::ferror(file) != 0;
    std::fclose(file);
    if (error) throw std::runtime_error("Native HBM address mapping query failed");
    return found;
#else
    throw std::runtime_error("Native HBM address mapping query is unsupported on this platform");
#endif
}

#if !defined(_WIN32)
struct StackWalk {
    std::uintptr_t caller;
    ChargedNativeHBMFrame* frames;
    std::size_t capacity, count{};
};
_Unwind_Reason_Code VisitFrame(_Unwind_Context* context, void* argument) {
    auto& walk = *static_cast<StackWalk*>(argument);
    const auto stack = static_cast<std::uintptr_t>(_Unwind_GetCFA(context));
    const auto instruction = static_cast<std::uintptr_t>(_Unwind_GetIP(context));
    // A zero return address belongs to the native terminator. Let the unwinder
    // finish naturally; returning a stop reason here is reported as an error
    // by libgcc's _Unwind_Backtrace even though no invalid frame was published.
    if (!instruction) return _URC_NO_REASON;
    if (stack < walk.caller) return _URC_NO_REASON;
    walk.frames[walk.count++] = {stack, instruction};
    return walk.count == walk.capacity ? _URC_END_OF_STACK : _URC_NO_REASON;
}
#endif
}

extern "C" bool ChargedNativeHBMPointerValid(const void* pointer) {
    if (!pointer) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (InLiveSDKBank(address) || InActiveCurrentStack(address)) return true;
    return ReadableMapping(address);
}

extern "C" std::uint32_t ChargedNativeHBMMapCursorWord(const void* cursor) {
    const auto value = reinterpret_cast<std::uintptr_t>(cursor);
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::out_of_range("Native HBM DVD map cursor is not an original Wii32 encoded value");
    return static_cast<std::uint32_t>(value);
}

extern "C" std::size_t ChargedNativeHBMCaptureStack(std::uintptr_t caller_frame,
    ChargedNativeHBMFrame* frames, std::size_t capacity) {
    if (!caller_frame || !frames || !capacity)
        throw std::invalid_argument("Native HBM stack capture requires its actual frame and storage");
#if defined(_WIN32)
    // Windows unwind lowering is separate; never publish a fabricated PPC chain.
    throw std::runtime_error("Native HBM stack unwind is unqualified on Windows");
#else
    StackWalk walk{caller_frame, frames, capacity};
    const auto result = _Unwind_Backtrace(VisitFrame, &walk);
    if (walk.count != capacity && result != _URC_END_OF_STACK)
        throw std::runtime_error("Native HBM stack unwind failed");
    return walk.count;
#endif
}

extern "C" const char* ChargedNativeHBMStackSymbol(std::uintptr_t instruction_address) {
#if defined(_WIN32)
    return nullptr;
#else
    Dl_info info{};
    return dladdr(reinterpret_cast<const void*>(instruction_address), &info) ? info.dli_sname : nullptr;
#endif
}

extern "C" void ChargedNativeHBMHalt(void) {
    // Like the already qualified native OSPanic hardware lowering, a genuine
    // source CPU halt cannot return to original game execution.
    std::fflush(nullptr);
    std::abort();
}

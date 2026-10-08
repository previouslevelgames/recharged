#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0602
#endif
#include "platform/native_ode_allocation_size.h"
#include "platform/string_format.h"
#include "NL/nlFile.h"
#include "NL/nlBundleFile.h"
#include "Game/FE/feAsyncImage.h"
#include "Game/Physics/Physics.h"
#include "ode/memory.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
unsigned checks;
void Check(bool valid, const char* message) {
    ++checks;
    if (!valid) throw std::runtime_error(message);
}
// Exercise the genuine native consumer of nlSNPrintf's va_list. This fixture
// generates native variadic arguments; it does not implement game formatting.
int Consume(char* output, std::size_t count, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = mscharged::FormatString(output, count, format, args);
    va_end(args);
    return result;
}
}

int main() {
    try {
        static_assert(std::is_same_v<decltype(&AsyncImage::BundleOpenComplete), FileOpenAsyncCallback>);
        static_assert(std::is_same_v<decltype(&AsyncImage::TextureLoadComplete), FileReadAsyncCallback>);
        static_assert(std::is_same_v<decltype(&ODEAlloc), dAllocFunction*>);
        static_assert(std::is_same_v<decltype(&ODERealloc), dReallocFunction*>);
        static_assert(std::is_same_v<decltype(&ODEFree), dFreeFunction*>);
        static_assert(sizeof(BundleAsyncParam) == sizeof(void*));
        static_assert(sizeof(nlFileAsyncParam) == sizeof(void*));
        static_assert(sizeof(AsyncImage::mTextureHandle) == sizeof(unsigned long));
        static_assert(sizeof(AsyncImage::mTextureSize) == sizeof(unsigned long));
#if defined(_WIN32)
        static_assert(sizeof(unsigned long) == 4 && sizeof(void*) == 8);
#endif
        constexpr std::size_t bytes = 4096;
#if defined(_WIN32)
        void* memory = VirtualAlloc(reinterpret_cast<void*>(std::uintptr_t{0x200050000ull}),
                                    bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Check(memory != nullptr, "Actual high Win64 caller storage allocation failed");
#else
        void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(memory != MAP_FAILED, "Actual caller storage mapping failed");
#endif
        Check(reinterpret_cast<std::uintptr_t>(memory) > UINT32_MAX,
              "Variadic fixture did not exercise a genuine high caller pointer");
        char* name = static_cast<char*>(memory);
        std::strcpy(name, "BANK-NAME");
        char output[128];
        unsigned long value = 123;
        // The exact donor mixed GP-word convention: string pointer when present,
        // numeric value otherwise. Actual consumer is the existing host formatter.
        std::uintptr_t first = name != nullptr ? reinterpret_cast<std::uintptr_t>(name) : value;
        Check(Consume(output, sizeof(output), "%-25s%dk\n", first, value) == 30,
              "Actual native variadic string/number format length changed");
        Check(std::strcmp(output, "BANK-NAME                123k\n") == 0,
              "High native string pointer or second original numeric value was lost");
        const char* absent = nullptr;
        first = absent != nullptr ? reinterpret_cast<std::uintptr_t>(absent) : value;
        Check(Consume(output, sizeof(output), "<free> %dk\n", first, value) == 12,
              "Actual native numeric variadic format length changed");
        Check(std::strcmp(output, "<free> 123k\n") == 0,
              "Original null-name numeric branch changed its output");
        value = 0xffffffffUL;
        first = absent != nullptr ? reinterpret_cast<std::uintptr_t>(absent) : value;
        Consume(output, sizeof(output), "%dk\n", first, value);
        Check(std::strcmp(output, "-1k\n") == 0,
              "Original 32-bit signed-format interpretation changed");
        const auto sourceMax = std::numeric_limits<unsigned long>::max();
        Check(mscharged::platform::NativeODEAllocationSize(0) == 0,
              "Native zero-size source operand changed");
        Check(mscharged::platform::NativeODEAllocationSize(65536) == 65536,
              "Native representable source allocation operand changed");
        Check(mscharged::platform::NativeODEAllocationSize(sourceMax) == sourceMax,
              "Native source-carrier maximum changed");
        if constexpr (sizeof(std::size_t) > sizeof(unsigned long)) {
            bool refused = false;
            try {
                (void)mscharged::platform::NativeODEAllocationSize(static_cast<std::size_t>(sourceMax) + 1);
            } catch (const std::length_error&) { refused = true; }
            Check(refused, "Unrepresentable LLP64 source size was silently narrowed");
        }
#if defined(_WIN32)
        Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "Actual caller storage release failed");
#else
        Check(munmap(memory, bytes) == 0, "Actual caller storage release failed");
#endif
        std::printf("Native callback/signature/variadic/size ABI PASS %u checks; actual formatter/high caller storage; no game owners or I/O requests.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native shared ABI failure: %s\n", error.what());
        return 1;
    }
}

#include <revolution/types.h>
#include <revolution/os/OSTime_fwd.h>
#include <revolution/os/OS_fwd.h>
#include "NL/nlChunk.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>

static_assert(sizeof(s32) == 4 && sizeof(u32) == 4);
static_assert(sizeof(OSTick) == 4 && sizeof(OSTime) == 8);
static_assert(sizeof(decltype(OSGetConsoleType())) == 4);
static_assert(sizeof(OSCalendarTime) == 40);
static_assert(sizeof(uintptr_t) == sizeof(void*));
static_assert(sizeof(nlChunk) == 8); // Serialized chunk header must not expand.

int main()
{
    auto* allocation = static_cast<unsigned char*>(std::malloc(512));
    if (!allocation) return 1;
    const auto address = (reinterpret_cast<std::uintptr_t>(allocation) + 3) & ~std::uintptr_t(3);
    auto* chunk = new (reinterpret_cast<void*>(address)) nlChunk{};
    auto require = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; std::free(allocation); std::exit(1); }
    };
    chunk->m_ID = 0x06000000; // 64-byte payload alignment.
    chunk->m_Size = 120;
    const auto payload = address + sizeof(nlChunk);
    const auto aligned = (payload + 63) & ~std::uintptr_t(63);
    require(reinterpret_cast<std::uintptr_t>(chunk->GetAlignedData()) == aligned,
            "Chunk payload alignment truncated its native address");
    chunk->m_ID = 0;
    require(chunk->GetAlignedData() == chunk->GetUnalignedData(), "Unaligned chunk gained padding");
    chunk->m_Size = 17;
    const auto next = (payload + 17 + 3) & ~std::uintptr_t(3);
    require(reinterpret_cast<std::uintptr_t>(chunk->GetNextChunk()) == next, "Next chunk traversal lost its address");
    require(chunk->GetLastChunk() == chunk->GetNextChunk(), "Chunk traversal disagrees");
    std::cout << "RVL/clock field widths and native chunk alignment passed; address above 4 GiB: "
              << (address > UINT32_MAX ? "yes" : "no") << '\n';
    std::free(allocation);
}

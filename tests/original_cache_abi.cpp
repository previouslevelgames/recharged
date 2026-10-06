// Compiler/declaration inventory only. No fake SDK implementation or runtime
// cache/hardware success is supplied by this object.
#include <revolution/os/OSCache_fwd.h>
#include <revolution/os/OSCache.h>
#include <dolphin/os/OSCache.h>

static_assert(sizeof(u32) == 4);
using CacheCall = void (*)(void*, u32);
using ConstCacheCall = void (*)(const void*, u32);
template<class A, class B> struct SameType { static constexpr bool value = false; };
template<class T> struct SameType<T, T> { static constexpr bool value = true; };
static_assert(SameType<decltype(static_cast<CacheCall>(&DCFlushRange)), CacheCall>::value);
static_assert(SameType<decltype(static_cast<ConstCacheCall>(&DCFlushRange)), ConstCacheCall>::value);

void OriginalCacheDeclarationCompilerInventory(void* mutableData, const void* source, u32 bytes)
{
    DCInvalidateRange(mutableData, bytes);
    DCInvalidateRange(source, bytes);
    DCFlushRange(mutableData, bytes);
    DCFlushRange(source, bytes);
    DCStoreRange(source, bytes);
    DCFlushRangeNoSync(source, bytes);
    DCStoreRangeNoSync(source, bytes);
    DCZeroRange(source, bytes);
    ICInvalidateRange(source, bytes);
    LCLoadBlocks(mutableData, source, bytes);
    LCStoreBlocks(mutableData, source, bytes);
    (void)LCStoreData(mutableData, source, bytes);
}

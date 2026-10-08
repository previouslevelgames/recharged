#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error MWCC runtime helpers belong to the original game module
#endif

// Native equivalents of two CodeWarrior runtime helpers that reconstructed
// game source calls explicitly to match MWCC code generation:
// Runtime/runtime.c __cvt_fp2unsigned (Game/AI/Variant.cpp hashing) and
// Runtime/NMWException.cpp __construct_new_array (Game/AI/GoalieSave.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

extern "C" unsigned long __cvt_fp2unsigned(double value)
{
    // Original asm: below 0.0 -> 0; at or above 2^32 -> 0xFFFFFFFF; below 2^31
    // -> fctiwz; otherwise fctiwz(value - 2^31) + 0x80000000. NaN takes the
    // last path, where fctiwz yields 0x80000000, so the result is 0.
    if (value < 0.0) return 0;
    if (value >= 4294967296.0) return 0xFFFFFFFFu;
    if (value < 2147483648.0) return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
    const double high = value - 2147483648.0;
    const std::uint32_t word = high == high ? static_cast<std::uint32_t>(static_cast<std::int32_t>(high))
                                            : 0x80000000u;
    return static_cast<std::uint32_t>(word + 0x80000000u);
}

extern "C" void* __construct_new_array(void* block, void* ctor, void*, std::size_t size, std::size_t count)
{
    if (block == nullptr) return nullptr;
    // Wii array cookie: 32-bit element size and count at the start of the
    // 16-byte header; the elements follow it.
    const std::uint32_t words[2] = {static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(count)};
    std::memcpy(block, words, sizeof(words));
    if (ctor != nullptr)
        throw std::logic_error("Native __construct_new_array has no MWCC constructor-call ABI");
    return static_cast<char*>(block) + 0x10;
}

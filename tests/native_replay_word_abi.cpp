#include "Game/Replay.h"
#include "Game/Effects/EmissionController.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace {
unsigned checks;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        static_assert(sizeof(std::uintptr_t) == 8);
        static_assert(std::is_same_v<decltype(EmissionController::m_uUserData), std::uintptr_t>);
        static_assert(std::is_base_of_v<ReplayablePod, ReplayableCategory<std::uintptr_t>::Type>
                   || std::is_same_v<ReplayablePod, ReplayableCategory<std::uintptr_t>::Type>);
#if defined(_WIN64)
        static_assert(sizeof(unsigned long) == 4);
        static_assert(std::is_same_v<std::uintptr_t, unsigned long long>);
#else
        static_assert(sizeof(unsigned long) == 8);
        static_assert(std::is_same_v<std::uintptr_t, unsigned long>);
#endif
        std::array<char, 80> bytes;
        bytes.fill(char(0x5A));
        SaveFrame save{};
        save.mInterval = 1;
        save.mStream.mStorage = bytes.data() + 1;
        std::uintptr_t word = UINT64_C(0x11223344ABCDEF10);
        std::uint32_t numeric = UINT32_C(0x9174B2E3);
        const auto original_word = word;
        const auto original_numeric = numeric;
        save.Replayable<2>(word);
        ::Replayable<2>(save, numeric);
        Check(save.mStream.mStorage == bytes.data() + 1, "Skipped interval advanced source stream");
        for (char byte : bytes) Check(byte == char(0x5A), "Skipped interval changed bytes");

        save.Replayable<0>(word);
        ::Replayable<0>(save, numeric);
        Check(save.mStream.mStorage == bytes.data() + 13, "Native word/numeric source widths changed");
        Check(std::memcmp(bytes.data() + 1, &original_word, 8) == 0, "Native POD address bytes changed");
        Check(std::memcmp(bytes.data() + 9, &original_numeric, 4) == 0, "Original numeric word bytes changed");
        Check(bytes[0] == char(0x5A) && bytes[13] == char(0x5A), "Unaligned source store exceeded exact cells");

        LoadFrame load{};
        load.mInterval = 1;
        load.mStream.mStorage = bytes.data() + 1;
        word = 0;
        numeric = 0;
        load.Replayable<2>(word);
        ::Replayable<2>(load, numeric);
        Check(word == 0 && numeric == 0 && load.mStream.mStorage == bytes.data() + 1,
              "Skipped read interval changed value or cursor");
        load.Replayable<0>(word);
        ::Replayable<0>(load, numeric);
        Check(word == original_word && numeric == original_numeric, "Source POD values did not round trip");
        Check(load.mStream.mStorage == bytes.data() + 13, "Source load cursor width changed");

        for (std::uintptr_t expected : {std::uintptr_t(0), UINTPTR_MAX, std::uintptr_t(0x100000000ULL)}) {
            save.mStream.mStorage = bytes.data() + 3;
            word = expected;
            ::Replayable<1>(save, word);
            Check(save.mStream.mStorage == bytes.data() + 11, "Matched interval did not write native cell");
            load.mStream.mStorage = bytes.data() + 3;
            word = ~expected;
            ::Replayable<1>(load, word);
            Check(word == expected && load.mStream.mStorage == bytes.data() + 11,
                  "Matched interval lost zero/high/all-one native word");
        }
        std::printf("Native original Replay POD ABI PASS %u checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Replay POD ABI failure: %s (%u checks)\n", error.what(), checks);
        return 1;
    }
}

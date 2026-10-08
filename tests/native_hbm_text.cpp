#include "NL/MemAlloc.h"
#include "platform/game_allocation_ownership.h"
#include "platform/hbm_text_transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" std::size_t ChargedWii_wcslen(const wchar_t*);
static_assert(sizeof(wchar_t) == 2);

namespace {
unsigned checks;
void Check(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
template<class Error = std::invalid_argument, class Action>
void Reject(Action action) {
    ++checks;
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("Invalid HBM text owner was accepted");
}
// Synthetic authored bytes: BOM, quote, A, e-acute, quote. The terminator is
// source-owned zero padding outside the completed logical file extent.
constexpr std::array<unsigned char, 10> wire{0xfe,0xff,0,0x22,0,0x41,0,0xe9,0,0x22};
void Complete(unsigned char* raw, std::size_t count = wire.size()) {
    using namespace mscharged::platform;
    GameByteWriteReservation write(raw, count);
    std::memcpy(raw, wire.data(), count);
    write.Complete(GameByteDomain::WiiSerialized);
}
}

int main() {
    using namespace mscharged::platform;
    try {
        alignas(64) std::array<std::byte, 32768> arena{};
        MemoryAllocator owner{};
        owner.Initialize(arena.data(), arena.size());
        const auto initialFree = owner.TotalFreeMemory();
        auto* raw = static_cast<unsigned char*>(owner.Allocate(32, 32, false));
        Check(raw != nullptr, "Original allocation failed");
        std::memset(raw, 0, 32);
        Reject([&] { PrepareNativeHBMMessage(nullptr); });
        Reject([&] { PrepareNativeHBMMessage(raw); });
        {
            GameByteWriteReservation pending(raw, wire.size());
            std::memcpy(raw, wire.data(), wire.size());
            Reject([&] { PrepareNativeHBMMessage(raw); });
        }
        Complete(raw);
        Reject([&] { PrepareNativeHBMMessage(raw + 1); });
        Reject([&] { PrepareNativeHBMMessage(raw + 2); });
        raw[wire.size()] = 1;
        Reject([&] { PrepareNativeHBMMessage(raw); });
        Check(std::memcmp(raw, wire.data(), wire.size()) == 0, "Rejection mutated the file");
        raw[wire.size()] = 0;
        auto* text = static_cast<wchar_t*>(PrepareNativeHBMMessage(raw));
        Check(text == reinterpret_cast<wchar_t*>(raw), "Native conversion replaced source storage");
        const std::array<std::uint16_t, 6> expected{0xfeff,0x22,0x41,0xe9,0x22,0};
        for (std::size_t i = 0; i < expected.size(); ++i)
            Check(static_cast<std::uint16_t>(text[i]) == expected[i], "BE16 cell changed");
        Check(ChargedWii_wcslen(text) == 5, "Original MSL did not consume Wii16 cells");
        GameCompletedSpan completed{};
        Check(FindGameCompletedSpan(raw, wire.size(), completed)
              && completed.base == raw && completed.bytes == wire.size(), "Logical file extent changed");
        const auto firstIncarnation = completed.allocation.incarnation;
        Check(!FindGameCompletedSpan(raw + wire.size(), 2, completed), "Padding became completed file data");
        Check(FindGameByteDomain(raw, wire.size()) == GameByteDomain::NativePayload,
              "Conversion did not publish native representation");
        text[4] = 0; // Stand-in for the original parser's closing-quote mutation.
        Check(PrepareNativeHBMMessage(raw) == raw && text[4] == 0,
              "Repeated conversion destroyed a caller mutation");
        Check(ChargedWii_wcslen(text + 2) == 2, "Original Wii16 string length changed");
        Complete(raw); // Actual new producer invalidates the prior representation.
        Check(FindGameByteDomain(raw, wire.size()) == GameByteDomain::WiiSerialized,
              "A fresh read retained the previous representation");
        PrepareNativeHBMMessage(raw);
        Check(text[4] == 0x22, "A fresh read was not converted independently");
        owner.Free(raw);
        Check(!FindGameCompletedSpan(raw, 2, completed), "Free retained completed data");
        Reject([&] { PrepareNativeHBMMessage(raw); });

        auto* reused = static_cast<unsigned char*>(owner.Allocate(32, 32, false));
        Check(reused == raw, "Fixture did not exercise actual free-list address reuse");
        Reject([&] { PrepareNativeHBMMessage(reused); });
        std::memset(reused, 0, 32);
        Complete(reused);
        Check(FindGameCompletedSpan(reused, 2, completed)
              && completed.allocation.incarnation != firstIncarnation, "Address reuse retained old lifetime");
        PrepareNativeHBMMessage(reused);
        Check(reinterpret_cast<wchar_t*>(reused)[3] == 0xe9, "Reused owner was not converted");
        Complete(reused, 9);
        Reject([&] { PrepareNativeHBMMessage(reused); });
        Complete(reused);
        reused[0] = 0xff; reused[1] = 0xfe;
        Reject([&] { PrepareNativeHBMMessage(reused); });
        owner.Free(reused);

        // Fill the genuinely recorded payload, including the allocator's
        // minimum/rounding, so no caller-owned terminator capacity remains.
        auto* exact = static_cast<unsigned char*>(owner.Allocate(32, 32, false));
        GameAllocationSpan allocation{};
        Check(FindGameAllocationSpan(exact, 1, allocation), "Allocation extent is absent");
        {
            GameByteWriteReservation write(exact, allocation.bytes);
            std::memset(exact, 0, allocation.bytes);
            std::memcpy(exact, wire.data(), wire.size());
            write.Complete(GameByteDomain::WiiSerialized);
        }
        Reject([&] { PrepareNativeHBMMessage(exact); });
        owner.Free(exact);
        Check(owner.TotalFreeMemory() == initialFree, "Original allocator did not recover its storage");
        std::printf("Native HBM text: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native HBM text at check %u: %s\n", checks, error.what());
        return 1;
    }
}

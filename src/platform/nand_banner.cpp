#include "platform/nand_banner.h"
#include "NL/plat/nlFlash.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <stdexcept>

namespace mscharged::platform {
namespace {
constexpr std::size_t HeaderBytes = 0xA0;
constexpr std::size_t BannerBytes = 0x6000;
constexpr std::size_t IconBytes = 0x1200;
constexpr std::size_t SourceBytes = HeaderBytes + BannerBytes + 8 * IconBytes;
static_assert(SourceBytes == 0xF0A0);
static_assert(std::endian::native == std::endian::little,
              "This banner transport is qualified for little-endian native hosts");

struct Free {
    void operator()(void* p) const noexcept {
#if defined(_WIN32)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }
};
using Snapshot = std::unique_ptr<void, Free>;
struct Pending {
    void* bytes;
    void (*callback)(std::int32_t);
};
Pending pending{};

void Field(unsigned char* bytes, std::size_t offset, unsigned width) {
    for (unsigned i = 0; i < width / 2; ++i)
        std::swap(bytes[offset + i], bytes[offset + width - 1 - i]);
}

void Complete(s32 result) {
    // Original FlashMemoryTask cleared its callback before reaching here.
    // Retire the actual completed IO snapshot before forwarding to the source
    // callback, which may legitimately submit another banner write.
    if (!pending.bytes || !pending.callback)
        throw std::logic_error("Native NAND banner completion has no live snapshot");
    Snapshot finished(pending.bytes);
    const auto callback = pending.callback;
    pending = {};
    finished.reset();
    callback(result);
}
}

bool OriginalNANDBannerWritePending() noexcept { return pending.bytes != nullptr; }

std::int32_t WriteOriginalNANDBanner(const void* native_banner,
                                   std::uint32_t bytes,
                                   void (*callback)(std::int32_t)) {
    if (!native_banner || bytes != SourceBytes)
        throw std::invalid_argument("Unqualified original NAND banner extent");
    if (pending.bytes)
        throw std::logic_error("Original NAND banner write is still pending");
#if defined(_WIN32)
    Snapshot copy(_aligned_malloc(SourceBytes, 32));
#else
    Snapshot copy(std::aligned_alloc(32, SourceBytes));
#endif
    if (!copy) return -2; // Actual NAND_RESULT_ALLOC_FAILED, no request submitted.
    std::memcpy(copy.get(), native_banner, SourceBytes);
    auto* wire = static_cast<unsigned char*>(copy.get());
    Field(wire, 0, 4);
    Field(wire, 4, 4);
    Field(wire, 8, 2);
    for (unsigned offset = 0x20; offset < HeaderBytes; offset += 2)
        Field(wire, offset, 2);
    if (!callback) return nlFlashWrite(copy.get(), bytes, nullptr);
    pending = {copy.release(), callback};
    s32 result;
    try {
        result = nlFlashWrite(pending.bytes, bytes, Complete);
    } catch (...) {
        Snapshot failed(pending.bytes);
        pending = {};
        throw;
    }
    if (result != 0) {
        Snapshot failed(pending.bytes);
        pending = {};
    }
    return result;
}
}

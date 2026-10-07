#include "platform/native_sp_table.h"
#include "platform/game_allocation_ownership.h"
#include "platform/dsp_memory_abi.h"
#include "NL/nlChunk.h"
#include "revolution/sp.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace mscharged::platform {
namespace {
std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
         | std::uint32_t(p[2]) << 8 | p[3];
}
std::uint16_t Half(const unsigned char* p) {
    return std::uint16_t(std::uint16_t(p[0]) << 8 | p[1]);
}
struct Storage { std::uint32_t marker, reserved; };
constexpr std::uint32_t Marker = 0x53505354;
}
static_assert(sizeof(SPADPCM) == 46);
static_assert(offsetof(SPSoundEntry, adpcm) == 24);
static_assert(alignof(SPSoundTable) <= alignof(std::max_align_t));

SPSoundTable* PrepareNativeSPSoundTable(nlChunk* chunk) {
    GameCompletedSpan complete{};
    if (!chunk || !FindGameCompletedSpan(chunk, 8, complete))
        throw std::invalid_argument("SP table requires actual completed NL source bytes");
    const auto sourceBytes = std::size_t(chunk->GetSize()) + 8;
    if (!FindGameCompletedSpan(chunk, sourceBytes, complete)
        || FindGameByteDomain(chunk, sourceBytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("SP table requires one original serialized source span");
    GameNativeBackingSpan previous{};
    if (FindGameNativeBacking(chunk, sourceBytes, previous)) {
        if (previous.bytes < sizeof(Storage) + sizeof(SPSoundTable)
            || static_cast<const Storage*>(previous.data)->marker != Marker)
            throw std::invalid_argument("SP source range has another native POD profile");
        return reinterpret_cast<SPSoundTable*>(static_cast<unsigned char*>(previous.data) + sizeof(Storage));
    }

    const auto* raw = static_cast<const unsigned char*>(chunk->GetData());
    const auto rawBytes = std::size_t(chunk->GetDataSize());
    if (rawBytes < 4 || !FindGameCompletedSpan(raw, rawBytes, complete))
        throw std::out_of_range("SP table header leaves its actual source bytes");
    const auto count = Word(raw);
    if (count > (rawBytes - 4) / 28)
        throw std::out_of_range("SP Wii28 records leave their original completed table");
    std::size_t adpcmCount = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto type = Word(raw + 4 + i * 28);
        // The original SPInitSoundTable consumes one sequential46-byte record
        // for types0/1 only; preserve the other original PCM/no-case branches.
        if (type == 0 || type == 1) ++adpcmCount;
    }
    const auto wireADPCM = std::size_t(4) + std::size_t(count) * 28;
    if (adpcmCount > (rawBytes - wireADPCM) / 46)
        throw std::out_of_range("SP ADPCM metadata leaves its original completed table");
    constexpr auto nativeHeader = offsetof(SPSoundTable, sound);
    if (count > (std::numeric_limits<std::size_t>::max() - nativeHeader) / sizeof(SPSoundEntry))
        throw std::overflow_error("SP native record extent overflows");
    const auto nativeADPCM = nativeHeader + std::size_t(count) * sizeof(SPSoundEntry);
    if (adpcmCount > (std::numeric_limits<std::size_t>::max() - nativeADPCM) / sizeof(SPADPCM))
        throw std::overflow_error("SP native ADPCM extent overflows");
    auto nativeBytes = nativeADPCM + adpcmCount * sizeof(SPADPCM);
    if (nativeBytes < sizeof(SPSoundTable)) nativeBytes = sizeof(SPSoundTable);
    if (nativeBytes > std::numeric_limits<std::size_t>::max() - sizeof(Storage))
        throw std::overflow_error("SP native profile extent overflows");
    GameNativeBackingReservation reservation(chunk, sourceBytes, sizeof(Storage) + nativeBytes);
    auto* storage = new(reservation.Data()) Storage{Marker, 0};
    auto* table = new(storage + 1) SPSoundTable{};
    table->entries = count;
    auto* native = reinterpret_cast<unsigned char*>(table);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* r = raw + 4 + i * 28;
        auto* sound = new(native + nativeHeader + i * sizeof(SPSoundEntry)) SPSoundEntry{};
        sound->type = Word(r); sound->sampleRate = Word(r + 4);
        sound->loopAddr = Word(r + 8); sound->loopEndAddr = Word(r + 12);
        sound->endAddr = Word(r + 16); sound->currentAddr = Word(r + 20);
        sound->adpcm = reinterpret_cast<SPADPCM*>(std::uintptr_t(Word(r + 24)));
    }
    for (std::size_t i = 0; i < adpcmCount; ++i) {
        const auto* r = raw + wireADPCM + i * 46;
        auto* adpcm = new(native + nativeADPCM + i * sizeof(SPADPCM)) SPADPCM{};
        for (unsigned j = 0; j < 8; ++j)
            for (unsigned k = 0; k < 2; ++k)
                adpcm->adpcm.a[j][k] = Half(r + j * 4 + k * 2);
        adpcm->adpcm.gain = Half(r + 32);
        adpcm->adpcm.pred_scale = Half(r + 34);
        adpcm->adpcm.yn1 = Half(r + 36); adpcm->adpcm.yn2 = Half(r + 38);
        adpcm->adpcmloop.loop_pred_scale = Half(r + 40);
        adpcm->adpcmloop.loop_yn1 = Half(r + 42);
        adpcm->adpcmloop.loop_yn2 = Half(r + 44);
    }
    reservation.Commit();
    return table;
}

std::uint32_t NativeAudioCachedWord(const void* data, std::uint32_t bytes) {
    GameAllocationSpan owner{};
    if (!bytes || !FindGameAllocationSpan(data, bytes, owner))
        throw std::invalid_argument("Audio hardware word has no actual live game allocation extent");
    // The real DSP pin supplies a canonical MEM1/MEM2/static physical address.
    // Original SP subtracts0x80000000 before its nibble/word arithmetic.
    const auto physical = ChargedDSPTaskMemoryWord(data, bytes, 0);
    if (physical > 0x7fffffffu)
        throw std::out_of_range("Audio physical span has no original cached Wii representation");
    return physical + 0x80000000u;
}
std::uint32_t NativeAudioSampleCachedWord(const void* data, std::uint32_t bytes) {
    GameCompletedSpan complete{};
    if (!bytes || !FindGameCompletedSpan(data, bytes, complete)
        || FindGameByteDomain(data, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Audio sample word requires genuine completed NL bytes");
    return NativeAudioCachedWord(data, bytes);
}
}

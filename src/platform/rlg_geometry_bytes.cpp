#include "platform/rlg_geometry_bytes.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "Game/GL/ShaderSkinMesh.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace mscharged::platform
{
namespace
{
void RequireWiiCopy(const void* source, std::size_t bytes)
{
    if (FindGameByteDomain(source, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RLG source copy requires completed owned Wii bytes");
}
void RequireOwnedWiiRead(const void* source, std::size_t bytes)
{
    GameCompletedSpan completed{};
    if (!FindGameCompletedSpan(source, bytes, completed))
        throw std::invalid_argument("RLG skin read leaves its completed original owner");
    RequireWiiCopy(source, bytes);
}
std::uint32_t SkinWord(const unsigned char* source) noexcept
{
    return (std::uint32_t(source[0]) << 24) | (std::uint32_t(source[1]) << 16)
        | (std::uint32_t(source[2]) << 8) | source[3];
}
struct WeightRows
{
    const void* raw;
    std::size_t bytes;
    GameCompletedSpan source;
    GameByteWriteReservation write;
    float* Data() noexcept { return reinterpret_cast<float*>(this + 1); }
};
struct Float3Rows
{
    const void* raw;
    std::size_t bytes;
    GameCompletedSpan source;
    GameByteDomain domain;
    nlVector3* Data() noexcept { return reinterpret_cast<nlVector3*>(this + 1); }
};
static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(alignof(WeightRows) >= alignof(float));
}
void CopyRLGWireVertices(void* output, const void* source, std::size_t bytes)
{
    if (!bytes) return;
    RequireWiiCopy(source, bytes);
    GameByteWriteReservation write(output, bytes);
    std::memcpy(output, source, bytes);
    write.Complete(GameByteDomain::WiiSerialized);
}
void CopyRLGWireSkin(void* output, const void* source, std::size_t bytes)
{
    // The caller retains its exact original sizeof(nlChunk)+GetDataSize request.
    CopyRLGWireVertices(output, source, bytes);
}
void CopyRLGNativeIndices(void* output, const void* source, std::size_t bytes)
{
    if (!bytes) return;
    RequireWiiCopy(source, bytes);
    GameByteWriteReservation write(output, bytes);
    const auto* raw = static_cast<const unsigned char*>(source);
    auto* native = static_cast<unsigned char*>(output);
    for (std::size_t offset = 0; offset + 1 < bytes; offset += sizeof(std::uint16_t))
    {
        const auto value = std::uint16_t((std::uint16_t(raw[offset]) << 8) | raw[offset + 1]);
        std::memcpy(native + offset, &value, sizeof(value));
    }
    // Original memcpy copies an odd terminal byte too; do not reject, round or
    // manufacture an extra index cell when preserving that raw size quirk.
    if (bytes & 1) native[bytes - 1] = raw[bytes - 1];
    write.Complete(GameByteDomain::NativePayload);
}
void CopyRLGNativeMatrices(void* output, const void* source, std::size_t bytes)
{
    if (!bytes) return;
    if (bytes % 64)
        throw std::invalid_argument("RLG matrix copy requires complete original 64-byte records");
    RequireWiiCopy(source, bytes);
    GameByteWriteReservation write(output, bytes);
    if (!write.Tracked())
        throw std::invalid_argument("RLG matrix output has no genuine original owner");
    const auto* raw = static_cast<const unsigned char*>(source);
    auto* native = static_cast<unsigned char*>(output);
    for (std::size_t offset = 0; offset < bytes; offset += sizeof(std::uint32_t))
    {
        const std::uint32_t bits = (std::uint32_t(raw[offset]) << 24)
            | (std::uint32_t(raw[offset + 1]) << 16)
            | (std::uint32_t(raw[offset + 2]) << 8) | raw[offset + 3];
        // No float arithmetic: retain authored zero, infinity and NaN bits.
        std::memcpy(native + offset, &bits, sizeof(bits));
    }
    write.Complete(GameByteDomain::NativeHeader);
}

std::uint32_t ReadRLGSkinWord(const void* source)
{
    RequireOwnedWiiRead(source, sizeof(std::uint32_t));
    return SkinWord(static_cast<const unsigned char*>(source));
}

void ReadRLGSkinMatrix(nlMatrix4& output, const void* source)
{
    static_assert(sizeof(nlMatrix4) == 64);
    static_assert(std::is_trivially_copyable_v<nlMatrix4>);
    RequireOwnedWiiRead(source, sizeof(nlMatrix4));
    std::array<std::uint32_t, 16> native{};
    const auto* raw = static_cast<const unsigned char*>(source);
    for (std::size_t i = 0; i < native.size(); ++i)
        native[i] = SkinWord(raw + i * 4);
    // No inversion/pose arithmetic here: the original factory still owns it.
    std::memcpy(&output, native.data(), sizeof(output));
}

const MorphDelta* ReadRLGSkinMorphDeltas(const void* source,
    std::size_t count, std::size_t sourceStride)
{
    static_assert(sizeof(MorphDelta) == 16);
    static_assert(offsetof(MorphDelta, delta) == 0);
    static_assert(offsetof(MorphDelta, index) == 12);
    static_assert(sizeof(MorphDelta::index) == 4);
    static_assert(std::is_trivially_copyable_v<MorphDelta>);
    // A zero-count original list retains its source pointer without reading or
    // allocating a record. The sixteen current NPC templates use this path.
    if (!count) return static_cast<const MorphDelta*>(source);
    if (sourceStride != sizeof(MorphDelta))
        throw std::invalid_argument("Authored morph record stride remains unqualified");
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(MorphDelta))
        throw std::length_error("Original morph record byte extent overflows");
    const auto bytes = count * sizeof(MorphDelta);
    RequireOwnedWiiRead(source, bytes);
    GameNativeBackingSpan previous{};
    if (FindGameNativeBacking(source, bytes, previous))
    {
        if (previous.bytes != bytes)
            throw std::invalid_argument("Original morph owner has a different native view");
        return static_cast<const MorphDelta*>(previous.data);
    }
    GameNativeBackingReservation backing(source, bytes, bytes);
    auto* output = static_cast<MorphDelta*>(backing.Data());
    const auto* raw = static_cast<const unsigned char*>(source);
    for (std::size_t i = 0; i < count; ++i)
    {
        std::array<std::uint32_t, 4> words{};
        for (std::size_t j = 0; j < words.size(); ++j)
            words[j] = SkinWord(raw + i * 16 + j * 4);
        MorphDelta native;
        std::memcpy(&native, words.data(), sizeof(native));
        new (output + i) MorphDelta(native);
    }
    backing.Commit();
    return output;
}

NativeRLGFloat3Rows::NativeRLGFloat3Rows(const void* source,
    std::size_t vertices, std::size_t sourceStride) : state_(nullptr), source_(source)
{
    static_assert(sizeof(nlVector3) == 12);
    static_assert(std::is_trivially_copyable_v<nlVector3>);
    static_assert(alignof(Float3Rows) >= alignof(nlVector3));
    if (!vertices) return;
    if (sourceStride != sizeof(nlVector3))
        throw std::invalid_argument("Original software skin requires qualified Float3 stream stride");
    if (vertices > (std::numeric_limits<std::size_t>::max() - sizeof(Float3Rows)) / sizeof(nlVector3))
        throw std::length_error("Original Float3 CPU view byte extent overflows");
    const auto bytes = vertices * sizeof(nlVector3);
    GameCompletedSpan span{};
    if (!FindGameCompletedSpan(source, bytes, span))
        throw std::invalid_argument("Original Float3 rows leave completed source storage");
    const auto domain = FindGameByteDomain(source, bytes);
    if (domain != GameByteDomain::WiiSerialized && domain != GameByteDomain::NativePayload)
        throw std::invalid_argument("Original Float3 rows have an unknown numeric byte domain");
    void* storage = ChargedNativeMetadataAllocate(sizeof(Float3Rows) + bytes);
    auto* rows = new (storage) Float3Rows{source, bytes, span, domain};
    const auto* raw = static_cast<const unsigned char*>(source);
    for (std::size_t i = 0; i < vertices; ++i)
    {
        std::array<std::uint32_t, 3> words{};
        for (std::size_t j = 0; j < words.size(); ++j)
        {
            const auto at = i * 12 + j * 4;
            if (domain == GameByteDomain::WiiSerialized) words[j] = SkinWord(raw + at);
            else std::memcpy(&words[j], raw + at, 4);
        }
        auto* record = new (rows->Data() + i) nlVector3;
        std::memcpy(record, words.data(), sizeof(*record));
    }
    state_ = rows;
}

NativeRLGFloat3Rows::~NativeRLGFloat3Rows()
{
    if (!state_) return;
    auto* rows = static_cast<Float3Rows*>(state_);
    rows->~Float3Rows();
    ChargedNativeMetadataRelease(rows);
}

const nlVector3* NativeRLGFloat3Rows::Data() const
{
    if (!state_) return static_cast<const nlVector3*>(source_);
    const auto& rows = *static_cast<const Float3Rows*>(state_);
    GameCompletedSpan span{};
    if (!FindGameCompletedSpan(rows.raw, rows.bytes, span)
        || span.allocation.base != rows.source.allocation.base
        || span.allocation.owner != rows.source.allocation.owner
        || span.allocation.incarnation != rows.source.allocation.incarnation
        || FindGameByteDomain(rows.raw, rows.bytes) != rows.domain)
        throw std::invalid_argument("Original Float3 source was retired, reused or changed domain");
    return const_cast<Float3Rows&>(rows).Data();
}

NativeRLGWeightRows::NativeRLGWeightRows(const void* source, std::size_t vertices,
    RLGWeightAccess access) : state_(nullptr)
{
    if (!vertices) return;
    if (access != RLGWeightAccess::ReadOnly && access != RLGWeightAccess::ReadWrite)
        throw std::invalid_argument("Unknown RLG weight access domain");
    if (vertices > (std::numeric_limits<std::size_t>::max() - sizeof(WeightRows)) / 16)
        throw std::length_error("Original RLG weight row byte extent overflows");
    const std::size_t bytes = vertices * 16;
    GameCompletedSpan span{};
    if (!FindGameCompletedSpan(source, bytes, span))
        throw std::invalid_argument("RLG weight rows leave their actual completed logical span");
    RequireWiiCopy(source, bytes);
    if (access == RLGWeightAccess::ReadWrite)
    {
        // The original index writes share this completed vertex-copy span.
        // Preserve its full logical extent when republishing the source swaps.
        GameCompletedSpan complete{};
        if (!FindGameCompletedSpan(span.base, span.bytes, complete)
            || complete.base != span.base || complete.bytes != span.bytes)
            throw std::invalid_argument("RLG weight rewrite has no complete original vertex copy");
        RequireWiiCopy(span.base, span.bytes);
    }
    void* storage = ChargedNativeMetadataAllocate(sizeof(WeightRows) + bytes);
    auto* rows = new (storage) WeightRows{source, bytes, span, {}};
    try
    {
        const auto* raw = static_cast<const unsigned char*>(source);
        for (std::size_t at = 0; at < bytes; at += 4)
        {
            const std::uint32_t bits = (std::uint32_t(raw[at]) << 24)
                | (std::uint32_t(raw[at + 1]) << 16)
                | (std::uint32_t(raw[at + 2]) << 8) | raw[at + 3];
            std::memcpy(reinterpret_cast<unsigned char*>(rows->Data()) + at, &bits, 4);
        }
        if (access == RLGWeightAccess::ReadWrite)
            rows->write = GameByteWriteReservation(const_cast<void*>(span.base), span.bytes,
                GameByteInPlaceRewrite{});
        state_ = rows;
    }
    catch (...)
    {
        rows->~WeightRows();
        ChargedNativeMetadataRelease(storage);
        throw;
    }
}

NativeRLGWeightRows::~NativeRLGWeightRows()
{
    if (!state_) return;
    auto* rows = static_cast<WeightRows*>(state_);
    rows->~WeightRows();
    ChargedNativeMetadataRelease(rows);
}

float* NativeRLGWeightRows::Data() const noexcept
{
    return state_ ? static_cast<WeightRows*>(state_)->Data() : nullptr;
}

void NativeRLGWeightRows::StoreWire()
{
    if (!state_) return;
    auto& rows = *static_cast<WeightRows*>(state_);
    if (!rows.write.Tracked())
        throw std::logic_error("RLG weight view has no original rewrite reservation");
    GameAllocationSpan owner{};
    if (!FindGameAllocationSpan(rows.raw, rows.bytes, owner)
        || owner.base != rows.source.allocation.base
        || owner.owner != rows.source.allocation.owner
        || owner.incarnation != rows.source.allocation.incarnation)
        throw std::invalid_argument("RLG weight source allocation was retired or reused");
    auto* output = static_cast<unsigned char*>(const_cast<void*>(rows.raw));
    const auto* input = reinterpret_cast<const unsigned char*>(rows.Data());
    for (std::size_t at = 0; at < rows.bytes; at += 4)
    {
        std::uint32_t bits;
        std::memcpy(&bits, input + at, 4);
        output[at] = static_cast<unsigned char>(bits >> 24);
        output[at + 1] = static_cast<unsigned char>(bits >> 16);
        output[at + 2] = static_cast<unsigned char>(bits >> 8);
        output[at + 3] = static_cast<unsigned char>(bits);
    }
    rows.write.Complete(GameByteDomain::WiiSerialized);
    rows.write.Reset();
}

}

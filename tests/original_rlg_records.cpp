#include "platform/rlg_record_abi.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
using Bytes = std::vector<unsigned char>;
using Row = std::vector<std::uint32_t>;
using Rows = std::vector<Row>;
std::size_t checks;
std::size_t corpusCases;

void Check(bool condition, const char* reason)
{
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}

// The fixture writer uses division/remainder. Expected field values are stored
// independently, and never obtained through the adapter's word-reading helper.
void Word(Bytes& bytes, std::size_t offset, std::uint32_t value, std::size_t width = 4)
{
    for (std::size_t i = width; i; --i)
    {
        bytes.at(offset + i - 1) = static_cast<unsigned char>(value % 256);
        value /= 256;
    }
}

class GuardedStorage
{
    std::vector<std::max_align_t> storage;
    std::size_t extent;
public:
    static constexpr std::size_t Prefix = 32;
    explicit GuardedStorage(std::size_t bytes) :
        storage((bytes + 2 * Prefix + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)),
        extent(bytes)
    {
        std::memset(storage.data(), 0xCD, storage.size() * sizeof(std::max_align_t));
    }
    unsigned char* Data() { return reinterpret_cast<unsigned char*>(storage.data()) + Prefix; }
    void CheckGuards()
    {
        const auto* all = reinterpret_cast<unsigned char*>(storage.data());
        for (std::size_t i = 0; i < Prefix; ++i) Check(all[i] == 0xCD, "leading allocation guard");
        for (std::size_t i = Prefix + extent; i < storage.size() * sizeof(std::max_align_t); ++i)
            Check(all[i] == 0xCD, "trailing allocation guard");
    }
};

template<class T> void Padding(GuardedStorage& storage, std::size_t count,
    const std::vector<std::pair<std::size_t, std::size_t>>& fields)
{
    std::array<bool, sizeof(T)> assigned{};
    for (auto [offset, bytes] : fields)
        for (std::size_t i = 0; i < bytes; ++i) assigned.at(offset + i) = true;
    for (std::size_t n = 0; n < count; ++n)
        for (std::size_t i = 0; i < sizeof(T); ++i)
            if (!assigned[i]) Check(storage.Data()[n * sizeof(T) + i] == 0xCD, "native padding was rewritten");
}

void Models(const Bytes& raw, const Rows& rows)
{
    Check(raw.size() / 12 == rows.size(), "model raw count");
    const std::size_t extent = rows.size() * sizeof(glModel) + raw.size() % 12;
    Check(mscharged::platform::RLGNativeRecordBytes(raw.size(), 12, sizeof(glModel)) == extent, "model allocation geometry");
    GuardedStorage storage(extent);
    auto* output = reinterpret_cast<glModel*>(storage.Data());
    Check(reinterpret_cast<std::uintptr_t>(output) % alignof(glModel) == 0, "model native alignment");
    mscharged::platform::DecodeRLGModels(output, raw.data(), rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        Check(output[i].id == rows[i][0], "model id");
        Check(output[i].numPackets == rows[i][1], "model packet count");
        Check(reinterpret_cast<std::uintptr_t>(output[i].packets) == rows[i][2], "model encoded packet word");
    }
    for (std::size_t i = rows.size() * sizeof(glModel); i < extent; ++i)
        Check(storage.Data()[i] == 0xCD, "model original complete-record-only tail");
    Padding<glModel>(storage, rows.size(), {{offsetof(glModel, id), sizeof(glModel::id)},
        {offsetof(glModel, numPackets), sizeof(glModel::numPackets)},
        {offsetof(glModel, packets), sizeof(glModel::packets)}});
    storage.CheckGuards();
}

void Streams(const Bytes& raw, const Rows& rows)
{
    Check(raw.size() / 8 == rows.size(), "stream raw count");
    const std::size_t extent = rows.size() * sizeof(glModelStream) + raw.size() % 8;
    Check(mscharged::platform::RLGNativeRecordBytes(raw.size(), 8, sizeof(glModelStream)) == extent, "stream allocation geometry");
    GuardedStorage storage(extent);
    auto* output = reinterpret_cast<glModelStream*>(storage.Data());
    Check(reinterpret_cast<std::uintptr_t>(output) % alignof(glModelStream) == 0, "stream native alignment");
    mscharged::platform::DecodeRLGStreams(output, raw.data(), rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        Check(reinterpret_cast<std::uintptr_t>(output[i].address) == rows[i][0], "stream encoded vertex offset");
        Check(output[i].index == rows[i][1], "stream source index");
        Check(output[i].stride == rows[i][2], "stream source stride");
        Check(output[i].id == rows[i][3], "stream source id");
        Check(output[i].unknown07 == rows[i][4], "stream unknown byte");
    }
    for (std::size_t i = rows.size() * sizeof(glModelStream); i < extent; ++i)
        Check(storage.Data()[i] == 0xCD, "stream original complete-record-only tail");
    Padding<glModelStream>(storage, rows.size(), {{offsetof(glModelStream, address), sizeof(glModelStream::address)},
        {offsetof(glModelStream, index), 1}, {offsetof(glModelStream, stride), 1},
        {offsetof(glModelStream, id), 1}, {offsetof(glModelStream, unknown07), 1}});
    storage.CheckGuards();
}

void Packets(const Bytes& raw, const Rows& rows)
{
    Check(raw.size() / 48 == rows.size(), "packet raw count");
    const std::size_t extent = rows.size() * sizeof(glModelPacket) + raw.size() % 48;
    Check(mscharged::platform::RLGNativeRecordBytes(raw.size(), 48, sizeof(glModelPacket)) == extent, "packet allocation geometry");
    GuardedStorage storage(extent);
    auto* output = reinterpret_cast<glModelPacket*>(storage.Data());
    Check(reinterpret_cast<std::uintptr_t>(output) % alignof(glModelPacket) == 0, "packet native alignment");
    mscharged::platform::DecodeRLGPackets(output, raw.data(), raw.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& p = output[i];
        const auto& r = rows[i];
        Check(reinterpret_cast<std::uintptr_t>(p.indexBuffer) == r[0], "packet encoded index offset");
        Check(p.numVertices == r[1], "packet original index count");
        Check(p.numUniqueVertices == r[2], "packet original unique count");
        unsigned char primitive;
        std::memcpy(&primitive, &p.primType, 1);
        Check(primitive == r[3], "packet primitive source bit pattern");
        Check(p.numStreams == r[4], "packet stream count");
        Check(reinterpret_cast<std::uintptr_t>(p.streams) == r[5], "packet encoded raw stream offset");
        Check(reinterpret_cast<std::uintptr_t>(p.materialProgram) == r[6], "packet encoded program id");
        Check(p.unknown14 == r[7], "packet unknown14");
        Check(p.matrix == r[8], "packet matrix source index");
        Check(p.rasterState == r[9], "packet raster state");
        Check(reinterpret_cast<std::uintptr_t>(p.materialParameters) == r[10], "packet encoded parameter offset");
        Check(reinterpret_cast<std::uintptr_t>(p.displayList) == r[11], "packet encoded display-list word");
        Check(p.skinnedVertices == r[12], "packet preserved unresolved u32 skin word");
        Check(p.skinnedNormals == r[13], "packet preserved unresolved u32 normal word");
    }
    for (std::size_t i = 0; i < raw.size() % 48; ++i)
        Check(storage.Data()[rows.size() * sizeof(glModelPacket) + i] == raw[rows.size() * 48 + i],
            "packet original whole-byte tail");
    Padding<glModelPacket>(storage, rows.size(), {
        {offsetof(glModelPacket, indexBuffer), sizeof(glModelPacket::indexBuffer)},
        {offsetof(glModelPacket, numVertices), 4}, {offsetof(glModelPacket, numUniqueVertices), 2},
        {offsetof(glModelPacket, primType), 1}, {offsetof(glModelPacket, numStreams), 1},
        {offsetof(glModelPacket, streams), sizeof(glModelPacket::streams)},
        {offsetof(glModelPacket, materialProgram), sizeof(glModelPacket::materialProgram)},
        {offsetof(glModelPacket, unknown14), 4}, {offsetof(glModelPacket, matrix), sizeof(glMatrixHandle)},
        {offsetof(glModelPacket, rasterState), sizeof(glModelPacket::rasterState)},
        {offsetof(glModelPacket, materialParameters), sizeof(glModelPacket::materialParameters)},
        {offsetof(glModelPacket, displayList), sizeof(glModelPacket::displayList)},
        {offsetof(glModelPacket, skinnedVertices), 4}, {offsetof(glModelPacket, skinnedNormals), 4}});
    storage.CheckGuards();
}

void Generated()
{
    static_assert(std::is_trivially_destructible_v<glModel>);
    static_assert(std::is_trivially_destructible_v<glModelStream>);
    static_assert(std::is_trivially_destructible_v<glModelPacket>);
    static_assert(sizeof(glModelPacket::skinnedVertices) == 4);
    static_assert(sizeof(glModelPacket::skinnedNormals) == 4);
    constexpr std::array<std::uint32_t, 8> values{0, 1, 0x01020304, 0x89ABCDEF,
        0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xA55A807F};
    const auto value = [&](std::size_t record, std::size_t field) {
        return values[(record * 5 + field) % values.size()];
    };
    for (std::size_t count : {0u, 1u, 2u, 7u, 17u})
    {
        for (std::size_t tail = 0; tail < 12; ++tail)
        {
            Bytes raw(count * 12 + tail, 0xEF);
            Rows rows;
            for (std::size_t i = 0; i < count; ++i)
            {
                Row row{value(i, 0), value(i, 1), value(i, 2)};
                for (std::size_t j = 0; j < row.size(); ++j) Word(raw, i * 12 + j * 4, row[j]);
                rows.push_back(row);
            }
            Models(raw, rows);
        }
        for (std::size_t tail = 0; tail < 8; ++tail)
        {
            Bytes raw(count * 8 + tail, 0xEF);
            Rows rows;
            for (std::size_t i = 0; i < count; ++i)
            {
                Row row{value(i, 0), value(i, 1) % 256, value(i, 2) % 256,
                    value(i, 3) % 256, value(i, 4) % 256};
                Word(raw, i * 8, row[0]);
                for (std::size_t j = 1; j < row.size(); ++j) raw[i * 8 + 3 + j] = row[j];
                rows.push_back(row);
            }
            Streams(raw, rows);
        }
        for (std::size_t tail = 0; tail < 48; ++tail)
        {
            Bytes raw(count * 48 + tail, 0xEF);
            Rows rows;
            for (std::size_t i = 0; i < count; ++i)
            {
                Row row;
                for (std::size_t j = 0; j < 14; ++j) row.push_back(value(i, j));
                row[2] %= 65536;
                row[3] %= 256;
                row[4] %= 256;
                Word(raw, i * 48, row[0]); Word(raw, i * 48 + 4, row[1]);
                Word(raw, i * 48 + 8, row[2], 2);
                raw[i * 48 + 10] = row[3]; raw[i * 48 + 11] = row[4];
                for (std::size_t j = 5; j < 14; ++j) Word(raw, i * 48 + 12 + (j - 5) * 4, row[j]);
                rows.push_back(row);
            }
            Packets(raw, rows);
        }
    }

    for (std::uintptr_t word : {std::uintptr_t(0), std::uintptr_t(8), std::uintptr_t(16),
        std::uintptr_t(0x80000000), std::uintptr_t(0xFFFFFFF8)})
    {
        const auto native = mscharged::platform::RLGNativeStreamOffset(reinterpret_cast<void*>(word));
        Check(native == word / 8 * sizeof(glModelStream), "raw8/native stream offset mapping");
        if (sizeof(void*) > 4 && word == 0xFFFFFFF8)
            Check(native > std::numeric_limits<std::uint32_t>::max(), "stream-table offset above4GiB");
    }
    for (std::uintptr_t word = 1; word < 8; ++word)
    {
        bool rejected = false;
        try { (void)mscharged::platform::RLGNativeStreamOffset(reinterpret_cast<void*>(word)); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "unrepresentable interior stream-record offset");
    }
    bool overflow = false;
    try { (void)mscharged::platform::RLGNativeRecordBytes(std::numeric_limits<std::size_t>::max(), 8, sizeof(glModelStream)); }
    catch (const std::length_error&) { overflow = true; }
    Check(overflow, "native allocation multiplication overflow");
    for (auto [raw, native] : {std::pair<std::size_t, std::size_t>{0, 16}, {8, 7}})
    {
        bool invalid = false;
        try { (void)mscharged::platform::RLGNativeRecordBytes(1, raw, native); }
        catch (const std::invalid_argument&) { invalid = true; }
        Check(invalid, "invalid adapter geometry");
    }
    bool streamOverflow = false;
    try { (void)mscharged::platform::RLGNativeStreamOffset(reinterpret_cast<void*>(std::numeric_limits<std::uintptr_t>::max() & ~std::uintptr_t(7))); }
    catch (const std::length_error&) { streamOverflow = true; }
    Check(streamOverflow, "native stream offset multiplication overflow");

    // Concrete LP64 negative: unadapted source memcpy/size cannot fit the native
    // packet and stores network words without conversion. No object is read
    // from undersized storage and no guessed source pool is created here.
    Check(sizeof(void*) == 8 && sizeof(glModelPacket) == 80, "fixture native64 profile");
    Check(48 < sizeof(glModelPacket), "unadapted packet allocation is undersized");
    Bytes raw(48, 0);
    Word(raw, 4, 0x01020304);
    std::uint32_t nativeWord;
    std::memcpy(&nativeWord, raw.data() + 4, sizeof(nativeWord));
    Check(nativeWord != 0x01020304, "unadapted little-endian packet count negative");
}

void Corpus(const std::filesystem::path& manifest)
{
    std::ifstream in(manifest);
    Check(bool(in), "owned corpus manifest");
    std::string kind, rawName, oracleName;
    while (in >> kind >> rawName >> oracleName)
    {
        std::ifstream bytes(manifest.parent_path() / rawName, std::ios::binary);
        Check(bool(bytes), "owned raw block");
        Bytes raw((std::istreambuf_iterator<char>(bytes)), {});
        std::ifstream oracle(manifest.parent_path() / oracleName);
        Check(bool(oracle), "independent owned oracle");
        std::size_t count, width;
        oracle >> count >> width;
        Check(bool(oracle), "independent oracle header");
        Rows rows(count, Row(width));
        for (auto& row : rows) for (auto& value : row) oracle >> value;
        Check(bool(oracle), "independent oracle fields");
        if (kind == "model") { Check(width == 3, "model oracle width"); Models(raw, rows); }
        else if (kind == "stream") { Check(width == 5, "stream oracle width"); Streams(raw, rows); }
        else if (kind == "packet") { Check(width == 14, "packet oracle width"); Packets(raw, rows); }
        else throw std::runtime_error("Unknown explicit record corpus kind");
        ++corpusCases;
    }
    Check(in.eof(), "owned corpus trailing tokens");
}
}

int main(int argc, char** argv)
{
    try
    {
        Generated();
        if (argc == 2) Corpus(argv[1]);
        else if (argc != 1) throw std::runtime_error("Expected optional independent record corpus manifest");
        std::cout << "original RLG record transport checks=" << checks << " corpus_cases=" << corpusCases
                  << "; source Load/Read/allocation lifetime/GX execution remains unqualified\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "RLG record transport failure after " << checks << ": " << error.what() << '\n';
        return 1;
    }
}

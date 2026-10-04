#include "resources/audio_catalog.h"
#include <fstream>
#include <iostream>
#include <source_location>

using namespace mscharged::resources;
namespace
{
using Data = std::vector<std::uint8_t>;
unsigned checks = 0;
void Check(bool value, const char* message)
{ ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action, std::source_location where = std::source_location::current())
{
    ++checks;
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Malformed audio input accepted at line " + std::to_string(where.line()));
}
void Put(Data& b, std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i != 4; ++i) b.at(offset + i) = std::uint8_t(value >> (24 - 8 * i));
}
Data Words(std::initializer_list<std::uint32_t> words)
{
    Data b(words.size() * 4); std::size_t offset = 0;
    for (auto word : words) { Put(b, offset, word); offset += 4; }
    return b;
}
std::size_t Append(Data& b, std::uint32_t id, const Data& payload)
{
    const auto offset = b.size(); b.resize(offset + 8);
    Put(b, offset, id); Put(b, offset + 4, unsigned(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end()); b.resize((b.size() + 3) & ~std::size_t(3));
    return offset + 8;
}
Data Wrap(std::uint32_t id, const Data& payload)
{ Data b; Append(b, id, payload); return b; }
struct Fixture
{
    Data bytes;
    std::size_t header, groups, refs, slots, names, name;
};
Fixture Banks()
{
    Data table;
    // Serialized addresses deliberately lie above signed 32-bit range. Group0
    // owns slots2,0 in that order; group1 owns slot1. Names have independent IDs.
    const auto header = Append(table, 0x23501, Words({2, 0x10101010, 3, 0xf0000000, 2, 0x20202020}));
    const auto groups = Append(table, 0x23502, Words({41, 0xabcdef01, 2, 0x30303030, 0, 59, 0x12345678, 1, 0, 0}));
    const auto refs = Append(table, 0x23503, Words({0xf0000030, 0xf0000000}));
    Append(table, 0x23503, Words({0xf0000018}));
    const auto slots = Append(table, 0x23504, Words({7, 0xcdef1234, 0, 0, 0, 0, 9, 0xbead1234, 0, 0, 0, 0x000100ff,
        11, 0xfedc4321, 0, 0, 0, 0x00001234}));
    const auto names = Append(table, 0x23505, Words({22, 0, 33, 0}));
    const auto name = Append(table, 0x23506, Data{'S','p','l','a','s','h',0});
    Append(table, 0x23506, Data{'S','t','r','e','a','m','-','2',0});
    Data root; Append(root, 0x4455, Words({0xdeadbeef}));
    const auto section = Append(root, 0x80023500, table);
    Append(root, 0x8877, Words({0x12345678}));
    const auto base = section + 8;
    return {Wrap(0x80000001, root), header + base, groups + base, refs + base, slots + base, names + base, name + base};
}
Fixture Cues()
{
    Data table;
    auto header = Append(table, 0x23001, Words({3, 0xffeeffee, 0}));
    auto records = Append(table, 0x23003, Words({0xde83984e, 0, 0, 0, 31,
        0xde83984e, 0, 0, 1, 77, 0, 12, 34, 56, 19}));
    return {Wrap(0x80000001, Wrap(0x80023000, table)), header + 16, 0, records + 16, 0, 0, 0};
}
void Synthetic()
{
    const auto fixture = Banks(); auto bytes = fixture.bytes;
    const auto banks = ReadAudioBankCatalog(bytes);
    bytes.assign(bytes.size(), 0); bytes.clear(); bytes.shrink_to_fit();
    Check(banks->groups.size() == 2 && banks->slots.size() == 3 && banks->names.size() == 2, "Bank counts changed");
    Check(banks->groups[0].id == 41 && banks->groups[0].hash == 0xabcdef01
        && banks->groups[0].slots == std::vector<std::uint32_t>{2, 0}, "Original group member order changed");
    Check(banks->slots[1].group == 1 && banks->slots[1].streaming && !banks->slots[2].streaming,
        "Slot relocation or source flag changed");
    Check(banks->slots[0].id == 7 && banks->slots[2].hash == 0xfedc4321, "Slot IDs or hashes changed");
    Check(banks->names[0].id == 22 && banks->names[0].name == "Splash" && banks->names[1].name == "Stream-2",
        "Bank filename ownership changed");
    for (std::size_t size = 0; size < fixture.bytes.size(); ++size)
        Reject([&] { ReadAudioBankCatalog(Bytes(fixture.bytes).first(size)); });
    const auto mutate = [&](std::size_t offset, std::uint32_t value) {
        auto bad = fixture.bytes; Put(bad, offset, value); Reject([&] { ReadAudioBankCatalog(bad); });
    };
    mutate(fixture.header, 4097); mutate(fixture.header + 8, 0); mutate(fixture.header + 16, 4097);
    mutate(fixture.header + 12, 0xfffffff0); mutate(fixture.header + 12, 0xf0000001);
    mutate(fixture.refs, 0xf0000001); mutate(fixture.refs, 0xefffffff); mutate(fixture.refs, 0xf0000048);
    mutate(fixture.refs, 0xf0000000); // Duplicate owner within a group.
    mutate(fixture.refs, 0xf0000018); // Duplicate owner across groups.
    mutate(fixture.groups + 8, 1); mutate(fixture.slots + 20, 0x00020000);
    mutate(fixture.names - 8, 0x23504); mutate(fixture.name, 0x2e2e2f78);
    mutate(fixture.name - 8, 0x23505);
    auto bad = fixture.bytes; bad[fixture.name + 6] = 'x'; Reject([&] { ReadAudioBankCatalog(bad); });
    bad = fixture.bytes; bad.push_back(0); Reject([&] { ReadAudioBankCatalog(bad); });
    // Two bank tables are ambiguous even if their content agrees.
    Data duplicate; Append(duplicate, 0x80023500, {}); Append(duplicate, 0x80023500, {});
    bad = Wrap(0x80000001, duplicate); Reject([&] { ReadAudioBankCatalog(bad); });
    Reject([&] { ReadAudioBankCatalog(Wrap(0x80000001, {})); });
    const auto cues_fixture = Cues(); bytes = cues_fixture.bytes;
    const auto cues = ReadAudioCueCatalog(bytes); bytes.clear(); bytes.shrink_to_fit();
    Check(cues->Find({0xde83984e, 0, 0, 0}) == 31 && cues->Find({0xde83984e, 0, 0, 1}) == 77
        && cues->Find({0, 12, 34, 56}) == 19, "SoundMap exact keys changed");
    Check(cues->Find({0xde83984e, 0, 0, 2}) == 0xffff && cues->Find({0, 0, 0, 0}) == 0xffff,
        "Missing SoundMap key did not return original sentinel");
    for (std::size_t size = 0; size < cues_fixture.bytes.size(); ++size)
        Reject([&] { ReadAudioCueCatalog(Bytes(cues_fixture.bytes).first(size)); });
    bad = cues_fixture.bytes; Put(bad, cues_fixture.refs + 20 + 12, 0); Reject([&] { ReadAudioCueCatalog(bad); });
    bad = cues_fixture.bytes; Put(bad, cues_fixture.header, 4097); Reject([&] { ReadAudioCueCatalog(bad); });
    Reject([&] { ReadAudioCueCatalog(fixture.bytes); });
    Reject([&] { ReadAudioBankCatalog(cues_fixture.bytes); });
    // A SoundMap may contain no keys; lookup still returns the source sentinel.
    Data empty; Append(empty, 0x23001, Words({0, 0, 0})); Append(empty, 0x23003, {});
    Check(ReadAudioCueCatalog(Wrap(0x80000001, Wrap(0x80023000, empty)))->Find({1,2,3,4}) == 0xffff,
        "Empty SoundMap changed lookup behavior");
}
Data Read(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open supplied audio metadata");
    return {std::istreambuf_iterator<char>(file), {}};
}
void Owned(const char* banks_path, const char* cues_path)
{
    const auto banks = ReadAudioBankCatalog(Read(banks_path));
    const auto cues = ReadAudioCueCatalog(Read(cues_path));
    Check(banks->groups.size() == 2 && banks->slots.size() == 24 && banks->names.size() == 54, "Owned bank inventory changed");
    Check(banks->groups[0].slots.size() == 21 && banks->groups[1].slots.size() == 3, "Owned group inventory changed");
    Check(banks->names[25].name == "FE_GEN_Splash" && !banks->slots[23].streaming, "Original boot bank profile changed");
    Check(cues->cues.size() == 2 && cues->Find({0xde83984e,0,0,0}) == 1 && cues->Find({0xf394c076,0,0,0}) == 0,
        "Owned boot cue resolution changed");
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc != 1 && argc != 3) throw std::runtime_error("Usage: audio_catalog_tests [nlxgs.bun FE_GEN_Splash.resbun]");
        Synthetic(); if (argc == 3) Owned(argv[1], argv[2]);
        std::cout << "Audio metadata checks: " << checks << '\n'; return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

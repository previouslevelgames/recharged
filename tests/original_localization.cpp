#include "NL/nlLocalization.h"
#include "NL/nlLocalizationLookup.h"
#include "NL/nlLocalization.inl"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include "platform/localization_data.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
namespace {
alignas(32) std::byte standard_fixture_memory[8 * 1024 * 1024];
alignas(32) std::byte virtual_fixture_memory[8 * 1024 * 1024];
std::uint64_t checks;
void Check(bool value, const char* why)
{
    ++checks;
    if (!value) throw std::runtime_error(why);
}
struct Query { std::string name; bool found; std::vector<unsigned short> units; };
struct Case {
    std::string disc, oracle;
    int language; bool game, accepted; std::size_t size;
    std::vector<Query> queries;
};
std::vector<Case> Manifest(const char* path)
{
    std::ifstream input(path); unsigned count;
    input >> count; Check(bool(input), "LOC manifest missing");
    std::vector<Case> cases;
    for (unsigned i = 0; i < count; ++i) {
        Case row; unsigned n;
        input >> std::quoted(row.disc) >> row.language >> row.game >> row.accepted >> row.size >> std::quoted(row.oracle) >> n;
        Check(bool(input), "LOC manifest truncated");
        for (unsigned j = 0; j < n; ++j) {
            Query query; unsigned units;
            input >> query.name >> query.found >> units;
            for (unsigned k = 0; k < units; ++k) {
                unsigned unit; input >> unit; query.units.push_back((unsigned short)unit);
            }
            Check(bool(input), "LOC independent query oracle truncated");
            row.queries.push_back(std::move(query));
        }
        cases.push_back(std::move(row));
    }
    return cases;
}
std::vector<unsigned char> Bytes(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(bool(input), "LOC independent binary oracle missing");
    return {std::istreambuf_iterator<char>(input), {}};
}
std::uint32_t OracleWord(const unsigned char* bytes)
{
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}
unsigned short OracleUnit(const unsigned char* bytes)
{
    return (unsigned short)(unsigned(bytes[0]) | (unsigned(bytes[1]) << 8));
}
struct Memory {
    u32 standard = StandardAllocator.TotalFreeMemory(), virt = VirtualAllocator.TotalFreeMemory();
    u32 standard_largest = StandardAllocator.LargestFreeBlock(), virtual_largest = VirtualAllocator.LargestFreeBlock();
    bool Same() const {
        return standard == StandardAllocator.TotalFreeMemory() && virt == VirtualAllocator.TotalFreeMemory() &&
               standard_largest == StandardAllocator.LargestFreeBlock() && virtual_largest == VirtualAllocator.LargestFreeBlock();
    }
};
bool Owns(const MemoryAllocator& owner, const void* pointer)
{
    const auto base = reinterpret_cast<std::uintptr_t>(owner.m_memory);
    const auto at = reinterpret_cast<std::uintptr_t>(pointer);
    return at >= base && at - base < owner.m_memory_size;
}
template<class Predicate> void Pump(Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        nlServiceFileSystem();
        Check(std::chrono::steady_clock::now() < deadline, "Original localization callback did not complete");
        if (!done()) SDL_Delay(1);
    }
}
void Verify(nlLocalization* localization, const Case& row, MemoryAllocator* owner)
{
    const auto oracle = Bytes(row.oracle);
    Check(oracle.size() == row.size && oracle.size() >= 20, "Independent LOC geometry mismatch");
    const auto* header = localization->m_pFile;
    Check(header && Owns(*owner, header), "Original LOC callback lost its source backing owner");
    if (sizeof(std::uintptr_t) > 4)
        Check(reinterpret_cast<std::uintptr_t>(header) > UINT32_MAX, "Source LOC pointer was truncated to Wii32");
    Check(std::memcmp(header->Thumbprint, oracle.data(), 4) == 0, "LOC thumbprint bytes changed");
    Check(header->Version == OracleWord(oracle.data() + 4), "LOC version changed");
    Check(header->Language == OracleWord(oracle.data() + 8), "LOC language ID changed");
    Check(header->StringCount == OracleWord(oracle.data() + 12), "LOC entry count changed");
    Check(header->Flags == OracleWord(oracle.data() + 16), "LOC authored flags changed");
    Check(localization->GetCurrentLanguage() == row.language, "Original language selection changed");
    Check(localization->m_LookupTable == reinterpret_cast<nlLocalization::StringLookup*>(localization->m_pFile + 1),
          "Original lookup address/20-byte geometry changed");
    Check(localization->m_FirstString == reinterpret_cast<unsigned short*>(localization->m_LookupTable + header->StringCount),
          "Original first-string address/8-byte geometry changed");
    for (u32 i = 0; i < header->StringCount; ++i) {
        Check(localization->m_LookupTable[i].hash == OracleWord(oracle.data() + 20 + 8 * i), "LOC authored hash/order changed");
        Check(localization->m_LookupTable[i].StringOffset == OracleWord(oracle.data() + 24 + 8 * i), "LOC authored offset/alias changed");
    }
    const auto first = std::size_t(20) + 8 * std::size_t(header->StringCount);
    for (std::size_t at = first; at < oracle.size(); at += 2)
        Check(localization->m_FirstString[(at - first) / 2] == OracleUnit(oracle.data() + at), "LOC UTF16 code unit/padding changed");
    // Keep the original empty-count binary search precondition/quirk.
    if (header->StringCount != 0) for (const auto& query : row.queries) {
        const auto* result = localization->GetString(query.name.c_str());
        if (!query.found) Check(result == MissingLocString, "Original missing-string fallback changed");
        else {
            Check(result != MissingLocString && result != LocalizationTableNotFound, "Original lookup rejected an authored entry");
            for (std::size_t i = 0; i < query.units.size(); ++i)
                Check(result[i] == query.units[i], "Original lookup returned different UTF16 units");
        }
    }
}
void One(const Case& row, MemoryAllocator* arena)
{
    const Memory before;
    nlLocalization::Initialize();
    auto* localization = g_pLocalization;
    Check(localization && Owns(StandardAllocator, localization), "Original Initialize allocation changed");
    Check(localization->m_pFile == nullptr && localization->m_LookupTable == nullptr && localization->m_FirstString == nullptr,
          "Original Initialize pointer state changed");
    Check(localization->GetString("missing_query") == LocalizationTableNotFound, "Original missing-table fallback changed");
    const Memory initialized;
    Check(localization->Load((nlLocalization::nlLanguage)row.language, row.game, arena) != 0, "Original LOC request submission failed");
    Check(localization->m_pFile == nullptr && localization->m_LookupTable == nullptr && localization->m_FirstString == nullptr,
          "Original LOC request fabricated callback completion");
    Check(!initialized.Same(), "Original asynchronous request allocated no source backing");
    if (row.accepted) {
        Pump([&] { return localization->m_pFile != nullptr; });
        Verify(localization, row, arena);
        // This is the actual source cleanup operation. Full AsyncLoading and
        // its broader managers are deliberately not run by this data qualifier.
        nlFree(localization->m_pFile);
    } else {
        Pump([&] { return initialized.Same(); });
        Check(localization->m_pFile == nullptr && localization->m_LookupTable == nullptr && localization->m_FirstString == nullptr,
              "Original invalid-header rejection/free decisions changed");
    }
    Check(CurrentAllocator == &StandardAllocator && AllocatorStackDepth == 1, "Original LOC callback changed the allocator stack");
    nlFree(localization); g_pLocalization = nullptr; // Fixture boundary, no source Load reset added.
    Check(before.Same(), "Actual LOC callback/cleanup did not restore exact source arena geometry");
}
void RetainedOldFile(const Case& main, const Case& game)
{
    const Memory before;
    nlLocalization::Initialize(); auto* localization = g_pLocalization;
    Check(localization->Load(nlLocalization::LangEnglish, false, &StandardAllocator) != 0, "Source first LOC request failed");
    Pump([&] { return localization->m_pFile != nullptr; });
    auto* old = localization->m_pFile;
    const auto* begin = reinterpret_cast<const unsigned char*>(old);
    const std::vector<unsigned char> retained(begin, begin + main.size);
    Check(localization->Load(nlLocalization::LangEnglish, true, &VirtualAllocator) != 0, "Source repeated LOC request failed");
    Check(localization->m_pFile == old && !localization->m_LookupTable && !localization->m_FirstString,
          "Host adapter repaired original retained-file/pending-pointer behavior");
    Pump([&] { return localization->m_pFile != old; });
    Verify(localization, game, &VirtualAllocator);
    Check(std::memcmp(old, retained.data(), retained.size()) == 0 && Owns(StandardAllocator, old),
          "Source repeated Load changed or freed its original retained allocation");
    // Explicit fixture cleanup owns both actual source buffers. Source Load
    // still leaves old backing live; no convenience free was added to it.
    nlFree(old); nlFree(localization->m_pFile); nlFree(localization); g_pLocalization = nullptr;
    Check(before.Same(), "Retained-file fixture cleanup did not restore source owner geometry");
}
void TransportGeometry()
{
    Check(sizeof(LOCHeader) == 20 && sizeof(nlLocalization::StringLookup) == 8, "Native LOC storage geometry changed");
    const char* fallback = "Localization Table Not Found";
    for (std::size_t i = 0; i <= std::strlen(fallback); ++i)
        Check(LocalizationTableNotFound[i] == (unsigned char)fallback[i], "Original Wii16 missing-table literal changed");
    fallback = "missing loc string";
    for (std::size_t i = 0; i <= std::strlen(fallback); ++i)
        Check(MissingLocString[i] == (unsigned char)fallback[i], "Original Wii16 missing-string literal changed");
    std::vector<unsigned char> tiny(19, 0xA5);
    bool rejected = false;
    try { mscharged::PrepareWiiLocalizationHeader(tiny.data(), tiny.size()); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected && tiny == std::vector<unsigned char>(19, 0xA5), "Incomplete LOC header was partially rewritten");
    const unsigned char raw[] = { 'N','L','O','C', 0,0,0,1, 0x12,0x34,0x56,0x78, 0,0,0,0, 0x80,0,0,1, 0xD8,0x3D,0xDE,0x42 };
    std::vector<unsigned char> empty(raw, raw + sizeof(raw));
    mscharged::PrepareWiiLocalizationHeader(empty.data(), empty.size());
    mscharged::PrepareWiiLocalizationTable(empty.data(), empty.size(), 0);
    auto* header = reinterpret_cast<const LOCHeader*>(empty.data());
    Check(header->StringCount == 0 && header->Flags == 0x80000001, "Zero-entry authored LOC header was normalized");
    unsigned short pair[2]; std::memcpy(pair, empty.data() + 20, 4);
    Check(pair[0] == 0xD83D && pair[1] == 0xDE42, "Original UTF16 surrogate code units changed");
    auto saved = empty;
    rejected = false;
    try { mscharged::PrepareWiiLocalizationTable(empty.data(), empty.size(), 0xFFFFFFFF); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected && empty == saved, "Out-of-span LOC table was partially rewritten");
    empty.push_back(0x5A); saved = empty; rejected = false;
    try { mscharged::PrepareWiiLocalizationTable(empty.data(), empty.size(), 0); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected && empty == saved, "Partial LOC UTF16 code unit was silently changed");
}
void Run(const std::vector<Case>& cases)
{
    // This minimal provider graph has no game static constructors. The test
    // supplies default source arenas explicitly; production pre-main/module
    // initialization and full game shutdown remain separate gates.
    Check(gMemoryInitialized == 0, "Unexpected game static allocation in minimal LOC graph");
    StandardAllocator.Initialize(standard_fixture_memory, sizeof(standard_fixture_memory));
    VirtualAllocator.Initialize(virtual_fixture_memory, sizeof(virtual_fixture_memory));
    gMemoryInitialized = 1;
    TransportGeometry();
    Check(SDL_Init(0), "SDL metadata initialization failed");
    aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE; aurora::g_config.mem2Size = 0;
    OSInit();
    std::string mounted;
    for (const auto& row : cases) {
        if (row.disc != mounted) {
            if (!mounted.empty()) { nlShutdownFileSystem(); aurora_dvd_close(); }
            Check(aurora_dvd_open(row.disc.c_str()), "LOC synthetic/owned-byte disc mount failed");
            nlInitFileSystem(); mounted = row.disc;
        }
        One(row, &StandardAllocator); One(row, &VirtualAllocator);
    }
    nlShutdownFileSystem(); aurora_dvd_close();
    Check(aurora_dvd_open(cases.at(0).disc.c_str()), "Retained-file source fixture mount failed");
    nlInitFileSystem(); RetainedOldFile(cases.at(0), cases.at(1));
    nlShutdownFileSystem(); aurora_dvd_close(); AuroraOSShutdown(); SDL_Quit();
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("usage: source_localization MANIFEST");
        { const auto cases = Manifest(argv[1]); Run(cases); }
        std::cout << "actual original localization checks=" << checks << '\n' << std::flush;
#if defined(__SANITIZE_ADDRESS__)
        if (__lsan_do_recoverable_leak_check()) return 1;
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

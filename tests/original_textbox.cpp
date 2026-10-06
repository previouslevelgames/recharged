#include "NL/nlTextBox.h"
#include "NL/nlBundleFile.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
namespace {
alignas(32) std::byte standard_memory[8 * 1024 * 1024];
alignas(32) std::byte virtual_memory[8 * 1024 * 1024];
std::uint64_t checks;
void Check(bool condition, const char* reason)
{
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
struct FontCase { std::string disc, bundle, descriptor, oracle; };
std::vector<FontCase> Manifest(const char* path)
{
    std::ifstream input(path); unsigned count;
    input >> count; Check(bool(input), "Text layout manifest missing");
    std::vector<FontCase> result;
    while (count--)
    {
        FontCase row;
        input >> std::quoted(row.disc) >> std::quoted(row.bundle)
              >> std::quoted(row.descriptor) >> std::quoted(row.oracle);
        Check(bool(input), "Text layout manifest truncated");
        result.push_back(std::move(row));
    }
    return result;
}
struct Sample
{
    std::string tag;
    std::array<std::uint32_t, 2> box;
    unsigned long flags;
    bool matrix;
    unsigned count;
    int y;
    std::vector<unsigned short> input, starts;
    std::vector<int> offsets;
};
struct Oracle
{
    unsigned long descriptor_size;
    std::uint64_t descriptor_hash;
    unsigned height, ascent, leading;
    std::uint32_t spacing;
    std::vector<Sample> samples;
    explicit Oracle(const std::string& path)
    {
        std::ifstream input(path); unsigned count;
        input >> descriptor_size >> descriptor_hash >> height >> ascent >> leading >> spacing >> count;
        while (count--)
        {
            Sample row; unsigned length;
            input >> row.tag >> row.box[0] >> row.box[1] >> row.flags
                  >> row.matrix >> row.count >> row.y >> length;
            while (length--) { unsigned value; input >> value; row.input.push_back((unsigned short)value); }
            row.input.push_back(0);
            for (unsigned i = 0; i <= row.count; ++i) { unsigned value; input >> value; row.starts.push_back((unsigned short)value); }
            for (unsigned i = 0; i < row.count; ++i) { int value; input >> value; row.offsets.push_back(value); }
            Check(bool(input), "Independent original text layout oracle truncated");
            samples.push_back(std::move(row));
        }
    }
};
std::uint64_t DataHash(const void* data, std::size_t length)
{
    std::uint64_t hash = 0xcbf29ce484222325;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3;
    return hash;
}
struct Memory
{
    u32 standard = StandardAllocator.TotalFreeMemory(), virt = VirtualAllocator.TotalFreeMemory();
    u32 standard_largest = StandardAllocator.LargestFreeBlock(), virtual_largest = VirtualAllocator.LargestFreeBlock();
    void Same() const
    {
        Check(standard == StandardAllocator.TotalFreeMemory() && virt == VirtualAllocator.TotalFreeMemory(),
              "Original font/text allocation did not restore owning arenas");
        Check(standard_largest == StandardAllocator.LargestFreeBlock() && virtual_largest == VirtualAllocator.LargestFreeBlock(),
              "Original font/text cleanup changed owning arena geometry");
    }
};
struct Callback
{
    void* buffer;
    unsigned long size;
    std::thread::id thread = std::this_thread::get_id();
    unsigned calls = 0;
    static void Complete(void* buffer, unsigned long size, BundleAsyncParam context)
    {
        auto& callback = *reinterpret_cast<Callback*>(context);
        if (sizeof(std::uintptr_t) > 4) Check(context > UINT32_MAX, "Source font callback context was truncated");
        Check(buffer == callback.buffer && size == callback.size, "Original descriptor callback buffer/size changed");
        Check(std::this_thread::get_id() == callback.thread, "Original descriptor callback moved to worker thread");
        ++callback.calls;
    }
};
void Await(Callback& callback)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!callback.calls)
    {
        nlServiceFileSystem();
        Check(std::chrono::steady_clock::now() < deadline, "Original descriptor callback did not finish");
        if (!callback.calls) SDL_Delay(1);
    }
    Check(callback.calls == 1, "Original descriptor callback count changed");
}
void Layout(const nlFont& font, const Sample& sample, MemoryAllocator* owner)
{
    for (bool external : {false, true})
    {
        const Memory before;
        std::vector<unsigned short> storage(sample.input.size());
        CurrentAllocator = owner;
        {
            FontCharString text(sample.input.data(), &font, external ? storage.data() : nullptr);
            Check(text.m_InternalBuffer == !external, "Original FontCharString caller/backing ownership changed");
            if (sizeof(std::uintptr_t) > 4) Check(reinterpret_cast<std::uintptr_t>(text.m_pString) > UINT32_MAX,
                                                 "Original text pointer was truncated");
            struct Guard
            {
                std::array<std::uint64_t, 2> before;
                nlTextBox::StringDrawInfo info;
                std::array<std::uint64_t, 2> after;
            } guard{};
            guard.before.fill(0x1928374655647382); guard.after.fill(0x8273645546372819);
            for (auto& row : guard.info.Rows) { row.XOffset = -23131; row.FirstChar = 0xa5a5; }
            // ProcessString stores this reference without reading the matrix;
            // drawing and matrix transport are separate source providers.
            nlMatrix4 matrix{};
            const auto* matrix_argument = sample.matrix ? &matrix : nullptr;
            const nlVector2 box = {{std::bit_cast<float>(sample.box[0]), std::bit_cast<float>(sample.box[1])}};
            nlTextBox::ProcessString(&text, &font, box, sample.flags, matrix_argument, guard.info);
            if (guard.info.RowCount != sample.count || guard.info.YOffset != sample.y)
                std::cerr << sample.tag << " rows/y actual " << guard.info.RowCount << '/' << guard.info.YOffset
                          << " expected " << sample.count << '/' << sample.y << '\n';
            Check(guard.info.RowCount == sample.count, "Original row boundaries/count differ from authored oracle");
            Check(guard.info.YOffset == sample.y, "Original vertical alignment/narrowing differs");
            Check(guard.info.pFont == &font && guard.info.String == text.m_pString && guard.info.pMatrix == matrix_argument,
                  "Original layout changed its source font/text/matrix references");
            Check(guard.info.DrawOptions == sample.flags, "Original layout changed draw flags");
            for (unsigned i = 0; i <= sample.count; ++i)
            {
                if (guard.info.Rows[i].FirstChar != sample.starts[i])
                    std::cerr << sample.tag << " row " << i << " start actual " << guard.info.Rows[i].FirstChar
                              << " expected " << sample.starts[i] << '\n';
                Check(guard.info.Rows[i].FirstChar == sample.starts[i], "Original row first-character/sentinel differs");
                if (i < sample.count)
                {
                    if (guard.info.Rows[i].XOffset != sample.offsets[i])
                        std::cerr << sample.tag << " row " << i << " x actual " << guard.info.Rows[i].XOffset
                                  << " expected " << sample.offsets[i] << '\n';
                    Check(guard.info.Rows[i].XOffset == sample.offsets[i], "Original horizontal alignment/narrowing differs");
                }
                else Check(guard.info.Rows[i].XOffset == -23131, "Original layout changed untouched sentinel XOffset");
            }
            for (unsigned i = sample.count + 1; i < 17; ++i)
                Check(guard.info.Rows[i].XOffset == -23131 && guard.info.Rows[i].FirstChar == 0xa5a5,
                      "Original layout wrote untouched row storage");
            Check(guard.before == std::array<std::uint64_t, 2>{0x1928374655647382, 0x1928374655647382} &&
                  guard.after == std::array<std::uint64_t, 2>{0x8273645546372819, 0x8273645546372819},
                  "Valid original text layout exceeded supplied native storage");
            // The source buffer owner persists across a different current arena.
            // This fixture does not install ordinary game new/delete aliases.
            CurrentAllocator = owner == &StandardAllocator ? &VirtualAllocator : &StandardAllocator;
        }
        CurrentAllocator = &StandardAllocator;
        before.Same();
    }
}
void Font(const FontCase& row)
{
    const Oracle oracle(row.oracle);
    const Memory initial;
    for (MemoryAllocator* owner : {&StandardAllocator, &VirtualAllocator})
    {
        for (bool async : {false, true})
        {
            CurrentAllocator = owner;
            const Memory before;
            {
                BundleFile bundle;
                Check(bundle.Open(row.bundle.c_str(), false), "Original font bundle open failed");
                BundleFileDirectoryEntry entry{};
                Check(bundle.GetFileInfo(row.descriptor.c_str(), &entry, true), "Original font descriptor not found");
                char* data = static_cast<char*>(nlMalloc(entry.m_length, 32, false));
                if (async)
                {
                    Callback callback{data, entry.m_length};
                    bundle.ReadFileAsync(row.descriptor.c_str(), data, entry.m_length, Callback::Complete,
                                         reinterpret_cast<BundleAsyncParam>(&callback));
                    Await(callback);
                }
                else bundle.ReadFileByIndex(bundle.FindHashIndex(BundleFile::HashFilename(row.descriptor.c_str()), true), data, entry.m_length);
                Check(entry.m_length == oracle.descriptor_size && DataHash(data, entry.m_length) == oracle.descriptor_hash,
                      "Actual NL descriptor bytes differ from independent raw-byte oracle");
                nlFont font;
                Check(font.Load(row.descriptor.c_str(), data, nlStringHash("text-layout-fixture")) == 1,
                      "Original font Load result changed");
                Check(font.m_Metrics.Height == oracle.height && font.m_Metrics.Ascent == oracle.ascent &&
                      font.m_Metrics.InternalLeading == oracle.leading && std::bit_cast<std::uint32_t>(font.m_Metrics.Spacing) == oracle.spacing,
                      "Original descriptor metrics differ from independent fields");
                CurrentAllocator = owner == &StandardAllocator ? &VirtualAllocator : &StandardAllocator;
                nlFree(data);
                for (const auto& sample : oracle.samples) Layout(font, sample, owner);
                CurrentAllocator = owner == &StandardAllocator ? &VirtualAllocator : &StandardAllocator;
            }
            CurrentAllocator = &StandardAllocator;
            before.Same();
        }
    }
    initial.Same();
}
void Run(const std::vector<FontCase>& fonts)
{
    Check(gMemoryInitialized == 0, "Unexpected original game static allocation in minimal text graph");
    StandardAllocator.Initialize(standard_memory, sizeof(standard_memory));
    VirtualAllocator.Initialize(virtual_memory, sizeof(virtual_memory));
    gMemoryInitialized = 1;
    Check(SDL_Init(0), "SDL CPU fixture initialization failed");
    aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE; aurora::g_config.mem2Size = 0;
    OSInit();
    std::string mounted;
    for (const auto& font : fonts)
    {
        if (font.disc != mounted)
        {
            if (!mounted.empty()) { nlShutdownFileSystem(); aurora_dvd_close(); }
            Check(aurora_dvd_open(font.disc.c_str()), "Original text fixture data partition open failed");
            nlInitFileSystem(); mounted = font.disc;
        }
        Font(font);
    }
    nlShutdownFileSystem(); aurora_dvd_close();
    AuroraOSShutdown(); SDL_Quit();
}
} // namespace
int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("usage: original_textbox MANIFEST");
        { const auto fonts = Manifest(argv[1]); Run(fonts); }
        std::cout << "actual original text layout checks=" << checks << '\n' << std::flush;
#if defined(__SANITIZE_ADDRESS__)
        if (__lsan_do_recoverable_leak_check()) return 1;
#endif
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

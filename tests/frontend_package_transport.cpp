// Native FEN transport: pointer conversion, storage discipline and rejection
// cleanup on the independent generated fixture. In the game module the
// ordinary operators allocate from the original game heap, so the transport
// must use only the host metadata interface, and only a few times per package.
#include "platform/frontend_package.h"
#include "Game/FE/feAnimation.h"
#include "Game/FE/feFontResource.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feText.h"
#include "Game/FE/tlSlide.h"
#include "Game/FE/tlTextInstance.h"
#include "generated_offsets.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
unsigned long g_operator_new = 0;
unsigned long g_host_allocations = 0;
long long g_host_live = 0;
struct HostHeader { std::size_t bytes; std::size_t pad; };
}

// Counts ordinary allocations: in the game module these are game-heap requests.
void* operator new(std::size_t bytes)
{
    ++g_operator_new;
    if (void* storage = std::malloc(bytes ? bytes : 1)) return storage;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

// The host metadata interface, as the executable provides it to the module.
extern "C" void* ChargedNativeMetadataAllocate(std::size_t bytes)
{
    auto* header = static_cast<HostHeader*>(std::malloc(sizeof(HostHeader) + bytes));
    if (!header) throw std::bad_alloc();
    header->bytes = bytes;
    ++g_host_allocations;
    g_host_live += static_cast<long long>(bytes);
    return header + 1;
}
extern "C" void ChargedNativeMetadataRelease(void* pointer) noexcept
{
    if (!pointer) return;
    auto* header = static_cast<HostHeader*>(pointer) - 1;
    g_host_live -= static_cast<long long>(header->bytes);
    std::free(header);
}

namespace
{
using mscharged::platform::PrepareFrontendPackage;
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::uint32_t BE32(const std::vector<unsigned char>& raw, std::size_t at)
{
    return (std::uint32_t(raw[at]) << 24) | (std::uint32_t(raw[at + 1]) << 16)
        | (std::uint32_t(raw[at + 2]) << 8) | raw[at + 3];
}
void PutBE32(std::vector<unsigned char>& raw, std::size_t at, std::uint32_t value)
{
    raw[at] = std::uint8_t(value >> 24); raw[at + 1] = std::uint8_t(value >> 16);
    raw[at + 2] = std::uint8_t(value >> 8); raw[at + 3] = std::uint8_t(value);
}
template<class T> T Native(const unsigned char* data, std::size_t at)
{
    T value; std::memcpy(&value, data + at, sizeof(value)); return value;
}

// A rejected input throws std::exception and leaves no host storage behind.
template<class F> void Rejects(F prepare, const char* message)
{
    const auto live = g_host_live;
    bool rejected = false;
    try { prepare(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
    Check(g_host_live == live, "A rejected FEN left host storage allocated");
}

void Accepted(const std::vector<unsigned char>& raw)
{
    const auto live = g_host_live;
    const auto operators = g_operator_new;
    const auto host = g_host_allocations;
    std::vector<std::byte> first;
    {
        auto package = PrepareFrontendPackage(raw.data(), raw.size());
        Check(g_operator_new == operators, "The FEN transport used the ordinary (game-heap) operators");
        Check(g_host_allocations - host <= 64, "The FEN transport allocates per record again");
        const auto* image = static_cast<const unsigned char*>(package.Data());
        first.assign(static_cast<const std::byte*>(package.Data()),
            static_cast<const std::byte*>(package.Data()) + package.Size());

        Check(std::memcmp(image, "FENL", 4) == 0 && Native<std::uint32_t>(image, 4) == 1, "Native FEN header changed");
        const auto data_size = Native<std::uint32_t>(image, 8), table_size = Native<std::uint32_t>(image, 12);
        Check(16 + std::size_t(data_size) + table_size == package.Size(), "Native FEN lengths do not cover the image");
        const auto raw_table = BE32(raw, 12);
        Check(table_size == raw_table / 4 * sizeof(std::uintptr_t), "Native relocation table lost an entry");
        const unsigned char* data = image + 16;

        // Records ascend by raw address; only the UTF-16 suffix alias shares storage.
        std::size_t previous = 0, end = 0;
        bool first_record = true;
        for (const auto& record : package.Records())
        {
            Check(first_record || record.wii_offset > previous, "Native records are not in raw address order");
            Check(record.native_offset + record.native_size <= data_size, "Native record exceeds the image");
            if (!first_record && record.native_offset < end)
                Check(record.wii_offset == gen_suffix, "Native records overlap outside the string alias");
            previous = record.wii_offset;
            end = std::max(end, record.native_offset + record.native_size);
            first_record = false;
        }
        Check(package.NativeOffset(gen_suffix) == package.NativeOffset(gen_string) + 2,
            "UTF-16 suffix alias lost its address identity");

        // Converted pointers hold the target's native offset (or the null word).
        auto pointer = [&](std::uint32_t record, std::size_t field) {
            return Native<std::uintptr_t>(data, package.NativeOffset(record) + field);
        };
        Check(pointer(gen_package, offsetof(FEPackage, m_pFEPresentation)) == package.NativeOffset(gen_presentation),
            "Package presentation pointer was not converted");
        Check(pointer(gen_package, offsetof(FEPackage, m_pComponentList)) == 0xffffffffu,
            "Relocated null pointer lost its null word");
        Check(pointer(gen_presentation, offsetof(FEPresentation, m_slides)) == package.NativeOffset(gen_slide_b),
            "Presentation slide pointer was not converted");
        Check(pointer(gen_slide_a, offsetof(TLSlide, m_animations)) == package.NativeOffset(gen_float_anim),
            "Slide animation pointer was not converted");
        Check(pointer(gen_float_anim, offsetof(FEAnimation, m_DLRingHead)) == package.NativeOffset(gen_float1),
            "Animation key pointer was not converted");
        const auto draw = offsetof(TLTextInstance, m_DrawInfo);
        Check(pointer(gen_text, draw + offsetof(nlTextBox::StringDrawInfo, String)) == package.NativeOffset(gen_string),
            "Text string pointer was not converted");
        Check(pointer(gen_text, draw + offsetof(nlTextBox::StringDrawInfo, pMatrix)) == package.NativeOffset(gen_matrix),
            "Text matrix pointer was not converted");
        Check(pointer(gen_text, offsetof(TLTextInstance, m_wcUserString)) == package.NativeOffset(gen_suffix),
            "Text suffix pointer was not converted");
        Check(pointer(gen_font, offsetof(FEFontResource, m_pFontReference)) == 0,
            "Unrelocated external pointer did not stay zero");

        // Each relocation entry names a slot that holds a record offset or the null word.
        for (std::size_t i = 0; i < raw_table / 4; ++i)
        {
            const auto slot = Native<std::uintptr_t>(data, data_size + i * sizeof(std::uintptr_t));
            Check(slot + sizeof(std::uintptr_t) <= data_size, "Native relocation slot exceeds the image");
            const auto value = Native<std::uintptr_t>(data, slot);
            bool record_start = value == 0xffffffffu;
            for (const auto& record : package.Records()) record_start = record_start || value == record.native_offset;
            Check(record_start, "Native relocation slot does not hold a record offset");
        }
    }
    Check(g_host_live == live, "The FEN transport leaked host storage");
    auto again = PrepareFrontendPackage(raw.data(), raw.size());
    Check(again.Size() == first.size() && std::memcmp(again.Data(), first.data(), first.size()) == 0,
        "The FEN transport is not deterministic");
}
}

int main(int argc, char** argv)
{
    try
    {
        Check(argc == 2, "Generated raw FEN required");
        std::ifstream in(argv[1], std::ios::binary);
        const std::vector<unsigned char> raw{std::istreambuf_iterator<char>(in), {}};
        Check(raw.size() > 16 && BE32(raw, 0) == 0x46454e4cu, "Generated raw FEN is missing");
        Accepted(raw);

        const std::size_t data = BE32(raw, 8), table = BE32(raw, 12);
        for (std::size_t size : {std::size_t(0), std::size_t(12), raw.size() - 1})
            Rejects([&] { PrepareFrontendPackage(raw.data(), size); }, "A truncated FEN was accepted");
        auto bad = raw;
        PutBE32(bad, 4, 2);
        Rejects([&] { PrepareFrontendPackage(bad.data(), bad.size()); }, "An unsupported FEN version was accepted");
        bad = raw;
        PutBE32(bad, 16 + data + 4, BE32(bad, 16 + data));
        Rejects([&] { PrepareFrontendPackage(bad.data(), bad.size()); }, "A duplicate relocation slot was accepted");
        bad = raw;
        PutBE32(bad, 16 + data, std::uint32_t(data));
        Rejects([&] { PrepareFrontendPackage(bad.data(), bad.size()); }, "An out-of-range relocation slot was accepted");
        bad = raw;
        PutBE32(bad, 16 + gen_package + 4, gen_presentation + 2);
        Rejects([&] { PrepareFrontendPackage(bad.data(), bad.size()); }, "An unaligned record was accepted");
        bad = raw;
        PutBE32(bad, 16 + gen_lib_group + 0x74, 9);
        Rejects([&] { PrepareFrontendPackage(bad.data(), bad.size()); }, "An unknown library record type was accepted");
        bad = raw;
        PutBE32(bad, 16 + gen_package + 4, gen_slide_a);
        Rejects([&] { PrepareFrontendPackage(bad.data(), bad.size()); }, "A conflicting typed alias was accepted");

        // Arbitrary corruption either converts or throws, never leaks.
        std::mt19937 random(0x46454e4c);
        unsigned accepted = 0, rejected = 0;
        for (unsigned i = 0; i < 2000; ++i)
        {
            auto mutated = raw;
            const unsigned flips = 1 + random() % 3;
            for (unsigned f = 0; f < flips; ++f)
                mutated[16 + random() % (data + table)] ^= std::uint8_t(1u << (random() % 8));
            const auto live = g_host_live;
            try { PrepareFrontendPackage(mutated.data(), mutated.size()); ++accepted; }
            catch (const std::exception&) { ++rejected; }
            Check(g_host_live == live, "A corrupted FEN left host storage allocated");
        }
        std::cout << "Native FEN transport: " << checks << " checks, mutations " << accepted
                  << " converted / " << rejected << " rejected\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

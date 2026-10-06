#include "Game/Font/fontmanager.h"
#include "Game/FE/feResourceManager.h"
#include "Game/FE/feFontResource.h"
#include "NL/nlFont.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/glx/glxTexture.h"
#include "Game/GL/GLInventory.h"
#include "Game/TweakRegistry.h"
#include "NL/nlFileGC.h"
#include "NL/nlBundleFile.h"
#include "NL/nlString.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
namespace {
std::uint64_t checks;
std::uint64_t service_polls;
void Check(bool condition, const char* reason)
{
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
struct Memory
{
    u32 standard = StandardAllocator.TotalFreeMemory(), virt = VirtualAllocator.TotalFreeMemory();
    u32 slargest = StandardAllocator.LargestFreeBlock(), vlargest = VirtualAllocator.LargestFreeBlock();
    void Same() const
    {
        Check(standard == StandardAllocator.TotalFreeMemory() && virt == VirtualAllocator.TotalFreeMemory(),
              "Original font loading did not restore exact owning arenas");
        Check(slargest == StandardAllocator.LargestFreeBlock() && vlargest == VirtualAllocator.LargestFreeBlock(),
              "Original font loading cleanup fragmented source arenas");
    }
};
struct FontCase
{
    std::string disc, bundle, descriptor, alias, oracle;
    bool concurrent;
};
struct Texture
{
    u32 hash, page, effect, width, height, levels, format, palette_count, bits, missing;
    std::size_t data_bytes;
    std::uint64_t data_hash, palette_hash;
};
struct Oracle
{
    unsigned long descriptor_size;
    std::uint64_t descriptor_hash;
    u32 alias_hash, pages, texture_type, height, ascent, leading, spacing;
    std::vector<Texture> textures;
    explicit Oracle(const std::string& path)
    {
        std::ifstream input(path); unsigned count;
        input >> descriptor_size >> descriptor_hash >> alias_hash >> pages >> texture_type
              >> height >> ascent >> leading >> spacing >> count;
        while (count--)
        {
            Texture texture{};
            input >> texture.hash >> texture.page >> texture.effect >> texture.width >> texture.height
                  >> texture.levels >> texture.format >> texture.data_bytes >> texture.data_hash
                  >> texture.palette_count >> texture.palette_hash >> texture.bits >> texture.missing;
            Check(bool(input), "Independent Wii bundle/GX page oracle truncated");
            textures.push_back(texture);
        }
    }
};
std::vector<FontCase> Manifest(const char* path)
{
    std::ifstream input(path); unsigned count;
    input >> count; Check(bool(input), "Original font loading manifest missing");
    std::vector<FontCase> result;
    while (count--)
    {
        FontCase row{};
        input >> std::quoted(row.disc) >> std::quoted(row.bundle) >> std::quoted(row.descriptor)
              >> std::quoted(row.alias) >> std::quoted(row.oracle) >> row.concurrent;
        Check(bool(input), "Original font loading manifest truncated");
        result.push_back(std::move(row));
    }
    return result;
}
std::uint64_t DataHash(const void* data, std::size_t size)
{
    auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 0xcbf29ce484222325;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3;
    return hash;
}
void AuditRawDescriptor(const FontCase& row, const Oracle& oracle)
{
    BundleFile bundle;
    Check(bundle.Open(row.bundle.c_str(), false), "Actual original bundle open failed");
    BundleFileDirectoryEntry entry{};
    Check(bundle.GetFileInfo(row.descriptor.c_str(), &entry, true), "Requested descriptor key missing in original bundle");
    Check(entry.m_length == oracle.descriptor_size, "Original descriptor length changed");
    std::vector<unsigned char> raw(entry.m_length);
    bundle.ReadFileByIndex(bundle.FindHashIndex(entry.m_hash), raw.data(), raw.size());
    Check(DataHash(raw.data(), raw.size()) == oracle.descriptor_hash,
          "Actual requested descriptor bytes differ from independent raw oracle");
    bundle.Close();
}
void Await(FontManager& manager)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!manager.IsLoadingComplete())
    {
        nlServiceFileSystem();
        ++service_polls;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Actual source all-page completion did not finish");
        if (!manager.IsLoadingComplete()) SDL_Delay(1);
    }
}
void FEFontReference(FEResourceManager& resources, unsigned long hash, nlFont* expected)
{
    FEFontResource handle{};
    handle.m_type = FERT_FONT;
    handle.m_hashID = hash;
    Check(!handle.IsValid(), "Fixture font resource did not start in its serialized invalid state");
    resources.QueueResourceLoad(&handle, &VirtualAllocator);
    Check(!handle.IsValid(), "Original queue fabricated font validity before Run");
    resources.Run(0.0f);
    Check(handle.IsValid() && handle.GetFontReference() == expected,
          "Actual FE font request reference/fallback/validity differs");
    resources.UnloadResource(&handle);
    Check(handle.IsValid() && handle.GetFontReference() == expected,
          "Original FE font unload behavior was repaired");
}
void PrimeOriginalFEQueue()
{
    // The original global pending queue retains its256-slot block until CRT
    // teardown. Execute one real empty lookup before per-load memory baselines;
    // keep that source allocation live, rather than add a host queue/free helper.
    FontManager fonts;
    FontManager::s_pInstance = &fonts;
    FEResourceManager resources;
    FEResourceManager::s_pInstance = &resources;
    FEFontReference(resources, 0x12345678, nullptr);
    resources.Cleanup();
    FEResourceManager::s_pInstance = nullptr;
    FontManager::s_pInstance = nullptr;
}
std::vector<nlFont*> Fonts(FontManager& manager)
{
    std::vector<nlFont*> fonts;
    auto it = manager.m_fonts.Begin();
    auto* current = it.m_Curr;
    while (current)
    {
        fonts.push_back(current->entry);
        current = nlDLRingIsEnd(it.m_Head, current) ? nullptr : current->m_next;
    }
    return fonts;
}
void VerifyFont(const nlFont& font, const Oracle& oracle)
{
    Check(font.m_Metrics.FontName == oracle.alias_hash, "Original manager alias lowercase/hash changed");
    Check(font.m_PageCount == oracle.pages && u32(font.m_TextureType) == oracle.texture_type,
          "Original requested font page/type selection changed");
    Check(font.m_Metrics.Height == oracle.height && font.m_Metrics.Ascent == oracle.ascent
          && font.m_Metrics.InternalLeading == oracle.leading && std::bit_cast<u32>(font.m_Metrics.Spacing) == oracle.spacing,
          "Actual descriptor loader changed independent metrics");
    for (const auto& expected : oracle.textures)
    {
        const auto* hashes = expected.effect ? font.m_EffectTextureHandles : font.m_TextureHandles;
        Check(hashes[expected.page] == expected.hash, "Original descriptor texture name/page/effect hash changed");
        auto* texture = glGetCurrentResourcePool()->m_inventory->GetTexture(expected.hash);
        Check(texture != nullptr, "Source completion reported success without every requested texture");
        Check(texture == glGetTextureManager()->mTextures[texture->m_TextureIndex],
              "Source inventory/texture-manager registration disagrees");
        Check(texture->m_Width == expected.width && texture->m_Height == expected.height
              && texture->m_Levels == expected.levels && u32(texture->m_Format) == expected.format,
              "Original texture platform metadata differs from raw fields");
        Check(texture->m_NativeDataBytes == expected.data_bytes && texture->m_NativePaletteBytes == expected.palette_count * 2,
              "Actual native retained GX storage geometry differs");
        Check(DataHash(texture->m_SwizzledData, texture->m_NativeDataBytes) == expected.data_hash,
              "Original page callback changed authored tiled bytes");
        Check(DataHash(texture->m_PaletteData, texture->m_NativePaletteBytes) == expected.palette_hash,
              "Original page callback changed authored palette bytes");
        const u32 texture_bits = (u32(texture->m_Bits[0]) << 24) | (u32(texture->m_Bits[1]) << 16)
                               | (u32(texture->m_Bits[2]) << 8) | u32(texture->m_Bits[3]);
        Check(texture_bits == expected.bits && texture->m_bMissingTexture == bool(expected.missing),
              "Source texture missing/bit fields changed");
        if (sizeof(std::uintptr_t) > 4)
            Check(reinterpret_cast<std::uintptr_t>(texture->m_SwizzledData) > UINT32_MAX,
                  "Actual original texture callback truncated its native pool pointer");
    }
}
void Group(const std::vector<FontCase>& rows, MemoryAllocator& owner)
{
    std::cout << "font callback group=" << rows.front().descriptor << " count=" << rows.size()
              << " owner=" << (&owner == &StandardAllocator ? "Standard" : "Virtual") << '\n' << std::flush;
    const Memory before;
    CurrentAllocator = &owner;
    glInitTextureManager(128);
    auto* textures = glGetTextureManager();
    glInitResourcePools();
    const GLMemoryRequirement budgets[] = {{GLM_Header, 1024 * 1024}, {GLM_TextureData, 8 * 1024 * 1024}};
    auto* pool = glCreateResourcePool(budgets, 2, "original-font-loading-fixture");
    glSetCurrentResourcePool(pool);
    const auto marker = pool->MarkResource();
    FontManager::s_pInstance = new (8, false) FontManager;
    auto& manager = *FontManager::Instance();
    FEResourceManager resources;
    FEResourceManager::s_pInstance = &resources;
    Check(manager.IsLoadingComplete(), "Actual source initial font slots not idle");
    Check(manager.m_pResourcePool == pool, "Actual FontManager lost source current pool");
    Check(manager.GetFontByHashID(0x12345678) == nullptr, "Original empty font fallback was fabricated");
    FEFontReference(resources, 0x12345678, nullptr);
    std::vector<Oracle> oracles;
    for (const auto& row : rows)
    {
        oracles.emplace_back(row.oracle);
        AuditRawDescriptor(row, oracles.back());
        std::cout << "load=" << row.descriptor << '\n' << std::flush;
        Check(manager.LoadFont(row.bundle.c_str(), row.descriptor.c_str(), row.alias.c_str()),
              "Original source LoadFont result changed");
        Check(!manager.IsLoadingComplete(), "Original font request bypassed genuine async completion");
        if (!row.concurrent) Await(manager);
    }
    Await(manager);
    const auto registered = Fonts(manager);
    Check(registered.size() == rows.size(), "Actual callback did not register exactly the requested fonts");
    for (unsigned i = 0; i < oracles.size(); ++i)
    {
        const auto* font = manager.GetFontByHashID(oracles[i].alias_hash);
        Check(font != nullptr, "Original requested font alias missing");
        VerifyFont(*font, oracles[i]);
        FEFontReference(resources, oracles[i].alias_hash, const_cast<nlFont*>(font));
    }
    Check(manager.GetFontByHashID(0x12345678) == registered.front(),
          "Original first-registered fallback was replaced");
    FEFontReference(resources, 0x12345678, registered.front());
    resources.Cleanup();
    FEResourceManager::s_pInstance = nullptr;
    // Shift current arena after load: class-scoped object frees and slot blocks
    // must still use the exact original allocation owners.
    CurrentAllocator = &owner == &StandardAllocator ? &VirtualAllocator : &StandardAllocator;
    delete FontManager::s_pInstance; FontManager::s_pInstance = nullptr;
    pool->ReleaseResource(marker);
    Check(pool->m_inventory->m_nLevel == 0 && pool->m_level == 0, "Original resource release levels changed");
    for (const auto& oracle : oracles)
        for (const auto& expected : oracle.textures)
            Check(pool->m_inventory->GetTexture(expected.hash) == nullptr,
                  "Original resource release retained a loaded font page");
    Check(textures->mFreeIndices->mCount == 128, "Actual texture release did not restore all source indices");
    glDestroyResourcePool(pool);
    Check(glGetResourcePools() == nullptr && glGetCurrentResourcePool() == nullptr,
          "Original resource-pool/ring destruction changed");
    nlDeleteGameObject(textures->mFreeIndices); nlFree(textures->mIndexBuffer); nlFree(textures->mTextures);
    nlDeleteGameObject(textures); gTextureManager = nullptr;
    CurrentAllocator = &StandardAllocator;
    before.Same();
}
void Run(const std::vector<FontCase>& rows)
{
    Check(SDL_Init(0), "SDL CPU fixture initialization failed");
    aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE; aurora::g_config.mem2Size = 0;
    OSInit();
    PrimeOriginalFEQueue();
    std::string mounted;
    for (unsigned i = 0; i < rows.size();)
    {
        if (rows[i].disc != mounted)
        {
            if (!mounted.empty()) { nlShutdownFileSystem(); aurora_dvd_close(); }
            Check(aurora_dvd_open(rows[i].disc.c_str()), "Original font fixture data partition open failed");
            nlInitFileSystem(); mounted = rows[i].disc;
        }
        std::vector<FontCase> group{rows[i++]};
        // Source FontManager::m_fonts(8) has delta0 despite16 load-state slots.
        // Respect its actual registered-font capacity; do not grow it in a patch.
        while (group.front().concurrent && group.size() < 8 && i < rows.size()
               && rows[i].concurrent && rows[i].disc == mounted)
            group.push_back(rows[i++]);
        for (MemoryAllocator* owner : {&StandardAllocator, &VirtualAllocator}) Group(group, *owner);
    }
    nlShutdownFileSystem(); aurora_dvd_close(); AuroraOSShutdown(); SDL_Quit();
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("usage: original_font_loading MANIFEST");
        Check(gMemoryInitialized == 1, "Explicit private fixture arenas did not precede real statics");
        Check(IsTweakRegistryInitialized() == 0 && gPendingTweakHead && gPendingTweakTail,
              "Original texture static constructor/real tweak queue was replaced");
        { const auto rows = Manifest(argv[1]); Run(rows); }
        std::cout << "actual original font manager and FE checks=" << checks
                  << " service_polls=" << service_polls << '\n' << std::flush;
#if defined(__SANITIZE_ADDRESS__)
        if (__lsan_do_recoverable_leak_check()) std::_Exit(1);
#endif
        // Original StringBlockAllocator's inherited null branch on CRT teardown
        // is separately recorded. Only explicitly tested lifetimes end here.
        std::_Exit(0);
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n' << std::flush; std::_Exit(1); }
}

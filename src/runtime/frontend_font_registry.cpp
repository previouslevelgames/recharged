#include "runtime/frontend_font_registry.h"
#include "runtime/frontend_font_packets.h"
#include "runtime/views.h"
#include "Game/GL/GLInventory.h"
#include "NL/FontTextState.h"
#include "NL/gl/gl.h"
#include "NL/gl/glDraw2.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <set>
#include <thread>

namespace mscharged
{
namespace
{
bool Linked(const GLResourcePool* pool)
{
    const auto* first = glGetResourcePools();
    if (!first) return false;
    auto* item = first;
    do { if (item == pool) return true; item = item->m_next; } while (item != first);
    return false;
}

}
struct FrontendFontRegistry::Implementation
{
    struct Page { const resources::Texture* source; PlatTexture* texture = nullptr; std::uint16_t index = 0xffff; };
    struct Font { std::shared_ptr<const resources::FrontendFont> owner; std::vector<Page> pages; };
    GLResourcePool& pool;
    void (*drain)();
    std::map<std::uint32_t, Font> fonts;
    std::thread::id thread = std::this_thread::get_id();
    GLResourceMark mark = 0;
    int level = 0;
    std::optional<std::uint64_t> pending;
    bool busy = false, failed = false;
    Implementation(GLResourcePool& p, std::span<const std::shared_ptr<const resources::FrontendFont>> input, void (*d)())
        : pool(p), drain(d)
    {
        if (!drain || !Linked(&pool) || !glGetTextureManager() || glNativeViewDispatchActive() || glIsFrameActive())
            throw std::logic_error("Font registration requires an idle initialized graphics pool and real drain callback");
        resources::Require(!input.empty() && input.size() <= 16, "Font registration requires one to 16 aliases");
        std::set<std::uint32_t> hashes;
        std::size_t bytes = 0;
        for (const auto& owner : input)
        {
            resources::Require(owner && !fonts.contains(owner->alias), "Missing font or duplicate font alias");
            // Check the same bounded metrics/glyph contract used by original
            // text measurement, including forged public resource records.
            resources::LayoutFrontendTextLine(owner, u"");
            Font font{owner, {}};
            for (const auto& page : owner->pages)
            {
                resources::Require(page.id != 0xffffffffU && hashes.insert(page.id).second,
                    "Font pages require distinct authored texture hashes");
                resources::Require(glGetTextureIndex(page.id) == 0xffff, "Font page hash is already registered");
                resources::Require(page.gx_format == 9 && page.game_format == 8 && page.levels == 1
                    && page.palette_entries && page.palette_entries <= 256
                    && page.palette.size() == std::size_t(page.palette_entries) * 2
                    && page.pixels.size() == std::size_t((page.width + 7) / 8) * ((page.height + 3) / 4) * 32,
                    "Unsupported or malformed CI8 font page");
                bytes += page.pixels.size() + page.palette.size();
                resources::Require(bytes <= 32 * 1024 * 1024, "Registered font pages exceed 32 MiB");
                font.pages.push_back({&page});
            }
            fonts.emplace(owner->alias, std::move(font));
        }
        mark = pool.MarkResource(); level = pool.m_level;
        try
        {
            for (auto& [alias, font] : fonts) for (auto& page : font.pages)
            {
                const auto& source = *page.source;
                page.texture = new (pool.Allocate(sizeof(PlatTexture), GLM_Header)) PlatTexture;
                auto& texture = *page.texture;
                texture.m_Width = source.width; texture.m_Height = source.height;
                texture.m_Levels = texture.m_MaxLevel = source.levels;
                texture.m_Format = static_cast<eGXTextureFormat>(source.game_format);
                texture.m_nPaletteEntries = source.palette_entries;
                std::copy(source.bits.begin(), source.bits.end(), texture.m_Bits);
                texture.m_SwizzledData = const_cast<std::uint8_t*>(source.pixels.data());
                texture.m_PaletteData = reinterpret_cast<u16*>(const_cast<std::uint8_t*>(source.palette.data()));
                texture.m_NativeDataBytes = source.pixels.size(); texture.m_NativePaletteBytes = source.palette.size();
                glRegisterTexture(source.id, &texture, &pool);
                page.index = texture.m_TextureIndex;
                if (page.index == 0xffff || page.index >= glGetTextureManager()->mCapacity
                    || glGetTextureManager()->mTextures[page.index] != &texture || glGetTextureIndex(source.id) != page.index)
                    throw std::logic_error("Font page registration did not establish a real texture slot");
            }
        }
        catch (...) { pool.ReleaseResource(mark); mark = 0; throw; }
    }
    void CheckThread() const
    {
        if (thread != std::this_thread::get_id() || busy)
            throw std::logic_error("Font registry requires its nonrecursive owner thread");
    }
    void CheckStorage() const
    {
        if (!mark || !Linked(&pool) || !glGetTextureManager() || pool.m_level < level)
            throw std::logic_error("Font graphics ownership is inactive");
        if (glNativeViewDispatchActive()) throw std::logic_error("Font registry cannot mutate a dispatching view");
        for (const auto& [alias, font] : fonts) for (const auto& page : font.pages)
        {
            if (pool.m_inventory->GetTexture(page.source->id) != page.texture
                || page.index >= glGetTextureManager()->mCapacity
                || glGetTextureManager()->mTextures[page.index] != page.texture
                || glGetTextureIndex(page.source->id) != page.index
                || page.texture->m_SwizzledData != page.source->pixels.data()
                || page.texture->m_PaletteData != reinterpret_cast<const u16*>(page.source->palette.data())
                || page.texture->m_NativeDataBytes != page.source->pixels.size()
                || page.texture->m_NativePaletteBytes != page.source->palette.size())
                throw std::logic_error("Registered font page ownership was replaced or mutated");
        }
    }
    void Check() const { CheckThread(); CheckStorage(); }
    void Release()
    {
        CheckThread();
        if (!mark) return;
        CheckStorage();
        if (pending || glIsFrameActive() || pool.m_level != level)
            throw std::logic_error("Finish font frames and nested pool owners before release");
        busy = true;
        try
        {
            drain(); CheckStorage();
            if (glIsFrameActive() || pool.m_level != level) throw std::logic_error("Font drain changed pool/frame ownership");
            pool.ReleaseResource(mark); mark = 0; fonts.clear(); busy = false;
        }
        catch (...) { busy = false; throw; }
    }
    ~Implementation() { try { Release(); } catch (...) { std::terminate(); } }
};
FrontendFontRegistry::FrontendFontRegistry(GLResourcePool& pool,
    std::span<const std::shared_ptr<const resources::FrontendFont>> fonts, void (*drain)())
    : impl_(std::make_unique<Implementation>(pool, fonts, drain)) {}
FrontendFontRegistry::~FrontendFontRegistry() = default;
bool FrontendFontRegistry::Active() const { impl_->CheckThread(); return impl_->mark != 0; }
std::shared_ptr<const resources::FrontendFont> FrontendFontRegistry::Find(std::uint32_t alias) const
{
    impl_->Check();
    const auto found = impl_->fonts.find(alias);
    if (found == impl_->fonts.end()) throw std::out_of_range("Font alias is not registered");
    return found->second.owner;
}
std::uint16_t FrontendFontRegistry::PageIndex(std::uint32_t alias, unsigned page) const
{
    impl_->Check();
    return impl_->fonts.at(alias).pages.at(page).index;
}
void FrontendFontRegistry::Release() { impl_->Release(); }
void FrontendFontRegistry::FinishFrame()
{
    impl_->Check();
    if (!impl_->pending) return;
    if (glIsFrameActive() || *impl_->pending == glNativeFrameGeneration())
        throw std::logic_error("Font frame must be sent or cancelled before finishing");
    const auto generation = glNativeFrameGeneration();
    impl_->busy = true;
    try
    {
        impl_->drain(); impl_->CheckStorage();
        if (glIsFrameActive() || generation != glNativeFrameGeneration())
            throw std::logic_error("Font drain changed the original frame");
        impl_->pending.reset(); impl_->failed = false; impl_->busy = false;
    }
    catch (...) { impl_->busy = false; throw; }
}
unsigned FrontendFontRegistry::Submit(GLView& view, const resources::FontLayout& layout, const nlMatrix4& model,
    std::array<std::uint8_t, 4> colour, int layer)
{
    impl_->Check();
    if (!glIsFrameActive() || impl_->failed || (impl_->pending && *impl_->pending != glNativeFrameGeneration())
        || !view.m_Interface || view.m_NativeIterating)
        throw std::logic_error("Font submission requires a collecting frame with earlier frames finished");
    resources::Require(layout.font && layout.quads.size() <= 4096 && layer >= 0, "Invalid font polygon batch");
    const auto found = impl_->fonts.find(layout.font->alias);
    resources::Require(found != impl_->fonts.end() && found->second.owner == layout.font,
        "Font layout does not retain its registered alias owner");
    const auto packets = detail::PrepareFontPackets(layout, model, colour);
    if (packets.quads.empty()) return 0;
    impl_->busy = true;
    struct Leave { bool& busy; ~Leave() { busy = false; } } leave{impl_->busy};
    impl_->pending = glNativeFrameGeneration(); // Any later allocation can fail.
    struct FailedSubmission
    {
        bool& failed;
        int exceptions = std::uncaught_exceptions();
        ~FailedSubmission() { if (std::uncaught_exceptions() > exceptions) failed = true; }
    } failed_submission{impl_->failed};
    detail::AttachFontPackets(view, packets, layer);
    return static_cast<unsigned>(packets.quads.size());
}
}

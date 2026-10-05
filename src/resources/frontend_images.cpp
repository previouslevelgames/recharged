#include "resources/frontend_images.h"
#include <algorithm>
#include <set>
#include <sstream>

namespace mscharged::resources
{
namespace
{
constexpr std::uint32_t Hash(std::string_view text)
{
    std::uint32_t result = 0xffffffff;
    for (unsigned char c : text) result = result * 33 + c;
    return result;
}
}
bool IsDynamicFrontendImage(std::uint32_t hash)
{ return hash == Hash("movie") || hash == Hash("target/grab_texture"); }
std::vector<FrontendImageBundleEntry> ReadFrontendImageDirectory(Bytes bytes)
{
    Require(bytes.size() <= MaximumAssetBytes, "Frontend image bundle exceeds 16 MiB");
    const auto sector = U32(bytes, 0), count = U32(bytes, 4);
    Require(sector == 32 && count <= 4096, "Unsupported frontend image bundle directory");
    const auto directory = std::uint64_t(U32(bytes, 8)) * sector;
    const auto data = std::uint64_t(U32(bytes, 12)) * sector;
    // Shipped empty bundles contain only the 16-byte header. The directory/data
    // fields both name the next sector, absent because there are no records.
    if (!count)
    {
        Require(bytes.size() == 16 && directory == 32 && data == 32, "Invalid empty frontend image bundle");
        return {};
    }
    Require(directory >= 16 && directory <= bytes.size() && data <= bytes.size()
        && data >= directory + std::uint64_t(count) * 12, "Invalid frontend image directory range");
    const auto table = Slice(bytes, std::size_t(directory), std::size_t(count) * 12);
    std::vector<FrontendImageBundleEntry> result;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto block = U32(table, i * 12 + 4), length = U32(table, i * 12 + 8);
        const auto offset = std::uint64_t(block) * sector;
        Require(offset >= data && offset <= bytes.size() && length, "Invalid frontend image entry range");
        Slice(bytes, std::size_t(offset), length);
        result.push_back({U32(table, i * 12), block, length, std::size_t(offset)});
        ranges.emplace_back(offset, offset + length);
    }
    std::sort(ranges.begin(), ranges.end());
    for (unsigned i = 1; i < ranges.size(); ++i)
        Require(ranges[i].first >= ranges[i - 1].second, "Overlapping frontend image entries");
    return result;
}
FrontendImageCatalog::Handle ReadFrontendImages(const FrontendScene& scene, std::span<const FrontendImageBundle> bundles)
{
    Require(scene.resources.size() <= 16384 && bundles.size() <= 8, "Frontend image request exceeds its limits");
    auto result = std::make_shared<FrontendImageCatalog>();
    std::set<std::uint32_t> required;
    for (const auto& resource : scene.resources)
    {
        Require(resource.type <= 2, "Unknown frontend resource type");
        if (resource.type != 0) continue;
        if (IsDynamicFrontendImage(resource.hash))
            result->unavailable.emplace(resource.hash, "Dynamic frontend image producer is not selected");
        else required.insert(resource.hash);
    }
    std::size_t total = 0, retained = 0;
    bool on_demand = false;
    for (const auto& bundle : bundles)
    {
        Require(!on_demand, "On-demand frontend bundle must be unique and last");
        Require(bundle.kind == FrontendImageBundleKind::Permanent || bundle.kind == FrontendImageBundleKind::OnDemand,
            "Unknown frontend image bundle kind");
        on_demand = bundle.kind == FrontendImageBundleKind::OnDemand;
        Require(bundle.bytes.size() <= 32 * 1024 * 1024 - total, "Frontend image bundle batch exceeds 32 MiB");
        total += bundle.bytes.size();
        const auto entries = ReadFrontendImageDirectory(bundle.bytes);
        for (const auto& entry : entries)
        {
            if (!required.contains(entry.hash) || result->textures.contains(entry.hash)) continue;
            auto texture = std::make_shared<Texture>(ReadTexture(Slice(bundle.bytes, entry.offset, entry.length), entry.hash));
            const auto size = texture->pixels.size() + texture->palette.size();
            Require(size <= MaximumAssetBytes - retained, "Frontend image decoded textures exceed 16 MiB");
            retained += size;
            result->textures.emplace(entry.hash, std::move(texture));
        }
    }
    for (const auto hash : required)
        if (!result->textures.contains(hash))
        {
            std::ostringstream message;
            message << "Frontend image hash 0x" << std::hex << hash << " is absent from the selected bundle profile";
            throw std::runtime_error(message.str());
        }
    return result;
}
FrontendImageCatalog::Handle ReadPermanentFrontendImages(Bytes bundle)
{
    auto result = std::make_shared<FrontendImageCatalog>();
    std::size_t retained = 0;
    for (const auto& entry : ReadFrontendImageDirectory(bundle))
    {
        if (result->textures.contains(entry.hash)) continue;
        Require(entry.hash != 0xffffffffU && !IsDynamicFrontendImage(entry.hash),
            "Permanent frontend bundle contains an unsupported texture identity");
        Require(result->textures.size() < 1024, "Permanent frontend bundle exceeds 1024 textures");
        auto texture = std::make_shared<Texture>(ReadTexture(Slice(bundle, entry.offset, entry.length), entry.hash));
        const auto size = texture->pixels.size() + texture->palette.size();
        Require(size <= 64 * 1024 * 1024 - retained, "Permanent frontend textures exceed 64 MiB");
        retained += size;
        result->textures.emplace(entry.hash, std::move(texture));
    }
    for (const auto hash : {Hash("movie"), Hash("target/grab_texture")})
        result->unavailable.emplace(hash, "Dynamic frontend image producer is not selected");
    return result;
}
void RequireFrontendImages(const FrontendScene& scene, const FrontendImageCatalog& catalog)
{
    Require(scene.resources.size() <= 16384, "Frontend resource request exceeds its limits");
    for (const auto& resource : scene.resources)
    {
        Require(resource.type <= 2, "Unknown frontend resource type");
        if (resource.type != 0 || IsDynamicFrontendImage(resource.hash)) continue;
        const auto found = catalog.textures.find(resource.hash);
        Require(found != catalog.textures.end() && found->second && found->second->id == resource.hash,
            "Frontend static image is absent from the retained resource profile");
    }
}
}

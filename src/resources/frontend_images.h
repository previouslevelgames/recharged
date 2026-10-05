#pragma once
#include "resources/frontend_scene.h"
#include "resources/texture_bundle.h"
#include <map>
#include <memory>
#include <string>

namespace mscharged::resources
{
struct FrontendImageCatalog
{
    using Handle = std::shared_ptr<const FrontendImageCatalog>;
    std::map<std::uint32_t, std::shared_ptr<const Texture>> textures;
    // Dynamic render targets require their actual producer; never a placeholder.
    std::map<std::uint32_t, std::string> unavailable;
};
enum class FrontendImageBundleKind { Permanent, OnDemand };
struct FrontendImageBundle { Bytes bytes; FrontendImageBundleKind kind; };
struct FrontendImageBundleEntry { std::uint32_t hash, block, length; std::size_t offset; };
// BundleFile's Wii records are 12 bytes, independent of native unsigned long.
// Offsets are absolute block * sector, NOT relative to the data-sector field.
std::vector<FrontendImageBundleEntry> ReadFrontendImageDirectory(Bytes bytes);
bool IsDynamicFrontendImage(std::uint32_t hash);
// The FEN resource ring is the requested set, including resources not presently
// visible. Saved valid/file_block fields and export pointers are not readiness.
// Permanent bundles are in original load order, followed by at most one
// on-demand bundle. Existing/first directory hashes win. Missing static hashes
// fail explicitly; fonts and scene contexts are outside this image catalog.
FrontendImageCatalog::Handle ReadFrontendImages(const FrontendScene& scene,
    std::span<const FrontendImageBundle> bundles);
// Original permanent loading visits every directory entry, retaining the first
// occurrence of a hash. No on-demand profile is implicitly made permanent.
FrontendImageCatalog::Handle ReadPermanentFrontendImages(Bytes bundle);
// Validate the complete resource ring, including hidden static references.
void RequireFrontendImages(const FrontendScene&, const FrontendImageCatalog&);
}

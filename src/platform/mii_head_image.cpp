#include "platform/disc.h"
#include "platform/mii_head.h"

namespace mscharged::platform::mii {
MiiParts ReadMiiPartsFromImage(const std::filesystem::path& image)
{
    // The archive the game's MiiManager loads (MiiManager::LoadResources):
    // 0.7 MB, with the same parts on every version (USA, Europe rev 1 and 2,
    // Japan, Korea).
    const auto archive = ReadDiscFile(image, "RFLRes01.arc", 8u << 20);
    return MiiParts::FromArchive(archive);
}
} // namespace mscharged::platform::mii

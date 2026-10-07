#pragma once

#include <cstdint>
#include <filesystem>

namespace mscharged::platform {
struct NativeFilesystemSettings {
    std::filesystem::path root;
    // Actual disc TMD identity, explicitly supplied by native boot setup.
    std::uint64_t title_id;
    // The configured virtual IOS process identity; retained with title backing.
    std::uint32_t uid;
    std::uint16_t gid;
};
// Install actual persistent backing before the original ISFS/NAND initializers.
// No existing game save, banner, Mii data, or original ready flag is generated.
void InitializeNativeFilesystem(NativeFilesystemSettings settings);
// Reject pending/active original callbacks before actual physical handle close.
void ShutdownNativeFilesystem();
}

#pragma once

#include "platform/filesystem_device.h"

namespace mscharged::platform {
struct NativeFilesystemBootSettings {
    std::filesystem::path disc;
    std::filesystem::path backing_root;
    // Explicit virtual IOS title-process UID. It is independent of the host
    // account and is retained by the existing persistent ownership catalog.
    // Kernel/IOS IDs and System Menu UID0x1000 are not a game process.
    std::uint32_t uid;
};
// Native owner setup before source NANDInit. Reads actual disc TMD title/group
// and installs physical storage only. Does not initialize NAND or game saves.
NativeFilesystemSettings InitializeNativeFilesystemForDisc(
    const NativeFilesystemBootSettings& settings);
}

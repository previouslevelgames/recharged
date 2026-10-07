#include "platform/filesystem_boot.h"
#include "platform/disc.h"

#include <stdexcept>

namespace mscharged::platform {
NativeFilesystemSettings InitializeNativeFilesystemForDisc(
    const NativeFilesystemBootSettings& settings)
{
    if (settings.disc.empty() || settings.backing_root.empty() || settings.uid <= 0x1000)
        throw std::invalid_argument("Native storage boot requires a disc, backing and explicit title-process UID above0x1000");
    NativeFilesystemSettings filesystem{
        settings.backing_root, ReadDiscTitleId(settings.disc), settings.uid,
        ReadDiscTitleGroupId(settings.disc)};
    InitializeNativeFilesystem(filesystem);
    return filesystem;
}
}

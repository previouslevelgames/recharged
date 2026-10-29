#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace mscharged::platform {
// Host loader services only. PE resolves every normal import before any module
// constructor runs. ELF/Mach-O retain their current RTLD_LAZY boundary.
// This does not supply missing source/SDK definitions or admit a game profile.
// A path whose file name names one of the program's static game images
// (cmake/StaticGameImages.cmake) loads that image instead of a file.
void* LoadNativeModule(const std::filesystem::path& path);
// Whether LoadNativeModule can load the path: a static game image or a file.
bool NativeModuleAvailable(const std::filesystem::path& path);
// Windows: keeps [base, base + size) reserved until the next LoadNativeModule,
// which frees it for the module whose fixed image base it is. The host
// allocates bottom-up into the same low 2 GB, and a fullscreen graphics device
// at a high 3D resolution had taken the game module's place before the module
// loaded (#98). False if the range was not free; elsewhere a no-op.
bool HoldNativeModuleBase(std::uintptr_t base, std::size_t size) noexcept;
void* FindNativeModuleSymbol(void* handle, const char* name) noexcept;
bool ReleaseNativeModule(void* owned_reference) noexcept;

struct NativeModuleImage {
    std::uintptr_t base{};
    std::filesystem::path path;
};
// These borrowed observations require the caller's live load/reference lease.
// A data symbol resolved through another image is not owned by the chosen DLL.
bool QueryNativeModuleImage(const void* address, NativeModuleImage& image) noexcept;
bool QueryNativeModuleHandle(void* handle, NativeModuleImage& image) noexcept;
void* RetainNativeModuleImage(const std::filesystem::path& path,
                             std::uintptr_t base) noexcept;
bool NativeModuleOwnsReadableExtent(std::uintptr_t base, const void* address,
                                   std::size_t bytes) noexcept;

// A real, opened source-file incarnation precedes module initialization.
// RequireUnchanged also checks the named path; retaining an old inode is not
// enough after replacement. On Windows the open handle forbids writes/delete.
// Fingerprint is ownership evidence, not a security signature.
class NativeModuleFile {
public:
    explicit NativeModuleFile(const std::filesystem::path& path);
    ~NativeModuleFile();
    NativeModuleFile(const NativeModuleFile&) = delete;
    NativeModuleFile& operator=(const NativeModuleFile&) = delete;
    const std::filesystem::path& Path() const noexcept;
    const std::string& Identity() const noexcept;
    void RequireUnchanged() const;
    bool OwnsImage(const NativeModuleImage& image) const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}

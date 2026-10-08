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
void* LoadNativeModule(const std::filesystem::path& path);
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

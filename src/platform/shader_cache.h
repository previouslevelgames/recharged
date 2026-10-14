#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace mscharged::platform {

// Shared by the original runtime and launcher; independent of the NAND setting.
std::filesystem::path ShaderCacheDirectory(const std::filesystem::path& executable_directory);

// Hold before Aurora opens either cache, until after Aurora has shut down,
// including exception cleanup. Multiple game instances may hold shared leases;
// clearing requires exclusive access. The OS releases a crashed process's lease.
class ShaderCacheUse {
public:
    explicit ShaderCacheUse(const std::filesystem::path& cache_directory);
    ~ShaderCacheUse();
    ShaderCacheUse(ShaderCacheUse&&) noexcept;
    ShaderCacheUse& operator=(ShaderCacheUse&&) noexcept;
    ShaderCacheUse(const ShaderCacheUse&) = delete;
    ShaderCacheUse& operator=(const ShaderCacheUse&) = delete;

private:
    struct State;
    std::unique_ptr<State> state_;
};

struct ShaderCacheClearResult {
    std::size_t removed_files = 0;
    std::uintmax_t removed_bytes = 0;
};

// Remove only the two renderer databases and their SQLite sidecars. Never walks
// subdirectories. Missing caches are successful; busy, symlink/special-file and
// I/O failures throw readable exceptions. Leaves NAND, logs and settings alone.
ShaderCacheClearResult ClearShaderCache(const std::filesystem::path& cache_directory);

} // namespace mscharged::platform

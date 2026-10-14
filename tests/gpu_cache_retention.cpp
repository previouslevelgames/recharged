#include "internal.hpp"
#include "webgpu/gpu.hpp"

#include <array>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace aurora::webgpu { void cache_shutdown(); }

int main(int argc, char** argv) {
  if (argc > 2) return 2;
  const auto directory = argc == 2 ? std::filesystem::path(argv[1]) :
      std::filesystem::temp_directory_path() / ("mscharged-cache-retention-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (std::filesystem::exists(directory)) {
    std::cerr << "Retention fixture requires a fresh directory\n";
    return 2;
  }
  if (!std::filesystem::create_directories(directory)) return 2;
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(directory, error); }
  } cleanup{directory};
  const auto cachePath = directory.string();
  aurora::g_config.cachePath = cachePath.c_str();
  const std::array<const char*, 4> keys{
      "backend-vulkan-device-a-aa1", "backend-d3d12-device-a-aa1",
      "backend-vulkan-device-a-aa4", "backend-vulkan-device-b-aa1"};
  const std::array<std::array<std::byte, 5>, 4> values{{
      {std::byte{1}, std::byte{3}, std::byte{5}, std::byte{7}, std::byte{9}},
      {std::byte{2}, std::byte{4}, std::byte{6}, std::byte{8}, std::byte{10}},
      {std::byte{11}, std::byte{13}, std::byte{15}, std::byte{17}, std::byte{19}},
      {std::byte{12}, std::byte{14}, std::byte{16}, std::byte{18}, std::byte{20}},
  }};
  using namespace aurora::webgpu;
  const auto verify = [&](size_t i) {
    std::array<std::byte, 5> loaded{};
    if (load_from_cache(keys[i], std::strlen(keys[i]), loaded.data(), loaded.size(), nullptr) != loaded.size()
        || loaded != values[i]) throw std::runtime_error("Earlier launch's cache entry was lost");
  };
  try {
    for (size_t i = 0; i < keys.size(); ++i) {
      store_to_cache(keys[i], std::strlen(keys[i]), values[i].data(), values[i].size(), nullptr);
    }
    cache_shutdown();
    // New launch selects only one backend/device/AA combination. Unvisited
    // entries must survive both completion maintenance and another reopen.
    verify(0);
    cache_prune();
    cache_shutdown();
    for (size_t i = 0; i < keys.size(); ++i) verify(i);
    cache_shutdown();
    // Empty-use startup maintenance must also preserve every entry.
    cache_prune();
    cache_shutdown();
    for (size_t i = 0; i < keys.size(); ++i) verify(i);
    cache_shutdown();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    cache_shutdown();
    return 1;
  }
  std::cout << "Persistent GPU blobs survive backend/device/AA and unused launches\n";
  return 0;
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mscharged::platform
{
// SHA-256 (FIPS 180-4), for checking downloaded packages and their files.
class Sha256
{
public:
    Sha256();
    void Update(const void* data, std::size_t size);
    // The digest as 64 lowercase hex digits; the object is spent afterwards.
    std::string FinishHex();

private:
    void Block(const std::uint8_t* block);
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

std::string Sha256Hex(std::string_view data);
// Throws std::runtime_error when the file cannot be read.
std::string Sha256File(const std::filesystem::path& file);
} // namespace mscharged::platform

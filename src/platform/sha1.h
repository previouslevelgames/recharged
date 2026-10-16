#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mscharged::platform
{
// SHA-1 (FIPS 180-4), for recognising a release's main.dol by the hash the
// decompilation records for it. Not for anything that needs a secure hash.
class Sha1
{
public:
    Sha1();
    void Update(const void* data, std::size_t size);
    // The digest as 40 lowercase hex digits; the object is spent afterwards.
    std::string FinishHex();

private:
    void Block(const std::uint8_t* block);
    std::array<std::uint32_t, 5> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

std::string Sha1Hex(std::string_view data);
std::string Sha1Hex(const void* data, std::size_t size);
} // namespace mscharged::platform

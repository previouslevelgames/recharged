#pragma once

#include <cstddef>
#include <cstdint>

namespace mscharged::platform::wii_disc
{
// AES-128 decryption (FIPS-197) in CBC mode, for Wii title keys and partition
// clusters. Portable tables built from the S-box at first use; on ARMv8 with
// the Crypto Extensions (the Switch's Cortex-A57) the AES instructions.
class Aes128Decryptor
{
public:
    explicit Aes128Decryptor(const std::uint8_t key[16]);

    // out = AES-CBC-decrypt(in) with `iv`; size is a multiple of 16. `in` and
    // `out` may be the same buffer, but must not overlap otherwise.
    void DecryptCbc(const std::uint8_t iv[16], const std::uint8_t* in, std::uint8_t* out, std::size_t size) const;

private:
    // Round keys of the equivalent inverse cipher, in decryption order: as
    // big-endian words for the tables and as bytes for the AES instructions.
    std::uint32_t words_[44];
    alignas(16) std::uint8_t bytes_[11][16];
};
} // namespace mscharged::platform::wii_disc

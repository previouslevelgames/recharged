#pragma once

#include <cstddef>
#include <cstdint>

namespace mscharged::platform {

// Original ordinary Strikers2 payload: UserInfo/rules, three Cup records,
// source Cup progress, and StrikerChallenge unlocks. No online-slot format.
inline constexpr std::size_t OriginalNormalSavePayloadBytes = 35578;

// Byte/bitfield transport only. The original game still serializes its live
// records and runs its own checksum before accepting a loaded payload. Encode
// uses the logical35578-byte payload; decode also accepts the original35608-byte
// payload capacity after its8-byte header, leaving the final30 bytes untouched.
void EncodeOriginalSavePayload(void*, std::size_t payload_bytes, bool online);
void DecodeOriginalSavePayload(void*, std::size_t payload_bytes, bool online);
std::uint32_t OriginalSaveWord(std::uint32_t);

} // namespace mscharged::platform

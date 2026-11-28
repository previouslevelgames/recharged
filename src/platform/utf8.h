#pragma once

// UTF-8 decoding for text that comes from players (names): one strict decoder
// instead of a copy per check.

#include <cstddef>
#include <optional>
#include <string_view>

namespace mscharged::platform
{
// The code point at `at` in `text`, `at` moved past it; none (and `at`
// unchanged) for anything but well-formed UTF-8: a stray or missing
// continuation byte, an overlong form, a surrogate or a value past U+10FFFF.
inline std::optional<char32_t> NextCodepoint(std::string_view text, std::size_t& at)
{
    if (at >= text.size()) return std::nullopt;
    const auto lead = static_cast<unsigned char>(text[at]);
    const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3
                             : (lead & 0xF8) == 0xF0                   ? 4
                                                                       : 0;
    if (!length || at + length > text.size()) return std::nullopt;
    char32_t c = length == 1 ? lead : lead & (0x7F >> length);
    for (std::size_t k = 1; k < length; ++k)
    {
        const auto next = static_cast<unsigned char>(text[at + k]);
        if ((next & 0xC0) != 0x80) return std::nullopt;
        c = (c << 6) | (next & 0x3F);
    }
    constexpr char32_t kSmallest[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (c < kSmallest[length] || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return std::nullopt;
    at += length;
    return c;
}
} // namespace mscharged::platform

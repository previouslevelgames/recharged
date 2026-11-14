#pragma once

#include <optional>
#include <string>
#include <string_view>

// Standard base64 (RFC 4648, with '=' padding), as the profile service sends
// bytes in JSON (a Mii's record).
namespace mscharged::platform
{
inline std::string Base64Encode(std::string_view bytes)
{
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t n = 0; n < bytes.size(); n += 3)
    {
        const std::size_t left = bytes.size() - n;
        const unsigned value = (static_cast<unsigned char>(bytes[n]) << 16)
            | (left > 1 ? static_cast<unsigned char>(bytes[n + 1]) << 8 : 0)
            | (left > 2 ? static_cast<unsigned char>(bytes[n + 2]) : 0);
        text += kDigits[(value >> 18) & 63];
        text += kDigits[(value >> 12) & 63];
        text += left > 1 ? kDigits[(value >> 6) & 63] : '=';
        text += left > 2 ? kDigits[value & 63] : '=';
    }
    return text;
}

// The bytes of `text`, or nothing when it is not padded base64.
inline std::optional<std::string> Base64Decode(std::string_view text)
{
    if (text.size() % 4 != 0) return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (std::size_t n = 0; n < text.size(); n += 4)
    {
        const bool last = n + 4 == text.size();
        const int pad = last ? (text[n + 3] == '=') + (text[n + 2] == '=') : 0;
        if (pad == 1 && text[n + 2] == '=') return std::nullopt; // "x=" before a digit
        unsigned value = 0;
        for (int k = 0; k < 4; ++k)
        {
            const int d = k >= 4 - pad ? 0 : digit(text[n + std::size_t(k)]);
            if (d < 0) return std::nullopt;
            value = (value << 6) | unsigned(d);
        }
        bytes += char((value >> 16) & 0xFF);
        if (pad < 2) bytes += char((value >> 8) & 0xFF);
        if (pad < 1) bytes += char(value & 0xFF);
    }
    return bytes;
}
} // namespace mscharged::platform

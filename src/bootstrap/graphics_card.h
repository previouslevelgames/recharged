#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mscharged
{
// A graphics card by its PCI vendor and device ID, which Vulkan and DirectX
// both report.
struct GraphicsCardId
{
    std::uint32_t vendor = 0;
    std::uint32_t device = 0;

    bool operator==(const GraphicsCardId& other) const { return vendor == other.vendor && device == other.device; }
    bool operator!=(const GraphicsCardId& other) const { return !(*this == other); }
};

// display.gpu: "auto" (none: the fastest card) or a card as "vendor:device"
// in hexadecimal, such as "10de:1f99". std::invalid_argument for anything else.
inline std::optional<GraphicsCardId> ParseGraphicsCard(std::string_view setting)
{
    if (setting == "auto") return std::nullopt;
    auto hex = [](std::string_view text, std::uint32_t& value) {
        if (text.size() != 4) return false;
        value = 0;
        for (const char c : text)
        {
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return false;
            value = value * 16 + std::uint32_t(digit);
        }
        return true;
    };
    GraphicsCardId id;
    if (setting.size() != 9 || setting[4] != ':' || !hex(setting.substr(0, 4), id.vendor)
        || !hex(setting.substr(5), id.device) || id.vendor == 0)
        throw std::invalid_argument("Graphics card '" + std::string(setting)
            + "' is neither auto nor a PCI ID such as 10de:1f99.");
    return id;
}

// The display.gpu value for a card (PCI IDs are 16-bit).
inline std::string GraphicsCardSetting(GraphicsCardId id)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%04x:%04x", unsigned(id.vendor), unsigned(id.device));
    return text;
}
}

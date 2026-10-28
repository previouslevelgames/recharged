#include "platform/mii_maker.h"
#include "platform/rfl_random_mii.h"

#include <random>
#include <stdexcept>

namespace mscharged::platform {
namespace {
constexpr std::size_t kNameChars = 10, kRecordBytes = 0x4A;
// Create IDs count 4-second steps from 2006-01-01 (Unix 1136073600) in their
// low 28 bits; the top bit marks an ordinary Mii, neither special (golden
// pants) nor temporary (RFLi_CREATE_ID_MASK_*, RFLi_Config.h).
constexpr std::int64_t kCreateEpoch = 1136073600;
constexpr std::uint32_t kNotSpecial = 0x80000000u, kCounterMask = 0x0FFFFFFFu;

// The draws of the Wii's random face (its OSGetTick), from one seeded engine.
std::mt19937 g_random;

// UTF-8 to UTF-16 cells of the basic plane; empty for anything else.
std::u16string Utf16(const std::string& text) {
    std::u16string cells;
    for (std::size_t n = 0; n < text.size();) {
        const auto lead = static_cast<unsigned char>(text[n]);
        const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 0;
        if (!length || n + length > text.size()) return {};
        std::uint32_t c = length == 1 ? lead : lead & (length == 2 ? 0x1F : 0x0F);
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[n + k]);
            if ((next & 0xC0) != 0x80) return {};
            c = (c << 6) | (next & 0x3F);
        }
        // Overlong forms and surrogate halves are not characters.
        if ((length == 2 && c < 0x80) || (length == 3 && c < 0x800) || (c >= 0xD800 && c < 0xE000)) return {};
        cells.push_back(char16_t(c));
        n += length;
    }
    return cells;
}
} // namespace

bool ValidMiiName(const std::string& name) {
    const auto cells = Utf16(name);
    if (cells.empty() || cells.size() > kNameChars) return false;
    for (const char16_t c : cells)
        if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return false;
    return true;
}

std::string MakeRandomMii(const std::string& name, std::uint32_t seed, std::int64_t created) {
    if (!ValidMiiName(name))
        throw std::invalid_argument("A Mii's name has 1 to 10 characters.");
    const auto cells = Utf16(name);
    unsigned short wii_name[kNameChars]{};
    for (std::size_t n = 0; n < cells.size(); ++n) wii_name[n] = cells[n];

    g_random.seed(seed);
    // The counter's 4-second steps, then four bytes in place of the
    // console's own: a different console for every Mii made here.
    const auto steps = std::uint32_t((created > kCreateEpoch ? created - kCreateEpoch : 0) / 4);
    const std::uint32_t id = kNotSpecial | (steps & kCounterMask), console = std::uint32_t(g_random());
    const unsigned char create_id[8] = {
        static_cast<unsigned char>(id >> 24),      static_cast<unsigned char>(id >> 16),
        static_cast<unsigned char>(id >> 8),       static_cast<unsigned char>(id),
        static_cast<unsigned char>(console >> 24), static_cast<unsigned char>(console >> 16),
        static_cast<unsigned char>(console >> 8),  static_cast<unsigned char>(console)};

    std::string record(kRecordBytes, '\0');
    ChargedMakeRandomMiiRecord(wii_name, create_id, record.data());
    return record;
}
} // namespace mscharged::platform

extern "C" unsigned int ChargedMiiRandomTick(void) {
    return static_cast<unsigned int>(mscharged::platform::g_random());
}

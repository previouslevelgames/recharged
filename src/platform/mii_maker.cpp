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
// A key's bits: FNV-1a over its bytes, spread by SplitMix64's finalizer.
// Stable everywhere, which is all a Mii's key needs.
std::uint64_t Mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::array<std::uint64_t, 2> KeyBits(const std::string& key) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    const auto first = Mix(hash);
    return {first, Mix(first)};
}

// UTF-8 to code points; nullopt for invalid UTF-8.
bool CodePoints(const std::string& text, std::u32string& points) {
    for (std::size_t n = 0; n < text.size();) {
        const auto lead = static_cast<unsigned char>(text[n]);
        const std::size_t length = lead < 0x80             ? 1
                                   : (lead & 0xE0) == 0xC0 ? 2
                                   : (lead & 0xF0) == 0xE0 ? 3
                                   : (lead & 0xF8) == 0xF0 ? 4
                                                           : 0;
        if (!length || n + length > text.size()) return false;
        std::uint32_t c = length == 1 ? lead : lead & (0x7F >> length);
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[n + k]);
            if ((next & 0xC0) != 0x80) return false;
            c = (c << 6) | (next & 0x3F);
        }
        points.push_back(c);
        n += length;
    }
    return true;
}

// Characters a Mii's name can show: the basic plane without control,
// surrogate, private-use and invisible format characters.
bool Showable(char32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0) || c > 0xFFFF) return false;
    if (c >= 0xD800 && c < 0xF900) return false; // surrogates, private use
    if (c == 0xAD || (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F)
        || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF || (c >= 0xFFF0))
        return false;
    return true;
}

std::string Fitted(const std::string& name) {
    std::u32string points;
    if (!CodePoints(name, points)) return {};
    std::u32string kept;
    for (const char32_t c : points) {
        const bool space = c == U' ' || c == U'\u00A0' || c == U'\u3000' || c == U'\t';
        if (space) {
            if (!kept.empty() && kept.back() != U' ') kept.push_back(U' ');
        } else if (Showable(c)) {
            kept.push_back(c);
        }
    }
    if (kept.size() > kNameChars) kept.resize(kNameChars);
    while (!kept.empty() && kept.back() == U' ') kept.pop_back();
    std::string fitted;
    for (const char32_t c : kept) {
        if (c < 0x80) {
            fitted += char(c);
        } else if (c < 0x800) {
            fitted += char(0xC0 | (c >> 6));
            fitted += char(0x80 | (c & 0x3F));
        } else {
            fitted += char(0xE0 | (c >> 12));
            fitted += char(0x80 | ((c >> 6) & 0x3F));
            fitted += char(0x80 | (c & 0x3F));
        }
    }
    return fitted;
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
std::array<unsigned char, 8> KeyedMiiCreateId(const std::string& key) {
    const auto bits = KeyBits(key)[1];
    std::array<unsigned char, 8> id{};
    for (std::size_t n = 0; n < id.size(); ++n) id[n] = static_cast<unsigned char>(bits >> (56 - 8 * n));
    // An ordinary Mii: not special (top bit set), not temporary, and the
    // other two flag bits clear as in every create ID the Wii makes.
    id[0] = static_cast<unsigned char>(0x80 | (id[0] & 0x0F));
    return id;
}

std::string MakeKeyedMii(const std::string& name, const std::string& key) {
    if (!ValidMiiName(name))
        throw std::invalid_argument("A Mii's name has 1 to 10 characters.");
    const auto cells = Utf16(name);
    unsigned short wii_name[kNameChars]{};
    for (std::size_t n = 0; n < cells.size(); ++n) wii_name[n] = cells[n];
    const auto create_id = KeyedMiiCreateId(key);
    g_random.seed(std::uint32_t(KeyBits(key)[0] >> 32));
    std::string record(kRecordBytes, '\0');
    ChargedMakeRandomMiiRecord(wii_name, create_id.data(), record.data());
    return record;
}

std::string FitMiiName(const std::string& name, const std::string& fallback) {
    for (const auto* candidate : {&name, &fallback})
        if (auto fitted = Fitted(*candidate); !fitted.empty()) return fitted;
    return "Player";
}
} // namespace mscharged::platform

extern "C" unsigned int ChargedMiiRandomTick(void) {
    return static_cast<unsigned int>(mscharged::platform::g_random());
}

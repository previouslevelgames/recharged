// New Miis (launcher): the records must be what the game's Mii library
// accepts, read back through the port's own record decoder.
#include "platform/mii_maker.h"
#include "platform/rfl_character_transport.h"
#include <RVLFaceLib/RFLi_Config.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace mscharged::platform;
unsigned checks{};
void Check(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

RFLiCharData Decode(const std::string& record) {
    Check(record.size() == 74, "Mii record size");
    RFLiCharData data;
    ChargedDecodeRFLCharRecordBE(record.data(), &data);
    return data;
}

// RFLiCheckValidInfo (RFL_DataUtility.c) for the USA and European games.
bool ValidForGame(const RFLiCharData& d) {
    return d.faceType <= RFLi_MAX_FACELINE_TYPE && d.faceColor <= RFLi_MAX_FACELINE_COLOR
        && d.faceTex <= RFLi_MAX_FACELINE_TEXTURE && d.hairType <= RFLi_MAX_HAIR_TYPE
        && d.hairColor <= RFLi_MAX_HAIR_COLOR && d.eyeType <= RFLi_MAX_EYE_TYPE && d.eyeColor <= RFLi_MAX_EYE_COLOR
        && d.eyeScale <= RFLi_MAX_EYE_SCALE && d.eyeRotate <= RFLi_MAX_EYE_ROTATE && d.eyeX <= RFLi_MAX_EYE_X
        && d.eyeY <= RFLi_MAX_EYE_Y && d.eyebrowType <= RFLi_MAX_EYEBROW_TYPE
        && d.eyebrowColor <= RFLi_MAX_EYEBROW_COLOR && d.eyebrowScale <= RFLi_MAX_EYEBROW_SCALE
        && d.eyebrowRotate <= RFLi_MAX_EYEBROW_ROTATE && d.eyebrowX <= RFLi_MAX_EYEBROW_X
        && d.eyebrowY >= RFLi_MIN_EYEBROW_Y && d.eyebrowY <= RFLi_MAX_EYEBROW_Y && d.noseType <= RFLi_MAX_NOSE_TYPE
        && d.noseScale <= RFLi_MAX_NOSE_SCALE && d.noseY <= RFLi_MAX_NOSE_Y && d.mouthType <= RFLi_MAX_MOUTH_TYPE
        && d.mouthColor <= RFLi_MAX_MOUTH_COLOR && d.mouthScale <= RFLi_MAX_MOUTH_SCALE
        && d.mouthY <= RFLi_MAX_MOUTH_Y && d.mustacheType <= RFLi_MAX_BEARD_MUSTACHE
        && d.beardType <= RFLi_MAX_BEARD_TYPE && d.beardColor <= RFLi_MAX_BEARD_COLOR
        && d.beardScale <= RFLi_MAX_BEARD_SCALE && d.beardY <= RFLi_MAX_BEARD_Y && d.glassType <= RFLi_MAX_GLASS_TYPE
        && d.glassColor <= RFLi_MAX_GLASS_COLOR && d.glassScale <= RFLi_MAX_GLASS_SCALE
        && d.glassY <= RFLi_MAX_GLASS_Y && d.moleType <= RFLi_MAX_MOLE_TYPE && d.moleScale <= RFLi_MAX_MOLE_SCALE
        && d.moleX <= RFLi_MAX_MOLE_X && d.moleY <= RFLi_MAX_MOLE_Y;
}

void Names() {
    for (const char* name : {"Mario", "A", "1234567890", "\xC3\xA9t\xC3\xA9", "Gold\xE2\x82\xAC" "nP2\xE2\x98\x86\xE2\x98\x86"})
        Check(ValidMiiName(name), std::string("Name refused: ") + name);
    for (const char* name : {"", "12345678901", "\xF0\x9F\x98\x80", "a\x01", "\xC3", "\xC0\xAF", "\xED\xA0\x80"})
        Check(!ValidMiiName(name), "Name accepted");
    bool thrown{};
    try { MakeRandomMii("", 1, 0); } catch (const std::invalid_argument&) { thrown = true; }
    Check(thrown, "A Mii without a name was made");
}

// Every bit of a record survives decoding and encoding: the encoder writes
// each field where the decoder reads it.
void RoundTrip() {
    std::mt19937 random(7);
    for (int n = 0; n < 1000; ++n) {
        std::string bytes(74, '\0');
        for (auto& byte : bytes) byte = char(random());
        RFLiCharData data;
        ChargedDecodeRFLCharRecordBE(bytes.data(), &data);
        std::string encoded(74, '\0');
        ChargedEncodeRFLCharRecordBE(&data, encoded.data());
        Check(encoded == bytes, "A record changed through decoding and encoding");
    }
}

void Faces() {
    // Created 0x02C6FC69 4-second steps after 2006: the Mii Channel's own form.
    const std::int64_t created = 1136073600 + 4 * std::int64_t(0x02C6FC69);
    const auto mario = MakeRandomMii("Mario", 42, created);
    Check(mario == MakeRandomMii("Mario", 42, created), "The same seed made another Mii");
    Check(mario.compare(24, 4, "\x82\xC6\xFC\x69") == 0, "Create ID counter and flags");
    const auto data = Decode(mario);
    const char16_t name[10] = {u'M', u'a', u'r', u'i', u'o'};
    Check(std::memcmp(data.name, name, sizeof name) == 0, "Mii name cells");
    Check(!data.creatorName[0] && !data.birthMonth && !data.birthDay && !data.favorite && !data.localonly,
          "New Mii personal data");

    // Many seeds: every face valid for the game, and faces that vary.
    std::set<std::string> faces;
    for (std::uint32_t seed = 0; seed < 5000; ++seed) {
        const auto record = MakeRandomMii("Mii", seed, created);
        const auto face = Decode(record);
        Check(ValidForGame(face), "Seed " + std::to_string(seed) + " made a Mii the game rejects");
        Check(face.eyebrowColor == face.hairColor, "Eyebrows not the hair's colour");
        Check((std::uint8_t(record[24]) & 0xA0) == 0x80, "Create ID special or temporary");
        faces.insert(record.substr(32, 22));
    }
    Check(faces.size() > 4000, "Random faces repeat");
}
} // namespace

int main() {
    try {
        Names();
        RoundTrip();
        Faces();
        std::cout << "mii_maker: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mii_maker: " << e.what() << '\n';
        return 1;
    }
}

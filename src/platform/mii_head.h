#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Mii heads drawn as the Wii draws its Mii icons (RFLMakeIcon, RFL_Icon.c):
// the head model and the face texture are built from the Mii parts of the
// player's own game image (Data/RFLRes01.arc, which holds the Mii library's
// RFL_Res.dat), with the same shapes, transforms, camera, light, texture
// stages and blending the Mii library sets up on the GX (RFL_Model.c,
// RFL_MakeTex.c), drawn on the CPU. No SDL or ImGui here: hosts upload the
// RGBA pixels as their own textures.
namespace mscharged::platform::mii {

// A Mii record: RFLiCharData, 74 bytes, as in RFL_DB.dat.
inline constexpr std::size_t kMiiRecordBytes = 0x4A;

// The face of a Mii: its parts and their placement, as the record holds them
// (big-endian bit fields of RFLiCharData). kMiiFaceFields lists the range of
// each value the Mii library accepts (RFLiCheckValidInfo).
struct MiiFace {
    int favorite_color = 0; // RFLFavoriteColor 0..11: the cap of hats, a background
    int face_type = 0, face_color = 0, face_texture = 0;
    int hair_type = 0, hair_color = 0, hair_flip = 0;
    int eyebrow_type = 0, eyebrow_color = 0, eyebrow_scale = 0, eyebrow_rotate = 0, eyebrow_x = 0, eyebrow_y = 3;
    int eye_type = 0, eye_color = 0, eye_scale = 0, eye_rotate = 0, eye_x = 0, eye_y = 0;
    int nose_type = 0, nose_scale = 0, nose_y = 0;
    int mouth_type = 0, mouth_color = 0, mouth_scale = 0, mouth_y = 0;
    int glasses_type = 0, glasses_color = 0, glasses_scale = 0, glasses_y = 0;
    int mustache_type = 0, beard_type = 0, beard_color = 0, beard_scale = 0, beard_y = 0;
    int mole_type = 0, mole_scale = 0, mole_x = 0, mole_y = 0;
};

// One value of MiiFace and its range, for editors.
struct MiiFaceField {
    const char* name; // e.g. "eye_type"
    int MiiFace::*value;
    int min, max;
};
extern const std::array<MiiFaceField, 39> kMiiFaceFields;

// The face of a record (74 bytes, as mii_maker.h and save_import.h hold
// them). Throws std::invalid_argument for fewer bytes.
MiiFace ReadMiiFace(std::string_view record);
// Puts `face` into a record, leaving its other bits (name, birthday, create
// ID ...) as they are. Values are cut to their bit fields; check ValidMiiFace
// first. Throws std::invalid_argument for fewer than 74 bytes.
void WriteMiiFace(const MiiFace& face, std::string& record);
// Whether every value is in its kMiiFaceFields range: the Mii library draws
// nothing for a record that fails its check.
bool ValidMiiFace(const MiiFace& face);

// RFLFavoriteColor's RGB (RFL_Model.c cFavoriteColor); index taken modulo 12.
std::array<std::uint8_t, 3> FavoriteColor(int index);

// RFLExpression.
enum class Expression { Normal, Smile, Anger, Sorrow, Surprise, Blink, OpenMouth };

// The Mii parts (RFL_Res.dat): shapes and textures of every part in 18
// archives. Immutable once made, so one can serve several threads.
class MiiParts {
public:
    // Checks the archive table and keeps the bytes. Throws
    // std::invalid_argument for bytes that are not a Mii parts file.
    explicit MiiParts(std::vector<std::uint8_t> resource);

    // RFL_Res.dat out of a Mii resource archive (RFLRes01.arc, a U8 archive).
    // Throws std::invalid_argument for a damaged archive or one without it.
    static MiiParts FromArchive(const std::vector<std::uint8_t>& archive);

    struct Bytes {
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
    };
    // A file of archive `archive` (RFLiArcID order); empty when there is none.
    Bytes File(int archive, int index) const;
    int FileCount(int archive) const;

private:
    struct Archive {
        std::size_t table = 0, data = 0;
        int files = 0;
    };
    std::vector<std::uint8_t> bytes_;
    std::array<Archive, 18> archives_{};
};

// A file of a U8 archive by its path in the archive ("RFL_Res.dat",
// "/dir/name"; case as stored); nullopt when the archive has none. Throws
// std::invalid_argument when the archive's tables are damaged.
std::optional<std::vector<std::uint8_t>> ReadU8File(const std::vector<std::uint8_t>& archive,
                                                    std::string_view path);

// The Mii parts of a Mario Strikers Charged image (ISO, RVZ or WBFS, any
// version): its Data/RFLRes01.arc, the archive the game's MiiManager loads.
// Throws std::runtime_error when the image or the file cannot be read and
// std::invalid_argument when the file is damaged.
MiiParts ReadMiiPartsFromImage(const std::filesystem::path& image);

struct MiiHeadOptions {
    // The image size in pixels, 1..1024 each (the game makes 128 x 128). The
    // face texture is 64 x 64 for heads up to 64 pixels, 128 x 128 up to
    // 128, else 256 x 256 (the game asks the Mii library for its large
    // textures).
    int width = 128, height = 128;
    Expression expression = Expression::Normal;
    // Transparent: RGBA as the Wii's icon holds it, alpha 0 around the head.
    // Color and Favorite: an opaque image, the head drawn on that colour as
    // RFLMakeIcon draws it on its background colour.
    enum class Background { Transparent, Color, Favorite } background = Background::Transparent;
    std::array<std::uint8_t, 3> background_color{};
    // The Wii's precision: 6 bits per channel in the colour buffer and the
    // RGB5A3 icon texture (5 bits per channel, 4 bits and 3 alpha bits where
    // the head is not opaque). Off keeps 8 bits per channel throughout.
    bool wii_precision = true;
};

// RGBA, 8 bits per channel, rows from the top, straight alpha.
struct RgbaImage {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
};

// The head of the Mii `face`, as RFLMakeIcon draws its front icon. Throws
// std::invalid_argument for a face ValidMiiFace rejects (a favourite colour
// past 11, which the Mii library does not check, draws as red), options out
// of range, or parts that do not hold the face's files. Deterministic, and
// safe to call from several threads at once.
RgbaImage RenderMiiHead(const MiiParts& parts, const MiiFace& face, const MiiHeadOptions& options = {});
// The same for a 74-byte record.
RgbaImage RenderMiiHead(const MiiParts& parts, std::string_view record, const MiiHeadOptions& options = {});
} // namespace mscharged::platform::mii

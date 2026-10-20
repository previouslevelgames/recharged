#pragma once

#include "platform/game_release.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged
{
// One original disc-message image (the loading message or a DVD error
// screen): a 1-bit picture in the game's run-length format, as main.dol's
// sDiscMessageImages table describes it. The port ships none of them; the
// player's own main.dol supplies them.
struct DiscMessageImage
{
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint8_t> data;
};

// The table has 7 languages (eng, fre, deu, spa, ita, jpn, uke), each with
// 5 images (fatal error, no disc, wrong disc, retry, loading).
constexpr std::size_t kDiscMessageLanguages = 7;
constexpr std::size_t kDiscMessageTypes = 5;
constexpr std::size_t kDiscMessageImageCount = kDiscMessageLanguages * kDiscMessageTypes;

// "HEADER_FatalErrorImage_eng": the namespace the decompilation's extractor
// gives table entry `index` (its g_ImageData array lives there).
std::string DiscMessageImageName(std::size_t index);

// Copies the 35 images out of an executable's data sections, from the table
// at table_address. Throws std::runtime_error for a damaged header, table or
// image (one the game would not unpack to exactly width * height / 8 bytes).
std::vector<DiscMessageImage> ExtractDiscMessageImages(const std::uint8_t* dol, std::size_t size,
                                                       std::uint32_t table_address);
// The same, after checking the executable against a main.dol hash: throws
// std::runtime_error for another executable too.
std::vector<DiscMessageImage> ExtractDiscMessageImages(const std::uint8_t* dol, std::size_t size,
                                                       std::string_view dol_sha1, std::uint32_t table_address);

// What a disc's main.dol holds for a release.
struct DiscMessages
{
    std::string dol_sha1;                 // of the disc's main.dol
    bool modified = false;                // not the release's own main.dol (a Wiimmfi-patched image, say)
    std::vector<DiscMessageImage> images; // empty when a modified main.dol has no valid table
    std::string missing;                  // why `images` is empty
};

// Reads main.dol from the disc image and extracts the release's images. The
// game never runs main.dol, so a modified one only matters for these images:
// they are taken when the release's table still holds valid ones. The
// release's own main.dol must hold them (std::runtime_error otherwise).
DiscMessages ReadDiscMessages(const std::filesystem::path& disc, const GameRelease& release);

// The game module's storage for one image (its export
// charged_disc_message_image): the data array, the array's size and the width
// and height the original table compiles in; null past the last image.
using DiscMessageImageStorage = unsigned char* (*)(unsigned int index, unsigned long* size, int* width, int* height);

// Copies the images into the module's arrays. Checks first that the module
// has exactly kDiscMessageImageCount arrays with the images' sizes and
// dimensions, and copies nothing otherwise (std::runtime_error).
void InstallDiscMessageImages(const std::vector<DiscMessageImage>& images, DiscMessageImageStorage storage);

// A packed image of exactly `packed_size` bytes that the game unpacks to
// width * height / 8 zero bytes: an empty picture for an array of that size.
// std::runtime_error when the array is too small for one.
std::vector<std::uint8_t> BlankDiscMessageImage(std::size_t packed_size, std::uint32_t width, std::uint32_t height);

// Installs what ReadDiscMessages found and returns the line for the log. A
// modified main.dol without images that fit the module's arrays gives empty
// pictures instead: the game's disc error and loading screens stay blank.
// The release's own main.dol keeps InstallDiscMessageImages' checks.
std::string InstallDiscMessages(const DiscMessages& found, DiscMessageImageStorage storage);
}

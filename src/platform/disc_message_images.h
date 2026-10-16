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

// Checks the executable against a main.dol hash, then copies the 35 images
// out of its data sections. Throws std::runtime_error for another executable
// or a damaged header, table or image.
std::vector<DiscMessageImage> ExtractDiscMessageImages(const std::uint8_t* dol, std::size_t size,
                                                       std::string_view dol_sha1, std::uint32_t table_address);

// Reads main.dol from the disc image and extracts the release's images.
std::vector<DiscMessageImage> ReadDiscMessageImages(const std::filesystem::path& disc, const GameRelease& release);

// The game module's storage for one image (its export
// charged_disc_message_image): the data array, the array's size and the width
// and height the original table compiles in; null past the last image.
using DiscMessageImageStorage = unsigned char* (*)(unsigned int index, unsigned long* size, int* width, int* height);

// Copies the images into the module's arrays. Checks first that the module
// has exactly kDiscMessageImageCount arrays with the images' sizes and
// dimensions, and copies nothing otherwise (std::runtime_error).
void InstallDiscMessageImages(const std::vector<DiscMessageImage>& images, DiscMessageImageStorage storage);
}

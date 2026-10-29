#include "bootstrap/config.h"
#include "bootstrap/launch_options.h"
#include "platform/disc.h"
#include "platform/path.h"
#include "runtime/original_game.h"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
// Without [game] disc in recharged.ini: the first Mario Strikers Charged image
// the game plays in its game folder (game/ beside the .nro), else the first
// image there, whose error the game then reports.
std::filesystem::path FindGameFolderDisc()
{
    const char* base = SDL_GetBasePath();
    if (!base) return {};
    const auto config = mscharged::LoadConfig(mscharged::DefaultConfigPath(mscharged::PathFromUtf8(base)), true);
    if (!config.settings.disc.empty()) return {};
    std::vector<std::filesystem::path> images;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(mscharged::ResolveGameFolder(config.settings, config.path), error))
    {
        const auto name = mscharged::PathUtf8(entry.path().filename());
        auto extension = mscharged::PathUtf8(entry.path().extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        // macOS leaves ._ files beside copies on FAT cards.
        if (name.rfind("._", 0) == 0 || (extension != ".iso" && extension != ".wbfs" && extension != ".rvz")) continue;
        images.push_back(entry.path());
    }
    std::sort(images.begin(), images.end());
    for (const auto& image : images)
    {
        const auto probe = mscharged::ProbeDisc(image);
        if (probe.kind == mscharged::DiscProbe::Kind::Charged && mscharged::SupportedRelease(probe.info)) return image;
    }
    return images.empty() ? std::filesystem::path{} : images.front();
}
}

// The Switch program starts the game directly; its settings come from
// recharged.ini beside the .nro, as on desktop. The Homebrew Menu passes no
// disc on the command line.
int main(int argc, char** argv)
{
    std::vector<char*> arguments(argv, argv + argc);
    const bool given = std::any_of(arguments.begin() + 1, arguments.end(), [](const char* argument) {
        return std::string_view(argument) == "--disc" || std::string_view(argument) == "--disk";
    });
    std::string disc;
    if (!given)
    {
        try { disc = mscharged::PathUtf8(FindGameFolderDisc()); } catch (...) {}
    }
    if (!disc.empty())
    {
        static char option[] = "--disc";
        arguments.push_back(option);
        arguments.push_back(disc.data());
    }
    arguments.push_back(nullptr);
    return mscharged::RunOriginalGame(int(arguments.size()) - 1, arguments.data(), nullptr, true);
}

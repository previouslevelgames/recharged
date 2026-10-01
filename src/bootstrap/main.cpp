#include <iostream>
#include <string_view>

#include "NL/nlEndian.h"
#include "NL/nlMath.h"
#include "bootstrap/config.h"
#include "mscharged/build_version.h"
#include "platform/disc.h"
#include "platform/path.h"

namespace
{
void PrintVersion()
{
    std::cout << "mscharged-port " << mscharged::build::version
              << " (" << MSCHARGED_BUILD_CONFIG << "; development bootstrap)\n";
}

void PrintUsage(std::ostream& output)
{
    output << "Usage: mscharged-bootstrap [--config FILE | --disc FILE | --self-test | --version | --help]\n"
           << "With no arguments, read [game] disc from ./mscharged.ini.\n"
           << "--disc accepts an ISO or RVZ directly; --self-test needs no game data.\n";
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--version")
    {
        PrintVersion();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        PrintVersion();
        PrintUsage(std::cout);
        return 0;
    }
    const bool self_test = argc == 2 && std::string_view(argv[1]) == "--self-test";
    const bool use_config = argc == 3 && std::string_view(argv[1]) == "--config";
    const bool use_disc = argc == 3 && std::string_view(argv[1]) == "--disc";
    if (argc != 1 && !self_test && !use_config && !use_disc)
    {
        PrintUsage(std::cerr);
        return 2;
    }

    unsigned short swapped = 0;
    nlSwapEndian(0x1234, &swapped);
    unsigned int seed = 0x12345678;
    const unsigned int first = nlRandom(1000, &seed);
    if (swapped != 0x3412 || first != 896)
    {
        std::cerr << "Foundation check failed.\n";
        return 1;
    }

    PrintVersion();
    std::cout << "Decomp revision: " << MSCHARGED_DECOMP_REVISION << '\n'
              << "Native endian and random utility checks passed.\n";
    if (!self_test)
    {
        try
        {
            const auto path = use_disc ? mscharged::PathFromUtf8(argv[2])
                : mscharged::LoadDiscPath(use_config ? mscharged::PathFromUtf8(argv[2]) : "mscharged.ini");
            std::cout << "Disc image: " << mscharged::PathUtf8(path) << '\n';
            const auto info = mscharged::InspectDisc(path);
            std::cout << "Disc: " << info.game_id << " revision " << unsigned(info.revision)
                      << " (" << info.format << ") - " << info.title << '\n'
                      << "Game data partition opened: " << info.file_count << " files.\n";
            if (info.game_id != "R4QE01" || info.revision != 1)
                std::cout << "This disc differs from the source baseline R4QE01 revision 1; asset compatibility is unverified.\n";
        }
        catch (const std::exception& error)
        {
            std::cerr << "Disc setup failed: " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "The decomp is incomplete; this executable does not run the game.\n";
    return 0;
}

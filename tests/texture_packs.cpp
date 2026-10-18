// HD texture packs: finding folder and zip packs, ordering them, and reading
// their images, against the packs tests/texture_packs_fixture.py writes.
#include "platform/texture_packs.h"
#include "platform/zip_archive.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace mscharged::platform;
namespace fs = std::filesystem;

namespace
{
int failures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

bool Throws(const std::function<void()>& action)
{
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}

std::vector<std::uint8_t> Data(const std::string& name)
{
    std::vector<std::uint8_t> bytes;
    for (int n = 0; n < 300; ++n)
    {
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back('\n');
    }
    return bytes;
}

const TexturePack* Find(const std::vector<TexturePack>& packs, const std::string& name)
{
    const auto it = std::find_if(packs.begin(), packs.end(), [&](const TexturePack& pack) { return pack.name == name; });
    return it == packs.end() ? nullptr : &*it;
}

std::vector<std::string> Names(const std::vector<TexturePack>& packs)
{
    std::vector<std::string> names;
    for (const auto& pack : packs) names.push_back(pack.name);
    return names;
}

const std::string A = "tex1_64x64_m_0123456789abcdef_14";
const std::string B = "tex1_128x128_fedcba9876543210_5";
const std::string C = "tex1_32x32_1111222233334444_5555666677778888_9";
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: texture_packs_tests FIXTURE_DIR\n";
        return 2;
    }
    const fs::path root = argv[1];

    Check(IsReplacementImageName(A + ".png") && IsReplacementImageName(B + ".DDS"), "image names");
    Check(!IsReplacementImageName(A + "_mip1.png") && !IsReplacementImageName(A + ".jpg")
              && !IsReplacementImageName("TEX1_8x8_bbbbbbbbbbbbbbbb_14.png") && !IsReplacementImageName("desktop.ini"),
          "not image names");
    Check(IsReplacementImageName("tex1_8x8_aaaa_14_mipmap.png"), "a name with _mip but no level");

    const auto packs = FindTexturePacks(root / "packs");
    const std::vector<std::string> expected{"Big Pack.zip", "Damaged Data.zip", "Folder Pack", "Zip Pack.zip", "broken.zip"};
    Check(Names(packs) == expected, "packs found, by name");
    if (const auto* pack = Find(packs, "Folder Pack"))
    {
        // A, B (dds and png), C, the R4QP01 image; not the mip levels or other files.
        Check(!pack->archive && pack->textures == 5 && pack->error.empty(), "folder pack");
    }
    if (const auto* pack = Find(packs, "Zip Pack.zip"))
        Check(pack->archive && pack->textures == 4 && pack->error.empty(), "zip pack");
    if (const auto* pack = Find(packs, "Big Pack.zip")) Check(pack->textures == 1, "zip64 pack");
    if (const auto* pack = Find(packs, "broken.zip")) Check(!pack->error.empty() && pack->textures == 0, "broken zip");
    Check(!Find(packs, "GALE01") && !Find(packs, "notes.zip") && !Find(packs, "Empty Folder") && !Find(packs, "Docs"),
          "no packs: another game, no images");
    Check(FindTexturePacks(root / "missing").empty(), "missing folder");

    // The packs folder is itself a pack: named after the game, or holding images.
    const auto own = FindTexturePacks(root / "R4Q");
    Check(own.size() == 1 && own[0].name == "R4Q" && own[0].textures == 1, "a folder named R4Q is one pack");
    const auto loose = FindTexturePacks(root / "loose");
    Check(loose.size() == 1 && loose[0].name == "loose" && loose[0].textures == 2, "a folder with images is one pack");

    // A dropped item: a pack, or nothing.
    const auto zip = ProbeTexturePack(root / "packs" / "Zip Pack.zip");
    Check(zip && zip->archive && zip->textures == 4 && zip->name == "Zip Pack.zip", "probe a zip pack");
    const auto folder = ProbeTexturePack(root / "packs" / "Folder Pack");
    Check(folder && !folder->archive && folder->textures == 5, "probe a folder pack");
    Check(!ProbeTexturePack(root / "packs" / "notes.zip") && !ProbeTexturePack(root / "packs" / "broken.zip")
              && !ProbeTexturePack(root / "packs" / "Docs") && !ProbeTexturePack(root / "packs" / "missing"),
          "probe things that are no packs");
    Check(!Find(packs, ".Hidden Pack"), "a hidden pack (being copied in) is left out");

    const auto ordered = OrderTexturePacks(packs, {"Zip Pack.zip", "gone", "Folder Pack"});
    Check(Names(ordered) == std::vector<std::string>{"Zip Pack.zip", "Folder Pack", "Big Pack.zip", "Damaged Data.zip",
                                                     "broken.zip"},
          "order");

    Check(SplitPackNames(" R4Q.zip |Folder Pack| | R4Q.zip") == std::vector<std::string>{"R4Q.zip", "Folder Pack"},
          "split names");
    Check(SplitPackNames("").empty(), "no names");
    Check(JoinPackNames({"A", "B c", "bad|name"}) == "A | B c", "join names");
    Check(StorablePackName("R4Q.zip") && !StorablePackName("a|b") && !StorablePackName(" a") && !StorablePackName(""),
          "storable names");

    // Reading a folder pack.
    if (const auto* pack = Find(packs, "Folder Pack"))
    {
        const TexturePackFiles files(*pack, "R4QE01");
        const std::vector<std::string> images{"Characters/" + A + ".png", "R4QE01/" + C + ".png", B + ".dds",
                                              B + ".png"};
        Check(files.Images() == images, "folder images, sorted, without R4QP01");
        std::vector<std::uint8_t> bytes;
        Check(files.Read("Characters/" + A + ".png", bytes) && bytes == Data("folder A"), "read a folder image");
        Check(files.Read("Characters/" + A + "_mip2.png", bytes) && bytes == Data("folder A mip2"), "read a mip level");
        Check(!files.Read("Characters/" + A + "_mip3.png", bytes) && bytes.empty(), "a missing mip level");
        Check(!files.Read("../Zip Pack.zip", bytes) && !files.Read("", bytes), "paths outside the pack");
        const TexturePackFiles europe(*pack, "R4QP01");
        Check(europe.Images().size() == 4
                  && std::find(europe.Images().begin(), europe.Images().end(),
                               "R4QP01/tex1_8x8_aaaaaaaaaaaaaaaa_14.png") != europe.Images().end(),
              "folder images for the European release");
    }

    // Reading a zip pack.
    if (const auto* pack = Find(packs, "Zip Pack.zip"))
    {
        const TexturePackFiles files(*pack, "R4QE01");
        const std::vector<std::string> images{"R4Q/Fonts/" + C + ".png", "R4Q/Stages/" + A + ".png", "R4Q/" + B + ".PNG"};
        Check(files.Images() == images, "zip images, sorted, backslashes as slashes, without R4QP01");
        std::vector<std::uint8_t> bytes;
        Check(files.Read("R4Q/Stages/" + A + ".png", bytes) && bytes == Data("zip A"), "read a deflated image");
        Check(files.Read("R4Q/Stages/" + A + "_mip1.png", bytes) && bytes == Data("zip A mip1"), "read a stored mip");
        Check(files.Read("R4Q/Fonts/" + C + ".png", bytes) && bytes == Data("zip C"), "read a backslash name");
        Check(!files.Read("R4Q/Stages/" + A + "_mip2.png", bytes) && bytes.empty(), "a missing zip entry");
        Check(!files.Read("R4Q/", bytes), "a zip folder is no file");
    }
    if (const auto* pack = Find(packs, "Big Pack.zip"))
    {
        const TexturePackFiles files(*pack, "R4QE01");
        std::vector<std::uint8_t> bytes;
        Check(files.Images().size() == 1 && files.Read(A + ".png", bytes) && bytes == Data("zip64 A"), "read zip64");
    }
    if (const auto* pack = Find(packs, "Damaged Data.zip"))
    {
        const TexturePackFiles files(*pack, "R4QE01");
        std::vector<std::uint8_t> bytes;
        Check(files.Images().size() == 1 && !files.Read(A + ".png", bytes) && bytes.empty(), "a damaged entry");
    }
    if (const auto* pack = Find(packs, "broken.zip"))
        Check(Throws([&] { TexturePackFiles files(*pack, "R4QE01"); }), "a broken zip cannot be opened");
    Check(Throws([&] { ZipArchive archive(root / "packs" / "missing.zip"); }), "a missing zip");

    if (failures)
    {
        std::cerr << failures << " texture pack check(s) failed\n";
        return 1;
    }
    std::cout << "texture packs: all checks passed\n";
    return 0;
}

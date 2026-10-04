#include "runtime/animation_bundle.h"
#include "Game/SAnim/AnimRetargeter.h"
#include "Game/SHierarchy.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Unsafe retarget operation accepted"); }
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read retarget fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
void Inspect(const char* path)
{
    auto bytes = Read(path);
    AnimationRetargetAsset::Handle retained;
    {
        AnimationRetargetAssets assets(bytes); bytes.clear(); bytes.shrink_to_fit();
        retained = assets.Selected(); Check(retained == assets.At(assets.Size() - 1), "Original Find(0) inventory order changed");
        Reject([&] { assets.At(assets.Size()); });
        for (unsigned index = 0; index < assets.Size(); ++index)
        {
            auto owner = assets.At(index); const auto& list = owner->Data();
            std::cout << "LIST " << list.GetHashID() << ' ' << list.m_NumAnimRetargets << '\n';
            Check(list.m_szName == nullptr, "Retarget owner followed the serialized name pointer");
            for (long i = 0; i < list.m_NumAnimRetargets; ++i)
            {
                const auto& map = list.m_pAnimRetarget[i];
                const AnimRetarget* expected = nullptr;
                for (long j = 0; j <= i; ++j)
                    if (list.m_pAnimRetarget[j].m_TargetHierarchySignature == map.m_TargetHierarchySignature)
                    { expected = &list.m_pAnimRetarget[j]; break; }
                cSAnim animation{}; animation.m_nHierarchySignature = map.m_TargetHierarchySignature; animation.m_nNumNodes = 32768;
                Check(owner->Find(std::uint32_t(map.m_TargetHierarchySignature)) == expected && owner->Find(animation) == expected,
                    "Original first-match retarget lookup changed");
                Check(&owner->RequireMap(animation, expected->m_Unknown08) == expected, "Map binding lost original identity");
                Reject([&] { owner->RequireMap(animation, std::size_t(expected->m_Unknown08) + 1); });
                animation.m_nNumNodes = 0;
                bool has_source = false;
                for (unsigned j = 0; j < expected->m_Unknown08; ++j) has_source |= expected->m_pMap[j] >= 0;
                if (has_source) Reject([&] { owner->RequireMap(animation, expected->m_Unknown08); });
                std::cout << "MAP " << map.m_TargetHierarchySignature << ' ' << map.m_NumBones << ' ' << map.m_Unknown08;
                for (unsigned j = 0; j < map.m_Unknown08; ++j) std::cout << ' ' << map.m_pMap[j];
                std::cout << '\n';
            }
            cSAnim missing{}; missing.m_nHierarchySignature = 0xffffffff;
            Check(!owner->Find(missing), "Unknown signature acquired a fabricated map");
            Reject([&] { owner->RequireMap(missing, 0); });
        }
    }
    bool stable = false;
    std::thread reader([&] { stable = retained->Data().m_NumAnimRetargets >= 0; }); reader.join();
    Check(stable, "Host-owned retarget storage did not survive collection/input disposal");
}
void Character(const char* root, unsigned index)
{
    const auto& profile = CharacterAnimation(index);
    auto read = [&](std::string_view relative) {
        auto path = std::string(relative); path.replace(0, 3, "Art");
        return Read(std::filesystem::path(root) / path);
    };
    auto hierarchy = read(profile.hierarchy_path), animations = read(profile.animation_path), maps = read(profile.retarget_path);
    auto bundle = AnimationBundle::DecodeCharacter(hierarchy, animations, maps, index);
    hierarchy.clear(); animations.clear(); maps.clear();
    const auto& target = bundle->Hierarchy()->Data();
    std::cout << "CHAR " << index << ' ' << target.GetNumNodes() << ' ' << bundle->Size() << '\n';
    Reject([&] { bundle->AnimationNode(0); });
    Reject([&] { bundle->MappedNode(bundle->Size(), 0); });
    Reject([&] { bundle->MappedNode(0, target.GetNumNodes()); });
    for (unsigned track = 0; track < bundle->Size(); ++track)
    {
        const auto& animation = bundle->At(track)->Data();
        const auto* map = bundle->Retarget(track);
        std::cout << "TRACK " << track << ' ' << animation.m_nHierarchySignature << ' ' << animation.m_nNumNodes << ' ' << bool(map) << '\n';
        for (int node = 0; node < target.GetNumNodes(); ++node)
        {
            const auto direct = bundle->MappedNode(track, node), mirrored = bundle->MappedNode(track, node, true);
            std::cout << "NODE " << node << ' ' << (direct ? int(*direct) : -1) << ' ' << (mirrored ? int(*mirrored) : -1) << '\n';
            if (direct)
            {
                Check(*direct < animation.m_nNumNodes, "Mapped animation node is out of range");
                bundle->At(track)->Weight(*direct, .5f);
            }
        }
    }
    auto retained = bundle->Retargets(); bundle.reset();
    Check(retained->Data().m_NumAnimRetargets > 0, "Retarget handle expired with its bundle");
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--inspect") Inspect(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--reject")
        { const auto bytes = Read(argv[2]); Reject([&] { AnimationRetargetAssets assets(bytes); }); }
        else if (argc == 4 && std::string_view(argv[1]) == "--character") Character(argv[2], std::stoul(argv[3]));
        else throw std::invalid_argument("Use --inspect FILE, --reject FILE or --character ROOT INDEX");
        std::cout << "CHECKS " << checks << '\n';
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}

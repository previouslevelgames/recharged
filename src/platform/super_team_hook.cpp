// Tweak "Super Team" inside the original game module: the host passes the
// GameCube robot's files and the robot side before original main, and the hooks
// of tweak patch 0026 (CharacterLoader) ask for each fielder whether it is a
// robot, and for the robot version of each character model as it loads
// (src/platform/super_team.cpp does the work).
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error The Super Team hook belongs to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_tweaks.h"
#include "NL/nlMemory.h"
#include "platform/game_allocation_ownership.h"
#include "platform/super_team.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>

namespace
{
// The host owns the files for the whole run.
mscharged::platform::SuperTeamSource sSource;
int sSides = 0; // bit 0 the home team, bit 1 the away team
bool sReady = false;

// A made model or bundle, in the game heap as a file the game loaded: the
// game's readers take only such bytes. Freed by the hook's caller.
void* Copy(const mscharged::platform::gx::HostVector<std::uint8_t>& bytes, unsigned long* size)
{
    if (bytes.empty()) return nullptr;
    void* out = nlMalloc(static_cast<unsigned long>(bytes.size()), 32, true);
    if (!out) return nullptr;
    mscharged::platform::GameByteWriteReservation write(out, bytes.size());
    std::memcpy(out, bytes.data(), bytes.size());
    write.Complete(mscharged::platform::GameByteDomain::WiiSerialized);
    *size = static_cast<unsigned long>(bytes.size());
    return out;
}
} // namespace

extern "C" {
// Host entry: the robot's model (.glg), skeleton (.shier) and textures (.glt)
// from a Super Mario Strikers disc, and the robot teams (bit 0 home, bit 1
// away). False (and no robots) when the files are not the Super Team's.
__attribute__((visibility("default"))) bool charged_configure_super_team(const void* model, unsigned long modelSize,
    const void* skeleton, unsigned long skeletonSize, const void* textures, unsigned long texturesSize, int sides)
{
    sSource = {{static_cast<const std::uint8_t*>(model), model ? modelSize : 0},
        {static_cast<const std::uint8_t*>(skeleton), skeleton ? skeletonSize : 0},
        {static_cast<const std::uint8_t*>(textures), textures ? texturesSize : 0}};
    sSides = sides & 3;
    sReady = sSides && mscharged::platform::ValidSuperTeamSource(sSource);
    return sReady;
}
}

int mscharged_tweak_super_team(int team, int characterClass)
{
    return sReady && mscharged_tweak_enabled(MSCHARGED_TWEAK_SUPER_TEAM) && team >= 0 && team < 2
        && (sSides & (1 << team)) && characterClass >= 0 && characterClass < 20
        && !mscharged::platform::SuperTeamKeepsLook(characterClass);
}

unsigned long mscharged_tweak_super_team_model_id(unsigned long id)
{
    return mscharged::platform::SuperTeamModelId(static_cast<std::uint32_t>(id));
}

unsigned long mscharged_tweak_super_team_texture(void)
{
    return mscharged::platform::kSuperTeamBody;
}

void* mscharged_tweak_super_team_model(const void* model, unsigned long size, const void* skeleton,
    unsigned long skeletonSize, unsigned long* madeSize)
{
    if (!sReady || !model || !skeleton || !madeSize) return nullptr;
    try
    {
        return Copy(mscharged::platform::MakeSuperTeamModel(sSource,
                        {static_cast<const std::uint8_t*>(model), size},
                        {static_cast<const std::uint8_t*>(skeleton), skeletonSize}),
            madeSize);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Super Team: the character keeps its look (%s)\n", error.what());
        return nullptr;
    }
}

void* mscharged_tweak_super_team_textures(unsigned long* madeSize)
{
    if (!sReady || !madeSize) return nullptr;
    try
    {
        return Copy(mscharged::platform::MakeSuperTeamTextures(sSource), madeSize);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Super Team: no robot textures (%s)\n", error.what());
        return nullptr;
    }
}

void mscharged_tweak_super_team_free(void* data)
{
    if (data) nlFree(data);
}

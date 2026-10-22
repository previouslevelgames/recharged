#pragma once

#include "bootstrap/config.h"
#include "platform/presence_snapshot.h"
#include "platform/recoloured_texture.h"

#include <filesystem>
#include <string_view>

namespace mscharged
{
// Hands the HD texture packs that are switched on ([mods], see Settings) to
// Aurora, which swaps their images in for the game's textures with the same
// hash: the first pack in the player's order wins. Only the host's graphics
// change; the game's own textures and data stay as on the disc. Logs what it
// loaded. Called once, after Aurora has started.
void LoadTexturePacks(const Settings& settings, const std::filesystem::path& config_path, std::string_view game_id);

// For the game module (charged_configure_hd_recolour): textures Mirror match
// recolours get the packs' HD images recoloured alike, which their first draw
// waits for; `presence` (the module's charged_presence_snapshot, may be null)
// tells when a match is played, when they stream in instead. Null without packs.
ChargedRecolouredTextureHook TexturePackRecolourHook(ChargedPresenceSnapshotFunction presence);
}

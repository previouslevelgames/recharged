#pragma once

// The texture packs of the launcher's Extras page (#85): the packs of the
// texture packs folder, read in the background, and packs dropped onto the
// page, checked and copied into that folder in the background. The launcher
// polls it once a frame (on any page, so a copy's result is never missed);
// the player's order and the packs turned off stay in its settings
// (PackOrderAfterMove, PacksOffWith).

#include "launcher/background_job.h"
#include "platform/texture_packs.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mscharged::launcher
{
class TexturePackLibrary
{
public:
    // What happened to a dropped pack, for the launcher's notice.
    struct Event
    {
        enum class Kind
        {
            Added,           // `pack` was copied into the folder
            AddFailed,       // `pack` could not be: `error`
            AlreadyInFolder, // `pack` is in the folder already
            FolderHasPack,   // the folder has a pack of that file name
            CopyBusy,        // another pack is still being added: `pack` is that one
            NotAPack,        // `source` holds no replacement textures (a game folder or image)
        };
        Kind kind;
        std::string pack;
        std::string error;
        std::filesystem::path source;
    };

    TexturePackLibrary() = default;
    TexturePackLibrary(const TexturePackLibrary&) = delete;
    TexturePackLibrary& operator=(const TexturePackLibrary&) = delete;
    // A copy under way stops (its hidden folder goes) instead of holding the launcher's exit.
    ~TexturePackLibrary();

    // Once a frame: what finished since the last call. `folder` is read again
    // while `shown` (the page is open): when the page opens, when the folder
    // changed, after a pack came in, and when asked (Rescan).
    std::vector<Event> Poll(const std::filesystem::path& folder, bool shown);
    void Rescan() { rescan_ = true; }

    // A folder or .zip file dropped onto the page: false when it can't be a
    // pack (another file: the launcher takes it as a game image); otherwise it
    // is checked and copied into `folder` in the background, and Poll tells.
    bool Add(const std::filesystem::path& source, const std::filesystem::path& folder);

    const std::vector<platform::TexturePack>& Packs() const { return packs_; }
    bool Scanning() const { return scan_.Busy(); }
    // Whether the folder was there when it was last read.
    bool FolderExists() const { return folder_exists_; }
    // A dropped pack is being checked (its file name known) or copied in (its
    // pack name and textures known).
    bool Checking() const { return probe_.Busy(); }
    bool Copying() const { return copy_.Busy(); }
    const std::string& AddingName() const { return adding_name_; }
    std::size_t AddingTextures() const { return adding_textures_; }

private:
    struct Scan
    {
        std::vector<platform::TexturePack> packs;
        bool folder_exists = false;
    };

    BackgroundJob<Scan> scan_;
    std::vector<platform::TexturePack> packs_;
    std::string scanned_folder_;
    bool folder_exists_ = false, rescan_ = false, shown_ = false;

    BackgroundJob<std::optional<platform::TexturePack>> probe_;
    BackgroundJob<std::string> copy_;
    std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);
    std::filesystem::path adding_source_, adding_folder_;
    std::string adding_name_;
    std::size_t adding_textures_ = 0;
    std::vector<Event> pending_; // events of Add, told by the next Poll
};

// mods.texture_pack_order with the pack at `from` of `ordered` (the packs in
// the player's order) moved to `to`; empty when nothing moves.
std::optional<std::string> PackOrderAfterMove(std::vector<platform::TexturePack> ordered, std::size_t from,
                                              std::size_t to);
// mods.texture_packs_off with the pack `name` turned on or off.
std::string PacksOffWith(const std::string& off, const std::string& name, bool on);
// Copies the pack `source` into `folder` under a hidden name first, so the
// folder never lists half a pack; stops when `stop` is set. Empty, or why it
// could not.
std::string CopyTexturePack(const std::filesystem::path& source, const std::filesystem::path& folder,
                            const std::atomic<bool>& stop);
} // namespace mscharged::launcher

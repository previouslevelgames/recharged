#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Other players' Mii heads for the game's menus (their records come from the
// profile service): drawn from the Mii parts of the game's disc on a thread
// of their own (the parts are read once, when the first head is asked for),
// then made ImGui textures on the thread that asks. A head that is not drawn
// yet, or cannot be, is null: the menus show the player's initials
// meanwhile. Nothing waits in a frame. Bounded: the overlay cannot free a
// texture, so at most kMaxHeads heads are made in a run (later ones stay
// initials).
namespace mscharged::runtime
{
class MiiHeads
{
public:
    static constexpr std::size_t kMaxHeads = 192;

    explicit MiiHeads(std::filesystem::path disc);
    ~MiiHeads();
    MiiHeads(const MiiHeads&) = delete;
    MiiHeads& operator=(const MiiHeads&) = delete;

    // The head of the Mii whose record (74 bytes) `key` names as the menus'
    // picture key ("miidata:<base64>"), as an ImGui texture; null until it
    // is drawn. Owner thread (Aurora's ImGui textures are made there).
    void* Head(const std::string& key);

private:
    struct Drawn
    {
        std::string key;
        int width = 0, height = 0;
        std::vector<unsigned char> rgba; // empty: it cannot be drawn
    };
    void Run();

    const std::filesystem::path disc_;
    // The owner's: the heads made (null: none can be made), and the keys asked for.
    std::unordered_map<std::string, void*> heads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::deque<std::pair<std::string, std::string>> queue_; // key, record
    std::vector<Drawn> drawn_;
    std::thread worker_;
};
} // namespace mscharged::runtime

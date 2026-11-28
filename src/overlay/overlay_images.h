#pragma once

#include "imgui.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mscharged::platform
{
class HttpCancel;
}

// Players' pictures for the overlay (OverlayImages, online_panel.h): their
// avatars, from the URL in OnlinePanelRow::image (Discord's), downloaded and
// decoded on a thread of its own and made textures by the host's uploader on
// the thread that asks for them (the owner, which draws: Aurora's and SDL's
// textures are made there). A picture that isn't there yet, or can't be had,
// is 0: the overlay draws the player's initials meanwhile. Nothing waits in a
// frame. Bounded: a cap on the textures (the game can't free one), on a
// download's size and on the picture's (larger ones are scaled down); a
// failure is remembered and tried again only a few times, minutes apart. Keys
// other than http(s) URLs ("mii:<profile>": there are no Mii faces yet) are
// always 0, and nothing is asked for them.
namespace mscharged::overlay
{
// A decoded picture: RGBA, 8 bits each, rows top to bottom without padding.
struct OverlayImage
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

// What OverlayImageCache keeps to.
struct OverlayImageLimits
{
    std::size_t textures = 64;      // pictures made at most (with those on their way)
    std::size_t keys = 256;         // keys remembered at all, failures included
    std::size_t bytes = 256 * 1024; // a download's largest
    int size = 128;                 // pixels: a larger picture is scaled down to fit
    int attempts = 3;               // tries for one picture
    std::chrono::milliseconds retry = std::chrono::minutes(2); // between them
};

class OverlayImageCache
{
public:
    // The bytes at `url`, at most `max_bytes`; throws on any failure (a status
    // other than 200 too). `cancel` ends it when the cache goes.
    using Fetch = std::function<std::string(const std::string& url, std::size_t max_bytes,
                                            const std::shared_ptr<platform::HttpCancel>& cancel)>;
    // The picture in `bytes`, or false.
    using Decode = std::function<bool(const std::string& bytes, OverlayImage& image)>;
    // A texture of `rgba` (`width` x `height`, as OverlayImage), made by the
    // host on its owner thread; 0 when it can't.
    using Upload = std::function<ImTextureID(int width, int height, const void* rgba)>;

    using Limits = OverlayImageLimits;

    // Without `fetch` and `decode`: HTTPS downloads (OverlayHttpFetch) and
    // PNG pictures (DecodeOverlayPng). The thread starts with the first picture.
    explicit OverlayImageCache(Upload upload, Limits limits = Limits(), Fetch fetch = {}, Decode decode = {});
    // Stops a download under way and waits for the thread; pictures not made
    // yet are dropped. The textures made stay the host's.
    ~OverlayImageCache();
    OverlayImageCache(const OverlayImageCache&) = delete;
    OverlayImageCache& operator=(const OverlayImageCache&) = delete;

    // The texture for `key`, or 0 (it may start loading it; a picture loaded
    // since the last call is made a texture now). On the owner thread.
    ImTextureID Get(const std::string& key);
    // Get as the overlay asks for it (GameOverlayStyle::image,
    // OverlayPrompt::image); the cache must outlive it.
    std::function<ImTextureID(const std::string& key)> Images();

    // Waits up to `timeout` until nothing is being downloaded or decoded (for
    // captures and tests): true when that is so.
    bool Settle(std::chrono::milliseconds timeout);

    struct Stats
    {
        std::size_t textures = 0;  // made
        std::size_t pending = 0;   // on their way (queued, loading, or loaded and not made yet)
        std::size_t failed = 0;    // keys whose last try failed
        std::size_t downloads = 0; // fetches tried
        std::string last_error;    // the last failure's reason, for the host's log (no address)
    };
    Stats Statistics() const;

private:
    struct Entry
    {
        enum class State
        {
            Queued,
            Loading,
            Loaded, // decoded: waiting for the owner to make it a texture
            Ready,
            Failed,
        };
        State state = State::Queued;
        OverlayImage image;
        ImTextureID texture = 0;
        int attempts = 0;
        std::chrono::steady_clock::time_point retry_at;
    };

    bool Queue(const std::string& key, Entry& entry);
    void Fail(Entry& entry, std::string reason, bool final);
    void Run();

    Upload upload_;
    Limits limits_;
    Fetch fetch_;
    Decode decode_;
    std::shared_ptr<platform::HttpCancel> cancel_;

    mutable std::mutex mutex_;
    std::condition_variable wake_, settled_;
    std::map<std::string, Entry> entries_; // never erased: the worker keeps references
    std::deque<std::string> queue_;
    std::size_t live_ = 0; // entries holding a texture's place (all but failed)
    std::size_t busy_ = 0; // downloads under way
    std::size_t downloads_ = 0;
    std::string last_error_;
    bool stop_ = false;
    std::thread worker_;
};

// The URL the cache downloads for a key: Discord's avatars
// (https://cdn.discordapp.com/avatars/<user>/<hash>.<ext>?size=N) as a PNG of
// `size` pixels or a little more (its sizes are powers of two); others as they are.
std::string OverlayImageUrl(const std::string& key, int size);
// Downloads through platform::HttpGet (https, and http for an http:// URL).
OverlayImageCache::Fetch OverlayHttpFetch();
// A PNG through SDL's decoder. Refuses a picture wider or taller than
// `max_side` from its header, before decoding it (a small file can hold a
// huge picture).
bool DecodeOverlayPng(const std::string& bytes, OverlayImage& image, int max_side = 1024);
// Scales `image` down (each pixel the average of those it covers, by their
// alpha) to fit `size` x `size`, keeping its shape; unchanged when it fits.
void ShrinkOverlayImage(OverlayImage& image, int size);
} // namespace mscharged::overlay

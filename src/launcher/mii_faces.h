#pragma once

#include "platform/mii_head.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// The launcher's Mii heads (mii_head.h): the Mii parts of the game image,
// read on a worker thread the first time a head is asked for, and heads drawn
// from them on that worker, which the host turns into textures on its own
// thread. A head is asked for every frame it shows; until it is drawn (a few
// frames) the caller shows the Mii's figure. Heads are kept by their face, so
// a Mii's name or create ID doesn't draw it again; a bounded number of
// textures stays, the ones shown least recently going first.
namespace mscharged::launcher
{
// How many textures, and how many bytes of pixels, stay. Heads shown in the
// last frame stay beyond them.
struct MiiFaceLimits
{
    std::size_t textures = 320;
    std::size_t bytes = 48u << 20;
};

// How a head looks: its size in pixels (square), its background (the Mii's
// favourite colour, as the Wii's icons, or none) and expression.
enum class MiiHeadBackground
{
    Favorite,
    Transparent
};
struct MiiHeadLook
{
    int size = 128;
    MiiHeadBackground background = MiiHeadBackground::Favorite;
    platform::mii::Expression expression = platform::mii::Expression::Normal;
};

class MiiFaces
{
public:
    using Texture = void*;
    struct Hooks
    {
        // On the host's thread: a texture of a head, and freeing one.
        std::function<Texture(const platform::mii::RgbaImage&)> make;
        std::function<void(Texture)> free;
        // On the worker: the parts of a game image (throws when it has none)
        // and a head drawn from them. Empty: ReadMiiPartsFromImage and
        // RenderMiiHead.
        std::function<platform::mii::MiiParts(const std::filesystem::path&)> load;
        std::function<platform::mii::RgbaImage(const platform::mii::MiiParts&, const platform::mii::MiiFace&,
                                               const platform::mii::MiiHeadOptions&)>
            render;
    };
    using Limits = MiiFaceLimits;

    explicit MiiFaces(Hooks hooks, Limits limits = {});
    ~MiiFaces();
    MiiFaces(const MiiFaces&) = delete;
    MiiFaces& operator=(const MiiFaces&) = delete;

    // The game image whose parts draw the heads (empty: none). Its parts are
    // read when a head is first asked for. Every version holds the same
    // parts, so heads drawn from another image's stay until this one's are
    // read; an image without parts drops them.
    void SetImage(const std::filesystem::path& image);

    // Once a frame, before drawing: textures for the heads the worker drew,
    // heads nobody asked for in the last frames no longer drawn, and the
    // textures over the limits freed.
    void Update();

    using Background = MiiHeadBackground;
    using Look = MiiHeadLook;
    // The head of `face` (or of a 74-byte record): its texture, or nullptr
    // until it is drawn, without parts, or for a face the Mii library
    // rejects. Heads asked for with a higher `priority` are drawn first,
    // else the latest asked for.
    Texture Head(const platform::mii::MiiFace& face, const Look& look = {}, int priority = 0);
    Texture Head(std::string_view record, const Look& look = {}, int priority = 0);
    // The texture of a head drawn already, without asking for one.
    Texture Cached(const platform::mii::MiiFace& face, const Look& look = {});

    enum class Parts
    {
        None,    // no image, or no head asked for yet
        Loading, // being read
        Ready,
        Failed // the image has none: Error() says why
    };
    Parts PartsState() const;
    std::string Error() const;
    // Nothing being read or drawn, and nothing drawn waiting for its texture.
    bool Settled() const;
    // For tests: the textures kept, their bytes, and heads drawn so far.
    std::size_t TextureCount() const { return textures_.size(); }
    std::size_t TextureBytes() const { return texture_bytes_; }
    std::size_t Drawn() const;

private:
    struct Job
    {
        platform::mii::MiiFace face;
        platform::mii::MiiHeadOptions options;
        int priority = 0;
        std::uint64_t asked = 0, id = 0;
        bool rendering = false, done = false;
        std::optional<platform::mii::RgbaImage> image; // done: nullopt when it could not be drawn
    };
    struct Entry
    {
        Texture texture = nullptr; // nullptr: the face could not be drawn
        std::size_t bytes = 0;
        std::uint64_t used = 0;
    };

    static std::string Key(const platform::mii::MiiFace& face, const Look& look);
    void Work();
    void FreeAll();

    Hooks hooks_;
    Limits limits_;
    // Host thread only.
    std::map<std::string, Entry> textures_;
    std::size_t texture_bytes_ = 0;
    std::uint64_t frame_ = 1;
    std::filesystem::path image_;
    // Shared with the worker.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, Job> jobs_;
    std::uint64_t next_job_ = 1, generation_ = 0, drawn_ = 0;
    std::filesystem::path load_; // to read: the image (generation_ tells which)
    bool load_pending_ = false;
    Parts parts_state_ = Parts::None;
    std::shared_ptr<const platform::mii::MiiParts> parts_;
    std::string error_;
    bool drop_textures_ = false; // the new image has no parts: Update drops the old heads
    bool stop_ = false;
    std::thread worker_;
};
} // namespace mscharged::launcher

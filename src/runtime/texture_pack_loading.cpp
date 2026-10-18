#include "runtime/texture_pack_loading.h"

#include "platform/hd_recolour.h"
#include "platform/path.h"
#include "platform/session_log.h"
#include "platform/texture_packs.h"

#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include "gfx/dds_io.hpp"
#include "gfx/png_io.hpp"
#include <xxhash.h>

#include <algorithm>
#include <cinttypes>
#include <condition_variable>
#include <deque>
#include <climits>
#include <cstdio>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace mscharged
{
namespace
{
bool ReadPackFile(void* pack, const char* path, std::vector<std::uint8_t>& out)
{
    return static_cast<const platform::TexturePackFiles*>(pack)->Read(path, out);
}

using KeyFields = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, bool>;
KeyFields Fields(const aurora::texture::TextureSourceKey& key)
{
    return {key.textureHash, key.tlutHash, key.width, key.height, key.format, key.hasTlut};
}

// The image that replaces each texture (the first pack's), for the HD
// recolour of Mirror match's away kits.
struct PackImage
{
    const platform::TexturePackFiles* files;
    std::string path;
};

// A texture the game recoloured whose original a pack replaces: Aurora's
// texture workers ask for its HD image, made from the pack's (ReadRecoloured).
struct RecolourJob
{
    std::string name; // the replacement's file name (the recoloured texture's key)
    PackImage source;
    platform::KitRecolour kit;
    bool mips = false;
    std::mutex mutex;
    std::condition_variable done;
    std::shared_ptr<const std::vector<std::uint8_t>> made; // kept while within kMadeBudget
    bool making = false, failed = false;
};

std::mutex gMutex;
std::map<KeyFields, PackImage>* gImages = nullptr;               // the packs' images, never freed
std::map<KeyFields, std::unique_ptr<RecolourJob>>* gJobs = nullptr; // never freed: Aurora's workers read them
// Made images kept for Aurora's next read (a thumbnail read, then the full one;
// again after Aurora evicted it), oldest first.
constexpr std::size_t kMadeBudget = 256u << 20;
std::list<RecolourJob*> gMadeOrder;
std::size_t gMadeBytes = 0;

void KeepMade(RecolourJob* job, std::shared_ptr<const std::vector<std::uint8_t>> made)
{
    std::lock_guard lock(gMutex);
    gMadeOrder.remove(job);
    gMadeOrder.push_back(job);
    gMadeBytes += made->size();
    {
        std::lock_guard jobLock(job->mutex);
        if (job->made) gMadeBytes -= job->made->size();
        job->made = std::move(made);
    }
    while (gMadeBytes > kMadeBudget && gMadeOrder.size() > 1)
    {
        RecolourJob* oldest = gMadeOrder.front();
        gMadeOrder.pop_front();
        std::lock_guard jobLock(oldest->mutex);
        if (oldest->made) gMadeBytes -= oldest->made->size();
        oldest->made.reset();
    }
}

// The pack's image decoded to 8-bit RGBA (its largest level), or false.
bool DecodePackImage(const PackImage& source, platform::RgbaImage& image, std::string& why)
{
    std::vector<std::uint8_t> bytes;
    if (!source.files->Read(source.path, bytes))
    {
        why = "it cannot be read";
        return false;
    }
    const bool png = source.path.size() >= 4
        && (source.path.compare(source.path.size() - 4, 4, ".png") == 0
            || source.path.compare(source.path.size() - 4, 4, ".PNG") == 0);
    const aurora::ArrayRef<std::uint8_t> view(bytes.data(), bytes.size());
    auto texture = png ? aurora::gfx::png::parse_png_bytes(view) : aurora::gfx::dds::parse_dds_bytes(view);
    if (!texture)
    {
        why = "it cannot be decoded";
        return false;
    }
    const bool bgra = texture->format == wgpu::TextureFormat::BGRA8Unorm;
    if (texture->format != wgpu::TextureFormat::RGBA8Unorm && !bgra)
    {
        why = "it is compressed (only uncompressed images are recoloured)";
        return false;
    }
    const std::size_t size = std::size_t(texture->width) * texture->height * 4;
    if (texture->data.size() < size)
    {
        why = "it is incomplete";
        return false;
    }
    image.width = texture->width;
    image.height = texture->height;
    image.rgba.assign(texture->data.data(), texture->data.data() + size);
    if (bgra)
        for (std::size_t i = 0; i < size; i += 4) std::swap(image.rgba[i], image.rgba[i + 2]);
    return true;
}

// The HD image of a recoloured texture, made once (again after it left the
// kept images); a second caller waits for the first. Null when it cannot be made.
std::shared_ptr<const std::vector<std::uint8_t>> Make(RecolourJob* job)
{
    {
        std::unique_lock lock(job->mutex);
        job->done.wait(lock, [&] { return !job->making; });
        if (job->failed) return nullptr;
        if (job->made) return job->made;
        job->making = true;
    }
    platform::RgbaImage image;
    std::string why;
    std::shared_ptr<const std::vector<std::uint8_t>> made;
    if (DecodePackImage(job->source, image, why) && platform::RecolourHdImage(job->kit, image, &why))
        made = std::make_shared<const std::vector<std::uint8_t>>(platform::EncodeDds(image, job->mips));
    if (made)
    {
        KeepMade(job, made);
        if (!why.empty()) platform::SessionLogLine("Texture packs: " + job->source.path + " recoloured in HD " + why);
    }
    else
        platform::SessionLogLine("Texture packs: " + job->source.path + " is not recoloured in HD (" + why
            + "); the game's own recolour is shown");
    {
        std::lock_guard lock(job->mutex);
        job->failed = !made;
        job->making = false;
    }
    job->done.notify_all();
    return made;
}

// Aurora's texture workers ask for the HD image when the texture is drawn.
bool ReadRecoloured(void* user, const char* path, std::vector<std::uint8_t>& out)
{
    auto* job = static_cast<RecolourJob*>(user);
    if (job->name != path) return false; // the mip levels are inside the image
    const auto made = Make(job);
    if (!made) return false;
    out = *made;
    return true;
}

// Images are made as soon as the game recolours, on a thread of their own:
// Aurora's workers are busy with the packs' other images while a match loads,
// and the recoloured picture (a loading screen's) may show only briefly. The
// thread and its queue live until the process ends.
struct MakeQueue
{
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<RecolourJob*> jobs;
    bool started = false;
};

void MakeSoon(RecolourJob* job)
{
    static auto* queue = new MakeQueue;
    std::lock_guard lock(queue->mutex);
    queue->jobs.push_back(job);
    queue->ready.notify_one();
    if (queue->started) return;
    queue->started = true;
    std::thread([] {
        for (;;)
        {
            RecolourJob* next;
            {
                std::unique_lock lock(queue->mutex);
                queue->ready.wait(lock, [] { return !queue->jobs.empty(); });
                next = queue->jobs.front();
                queue->jobs.pop_front();
            }
            Make(next);
        }
    }).detach();
}

// The game module recoloured a texture (Mirror match): where a pack replaces
// its original, the pack's image is recoloured alike for the recoloured one.
void OnRecoloured(const ChargedRecolouredTexture* texture)
{
    if (!texture || !texture->original || !texture->recoloured || !texture->mask || !texture->original_rgba
        || !texture->width || !texture->height || !texture->bytes)
        return;
    const std::uint64_t before = XXH64(texture->original, texture->bytes, 0);
    const std::uint64_t after = XXH64(texture->recoloured, texture->bytes, 0);
    if (before == after) return;
    const KeyFields original{before, 0, texture->width, texture->height, texture->gx_format, false};
    const KeyFields recoloured{after, 0, texture->width, texture->height, texture->gx_format, false};
    RecolourJob* job = nullptr;
    {
        std::lock_guard lock(gMutex);
        if (!gImages || !gJobs || gJobs->count(recoloured)) return;
        const auto image = gImages->find(original);
        if (image == gImages->end()) return;
        auto made = std::make_unique<RecolourJob>();
        char name[96];
        std::snprintf(name, sizeof name, "tex1_%ux%u_%016" PRIx64 "_%u.dds", texture->width, texture->height, after,
                      texture->gx_format);
        made->name = name;
        made->source = image->second;
        const std::size_t texels = std::size_t(texture->width) * texture->height;
        made->kit.picture = texture->kind == 2;
        made->kit.width = texture->width;
        made->kit.height = texture->height;
        made->kit.mask.assign(texture->mask, texture->mask + texels);
        made->kit.rgba.assign(texture->original_rgba, texture->original_rgba + texels * 4);
        std::copy(texture->to, texture->to + 3, made->kit.to);
        made->mips = texture->levels > 1;
        job = made.get();
        gJobs->emplace(recoloured, std::move(made));
    }
    // Above every pack: no pack has an image of a texture the port makes.
    const auto registration = aurora::texture::register_virtual_replacement(
        job->name, {.read = ReadRecoloured, .userData = job}, {.priority = INT32_MAX});
    if (!registration.id) return;
    platform::SessionLogLine("Texture packs: " + job->source.path + " recoloured for Mirror match");
    MakeSoon(job);
}

void LoadPacks(const Settings& settings, const std::filesystem::path& config_path, std::string_view game_id)
{
    if (settings.dump_textures)
        platform::SessionLogLine("Texture dumps: on (the game's textures are saved to data/texture_dumps)");
    if (!settings.texture_packs)
    {
        platform::SessionLogLine("Texture packs: off");
        return;
    }
    const auto folder = ResolveTextureFolder(settings, config_path);
    const auto off = platform::SplitPackNames(settings.texture_packs_off);
    auto packs = platform::OrderTexturePacks(platform::FindTexturePacks(folder),
                                             platform::SplitPackNames(settings.texture_pack_order));
    packs.erase(std::remove_if(packs.begin(), packs.end(),
                               [&](const platform::TexturePack& pack) {
                                   return std::find(off.begin(), off.end(), pack.name) != off.end();
                               }),
                packs.end());
    if (packs.empty())
    {
        platform::SessionLogLine("Texture packs: none in " + PathUtf8(folder));
        return;
    }
    // Aurora's texture workers read the packs until the process ends, so
    // they are never freed.
    static auto* loaded = new std::vector<std::unique_ptr<platform::TexturePackFiles>>;
    auto images = std::make_unique<std::map<KeyFields, PackImage>>();
    std::string summary;
    std::size_t total = 0;
    // The first pack gets the highest priority.
    auto priority = std::int32_t(packs.size());
    for (const auto& pack : packs)
    {
        const auto current = priority--;
        if (!pack.error.empty())
        {
            platform::SessionLogLine("Texture packs: " + pack.name + " skipped: " + pack.error);
            continue;
        }
        std::unique_ptr<platform::TexturePackFiles> files;
        try
        {
            files = std::make_unique<platform::TexturePackFiles>(pack, game_id);
        }
        catch (const std::exception& failure)
        {
            platform::SessionLogLine("Texture packs: " + pack.name + " skipped: " + failure.what());
            continue;
        }
        // Within a pack the first image of a name wins (as in Dolphin).
        std::set<KeyFields> seen;
        std::size_t count = 0;
        for (const auto& image : files->Images())
        {
            const auto slash = image.rfind('/');
            const auto key =
                aurora::texture::parse_replacement_filename(slash == std::string::npos ? image : image.substr(slash + 1));
            if (!key || !seen.insert(Fields(*key)).second) continue;
            const auto registration = aurora::texture::register_virtual_replacement(
                image, {.read = ReadPackFile, .userData = files.get()}, {.priority = current});
            if (!registration.id) continue;
            ++count;
            // An earlier pack's image of the same texture wins.
            images->emplace(Fields(*key), PackImage{files.get(), image});
        }
        if (!count) continue;
        total += count;
        summary += (summary.empty() ? "" : ", ") + pack.name + " (" + std::to_string(count) + ")";
        loaded->push_back(std::move(files));
    }
    platform::SessionLogLine(total ? "Texture packs: " + summary : "Texture packs: no usable images in " + PathUtf8(folder));
    if (!total) return;
    std::lock_guard lock(gMutex);
    gImages = images.release();
    gJobs = new std::map<KeyFields, std::unique_ptr<RecolourJob>>;
}
}

void LoadTexturePacks(const Settings& settings, const std::filesystem::path& config_path, std::string_view game_id)
{
    // A pack never stops the game from starting: it plays with its own textures.
    try
    {
        LoadPacks(settings, config_path, game_id);
    }
    catch (const std::exception& failure)
    {
        platform::SessionLogLine(std::string("Texture packs: not loaded: ") + failure.what());
    }
}

ChargedRecolouredTextureHook TexturePackRecolourHook()
{
    std::lock_guard lock(gMutex);
    return gImages ? OnRecoloured : nullptr;
}
}

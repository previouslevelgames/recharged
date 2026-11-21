#include "overlay/overlay_images.h"

#include "platform/http_client.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mscharged::overlay
{
namespace
{
using Clock = std::chrono::steady_clock;

// Avatars' URLs only; nothing else is downloaded ("mii:<profile>" and the
// launcher preview's made-up keys stay 0). A sane length too.
bool Downloadable(const std::string& key)
{
    return key.size() <= 2048 && (key.rfind("https://", 0) == 0 || key.rfind("http://", 0) == 0);
}

bool Whole(const OverlayImage& image)
{
    return image.width > 0 && image.height > 0
        && image.rgba.size() == std::size_t(image.width) * std::size_t(image.height) * 4;
}

std::uint32_t BigEndian32(const unsigned char* bytes)
{
    return std::uint32_t(bytes[0]) << 24 | std::uint32_t(bytes[1]) << 16 | std::uint32_t(bytes[2]) << 8
        | std::uint32_t(bytes[3]);
}
} // namespace

OverlayImageCache::OverlayImageCache(Upload upload, Limits limits, Fetch fetch, Decode decode)
    : upload_(std::move(upload)), limits_(limits), fetch_(fetch ? std::move(fetch) : OverlayHttpFetch()),
      decode_(decode ? std::move(decode) : Decode([](const std::string& bytes, OverlayImage& image) {
                  return DecodeOverlayPng(bytes, image);
              })),
      cancel_(std::make_shared<platform::HttpCancel>())
{
}

OverlayImageCache::~OverlayImageCache()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cancel_->Cancel();
    wake_.notify_all();
    settled_.notify_all();
    if (worker_.joinable()) worker_.join();
}

ImTextureID OverlayImageCache::Get(const std::string& key)
{
    if (!Downloadable(key)) return ImTextureID(0);
    std::unique_lock<std::mutex> lock(mutex_);
    auto found = entries_.find(key);
    if (found == entries_.end())
    {
        // A new picture, while there is room for it.
        if (stop_ || entries_.size() >= limits_.keys || live_ >= limits_.textures) return ImTextureID(0);
        found = entries_.emplace(key, Entry{}).first;
        ++live_;
        if (!Queue(key, found->second)) Fail(found->second, "no thread for the pictures", true);
        return ImTextureID(0);
    }
    Entry& entry = found->second;
    switch (entry.state)
    {
    case Entry::State::Ready: return entry.texture;
    case Entry::State::Loaded:
    {
        // The owner makes it a texture, outside the lock: the worker leaves a
        // loaded entry alone, and only this thread changes the map.
        OverlayImage image = std::move(entry.image);
        entry.image = {};
        entry.state = Entry::State::Ready;
        lock.unlock();
        const ImTextureID texture = upload_ ? upload_(image.width, image.height, image.rgba.data()) : ImTextureID(0);
        lock.lock();
        if (texture) entry.texture = texture;
        else Fail(entry, "the picture could not be made a texture", true); // not again
        return texture;
    }
    case Entry::State::Failed:
        // Again a while later, a few times, if there's room.
        if (!stop_ && entry.attempts < limits_.attempts && Clock::now() >= entry.retry_at
            && live_ < limits_.textures)
        {
            ++live_;
            if (!Queue(key, entry)) Fail(entry, "no thread for the pictures", true);
        }
        return ImTextureID(0);
    default: return ImTextureID(0);
    }
}

std::function<ImTextureID(const std::string& key)> OverlayImageCache::Images()
{
    return [this](const std::string& key) { return Get(key); };
}

// Under the lock: `entry` (holding a place) to the worker; false when it can't start.
bool OverlayImageCache::Queue(const std::string& key, Entry& entry)
{
    entry.state = Entry::State::Queued;
    ++entry.attempts;
    if (!worker_.joinable())
    {
        try
        {
            worker_ = std::thread(&OverlayImageCache::Run, this);
        }
        catch (const std::system_error&)
        {
            return false;
        }
    }
    queue_.push_back(key);
    wake_.notify_one();
    return true;
}

// Under the lock: the entry's try failed; it gives its place back. `final`: never again.
void OverlayImageCache::Fail(Entry& entry, std::string reason, bool final)
{
    entry.state = Entry::State::Failed;
    entry.image = {};
    entry.retry_at = Clock::now() + limits_.retry;
    if (final) entry.attempts = std::max(entry.attempts, limits_.attempts);
    --live_;
    last_error_ = std::move(reason);
}

void OverlayImageCache::Run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        const std::string key = std::move(queue_.front());
        queue_.pop_front();
        Entry& entry = entries_.at(key);
        entry.state = Entry::State::Loading;
        ++busy_;
        ++downloads_;
        lock.unlock();

        OverlayImage image;
        std::string error;
        try
        {
            const std::string bytes = fetch_(OverlayImageUrl(key, limits_.size), limits_.bytes, cancel_);
            if (bytes.size() > limits_.bytes) error = "the picture is too large";
            else if (!decode_(bytes, image) || !Whole(image)) error = "the picture could not be read";
            else ShrinkOverlayImage(image, limits_.size);
        }
        catch (const std::exception& failure)
        {
            error = failure.what();
            if (error.empty()) error = "the download failed";
        }
        catch (...)
        {
            error = "the download failed";
        }

        lock.lock();
        --busy_;
        if (error.empty())
        {
            entry.image = std::move(image);
            entry.state = Entry::State::Loaded;
        }
        else Fail(entry, error, false); // not the address: logs are shared, it names a player
        settled_.notify_all();
    }
}

bool OverlayImageCache::Settle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return settled_.wait_for(lock, timeout, [this] { return stop_ || (queue_.empty() && busy_ == 0); });
}

OverlayImageCache::Stats OverlayImageCache::Statistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats;
    for (const auto& [key, entry] : entries_)
    {
        if (entry.state == Entry::State::Ready) ++stats.textures;
        else if (entry.state == Entry::State::Failed) ++stats.failed;
        else ++stats.pending;
    }
    stats.downloads = downloads_;
    stats.last_error = last_error_;
    return stats;
}

std::string OverlayImageUrl(const std::string& key, int size)
{
    static const std::string kAvatars = "https://cdn.discordapp.com/avatars/";
    if (key.rfind(kAvatars, 0) != 0) return key;
    int pixels = 16;
    while (pixels < size && pixels < 4096) pixels *= 2;
    std::string path = key.substr(0, key.find_first_of("?#"));
    const auto slash = path.rfind('/'), dot = path.rfind('.');
    if (dot != std::string::npos && dot > slash) path.erase(dot);
    return path + ".png?size=" + std::to_string(pixels);
}

OverlayImageCache::Fetch OverlayHttpFetch()
{
    return [](const std::string& url, std::size_t max_bytes, const std::shared_ptr<platform::HttpCancel>& cancel) {
        platform::HttpRequest request;
        request.url = url;
        request.allow_http = url.rfind("http://", 0) == 0;
        request.max_size = max_bytes;
        request.cancel = cancel;
        request.connect_timeout = 10;
        request.stall_timeout = 15;
        request.headers = {"Accept: image/png"};
        platform::HttpResponse response = platform::HttpGet(request);
        if (response.status != 200) throw std::runtime_error("HTTP status " + std::to_string(response.status));
        return std::move(response.body);
    };
}

bool DecodeOverlayPng(const std::string& bytes, OverlayImage& image, int max_side)
{
    // Its size from its header (the signature, then IHDR's width and height).
    static constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() < 33 || std::memcmp(data, kSignature, 8) != 0 || std::memcmp(data + 12, "IHDR", 4) != 0)
        return false;
    const std::uint32_t width = BigEndian32(data + 16), height = BigEndian32(data + 20);
    if (width == 0 || height == 0 || max_side <= 0 || width > std::uint32_t(max_side)
        || height > std::uint32_t(max_side))
        return false;

    SDL_IOStream* stream = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (!stream) return false;
    SDL_Surface* loaded = SDL_LoadPNG_IO(stream, true);
    if (!loaded) return false;
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba) return false;
    bool ok = rgba->w == int(width) && rgba->h == int(height) && SDL_LockSurface(rgba);
    if (ok)
    {
        image.width = rgba->w;
        image.height = rgba->h;
        const std::size_t row = std::size_t(rgba->w) * 4;
        image.rgba.resize(row * std::size_t(rgba->h));
        for (int y = 0; y < rgba->h; ++y)
            std::memcpy(image.rgba.data() + row * std::size_t(y),
                        static_cast<const unsigned char*>(rgba->pixels) + std::size_t(rgba->pitch) * std::size_t(y),
                        row);
        SDL_UnlockSurface(rgba);
    }
    SDL_DestroySurface(rgba);
    return ok;
}

void ShrinkOverlayImage(OverlayImage& image, int size)
{
    if (size <= 0 || !Whole(image) || (image.width <= size && image.height <= size)) return;
    const double scale = double(size) / double(std::max(image.width, image.height));
    const int width = std::clamp(int(std::lround(image.width * scale)), 1, size);
    const int height = std::clamp(int(std::lround(image.height * scale)), 1, size);
    std::vector<std::uint8_t> out(std::size_t(width) * std::size_t(height) * 4);
    for (int y = 0; y < height; ++y)
    {
        const int y0 = int(std::int64_t(y) * image.height / height);
        const int y1 = std::max(y0 + 1, int(std::int64_t(y + 1) * image.height / height));
        for (int x = 0; x < width; ++x)
        {
            const int x0 = int(std::int64_t(x) * image.width / width);
            const int x1 = std::max(x0 + 1, int(std::int64_t(x + 1) * image.width / width));
            // By alpha: transparent pixels give no colour (no dark fringes).
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                {
                    const std::uint8_t* p =
                        image.rgba.data() + (std::size_t(sy) * std::size_t(image.width) + std::size_t(sx)) * 4;
                    const double alpha = p[3];
                    r += p[0] * alpha;
                    g += p[1] * alpha;
                    b += p[2] * alpha;
                    a += alpha;
                }
            std::uint8_t* q = out.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
            const double count = double((y1 - y0) * (x1 - x0));
            if (a > 0)
            {
                q[0] = std::uint8_t(std::lround(r / a));
                q[1] = std::uint8_t(std::lround(g / a));
                q[2] = std::uint8_t(std::lround(b / a));
            }
            q[3] = std::uint8_t(std::lround(a / count));
        }
    }
    image.width = width;
    image.height = height;
    image.rgba = std::move(out);
}
} // namespace mscharged::overlay

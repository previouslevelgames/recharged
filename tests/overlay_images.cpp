// Players' pictures for the overlay (src/overlay/overlay_images): with a made-up
// download and decoder, the cache (each picture fetched once, a texture made
// on the asking thread, 0 meanwhile), failures (remembered, a few tries
// minutes apart, never a storm), the caps (textures, keys, bytes, picture
// size), keys that aren't URLs, and shutdown during a download; with SDL's
// decoder, real PNGs (and one whose header claims a huge picture); Discord's
// avatar URLs; scaling down.

#include "overlay/overlay_images.h"

#include "platform/http_client.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_surface.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mscharged::overlay;
using namespace std::chrono_literals;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (ok) return;
    std::fprintf(stderr, "overlay_images.cpp:%d: %s\n", line, what);
    ++failures;
}
#define CHECK(x) Check((x), #x, __LINE__)

// A made-up network and GPU: downloads count per URL ("bad" ones fail), the
// decoder makes a picture of the size in the bytes, uploads count and say
// which thread made them.
struct Fakes
{
    std::mutex mutex;
    std::map<std::string, int> fetched;
    std::size_t max_bytes = 0;
    int width = 4, height = 4;
    std::atomic<int> uploads{0};
    std::thread::id upload_thread;
    int last_width = 0, last_height = 0;
    bool refuse_upload = false;
    std::uintptr_t next = 100;

    OverlayImageCache::Fetch Fetch()
    {
        return [this](const std::string& url, std::size_t limit, const std::shared_ptr<mscharged::platform::HttpCancel>&) {
            std::lock_guard<std::mutex> lock(mutex);
            ++fetched[url];
            max_bytes = limit;
            if (url.find("bad") != std::string::npos) throw std::runtime_error("HTTP status 404");
            if (url.find("huge") != std::string::npos) return std::string(limit + 1, 'x');
            if (url.find("garbage") != std::string::npos) return std::string("garbage");
            return "picture:" + std::to_string(width) + "x" + std::to_string(height);
        };
    }
    static OverlayImageCache::Decode Decode()
    {
        return [](const std::string& bytes, OverlayImage& image) {
            int w = 0, h = 0;
            if (std::sscanf(bytes.c_str(), "picture:%dx%d", &w, &h) != 2) return false;
            image.width = w;
            image.height = h;
            image.rgba.assign(std::size_t(w) * std::size_t(h) * 4, 200);
            return true;
        };
    }
    OverlayImageCache::Upload Upload()
    {
        return [this](int w, int h, const void* rgba) {
            ++uploads;
            upload_thread = std::this_thread::get_id();
            last_width = w;
            last_height = h;
            if (refuse_upload || !rgba) return ImTextureID(0);
            return ImTextureID(next++);
        };
    }
    int Fetched(const std::string& url)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = fetched.find(url);
        return found == fetched.end() ? 0 : found->second;
    }
};

const std::string kA = "https://example.com/a.png", kB = "https://example.com/b.png";

// Asked twice, a picture is downloaded once and made a texture once, on the
// thread that asks; 0 until then.
void TestCache()
{
    Fakes fakes;
    OverlayImageCache cache(fakes.Upload(), {}, fakes.Fetch(), Fakes::Decode());
    CHECK(cache.Get(kA) == 0);
    CHECK(cache.Settle(2s));
    const ImTextureID a = cache.Get(kA);
    CHECK(a != 0);
    CHECK(fakes.upload_thread == std::this_thread::get_id());
    CHECK(fakes.last_width == 4 && fakes.last_height == 4);
    for (int n = 0; n < 50; ++n) CHECK(cache.Get(kA) == a);
    CHECK(fakes.Fetched(kA) == 1 && fakes.uploads == 1);
    // Another picture, through the function the overlay is given.
    const auto images = cache.Images();
    CHECK(images(kB) == 0);
    CHECK(cache.Settle(2s));
    const ImTextureID b = images(kB);
    CHECK(b != 0 && b != a && fakes.uploads == 2);
    const auto stats = cache.Statistics();
    CHECK(stats.textures == 2 && stats.pending == 0 && stats.failed == 0 && stats.downloads == 2);
    CHECK(fakes.max_bytes == OverlayImageLimits{}.bytes);
}

// Keys that aren't URLs (a Mii's, the preview's made-up ones, none): 0, and nothing asked.
void TestOtherKeys()
{
    Fakes fakes;
    OverlayImageCache cache(fakes.Upload(), {}, fakes.Fetch(), Fakes::Decode());
    for (const char* key : {"mii:1000000004", "preview:mario", "", "file:///etc/passwd", "ftp://example.com/a.png"})
        CHECK(cache.Get(key) == 0);
    CHECK(cache.Get("https://example.com/" + std::string(3000, 'x')) == 0);
    CHECK(cache.Settle(1s));
    CHECK(cache.Statistics().downloads == 0 && fakes.uploads == 0);
}

// A failure stays 0 and is not asked again every frame; a few times, a while apart.
void TestFailures()
{
    {
        Fakes fakes;
        OverlayImageCache cache(fakes.Upload(), {}, fakes.Fetch(), Fakes::Decode());
        const std::string bad = "https://example.com/bad.png";
        for (int frame = 0; frame < 200; ++frame)
        {
            CHECK(cache.Get(bad) == 0);
            if (frame % 20 == 0) cache.Settle(1s);
        }
        CHECK(fakes.Fetched(bad) == 1 && fakes.uploads == 0);
        const auto stats = cache.Statistics();
        CHECK(stats.failed == 1 && stats.last_error.find("404") != std::string::npos);
        // Too large, unreadable: failures too, and the cache goes on.
        for (const char* key : {"https://example.com/huge.png", "https://example.com/garbage.png"})
        {
            CHECK(cache.Get(key) == 0);
            CHECK(cache.Settle(1s));
            CHECK(cache.Get(key) == 0);
        }
        CHECK(cache.Statistics().failed == 3);
        CHECK(cache.Get(kA) == 0 && cache.Settle(1s) && cache.Get(kA) != 0);
    }
    {
        // Tries again once the wait is over, up to the attempts.
        Fakes fakes;
        OverlayImageLimits limits;
        limits.retry = 0ms;
        limits.attempts = 3;
        OverlayImageCache cache(fakes.Upload(), limits, fakes.Fetch(), Fakes::Decode());
        const std::string bad = "https://example.com/bad.png";
        for (int frame = 0; frame < 50; ++frame)
        {
            cache.Get(bad);
            cache.Settle(1s);
        }
        CHECK(fakes.Fetched(bad) == 3);
    }
    {
        // A picture the host can't make a texture of: never again.
        Fakes fakes;
        fakes.refuse_upload = true;
        OverlayImageLimits limits;
        limits.retry = 0ms;
        OverlayImageCache cache(fakes.Upload(), limits, fakes.Fetch(), Fakes::Decode());
        for (int frame = 0; frame < 20; ++frame)
        {
            CHECK(cache.Get(kA) == 0);
            cache.Settle(1s);
        }
        CHECK(fakes.uploads == 1 && fakes.Fetched(kA) == 1);
    }
}

// The caps: so many textures, so many keys; larger pictures scaled down.
void TestCaps()
{
    Fakes fakes;
    OverlayImageLimits limits;
    limits.textures = 3;
    limits.keys = 5;
    limits.size = 64;
    OverlayImageCache cache(fakes.Upload(), limits, fakes.Fetch(), Fakes::Decode());
    const auto key = [](int n) { return "https://example.com/" + std::to_string(n) + ".png"; };
    for (int n = 0; n < 6; ++n) cache.Get(key(n));
    CHECK(cache.Settle(2s));
    int made = 0;
    for (int n = 0; n < 6; ++n) made += cache.Get(key(n)) != 0;
    CHECK(made == 3 && fakes.uploads == 3 && cache.Statistics().downloads == 3);
    // Room once more is no more: still three.
    for (int n = 6; n < 10; ++n) CHECK(cache.Get(key(n)) == 0);
    CHECK(cache.Settle(1s) && fakes.uploads == 3);

    // Failures take no texture's place, but count as keys.
    Fakes others;
    others.width = 300;
    others.height = 150;
    OverlayImageCache second(others.Upload(), limits, others.Fetch(), Fakes::Decode());
    for (int n = 0; n < 4; ++n)
    {
        second.Get("https://example.com/bad" + std::to_string(n) + ".png");
        CHECK(second.Settle(2s));
    }
    second.Get(kA);
    CHECK(second.Get(kB) == 0); // the sixth key
    CHECK(second.Settle(2s));
    CHECK(second.Get(kA) != 0 && second.Get(kB) == 0);
    CHECK(others.last_width == 64 && others.last_height == 32);
}

// Gone while a download hangs: it is cancelled and the thread joins at once;
// a picture loaded but not made yet is dropped.
void TestShutdown()
{
    std::atomic<bool> started{false}, saw_cancel{false};
    std::atomic<int> uploads{0};
    const auto hang = [&](const std::string&, std::size_t, const std::shared_ptr<mscharged::platform::HttpCancel>& cancel) {
        started = true;
        for (int n = 0; n < 5000 && !cancel->Cancelled(); ++n) std::this_thread::sleep_for(1ms);
        saw_cancel = cancel->Cancelled();
        throw std::runtime_error("cancelled");
        return std::string();
    };
    const auto upload = [&](int, int, const void*) {
        ++uploads;
        return ImTextureID(1);
    };
    std::chrono::steady_clock::duration closing{};
    {
        std::optional<OverlayImageCache> cache;
        cache.emplace(upload, OverlayImageLimits{}, hang, Fakes::Decode());
        cache->Get(kA);
        cache->Get(kB); // waits behind it
        for (int n = 0; n < 2000 && !started; ++n) std::this_thread::sleep_for(1ms);
        CHECK(started);
        CHECK(!cache->Settle(20ms)); // a download under way
        const auto before = std::chrono::steady_clock::now();
        cache.reset();
        closing = std::chrono::steady_clock::now() - before;
    }
    CHECK(saw_cancel && closing < 1s && uploads == 0);
    {
        Fakes fakes;
        {
            OverlayImageCache cache(fakes.Upload(), {}, fakes.Fetch(), Fakes::Decode());
            cache.Get(kA);
            CHECK(cache.Settle(2s));
        }
        CHECK(fakes.uploads == 0 && fakes.Fetched(kA) == 1);
    }
    // Never used: no thread, nothing to wait for.
    {
        Fakes fakes;
        OverlayImageCache cache(fakes.Upload(), {}, fakes.Fetch(), Fakes::Decode());
        CHECK(cache.Settle(10ms));
    }
}

// A PNG of `width` x `height` from SDL's encoder: a red-to-blue ramp, alpha from the rows.
std::string MakePng(int width, int height)
{
    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return {};
    for (int y = 0; y < height; ++y)
    {
        auto* row = static_cast<std::uint8_t*>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < width; ++x)
        {
            row[x * 4 + 0] = std::uint8_t(255 - x * 255 / std::max(1, width - 1));
            row[x * 4 + 1] = 40;
            row[x * 4 + 2] = std::uint8_t(x * 255 / std::max(1, width - 1));
            row[x * 4 + 3] = std::uint8_t(y == 0 ? 0 : 255);
        }
    }
    std::string bytes;
    SDL_IOStream* stream = SDL_IOFromDynamicMem();
    if (stream && SDL_SavePNG_IO(surface, stream, false))
    {
        const auto size = SDL_TellIO(stream);
        const void* data =
            SDL_GetPointerProperty(SDL_GetIOProperties(stream), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr);
        if (data && size > 0) bytes.assign(static_cast<const char*>(data), std::size_t(size));
    }
    if (stream) SDL_CloseIO(stream);
    SDL_DestroySurface(surface);
    return bytes;
}

void TestPng()
{
    const std::string png = MakePng(40, 20);
    CHECK(png.size() > 33);
    OverlayImage image;
    CHECK(DecodeOverlayPng(png, image));
    CHECK(image.width == 40 && image.height == 20 && image.rgba.size() == 40u * 20u * 4u);
    CHECK(image.rgba[3] == 0); // the first row: transparent
    const std::uint8_t* left = &image.rgba[(1 * 40 + 0) * 4];
    const std::uint8_t* right = &image.rgba[(1 * 40 + 39) * 4];
    CHECK(left[0] == 255 && left[1] == 40 && left[2] == 0 && left[3] == 255);
    CHECK(right[0] == 0 && right[2] == 255 && right[3] == 255);
    // Larger than allowed by its header: refused before decoding.
    OverlayImage refused;
    CHECK(!DecodeOverlayPng(png, refused, 32) && refused.rgba.empty());
    std::string lying = png;
    lying[16] = 0x00;
    lying[17] = 0x01; // IHDR width 65576
    CHECK(!DecodeOverlayPng(lying, refused));
    CHECK(!DecodeOverlayPng("not a picture at all, but long enough to have a header", refused));
    CHECK(!DecodeOverlayPng(png.substr(0, png.size() / 2), refused));
    CHECK(!DecodeOverlayPng("", refused));

    // The cache's own decoder: a 256 pixel PNG made a 128 pixel texture.
    const std::string big = MakePng(256, 256);
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
    OverlayImageCache cache(
        [&](int w, int h, const void* rgba) {
            width = w;
            height = h;
            pixels.assign(static_cast<const std::uint8_t*>(rgba), static_cast<const std::uint8_t*>(rgba) + w * h * 4);
            return ImTextureID(7);
        },
        {}, [&](const std::string&, std::size_t, const std::shared_ptr<mscharged::platform::HttpCancel>&) { return big; });
    cache.Get(kA);
    CHECK(cache.Settle(2s));
    CHECK(cache.Get(kA) == 7 && width == 128 && height == 128 && pixels.size() == 128u * 128u * 4u);
}

void TestUrls()
{
    CHECK(OverlayImageUrl("https://cdn.discordapp.com/avatars/80351110224678912/8342729096ea3675442027381ff50dfe.png?size=128",
                          128)
          == "https://cdn.discordapp.com/avatars/80351110224678912/8342729096ea3675442027381ff50dfe.png?size=128");
    CHECK(OverlayImageUrl("https://cdn.discordapp.com/avatars/1/a_abc.webp?size=1024", 128)
          == "https://cdn.discordapp.com/avatars/1/a_abc.png?size=128");
    CHECK(OverlayImageUrl("https://cdn.discordapp.com/avatars/1/abc", 100)
          == "https://cdn.discordapp.com/avatars/1/abc.png?size=128");
    CHECK(OverlayImageUrl("https://cdn.discordapp.com/avatars/1/abc.gif", 64)
          == "https://cdn.discordapp.com/avatars/1/abc.png?size=64");
    // Discord's default avatars and other addresses as they are.
    CHECK(OverlayImageUrl("https://cdn.discordapp.com/embed/avatars/3.png", 128)
          == "https://cdn.discordapp.com/embed/avatars/3.png");
    CHECK(OverlayImageUrl("http://192.168.1.5/avatar.png?x=1", 128) == "http://192.168.1.5/avatar.png?x=1");
}

void TestShrink()
{
    // Half the size: each pixel the average of four, by alpha (a transparent
    // one adds no colour).
    OverlayImage image;
    image.width = 4;
    image.height = 2;
    image.rgba = {255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255,
                  255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255};
    ShrinkOverlayImage(image, 2);
    CHECK(image.width == 2 && image.height == 1 && image.rgba.size() == 8);
    CHECK(image.rgba[0] == 255 && image.rgba[1] == 0 && image.rgba[2] == 0 && image.rgba[3] == 128);
    CHECK(image.rgba[4] == 0 && image.rgba[5] == 255 && image.rgba[6] == 0 && image.rgba[7] == 255);
    // Fits already: unchanged.
    OverlayImage small;
    small.width = small.height = 2;
    small.rgba.assign(16, 9);
    ShrinkOverlayImage(small, 128);
    CHECK(small.width == 2 && small.rgba.size() == 16 && small.rgba[0] == 9);
    // Its shape kept.
    OverlayImage wide;
    wide.width = 1000;
    wide.height = 10;
    wide.rgba.assign(std::size_t(1000 * 10 * 4), 255);
    ShrinkOverlayImage(wide, 128);
    CHECK(wide.width == 128 && wide.height == 1 && wide.rgba[0] == 255 && wide.rgba[3] == 255);
}
} // namespace

int main()
{
    TestCache();
    TestOtherKeys();
    TestFailures();
    TestCaps();
    TestShutdown();
    TestPng();
    TestUrls();
    TestShrink();
    if (failures)
    {
        std::fprintf(stderr, "%d overlay picture checks failed\n", failures);
        return 1;
    }
    std::printf("Overlay picture checks passed\n");
    return 0;
}

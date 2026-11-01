// The launcher's Mii heads (launcher/mii_faces.h): parts read once on the
// worker, heads by face, priorities, heads no longer asked for, the texture
// limits, an image without parts; with MSCHARGED_TEST_DISC the real parts.
#include "launcher/mii_faces.h"
#include "platform/mii_maker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using mscharged::launcher::MiiFaces;
namespace mii = mscharged::platform::mii;
unsigned checks{};
void Check(bool value, const std::string& message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

// Parts with 18 empty archives: enough for MiiParts, drawn by the fake.
std::vector<std::uint8_t> EmptyParts()
{
    std::vector<std::uint8_t> bytes(4 + 18 * 4 + 18 * 8, 0);
    bytes[1] = 18;
    for (int a = 0; a < 18; ++a)
    {
        const std::uint32_t section = 4 + 18 * 4 + 8 * std::uint32_t(a);
        for (int n = 0; n < 4; ++n) bytes[4 + 4 * a + n] = std::uint8_t(section >> (24 - 8 * n));
    }
    return bytes;
}

// What the fake hooks saw: loads, heads drawn in order (by hair type),
// textures made and freed. A gate holds the worker before each head.
struct Fake
{
    std::mutex mutex;
    std::condition_variable changed;
    int loads = 0;
    std::vector<int> drawn;
    bool hold = false;
    int waiting = 0;
    std::map<void*, int> live;
    int made = 0, freed = 0;

    MiiFaces::Hooks Hooks()
    {
        MiiFaces::Hooks hooks;
        hooks.load = [this](const std::filesystem::path& image) {
            std::lock_guard<std::mutex> lock(mutex);
            ++loads;
            if (image == "none") throw std::runtime_error("no parts here");
            return mii::MiiParts(EmptyParts());
        };
        hooks.render = [this](const mii::MiiParts&, const mii::MiiFace& face, const mii::MiiHeadOptions& options) {
            std::unique_lock<std::mutex> lock(mutex);
            ++waiting;
            changed.notify_all();
            changed.wait(lock, [&] { return !hold; });
            --waiting;
            drawn.push_back(face.hair_type);
            mii::RgbaImage image;
            image.width = options.width;
            image.height = options.height;
            image.pixels.assign(std::size_t(options.width) * std::size_t(options.height) * 4,
                                std::uint8_t(face.hair_type));
            return image;
        };
        hooks.make = [this](const mii::RgbaImage& image) {
            void* texture = new int(image.pixels.empty() ? -1 : image.pixels[0]);
            live[texture] = image.width;
            ++made;
            return texture;
        };
        hooks.free = [this](void* texture) {
            Check(live.erase(texture) == 1, "A texture freed twice or never made");
            delete static_cast<int*>(texture);
            ++freed;
        };
        return hooks;
    }
    void Hold(bool on)
    {
        std::lock_guard<std::mutex> lock(mutex);
        hold = on;
        changed.notify_all();
    }
    // Until the worker waits at the gate.
    void AwaitWorker()
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return waiting > 0; });
        Check(waiting > 0, "The worker did not start a head");
    }
    std::vector<int> Drawn()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return drawn;
    }
};

mii::MiiFace Face(int hair)
{
    mii::MiiFace face;
    face.hair_type = hair;
    return face;
}

int Value(void* texture) { return *static_cast<int*>(texture); }

// Frames until `done` (each asking for the heads in `ask`), at most 5 seconds.
template <class Ask, class Done> void Frames(MiiFaces& faces, Ask ask, Done done, const char* what)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end)
    {
        faces.Update();
        ask();
        if (done()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(false, what);
}

void Basics()
{
    Fake fake;
    {
        MiiFaces faces(fake.Hooks());
        // No image: no parts are read, no head comes.
        faces.Update();
        Check(!faces.Head(Face(3)) && faces.Settled() && faces.PartsState() == MiiFaces::Parts::None,
              "A head without an image");
        faces.SetImage("game.rvz");
        faces.Update();
        Check(faces.Settled() && faces.PartsState() == MiiFaces::Parts::None, "Parts read before a head was asked for");

        // The first head reads the parts, then comes as a texture.
        void* head = nullptr;
        Frames(faces, [&] { head = faces.Head(Face(3)); }, [&] { return head != nullptr; },
               "The first head never came");
        Check(Value(head) == 3 && fake.live[head] == 128 && fake.loads == 1, "First head's picture or size");
        faces.Update();
        Check(faces.Settled() && faces.PartsState() == MiiFaces::Parts::Ready, "Not settled after the first head");

        // By face: a record with that face and another name or create ID is the same head.
        const auto record = mscharged::platform::MakeRandomMii("Mario", 5, 1760000000);
        auto other = mscharged::platform::MakeRandomMii("Luigi", 5, 1760000040);
        mii::WriteMiiFace(mii::ReadMiiFace(record), other);
        void* a = nullptr;
        Frames(faces, [&] { a = faces.Head(record); }, [&] { return a != nullptr; }, "A record's head never came");
        Check(faces.Head(other) == a && fake.made == 2, "The same face drawn twice");
        // Another size or background is another head; Cached never asks for one.
        Check(!faces.Cached(Face(3), {64}) && !faces.Head(Face(3), {64}), "Another size came at once");
        void* small = nullptr;
        Frames(faces, [&] { small = faces.Head(Face(3), {64}); }, [&] { return small != nullptr; }, "No 64-pixel head");
        Check(small != head && Value(small) == 3 && fake.live[small] == 64, "The 64-pixel head");
        Check(faces.Cached(Face(3)) == head, "Cached lost a head");

        // A face the Mii library rejects, and an odd record: no head, nothing asked.
        auto bad = Face(3);
        bad.eyebrow_y = 0;
        const auto drawn = fake.Drawn().size();
        Check(!faces.Head(bad) && !faces.Head(std::string_view("short")) && faces.Settled(),
              "A rejected face was asked for");
        for (int n = 0; n < 5; ++n) faces.Update();
        Check(fake.Drawn().size() == drawn, "A rejected face was drawn");
    }
    Check(fake.live.empty() && fake.made == fake.freed, "Textures left after the heads went");
}

// Higher priority first, then the latest asked for; a head nobody asks for
// any more is not drawn.
void Order()
{
    Fake fake;
    MiiFaces faces(fake.Hooks());
    faces.SetImage("game.rvz");
    void* first = nullptr;
    Frames(faces, [&] { first = faces.Head(Face(0)); }, [&] { return first != nullptr; }, "No first head");

    fake.Hold(true);
    faces.Update();
    faces.Head(Face(1)); // the worker takes it and waits at the gate
    fake.AwaitWorker();
    faces.Head(Face(10), {}, 0);
    faces.Head(Face(11), {}, 0);
    faces.Head(Face(20), {}, 2);
    faces.Head(Face(30), {}, 0); // asked for once only
    faces.Update();
    faces.Head(Face(10), {}, 0);
    faces.Head(Face(11), {}, 1);
    faces.Head(Face(20), {}, 2);
    for (int n = 0; n < 3; ++n)
    {
        faces.Update();
        faces.Head(Face(10));
        faces.Head(Face(11), {}, 1);
        faces.Head(Face(20), {}, 2);
    }
    fake.Hold(false);
    Frames(
        faces, [&] { faces.Head(Face(10)), faces.Head(Face(11), {}, 1), faces.Head(Face(20), {}, 2); },
        [&] { return faces.Settled(); }, "The heads asked for were not drawn");
    Check(fake.Drawn() == std::vector<int>({0, 1, 20, 11, 10}), "Heads drawn out of order, or a dropped one drawn");
    Check(faces.Cached(Face(1)) && !faces.Cached(Face(30)), "A head drawn while nobody asked was lost, or kept");
}

// At most the limit stays, the heads shown last kept.
void Limits()
{
    Fake fake;
    MiiFaces faces(fake.Hooks(), {4, 1u << 30});
    faces.SetImage("game.rvz");
    for (int hair = 0; hair < 10; ++hair)
    {
        void* head = nullptr;
        Frames(faces, [&] { head = faces.Head(Face(hair)); }, [&] { return head != nullptr; }, "A head never came");
        Check(faces.TextureCount() <= 5, "More textures than the limit kept");
    }
    faces.Update();
    faces.Update();
    Check(faces.TextureCount() == 4 && fake.live.size() == 4 && faces.Cached(Face(9)) && !faces.Cached(Face(0)),
          "The limit dropped the wrong heads");
    // Heads shown together beyond the limit all stay while they show.
    std::vector<void*> shown(6, nullptr);
    Frames(
        faces, [&] { for (int n = 0; n < 6; ++n) shown[std::size_t(n)] = faces.Head(Face(20 + n)); },
        [&] { for (void* t : shown) if (!t) return false; return true; }, "Six heads shown at once never came");
    Check(faces.TextureCount() >= 6, "A head on screen was dropped");
    // Bytes count too: 128 x 128 x 4 each.
    Fake bytes;
    MiiFaces small(bytes.Hooks(), {100, 3 * 128 * 128 * 4});
    small.SetImage("game.rvz");
    for (int hair = 0; hair < 6; ++hair)
    {
        void* head = nullptr;
        Frames(small, [&] { head = small.Head(Face(hair)); }, [&] { return head != nullptr; }, "A head never came");
    }
    small.Update();
    small.Update();
    Check(small.TextureCount() == 3 && small.TextureBytes() == 3u * 128 * 128 * 4, "The byte limit was not kept");
}

// An image without parts drops the heads (the figure shows); another image's
// parts keep them; no image drops them.
void Images()
{
    Fake fake;
    MiiFaces faces(fake.Hooks());
    faces.SetImage("game.rvz");
    void* head = nullptr;
    Frames(faces, [&] { head = faces.Head(Face(4)); }, [&] { return head != nullptr; }, "No head");
    faces.SetImage("europe.rvz");
    Check(faces.Head(Face(4)) == head, "Another image dropped the heads before its parts were read");
    void* next = nullptr;
    Frames(faces, [&] { next = faces.Head(Face(5)); }, [&] { return next != nullptr; }, "No head from the new image");
    Check(fake.loads == 2 && faces.Cached(Face(4)) == head, "The new image's parts, or the old heads");

    faces.SetImage("none");
    Frames(faces, [&] { faces.Head(Face(6)); }, [&] { return faces.PartsState() == MiiFaces::Parts::Failed; },
           "An image without parts did not fail");
    faces.Update();
    Check(!faces.Error().empty() && faces.TextureCount() == 0 && fake.live.empty() && !faces.Head(Face(4))
              && faces.Settled(),
          "An image without parts kept heads or asked again");
    faces.SetImage("game.rvz");
    Frames(faces, [&] { head = faces.Head(Face(4)); }, [&] { return head != nullptr; }, "No head after a failed image");
    faces.SetImage({});
    Check(faces.TextureCount() == 0 && !faces.Head(Face(4)) && faces.Settled(), "No image kept heads");
}

// The real parts of a game image, drawn as the launcher asks for them.
void RealParts(const char* disc)
{
    std::map<void*, std::vector<std::uint8_t>> made;
    MiiFaces::Hooks hooks;
    hooks.make = [&](const mii::RgbaImage& image) {
        void* texture = new int(0);
        made[texture] = image.pixels;
        return texture;
    };
    hooks.free = [&](void* texture) {
        made.erase(texture);
        delete static_cast<int*>(texture);
    };
    MiiFaces faces(hooks);
    faces.SetImage(disc);
    const auto record = mscharged::platform::MakeRandomMii("Mario", 9, 1760000000, 5);
    void* head = nullptr;
    Frames(faces, [&] { head = faces.Head(record, {64}); }, [&] { return head != nullptr; }, "No real head");
    const auto& pixels = made[head];
    const auto blue = mii::FavoriteColor(5);
    const auto near = [](int a, int b) { return a - b <= 8 && b - a <= 8; };
    Check(pixels.size() == 64 * 64 * 4 && near(pixels[0], blue[0]) && near(pixels[1], blue[1])
              && near(pixels[2], blue[2]) && pixels[3] == 255,
          "A real head's size or favourite-colour background");
    Check(faces.PartsState() == MiiFaces::Parts::Ready, "Real parts");
}
} // namespace

int main()
{
    try
    {
        Basics();
        Order();
        Limits();
        Images();
        if (const char* disc = std::getenv("MSCHARGED_TEST_DISC"); disc && *disc) RealParts(disc);
        std::cout << "mii_faces: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "mii_faces: " << e.what() << '\n';
        return 1;
    }
}

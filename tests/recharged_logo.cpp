// The logo's RECHARGED rewrite (issue #63) on a synthetic scene shaped like
// the game's logo: letter components with looping scale animations in a
// "charged" group, the lightning beside it and the main menu's dark plate.
// The result must be a whole scene for the strict reader, with R and E
// copied from r and e, the word moved right and the lightning extended.
#include "platform/recharged_logo.h"
#include "resources/frontend_scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using mscharged::resources::FrontendScene;

namespace
{
int checks = 0;
void Check(bool good, const std::string& what)
{
    ++checks;
    if (!good) throw std::runtime_error("Check failed: " + what);
}
void Near(float value, float expected, const std::string& what)
{
    Check(std::fabs(value - expected) < 1e-3f, what + " = " + std::to_string(value) + ", expected " + std::to_string(expected));
}

constexpr std::uint32_t kNull = 0xffffffffu;

// A Wii FENL scene being written: big-endian package data and its relocation table.
struct Builder
{
    std::vector<std::uint8_t> data = std::vector<std::uint8_t>(0x18);
    std::vector<std::uint32_t> pointers;

    std::uint32_t Add(std::uint32_t size)
    {
        const std::uint32_t at = std::uint32_t(data.size());
        data.resize(at + size);
        return at;
    }
    void Word(std::uint32_t at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i) data.at(at + i) = std::uint8_t(value >> (24 - 8 * i));
    }
    void Half(std::uint32_t at, std::uint16_t value)
    {
        data.at(at) = std::uint8_t(value >> 8);
        data.at(at + 1) = std::uint8_t(value);
    }
    void Float(std::uint32_t at, float value) { Word(at, std::bit_cast<std::uint32_t>(value)); }
    void Ptr(std::uint32_t at, std::uint32_t to)
    {
        Word(at, to);
        if (std::find(pointers.begin(), pointers.end(), at) == pointers.end()) pointers.push_back(at);
    }
    void Name(std::uint32_t at, const char* name) { std::memcpy(&data.at(at), name, std::strlen(name)); }
    void Ring(const std::vector<std::uint32_t>& nodes, std::uint32_t links = 0)
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            Ptr(nodes[i] + links, nodes[(i + 1) % nodes.size()]);
            Ptr(nodes[i] + links + 4, nodes[(i + nodes.size() - 1) % nodes.size()]);
        }
    }
    // FELibObjectAttributes: position, rotation, scale, pivot, visible, colour, uv.
    void Attributes(std::uint32_t at, float x, float y, float sx = 1, float sy = 1, float px = 0, float py = 0)
    {
        Float(at, x);
        Float(at + 4, y);
        Float(at + 0x18, sx);
        Float(at + 0x1c, sy);
        Float(at + 0x20, 1);
        Float(at + 0x24, px);
        Float(at + 0x28, py);
        data.at(at + 0x30) = 1;
        std::fill_n(&data.at(at + 0x31), 4, 255);
        Float(at + 0x40, 1);
        Float(at + 0x44, 1);
    }
    std::uint32_t Library(std::uint32_t type, const char* name)
    {
        const std::uint32_t at = Add(type == 1 ? 0x7c : type == 3 ? 0x80 : 0x78);
        Attributes(at + 8, 0, 0);
        Name(at + 0x54, name);
        Word(at + 0x74, type);
        libraries.push_back(at);
        return at;
    }
    std::uint32_t Slide(const char* name, float duration, std::uint32_t mode)
    {
        const std::uint32_t at = Add(0x48);
        Float(at + 0x14, duration);
        Word(at + 0x1c, mode);
        Name(at + 0x20, name);
        return at;
    }
    std::uint32_t Instance(std::uint32_t type, std::uint32_t library, const char* name, float duration)
    {
        const std::uint32_t at = Add(type == 2 ? 0x98 : 0x90);
        Ptr(at + 8, kNull);
        Ptr(at + 12, library);
        Float(at + 0x14, duration);
        Name(at + 0x18, name);
        Word(at + 0x88, type);
        data.at(at + 0x8e) = 1;
        return at;
    }
    // A vector animation of `target` with keys (time, x, y, z).
    std::uint32_t Animation(std::uint32_t target, std::uint32_t type, const std::vector<std::array<float, 4>>& keys)
    {
        const std::uint32_t at = Add(0x1c);
        Ptr(at + 12, target);
        Half(at + 16, 1);
        Word(at + 20, type);
        std::vector<std::uint32_t> nodes;
        for (std::size_t n = 0; n < keys.size(); ++n)
        {
            const std::uint32_t key = Add(0x38);
            for (int c = 0; c < 3; ++c)
            {
                const bool last = n + 1 == keys.size();
                const float next = last ? -1.0f : keys[n + 1][1 + c];
                Float(key + 16 * c, keys[n][1 + c]);
                Float(key + 16 * c + 4, last ? -1.0f : (2 * keys[n][1 + c] + next) / 3);
                Float(key + 16 * c + 8, last ? -1.0f : (keys[n][1 + c] + 2 * next) / 3);
                Float(key + 16 * c + 12, keys[n][0]);
            }
            nodes.push_back(key);
        }
        Ring(nodes, 0x30);
        Ptr(at + 24, nodes.back());
        return at;
    }
    std::vector<std::uint8_t> File() const
    {
        std::vector<std::uint8_t> file(16 + data.size() + 4 * pointers.size());
        const auto put = [&file](std::size_t at, std::uint32_t value) {
            for (int i = 0; i < 4; ++i) file[at + i] = std::uint8_t(value >> (24 - 8 * i));
        };
        put(0, 0x46454e4c);
        put(4, 1);
        put(8, std::uint32_t(data.size()));
        put(12, std::uint32_t(4 * pointers.size()));
        std::copy(data.begin(), data.end(), file.begin() + 16);
        for (std::size_t i = 0; i < pointers.size(); ++i) put(16 + data.size() + 4 * i, pointers[i]);
        return file;
    }
    std::vector<std::uint32_t> libraries;
};

// The title's layout: letters (x, y) in the group at (-62, -128).
constexpr float kLetters[7][2] = {{-102, 16}, {-77, 10}, {-51, 24}, {-21, 20}, {16, 31}, {51, 42}, {87, 33}};
constexpr const char* kNames[7] = {"C", "h", "a", "r", "g", "e", "d"};

std::vector<std::uint8_t> LogoScene(bool outline = false)
{
    Builder b;
    const std::uint32_t texture = b.Add(0x20);
    b.Ptr(texture, texture);
    b.Ptr(texture + 4, texture);
    b.Word(texture + 12, 0xd265d834);
    const std::uint32_t image = b.Library(1, "SMC_letter");
    b.Ptr(image + 0x78, texture);
    const std::uint32_t group = b.Library(4, "Group");

    // The letter components: a looping slide with the letter's image and its scale animation.
    std::uint32_t letters[7];
    for (int i = 0; i < 7; ++i)
    {
        const std::uint32_t library = b.Library(3, kNames[i]);
        const std::uint32_t slide = b.Slide("Slide1", i == 5 ? 0.51f : 0.394f, 1);
        b.Ring({slide});
        b.Ptr(library + 0x78, slide);
        b.Ptr(library + 0x7c, slide);
        const std::uint32_t letter = b.Instance(2, image, "SMC_letter", 0.394f);
        b.Ring({letter});
        b.Ptr(letter + 0x90, texture);
        b.Word(letter + 0x94, 1);
        b.Word(letter + 0x84, 0x14);
        b.Attributes(letter + 0x3c, 0, 0, 0.64f, 0.64f);
        b.Ptr(slide + 8, letter);
        const std::uint32_t pulse = b.Animation(letter, 3, {{0, 0.64f, 0.64f, 1}, {0.2f, 0.7f, 0.7f, 1}, {0.394f, 0.6f, 0.6f, 1}});
        b.Ring({pulse}, 4);
        b.Ptr(slide + 12, pulse);
        letters[i] = b.Instance(4, library, kNames[i], 4);
        b.Word(letters[i] + 0x84, 0x1f);
        b.Attributes(letters[i] + 0x3c, kLetters[i][0], kLetters[i][1]);
    }

    // The lightning: an image of the bolts, its pivot off centre, with a jitter.
    const std::uint32_t boltImage = b.Library(1, "SMC_lightning");
    b.Ptr(boltImage + 0x78, texture);
    const std::uint32_t bolts = b.Library(3, "lightning");
    const std::uint32_t boltSlide = b.Slide("Slide1", 0.2f, 1);
    b.Ring({boltSlide});
    b.Ptr(bolts + 0x78, boltSlide);
    b.Ptr(bolts + 0x7c, boltSlide);
    const std::uint32_t bolt = b.Instance(2, boltImage, "SMC_lightning", 0.2f);
    b.Ring({bolt});
    b.Ptr(bolt + 0x90, texture);
    b.Word(bolt + 0x84, 0x1d);
    b.Attributes(bolt + 0x3c, -40, -26, 5.12f, 2.56f, -8, -10);
    b.Ptr(boltSlide + 8, bolt);
    const std::uint32_t jitter = b.Animation(bolt, 3, {{0, 5.12f, 2.56f, 1}, {0.1f, 5.22f, 2.68f, 1}, {0.2f, 5.12f, 2.56f, 1}});
    b.Ring({jitter}, 4);
    b.Ptr(boltSlide + 12, jitter);

    // The plate behind the word (main menu), and the logo component.
    const std::uint32_t plateImage = b.Library(1, outline ? "charged_outline" : "MSC_charged_back");
    b.Ptr(plateImage + 0x78, texture);
    const std::uint32_t logo = b.Library(3, "mainlogo");
    const std::uint32_t slide = b.Slide("UK", 4, 1);
    b.Ring({slide});
    b.Ptr(logo + 0x78, slide);
    b.Ptr(logo + 0x7c, slide);
    const std::uint32_t lightning = b.Instance(4, bolts, "lightning", 4);
    b.Word(lightning + 0x84, 0x1f);
    b.Attributes(lightning + 0x3c, -35, -77);
    const std::uint32_t charged = b.Instance(5, group, "charged", 4);
    b.Word(charged + 0x84, 0x11);
    b.Attributes(charged + 0x3c, -62, -128);
    b.Ring(std::vector<std::uint32_t>(letters, letters + 7));
    b.Ptr(charged + 8, letters[6]);
    const std::uint32_t plate = b.Instance(2, plateImage, outline ? "charged_outline" : "MSC_charged_back", 4);
    b.Ptr(plate + 0x90, texture);
    b.Word(plate + 0x84, 0x15);
    b.Attributes(plate + 0x3c, -31, -130, 2.56f, 0.64f);
    b.Ring({lightning, charged, plate});
    b.Ptr(slide + 8, plate);
    const std::uint32_t bob = b.Animation(lightning, 1, {{0, -35, -77, 0}, {2, -35, -70, 0}, {4, -35, -77, 0}});
    const std::uint32_t grow = b.Animation(plate, 3, {{0, 2.56f, 0.64f, 1}, {0.4f, 5.12f, 1.28f, 1}});
    b.Ring({bob, grow}, 4);
    b.Ptr(slide + 12, grow);

    // The presentation shows the logo.
    const std::uint32_t presentation = b.Add(12);
    const std::uint32_t regular = b.Slide("regular", 2, 0);
    b.Ring({regular});
    const std::uint32_t shown = b.Instance(4, logo, "logo", 2);
    b.Ring({shown});
    b.Ptr(regular + 8, shown);
    b.Ptr(presentation, regular);
    b.Ptr(presentation + 4, regular);
    b.Ptr(4, presentation);
    b.Ptr(8, texture);
    b.Ring(b.libraries);
    b.Ptr(12, b.libraries.back());
    b.Word(16, 0x87654321);
    b.Word(20, 1);
    return b.File();
}

std::span<const std::byte> Bytes(const std::vector<std::uint8_t>& file)
{
    return std::as_bytes(std::span<const std::uint8_t>(file));
}

FrontendScene Read(const mscharged::platform::FrontendPackageVector<std::byte>& file)
{
    return mscharged::resources::ReadFrontendScene(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(file.data()), file.size()));
}

const mscharged::resources::FrontendInstance& InstanceAt(const FrontendScene& scene, std::uint32_t offset)
{
    for (const auto& instance : scene.instances)
        if (instance.offset == offset) return instance;
    throw std::runtime_error("No instance at an offset");
}

const mscharged::resources::FrontendInstance& InstanceNamed(const FrontendScene& scene, std::string_view name)
{
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < scene.instances.size(); ++i)
        if (scene.instances[i].name == name)
        {
            Check(!found, "one instance named " + std::string(name));
            found = i;
        }
    Check(found.has_value(), "an instance named " + std::string(name));
    return scene.instances[*found];
}

const mscharged::resources::FrontendLibraryObject& LibraryAt(const FrontendScene& scene, std::uint32_t offset)
{
    for (const auto& library : scene.library)
        if (library.offset == offset) return library;
    throw std::runtime_error("No library at an offset");
}

const mscharged::resources::FrontendSlide& SlideAt(const FrontendScene& scene, std::uint32_t offset)
{
    for (const auto& slide : scene.slides)
        if (slide.offset == offset) return slide;
    throw std::runtime_error("No slide at an offset");
}

// The animations of a slide that move `target`.
std::vector<const mscharged::resources::FrontendAnimation*> AnimationsOf(
    const FrontendScene& scene, const mscharged::resources::FrontendSlide& slide, std::uint32_t target)
{
    std::vector<const mscharged::resources::FrontendAnimation*> found;
    for (const auto& animation : scene.animations)
        if (animation.target == target
            && std::find(slide.animations.begin(), slide.animations.end(), animation.offset) != slide.animations.end())
            found.push_back(&animation);
    return found;
}

void TestRecharge()
{
    const auto original = LogoScene();
    mscharged::resources::ReadFrontendScene(original); // the fixture itself is a whole scene
    mscharged::platform::FrontendPackageVector<std::byte> out;
    Check(mscharged::platform::RechargeLogo(Bytes(original), out) == 1, "one group recharged");
    const FrontendScene scene = Read(out);

    // R and E in front of C, the word 34.5/31.5 spacings further right, all
    // at 90 % through the group's scale.
    const auto& charged = InstanceNamed(scene, "charged");
    std::vector<std::string> order;
    for (const auto child : charged.children) order.push_back(InstanceAt(scene, child).name);
    Check(order == std::vector<std::string>{"Re_R", "Re_E", "C", "h", "a", "r", "g", "e", "d"}, "letter order");
    const float spacing = (87.0f + 102.0f) / 6.0f, shift = 34.5f / 31.5f * spacing, scale = 0.9f;
    Check((charged.overload_flags & 0x5) == 0x5, "group position and scale");
    Near(charged.attributes.position[0], -62, "group x");
    Near(charged.attributes.position[1], -128, "group y");
    Near(charged.attributes.scale[0], scale, "group x scale");
    Near(charged.attributes.scale[1], scale, "group y scale");
    for (int i = 0; i < 7; ++i)
    {
        const auto& letter = InstanceAt(scene, charged.children[2 + i]);
        Near(letter.attributes.position[0], kLetters[i][0] + shift, std::string("x of ") + kNames[i]);
        Near(letter.attributes.position[1], kLetters[i][1], std::string("y of ") + kNames[i]);
    }
    const auto& r = InstanceNamed(scene, "Re_R");
    const auto& e = InstanceNamed(scene, "Re_E");
    Near(r.attributes.position[0], -102 + shift - 58.0f / 31.5f * spacing, "x of R");
    Near(r.attributes.position[1], 16 - 6.0f / 31.5f * spacing, "y of R");
    Near(e.attributes.position[0], -102 + shift - 26.0f / 31.5f * spacing, "x of E");
    Near(e.attributes.position[1], 16 - 2.0f / 31.5f * spacing, "y of E");

    // Each new letter has its own component (slide time) with the letter's
    // image, animated half a loop ahead of the original.
    for (const auto* added : {&r, &e})
    {
        const auto& source = InstanceNamed(scene, added == &r ? "r" : "e");
        const auto& library = LibraryAt(scene, *added->library);
        Check(library.type == 3 && library.name == added->name && *added->library != *source.library, "own letter component");
        Check(library.slides.size() == 1 && library.active_slide == library.slides[0], "letter slide");
        const auto& slide = SlideAt(scene, library.slides[0]);
        Near(slide.time, 0.5f * slide.duration, "letter phase");
        Check(slide.children.size() == 1, "letter image");
        const auto& image = InstanceAt(scene, slide.children[0]);
        const auto& sourceImage = InstanceAt(scene, SlideAt(scene, LibraryAt(scene, *source.library).slides[0]).children[0]);
        Check(image.offset != sourceImage.offset && image.resource == sourceImage.resource
                && image.library == sourceImage.library, "letter image shares its texture");
        const auto pulses = AnimationsOf(scene, slide, image.offset);
        Check(pulses.size() == 1 && pulses[0]->type == 3 && pulses[0]->keys.size() == 3, "letter pulse");
    }

    // Where a point of the group's word lands in the slide.
    const auto wordX = [&](float x) { return -62 + scale * x; };
    const auto wordY = [&](float y) { return -128 + scale * y; };

    // The lightning follows the letters, at their scale; its copy shows the hook at R.
    const auto& logoSlide = *std::find_if(scene.slides.begin(), scene.slides.end(), [](const auto& s) { return s.name == "UK"; });
    std::vector<std::string> siblings;
    for (const auto child : logoSlide.children) siblings.push_back(InstanceAt(scene, child).name);
    Check(siblings == std::vector<std::string>{"lightning", "Re_lightning", "charged", "MSC_charged_back"}, "slide order");
    const auto& lightning = InstanceNamed(scene, "lightning");
    const auto& hook = InstanceNamed(scene, "Re_lightning");
    const float hookMove = scale * ((-58.0f / 31.5f * spacing - 18) - (-16));
    Near(lightning.attributes.position[0], wordX(-35 + 62 + shift), "lightning x");
    Near(lightning.attributes.position[1], wordY(-77 + 128), "lightning y");
    Near(hook.attributes.position[0], wordX(-35 + 62 + shift) + hookMove, "hook x");
    for (const auto* bolts : {&lightning, &hook})
    {
        Near(bolts->attributes.scale[0], scale, "bolts x scale");
        Near(bolts->attributes.scale[1], scale, "bolts y scale");
        const auto bobs = AnimationsOf(scene, logoSlide, bolts->offset);
        Check(bobs.size() == 1 && bobs[0]->keys.size() == 3, "bolts bob");
        const float x = bolts->attributes.position[0], y = bolts->attributes.position[1];
        for (std::size_t n = 0; n < 3; ++n) Near(bobs[0]->keys[n].channels[0][0], x, "bob key x");
        Near(bobs[0]->keys[0].channels[0][1], x, "bob control x");
        // The bob it shares with the letters keeps its size.
        Near(bobs[0]->keys[0].channels[1][0], y, "bob rest y");
        Near(bobs[0]->keys[1].channels[1][0], y + 7, "bob top y");
        Near(bobs[0]->keys[2].channels[0][1], -1, "last key control");
    }
    Check(*hook.library != *lightning.library && LibraryAt(scene, *hook.library).name == "Re_lightning", "hook component");
    const float keep = 122.0f / 512.0f;
    for (const auto* bolts : {&lightning, &hook})
    {
        const bool isHook = bolts == &hook;
        const float from = isHook ? 0 : keep, width = isHook ? keep : 1 - keep;
        const auto& slide = SlideAt(scene, LibraryAt(scene, *bolts->library).slides[0]);
        const auto& image = std::find_if(slide.children.begin(), slide.children.end(), [&](auto c) {
            return InstanceAt(scene, c).type == 2;
        });
        const auto& bolt = InstanceAt(scene, *image);
        Check((bolt.overload_flags & 0x14c) == 0x14c, "crop flags");
        Near(bolt.attributes.uv[0], from, "crop start");
        Near(bolt.attributes.uv[2], width, "crop width");
        Near(bolt.attributes.scale[0], 5.12f * width, "crop scale");
        // The texture column under the pivot keeps its place.
        Near(bolt.attributes.pivot[0], ((42.0f / 100.0f) - from) / width * 100 - 50, "crop pivot");
        const auto jitters = AnimationsOf(scene, slide, bolt.offset);
        Check(jitters.size() == 1, "bolts jitter");
        Near(jitters[0]->keys[1].channels[0][0], 5.22f * width, "jitter x");
        Near(jitters[0]->keys[1].channels[1][0], 2.68f, "jitter y");
    }
    // Texel for texel: a column of the texture lands where it did relative
    // to the word, at the word's scale (the hook moved on to R).
    const auto column = [&](const mscharged::resources::FrontendInstance& bolts, float u) {
        const auto& slide = SlideAt(scene, LibraryAt(scene, *bolts.library).slides[0]);
        const auto& bolt = InstanceAt(scene, slide.children[0]);
        const float corner = (u - bolt.attributes.uv[0]) / bolt.attributes.uv[2] * 100 - 50;
        return bolts.attributes.position[0]
            + bolts.attributes.scale[0] * (bolt.attributes.position[0] + (corner - bolt.attributes.pivot[0]) * bolt.attributes.scale[0]);
    };
    const auto original_column = [](float u) { return -35 + -40 + (u * 100 - 50 + 8) * 5.12f; };
    for (const float u : {0.3f, 0.5f, 0.9f})
        Near(column(lightning, u), wordX(original_column(u) + 62 + shift), "bolt column");
    for (const float u : {0.0f, 0.1f, 0.2f})
        Near(column(hook, u), wordX(original_column(u) + 62 + shift) + hookMove, "hook column");

    // The plate covers the new word: from R's left edge to D's right edge.
    const auto& plate = InstanceNamed(scene, "MSC_charged_back");
    const float oldLeft = -102 - 16, oldRight = 87 + 27;
    const float newLeft = -102 + shift - 58.0f / 31.5f * spacing - 18, newRight = oldRight + shift;
    const float stretch = scale * (newRight - newLeft) / (oldRight - oldLeft);
    Near(plate.attributes.position[0], wordX(newLeft) + stretch * (-31 - (-62 + oldLeft)), "plate x");
    Near(plate.attributes.position[1], wordY(-130 + 128), "plate y");
    Near(plate.attributes.scale[0], 2.56f * stretch, "plate width");
    Near(plate.attributes.scale[1], 0.64f * scale, "plate height");
    const auto grows = AnimationsOf(scene, logoSlide, plate.offset);
    Check(grows.size() == 1, "plate animation");
    Near(grows[0]->keys[1].channels[0][0], 5.12f * stretch, "plate grown width");
    Near(grows[0]->keys[1].channels[1][0], 1.28f * scale, "plate grown height");

    // Every new component is in the library.
    for (const char* name : {"Re_R", "Re_E", "Re_lightning"})
        Check(std::count_if(scene.library.begin(), scene.library.end(), [&](const auto& l) { return l.name == name; }) == 1,
            std::string("library has ") + name);

    // Once is enough: the rewritten scene has no "charged" group of seven letters.
    std::vector<std::uint8_t> again(reinterpret_cast<const std::uint8_t*>(out.data()),
        reinterpret_cast<const std::uint8_t*>(out.data()) + out.size());
    mscharged::platform::FrontendPackageVector<std::byte> twice;
    Check(mscharged::platform::RechargeLogo(Bytes(again), twice) == 0 && twice.empty(), "already recharged");
}

void TestUnchanged()
{
    mscharged::platform::FrontendPackageVector<std::byte> out;
    // The online start screen's design draws an outline under the letters.
    Check(mscharged::platform::RechargeLogo(Bytes(LogoScene(true)), out) == 0 && out.empty(), "outline design kept");
    // Truncated or broken scenes are left to the game.
    auto file = LogoScene();
    for (const std::size_t size : {std::size_t(0), std::size_t(15), file.size() / 2, file.size() - 4})
    {
        const std::vector<std::uint8_t> part(file.begin(), file.begin() + size);
        Check(mscharged::platform::RechargeLogo(Bytes(part), out) == 0 && out.empty(), "truncated scene");
    }
    auto bad = file;
    const std::size_t table = 16 + ((std::size_t(file[8]) << 24) | (file[9] << 16) | (file[10] << 8) | file[11]);
    bad[table + 3] |= 2; // a relocation off its word
    Check(mscharged::platform::RechargeLogo(Bytes(bad), out) == 0 && out.empty(), "broken relocation table");
    // Damaged bytes never take the rewrite outside the scene.
    std::mt19937 random(63);
    int rewritten = 0;
    for (int n = 0; n < 4000; ++n)
    {
        auto damaged = file;
        for (int flips = 1 + int(random() % 3); flips > 0; --flips)
            damaged[random() % damaged.size()] ^= std::uint8_t(1u << (random() % 8));
        if (mscharged::platform::RechargeLogo(Bytes(damaged), out) == 0) continue;
        ++rewritten;
        // A scene the strict reader takes stays one it takes.
        bool whole = true;
        try
        {
            mscharged::resources::ReadFrontendScene(damaged);
        }
        catch (const std::exception&)
        {
            whole = false;
        }
        if (whole) Read(out);
    }
    Check(rewritten > 0, "damaged scenes still rewritten at times");
}
} // namespace

int main()
{
    try
    {
        TestRecharge();
        TestUnchanged();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("recharged logo: %d checks passed\n", checks);
    return 0;
}

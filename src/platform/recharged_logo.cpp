#include "platform/recharged_logo.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
template<class T> using HostVector = FrontendPackageVector<T>;

// The Wii FENL layout (big-endian; fePackage.h, feLibObject.h, tlInstance.h,
// tlSlide.h, feAnimation.h). Pointers are offsets into the package data,
// listed in the relocation table; rings link their nodes both ways and are
// reached through their last node.
constexpr std::uint32_t kNull = 0xffffffffu;
constexpr std::size_t kMaximumScene = 16 * 1024 * 1024;
constexpr std::size_t kMaximumRing = 16384;
constexpr std::uint32_t kPackageLibraries = 12;

constexpr std::uint32_t kLibraryAttributes = 0x08, kLibraryHash = 0x50, kLibraryName = 0x54, kLibraryType = 0x74,
                        kLibrarySlides = 0x78;
constexpr std::uint32_t kComponentLibrary = 3;

constexpr std::uint32_t kInstanceChildren = 0x08, kInstanceLibrary = 0x0c, kInstanceName = 0x18,
                        kInstanceHash = 0x38, kInstanceAttributes = 0x3c, kInstanceFlags = 0x84,
                        kInstanceType = 0x88;
constexpr std::uint32_t kImageInstance = 2, kTextInstance = 3, kComponentInstance = 4, kGroupInstance = 5;

constexpr std::uint32_t kSlideSize = 0x48, kSlideChildren = 0x08, kSlideAnimations = 0x0c, kSlideStart = 0x10,
                        kSlideDuration = 0x14, kSlideTime = 0x18;

constexpr std::uint32_t kAnimationSize = 0x1c, kAnimationLinks = 0x04, kAnimationTarget = 0x0c,
                        kAnimationCast = 0x10, kAnimationType = 0x14, kAnimationKeys = 0x18;
constexpr std::uint32_t kVectorCast = 1, kPositionAnimation = 1, kScaleAnimation = 3;
// Keys: point, two control points and time, per channel (x first).
constexpr std::uint32_t kFloatKeySize = 0x18, kFloatKeyLinks = 0x10, kVectorKeySize = 0x38, kVectorKeyLinks = 0x30;

// FELibObjectAttributes, and the overload flags that select an instance's own.
constexpr std::uint32_t kPositionX = 0x00, kPositionY = 0x04, kScaleX = 0x18, kPivotX = 0x24, kUvX = 0x38,
                        kUvWidth = 0x40;
constexpr std::uint32_t kOverloadPosition = 0x1, kOverloadScale = 0x4, kOverloadPivot = 0x8, kOverloadUvX = 0x40,
                        kOverloadUvWidth = 0x100;

std::uint32_t InstanceSize(std::uint32_t type)
{
    return type == kImageInstance ? 0x98 : type == kTextInstance ? 0x114 : 0x90;
}

std::uint32_t LibrarySize(std::uint32_t type)
{
    switch (type)
    {
    case 1: return 0x7c; // image
    case 2: return 0x88; // text
    case 3: return 0x80; // component
    default: return 0x78; // layer, group
    }
}

// The layout of RECHARGED, measured on the title screen's letters (64x64
// texels at one unit per texel, 31.5 units apart on average) and applied in
// units of each group's own spacing. R and E keep the word's slope (its
// letters climb about 0.12 units per unit to the right) and its rhythm: R is
// as far from E as the letters of CHARGED are apart on average, and E
// overlaps C as C overlaps h.
constexpr float kTitleSpacing = 31.5f;
constexpr float kRx = -58.0f / kTitleSpacing, kRy = -6.0f / kTitleSpacing;
constexpr float kEx = -26.0f / kTitleSpacing, kEy = -2.0f / kTitleSpacing;
// The logo's black backdrop narrows towards the bottom of the word on the
// left, and the tail of STRIKERS' S comes down over its right end. Nine
// letters fit between them, all on the black, at 90 % of their size (letters,
// gaps and the lightning around them alike), the word moved right by this
// much (before that scale).
constexpr float kShift = 34.5f / kTitleSpacing;
constexpr float kWordScale = 0.9f;
// How far the letters' glyphs reach beyond their centres (texels).
constexpr float kCLeft = -16.0f, kRLeft = -18.0f, kDRight = 27.0f;
// R and E start half a loop into their scale animations, so they do not
// pulse in step with r and e.
constexpr float kLetterPhase = 0.5f;
// The lightning texture's columns: its hook in front of C ends before this
// fraction of the width; then come the bolts around the word.
constexpr float kBoltHook = 122.0f / 512.0f;
constexpr const char* kLetters[7] = {"c", "h", "a", "r", "g", "e", "d"};

std::uint32_t ReadWord(std::span<const std::byte> bytes, std::size_t at)
{
    if (at + 4 > bytes.size()) throw std::runtime_error("FEN read outside the scene");
    return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at + 1]) << 16) | (std::uint32_t(bytes[at + 2]) << 8)
        | std::uint32_t(bytes[at + 3]);
}

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t LowerHash(const char* text)
{
    std::uint32_t hash = 0xffffffffu;
    for (; *text; ++text) hash = hash + (hash << 5) + std::uint32_t(std::tolower(static_cast<unsigned char>(*text)));
    return hash;
}

// A scene's package data and relocation table, which records can be added to.
class Scene
{
public:
    explicit Scene(std::span<const std::byte> file)
    {
        Require(file.size() >= 16 && file.size() <= kMaximumScene, "FEN scene size");
        Require(ReadWord(file, 0) == 0x46454e4c && ReadWord(file, 4) == 1, "Not a FENL version 1 scene");
        const std::size_t size = ReadWord(file, 8), table = ReadWord(file, 12);
        Require(size % 4 == 0 && table % 4 == 0 && size + table == file.size() - 16, "FEN scene lengths");
        data_.assign(file.begin() + 16, file.begin() + 16 + size);
        relocated_.assign((size / 4 + 63) / 64, 0);
        for (std::size_t i = 0; i < table; i += 4)
        {
            const std::uint32_t field = ReadWord(file, 16 + size + i);
            Require(field % 4 == 0 && field + 4 <= size && !Relocated(field), "FEN relocation table");
            Mark(field);
            relocations_.push_back(field);
        }
    }

    std::uint32_t Size() const { return std::uint32_t(data_.size()); }
    std::uint32_t Word(std::uint32_t at) const { return ReadWord(data_, at); }
    std::uint16_t Half(std::uint32_t at) const { return std::uint16_t(Word(at & ~3u) >> ((at & 2) ? 0 : 16)); }
    float Float(std::uint32_t at) const
    {
        const float value = std::bit_cast<float>(Word(at));
        Require(std::isfinite(value), "FEN value is not finite");
        return value;
    }
    void SetWord(std::uint32_t at, std::uint32_t value)
    {
        Require(std::size_t(at) + 4 <= data_.size(), "FEN write outside the scene");
        for (int i = 0; i < 4; ++i) data_[at + i] = std::byte(value >> (24 - 8 * i));
    }
    void SetFloat(std::uint32_t at, float value) { SetWord(at, std::bit_cast<std::uint32_t>(value)); }

    bool Relocated(std::uint32_t at) const
    {
        return at % 4 == 0 && at / 256 < relocated_.size() && (relocated_[at / 256] >> (at / 4 % 64) & 1);
    }
    // A pointer field's target, kNull for none.
    std::uint32_t Pointer(std::uint32_t at) const
    {
        if (!Relocated(at)) return kNull;
        const std::uint32_t target = Word(at);
        Require(target == kNull || (target % 4 == 0 && target < data_.size()), "FEN pointer target");
        return target;
    }
    void SetPointer(std::uint32_t at, std::uint32_t target)
    {
        if (!Relocated(at))
        {
            Mark(at);
            relocations_.push_back(at);
        }
        SetWord(at, target);
    }

    // The nodes of a ring, from the first to `tail`; `links` is the offset of
    // the nodes' next/previous pointers.
    HostVector<std::uint32_t> Ring(std::uint32_t tail, std::uint32_t links = 0) const
    {
        HostVector<std::uint32_t> nodes;
        if (tail == kNull) return nodes;
        for (std::uint32_t node = Pointer(tail + links);; node = Pointer(node + links))
        {
            Require(node != kNull && nodes.size() < kMaximumRing, "FEN ring");
            nodes.push_back(node);
            if (node == tail) return nodes;
        }
    }
    // Links `node` into a ring after `after`.
    void InsertAfter(std::uint32_t after, std::uint32_t node, std::uint32_t links = 0)
    {
        const std::uint32_t next = Pointer(after + links);
        Require(next != kNull, "FEN ring");
        SetPointer(node + links, next);
        SetPointer(node + links + 4, after);
        SetPointer(after + links, node);
        SetPointer(next + links + 4, node);
    }

    // A record's 32-byte name, compared without case.
    bool NameIs(std::uint32_t at, const char* name) const
    {
        const std::size_t length = std::strlen(name);
        Require(std::size_t(at) + 32 <= data_.size(), "FEN name");
        for (std::size_t i = 0; i < 32; ++i)
        {
            const char c = char(data_[at + i]);
            if (i == length) return c == 0;
            if (std::tolower(static_cast<unsigned char>(c)) != std::tolower(static_cast<unsigned char>(name[i])))
                return false;
        }
        return false;
    }
    void SetName(std::uint32_t at, const char* name)
    {
        Require(std::size_t(at) + 32 <= data_.size() && std::strlen(name) < 32, "FEN name");
        std::memset(&data_[at], 0, 32);
        std::memcpy(&data_[at], name, std::strlen(name));
    }

    // A copy of a record at the end of the package data.
    std::uint32_t Append(std::uint32_t from, std::uint32_t size)
    {
        Require(std::size_t(from) + size <= data_.size() && size % 4 == 0, "FEN record");
        Require(data_.size() + size <= kMaximumScene, "FEN scene size");
        const std::uint32_t at = Size();
        data_.resize(data_.size() + size);
        std::memcpy(&data_[at], &data_[from], size);
        relocated_.resize((data_.size() / 4 + 63) / 64, 0);
        return at;
    }

    void Write(HostVector<std::byte>& out) const
    {
        out.resize(16 + data_.size() + 4 * relocations_.size());
        const auto put = [&out](std::size_t at, std::uint32_t value) {
            for (int i = 0; i < 4; ++i) out[at + i] = std::byte(value >> (24 - 8 * i));
        };
        put(0, 0x46454e4c);
        put(4, 1);
        put(8, Size());
        put(12, std::uint32_t(4 * relocations_.size()));
        std::memcpy(&out[16], data_.data(), data_.size());
        for (std::size_t i = 0; i < relocations_.size(); ++i) put(16 + data_.size() + 4 * i, relocations_[i]);
    }

private:
    void Mark(std::uint32_t at) { relocated_[at / 256] |= std::uint64_t(1) << (at / 4 % 64); }

    HostVector<std::byte> data_;
    HostVector<std::uint32_t> relocations_; // in table order
    HostVector<std::uint64_t> relocated_;   // one bit per data word
};

// Deep copies of records: component libraries with their slides, animations,
// keys and instances (nested components get their own library, as a
// component's library holds its slide's time). Pointers between copied
// records point at the copies; others (image libraries, textures, strings)
// are shared. Ring links to records outside the copy are set by the caller.
class Cloner
{
public:
    explicit Cloner(Scene& scene) : scene_(scene) {}

    std::uint32_t Library(std::uint32_t library, int depth = 0)
    {
        Require(depth < 8, "FEN component nesting");
        const std::uint32_t copy = Copy(library, LibrarySize(scene_.Word(library + kLibraryType)));
        for (const std::uint32_t slide : scene_.Ring(scene_.Pointer(library + kLibrarySlides)))
        {
            Copy(slide, kSlideSize);
            for (const std::uint32_t animation : scene_.Ring(scene_.Pointer(slide + kSlideAnimations), kAnimationLinks))
                Animation(animation);
            for (const std::uint32_t instance : scene_.Ring(scene_.Pointer(slide + kSlideChildren)))
                Instance(instance, depth + 1);
        }
        return copy;
    }

    std::uint32_t Instance(std::uint32_t instance, int depth = 0)
    {
        Require(depth < 16, "FEN instance nesting");
        const std::uint32_t type = scene_.Word(instance + kInstanceType);
        const std::uint32_t copy = Copy(instance, InstanceSize(type));
        const std::uint32_t library = scene_.Pointer(instance + kInstanceLibrary);
        if (type == kComponentInstance && library != kNull && Find(library) == kNull) Library(library, depth);
        for (const std::uint32_t child : scene_.Ring(scene_.Pointer(instance + kInstanceChildren)))
            Instance(child, depth + 1);
        return copy;
    }

    std::uint32_t Animation(std::uint32_t animation)
    {
        const std::uint32_t copy = Copy(animation, kAnimationSize);
        const bool vector = scene_.Half(animation + kAnimationCast) == kVectorCast;
        for (const std::uint32_t key :
             scene_.Ring(scene_.Pointer(animation + kAnimationKeys), vector ? kVectorKeyLinks : kFloatKeyLinks))
            Copy(key, vector ? kVectorKeySize : kFloatKeySize);
        return copy;
    }

    // The copy of a record, kNull for none.
    std::uint32_t Find(std::uint32_t record) const
    {
        for (const auto& copied : copies_)
            if (copied.from == record) return copied.to;
        return kNull;
    }

    // Points the copies' pointer fields at the copies (or the shared records).
    void Finish()
    {
        for (const auto& copied : copies_)
            for (std::uint32_t field = 0; field < copied.size; field += 4)
                if (scene_.Relocated(copied.from + field))
                {
                    const std::uint32_t target = scene_.Pointer(copied.from + field);
                    const std::uint32_t copy = target == kNull ? kNull : Find(target);
                    scene_.SetPointer(copied.to + field, copy == kNull ? target : copy);
                }
    }

private:
    struct Copied
    {
        std::uint32_t from, to, size;
    };

    std::uint32_t Copy(std::uint32_t record, std::uint32_t size)
    {
        Require(Find(record) == kNull && copies_.size() < 4096, "FEN copy");
        const std::uint32_t copy = scene_.Append(record, size);
        copies_.push_back({record, copy, size});
        return copy;
    }

    Scene& scene_;
    HostVector<Copied> copies_;
};

HostVector<std::uint32_t> AnimationsOf(const Scene& scene, std::uint32_t slide, std::uint32_t instance)
{
    HostVector<std::uint32_t> found;
    for (const std::uint32_t animation : scene.Ring(scene.Pointer(slide + kSlideAnimations), kAnimationLinks))
        if (scene.Pointer(animation + kAnimationTarget) == instance) found.push_back(animation);
    return found;
}

// Applies `change` to one channel (0 x, 1 y) of a vector animation's key
// points and their control points (the last key's controls are unused).
template<class Change> void ChangeKeys(Scene& scene, std::uint32_t animation, std::uint32_t axis, Change change)
{
    const auto keys = scene.Ring(scene.Pointer(animation + kAnimationKeys), kVectorKeyLinks);
    for (std::size_t n = 0; n < keys.size(); ++n)
        for (std::uint32_t part = 0; part < (n + 1 < keys.size() ? 12u : 4u); part += 4)
        {
            const std::uint32_t at = keys[n] + 16 * axis + part;
            scene.SetFloat(at, change(scene.Float(at)));
        }
}

// An instance's own position or scale (`offset` in its attributes), which it
// then draws with: its library's until now, when it had none.
std::uint32_t Own(Scene& scene, std::uint32_t instance, std::uint32_t flag, std::uint32_t offset)
{
    const std::uint32_t flags = scene.Word(instance + kInstanceFlags);
    const std::uint32_t attribute = instance + kInstanceAttributes + offset;
    if (!(flags & flag))
    {
        const std::uint32_t library = scene.Pointer(instance + kInstanceLibrary);
        Require(library != kNull, "FEN instance library");
        for (std::uint32_t i = 0; i < 12; i += 4)
            scene.SetFloat(attribute + i, scene.Float(library + kLibraryAttributes + offset + i));
        scene.SetWord(instance + kInstanceFlags, flags | flag);
    }
    return attribute;
}

// Moves an instance of a slide along an axis (0 x, 1 y): position p becomes
// change(p), in its slide's position animations too.
template<class Change>
void Move(Scene& scene, std::uint32_t slide, std::uint32_t instance, std::uint32_t axis, Change change)
{
    const std::uint32_t position = Own(scene, instance, kOverloadPosition, kPositionX) + 4 * axis;
    scene.SetFloat(position, change(scene.Float(position)));
    for (const std::uint32_t animation : AnimationsOf(scene, slide, instance))
        if (scene.Half(animation + kAnimationCast) == kVectorCast
            && scene.Word(animation + kAnimationType) == kPositionAnimation)
            ChangeKeys(scene, animation, axis, change);
}

// Scales an instance of a slide along an axis, in its slide's scale
// animations too.
void Scale(Scene& scene, std::uint32_t slide, std::uint32_t instance, std::uint32_t axis, float factor)
{
    const std::uint32_t scale = Own(scene, instance, kOverloadScale, kScaleX) + 4 * axis;
    scene.SetFloat(scale, scene.Float(scale) * factor);
    for (const std::uint32_t animation : AnimationsOf(scene, slide, instance))
        if (scene.Half(animation + kAnimationCast) == kVectorCast
            && scene.Word(animation + kAnimationType) == kScaleAnimation)
            ChangeKeys(scene, animation, axis, [factor](float value) { return value * factor; });
}

// A copy of an instance of a slide, with its animations there and (for a
// component) its own library, linked in after the original. Returns the copy.
std::uint32_t CopySibling(Scene& scene, std::uint32_t slide, std::uint32_t instance, const char* name)
{
    Cloner clone(scene);
    const std::uint32_t copy = clone.Instance(instance);
    const auto animations = AnimationsOf(scene, slide, instance);
    for (const std::uint32_t animation : animations) clone.Animation(animation);
    clone.Finish();
    scene.SetName(copy + kInstanceName, name);
    scene.SetWord(copy + kInstanceHash, LowerHash(name));
    const std::uint32_t library = scene.Pointer(instance + kInstanceLibrary);
    if (const std::uint32_t libraryCopy = clone.Find(library); libraryCopy != kNull)
    {
        scene.SetName(libraryCopy + kLibraryName, name);
        scene.SetWord(libraryCopy + kLibraryHash, LowerHash(name));
        scene.InsertAfter(library, libraryCopy);
    }
    scene.InsertAfter(instance, copy);
    for (const std::uint32_t animation : animations)
        scene.InsertAfter(animation, clone.Find(animation), kAnimationLinks);
    return copy;
}

// Shows only the texture columns [from, to) (fractions of the width) of a
// component's images, texel for texel where they were: the image narrows to
// those columns, and its pivot, the centre of its scale animation, keeps its
// texture column.
void CropImages(Scene& scene, std::uint32_t library, float from, float to)
{
    const float width = to - from;
    for (const std::uint32_t slide : scene.Ring(scene.Pointer(library + kLibrarySlides)))
        for (const std::uint32_t image : scene.Ring(scene.Pointer(slide + kSlideChildren)))
        {
            if (scene.Word(image + kInstanceType) != kImageInstance) continue;
            const std::uint32_t flags = scene.Word(image + kInstanceFlags);
            const std::uint32_t attributes = image + kInstanceAttributes;
            const std::uint32_t own = scene.Pointer(image + kInstanceLibrary);
            Require(own != kNull, "FEN image library");
            const std::uint32_t shared = own + kLibraryAttributes;
            const float pivot = scene.Float((flags & kOverloadPivot ? attributes : shared) + kPivotX);
            const float scale = scene.Float((flags & kOverloadScale ? attributes : shared) + kScaleX);
            const float column = (pivot + 50.0f) / 100.0f;
            scene.SetFloat(attributes + kPivotX, (column - from) / width * 100.0f - 50.0f);
            scene.SetFloat(attributes + kScaleX, scale * width);
            scene.SetFloat(attributes + kUvX, from);
            scene.SetFloat(attributes + kUvWidth, width);
            scene.SetWord(image + kInstanceFlags,
                flags | kOverloadScale | kOverloadPivot | kOverloadUvX | kOverloadUvWidth);
            for (const std::uint32_t animation : AnimationsOf(scene, slide, image))
                if (scene.Half(animation + kAnimationCast) == kVectorCast
                    && scene.Word(animation + kAnimationType) == kScaleAnimation)
                    ChangeKeys(scene, animation, 0, [width](float x) { return x * width; });
        }
}

// R or E: a copy of the group's r or e at (x, y), linked into the libraries
// after the letter's own. Returns the new letter instance.
std::uint32_t AddLetter(Scene& scene, std::uint32_t letter, const char* name, float x, float y)
{
    Cloner clone(scene);
    const std::uint32_t copy = clone.Instance(letter);
    clone.Finish();
    const std::uint32_t library = scene.Pointer(letter + kInstanceLibrary);
    const std::uint32_t libraryCopy = clone.Find(library);
    Require(libraryCopy != kNull, "FEN letter library");
    scene.SetName(copy + kInstanceName, name);
    scene.SetWord(copy + kInstanceHash, LowerHash(name));
    scene.SetName(libraryCopy + kLibraryName, name);
    scene.SetWord(libraryCopy + kLibraryHash, LowerHash(name));
    for (const std::uint32_t attributes : {copy + kInstanceAttributes, libraryCopy + kLibraryAttributes})
    {
        scene.SetFloat(attributes + kPositionX, x);
        scene.SetFloat(attributes + kPositionY, y);
    }
    for (const std::uint32_t slide : scene.Ring(scene.Pointer(libraryCopy + kLibrarySlides)))
        scene.SetFloat(slide + kSlideTime,
            scene.Float(slide + kSlideStart) + kLetterPhase * scene.Float(slide + kSlideDuration));
    scene.InsertAfter(library, libraryCopy);
    return copy;
}

// One "charged" group of a slide: whether it became RECHARGED.
bool RechargeGroup(Scene& scene, std::uint32_t slide, std::uint32_t group)
{
    const auto letters = scene.Ring(scene.Pointer(group + kInstanceChildren));
    if (letters.size() != 7) return false;
    for (std::size_t i = 0; i < 7; ++i)
        if (scene.Word(letters[i] + kInstanceType) != kComponentInstance || !scene.NameIs(letters[i] + kInstanceName, kLetters[i])
            || !(scene.Word(letters[i] + kInstanceFlags) & kOverloadPosition)
            || scene.Pointer(letters[i] + kInstanceLibrary) == kNull)
            return false;
    const auto x = [&](std::size_t i) { return scene.Float(letters[i] + kInstanceAttributes + kPositionX); };
    const float cx = x(0), cy = scene.Float(letters[0] + kInstanceAttributes + kPositionY);
    const float spacing = (x(6) - cx) / 6.0f;
    if (!(spacing >= 10.0f && spacing <= 80.0f)) return false;
    // Other designs (the online start screen's) draw their own outline under
    // the letters, which has no R or E.
    const auto siblings = scene.Ring(scene.Pointer(slide + kSlideChildren));
    for (const std::uint32_t sibling : siblings)
        if (scene.NameIs(sibling + kInstanceName, "charged_outline")) return false;

    const float shift = kShift * spacing;
    const std::uint32_t r = AddLetter(scene, letters[3], "Re_R", cx + shift + kRx * spacing, cy + kRy * spacing);
    const std::uint32_t e = AddLetter(scene, letters[5], "Re_E", cx + shift + kEx * spacing, cy + kEy * spacing);
    for (std::size_t i = 0; i < 7; ++i) scene.SetFloat(letters[i] + kInstanceAttributes + kPositionX, x(i) + shift);
    // In front of C: the front end's view draws in reverse order, so they
    // overlap C as C overlaps h.
    scene.InsertAfter(letters[6], e);
    scene.InsertAfter(letters[6], r);
    // The group draws the word smaller, about its own position (which bobs).
    Scale(scene, slide, group, 0, kWordScale);
    Scale(scene, slide, group, 1, kWordScale);

    // What surrounds the word in its slide follows it: its place relative to
    // the group scaled, moved right with the letters. The bob it shares with
    // the group stays as it is.
    const float groupX = scene.Float(Own(scene, group, kOverloadPosition, kPositionX));
    const float groupY = scene.Float(Own(scene, group, kOverloadPosition, kPositionX) + 4);
    const auto follow = [&](std::uint32_t instance, float move) {
        const std::uint32_t position = Own(scene, instance, kOverloadPosition, kPositionX);
        const float px = scene.Float(position), py = scene.Float(position + 4);
        const float dx = groupX + kWordScale * (px - groupX + move) - px, dy = groupY + kWordScale * (py - groupY) - py;
        Move(scene, slide, instance, 0, [dx](float value) { return value + dx; });
        Move(scene, slide, instance, 1, [dy](float value) { return value + dy; });
    };
    // The word in the group: it starts at R instead of C, and ends `shift`
    // further right.
    const float oldLeft = cx + kCLeft, oldRight = x(6) - shift + kDRight;
    const float newLeft = cx + shift + kRx * spacing + kRLeft, newRight = oldRight + shift;
    for (const std::uint32_t sibling : siblings)
    {
        const std::uint32_t type = scene.Word(sibling + kInstanceType);
        if (type == kComponentInstance && scene.NameIs(sibling + kInstanceName, "lightning"))
        {
            // The bolts keep their shape and follow the letters; a copy of
            // their hook in front of C moves on to frame R.
            follow(sibling, shift);
            Scale(scene, slide, sibling, 0, kWordScale);
            Scale(scene, slide, sibling, 1, kWordScale);
            const std::uint32_t hook = CopySibling(scene, slide, sibling, "Re_lightning");
            const float hookMove = kWordScale * (newLeft - (oldLeft + shift));
            Move(scene, slide, hook, 0, [hookMove](float value) { return value + hookMove; });
            CropImages(scene, scene.Pointer(sibling + kInstanceLibrary), kBoltHook, 1.0f);
            CropImages(scene, scene.Pointer(hook + kInstanceLibrary), 0.0f, kBoltHook);
        }
        else if (type == kImageInstance && scene.NameIs(sibling + kInstanceName, "MSC_charged_back"))
        {
            // The main menu's dark plate behind the word: as wide as the new word.
            const float from = groupX + oldLeft, to = groupX + kWordScale * newLeft;
            const float stretch = kWordScale * (newRight - newLeft) / (oldRight - oldLeft);
            const float py = scene.Float(Own(scene, sibling, kOverloadPosition, kPositionX) + 4);
            const float dy = groupY + kWordScale * (py - groupY) - py;
            Move(scene, slide, sibling, 0, [=](float value) { return to + stretch * (value - from); });
            Move(scene, slide, sibling, 1, [dy](float value) { return value + dy; });
            Scale(scene, slide, sibling, 0, stretch);
            Scale(scene, slide, sibling, 1, kWordScale);
        }
    }
    return true;
}
} // namespace

int RechargeLogo(std::span<const std::byte> file, FrontendPackageVector<std::byte>& out)
{
    out.clear();
    try
    {
        Scene scene(file);
        int changed = 0;
        for (const std::uint32_t library : scene.Ring(scene.Pointer(kPackageLibraries)))
        {
            if (scene.Word(library + kLibraryType) != kComponentLibrary) continue;
            for (const std::uint32_t slide : scene.Ring(scene.Pointer(library + kLibrarySlides)))
                for (const std::uint32_t instance : scene.Ring(scene.Pointer(slide + kSlideChildren)))
                    if (scene.Word(instance + kInstanceType) == kGroupInstance
                        && scene.NameIs(instance + kInstanceName, "charged"))
                        changed += RechargeGroup(scene, slide, instance);
        }
        if (changed) scene.Write(out);
        return changed;
    }
    catch (const std::exception&)
    {
        out.clear();
        return 0;
    }
}
}

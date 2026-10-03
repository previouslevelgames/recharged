#include "resources/world_objects.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>

using namespace mscharged::resources;
namespace
{
using Buffer = std::vector<std::uint8_t>;
unsigned checks = 0;
void Check(bool yes) { ++checks; if (!yes) throw std::runtime_error("World record assertion failed"); }
template<class F> void Reject(F f)
{
    ++checks;
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid world record accepted");
}
void Put(Buffer& b, std::size_t p, std::uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) b.at(p + i) = v >> (24 - 8 * i); }
void Float(Buffer& b, std::size_t p, float v) { Put(b, p, std::bit_cast<std::uint32_t>(v)); }
Buffer Drawable(std::uint32_t id, bool stadium = false)
{
    Buffer b(stadium ? 0x90 : 0x70);
    Put(b, 0, 0xfedcba98); Put(b, 4, id); Put(b, 8, stadium ? 0x10002 : 0x101); Put(b, 12, 3);
    Put(b, 0x10, 0xaabbccdd); Put(b, 0x14, 0xffffffff); Put(b, 0x18, 0x12345678);
    for (unsigned i = 0; i < 4; ++i) Float(b, 0x20 + 20 * i, 1);
    Float(b, 0x20, 2); Float(b, 0x34, -3); Float(b, 0x50, 12.5f);
    Float(b, 0x54, -4); Float(b, 0x58, .25f); Float(b, 0x60, 3.25f);
    Put(b, 0x64, 0xfedc0123);
    if (stadium)
    {
        for (unsigned i = 0; i < 3; ++i) { Float(b, 0x70 + i * 4, -10.f + i); Float(b, 0x7c + i * 4, 20.f + i); }
        Float(b, 0x8c, 1);
    }
    return b;
}
Buffer World(std::initializer_list<Buffer> records)
{
    Buffer b(32);
    Put(b, 0, 0x80000001); Put(b, 8, 0x04026000); Put(b, 16, records.size());
    for (const auto& r : records) b.insert(b.end(), r.begin(), r.end());
    Put(b, 4, b.size() - 8); Put(b, 12, b.size() - 16); return b;
}
void Run()
{
    const auto file = World({Drawable(1), Drawable(2, true)});
    const std::uint32_t ids[] = {2, 1};
    const auto before = file;
    const auto index = ReadWorldObjectIndex(file);
    Check(index.size() == 2 && index[0].id == 1 && index[0].offset == 32 && index[0].size == 0x70);
    Check(index[1].type == 0x10002 && index[1].offset == 32 + 0x70 && !index[1].animated);
    const auto decoded = ReadStaticWorldObjects(file, ids);
    Check(decoded.size() == 2 && decoded[0].id == 2 && decoded[1].id == 1 && decoded[0].model == 0xfedc0123);
    Check(decoded[0].transform[0] == 2 && decoded[0].transform[5] == -3 && decoded[0].transform[12] == 12.5f
        && decoded[0].transform[13] == -4 && decoded[0].transform[14] == .25f && decoded[0].radius == 3.25f);
    Check(decoded[0].bounds_min[2] == -8 && decoded[0].bounds_max[1] == 21 && file == before);
    auto input = file; const auto owned = ReadStaticWorldObjects(input, ids); std::fill(input.begin(), input.end(), 0);
    Check(owned[0].model == 0xfedc0123 && owned[0].transform == decoded[0].transform);
    Buffer unaligned(file.size() + 1); std::copy(file.begin(), file.end(), unaligned.begin() + 1);
    Check(ReadStaticWorldObjects(Bytes(unaligned).subspan(1), ids)[1].transform == decoded[1].transform);
    for (std::size_t n = 0; n < file.size(); ++n) Reject([&] { ReadWorldObjectIndex(Bytes(file).first(n)); });
    auto change = [&](std::size_t offset, std::uint32_t value) {
        auto bad = file; Put(bad, offset, value); Reject([&] { ReadStaticWorldObjects(bad, ids); });
    };
    change(0, 0); change(4, file.size()); change(8, 0x06026000); change(12, file.size());
    change(16, 0); change(16, 1); change(16, 3); change(16, MaximumWorldObjects + 1); change(16, 0xffffffff);
    change(32 + 0x70 + 4, 1); change(32 + 8, 0x105); change(32 + 8, 0x1000b);
    change(32 + 12, 7); change(32 + 0x14, 0);
    for (unsigned i = 0; i < 16; ++i)
    {
        change(32 + 0x20 + 4 * i, 0x7fc00000); change(32 + 0x20 + 4 * i, 0x7f800000);
    }
    change(32 + 0x60, std::bit_cast<std::uint32_t>(-1.f));
    change(32 + 0x60, 0x7f800000);
    change(32 + 0x20 + 3 * 4, std::bit_cast<std::uint32_t>(1.f));
    change(32 + 0x20 + 15 * 4, 0);
    change(32 + 0x70 + 0x88, 2); change(32 + 0x70 + 0x8c, 0);
    change(32 + 0x70 + 0x70, std::bit_cast<std::uint32_t>(100.f));
    change(32 + 0x70 + 0x80, 0x7fc00000);
    const std::uint32_t absent[] = {3}, duplicate[] = {1, 1};
    Reject([&] { ReadStaticWorldObjects(file, absent); });
    Reject([&] { ReadStaticWorldObjects(file, duplicate); });
    Reject([&] { ReadStaticWorldObjects(file, {}); });
    auto unknown = World({Drawable(1)}); Put(unknown, 32 + 8, 0x108);
    Check(ReadWorldObjectIndex(unknown)[0].type == 0x108);
    const std::uint32_t one[] = {1};
    Reject([&] { ReadStaticWorldObjects(unknown, one); });

    // Original parent/binding layout; the controller record follows the selected
    // drawable, so static selection must inspect the complete object stream.
    Buffer parent(48), animation(0x90);
    Put(parent, 8, 0x10); Put(parent, 12, parent.size()); Put(parent, 32, 1); Put(parent, 36, 999);
    Put(parent, 40, 888); Put(animation, 4, 77); Put(animation, 8, 0x106);
    Put(animation, 0x60, 1); Put(animation, 0x70, 1);
    auto animated = World({Drawable(1), parent, animation, Drawable(2, true)});
    auto records = ReadWorldObjectIndex(animated);
    Check(records.size() == 4 && records[0].animated && !records[3].animated && records[1].type == 0x10);
    Reject([&] { ReadStaticWorldObjects(animated, one); });
    const std::uint32_t two[] = {2};
    Check(ReadStaticWorldObjects(animated, two)[0].id == 2);
    Reject([&] { ReadWorldObjectIndex(World({animation})); });
    Reject([&] { ReadWorldObjectIndex(World({parent})); });
    Reject([&] { ReadWorldObjectIndex(World({parent, parent, animation})); });
    Reject([&] { ReadStaticWorldObjects(World({parent, Drawable(1)}), one); });
    for (std::uint32_t size : {0u, 16u, 31u, 33u, 64u, 0xffffffffu})
    { auto bad = parent; Put(bad, 12, size); Reject([&] { ReadWorldObjectIndex(World({bad, animation})); }); }
    for (std::uint32_t count : {3u, 16385u, 0xffffffffu})
    { auto bad = animation; Put(bad, 0x60, count); Reject([&] { ReadWorldObjectIndex(World({parent, bad})); }); }
    auto bad = animation; Put(bad, 0x70, 100); Reject([&] { ReadWorldObjectIndex(World({parent, bad})); });
    // All known record extents can be indexed independently of their unselected
    // behavior; no unsupported object becomes a fabricated static drawable.
    const std::pair<std::uint32_t, unsigned> extents[] = {
        {0x102,48},{0x103,144},{0x104,96},{0x107,128},{0x108,112},{0x109,160},
        {0x10000,144},{0x10001,128},{0x10003,144},{0x10004,128},{0x10005,128},
        {0x10006,112},{0x10007,112},{0x10008,128},{0x10009,128},{0x1000a,144}};
    for (auto [type, size] : extents)
    {
        Buffer record(size); Put(record, 4, 1); Put(record, 8, type);
        auto world = World({record}); Check(ReadWorldObjectIndex(world)[0].size == size);
        Reject([&] { ReadStaticWorldObjects(world, one); });
    }
    Check(ReadWorldObjectIndex(World({})).empty());
    auto extra = file; extra.push_back(0); Reject([&] { ReadWorldObjectIndex(extra); });
    extra = file; extra.insert(extra.end(), file.begin() + 8, file.end()); Put(extra, 4, extra.size() - 8);
    Reject([&] { ReadWorldObjectIndex(extra); });
    auto host = decoded[0]; host.bounds_min[0] = std::numeric_limits<float>::quiet_NaN();
    Reject([&] { ValidateStaticWorldObject(host); });
    host = decoded[0]; host.transform[0] = 1e8f; Reject([&] { ValidateStaticWorldObject(host); });
}
}
int main()
{
    try { Run(); std::cout << "World resource checks: " << checks << '\n'; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << " (check " << checks << ")\n"; return 1; }
}

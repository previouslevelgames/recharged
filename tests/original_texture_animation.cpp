#include "Game/GL/GLTextureAnim.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glTexture.h"
#include "NL/glx/glxTexture.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "platform/texture_animation_abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks;
void Check(bool ok, const char* reason)
{
    ++checks;
    if (!ok) throw std::runtime_error(reason);
}
u32 Bits(float value)
{
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
float Float(u32 bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
u32 Word(std::istream& input)
{
    u32 word;
    input >> word;
    Check(bool(input), "independent fixture oracle is incomplete");
    return word;
}
std::vector<u8> Read(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    Check(bool(input), "cannot open independent Wii bytes");
    return {std::istreambuf_iterator<char>(input), {}};
}
struct TextureStorage {
    alignas(PlatTexture) std::array<std::byte, sizeof(PlatTexture)> bytes;
};
void Record(const char* bytes_path, const char* oracle_path)
{
    const auto bytes = Read(bytes_path);
    std::ifstream oracle(oracle_path);
    Check(bool(oracle), "cannot open independent arithmetic oracle");
    const auto claimed_size = Word(oracle);
    const bool accepted = Word(oracle) != 0;
    Check(glIsTextureAnim(bytes.data(), claimed_size) == accepted,
        "original animation selection differs from Wii magic/36-byte oracle");
    if (!accepted) return;

    GLTextureAnim anim{};
    mscharged::platform::ExpandTextureAnimHeader(anim, bytes.data());
    Check(u32(anim.m_nFrame) == Word(oracle), "Wii signed frame bits changed");
    Check(anim.m_uHashID == Word(oracle), "Wii animation hash width changed");
    const auto count = Word(oracle);
    Check(u32(anim.m_nNumTextures) == count, "Wii signed frame count changed");
    Check(u32(anim.m_ePlayMode) == Word(oracle), "Wii play mode changed");
    Check(u32(anim.m_nPlayDir) == Word(oracle), "Wii signed direction changed");
    Check(anim.m_bPaused == (Word(oracle) != 0), "authored pause boolean changed");
    for (const auto byte : anim.m_pad15)
        Check(byte == Word(oracle), "authored header padding changed");
    Check(anim.m_textureIndex == Word(oracle), "Wii index bits changed");
    Check(Bits(anim.m_fTime) == Word(oracle), "authored header float bits changed");
    Check(reinterpret_cast<std::uintptr_t>(anim.m_pAnimTex) == Word(oracle),
        "Wii temporary pointer word was truncated/altered");

    std::vector<GLAnimTex> frames(count);
    for (u32 i = 0; i < count; ++i) {
        mscharged::platform::ExpandTextureAnimFrame(frames[i],
            bytes.data() + 36 + i * 8);
        Check(frames[i].m_TexHandle == Word(oracle), "authored frame hash changed");
        Check(Bits(frames[i].m_fTime) == Word(oracle), "authored frame duration bits changed");
    }
    const auto steps = Word(oracle);
    if (steps == 0) return; // Transport-only unusual records remain uninterpreted.

    const auto free = StandardAllocator.TotalFreeMemory();
    auto* manager = new (8, false) glTextureManager(count + 1);
    gTextureManager = manager;
    Check(manager->mFreeIndices->mCount == count + 1,
        "original constructor queue did not retain source order/count");
    std::vector<TextureStorage> textures(count);
    for (u32 i = 0; i < count; ++i) {
        auto* texture = new (textures[i].bytes.data()) PlatTexture;
        manager->RegisterTexture(texture);
        Check(texture->m_TextureIndex == i && manager->mTextures[i] == texture,
            "original static texture registration order changed");
        frames[i].m_TexHandle = i; // Explicit fixture state, not game-side loading.
    }
    anim.m_pAnimTex = frames.data();
    anim.m_nFrame = 0;
    anim.m_textureIndex = 0xFFFF;
    manager->RegisterTextureAnim(&anim);
    Check(anim.m_textureIndex == count && manager->mTextures[count] == manager->mTextures[0],
        "original animation alias allocation changed");
    glTextureBinding cached{};
    cached.texture = 0xE13AB491;
    cached.textureIndex = count;
    manager->ResolveTextureIndex(&cached);
    Check(cached.textureIndex == count, "original cached binding was re-resolved");
    const unsigned long absent = 0xFFFF;
    Check(manager->GetTextureAtIndex(&absent) == nullptr,
        "original FFFF binding sentinel changed");
    for (u32 i = 0; i < steps; ++i) {
        const auto dt = Float(Word(oracle));
        const auto frame = Word(oracle), direction = Word(oracle), time = Word(oracle);
        anim.Update(dt);
        Check(u32(anim.m_nFrame) == frame, "original frame transition differs");
        Check(u32(anim.m_nPlayDir) == direction, "original ping-pong direction differs");
        // Python does not preserve NaN payload/sign through host arithmetic.
        const auto actual_time = Bits(anim.m_fTime);
        const bool expected_nan = (time & 0x7F800000) == 0x7F800000 && (time & 0x7FFFFF);
        Check(expected_nan ? ((actual_time & 0x7F800000) == 0x7F800000 && (actual_time & 0x7FFFFF))
                           : actual_time == time,
            "original float accumulation/drop-overrun differs");
        Check(manager->mTextures[count] == manager->mTextures[anim.m_nFrame],
            "original alias refresh differs from actual retained frame");
    }
    glReleaseTextureAnim(&anim);
    Check(anim.m_textureIndex == 0xFFFF && manager->mTextures[count] == nullptr,
        "original animation release did not clear/reset its slot");
    Check(manager->mFreeIndices->mCount == 1 && manager->mFreeIndices->RemoveStart() == count,
        "original animation release queue order changed");

    // The original manager has no destructor. This bounded fixture owns and
    // frees its explicit component allocations; it does not claim game shutdown.
    nlDeleteGameObject(manager->mFreeIndices);
    nlFree(manager->mIndexBuffer);
    nlFree(manager->mTextures);
    nlDeleteGameObject(manager);
    gTextureManager = nullptr;
    Check(StandardAllocator.TotalFreeMemory() == free,
        "fixture game allocations did not return to their owning arena");
}
void Queue()
{
    u16 words[5] = {90,91,92,93,94};
    glTextureIndexQueue queue(words, 5);
    for (u16 i = 0; i < 5; ++i) queue.AddEnd(i);
    for (u16 i = 0; i < 5; ++i) Check(queue.RemoveStart() == i, "original queue order changed");
    Check(queue.RemoveStart() == words[queue.mHead & queue.mCapacity],
        "original empty-queue branch was replaced by validation");
    queue.AddEnd(123);
    Check(queue.RemoveStart() == 123, "original queue wrap changed");
}
} // namespace
int main(int argc, char** argv)
{
    try {
        if (argc != 3) throw std::runtime_error("usage: original_texture_animation BYTES ORACLE");
        std::vector<std::uint64_t> standard(128*1024), virtual_memory(128*1024);
        StandardAllocator.Initialize(standard.data(), standard.size()*sizeof(standard[0]));
        VirtualAllocator.Initialize(virtual_memory.data(), virtual_memory.size()*sizeof(virtual_memory[0]));
        gMemoryInitialized = 1;
        Queue();
        Record(argv[1], argv[2]);
        std::cout << "original texture transport/queue/manager/update checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

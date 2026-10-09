// Native storage and Aurora GX forwarding for the original GL target registry.
#include "runtime/views.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glStruct.h"
#include "NL/gl/glTextureManager.h"
#include "NL/gl/glTexture.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/glxTexture.h"
#include "NL/glx/glxGX.h"
#include <dolphin/gx.h>
#include <dolphin/gx/GXExtra.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
struct Storage
{
    u64 generation;
    GLResourcePool* pool = nullptr;
    bool copied = false;
};
std::map<const GLXTarget*, Storage> storage;
std::unique_ptr<GLXTarget> backbuffer;
u64 next_generation = 1;
void (*drain_gpu)() = nullptr;
struct alignas(32) CopyBlock { u8 data[32]; };
std::vector<CopyBlock> clear_storage;
bool clear_used = false;
void RequireGX()
{
    if (!backbuffer || !drain_gpu) throw std::logic_error("GL target rendering requires an active GX session");
}
void DefaultClear()
{
    const auto& c = backbuffer->mClearColour;
    GXSetCopyClear({c.c[0], c.c[1], c.c[2], c.c[3]}, GX_MAX_Z24);
}
struct PreserveWrites
{
    bool colour, alpha;
    PreserveWrites(bool colour_write, bool alpha_write, bool depth_write)
        : colour(gxSetColourUpdate(colour_write)), alpha(gxSetAlphaUpdate(alpha_write))
    { gxSaveZMode(); gxSetZMode(false, GX_LEQUAL, depth_write); }
    ~PreserveWrites() { gxSetColourUpdate(colour); gxSetAlphaUpdate(alpha); gxRestoreZMode(); }
};
}

u64 glNativeTargetGeneration(const GLXTarget* target)
{
    const auto found = storage.find(target);
    if (found == storage.end()) throw std::invalid_argument("Foreign native GL target");
    return found->second.generation;
}
GLXTarget* glplatGetBackBufferTarget() { return backbuffer.get(); }
GLXTarget* glplatCreateTarget(const GLTargetInfo* info)
{
    if (!info || !info->width || !info->height || info->width > glGetScreenWidth()
        || info->height > glGetScreenHeight()
        || (info->format != GLTargetFormat_RGB5A3 && info->format != GLTargetFormat_RGB565
            && info->format != GLTargetFormat_RGBA8 && info->format != GLTargetFormat_A8
            && info->format != GLTargetFormat_IA8))
        throw std::invalid_argument("Unsupported native GL target size or format");
    auto target = std::make_unique<GLXTarget>(info);
    storage.emplace(target.get(), Storage{next_generation++});
    return target.release();
}
GLXTarget::~GLXTarget()
{
    const auto found = storage.find(this);
    if (found == storage.end()) return; // Constructor/registration failure.
    if (found->second.pool) DestroyTexture(mTextureHash);
    storage.erase(this);
}
void GLXTarget::Activate(unsigned long mode)
{
    RequireGX();
    if (mode) throw std::invalid_argument("Unsupported GL target activation mode");
    glNativeTargetGeneration(this);
    // Wii views share the EFB; explicit copy modes resolve it to target textures.
}
void GLXTarget::ReservedTargetHookA() {} // Empty in the selected original implementation.
void GLXTarget::ReservedTargetHookB() {} // Empty in the selected original implementation.
void GLXTarget::CreateTexture(unsigned long hash)
{
    auto& record = storage.at(this);
    if (record.pool || this == backbuffer.get() || !hash || hash > UINT32_MAX || glx_GetTex(hash))
        throw std::invalid_argument("Invalid or duplicate native target texture");
    eGXTextureFormat format;
    std::array<u8, 4> bits{};
    switch (mFormat)
    {
    case GLTargetFormat_RGB5A3: mCopyFormat = GX_TF_RGB5A3; format = GXTex_RGB5A3; bits = {5, 5, 5, 3}; break;
    case GLTargetFormat_RGB565: mCopyFormat = GX_TF_RGB565; format = GXTex_RGB565; bits = {5, 6, 5, 0}; break;
    case GLTargetFormat_RGBA8: mCopyFormat = GX_TF_RGBA8; format = GXTex_RGBA8; bits = {8, 8, 8, 8}; break;
    case GLTargetFormat_A8: mCopyFormat = GX_CTF_A8; format = GXTex_A8; bits = {0, 0, 0, 8}; break;
    case GLTargetFormat_IA8: mCopyFormat = GX_TF_IA8; format = GXTex_IA8; bits = {8, 8, 8, 8}; break;
    default: throw std::invalid_argument("Unsupported native target texture format");
    }
    const auto bytes = GXGetTexBufferSize(mWidth, mHeight, GXTexFmt(mCopyFormat), GX_FALSE, 0);
    const GLMemoryRequirement requirements[] = {{GLM_Header, 1024}, {GLM_Target, bytes + 32}};
    auto* pool = glCreateResourcePool(requirements, 2, "Render target");
    try
    {
        auto* texture = new (pool->Allocate(sizeof(PlatTexture), GLM_Header)) PlatTexture;
        auto* pixels = pool->Allocate(bytes, GLM_Target);
        std::memset(pixels, 0, bytes);
        texture->m_Width = mWidth; texture->m_Height = mHeight;
        texture->m_Format = format; texture->m_Levels = texture->m_MaxLevel = 1;
        std::copy(bits.begin(), bits.end(), texture->m_Bits);
        texture->m_SwizzledData = pixels; texture->m_NativeDataBytes = bytes;
        glRegisterTexture(hash, texture, pool);
        mTextureHash = hash; mTexture = texture; mTextureData = pixels;
        record.pool = pool;
    }
    catch (...) { glDestroyResourcePool(pool); throw; }
}
void GLXTarget::DestroyTexture(unsigned long hash)
{
    auto& record = storage.at(this);
    if (!record.pool) return;
    if (hash != mTextureHash) throw std::invalid_argument("Wrong target texture hash");
    if (record.copied) { GXDestroyCopyTex(mTextureData); drain_gpu(); }
    else if (drain_gpu) drain_gpu();
    glDestroyResourcePool(record.pool);
    record.pool = nullptr; record.copied = false;
    mTexture = nullptr; mTextureData = nullptr; mTextureHash = 0;
}
void GLXTarget::ClearDefault() { ClearBuffers(mClearColourEnabled, mClearDepthEnabled, false); }
void GLXTarget::ClearBuffers(bool colour, bool depth, bool)
{
    RequireGX();
    PreserveWrites writes(colour && mClearColourEnabled, colour && mClearColourEnabled, depth && mClearDepthEnabled);
    const auto& c = mClearColour;
    GXSetCopyClear({c.c[0], c.c[1], c.c[2], c.c[3]}, GX_MAX_Z24);
    GXSetTexCopySrc(0, 0, glGetScreenWidth(), glGetScreenHeight());
    GXSetTexCopyDst(glGetScreenWidth(), glGetScreenHeight(), GX_CTF_R8, GX_FALSE);
    GXCopyTex(clear_storage.data(), GX_TRUE);
    clear_used = true;
    DefaultClear();
}
void GLXTarget::CopyToTexture(bool clear, bool alpha_only_clear)
{
    RequireGX();
    auto& record = storage.at(this);
    if (!record.pool || !mTextureData) throw std::logic_error("GL target has no copy texture");
    const auto& vp = *glplatGetViewport();
    if (vp.x < 0 || vp.y < 0 || vp.width <= 0 || vp.height <= 0
        || unsigned(vp.width) > glGetScreenWidth() || unsigned(vp.height) > glGetScreenHeight()
        || unsigned(vp.x) > glGetScreenWidth() - unsigned(vp.width)
        || unsigned(vp.y) > glGetScreenHeight() - unsigned(vp.height))
        throw std::invalid_argument("Invalid GL target copy rectangle");
    const bool half = mWidth * 2 == unsigned(vp.width) && mHeight * 2 == unsigned(vp.height);
    if (!half && (mWidth != unsigned(vp.width) || mHeight != unsigned(vp.height)))
        throw std::invalid_argument("GL target copy requires equal or half-size dimensions");
    PreserveWrites writes(!alpha_only_clear, true, !alpha_only_clear);
    GXSetTexCopySrc(vp.x, vp.y, vp.width, vp.height);
    GXSetTexCopyDst(mWidth, mHeight, GXTexFmt(mCopyFormat), half);
    GXCopyTex(mTextureData, clear);
    GXPixModeSync();
    record.copied = true;
}
void glplatCopyTargetToTexture(GLXTarget* target, bool clear, bool alpha_only_clear)
{
    glNativeTargetGeneration(target);
    target->CopyToTexture(clear, alpha_only_clear);
}
namespace mscharged
{
void InitializeNativeTargets(unsigned width, unsigned height, void (*drain)())
{
    if (backbuffer || !storage.empty()) throw std::logic_error("Native targets already initialized");
    GLTargetInfo info{};
    info.width = width; info.height = height; info.format = GLTargetFormat_RGBA8;
    info.clearFlags = 7; info.clearDepthSetting = 3;
    info.colour[0] = 24; info.colour[1] = 28; info.colour[2] = 34; info.colour[3] = 255;
    // Padded R8 copy storage; its address remains valid until GX has drained.
    clear_storage.resize(((width + 7) / 8 * 8) * ((height + 3) / 4 * 4) / sizeof(CopyBlock));
    backbuffer.reset(glplatCreateTarget(&info));
    drain_gpu = drain;
}
void ShutdownNativeTargets()
{
    if (storage.size() > (backbuffer ? 1u : 0u)) throw std::logic_error("Release GL target registry before platform targets");
    if (clear_used) { GXDestroyCopyTex(clear_storage.data()); drain_gpu(); }
    clear_used = false;
    backbuffer.reset();
    clear_storage.clear(); clear_storage.shrink_to_fit(); drain_gpu = nullptr;
}
}

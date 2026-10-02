#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_math.h"
#include "runtime/startup.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxMatrix.h"
#include "NL/nlString.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>

static_assert(sizeof(glMatrixHandle) == sizeof(std::uintptr_t));
static_assert(sizeof(glModelPacket::matrix) == sizeof(std::uintptr_t));
static_assert(sizeof(glStateBundle::matrix) == sizeof(std::uintptr_t));
static_assert(sizeof(glStateBundle::raster) == 4 && sizeof(glStateBundle::texture[0]) == 4);

namespace
{
unsigned checks = 0;
void Require(bool value, const char* message)
{ ++checks; if (!value) throw std::runtime_error(message); }
template<class Error, class Action> void Reject(Action action)
{
    try { action(); } catch (const Error&) { ++checks; return; }
    throw std::runtime_error("Invalid matrix/state operation was accepted");
}
void Near(float value, double expected, double tolerance = 0.0003)
{
    ++checks;
    if (!std::isfinite(value) || std::abs(value - expected) > tolerance)
        throw std::runtime_error("Matrix/math fixture: got " + std::to_string(value)
            + ", expected " + std::to_string(expected));
}
void Identity(const nlMatrix4& matrix)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) Near(matrix.e2[c][r], c == r ? 1 : 0);
}
void Same(const nlMatrix4& a, const nlMatrix4& b)
{ for (int i = 0; i < 16; ++i) Near(a.e[i], b.e[i]); }
struct HostFree { void operator()(void* p) const { ::operator delete(p, std::align_val_t(64)); } };
struct Arenas
{
    std::unique_ptr<void, HostFree> mem1{::operator new(8 * 1024 * 1024, std::align_val_t(64))};
    std::unique_ptr<void, HostFree> mem2{::operator new(16 * 1024 * 1024, std::align_val_t(64))};
    Arenas()
    {
        mscharged::ResetStartupMemory();
        StandardAllocator.Initialize(mem1.get(), 8 * 1024 * 1024);
        VirtualAllocator.Initialize(mem2.get(), 16 * 1024 * 1024);
        gMemoryInitialized = 1;
        Require(reinterpret_cast<std::uintptr_t>(mem1.get()) > UINT32_MAX, "Tests require a native address above 4 GiB");
    }
    ~Arenas() { glShutdownMemory(); mscharged::ResetStartupMemory(); }
};
unsigned invalidations = 0;
void Invalidate() { ++invalidations; } // CPU lifetime test; the scene installs real GX invalidation.
const GLMemoryRequirement requirements[] = {{GLM_Header, 65536}, {GLM_VertexData, 65536}};
const GLMemoryConfig config{512, 512, requirements, 2, 4};

void CheckHandles()
{
    Reject<std::logic_error>([] { mscharged::InitializeOriginalGraphicsState(); });
    Reject<std::logic_error>([] { glGetCurrentMatrix(); });
    Reject<std::invalid_argument>([] { nlMatrix4 m; glGetMatrix(GL_INVALID_MATRIX, m); });
    glInitMemory(&config);
    mscharged::InitializeOriginalGraphicsState();
    auto identity = glGetIdentityMatrix();
    Require(identity > UINT32_MAX && identity % 32 == 0, "Original identity handle lost address width or alignment");
    nlMatrix4 value, copy;
    glGetMatrix(identity, copy); Identity(copy);
    Require(glGetCurrentMatrix() == identity, "Original default state lost its identity handle");
    Reject<std::logic_error>([] { mscharged::InitializeOriginalGraphicsState(); });
    Require(glGetCurrentMatrix() == identity, "Repeated startup corrupted live state");
    nlMakeTranslationMatrix(value, 3, 5, 7);
    std::array<glModelPacket, 3> packets{};
    glModel model{1, 3, packets.data()};
    glModelSetMatrix(&model, value);
    auto frame = packets[0].matrix;
    Require(frame > UINT32_MAX && frame % 32 == 0 && frame != identity, "Frame handle is not a separate native allocation");
    for (const auto& packet : packets) Require(packet.matrix == frame, "Model matrix propagation truncated an address");
    glModelGetMatrix(&model, copy); Same(value, copy);
    Require(glSetCurrentMatrix(frame) == identity, "Previous global matrix was truncated");
    glStateBundle saved; glStateSave(saved);
    glSetCurrentMatrix(identity); glStateRestore(saved);
    Require(saved.matrix == frame && glGetCurrentMatrix() == frame, "State save/restore lost a native handle");
    alignas(32) nlMatrix4 foreign;
    for (auto bad : {glMatrixHandle(0), GL_INVALID_MATRIX, frame + 1, frame + 32, reinterpret_cast<glMatrixHandle>(&foreign)})
    {
        Reject<std::invalid_argument>([&] { glGetMatrix(bad, copy); });
        Reject<std::invalid_argument>([&] { glSetMatrix(bad, value); });
        if (bad != GL_INVALID_MATRIX)
        {
            Reject<std::invalid_argument>([&] { glSetCurrentMatrix(bad); });
            Reject<std::invalid_argument>([&] { glModelSetMatrix(&model, bad); });
            Require(packets[0].matrix == frame && glGetCurrentMatrix() == frame, "Invalid handle changed live state");
        }
    }
    glStateBundle bad = saved; bad.matrix = 0;
    Reject<std::invalid_argument>([&] { glStateRestore(bad); });
    glModel empty{};
    Reject<std::invalid_argument>([&] { glModelSetMatrix(nullptr, value); });
    Reject<std::invalid_argument>([&] { glModelSetMatrix(&empty, identity); });
    Reject<std::invalid_argument>([&] { glModelGetMatrix(&empty, copy); });
    glModelSetRasterState(&model, 0x87654321);
    for (const auto& packet : packets) Require(packet.rasterState == 0x87654321, "Model raster propagation lost bits");
    // Allocation failure must leave packet handles and pool usage intact.
    glFrameAlloc(448, GLM_Matrix);
    Reject<std::bad_alloc>([&] { glModelSetMatrix(&model, value); });
    Require(packets[0].matrix == frame, "Matrix allocation failure published an invalid packet handle");
    mscharged::SetGraphicsCacheInvalidator(Invalidate);
    glplatFrameAllocNextFrame();
    Reject<std::invalid_argument>([&] { glGetMatrix(frame, copy); });
    Reject<std::invalid_argument>([&] { glStateRestore(saved); });
    auto next = glAllocSetMatrix(value);
    Require(next != frame && next > UINT32_MAX && invalidations == 1, "Frame advance failed to change live storage");
    glGetMatrix(next, copy); Same(value, copy);
    glGetMatrix(identity, copy); Identity(copy);
    auto* pool = glGetCurrentResourcePool();
    auto mark = pool->MarkResource();
    auto resource = reinterpret_cast<glMatrixHandle>(glResourceAlloc(sizeof(nlMatrix4), GLM_Matrix, pool));
    glSetMatrix(resource, value); glModelSetMatrix(&model, resource);
    pool->ReleaseResource(mark);
    Reject<std::invalid_argument>([&] { glGetMatrix(resource, copy); });
    glGetMatrix(identity, copy); Identity(copy);
    auto* other = glCreateResourcePool(requirements, 2, "matrix lifetime");
    resource = reinterpret_cast<glMatrixHandle>(glResourceAlloc(sizeof(nlMatrix4), GLM_Matrix, other));
    glSetMatrix(resource, value); glDestroyResourcePool(other);
    Reject<std::invalid_argument>([&] { glGetMatrix(resource, copy); });
    glShutdownMemory();
    Require(glGetIdentityMatrix() == GL_INVALID_MATRIX, "Graphics shutdown retained identity storage");
    Reject<std::logic_error>([] { glGetCurrentMatrix(); });
    Reject<std::invalid_argument>([&] { glGetMatrix(identity, copy); });
}

void CheckPackedState()
{
    glInitMemory(&config); mscharged::InitializeOriginalGraphicsState();
    const unsigned widths[] = {2,2,2,2,2,2,1,1,1,1,1,1,6,6,6,6,6,6};
    std::array<unsigned, 18> values{};
    for (int i = 0; i < GLTS_Num; ++i) glSetTextureState(static_cast<eGLTextureState>(i), 0);
    // Independent Wii memory-word oracle: fields count from bit 0 of word 0;
    // word 0 becomes the numeric high word in a big-endian 64-bit state.
    auto oracle = [&] {
        std::array<std::uint32_t, 2> words{};
        unsigned offset = 0;
        for (unsigned i = 0; i < values.size(); ++i)
        {
            for (unsigned bit = 0; bit < widths[i]; ++bit)
                words[(offset + bit) / 32] |= ((values[i] >> bit) & 1) << ((offset + bit) % 32);
            offset += widths[i];
        }
        return (std::uint64_t(words[0]) << 32) | words[1];
    };
    std::mt19937 random(701);
    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        unsigned field = iteration % GLTS_Num, value = random();
        Require(glSetTextureState(static_cast<eGLTextureState>(field), value) == values[field], "Texture setter returned the wrong previous field");
        values[field] = value & ((1u << widths[field]) - 1);
        Require(glHandleizeTextureState() == oracle(), "Texture state changed Wii word ordering or neighboring bits");
        for (int i = 0; i < GLTS_Num; ++i)
            Require(glGetTextureState(oracle(), static_cast<eGLTextureState>(i)) == values[i], "Texture field extraction failed across a word boundary");
    }
    const unsigned raster_widths[] = {1,1,2,1,8,3,2,2,1,1,1,2,2,2,1,2};
    u32 raster = 0xa5a5a5a5, expected = raster;
    unsigned offset = 0;
    for (int i = 0; i < GLS_Num; ++i)
    {
        unsigned mask = (1u << raster_widths[i]) - 1, value = random();
        unsigned previous = (expected >> offset) & mask;
        Require(glSetRasterState(raster, static_cast<eGLState>(i), value) == previous, "Raster setter returned the wrong previous field");
        expected = (expected & ~(mask << offset)) | ((value & mask) << offset);
        Require(raster == expected && glGetRasterState(raster, static_cast<eGLState>(i)) == (value & mask), "Raster fields overlap or lose high bits");
        offset += raster_widths[i];
    }
    glSetDefaultState(true);
    Require(glGetRasterState(glGetCurrentRasterState(), GLS_DepthTest) == 1
        && glGetRasterState(glGetCurrentRasterState(), GLS_DepthWrite) == 1
        && glGetRasterState(glGetCurrentRasterState(), GLS_DepthFunc) == 1
        && glGetRasterState(glGetCurrentRasterState(), GLS_Culling) == 1
        && glGetRasterState(glGetCurrentRasterState(), GLS_ColourWrite) == 3, "Original raster defaults changed");
    for (int i = 0; i < GLTS_Num; ++i)
        Require(glGetTextureState(static_cast<eGLTextureState>(i)) == (i >= 12 && i != 13 ? 63 : 0), "Original texture defaults changed");
    for (int i = 0; i < GLTT_Num; ++i)
    {
        auto type = static_cast<eGLTextureType>(i);
        Require(glSetCurrentTexture(0x87654321, type) == UINT32_MAX && glGetCurrentTexture(type) == 0x87654321, "Texture IDs lost their 32-bit contract");
    }
    Require(gl_GetCurrentStateBundle()->texconfig == 63, "Enabled texture mask is wrong");
    for (int i = 0; i < GLTT_Num; ++i) glSetCurrentTexture(UINT32_MAX, static_cast<eGLTextureType>(i));
    Require(gl_GetCurrentStateBundle()->texconfig == 0, "Disabled texture mask is wrong");
    auto texture_before = glHandleizeTextureState(); auto raster_before = glHandleizeRasterState();
    for (int invalid : {-1, 999})
    {
        Reject<std::invalid_argument>([&] { glSetTextureState(static_cast<eGLTextureState>(invalid), 1); });
        Reject<std::invalid_argument>([&] { glGetTextureState(static_cast<eGLTextureState>(invalid)); });
        Reject<std::invalid_argument>([&] { glSetRasterState(static_cast<eGLState>(invalid), 1); });
        Reject<std::invalid_argument>([&] { glGetRasterState(raster, static_cast<eGLState>(invalid)); });
        Reject<std::invalid_argument>([&] { glSetCurrentTexture(1, static_cast<eGLTextureType>(invalid)); });
    }
    Require(texture_before == glHandleizeTextureState() && raster_before == glHandleizeRasterState(), "Rejected state changes altered packed bits");
    Require(glGetTexture(nullptr) == UINT32_MAX && glGetTexture("") == UINT32_MAX, "Null/empty texture hashing changed");
    const char name[] = {'b', char(0xff), 'A', 0};
    Require(glGetTexture(name) == 79265, "Texture name hash changed unsigned byte handling");
    glShutdownMemory();
    Reject<std::logic_error>([] { glHandleizeTextureState(); });
}

void CheckMath()
{
    nlMatrix4 scale, translate, result, inverse, expected;
    nlMakeScaleMatrix(scale, 2, 3, 4); nlMakeTranslationMatrix(translate, 5, 7, 11);
    nlMultMatrices(result, scale, translate); // Apply scale, then translation.
    nlVector3 vector{1, 2, 3}; nlMultPosVectorMatrix(vector, vector, result);
    Near(vector.x, 7); Near(vector.y, 13); Near(vector.z, 23);
    vector = {1, 2, 3}; nlMultDirVectorMatrix(vector, vector, result);
    Near(vector.x, 2); Near(vector.y, 6); Near(vector.z, 12);
    expected = result; nlMultMatrices(scale, scale, translate); Same(scale, expected);
    nlInvertMatrix(inverse, result); nlMultMatrices(result, inverse, expected); Identity(result);
    nlInvertMatrix(expected, expected); Same(expected, inverse);
    // Inverse must pivot when the first diagonal entry is zero.
    result.SetIdentity(); result.e2[0][0] = result.e2[1][1] = 0;
    result.e2[1][0] = result.e2[0][1] = 1;
    nlInvertMatrix(inverse, result); Same(inverse, result);
    result = {}; expected.SetIdentity();
    Reject<std::invalid_argument>([&] { nlInvertMatrix(expected, result); }); Identity(expected);
    nlMakeTranslationMatrix(result, 5, 7, 11); nlTransposeMatrix(result, result);
    Near(result.e2[0][3], 5); Near(result.e2[1][3], 7); Near(result.e2[2][3], 11);
    nlMatrix4 view;
    glMatrixLookAt(view, {0,0,5}, {0,0,0}, {0,1,0});
    vector = {1,2,3}; nlMultPosVectorMatrix(vector, vector, view);
    Near(vector.x, 1); Near(vector.y, 2); Near(vector.z, -2);
    // Exercise Charged's special tilted-up branch with an analytic basis.
    glMatrixLookAt(view, {0,0,5}, {0,0,0}, {0,0.6f,0.8f});
    Near(view.e2[1][1], 0.8); Near(view.e2[2][1], -0.6);
    Near(view.e2[1][2], 0.6); Near(view.e2[2][2], 0.8); Near(view.e2[3][2], -5);
    for (int row = 0; row < 3; ++row)
    {
        double length = 0;
        for (int c = 0; c < 3; ++c) length += view.e2[c][row] * view.e2[c][row];
        Near(static_cast<float>(length), 1);
        for (int other = row + 1; other < 3; ++other)
        {
            double dot = 0;
            for (int c = 0; c < 3; ++c) dot += view.e2[c][row] * view.e2[c][other];
            Near(static_cast<float>(dot), 0);
        }
    }
    float gx3[3][4], gx4[4][4];
    glxCopyMatrix(gx3, view); glxCopyMatrix(gx4, view);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) Near(gx3[r][c], view.e2[c][r]);
    Require(std::memcmp(gx4, view.e2, sizeof(gx4)) == 0, "GX projection copy changed word ordering");
    glxCopyMatrix(result, gx3); Same(result, view);
    glMatrixPerspective(result, 3.1415927f / 2, 2, 1, 11);
    Near(result.e2[0][0], 0.5); Near(result.e2[1][1], 1);
    Near(result.e2[2][2], -0.1); Near(result.e2[2][3], -1.1); Near(result.e2[3][2], -1);
    glMatrixOrthographicCentered(result, 8, 4, 1, 11);
    Near(result.e2[0][0], 0.25); Near(result.e2[1][1], 0.5); Near(result.e2[2][2], -0.1); Near(result.e2[2][3], -1.1);
    glMatrixOrthographic(result, 640, 480);
    Near(result.e2[0][0], 2.0/640); Near(result.e2[1][1], -2.0/480);
    for (float angle : {-1000.0f, -7.0f, -3.1415927f, 0.0f, 1.5707963f, 3.1415927f, 7.0f, 1000.0f})
    {
        nlMakeRotationMatrixY(result, angle);
        vector = {1,0,0}; nlMultDirVectorMatrix(vector, vector, result);
        // Original trig quantizes to 16-bit turns; compare bounded angles to geometry.
        if (std::abs(angle) < 10) { Near(vector.x, std::cos(angle)); Near(vector.z, -std::sin(angle)); }
        Near(vector.x * vector.x + vector.z * vector.z, 1, 0.0005);
    }
    Require(mscharged::NativeFixedAngle16(7) > 0, "Wrapped angle lost its fractional turn");
    Reject<std::invalid_argument>([] { nlMatrix4 m; nlMakeRotationMatrixY(m, std::numeric_limits<float>::infinity()); });
    nlQuaternion q; nlQuatIdentity(q); nlQuatScale(q, q, 2);
    Near(nlQuatDot(q, q), 4); nlQuatNormalize(q, q); Near(q.w, 1);
    nlQuatToMatrix(result, q, true); Identity(result);
    for (float number : {0.125f, 1.0f, 2.0f, 97.0f, 10000.0f})
    {
        Near(nlSqrt(number, true), std::sqrt(number), 0.0001);
        Near(nlSqrt(number, false), std::sqrt(number), 0.0001);
        Near(nlRecipSqrt(number, true), 1.0/std::sqrt(number), 0.00001);
    }
    Require(nlSqrt(0, true) == 0 && std::signbit(nlSqrt(-0.0f, true)), "Square root zero handling changed");
    Require(std::isnan(nlSqrt(-1, true)) && std::isnan(nlSqrt(std::numeric_limits<float>::quiet_NaN(), true))
        && std::isinf(nlRecipSqrt(0, true)), "Original root edge cases changed");
    std::array<unsigned char, 300> bytes; bytes.fill(0x77); nlZeroMemory(bytes.data() + 1, 257);
    Require(bytes.front() == 0x77 && bytes[258] == 0x77, "Native memory zero changed surrounding bytes");
    for (unsigned i = 1; i <= 257; ++i) Require(bytes[i] == 0, "Native memory zero missed bytes");
}
}

int main()
{
    try
    {
        Arenas arenas;
        auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
        CheckHandles(); CheckPackedState(); CheckMath();
        for (unsigned pass = 0; pass < 5; ++pass)
        {
            InitializeOriginalGraphicsMemory(); mscharged::InitializeOriginalGraphicsState();
            glSetDefaultState(false);
            Require(glGetRasterState(glGetCurrentRasterState(), GLS_DepthTest) == 0, "Non-depth default state changed");
            glShutdownMemory(); glShutdownMemory();
        }
        Require(StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2,
            "Matrix/state restart failed to recover original arenas");
        std::cout << "Original matrices/state: " << checks << " native-handle, lifetime, packed Wii word, camera/math and restart checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAILED after " << checks << " checks: " << error.what() << '\n'; return 1; }
}

// Native GX identity texgen qualification; uses actual GPU texture samples.
#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "webgpu/gpu.hpp"
#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
unsigned checks = 0;
std::atomic_uint logErrors{};
void Require(bool yes, const char* message) {
  ++checks;
  if (!yes) throw std::runtime_error(message);
}
void Log(AuroraLogLevel level, const char* module, const char* text, unsigned size) {
  if (level >= LOG_ERROR) ++logErrors;
  std::cerr << '[' << module << "] " << std::string_view(text, size) << '\n';
}
std::string Utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void SetState(unsigned width, unsigned height) {
  GXSetViewport(0, 0, width, height, 0, 1);
  GXSetScissor(0, 0, width, height);
  Mtx44 projection{};
  C_MTXOrtho(projection, 1, -1, -1, 1, 0, 1);
  Mtx matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  GXSetProjection(projection, GX_ORTHOGRAPHIC);
  GXLoadPosMtxImm(matrix, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXSetCullMode(GX_CULL_NONE);
  GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL,
                GX_DF_NONE, GX_AF_NONE);
  GXSetNumTevStages(1);
  GXSetNumTexGens(0);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
}
std::vector<u8> ReadTexture(const wgpu::Texture &texture,
                            wgpu::TextureFormat format, unsigned x,
                            unsigned y) {
  using namespace aurora::webgpu;
  const wgpu::BufferDescriptor desc{.label = "completion proof readback",
                                    .usage = wgpu::BufferUsage::CopyDst |
                                             wgpu::BufferUsage::MapRead,
                                    .size = 256};
  auto buffer = g_device.CreateBuffer(&desc);
  auto encoder = g_device.CreateCommandEncoder();
  const wgpu::TexelCopyTextureInfo src{.texture = texture, .origin = {x, y, 0}};
  const wgpu::TexelCopyBufferInfo dst{
      .layout = {.offset = 0, .bytesPerRow = 256, .rowsPerImage = 1},
      .buffer = buffer};
  const wgpu::Extent3D extent{1, 1, 1};
  encoder.CopyTextureToBuffer(&src, &dst, &extent);
  auto commands = encoder.Finish();
  g_queue.Submit(1, &commands);
  wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::Error;
  const auto future = buffer.MapAsync(
      wgpu::MapMode::Read, 0, 256, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus v, wgpu::StringView) { status = v; });
  Require(g_instance.WaitAny(future, 5'000'000'000) ==
              wgpu::WaitStatus::Success,
          "physical readback map timeout");
  Require(status == wgpu::MapAsyncStatus::Success,
          "physical readback map failed");
  const auto *bytes = static_cast<const u8 *>(buffer.GetConstMappedRange());
  std::vector<u8> result(bytes, bytes + 4);
  buffer.Unmap();
  if (format == wgpu::TextureFormat::BGRA8Unorm ||
      format == wgpu::TextureFormat::BGRA8UnormSrgb)
    std::swap(result[0], result[2]);
  return result;
}
// Real texture sampling. An explicitly loaded identity matrix forces the
// original 3x4 shader path, independently of the candidate key normalization.
enum class IdentityTexgenCase { Identity, MatrixIdentity, MatrixQ, PostQ, IndexedQ, Normalize };
struct IdentityTexgenSample {
  aurora::gx::PipelineConfig config{};
  std::array<std::array<u8, 4>, 9> pixels{};
};
IdentityTexgenSample IdentityTexgenDraw(GXTexGenType type, IdentityTexgenCase mode,
                                       unsigned destination, unsigned source, bool perspective) {
  Require(aurora::gfx::create_pass(64, 64), "identity texgen pass creation failed");
  SetState(64, 64);
  GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
  GXSetDither(GX_FALSE);
  GXSetCoPlanar(GX_FALSE);
  GXSetDstAlpha(GX_FALSE, 0);
  GXSetFog(GX_FOG_NONE, 0, 1, 0, 1, {0, 0, 0, 0});
  GXSetNumIndStages(0);
  GXSetTevDirect(GX_TEVSTAGE0);
  GXSetNumChans(0);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(static_cast<GXAttr>(GX_VA_TEX0 + source), GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, static_cast<GXAttr>(GX_VA_TEX0 + source), GX_TEX_ST, GX_F32, 0);
  if (mode == IdentityTexgenCase::IndexedQ)
    GXSetVtxDesc(static_cast<GXAttr>(GX_VA_TEX0MTXIDX + destination), GX_DIRECT);
  for (unsigned i = 0; i != 8; ++i)
    GXSetTexCoordGen2(static_cast<GXTexCoordID>(i), GX_TG_MTX2x4,
                     static_cast<GXTexGenSrc>(GX_TG_TEX0 + i), GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
  Mtx matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}};
  if (mode == IdentityTexgenCase::MatrixQ || mode == IdentityTexgenCase::IndexedQ ||
      mode == IdentityTexgenCase::PostQ) matrix[2][3] = 2;
  GXLoadTexMtxImm(matrix, GX_TEXMTX0, GX_MTX3x4);
  GXLoadTexMtxImm(matrix, GX_PTTEXMTX0, GX_MTX3x4);
  const bool primaryMatrix = mode == IdentityTexgenCase::MatrixIdentity || mode == IdentityTexgenCase::MatrixQ;
  GXSetTexCoordGen2(static_cast<GXTexCoordID>(destination), type,
                   static_cast<GXTexGenSrc>(GX_TG_TEX0 + source),
                   primaryMatrix ? GX_TEXMTX0 : GX_IDENTITY,
                   mode == IdentityTexgenCase::Normalize ? GX_TRUE : GX_FALSE,
                   mode == IdentityTexgenCase::PostQ ? GX_PTTEXMTX0 : GX_PTIDENTITY);
  GXSetNumTexGens(destination + 1);
  GXSetTevOrder(GX_TEVSTAGE0, static_cast<GXTexCoordID>(destination), GX_TEXMAP0, GX_COLOR_NULL);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  // Native GX RGBA8 consists of 4x4 AR/GB tiles. Three color ramps make
  // coordinate changes observable; a constant texture would not test projection.
  alignas(32) static const std::array<u8, 256> texture = [] {
    std::array<u8, 256> result{};
    for (unsigned y = 0; y != 8; ++y) for (unsigned x = 0; x != 8; ++x) {
      const auto offset = ((y / 4) * 2 + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2;
      result[offset] = 255;
      result[offset + 1] = x * 32;
      result[offset + 32] = y * 32;
      result[offset + 33] = (x + y) * 16;
    }
    return result;
  }();
  GXTexObj object{};
  GXInitTexObj(&object, texture.data(), 8, 8, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&object, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXLoadTexObj(&object, GX_TEXMAP0);
  if (perspective) {
    Mtx44 projection{};
    C_MTXPerspective(projection, 70.f, 1.f, .1f, 5.f);
    GXSetProjection(projection, GX_PERSPECTIVE);
  }
  GXFlush();
  aurora::gx::fifo::drain();
  IdentityTexgenSample result;
  aurora::gx::populate_pipeline_config(result.config, GX_QUADS, GX_VTXFMT0);
  Require(aurora::gx::g_gxState.tcgs[destination].type == type,
          "Identity key normalization changed the live GX texgen register");
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (const auto vertex : std::array<std::array<float, 4>, 4>{{
           {-.8f, -.8f, .125f, .125f}, {.8f, -.8f, .875f, .125f},
           {.8f, .8f, .875f, .875f}, {-.8f, .8f, .125f, .875f}}}) {
    if (mode == IdentityTexgenCase::IndexedQ) GXParam1u8(GX_TEXMTX0);
    const float depth = perspective ? (vertex[0] < 0 ? .4f : 1.4f) : .5f;
    const float extent = perspective ? depth * .700207538f : 1.f; // tan(35 degrees)
    GXPosition3f32(vertex[0] * extent, vertex[1] * extent, -depth);
    GXTexCoord2f32(vertex[2], vertex[3]);
  }
  GXEnd();
  GXDrawDone();
  aurora::gfx::ResolvedTargets target;
  Require(aurora::gfx::resolve_pass({true, true, false}, target), "identity texgen snapshot failed");
  GXDrawDone();
  unsigned at = 0;
  for (unsigned y : {16u, 32u, 48u}) for (unsigned x : {16u, 32u, 48u}) {
    const auto bytes = ReadTexture(target.colorTexture, target.colorFormat, x, y);
    std::copy(bytes.begin(), bytes.end(), result.pixels[at++].begin());
  }
  return result;
}
void RecordedIdentityTexgenPairs(const char* path) {
  if (!path) return;
  std::ifstream input(path, std::ios::binary);
  Require(bool(input), "Recorded texgen pair file unavailable");
  std::array<char, 8> magic{};
  input.read(magic.data(), magic.size());
  Require(std::string_view(magic.data(), magic.size()) == "TCGPAIR1", "Recorded texgen pair header mismatch");
  const auto read32 = [&] {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    Require(bool(input), "Recorded texgen pair metadata truncated");
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
  };
  const auto size = read32(), count = read32();
  Require(size == sizeof(aurora::gx::PipelineConfig) && count == 13,
          "Recorded cold9/warm4 configurations disagree with native ABI/count");
  for (unsigned i = 0; i != count; ++i) {
    aurora::gx::PipelineConfig missed{}, prepared{};
    input.read(reinterpret_cast<char*>(&missed), sizeof missed);
    input.read(reinterpret_cast<char*>(&prepared), sizeof prepared);
    Require(bool(input) && missed.version == 14 && prepared.version == 14,
            "Recorded texgen pair has an incompatible saved version");
    Require(std::memcmp(&missed, &prepared, sizeof missed) != 0, "Recorded pair was not a real prior key difference");
    aurora::gx::canonicalize_identity_texgens(missed.shaderConfig);
    aurora::gx::canonicalize_identity_texgens(prepared.shaderConfig);
    missed.version = prepared.version = aurora::gx::GXPipelineConfigVersion;
    Require(std::memcmp(&missed, &prepared, sizeof missed) == 0,
            "Canonicalized real missed/prepared configs differ in their full native bytes");
  }
  Require(input.peek() == std::char_traits<char>::eof(), "Recorded texgen pair file has trailing bytes");
  std::cout << "Exact native texgen coverage: all cold9/warm4 missed configs equal already-prepared full configs\n";
}
void IdentityTexgenProbe(const char* recordedPairs) {
  const auto keyEqual = [](const auto& a, const auto& b) {
    return std::memcmp(&a.config, &b.config, sizeof a.config) == 0;
  };
  const auto pixelDistance = [](const auto& a, const auto& b) {
    int distance = 0;
    for (unsigned i = 0; i != a.pixels.size(); ++i) for (unsigned channel = 0; channel != 4; ++channel)
      distance = std::max(distance, std::abs(int(a.pixels[i][channel]) - int(b.pixels[i][channel])));
    return distance;
  };
  for (bool perspective : {false, true}) for (unsigned slot : {0u, 1u, 2u}) {
    const auto two = IdentityTexgenDraw(GX_TG_MTX2x4, IdentityTexgenCase::Identity, slot, slot, perspective);
    const auto three = IdentityTexgenDraw(GX_TG_MTX3x4, IdentityTexgenCase::Identity, slot, slot, perspective);
    const auto originalThree = IdentityTexgenDraw(GX_TG_MTX3x4, IdentityTexgenCase::MatrixIdentity, slot, slot, perspective);
    Require(keyEqual(two, three), "Equivalent identity texture generators still have distinct pipeline keys");
    Require(pixelDistance(two, three) == 0, "Equivalent canonical keys changed gradient texture samples");
    Require(originalThree.config.shaderConfig.tcgs[slot].type == GX_TG_MTX3x4 && !keyEqual(three, originalThree),
            "Independent reference did not execute the original projective 3x4 shader");
    Require(pixelDistance(three, originalThree) <= 1,
            "Canonical identity samples differ from original 3x4 gradient projection");
    Require(std::abs(int(two.pixels[3][0]) - int(two.pixels[5][0])) > 30 &&
            std::abs(int(two.pixels[1][1]) - int(two.pixels[7][1])) > 30,
            "Identity proof texture did not produce a visible two-dimensional gradient");
  }
  for (const auto mode : {IdentityTexgenCase::MatrixQ, IdentityTexgenCase::PostQ,
                          IdentityTexgenCase::IndexedQ, IdentityTexgenCase::Normalize}) {
    // Destination2/source0 also proves the matrix-index guard follows destination
    // generator slot, not the source texture-coordinate attribute's slot.
    const auto two = IdentityTexgenDraw(GX_TG_MTX2x4, mode, 2, 0, true);
    const auto three = IdentityTexgenDraw(GX_TG_MTX3x4, mode, 2, 0, true);
    Require(!keyEqual(two, three) && three.config.shaderConfig.tcgs[2].type == GX_TG_MTX3x4,
            "A consumed projective texgen guard was incorrectly canonicalized");
    Require(pixelDistance(two, three) > 12,
            "A guarded projective/normalized/matrix-index case lost distinct gradient pixels");
  }
  // Exhaust the pure predicate's eight source/destination slots, including
  // destination/source aliasing and every non-absent matrix-index encoding.
  for (unsigned destination = 0; destination != 8; ++destination) {
    for (unsigned source = 0; source != 8; ++source) {
      aurora::gx::ShaderConfig base{};
      base.tcgs[destination].type = GX_TG_MTX3x4;
      base.tcgs[destination].src = static_cast<GXTexGenSrc>(GX_TG_TEX0 + source);
      auto candidate = base;
      aurora::gx::canonicalize_identity_texgens(candidate);
      Require(candidate.tcgs[destination].type == GX_TG_MTX2x4,
              "Identity TEXn source/destination predicate boundary was missed");
      for (const auto attr : {GX_DIRECT, GX_INDEX8, GX_INDEX16}) {
        candidate = base;
        candidate.attrs[GX_VA_TEX0MTXIDX + destination].attrType = attr;
        aurora::gx::canonicalize_identity_texgens(candidate);
        Require(candidate.tcgs[destination].type == GX_TG_MTX3x4,
                "Present destination texture matrix index was canonicalized");
      }
    }
  }
  // Non-texture sources have a consumed z/q component even with identity
  // selectors; key-only boundary checks avoid sampling undefined q=0 fixtures.
  for (auto source : {GX_TG_POS, GX_TG_NRM, GX_TG_BINRM, GX_TG_TANGENT, GX_TG_COLOR0, GX_TG_COLOR1}) {
    aurora::gx::ShaderConfig config{};
    config.tcgs[0].type = GX_TG_MTX3x4;
    config.tcgs[0].src = source;
    aurora::gx::canonicalize_identity_texgens(config);
    Require(config.tcgs[0].type == GX_TG_MTX3x4, "Non-TEX source projection was canonicalized");
  }
  RecordedIdentityTexgenPairs(recordedPairs);
  std::cout << "Identity texgens: gradient pixels/key equivalence; dynamic index, matrix, postmatrix and normalization guards passed\n";
}

} // namespace
int main(int argc, char** argv) {
  bool live = false;
  try {
#if defined(_WIN32)
    AuroraBackend backend = BACKEND_D3D12;
#elif defined(__APPLE__)
    AuroraBackend backend = BACKEND_METAL;
#else
    AuroraBackend backend = BACKEND_VULKAN;
#endif
    auto directory = std::filesystem::temp_directory_path() /
        ("charged-identity-texgen-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path recordedPairs;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "--directory" && i + 1 < argc) {
        directory = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[++i]));
      } else if (arg == "--recorded-pairs" && i + 1 < argc) {
        recordedPairs = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[++i]));
      } else if (arg == "--backend" && i + 1 < argc) {
        const std::string_view name = argv[++i];
        if (name == "vulkan") backend = BACKEND_VULKAN;
        else if (name == "dx12") backend = BACKEND_D3D12;
        else if (name == "metal") backend = BACKEND_METAL;
        else throw std::runtime_error("Unknown backend");
      } else {
        throw std::runtime_error("Usage: identity_texgen_gpu_tests [--backend vulkan|dx12|metal] [--directory NEW_PATH] [--recorded-pairs FILE]");
      }
    }
    Require(!std::filesystem::exists(directory), "Fixture directory must be new (preserves existing caches)");
    std::filesystem::create_directories(directory);
    const auto path = Utf8(directory);
    AuroraConfig config{};
    config.appName = "Charged identity texgen qualification";
    config.userPath = config.cachePath = config.resourcesPath = path.c_str();
    config.desiredBackend = backend;
    config.msaa = 1;
    config.windowWidth = 320; config.windowHeight = 240;
    config.windowPosX = config.windowPosY = -1;
    config.vsync = false;
    config.enableBackendValidation = true;
    config.mem1Size = MEM1_DEFAULT_SIZE;
    config.logCallback = Log;
    config.logLevel = LOG_WARNING;
    const auto host = aurora_initialize(argc, argv, &config);
    live = true;
    Require(host.window && host.backend == backend, "Requested GPU backend unavailable");
    Require(aurora::webgpu::g_adapterInfo.adapterType != wgpu::AdapterType::CPU,
            "CPU adapter cannot qualify identity texgen projection");
    OSInit();
    VIInit();
    VIConfigure(&GXNtsc480IntDf);
    alignas(32) std::array<u8, 65536> fifo{};
    GXInit(fifo.data(), fifo.size());
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    GXSetCopyClear({0, 0, 0, 255}, GX_MAX_Z24);
    aurora_update();
    Require(aurora_begin_frame(), "Fixture frame unavailable");
    const auto pairsPath = Utf8(recordedPairs);
    IdentityTexgenProbe(pairsPath.empty() ? nullptr : pairsPath.c_str());
    aurora_end_frame_no_present();
    aurora::gfx::synchronize();
    aurora_shutdown();
    live = false;
    Require(logErrors == 0, "GPU fixture reported a native error");
    std::cout << "Identity texgen GPU passed: " << checks << " checks; evidence: " << path << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Identity texgen GPU failure: " << e.what() << '\n';
    if (live) aurora_shutdown();
    return 1;
  }
}

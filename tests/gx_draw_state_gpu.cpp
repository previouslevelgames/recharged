// Native GX fog and vertex array byte order on the actual GPU. Since GX
// configuration v17 both are selected per draw, so a single pipeline serves
// every fog type and byte order. --record FILE saves every sampled pixel;
// --compare FILE requires identical pixels, e.g. from a build before v17.
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
#include <cstring>
#include <deque>
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

constexpr unsigned Size = 64;
using Row = std::array<std::array<u8, 4>, Size>;

Row ReadRow(const wgpu::Texture& texture, wgpu::TextureFormat format, unsigned y) {
  using namespace aurora::webgpu;
  const wgpu::BufferDescriptor desc{.label = "draw state readback",
                                    .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                    .size = Size * 4};
  auto buffer = g_device.CreateBuffer(&desc);
  auto encoder = g_device.CreateCommandEncoder();
  const wgpu::TexelCopyTextureInfo src{.texture = texture, .origin = {0, y, 0}};
  const wgpu::TexelCopyBufferInfo dst{.layout = {.offset = 0, .bytesPerRow = Size * 4, .rowsPerImage = 1},
                                      .buffer = buffer};
  const wgpu::Extent3D extent{Size, 1, 1};
  encoder.CopyTextureToBuffer(&src, &dst, &extent);
  auto commands = encoder.Finish();
  g_queue.Submit(1, &commands);
  wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::Error;
  const auto future = buffer.MapAsync(wgpu::MapMode::Read, 0, Size * 4, wgpu::CallbackMode::WaitAnyOnly,
                                      [&](wgpu::MapAsyncStatus v, wgpu::StringView) { status = v; });
  Require(g_instance.WaitAny(future, 5'000'000'000) == wgpu::WaitStatus::Success, "readback map timeout");
  Require(status == wgpu::MapAsyncStatus::Success, "readback map failed");
  const auto* bytes = static_cast<const u8*>(buffer.GetConstMappedRange());
  Row row{};
  for (unsigned x = 0; x != Size; ++x) {
    std::copy_n(bytes + x * 4, 4, row[x].begin());
    if (format == wgpu::TextureFormat::BGRA8Unorm || format == wgpu::TextureFormat::BGRA8UnormSrgb)
      std::swap(row[x][0], row[x][2]);
  }
  buffer.Unmap();
  return row;
}

bool Orthographic(GXFogType type) { return (type & 0x08) != 0; }

void SetState(GXFogType fog, bool range) {
  GXSetViewport(0, 0, Size, Size, 0, 1);
  GXSetScissor(0, 0, Size, Size);
  Mtx44 projection{};
  if (Orthographic(fog)) {
    C_MTXOrtho(projection, 1, -1, -1, 1, .1f, 5.f);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
  } else {
    C_MTXPerspective(projection, 70.f, 1.f, .1f, 5.f);
    GXSetProjection(projection, GX_PERSPECTIVE);
  }
  Mtx matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  GXLoadPosMtxImm(matrix, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXSetCullMode(GX_CULL_NONE);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
  GXSetDither(GX_FALSE);
  GXSetCoPlanar(GX_FALSE);
  GXSetDstAlpha(GX_FALSE, 0);
  GXSetNumIndStages(0);
  GXSetTevDirect(GX_TEVSTAGE0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTevStages(1);
  GXSetNumTexGens(0);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXSetFog(fog, .5f, 4.5f, .1f, 5.f, {20, 120, 230, 255});
  // Range adjustment scales the fog by the distance from the centre column.
  static GXFogAdjTable table = [] {
    GXFogAdjTable result{};
    for (unsigned i = 0; i != 10; ++i) result.r[i] = static_cast<u16>(0x100 + i * 0x30);
    return result;
  }();
  GXSetFogRangeAdj(range ? GX_ENABLE : GX_DISABLE, Size / 2, &table);
}

// Depth grows from the left to the right edge, so each row spans the fog ramp.
struct Corner {
  float x, y, depth;
};
std::array<Corner, 4> QuadCorners(GXFogType fog, float top, float bottom) {
  constexpr float near = .6f, far = 4.4f;
  if (Orthographic(fog)) return {{{-1, bottom, near}, {1, bottom, far}, {1, top, far}, {-1, top, near}}};
  constexpr float tangent = .700207538f; // tan(35 degrees)
  return {{{-near * tangent, bottom * near * tangent, near}, {far * tangent, bottom * far * tangent, far},
           {far * tangent, top * far * tangent, far}, {-near * tangent, top * near * tangent, near}}};
}

void DirectQuad(GXFogType fog, float top, float bottom) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (const auto& corner : QuadCorners(fog, top, bottom)) {
    GXPosition3f32(corner.x, corner.y, -corner.depth);
    GXColor4u8(200, 60, 20, 255);
  }
  GXEnd();
}

// The same quad from indexed arrays: s16 positions (8 fractional bits) and
// RGB565 colours, stored little- or big-endian. The colour is constant, so a
// half quad samples like the full one; its two bytes differ, so order matters.
struct ArrayData {
  alignas(32) std::array<u8, 4 * 3 * 2> positions{};
  alignas(32) std::array<u8, 4 * 2> colors{};
};
// Native GX caches array data by address within a frame: every quad gets its own.
const ArrayData& MakeArrays(GXFogType fog, float top, float bottom, bool le) {
  static std::deque<ArrayData> arrays;
  auto& data = arrays.emplace_back();
  const auto store = [le](u8* to, u16 value) {
    to[le ? 0 : 1] = static_cast<u8>(value);
    to[le ? 1 : 0] = static_cast<u8>(value >> 8);
  };
  unsigned i = 0;
  for (const auto& corner : QuadCorners(fog, top, bottom)) {
    store(&data.positions[i * 6], static_cast<u16>(static_cast<s16>(corner.x * 256)));
    store(&data.positions[i * 6 + 2], static_cast<u16>(static_cast<s16>(corner.y * 256)));
    store(&data.positions[i * 6 + 4], static_cast<u16>(static_cast<s16>(-corner.depth * 256)));
    store(&data.colors[i * 2], static_cast<u16>((25u << 11) | (15u << 5) | 3u));
    ++i;
  }
  return data;
}
void IndexedQuad(const ArrayData& data, bool le) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
  GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGB, GX_RGB565, 0);
  GXSetArray(GX_VA_POS, data.positions.data(), data.positions.size(), 6, le);
  GXSetArray(GX_VA_CLR0, data.colors.data(), data.colors.size(), 2, le);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (u8 i = 0; i != 4; ++i) {
    GXPosition1x8(i);
    GXColor1x8(i);
  }
  GXEnd();
}

struct Sample {
  aurora::gx::PipelineConfig config{};
  std::vector<Row> rows;
};
template <typename Draw>
Sample Render(Draw&& draw, std::initializer_list<unsigned> rows) {
  Require(aurora::gfx::create_pass(Size, Size), "pass creation failed");
  draw();
  // The key of the last draw, from the state the FIFO worker has processed.
  GXFlush();
  aurora::gx::fifo::drain();
  Sample result;
  aurora::gx::populate_pipeline_config(result.config, GX_QUADS, GX_VTXFMT0);
  GXDrawDone();
  aurora::gfx::ResolvedTargets target;
  Require(aurora::gfx::resolve_pass({true, true, false}, target), "pass snapshot failed");
  GXDrawDone();
  for (const auto y : rows) result.rows.push_back(ReadRow(target.colorTexture, target.colorFormat, y));
  return result;
}

bool SameKey(const Sample& a, const Sample& b) { return std::memcmp(&a.config, &b.config, sizeof a.config) == 0; }
int Distance(const Row& a, const Row& b) {
  int distance = 0;
  for (unsigned x = 0; x != Size; ++x)
    for (unsigned c = 0; c != 3; ++c) distance = std::max(distance, std::abs(int(a[x][c]) - int(b[x][c])));
  return distance;
}

constexpr std::array FogTypes{GX_FOG_NONE,          GX_FOG_PERSP_LIN,   GX_FOG_PERSP_EXP, GX_FOG_PERSP_EXP2,
                              GX_FOG_PERSP_REVEXP,  GX_FOG_PERSP_REVEXP2, GX_FOG_ORTHO_LIN, GX_FOG_ORTHO_EXP,
                              GX_FOG_ORTHO_EXP2,    GX_FOG_ORTHO_REVEXP, GX_FOG_ORTHO_REVEXP2};
constexpr bool PerDrawState = aurora::gx::GXPipelineConfigVersion >= 17;

void Probe(std::vector<Row>& recorded) {
  const auto keep = [&](const Sample& sample) {
    recorded.insert(recorded.end(), sample.rows.begin(), sample.rows.end());
  };
  // Every fog type, with and without range adjustment, on one full quad.
  struct Fogged {
    GXFogType fog;
    Sample sample;
  };
  std::vector<Fogged> fogged;
  for (const auto fog : FogTypes) {
    for (const bool range : {false, true}) {
      if (fog == GX_FOG_NONE && range) continue;
      fogged.push_back({fog, Render([&] {
                          SetState(fog, range);
                          DirectQuad(fog, 1, -1);
                        }, {16, 48})});
      keep(fogged.back().sample);
    }
  }
  const auto& none = fogged.front().sample;
  const auto& ortho = std::find_if(fogged.begin(), fogged.end(), [](const auto& e) { return Orthographic(e.fog); })->sample;
  for (const auto& [fog, sample] : fogged) {
    if (fog == GX_FOG_NONE) continue;
    // The constant vertex colour must move towards the fog colour on the ramp.
    Require(Distance(sample.rows[0], none.rows[0]) > 8, "A fog type left the depth ramp unfogged");
    // One pipeline per projection serves every fog type and range setting.
    if (PerDrawState)
      Require(SameKey(sample, Orthographic(fog) ? ortho : none), "Fog type or range still selects a distinct pipeline");
  }
  if (!PerDrawState) Require(!SameKey(fogged[1].sample, none), "Pre-v17 fog type did not change the pipeline key");

  // Two fog types in one pass: each half matches its own full-quad pass.
  for (const auto [upper, lower] : std::array<std::pair<GXFogType, GXFogType>, 3>{
           {{GX_FOG_PERSP_EXP, GX_FOG_PERSP_LIN}, {GX_FOG_NONE, GX_FOG_PERSP_REVEXP2}, {GX_FOG_PERSP_EXP2, GX_FOG_NONE}}}) {
    const auto mixed = Render([&] {
      SetState(upper, false);
      DirectQuad(upper, 1, 0);
      SetState(lower, false);
      DirectQuad(lower, 0, -1);
    }, {16, 48});
    keep(mixed);
    const auto alone = [&](GXFogType fog) {
      return Render([&] {
        SetState(fog, false);
        DirectQuad(fog, 1, -1);
      }, {16, 48});
    };
    Require(Distance(mixed.rows[0], alone(upper).rows[0]) == 0 && Distance(mixed.rows[1], alone(lower).rows[1]) == 0,
            "A fog type change between two draws of one pass was not applied per draw");
  }

  // Little- and big-endian arrays draw identical pixels, also mixed in a pass.
  for (const auto fog : {GX_FOG_NONE, GX_FOG_PERSP_EXP, GX_FOG_ORTHO_LIN}) {
    std::array<Sample, 2> byOrder;
    for (const bool le : {false, true}) {
      const auto& data = MakeArrays(fog, 1, -1, le);
      byOrder[le] = Render([&] {
        SetState(fog, false);
        IndexedQuad(data, le);
      }, {16, 48});
      keep(byOrder[le]);
    }
    Require(Distance(byOrder[0].rows[0], byOrder[1].rows[0]) == 0 &&
            Distance(byOrder[0].rows[1], byOrder[1].rows[1]) == 0,
            "Little- and big-endian arrays drew different pixels");
    Require(SameKey(byOrder[0], byOrder[1]) == PerDrawState, "Array byte order selected the wrong pipeline keys");
    const auto& upper = MakeArrays(fog, 1, 0, true);
    const auto& lower = MakeArrays(fog, 0, -1, false);
    const auto mixed = Render([&] {
      SetState(fog, false);
      IndexedQuad(upper, true);
      IndexedQuad(lower, false);
    }, {16, 48});
    keep(mixed);
    Require(Distance(mixed.rows[0], byOrder[1].rows[0]) == 0 && Distance(mixed.rows[1], byOrder[0].rows[1]) == 0,
            "A byte order change between two draws of one pass was not applied per draw");
  }
  std::cout << "Fog and array byte order: " << fogged.size() << " fog states, per-draw changes and byte orders"
            << (PerDrawState ? " on one pipeline per state" : " (pre-v17 keys)") << '\n';
}

constexpr std::string_view Magic = "GXDRAWS1";
void Record(const std::filesystem::path& path, const std::vector<Row>& rows) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(Magic.data(), Magic.size());
  for (const auto& row : rows) output.write(reinterpret_cast<const char*>(row.data()), sizeof row);
  Require(bool(output), "Pixel record could not be written");
}
void Compare(const std::filesystem::path& path, const std::vector<Row>& rows) {
  std::ifstream input(path, std::ios::binary);
  std::array<char, Magic.size()> magic{};
  input.read(magic.data(), magic.size());
  Require(bool(input) && std::string_view(magic.data(), magic.size()) == Magic, "Pixel record header mismatch");
  unsigned differing = 0;
  int distance = 0;
  for (const auto& row : rows) {
    Row expected{};
    input.read(reinterpret_cast<char*>(expected.data()), sizeof expected);
    Require(bool(input), "Pixel record is shorter than this run");
    const int d = Distance(row, expected);
    differing += d != 0;
    distance = std::max(distance, d);
  }
  Require(input.peek() == std::char_traits<char>::eof(), "Pixel record is longer than this run");
  std::cout << "Compared " << rows.size() << " rows with the record: " << differing
            << " differ, largest channel difference " << distance << '\n';
  Require(differing == 0, "Pixels differ from the recorded run");
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
        ("charged-gx-draw-state-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path record, compare;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      const auto path = [&] { return std::filesystem::path(reinterpret_cast<const char8_t*>(argv[++i])); };
      if (arg == "--directory" && i + 1 < argc) directory = path();
      else if (arg == "--record" && i + 1 < argc) record = path();
      else if (arg == "--compare" && i + 1 < argc) compare = path();
      else if (arg == "--backend" && i + 1 < argc) {
        const std::string_view name = argv[++i];
        if (name == "vulkan") backend = BACKEND_VULKAN;
        else if (name == "dx12") backend = BACKEND_D3D12;
        else if (name == "metal") backend = BACKEND_METAL;
        else throw std::runtime_error("Unknown backend");
      } else {
        throw std::runtime_error("Usage: gx_draw_state_gpu_tests [--backend vulkan|dx12|metal] [--directory NEW_PATH] "
                                 "[--record FILE | --compare FILE]");
      }
    }
    Require(!std::filesystem::exists(directory), "Fixture directory must be new (preserves existing caches)");
    std::filesystem::create_directories(directory);
    const auto path = Utf8(directory);
    AuroraConfig config{};
    config.appName = "Charged GX draw state qualification";
    config.userPath = config.cachePath = config.resourcesPath = path.c_str();
    config.desiredBackend = backend;
    config.msaa = 1;
    config.windowWidth = 320;
    config.windowHeight = 240;
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
            "CPU adapter cannot qualify GX draw state");
    OSInit();
    VIInit();
    VIConfigure(&GXNtsc480IntDf);
    alignas(32) static std::array<u8, 65536> fifo{};
    GXInit(fifo.data(), fifo.size());
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    GXSetCopyClear({0, 0, 0, 255}, GX_MAX_Z24);
    aurora_update();
    Require(aurora_begin_frame(), "Fixture frame unavailable");
    std::vector<Row> rows;
    Probe(rows);
    aurora_end_frame_no_present();
    aurora::gfx::synchronize();
    aurora_shutdown();
    live = false;
    Require(logErrors == 0, "GPU fixture reported a native error");
    if (!record.empty()) Record(record, rows);
    if (!compare.empty()) Compare(compare, rows);
    std::cout << "GX draw state GPU passed: " << checks << " checks; evidence: " << path << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "GX draw state GPU failure: " << e.what() << '\n';
    if (live) aurora_shutdown();
    return 1;
  }
}

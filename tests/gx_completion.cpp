// Generated hardware qualification only; no game manager or scene logic.
#include "lib/gx/fifo.hpp"
#include "lib/webgpu/gpu.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <chrono>
#include <cstring>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/os/OSContext.h>
#include <dolphin/vi.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
unsigned checks = 0;
void Require(bool yes, const char *text) {
  ++checks;
  if (!yes)
    throw std::runtime_error(text);
}
std::atomic_uint callbacks{}, callback_errors{}, log_errors{}, submits{};
std::thread::id owner;
OSContext *prior_context{};
void Log(AuroraLogLevel level, const char *module, const char *text,
         unsigned n) {
  if (level >= LOG_ERROR)
    ++log_errors;
  std::cerr << '[' << module << "] " << std::string_view(text, n) << '\n';
}
void Callback() {
  if (std::this_thread::get_id() != owner)
    ++callback_errors;
  if (OSGetCurrentContext() == prior_context ||
      OSGetCurrentContext() == nullptr)
    ++callback_errors;
  const BOOL mask = OSDisableInterrupts();
  if (mask)
    ++callback_errors;
  OSRestoreInterrupts(mask);
  // A callback's clock query must not recursively dispatch another masked IRQ.
  (void)OSGetTick();
  ++callbacks;
}
void CallbackReplacement() { Callback(); }
void SpinForCallback(unsigned expected) {
  const auto start = OSGetTick();
  while (callbacks.load() < expected) {
    (void)OSGetTick(); // Actual unchanged source glx time-only wait boundary.
    if (OSTicksToSeconds(static_cast<u32>(OSGetTick() - start)) >= 5)
      throw std::runtime_error("time-only owner interrupt spin expired");
  }
  Require(callbacks.load() == expected, "unexpected PE callback count");
  Require(callback_errors.load() == 0,
          "callback ran on wrong thread/mask/context");
  Require(OSGetCurrentContext() == prior_context,
          "PE context was not restored");
  const BOOL mask = OSDisableInterrupts();
  Require(mask, "PE interrupt exclusion was not restored");
  OSRestoreInterrupts(mask);
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
void Rect(float left, float right, float z, GXColor color) {
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (auto p : std::array<std::array<float, 2>, 4>{
           {{left, -.75f}, {right, -.75f}, {right, .75f}, {left, .75f}}}) {
    GXPosition3f32(p[0], p[1], -z);
    GXColor4u8(color.r, color.g, color.b, color.a);
  }
  GXEnd();
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
void Pixel(const wgpu::Texture &texture, wgpu::TextureFormat format, unsigned x,
           unsigned y, std::array<u8, 3> expected, const char *label) {
  auto got = ReadTexture(texture, format, x, y);
  if (!std::equal(expected.begin(), expected.end(), got.begin())) {
    std::cerr << label << ": " << unsigned(got[0]) << ',' << unsigned(got[1])
              << ',' << unsigned(got[2]) << " expected "
              << unsigned(expected[0]) << ',' << unsigned(expected[1]) << ','
              << unsigned(expected[2]) << '\n';
    throw std::runtime_error(label);
  }
  ++checks;
}
struct Payload {
  aurora::gfx::Range vertices, indices;
};
struct NativeDraw {
  wgpu::RenderPipeline pipeline;
};
void EncodeDraw(const aurora::gfx::DrawContext &ctx,
                const wgpu::RenderPassEncoder &pass, const void *raw,
                size_t size, void *user) {
  if (size != sizeof(Payload))
    throw std::runtime_error("hardware proof payload ABI");
  const auto &data = *static_cast<const Payload *>(raw);
  pass.SetPipeline(static_cast<NativeDraw *>(user)->pipeline);
  pass.SetVertexBuffer(0, ctx.vertexBuffer, data.vertices.offset, 4);
  pass.SetIndexBuffer(ctx.indexBuffer, wgpu::IndexFormat::Uint16,
                      data.indices.offset, 6);
  pass.DrawIndexed(3);
}
wgpu::RenderPipeline MakePipeline() {
  using namespace aurora::webgpu;
  const char *shader =
      R"(@vertex fn v(@location(0) color:vec4f,@builtin(vertex_index) i:u32)->VOut {
 var points=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));var o:VOut;o.position=vec4f(points[i],0.5,1);o.color=color;return o;}
 struct VOut{@builtin(position) position:vec4f,@location(0) color:vec4f};
 @fragment fn f(in:VOut)->@location(0) vec4f{return in.color;})";
  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = shader;
  const wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &wgsl};
  auto module = g_device.CreateShaderModule(&moduleDesc);
  const wgpu::VertexAttribute attr{
      .format = wgpu::VertexFormat::Unorm8x4, .offset = 0, .shaderLocation = 0};
  wgpu::VertexBufferLayout buffer;
  buffer.arrayStride = 0;
  buffer.stepMode = wgpu::VertexStepMode::Vertex;
  buffer.attributeCount = 1;
  buffer.attributes = &attr;
  const wgpu::ColorTargetState target{
      .format = g_graphicsConfig.surfaceConfiguration.format};
  const wgpu::FragmentState fragment{.module = module,
                                     .entryPoint = "f",
                                     .targetCount = 1,
                                     .targets = &target};
  const wgpu::DepthStencilState depth{.format = g_graphicsConfig.depthFormat,
                                      .depthWriteEnabled = false,
                                      .depthCompare =
                                          wgpu::CompareFunction::Always};
  const wgpu::RenderPipelineDescriptor pipeline{.vertex = {.module = module,
                                                           .entryPoint = "v",
                                                           .bufferCount = 1,
                                                           .buffers = &buffer},
                                                .depthStencil = &depth,
                                                .fragment = &fragment};
  return g_device.CreateRenderPipeline(&pipeline);
}
void AfterSubmit(const aurora::gfx::EncoderTaskCompletionContext &,
                 const void *, size_t, void *) {
  ++submits;
}
void NoCommands(const aurora::gfx::EncoderTaskContext &,
                const wgpu::CommandEncoder &, const void *, size_t, void *) {}
} // namespace
int main(int argc, char **argv) {
  bool live = false;
  try {
    owner = std::this_thread::get_id();
    const char *base_path = SDL_GetBasePath();
    if (!base_path)
      throw std::runtime_error("executable path unavailable");
    const std::string base = base_path;
    const auto directory = std::filesystem::path(base) / "gx-completion-data";
    std::filesystem::create_directories(directory);
    const auto path = directory.string();
    AuroraConfig config{};
    config.appName = "GX completion qualification";
    config.userPath = config.cachePath = path.c_str();
    config.resourcesPath = base.c_str();
    config.desiredBackend = BACKEND_VULKAN;
    config.msaa = 1;
    config.windowWidth = 800;
    config.windowHeight = 600;
    config.windowPosX = config.windowPosY = -1;
    config.vsync = false;
    config.enableBackendValidation = true;
    config.mem1Size = MEM1_DEFAULT_SIZE;
    config.logCallback = Log;
    config.logLevel = LOG_INFO;
    auto info = aurora_initialize(argc, argv, &config);
    live = true;
    Require(info.backend == BACKEND_VULKAN && info.window,
            "real Vulkan required");
    OSInit();
    VIInit();
    VIConfigure(&GXNtsc480IntDf);
    alignas(32) std::array<u8, 65536> fifo{};
    GXInit(fifo.data(), fifo.size());
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    GXSetCopyClear({0, 0, 0, 255}, GX_MAX_Z24);
    prior_context = OSGetCurrentContext();
    Require(GXSetDrawDoneCallback(Callback) == nullptr,
            "unexpected previous callback");
    Require(GXSetDrawDoneCallback(CallbackReplacement) == Callback,
            "callback replacement did not return previous");
    Require(GXSetDrawDoneCallback(Callback) == CallbackReplacement,
            "replacement restore failed");
    // Startup finish marker and hardware masking; CPU drain is only fixture
    // coordination and does not establish its own physical completion result.
    const auto masked = OSDisableInterrupts();
    GXSetDrawDone();
    aurora::gx::fifo::drain();
    Require(callbacks.load() == 0, "callback fired while owner masked");
    Require(!AuroraServiceGXDrawDone(), "masked owner serviced PE");
    (void)OSGetTick();
    Require(callbacks.load() == 0, "masked clock serviced PE");
    std::atomic_bool foreign{true};
    std::thread worker([&] {
      foreign = AuroraServiceGXDrawDone();
      (void)OSGetTick();
    });
    worker.join();
    Require(!foreign && callbacks.load() == 0, "foreign thread serviced PE");
    OSRestoreInterrupts(masked);
    SpinForCallback(1);
    NativeDraw native{MakePipeline()};
    auto type = aurora::gfx::register_draw_type(
        {"unaligned prefix proof", EncodeDraw, &native});
    auto task = aurora::gfx::register_encoder_task_type(
        {"segment ownership", NoCommands, nullptr, AfterSubmit});
    // Resolve asynchronous source GX pipelines before the actual colour gate.
    for (unsigned warm = 0; warm < 12; ++warm) {
      aurora_update();
      if (!aurora_begin_frame()) {
        --warm;
        SDL_Delay(1);
        continue;
      }
      SetState(640, 480);
      Rect(-.9f, -.3f, .2f, {255, 0, 0, 255});
      Rect(.3f, .9f, .4f, {0, 0, 255, 255});
      GXDrawDone();
      aurora_end_frame();
    }
    callbacks = 0;
    Require(aurora_begin_frame(), "no active proof frame");
    const auto frame = aurora::gfx::current_frame();
    aurora::gx::fifo::drain();
    // Odd high-water marks in both streams: segment2 copies a rounded range
    // starting before the previously submitted3-byte prefix. Its original bytes
    // must survive Unmap/Map, not merely its logical cursor value.
    const std::array<u8, 3> firstV{255, 0, 255}, firstI{0, 0, 1};
    auto v = aurora::gfx::push_verts(firstV.data(), 3, 1);
    auto i = aurora::gfx::push_indices(firstI.data(), 3, 1);
    Require(v.offset % 4 == 0 && i.offset % 4 == 0, "fixture prefix alignment");
    GXSetDrawDone();
    SpinForCallback(1);
    Require(aurora::gfx::current_frame() == frame,
            "finish advanced/reset frame index");
    const u8 alpha = 255;
    const std::array<u8, 3> restI{0, 2, 0};
    auto v2 = aurora::gfx::push_verts(&alpha, 1, 1);
    auto i2 = aurora::gfx::push_indices(restI.data(), 3, 1);
    Require(v2.offset == v.offset + 3 && i2.offset == i.offset + 3,
            "finish lost original staging high-water marks");
    Payload payload{{v.offset, 4}, {i.offset, 6}};
    Require(aurora::gfx::push_custom_draw(type, &payload, sizeof(payload)),
            "custom prefix draw rejected");
    Require(aurora::gfx::push_encoder_task(task, nullptr, 0),
            "encoder task rejected");
    GXSetDrawDone();
    SpinForCallback(2);
    Require(submits == 1, "after-submit ownership/order incorrect");
    using namespace aurora::webgpu;
    Pixel(g_frameBuffer.texture, g_frameBuffer.format,
          g_frameBuffer.size.width / 2, g_frameBuffer.size.height / 2,
          {255, 0, 255}, "unaligned GPU stream prefix lost");
    SetState(640, 480);
    Rect(-.9f, -.3f, .2f, {255, 0, 0, 255});
    GXSetDrawDone();
    SpinForCallback(3);
    Pixel(g_frameBuffer.texture, g_frameBuffer.format,
          g_frameBuffer.size.width / 5, g_frameBuffer.size.height / 2,
          {255, 0, 0}, "pre-marker source draws missing");
    Rect(.3f, .9f, .4f, {0, 0, 255, 255});
    GXSetDrawDone();
    SpinForCallback(4);
    Pixel(g_frameBuffer.texture, g_frameBuffer.format,
          g_frameBuffer.size.width / 5, g_frameBuffer.size.height / 2,
          {255, 0, 0}, "continuation cleared earlier EFB");
    Pixel(g_frameBuffer.texture, g_frameBuffer.format,
          4 * g_frameBuffer.size.width / 5, g_frameBuffer.size.height / 2,
          {0, 0, 255}, "post-marker source draw missing");
    // Genuine offscreen continuation and snapshot consumer, then parent
    // restore.
    Require(aurora::gfx::create_pass(64, 64), "offscreen pass failed");
    SetState(64, 64);
    Rect(-.9f, -.3f, .2f, {255, 0, 0, 255});
    GXSetDrawDone();
    SpinForCallback(5);
    Rect(.3f, .9f, .4f, {0, 0, 255, 255});
    GXDrawDone();
    Require(callbacks == 6, "offscreen completion missing");
    aurora::gfx::ResolvedTargets snapshot;
    Require(aurora::gfx::resolve_pass({true, true, false}, snapshot),
            "offscreen snapshot failed");
    GXDrawDone();
    Require(callbacks == 7, "snapshot completion missing");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 12, 32, {255, 0, 0},
          "offscreen continuation lost first draw");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 51, 32, {0, 0, 255},
          "offscreen continuation lost second draw");
    Pixel(g_frameBuffer.texture, g_frameBuffer.format,
          g_frameBuffer.size.width / 5, g_frameBuffer.size.height / 2,
          {255, 0, 0}, "offscreen restore lost parent EFB");
    Require(aurora::gfx::current_frame() == frame,
            "midframe completion changed frame identity");
    aurora_end_frame();
    Require(aurora::gfx::current_frame() == frame + 1,
            "source frame end did not advance once");
    snapshot = {};
    aurora::gfx::synchronize();
    aurora::gfx::unregister_encoder_task_type(task);
    aurora::gfx::unregister_draw_type(type);
    native.pipeline = nullptr;
    if (argc > 1 && std::string_view(argv[1]) == "--device-loss") {
      // Physical native Dawn loss must never be converted to PE success.
      std::cout << "Forcing device loss after " << checks
                << " successful checks\n"
                << std::flush;
      callbacks = 0;
      g_device.ForceLoss(wgpu::DeviceLostReason::Unknown,
                         "GX completion failure qualification");
      GXSetDrawDone();
      GXWaitDrawDone();
      throw std::runtime_error("lost device incorrectly reported completion");
    }
    // Pending genuine idle marker is drained and delivered before GX services
    // die.
    auto before = callbacks.load();
    GXSetDrawDone();
    aurora_shutdown();
    live = false;
    Require(callbacks.load() == before + 1,
            "shutdown lost pending finish interrupt");
    Require(!callback_errors && !log_errors, "hardware callback/log failure");
    std::cout
        << "GX physical completion probe passed: " << checks
        << " checks; owner/mask/context, odd staging streams, seven barriers, "
           "EFB/offscreen readback, frame identity and stop/drain\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "GX completion probe failed: " << e.what() << '\n';
    if (live)
      aurora_shutdown();
    return 1;
  }
}

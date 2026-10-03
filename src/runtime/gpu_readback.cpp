#include "runtime/gpu_readback.h"
#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace mscharged
{
namespace
{
constexpr unsigned stride = 256, size = stride * 9;
struct Readback : std::enable_shared_from_this<Readback>
{
    aurora::gfx::ResolvedTargets target;
    wgpu::Buffer buffer;
    ColourSamples colours{};
    std::atomic_int done = 0;
    bool bgra = false;
    static void Copy(const aurora::gfx::EncoderTaskContext &ctx, const wgpu::CommandEncoder &cmd, const void *,
                     std::size_t, void *data)
    {
        auto &read = *static_cast<Readback *>(data);
        const wgpu::BufferDescriptor descriptor{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                                .size = size};
        read.buffer = ctx.device.CreateBuffer(&descriptor);
        for (unsigned i = 0; i < 9; ++i)
        {
            const wgpu::TexelCopyTextureInfo source{
                .texture = read.target.colorTexture,
                .origin = {read.target.width * (i % 3 + 1) / 4, read.target.height * (i / 3 + 1) / 4, 0}};
            const wgpu::TexelCopyBufferInfo dest{
                .layout = {.offset = i * stride, .bytesPerRow = stride, .rowsPerImage = 1}, .buffer = read.buffer};
            const wgpu::Extent3D extent{1, 1, 1};
            cmd.CopyTextureToBuffer(&source, &dest, &extent);
        }
    }
    static void Map(const aurora::gfx::EncoderTaskCompletionContext &, const void *, std::size_t, void *data)
    {
        // A timeout or shutdown must not destroy storage retained by the callback.
        auto read = static_cast<Readback *>(data)->shared_from_this();
        read->buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::AllowSpontaneous,
                              [read](wgpu::MapAsyncStatus status, wgpu::StringView)
                              {
                                  if (status != wgpu::MapAsyncStatus::Success)
                                  {
                                      read->done = -1;
                                      return;
                                  }
                                  const auto *bytes =
                                      static_cast<const std::uint8_t *>(read->buffer.GetConstMappedRange(0, size));
                                  for (unsigned i = 0; i < 9; ++i)
                                  {
                                      auto &colour = read->colours[i];
                                      std::copy_n(bytes + i * stride, 4, colour.begin());
                                      if (read->bgra)
                                          std::swap(colour[0], colour[2]);
                                  }
                                  read->buffer.Unmap();
                                  read->done = 1;
                              });
    }
};
} // namespace
ColourSamples EndFrameAndReadColours(const std::function<void()>& end_frame)
{
    auto read = std::make_shared<Readback>();
    if (!aurora::gfx::resolve_pass({}, read->target))
        throw std::runtime_error("Cannot snapshot diagnostic EFB");
    read->bgra = read->target.colorFormat == wgpu::TextureFormat::BGRA8Unorm;
    if (!read->bgra && read->target.colorFormat != wgpu::TextureFormat::RGBA8Unorm)
        throw std::runtime_error("Unsupported diagnostic colour target format");
    const aurora::gfx::EncoderTaskDescriptor descriptor{"Diagnostic colour readback", Readback::Copy, read.get(),
                                                        Readback::Map};
    const auto task = aurora::gfx::register_encoder_task_type(descriptor);
    const bool queued = aurora::gfx::push_encoder_task(task, nullptr, 0);
    if (end_frame) end_frame();
    else aurora_end_frame();
    aurora::gfx::synchronize();
    aurora::gfx::unregister_encoder_task_type(task);
    if (!queued)
        throw std::runtime_error("Cannot enqueue diagnostic colour readback");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!read->done && std::chrono::steady_clock::now() < deadline)
    {
        aurora::gfx::device().Tick();
        SDL_Delay(1);
    }
    if (read->done != 1)
        throw std::runtime_error("Diagnostic colour readback failed or timed out");
    return read->colours;
}
} // namespace mscharged

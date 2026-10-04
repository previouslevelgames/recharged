#include "runtime/particle_files.h"
#include "runtime/whole_file.h"
#include "resources/compressed_asset.h"
#include "NL/MemAlloc.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include <algorithm>
#include <exception>
#include <thread>

namespace mscharged
{
namespace
{
// EmissionManager.cpp::StartLoading, fourth argument true (original FE boot).
constexpr const char* paths[]{"art/effects/effects.bun", "art/effects/effectsNonRes.bun.zlib",
    "art/objects/effectsgeometry.bun", "art/objects/effectsgeometrytextures.rlt"};
struct FreeBuffer { void operator()(void* data) const noexcept { nlFree(data); } };
}
struct ParticleFileLoad::Implementation
{
    struct Request { Implementation* owner; unsigned index, token = 0; bool finished = false, valid = false; };
    const std::thread::id thread = std::this_thread::get_id();
    std::array<Request, 4> requests{{{this, 0}, {this, 1}, {this, 2}, {this, 3}}};
    std::shared_ptr<ParticleFiles> files = std::make_shared<ParticleFiles>();
    std::exception_ptr error;
    ParticleFileState state = ParticleFileState::Loading;
    std::size_t retained_bytes = 0;

    void CheckThread() const
    {
        if (std::this_thread::get_id() != thread)
            throw std::logic_error("Particle files require their NL servicing thread");
    }
    void Drain()
    {
        for (auto& request : requests)
        {
            if (request.token) nlCancelEntireFileLoad(request.token, nullptr);
            request.token = 0;
        }
    }
    void Start()
    {
        if (!gMemoryInitialized || !nlFileSystemReady())
            throw std::logic_error("Particle files require initialized memory and NL files");
        std::size_t total = 0;
        // Reject missing or oversized batches before submitting any callback.
        for (unsigned i = 0; i < requests.size(); ++i)
        {
            std::unique_ptr<nlFile> file(nlOpen(paths[i]));
            if (!file) throw std::runtime_error(std::string("Particle file is missing: ") + paths[i]);
            const auto size = nlFileSize(file.get(), nullptr);
            if (!size || size > resources::MaximumAssetBytes || size > MaximumRetainedBytes - total)
                throw std::length_error(std::string("Particle file exceeds its batch size limits: ") + paths[i]);
            total += size; files->source_sizes[i] = size;
        }
        try
        {
            for (auto& request : requests)
            {
                request.token = nlLoadEntireFileAsync(paths[request.index], Complete, &request,
                    32, AllocateEnd, nullptr, 0, &VirtualAllocator);
                if (!request.token && !request.finished)
                    throw std::runtime_error("Particle file read was not queued");
                if (error) std::rethrow_exception(error);
            }
        }
        catch (...) { Drain(); throw; }
    }
    static void Complete(void* data, unsigned long size, void* context)
    {
        std::unique_ptr<void, FreeBuffer> buffer(data);
        auto& request = *static_cast<Request*>(context);
        auto& load = *request.owner;
        load.CheckThread(); request.token = 0; request.finished = true;
        if (load.error) return;
        try
        {
            if (size != load.files->source_sizes[request.index])
                throw std::runtime_error("Particle file changed size during its read");
            const resources::Bytes bytes{static_cast<const std::uint8_t*>(data), size};
            const bool compressed = request.index == static_cast<unsigned>(ParticleFileKind::NonResident);
            const std::size_t decoded_size = compressed ? resources::U32(bytes, 0) : size;
            if (!decoded_size || decoded_size > MaximumRetainedBytes - load.retained_bytes)
                throw std::length_error("Decoded particle files exceed their batch size limit");
            auto& result = load.files->data[request.index];
            if (compressed) result = resources::InflateAsset(bytes);
            else result.assign(bytes.begin(), bytes.end());
            load.retained_bytes += result.size(); request.valid = true;
        }
        catch (...) { load.error = std::current_exception(); }
    }
};
ParticleFileLoad::ParticleFileLoad() : impl_(std::make_unique<Implementation>()) { impl_->Start(); }
ParticleFileLoad::~ParticleFileLoad()
{ try { Cancel(); } catch (...) { std::terminate(); } }
void ParticleFileLoad::Poll()
{
    auto& s = *impl_; s.CheckThread();
    if (s.state != ParticleFileState::Loading) return;
    if (!s.error)
        for (const auto& r : s.requests)
            if (!r.finished && !WholeFileLoadPending(r.token))
            {
                s.error = std::make_exception_ptr(std::runtime_error(
                    std::string("Particle read failed or file services stopped: ") + paths[r.index]));
                break;
            }
    if (s.error)
    {
        s.Drain(); s.files.reset(); s.state = ParticleFileState::Failed;
    }
    else if (std::all_of(s.requests.begin(), s.requests.end(), [](const auto& r) { return r.valid; }))
        s.state = ParticleFileState::Ready;
}
void ParticleFileLoad::Service()
{
    Poll(); if (impl_->state != ParticleFileState::Loading) return;
    try { nlServiceFileSystem(); } catch (...) { Poll(); throw; }
    Poll();
}
void ParticleFileLoad::Cancel()
{
    Poll(); auto& s = *impl_;
    if (s.state != ParticleFileState::Loading) return;
    s.Drain(); s.files.reset(); s.state = ParticleFileState::Cancelled;
}
ParticleFileState ParticleFileLoad::State() const { impl_->CheckThread(); return impl_->state; }
unsigned ParticleFileLoad::CompletedFiles() const
{
    impl_->CheckThread();
    return std::count_if(impl_->requests.begin(), impl_->requests.end(), [](const auto& r) { return r.valid; });
}
std::shared_ptr<const ParticleFiles> ParticleFileLoad::Result() const
{
    impl_->CheckThread();
    if (impl_->error) std::rethrow_exception(impl_->error);
    if (impl_->state != ParticleFileState::Ready) throw std::logic_error("Particle files are pending or cancelled");
    return impl_->files;
}
}

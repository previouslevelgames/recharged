#include "runtime/audio_bank_load.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <array>
#include <exception>
#include <thread>

namespace mscharged
{
namespace
{
struct FreeBuffer
{
    void operator()(std::uint8_t* data) const noexcept
    { if (data) VirtualAllocator.Free(data); }
};
}
struct AudioBankLoad::Implementation
{
    struct Read
    {
        Implementation* owner = nullptr;
        std::unique_ptr<nlFile> file;
        std::unique_ptr<std::uint8_t, FreeBuffer> bytes;
        unsigned size = 0;
        bool issued = false, complete = false;
        resources::Bytes Bytes() const { return {bytes.get(), size}; }
    };
    std::array<Read, 2> reads;
    LoadedAudioBank identity;
    LoadedAudioBank::Handle result;
    AudioBankLoadState state = AudioBankLoadState::Loading;
    AudioBankLoadProgress progress;
    std::exception_ptr error;
    const std::thread::id thread = std::this_thread::get_id();
    bool servicing = false;

    void Thread() const
    {
        if (thread != std::this_thread::get_id())
            throw std::logic_error("Audio bank requires its NL servicing thread");
    }
    void Mutable() const
    {
        Thread();
        if (servicing || nlGetCurrentAsyncRead())
            throw std::logic_error("Audio bank mutation during an NL callback is unsupported");
    }
    void Drain()
    {
        for (auto& read : reads)
            if (read.file) nlCancelPendingAsyncReads(read.file.get(), nullptr);
        // Workers have joined before destinations or callback contexts expire.
        for (auto& read : reads) { read.file.reset(); read.bytes.reset(); }
    }
    static void Complete(nlFile* file, void* data, unsigned size, nlFileAsyncParam context)
    {
        auto& read = *reinterpret_cast<Read*>(context);
        auto& owner = *read.owner;
        try
        {
            owner.Thread();
            resources::Require(read.issued && !read.complete && read.file.get() == file
                && read.bytes.get() == data && read.size == size, "Audio bank callback identity differs");
            read.complete = true;
            ++owner.progress.completed_reads;
        }
        catch (...) { if (!owner.error) owner.error = std::current_exception(); }
    }
    void Issue(Read& read)
    {
        read.bytes.reset(static_cast<std::uint8_t*>(VirtualAllocator.Allocate(read.size, 32, false)));
        read.issued = true;
        ++progress.requested_reads;
        auto* token = nlReadAsync(read.file.get(), read.bytes.get(), read.size, Complete,
                                 reinterpret_cast<nlFileAsyncParam>(&read), read.size);
        if (!token && !read.complete) throw std::runtime_error("Audio bank read was not queued");
    }
    Implementation(resources::AudioBankCatalog::Handle catalog, std::uint32_t name, std::uint32_t slot)
    {
        Mutable();
        if (!gMemoryInitialized || !nlFileSystemReady())
            throw std::logic_error("Audio bank requires native memory and NL files");
        resources::Require(catalog && name < catalog->names.size() && slot < catalog->slots.size(),
                           "Audio bank name or slot index is absent");
        // Snapshot the records: callers may retain a mutable alias of a catalog.
        identity = {name, slot, catalog->names[name], catalog->slots[slot], {}};
        if (identity.slot.streaming) throw resources::UnsupportedResource("Streamed audio bank loading is unavailable");
        const auto& bank_name = identity.name.name;
        resources::Require(!bank_name.empty() && bank_name.size() <= 112
            && bank_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos,
            "Invalid audio bank resource name");
        try
        {
            // Preflight both exact files before submitting any asynchronous IO.
            const std::array<std::string, 2> paths{
                "audio/" + bank_name + ".resbun", "audio/" + bank_name + ".nlxwb"};
            for (unsigned i = 0; i < reads.size(); ++i)
            {
                auto& read = reads[i];
                read.owner = this;
                read.file.reset(nlOpen(paths[i].c_str()));
                if (!read.file) throw std::runtime_error("Audio bank file is missing: " + paths[i]);
                read.size = nlFileSize(read.file.get(), nullptr);
                resources::Require(read.size && read.size <= (i ? 64 * 1024 * 1024 : resources::MaximumAssetBytes),
                                   "Audio bank file is empty or exceeds its size limit");
            }
            Issue(reads[0]);
        }
        catch (...) { Drain(); throw; }
    }
    void Fail(std::exception_ptr failure)
    {
        if (!error) error = failure;
        Drain();
        state = AudioBankLoadState::Failed;
    }
    void Poll()
    {
        if (state != AudioBankLoadState::Loading) return;
        try
        {
            if (error) std::rethrow_exception(error);
            if (!nlFileSystemReady()) throw std::runtime_error("Audio bank file services stopped");
            for (const auto& read : reads)
                if (read.issued && !read.complete && !nlAsyncReadsPending(read.file.get()))
                    throw std::runtime_error("Audio bank read failed or was cancelled");
            if (!reads[0].complete) return;
            if (!reads[1].issued) { Issue(reads[1]); return; }
            if (!reads[1].complete) return;
            auto next = std::make_shared<LoadedAudioBank>(identity);
            next->bank = resources::ReadAudioResidentBank(reads[0].Bytes(), reads[1].Bytes());
            Drain();
            result = std::move(next);
            state = AudioBankLoadState::Ready;
        }
        catch (...) { Fail(std::current_exception()); }
    }
};
AudioBankLoad::AudioBankLoad(resources::AudioBankCatalog::Handle catalog, std::uint32_t name, std::uint32_t slot)
    : impl_(std::make_unique<Implementation>(std::move(catalog), name, slot)) {}
AudioBankLoad::~AudioBankLoad() { try { Cancel(); } catch (...) { std::terminate(); } }
void AudioBankLoad::Poll() { impl_->Mutable(); impl_->Poll(); }
void AudioBankLoad::Service()
{
    auto& s = *impl_;
    s.Mutable(); s.Poll();
    if (s.state != AudioBankLoadState::Loading) return;
    s.servicing = true;
    try { nlServiceFileSystem(); }
    catch (...) { s.servicing = false; s.Fail(std::current_exception()); throw; }
    s.servicing = false;
    s.Poll();
}
void AudioBankLoad::Cancel()
{
    auto& s = *impl_; s.Mutable();
    if (s.state != AudioBankLoadState::Loading) return;
    s.Drain(); s.state = AudioBankLoadState::Cancelled;
}
AudioBankLoadState AudioBankLoad::State() const { impl_->Thread(); return impl_->state; }
AudioBankLoadProgress AudioBankLoad::Progress() const { impl_->Thread(); return impl_->progress; }
LoadedAudioBank::Handle AudioBankLoad::Result() const
{
    impl_->Thread();
    if (impl_->error) std::rethrow_exception(impl_->error);
    if (impl_->state != AudioBankLoadState::Ready) throw std::logic_error("Audio bank is pending or cancelled");
    return impl_->result;
}
}

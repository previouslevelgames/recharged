#if defined(MSCHARGED_GAME_MODULE)
#error "The original game module must compile the complete original NL/nlFile.cpp"
#endif

#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include "runtime/whole_file.h"
#include "NL/nlMemory.h"
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>

namespace
{
using CancelCallback = void (*)(void*, unsigned long, void*, LoadAsyncCallback);

struct Load
{
    std::unique_ptr<nlFile> file;
    void* buffer = nullptr;
    bool ownsBuffer = false;
    unsigned long size = 0;
    LoadAsyncCallback callback = nullptr;
    CancelCallback cancel = nullptr;
    void* user = nullptr;
    AsyncEntry* read = nullptr;
    unsigned int handle = 0;

    ~Load()
    {
        // Closing the file joins all DVD workers before freeing their destination.
        file.reset();
        if (ownsBuffer) nlFree(buffer);
    }
};

std::map<unsigned int, std::unique_ptr<Load>> loads;
std::uint64_t nextHandle = 1;
static_assert(std::numeric_limits<unsigned int>::digits == 32);

std::unique_ptr<Load> Take(nlFileAsyncParam context)
{
    auto* load = reinterpret_cast<Load*>(context);
    auto found = loads.find(load->handle);
    if (found == loads.end() || found->second.get() != load)
        throw std::logic_error("NL whole-file callback has no pending request");
    auto node = loads.extract(found);
    return std::move(node.mapped());
}

void Complete(nlFile*, void*, unsigned int, nlFileAsyncParam context)
{
    auto load = Take(context); // Detach before user code can queue/cancel/service.
    // Ownership transfers at callback entry, even if it frees the buffer and throws.
    load->ownsBuffer = false;
    load->callback(load->buffer, load->size, load->user);
}

void Cancel(nlFile*, void*, unsigned int, nlFileAsyncParam context, ReadAsyncCallback)
{
    auto load = Take(context);
    if (load->cancel) load->cancel(load->buffer, load->size, load->user, load->callback);
    // The cancel callback only borrows the buffer. The record frees owned data;
    // a caller-supplied buffer remains the caller's responsibility throughout.
}
}

bool mscharged::WholeFileLoadPending(unsigned handle) { return loads.find(handle) != loads.end(); }

unsigned int nlLoadEntireFileAsync(const char* filename, LoadAsyncCallback callback,
    void* user, unsigned int alignment, eAllocType type, void* buffer,
    unsigned long capacity, MemoryAllocator* allocator)
{
    if (!nlFileSystemReady()) throw std::logic_error("NL file system is not initialized");
    if (!callback) throw std::invalid_argument("NL whole-file load requires a completion callback");
    auto load = std::make_unique<Load>();
    load->file.reset(nlOpen(filename));
    if (!load->file) return 0;
    unsigned int padded = 0;
    load->size = nlFileSize(load->file.get(), &padded);
    if (!load->size)
    {
        load->file.reset();
        callback(nullptr, 0, user); // Preserve the original inline empty-file case.
        return 0;
    }
    if (nextHandle > std::numeric_limits<unsigned int>::max())
        throw std::overflow_error("NL whole-file request handles are exhausted");
    load->callback = callback;
    load->user = user;
    if (buffer)
    {
        if (capacity && capacity < load->size)
            throw std::length_error("NL whole-file destination is too small");
        load->buffer = buffer;
    }
    else
    {
        if (!allocator) allocator = CurrentAllocator;
        if (allocator != &StandardAllocator && allocator != &VirtualAllocator)
            throw std::runtime_error("NL whole-file custom allocator ownership is not connected");
        load->buffer = allocator->Allocate(padded, alignment, type == AllocateEnd);
        load->ownsBuffer = true;
        capacity = padded;
    }
    // Tokens retain the existing game API's 32-bit type without casting pointers.
    // Never recycle them, including across shutdown, so stale tokens cannot cancel
    // later requests that reuse an AsyncEntry pool slot.
    load->handle = static_cast<unsigned int>(nextHandle++);
    const auto handle = load->handle;
    auto* record = load.get();
    loads.emplace(handle, std::move(load));
    try
    {
        record->read = nlReadAsync(record->file.get(), record->buffer, record->size,
            Complete, reinterpret_cast<nlFileAsyncParam>(record), capacity);
    }
    catch (...) { loads.erase(handle); throw; }
    return handle;
}

bool nlCancelEntireFileLoad(unsigned int handle, CancelCallback callback)
{
    const auto found = loads.find(handle);
    if (found == loads.end()) return false;
    auto* load = found->second.get();
    load->cancel = callback;
    // Aurora submits native reads immediately. Join even a busy worker, matching
    // native raw-read cancellation, before calling user code or freeing storage.
    return nlCancelAsyncRead(load->read, Cancel);
}

void nlAbortEntireFileLoad(nlFile* file)
{
    for (auto found = loads.begin(); found != loads.end(); ++found)
        if (found->second->file.get() == file)
        {
            loads.erase(found); // Read errors have no successful completion callback.
            return;
        }
}

void nlShutdownEntireFileLoads()
{
    loads.clear(); // No callbacks during shutdown; each destination outlives its workers.
}

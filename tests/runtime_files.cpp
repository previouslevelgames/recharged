#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "runtime/game_config.h"
#include "runtime/camera_assets.h"
#include "platform/path.h"
#include "dvd_fixture_medium.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlFunction.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

thread_local long dvd_allocation_budget = -1;
void* operator new(std::size_t size)
{
    if (dvd_allocation_budget == 0) throw std::bad_alloc();
    if (dvd_allocation_budget > 0) --dvd_allocation_budget;
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

void nlRegHandleDVDMessageCB(const Function<void(int)>& callback);

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

template<class Error, class Callable>
std::string ExpectThrow(Callable&& callable, const char* message)
{
    try { callable(); }
    catch (const Error& error) { return error.what(); }
    throw std::runtime_error(message);
}

struct FileCloser { void operator()(nlFile* file) const { nlClose(file); } };
using File = std::unique_ptr<nlFile, FileCloser>;
File Open(const char* path)
{
    File file(nlOpen(path));
    Require(file != nullptr, "Synthetic disc file failed to open");
    return file;
}

void ServiceUntil(const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done())
    {
        nlServiceFileSystem();
        Require(std::chrono::steady_clock::now() < deadline, "NL read completion timed out");
        if (!done()) SDL_Delay(1);
    }
}

struct Callback
{
    nlFile* file = nullptr;
    void* buffer = nullptr;
    unsigned size = 0;
    std::thread::id caller = std::this_thread::get_id();
    unsigned calls = 0;
    std::function<void()> action;
    static void Complete(nlFile* file, void* buffer, unsigned size, nlFileAsyncParam param)
    {
        auto& self = *reinterpret_cast<Callback*>(param);
        Require(file == self.file && buffer == self.buffer && size == self.size,
                "NL callback lost its file/buffer/count or pointer-sized context");
        Require(std::this_thread::get_id() == self.caller, "NL callback ran on the DVD worker thread");
        ++self.calls;
        if (self.action) self.action();
    }
    static void Cancel(nlFile* file, void* buffer, unsigned size, nlFileAsyncParam param, ReadAsyncCallback cb)
    {
        Require(cb == Complete, "Cancellation lost the original completion callback");
        Complete(file, buffer, size, param);
    }
    AsyncEntry* Queue(nlFile* source, void* target, unsigned count, unsigned capacity = 0)
    {
        file = source; buffer = target; size = capacity > count ? AlignUp32(count) : count;
        return nlReadAsync(file, buffer, count, Complete, reinterpret_cast<nlFileAsyncParam>(this), capacity);
    }
};

struct CallbackError : std::runtime_error
{ CallbackError() : std::runtime_error("callback throw check") {} };

struct GameFree { void operator()(void* buffer) const { if (buffer) nlFree(buffer); } };
void CheckBytes(const unsigned char* bytes, unsigned position, unsigned count);

struct Whole
{
    unsigned long size = 4097;
    void* supplied = nullptr;
    unsigned handle = 0, calls = 0, cancellations = 0;
    unsigned alignment = 128;
    std::thread::id caller = std::this_thread::get_id();
    std::function<void(void*)> action, onCancel;

    static void Complete(void* buffer, unsigned long size, void* user)
    {
        auto& self = *static_cast<Whole*>(user);
        // The callback owns allocated output, including when its action throws.
        std::unique_ptr<void, GameFree> owner(self.supplied ? nullptr : buffer);
        Require(std::this_thread::get_id() == self.caller && size == self.size,
                "Whole-file completion lost its logical size/context/servicing thread");
        Require(self.supplied ? buffer == self.supplied || !size : !size || buffer != nullptr,
                "Whole-file completion lost its destination");
        if (size)
        {
            if (!self.supplied)
                Require(reinterpret_cast<std::uintptr_t>(buffer) > UINT32_MAX
                    && reinterpret_cast<std::uintptr_t>(buffer)%self.alignment == 0,
                    "Whole-file output was truncated or lost its allocation alignment");
            CheckBytes(static_cast<unsigned char*>(buffer), 0, size);
        }
        else Require(buffer == nullptr, "Empty whole-file completion returned a buffer");
        ++self.calls;
        if (self.action) self.action(buffer);
    }

    static void Cancel(void* buffer, unsigned long size, void* user, LoadAsyncCallback original)
    {
        auto& self = *static_cast<Whole*>(user);
        Require(std::this_thread::get_id() == self.caller && size == self.size
            && original == Complete && buffer && (!self.supplied || buffer == self.supplied),
            "Whole-file cancellation lost its buffer/size/context/callback/servicing thread");
        ++self.cancellations;
        if (self.onCancel) self.onCancel(buffer);
    }

    unsigned Queue(MemoryAllocator* allocator = nullptr, eAllocType type = AllocateStart,
        void* buffer = nullptr, unsigned long capacity = 0, const char* path = "/large.bin")
    {
        supplied = buffer;
        Require(reinterpret_cast<std::uintptr_t>(this) > UINT32_MAX,
                "Whole-file test context must exercise addresses above 4 GiB");
        handle = nlLoadEntireFileAsync(path, Complete, this, alignment, type, buffer, capacity, allocator);
        return handle;
    }
};

struct Session
{
    bool live = false;
    bool disc = false;
    ~Session()
    {
        if (live) mscharged::ResetStartupFiles();
        if (disc) aurora_dvd_close();
        if (live) { mscharged::ResetStartupMemory(); aurora_shutdown(); }
    }
};

unsigned char Expected(unsigned index) { return (index*37+11)&255; }
void CheckBytes(const unsigned char* bytes, unsigned position, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        Require(bytes[i] == Expected(position+i), "NL disc bytes differ from the original synthetic payload");
}
void CheckGuard(const unsigned char* bytes, unsigned start, unsigned end)
{
    for (unsigned i = start; i < end; ++i)
        Require(bytes[i] == 0xa5, "NL read wrote past its destination");
}

void CheckSync()
{
    Require(nlFileExists("/folder/end.bin") && !nlFileExists("/absent.bin"), "NL path lookup is incorrect");
    auto empty = Open("/empty.bin");
    Require(nlFileSize(empty.get(), nullptr) == 0, "Empty file has a nonzero length");
    unsigned long empty_size = 99;
    Require(nlLoadEntireFile("/empty.bin", &empty_size, 32, AllocateStart, nullptr, 0, nullptr) == nullptr
        && empty_size == 0, "Empty whole-file load changed its contract");
    auto file = Open("/large.bin");
    unsigned aligned = 0;
    Require(nlFileSize(file.get(), &aligned) == 4097 && aligned == 4128, "NL logical/padded file sizes are incorrect");
    alignas(32) std::array<unsigned char, 160> data{};
    for (unsigned size : {0u, 1u, 23u, 31u, 32u, 33u, 63u, 64u, 65u, 127u})
    {
        data.fill(0xa5);
        nlSeek(file.get(), 3, 0);
        nlRead(file.get(), data.data(), size, size);
        CheckBytes(data.data(), 3, size);
        CheckGuard(data.data(), size, data.size());
    }
    nlSeek(file.get(), 17, 0);
    nlSeek(file.get(), 9, 1);
    nlRead(file.get(), data.data(), 1, 1); CheckBytes(data.data(), 26, 1);
    nlSeek(file.get(), 1, 2);
    nlRead(file.get(), data.data(), 1, 1); CheckBytes(data.data(), 4096, 1);
    nlRead(file.get(), nullptr, 0, 0);
    ExpectThrow<std::out_of_range>([&] { nlRead(file.get(), data.data(), 1, 1); }, "EOF read was accepted");
    ExpectThrow<std::out_of_range>([&] { nlSeek(file.get(), 4098, 0); }, "Out-of-range seek was accepted");
    ExpectThrow<std::out_of_range>([&] { nlSeek(file.get(), 1, 1); }, "Relative seek past EOF was accepted");
    ExpectThrow<std::out_of_range>([&] { nlSeek(file.get(), 4098, 2); }, "Seek backwards past start was accepted");
    ExpectThrow<std::invalid_argument>([&] { nlSeek(file.get(), 0, 3); }, "Invalid seek origin was accepted");
    nlSeek(file.get(), 0, 0);
    ExpectThrow<std::length_error>([&] { nlRead(file.get(), data.data(), 33, 32); }, "Undersized destination was accepted");
    ExpectThrow<std::length_error>([&] { nlRead(file.get(), data.data(), 33, 34); }, "Undersized padded destination was accepted");
    ExpectThrow<std::invalid_argument>([&] { nlRead(file.get(), nullptr, 1, 1); }, "Null destination was accepted");
    data.fill(0xa5);
    nlRead(file.get(), data.data(), 33, 64);
    CheckBytes(data.data(), 0, 64); // Original padded-read contract.
    CheckGuard(data.data(), 64, data.size());
    nlRead(file.get(), data.data(), 1, 1); CheckBytes(data.data(), 33, 1); // Logical cursor.
    std::cout << "NL sync paths, seek/EOF, exact/padded/unaligned reads and bounds passed\n";
}

void CheckAsync()
{
    alignas(32) std::array<unsigned char, 160> data{};
    Callback callback;
    Require(reinterpret_cast<std::uintptr_t>(&callback) > UINT32_MAX,
            "This host test must exercise callback context above 4 GiB");
    auto file = Open("/large.bin");
    for (unsigned size : {0u, 1u, 23u, 32u, 33u, 64u, 65u, 127u})
    {
        nlSeek(file.get(), 3, 0); data.fill(0xa5); callback.calls = 0;
        auto* request = callback.Queue(file.get(), data.data(), size, size);
        ServiceUntil([&] { return callback.calls != 0; });
        Require(callback.calls == 1 && !nlAsyncReadsPending(file.get()) && !nlAsyncReadBusy(request),
                "Completed request is still pending or called back twice");
        CheckBytes(data.data(), 3, size); CheckGuard(data.data(), size, data.size());
    }
    nlSeek(file.get(), 0, 0); callback.calls = 0; data.fill(0xa5);
    callback.Queue(file.get(), data.data(), 33, 64);
    ServiceUntil([&] { return callback.calls != 0; });
    CheckBytes(data.data(), 0, 64); CheckGuard(data.data(), 64, data.size());
    std::cout << "NL async head/tail, zero/padded reads, caller thread and 64-bit context passed\n";
}

void CheckReentrancy()
{
    alignas(32) std::array<unsigned char, 128> first{}, second{}, nested{};
    Callback a, b;
    auto file = Open("/large.bin");
    auto other = Open("/large.bin");
    a.action = [&] {
        auto* current = nlGetCurrentAsyncRead();
        Require(current != nullptr, "Callback lost its current entry");
        ExpectThrow<std::logic_error>([] { nlShutdownFileSystem(); }, "Callback could destroy its active manager");
        b.Queue(other.get(), second.data(), 65, 65);
        nlSeek(file.get(), 7, 0);
        nlRead(file.get(), nested.data(), 33, 33); // Services b and this synchronous request.
        Require(nlGetCurrentAsyncRead() == current, "Nested service lost the outer callback entry");
    };
    a.Queue(file.get(), first.data(), 65, 65);
    ServiceUntil([&] { return a.calls && b.calls; });
    CheckBytes(first.data(), 0, 65); CheckBytes(second.data(), 0, 65); CheckBytes(nested.data(), 7, 33);
    Require(nlGetCurrentAsyncRead() == nullptr && !nlAsyncReadsPending(nullptr), "Reentrant callback left active state");
    a.calls = 0;
    a.action = [] { throw CallbackError(); };
    a.Queue(file.get(), first.data(), 64, 64);
    ExpectThrow<CallbackError>([&] { ServiceUntil([&] { return false; }); }, "Callback exception was swallowed");
    Require(a.calls == 1 && !nlAsyncReadsPending(file.get()) && nlGetCurrentAsyncRead() == nullptr,
            "Throwing callback leaked an active entry");
    a.action = {}; a.calls = 0;
    nlSeek(file.get(), 0, 0); a.Queue(file.get(), first.data(), 64, 64);
    ServiceUntil([&] { return a.calls != 0; });
    // Closing the source from its completion callback must not access it again.
    a.action = [&] { file.reset(); };
    a.calls = 0; a.Queue(file.get(), first.data(), 65, 65);
    ServiceUntil([&] { return a.calls != 0; });
    Require(!file && !nlAsyncReadsPending(nullptr), "Closing a file in its callback left requests live");
    std::cout << "NL callback queueing, recursive sync reads, exceptions and callback close passed\n";
}

void CheckCancellation()
{
    alignas(32) std::array<unsigned char, 128> data{};
    Callback callback;
    auto file = Open("/large.bin");
    auto* request = callback.Queue(file.get(), data.data(), 65, 65);
    callback.action = [&] { data.fill(0xa5); file.reset(); }; // Worker must already be drained.
    Require(nlCancelAsyncRead(request, Callback::Cancel), "Pending head/tail read could not be cancelled");
    SDL_Delay(5); CheckGuard(data.data(), 0, data.size());
    Require(callback.calls == 1 && !file && !nlAsyncReadsPending(nullptr), "Cancelled pair retained its tail or callback");
    Require(!nlCancelAsyncRead(request, Callback::Cancel), "Completed cancellation remained active");
    file = Open("/large.bin"); callback.calls = 0; callback.action = {};
    callback.Queue(file.get(), data.data(), 65, 65);
    Callback second;
    second.Queue(file.get(), data.data()+65, 23, 23);
    nlCancelPendingAsyncReads(file.get(), Callback::Cancel);
    Require(callback.calls == 1 && second.calls == 1 && !nlAsyncReadsPending(file.get()), "Cancel-all missed a request");
    // No callback for close/shutdown; destination can be released immediately afterward.
    callback.calls = 0;
    callback.Queue(file.get(), data.data(), 65, 65);
    file.reset(); data.fill(0xa5); SDL_Delay(5);
    Require(callback.calls == 0 && !nlAsyncReadsPending(nullptr), "Closing a file completed a cancelled request");
    CheckGuard(data.data(), 0, data.size());
    std::cout << "NL cancellation pairs, callback close, cancel-all and pending close passed\n";
}

void CheckPools()
{
    std::vector<File> files;
    for (unsigned i = 0; i < 96; ++i) files.push_back(Open("/large.bin"));
    ExpectThrow<std::bad_alloc>([] { Open("/large.bin"); }, "NL file pool exhaustion was accepted");
    files.clear(); // Every slot must be reusable.
    auto file = Open("/large.bin");
    alignas(32) std::array<std::array<unsigned char, 64>, 64> buffers{};
    for (unsigned i = 0; i < 63; ++i)
        nlReadAsync(file.get(), buffers[i].data(), 32, nullptr, 0, 32);
    // One free slot cannot hold a 33-byte head/tail pair. Failure must be atomic.
    ExpectThrow<std::bad_alloc>([&] { nlReadAsync(file.get(), buffers[63].data(), 33, nullptr, 0, 33); },
                              "Partial head/tail request was accepted with one free slot");
    Callback last;
    last.Queue(file.get(), buffers[63].data(), 32, 32);
    ExpectThrow<std::bad_alloc>([&] { nlReadAsync(file.get(), buffers[63].data(), 1, nullptr, 0, 1); },
                              "Full async queue was accepted");
    ServiceUntil([&] { return !nlAsyncReadsPending(file.get()); });
    Require(last.calls == 1, "Full async queue lost a completion");
    CheckBytes(buffers[63].data(), 63*32, 32); // Failed split must not advance cursor.
    for (unsigned i = 0; i < 64; ++i)
        nlReadAsync(file.get(), buffers[i].data(), 32, nullptr, 0, 32);
    nlCancelPendingAsyncReads(file.get(), nullptr);
    Require(!nlAsyncReadsPending(nullptr), "Exhausted queue could not be drained");
    std::cout << "NL 96-file/64-request pools and atomic head/tail capacity checks passed\n";
}

void CheckWholeFiles()
{
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    auto* selected = CurrentAllocator;
    const auto depth = AllocatorStackDepth;
    for (auto* allocator : {&StandardAllocator, &VirtualAllocator})
        for (auto type : {AllocateStart, AllocateEnd})
        {
            unsigned long size = 0;
            auto* bytes = static_cast<unsigned char*>(nlLoadEntireFile("/large.bin", &size, 128, type, nullptr, 0, allocator));
            Require(bytes && size == 4097 && reinterpret_cast<std::uintptr_t>(bytes)%128 == 0,
                    "Whole-file load lost length/alignment");
            CheckBytes(bytes, 0, size);
            Require(CurrentAllocator == selected && AllocatorStackDepth == depth, "Whole-file load changed allocator selection");
            nlFree(bytes);
        }
    alignas(32) std::array<unsigned char, 4160> destination{};
    destination.fill(0xa5);
    unsigned long size = 0;
    Require(nlLoadEntireFile("/large.bin", &size, 32, AllocateStart, destination.data(), 4097, nullptr) == destination.data(),
            "Whole-file load discarded the supplied destination");
    CheckBytes(destination.data(), 0, 4097); CheckGuard(destination.data(), 4097, destination.size());
    ExpectThrow<std::length_error>([&] {
        nlLoadEntireFile("/large.bin", &size, 32, AllocateStart, destination.data(), 33, nullptr);
    }, "Undersized whole-file destination was accepted");
    MemoryAllocator unsupported{};
    ExpectThrow<std::runtime_error>([&] {
        nlLoadEntireFile("/large.bin", &size, 32, AllocateStart, nullptr, 0, &unsupported);
    }, "Custom-heap ownership was silently accepted");
    // Force a real allocation failure and verify stack restoration and file close.
    std::vector<void*> allocations;
    for (;;)
    {
        try { allocations.push_back(StandardAllocator.Allocate(256*1024, 32, false)); }
        catch (const std::bad_alloc&) { break; }
    }
    const auto remaining = StandardAllocator.LargestFreeBlock();
    if (remaining > 256) allocations.push_back(StandardAllocator.Allocate(remaining-128, 32, false));
    ExpectThrow<std::bad_alloc>([&] {
        nlLoadEntireFile("/large.bin", &size, 32, AllocateStart, nullptr, 0, &StandardAllocator);
    }, "Whole-file allocation failure was not reported");
    Whole async;
    ExpectThrow<std::bad_alloc>([&] { async.Queue(&StandardAllocator); },
                               "Async whole-file allocation failure was not reported");
    Require(async.calls == 0 && !nlAsyncReadsPending(nullptr), "Async allocation failure queued or completed a read");
    for (void* block : allocations) if (block) StandardAllocator.Free(block);
    Require(CurrentAllocator == selected && AllocatorStackDepth == depth
        && StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2,
            "Whole-file failure leaked memory or allocator state");
    std::cout << "NL whole-file MEM1/MEM2 ownership, supplied buffers and failure cleanup passed\n";
}

void CheckAsyncWholeFiles()
{
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    auto* selected = CurrentAllocator;
    const auto depth = AllocatorStackDepth;
    std::array<MemoryAllocator*, 16> stack{};
    std::copy(std::begin(AllocatorStack), std::end(AllocatorStack), stack.begin());
    for (auto* allocator : {&StandardAllocator, &VirtualAllocator})
        for (auto type : {AllocateStart, AllocateEnd})
        {
            Whole completion;
            completion.action = [&](void*) {
                Require(!nlCancelEntireFileLoad(completion.handle, Whole::Cancel),
                        "Whole-file callback retained a cancellable token");
            };
            Require(completion.Queue(allocator, type) && !completion.calls, "Nonempty whole file completed inline");
            ServiceUntil([&] { return completion.calls != 0; });
            Require(completion.calls == 1 && !nlCancelEntireFileLoad(completion.handle, Whole::Cancel),
                    "Whole file completed twice or left a live token");
        }
    alignas(32) std::array<unsigned char, 4160> destination{};
    for (unsigned capacity : {0u, 4097u, 4128u})
    {
        destination.fill(0xa5);
        Whole completion;
        completion.Queue(nullptr, AllocateStart, destination.data(), capacity);
        ServiceUntil([&] { return completion.calls != 0; });
        CheckGuard(destination.data(), capacity ? capacity : 4097, destination.size());
    }
    destination.fill(0xa5);
    Whole unaligned;
    unaligned.Queue(nullptr, AllocateStart, destination.data()+1, 4097);
    ServiceUntil([&] { return unaligned.calls != 0; });
    Require(destination.front() == 0xa5, "Unaligned whole-file read overwrote its prefix");
    CheckGuard(destination.data(), 4098, destination.size());
    Whole missing;
    Require(!missing.Queue(nullptr, AllocateStart, nullptr, 0, "/absent.bin") && !missing.calls,
            "Missing whole file returned a token or success callback");
    destination.fill(0xa5);
    Whole empty; empty.size = 0;
    Require(!empty.Queue(nullptr, AllocateStart, destination.data(), destination.size(), "/empty.bin")
        && empty.calls == 1, "Empty whole file did not complete inline with token zero");
    CheckGuard(destination.data(), 0, destination.size());
    Whole invalid;
    ExpectThrow<std::invalid_argument>([&] {
        nlLoadEntireFileAsync("/large.bin", nullptr, &invalid, 32, AllocateStart, nullptr, 0, nullptr);
    }, "Whole-file load accepted a null completion callback");
    for (unsigned capacity : {33u, 4098u})
        ExpectThrow<std::length_error>([&] {
            invalid.Queue(nullptr, AllocateStart, destination.data(), capacity);
        }, "Whole-file load accepted insufficient logical/padded capacity");
    MemoryAllocator unsupported{};
    ExpectThrow<std::runtime_error>([&] { invalid.Queue(&unsupported); }, "Async custom-heap ownership was accepted");
    invalid.alignment = 3;
    ExpectThrow<std::invalid_argument>([&] { invalid.Queue(); }, "Async invalid allocation alignment was accepted");
    // Saturate the real low-level queue. Submission must roll back its output/file/state.
    auto file = Open("/large.bin");
    alignas(32) std::array<std::array<unsigned char, 32>, 64> raw{};
    for (auto& bytes : raw) nlReadAsync(file.get(), bytes.data(), bytes.size(), nullptr, 0, bytes.size());
    invalid.alignment = 128;
    ExpectThrow<std::bad_alloc>([&] { invalid.Queue(&VirtualAllocator); }, "Whole file entered a full raw queue");
    nlCancelPendingAsyncReads(file.get(), nullptr);
    Require(CurrentAllocator == selected && AllocatorStackDepth == depth
        && std::equal(stack.begin(), stack.end(), std::begin(AllocatorStack))
        && StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2
        && !nlAsyncReadsPending(nullptr), "Async whole-file loads leaked allocations or mutated allocator selection");
    std::vector<File> slots;
    file.reset();
    for (unsigned i = 0; i < 96; ++i) slots.push_back(Open("/large.bin"));
    std::cout << "NL whole-file async MEM1/MEM2, logical counts, buffers, zero/missing and failure rollback passed\n";
}

void CheckWholeReentrancyAndCancellation()
{
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    Whole first, second;
    first.action = [&](void*) {
        const auto* current = nlGetCurrentAsyncRead();
        ExpectThrow<std::logic_error>([] { nlShutdownFileSystem(); }, "Whole-file callback destroyed its active manager");
        second.Queue(&VirtualAllocator, AllocateEnd);
        auto file = Open("/large.bin");
        alignas(32) std::array<unsigned char, 33> nested{};
        nlRead(file.get(), nested.data(), nested.size(), nested.size());
        CheckBytes(nested.data(), 0, nested.size());
        Require(current == nlGetCurrentAsyncRead(), "Nested whole-file service lost its outer entry");
    };
    first.Queue();
    ServiceUntil([&] { return first.calls && second.calls; });
    Require(first.calls == 1 && second.calls == 1, "Nested whole-file completion count is incorrect");
    // A throwing completion owns its output; it must not abort unrelated requests.
    Whole throwing, survivor;
    throwing.action = [](void*) { throw CallbackError(); };
    throwing.Queue(); survivor.Queue(&VirtualAllocator);
    ExpectThrow<CallbackError>([&] { ServiceUntil([] { return false; }); }, "Whole-file callback exception was swallowed");
    Require(throwing.calls == 1 && !survivor.calls && !nlCancelEntireFileLoad(throwing.handle, Whole::Cancel),
            "Throwing whole-file callback retained its token or completed another request");
    ServiceUntil([&] { return survivor.calls != 0; });
    Whole cancelFirst, cancelSecond;
    cancelFirst.Queue(); cancelSecond.Queue(&VirtualAllocator);
    cancelFirst.onCancel = [&](void*) {
        Require(!nlCancelEntireFileLoad(cancelFirst.handle, Whole::Cancel), "Cancellation retained its own token");
        Require(nlCancelEntireFileLoad(cancelSecond.handle, Whole::Cancel), "Nested cancellation failed");
    };
    Require(nlCancelEntireFileLoad(cancelFirst.handle, Whole::Cancel), "Pending whole file could not be cancelled");
    Require(!cancelFirst.calls && !cancelSecond.calls && cancelFirst.cancellations == 1
        && cancelSecond.cancellations == 1, "Nested cancellation used the wrong callback or completed cancelled data");
    const auto stale = cancelFirst.handle;
    Whole replacement;
    Require(replacement.Queue() != stale && !nlCancelEntireFileLoad(stale, Whole::Cancel)
        && !nlCancelEntireFileLoad(0, Whole::Cancel) && !nlCancelEntireFileLoad(UINT32_MAX, Whole::Cancel),
        "Stale/unknown whole-file token cancelled a new request");
    ServiceUntil([&] { return replacement.calls != 0; });
    Whole cancelThrow;
    cancelThrow.onCancel = [](void*) { throw CallbackError(); };
    cancelThrow.Queue(&VirtualAllocator);
    ExpectThrow<CallbackError>([&] { nlCancelEntireFileLoad(cancelThrow.handle, Whole::Cancel); },
                               "Cancellation callback exception was swallowed");
    Require(cancelThrow.cancellations == 1 && !cancelThrow.calls
        && !nlCancelEntireFileLoad(cancelThrow.handle, Whole::Cancel), "Throwing cancellation retained its state");
    alignas(32) std::array<unsigned char, 4160> borrowed{};
    Whole supplied;
    supplied.Queue(nullptr, AllocateStart, borrowed.data(), 4097);
    supplied.onCancel = [&](void*) { borrowed.fill(0xa5); };
    Require(nlCancelEntireFileLoad(supplied.handle, Whole::Cancel), "Borrowed-buffer whole-file cancellation failed");
    SDL_Delay(5); CheckGuard(borrowed.data(), 0, borrowed.size());
    Require(supplied.cancellations == 1 && !supplied.calls && !nlAsyncReadsPending(nullptr)
        && StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2,
        "Whole-file reentrancy/cancellation leaked an allocation, callback or request");
    std::cout << "NL whole-file recursive reads, throwing callbacks, nested cancellation, borrowed data and stale tokens passed\n";
}

// Aurora's test-only overlay lets us force a short/error read and a worker
// that is definitely active during cancellation, without corrupting retail data.
struct FaultFile
{
    enum Mode { Short, Error, Blocked } mode = Short;
    std::atomic<bool> entered{false}, finished{false};
    std::atomic<unsigned> handles{0};
    std::mutex mutex;
    std::condition_variable release;
    bool ready = false;
    struct Handle { FaultFile* owner; std::int64_t position = 0; };
    static void* Open(void* user)
    {
        auto* owner = static_cast<FaultFile*>(user);
        auto* handle = new Handle{owner}; ++owner->handles;
        return handle;
    }
    static void Close(void* user)
    {
        auto* handle = static_cast<Handle*>(user);
        --handle->owner->handles; delete handle;
    }
    static std::int64_t Seek(void* user, std::int64_t offset, std::int32_t origin)
    {
        auto& handle = *static_cast<Handle*>(user);
        if (origin != 0 || offset < 0 || offset > 128) return -1;
        return handle.position = offset;
    }
    static std::int64_t Read(void* user, std::uint8_t* bytes, std::size_t size)
    {
        auto& handle = *static_cast<Handle*>(user);
        auto& owner = *handle.owner;
        owner.entered = true;
        if (owner.mode == Error) { owner.finished = true; return -1; }
        if (owner.mode == Short && handle.position >= 3) { owner.finished = true; return 0; }
        if (owner.mode == Blocked)
        {
            std::unique_lock lock(owner.mutex);
            if (!owner.release.wait_for(lock, std::chrono::seconds(2), [&] { return owner.ready; }))
            { owner.finished = true; return -1; }
        }
        const auto count = std::min<std::size_t>(size, owner.mode == Short ? 3 : 128-handle.position);
        for (std::size_t i = 0; i < count; ++i) bytes[i] = Expected(handle.position+i);
        handle.position += count; owner.finished = true;
        return count;
    }
};

void CheckDvdAllocationFailures()
{
    struct Budget
    {
        explicit Budget(long value) { dvd_allocation_budget = value; }
        ~Budget() { dvd_allocation_budget = -1; }
    };
    const auto entry = DVDConvertPathToEntrynum("/large.bin");
    Require(entry >= 0, "DVD admission fixture is absent");
    DVDFileInfo info{};
    const bool opened = [&] { Budget failure(0); return DVDFastOpen(entry, &info); }();
    Require(!opened && !info.cb.userData && DVDClose(&info), "Failed nod command allocation published ownership");
    auto file = Open("/large.bin");
    alignas(32) std::array<std::array<unsigned char,32>,65> buffers{};
    bool rejected = false;
    try { Budget failure(0); nlReadAsync(file.get(), buffers[0].data(), 32, nullptr, 0, 32); }
    catch (const std::bad_alloc&) { rejected = true; }
    Require(rejected && !nlAsyncReadsPending(file.get()), "Failed raw open retained PendingAsync");
    for (unsigned i = 0; i < 64; ++i)
    {
        nlSeek(file.get(), 0, 0);
        nlReadAsync(file.get(), buffers[i].data(), 32, nullptr, 0, 32);
    }
    ExpectThrow<std::bad_alloc>([&] { nlReadAsync(file.get(), buffers[64].data(), 32, nullptr, 0, 32); },
                               "Failed DVD admission lost the original request capacity");
    nlCancelPendingAsyncReads(file.get(), nullptr);
    Require(!nlAsyncReadsPending(nullptr), "Restored DVD request queue did not drain");

    FaultFile fault;
    const AuroraOverlayCallbacks callbacks{FaultFile::Open, FaultFile::Close, FaultFile::Read, FaultFile::Seek};
    aurora_dvd_overlay_callbacks(&callbacks);
    const AuroraOverlayFile overlay{"/allocation-fault.bin", &fault, 128};
    aurora_dvd_overlay_files(&overlay, 1, nullptr);
    struct Clear { ~Clear() { aurora_dvd_overlay_files(nullptr, 0, nullptr); } } cleanup;
    const auto overlay_entry = DVDConvertPathToEntrynum("/allocation-fault.bin");
    Require(overlay_entry >= 0, "Overlay allocation fixture is absent");
    for (long value : {0L, 1L})
    {
        DVDFileInfo overlay_info{};
        const bool accepted = [&] { Budget failure(value); return DVDFastOpen(overlay_entry, &overlay_info); }();
        Require(!accepted && !overlay_info.cb.userData && fault.handles == 0 && DVDClose(&overlay_info),
                "Failed overlay allocation retained provider ownership");
    }
    std::cout << "DVD nod/overlay allocation failure and original 64-request recovery passed\n";
}

void CheckReadFailures()
{
    FaultFile fault;
    const AuroraOverlayCallbacks callbacks{FaultFile::Open, FaultFile::Close, FaultFile::Read, FaultFile::Seek};
    aurora_dvd_overlay_callbacks(&callbacks);
    const AuroraOverlayFile overlay{"/read-fault.bin", &fault, 128};
    aurora_dvd_overlay_files(&overlay, 1, nullptr);
    struct ClearOverlay { ~ClearOverlay() { aurora_dvd_overlay_files(nullptr, 0, nullptr); } } cleanup;
    for (auto mode : {FaultFile::Short, FaultFile::Error})
    {
        fault.mode = mode;
        const bool fatal = mode == FaultFile::Error;
        const auto finishCase = [&] {
            Require(fault.entered && fault.finished && !fault.handles,
                    "Fault case did not perform and retire its actual overlay read");
            mscharged::test::FinishDVDTestFaultCase(fatal);
            // A fatal case gets a new real media owner and therefore a new FST.
            if (fatal) aurora_dvd_overlay_files(&overlay, 1, nullptr);
            fault.entered = false;
            fault.finished = false;
        };
        alignas(32) std::array<unsigned char, 128> bytes{};
        Callback completion;
        {
            alignas(32) std::array<unsigned char, 64> survivorBytes{};
            survivorBytes.fill(0xa5);
            Callback survivor;
            // File destructors drain while the borrowed buffer/context live.
            auto file = Open("/read-fault.bin");
            auto survivorFile = Open("/large.bin");
            // An unpadded 65-byte request has both raw head and tail entries.
            // Keep the failed file open: its destructor must not hide a leak.
            auto* request = completion.Queue(file.get(), bytes.data(), 65, 65);
            survivor.Queue(survivorFile.get(), survivorBytes.data(), 64, 64);
            const auto error = ExpectThrow<std::runtime_error>([&] { ServiceUntil([&] { return completion.calls != 0; }); },
                                                              "Short/failed disc read was reported as successful");
            Require(error == (mode == FaultFile::Short ? "Short NL disc read" : "NL asynchronous DVD read failed"),
                    "Read failure did not report its actual DVD/transfer error");
            Require(!completion.calls && !nlAsyncReadsPending(file.get()) && !fault.handles
                && !nlCancelAsyncRead(request, Callback::Cancel) && !survivor.calls
                && nlAsyncReadsPending(survivorFile.get()),
                "Failed raw read retained its pair or changed an unrelated request");
            if (fatal)
            {
                Require(ExpectThrow<std::runtime_error>([&] {
                    ServiceUntil([&] { return survivor.calls != 0; });
                }, "Queued read succeeded after a fatal drive error") == "NL asynchronous DVD read failed",
                    "Queued read lost the latched fatal-drive result");
                Require(!survivor.calls && !nlAsyncReadsPending(survivorFile.get()),
                        "Post-fatal request retained state or reported success");
                CheckGuard(survivorBytes.data(), 0, survivorBytes.size());
            }
            else
            {
                ServiceUntil([&] { return survivor.calls != 0; });
                CheckBytes(survivorBytes.data(), 0, survivorBytes.size());
            }
        }
        Require(completion.calls == 0 && !nlAsyncReadsPending(nullptr) && fault.handles == 0,
                "Failed read called back with success or retained its worker handle");
        finishCase();
        const auto free = StandardAllocator.TotalFreeMemory();
        ExpectThrow<std::runtime_error>([&] {
            nlLoadEntireFile("/read-fault.bin", nullptr, 32, AllocateStart, nullptr, 0, nullptr);
        }, "Failed whole-file read was accepted");
        Require(StandardAllocator.TotalFreeMemory() == free && fault.handles == 0 && !nlAsyncReadsPending(nullptr),
                "Failed whole-file read leaked its allocation/file/request");
        finishCase();
        const auto mem2 = VirtualAllocator.TotalFreeMemory();
        for (auto* allocator : {&StandardAllocator, &VirtualAllocator})
        {
            Whole failed, survivor; failed.size = 128;
            failed.Queue(allocator, AllocateStart, nullptr, 0, "/read-fault.bin");
            survivor.Queue(&VirtualAllocator);
            ExpectThrow<std::runtime_error>([&] { ServiceUntil([] { return false; }); },
                                           "Failed async whole file completed successfully");
            Require(!failed.calls && !failed.cancellations && !survivor.calls && fault.handles == 0
                && !nlCancelEntireFileLoad(failed.handle, Whole::Cancel),
                "Failed whole-file request retained state or aborted/completed another request");
            if (fatal)
            {
                Require(ExpectThrow<std::runtime_error>([&] {
                    ServiceUntil([&] { return survivor.calls != 0; });
                }, "Queued whole file succeeded after a fatal drive error") == "NL asynchronous DVD read failed",
                    "Queued whole file lost the latched fatal-drive result");
                Require(!survivor.calls && !survivor.cancellations
                    && !nlCancelEntireFileLoad(survivor.handle, Whole::Cancel),
                    "Post-fatal whole file retained state or reported success");
            }
            else ServiceUntil([&] { return survivor.calls != 0; });
            finishCase();
        }
        Whole borrowed; borrowed.size = 128;
        bytes.fill(0xa5);
        borrowed.Queue(nullptr, AllocateStart, bytes.data(), bytes.size(), "/read-fault.bin");
        // DolphinFile::Read services the same manager directly. Errors must also
        // release the pending whole-file record through this entry point.
        ExpectThrow<std::runtime_error>([] {
            nlLoadEntireFile("/large.bin", nullptr, 32, AllocateStart, nullptr, 0, nullptr);
        }, "Synchronous servicing swallowed an async whole-file error");
        Require(!borrowed.calls && !borrowed.cancellations && !fault.handles
            && !nlCancelEntireFileLoad(borrowed.handle, Whole::Cancel) && !nlAsyncReadsPending(nullptr)
            && StandardAllocator.TotalFreeMemory() == free && VirtualAllocator.TotalFreeMemory() == mem2,
            "Async read failure leaked its file/output or freed borrowed data");
        finishCase();
    }
    fault.mode = FaultFile::Blocked; fault.entered = false; fault.finished = false;
    alignas(32) std::array<unsigned char, 128> bytes{};
    Callback completion;
    auto file = Open("/read-fault.bin");
    auto* request = completion.Queue(file.get(), bytes.data(), 65, 65);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!fault.entered)
    { Require(std::chrono::steady_clock::now() < deadline, "Read worker did not start"); SDL_Delay(1); }
    std::jthread release([&] {
        SDL_Delay(10);
        { std::lock_guard lock(fault.mutex); fault.ready = true; }
        fault.release.notify_all();
    });
    completion.action = [&] {
        Require(fault.finished && fault.handles == 0, "Cancellation callback ran before draining the active worker");
        bytes.fill(0xa5);
    };
    Require(nlCancelAsyncRead(request, Callback::Cancel), "Active worker request could not be cancelled");
    Require(completion.calls == 1 && !nlAsyncReadsPending(nullptr), "Active cancellation retained its paired tail");
    SDL_Delay(5); CheckGuard(bytes.data(), 0, bytes.size());
    std::cout << "NL short/error reads, whole-file failure cleanup and active-worker cancellation passed\n";
}

void CheckActiveWholeCancellationAndShutdown()
{
    FaultFile fault; fault.mode = FaultFile::Blocked;
    const AuroraOverlayCallbacks callbacks{FaultFile::Open, FaultFile::Close, FaultFile::Read, FaultFile::Seek};
    aurora_dvd_overlay_callbacks(&callbacks);
    const AuroraOverlayFile overlay{"/read-fault.bin", &fault, 128};
    aurora_dvd_overlay_files(&overlay, 1, nullptr);
    struct ClearOverlay { ~ClearOverlay() { aurora_dvd_overlay_files(nullptr, 0, nullptr); } } cleanup;
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    alignas(32) std::array<unsigned char, 128> borrowed{};
    unsigned stale = 0;
    for (bool shutdown : {false, true})
    {
        fault.entered = false; fault.finished = false; fault.ready = false;
        Whole active; active.size = borrowed.size();
        stale = active.Queue(nullptr, AllocateStart, borrowed.data(), borrowed.size(), "/read-fault.bin");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!fault.entered)
        { Require(std::chrono::steady_clock::now() < deadline, "Whole-file worker did not start"); SDL_Delay(1); }
        std::jthread release([&] {
            SDL_Delay(10);
            { std::lock_guard lock(fault.mutex); fault.ready = true; }
            fault.release.notify_all();
        });
        active.onCancel = [&](void*) {
            Require(fault.finished && fault.handles == 0, "Whole-file cancellation returned storage before its worker stopped");
            borrowed.fill(0xa5);
        };
        if (shutdown)
        {
            Whole owned;
            owned.Queue(&VirtualAllocator, AllocateEnd);
            nlShutdownFileSystem();
            Require(!owned.calls && !owned.cancellations, "Shutdown invoked an owned whole-file callback");
            borrowed.fill(0xa5);
        }
        else Require(nlCancelEntireFileLoad(active.handle, Whole::Cancel), "Busy whole-file worker was not cancelled");
        Require(fault.finished && !fault.handles && !active.calls && active.cancellations == !shutdown
            && !nlCancelEntireFileLoad(stale, Whole::Cancel), "Active whole-file cleanup retained a handle/callback/token");
        SDL_Delay(5); CheckGuard(borrowed.data(), 0, borrowed.size());
        if (shutdown) nlInitFileSystem();
        Require(StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2,
                "Active whole-file cancellation/shutdown leaked its arena allocation");
    }
    Whole afterRestart;
    Require(afterRestart.Queue() != stale && !nlCancelEntireFileLoad(stale, Whole::Cancel),
            "Shutdown recycled a stale whole-file token");
    ServiceUntil([&] { return afterRestart.calls != 0; });
    std::cout << "NL busy whole-file worker draining, shutdown ownership and tokens across reinitialization passed\n";
}

void CheckCameraAssets()
{
    constexpr auto path = "/Art/fe/environments/cameras/camera_idle.cam";
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    {
        auto sync = mscharged::LoadCameraAsset(path,"fixture");
        Require(sync->Data().m_uKeyCount==3 && sync->Data().cameraPos[2].x==2
            && sync->Data().fFOV[1]==41 && sync->Data().cameraRot[0].w==1, "Camera fixture decoding differs");
        mscharged::CameraAssetLibrary library;
        library.Insert(sync);
        mscharged::CameraAssetLoad good(path,"fixture"), bad("/invalid.cam","bad");
        ExpectThrow<std::logic_error>([&] { good.Result(); },"Pending camera read returned success");
        ServiceUntil([&] { return good.Ready() && bad.Ready(); });
        Require(good.Result()->Data().targetPos[1].y==8,"Asynchronous camera values differ");
        ExpectThrow<std::runtime_error>([&] { bad.Result(); },"Malformed camera read succeeded");
        Require(library.Find("FIXTURE")==sync,"Rejected camera load changed the library");
        mscharged::CameraAssetLoad inline_empty("/empty.bin","empty");
        Require(inline_empty.Ready(),"Inline empty camera completion was lost");
        ExpectThrow<std::runtime_error>([&] { inline_empty.Result(); },"Empty camera silently succeeded");
        ExpectThrow<std::runtime_error>([] { mscharged::CameraAssetLoad missing("/missing.cam","missing"); },"Missing camera request succeeded");
        ExpectThrow<std::runtime_error>([] { mscharged::LoadCameraAsset("/missing.cam","missing"); },"Missing synchronous camera succeeded");
        mscharged::CameraAssetLoad cancelled(path,"cancelled");
        cancelled.Cancel();cancelled.Cancel();cancelled.Service();
        Require(cancelled.Ready(),"Cancelled camera remains pending");
        ExpectThrow<std::runtime_error>([&] { cancelled.Result(); },"Cancelled camera returned an asset");
        { mscharged::CameraAssetLoad abandoned(path,"abandoned"); }
        nlServiceFileSystem();
        bool wrong_thread=false;
        std::thread worker([&] { try { good.Ready(); } catch(const std::logic_error&) { wrong_thread=true; } });worker.join();
        Require(wrong_thread,"Camera request accepted the wrong servicing thread");
        mscharged::CameraAssetLoad serviced(path,"serviced");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!serviced.Ready())
        {
            serviced.Service();
            Require(std::chrono::steady_clock::now()<deadline,"Camera Service timed out");
            SDL_Delay(1);
        }
        Require(serviced.Result()->Data().fFocalLength[2]==4,"Serviced camera result differs");
        mscharged::CameraAssetLoad shutdown(path,"shutdown");
        nlShutdownFileSystem();
        nlInitFileSystem();
        shutdown.Service();
        Require(shutdown.Ready(),"Camera request remained pending after file service restarted");
        ExpectThrow<std::runtime_error>([&] { shutdown.Result(); },"Camera shutdown failure was lost");
    }
    {
        FaultFile fault;
        const AuroraOverlayCallbacks callbacks{FaultFile::Open,FaultFile::Close,FaultFile::Read,FaultFile::Seek};
        aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile overlay{"/camera-fault.cam",&fault,128};
        aurora_dvd_overlay_files(&overlay,1,nullptr);
        struct ClearOverlay { ~ClearOverlay() { aurora_dvd_overlay_files(nullptr,0,nullptr); } } cleanup;
        for(auto mode:{FaultFile::Short,FaultFile::Error}) for(bool direct:{false,true})
        {
            fault.mode=mode;
            fault.entered=false; fault.finished=false;
            mscharged::CameraAssetLoad failed("/camera-fault.cam","failure");
            ExpectThrow<std::runtime_error>([&] {
                if(!direct) ServiceUntil([&] { return failed.Ready(); });
                else {
                    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                    while(!failed.Ready()) { failed.Service();SDL_Delay(1);Require(std::chrono::steady_clock::now()<end,"Camera read timed out"); }
                }
            },"Camera disc read error was swallowed");
            Require(failed.Ready() && fault.handles==0 && fault.entered && fault.finished,
                    "Failed camera read did not execute and retire its actual file");
            ExpectThrow<std::runtime_error>([&] { failed.Result(); },"Failed camera read returned an asset");
            mscharged::test::FinishDVDTestFaultCase(mode==FaultFile::Error);
            if(mode==FaultFile::Error) aurora_dvd_overlay_files(&overlay,1,nullptr);
        }
    }
    Require(StandardAllocator.TotalFreeMemory()==mem1 && VirtualAllocator.TotalFreeMemory()==mem2,
        "Camera file loading/cancel/shutdown leaked an arena");
    std::cout << mscharged::VerifyStartupCameraAssets() << '\n';
}

void CheckLifecycle()
{
    const auto initialized_free = StandardAllocator.TotalFreeMemory();
    alignas(32) std::array<unsigned char, 128> data{};
    Callback callback;
    auto file = Open("/large.bin");
    callback.Queue(file.get(), data.data(), 65, 65);
    nlShutdownFileSystem();
    Require(!nlFileSystemReady() && !nlAsyncReadsPending(file.get()) && !nlAsyncReadsPending(nullptr),
            "Shutdown left NL requests or manager alive");
    file.reset(); data.fill(0xa5); SDL_Delay(5); CheckGuard(data.data(), 0, data.size());
    Require(callback.calls == 0, "Shutdown invoked a cancelled completion callback");
    const auto shutdown_free = StandardAllocator.TotalFreeMemory();
    Require(shutdown_free > initialized_free, "NL manager/tail buffers were not freed");
    ExpectThrow<std::logic_error>([] { nlServiceFileSystem(); }, "Service after shutdown silently succeeded");
    for (unsigned i = 0; i < 3; ++i)
    {
        nlInitFileSystem(); nlInitFileSystem();
        Require(StandardAllocator.TotalFreeMemory() == initialized_free, "Repeated NL init leaked manager memory");
        mscharged::VerifyStartupFileReads();
        Whole pending;
        pending.Queue(i%2 ? &StandardAllocator : &VirtualAllocator);
        nlShutdownFileSystem();
        Require(!pending.calls && !pending.cancellations && !nlCancelEntireFileLoad(pending.handle, Whole::Cancel),
                "Repeated shutdown retained a whole-file callback/token");
        Require(StandardAllocator.TotalFreeMemory() == shutdown_free, "Repeated NL shutdown leaked memory");
    }
    nlInitFileSystem();
    {
        unsigned invoked = 0;
        Function<void(int)> functor([&](int) { ++invoked; });
        nlRegHandleDVDMessageCB(functor); // Native game-arena storage and a real cloned functor.
        Require(StandardAllocator.TotalFreeMemory() < initialized_free, "Callback functor did not use its game allocator");
    }
    nlShutdownFileSystem();
    Require(StandardAllocator.TotalFreeMemory() == shutdown_free, "Shutdown leaked a registered callback functor");
    nlInitFileSystem();
    std::cout << "NL pending shutdown, repeated initialization and arena recovery passed\n";
}
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 3, "Supply a synthetic Wii ISO and temporary data directory");
        const char* base = SDL_GetBasePath();
        const auto data = mscharged::PathUtf8(mscharged::PathFromUtf8(argv[2]) / "runtime-data");
        std::filesystem::create_directories(mscharged::PathFromUtf8(data));
        AuroraConfig config{};
        config.appName = "Charged NL file check";
        config.userPath = config.cachePath = data.c_str(); config.resourcesPath = base;
        config.desiredBackend = BACKEND_NULL; config.windowWidth = 320; config.windowHeight = 240;
        config.windowPosX = config.windowPosY = -1; config.logLevel = LOG_WARNING;
        config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64*1024*1024;
        Session session;
        const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Require(host.window != nullptr, "Aurora core initialization failed");
        mscharged::InitializeStartupOS(); nlInitMemory();
        ExpectThrow<std::runtime_error>([] { nlInitFileSystem(); }, "NL init without a disc was accepted");
        Require(!nlFileSystemReady(), "Failed init retained a manager");
        Require(aurora_dvd_open(argv[1]), "Synthetic Wii partition could not be mounted"); session.disc = true;
        mscharged::test::ConfigureDVDTestMedium(argv[1]);
        nlInitFileSystem();
        CheckSync(); CheckAsync(); CheckReentrancy(); CheckCancellation(); CheckPools(); CheckWholeFiles();
        CheckAsyncWholeFiles(); CheckWholeReentrancyAndCancellation();
        CheckDvdAllocationFailures(); CheckReadFailures(); CheckActiveWholeCancellationAndShutdown(); CheckLifecycle();
        CheckCameraAssets();
        mscharged::VerifyStartupFileReads();
        std::cout << mscharged::StartupFileSummary() << '\n';
        const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
        std::cout << mscharged::VerifyStartupWholeFileLoads() << '\n';
        std::cout << mscharged::VerifyStartupConfig() << '\n';
        {
            unsigned callbacks = 0;
            auto service = [&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (callbacks == 0)
                {
                    nlServiceFileSystem();
                    Require(std::chrono::steady_clock::now() < deadline, "Configuration callback timed out");
                    SDL_Delay(1);
                }
            };
            auto owner = std::make_unique<Config>(Config::ALLOCATE_LOW, 1024, 32);
            owner->LoadFromFileAsync("/ini/common.ini", Function<Config*>([&](Config* value) {
                Require(value->mLoaded && value->Get<int>("test/value", 0) == 7, "Configuration callback ran before parsing");
                ++callbacks;
                owner.reset(); // Completion must not touch the destroyed owner again.
            }));
            service();
            Require(!owner && callbacks == 1, "Configuration owner deletion failed");
            owner = std::make_unique<Config>(Config::ALLOCATE_HIGH, 1024, 32);
            owner->LoadFromFileAsync("/ini/common.ini", Function<Config*>([&](Config*) { ++callbacks; }));
            owner.reset();
            nlServiceFileSystem();
            Require(callbacks == 1, "Destroyed configuration received a callback");
            owner = std::make_unique<Config>(Config::ALLOCATE_LOW, 1024, 32);
            callbacks = 0;
            owner->LoadFromFileAsync("/ini/common.ini", Function<Config*>([&](Config*) { callbacks += 10; }));
            owner->LoadFromFileAsync("/empty.bin", Function<Config*>([&](Config* value) {
                Require(value->mLoaded, "Empty inline configuration did not complete");
                ++callbacks;
                value->LoadFromFileAsync("/ini/common.ini", Function<Config*>([&](Config*) { ++callbacks; }));
            }));
            Require(callbacks == 1 && !owner->mLoaded, "Inline reentrant load was lost");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (callbacks < 2)
            {
                nlServiceFileSystem(); SDL_Delay(1);
                Require(std::chrono::steady_clock::now() < deadline, "Reentrant configuration load timed out");
            }
            ExpectThrow<std::runtime_error>([&] { owner->LoadFromFile("/missing.ini"); }, "Missing configuration claimed success");
            Require(!owner->mLoaded, "Missing configuration retained loaded state");
            callbacks = 0;
            owner->LoadFromFileAsync("/ini/common.ini", Function<Config*>([&](Config*) { ++callbacks; throw std::runtime_error("config callback"); }));
            ExpectThrow<std::runtime_error>(service, "Configuration callback exception was swallowed");
            owner->LoadFromFile("/ini/common.ini");
            Require(owner->mLoaded, "Configuration could not recover after callback failure");
            owner->LoadFromFileAsync("/ini/common.ini", Function<Config*>([&](Config*) { ++callbacks; }));
            nlShutdownFileSystem(); owner.reset(); nlInitFileSystem();
            Require(callbacks == 1, "File shutdown invoked a configuration callback");
            std::cout << "Original configuration async completion, destruction, replacement, reentrancy, exceptions and shutdown passed\n";
        }
        Require(StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2,
                "Boot whole-file diagnostic leaked its callback-owned output");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "NL file check failed: " << error.what() << '\n'; return 1; }
}

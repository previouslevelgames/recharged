#include "runtime/startup_files.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include <dolphin/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
std::string summary;
struct FileCloser { void operator()(nlFile* file) const { nlClose(file); } };
using File = std::unique_ptr<nlFile, FileCloser>;
struct Directory
{
    DVDDir value{};
    bool open = false;
    ~Directory() { if (open) DVDCloseDir(&value); }
};

std::string FindFile(const std::string& path, unsigned depth = 0)
{
    if (depth > 64) throw std::runtime_error("Disc directory nesting exceeds the startup check limit.");
    Directory directory;
    directory.open = DVDOpenDir(path.c_str(), &directory.value);
    if (!directory.open) throw std::runtime_error("Cannot enumerate disc directory: " + path);
    DVDDirEntry entry{};
    while (DVDReadDir(&directory.value, &entry))
    {
        const auto name = path + (path == "/" ? "" : "/") + entry.name;
        if (entry.isDir)
        {
            const auto candidate = FindFile(name, depth+1);
            if (!candidate.empty()) return candidate;
        }
        else
        {
            File file(nlOpen(name.c_str()));
            if (file && nlFileSize(file.get(), nullptr)) return name;
        }
    }
    return {};
}

struct Completion
{
    nlFile* file = nullptr;
    void* buffer = nullptr;
    unsigned size = 0;
    std::thread::id caller = std::this_thread::get_id();
    unsigned calls = 0;
};

void Complete(nlFile* file, void* buffer, unsigned size, nlFileAsyncParam param)
{
    auto& result = *reinterpret_cast<Completion*>(param);
    if (result.file != file || result.buffer != buffer || result.size != size
        || result.caller != std::this_thread::get_id() || ++result.calls != 1)
        throw std::runtime_error("NL asynchronous callback arguments or thread are incorrect.");
}

struct GameFree { void operator()(void* data) const { if (data) nlFree(data); } };
struct WholeCompletion
{
    const std::vector<unsigned char>* expected = nullptr;
    std::unique_ptr<void, GameFree> output;
    std::thread::id caller = std::this_thread::get_id();
    unsigned calls = 0;
    static void Complete(void* data, unsigned long size, void* user)
    {
        auto& self = *static_cast<WholeCompletion*>(user);
        self.output.reset(data); // Adopt ownership before checks that can throw.
        if (!data || size != self.expected->size() || ++self.calls != 1
            || self.caller != std::this_thread::get_id()
            || reinterpret_cast<std::uintptr_t>(data)%32 != 0
            || std::memcmp(data, self.expected->data(), size) != 0)
            throw std::runtime_error("NL whole-file startup bytes, size, alignment or callback thread differ.");
    }
};

struct PendingWhole
{
    unsigned handle = 0;
    ~PendingWhole() { if (handle) nlCancelEntireFileLoad(handle, nullptr); }
};
}

namespace mscharged
{
void ResetStartupFiles()
{
    nlShutdownFileSystem(); // Join every DVD worker before closing the disc/arenas.
    summary.clear();
}

void VerifyStartupFileReads()
{
    if (!nlFileSystemReady()) throw std::runtime_error("Original nlInitFileSystem did not initialize.");
    const auto path = FindFile("/");
    if (path.empty()) throw std::runtime_error("Disc contains no nonempty file for the NL read check.");
    // These outlive the file: closing it drains pending writes even on failure.
    alignas(32) std::array<unsigned char, 160> sync{}, async{};
    Completion completion;
    File file(nlOpen(path.c_str()));
    if (!file) throw std::runtime_error("Cannot reopen the startup check file.");
    const auto size = nlFileSize(file.get(), nullptr);
    const auto count = std::min(size, 127u);
    sync.fill(0xa5); async.fill(0xa5);
    nlRead(file.get(), sync.data(), count, count);
    nlSeek(file.get(), 0, 0);
    completion.file = file.get(); completion.buffer = async.data(); completion.size = count;
    nlReadAsync(file.get(), async.data(), count, Complete,
                reinterpret_cast<nlFileAsyncParam>(&completion), count);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completion.calls)
    {
        nlServiceFileSystem();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("NL asynchronous startup read timed out.");
        if (!completion.calls) SDL_Delay(1);
    }
    if (std::memcmp(sync.data(), async.data(), count) != 0 || nlAsyncReadsPending(file.get()))
        throw std::runtime_error("NL synchronous and asynchronous disc reads differ.");
    for (unsigned i = count; i < sync.size(); ++i)
        if (sync[i] != 0xa5 || async[i] != 0xa5)
            throw std::runtime_error("NL startup read overwrote the destination boundary.");
    std::uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < count; ++i) hash = (hash ^ sync[i]) * 16777619u;
    std::ostringstream text;
    text << "Original nlInitFileSystem completed; NL sync/async reads verified: "
         << path << " (" << count << " of " << size << " bytes; FNV-1a 0x"
         << std::hex << std::setfill('0') << std::setw(8) << hash
         << "); callback on servicing thread.";
    summary = text.str();
}

std::string VerifyStartupWholeFileLoads()
{
    std::ostringstream text;
    text << "Native NL whole-file async loads verified (bytes only): ";
    bool first = true;
    for (const auto* path : {"/ini/common.ini", "/ini/datetime.ini"})
    {
        // These are actual early Initialize resources. Never hide a missing
        // boot file behind a generated replacement or a successful callback.
        File file(nlOpen(path));
        if (!file) throw std::runtime_error(std::string("Missing boot INI: ") + path);
        const auto size = nlFileSize(file.get(), nullptr);
        if (!size || size > 4*1024*1024)
            throw std::runtime_error(std::string("Boot INI is empty or exceeds the diagnostic limit: ") + path);
        std::vector<unsigned char> expected(size);
        nlRead(file.get(), expected.data(), size, size);
        file.reset();
        WholeCompletion completion;
        completion.expected = &expected;
        PendingWhole pending;
        pending.handle = nlLoadEntireFileAsync(path, WholeCompletion::Complete, &completion,
            32, AllocateStart, nullptr, 0, nullptr);
        if (!pending.handle) throw std::runtime_error(std::string("Boot INI async load was not queued: ") + path);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!completion.calls)
        {
            nlServiceFileSystem();
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error(std::string("Boot INI async load timed out: ") + path);
            if (!completion.calls) SDL_Delay(1);
        }
        std::uint32_t hash = 2166136261u;
        for (auto byte : expected) hash = (hash ^ byte) * 16777619u;
        if (!first) text << "; ";
        first = false;
        text << path << " (" << std::dec << size << " bytes; FNV-1a 0x"
             << std::hex << std::setfill('0') << std::setw(8) << hash << ')';
    }
    text << "; callbacks on servicing thread. Original INI parsing is not executed.";
    return text.str();
}

std::string StartupFileSummary()
{
    return summary.empty() ? "Original NL disc reads have not been verified." : summary;
}
}

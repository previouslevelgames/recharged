#include "runtime/game_config.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <chrono>
#include <map>
#include <stdexcept>
#include <thread>

namespace
{
Config* global = nullptr;
struct Load
{
    Config* owner;
    Function<Config*> callback;
    unsigned int handle = 0;
    bool completed = false;
};
std::map<Config*, std::shared_ptr<Load>> loads;

void Complete(void* data, unsigned long size, void* context)
{
    mscharged::ConfigBuffer buffer(data);
    auto* request = static_cast<Load*>(context);
    const auto found = loads.find(request->owner);
    if (found == loads.end() || found->second.get() != request)
        throw std::logic_error("Configuration callback lost its owner");
    auto load = found->second;
    loads.erase(found); // Callback may delete its owner or start another load.
    load->completed = true;
    if (size > 16 * 1024 * 1024) throw std::length_error("Configuration file exceeds 16 MiB");
    load->owner->LoadFromBuffer(static_cast<const char*>(data), static_cast<int>(size));
    buffer.reset();
    if (load->callback) load->callback(load->owner);
}

void CheckFile(const char* filename)
{
    if (!filename || !nlFileExists(filename)) throw std::runtime_error("Configuration file is missing");
    std::unique_ptr<nlFile> file(nlOpen(filename));
    if (!file || nlFileSize(file.get(), nullptr) > 16 * 1024 * 1024)
        throw std::length_error("Configuration file exceeds 16 MiB");
}
}

namespace mscharged
{
void FreeConfigBuffer::operator()(void* buffer) const noexcept { nlFree(buffer); }
OriginalConfig::OriginalConfig()
{
    if (global || !gMemoryInitialized) throw std::logic_error("Invalid global configuration lifetime");
    config_ = std::make_unique<Config>(Config::ALLOCATE_LOW, 0x2800, 0x400);
    global = config_.get();
}
OriginalConfig::~OriginalConfig() { global = nullptr; config_.reset(); }
Config& GlobalGameConfig()
{
    if (!global) throw std::logic_error("Original global configuration is not initialized");
    return *global;
}
void ValidateConfigInput(const char* data, int size)
{
    if (size < 0 || size > 16 * 1024 * 1024 || (size && !data))
        throw std::invalid_argument("Invalid configuration buffer");
    // SimpleLineReader has a 256-byte line buffer. Do not silently split an
    // oversized physical line into unrelated configuration entries.
    unsigned column = 0;
    for (int i = 0; i < size; ++i)
    {
        if (!data[i]) throw std::invalid_argument("Configuration contains an embedded NUL");
        if (data[i] == '\n') column = 0;
        else if (++column > 254) throw std::length_error("Configuration line exceeds 254 bytes");
    }
}
void CancelConfigLoad(Config& config)
{
    auto found = loads.find(&config);
    if (found == loads.end()) return;
    auto load = found->second;
    loads.erase(found);
    if (load->handle) nlCancelEntireFileLoad(load->handle, nullptr);
}
void LoadGameConfig(Config& config, const char* filename)
{
    CancelConfigLoad(config);
    config.mLoaded = false;
    CheckFile(filename);
    unsigned long size = 0;
    ConfigBuffer data(nlLoadEntireFile(filename, &size, 0x20, AllocateEnd, nullptr, 0, nullptr));
    if (size > 16 * 1024 * 1024) throw std::length_error("Configuration file exceeds 16 MiB");
    config.LoadFromBuffer(static_cast<const char*>(data.get()), static_cast<int>(size));
}
void LoadGameConfigAsync(Config& config, const char* filename, const Function<Config*>& callback)
{
    CancelConfigLoad(config);
    config.mLoaded = false;
    CheckFile(filename);
    auto load = std::make_shared<Load>();
    load->owner = &config;
    load->callback = callback;
    loads.emplace(&config, load);
    try
    {
        load->handle = nlLoadEntireFileAsync(filename, Complete, load.get(), 0x20, AllocateEnd, nullptr, 0, nullptr);
        if (!load->handle && !load->completed) throw std::runtime_error("Configuration load did not start");
    }
    catch (...)
    {
        auto found = loads.find(&config);
        if (found != loads.end() && found->second == load) loads.erase(found);
        throw;
    }
}
std::string VerifyStartupConfig()
{
    const auto free1 = StandardAllocator.TotalFreeMemory(), free2 = VirtualAllocator.TotalFreeMemory();
    unsigned tags = 0;
    {
        OriginalConfig session;
        Config& config = Config::Global();
        config.LoadFromFileAsync("/ini/common.ini", Function<Config*>());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!config.mLoaded)
        {
            nlServiceFileSystem();
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Configuration load timed out");
            std::this_thread::yield();
        }
        for (unsigned i = 0; i < config.mTvpCapacity; ++i) if (config.mTvpHash[i].tag) ++tags;
        Config synchronous(Config::ALLOCATE_HIGH, 0x2800, 0x400);
        synchronous.LoadFromFile("/ini/common.ini");
        for (unsigned i = 0; i < config.mTvpCapacity; ++i)
        {
            const auto& a = config.mTvpHash[i];
            if (!a.tag) continue;
            if (!synchronous.Exists(a.tag)) throw std::runtime_error("Sync/async configuration tags differ");
            const auto& b = synchronous.FindTvp(a.tag);
            bool same = a.type == b.type;
            if (same) switch (a.type)
            {
            case CONFIG_BOOL: same = a.value.boolValue == b.value.boolValue; break;
            case CONFIG_INT: same = a.value.intValue == b.value.intValue; break;
            case CONFIG_FLOAT: same = a.value.floatValue == b.value.floatValue; break;
            case CONFIG_STRING: same = strcmp(a.value.stringValue, b.value.stringValue) == 0; break;
            }
            if (!same) throw std::runtime_error("Sync/async configuration values differ");
        }
    }
    if (free1 != StandardAllocator.TotalFreeMemory() || free2 != VirtualAllocator.TotalFreeMemory())
        throw std::runtime_error("Configuration did not recover both game arenas");
    return "Original boot configuration parsed through sync/async NL loading: " + std::to_string(tags)
        + " matching typed entries; both arenas recovered.";
}
}

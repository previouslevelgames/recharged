#include "dvd_fixture_medium.h"
#include "NL/nlBundleFile.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
namespace
{
unsigned checks = 0;
void Check(bool value, const char* reason)
{ ++checks; if (!value) throw std::runtime_error(reason); }
template<class F> void Reject(F&& fn)
{ ++checks; try { fn(); } catch (const std::exception&) { return; } throw std::runtime_error("Expected native failure was accepted"); }
std::vector<std::uint8_t> Read(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    Check(bool(file), "Cannot open independent raw bytes");
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Host
{
    bool dvd = false, files = false;
    std::vector<std::uint64_t> standard = std::vector<std::uint64_t>(4*1024*1024);
    std::vector<std::uint64_t> virtual_arena = std::vector<std::uint64_t>(8*1024*1024);
    explicit Host(const char* path)
    {
        Check(SDL_Init(0), "SDL host initialization failed");
        aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size = 0;
        mscharged::InitializeStartupOS();
        Check(aurora_dvd_open(path), "Cannot mount generated/owned Wii partition"); dvd = true;
        StandardAllocator.Initialize(standard.data(), standard.size()*8);
        VirtualAllocator.Initialize(virtual_arena.data(), virtual_arena.size()*8);
        gMemoryInitialized = 1;
        nlInitFileSystem(); files = true; mscharged::test::ConfigureDVDTestMedium(path);
    }
    ~Host()
    {
        if (files) mscharged::ResetStartupFiles();
        if (dvd) aurora_dvd_close();
        mscharged::ResetStartupMemory(); AuroraOSShutdown(); SDL_Quit();
    }
};
struct Memory
{
    unsigned standard = StandardAllocator.TotalFreeMemory();
    unsigned virtual_arena = VirtualAllocator.TotalFreeMemory();
    unsigned standard_largest = StandardAllocator.LargestFreeBlock();
    unsigned virtual_largest = VirtualAllocator.LargestFreeBlock();
    void Same() const
    {
        Check(StandardAllocator.TotalFreeMemory() == standard && VirtualAllocator.TotalFreeMemory() == virtual_arena,
            "Bundle native ownership did not recover both game arenas");
        Check(StandardAllocator.LargestFreeBlock() == standard_largest && VirtualAllocator.LargestFreeBlock() == virtual_largest,
            "Bundle native ownership fragmented the recovered game arenas");
    }
};
void Pump(const std::function<bool()>& done)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done())
    {
        nlServiceFileSystem();
        Check(std::chrono::steady_clock::now() < until, "Original BundleFile callback timed out");
        if (!done()) SDL_Delay(1);
    }
}
struct Record { std::uint32_t hash, block, size; std::string name; };
struct Oracle
{
    std::uint32_t sector, count, directory, data;
    std::vector<Record> records;
    std::vector<std::uint8_t> raw;
    Oracle(const char* bytes, const char* manifest) : raw(Read(bytes))
    {
        std::ifstream file(manifest);
        file >> sector >> count >> directory >> data;
        for (unsigned i=0; i<count; ++i)
        {
            Record record;
            file >> record.hash >> record.block >> record.size >> record.name;
            records.push_back(record);
        }
        Check(bool(file), "Cannot parse Python struct.unpack oracle");
    }
    void Directory(BundleFile& bundle) const
    {
        Check(bundle.nSectorSize == sector && bundle.nNumFiles == count
            && bundle.nDirectoryOffsetInSectors == directory && bundle.nDataOffsetInSectors == data,
            "Original header differs from independent Wii word oracle");
        Check(bundle.GetNumFiles() == records.size(), "Original bundle count differs");
        for (unsigned i=0; i<count; ++i)
        {
            const auto& expected = records[i];
            BundleFileDirectoryEntry entry{};
            Check(bundle.GetFileInfoByIndex(i, &entry), "Original index lookup failed");
            Check(entry.m_hash == expected.hash && entry.m_blockNumber == expected.block && entry.m_length == expected.size,
                "Original directory order/words differ from raw oracle");
            Check(bundle.FindHashIndex(expected.hash, false) <= i, "Original largest/first matching hash order changed");
        }
        BundleFileDirectoryEntry absent{};
        Check(!bundle.GetFileInfoByIndex(count, &absent) && !bundle.GetFileInfo(0x12345432UL, &absent, false),
            "Original absent lookup changed");
    }
    void Bytes(unsigned index, const std::vector<std::uint8_t>& output, unsigned count) const
    {
        const auto offset = std::size_t(records[index].block)*sector;
        Check(offset+count <= raw.size(), "Oracle payload exceeds raw file");
        Check(std::memcmp(output.data(), raw.data()+offset, count) == 0, "Original NL read differs from raw authored bytes");
        for (unsigned i=count; i<output.size(); ++i) Check(output[i] == 0xa5, "Original bundle read exceeded its logical destination");
    }
};
struct Callback
{
    unsigned calls = 0;
    void* buffer = nullptr;
    unsigned long size = 0;
    std::thread::id thread = std::this_thread::get_id();
    std::function<void()> action;
    static void Complete(void* bytes, unsigned long size, BundleAsyncParam context)
    {
        auto& self = *reinterpret_cast<Callback*>(context);
        Check(context > UINT32_MAX, "Bundle context did not exercise addresses above 4 GiB");
        Check(std::this_thread::get_id() == self.thread, "Original callback ran on a DVD worker thread");
        Check(bytes == self.buffer && size == self.size, "Original callback lost its pointer-sized context/buffer/count");
        ++self.calls;
        if (self.action) self.action();
    }
    BundleAsyncParam Param() { return reinterpret_cast<BundleAsyncParam>(this); }
};
struct OpenCallback
{
    BundleFile* bundle;
    unsigned calls = 0;
    std::thread::id thread = std::this_thread::get_id();
    static void Complete(void* bytes, unsigned long size, BundleAsyncParam context)
    {
        auto& self = *reinterpret_cast<OpenCallback*>(context);
        Check(context > UINT32_MAX && std::this_thread::get_id() == self.thread,
            "Original open callback context/thread changed");
        Check(bytes == self.bundle->m_pDirectory && size == self.bundle->nNumFiles*12,
            "Original open callback directory buffer/count changed");
        ++self.calls;
    }
    void Start(const char* path)
    {
        Check(bundle->OpenAsync(path, Complete, reinterpret_cast<BundleAsyncParam>(this), true), "Original async open failed");
        Check(calls == 0, "Nonzero header fabricated inline readiness");
    }
};
void Sync(const char* path, const Oracle& oracle)
{
    const Memory memory;
    {
        BundleFile bundle;
        Check(bundle.Open(path, true), "Original sync bundle open failed"); oracle.Directory(bundle);
        Check(std::string(bundle.m_pFilename) == path, "Original requested filename copy changed");
        for (unsigned i=0; i<oracle.count; ++i)
        {
            std::vector<std::uint8_t> output(oracle.records[i].size+35, 0xa5);
            bundle.ReadFileByIndex(i, output.data(), 0); // Original method deliberately ignores its size argument.
            oracle.Bytes(i, output, oracle.records[i].size);
            if (oracle.records[i].name != "-")
            {
                std::string canonical = oracle.records[i].name;
                for (char& c : canonical) { if (c == '/') c = '\\'; else if (c >= 'a' && c <= 'z') c -= 32; }
                BundleFileDirectoryEntry entry{};
                Check(bundle.GetFileInfo(canonical.c_str(), &entry, false) && entry.m_hash == oracle.records[i].hash,
                    "Original filename lower-case/slash hashing changed");
            }
        }
        bundle.Close(); bundle.Close();
        Check(!bundle.m_pFile && !bundle.m_pDirectory && !bundle.m_pFilename, "Original Close did not clear native ownership");
    }
    memory.Same();
    for (bool end : {false, true})
    {
        CurrentAllocator = &VirtualAllocator;
        auto* bundle = new (128, end) BundleFile;
        Check(reinterpret_cast<std::uintptr_t>(bundle)%128 == 0, "Original aligned BundleFile allocation changed");
        Check(bundle->Open(path, true), "Aligned game-owned bundle did not open");
        CurrentAllocator = &StandardAllocator;
        delete bundle;
        memory.Same();
    }
    auto* ordinary = new BundleFile;
    Check(reinterpret_cast<std::uintptr_t>(ordinary) > UINT32_MAX, "Bundle object did not exercise native pointer width");
    delete ordinary; memory.Same();
}
void Async(const char* path, const Oracle& oracle)
{
    const Memory memory;
    {
        BundleFile bundle; OpenCallback opened{&bundle}; opened.Start(path);
        Pump([&]{ return opened.calls != 0; });
        Check(opened.calls == 1 && !bundle.m_pOpenCallback && !bundle.m_openUserParam, "Original open callback cleanup changed");
        oracle.Directory(bundle);
        for (unsigned i=0; i<oracle.count; ++i)
        {
            const auto& record = oracle.records[i];
            for (unsigned method=0; method<3; ++method)
            {
                std::vector<std::uint8_t> output(record.size+35, 0xa5);
                Callback callback; callback.buffer = output.data(); callback.size = record.size;
                if (method == 0 && record.name != "-") bundle.ReadFileAsync(record.name.c_str(), output.data(), record.size, Callback::Complete, callback.Param());
                else if (method == 1) bundle.ReadFileAsync(record.hash, output.data(), record.size, Callback::Complete, callback.Param());
                else bundle.ReadFileAsyncByIndex(i, output.data(), record.size, Callback::Complete, callback.Param());
                if (!record.size) Check(callback.calls == 1, "Original zero-byte read did not complete inline");
                Pump([&]{ return callback.calls != 0; });
                Check(callback.calls == 1 && !bundle.m_nativeReadContexts, "Original read completion retained native contexts");
                oracle.Bytes(i, output, record.size);
            }
        }
        if (oracle.count)
        {
            std::vector<std::uint8_t> output(35, 0xa5);
            Callback callback; callback.buffer = output.data(); callback.size = 0;
            callback.action = []{ throw std::runtime_error("Native zero-byte callback exception"); };
            Reject([&]{ bundle.ReadFileAsyncByIndex(0, output.data(), 0, Callback::Complete, callback.Param()); });
            Check(callback.calls == 1 && !bundle.m_nativeReadContexts, "Inline callback exception retained/double-freed metadata");
            const auto nonzero = std::find_if(oracle.records.begin(), oracle.records.end(), [](const auto& record){return record.size != 0;});
            if (nonzero != oracle.records.end())
            {
                unsigned index = nonzero-oracle.records.begin();
                output.assign(nonzero->size+35, 0xa5); callback.calls=0; callback.size=nonzero->size; callback.buffer=output.data();
                bundle.ReadFileAsyncByIndex(index, output.data(), callback.size, Callback::Complete, callback.Param());
                Reject([&]{ Pump([&]{return callback.calls != 0;}); });
                Check(callback.calls == 1 && !bundle.m_nativeReadContexts && !nlAsyncReadsPending(nullptr),
                    "Deferred callback exception retained contexts/raw workers");
            }
        }
    }
    memory.Same();
    if (!oracle.count) return;
    for (unsigned size : {0u, std::min(oracle.records[0].size, 1u)})
    {
        auto* bundle = new (32, true) BundleFile; Check(bundle->Open(path, false), "Destroy-in-callback open failed");
        std::vector<std::uint8_t> output(size+35, 0xa5); Callback callback; callback.buffer=output.data(); callback.size=size;
        callback.action=[&]{ bundle->Close(); delete bundle; bundle=nullptr; };
        bundle->ReadFileAsyncByIndex(0, output.data(), size, Callback::Complete, callback.Param());
        Pump([&]{return callback.calls != 0;});
        Check(!bundle && callback.calls == 1 && !nlAsyncReadsPending(nullptr), "Original final callback could not close/destroy its bundle");
        memory.Same();
    }
}
struct Overlay
{
    enum Mode { Error, Short, Blocked } mode;
    const std::vector<std::uint8_t>& bytes;
    std::size_t selected;
    std::atomic<unsigned> handles{0}; std::atomic<bool> entered{false};
    std::mutex mutex; std::condition_variable condition; bool released=false;
    struct Handle { Overlay* owner; std::size_t at=0; };
    static void* Open(void* context) { auto* self=static_cast<Overlay*>(context); ++self->handles; return new Handle{self}; }
    static void Close(void* context) { auto* handle=static_cast<Handle*>(context); --handle->owner->handles; delete handle; }
    static std::int64_t Seek(void* context, std::int64_t at, std::int32_t origin)
    {
        auto& handle=*static_cast<Handle*>(context);
        if (origin || at<0 || std::size_t(at)>handle.owner->bytes.size()) return -1;
        return handle.at=at;
    }
    static std::int64_t Read(void* context, std::uint8_t* out, std::size_t size)
    {
        auto& handle=*static_cast<Handle*>(context); auto& self=*handle.owner;
        if (handle.at == self.selected)
        {
            self.entered=true;
            if (self.mode == Error) return -1;
            if (self.mode == Short) return 0;
            std::unique_lock lock(self.mutex);
            if (!self.condition.wait_for(lock, std::chrono::seconds(5), [&]{return self.released;})) return -1;
        }
        auto count=std::min(size, self.bytes.size()-handle.at);
        std::memcpy(out, self.bytes.data()+handle.at, count); handle.at+=count; return count;
    }
    Overlay(const char* path, const Oracle& oracle, Mode mode, std::size_t selected)
      : mode(mode), bytes(oracle.raw), selected(selected)
    {
        const AuroraOverlayCallbacks callbacks{Open,Close,Read,Seek}; aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{path,this,bytes.size()}; aurora_dvd_overlay_files(&file,1,nullptr);
    }
    void Release() { std::lock_guard lock(mutex); released=true; condition.notify_all(); }
    ~Overlay() { Release(); aurora_dvd_overlay_files(nullptr,0,nullptr); if (handles) std::terminate(); }
};
void Lifecycle(const char* path, const Oracle& oracle, Host& host)
{
    const Memory memory;
    const auto first_data=std::find_if(oracle.records.begin(),oracle.records.end(),[](const auto& record){return record.size!=0;});
    if (first_data != oracle.records.end())
    {
        BundleFile cancelled, retained;
        Check(cancelled.Open(path,false) && retained.Open(path,false),"Parallel original bundles did not open");
        const unsigned index=first_data-oracle.records.begin();
        std::vector<std::uint8_t> a(first_data->size+35,0xa5), b(first_data->size+35,0xa5);
        Callback x,y; x.buffer=a.data(); y.buffer=b.data(); x.size=y.size=first_data->size;
        cancelled.ReadFileAsyncByIndex(index,a.data(),x.size,Callback::Complete,x.Param());
        retained.ReadFileAsyncByIndex(index,b.data(),y.size,Callback::Complete,y.Param());
        cancelled.Close(); Pump([&]{return y.calls!=0;});
        Check(!x.calls && y.calls==1 && !retained.m_nativeReadContexts,"Closing one bundle changed a different original request");
        oracle.Bytes(index,b,first_data->size); retained.Close(); memory.Same();
    }
    for (unsigned stage=0; stage<3; ++stage)
    {
        const auto found=std::find_if(oracle.records.begin(),oracle.records.end(),[](const auto& record){return record.size!=0;});
        if (stage == 2 && found == oracle.records.end()) continue;
        const unsigned index=found-oracle.records.begin();
        const auto offset=stage==0?0:stage==1?std::size_t(oracle.directory)*oracle.sector:std::size_t(found->block)*oracle.sector;
        for (auto mode : {Overlay::Error, Overlay::Short, Overlay::Blocked})
        {
            Overlay overlay(path, oracle, mode, offset); BundleFile bundle; OpenCallback opened{&bundle}; Callback read;
            std::vector<std::uint8_t> output(stage==2?found->size+35:35,0xa5);
            if (stage<2) opened.Start(path);
            else
            {
                Check(bundle.Open(path,false), "Fault-stage original sync open failed");
                read.buffer=output.data(); read.size=found->size;
                bundle.ReadFileAsyncByIndex(index,output.data(),read.size,Callback::Complete,read.Param());
            }
            if (mode == Overlay::Blocked)
            {
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (!overlay.entered) { nlServiceFileSystem(); Check(std::chrono::steady_clock::now()<until,"Worker did not enter cancellation range"); SDL_Delay(1); }
                std::thread release([&]{ SDL_Delay(10); overlay.Release(); });
                bundle.Close(); release.join();
            }
            else { Reject([&]{Pump([]{return false;});}); bundle.Close(); }
            Check(!opened.calls && !read.calls && !nlAsyncReadsPending(nullptr), "Failed/cancelled original request fabricated readiness");
            Check(!bundle.m_nativeReadContexts && overlay.handles==0, "Cancelled bundle retained callback/worker ownership");
            memory.Same();
            Check(overlay.entered, "Independent bundle fault/cancellation never entered its actual provider");
            mscharged::test::FinishDVDTestFaultCase(mode==Overlay::Error);
        }
    }
    // Original synchronous header allocation must unwind on real raw read failure.
    for (auto mode : {Overlay::Error, Overlay::Short})
    {
        Overlay overlay(path,oracle,mode,0); BundleFile bundle;
        Reject([&]{bundle.Open(path,true);}); bundle.Close(); memory.Same();
        Check(overlay.entered && !overlay.handles, "Synchronous bundle fault did not perform I/O and close");
        mscharged::test::FinishDVDTestFaultCase(mode==Overlay::Error);
    }
    if (oracle.count)
    {
        BundleFile bundle; Check(bundle.Open(path,false), "Shutdown-stage open failed");
        std::vector<std::uint8_t> output(oracle.records[0].size+35,0xa5); Callback callback;
        callback.buffer=output.data(); callback.size=oracle.records[0].size;
        if (callback.size)
        {
            bundle.ReadFileAsyncByIndex(0,output.data(),callback.size,Callback::Complete,callback.Param());
            nlShutdownFileSystem(); host.files=false; bundle.Close();
            Check(callback.calls==0 && !bundle.m_nativeReadContexts, "Shutdown fabricated original completion or retained metadata");
            nlInitFileSystem(); host.files=true; memory.Same();
        }
    }
}
void Capacity(const char* path, const Oracle& oracle)
{
    if (oracle.raw.size()<32 || !oracle.count) return;
    const auto nonzero=std::find_if(oracle.records.begin(),oracle.records.end(),[](const auto& record){return record.size!=0;});
    if (nonzero==oracle.records.end()) return;
    const Memory memory;
    {
        BundleFile bundle; Check(bundle.Open(path,false),"Capacity-stage open failed");
        const Memory open_memory;
        std::array<std::array<std::uint8_t,32>,64> buffers{};
        for (auto& buffer:buffers) { nlSeek(bundle.m_pFile,0,0); nlReadAsync(bundle.m_pFile,buffer.data(),32,nullptr,0,32); }
        std::vector<std::uint8_t> output(nonzero->size+35,0xa5); Callback callback;
        callback.buffer=output.data(); callback.size=nonzero->size;
        Reject([&]{bundle.ReadFileAsyncByIndex(nonzero-oracle.records.begin(),output.data(),callback.size,Callback::Complete,callback.Param());});
        Check(!callback.calls && !bundle.m_nativeReadContexts,"Rejected NL admission leaked contexts or called success");
        nlCancelPendingAsyncReads(bundle.m_pFile,nullptr); open_memory.Same();
        CurrentAllocator=&VirtualAllocator;
        auto* fill=nlMalloc(VirtualAllocator.LargestFreeBlock()-32,8,false);
        Reject([&]{bundle.ReadFileAsyncByIndex(nonzero-oracle.records.begin(),output.data(),callback.size,Callback::Complete,callback.Param());});
        nlFree(fill); CurrentAllocator=&StandardAllocator;
        Check(!callback.calls && !bundle.m_nativeReadContexts && !nlAsyncReadsPending(nullptr),"Context OOM fabricated completion/retained ownership");
        open_memory.Same();
    }
    memory.Same();
    if (oracle.count >= 8)
    {
        // Leave enough native arena space for the exact original 16-byte sync
        // header allocation, but less than the original 96-byte directory.
        for (bool async : {false,true})
        {
            CurrentAllocator=&VirtualAllocator;
            auto* fill=nlMalloc(VirtualAllocator.LargestFreeBlock()-88,8,false);
            BundleFile bundle; OpenCallback opened{&bundle};
            Reject([&]{
                if (async) { opened.Start(path); Pump([&]{return opened.calls!=0;}); }
                else bundle.Open(path,false);
            });
            Check(bundle.nNumFiles==oracle.count && !bundle.m_pDirectory && !opened.calls,
                "Directory OOM did not preserve the original parsed header/failure boundary");
            bundle.Close(); nlFree(fill); CurrentAllocator=&StandardAllocator;
            Check(!nlAsyncReadsPending(nullptr),"Directory OOM retained original file work"); memory.Same();
        }
    }
}
}
int main(int argc, char** argv)
{
    try
    {
        std::cout<<std::unitbuf;
        Check(argc==6 || (argc==5 && std::string(argv[1])=="--dump"),"Supply disc/raw/manifest/NL-path/mode or --dump disc NL-path raw-output");
        if (argc==5)
        {
            Host host(argv[2]); unsigned long size=0;
            std::unique_ptr<void,void(*)(void*)> bytes(nlLoadEntireFile(argv[3],&size,32,AllocateEnd,nullptr,0,&VirtualAllocator),nlFree);
            std::ofstream output(argv[4],std::ios::binary); output.write(static_cast<const char*>(bytes.get()),size);
            Check(bool(output),"Cannot freeze owned raw bundle"); std::cout<<"Owned raw NL bytes "<<size<<"\n"; return 0;
        }
        Host host(argv[1]); const std::string mode=argv[5]; const char* path=argv[4]; const Memory memory;
        if (mode=="missing")
        {
            BundleFile sync; Check(!sync.Open(path,true),"Missing original bundle returned success"); sync.Close();
            BundleFile async; OpenCallback opened{&async};
            Check(!async.OpenAsync(path,OpenCallback::Complete,reinterpret_cast<BundleAsyncParam>(&opened),true)
                && !opened.calls && !nlAsyncReadsPending(nullptr),"Missing async original bundle fabricated readiness"); async.Close();
        }
        else if (mode=="malformed")
        {
            BundleFile sync; Reject([&]{sync.Open(path,true);}); sync.Close();
            BundleFile async; OpenCallback opened{&async};
            Reject([&]{
                Check(async.OpenAsync(path,OpenCallback::Complete,reinterpret_cast<BundleAsyncParam>(&opened),true),"Malformed file was absent");
                Pump([&]{return opened.calls!=0;});
            }); async.Close();
            Check(!opened.calls&&!nlAsyncReadsPending(nullptr),"Malformed serialized range became ready");
        }
        else
        {
            Oracle oracle(argv[2],argv[3]); Sync(path,oracle); Async(path,oracle);
            if (mode=="lifecycle") { Lifecycle(path,oracle,host); Capacity(path,oracle); }
        }
        memory.Same();
        Check(!nlAsyncReadsPending(nullptr),"Bundle fixture retained raw worker ownership");
        std::cout<<"Original BundleFile "<<mode<<" passed "<<checks<<" checks\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr<<"Original BundleFile: "<<error.what()<<"\n"; return 1; }
}

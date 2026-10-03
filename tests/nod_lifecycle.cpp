#include <nod.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
constexpr std::uint64_t GroupSize=64*32768;
void Check(bool good,const char* message){if(!good)throw std::runtime_error(message);}
struct Stream
{
    std::vector<char> bytes;
    std::mutex mutex;
    std::condition_variable changed;
    bool gate=false,armed=false,blocked=false,released=false,freeStarted=false,freeDone=false;
    unsigned reads=0,closeCount=0;
    std::thread::id closeThread;
};
std::int64_t Read(void* data,std::uint64_t offset,void* out,std::size_t length)
{
    auto& stream=*static_cast<Stream*>(data);
    std::unique_lock lock(stream.mutex);
    ++stream.reads;
    if(stream.gate&&(offset/GroupSize)%2)
    {
        // Before arming, reject read-ahead only. This lets requested groups 0
        // and 2 finish even though the FFI serializes every stream callback.
        if(!stream.armed)return -1;
        stream.blocked=true;stream.changed.notify_all();
        stream.changed.wait(lock,[&]{return stream.released;});
    }
    if(offset>=stream.bytes.size())return 0;
    length=std::min(length,stream.bytes.size()-static_cast<std::size_t>(offset));
    std::memcpy(out,stream.bytes.data()+offset,length);
    return static_cast<std::int64_t>(length);
}
std::int64_t Length(void* data){return static_cast<Stream*>(data)->bytes.size();}
void Close(void* data)
{
    auto& stream=*static_cast<Stream*>(data);std::lock_guard lock(stream.mutex);
    ++stream.closeCount;stream.closeThread=std::this_thread::get_id();stream.changed.notify_all();
}
struct Reader
{
    Stream& stream;
    NodHandle* handle=nullptr;
    ~Reader()
    {
        {std::lock_guard lock(stream.mutex);stream.released=true;stream.changed.notify_all();}
        if(handle)nod_free(handle);
        std::unique_lock lock(stream.mutex);
        if(!stream.changed.wait_for(lock,3s,[&]{return stream.closeCount!=0;}))
        {
            // A failing dependency must never turn this test's callback state
            // into a dangling pointer while an escaped worker is still alive.
            std::cerr<<"FAILED: nod callback storage still live after cleanup timeout\n";
            std::_Exit(1);
        }
    }
    void CloseNow(){nod_free(handle);handle=nullptr;}
};
void Run(const std::vector<char>& bytes,unsigned threads,bool threading)
{
    Stream stream;stream.bytes=bytes;stream.gate=threading&&threads==2;
    NodDiscStream callbacks{&stream,Read,Length,Close};NodDiscOptions options{};options.preloader_threads=threads;
    NodHandle* disc=nullptr;
    Check(nod_disc_open_stream(&callbacks,&options,&disc)==NOD_RESULT_OK,"Cannot open generated ISO through nod FFI callbacks");
    Reader owner{stream,disc};
    auto read=[&](std::int64_t position)
    {
        unsigned char byte=0;
        Check(nod_seek(disc,position,0)==position&&nod_read(disc,&byte,1)==1,"Generated ISO read failed");
        Check(byte==static_cast<unsigned char>(bytes[position]),"Generated ISO payload changed");
    };
    read(0);
    if(!stream.gate)
    {
        owner.CloseNow();
        std::unique_lock lock(stream.mutex);
        const bool complete=stream.closeCount==1;
        // Preserve callback storage even when testing the broken detached pin.
        stream.changed.wait_for(lock,3s,[&]{return stream.closeCount!=0;});
        Check(complete,"Final nod_free returned before its stream close callback");
        Check(stream.closeThread==std::this_thread::get_id(),"Stream closed on a detached worker");
        Check(stream.reads!=0,"Lifecycle test never performed native I/O");
        return;
    }
    read(2*GroupSize);
    {std::lock_guard lock(stream.mutex);stream.armed=true;}
    const auto deadline=std::chrono::steady_clock::now()+3s;
    // Each reader-local cache miss revisits the preloader, retrying the injected
    // read-ahead error. Both requested groups are already cached and need no I/O.
    for(unsigned i=0;;++i)
    {
        read(i%2?2*GroupSize:0);
        std::unique_lock lock(stream.mutex);
        if(stream.changed.wait_for(lock,1ms,[&]{return stream.blocked;}))break;
        Check(std::chrono::steady_clock::now()<deadline,"No background read-ahead callback was observed");
    }
    std::thread closer([&]
    {
        {std::lock_guard lock(stream.mutex);stream.freeStarted=true;stream.changed.notify_all();}
        nod_free(disc);
        {std::lock_guard lock(stream.mutex);stream.freeDone=true;stream.changed.notify_all();}
    });
    owner.handle=nullptr; // The closer now owns the final handle.
    const auto closingThread=closer.get_id();
    bool returnedWithReadActive=false;
    {
        std::unique_lock lock(stream.mutex);
        stream.changed.wait(lock,[&]{return stream.freeStarted;});
        returnedWithReadActive=stream.changed.wait_for(lock,100ms,[&]{return stream.freeDone;});
        stream.released=true;stream.changed.notify_all();
    }
    closer.join();
    std::unique_lock lock(stream.mutex);
    const bool closedBeforeReturn=stream.closeCount==1;
    stream.changed.wait_for(lock,3s,[&]{return stream.closeCount!=0;});
    Check(!returnedWithReadActive,"Final nod_free detached an active preloader read");
    Check(closedBeforeReturn&&stream.closeCount==1,"Stream close callback did not finish before final nod_free returned");
    Check(stream.closeThread==closingThread,"Stream close escaped onto a detached worker");
}
void RetainedReaders(const std::vector<char>& bytes,unsigned threads)
{
    Stream stream;stream.bytes=bytes;
    NodDiscStream callbacks{&stream,Read,Length,Close};NodDiscOptions options{};options.preloader_threads=threads;
    NodHandle* disc=nullptr;NodHandle* partition=nullptr;NodHandle* file=nullptr;
    Check(nod_disc_open_stream(&callbacks,&options,&disc)==NOD_RESULT_OK,"Cannot open retained-reader ISO");
    Reader owner{stream,disc};
    Check(nod_disc_open_partition(disc,0,nullptr,&partition)==NOD_RESULT_OK,"Cannot open generated data partition");
    std::unique_ptr<NodHandle,decltype(&nod_free)> partitionOwner(partition,nod_free);
    const auto index=nod_partition_find_file(partition,"test.txt",nullptr,nullptr);
    Check(index!=NOD_FST_STOP&&nod_partition_open_file(partition,index,&file)==NOD_RESULT_OK,"Cannot open generated FST file");
    std::unique_ptr<NodHandle,decltype(&nod_free)> fileOwner(file,nod_free);
    owner.CloseNow();
    {std::lock_guard lock(stream.mutex);Check(!stream.closeCount,"Disc close invalidated surviving partition/file");}
    partitionOwner.reset();
    {std::lock_guard lock(stream.mutex);Check(!stream.closeCount,"Partition close invalidated surviving file");}
    unsigned char actual[24]{};constexpr char expected[]="Synthetic fixture data.\n";
    Check(nod_read(file,actual,sizeof(expected)-1)==sizeof(expected)-1&&
        std::memcmp(actual,expected,sizeof(expected)-1)==0,"Retained file reader lost generated bytes");
    fileOwner.reset();
    std::lock_guard lock(stream.mutex);
    Check(stream.closeCount==1&&stream.closeThread==std::this_thread::get_id(),"Final retained reader escaped stream cleanup");
}
}
int main(int argc,char** argv)
{
    try
    {
        const bool threading=argc==2;
        Check(threading||(argc==3&&std::string_view(argv[2])=="--without-threading"),"Supply generated ISO [--without-threading]");
        std::ifstream file(argv[1],std::ios::binary);
        Check(file.good(),"Cannot read generated ISO");
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
        Check(bytes.size()==4*GroupSize,"Lifecycle fixture must have four synthetic sector groups");
        // Exercise the active worker first so the old pin fails deterministically.
        for(unsigned threads:{2u,0u,1u})for(unsigned i=0;i<3;++i){Run(bytes,threads,threading);RetainedReaders(bytes,threads);}
        std::cout<<"Eighteen real nod stream sessions passed: 0/1/2 requested workers, "
            <<(threading?"active read drain, ":"threading disabled, ")
            <<"retained partition/file, synchronous final callback close\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}

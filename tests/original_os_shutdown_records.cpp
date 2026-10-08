#include <aurora/aurora.h>
#include <aurora/hardware.h>
#include "platform/interrupt_controller.h"
#include "platform/system.h"
#include "platform/video_device.h"
#include "platform/ipc_boot_buffer.h"
#include "platform/filesystem_device.h"
#include "platform/alarms.h"
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include "platform/os_shutdown_record_transport.h"
#include "platform/ios_device.h"
#include "platform/interrupts.h"
#include <revolution/os/OSStateFlags.h>
#include <revolution/os/OSPlayRecord.h>
#include <revolution/os/OSIpc.h>
#include <dolphin/os.h>
#include "os_record_oracles.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
extern "C" u32 fixture_play_checksum(const void*);
extern "C" int fixture_play_state(void);
extern "C" s64 fixture_play_last_close(void);
extern "C" void fixture_play_copy(void*);
extern "C" int fixture_play_alarm_pending(void);
namespace {
using namespace mscharged::platform;
unsigned checks{},callbacks{},source_cycles{};
void Check(bool okay,const char* text){++checks;if(!okay)throw std::runtime_error(text);}
void Require(bool okay,const char* text){if(!okay)throw std::runtime_error(text);}
template<class P>void Until(P ready){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!ready()){
        if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("Real record schedule timed out");
        std::this_thread::yield();
    }
}
// A test-only barrier pauses a real accepted NAND submission before its native
// FinishSubmission, or observes a worker reaching the shared exclusion. It
// does not supply an I/O result, callback, source state or readiness.
struct SubmissionProbe {
    std::mutex mutex;std::condition_variable changed;
    NANDCommandBlock* block{};unsigned phase{};bool active{},entered{},release{};
    void Arm(NANDCommandBlock* next,unsigned at){std::lock_guard lock(mutex);block=next;phase=at;active=true;entered=false;release=false;}
    void Reach(NANDCommandBlock* at,unsigned when){
        std::unique_lock lock(mutex);if(!active||block!=at||phase!=when)return;
        entered=true;changed.notify_all();changed.wait(lock,[&]{return release;});active=false;
    }
    void Wait(){std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}))throw std::runtime_error("Accepted NAND submission barrier timed out");}
    void Continue(){std::lock_guard lock(mutex);release=true;changed.notify_all();}
} submission_probe;
template<class T>T BE(const unsigned char* p){T value{};for(unsigned i=0;i<sizeof(T);++i)value=T((value<<8)|p[i]);return value;}
template<class T>T Native(const void* p){T value;std::memcpy(&value,p,sizeof value);return value;}
struct Play {u32 checksum;u16 title[40];char unknown[4];s64 start,stop;char id[6],tail[18];};
static_assert(sizeof(Play)==128&&offsetof(Play,start)==88&&offsetof(Play,stop)==96);
constexpr char StatePath[]="/title/00000001/00000002/data/state.dat";
constexpr char PlayPath[]="/title/00000001/00000002/data/play_rec.dat";
template<std::size_t N>std::array<unsigned char,N> Raw(const char* path){
    NANDFileInfo file{};alignas(32) std::array<unsigned char,N> wire{};
    Check(NANDOpen(path,&file,NAND_ACCESS_READ)==0,"Actual raw file open failed");
    Check(NANDRead(&file,wire.data(),N)==N,"Actual raw file read count changed");
    Check(NANDClose(&file)==0,"Actual raw file close failed");return wire;
}
void Replace(const char* path,const void* bytes,u32 count){
    auto removed=NANDDelete(path);Check(removed==0||removed==NAND_RESULT_NOEXISTS,"Fixture file delete returned unexpected error");
    Check(NANDCreate(path,NAND_PERM_RUSR|NAND_PERM_WUSR,0)==0,"Actual NAND fixture create failed");
    NANDFileInfo file{};Check(NANDOpen(path,&file,NAND_ACCESS_WRITE)==0,"Actual fixture open failed");
    alignas(32) std::array<unsigned char,128> wire{};
    Check(count<=wire.size(),"Fixture wire exceeds actual fixed record");std::memcpy(wire.data(),bytes,count);
    Check(NANDWrite(&file,wire.data(),count)==s32(count),"Actual fixture wire write failed");
    Check(NANDClose(&file)==0,"Actual fixture close failed");
}
u32 WireChecksum(const std::array<unsigned char,128>& wire){
    u32 value{};for(unsigned at=4;at<128;at+=4)value+=BE<u32>(wire.data()+at);return value;
}
void ScalarOracles(){
    for(unsigned n=0;n<StateOracle.size();++n){
        OSStateFlags native{};std::array<unsigned char,32> output{};
        mscharged_os_decode_state_record(&native,StateOracle[n].data());
        Check(native.checkSum==StateChecksums[n],"State checksum scalar byte order changed");
        Check(std::memcmp(&native.lastAppType,StateOracle[n].data()+4,28)==0,"State selector/padding bytes changed");
        mscharged_os_encode_state_record(output.data(),&native);
        Check(output==StateOracle[n],"State wire roundtrip mismatch");
        mscharged_os_decode_state_record(output.data(),output.data());
        Check(Native<u32>(output.data())==StateChecksums[n],"In-place state conversion changed native scalar");
        mscharged_os_encode_state_record(output.data(),output.data());
        Check(output==StateOracle[n],"In-place state encoding changed bytes");
    }
    for(unsigned n=0;n<PlayOracle.size();++n){
        Play native{};std::array<unsigned char,128> output{};
        mscharged_os_decode_play_record(&native,PlayOracle[n].data());
        Check(native.checksum==PlayChecksums[n],"Play checksum scalar byte order changed");
        for(unsigned k=0;k<40;++k)Check(native.title[k]==BE<u16>(PlayOracle[n].data()+4+k*2),"Play title cell byte order changed");
        Check(Native<u64>(&native.start)==BE<u64>(PlayOracle[n].data()+88)&&Native<u64>(&native.stop)==BE<u64>(PlayOracle[n].data()+96),"Signed timestamp bit patterns changed");
        Check(std::memcmp(native.unknown,PlayOracle[n].data()+84,4)==0&&std::memcmp(native.id,PlayOracle[n].data()+104,24)==0,"Play opaque/ASCII bytes changed");
        Check(fixture_play_checksum(&native)==PlayChecksums[n],"Whole original play checksum differs from independent Wii oracle");
        mscharged_os_encode_play_record(output.data(),&native);Check(output==PlayOracle[n],"Play wire roundtrip changed bytes");
        mscharged_os_decode_play_record(output.data(),output.data());
        mscharged_os_encode_play_record(output.data(),output.data());Check(output==PlayOracle[n],"In-place play encoding changed bytes");
    }
}
void StateSource(){
    OSStateFlags output;std::memset(&output,0xa5,sizeof output);
    OSStateFlags zero{};Check(!__OSReadStateFlags(&output)&&std::memcmp(&output,&zero,32)==0,"Original missing state file decision changed");
    Check(!__OSWriteStateFlags(&output),"Original state write created an absent file");
    Replace(StatePath,StateOracle[0].data(),32);
    for(unsigned n=0;n<StateOracle.size();++n){
        OSStateFlags input{};mscharged_os_decode_state_record(&input,StateOracle[n].data());
        input.checkSum=0xdeadbeef;const auto before=input;
        Check(__OSWriteStateFlags(&input),"Original state write rejected genuine backing");
        Check(std::memcmp(&input,&before,32)==0,"Original state write mutated caller record");
        Check(Raw<32>(StatePath)==StateOracle[n],"Original state checksum/write wire bytes differ from independent oracle");
        Check(__OSReadStateFlags(&output)&&output.checkSum==StateChecksums[n]&&std::memcmp(&output.lastAppType,&input.lastAppType,28)==0,"Original valid state read/copy changed");
    }
    auto bad=StateOracle[0];bad[0]^=1;Replace(StatePath,bad.data(),32);
    std::memset(&output,0xa5,32);Check(!__OSReadStateFlags(&output)&&std::memcmp(&output,&zero,32)==0,"Original bad checksum did not zero caller");
    Check(Raw<32>(StatePath)==bad,"Original checksum failure deleted/rewrote file");
    Replace(StatePath,StateOracle[1].data(),7);
    std::memset(&output,0xa5,32);Check(!__OSReadStateFlags(&output)&&std::memcmp(&output,&zero,32)==0,"Original short state read did not zero caller");
    NANDFileInfo absent{};Check(NANDOpen(StatePath,&absent,NAND_ACCESS_READ)==NAND_RESULT_NOEXISTS,"Original short state read did not delete file");
    // Real partial read must preserve the untouched tail in native form.
    Replace(StatePath,StateOracle[1].data(),7);NANDFileInfo file{};
    Check(NANDOpen(StatePath,&file,NAND_ACCESS_READ)==0,"Partial state file open failed");
    mscharged_os_decode_state_record(&output,StateOracle[2].data());
    Check(mscharged_os_state_nand_read(&file,&output,32)==7,"Native boundary changed genuine partial read count");
    std::array<unsigned char,32> observed{},expected=StateOracle[2];std::memcpy(expected.data(),StateOracle[1].data(),7);
    mscharged_os_encode_state_record(observed.data(),&output);Check(observed==expected,"Native state partial read lost original untouched tail");
    Check(NANDClose(&file)==0,"Partial state close failed");
}
struct Receipt {
    std::thread::id owner=std::this_thread::get_id();OSContext* prior=OSGetCurrentContext();
    NANDFileInfo* file{};Play* native{};std::array<unsigned char,128> expected{};
    s32 expected_count=128;unsigned calls{};bool resubmit{};
    static void Receive(s32 result,NANDCommandBlock* block){
        auto& r=*static_cast<Receipt*>(block->userData);
        Check(NativeInterruptDispatchActive()&&!NativeInterruptsEnabled()&&std::this_thread::get_id()==r.owner&&OSGetCurrentContext()!=r.prior,"NAND record callback escaped genuine owner IRQ context");
        Check(block->callback==Receive,"Native bridge did not restore original callback before delivery");
        Check(mscharged_os_record_pending_count()==0,"Native wire remained registered through original callback");
        Check(result==r.expected_count,"Record boundary changed actual NAND result");
        std::array<unsigned char,128> wire{};mscharged_os_encode_play_record(wire.data(),r.native);
        Check(wire==r.expected,"Original callback saw unconverted/incorrect native play record");
        ++r.calls;++callbacks;
        if(r.resubmit&&r.calls==1){
            Check(NANDSeek(r.file,0,NAND_SEEK_BEG)==0,"Reentrant original block seek failed");
            Check(mscharged_os_play_nand_write_async(r.file,r.native,128,Receive,block)==0,"Reentrant original block reuse failed");
        }
    }
};
void TransportAsync(){
    Replace(PlayPath,PlayOracle[0].data(),128);NANDFileInfo file{};
    Check(NANDOpen(PlayPath,&file,NAND_ACCESS_RW)==0,"Async play file open failed");
    Play native{};Receipt read;read.file=&file;read.native=&native;read.expected=PlayOracle[0];read.resubmit=true;
    NANDCommandBlock block{};block.userData=&read;
    Check(mscharged_os_play_nand_read_async(&file,&native,128,Receipt::Receive,&block)==0,"Actual play read submission failed");
    Check(!read.calls&&mscharged_os_record_pending_count()==1&&GetNativeIOSStatus().pending==1,"Record read completed before actual hardware servicing");
    const auto mask=OSDisableInterrupts();Check(!ServiceNativeIOSRequests()&&!read.calls,"Masked IOS delivered play callback");OSRestoreInterrupts(mask);
    Check(ServiceNativeIOSRequests()&&read.calls==1&&mscharged_os_record_pending_count()==1,"Read completion/reentrant write lifetime changed");
    Check(ServiceNativeIOSRequests()&&read.calls==2&&!mscharged_os_record_pending_count(),"Reentrant play write completion failed");
    Check(Raw<128>(PlayPath)==PlayOracle[0],"Async write wire bytes differ from independent oracle");
    Check(NANDClose(&file)==0,"Async play close failed");
    // The real IOS asynchronous API accepts this request, then reports the
    // invalid descriptor at completion. Preserve that queue/error distinction.
    NANDFileInfo invalid{};invalid.fd=-1;block={};block.userData=&read;read.calls=0;read.resubmit=false;read.expected_count=NAND_RESULT_INVALID;
    const auto result=mscharged_os_play_nand_write_async(&invalid,&native,128,Receipt::Receive,&block);
    Check(result==0&&!read.calls&&mscharged_os_record_pending_count()==1,"Actual invalid descriptor queue behavior changed");
    Check(ServiceNativeIOSRequests()&&read.calls==1&&!mscharged_os_record_pending_count()&&block.callback==Receipt::Receive,"Error completion changed result/callback/request lifetime");
    Replace(PlayPath,PlayOracle[1].data(),7);Check(NANDOpen(PlayPath,&file,NAND_ACCESS_RW)==0,"Partial play open failed");
    mscharged_os_decode_play_record(&native,PlayOracle[2].data());Receipt partial;partial.native=&native;partial.expected=PlayOracle[2];partial.expected_count=7;
    std::memcpy(partial.expected.data(),PlayOracle[1].data(),7);block={};block.userData=&partial;
    Check(mscharged_os_play_nand_read_async(&file,&native,128,Receipt::Receive,&block)==0,"Partial play submit failed");
    Check(ServiceNativeIOSRequests()&&partial.calls==1&&!mscharged_os_record_pending_count(),"Partial play actual completion failed");
    Check(NANDClose(&file)==0,"Partial play close failed");
}
void ConcurrentTransport(){
    Replace(PlayPath,PlayOracle[0].data(),128);NANDFileInfo file{};
    Check(NANDOpen(PlayPath,&file,NAND_ACCESS_RW)==0,"Concurrent play file open failed");
    Play native{};Receipt read;read.file=&file;read.native=&native;read.expected=PlayOracle[0];
    NANDCommandBlock block{};block.userData=&read;
    submission_probe.Arm(&block,1);
    std::exception_ptr failure;std::atomic<bool> returned{};
    std::thread worker([&]{try{
        Require(mscharged_os_play_nand_read_async(&file,&native,128,Receipt::Receive,&block)==0,"Actual worker submission failed");
        returned=true;
    }catch(...){failure=std::current_exception();}});
    try{
        submission_probe.Wait();
        Check(GetNativeIOSStatus().pending==1&&mscharged_os_record_pending_count()==1,"Worker did not retain the actual accepted request");
        Check(!ServiceNativeIOSRequests()&&!read.calls&&!returned.load(),"Owner delivered while worker still borrowed the submission wire");
        submission_probe.Continue();worker.join();if(failure)std::rethrow_exception(failure);
    }catch(...){submission_probe.Continue();if(worker.joinable())worker.join();throw;}
    Check(returned&&ServiceNativeIOSRequests()&&read.calls==1&&!mscharged_os_record_pending_count(),"Worker request failed after submission exclusion released");
    Check(NANDSeek(&file,0,NAND_SEEK_BEG)==0,"Duplicate block test seek failed");
    read.calls=0;
    Check(mscharged_os_play_nand_read_async(&file,&native,128,Receipt::Receive,&block)==0,"Duplicate block first request failed");
    const auto callback=block.callback;bool rejected{};
    try{(void)mscharged_os_play_nand_write_async(&file,&native,128,Receipt::Receive,&block);}catch(const std::logic_error&){rejected=true;}
    Check(rejected&&block.callback==callback&&mscharged_os_record_pending_count()==1&&GetNativeIOSStatus().pending==1,"Premature same-block reuse mutated its actual owner");
    Check(ServiceNativeIOSRequests()&&read.calls==1&&!mscharged_os_record_pending_count(),"Duplicate refusal damaged the original completion");
    Check(NANDClose(&file)==0,"Concurrent play file close failed");
}
struct DeliverySchedule {
    NANDFileInfo file{};NANDCommandBlock block{};Play first{},second{};
    std::thread::id owner=std::this_thread::get_id();std::thread worker;
    std::exception_ptr failure;std::atomic<bool> entered{},returned{};unsigned calls{};
    static void Second(s32 result,NANDCommandBlock* block){
        auto& r=*static_cast<DeliverySchedule*>(block->userData);
        Check(std::this_thread::get_id()==r.owner&&NativeInterruptDispatchActive()&&block->callback==Second&&result==128,"Foreign resubmission lost the original second callback");
        ++r.calls;++callbacks;
    }
    static void First(s32 result,NANDCommandBlock* block){
        auto& r=*static_cast<DeliverySchedule*>(block->userData);
        Check(std::this_thread::get_id()==r.owner&&NativeInterruptDispatchActive()&&block->callback==First&&result==128,"First scheduled callback lost actual owner/result");
        std::array<unsigned char,128> wire{};mscharged_os_encode_play_record(wire.data(),&r.first);
        Check(wire==PlayOracle[0],"First callback borrowed a retired/incorrect wire");
        submission_probe.Arm(block,0);r.entered=true;submission_probe.Wait();
        submission_probe.Continue();
        Check(!r.returned.load()&&block->callback==First,"Foreign same-block reuse crossed active original delivery");
        ++r.calls;++callbacks;
    }
};
void ConcurrentDelivery(){
    Replace(PlayPath,PlayOracle[0].data(),128);DeliverySchedule r;
    Check(NANDOpen(PlayPath,&r.file,NAND_ACCESS_RW)==0,"Delivery schedule file open failed");
    mscharged_os_decode_play_record(&r.second,PlayOracle[1].data());r.block.userData=&r;
    Check(mscharged_os_play_nand_read_async(&r.file,&r.first,128,DeliverySchedule::First,&r.block)==0,"Delivery schedule read failed");
    r.worker=std::thread([&]{try{
        Until([&]{return r.entered.load();});
        // Real IRQ exclusion must already prevent this caller from touching
        // the source block, independently of the observer's scheduling barrier.
        {NativeInterruptRead exclusion;Require(!exclusion,"Owner callback did not hold actual shared IRQ exclusion");}
        Require(mscharged_os_play_nand_write_async(&r.file,&r.second,128,DeliverySchedule::Second,&r.block)==0,"Post-delivery foreign submission failed");
        r.returned=true;
    }catch(...){r.failure=std::current_exception();}});
    try{Check(ServiceNativeIOSRequests()&&r.calls==1,"First real scheduled completion failed");r.worker.join();if(r.failure)std::rethrow_exception(r.failure);}
    catch(...){submission_probe.Continue();if(r.worker.joinable())r.worker.join();throw;}
    Check(r.returned&&mscharged_os_record_pending_count()==1&&ServiceNativeIOSRequests()&&r.calls==2&&!mscharged_os_record_pending_count(),"Foreign block reuse failed after original callback return");
    const auto raw=Raw<256>(PlayPath);
    Check(std::equal(PlayOracle[0].begin(),PlayOracle[0].end(),raw.begin())&&std::equal(PlayOracle[1].begin(),PlayOracle[1].end(),raw.begin()+128),"Serialized worker write changed actual record bytes");
    Check(NANDClose(&r.file)==0,"Delivery schedule close failed");
}
void SourceStartStop(){
    constexpr int Open=1,Read=2,Seek=3,Alarm=4,Closed=7,Stopped=9;
    Check(fixture_play_state()==Stopped,"Original play owner was not cold STOPPED");
    __OSStopPlayRecord();Check(fixture_play_state()==Stopped,"Original cold Stop changed state");
    for(const auto target:{Open,Read,Seek,Alarm}){
        Replace(PlayPath,PlayOracle[3].data(),128);
        __OSStartPlayRecord();Check(fixture_play_state()==Open,"Original Start did not issue its actual open request");
        while(fixture_play_state()!=target){Check(ServiceNativeIOSRequests(),"Original play chain has no pending real completion");}
        if(target==Alarm){
            Play snapshot{};fixture_play_copy(&snapshot);
            std::array<unsigned char,128> wire{};mscharged_os_encode_play_record(wire.data(),&snapshot);
            Check(wire==PlayOracle[3],"Original read/seek/alarm path lost source record");
            Check(Native<u64>(&snapshot.stop)==static_cast<u64>(fixture_play_last_close()),"Original LastCloseTime did not use decoded timestamp");
        }
        const auto before=target==Alarm?OSGetTime():0;__OSStopPlayRecord();const auto after=target==Alarm?OSGetTime():0;
        Check(fixture_play_state()==Stopped&&GetNativeIOSStatus().pending==0&&!mscharged_os_record_pending_count()&&!fixture_play_alarm_pending(),"Original Stop did not drain real pending record callback/alarm");
        if(target==Alarm){
            const auto wire=Raw<128>(PlayPath);
            Check(BE<u32>(wire.data())==WireChecksum(wire),"Original Stop checksum/write differs from independent Wii sum");
            Check(BE<u64>(wire.data()+96)>=u64(before)&&BE<u64>(wire.data()+96)<=u64(after),"Original Stop timestamp changed clock or byte units");
            auto fixed=wire;std::copy(PlayOracle[3].begin(),PlayOracle[3].begin()+4,fixed.begin());
            std::copy(PlayOracle[3].begin()+96,PlayOracle[3].begin()+104,fixed.begin()+96);
            Check(fixed==PlayOracle[3],"Original Stop changed title/start/opaque record fields");
        }else Check(Raw<128>(PlayPath)==PlayOracle[3],"Pending Stop unexpectedly wrote record");
        ++source_cycles;
    }
    Check(NANDDelete(PlayPath)==0,"Absent source path setup failed");__OSStartPlayRecord();
    Check(ServiceNativeIOSRequests()&&fixture_play_state()==Closed,"Original missing play file did not follow CLOSED error decision");
    __OSStopPlayRecord();Check(fixture_play_state()==Stopped&&!mscharged_os_record_pending_count(),"Original missing-file Stop leaked request");
}
}
void GameIdentity(){
    // Explicit test process identity matches the current owned R4QE01 TMD
    // observation. This fixture neither imports a save nor switches to UID0.
    __OSInitIPCBuffer();IPCInit();
    Check(NANDInit()==0&&nandIsInitialized(),"Game-identity NANDInit failed");
    char home[64]{};Check(NANDGetHomeDir(home)==0&&std::strcmp(home,"/title/00010000/52345145/data")==0,"Real NAND home did not preserve the game title identity");
    OSStateFlags state,zero{};std::memset(&state,0xa5,sizeof state);
    Check(!__OSReadStateFlags(&state)&&std::memcmp(&state,&zero,sizeof state)==0,"Absent system state did not follow original zero/failure path");
    Check(!__OSWriteStateFlags(&state),"State source created an absent system record under game identity");
    for(const char* path:{StatePath,PlayPath}){
        NANDFileInfo file{};
        Check(NANDOpen(path,&file,NAND_ACCESS_READ)==NAND_RESULT_NOEXISTS,"Fresh game backing invented a system record");
        Check(NANDPrivateCreate(path,NAND_PERM_RUSR|NAND_PERM_WUSR,0)==NAND_RESULT_NOEXISTS,"Game process created a file under an absent system directory");
    }
    Check(NANDPrivateCreateDir("/title/00000001",NAND_PERM_RUSR|NAND_PERM_WUSR,0)==NAND_RESULT_ACCESS,"Game identity obtained write access to the system-title root");
    Check(fixture_play_state()==9,"Original game-identity play owner was not cold STOPPED");
    __OSStartPlayRecord();Check(fixture_play_state()==1&&GetNativeIOSStatus().pending==1,"Game-identity Start did not issue the original request");
    Check(ServiceNativeIOSRequests()&&fixture_play_state()==7&&!GetNativeIOSStatus().pending,"Missing system play file did not reach original CLOSED decision");
    __OSStopPlayRecord();Check(fixture_play_state()==9&&!fixture_play_alarm_pending()&&!mscharged_os_record_pending_count(),"Missing system play Stop changed source lifetime");
    std::printf("OSRecord517 game-identity PASS %u checks; title00010000/52345145 UID1001 GID3031; original missing/access failures preserved, no system record provisioning.\n",checks);
}
unsigned Qualify(){
    ScalarOracles();
    Play cold{};NANDFileInfo absent{};NANDCommandBlock block{};
    block.callback=Receipt::Receive;
    Check(!nandIsInitialized()&&mscharged_os_play_nand_read_async(&absent,&cold,128,Receipt::Receive,&block)==NAND_RESULT_FATAL_ERROR&&block.callback==Receipt::Receive&&!mscharged_os_record_pending_count(),"Cold async boundary changed original rejection/fields");
    __OSInitIPCBuffer();IPCInit();Check(NANDInit()==0&&nandIsInitialized(),"Whole original NANDInit failed real system-title fixture");
    StateSource();TransportAsync();ConcurrentTransport();ConcurrentDelivery();SourceStartStop();
    Check(!GetNativeIOSStatus().pending&&!GetNativeIOSStatus().active&&!mscharged_os_record_pending_count(),"Native record owner still has actual work");
    std::printf("OSRecord517 current-source PASS %u checks; %u real NAND user callbacks; %u original Start/Stop paths; state32/play128; reset/IOS revision/persistent game saves not admitted.\n",checks,callbacks,source_cycles);
    return checks;
}

extern "C" void mscharged_os_record_test_submission(NANDCommandBlock* block,unsigned phase){submission_probe.Reach(block,phase);}

namespace aurora { extern AuroraConfig g_config; }
namespace {
unsigned clock_completions;
void HardwareService() {
    mscharged::platform::ServiceNativeAlarms();
    if(mscharged::platform::ServiceNativeIOSRequests()) ++clock_completions;
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2&&argc!=3)throw std::runtime_error("usage: original_os_shutdown_record_tests DISPOSABLE-ROOT [--game-identity]");
        const bool game_identity=argc==3;
        if(game_identity&&std::strcmp(argv[2],"--game-identity"))throw std::runtime_error("Unknown record fixture mode");
        using namespace mscharged::platform;
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size=64u*1024u*1024u;
        OSInit();InitializeNativeInterruptController();
        InitializeNativeAlarms();
        mscharged::ConfigureNativeSystemSettings({1,0,0,0,1});
        ConfigureNativeVideoHardware(0,false);
        alignas(32) static std::array<unsigned char,32768> boot;
        InstallNativeIPCBootBuffer(boot.data(),boot.size());
        // Explicit disposable virtual system-title fixture, not the game's
        // actual ES identity or source readiness. No existing saves are read.
        const auto unique=std::string("owned-")+std::to_string(getpid())+"-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto root=std::filesystem::path(argv[1])/unique;
        if(game_identity)InitializeNativeFilesystem({root,0x0001000052345145ULL,0x1001,0x3031});
        else InitializeNativeFilesystem({root,0x0000000100000002ULL,0,0});
        if(!aurora_register_hardware_service(HardwareService))throw std::runtime_error("Real clock service already owned");
        if(game_identity)GameIdentity();
        else if(!Qualify())throw std::runtime_error("Whole-source record qualifier failed");
        if(GetNativeIOSStatus().pending||GetNativeIOSStatus().active)throw std::runtime_error("Actual record callbacks remain live");
        if(!aurora_unregister_hardware_service(HardwareService))throw std::runtime_error("Clock service retirement failed");
        ShutdownNativeAlarms();
        ShutdownNativeFilesystem();
        std::printf("Record517 real clock completions=%u; disposable backing=%s\n",clock_completions,root.c_str());
        // Every request/file/alarm is drained. SDK arenas remain
        // terminally held; this does not claim OSShutdownSystem/full shutdown.
        std::fflush(nullptr);std::_Exit(0);
    }catch(const std::exception& error){
        std::fprintf(stderr,"Record517 source boundary: %s\n",error.what());
        std::fflush(nullptr);std::_Exit(1);
    }
}

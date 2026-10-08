#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include "platform/thread_registry_abi.h"
#include <dolphin/os.h>
#include <revolution/os/OSMessage.h>
#include <revolution/os/OSMutex.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace mscharged::platform;
unsigned checks;
std::atomic<unsigned> entries{}, returns{}, destructions{}, services{};
void Check(bool condition,const char* error) { ++checks;if(!condition)throw std::runtime_error(error); }
template<class F> void Reject(F action,const char* error) {
    bool rejected=false;try { action(); }catch(const std::logic_error&){rejected=true;}Check(rejected,error);
}
template<class F> void Until(F predicate) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!predicate()) { if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("Actual worker boundary timed out");std::this_thread::yield(); }
}
struct Job {
    OSThread sdk{};
    alignas(32) std::array<unsigned char,16384> stack{};
    OSMessageQueue messages{};
    OSMessage cell{};
    OSMutex mutex{};
    OSMutex* contested{};
    bool self{},hold{},service{};
};
Job* job; // Real source frame/descriptor storage retained through process removal.
void Service() { ++services; }
void* Entry(void* context) {
    auto& j=*static_cast<Job*>(context);
    struct Frame { ~Frame(){++destructions;} } frame;
    ++entries;
    if(j.service)SetNativeThreadWaitService(Service);
    if(j.hold)OSLockMutex(&j.mutex);
    if(j.contested)OSLockMutex(j.contested);
    if(j.self)OSSuspendThread(OSGetCurrentThread());
    OSMessage message{};
    (void)OSReceiveMessage(&j.messages,&message,OS_MESSAGE_BLOCK);
    if(j.hold)OSUnlockMutex(&j.mutex);
    if(j.contested)OSUnlockMutex(j.contested);
    ++returns;
    return message;
}
void Create(u16 attr=0) {
    OSInitMessageQueue(&job->messages,&job->cell,1);
    OSInitMutex(&job->mutex);
    Check(OSCreateThread(&job->sdk,Entry,job,job->stack.data()+job->stack.size(),job->stack.size(),3,attr),"Source Create failed");
    Check(OSResumeThread(&job->sdk)==1,"Actual first Resume failed");
}
bool Waiting(OSThreadQueue& queue) {
    const auto mask=OSDisableInterrupts();
    const bool result=job->sdk.state==OS_THREAD_STATE_WAITING&&job->sdk.queue==&queue&&queue.head==&job->sdk&&queue.tail==&job->sdk;
    OSRestoreInterrupts(mask);return result;
}
struct Snapshot {
    std::array<unsigned char,sizeof(OSThread)> thread;
    std::array<unsigned char,sizeof(OSMessageQueue)> queue;
    std::array<unsigned char,sizeof(OSMutex)> mutex;
    unsigned entry,returned,destroyed;
    Snapshot() : entry(entries),returned(returns),destroyed(destructions) {
        std::memcpy(thread.data(),&job->sdk,thread.size());
        std::memcpy(queue.data(),&job->messages,queue.size());
        std::memcpy(mutex.data(),&job->mutex,mutex.size());
    }
    void Same() const {
        Check(!std::memcmp(thread.data(),&job->sdk,thread.size()),"Rejected cancellation changed source descriptor");
        Check(!std::memcmp(queue.data(),&job->messages,queue.size()),"Rejected cancellation changed source message ring/queue");
        Check(!std::memcmp(mutex.data(),&job->mutex,mutex.size()),"Rejected cancellation changed original mutex");
        Check(entries==entry&&returns==returned&&destructions==destroyed,"Rejected cancellation resumed/unwound source frame");
    }
};
void Disable() {
    // Source software entry may finish a real wait-service callback between
    // observations. Retry only the unchanged host-only diagnostic acquisition.
    bool disabled=false;
    Until([&]{try { Check(OSDisableScheduler()==0,"Unexpected scheduler disable count");disabled=true; }
              catch(const std::logic_error&){}return disabled;});
}
void Arm() {
    OSDisableInterrupts();
    auto* context=OSGetCurrentContext();
    ChargedNativeBeginThreadPowerRemoval();
    Check(!NativeInterruptsEnabled()&&OSGetCurrentContext()==context,"Power boundary changed mask/context");
}
void Negatives() {
    Snapshot before;
    Reject([]{OSCancelThread(&job->sdk);},"Unqualified actual source context was cancelled");
    before.Same();
    Reject([]{(void)ValidateNativeThreadsForPowerRemoval();},"Unqualified actual source context passed power fence");
    before.Same();
    Reject([]{OSEnableScheduler();},"Armed terminal scheduler resumed source execution");
    before.Same();
}
void Early() {
    auto* owner=OSGetCurrentThread();auto* context=OSGetCurrentContext();
    Reject([]{ChargedNativeBeginThreadPowerRemoval();},"Enabled interrupts armed power boundary");
    Check(NativeInterruptsEnabled()&&OSGetCurrentThread()==owner&&OSGetCurrentContext()==context,"Early rejection changed source owner");
    auto mask=OSDisableInterrupts();
    Reject([]{ChargedNativeBeginThreadPowerRemoval();},"No disabled scheduler armed power boundary");
    Check(!NativeInterruptsEnabled()&&OSGetCurrentContext()==context,"Masked early rejection changed context/mask");
    OSRestoreInterrupts(mask);
    Check(OSDisableScheduler()==0,"Early scheduler count changed");
    Reject([]{ChargedNativeBeginThreadPowerRemoval();},"Enabled source mask armed final scheduler");
    mask=OSDisableInterrupts();
    {
        NativeInterruptGuard guard;
        Reject([]{ChargedNativeBeginThreadPowerRemoval();},"Extra retained native guard armed power boundary");
    }
    Check(!NativeInterruptsEnabled()&&NativeInterruptWaitAllowed(),"Guard rejection leaked host exclusion or source mask");
    OSRestoreInterrupts(mask);
    Check(OSEnableScheduler()==1,"Early rejected attempts changed power/scheduler owner");
    Check(NativeInterruptsEnabled()&&OSGetCurrentContext()==context,"Early cases leaked caller exclusion");
}
[[noreturn]] void Finish(const char* mode) {
    std::printf("Terminal context case %s PASS: %u checks, entries%u returns%u destructors%u services%u; retained frames, no STM or HOME acceptance.\n",mode,checks,entries.load(),returns.load(),destructions.load(),services.load());
    std::fflush(nullptr);std::_Exit(0);
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Pass early|self|ready|held|contended|service|detached");
        const std::string mode=argv[1];
        // Same real native kernel foundation as original_os_mutex_tests.
        // No Aurora renderer/OSInit owner is claimed by this software leaf.
        (void)OSGetCurrentThread();
        if(mode=="early"){Early();Finish(argv[1]);}
        job=new Job;
        OSMutex* contested=nullptr;
        if(mode=="self")job->self=true;
        else if(mode=="held")job->hold=true;
        else if(mode=="service")job->service=true;
        else if(mode=="contended"){
            contested=new OSMutex{};OSInitMutex(contested);OSLockMutex(contested);job->contested=contested;
        } else if(mode!="ready"&&mode!="detached")throw std::runtime_error("Unknown case");
        Create(mode=="detached"?OS_THREAD_ATTR_DETACH:0);
        if(mode=="self")Until([]{
            const auto mask=OSDisableInterrupts();
            const bool result=entries==1&&job->sdk.state==OS_THREAD_STATE_READY&&job->sdk.suspend==1;
            OSRestoreInterrupts(mask);return result;
        });
        else Until([&]{return Waiting(contested?contested->queue:job->messages.queueReceive);});
        if(mode=="service")Until([]{return services>0;});
        // This gives native execution an opportunity to enter its physical
        // wait; success still depends on the actual kernel's parked predicate.
        for(unsigned n=0;n<32;++n)std::this_thread::yield();
        Disable();
        OSDisableInterrupts();
        if(mode=="ready"){
            Check(OSSendMessage(&job->messages,job,OS_MESSAGE_NOBLOCK),"Actual ready transition Send failed");
            Check(job->sdk.state==OS_THREAD_STATE_READY&&!job->sdk.queue&&job->messages.usedCount==1,"Source Send did not make actual READY worker");
        }
        Arm();
        if(mode!="detached"){
            std::array<unsigned char,sizeof(OSMutex)> held{};
            if(contested)std::memcpy(held.data(),contested,held.size());
            Negatives();
            if(contested)Check(!std::memcmp(held.data(),contested,held.size()),"Rejected terminal cancellation changed real contended mutex/owner");
        }else{
            const auto value=job->sdk.val;
            OSCancelThread(&job->sdk);
            const auto status=ValidateNativeThreadsForPowerRemoval();
            Check(status.stopped_workers==1&&status.completed_workers==0&&status.retained_moribund_threads==0,"Detached stopped frame mislabeled completed/active");
            Check(job->sdk.state==0&&!job->sdk.queue&&job->sdk.val==value&&OSIsThreadTerminated(&job->sdk),"Original detached cancellation result/state differs");
            Check(!job->messages.queueReceive.head&&!job->messages.queueReceive.tail&&!job->messages.usedCount,"Detached cancellation changed ring or retained wait link");
            Check(entries==1&&returns==0&&destructions==0,"Detached cancellation unwound actual source frame");
            Snapshot before;
            Reject([]{OSJoinThread(&job->sdk,nullptr);},"Stopped detached context joined");before.Same();
            Reject([]{OSDetachThread(&job->sdk);},"Stopped detached context detached/reused");before.Same();
            Reject([]{OSResumeThread(&job->sdk);},"Stopped detached context resumed");before.Same();
            Reject([]{DrainNativeThreadLifetimes();},"Stopped detached context lifetime retired");before.Same();
            Reject([]{OSEnableScheduler();},"Stopped detached context scheduler re-enabled");before.Same();
        }
        Finish(argv[1]);
    }catch(const std::exception& error){
        std::fprintf(stderr,"Terminal context case failure: %s (%u checks)\n",error.what(),checks);std::fflush(nullptr);std::_Exit(1);
    }
}

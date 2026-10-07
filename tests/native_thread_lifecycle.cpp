#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include "platform/thread.h"
#include <dolphin/os.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mscharged::platform { void DrainNativeThreadLifetimes(); }

namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
std::atomic<unsigned> checks{}, entries{}, returns{};
void Check(bool condition, const char* error) {
    ++checks;
    if (!condition) throw std::runtime_error(error);
}
template<class F> void Until(F condition) {
    const auto limit=std::chrono::steady_clock::now()+2s;
    while (!condition()) {
        if (std::chrono::steady_clock::now()>=limit) throw std::runtime_error("Native SDK lifecycle operation timed out");
        std::this_thread::yield();
    }
}
template<class F> void Reject(F action,const char* error) {
    bool rejected{};
    try { action(); } catch (const std::logic_error&) { rejected=true; }
    Check(rejected,error);
}
unsigned Waiters(OSThreadQueue& queue) {
    const auto mask=OSDisableInterrupts();
    unsigned count{};
    for (auto* thread=queue.head;thread;thread=thread->link.next)++count;
    OSRestoreInterrupts(mask);
    return count;
}
struct Owned {
    OSThread sdk;
    alignas(8) std::array<unsigned char,0x4000+32> stack;
    Owned() { std::memset(&sdk,0xa5,sizeof(sdk));stack.fill(0xa5); }
    void* Top() { return stack.data()+16+0x4000; }
    unsigned char* Bottom() { return stack.data()+16; }
};
struct Job {
    Owned* storage{};
    OSMessageQueue queue;
    OSMessage cell{};
    void* payload{};
    std::atomic<bool> entered{}, returned{};
    bool self_suspend{};
};
void* Worker(void* data) {
    auto& job=*static_cast<Job*>(data);
    ++entries;
    Check(OSGetCurrentThread()==&job.storage->sdk,"Created SDK worker lost caller-owned descriptor identity");
    Check(std::this_thread::get_id()!=std::thread::id{},"Native worker lacks real execution identity");
    Check(OSGetCurrentContext()==&job.storage->sdk.context,"Created source worker lacks native SDK context");
    Check(NativeInterruptsEnabled()&&!NativeInterruptDispatchActive(),"Created worker inherited owner mask/IRQ dispatch");
    Check(SetNativeThreadWaitService(nullptr)==nullptr,"Created worker inherited source owner hardware service");
    const auto bounds=mscharged::CurrentThreadStackLimits();
#if defined(_MSC_VER)
    const auto physical_frame=reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
    const auto physical_frame=reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
    Check(job.storage->sdk.stackBase==reinterpret_cast<u8*>(bounds.high)&&
        job.storage->sdk.stackEnd==reinterpret_cast<u8*>(bounds.low)&&
        physical_frame>=bounds.low&&physical_frame<bounds.high,
        "Native SDK stack fields do not describe actual running native stack");
    job.entered=true;
    if (job.self_suspend) {
        const auto prior=OSSuspendThread(OSGetCurrentThread());
        Check(prior==0&&NativeInterruptsEnabled(),"Actual source self-suspension lost its counter/caller mask");
    }
    OSMessage value{};
    Check(OSReceiveMessage(&job.queue,&value,OS_MESSAGE_BLOCK)&&value==job.payload,
        "Created source worker lost genuine whole OSMessage data/wake");
    Check(OSGetCurrentThread()==&job.storage->sdk&&OSGetCurrentContext()==&job.storage->sdk.context,
        "Source message wait lost created SDK descriptor/context");
    job.returned=true;
    ++returns;
    return value;
}
void Prepare(Owned& storage,Job& job,void* payload,bool self_suspend=false) {
    job.storage=&storage;job.payload=payload;job.self_suspend=self_suspend;
    OSInitMessageQueue(&job.queue,&job.cell,1);
}
void Create(Owned& storage,Job& job,u16 flags=0) {
    Check(OSCreateThread(&storage.sdk,Worker,&job,storage.Top(),0x4000,14,flags),"Native SDK Create failed");
    Check(storage.sdk.state==OS_THREAD_STATE_READY&&storage.sdk.suspend==1&&
        storage.sdk.base==14&&storage.sdk.priority==14&&
        storage.sdk.attr==(flags&OS_THREAD_ATTR_DETACH),"Original creation request/counters/attribute mask changed");
    Check(!job.entered&&!OSIsThreadTerminated(&storage.sdk)&&OSIsThreadSuspended(&storage.sdk),
        "Created worker entered before original Resume");
    Check(storage.sdk.val==reinterpret_cast<void*>(std::uintptr_t{0xffffffffu})&&
        !storage.sdk.error&&!storage.sdk.specific[0]&&!storage.sdk.specific[1]&&
        !storage.sdk.queueJoin.head&&!storage.sdk.queueJoin.tail,
        "Original SDK default live fields/request initialization changed");
    u32 magic{},zero1=1,zero2=1;
    std::memcpy(&magic,storage.Bottom(),4);
    std::memcpy(&zero1,static_cast<unsigned char*>(storage.Top())-8,4);
    std::memcpy(&zero2,static_cast<unsigned char*>(storage.Top())-4,4);
    Check(magic==OS_THREAD_STACK_MAGIC&&!zero1&&!zero2,"Original logical stack sentinel/top words changed");
    for (unsigned n=0;n<16;++n)Check(storage.stack[n]==0xa5&&storage.stack[0x4000+16+n]==0xa5,
        "Native thread creation overran original caller stack extent");
}
void StartAndWait(Owned& storage,Job& job) {
    Check(OSResumeThread(&storage.sdk)==1,"Original first Resume counter changed");
    Until([&]{ if(!job.entered) { (void)OSIsThreadTerminated(&storage.sdk); return false; } return Waiters(job.queue.queueReceive)==1; });
    Check(!OSIsThreadTerminated(&storage.sdk)&&!OSIsThreadSuspended(&storage.sdk),"Blocked live source worker was falsely terminated/suspended");
}
struct RunningJob { std::atomic<bool> entered{}, release{}; void* result{}; };
void* RunningWorker(void* argument) {
    auto& job=*static_cast<RunningJob*>(argument);
    ++entries; job.entered=true;
    while (!job.release) std::this_thread::yield();
    ++returns; return job.result;
}
std::atomic<bool> failed_frame_destroyed{};
void* FailedWorker(void*) {
    struct Frame { ~Frame() { failed_frame_destroyed=true; } } frame;
    ++entries;
    throw std::runtime_error("Actual native source entry failure");
}
std::atomic<bool> owner_pending{};
OSThread* suspended_owner{};
std::atomic<unsigned> owner_irqs{}, owner_services{};
std::thread::id owner_id;
OSContext* owner_context{};
void ResumeIRQ() {
    Check(std::this_thread::get_id()==owner_id&&NativeInterruptDispatchActive()&&!NativeInterruptsEnabled(),
        "SDK Resume IRQ escaped actual owner/exclusion");
    Check(OSGetCurrentContext()!=owner_context,"Resume IRQ lacked temporary native context");
    Check(OSResumeThread(suspended_owner)==1&&!NativeInterruptsEnabled(),"Original Resume IRQ changed caller mask/counter");
    ++owner_irqs;
}
void OwnerService() {
    ++owner_services;
    Check(std::this_thread::get_id()==owner_id&&NativeInterruptsEnabled(),"SDK lifecycle wait service escaped actual owner");
    if (owner_pending.exchange(false))Check(DispatchNativeInterrupt(ResumeIRQ),"Latched actual owner Resume IRQ not delivered");
}
}
int main() {
    try {
        static_assert(sizeof(OSThread)==856&&alignof(OSThread)==8);
        static_assert(sizeof(OSThread::suspend)==4&&sizeof(OSPriority)==4);
        owner_id=std::this_thread::get_id();owner_context=OSGetCurrentContext();
        auto* caller=OSGetCurrentThread();
        auto payload=std::make_unique<unsigned>(0x1234abcdu);
        Check(std::uintptr_t(payload.get())>0xffffffffULL,"Actual native pointer owner is not above4GiB");
        {
            Owned invalid;auto before=invalid.sdk;
            for (s32 priority:{-1,32})Check(!OSCreateThread(&invalid.sdk,Worker,nullptr,invalid.Top(),0x4000,priority,0)&&
                std::memcmp(&invalid.sdk,&before,sizeof(before))==0,"Invalid priority changed original creation return/descriptor");
        }
        {
            Owned storage;Job job;Prepare(storage,job,payload.get());Create(storage,job,0xfffe);
            Check(OSSuspendThread(&storage.sdk)==1&&storage.sdk.suspend==2,"Nested parked suspension lost original counter");
            Check(OSSetThreadPriority(&storage.sdk,11)&&storage.sdk.base==11&&storage.sdk.priority==14,
                "Suspended source priority changes affected runnable priority early");
            Check(OSResumeThread(&storage.sdk)==2&&storage.sdk.suspend==1&&!job.entered,"Nested Resume executed a still-suspended source worker");
            StartAndWait(storage,job);
            Check(storage.sdk.priority==11,"Resume did not restore original source base priority");
            Reject([&]{ DrainNativeThreadLifetimes(); },"Teardown accepted an active source callback/descriptor");
            Reject([&]{OSCancelThread(&storage.sdk);},"Running source cancellation silently killed native C++ frames");
            Reject([&]{OSCreateThread(&storage.sdk,Worker,&job,storage.Top(),0x4000,14,0);},"Native Create accepted still-borrowed descriptor");
            Check(OSSuspendThread(&storage.sdk)==0&&storage.sdk.priority==32&&storage.sdk.suspend==1,
                "Actual blocked source suspension did not preserve sentinel priority/counter");
            Check(OSSendMessage(&job.queue,payload.get(),0),"Original send to suspended source worker failed");
            std::this_thread::sleep_for(3ms);
            Check(!job.returned&&!OSIsThreadTerminated(&storage.sdk),"Actual wake executed a still-suspended source callback");
            Check(OSResumeThread(&storage.sdk)==1,"Original suspended-blocked Resume counter changed");
            void* output{};
            Check(OSJoinThread(&storage.sdk,&output)&&output==payload.get()&&job.returned,
                "Real attached join lost native pointer result/source lifetime");
            Check(storage.sdk.state==0&&OSIsThreadTerminated(&storage.sdk),"Real join did not retire source state");
            auto* marker=reinterpret_cast<void*>(std::uintptr_t{0xfedcba9876543210ULL});output=marker;
            std::atomic<bool> repeat_returned{};
            std::thread repeat([&] {
                Check(!OSJoinThread(&storage.sdk,&output)&&output==marker,
                    "Repeated source join changed its post-wake FALSE/output decision");
                repeat_returned=true;
            });
            Until([&]{return Waiters(storage.sdk.queueJoin)==1;});
            Check(!repeat_returned,"Native platform repaired original attached EXITED rejoin wait quirk");
            OSDetachThread(&storage.sdk);
            repeat.join();
            Check(repeat_returned,"Source Detach did not release actual repeated-Join waiter");
            DrainNativeThreadLifetimes();
        }
        {
            Owned storage;Job job;Prepare(storage,job,payload.get());Create(storage,job,1);
            void* output=reinterpret_cast<void*>(std::uintptr_t{0xa5a5a5a5a5a5a5a5ULL});
            Check(!OSJoinThread(&storage.sdk,&output)&&std::uintptr_t(output)==0xa5a5a5a5a5a5a5a5ULL,
                "Detached live source join gained success or wrote output");
            StartAndWait(storage,job);
            Check(OSSendMessage(&job.queue,payload.get(),0),"Original detached callback wake failed");
            Until([&]{return OSIsThreadTerminated(&storage.sdk);});
            Check(storage.sdk.state==0&&storage.sdk.val==reinterpret_cast<void*>(std::uintptr_t{0xffffffffu}),
                "Detached source exit gained attached return-value writes");
            DrainNativeThreadLifetimes();
        }
        {
            Owned storage;Job job;Prepare(storage,job,payload.get());Create(storage,job);
            Check(OSSendMessage(&job.queue,payload.get(),0),"Actual completed worker setup send failed");
            Check(OSResumeThread(&storage.sdk)==1,"Completed worker Resume failed");
            Until([&]{return OSIsThreadTerminated(&storage.sdk);});
            Check(storage.sdk.state==OS_THREAD_STATE_MORIBUND,"Attached source worker skipped original MORIBUND state");
            Reject([&]{DrainNativeThreadLifetimes();},"Teardown accepted unjoined attached source lifetime");
            OSDetachThread(&storage.sdk);
            Check(storage.sdk.state==0&&storage.sdk.attr==1,"Source Detach failed to retire actual MORIBUND thread");
            DrainNativeThreadLifetimes();
        }
        for (u16 attr:{u16(0),u16(1)}) {
            Owned storage;Job job;Prepare(storage,job,payload.get());Create(storage,job,attr);
            const auto before=entries.load();OSCancelThread(&storage.sdk);
            Check(!job.entered&&entries==before&&OSIsThreadTerminated(&storage.sdk),
                "Cancelled parked worker executed a source callback or remained live");
            if (!attr) {void* output{};Check(OSJoinThread(&storage.sdk,&output)&&std::uintptr_t(output)==0xffffffffu,
                "Parked attached cancellation changed source join/default result");}
            else Check(storage.sdk.state==0,"Detached parked cancellation failed to retire real worker");
            DrainNativeThreadLifetimes();
        }
        {
            Owned storage;Job job;Prepare(storage,job,payload.get(),true);Create(storage,job);
            Check(OSResumeThread(&storage.sdk)==1,"Self-suspending source worker Resume failed");
            Until([&]{return job.entered&&OSIsThreadSuspended(&storage.sdk);});
            Check(storage.sdk.state==OS_THREAD_STATE_READY&&!job.returned,"Source self-suspension did not really park native execution");
            Check(OSResumeThread(&storage.sdk)==1,"External Resume failed to release actual self-suspended source worker");
            Until([&]{return Waiters(job.queue.queueReceive)==1;});
            Check(OSSendMessage(&job.queue,payload.get(),0),"Self-suspended source worker message release failed");
            void* result{};Check(OSJoinThread(&storage.sdk,&result)&&result==payload.get(),"Self-suspended native source lifetime did not join");
            DrainNativeThreadLifetimes();
        }
        {
            Owned storage; Job first; Prepare(storage,first,payload.get()); Create(storage,first);
            StartAndWait(storage,first); Check(OSSendMessage(&first.queue,payload.get(),0),"Reuse first original send failed");
            void* result{}; Check(OSJoinThread(&storage.sdk,&result)&&result==payload.get(),"Reuse first original join failed");
            Job second; Prepare(storage,second,payload.get()); Create(storage,second);
            StartAndWait(storage,second); Check(OSSendMessage(&second.queue,payload.get(),0),"Reused original send failed");
            Check(OSJoinThread(&storage.sdk,&result)&&result==payload.get(),"Reused original descriptor failed to join true second worker");
            DrainNativeThreadLifetimes();
        }
        {
            Owned storage; RunningJob job; job.result=payload.get();
            Check(OSCreateThread(&storage.sdk,RunningWorker,&job,storage.Top(),0x4000,10,0),"Running source Create failed");
            Check(OSResumeThread(&storage.sdk)==1,"Running source Resume failed"); Until([&]{return job.entered.load();});
            Reject([&]{OSSuspendThread(&storage.sdk);},"External RUNNING suspension faked native preemption");
            Check(!OSIsThreadSuspended(&storage.sdk)&&storage.sdk.state==OS_THREAD_STATE_RUNNING,
                "Unsupported running Suspend changed source counter/state");
            Reject([&]{OSCancelThread(&storage.sdk);},"External RUNNING cancellation destroyed live source frames");
            job.release=true; void* result{};
            Check(OSJoinThread(&storage.sdk,&result)&&result==payload.get(),"Real running callback failed to return/join");
            DrainNativeThreadLifetimes();
        }
        {
            auto storage=std::make_unique<Owned>();
            Check(OSCreateThread(&storage->sdk,FailedWorker,nullptr,storage->Top(),0x4000,12,0),"Failing source Create failed");
            Check(OSResumeThread(&storage->sdk)==1,"Failing source Resume failed");
            Until([&]{ const auto mask=OSDisableInterrupts(); bool done=storage->sdk.state==OS_THREAD_STATE_MORIBUND;
                OSRestoreInterrupts(mask); return done; });
            Check(failed_frame_destroyed,"Actual failed source entry did not destroy its ordinary frame before completion");
            bool rejected{}; try { (void)OSIsThreadTerminated(&storage->sdk); } catch(const std::runtime_error&) {rejected=true;}
            Check(rejected,"Failing source entry became successful IsTerminated readiness");
            auto* marker=reinterpret_cast<void*>(std::uintptr_t{0xfedcba9876543210ULL}); void* result=marker;
            rejected=false; try { (void)OSJoinThread(&storage->sdk,&result); } catch(const std::runtime_error&) {rejected=true;}
            Check(rejected&&result==marker&&storage->sdk.state==0,
                "Failed source join fabricated pointer result or failed to retire actual worker");
            storage.reset(); // True join has ended source access before borrowed descriptor/stack retirement.
            DrainNativeThreadLifetimes();
        }
        {
            suspended_owner=caller;
            Check(SetNativeThreadWaitService(OwnerService)==nullptr,"Actual lifecycle owner wait slot occupied");
            std::thread producer([]{std::this_thread::sleep_for(8ms);owner_pending=true;});
            const auto mask=OSDisableInterrupts();
            Check(OSSuspendThread(caller)==0&&!NativeInterruptsEnabled(),"Actual owner self-suspension lost original caller mask");
            OSRestoreInterrupts(mask);producer.join();
            Check(owner_irqs==1&&owner_services>0&&!owner_pending,"Actual suspended owner resumed without real IRQ or repeated it");
            Check(SetNativeThreadWaitService(nullptr)==OwnerService&&OSGetCurrentContext()==owner_context,
                "Actual owner lifecycle wait leaked callback/context lifetime");
        }
        Check(OSGetCurrentThread()==caller&&OSGetCurrentContext()==owner_context&&NativeInterruptsEnabled(),
            "Native lifecycle changed original owner thread/context/mask");
        Check(entries==returns+1&&failed_frame_destroyed&&*payload==0x1234abcdu,"Source callback lifetime/payload ownership corrupted");
        std::printf("Native SDK lifecycle: %u checks, %u real worker entries (%u ordinary returns, one real entry failure), %u owner IRQ; original message TU, pointer64 returns, actual borrowedSDK856/align8. Running external Cancel/Suspend, mutex/full scheduler, HBM runtime and source network readiness remain HOLD.\n",
            checks.load(),entries.load(),returns.load(),owner_irqs.load());
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Native SDK lifecycle failure: %s (%u checks)\n",error.what(),checks.load());
        return 1;
    }
}

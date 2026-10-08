#include "platform/alarms.h"
#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include <aurora/aurora.h>
#include <aurora/hardware.h>
#include <aurora/time.hpp>
#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <revolution/os/OSAlarm.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
namespace {
using namespace mscharged::platform;
unsigned checks{}, calls{};
std::thread::id owner;
OSContext* interrupted{};
std::vector<unsigned> sequence;
OSAlarm* rearm{};
OSAlarm* cancel{};
bool throws{}, releases{}, enables{};
void Check(bool okay, const char* message) {
    ++checks; if (!okay) throw std::runtime_error(message);
}
template<class F> void Reject(F fn,const char* message) {
    bool rejected{}; try { fn(); } catch(const std::logic_error&) { rejected=true; }
    Check(rejected,message);
}
void Callback(OSAlarm* alarm,OSContext* context) {
    ++calls;
    Check(std::this_thread::get_id()==owner,"Alarm escaped real owner thread");
    Check(!NativeInterruptsEnabled(),"Alarm callback was unmasked");
    Check(context==interrupted,"Alarm lost interrupted native SDK context");
    Check(OSGetCurrentContext()!=interrupted,"Alarm did not install temporary exception context");
    Check(OSCheckAlarmQueue(),"Original periodic requeue/link invariants failed before callback");
    sequence.push_back(alarm->tag);
    Check(ServiceNativeAlarms()==0,"Alarm service reentered its active source callback");
    if (enables) {
        const auto old=OSEnableInterrupts();
        Check(ServiceNativeAlarms()==0,"Enabled callback recursively delivered another alarm");
        (void)OSGetTime();
        OSRestoreInterrupts(old);
    }
    if (cancel) { OSCancelAlarm(cancel); cancel=nullptr; }
    if (rearm) { OSSetAlarm(rearm,OSMillisecondsToTicks(1),Callback); rearm=nullptr; }
    if (releases) { releases=false; delete alarm; }
    if (throws) { throws=false; throw std::runtime_error("Intentional callback exception"); }
}
void UserDataCarrierChecks() {
    OSAlarm alarm{};
    OSCreateAlarm(&alarm);
    int context{};
    unsigned evaluations{};
    OSSetAlarmUserDataAny(&alarm, (++evaluations, &context));
    Check(evaluations == 1, "Alarm userdata expression was evaluated more than once");
    Check(OSGetAlarmUserData(&alarm) == &context,
          "Alarm userdata lost a real native pointer");
    for (int label : {-1, 0, 1, 3, 0x7fffffff}) {
        OSSetAlarmUserDataAny(&alarm, label);
        Check(reinterpret_cast<std::uintptr_t>(OSGetAlarmUserData(&alarm))
                  == static_cast<std::uintptr_t>(label),
              "Alarm userdata changed a signed numeric label");
    }
    for (u32 label : {u32(0), u32(1), u32(0x80000000), u32(0xffffffff)}) {
        OSSetAlarmUserDataAny(&alarm, label);
        Check(reinterpret_cast<std::uintptr_t>(OSGetAlarmUserData(&alarm))
                  == static_cast<std::uintptr_t>(label),
              "Alarm userdata changed an unsigned numeric label");
    }
    OSSetAlarmUserDataAny(&alarm, &context);
    Check(OSGetAlarmUserDataAny(int*, &alarm) == &context,
          "Alarm userdata numeric labels changed the real pointer domain");
}
void Hook() { (void)ServiceNativeAlarms(); }
void Sleep(unsigned milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
void Until(unsigned count) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(calls<count) {
        (void)OSGetTime();
        if (std::chrono::steady_clock::now()>end) throw std::runtime_error("Actual alarm deadline never delivered");
        std::this_thread::yield();
    }
    ++checks;
}
OSTime Add(OSTime a,OSTime b) { return std::bit_cast<OSTime>(std::uint64_t(a)+std::uint64_t(b)); }

OSThread* timed_thread{};
unsigned timed_services{}, external_resumes{};
enum class SleepServiceMode { Deadline, ExternalResume, Failure };
SleepServiceMode sleep_mode{};
void TimedSleepService() {
    ++timed_services;
    Check(std::this_thread::get_id()==owner&&OSGetCurrentThread()==timed_thread,
          "Timed sleep hardware service escaped its actual source owner");
    Check(NativeInterruptsEnabled()&&!NativeInterruptDispatchActive()&&
          OSGetCurrentContext()==interrupted,
          "Timed sleep service retained source interrupt mask/context");
    Check(timed_thread->state==OS_THREAD_STATE_READY&&timed_thread->suspend==1,
          "Timed sleep did not genuinely suspend its source thread");
    if (sleep_mode==SleepServiceMode::Failure)
        throw std::runtime_error("Intentional timed-sleep owner failure");
    if (sleep_mode==SleepServiceMode::ExternalResume) {
        Check(OSResumeThread(timed_thread)==1,"Actual early resume lost original suspend count");
        ++external_resumes;
    } else aurora_service_hardware_interrupts();
}
void RejectInterruptSleep() {
    Reject([]{OSSleepTicks(0);},"Original alarm interrupt accepted a blocking timed sleep");
}
void TimedSleepChecks() {
    timed_thread=OSGetCurrentThread();
    Check(reinterpret_cast<std::uintptr_t>(timed_thread)>0xffffffffULL,
          "Timed-sleep full-width thread identity was not above4GiB");
    Reject([]{OSSleepTicks(0);},"Timed sleep fabricated an absent owner wait service");
    Check(SetNativeThreadWaitService(TimedSleepService)==nullptr,"Owner timed-wait service already occupied");
    const auto tag=NativeThreadAlarmTag();
    Check(tag!=0&&NativeThreadAlarmTag()==tag,"Live native thread alarm identity changed");
    for (const OSTime ticks : {OSTime(OSMillisecondsToTicks(25)),OSTime(0),OSTime(-1)}) {
        const auto before=OSGetTime();
        const auto prior_services=timed_services;
        OSSleepTicks(ticks);
        const auto elapsed=OSGetTime()-before;
        Check(ticks<=0||elapsed>=ticks,"Source thread resumed before its actual alarm deadline");
        Check(timed_services>prior_services,"Timed sleep returned without genuine suspend/service");
        Check(!timed_thread->suspend&&timed_thread->state==OS_THREAD_STATE_RUNNING&&
              NativeInterruptsEnabled()&&OSGetCurrentContext()==interrupted,
              "Completed timed sleep retained source suspension/mask/context");
        Check(OSCheckAlarmQueue()&&ServiceNativeAlarms()==0,"Completed timed sleep retained its stack alarm");
        Check(NativeThreadAlarmTag()==tag,"Timed sleep changed incarnation's cancellation identity");
    }
    const auto mask=OSDisableInterrupts();
    OSSleepTicks(OSMillisecondsToTicks(2));
    Check(!NativeInterruptsEnabled()&&OSGetCurrentContext()==interrupted,
          "Timed sleep lost original caller's disabled mask/context");
    OSRestoreInterrupts(mask);

    sleep_mode=SleepServiceMode::ExternalResume;
    OSSleepTicks(OSMillisecondsToTicks(8));
    Check(external_resumes==1,"Actual early-resume request was lost or replayed");
    Sleep(12);
    Check(ServiceNativeAlarms()==0&&!timed_thread->suspend,
          "Early resume retained or later fired its borrowed stack alarm");

    sleep_mode=SleepServiceMode::Failure;
    bool failed{};
    try { OSSleepTicks(0); } catch(const std::runtime_error& e) {
        failed=std::strcmp(e.what(),"Intentional timed-sleep owner failure")==0;
    }
    Check(failed,"Failed timed sleep returned invented successful completion");
    Check(timed_thread->state==OS_THREAD_STATE_RUNNING&&!timed_thread->suspend&&
          NativeInterruptsEnabled()&&OSGetCurrentContext()==interrupted,
          "Failed timed sleep retained its own suspension/mask/context");
    Check(OSCheckAlarmQueue()&&ServiceNativeAlarms()==0,"Failed timed sleep retained borrowed stack alarm");
    sleep_mode=SleepServiceMode::Deadline;
    OSSleepTicks(0); // A genuinely restored failed host scope can make a fresh request.
    Check(DispatchNativeInterrupt(RejectInterruptSleep),"Timed-sleep IRQ rejection probe did not dispatch");
    {
        NativeInterruptGuard guard;
        Reject([]{OSSleepTicks(0);},"Timed sleep accepted an additional host exclusion");
    }
    bool foreign_rejected{};
    std::thread foreign([&]{try{OSSleepTicks(0);}catch(const std::logic_error&){foreign_rejected=true;}});
    foreign.join();
    Check(foreign_rejected,"Foreign worker timed sleep claimed unqualified alarm ownership");
    Check(SetNativeThreadWaitService(nullptr)==TimedSleepService,"Borrowed timed-wait service did not retire");
    timed_thread=nullptr;
}
}
int main() {
    try {
        Reject([]{ InitializeNativeAlarms(); },"Alarm initialization accepted absent actual SDK memory");
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size=64u*1024u*1024u;
        OSInit();
        owner=std::this_thread::get_id();
        interrupted=OSGetCurrentContext();
        Check(interrupted&&OSGetArenaLo()&&OSGetMEM2ArenaLo(),"Actual SDK memory/context unavailable");
        OSInitAlarm(); __OSInitAlarm();
        UserDataCarrierChecks();
        Check(OSCheckAlarmQueue(),"Empty source queue invariants failed");
        Check(sizeof(OSAlarm)==64 && offsetof(OSAlarm,fire)==offsetof(OSAlarm,end),"Native source alarm descriptor ABI mismatch");
        Check(aurora_register_hardware_service(Hook),"Actual SDK hardware clock hook occupied");

        OSAlarm fields;
        std::memset(&fields,0xA5,sizeof(fields));
        std::array<unsigned char,sizeof(OSAlarm)> expected{};
        std::memcpy(expected.data(),&fields,sizeof(fields));
        OSCreateAlarm(&fields);
        std::memset(expected.data()+offsetof(OSAlarm,handler),0,sizeof(fields.handler));
        std::memset(expected.data()+offsetof(OSAlarm,tag),0,sizeof(fields.tag));
        Check(std::memcmp(expected.data(),&fields,sizeof(fields))==0,"OSCreateAlarm changed noninitialized retail fields");
        std::uintptr_t wide=0x123456789ABCDEF0ULL;
        OSSetAlarmUserData(&fields,&wide);
        Check(OSGetAlarmUserData(&fields)==&wide,"Native alarm user data truncated address");

        std::array<OSAlarm,4> alarm{};
        for(auto& a:alarm) OSCreateAlarm(&a);
        const auto mask=OSDisableInterrupts();
        const OSTime deadline=Add(OSGetTime(),OSMillisecondsToTicks(8));
        for(unsigned n=0;n<alarm.size();++n) { OSSetAlarmTag(&alarm[n],n+1); OSSetAbsAlarm(&alarm[n],deadline,Callback); }
        Check(OSCheckAlarmQueue(),"Equal-deadline queue not coherent");
        OSCancelAlarm(&alarm[1]);
        OSCancelAlarm(&alarm[3]);
        Check(!alarm[1].handler&&!alarm[3].handler,"Cancellation left pending callbacks");
        Sleep(12);
        const auto before=calls;
        Check(ServiceNativeAlarms()==0&&calls==before,"Masked alarm escaped exclusion");
        OSRestoreInterrupts(mask);
        Check(ServiceNativeAlarms()==1&&calls==before+1,"Alarm service did not bound delivery to one current request");
        Check(ServiceNativeAlarms()==1&&calls==before+2,"Second equal-time alarm lost deadline");
        Check(sequence.size()==2&&sequence[0]==1&&sequence[1]==3,"Equal-time source insertion/cancellation order changed");
        Check(OSGetCurrentContext()==interrupted&&NativeInterruptsEnabled(),"Alarm context/mask not restored");
        OSCancelAlarm(&alarm[0]); OSCancelAlarm(&alarm[2]);
        Check(OSCheckAlarmQueue(),"Completed one-shot cancellation changed queue");

        // This is exactly the original AudioBackend's requested 6,666,667ns
        // hardware periodic timer, not a copied game speaker/mixer callback.
        OSCreateAlarm(&alarm[0]);
        const u32 ticks=OSNanosecondsToTicks(6666667);
        const auto startCount=calls;
        OSSetPeriodicAlarm(&alarm[0],ticks,ticks,Callback);
        Check(alarm[0].period==ticks&&alarm[0].start==ticks,"Original source timer request changed");
        Check(alarm[0].fire%ticks==0,"Periodic phase is not anchored to source absolute start");
        aurora::time::set_scale(0.f);
        Until(startCount+4);
        Check(calls>=startCount+4,"Wii hardware alarm stopped with presentation pause");
        aurora::time::set_scale(1.f);
        OSCancelAlarm(&alarm[0]);
        const auto afterCancel=calls;
        Sleep(10); (void)OSGetTime();
        Check(calls==afterCancel,"Cancelled source speaker-period timer still delivered");

        // Real missed hardware periods are skipped by original InsertAlarm,
        // not replayed as synthetic callbacks.
        OSCreateAlarm(&alarm[0]);
        OSSetPeriodicAlarm(&alarm[0],ticks,ticks,Callback);
        const auto disabled=OSDisableInterrupts();
        const auto pending=calls;
        Sleep(29);
        const auto now=OSGetTime();
        Check(calls==pending,"Pending periodic callback ran masked");
        OSRestoreInterrupts(disabled);
        Check(ServiceNativeAlarms()==1&&calls==pending+1,"Delayed periodic request did not deliver once");
        Check(alarm[0].handler&&alarm[0].fire>now&&alarm[0].fire%ticks==0,"Periodic source reinsertion phase changed");
        Check(ServiceNativeAlarms()==0,"Missed periodic occurrences became callback backlog");
        OSCancelAlarm(&alarm[0]);

        OSCreateAlarm(&alarm[0]); OSCreateAlarm(&alarm[1]);
        const auto blocking=OSDisableInterrupts();
        OSSetAlarm(&alarm[0],OSMillisecondsToTicks(1),Callback);
        OSSetAlarm(&alarm[1],OSMillisecondsToTicks(1),Callback);
        Sleep(3);
        const auto workerCount=calls;
        OSRestoreInterrupts(blocking);
        unsigned workerServices=99; bool foreignRejected{};
        std::thread worker([&]{
            workerServices=ServiceNativeAlarms(); (void)OSGetTime();
            try { OSCancelAlarm(&alarm[0]); } catch(const std::logic_error&) { foreignRejected=true; }
        }); worker.join();
        Check(workerServices==0&&calls==workerCount&&foreignRejected,"Foreign clock/API delivered or mutated source alarm");
        cancel=&alarm[1]; enables=true;
        Check(ServiceNativeAlarms()==1&&calls==workerCount+1,"Source callback cancellation did not execute");
        enables=false;
        Check(!alarm[1].handler&&ServiceNativeAlarms()==0,"Callback cancellation failed or reentered");

        OSCreateAlarm(&alarm[0]);
        OSSetAlarm(&alarm[0],OSMillisecondsToTicks(1),Callback); rearm=&alarm[0];
        const auto rearmed=calls; Until(rearmed+2);
        Check(!alarm[0].handler,"One-shot callback self-rearm was lost");

        OSCreateAlarm(&alarm[0]); OSSetAlarm(&alarm[0],OSMillisecondsToTicks(1),Callback);
        const auto expiryMask=OSDisableInterrupts(); Sleep(3); OSRestoreInterrupts(expiryMask);
        throws=true;
        bool thrown{}; try { ServiceNativeAlarms(); } catch(const std::runtime_error&) { thrown=true; }
        Check(thrown&&OSGetCurrentContext()==interrupted&&NativeInterruptsEnabled(),"Throwing callback leaked native context/mask");
        Check(!alarm[0].handler&&OSCheckAlarmQueue(),"Throwing callback left borrowed one-shot pending");

        auto* heapAlarm=new OSAlarm{}; OSCreateAlarm(heapAlarm);
        OSSetAlarm(heapAlarm,OSMillisecondsToTicks(1),Callback); releases=true;
        const auto freedCount=calls; Until(freedCount+1);
        Check(OSCheckAlarmQueue(),"Completed callback freed descriptor still borrowed by queue");

        for(unsigned n=0;n<alarm.size();++n) {
            OSCreateAlarm(&alarm[n]); OSSetAlarmTag(&alarm[n],n<2?77:0);
            OSSetAlarm(&alarm[n],OSSecondsToTicks(1),Callback);
        }
        OSCancelAlarms(0);
        Check(alarm[2].handler&&alarm[3].handler,"Canonical system tag zero batch was cancelled");
        OSCancelAlarms(77);
        Check(!alarm[0].handler&&!alarm[1].handler&&alarm[2].handler&&alarm[3].handler,"Source tag cancellation crossed owners");
        const auto retirementCount=calls;
        ShutdownNativeAlarms();
        Check(!alarm[2].handler&&!alarm[3].handler,"Alarm device retirement retained borrowed descriptors");
        Check(ServiceNativeAlarms()==0&&calls==retirementCount,"Retired source alarms fabricated completion");
        Reject([&]{OSSetAlarm(&alarm[0],1,Callback);},"Retired alarm API silently accepted source request");
        OSInitAlarm();
        Check(OSCheckAlarmQueue(),"Restart inherited prior descriptor links");
        OSCreateAlarm(&alarm[0]); OSSetAlarm(&alarm[0],OSMillisecondsToTicks(1),Callback);
        const auto restartCount=calls; Until(restartCount+1);
        TimedSleepChecks();
        ShutdownNativeAlarms();
        Check(aurora_unregister_hardware_service(Hook),"SDK alarm hook identity changed");
        AuroraOSShutdown();
        Check(!OSGetArenaLo()&&!OSGetMEM2ArenaLo(),"Actual SDK arena retirement failed");
        std::printf("Native OSAlarm: %u checks, %u actual owner callbacks, original AudioBackend periodic request; no AX/DSP/factory readiness.\n",checks,calls);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"Native OSAlarm failure: %s (%u checks/%u callbacks)\n",e.what(),checks,calls);
        return 1;
    }
}

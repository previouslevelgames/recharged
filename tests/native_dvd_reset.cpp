#include "platform/interrupts.h"
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
unsigned checks{};
std::atomic<unsigned> callbacks{};
std::atomic<s32> callback_result{};
void Check(bool okay, const char* message) {
    ++checks;
    if (!okay) throw std::runtime_error(message);
}
template<class F> void Reject(F fn, const char* message) {
    bool rejected{};
    try { fn(); } catch (const std::logic_error&) { rejected=true; }
    Check(rejected, message);
}
template<class F> void Until(F predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now()>deadline)
            throw std::runtime_error("Real DVD worker boundary timed out");
        std::this_thread::yield();
    }
}
u8 Expected(unsigned i) { return static_cast<u8>(i*37+11); }
void Completion(s32 result, DVDFileInfo*) {
    callback_result=result;
    ++callbacks;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released{};
    void Wait() {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock,std::chrono::seconds(3),[&]{return released;}))
            throw std::runtime_error("DVD test gate was not released");
    }
    void Release() {
        std::lock_guard lock(mutex);
        released=true;
        cv.notify_all();
    }
};
struct Overlay {
    Gate gate;
    bool blocked{};
    bool fail{};
    std::atomic<unsigned> handles{}, reads{}, seeks{};
    std::atomic<bool> entered{}, finished{};
    struct Handle { Overlay* owner; std::int64_t position{}; };
    static void* Open(void* user) {
        auto* owner=static_cast<Overlay*>(user);
        auto* handle=new Handle{owner};
        ++owner->handles;
        return handle;
    }
    static void Close(void* user) {
        auto* handle=static_cast<Handle*>(user);
        --handle->owner->handles;
        delete handle;
    }
    static std::int64_t Seek(void* user, std::int64_t offset, std::int32_t origin) {
        auto& handle=*static_cast<Handle*>(user);
        ++handle.owner->seeks;
        if (origin || offset<0 || offset>128) return -1;
        return handle.position=offset;
    }
    static std::int64_t Read(void* user,u8* destination,std::size_t size) {
        auto& handle=*static_cast<Handle*>(user);
        auto& owner=*handle.owner;
        ++owner.reads;
        owner.entered=true;
        if (owner.blocked) owner.gate.Wait();
        if (owner.fail) {
            owner.finished=true;
            return -1;
        }
        const auto count=std::min<std::size_t>(size,128-handle.position);
        for (std::size_t i=0;i<count;++i) destination[i]=Expected(handle.position+i);
        handle.position+=count;
        owner.finished=true;
        return count;
    }
};
Gate callback_gate;
std::atomic<bool> callback_entered{}, callback_finished{};
void BlockedCallback(s32 result, DVDFileInfo*) {
    callback_entered=true;
    callback_gate.Wait();
    // Real source callbacks may need the shared native critical section.
    const auto prior=OSDisableInterrupts();
    OSRestoreInterrupts(prior);
    callback_result=result;
    ++callbacks;
    callback_finished=true;
}
std::atomic<bool> callback_reset_rejected{}, callback_close_rejected{}, callback_mask_preserved{};
std::atomic<bool> close_callback_close_rejected{}, close_callback_reset_rejected{};
void ReentrantCallback(s32, DVDFileInfo*) {
    const auto prior=OSDisableInterrupts();
    try { __DVDPrepareReset(); } catch(const std::logic_error&) {callback_reset_rejected=true;}
    callback_mask_preserved=!NativeInterruptsEnabled();
    OSRestoreInterrupts(prior);
    try { aurora_dvd_close(); } catch(const std::logic_error&) {callback_close_rejected=true;}
    ++callbacks;
}
void CloseCanceledCallback(s32 result,DVDFileInfo*) {
    try {aurora_dvd_close();} catch(const std::logic_error&) {close_callback_close_rejected=true;}
    try {__DVDPrepareReset();} catch(const std::logic_error&) {close_callback_reset_rejected=true;}
    callback_result=result;
    ++callbacks;
}
void CheckNodRead() {
    DVDFileInfo info{};
    Check(DVDOpen("/payload.bin",&info),"Actual Nod fixture did not open");
    std::array<u8,128> bytes{};
    Check(DVDReadPrio(&info,bytes.data(),bytes.size(),64,2)==128,"Actual Nod read failed");
    for(unsigned i=0;i<bytes.size();++i) Check(bytes[i]==Expected(i+64),"Actual Nod read bytes changed");
    Check(DVDClose(&info),"Actual Nod handle did not retire");
    Check(DVDGetDriveStatus()==DVD_STATE_END,"Completed Nod transfer left the drive busy");
}
void ResetBlockedRead() {
    Overlay active, waiting;
    active.blocked=true;
    const AuroraOverlayCallbacks functions{Overlay::Open,Overlay::Close,Overlay::Read,Overlay::Seek};
    aurora_dvd_overlay_callbacks(&functions);
    const std::array<AuroraOverlayFile,2> files{{{"/reset-active.bin",&active,128},{"/reset-waiting.bin",&waiting,128}}};
    aurora_dvd_overlay_files(files.data(),files.size(),nullptr);
    DVDFileInfo first{}, queued{}, rejected{}, canceled{};
    Check(DVDOpen(files[0].fileName,&first)&&DVDOpen(files[1].fileName,&queued)&&
          DVDOpen(files[1].fileName,&rejected),"Reset fixture handles did not open");
    std::array<u8,128> first_bytes{},queued_bytes{},rejected_bytes{};
    first_bytes.fill(0x6a);queued_bytes.fill(0x6b);rejected_bytes.fill(0x6c);
    callbacks=0;
    Check(DVDReadAsyncPrio(&first,first_bytes.data(),128,0,Completion,2),"Blocked read was not admitted");
    Until([&]{return active.entered.load();});
    const auto active_drive=DVDGetDriveStatus();
    Check(DVDReadAsyncPrio(&queued,queued_bytes.data(),128,0,Completion,2),"Waiting read was not admitted");
    Check(DVDGetFileInfoStatus(&queued)==DVD_STATE_WAITING,"Queued read was not genuinely waiting");
    const auto queued_drive=DVDGetDriveStatus();
    Check(DVDOpen(files[1].fileName,&canceled),"Ordinary cancel handle did not open");
    Check(DVDReadAsyncPrio(&canceled,rejected_bytes.data(),128,0,Completion,2),"Ordinary cancel read failed admission");
    const auto canceled_return=DVDCancel(&canceled.cb);
    const auto canceled_result=callback_result.load();
    const auto canceled_state=DVDGetFileInfoStatus(&canceled);
    Check(canceled_return==DVD_RESULT_GOOD&&callbacks==1&&
          canceled_result==DVD_RESULT_CANCELED,"Normal queued cancellation lost its actual callback");
    Check(DVDClose(&canceled),"Ordinary canceled handle did not retire");
    callbacks=0;
    const auto first_callback=first.cb.callback;
    {
        const auto prior=OSDisableInterrupts();
        Reject([]{aurora_dvd_close();},"DVD media close retained the source interrupt mask while waiting");
        Check(!NativeInterruptsEnabled(),"Rejected masked close enabled source interrupts");
        OSRestoreInterrupts(prior);
    }
    {
        NativeInterruptGuard guard;
        Reject([]{__DVDPrepareReset();},"DVD reset waited while retaining a native guard");
        Reject([]{aurora_dvd_close();},"DVD close waited while retaining a native guard");
        Check(NativeInterruptsEnabled(),"Rejected guarded reset mutated caller's enabled mask");
        const auto prior=OSDisableInterrupts();
        Reject([]{__DVDPrepareReset();},"Masked DVD reset waited while retaining a native guard");
        Check(!NativeInterruptsEnabled(),"Rejected guarded reset enabled caller's original mask");
        OSRestoreInterrupts(prior);
    }
    Check(DispatchNativeInterrupt([] {
        Reject([]{__DVDPrepareReset();},"DVD reset blocked its actual hardware interrupt context");
        Reject([]{aurora_dvd_close();},"DVD close blocked its actual hardware interrupt context");
        Check(!NativeInterruptsEnabled(),"Rejected interrupt reset lost original callback mask");
        const auto prior=OSEnableInterrupts();
        Reject([]{__DVDPrepareReset();},"Explicitly enabled IRQ accepted blocking DVD reset");
        Check(NativeInterruptsEnabled(),"Rejected enabled-IRQ reset mutated explicit callback mask");
        OSRestoreInterrupts(prior);
    }),"Real hardware interrupt guard fixture did not dispatch");
    Check(first.cb.callback==first_callback&&DVDGetFileInfoStatus(&first)==DVD_STATE_BUSY&&
          DVDGetFileInfoStatus(&queued)==DVD_STATE_WAITING&&callbacks==0&&
          __DVDGetCoverStatus()==DVD_COVER_CLOSED,"Rejected reset mutated active/queued transport or cover");
    std::exception_ptr helper_error;
    std::atomic<s32> reset_drive{DVD_STATE_IGNORED};
    std::thread release([&] {
        try {
            Until([]{return __DVDGetCoverStatus()==DVD_COVER_BUSY;});
            reset_drive=DVDGetDriveStatus();
            if (active.finished || active.handles!=1 || waiting.handles!=2)
                throw std::runtime_error("Reset retired borrowed handles before real worker drain");
            if (DVDReadAsyncPrio(&rejected,rejected_bytes.data(),128,0,Completion,2))
                throw std::runtime_error("A read was accepted during reset quiescence");
            if (DVDSeekAsyncPrio(&rejected,0,nullptr,2))
                throw std::runtime_error("A seek was accepted during reset quiescence");
            Reject([]{aurora_dvd_close();},"Media close raced reset's live worker");
            Reject([]{__DVDPrepareReset();},"Overlapping reset was silently acknowledged");
        } catch (...) { helper_error=std::current_exception(); }
        active.gate.Release();
    });
    OSDisableInterrupts();
    __DVDPrepareReset();
    release.join();
    if (helper_error) std::rethrow_exception(helper_error);
    Check(NativeInterruptsEnabled(),"Original reset did not enable interrupts before its wait");
    Check(active.finished&&callbacks==0,"Reset returned before real read or delivered a suppressed callback");
    Check(first.cb.callback==nullptr&&DVDGetFileInfoStatus(&first)==DVD_STATE_CANCELED&&
          DVDGetTransferredSize(&first)==0,"Active reset cancellation state differed from original SDK");
    Check(DVDGetFileInfoStatus(&queued)==DVD_STATE_WAITING&&queued.cb.callback!=nullptr&&
          waiting.reads==0,"Silent queue discard rewrote source state or ran waiting data");
    Check(DVDGetFileInfoStatus(&rejected)==DVD_STATE_IGNORED,"Failed admission invented successful state");
    Check(active.handles==1&&waiting.handles==2,"Reset freed caller-owned request handles");
    for(auto byte:queued_bytes) Check(byte==0x6b,"Reset fabricated queued read bytes");
    for(auto byte:rejected_bytes) Check(byte==0x6c,"Failed admission wrote destination bytes");
    Check(__DVDGetCoverStatus()==DVD_COVER_CLOSED,"Drain did not retain real mounted media");
    Check(DVDClose(&first)&&DVDClose(&queued)&&DVDClose(&rejected),"Caller could not retire reset handles");
    Check(active.handles==0&&waiting.handles==0,"Actual handle retirement leaked overlays");
    // Check captured states after real drain: a failing predecessor must not
    // unwind an overlay while its worker still borrows the actual destination.
    Check(active_drive==DVD_STATE_BUSY,"Actual active DVD transfer reported an idle drive");
    Check(queued_drive==DVD_STATE_BUSY,"Queued request hid the actual executing transfer");
    Check(reset_drive==DVD_STATE_BUSY,"Reset wait reported idle before the real transfer returned");
    Check(DVDGetDriveStatus()==DVD_STATE_END,"Drained cancellation invented a busy drive");
    aurora_dvd_overlay_files(nullptr,0,nullptr);
    // Independent literal Wii ABI oracle, after real worker/handle retirement.
    // Keep result -3 distinct from the original command-state value 10.
    std::printf("Native DVD cancel ABI: return=%d callback=%d state=%d\n",
                canceled_return,canceled_result,canceled_state);
    Check(canceled_return==0&&canceled_result==-3&&canceled_state==10,
          "Literal Wii ABI: cancel returns 0, canceled callback -3, command state 10");
    Check(DVD_RESULT_GOOD==0&&DVD_RESULT_FATAL_ERROR==-1&&DVD_RESULT_IGNORED==-2&&
          DVD_RESULT_CANCELED==-3&&DVD_STATE_CANCELED==10,
          "Canonical DVD result/state numbers differ from the Wii SDK enum");
#ifdef DVD_RESULT_COVER_CLOSED
    Check(DVD_RESULT_COVER_CLOSED==-4,"Wii cover-closed result must be -4");
#else
    Check(false,"Canonical DVD header omits Wii cover-closed result -4");
#endif
}
void ResetStartedCallback() {
    DVDFileInfo info{};
    std::array<u8,128> bytes{};
    callbacks=0;
    Check(DVDOpen("/payload.bin",&info),"Callback-drain source did not open");
    Check(DVDReadAsyncPrio(&info,bytes.data(),128,0,BlockedCallback,2),"Callback-drain read failed");
    Until([]{return callback_entered.load();});
    const auto callback_drive=DVDGetDriveStatus();
    std::thread release([] {
        Until([]{return __DVDGetCoverStatus()==DVD_COVER_BUSY;});
        callback_gate.Release();
    });
    __DVDPrepareReset();release.join();
    Check(callback_finished&&callbacks==1&&callback_result==128,
          "Reset skipped/jumped an already-running callback instead of draining it");
    Check(DVDClose(&info),"Completed callback handle failed to close");
    Check(callback_drive==DVD_STATE_END,
          "Completed transfer was confused with the still-retained callback");
}
void CheckReentrant() {
    DVDFileInfo info{};std::array<u8,128> bytes{};
    callbacks=0;
    Check(DVDOpen("/payload.bin",&info),"Reentrant fixture failed to open");
    Check(DVDReadAsyncPrio(&info,bytes.data(),128,0,ReentrantCallback,2),"Reentrant fixture failed to read");
    Until([]{return callbacks.load()==1;});
    Check(DVDClose(&info),"Rejected reentrant reset prevented actual caller handle retirement");
    Check(callback_reset_rejected&&callback_close_rejected&&callback_mask_preserved,
          "DVD worker self-retirement succeeded or lost source interrupt mask");
    CheckNodRead();
}
void CloseBlockedRead() {
    Overlay active;
    active.blocked=true;
    const AuroraOverlayCallbacks functions{Overlay::Open,Overlay::Close,Overlay::Read,Overlay::Seek};
    aurora_dvd_overlay_callbacks(&functions);
    const AuroraOverlayFile file{"/close-active.bin",&active,128};
    aurora_dvd_overlay_files(&file,1,nullptr);
    DVDFileInfo first{},rejected{},queued{};
    std::array<u8,128> first_bytes{},rejected_bytes{},queued_bytes{};
    rejected_bytes.fill(0x7c);
    queued_bytes.fill(0x7d);
    Check(DVDOpen(file.fileName,&first)&&DVDOpen(file.fileName,&rejected)&&
          DVDOpen(file.fileName,&queued),"Close fixtures did not open");
    callbacks=0;
    Check(DVDReadAsyncPrio(&first,first_bytes.data(),128,0,Completion,2),"Close active read was not admitted");
    Until([&]{return active.entered.load();});
    Check(DVDReadAsyncPrio(&queued,queued_bytes.data(),128,0,CloseCanceledCallback,2),
          "Closing queued cancellation fixture did not admit");
    std::exception_ptr helper_error;
    std::thread release([&] {
        try {
            Until([]{return __DVDGetCoverStatus()==DVD_COVER_BUSY;});
            if (active.finished || active.handles!=3)
                throw std::runtime_error("Close did not retain borrowed active handles during drain");
            if (DVDReadAsyncPrio(&rejected,rejected_bytes.data(),128,0,Completion,2)||
                DVDSeekAsyncPrio(&rejected,0,nullptr,2))
                throw std::runtime_error("Close admitted a caller-side read outside the joined worker");
            Reject([]{aurora_dvd_close();},"Two close callers attempted to join the same DVD worker");
            Reject([]{__DVDPrepareReset();},"Reset overlapped actual DVD close retirement");
        } catch (...) {helper_error=std::current_exception();}
        active.gate.Release();
    });
    aurora_dvd_close();release.join();
    if (helper_error) std::rethrow_exception(helper_error);
    const auto closed_result=callback_result.load();
    const auto closed_active_state=DVDGetFileInfoStatus(&first);
    const auto closed_queued_state=DVDGetFileInfoStatus(&queued);
    Check(active.finished&&callbacks==2&&callback_result==DVD_RESULT_CANCELED,
          "Close skipped real read retirement or changed the genuine cancellation callback");
    Check(close_callback_close_rejected&&close_callback_reset_rejected&&
          DVDGetFileInfoStatus(&queued)==DVD_STATE_CANCELED,
          "Close caller's original cancellation callback corrupted overlapping lifecycle state");
    Check(DVDGetFileInfoStatus(&first)==DVD_STATE_CANCELED&&
          DVDGetFileInfoStatus(&rejected)==DVD_STATE_IGNORED&&active.reads==1,
          "Close retirement ran a rejected read or fabricated command completion");
    for(auto byte:rejected_bytes) Check(byte==0x7c,"Rejected closing read wrote destination bytes");
    Check(active.handles==3,"Media close released caller-owned opened file handles");
    Check(!DVDReadAsyncPrio(&rejected,rejected_bytes.data(),128,0,Completion,2)&&
          !DVDSeekAsyncPrio(&rejected,0,nullptr,2)&&active.reads==1&&callbacks==2,
          "Stopped worker admitted a live-handle read after media retirement");
    Check(DVDClose(&first)&&DVDClose(&rejected)&&DVDClose(&queued)&&active.handles==0,
          "Closed-media file handles could not be actually retired by their callers");
    aurora_dvd_close();
    Check(__DVDGetCoverStatus()==DVD_COVER_OPENED,"Sequential closed-media retirement was not idempotent");
    Check(DVDGetDriveStatus()==DVD_STATE_NO_DISK,"Closed media retained a drive-ready status");
    aurora_dvd_overlay_files(nullptr,0,nullptr);
    Check(closed_result==-3&&closed_active_state==10&&closed_queued_state==10,
          "Literal Wii ABI: active/queued close cancellation changed -3/10");
}

struct CancelAllProbe {
    std::array<Overlay,2> providers;
    std::array<DVDFileInfo,2> files{};
    std::array<std::array<u8,128>,2> bytes{};
    std::array<std::atomic<unsigned>,2> command_calls{}, command_order{};
    std::array<std::atomic<s32>,2> command_results{}, command_states{};
    std::atomic<unsigned> sequence{}, operation_calls{};
    unsigned operation_order{};
    s32 operation_result{};
    DVDCommandBlock* operation_argument{};
    bool close_handles{}, operation_after_drain{}, operation_handles_live{}, retired{};
    s32 operation_drive{};
    static CancelAllProbe* current;
    static void Command(s32 result,DVDFileInfo* file) {
        auto& p=*current;
        const unsigned index=file==&p.files[0]?0:file==&p.files[1]?1:2;
        if(index==2) std::terminate();
        p.command_results[index]=result;
        p.command_states[index]=DVDGetFileInfoStatus(file);
        p.command_order[index]=++p.sequence;
        ++p.command_calls[index];
    }
    static void Operation(s32 result,DVDCommandBlock* argument) {
        auto& p=*current;
        p.operation_result=result;
        p.operation_argument=argument; // observed only; active nullptr is a separate hold
        p.operation_order=++p.sequence;
        p.operation_drive=DVDGetDriveStatus(); // must be outside the worker latch
        if(p.close_handles) {
            p.operation_after_drain=p.providers[0].finished&&p.command_calls[0]==1&&p.command_calls[1]==1;
            p.operation_handles_live=p.providers[0].handles==1&&p.providers[1].handles==1;
            p.retired=DVDClose(&p.files[0])&&DVDClose(&p.files[1]);
        }
        ++p.operation_calls;
    }
};
CancelAllProbe* CancelAllProbe::current{};

void CheckCancelAllCompletion() {
    CancelAllProbe empty;
    CancelAllProbe::current=&empty;
    const auto empty_admitted=DVDCancelAllAsync(CancelAllProbe::Operation);
    const auto empty_result=empty.operation_result;
    const auto empty_calls=empty.operation_calls.load();
    const auto empty_argument=empty.operation_argument;
    CancelAllProbe::current=nullptr;
    Check(empty_admitted&&empty_calls==1&&empty_argument==nullptr,
          "Empty cancel-all lost its one actual operation callback");

    CancelAllProbe owner;
    owner.providers[0].blocked=true;
    owner.close_handles=true;
    CancelAllProbe::current=&owner;
    const AuroraOverlayCallbacks functions{Overlay::Open,Overlay::Close,Overlay::Read,Overlay::Seek};
    aurora_dvd_overlay_callbacks(&functions);
    const std::array<AuroraOverlayFile,2> files{{
        {"/cancel-all-active.bin",&owner.providers[0],128},
        {"/cancel-all-queued.bin",&owner.providers[1],128}}};
    aurora_dvd_overlay_files(files.data(),files.size(),nullptr);
    owner.bytes[0].fill(0x71);owner.bytes[1].fill(0x72);
    Check(DVDOpen(files[0].fileName,&owner.files[0])&&DVDOpen(files[1].fileName,&owner.files[1]),
          "Cancel-all actual handles did not open");
    Check(DVDReadAsyncPrio(&owner.files[0],owner.bytes[0].data(),128,0,CancelAllProbe::Command,2),
          "Cancel-all active read was not admitted");
    Until([&]{return owner.providers[0].entered.load();});
    Check(DVDReadAsyncPrio(&owner.files[1],owner.bytes[1].data(),128,0,CancelAllProbe::Command,2)&&
          DVDGetFileInfoStatus(&owner.files[1])==2,"Cancel-all queued request was not really waiting");
    bool pending_lifetimes{};
    std::exception_ptr release_error;
    std::thread release([&] {
        try {
            Until([&]{return owner.command_calls[1].load()==1;});
            pending_lifetimes=owner.operation_calls==0&&!owner.providers[0].finished&&
                owner.providers[0].handles==1&&owner.providers[1].handles==1&&
                owner.providers[1].reads==0&&owner.command_results[1]==-3;
        } catch(...) {release_error=std::current_exception();}
        owner.providers[0].gate.Release();
    });
    const auto admitted=DVDCancelAllAsync(CancelAllProbe::Operation);
    release.join();
    aurora_dvd_overlay_files(nullptr,0,nullptr);
    CancelAllProbe::current=nullptr;
    if(release_error) std::rethrow_exception(release_error);
    // All actual callbacks, worker and caller-owned handles have now retired.
    std::printf("Native DVD cancel-all: commands=%d/%d operation=%d active_argument=%s\n",
                owner.command_results[0].load(),owner.command_results[1].load(),
                owner.operation_result,owner.operation_argument?"non-null":"null (separate pointer hold)");
    Check(admitted&&pending_lifetimes,"Cancel-all invented completion before actual active worker drain");
    Check(owner.command_calls[0]==1&&owner.command_calls[1]==1&&owner.operation_calls==1,
          "Cancel-all duplicated/lost real command or operation callbacks");
    Check(owner.command_results[0]==-3&&owner.command_results[1]==-3&&
          owner.command_states[0]==10&&owner.command_states[1]==10,
          "Literal Wii ABI: cancel-all command callbacks must remain -3/state10");
    Check(owner.command_order[1]==1&&owner.command_order[0]==2&&owner.operation_order==3,
          "Cancel-all operation completed before original queued/active command callbacks");
    Check(owner.operation_after_drain&&owner.operation_handles_live&&owner.retired&&
          owner.providers[0].handles==0&&owner.providers[1].handles==0,
          "Cancel-all completion lost actual owner lifetime/reentrant handle retirement");
    Check(owner.providers[0].reads==1&&owner.providers[1].reads==0&&owner.operation_drive==0,
          "Cancel-all executed waiting I/O or retained an actual busy drive after drain");
    for(auto byte:owner.bytes[1]) Check(byte==0x72,"Canceled queued read wrote caller bytes");
    Check(owner.operation_result==0,"Literal Wii ABI: cancel-all operation completion must be GOOD0");
    Check(empty_result==0,"Literal Wii ABI: empty cancel-all completion must be GOOD0");
}

void CheckDriveFault(const char* disc, bool reset_during_error) {
    Overlay fault;
    fault.fail=true;
    fault.blocked=reset_during_error;
    const AuroraOverlayCallbacks functions{Overlay::Open,Overlay::Close,Overlay::Read,Overlay::Seek};
    aurora_dvd_overlay_callbacks(&functions);
    const AuroraOverlayFile file{"/status-error.bin",&fault,128};
    aurora_dvd_overlay_files(&file,1,nullptr);
    DVDFileInfo info{};
    std::array<u8,128> bytes{};
    bytes.fill(0x65);
    Check(DVDOpen(file.fileName,&info),"Actual drive-error fixture did not open");
    callbacks=0;
    Check(DVDReadAsyncPrio(&info,bytes.data(),128,0,Completion,2),"Actual error read was not admitted");
    if (reset_during_error) {
        Until([&]{return fault.entered.load();});
        std::thread release([&] {
            Until([]{return __DVDGetCoverStatus()==DVD_COVER_BUSY;});
            fault.gate.Release();
        });
        __DVDPrepareReset();
        release.join();
    } else {
        Until([]{return callbacks.load()==1;});
    }
    const auto callback_drive=DVDGetDriveStatus();
    const auto command_state=DVDGetFileInfoStatus(&info);
    const auto command_callback=info.cb.callback;
    const auto transferred=DVDGetTransferredSize(&info);
    // Actual DVDClose retires a completed block as END. Observe the real
    // completion before that existing close behavior, then drain the owner.
    Check(DVDClose(&info)&&fault.handles==0,"Error handle/worker did not really retire");
    Check(fault.entered&&fault.finished&&fault.reads==1,"Failed I/O was not actually performed");
    if (reset_during_error) {
        Check(callbacks==0&&command_callback==nullptr&&command_state==DVD_STATE_CANCELED,
              "Reset's original silent cancellation/result was changed by drive status");
    } else {
        Check(callbacks==1&&callback_result==DVD_RESULT_FATAL_ERROR&&
              command_state==DVD_STATE_FATAL_ERROR,
              "Negative real I/O was not preserved in the original completion");
    }
    Check(transferred==0,"Failed/canceled I/O fabricated transfer bytes");
    for(auto byte:bytes) Check(byte==0x65,"Failed read fabricated successful data");
    Check(callback_drive==DVD_STATE_FATAL_ERROR&&DVDGetDriveStatus()==DVD_STATE_FATAL_ERROR,
          "Real media failure was hidden by an idle-drive report");
    __DVDPrepareReset();
    Check(DVDGetDriveStatus()==DVD_STATE_FATAL_ERROR,
          "Cancellation/reset fabricated recovery of the actual failed medium");
    aurora_dvd_close();
    Check(DVDGetDriveStatus()==DVD_STATE_NO_DISK,"Retired faulty media remained drive-ready");
    aurora_dvd_overlay_files(nullptr,0,nullptr);
    Check(aurora_dvd_open(disc)&&DVDGetDriveStatus()==DVD_STATE_END,
          "A genuine new media owner retained its predecessor's drive fault");
    CheckNodRead();
}

struct FatalDispatch {
    static constexpr unsigned count=5;
    std::array<Overlay,count> providers;
    std::array<DVDFileInfo,count> files{};
    std::array<std::array<u8,128>,count> bytes{};
    std::array<std::atomic<unsigned>,count> completions{};
    std::array<std::atomic<s32>,count> results{}, states{}, drives{};
    std::atomic<bool> reentrant_admitted{}, first_entered{}, first_returned{};
    Gate first_callback;
    static FatalDispatch* current;

    static void Callback(s32 result, DVDFileInfo* file) {
        auto& owner=*current;
        unsigned index{};
        while (index<count && file!=&owner.files[index]) ++index;
        if (index==count) std::terminate();
        owner.results[index]=result;
        owner.states[index]=DVDGetFileInfoStatus(file);
        owner.drives[index]=DVDGetDriveStatus();
        ++owner.completions[index];
        if (index==0) {
            // Actual callback reentry admits a new original async request;
            // neither the fixture nor the worker publishes its completion.
            owner.reentrant_admitted=DVDReadAsyncPrio(&owner.files[3],owner.bytes[3].data(),128,0,Callback,2);
            owner.first_entered=true;
            owner.first_callback.Wait();
            owner.first_returned=true;
        }
    }
};
FatalDispatch* FatalDispatch::current{};

void CheckFatalDispatch(const char* disc, bool reset_during_callback) {
    FatalDispatch owner;
    FatalDispatch::current=&owner;
    owner.providers[0].fail=true;
    owner.providers[0].blocked=true;
    const AuroraOverlayCallbacks functions{Overlay::Open,Overlay::Close,Overlay::Read,Overlay::Seek};
    aurora_dvd_overlay_callbacks(&functions);
    const std::array<AuroraOverlayFile,FatalDispatch::count> overlay{{
        {"/dispatch-fault.bin",&owner.providers[0],128},
        {"/dispatch-waiting-read.bin",&owner.providers[1],128},
        {"/dispatch-waiting-seek.bin",&owner.providers[2],128},
        {"/dispatch-reentrant.bin",&owner.providers[3],128},
        {"/dispatch-after-latch.bin",&owner.providers[4],128}}};
    aurora_dvd_overlay_files(overlay.data(),overlay.size(),nullptr);
    for (unsigned i=0;i<owner.count;++i) {
        owner.bytes[i].fill(static_cast<u8>(0x60+i));
        Check(DVDOpen(overlay[i].fileName,&owner.files[i]),"Fatal dispatch handle did not open");
    }
    Check(DVDReadAsyncPrio(&owner.files[0],owner.bytes[0].data(),128,0,FatalDispatch::Callback,2),
          "Real initial faulty I/O was not admitted");
    Until([&]{return owner.providers[0].entered.load();});
    const auto read_admitted=DVDReadAsyncPrio(&owner.files[1],owner.bytes[1].data(),128,0,FatalDispatch::Callback,2);
    const auto seek_admitted=DVDSeekAsyncPrio(&owner.files[2],0,FatalDispatch::Callback,2);
    const auto read_waiting=DVDGetFileInfoStatus(&owner.files[1]);
    const auto seek_waiting=DVDGetFileInfoStatus(&owner.files[2]);
    owner.providers[0].gate.Release();
    Until([&]{return owner.first_entered.load();});
    const auto latched_drive=DVDGetDriveStatus();
    const auto late_admitted=DVDReadAsyncPrio(&owner.files[4],owner.bytes[4].data(),128,0,FatalDispatch::Callback,2);
    const auto late_waiting=DVDGetFileInfoStatus(&owner.files[4]);
    const auto reentrant_waiting=DVDGetFileInfoStatus(&owner.files[3]);
    if (reset_during_callback) {
        std::thread release([&] {
            Until([]{return __DVDGetCoverStatus()==DVD_COVER_BUSY;});
            owner.first_callback.Release();
        });
        __DVDPrepareReset();
        release.join();
    } else {
        owner.first_callback.Release();
        Until([&] {
            unsigned calls{};
            for (auto& count:owner.completions) calls+=count.load();
            return calls==owner.count;
        });
    }

    std::array<s32,FatalDispatch::count> final_states{};
    std::array<u32,FatalDispatch::count> transferred{};
    std::array<DVDCBCallback,FatalDispatch::count> callbacks_at_drain{};
    bool all_closed=true;
    for (unsigned i=0;i<owner.count;++i) {
        final_states[i]=DVDGetFileInfoStatus(&owner.files[i]);
        transferred[i]=DVDGetTransferredSize(&owner.files[i]);
        callbacks_at_drain[i]=owner.files[i].cb.callback;
        all_closed=DVDClose(&owner.files[i])&&all_closed;
    }
    const auto final_drive=DVDGetDriveStatus();
    __DVDPrepareReset();
    const auto reset_drive=DVDGetDriveStatus();
    aurora_dvd_close();
    const auto retired_drive=DVDGetDriveStatus();
    aurora_dvd_overlay_files(nullptr,0,nullptr);
    FatalDispatch::current=nullptr;

    // Every borrowed worker/callback/handle is truly drained before checking
    // a negative predecessor; no failing assertion frees a live overlay.
    Check(all_closed&&owner.first_returned,"Fatal callback/handle did not really drain");
    Check(read_admitted&&seek_admitted&&late_admitted&&owner.reentrant_admitted,
          "Original queued/subsequent/reentrant request admission changed");
    Check(read_waiting==DVD_STATE_WAITING&&seek_waiting==DVD_STATE_WAITING&&
          reentrant_waiting==DVD_STATE_WAITING&&late_waiting==DVD_STATE_WAITING,
          "The actual waiting request boundary was bypassed");
    Check(owner.providers[0].reads==1&&owner.providers[0].seeks==1,
          "Initial physical fault was not established by real I/O");
    Check(owner.completions[0]==1&&owner.results[0]==DVD_RESULT_FATAL_ERROR&&
          owner.states[0]==DVD_STATE_FATAL_ERROR&&owner.drives[0]==DVD_STATE_FATAL_ERROR,
          "Initial real fault callback/status changed");
    for (unsigned i=1;i<owner.count;++i) {
        Check(owner.providers[i].reads==0&&owner.providers[i].seeks==0,
              "Latched fatal drive executed queued/subsequent media I/O");
        if (reset_during_callback) {
            Check(owner.completions[i]==0&&callbacks_at_drain[i]!=nullptr&&final_states[i]==DVD_STATE_WAITING,
                  "Real reset's silent waiting-queue unlink changed source fields");
        } else {
            Check(owner.completions[i]==1&&owner.results[i]==DVD_RESULT_FATAL_ERROR&&
                  owner.states[i]==DVD_STATE_FATAL_ERROR&&owner.drives[i]==DVD_STATE_FATAL_ERROR&&
                  final_states[i]==DVD_STATE_FATAL_ERROR,
                  "Original post-fatal command callback/status was not delivered");
        }
    }
    for (unsigned i=0;i<owner.count;++i) {
        Check(owner.providers[i].handles==0,"Fatal dispatch retained a retired provider handle");
        Check(transferred[i]==0,"Fatal/canceled dispatch fabricated transferred bytes");
        for (auto byte:owner.bytes[i]) Check(byte==static_cast<u8>(0x60+i),
                                            "Fatal/canceled dispatch changed a real destination");
    }
    Check(latched_drive==DVD_STATE_FATAL_ERROR&&final_drive==DVD_STATE_FATAL_ERROR&&
          reset_drive==DVD_STATE_FATAL_ERROR&&retired_drive==DVD_STATE_NO_DISK,
          "Post-fatal dispatch/reset/retirement changed the actual media fault");
    Check(aurora_dvd_open(disc)&&DVDGetDriveStatus()==DVD_STATE_END,
          "Genuine new media owner failed to admit a fresh physical request");
    CheckNodRead();
}
} // namespace

int main(int argc,char** argv) {
    try {
        Check(argc==2,"Supply the existing synthetic Wii disc fixture");
        Check(__DVDGetCoverStatus()==DVD_COVER_OPENED,"Unmounted native medium reported closed cover");
        Check(DVDGetDriveStatus()==DVD_STATE_NO_DISK,"Absent media reported a ready drive");
        __DVDPrepareReset();
        Check(NativeInterruptsEnabled(),"Idle reset lost original enabled-mask outcome");
        Check(!aurora_dvd_open("/nonexistent/charged-reset-fixture.iso")&&
              __DVDGetCoverStatus()==DVD_COVER_OPENED,"Failed mount invented closed media");
        Check(DVDGetDriveStatus()==DVD_STATE_NO_DISK,"Failed mount fabricated drive readiness");
        Check(aurora_dvd_open(argv[1]),"Real Nod data partition mount failed");
        Check(__DVDGetCoverStatus()==DVD_COVER_CLOSED,"Mounted native medium lacked truthful cover");
        Check(DVDGetDriveStatus()==DVD_STATE_END,"Mounted idle drive reported a pending transfer");
        CheckNodRead();ResetBlockedRead();ResetStartedCallback();CheckReentrant();
        CheckCancelAllCompletion();
        CloseBlockedRead();
        Check(__DVDGetCoverStatus()==DVD_COVER_OPENED,"Closed media retained published cover");
        __DVDPrepareReset();
        Check(__DVDGetCoverStatus()==DVD_COVER_OPENED,"Closed idle reset fabricated disc-ready state");
        Check(DVDGetDriveStatus()==DVD_STATE_NO_DISK,"Empty reset fabricated a mounted drive");
        Check(aurora_dvd_open(argv[1])&&__DVDGetCoverStatus()==DVD_COVER_CLOSED,
              "Successful genuine remount did not restore worker admission");
        CheckNodRead();
        CheckDriveFault(argv[1],false);
        CheckDriveFault(argv[1],true);
        CheckFatalDispatch(argv[1],false);
        CheckFatalDispatch(argv[1],true);
        aurora_dvd_close();
        Check(__DVDGetCoverStatus()==DVD_COVER_OPENED,"Remounted media failed to retire");
        std::printf("Native DVD reset: %u checks; real Nod/blocked reads/callback drain/handle lifetime passed\n",checks);
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"Native DVD reset failed: %s\n",e.what());return 1;}
}

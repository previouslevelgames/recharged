#include "platform/ios_device.h"
#include "platform/interrupts.h"
#include "platform/ios_revision_policy.h"
#include <revolution/os/OSIOSRev.h>
#include <dolphin/os.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
unsigned checks;
void Check(bool value, const char* message) {
    ++checks;
    if(!value) throw std::runtime_error(message);
}
template<class F> void Reject(F function,const char* message) {
    bool rejected=false;
    try { function(); } catch(const std::logic_error&) { rejected=true; }
    Check(rejected,message);
}
// Explicit test hardware: two independent devices deliberately use the same
// local descriptor. This does not provide a successful filesystem/ES stub.
struct Device {
    const char* path;
    std::array<unsigned char,64> data{};
    bool open{};
    unsigned operations{};
    static bool Owns(void* context,const char* path) {
        return path && std::strcmp(static_cast<Device*>(context)->path,path)==0;
    }
    static s32 Execute(void* context,const IPCRequest& r) {
        auto& d=*static_cast<Device*>(context);++d.operations;
        if(r.type==IPC_REQ_OPEN) {
            if(d.open) return IPC_RESULT_OPENFD;
            d.open=true;return 7;
        }
        if(!d.open || r.fd!=7) return IPC_RESULT_INVALID;
        if(r.type==IPC_REQ_CLOSE) { d.open=false;return IPC_RESULT_OK; }
        if(r.type==IPC_REQ_READ || r.type==IPC_REQ_WRITE) {
            if(!r.rw.data || r.rw.length>d.data.size()) return IPC_RESULT_INVALID;
            if(r.type==IPC_REQ_READ) std::memcpy(r.rw.data,d.data.data(),r.rw.length);
            else std::memcpy(d.data.data(),r.rw.data,r.rw.length);
            return r.rw.length;
        }
        return IPC_RESULT_INVALID;
    }
};
struct Receipt {
    std::thread::id owner;
    OSContext* prior;
    unsigned calls{};
    s32 result{-999};
    bool throws{};
    NativeIOSDeviceLease retire{};
    static s32 Receive(s32 result,void* context) {
        auto& receipt=*static_cast<Receipt*>(context);
        Check(std::this_thread::get_id()==receipt.owner,"IOS callback ran on a submitter");
        Check(!NativeInterruptsEnabled() && NativeInterruptDispatchActive(),
              "IOS completion did not enter the actual native interrupt exclusion");
        Check(OSGetCurrentContext()!=receipt.prior,"IOS completion reused source thread context");
        Check(!ServiceNativeIOSRequests(),"IOS callback recursively delivered another request");
        Reject([&]{ConfigureNativeIOSServiceRevision();},"Active source callback changed IOS metadata");
        if(receipt.retire.generation)
            Reject([&]{UnregisterNativeIOSDevice(receipt.retire);},"Active IOS image retired");
        ++receipt.calls;receipt.result=result;
        if(receipt.throws) throw std::runtime_error("Explicit callback exception fixture");
        return 0;
    }
};
void Run() {
    static_assert(sizeof(OSIOSRev)==8 && offsetof(OSIOSRev,buildYear)==6);
    OSIOSRev revision;std::memset(&revision,0xa5,sizeof(revision));
    const auto sentinel=revision;
    Reject([&]{__OSGetIOSRev(&revision);},"Absent IOS bus supplied revision metadata");
    Check(std::memcmp(&revision,&sentinel,sizeof(revision))==0,"Failed IOS query modified output");
    Reject([&]{ConfigureNativeIOSServiceRevision();},"Metadata configured without actual devices");
    Check(!ServiceNativeIOSRequests(),"Absent IOS device fabricated completion");
    Check(IOS_Open("/dev/missing",IPC_OPEN_NONE)==IPC_RESULT_NOEXISTS,"Missing device opened");
    Device first{"/test/first"},second{"/test/second"};
    auto a=RegisterNativeIOSDevice({&first,Device::Owns,Device::Execute});
    auto b=RegisterNativeIOSDevice({&second,Device::Owns,Device::Execute});
    Reject([&]{RegisterNativeIOSDevice({&first,Device::Owns,Device::Execute});},"Same device registered twice");
    Reject([&]{__OSGetIOSRev(&revision);},"Live unconfigured IOS bus supplied metadata");
    Check(std::memcmp(&revision,&sentinel,sizeof(revision))==0,"Unconfigured query modified output");
    std::atomic<bool> configuration_rejected{};
    std::thread metadata_worker([&] {
        try {ConfigureNativeIOSServiceRevision();} catch(const std::logic_error&) {configuration_rejected=true;}
    });
    metadata_worker.join();Check(configuration_rejected,"Foreign thread configured IOS metadata");
    ConfigureNativeIOSServiceRevision();
    Reject([&]{ConfigureNativeIOSServiceRevision();},"Live IOS revision was replaced");
    __OSGetIOSRev(&revision);
    Check(revision.idHi==0x4e && revision.idLo==0x53 && revision.verMajor==1 && revision.verMinor==0,
          "Native service ABI identity misreported as Wii firmware");
    Check(detail::ValidNativeIOSBuildDate(revision),"Provider compilation date is not representable");
    constexpr auto leap=detail::NativeIOSRevisionForBuild("Feb 29 2024");
    constexpr auto single=detail::NativeIOSRevisionForBuild("Oct  8 2026");
    constexpr auto end=detail::NativeIOSRevisionForBuild("Dec 31 2099");
    static_assert(leap.buildMon==2 && leap.buildDay==29 && leap.buildYear==2024);
    static_assert(single.buildMon==10 && single.buildDay==8 && single.buildYear==2026);
    static_assert(end.buildMon==12 && end.buildDay==31 && end.buildYear==2099);
    static_assert(detail::ValidNativeIOSBuildDate(leap));
    static_assert(!detail::ValidNativeIOSBuildDate(detail::NativeIOSRevisionForBuild("Feb 29 2023")));
    std::cout<<"native_ios revision: NS "<<unsigned(revision.verMajor)<<'.'<<unsigned(revision.verMinor)
             <<" build "<<revision.buildYear<<'-'<<unsigned(revision.buildMon)<<'-'<<unsigned(revision.buildDay)<<'\n';
    OSIOSRev worker_revision{};
    std::thread query_worker([&] {__OSGetIOSRev(&worker_revision);});query_worker.join();
    Check(std::memcmp(&revision,&worker_revision,sizeof(revision))==0,"Read-only IOS metadata query changed on worker");
    bool null_rejected=false;
    try {__OSGetIOSRev(nullptr);} catch(const std::invalid_argument&) {null_rejected=true;}
    Check(null_rejected,"Null IOS revision destination accepted");
    const auto fa=IOS_Open(first.path,IPC_OPEN_RW),fb=IOS_Open(second.path,IPC_OPEN_RW);
    Check(fa>=0 && fb>=0 && fa!=fb,"Independent local descriptors collided at source interface");
    Check(IOS_Open(first.path,IPC_OPEN_RW)==IPC_RESULT_OPENFD,"Duplicate physical open succeeded");
    std::array<unsigned char,64> input,output{};input.fill(0x6d);
    Check(IOS_Write(fa,input.data(),input.size())==64,"Actual first device write failed");
    Check(IOS_Read(fb,output.data(),output.size())==64 && output[0]==0,
          "Descriptor routing wrote a different device");
    Receipt receipt{std::this_thread::get_id(),OSGetCurrentContext()};receipt.retire=a;
    std::atomic<bool> submitted{},worker_rejected{};
    std::thread worker([&] {
        submitted=IOS_ReadAsync(fa,output.data(),output.size(),Receipt::Receive,&receipt)==0;
        try { ServiceNativeIOSRequests(); } catch(const std::logic_error&) { worker_rejected=true; }
    });
    worker.join();
    Check(submitted && worker_rejected && receipt.calls==0 && output[0]==0,
          "Submitting worker performed owner completion/device work");
    Reject([&]{UnregisterNativeIOSDevice(a);},"Pending source callback image retired");
    const auto enabled=OSDisableInterrupts();
    Check(!ServiceNativeIOSRequests() && receipt.calls==0 && output[0]==0,
          "Masked IOS completion consumed request");
    OSRestoreInterrupts(enabled);
    Check(ServiceNativeIOSRequests() && receipt.calls==1 && receipt.result==64 && output==input,
          "Real owner completion lost the full-width callback/buffer");
    Check(NativeInterruptsEnabled() && OSGetCurrentContext()==receipt.prior,
          "IOS completion leaked source IRQ/context");
    input.fill(0x47);
    Check(IOS_WriteAsync(fa,input.data(),input.size(),Receipt::Receive,&receipt)==0,
          "Ordered async write rejected");
    Check(IOS_Read(fa,output.data(),output.size())==64 && output==input && receipt.calls==1,
          "Synchronous operation bypassed earlier work or delivered callback on submitter");
    Check(ServiceNativeIOSRequests() && receipt.calls==2,"Completed ordered request lost receipt");
    Check(IOS_ReadAsync(fa,output.data(),4,nullptr,nullptr)==4,
          "Source ipcclt null-callback synchronous contract changed");
    // Revision reads are independent of actual pending source callbacks.
    Check(IOS_ReadAsync(fa,output.data(),1,Receipt::Receive,&receipt)==0,"Metadata queue fixture rejected");
    const auto metadata_state=GetNativeIOSStatus();
    OSIOSRev pending_revision{};__OSGetIOSRev(&pending_revision);
    Check(std::memcmp(&revision,&pending_revision,sizeof(revision))==0 &&
          GetNativeIOSStatus().pending==metadata_state.pending && receipt.calls==2,
          "Revision query performed hardware work or source completion");
    Check(ServiceNativeIOSRequests() && receipt.calls==3,"Metadata query lost real pending work");
    const auto prior=receipt.calls;
    for(unsigned i=0;i<64;++i)
        Check(IOS_ReadAsync(fa,output.data(),1,Receipt::Receive,&receipt)==0,"Transport exhausted early");
    Check(IOS_ReadAsync(fa,output.data(),1,Receipt::Receive,&receipt)==IPC_RESULT_BUSY_INTERNAL,
          "Actual transport exhaustion silently accepted an extra request");
    for(unsigned i=0;i<64;++i) Check(ServiceNativeIOSRequests(),"Queued receipt disappeared");
    Check(receipt.calls==prior+64 && !ServiceNativeIOSRequests(),"Exhausted queue duplicate callback");
    receipt.throws=true;
    Check(IOS_ReadAsync(fa,output.data(),1,Receipt::Receive,&receipt)==0,"Exception request rejected");
    bool threw=false;try { ServiceNativeIOSRequests(); } catch(const std::runtime_error&) { threw=true; }
    Check(threw && !GetNativeIOSStatus().active && GetNativeIOSStatus().pending==0 &&
              NativeInterruptsEnabled() && OSGetCurrentContext()==receipt.prior,
          "Callback exception left pending identity, active lease or IRQ/context");
    receipt.throws=false;
    Check(IOS_Close(fa)==0 && IOS_Close(fb)==0,"Actual local device close failed");
    Check(IOS_Read(fa,output.data(),1)==IPC_RESULT_INVALID,"Closed descriptor reused");
    UnregisterNativeIOSDevice(a);
    OSIOSRev remaining_revision{};__OSGetIOSRev(&remaining_revision);
    Check(std::memcmp(&revision,&remaining_revision,sizeof(revision))==0,
          "One device retirement invalidated another live bus member");
    UnregisterNativeIOSDevice(b);
    std::memset(&revision,0xa5,sizeof(revision));
    Reject([&]{__OSGetIOSRev(&revision);},"Retired IOS bus supplied stale metadata");
    Check(std::memcmp(&revision,&sentinel,sizeof(revision))==0,"Retired query modified output");
    Reject([&]{UnregisterNativeIOSDevice(a);},"Stale device lease retired a successor");
    a=RegisterNativeIOSDevice({&first,Device::Owns,Device::Execute});
    Reject([&]{__OSGetIOSRev(&revision);},"New device incarnation inherited retired metadata");
    Check(std::memcmp(&revision,&sentinel,sizeof(revision))==0,"New unconfigured query modified output");
    auto next=IOS_Open(first.path,IPC_OPEN_RW);
    Receipt pending{std::this_thread::get_id(),OSGetCurrentContext()};
    Check(IOS_ReadAsync(next,output.data(),1,Receipt::Receive,&pending)==0,"New metadata pending request rejected");
    Reject([&]{ConfigureNativeIOSServiceRevision();},"Metadata configured across a pending callback");
    Check(ServiceNativeIOSRequests() && pending.calls==1,"Real callback did not drain before metadata setup");
    ConfigureNativeIOSServiceRevision();__OSGetIOSRev(&revision);
    Check(revision.idHi==0x4e && revision.idLo==0x53,"New live IOS bus configuration failed");
    Check(next!=fa && next!=fb && a.generation!=b.generation,"Reload reused source descriptor identity");
    Check(IOS_Read(fa,output.data(),1)==IPC_RESULT_INVALID,"Old handle accessed reloaded device");
    Check(IOS_Close(next)==0,"Reloaded physical close failed");UnregisterNativeIOSDevice(a);
    const auto state=GetNativeIOSStatus();
    Check(!state.devices && !state.descriptors && !state.pending && !state.active,
          "IOS hardware/receipt ownership did not drain");
    std::cout << "native_ios: " << checks
              << " checks; real device lifecycle, explicit native service revision, owner IRQs and ordered work\n";
}
}
int main() {
    try { Run();return 0; }
    catch(const std::exception& e) { std::cerr<<"IOS check "<<checks<<": "<<e.what()<<'\n';return 1; }
}

#include "fixtures/original_save_data_api.h"
#include "platform/filesystem_boot.h"
#include "platform/ios_device.h"
#include "platform/ipc_boot_buffer.h"
#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include "platform/thread.h"
#include <revolution/nand.h>
#include <revolution/os/OSIpc.h>
#include <dolphin/os.h>
#include <aurora/aurora.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <dlfcn.h>

void AuroraOSShutdown();
namespace aurora { extern AuroraConfig g_config; }
namespace {
using namespace mscharged::platform;
unsigned host_checks,irq_completions;
std::thread::id owner;
OSContext* original_context;
bool tested_foreign;
void Check(bool okay,const char* reason) {
    ++host_checks;if(!okay)throw std::runtime_error(reason);
}
}
extern "C" void SaveGateCheck(bool okay,const char* reason) {Check(okay,reason);}
extern "C" bool SaveGateIOSPending() {const auto s=GetNativeIOSStatus();return s.pending||s.active;}
extern "C" bool SaveGateOwnerContextRestored() {
    return std::this_thread::get_id()==owner&&OSGetCurrentContext()==original_context
        &&NativeInterruptsEnabled()&&!NativeInterruptDispatchActive();
}
extern "C" void SaveGateServiceIOS(bool expect_work) {
    const auto before=GetNativeIOSStatus();
    const auto mask=OSDisableInterrupts();
    Check(!ServiceNativeIOSRequests()&&GetNativeIOSStatus().pending==before.pending,
        "Masked owner consumed the real flash NAND completion");
    OSRestoreInterrupts(mask);
    if(!tested_foreign) {
        bool rejected{};
        std::thread foreign([&]{try{(void)ServiceNativeIOSRequests();}catch(const std::logic_error&){rejected=true;}});
        foreign.join();tested_foreign=true;
        Check(rejected&&GetNativeIOSStatus().pending==before.pending,
            "Foreign host thread consumed the original NAND request/callback");
    }
    unsigned deliveries{};
    while(GetNativeIOSStatus().pending) {
        Check(deliveries<64&&ServiceNativeIOSRequests(),"Real IOS source completion chain stopped or exceeded its bounded fixture");
        ++deliveries;++irq_completions;
    }
    Check(bool(deliveries)==expect_work&&SaveGateOwnerContextRestored(),
        "Actual source NAND servicing lost owner IRQ/context restoration");
}
int main(int argc,char** argv) {
    void* module{};
    try {
        if(argc!=4)throw std::invalid_argument("Expected original module, owned disc and fresh private backing");
        owner=std::this_thread::get_id();
        // Physical SDK arena configuration only; original nlInitMemory still
        // performs its own actual heap/reserve/free-list initialization.
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size=64u*1024u*1024u;
        OSInit();original_context=OSGetCurrentContext();
        alignas(32) static std::array<unsigned char,32768> boot;
        InstallNativeIPCBootBuffer(boot.data(),boot.size());__OSInitIPCBuffer();IPCInit();
        module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
        if(!module)throw std::runtime_error(dlerror());
        using Entry=void(*)(SaveGateObservation*);
        auto cold=reinterpret_cast<Entry>(dlsym(module,"SaveGateCold"));
        auto run=reinterpret_cast<Entry>(dlsym(module,"SaveGateRun"));
        Check(cold&&run,"Whole source flash diagnostic exports are missing");
        SaveGateObservation observation{};cold(&observation);
        Check(!nandIsInitialized(),"Isolated absent-device source initializer invented NAND state");
        const auto settings=InitializeNativeFilesystemForDisc({argv[2],argv[3],0x1001});
        Check(settings.title_id==0x0001000052345145ULL&&settings.gid==0x3031,
            "Owned fixture TMD identity no longer matches the exact reviewed R4QE01 data");
        Check(!nandIsInitialized(),"Native host installation called the omitted game/flash initializer");
        run(&observation);
        Check(nandIsInitialized()&&!SaveGateIOSPending()&&SaveGateOwnerContextRestored(),
            "Actual whole flash diagnostic failed to retire callbacks at its terminal boundary");
        Check(dlclose(module)==0,"Original source module terminal fixture did not unload");module=nullptr;
        ShutdownNativeFilesystem();DrainNativeThreadLifetimes();AuroraOSShutdown();
        std::printf("{\"host_checks\":%u,\"source_checks\":%u,\"checks\":%u,\"source_user_callbacks\":%u,\"original_task_runs\":%u,\"irq_completions\":%u,\"allocated_read_bytes\":%u,\"raw_read_result\":%u,\"title\":\"%016llx\",\"group\":%u,\"sdk\":45}\n",
            host_checks,observation.checks,host_checks,observation.source_user_callbacks,observation.original_task_runs,
            irq_completions,observation.allocated_read_bytes,observation.raw_read_result,
            static_cast<unsigned long long>(settings.title_id),settings.gid);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Original save-data source gate: %s (%u checks)\n",error.what(),host_checks);
        // A failed source fixture has not established safe task/arena teardown.
        // Let process termination retire this diagnostic; do not invent cleanup.
        return 1;
    }
}

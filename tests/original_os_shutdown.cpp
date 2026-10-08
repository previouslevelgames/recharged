// Whole original OSShutdownSystem on explicit disposable native devices.
// This is an SDK lifecycle leaf, not ResetTask/main/game shutdown acceptance.
#include "platform/ax_functional_device.h"
#include "platform/ax_storage_abi.h"
#include "platform/ai.h"
#include "platform/alarms.h"
#include "platform/dsp_control_abi.h"
#include "platform/dsp_mailbox_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "platform/filesystem_device.h"
#include "platform/ipc_boot_buffer.h"
#include "platform/ios_device.h"
#include "platform/rtc_device.h"
#include "platform/rtc_policy.h"
#include "platform/stm_device.h"
#include "platform/system.h"
#include "platform/video_device.h"
#include "platform/os_shutdown_record_transport.h"
#include "credits_movie_hardware.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/video.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <dolphin/ai.h>
#include <dolphin/vi.h>
#include <dolphin/dvd.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
extern "C" {
#include <revolution/ax.h>
#include <revolution/dsp.h>
#include <revolution/nand.h>
#include <revolution/os/OSIpc.h>
#include <revolution/os/OSReset.h>
#include <revolution/os/OSPlayRecord.h>
void __OSInitSram();
BOOL __OSSyncSram();
BOOL __OSInitSTM();
int fixture_play_state();
int fixture_play_alarm_pending();
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace aurora { extern AuroraConfig g_config; }
namespace {
using namespace mscharged::platform;
unsigned checks{},ios_completions{};
void Check(bool okay,const char* text) {++checks;if(!okay)throw std::runtime_error(text);}
template<class F> F Load(SDL_SharedObject* image,const char* name) {
    auto* result=SDL_LoadFunction(image,name);
    if(!result)throw std::runtime_error(std::string(name)+": "+SDL_GetError());
    return reinterpret_cast<F>(result);
}
struct Lease {
    std::string path;
    std::vector<SDL_SharedObject*> images;
    static BOOL Retain(void* context) noexcept {
        auto& lease = *static_cast<Lease*>(context);
        auto* image = SDL_LoadObject(lease.path.c_str());
        if (!image) return FALSE;
        try { lease.images.push_back(image); }
        catch (...) { SDL_UnloadObject(image); return FALSE; }
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& lease = *static_cast<Lease*>(context);
        auto* image = lease.images.back();
        lease.images.pop_back();
        SDL_UnloadObject(image);
    }
};
struct Span {
    const char* name;
    ChargedAXStorage storage;
    NativeDSPMemoryEncoding encoding;
    bool writable;
    OSNativeStaticMemory mapping{};
    NativeDSPMemoryPin pin{};
};

void ProgressOtherDevices() {
    ServiceNativeAlarms();
    if(ServiceNativeIOSRequests())++ios_completions;
    ServiceNativeSTMDevice();
}
void DeviceService(void* context) {static_cast<NativeAXFunctionalDevice*>(context)->ServiceOwner();}
void Store(const std::filesystem::path& path,const void* bytes,std::size_t count) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(static_cast<const char*>(bytes),count);
    out.flush();Check(bool(out),"Disposable evidence write failed");out.close();Check(!out.fail(),"Evidence close failed");
}
std::vector<unsigned char> Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);Check(bool(in),"Actual persistent bytes unavailable");
    return {std::istreambuf_iterator<char>(in),{}};
}
u32 BE32(const unsigned char* p) {return u32(p[0])<<24|u32(p[1])<<16|u32(p[2])<<8|p[3];}
void Put32(unsigned char* p,u32 value) {for(unsigned i=0;i<4;++i)p[i]=u8(value>>(24-i*8));}
void Checksum(unsigned char* bytes,std::size_t size) {
    u32 sum=0;for(std::size_t at=4;at<size;at+=4)sum+=BE32(bytes+at);Put32(bytes,sum);
}
void CreateRecord(const char* path,const void* bytes,u32 count) {
    Check(NANDCreate(path,NAND_PERM_RUSR|NAND_PERM_WUSR,0)==NAND_RESULT_OK,"Actual fixture record create failed");
    NANDFileInfo file{};Check(NANDOpen(path,&file,NAND_ACCESS_WRITE)==0,"Actual fixture record open failed");
    Check(NANDWrite(&file,bytes,count)==s32(count),"Actual fixture record write failed");
    Check(NANDClose(&file)==0,"Actual fixture record close failed");
}
struct Terminal {
    std::filesystem::path root;
    NativeAXFunctionalDevice* device;
    std::uint64_t before_resets{},before_frames{},before_irqs{};
    static void Verify(void* context,const NativeSTMPowerRequest& request) {
        auto& self=*static_cast<Terminal*>(context);
        Check(std::all_of(request.input.begin(),request.input.end(),[](u8 x){return x==0;}),
              "Original STM standby request bytes changed");
        const auto ai=GetNativeAIStatus();const auto dsp=GetNativeDSPMailboxStatus();
        const auto state=self.device->Status();
        Check(!ai.running&&!ai.callback_active&&!ai.interrupt_pending&&!ai.retained_blocks&&!ai.queued_input_bytes,
              "Original OS stop did not drain real AI DMA/callback storage");
        Check(!state.native_initialized&&state.protocol.hardware_halted&&state.protocol.phase==NativeAXBootstrapPhase::Cold&&
              state.protocol.resets==self.before_resets+1&&!dsp.cpu_mail_full&&!dsp.dsp_mail_full,
              "Original OS stop did not perform real native HALT/mail/reset");
        Check(state.frames.phase==NativeAXFramePhase::Unavailable&&!state.frames.completed_frames&&!state.frames.processed_frames&&
              GetNativeInterruptControllerStatus().dispatched>=self.before_irqs,
              "Actual native reset retained frame history or lost interrupt ownership");
        Check(fixture_play_state()==9&&!fixture_play_alarm_pending()&&!mscharged_os_record_pending_count(),
              "Original StopPlayRecord retains a callback or alarm");
        Check(!GetNativeIOSStatus().pending&&!GetNativeIOSStatus().active,"NAND shutdown callback remains borrowed");
        Check(ReadNativeRTCImage().flags==0,"Original shutdown failed real RTC event clear");
        bool display=true;Check(aurora_get_video_display_enabled(&display)&&!display,
              "Original STM request did not disable actual VI DCR");
        Check(!NativeInterruptsEnabled()&&OSGetCurrentThread()->state==OS_THREAD_STATE_RUNNING,
              "Source terminal caller changed its mask/thread state");
        const auto raw=Read(self.root/"nand/data/title/00000001/00000002/data/state.dat");
        Check(raw.size()==32&&raw[5]==OS_SHUTDOWN_REBOOT&&raw[6]==DVD_STATE_WAITING,
              "Original shutdown state/RTC/cover decision changed");
        std::printf("Whole original OSShutdownSystem terminal PASS: %u checks; %u real IOS completions; %llu source AX frames; no ResetTask/main/active-voice shutdown claim.\n",
            checks,ios_completions,static_cast<unsigned long long>(self.before_frames));
        std::fflush(nullptr);
        std::ofstream report(self.root/"terminal.txt");
        report<<"whole_source_shutdown=1\nchecks="<<checks<<"\nios_completions="<<ios_completions
              <<"\nax_frames="<<self.before_frames<<"\n";
        report.flush();Check(bool(report),"Terminal evidence could not be persisted");report.close();Check(!report.fail(),"Terminal evidence close failed");
    }
};
void Run(int argc,char** argv) {
    Check(argc==5&&std::strlen(argv[2])==64,"Need actual AX image/hash, synthetic disc and disposable root");
    const auto root=std::filesystem::absolute(argv[4]);std::filesystem::create_directories(root);
    aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;aurora::g_config.mem2Size=64u*1024u*1024u;
    OSInit();Check(OSGetArenaLo()&&OSGetMEM2ArenaLo(),"Actual SDK arenas unavailable");
    InitializeNativeInterruptController();InitializeNativeAlarms();
    mscharged::ConfigureNativeSystemSettings({1,0,0,0,1});
    mscharged::ConfigureNativeSystemIdleMode(nullptr,0);
    ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT,false);VIInit();
    auto rtc=CreateVirginRTCImage();rtc.flags=5; // Explicit generated device-event fixture, not a source flag.
    InitializeNativeRTC(root/"rtc.bin",rtc);__OSInitSram();Check(__OSSyncSram(),"Original SRAM cache failed real EXI reads");
    alignas(32) static std::array<unsigned char,32768> boot{};
    InstallNativeIPCBootBuffer(boot.data(),boot.size());__OSInitIPCBuffer();IPCInit();
    // Disposable privileged system-title profile permits real system-record IO.
    // It is explicitly distinct from the production game-title UID/access test.
    InitializeNativeFilesystem({root/"nand",0x0000000100000002ULL,0,0});
    Check(NANDInit()==0&&nandIsInitialized(),"Original NANDInit failed registration");
    InitializeNativeSTMDevice();ConfigureNativeIOSServiceRevision();
    Check(__OSInitSTM(),"Original STM initialization failed");
    Check(aurora_dvd_open(argv[3])&&__DVDGetCoverStatus()==DVD_COVER_CLOSED,"Actual synthetic Nod media unavailable");
    alignas(32) std::array<unsigned char,32> flags{};
    flags[4]=0x80;flags[5]=0x55;flags[6]=DVD_STATE_BUSY;flags[7]=0x5a;
    for(unsigned i=8;i<32;++i)flags[i]=u8(i*13+5);Checksum(flags.data(),flags.size());
    alignas(32) std::array<unsigned char,128> play{};
    std::memcpy(play.data()+104,"R4QE01",6);Checksum(play.data(),play.size());
    Store(root/"state-before.bin",flags.data(),flags.size());Store(root/"play-before.bin",play.data(),play.size());
    CreateRecord("/title/00000001/00000002/data/state.dat",flags.data(),flags.size());
    CreateRecord("/title/00000001/00000002/data/play_rec.dat",play.data(),play.size());
    mscharged::diagnostic::InitializeOriginalGameAudioHardware(ProgressOtherDevices);
    const auto memory=AttachNativeDSPMEM1();const auto mail=AttachNativeDSPMailboxes();const auto control=AttachNativeDSPControl(mail);
    Lease lease{std::filesystem::absolute(argv[1]).string(),{}};
    auto* image=SDL_LoadObject(lease.path.c_str());Check(image,"Actual whole AX/DSP source fixture unavailable");
    auto output = Load<decltype(&ChargedAXGetOutputStorage)>(image, "ChargedAXGetOutputStorage");
    auto task_storage = Load<decltype(&ChargedAXGetTaskStorage)>(image, "ChargedAXGetTaskStorage");
    auto command = Load<decltype(&ChargedAXGetCommandStorage)>(image, "ChargedAXGetCommandStorage");
    auto voices = Load<decltype(&ChargedAXGetVoiceStorage)>(image, "ChargedAXGetVoiceStorage");
    auto aux = Load<decltype(&ChargedAXGetAuxStorage)>(image, "ChargedAXGetAuxStorage");
    auto compressor = Load<decltype(&ChargedAXGetCompressorStorage)>(image, "ChargedAXGetCompressorStorage");
    auto studio = Load<decltype(&__AXGetStudio)>(image, "__AXGetStudio");
    ChargedAXStorage outputs[5]{}, voice_storage[2]{}, auxiliary[3]{};
    output(outputs); voices(voice_storage); aux(auxiliary);
    std::array<Span, 13> spans{{
        {"AXCommandLists", command(), NativeDSPMemoryEncoding::NativeU16, true},
        {"AXPB", voice_storage[0], NativeDSPMemoryEncoding::AXParameterBlocks, true},
        {"AXITD", voice_storage[1], NativeDSPMemoryEncoding::RawBytes, true},
        {"AXAuxA", auxiliary[0], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXAuxB", auxiliary[1], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXAuxC", auxiliary[2], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXCompressor", compressor(), NativeDSPMemoryEncoding::NativeU16, false},
        {"AXStudio", {studio(), sizeof(AXSTUDIO)}, NativeDSPMemoryEncoding::AXStudio, false},
        {"AXStereoPCM16", outputs[0], NativeDSPMemoryEncoding::NativeU16, true},
        {"AXSurround32", outputs[1], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXRemotePCM16", outputs[2], NativeDSPMemoryEncoding::NativeU16, true},
        {"AXDramContext", outputs[3], NativeDSPMemoryEncoding::RawBytes, true},
        {"AXFirmware", outputs[4], NativeDSPMemoryEncoding::RawBytes, false}
    }};
    const std::array<std::uint32_t, 13> extents{{256,30720,6144,4608,4608,3456,4032,120,1152,768,1440,64,8192}};
    for (std::size_t i = 0; i < spans.size(); ++i) {
        auto& span = spans[i];
        Check(span.storage.address && span.storage.bytes == extents[i],
              "actual source descriptor geometry differs");
        OSNativeStaticMemoryOwner owner{argv[2], span.name, span.storage.address,
            span.storage.bytes, span.writable, &lease, Lease::Retain, Lease::Release};
        span.mapping = OSNativeRegisterStaticMemory(&owner);
        span.pin = PinNativeDSPMemory(span.storage.address, span.storage.bytes,
                                      span.writable, span.encoding);
        DSPBackendValidateMemory(memory, span.mapping.physical_address,
                                 span.storage.bytes, span.writable);
    }

    NativeAXFunctionalBindings bindings{};
    for(unsigned i=0;i<spans.size();++i)bindings.addresses[i]=spans[i].mapping.physical_address;
    NativeAXFunctionalDevice device(memory,mail,control,bindings,NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1);
    mscharged::diagnostic::BindCreditsMovieDeviceService(DeviceService,&device);
    // Real platform AI prerequisite, then unchanged complete original AXInit.
    AIInit(nullptr);Load<decltype(&AXInit)>(image,"AXInit")();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(device.Status().frames.completed_frames<8&&std::chrono::steady_clock::now()<deadline){(void)OSGetTime();std::this_thread::yield();}
    Check(device.Status().frames.completed_frames>=8&&device.Status().frames.phase==NativeAXFramePhase::ReadyForListSize,
          "Whole source AX owner did not complete actual stopped frames");
    __OSStartPlayRecord();
    while(fixture_play_state()!=4&&std::chrono::steady_clock::now()<deadline){(void)OSGetTime();std::this_thread::yield();}
    Check(fixture_play_state()==4&&fixture_play_alarm_pending(),"Original play-record callbacks did not reach their real alarm");
    Check(!GetNativeIOSStatus().pending&&!mscharged_os_record_pending_count(),"Source record predecessor still owns pending IO");
    const auto before=device.Status();
    Terminal terminal{root,&device,before.protocol.resets,before.frames.completed_frames,GetNativeInterruptControllerStatus().dispatched};
    ConfigureNativeSTMPowerRemoval({&terminal,Terminal::Verify});
    OSShutdownSystem(); // Entire original function owns every decision/call below.
    throw std::runtime_error("Original terminal shutdown unexpectedly returned");
}
}
int main(int argc,char** argv) {
    std::set_terminate([] {
        if(auto pending=std::current_exception())try{std::rethrow_exception(pending);}catch(const std::exception& error){
            std::fprintf(stderr,"Retained shutdown owner: %s\n",error.what());
        }
        std::fflush(nullptr);std::_Exit(1);
    });
    try {Run(argc,argv);}catch(const std::exception& error){std::fprintf(stderr,"Whole shutdown boundary: %s (%u checks)\n",error.what(),checks);std::fflush(nullptr);std::_Exit(1);}
}

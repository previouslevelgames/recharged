#include "platform/ax_bootstrap_device.h"
#include "platform/dsp_control_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
extern "C" {
#include <revolution/dsp.h>
}
#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace {
using namespace mscharged::platform;
unsigned checks{},callbacks{};
std::thread::id owner;
DSPTask* expected_task{};
DSPTask** actual_current{};
NativeAXBootstrapDevice* observing_device{};
void Check(bool value,const char* text) {++checks;if(!value)throw std::runtime_error(text);}
template<class F>void Throws(F f,const char* text) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed,text);}
template<class F>F Load(SDL_SharedObject* image,const char* name) {
 auto* result=SDL_LoadFunction(image,name);if(!result)throw std::runtime_error(std::string(name)+": "+SDL_GetError());return reinterpret_cast<F>(result);
}
struct Lease {
 std::string path;std::vector<SDL_SharedObject*> images;
 static BOOL Retain(void* context) noexcept {auto& s=*static_cast<Lease*>(context);auto* h=SDL_LoadObject(s.path.c_str());if(!h)return FALSE;try{s.images.push_back(h);}catch(...){SDL_UnloadObject(h);return FALSE;}return TRUE;}
 static void Release(void* context) noexcept {auto& s=*static_cast<Lease*>(context);auto* h=s.images.back();s.images.pop_back();SDL_UnloadObject(h);}
};
void Observe(DSPTask* task) {
 Check(std::this_thread::get_id()==owner,"real source callback ran on foreign worker");
 Check(task==expected_task&&*actual_current==task,"source init callback did not own actual source task");
 Check(task->state==DSP_TASK_STATE_1&&task->flags==DSP_TASK_ACTIVE,"actual original handler did not make the retail state transition");
 Check(!NativeInterruptsEnabled()&&OSGetCurrentContext(),"actual source handler lacked masked owner/context");
 observing_device->ServiceOwner(); // Genuine nested safe point must not redispatch.
 Check(GetNativeInterruptControllerStatus().dispatch_depth==1,"nested device poll reentered source handler");
 ++callbacks;
}
void Run(int argc,char** argv) {
 const bool masked=argc==4&&std::strcmp(argv[3],"--masked")==0;
 Check((argc==3||masked)&&std::strlen(argv[2])==64,"need actual full source image and SHA256 identity");owner=std::this_thread::get_id();
 const auto directory=std::filesystem::absolute("sdk-data").string();std::filesystem::create_directories(directory);
 AuroraConfig config{};config.appName="Original AX device init prefix";config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
 config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;
 config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
 Check(aurora_initialize(argc,argv,&config).window!=nullptr,"sole actual SDK window missing");OSInit();
 Lease lease{std::filesystem::absolute(argv[1]).string(),{}};auto* image=SDL_LoadObject(lease.path.c_str());
 Check(image!=nullptr,"whole original DSP source module failed to load");
 auto* firmware=Load<unsigned char*>(image,"axDspSlave");auto* firmware_bytes=Load<std::uint16_t*>(image,"axDspSlaveLength");
 Check(*firmware_bytes==8192,"actual source firmware extent changed");
 OSNativeStaticMemoryOwner owner_record{argv[2],"axDspSlave",firmware,8192,FALSE,&lease,Lease::Retain,Lease::Release};
 const auto mapping=OSNativeRegisterStaticMemory(&owner_record);const auto memory=AttachNativeDSPMEM1();
 const auto pin=PinNativeDSPMemory(firmware,8192,false);InitializeNativeInterruptController();
 const auto mail=AttachNativeDSPMailboxes();const auto control=AttachNativeDSPControl(mail);
 Check(ChargedDSPControlRead()==4,"actual diagnostic control cold HALT differs");
 Throws([&]{ChargedDSPControlWrite(0x0804);},"default DSP endpoint invented bootstrap without a processor");
 auto send=Load<decltype(&DSPSendMailToDSP)>(image,"DSPSendMailToDSP");
 auto full=Load<decltype(&DSPCheckMailToDSP)>(image,"DSPCheckMailToDSP");
 auto incoming=Load<decltype(&DSPCheckMailFromDSP)>(image,"DSPCheckMailFromDSP");
 auto read=Load<decltype(&DSPReadMailFromDSP)>(image,"DSPReadMailFromDSP");
 auto source_init=Load<decltype(&DSPInit)>(image,"DSPInit");auto is_init=Load<decltype(&DSPCheckInit)>(image,"DSPCheckInit");
 auto add=Load<decltype(&DSPAddTask)>(image,"DSPAddTask");
 actual_current=Load<DSPTask**>(image,"__DSP_curr_task");
 auto* handler=Load<__OSInterruptHandler>(image,"__DSPHandler");
 Check(!is_init()&&!*actual_current,"cold actual source flags/task are not zero");
 auto* scratch=static_cast<unsigned char*>(OSAllocFromArenaLo(8192,32));
 std::memcpy(scratch,firmware,8192);scratch[0]^=1;
 const auto scratch_pin=PinNativeDSPMemory(scratch,8192,false);
 Throws([&]{NativeAXBootstrapDevice invalid(memory,mail,control,OSCachedToPhysical(scratch));},"wrong full-image SHA produced a device");
 ReleaseNativeDSPMemory(scratch_pin);
 NativeAXBootstrapDevice device(memory,mail,control,mapping.physical_address);observing_device=&device;
 Throws([&]{OSNativeReleaseStaticMemory(mapping);},"live device pin did not retain genuine source image owner");
 const auto nativeSend=[&](std::uint32_t word){send(reinterpret_cast<DSPMail>(std::uintptr_t(word)));};
 const auto reset=[&]{ChargedDSPControlWrite(0x0805);Check(!incoming()&&!full(),"real reset retained mailbox cells");};
 const auto loader=[&]{ChargedDSPControlWrite(0x0800);Check(incoming(),"operational native loader did not publish actual ready cell");Check(reinterpret_cast<std::uintptr_t>(read())==0x8071feed,"source high/acklow loader word changed");};
 loader();nativeSend(0x80f3a001);Throws([&]{nativeSend(mapping.physical_address+32);},"unauthorized actual bus image accepted");
 Check(device.Status().phase==NativeAXBootstrapPhase::Faulted&&!is_init()&&!callbacks,"invalid loader created original readiness");reset();
 loader();Throws([&]{nativeSend(0x80f3c002);},"out-of-order source loader command accepted");reset();
 loader();nativeSend(0x80f3a001);nativeSend(mapping.physical_address);nativeSend(0x80f3c002);
 Throws([&]{nativeSend(1);},"wrong DSP word destination accepted");reset();
 loader();nativeSend(0x80f3a001);nativeSend(mapping.physical_address);nativeSend(0x80f3c002);nativeSend(0);nativeSend(0x80f3a002);
 Throws([&]{nativeSend(8190);},"truncated source firmware extent accepted");reset();
 loader();ChargedDSPControlWrite(0x0804);nativeSend(0x80f3a001);
 Check(full()&&device.Status().loader_words==0,"HALT consumed actual pending source word");reset();
 // Force a fresh INIT transition as original DSPInit requests, with source
 // flags untouched. All previous tests were direct hardware conformance only.
 ChargedDSPControlWrite(4);
 Check(!is_init()&&!*actual_current&&!callbacks,"hardware tests manually altered source init/task flags");
 source_init();
 Check(is_init()&&!*actual_current&&callbacks==0,"actual whole DSPInit did not own its initialization flags");
 Check(__OSGetInterruptHandler(__OS_INTERRUPT_DSP_DSP)==handler,"source DSPInit handler registration was bypassed");
 DSPTask task{};expected_task=&task;task.iramMmemAddr=firmware;task.iramMmemLen=8192;task.iramDspAddr=0;
 task.startVector=0x10;task.resumeVector=0x37;task.prio=0;task.initCallback=Observe;
 const BOOL before=masked?OSDisableInterrupts():TRUE;
 Check(add(&task)==&task,"actual DSPAddTask changed source task pointer");
 if(masked) {
  Check(callbacks==0&&task.state==DSP_TASK_STATE_0&&(ChargedDSPControlRead()&0x80),"source critical section delivered actual INIT IRQ");
  device.ServiceOwner();Check(callbacks==0,"masked owner hardware poll dispatched source callback");
  OSRestoreInterrupts(before);device.ServiceOwner();
 }
 const auto done=device.Status();
 Check(done.phase==NativeAXBootstrapPhase::InitPrefixCompleted&&done.loader_words==10&&done.firmware_instructions==21,
       "real loader or source21instructions did not complete");
 Check(callbacks==1&&task.state==DSP_TASK_STATE_1&&task.flags==DSP_TASK_ACTIVE&&*actual_current==&task,
       "real source handler/init callback did not receive actual instruction-generated IRQ");
 Check(!incoming()&&!full()&&!(ChargedDSPControlRead()&0x80),"source W1C/mail acknowledgment left pending hardware cause");
 Check(GetNativeInterruptControllerStatus().dispatched==1,"source INIT interrupt delivered wrong count");
 Check(!ServiceNativeInterruptController()&&callbacks==1,"completed source INIT callback was repeated");
 Throws([&]{DetachNativeDSPControl();},"live processor control lifetime was dropped");
 Throws([&]{DetachNativeDSPMailboxes();},"live processor mailbox lifetime was dropped");
 Throws([&]{device.Close();},"unhalted processor retired actual device context");
 bool wrong_owner_rejected=false;std::thread other([&]{try{device.ServiceOwner();}catch(const std::exception&){wrong_owner_rejected=true;}});other.join();
 Check(wrong_owner_rejected&&callbacks==1,"foreign CPU owner serviced source callback");
 Throws([&]{nativeSend(0xbabe0080);},"incomplete frame/active kernel returned success");
 Check(device.Status().phase==NativeAXBootstrapPhase::Faulted&&callbacks==1&&task.state==DSP_TASK_STATE_1,
       "unsupported command invented completion or repaired original task flags");
 ChargedDSPControlWrite(0x0804);device.Close();
 Throws([&]{device.ServiceOwner();},"retired processor serviced actual hardware");
 // Source task remains source-owned. Hardware/image retirement is scoped;
 // no DONE/state3 or game/task shutdown success is invented by this fixture.
 DetachNativeDSPControl();DetachNativeDSPMailboxes();ShutdownNativeInterruptController();
 ReleaseNativeDSPMemory(pin);OSNativeReleaseStaticMemory(mapping);Check(lease.images.empty(),"true firmware lease survived pin drain");
 SDL_UnloadObject(image);DetachNativeDSPMEM1();aurora_shutdown();
 std::cout<<"native_ax_bootstrap: "<<checks<<" checks ("<<(masked?"masked":"unmasked")<<"); wholeDSPInit/AddTask/Handler,10 source loader words,21 actual source instructions,1 real owner INIT callback; AXInit/frame/SRC held\n";
}
} // namespace
int main(int argc,char** argv){try{Run(argc,argv);return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

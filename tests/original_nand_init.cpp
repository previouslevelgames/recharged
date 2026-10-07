#include "platform/filesystem_device.h"
#include "platform/ios_device.h"
#include "platform/ipc_boot_buffer.h"
#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include "platform/thread_registry_abi.h"
#include <revolution/nand.h>
#include <revolution/fs.h>
#include <revolution/os/OSIOSRev.h>
#include <revolution/os/OSIpc.h>
#include <revolution/os/OSReset.h>
#include <dolphin/os.h>
#include <aurora/hardware.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <unistd.h>

void AuroraOSShutdown();
namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
unsigned checks{}, completions{}, clock_completions{};
void Check(bool okay,const char* error) {++checks;if(!okay)throw std::runtime_error(error);}
template<class F> void Reject(F function,const char* error) {
 bool caught{};try{function();}catch(const std::logic_error&){caught=true;}Check(caught,error);
}
template<class F> void Until(F done) {
 const auto end=std::chrono::steady_clock::now()+2s;
 while(!done()){if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("Actual source/native lifetime wait timed out");std::this_thread::yield();}
}
void HardwareService() {if(ServiceNativeIOSRequests())++clock_completions;}
BOOL FixtureHook(BOOL,u32){throw std::logic_error("Fixture priority-node callbacks are not shutdown providers");}
// Borrowed source registrations stay alive for the entire terminal fixture.
OSShutdownFunctionInfo low{FixtureHook,100},equal_first{FixtureHook,255},
 equal_second{FixtureHook,255},high{FixtureHook,500};
unsigned ActiveCount() {
 const auto mask=OSDisableInterrupts();auto* queue=ChargedNativeActiveThreadQueue();
 OSThread* previous{};unsigned count{};
 for(auto* thread=queue->head;thread;thread=thread->linkActive.next){
  Check(thread->linkActive.prev==previous,"Same native335 active intrusive prev link changed");previous=thread;
  if(++count>16)throw std::runtime_error("Native active queue cycle");
 }
 Check(previous==queue->tail,"Same native active queue tail changed");OSRestoreInterrupts(mask);return count;
}
void* NotEntered(void*){throw std::logic_error("Parked source SDK thread ran without Resume");}
struct NandReceipt {
 std::thread::id owner;OSContext* prior;unsigned calls{};s32 result{-999};
 static void Receive(s32 result,NANDCommandBlock* block) {
  auto& value=*static_cast<NandReceipt*>(block->userData);
  Check(NativeInterruptDispatchActive()&&!NativeInterruptsEnabled()&&std::this_thread::get_id()==value.owner,
   "Genuine original NAND completion escaped owner IRQ context");
  Check(OSGetCurrentContext()!=value.prior,"Original NAND callback did not enter native IRQ context");
  value.result=result;++value.calls;++completions;
 }
};
void Run(const std::filesystem::path& root) {
 static_assert(sizeof(OSIOSRev)==8&&offsetof(OSIOSRev,buildYear)==6);
 OSInit(); // Genuine immutable SDK45 memory/boot/native clock foundation.
 auto* caller=OSGetCurrentThread();auto* context=OSGetCurrentContext();
 Check(caller&&std::uintptr_t(caller)>UINT32_MAX,"Actual SDK caller lacks native64 backing");
 Reject([]{(void)ChargedNativeActiveThreadQueue();},"Unmasked caller borrowed the active registry");
 Check(ActiveCount()==1,"Original default caller missing from the same335 active queue");
 OSThread descriptor{};alignas(8) std::array<unsigned char,0x4000> stack;
 Check(OSCreateThread(&descriptor,NotEntered,nullptr,stack.data()+stack.size(),stack.size(),12,0),"Actual parked source SDK creation failed");
 Check(ActiveCount()==2,"Created source SDK descriptor went to a second registry");
 const auto mask=OSDisableInterrupts();auto* list=ChargedNativeActiveThreadQueue();
 Check(list->head==caller&&list->tail==&descriptor&&caller->linkActive.next==&descriptor&&descriptor.linkActive.prev==caller,
  "Original active list order/native64 descriptor links changed");OSRestoreInterrupts(mask);
 OSCancelThread(&descriptor);Check(OSJoinThread(&descriptor,nullptr),"Actual source parked cancel/join failed");
 Check(ActiveCount()==1,"Source join did not remove original active descriptor");DrainNativeThreadLifetimes();
 std::atomic<bool> entered{},release{};std::thread foreign([&]{(void)OSGetCurrentThread();entered=true;while(!release)std::this_thread::yield();});
 Until([&]{return entered.load();});Check(ActiveCount()==2,"Actual foreign SDK caller missing from shared registry");release=true;foreign.join();
 Check(ActiveCount()==1,"Returning actual foreign caller retained its borrowed active object");
 alignas(32) static std::array<unsigned char,32768> boot;
 InstallNativeIPCBootBuffer(boot.data(),boot.size());__OSInitIPCBuffer();IPCInit();
 char home[64];std::memset(home,0x5a,sizeof(home));
 Check(!nandIsInitialized()&&NANDGetHomeDir(home)==NAND_RESULT_FATAL_ERROR&&home[0]==char(0x5a),
  "Original cold NAND invented initialized home/output");
 Check(NANDInit()==NAND_RESULT_NOEXISTS&&!nandIsInitialized(),"Absent FS/ES hid original initialization failure");
 // Original registration algorithm keeps priority order and equal-priority FIFO.
 OSRegisterShutdownFunction(&high);OSRegisterShutdownFunction(&equal_first);OSRegisterShutdownFunction(&low);OSRegisterShutdownFunction(&equal_second);
 Check(!low.prev&&low.next==&equal_first&&equal_first.prev==&low&&equal_first.next==&equal_second&&
  equal_second.prev==&equal_first&&equal_second.next==&high&&high.prev==&equal_second&&!high.next,
  "Whole original shutdown registration changed priority/equal-priority FIFO");
 constexpr u64 title=0x0001000052345145ULL;
 InitializeNativeFilesystem({root,title,0x1000,0x41});
 Check(aurora_register_hardware_service(HardwareService),"Actual clock hardware owner service could not register");
 Check(NANDInit()==NAND_RESULT_OK&&nandIsInitialized(),"Whole original NANDInit failed real FS/ES/title/home path");
 Check(NANDGetHomeDir(home)==NAND_RESULT_OK&&std::strcmp(home,"/title/00010000/52345145/data")==0,
  "Original NAND home did not come from actual configured ES identity");
 char current[64];Check(NANDGetCurrentDir(current)==NAND_RESULT_OK&&std::strcmp(current,home)==0,
  "Original NAND did not set current directory at its actual successful source position");
 auto* nand_hook=equal_second.next;
 Check(nand_hook&&nand_hook!=&high&&nand_hook->priority==255&&nand_hook->prev==&equal_second&&nand_hook->next==&high&&high.prev==nand_hook,
  "Original NAND registration missing/duplicated or source priority255 reordered");
 const auto ipc_lo=IPCGetBufferLo();const auto before=GetNativeIOSStatus();
 Check(NANDInit()==NAND_RESULT_OK&&GetNativeIOSStatus().descriptors==before.descriptors&&IPCGetBufferLo()==ipc_lo&&equal_second.next==nand_hook&&nand_hook->next==&high,
  "Original already-initialized branch repeated FS/ES/heap/hook ownership");
 Check(nand_hook->func(TRUE,OS_SHUTDOWN_SHUTDOWN)&&nand_hook->func(FALSE,OS_SHUTDOWN_RESTART)&&GetNativeIOSStatus().pending==0,
  "Original final/non-shutdown NAND callback issued extra host requests");
 Check(NANDCreateDir("profile",NAND_PERM_RUSR|NAND_PERM_WUSR,6)==NAND_RESULT_OK,"Original relative NAND directory creation failed");
 NandReceipt create{std::this_thread::get_id(),context};NANDCommandBlock create_block{};create_block.userData=&create;
 Check(std::uintptr_t(&create_block)>UINT32_MAX&&NANDCreateAsync("profile/raw",NAND_PERM_RUSR|NAND_PERM_WUSR,4,NandReceipt::Receive,&create_block)==NAND_RESULT_OK,
  "Whole NAND async create lost actual native context/source request");
 Check(create.calls==0&&GetNativeIOSStatus().pending==1,"Original create callback ran before actual owner completion");
 const auto disabled=OSDisableInterrupts();Check(!ServiceNativeIOSRequests()&&!create.calls,"Masked NAND callback consumed hardware work");OSRestoreInterrupts(disabled);
 Check(ServiceNativeIOSRequests()&&create.calls==1&&create.result==NAND_RESULT_OK,"Original NAND callback did not convert genuine FS completion");
 NANDStatus status{};Check(NANDGetStatus("profile/raw",&status)==NAND_RESULT_OK&&status.ownerId==0x1000&&status.groupId==0x41&&status.attr==4&&status.perm==0x30,
  "Original NAND status/permission compose changed native scalar cells");
 NANDFileInfo file{};Check(NANDOpen("profile/raw",&file,NAND_ACCESS_RW)==NAND_RESULT_OK&&file.mark==1,"Whole original NAND open did not own a real descriptor");
 alignas(32) std::array<unsigned char,64> bytes;for(unsigned n=0;n<bytes.size();++n)bytes[n]=(n*29+7)&255;
 Check(NANDWrite(&file,bytes.data(),bytes.size())==bytes.size(),"Original NANDWrite lost actual payload/count");
 u32 length{};Check(NANDGetLength(&file,&length)==NAND_RESULT_OK&&length==bytes.size(),"Original NAND source length callback cells changed");
 Check(NANDSeek(&file,0,NAND_SEEK_BEG)==0,"Original NAND seek failed");
 alignas(32) std::array<unsigned char,64> output{};NandReceipt read{std::this_thread::get_id(),context};NANDCommandBlock read_block{};read_block.userData=&read;
 Check(NANDReadAsync(&file,output.data(),output.size(),NandReceipt::Receive,&read_block)==NAND_RESULT_OK,"Whole original NAND async read submission failed");
 Check(ServiceNativeIOSRequests()&&read.calls==1&&read.result==output.size()&&output==bytes,
  "Original NAND read callback/full-width block did not preserve actual data");
 Check(NANDClose(&file)==NAND_RESULT_OK&&file.mark==2,"Original close did not retire source descriptor mark");
 const auto source_start=OSGetTime();Check(nand_hook->func(FALSE,OS_SHUTDOWN_SHUTDOWN),"Original NAND hardware shutdown hook failed");
 Check(clock_completions==1&&GetNativeIOSStatus().pending==0&&!GetNativeIOSStatus().active&&OSGetTime()>=source_start,
  "Actual original shutdown callback was not completed by real clock/IOS owner servicing");
 Check(OSGetCurrentContext()==context&&NativeInterruptsEnabled(),"NAND source lifecycle leaked IRQ context/mask");
 Check(aurora_unregister_hardware_service(HardwareService),"Actual clock hardware service failed to retire");
 ShutdownNativeFilesystem();
 Check(nandIsInitialized()&&NANDInit()==NAND_RESULT_OK,"Adapter changed original terminal NAND initialized-state quirk");
 NANDFileInfo absent{};Check(NANDOpen("profile/raw",&absent,NAND_ACCESS_READ)==NAND_RESULT_NOEXISTS,
  "Native device retirement invented successful source file readiness");
 DrainNativeThreadLifetimes();AuroraOSShutdown();
 std::printf("Original NANDInit/OSReset registration: %u checks, %u original NAND user completions, %u original shutdown completion through real SDK45 clock; same335 registry. SRAM/reset/full saves/banner/Mii remain HOLD.\n",checks,completions,clock_completions);
}
}
int main(int argc,char** argv) {
 try{if(argc!=2)throw std::invalid_argument("Expected one private fixture base directory");
  const auto unique=std::string("owned-")+std::to_string(::getpid())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Run(std::filesystem::path(argv[1])/unique);return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"NANDInit gate: %s (%u checks)\n",e.what(),checks);return 1;}
}

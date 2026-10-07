#include "platform/filesystem_boot.h"
#include "platform/ios_device.h"
#include "platform/ipc_boot_buffer.h"
#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include "platform/path.h"
#include <revolution/nand.h>
#include <revolution/os/OSIpc.h>
#include <dolphin/os.h>
#include <nod.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

void AuroraOSShutdown();
namespace {
using namespace mscharged;
using namespace mscharged::platform;
unsigned checks{}, completions{};
void Check(bool value,const char* error) {++checks;if(!value)throw std::runtime_error(error);}
struct Receipt {
    std::thread::id owner; OSContext* previous; unsigned calls{}; s32 result{-999};
    static void Receive(s32 result,NANDCommandBlock* block) {
        auto& value=*static_cast<Receipt*>(block->userData);
        Check(std::this_thread::get_id()==value.owner&&NativeInterruptDispatchActive()&&!NativeInterruptsEnabled(),
            "Original NAND completion left its actual native owner IRQ");
        Check(OSGetCurrentContext()!=value.previous,"Original NAND callback did not enter genuine IRQ context");
        value.result=result;++value.calls;++completions;
    }
};
void RawTmd(const std::filesystem::path& disc,const std::filesystem::path& destination) {
    using Handle=std::unique_ptr<NodHandle,decltype(&nod_free)>;
    NodHandle* raw{};
    Check(nod_disc_open(PathUtf8(disc).c_str(),nullptr,&raw)==NOD_RESULT_OK,"Owned raw TMD oracle disc open failed");
    Handle image(raw,nod_free);raw=nullptr;
    Check(nod_disc_open_partition_kind(image.get(),NOD_PARTITION_KIND_DATA,nullptr,&raw)==NOD_RESULT_OK,"Owned raw TMD oracle partition failed");
    Handle partition(raw,nod_free);NodPartitionMeta meta{};
    Check(nod_partition_meta(partition.get(),&meta)==NOD_RESULT_OK&&meta.raw_tmd.data&&meta.raw_tmd.size>=0x1e4,
        "Owned raw TMD oracle has no actual complete borrowed bytes");
    std::ofstream out(destination,std::ios::binary);out.write(reinterpret_cast<const char*>(meta.raw_tmd.data),meta.raw_tmd.size);
    Check(bool(out),"Private actual TMD oracle write failed");
}
void Run(const NativeFilesystemBootSettings& boot_settings,bool reopen,u8 home_permissions) {
    OSInit(); // One immutable current SDK45, no window/GX or alternate OS.
    auto* previous=OSGetCurrentContext();
    alignas(32) static std::array<unsigned char,32768> boot;
    InstallNativeIPCBootBuffer(boot.data(),boot.size());__OSInitIPCBuffer();IPCInit();
    Check(!nandIsInitialized()&&NANDInit()==NAND_RESULT_NOEXISTS&&!nandIsInitialized(),
        "Absent host device invented original NAND readiness");
    const auto settings=InitializeNativeFilesystemForDisc(boot_settings);
    Check(settings.title_id!=0&&settings.uid==boot_settings.uid,"Actual disc/config process identity was lost");
    Check(!nandIsInitialized(),"Native filesystem boot invoked source NANDInit or changed its state");
    Check(std::filesystem::exists(settings.root/"metadata.txt"),"Physical ownership catalog was not persisted");
    Check(NANDInit()==NAND_RESULT_OK&&nandIsInitialized(),"Whole original NANDInit failed actual TMD-backed ES path");
    std::ostringstream expected;
    expected<<"/title/"<<std::hex<<std::setw(8)<<std::setfill('0')<<u32(settings.title_id>>32)<<'/'<<std::setw(8)<<u32(settings.title_id)<<"/data";
    char home[64],current[64];
    Check(NANDGetHomeDir(home)==NAND_RESULT_OK&&home==expected.str(),"Source NAND home was inferred from game ID instead of actual TMD");
    Check(NANDGetCurrentDir(current)==NAND_RESULT_OK&&std::strcmp(current,home)==0,"Original successful initialization did not assign its current directory");
    NANDStatus status{};
    Check(NANDGetStatus(home,&status)==NAND_RESULT_OK&&status.ownerId==settings.uid&&status.groupId==settings.gid&&status.perm==home_permissions,
        "Actual title-home owner/group or new3/0/0 permissions changed");
    const std::string name="transport";
    alignas(32) std::array<unsigned char,64> bytes;for(unsigned n=0;n<bytes.size();++n)bytes[n]=(n*37+19)&255;
    NANDFileInfo file{};
    if(!reopen) {
        Receipt receipt{std::this_thread::get_id(),previous};NANDCommandBlock command{};command.userData=&receipt;
        Check(std::uintptr_t(&command)>UINT32_MAX&&NANDCreateAsync(name.c_str(),NAND_PERM_RUSR|NAND_PERM_WUSR,4,Receipt::Receive,&command)==NAND_RESULT_OK,
            "Original raw fixture file request lost source callback/native context");
        Check(!receipt.calls&&GetNativeIOSStatus().pending==1,"Original source completion ran before owner IRQ service");
        const auto enabled=OSDisableInterrupts();Check(!ServiceNativeIOSRequests()&&!receipt.calls,"Masked owner consumed the real NAND completion");OSRestoreInterrupts(enabled);
        Check(ServiceNativeIOSRequests()&&receipt.calls==1&&receipt.result==NAND_RESULT_OK,"Actual original NAND create completion failed");
        Check(NANDOpen(name.c_str(),&file,NAND_ACCESS_RW)==NAND_RESULT_OK,"Source relative open failed actual title home");
        Check(NANDWrite(&file,bytes.data(),bytes.size())==bytes.size(),"Original NANDWrite changed actual raw payload/count");
        Check(NANDClose(&file)==NAND_RESULT_OK,"Actual original NAND close failed");
    }
    Check(NANDGetStatus(name.c_str(),&status)==NAND_RESULT_OK&&status.ownerId==settings.uid&&status.groupId==settings.gid&&status.perm==0x30&&status.attr==4,
        "Physical file reopen lost original native ownership/modes");
    Check(NANDOpen(name.c_str(),&file,NAND_ACCESS_READ)==NAND_RESULT_OK,"Actual original persistent raw file did not open");
    u32 length{};Check(NANDGetLength(&file,&length)==NAND_RESULT_OK&&length==bytes.size(),"Original persistent source length changed");
    alignas(32) std::array<unsigned char,64> output{};
    Receipt receipt{std::this_thread::get_id(),previous};NANDCommandBlock command{};command.userData=&receipt;
    Check(NANDReadAsync(&file,output.data(),output.size(),Receipt::Receive,&command)==NAND_RESULT_OK&&receipt.calls==0,
        "Original persistent read did not retain its actual callback/block");
    Check(ServiceNativeIOSRequests()&&receipt.calls==1&&receipt.result==bytes.size()&&output==bytes,
        "Original persistent actual bytes/callback failed");
    Check(NANDClose(&file)==NAND_RESULT_OK,"Original read descriptor did not close");
    Check(OSGetCurrentContext()==previous&&NativeInterruptsEnabled(),"Native device servicing leaked original context/mask");
    RawTmd(boot_settings.disc,settings.root.parent_path()/"actual-tmd.bin");
    ShutdownNativeFilesystem();
    Check(nandIsInitialized(),"Native storage teardown changed the original terminal initialized-state quirk");
    DrainNativeThreadLifetimes();AuroraOSShutdown();
    std::printf("{\"checks\":%u,\"completions\":%u,\"title\":\"%016llx\",\"group\":%u,\"uid\":%u,\"reopen\":%s}\n",
        checks,completions,(unsigned long long)settings.title_id,settings.gid,settings.uid,reopen?"true":"false");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=5)throw std::invalid_argument("Expected disc, backing, explicit native IOS uid and fresh/reopen mode");
        const auto uid=std::stoull(argv[3],nullptr,0);if(uid>UINT32_MAX)throw std::invalid_argument("Native IOS UID exceeds its original u32 field");
        const bool reopen=std::strcmp(argv[4],"reopen")==0||std::strcmp(argv[4],"reopen-legacy")==0;
        Run({argv[1],argv[2],std::uint32_t(uid)},reopen,std::strcmp(argv[4],"reopen-legacy")==0?0x3d:0x30);return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"Original storage boot gate: %s (%u checks)\n",e.what(),checks);return 1;}
}

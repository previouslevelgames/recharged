#include "platform/filesystem_device.h"
#include "platform/ios_device.h"
#include "platform/interrupts.h"
#include "platform/ipc_boot_buffer.h"
#include <revolution/fs.h>
#include <revolution/ipc.h>
#include <revolution/os/OSIpc.h>
#include <dolphin/os.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
using namespace mscharged::platform;
unsigned checks{},completions{};
void Check(bool value,const char* error){++checks;if(!value)throw std::runtime_error(error);}
template<class F> void Reject(F function,const char* error){bool failed{};try{function();}catch(const std::exception&){failed=true;}Check(failed,error);}
struct Receipt {
    std::thread::id owner;
    OSContext* prior;
    unsigned calls{};
    s32 result{-999};
    Receipt* follow{};
    const char* attr_path{};
    u32 uid{},attr{},user{},group{},other{};u16 gid{};
    static void Receive(s32 value,void* argument) {
        auto& receipt=*static_cast<Receipt*>(argument);
        Check(std::this_thread::get_id()==receipt.owner&&NativeInterruptDispatchActive()&&!NativeInterruptsEnabled(),
            "Original FS callback escaped real owner interrupt scope");
        Check(OSGetCurrentContext()!=receipt.prior,"Original FS callback reused native thread context");
        Check(!ServiceNativeIOSRequests(),"Original callback reentered IOS completion delivery");
        ++receipt.calls;++completions;receipt.result=value;
        if(receipt.follow) {
            auto& next=*receipt.follow;
            Check(ISFS_GetAttrAsync(receipt.attr_path,&next.uid,&next.gid,&next.attr,&next.user,&next.group,&next.other,
                Receive,&next)==0,"Original reentrant FS request rejected");
        }
    }
};
void Pump(){Check(ServiceNativeIOSRequests(),"Actual queued FS completion missing");Check(NativeInterruptsEnabled(),"FS source completion leaked mask");}
void Run(const std::filesystem::path& root) {
    alignas(32) static std::array<unsigned char,32768> boot;
    Check(std::uintptr_t(boot.data())>UINT32_MAX,"Original IPC owner lacks actual native64 address");
    InstallNativeIPCBootBuffer(boot.data(),boot.size());__OSInitIPCBuffer();IPCInit();
    Check(ISFS_OpenLib()==IPC_RESULT_NOEXISTS,"Absent filesystem invented library readiness");
    constexpr u64 title=0x0001000052345145ULL;
    constexpr u32 uid=0x1000;constexpr u16 gid=0x41;
    const NativeFilesystemSettings settings{root,title,uid,gid};
    InitializeNativeFilesystem(settings);
    const auto owner=std::this_thread::get_id();auto* prior=OSGetCurrentContext();
    auto es=IOS_Open("/dev/es",IPC_OPEN_NONE);Check(es>=0,"Actual ES endpoint did not open");
    alignas(32) u64 output_title{};alignas(32) std::array<char,32> home;
    home.fill(char(0x5a));IPCIOVector title_vector{&output_title,8};
    Check(IOS_Ioctlv(es,32,0,1,&title_vector)==0&&output_title==title,"ES title did not reflect explicit boot identity");
    alignas(32) IPCIOVector vectors[2]={{&output_title,8},{home.data(),30}};
    Check(IOS_Ioctlv(es,29,1,1,vectors)==0&&std::strcmp(home.data(),"/title/00010000/52345145/data")==0&&home[30]==char(0x5a),
        "ES data directory lost exact title or output extent");
    output_title^=1;home.fill(char(0x5a));
    Check(IOS_Ioctlv(es,29,1,1,vectors)==IPC_RESULT_NOEXISTS&&home[0]==char(0x5a),"ES unknown title gained existing directory/output writes");
    Check(IOS_Close(es)==0,"ES real descriptor did not close");
    Check(ISFS_OpenLib()==0,"Whole source ISFS_OpenLib failed genuine physical initialization");
    Check(IPCGetBufferLo()==boot.data()+0x1540,"Original FS heap reservation/cursor changed");
    const std::string base="/title/00010000/52345145/data";
    const auto dir=base+"/profile",file=dir+"/raw.bin",renamed=dir+"/save.bin",readonly=dir+"/readonly";
    Check(ISFS_CreateDir(dir.c_str(),7,3,1,0)==0,"Original create-dir request failed");
    Check(ISFS_CreateFile(file.c_str(),4,3,1,0)==0,"Original create-file request failed");
    Check(ISFS_CreateFile(file.c_str(),4,3,1,0)==IPC_RESULT_EXISTS,"Existing file silently recreated");
    Check(ISFS_CreateFile((dir+"/../escape").c_str(),0,3,3,3)==IPC_RESULT_INVALID,"IOS path escaped actual native namespace");
    u32 actual_uid{},attr{},user{},group{},other{};u16 actual_gid{};
    Check(ISFS_GetAttr(file.c_str(),&actual_uid,&actual_gid,&attr,&user,&group,&other)==0&&
        actual_uid==uid&&actual_gid==gid&&attr==4&&user==3&&group==1&&other==0,
        "Original native scalar attr outputs lost persistent permissions/owner");
    Check(ISFS_CreateFile(readonly.c_str(),0,1,0,0)==0,"Readonly file physical creation failed");
    Check(ISFS_Open(readonly.c_str(),IPC_OPEN_WRITE)==IPC_RESULT_ACCESS,"Real readonly permissions were ignored");
    auto fd=ISFS_Open(file.c_str(),IPC_OPEN_RW);Check(fd>=0,"Original real file open failed");
    alignas(32) std::array<unsigned char,0x9000> input;
    for(std::size_t n=0;n<input.size();++n)input[n]=static_cast<unsigned char>((n*37+19)&255);
    Check(ISFS_Write(fd,input.data(),input.size())==input.size(),"Original ISFS_Write lost actual raw bytes/count");
    FSFileStats stats{};
    Check(ISFS_GetFileStats(fd,&stats)==0&&stats.length==input.size()&&stats.position==input.size(),"Source file statistics/position changed");
    Check(ISFS_Seek(fd,5,IPC_SEEK_END)==IPC_RESULT_INVALID,"Unqualified past-EOF position gained successful readiness");
    Check(ISFS_Seek(fd,-5,IPC_SEEK_END)==input.size()-5,"Original end-relative seek failed");
    alignas(32) std::array<unsigned char,64> output;output.fill(0xa5);
    Check(ISFS_Read(fd,output.data(),32)==5&&std::memcmp(output.data(),input.data()+input.size()-5,5)==0&&output[5]==0xa5,
        "Original EOF short count overwrote output tail or bytes");
    Check(ISFS_Read(fd,output.data(),32)==0,"Original true EOF gained bytes");
    Check(ISFS_Read(fd,output.data()+1,1)==IPC_RESULT_INVALID,"Original source alignment validation was bypassed");
    Check(ISFS_Seek(fd,0,IPC_SEEK_BEG)==0,"Original reset seek failed");
    Receipt receipt{owner,prior},follow{owner,prior};receipt.follow=&follow;receipt.attr_path=file.c_str();
    std::thread submitter([&]{Check(ISFS_ReadAsync(fd,output.data(),32,Receipt::Receive,&receipt)==0,"Real submitting worker FS request rejected");});submitter.join();
    Check(!receipt.calls&&GetNativeIOSStatus().pending==1,"FS source callback ran on submitter or work disappeared");
    Reject([]{ShutdownNativeFilesystem();},"Filesystem retired a queued original command block/callback");
    const auto mask=OSDisableInterrupts();Check(!ServiceNativeIOSRequests()&&!receipt.calls,"Masked original FS completion consumed work");OSRestoreInterrupts(mask);
    Pump();Check(receipt.calls==1&&receipt.result==32&&std::memcmp(output.data(),input.data(),32)==0&&follow.calls==0,
        "Actual original FS callback lost byte data/order or invented next completion");
    Pump();Check(follow.calls==1&&follow.result==0&&follow.uid==uid&&follow.attr==4,
        "Original reentrant GetAttr callback/cell output failed");
    Check(OSGetCurrentContext()==prior,"Real FS callback did not restore owner context");
    s32 blocks=-1,files=-1;
    Check(ISFS_GetUsage(dir.c_str(),&blocks,&files)==0&&blocks==3&&files==3,"Real recursive cluster/inode usage did not reflect disk backing");
    Receipt usage{owner,prior};blocks=files=-1;
    Check(ISFS_GetUsageAsync(dir.c_str(),&blocks,&files,Receipt::Receive,&usage)==0,"Original async usage submission failed");
    Pump();Check(usage.calls==1&&usage.result==0&&blocks==3&&files==3,"Original native callback context interpreted wrong usage offsets");
    u32 count=99;
    Check(ISFS_ReadDir(dir.c_str(),nullptr,&count)==0&&count==2,"Actual original count-only directory route failed");
    alignas(32) std::array<char,64> names;names.fill(char(0x5a));count=2;
    Check(ISFS_ReadDir(dir.c_str(),names.data(),&count)==0&&count==2&&std::strcmp(names.data(),"raw.bin")==0&&
        std::strcmp(names.data()+8,"readonly")==0&&names[17]==char(0x5a),"Original directory output packed names/count/guards failed");
    Receipt listing{owner,prior};count=1;names.fill(char(0x5a));
    Check(ISFS_ReadDirAsync(dir.c_str(),names.data(),&count,Receipt::Receive,&listing)==0,"Original directory async request failed");
    Pump();Check(listing.calls==1&&listing.result==0&&count==1&&std::strcmp(names.data(),"raw.bin")==0,
        "Original native ReadDir completion/capacity failed");
    Check(ISFS_Delete(dir.c_str())==IPC_RESULT_NOTEMPTY,"Nonempty source directory deletion succeeded");
    Check(ISFS_Delete(file.c_str())==IPC_RESULT_OPENFD,"Filesystem deleted an actually open source file");
    Check(ISFS_Close(fd)==0,"Source close did not flush/close real data backing");
    Check(ISFS_Rename(file.c_str(),renamed.c_str())==0,"Original safe-save rename request failed");
    Check(ISFS_GetAttr(file.c_str(),&actual_uid,&actual_gid,&attr,&user,&group,&other)==IPC_RESULT_NOEXISTS,
        "Original old pathname survived actual rename");
    Receipt missing{owner,prior};
    Check(ISFS_OpenAsync(file.c_str(),IPC_OPEN_READ,Receipt::Receive,&missing)==0,"Original missing-open submission contract changed");
    Pump();Check(missing.calls==1&&missing.result==IPC_RESULT_NOEXISTS,"Original missing file callback fabricated successful source open");
    Check(ISFS_Delete(readonly.c_str())==0,"Actual source deletion failed");
    Receipt shutdown{owner,prior};Check(ISFS_ShutdownAsync(Receipt::Receive,&shutdown)==0,"Original filesystem flush request rejected");
    Pump();Check(shutdown.calls==1&&shutdown.result==0,"Original shutdown request lacked real IO completion");
    ShutdownNativeFilesystem();Check(GetNativeIOSStatus().devices==0&&GetNativeIOSStatus().pending==0&&GetNativeIOSStatus().descriptors==0,
        "Real filesystem routing/descriptor lifecycle did not retire");
    Check(ISFS_Open(renamed.c_str(),IPC_OPEN_READ)==IPC_RESULT_NOEXISTS,
        "Retired filesystem accepted a fresh original open request");
    Check(IOS_Read(fd,output.data(),32)==IPC_RESULT_INVALID,"Retired original descriptor reached device backing");
    Reject([&]{InitializeNativeFilesystem({root,title^1,uid,gid});},"Mismatched title identity reused persistent permissions/backing");
    InitializeNativeFilesystem(settings);Check(ISFS_OpenLib()==0,"Whole original library did not open genuine persisted device again");
    Check(ISFS_GetAttr(renamed.c_str(),&actual_uid,&actual_gid,&attr,&user,&group,&other)==0&&actual_uid==uid&&actual_gid==gid&&attr==4&&user==3,
        "Actual stored permission/identity fields did not survive device reload");
    fd=ISFS_Open(renamed.c_str(),IPC_OPEN_READ);Check(fd>=0,"Persisted source file failed to reopen");
    alignas(32) std::array<unsigned char,0x9000> persisted;
    Check(ISFS_Read(fd,persisted.data(),persisted.size())==persisted.size()&&persisted==input,"Reload changed original raw file data");
    Check(ISFS_Close(fd)==0,"Persisted source file failed to close");ShutdownNativeFilesystem();
    Check(NativeInterruptsEnabled()&&OSGetCurrentContext()==prior,"Original FS lifecycle leaked native owner context/mask");
    std::cout<<"Original ISFS/native FS+ES: "<<checks<<" checks, "<<completions<<" genuine original completions; whole fs.c+originalIPC heap/boot, actual persistent bytes/permissions. NANDInit/shutdown registration, game saves/Wii32/banner/Mii readiness remain HOLD.\n";
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Expected one private fixture backing directory");
        const auto unique=std::string("owned-")+std::to_string(::getpid())+"-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        Run(std::filesystem::path(argv[1])/unique);return 0;
    }catch(const std::exception& error){std::cerr<<"Filesystem gate failure: "<<error.what()<<" ("<<checks<<" checks)\n";return 1;}
}

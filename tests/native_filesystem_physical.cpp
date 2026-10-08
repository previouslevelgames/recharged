#include "platform/filesystem_device.h"
#include "platform/ios_device.h"
#include "platform/interrupts.h"
#include <revolution/ipc.h>
#include <dolphin/os.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace {
namespace fs=std::filesystem;
using namespace mscharged::platform;
unsigned checks{},callbacks{};
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F f,const char* message){bool failed{};try{f();}catch(const std::exception&){failed=true;}Check(failed,message);}
struct Fields {u32 uid;u16 gid;char path[64];u8 owner,group,other,attr,unused[2];};
static_assert(sizeof(Fields)==76&&offsetof(Fields,path)==6&&offsetof(Fields,owner)==70);
std::array<char,64> Name(const std::string& name){std::array<char,64> bytes{};Check(name.size()<bytes.size(),"Fixture name exceeds IOS buffer");std::memcpy(bytes.data(),name.data(),name.size());return bytes;}
s32 Create(s32 fd,const std::string& path,bool directory=false,u8 attr=9,u8 mode=3){alignas(32) Fields fields{};std::memcpy(fields.path,path.c_str(),path.size()+1);fields.owner=mode;fields.attr=attr;return IOS_Ioctl(fd,directory?3:9,&fields,sizeof(fields),nullptr,0);}
s32 Delete(s32 fd,const std::string& path){auto bytes=Name(path);return IOS_Ioctl(fd,7,bytes.data(),bytes.size(),nullptr,0);}
s32 Rename(s32 fd,const std::string& a,const std::string& b){std::array<char,128> bytes{};std::memcpy(bytes.data(),a.c_str(),a.size()+1);std::memcpy(bytes.data()+64,b.c_str(),b.size()+1);return IOS_Ioctl(fd,8,bytes.data(),bytes.size(),nullptr,0);}
Fields Attr(s32 fd,const std::string& path){auto name=Name(path);Fields output{};Check(IOS_Ioctl(fd,6,name.data(),name.size(),&output,sizeof(output))==0,"Actual attr failed");return output;}
std::vector<std::string> Listing(s32 fd,const std::string& path){auto name=Name(path);u32 capacity=24,count{};std::array<char,24*13> names{};IPCIOVector vectors[4]={{name.data(),64},{&capacity,4},{names.data(),names.size()},{&count,4}};Check(IOS_Ioctlv(fd,4,2,2,vectors)==0,"Actual directory enumeration failed");std::vector<std::string> result;const char* next=names.data();for(u32 n=0;n<count;++n){result.emplace_back(next);next+=result.back().size()+1;}return result;}
std::string Bytes(const fs::path& path){std::ifstream file(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});}
fs::path Backing(const fs::path& root,const std::string& name){auto out=root/"data";std::size_t begin=1;while(begin<name.size()){auto end=name.find('/',begin);const auto part=name.substr(begin,(end==std::string::npos?name.size():end)-begin);
#ifdef _WIN32
 std::string encoded="n";for(const unsigned char b:part){static constexpr char hex[]="0123456789abcdef";encoded+=hex[b>>4];encoded+=hex[b&15];}out/=encoded;
#else
 out/=part;
#endif
 if(end==std::string::npos)break;begin=end+1;}
#ifdef _WIN32
return fs::path(std::wstring(L"\\\\?\\")+fs::absolute(out).native());
#else
return out;
#endif
}
struct Receipt {unsigned calls{};s32 result{};std::thread::id owner=std::this_thread::get_id();OSContext* prior=OSGetCurrentContext();static s32 Done(s32 result,void* context){auto& r=*static_cast<Receipt*>(context);Check(std::this_thread::get_id()==r.owner&&NativeInterruptDispatchActive()&&!NativeInterruptsEnabled()&&OSGetCurrentContext()!=r.prior,"IOS callback lost genuine owner/IRQ context");Check(!ServiceNativeIOSRequests(),"IOS callback reentered delivery");++r.calls;++callbacks;r.result=result;return 0;}};
void Pump(){Check(ServiceNativeIOSRequests(),"Actual async completion missing");Check(NativeInterruptsEnabled(),"Completion leaked source IRQ mask");}
std::array<unsigned char,32> Pattern(unsigned n){std::array<unsigned char,32> bytes{};for(unsigned i=0;i<bytes.size();++i)bytes[i]=static_cast<unsigned char>(i*71+n*13);bytes[0]=0;bytes[1]=13;bytes[2]=10;bytes[3]=26;bytes[4]=255;return bytes;}
void Write(const std::string& path,const std::array<unsigned char,32>& bytes){auto fd=IOS_Open(path.c_str(),IPC_OPEN_RW);Check(fd>=0,"Physical IOS file open failed");Check(IOS_Write(fd,bytes.data(),bytes.size())==bytes.size(),"Binary physical write changed bytes/count");Check(IOS_Close(fd)==0,"Actual physical flush/close failed");}
void Read(const std::string& path,const std::array<unsigned char,32>& bytes){auto fd=IOS_Open(path.c_str(),IPC_OPEN_READ);Check(fd>=0,"Existing physical file missing");std::array<unsigned char,40> out;out.fill(0x5a);Check(IOS_Read(fd,out.data(),out.size())==bytes.size()&&!std::memcmp(out.data(),bytes.data(),bytes.size())&&out[32]==0x5a,"Physical bytes/EOF tail changed");Check(IOS_Close(fd)==0,"Physical read descriptor failed to retire");}
#ifdef _WIN32
void Junction(const fs::path& alias,const fs::path& target) {
    Check(CreateDirectoryW(alias.c_str(),nullptr),"Fixture junction directory creation failed");
    const auto handle=CreateFileW(alias.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    Check(handle!=INVALID_HANDLE_VALUE,"Fixture reparse descriptor creation failed");
    const auto print=fs::absolute(target).native(),substitute=std::wstring(L"\\??\\")+print;
    struct Buffer {DWORD tag;WORD length,reserved,suboffset,sublength,printoffset,printlength;WCHAR text[2048];} data{};
    data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.sublength=static_cast<WORD>(substitute.size()*2);data.printoffset=data.sublength+2;data.printlength=static_cast<WORD>(print.size()*2);
    data.length=8+data.printoffset+data.printlength+2;
    std::memcpy(data.text,substitute.c_str(),data.sublength+2);std::memcpy(reinterpret_cast<unsigned char*>(data.text)+data.printoffset,print.c_str(),data.printlength+2);
    DWORD returned{};const auto success=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,data.length+8,nullptr,0,&returned,nullptr);const auto error=GetLastError();CloseHandle(handle);
    if(!success)throw std::runtime_error("Real junction creation unavailable: "+std::to_string(error));++checks;
}
#endif
void Run(const fs::path& root) {
    constexpr u64 title=0x0001000052345145ULL;constexpr u32 uid=0x1000;constexpr u16 gid=0x41;
    const NativeFilesystemSettings settings{root,title,uid,gid};
    InitializeNativeFilesystem(settings);const auto owner=std::this_thread::get_id();
    auto device=IOS_Open("/dev/fs",IPC_OPEN_NONE);Check(device>=0,"Actual FS endpoint did not open");
    const std::string base="/title/00010000/52345145/data",dir=base+"/names";
    Check(Create(device,dir,true)==0,"Actual directory create failed");
    const std::vector<std::string> names={"Save","save","CON","con","a:b","x?","trail.",std::string("\x80\xfe")};
    for(unsigned i=0;i<names.size();++i){Check(Create(device,dir+"/"+names[i],false,u8(i+1))==0,"Console byte namespace collapsed/rejected");Write(dir+"/"+names[i],Pattern(i));}
    Check(Listing(device,dir)==names,"Console case/byte names or source directory insertion order changed");
    for(unsigned i=0;i<names.size();++i){Read(dir+"/"+names[i],Pattern(i));const auto attr=Attr(device,dir+"/"+names[i]);Check(attr.uid==uid&&attr.gid==gid&&attr.owner==3&&!attr.group&&!attr.other&&attr.attr==i+1,"Console persistent permissions changed");}
    Check(Create(device,dir+"/read-only",false,9,1)==0&&IOS_Open((dir+"/read-only").c_str(),IPC_OPEN_WRITE)==IPC_RESULT_ACCESS,"Console readonly permissions became host success stub");
    Check(IOS_Open((dir+"/SAVE").c_str(),IPC_OPEN_READ)==IPC_RESULT_NOEXISTS,"Console namespace became case-insensitive");
    Check(Create(device,dir+"/../escape")==IPC_RESULT_INVALID,"Invalid console path escaped backing");
    Check(Delete(device,dir)==IPC_RESULT_NOTEMPTY,"Source nonempty directory deleted");
    auto fd=IOS_Open((dir+"/Save").c_str(),IPC_OPEN_RW);Check(fd>=0,"Async backing descriptor missing");
    Check(Delete(device,dir+"/Save")==IPC_RESULT_OPENFD,"IOS open-file lifetime ignored");
    std::array<unsigned char,40> output{};Receipt receipt;
    std::thread worker([&]{Check(IOS_ReadAsync(fd,output.data(),output.size(),Receipt::Done,&receipt)==0,"Real worker submission failed");});worker.join();
    Check(receipt.calls==0&&GetNativeIOSStatus().pending==1,"Callback ran on worker or disappeared");
    Reject([]{ShutdownNativeFilesystem();},"Queued callback owner retired");
    const auto mask=OSDisableInterrupts();Check(!ServiceNativeIOSRequests()&&!receipt.calls,"Masked callback consumed real request");OSRestoreInterrupts(mask);
    Pump();Check(receipt.calls==1&&receipt.result==32&&!std::memcmp(output.data(),Pattern(0).data(),32),"Real owner completion lost actual binary bytes");
    Check(IOS_Close(fd)==0,"Async source descriptor did not close");
#ifdef _WIN32
    // Independently fixed byte spelling, including reserved and colon names.
    Check(fs::is_regular_file(Backing(root,dir)/"n434f4e")&&fs::is_regular_file(Backing(root,dir)/"n613a62")&&fs::is_regular_file(Backing(root,dir)/"n53617665")&&fs::is_regular_file(Backing(root,dir)/"n73617665"),"Windows physical hex format is not injective");
    const auto catalog=root/"metadata.txt";const auto before=Bytes(catalog);
    auto lock=CreateFileW(catalog.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(lock!=INVALID_HANDLE_VALUE,"Actual metadata lock fixture failed");
    Check(Create(device,dir+"/blocked")==IPC_RESULT_ACCESS,"Locked Windows catalog replacement reported success");
    Check(Bytes(catalog)==before&&!fs::exists(Backing(root,dir+"/blocked"))&&!fs::exists(root/"metadata.txt.tmp"),"Failed catalog replacement changed catalog/leaked staging/new backing");
    Check(CloseHandle(lock),"Real metadata lock failed to retire");
    Check(Create(device,dir+"/blocked")==0,"Catalog replacement did not recover after real lock retirement");
    lock=CreateFileW(Backing(root,dir+"/save").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(lock!=INVALID_HANDLE_VALUE,"Actual destination lock fixture failed");
    Check(Rename(device,dir+"/Save",dir+"/save")==IPC_RESULT_ACCESS,"Locked physical replacement hid Windows failure");
    Check(CloseHandle(lock),"Actual destination lock retirement failed");Read(dir+"/Save",Pattern(0));Read(dir+"/save",Pattern(1));
#endif
    Check(Rename(device,dir+"/Save",dir+"/save")==0,"Real replacement rename failed");Read(dir+"/save",Pattern(0));
    Check(IOS_Open((dir+"/Save").c_str(),IPC_OPEN_READ)==IPC_RESULT_NOEXISTS&&Attr(device,dir+"/save").attr==1,"Original metadata/old pathname changed across rename");
    Check(Listing(device,dir).front()=="save","Rename changed original persistent insertion order");
    const auto large=dir+"/large";Check(Create(device,large)==0,"Large physical fixture create failed");
    constexpr std::uint64_t length=0x100000003ULL;
#ifdef _WIN32
    auto native=CreateFileW(Backing(root,large).c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(native!=INVALID_HANDLE_VALUE,"Sparse physical native owner missing");DWORD returned{};
    Check(DeviceIoControl(native,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr),"Sparse Windows backing unsupported");
    LARGE_INTEGER end{};end.QuadPart=length;Check(SetFilePointerEx(native,end,nullptr,FILE_BEGIN)&&SetEndOfFile(native),"64-bit Windows physical extent failed");Check(CloseHandle(native),"Sparse native handle did not retire");
#else
    auto native=::open(Backing(root,large).c_str(),O_WRONLY);Check(native>=0,"Sparse native owner missing");Check(!::ftruncate(native,length)&&!::close(native),"64-bit physical extent failed");
#endif
    auto usage_name=Name(dir);u32 blocks{},files{};IPCIOVector usage[3]={{usage_name.data(),64},{&blocks,4},{&files,4}};
    Check(IOS_Ioctlv(device,12,1,2,usage)==0&&blocks==262152,"64-bit physical usage truncated actual sparse extent");
    fd=IOS_Open(large.c_str(),IPC_OPEN_READ);Check(fd>=0,"64-bit physical open failed");std::array<u32,2> stats={0x5a5a5a5a,0x5a5a5a5a};
    Check(IOS_Ioctl(fd,11,nullptr,0,stats.data(),8)==IPC_RESULT_MAXBLOCKS&&stats[0]==0x5a5a5a5a&&stats[1]==0x5a5a5a5a,"64-bit stat truncated into successful Wii32 output");
    Check(IOS_Seek(fd,0,IPC_SEEK_END)==IPC_RESULT_INVALID&&IOS_Seek(fd,0,IPC_SEEK_BEG)==0,"IOS seek lost bounded signed position policy");Check(IOS_Close(fd)==0,"Large physical descriptor leaked");
    Check(Delete(device,large)==0,"Sparse physical fixture delete failed");
    Receipt flush;Check(IOS_IoctlAsync(device,13,nullptr,0,nullptr,0,Receipt::Done,&flush)==0,"Real async flush request rejected");Pump();Check(flush.calls==1&&flush.result==0,"Real FlushFileBuffers/catalog completion failed");
    Check(IOS_Close(device)==0,"FS device descriptor did not retire");ShutdownNativeFilesystem();Check(GetNativeIOSStatus().devices==0&&GetNativeIOSStatus().descriptors==0,"Physical owner bus lifetime leaked");
    InitializeNativeFilesystem(settings);device=IOS_Open("/dev/fs",IPC_OPEN_NONE);Check(device>=0,"Persisted filesystem did not reopen");Read(dir+"/save",Pattern(0));Read(dir+"/a:b",Pattern(4));Read(dir+"/"+names.back(),Pattern(7));Check(Attr(device,dir+"/save").attr==1,"Reload changed console metadata");
    Check(IOS_Close(device)==0,"Reload descriptor failed to retire");ShutdownNativeFilesystem();
    const auto metadata=root/"metadata.txt";const auto old=Bytes(metadata);auto incompatible=old;incompatible.replace(0,incompatible.find(' '),"unsupported-physical-format");{std::ofstream file(metadata,std::ios::binary|std::ios::trunc);file<<incompatible;}
    Reject([&]{InitializeNativeFilesystem(settings);},"Incompatible physical catalog silently imported");Check(!GetNativeIOSStatus().devices,"Invalid catalog installed IOS device");{std::ofstream file(metadata,std::ios::binary|std::ios::trunc);file<<old;}
#ifdef _WIN32
    const auto alias=root.parent_path()/(root.filename().native()+L"-junction");Junction(alias,root);
    Reject([&]{InitializeNativeFilesystem({alias,title,uid,gid});},"Windows reparse root imported physical data");Check(!GetNativeIOSStatus().devices&&Bytes(metadata)==old,"Rejected reparse root mutated real owner/catalog");Check(RemoveDirectoryW(alias.c_str()),"Fixture reparse root did not retire");
    const auto slot=Backing(root,dir+"/CON");Check(DeleteFileW(slot.c_str()),"Owned file-to-reparse negative setup failed");Junction(slot,root);
    Reject([&]{InitializeNativeFilesystem(settings);},"Windows catalog imported an exact reparse file");Check(!GetNativeIOSStatus().devices,"Child reparse rejection installed IOS device");Check(RemoveDirectoryW(slot.c_str()),"Fixture child reparse did not retire");std::ofstream(slot,std::ios::binary).write(reinterpret_cast<const char*>(Pattern(2).data()),32);
#else
    const auto alias=root.parent_path()/(root.filename().string()+"-symlink");fs::create_directory_symlink(root,alias);
    // Existing POSIX root canonicalization is deliberately unchanged; child
    // symlinks still reject. Windows root rejection is stricter and explicit.
    fs::remove(alias);
    const auto slot=Backing(root,dir+"/CON");fs::remove(slot);fs::create_symlink(root/"metadata.txt",slot);
    Reject([&]{InitializeNativeFilesystem(settings);},"POSIX catalog imported a physical child symlink");fs::remove(slot);std::ofstream(slot,std::ios::binary).write(reinterpret_cast<const char*>(Pattern(2).data()),32);
#endif
    Check(NativeInterruptsEnabled()&&std::this_thread::get_id()==owner,"Physical lifecycle changed hardware owner/mask");
    std::cout<<"Physical IOS filesystem: "<<checks<<" checks / "<<callbacks<<" real owner callbacks; actual binary/64-bit backing, permissions, console byte names, persisted catalog and retirement. No original Windows fs.c/startup/game-save acceptance.\n";
}
}
int main(int argc,char** argv){try{if(argc!=2)throw std::runtime_error("Expected disposable backing parent");
#ifdef _WIN32
const auto pid=GetCurrentProcessId();
#else
const auto pid=getpid();
#endif
const auto unique="owned-"+std::to_string(pid)+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());Run(fs::absolute(fs::path(argv[1]))/fs::path(u8"unicode-\u00e9-\u65e5")/unique);return 0;}catch(const std::exception& error){std::cerr<<"Physical FS gate failure: "<<error.what()<<" ("<<checks<<" checks)\n";return 1;}}

#include "platform/rtc_device.h"
#include "platform/interrupts.h"
#include <dolphin/exi.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
using mscharged::platform::NativeInterruptGuard;
using mscharged::platform::NativeRTCImage;
enum class Request { None, ReadSram, WriteSram, ReadFlags, WriteFlags };
struct RTC {
    std::filesystem::path file;
    NativeRTCImage image{}, pending{};
    std::thread::id owner;
    Request request{};
    std::size_t cursor{}, transferred{};
    EXICallback retry{};
    unsigned callback_depth{};
    bool initialized{}, locked{}, selected{}, busy{}, changed{};
} rtc;
bool Owner() { return rtc.initialized && rtc.owner==std::this_thread::get_id(); }
bool Channel(s32 channel) { return channel==0 && Owner(); }
bool Transfer(s32 channel) { return Channel(channel) && rtc.locked && rtc.selected && !rtc.busy; }
std::array<std::uint8_t,68> Bytes(const NativeRTCImage& image) {
    std::array<std::uint8_t,68> result{};
    std::copy(image.sram.begin(),image.sram.end(),result.begin());
    for(unsigned i=0;i<4;++i)result[64+i]=image.flags>>(24-i*8);
    return result;
}
NativeRTCImage Decode(const std::array<std::uint8_t,68>& bytes) {
    NativeRTCImage result;std::copy_n(bytes.begin(),64,result.sram.begin());
    for(unsigned i=0;i<4;++i)result.flags=(result.flags<<8)|bytes[64+i];
    return result;
}
std::array<std::uint8_t,68> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);std::array<std::uint8_t,68> bytes{};
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()) || file.peek()!=EOF)
        throw std::runtime_error("Native RTC backing must contain exactly68 bytes");
    return bytes;
}
bool Commit(const NativeRTCImage& image,bool creating=false) {
    const auto temporary=std::filesystem::path(rtc.file.string()+".commit");
    std::error_code error;
    // Exclusive temporary-directory ownership avoids borrowing/clobbering an
    // unrelated file. A failed commit changes neither live nor source backing.
    if(!std::filesystem::create_directory(temporary,error))return false;
    bool okay{};
    try {
        const auto next=temporary/"image";const auto bytes=Bytes(image);
        std::ofstream file(next,std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.flush();
        if(!file)throw std::runtime_error("Native RTC backing write failed");
        file.close();if(!file)throw std::runtime_error("Native RTC backing close failed");
        if(!creating && ReadFile(rtc.file)!=Bytes(rtc.image))
            throw std::runtime_error("Native RTC backing changed outside its device owner");
        if(creating && std::filesystem::exists(rtc.file))
            throw std::runtime_error("Native RTC backing appeared during initialization");
        std::filesystem::rename(next,rtc.file);okay=true;
    } catch(...) {}
    std::filesystem::remove_all(temporary,error);return okay;
}
}
namespace mscharged::platform {
void InitializeNativeRTC(const std::filesystem::path& path,std::optional<NativeRTCImage> initial) {
    NativeInterruptGuard guard;
    if(rtc.initialized || path.empty())throw std::logic_error("Native RTC requires one explicit unused backing owner");
    RTC fresh;fresh.file=std::filesystem::absolute(path);fresh.owner=std::this_thread::get_id();
    if(std::filesystem::exists(fresh.file))fresh.image=Decode(ReadFile(fresh.file));
    else {
        if(!initial)throw std::logic_error("Missing native RTC backing has no explicitly supplied image");
        rtc.file=fresh.file;
        if(!Commit(*initial,true)){rtc={};throw std::runtime_error("Native RTC initial backing could not commit");}
        fresh.image=*initial;
    }
    fresh.initialized=true;rtc=std::move(fresh);
}
void ShutdownNativeRTC() {
    NativeInterruptGuard guard;
    if(!Owner() || rtc.locked || rtc.selected || rtc.busy || rtc.retry || rtc.callback_depth)
        throw std::logic_error("Native RTC retirement requires its unborrowed live owner");
    rtc={};
}
NativeRTCImage ReadNativeRTCImage() {
    NativeInterruptGuard guard;
    if(!Owner())throw std::logic_error("Native RTC snapshot requires its live owner");
    return rtc.image;
}
}
extern "C" BOOL EXILock(s32 channel,u32 device,EXICallback callback) {
    NativeInterruptGuard guard;
    if(!Channel(channel)||device!=1)return FALSE;
    if(rtc.locked) {if(callback && !rtc.retry)rtc.retry=callback;return FALSE;}
    rtc.locked=true;return TRUE;
}
extern "C" BOOL EXIUnlock(s32 channel) {
    const auto enabled=OSDisableInterrupts();
    auto restore=[&]{OSRestoreInterrupts(enabled);};
    try {
        EXICallback retry{};
        {
            NativeInterruptGuard guard;
            if(!Channel(channel)||!rtc.locked||rtc.selected||rtc.busy){restore();return FALSE;}
            rtc.locked=false;retry=rtc.retry;rtc.retry=nullptr;
            if(retry)++rtc.callback_depth;
        }
        // Literal original EXIUnlock dispatches the queued lock retry on this
        // same caller with interrupts disabled and NULL OSContext.
        if(retry) {
            try {retry(channel,nullptr);}catch(...) {--rtc.callback_depth;throw;}
            --rtc.callback_depth;
        }
        restore();return TRUE;
    }catch(...){restore();throw;}
}
extern "C" BOOL EXISelect(s32 channel,u32 device,u32 frequency) {
    NativeInterruptGuard guard;
    if(!Channel(channel)||!rtc.locked||rtc.selected||device!=1||frequency!=3)return FALSE;
    rtc.selected=true;rtc.request=Request::None;rtc.cursor=rtc.transferred=0;
    rtc.pending=rtc.image;rtc.changed=false;return TRUE;
}
extern "C" BOOL EXIDeselect(s32 channel) {
    NativeInterruptGuard guard;
    if(!Channel(channel)||!rtc.selected||rtc.busy)return FALSE;
    bool okay=true;
    if(rtc.changed) {
        okay=Commit(rtc.pending);
        if(okay)rtc.image=rtc.pending;
    }
    rtc.selected=false;rtc.changed=false;rtc.request=Request::None;return okay;
}
extern "C" BOOL EXIImm(s32 channel,void* buffer,s32 length,u32 type,EXICallback callback) {
    NativeInterruptGuard guard;
    if(!Transfer(channel)||!buffer||length<1||length>4||callback)return FALSE;
    if(rtc.request==Request::None) {
        if(length!=4||type!=EXI_WRITE)return FALSE;
        // Original OSRtc command pointers refer to native CPU u32 values.
        u32 command;std::memcpy(&command,buffer,4);
        if(command==0x20000100)rtc.request=Request::ReadSram;
        else if(command==0x21000800)rtc.request=Request::ReadFlags;
        else if(command==0xA1000800)rtc.request=Request::WriteFlags;
        else if(command>=0xA0000100 && command<=0xA00010C0 && (command-0xA0000100)%64==0) {
            rtc.request=Request::WriteSram;rtc.cursor=(command-0xA0000100)/64;
        }else return FALSE;
    }else if(rtc.request==Request::ReadFlags) {
        if(length!=4||type!=EXI_READ||rtc.transferred)return FALSE;
        std::memcpy(buffer,&rtc.image.flags,4);rtc.transferred=4;
    }else if(rtc.request==Request::WriteFlags) {
        if(length!=4||type!=EXI_WRITE||rtc.transferred)return FALSE;
        u32 clear;std::memcpy(&clear,buffer,4);
        if(clear!=0)return FALSE; // Only the actual original clear request is supported.
        rtc.pending.flags=0;rtc.transferred=4;rtc.changed=true;
    }else if(rtc.request==Request::WriteSram) {
        if(type!=EXI_WRITE||std::size_t(length)>64-rtc.cursor)return FALSE;
        std::memcpy(rtc.pending.sram.data()+rtc.cursor,buffer,length);
        rtc.cursor+=length;rtc.transferred+=length;rtc.changed=true;
    }else return FALSE;
    rtc.busy=true;return TRUE;
}
extern "C" BOOL EXISync(s32 channel) {
    NativeInterruptGuard guard;
    if(!Channel(channel)||!rtc.selected||!rtc.busy)return FALSE;
    rtc.busy=false;return TRUE;
}
extern "C" BOOL EXIDma(s32 channel,void* buffer,s32 length,u32 type,EXICallback callback) {
    NativeInterruptGuard guard;
    if(!Transfer(channel)||!buffer||length!=64||type!=EXI_READ||callback||
       reinterpret_cast<std::uintptr_t>(buffer)%32 || rtc.request!=Request::ReadSram || rtc.transferred)return FALSE;
    std::memcpy(buffer,rtc.image.sram.data(),64);rtc.transferred=64;rtc.busy=true;return TRUE;
}
extern "C" BOOL EXIImmEx(s32 channel,void* buffer,s32 length,u32 type) {
    if(length<0||(!buffer&&length))return FALSE;
    auto* bytes=static_cast<std::uint8_t*>(buffer);
    if(length) {
        NativeInterruptGuard guard;
        if(!Transfer(channel)||rtc.request!=Request::WriteSram||type!=EXI_WRITE||
           std::size_t(length)>64-rtc.cursor)return FALSE;
    }
    while(length) {
        const auto count=std::min<s32>(length,4);
        if(!EXIImm(channel,bytes,count,type,nullptr)||!EXISync(channel))return FALSE;
        bytes+=count;length-=count;
    }
    return TRUE;
}

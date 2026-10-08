#include "platform/os_shutdown_record_transport.h"
#include "platform/host_metadata.h"
#include "platform/interrupts.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>

// Passive scheduling probe compiled only into the current-source test. The
// original module has no probe calls or extra scheduling endpoint.
#if defined(MSCHARGED_OS_RECORD_TEST_OBSERVER)
extern "C" void mscharged_os_record_test_submission(NANDCommandBlock*,unsigned);
#endif

namespace {
using Byte = unsigned char;
constexpr std::size_t StateBytes=32, PlayBytes=128;
template<class T> T Native(const Byte* p) { T value; std::memcpy(&value,p,sizeof value); return value; }
template<class T> void Native(Byte* p,T value) { std::memcpy(p,&value,sizeof value); }
template<class T> T Big(const Byte* p) {
    T value{}; for(std::size_t i=0;i<sizeof(T);++i) value=static_cast<T>((value<<8)|p[i]); return value;
}
template<class T> void Big(Byte* p,T value) {
    for(std::size_t i=0;i<sizeof(T);++i) { p[sizeof(T)-1-i]=static_cast<Byte>(value); value>>=8; }
}
void Convert(void* out,const void* in,bool play,bool encode) {
    if(!out||!in) throw std::invalid_argument("OS record transport requires a live fixed-size record");
    // The source/destination may be the same object; snapshot before touching it.
    std::array<Byte,PlayBytes> input{};
    const auto bytes=play?PlayBytes:StateBytes;
    std::memcpy(input.data(),in,bytes); auto* result=static_cast<Byte*>(out);
    std::memcpy(result,input.data(),bytes);
    if(encode) Big<u32>(result,Native<u32>(input.data()));
    else Native<u32>(result,Big<u32>(input.data()));
    if(!play) return;
    for(std::size_t offset=4;offset<84;offset+=2) {
        if(encode) Big<u16>(result+offset,Native<u16>(input.data()+offset));
        else Native<u16>(result+offset,Big<u16>(input.data()+offset));
    }
    for(const auto offset:{88u,96u}) {
        if(encode) Big<u64>(result+offset,Native<u64>(input.data()+offset));
        else Native<u64>(result+offset,Big<u64>(input.data()+offset));
    }
}
void Require(const void* native,u32 bytes,std::size_t expected) {
    if(!native||bytes!=expected) throw std::invalid_argument("OS record transport received a different wire extent");
}
std::size_t WordOffset(const void* native,const void* word,std::size_t bytes) {
    const auto base=reinterpret_cast<std::uintptr_t>(native),address=reinterpret_cast<std::uintptr_t>(word);
    if(!native||address<base||address-base<4||address-base>bytes-4||(address-base)%4)
        throw std::invalid_argument("OS checksum word is outside its original fixed record");
    return address-base;
}

// Wire storage belongs to this actual NAND request until its real completion.
// No source block fields/userData are used for native metadata. The original
// callback is restored before delivery, including reentrant block reuse.
struct alignas(32) Pending {
    alignas(32) std::array<Byte,PlayBytes> wire;
    Pending* next{};
    void* allocation{};
    void* native{};
    NANDCommandBlock* block{};
    NANDAsyncCallback callback{};
    bool reading{},submitting{true},completed{},delivering{};
};
struct Requests { std::mutex mutex; Pending* head{}; };
Requests& State() { static Requests state; return state; }
void Destroy(Pending* value) {
    auto* allocation=value->allocation; value->~Pending(); ChargedNativeMetadataRelease(allocation);
}
void Unlink(Requests& state,Pending* value) {
    auto** slot=&state.head; while(*slot&&*slot!=value) slot=&(*slot)->next;
    if(*slot) *slot=value->next;
}
void FinishSubmission(Pending* value,bool accepted) {
    bool release{},restore{};
    {
        auto& state=State(); std::lock_guard lock(state.mutex);
        value->submitting=false;
        if(value->completed) release=!value->delivering;
        else if(!accepted) {Unlink(state,value);release=true;restore=true;}
    }
    if(!release) return;
    // If the real NAND initializer rejected the call before assigning callback,
    // preserve its old field. If it assigned our bridge, restore the callback
    // that the same original request would have assigned.
    if(restore) value->block->callback=value->callback;
    Destroy(value);
}
void FinishDelivery(Pending* value) {
    bool release{};
    {
        auto& state=State();std::lock_guard lock(state.mutex);
        value->delivering=false;release=!value->submitting;
    }
    if(release) Destroy(value);
}
void Complete(s32 result,NANDCommandBlock* block) {
    // Actual IOS delivery already owns this recursive exclusion. Keep it
    // through callback restoration/return so a foreign submission cannot
    // reuse the same block while its original callback still borrows it.
    mscharged::platform::NativeInterruptGuard exclusion;
    Pending* value{};
    {
        auto& state=State(); std::lock_guard lock(state.mutex);
        for(value=state.head;value&&value->block!=block;value=value->next) {}
        if(!value) throw std::logic_error("OS record completion has no live request-owned wire buffer");
        Unlink(state,value);value->completed=true;value->delivering=true;
    }
    struct RetireAfterDelivery {
        Pending* value;
        ~RetireAfterDelivery() { FinishDelivery(value); }
    } retire{value};
    // The buffer was seeded from the native record before the real read.
    // Decode all cells even on short/error returns, preserving untouched bytes
    // and any genuine partial payload; source decides what the result means.
    if(value->reading) Convert(value->native,value->wire.data(),true,false);
    const auto callback=value->callback;
    block->callback=callback;
    callback(result,block);
}
s32 Submit(NANDFileInfo* file,void* native,u32 bytes,NANDAsyncCallback callback,
           NANDCommandBlock* block,bool reading) {
    // The original callback field is shared with whole NAND. Use the same
    // exclusion as its hardware owner before any cold/accepted/rejected
    // submission can inspect or mutate that field or request metadata.
#if defined(MSCHARGED_OS_RECORD_TEST_OBSERVER)
    mscharged_os_record_test_submission(block,0);
#endif
    mscharged::platform::NativeInterruptGuard exclusion;
    Require(native,bytes,PlayBytes);
    if(!file||!block||!callback) throw std::invalid_argument("OS async record requires the actual file/block/callback owners");
    // Preserve the original cold rejection before its callback field mutation.
    if(!nandIsInitialized()) return reading?NANDReadAsync(file,native,bytes,callback,block)
        :NANDWriteAsync(file,native,bytes,callback,block);
    auto* allocation=ChargedNativeMetadataAllocate(sizeof(Pending)+alignof(Pending)-1);
    auto address=(reinterpret_cast<std::uintptr_t>(allocation)+alignof(Pending)-1)&~std::uintptr_t(alignof(Pending)-1);
    auto* value=new(reinterpret_cast<void*>(address)) Pending{};
    value->allocation=allocation;value->native=native;value->block=block;value->callback=callback;value->reading=reading;
    Convert(value->wire.data(),native,true,true);
    {
        auto& state=State();std::lock_guard lock(state.mutex);
        for(auto* active=state.head;active;active=active->next)
            if(active->block==block) {Destroy(value);throw std::logic_error("OS record NAND block was reused before its actual completion");}
        value->next=state.head;state.head=value;
    }
    s32 result;
    try {
        result=reading?NANDReadAsync(file,value->wire.data(),bytes,Complete,block)
            :NANDWriteAsync(file,value->wire.data(),bytes,Complete,block);
    } catch(...) {FinishSubmission(value,false);throw;}
#if defined(MSCHARGED_OS_RECORD_TEST_OBSERVER)
    mscharged_os_record_test_submission(block,1);
#endif
    FinishSubmission(value,result==NAND_RESULT_OK);
    return result;
}
}

extern "C" void mscharged_os_encode_state_record(void* out,const void* in) {Convert(out,in,false,true);}
extern "C" void mscharged_os_decode_state_record(void* out,const void* in) {Convert(out,in,false,false);}
extern "C" void mscharged_os_encode_play_record(void* out,const void* in) {Convert(out,in,true,true);}
extern "C" void mscharged_os_decode_play_record(void* out,const void* in) {Convert(out,in,true,false);}
extern "C" u32 mscharged_os_state_checksum_word(const void* native,const void* word) {
    return Big<u32>(static_cast<const Byte*>(native)+WordOffset(native,word,StateBytes));
}
extern "C" u32 mscharged_os_play_checksum_word(const void* native,const void* word) {
    const auto offset=WordOffset(native,word,PlayBytes);
    const auto* source=static_cast<const Byte*>(native);
    if(offset<84) return (u32(Native<u16>(source+offset))<<16)|Native<u16>(source+offset+2);
    if(offset>=88&&offset<104) {
        const auto value=Native<u64>(source+(offset<96?88:96));
        return static_cast<u32>(value>>((offset%8)?0:32));
    }
    return Big<u32>(source+offset);
}
extern "C" s32 mscharged_os_state_nand_read(NANDFileInfo* file,void* native,u32 bytes) {
    Require(native,bytes,StateBytes);alignas(32) std::array<Byte,StateBytes> wire;
    Convert(wire.data(),native,false,true);
    const auto result=NANDRead(file,wire.data(),bytes);Convert(native,wire.data(),false,false);return result;
}
extern "C" s32 mscharged_os_state_nand_write(NANDFileInfo* file,const void* native,u32 bytes) {
    Require(native,bytes,StateBytes);alignas(32) std::array<Byte,StateBytes> wire;
    Convert(wire.data(),native,false,true);return NANDWrite(file,wire.data(),bytes);
}
extern "C" s32 mscharged_os_play_nand_write(NANDFileInfo* file,const void* native,u32 bytes) {
    Require(native,bytes,PlayBytes);alignas(32) std::array<Byte,PlayBytes> wire;
    Convert(wire.data(),native,true,true);return NANDWrite(file,wire.data(),bytes);
}
extern "C" s32 mscharged_os_play_nand_read_async(NANDFileInfo* file,void* native,u32 bytes,
        NANDAsyncCallback callback,NANDCommandBlock* block) {return Submit(file,native,bytes,callback,block,true);}
extern "C" s32 mscharged_os_play_nand_write_async(NANDFileInfo* file,const void* native,u32 bytes,
        NANDAsyncCallback callback,NANDCommandBlock* block) {return Submit(file,const_cast<void*>(native),bytes,callback,block,false);}
extern "C" size_t mscharged_os_record_pending_count() {
    auto& state=State();std::lock_guard lock(state.mutex);std::size_t count{};
    for(auto* active=state.head;active;active=active->next) ++count;
    return count;
}

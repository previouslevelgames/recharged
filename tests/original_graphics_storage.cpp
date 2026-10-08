#include "NL/MemAlloc.h"
#include "platform/game_allocation_ownership.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>

namespace {
thread_local int budget = -1;
unsigned live = 0, checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Error, class Action> void Reject(Action action) {
    ++checks; try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("Unsupported graphics metadata operation succeeded");
}
void Word(unsigned char* p, std::uint32_t value) {
    p[0]=value>>24; p[1]=value>>16; p[2]=value>>8; p[3]=value;
}
}
// Genuine host CRT boundary with test-only host metadata OOM injection.
extern "C" void* ChargedNativeMetadataAllocate(std::size_t bytes) {
    if (budget == 0) throw std::bad_alloc();
    if (budget > 0) --budget;
    auto* p = ::operator new(bytes); ++live; return p;
}
extern "C" void ChargedNativeMetadataRelease(void* p) noexcept {
    if (p) { --live; ::operator delete(p); }
}

int main() {
    using namespace mscharged::platform;
    try {
        alignas(64) std::array<std::byte,16384> arena{};
        MemoryAllocator pool{}; pool.Initialize(arena.data(),arena.size());
        const auto free = pool.TotalFreeMemory();
        auto* raw=static_cast<unsigned char*>(pool.Allocate(4096,32,false));
        GameAllocationSpan allocation{};
        Check(FindGameAllocationSpan(raw,4096,allocation) && allocation.owner==&pool,
              "Actual original backing allocation missing");
        Check(reinterpret_cast<std::uintptr_t>(raw)>UINT32_MAX,"No native pointer-width coverage");
        GameByteWriteReservation original(raw,4096);
        std::memset(raw,0xA5,4096); original.Complete(GameByteDomain::WiiSerialized); original.Reset();

        const auto stableLive=live;
        for (int quota=0;quota<4;++quota) {
            budget=quota;
            Reject<std::bad_alloc>([&]{GameGraphicsStorageReservation pending(12);});
            budget=-1;
            Check(live==stableLive,"Failed storage reservation leaked metadata");
            Check(FindGameByteDomain(raw+64,12)==GameByteDomain::WiiSerialized,
                  "Failed storage reservation invalidated untouched source bytes");
        }
        GameGraphicsStorageReservation reserve(12);
        budget=0; reserve.Commit(raw+64); budget=-1;
        GameGraphicsStorageSpan a{};
        Check(FindGameGraphicsStorage(raw+64,12,a) && a.base==raw+64 && a.bytes==12
              && a.allocation.base==raw && a.allocation.owner==&pool,
              "Graphics storage invented an independent allocation owner");
        Check(FindGameAllocationOwner(raw+64)==nullptr,"GL suballocation became a freeable MemoryAllocator block");
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+64);});
        Reject<std::invalid_argument>([&]{FindGameGraphicsStorage(raw+72,5,a);});
        Reject<std::invalid_argument>([&]{GameByteWriteReservation crossing(raw+72,5);});
        Reject<std::invalid_argument>([&]{GameByteWriteReservation crossing(raw+63,2);});
        Check(FindGameByteDomain(raw,64)==GameByteDomain::WiiSerialized
              && FindGameByteDomain(raw+76,4020)==GameByteDomain::WiiSerialized,
              "Registration failed to preserve unaffected logical raw domains");
        const float values[3]={-0.0f,1.25f,-73.5f}; std::memcpy(raw+64,values,sizeof(values));
        budget=0; PublishGameGraphicsNativeBytes(raw+64,raw+76); budget=-1;
        auto array=ResolveGameGraphicsArray(raw+64);
        const std::uint16_t one=1; const bool nativeLE=*reinterpret_cast<const unsigned char*>(&one)==1;
        Check(array.bytes==12 && array.little_endian==nativeLE && array.storage.incarnation==a.incarnation,
              "Native extent/endian was not proven from source storage/producer");
        Check(ResolveGameGraphicsArray(raw+68).bytes==8,"Interior array used guessed count/outer extent");
        Check(!std::memcmp(array.data,values,sizeof(values)),"Native source scalar representation changed");
        budget=0; PublishGameGraphicsNativeBytes(raw+64,raw+76); budget=-1;
        Check(ResolveGameGraphicsArray(raw+64).bytes==12,"Original repeated End was rejected");

        GameGraphicsStorageReservation rawStorage(36); rawStorage.Commit(raw+128);
        GameByteWriteReservation rawWrite(raw+128,36); for(unsigned i=0;i<9;++i)Word(raw+128+i*4,0x3F800000+i);
        budget=0; rawWrite.Complete(GameByteDomain::WiiSerialized); rawWrite.Reset(); budget=-1;
        Check(ResolveGameGraphicsArray(raw+128).bytes==36 && !ResolveGameGraphicsArray(raw+128).little_endian,
              "Actual raw producer was treated as blanket native endian");
        {
            GameByteWriteReservation header(raw+128,8); const std::uint32_t h[2]={7,0};
            std::memcpy(raw+128,h,8); header.Complete(GameByteDomain::NativeHeader);
        }
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+128);});
        Check(ResolveGameGraphicsArray(raw+136).bytes==28 && !ResolveGameGraphicsArray(raw+136).little_endian,
              "Mixed native header changed unconverted raw payload domain");

        GameGraphicsStorageReservation shortStorage(48); shortStorage.Commit(raw+256);
        std::memcpy(raw+256,values,sizeof(values)); PublishGameGraphicsNativeBytes(raw+256,raw+268);
        Check(ResolveGameGraphicsArray(raw+256).bytes==12,"Partial source stream invented full allocation completion");
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+268);});
        std::memcpy(raw+268,values,sizeof(values));
        budget=0; PublishGameGraphicsNativeBytes(raw+256,raw+280); budget=-1;
        Check(ResolveGameGraphicsArray(raw+256).bytes==24,"Continued original source cursor was forbidden");
        GameGraphicsStorageSpan old{};Check(FindGameGraphicsStorage(raw+256,48,old),"Short storage missing");
        GameByteWriteReservation stale(raw+256,48);
        GameGraphicsRetirementReservation rewind;
        budget=0;rewind.Commit(raw+256,48);budget=-1;
        Check(!FindGameGraphicsStorage(raw+256,1,a),"Rewind kept source storage lifetime alive");
        Reject<std::invalid_argument>([&]{stale.Complete(GameByteDomain::WiiSerialized);});
        GameGraphicsStorageReservation reused(48);reused.Commit(raw+256);
        Check(FindGameGraphicsStorage(raw+256,48,a) && a.incarnation!=old.incarnation,
              "Source address reuse revived the old storage incarnation");
        GameByteWriteReservation newWrite(raw+256,48);
        stale.Reset();std::memset(raw+256,0x19,48);newWrite.Complete(GameByteDomain::WiiSerialized);newWrite.Reset();
        Check(ResolveGameGraphicsArray(raw+256).bytes==48,"Old token cleanup invalidated the new source producer");

        // Parsed RLG views share one completed source vertex chunk. Bounds
        // preserve its owner/endian while excluding following packet streams.
        GameGraphicsStorageReservation parsedStorage(128); parsedStorage.Commit(raw+512);
        GameByteWriteReservation parsedWrite(raw+512,128); std::memset(raw+512,0x41,128);
        parsedWrite.Complete(GameByteDomain::WiiSerialized); parsedWrite.Reset();
        GameGraphicsStorageSpan parsedOwner{};
        Check(FindGameGraphicsStorage(raw+512,128,parsedOwner),"Parsed source storage missing");
        RegisterGameGraphicsArray(raw+512,24); RegisterGameGraphicsArray(raw+536,24);
        Check(ResolveGameGraphicsArray(raw+512).bytes==24 && !ResolveGameGraphicsArray(raw+512).little_endian,
              "RLG array includes following streams or changes wire domain");
        Check(ResolveGameGraphicsArray(raw+520).bytes==16,"Interior parsed array loses original byte offset");
        const auto beforeDuplicate=live;budget=0;RegisterGameGraphicsArray(raw+512,24);budget=-1;
        Check(live==beforeDuplicate,"Exact shared array alias allocates metadata");
        budget=0;Reject<std::bad_alloc>([&]{RegisterGameGraphicsArray(raw+560,24);});budget=-1;
        Check(live==beforeDuplicate && ResolveGameGraphicsArray(raw+512).bytes==24,
              "Array metadata OOM leaks or changes existing source extent");
        Reject<std::invalid_argument>([&]{RegisterGameGraphicsArray(raw+632,16);});
        RegisterGameGraphicsArray(raw+520,24); RegisterGameGraphicsArray(raw+512,32);
        Check(ResolveGameGraphicsArray(raw+512).bytes==32 && ResolveGameGraphicsArray(raw+520).bytes==24
              && ResolveGameGraphicsArray(raw+524).bytes==20 && ResolveGameGraphicsArray(raw+536).bytes==24,
              "Shared/interleaved authored aliases reject or truncate valid source reads");
        GameByteWriteReservation freshRaw(raw+512,128);
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+512);});
        std::memset(raw+512,0x42,128);freshRaw.Complete(GameByteDomain::WiiSerialized);freshRaw.Reset();
        Check(ResolveGameGraphicsArray(raw+512).bytes==128,"Old asset bounds clamp a new raw producer");
        RegisterGameGraphicsArray(raw+512,16);
        Check(ResolveGameGraphicsArray(raw+512).bytes==16 && ResolveGameGraphicsArray(raw+528).bytes==112,
              "New logical tag inherits stale neighboring source bounds");
        GameByteWriteReservation converted(raw+512,128);
        std::memset(raw+512,0x43,128);converted.Complete(GameByteDomain::NativePayload);converted.Reset();
        Check(ResolveGameGraphicsArray(raw+512).bytes==128 && ResolveGameGraphicsArray(raw+512).little_endian==nativeLE,
              "Old wire-domain bound survives native conversion");
        RegisterGameGraphicsArray(raw+512,40);
        Check(ResolveGameGraphicsArray(raw+512).bytes==40,"Native domain cannot register its actual source view");
        GameByteWriteReservation replacedPart(raw+528,16);
        std::memset(raw+528,0x44,16);replacedPart.Complete(GameByteDomain::WiiSerialized);replacedPart.Reset();
        Check(ResolveGameGraphicsArray(raw+512).bytes==16 && ResolveGameGraphicsArray(raw+528).bytes==16,
              "Partial source rewrite crosses completed logical origins");
        RegisterGameGraphicsArray(raw+528,8);
        Check(ResolveGameGraphicsArray(raw+528).bytes==8,"Fresh partial source origin cannot register exact bytes");
        GameGraphicsRetirementReservation retireParsed;retireParsed.Commit(raw+512,128);
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+512);});
        GameGraphicsStorageReservation recycledParsed(128);recycledParsed.Commit(raw+512);
        GameByteWriteReservation recycledWrite(raw+512,128);std::memset(raw+512,0x45,128);
        recycledWrite.Complete(GameByteDomain::WiiSerialized);recycledWrite.Reset();
        GameGraphicsStorageSpan recycledOwner{};Check(FindGameGraphicsStorage(raw+512,128,recycledOwner)
            && recycledOwner.incarnation!=parsedOwner.incarnation && ResolveGameGraphicsArray(raw+512).bytes==128,
            "Reused graphics storage revives old parsed-array bounds");
        // The same producer's in-place rewrite of its complete serialized extent
        // (RLG weight/bone-index reordering) keeps the bounds registered for it.
        RegisterGameGraphicsArray(raw+512,24); RegisterGameGraphicsArray(raw+536,24);
        Reject<std::invalid_argument>([&]{GameByteWriteReservation partial(raw+512,64,GameByteInPlaceRewrite{});});
        Reject<std::invalid_argument>([&]{GameByteWriteReservation offset(raw+520,120,GameByteInPlaceRewrite{});});
        Check(FindGameByteDomain(raw+512,128)==GameByteDomain::WiiSerialized && ResolveGameGraphicsArray(raw+512).bytes==24,
              "Rejected in-place rewrite changed the serialized producer");
        GameByteWriteReservation inPlace(raw+512,128,GameByteInPlaceRewrite{});
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+512);});
        std::memset(raw+512,0x46,128);inPlace.Complete(GameByteDomain::WiiSerialized);inPlace.Reset();
        Check(ResolveGameGraphicsArray(raw+512).bytes==24 && ResolveGameGraphicsArray(raw+536).bytes==24
              && !ResolveGameGraphicsArray(raw+512).little_endian,
              "In-place weight rewrite dropped its own authored array bounds");
        GameByteWriteReservation plainRewrite(raw+512,128);std::memset(raw+512,0x47,128);
        plainRewrite.Complete(GameByteDomain::WiiSerialized);plainRewrite.Reset();
        Check(ResolveGameGraphicsArray(raw+512).bytes==128,"A plain rewrite inherited old producer bounds");
        GameByteWriteReservation nativeCopy(raw+512,128);std::memset(raw+512,0x48,128);
        nativeCopy.Complete(GameByteDomain::NativePayload);nativeCopy.Reset();
        Reject<std::invalid_argument>([&]{GameByteWriteReservation native(raw+512,128,GameByteInPlaceRewrite{});});
        Check(FindGameByteDomain(raw+512,128)==GameByteDomain::NativePayload,
              "Rejected native-domain in-place rewrite changed the producer");
        RegisterGameGraphicsArray(nullptr,0); // Original zero-size source request stays neutral.

        // Retirement allocates no metadata, even when it must split one raw
        // logical origin into two unaffected pieces around the source rewind.
        GameByteWriteReservation restore(raw+1024,512);std::memset(raw+1024,0xCD,512);
        restore.Complete(GameByteDomain::WiiSerialized);restore.Reset();
        GameGraphicsRetirementReservation middle;
        budget=0;middle.Commit(raw+1152,128);budget=-1;
        Check(FindGameByteDomain(raw+1024,128)==GameByteDomain::WiiSerialized
              && FindGameByteDomain(raw+1280,256)==GameByteDomain::WiiSerialized,
              "Partial rewind dropped the unaffected other source bytes");
        GameCompletedSpan complete{};
        Check(!FindGameCompletedSpan(raw+1152,1,complete),"Retired hole remained completed");
        Reject<std::invalid_argument>([&]{GameGraphicsRetirementReservation partial;partial.Commit(raw+65,1);});
        Check(ResolveGameGraphicsArray(raw+64).bytes==12,"Rejected partial rewind mutated valid source storage");

        GameGraphicsStorageReservation nestedStorage(1024);nestedStorage.Commit(raw+2048);
        std::memset(raw+2048,0x33,1024);PublishGameGraphicsNativeBytes(raw+2048,raw+3072);
        MemoryAllocator child{};child.Initialize(raw+2304,256);auto* leaf=child.Allocate(32,8,false);
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+2048);});
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(leaf);});
        child.Free(leaf);
        Check(ResolveGameGraphicsArray(raw+2048).bytes==1024,"Source nested-free did not restore actual outer storage query");

        const auto beforeZero=live;GameGraphicsStorageReservation zero(0);zero.Commit(nullptr);
        Check(live==beforeZero,"Zero request invented a positive storage allocation");
        Check(!FindGameGraphicsStorage(raw,0,a),"Zero-byte query invented storage readiness");
        unsigned char stack[16]{};Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(stack);});
        pool.Free(raw);
        Check(!FindGameGraphicsStorage(raw+64,1,a),"True backing free retained GL suballocation records");
        Reject<std::invalid_argument>([&]{ResolveGameGraphicsArray(raw+64);});
        Check(pool.TotalFreeMemory()==free,"Metadata changed original allocation/free algorithms");

        // Live native MEM headers still reject frees of their source backing,
        // including an outer allocation around the owning record; unrelated
        // frees and frees after retirement remain unaffected.
        auto* host=static_cast<unsigned char*>(pool.Allocate(2048,32,false));
        auto* other=static_cast<unsigned char*>(pool.Allocate(256,32,false));
        auto* spare=static_cast<unsigned char*>(pool.Allocate(256,32,false));
        MemoryAllocator inner{};inner.Initialize(host+1024,1024);
        auto* nested=static_cast<unsigned char*>(inner.Allocate(512,32,false));
        GameHeapMetadataReservation heap(host+64,512,64);void* heapOwner=heap.Data();heap.Commit(heapOwner);
        GameHeapMetadataReservation nestedHeap(nested+64,256,64);void* nestedOwner=nestedHeap.Data();
        nestedHeap.Commit(nestedOwner);
        Reject<std::logic_error>([&]{pool.Free(host);});
        Reject<std::logic_error>([&]{inner.Free(nested);});
        pool.Free(other);
        Check(FindGameAllocationOwner(host)==&pool,"Rejected free released the owning allocation record");
        RetireGameHeapMetadata(nestedOwner);
        inner.Free(nested);
        Reject<std::logic_error>([&]{pool.Free(host);});
        RetireGameHeapMetadata(heapOwner);
        pool.Free(spare);pool.Free(host);
        Check(pool.TotalFreeMemory()==free,"MEM metadata checks changed the original free algorithm");
        // Local reservations own their pre-reserved scratch nodes until their
        // actual scope destruction. Selected LeakSanitizer checks that cleanup;
        // the separate unchanged197 primitive gate asserts registry emptiness.
        std::printf("Original allocator graphics metadata: %u checks; exact source ownership/domains; no GX render claim\n",checks);
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}

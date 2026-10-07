#include "NL/MemAlloc.h"
#include "NL/nlChunk.h"
#include "platform/game_allocation_ownership.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>

namespace {
thread_local int metadata_budget = -1;
unsigned metadata_live = 0, metadata_allocations = 0, checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Error, class Action> void Reject(Action action, const char* message="Unsupported domain operation succeeded") {
    ++checks; try { action(); } catch (const Error&) { return; }
    throw std::runtime_error(message);
}
void Word(unsigned char* p, std::uint32_t value) {
    p[0]=value>>24; p[1]=value>>16; p[2]=value>>8; p[3]=value;
}
}
// Test-only host metadata fault injection. Both functions retain the genuine
// host CRT new/delete boundary; original game allocations use real MemAlloc.
extern "C" void* ChargedNativeMetadataAllocate(std::size_t bytes) {
    if (metadata_budget == 0) throw std::bad_alloc();
    if (metadata_budget > 0) --metadata_budget;
    auto* p = ::operator new(bytes); ++metadata_live; ++metadata_allocations; return p;
}
extern "C" void ChargedNativeMetadataRelease(void* p) noexcept {
    if (p) { --metadata_live; ::operator delete(p); }
}

int main() {
    using namespace mscharged::platform;
    try {
        alignas(64) std::array<std::byte, 65536> arena{};
        MemoryAllocator parent{}, child{}, nested{};
        parent.Initialize(arena.data(), arena.size());
        const auto initial = parent.TotalFreeMemory();
        auto* backing = static_cast<unsigned char*>(parent.Allocate(16384,64,false));
        child.Initialize(backing,16384);
        auto* middle = static_cast<unsigned char*>(child.Allocate(4096,32,false));
        nested.Initialize(middle,4096);
        auto* leaf = static_cast<unsigned char*>(nested.Allocate(256,32,false));
        GameAllocationSpan span{};
        Check(FindGameAllocationSpan(leaf+17,239,span) && span.base==leaf && span.bytes==256
              && span.owner==&nested, "Exact innermost allocation was not selected");
        // Negative191 accepted this via its outer allocation fallback.
        Reject<std::invalid_argument>([&]{FindGameAllocationSpan(leaf+17,240,span);});
        Reject<std::invalid_argument>([&]{FindGameAllocationSpan(leaf-1,2,span);});
        Reject<std::invalid_argument>([&]{FindGameAllocationSpan(middle+1,4096,span);});
        Check(!FindGameAllocationSpan(leaf,0,span), "Zero bytes invented allocation evidence");
        Check(!FindGameAllocationSpan(arena.data()+arena.size(),1,span), "Arena-end pointer became live");
        Check(!FindGameAllocationSpan(arena.data(),1,span), "Unallocated arena bytes became a payload");
        parent.Free(backing); // Genuine bulk backing retirement, no child frees.
        Check(!FindGameAllocationSpan(leaf,1,span), "Outer free retained nested spans");
        Check(parent.TotalFreeMemory()==initial, "Nested ownership test changed parent free-list behavior");

        auto* raw = static_cast<unsigned char*>(parent.Allocate(256,32,false));
        Check(reinterpret_cast<std::uintptr_t>(raw)>UINT32_MAX, "Fixture did not exercise native pointer width");
        std::memset(raw,0xa5,256);
        auto* root = reinterpret_cast<nlChunk*>(raw);
        Reject<std::invalid_argument>([&]{root->GetID();});
        GameCompletedSpan completed{};
        Check(!FindGameCompletedSpan(raw,8,completed), "Unwritten allocation became completed data");
        {
            GameByteWriteReservation write(raw,128,160);
            Word(raw,0x8001B000); Word(raw+4,120);
            Word(raw+8,0x1B001); Word(raw+12,12);
            Word(raw+16,0x12345678); Word(raw+20,0); Word(raw+24,0);
            Reject<std::invalid_argument>([&]{root->GetSize();});
            metadata_budget=0; write.Complete(GameByteDomain::WiiSerialized); metadata_budget=-1;
            Reject<std::logic_error>([&]{write.Complete(GameByteDomain::WiiSerialized);});
        }
        Check(root->GetID()==0x8001B000 && root->GetSize()==120 && root->GetDataSize()==120,
              "Original chunk ID/size/data geometry changed");
        Check(root->GetFirstChunk()==reinterpret_cast<nlChunk*>(raw+8)
              && root->GetNextChunk()==reinterpret_cast<nlChunk*>(raw+128), "Original chunk address iteration changed");
        Check(FindGameCompletedSpan(raw,128,completed) && completed.base==raw && completed.bytes==128
              && completed.allocation.base==raw, "Logical source extent differs from publication");
        Check(!FindGameCompletedSpan(raw+128,1,completed), "DMA padding became completed logical bytes");
        Reject<std::invalid_argument>([&]{FindGameByteDomain(raw+128,1);});
        const auto before = std::array<unsigned char,128>{}; (void)before;
        unsigned char untouched[100]; std::memcpy(untouched,raw+28,100);
        {
            GameByteWriteReservation header(raw+8,8), payload(raw+16,12);
            Check(!FindGameCompletedSpan(raw,128,completed), "Pending conversion retained false full-span validity");
            Reject<std::invalid_argument>([&]{root->GetID();});
            const std::uint32_t h[2]={0x1B001,12}, p[3]={0x12345678,0,0};
            std::memcpy(raw+8,h,8); std::memcpy(raw+16,p,12);
            metadata_budget=0;
            header.Complete(GameByteDomain::NativeHeader);
            payload.Complete(GameByteDomain::NativePayload);
            metadata_budget=-1;
        }
        Check(root->GetSize()==120 && root->GetFirstChunk()->GetID()==0x1B001,
              "Mixed-domain child conversion damaged parent header interpretation");
        Check(FindGameCompletedSpan(raw,128,completed) && completed.base==raw && completed.bytes==128,
              "Native leaf conversion lost actual original logical extent");
        Check(FindGameByteDomain(raw,8)==GameByteDomain::WiiSerialized
              && FindGameByteDomain(raw+8,8)==GameByteDomain::NativeHeader
              && FindGameByteDomain(raw+16,12)==GameByteDomain::NativePayload,
              "Explicit header/payload/raw domains were conflated");
        Check(std::memcmp(untouched,raw+28,100)==0, "Conversion changed unrelated raw backing bytes");
        Reject<std::invalid_argument>([&]{ReadGameChunkWord(raw+16,0);});
        Reject<std::invalid_argument>([&]{ReadGameChunkWord(raw,2);});
        {
            GameByteWriteReservation first(raw+32,8);
            Reject<std::logic_error>([&]{GameByteWriteReservation overlap(raw+36,8);});
            GameByteWriteReservation second(raw+48,8);
            first.Reset(); second.Reset();
        }
        Check(!FindGameCompletedSpan(raw,128,completed), "Cancellation revived overwritten logical bytes");
        Check(FindGameByteDomain(raw,8)==GameByteDomain::WiiSerialized
              && FindGameByteDomain(raw+64,32)==GameByteDomain::WiiSerialized,
              "Cancellation invalidated unaffected completed bytes");

        // Restore by a genuine fixture producer, then prove node reservation OOM
        // leaves existing completion metadata and original allocator unchanged.
        { GameByteWriteReservation write(raw,128); write.Complete(GameByteDomain::WiiSerialized); }
        for (int budget : {0,1,2,3}) {
            const auto free=parent.TotalFreeMemory(), count=parent.m_allocation_count;
            bool oom=false;
            metadata_budget=budget;
            try { GameByteWriteReservation write(raw+8,8); }
            catch(const std::bad_alloc&) { oom=true; }
            metadata_budget=-1;
            Check(oom, "Expected reservation metadata failure was not reached");
            Check(FindGameCompletedSpan(raw,128,completed), "Reservation OOM mutated completed metadata");
            Check(parent.TotalFreeMemory()==free && parent.m_allocation_count==count,
                  "Metadata reservation changed original allocation requests");
        }
        // Four pre-write reservations suffice even when many independent leaf
        // conversions retain the same original logical completion. Completion
        // and unaffected-domain retention must perform no host allocation.
        {
            metadata_budget=4;
            GameByteWriteReservation write(raw+8,8);
            Check(metadata_budget==0, "Reservation did not exercise every reserved node");
            write.Complete(GameByteDomain::NativeHeader);
            metadata_budget=-1;
        }
        auto* many = static_cast<unsigned char*>(parent.Allocate(32768,32,false));
        std::memset(many,0x6b,32768);
        { GameByteWriteReservation write(many,32768); write.Complete(GameByteDomain::WiiSerialized); }
        for(unsigned offset=8;offset<32768;offset+=16) {
            const auto beforeAllocations=metadata_allocations;
            GameByteWriteReservation write(many+offset,8);
            Check(metadata_allocations-beforeAllocations==4,
                  "Leaf conversion copied metadata for unrelated completed spans");
            metadata_budget=0;
            write.Complete(GameByteDomain::NativePayload);
            metadata_budget=-1;
        }
        Check(FindGameCompletedSpan(many,32768,completed) && completed.base==many
              && completed.bytes==32768, "Many converted leaves lost the original logical extent");
        for(unsigned offset=0;offset<32768;offset+=16) {
            Check(FindGameByteDomain(many+offset,8)==GameByteDomain::WiiSerialized
                  && FindGameByteDomain(many+offset+8,8)==GameByteDomain::NativePayload,
                  "Range-local retirement altered an unrelated leaf domain");
        }
        std::array<unsigned char,32768> expectedBytes{}; expectedBytes.fill(0x6b);
        Check(std::memcmp(many,expectedBytes.data(),expectedBytes.size())==0,
              "Metadata conversion changed source bytes");
        // A crossing producer erases several spans and trims both edges while
        // keeping the same source origin in its completed native publication.
        { GameByteWriteReservation crossing(many+3,74); crossing.Complete(GameByteDomain::NativePayload); }
        Check(FindGameByteDomain(many,3)==GameByteDomain::WiiSerialized
              && FindGameByteDomain(many+3,74)==GameByteDomain::NativePayload
              && FindGameByteDomain(many+77,3)==GameByteDomain::NativePayload
              && FindGameByteDomain(many+80,8)==GameByteDomain::WiiSerialized,
              "Multi-span retirement damaged prefix/suffix or unrelated domains");
        Check(FindGameCompletedSpan(many,32768,completed), "Crossing write lost the shared source origin");
        parent.Free(many);

        // A native twin belongs to the actual source allocation and completed
        // bytes. Views may overlap or nest; one producer invalidates every
        // covering view while leaving disjoint views alive.
        auto* packed=static_cast<unsigned char*>(parent.Allocate(256,32,false));
        GameNativeBackingSpan twin{};
        GameNativeBackingSourceSpan reverse{};
        Reject<std::invalid_argument>([&]{GameNativeBackingReservation v(packed,256,512);});
        { GameByteWriteReservation producer(packed,256); producer.Complete(GameByteDomain::WiiSerialized); }
        const auto beforeTwinMetadata=metadata_live;
        {
            GameNativeBackingReservation abandoned(packed,8,32);
            Check(abandoned.Data()!=packed, "Native twin aliases authored source storage");
            Check(!FindGameNativeBacking(packed,8,twin), "Uncommitted native view became discoverable");
            Check(!FindGameNativeBackingSource(abandoned.Data(),1,packed,1,reverse),
                  "Unpublished native reservation became reverse-discoverable");
        }
        Check(metadata_live==beforeTwinMetadata, "Abandoned native reservation leaked metadata");
        metadata_budget=0;
        Reject<std::bad_alloc>([&]{GameNativeBackingReservation v(packed,8,32);});
        metadata_budget=-1;
        Check(!FindGameNativeBacking(packed,8,twin) && FindGameCompletedSpan(packed,256,completed),
              "Native backing OOM changed actual source bytes or publication");
        auto attach=[&](unsigned offset,unsigned bytes) {
            GameNativeBackingReservation view(packed+offset,bytes,bytes*2);
            std::memset(view.Data(),0x6c,bytes*2); view.Commit();
            Check(FindGameNativeBacking(packed+offset,bytes,twin) && twin.bytes==bytes*2
                  && twin.allocation.base==packed && twin.allocation.owner==&parent,
                  "Published native backing lost its genuine source owner");
        };
        attach(0,8); attach(8,64); attach(24,16); attach(28,8); attach(128,32);
        auto* firstNative = static_cast<unsigned char*>(twin.data);
        const auto firstIncarnation = twin.allocation.incarnation;
        const auto lookupMetadata = metadata_allocations;
        metadata_budget=0;
        Check(FindGameNativeBackingSource(firstNative+63,1,packed+129,1,reverse)
              && reverse.source==packed+128 && reverse.source_bytes==32
              && reverse.backing.data==firstNative && reverse.backing.bytes==64
              && reverse.backing.allocation.base==packed
              && reverse.backing.allocation.owner==&parent
              && reverse.backing.allocation.incarnation==firstIncarnation,
              "Native reverse lookup lost exact retained source range/owner/incarnation");
        metadata_budget=-1;
        Check(metadata_allocations==lookupMetadata,"Read-only reverse lookup allocated metadata");
        Check(!FindGameNativeBackingSource(firstNative+63,2,packed+128,1,reverse)
              && reverse.source==nullptr && reverse.backing.data==nullptr,
              "Native reverse lookup crossed its true native extent or retained stale output");
        Check(!FindGameNativeBackingSource(firstNative+64,1,packed+128,1,reverse),
              "Native one-past-end became a live view");
        Check(!FindGameNativeBackingSource(firstNative,1,packed,1,reverse),
              "Foreign raw probe selected another attached native view");
        Check(!FindGameNativeBackingSource(packed,1,packed+128,1,reverse),
              "Raw bytes masqueraded as native backing");
        Check(!FindGameNativeBackingSource(firstNative,0,packed+128,1,reverse)
              && !FindGameNativeBackingSource(firstNative,1,packed+128,0,reverse),
              "Zero-width query invented native backing evidence");
        Check(!FindGameNativeBackingSource(reinterpret_cast<void*>(UINTPTR_MAX),2,
                                           packed+128,1,reverse),
              "Overflowing foreign address became native backing");
        auto* foreignRaw = parent.Allocate(32,32,false);
        Check(!FindGameNativeBackingSource(firstNative,1,foreignRaw,1,reverse),
              "Another actual source owner range admitted a foreign native view");
        parent.Free(foreignRaw);
        Check(FindGameNativeBacking(packed+8,64,twin),"Middle source twin absent");
        auto* middleNative=twin.data;
        Check(FindGameNativeBackingSource(middleNative,1,packed+32,1,reverse)
              && reverse.source==packed+8 && reverse.source_bytes==64,
              "Overlapping source native view selected the wrong exact retained backing");
        Reject<std::logic_error>([&]{GameNativeBackingReservation duplicate(packed,8,32);});
        { GameByteWriteReservation crossing(packed+32,1); crossing.Complete(GameByteDomain::NativePayload); }
        Check(!FindGameNativeBacking(packed+8,64,twin) && !FindGameNativeBacking(packed+24,16,twin)
              && !FindGameNativeBacking(packed+28,8,twin), "Producer retained a covering native view");
        Check(FindGameNativeBacking(packed,8,twin) && FindGameNativeBacking(packed+128,32,twin),
              "Producer retired disjoint native views");
        Check(!FindGameNativeBackingSource(middleNative,1,packed+32,1,reverse),
              "Overlapping producer retained a stale native reverse view");
        Check(FindGameNativeBackingSource(firstNative,1,packed+128,1,reverse),
              "Disjoint producer retired another native reverse view");
        {
            GameNativeBackingReservation staleView(packed+80,16,32);
            // Native conversion preserves the original logical-completion tag,
            // but the expanded pending view still requires serialized bytes.
            GameByteWriteReservation conversion(packed+80,16);
            conversion.Complete(GameByteDomain::NativePayload);
            Reject<std::invalid_argument>([&]{staleView.Commit();}, "Pending backing accepted converted source bytes");
        }
        parent.Free(packed);
        Check(!FindGameNativeBacking(packed,8,twin) && !FindGameNativeBacking(packed+128,32,twin),
              "Source free retained CRT-owned native data");
        Check(!FindGameNativeBackingSource(firstNative,1,packed+128,1,reverse),
              "Actual original source free retained a native reverse view");
        GameByteWriteReservation stale(raw,32);
        parent.Free(raw);
        auto* reused=static_cast<unsigned char*>(parent.Allocate(256,32,false));
        Check(reused==raw, "Address-reuse case did not actually reuse source backing");
        Reject<std::invalid_argument>([&]{stale.Complete(GameByteDomain::WiiSerialized);});
        stale.Reset();
        Check(!FindGameCompletedSpan(reused,8,completed), "Stale completion revived a new allocation");
        {
            GameByteWriteReservation reset(reused,32);
            parent.Initialize(arena.data(),arena.size());
            Reject<std::invalid_argument>([&]{reset.Complete(GameByteDomain::WiiSerialized);});
        }
        Check(parent.TotalFreeMemory()==initial && metadata_live==0,
              "Source reset or ticket teardown retained host metadata/game storage");
        std::printf("Actual original allocator/raw-domain primitive: %u checks; metadata_live=%u\n",checks,metadata_live);
        return 0;
    } catch(const std::exception& error) {
        metadata_budget=-1; std::fprintf(stderr,"%s\n",error.what()); return 1;
    }
}

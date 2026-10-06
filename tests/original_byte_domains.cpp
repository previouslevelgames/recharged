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
unsigned metadata_live = 0, checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Error, class Action> void Reject(Action action) {
    ++checks; try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("Unsupported domain operation succeeded");
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
    auto* p = ::operator new(bytes); ++metadata_live; return p;
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
        for (int budget : {0,1,2,3,4}) {
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

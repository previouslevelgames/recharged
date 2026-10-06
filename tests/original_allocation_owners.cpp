#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "platform/game_allocation_ownership.h"
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>

namespace
{
thread_local int metadata_budget = -1;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class Error, class Action> void Reject(Action action)
{
    ++checks;
    try { action(); }
    catch (const Error&) { return; }
    throw std::runtime_error("Rejected ownership operation succeeded");
}
}
// Fixture-only global host operator override for synchronous metadata OOM.
// SDK and production targets retain their ordinary host CRT operators. The
// failure budget is enabled only around the original allocation calls below.
void* operator new(std::size_t size)
{
    if (metadata_budget == 0) throw std::bad_alloc();
    if (metadata_budget > 0) --metadata_budget;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main()
{
    using namespace mscharged::platform;
    try
    {
        alignas(64) std::array<std::byte, 131072> a{}, b{};
        MemoryAllocator mem1{}, mem2{}, child{}, nested{};
        mem1.Initialize(a.data(), a.size());
        mem2.Initialize(b.data(), b.size());
        auto* backing = mem1.Allocate(32768, 64, false);
        child.Initialize(backing, 32768);
        auto* nestedBacking = child.Allocate(8192, 32, true);
        nested.Initialize(nestedBacking, 8192);
        auto* one = mem1.Allocate(192, 8, true);
        auto* two = mem2.AllocateFromStart(192, 32);
        auto* three = child.AllocateFromEnd(192, 64);
        auto* four = nested.Allocate(192, 32, false);
        std::memset(one, 0x11, 192);
        std::memset(two, 0x22, 192);
        std::memset(three, 0x33, 192);
        std::memset(four, 0x44, 192);
        for (auto [pointer, owner] : {std::pair{one,&mem1}, {two,&mem2}, {three,&child}, {four,&nested}})
        {
            Check(FindGameAllocationOwner(pointer)==owner, "Wrong exact original allocator owner");
            Check(FindGameAllocationOwner(static_cast<std::byte*>(pointer)+8)==nullptr,
                "Interior payload invented an allocation owner");
        }
        const auto parentTotal = mem1.TotalFreeMemory();
        const auto childTotal = child.TotalFreeMemory();
        Reject<std::invalid_argument>([&]{mem1.Free(three);});
        Reject<std::invalid_argument>([&]{nlFree(static_cast<std::byte*>(four)+8);});
        Check(mem1.TotalFreeMemory()==parentTotal && child.TotalFreeMemory()==childTotal,
            "Rejected owner changed original rings");
        for (unsigned i=0;i<192;++i)
        {
            Check(static_cast<unsigned char*>(one)[i]==0x11, "Parent payload damaged");
            Check(static_cast<unsigned char*>(two)[i]==0x22, "Separate arena payload damaged");
            Check(static_cast<unsigned char*>(three)[i]==0x33, "Nested child payload damaged");
            Check(static_cast<unsigned char*>(four)[i]==0x44, "Deep child payload damaged");
        }
        nlFree(three); // No ambient allocator or stack lookup.
        Check(FindGameAllocationOwner(three)==nullptr, "Freed allocation stayed registered");
        Check(mem1.TotalFreeMemory()==parentTotal, "Child free changed parent heap");
        nlFree(four);
        nlFree(nestedBacking);
        nlFree(backing);
        nlFree(two);
        nlFree(one);
        Check(mem1.TotalFreeMemory()==a.size() && mem1.LargestFreeBlock()==a.size(), "Parent arena did not recover");
        Check(mem2.TotalFreeMemory()==b.size() && mem2.LargestFreeBlock()==b.size(), "Second arena did not recover");
        Reject<std::invalid_argument>([&]{nlFree(one);});
        nlFree(nullptr);

        // Both host allocations in a reservation must fail before original
        // allocation count/ring mutation. In particular, the map node is fully
        // reserved before the successful source allocation is published.
        for (int budget : {0,1})
        {
            auto* head = mem1.m_free_block_list;
            const auto count = mem1.m_allocation_count;
            bool rejected = false;
            metadata_budget = budget;
            try { mem1.Allocate(256,32,false); }
            catch (const std::bad_alloc&) { rejected=true; }
            metadata_budget=-1;
            Check(rejected, "Native ownership metadata OOM was accepted");
            Check(mem1.m_free_block_list==head && mem1.m_allocation_count==count
                && mem1.TotalFreeMemory()==a.size(), "Metadata OOM mutated the original heap");
        }

        // Original parent backing release can bulk-discard a child heap.
        backing = mem1.Allocate(16384,32,false);
        child.Initialize(backing,16384);
        three=child.Allocate(128,8,false);
        nestedBacking=child.Allocate(4096,32,false);
        nested.Initialize(nestedBacking,4096);
        four=nested.Allocate(128,8,false);
        nlFree(backing);
        Check(!FindGameAllocationOwner(backing)&&!FindGameAllocationOwner(three)
            &&!FindGameAllocationOwner(nestedBacking)&&!FindGameAllocationOwner(four),
            "Bulk backing release retained descendant ownership");
        Reject<std::invalid_argument>([&]{child.Free(three);});
        Check(mem1.TotalFreeMemory()==a.size(), "Bulk backing release leaked parent memory");

        // Reinitializing an original allocator discards its old backing records.
        one=mem1.Allocate(16384,32,false);
        child.Initialize(one,16384);
        three=child.Allocate(128,8,false);
        auto* foreign=mem2.Allocate(128,8,false);
        mem1.Initialize(a.data(),a.size());
        Check(!FindGameAllocationOwner(one)&&!FindGameAllocationOwner(three),
            "Allocator reset retained stale nested allocations");
        Check(FindGameAllocationOwner(foreign)==&mem2, "Reset discarded another arena");
        nlFree(foreign);

        // An explicitly copied source arena snapshot may perform its original
        // free, after which the fixture restores that snapshot to the owner.
        one=mem1.Allocate(a.size()-8,8,false);
        auto copied=mem1;
        copied.Free(one);
        mem1=copied;
        Check(!FindGameAllocationOwner(one)&&mem1.TotalFreeMemory()==a.size(), "Source arena copy lost free ownership");
        for (unsigned i=0;i<1000;++i)
        {
            one=mem1.Allocate(32+(i%97),8<<(i%4),i%2);
            Check(FindGameAllocationOwner(one)==&mem1, "Reused address has stale owner");
            nlFree(one);
        }
        Check(mem1.TotalFreeMemory()==a.size()&&mem1.LargestFreeBlock()==a.size(), "Repeated ownership fragmented original arena");
        // Exercise original nlMalloc/nlFree after the custom allocator has
        // left the source stack. No parent/default arena range can select it.
        backing=mem1.Allocate(8192,32,false);
        child.Initialize(backing,8192);
        gMemoryInitialized=1;
        CurrentAllocator=&child;
        AllocatorStack[1]=&child;
        AllocatorStackDepth=2;
        auto* fromSource=nlMalloc(128,32,true);
        CurrentAllocator=&StandardAllocator;
        AllocatorStack[1]=nullptr;
        AllocatorStackDepth=1;
        Check(FindGameAllocationOwner(fromSource)==&child, "Original nlMalloc lost custom ownership");
        nlFree(fromSource);
        Check(child.TotalFreeMemory()==8192, "Original nlFree used ambient/default owner");
        nlFree(backing);
        gMemoryInitialized=0;
        Check(mem1.TotalFreeMemory()==a.size(), "Original source frees did not recover backing");
        std::cout << "Exact original allocation ownership: " << checks << " checks\n";
    }
    catch (const std::exception& error)
    {
        metadata_budget=-1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}

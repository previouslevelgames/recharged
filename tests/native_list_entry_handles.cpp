#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#endif
#include "NL/nlDLListContainer.h"
#include "NL/MemAlloc.h"
#include "platform/game_allocation_ownership.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
unsigned checks;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Payload {
    std::uint64_t value = 0;
    bool operator==(const Payload& other) const { return value == other.value; }
};
// Caller-supplied allocator protocol for the actual source templates. Every
// byte comes from the genuine original MemoryAllocator/ownership registry.
// No nlMalloc, ring/list operation or allocation result is replaced here.
template<class Node> struct Adapter {
    MemoryAllocator* owner;
    Node* Allocate() {
        return static_cast<Node*>(owner->Allocate(sizeof(Node), alignof(Node), false));
    }
    void Allocate(Node*& node) { node = Allocate(); }
    void AllocateForReturn(Node*& node) { node = Allocate(); }
    Node* New(const Node& value) { return new (Allocate()) Node(value); }
    void DeleteEntry(Node* node) { owner->Free(node); }
};
template<class Node> void CheckHandle(std::uintptr_t handle, Node* node, MemoryAllocator& owner) {
    Check(handle > UINT32_MAX, "Actual node handle did not exercise an address above 4 GiB");
    Check(handle == reinterpret_cast<std::uintptr_t>(node), "Entry handle truncated the actual node address");
    Check(reinterpret_cast<Node*>(handle) == node, "Entry handle round trip selected a different node");
    mscharged::platform::GameAllocationSpan span{};
    Check(mscharged::platform::FindGameAllocationSpan(node, sizeof(Node), span)
          && span.owner == &owner && span.base == node,
          "Node is not the exact live original allocator-owned payload");
}
template<class List> void CheckRing(List& list, const std::array<std::uint64_t, 4>& values) {
    auto it = list.Begin();
    for (auto expected : values) {
        Check(!it.IsDone() && (*it).value == expected, "Original ring order or payload changed");
        auto* node = it.CurrentEntry();
        Check(node->m_next->m_prev == node && node->m_prev->m_next == node,
              "Original intrusive ring links lost reciprocity");
        it.Step();
    }
    Check(it.IsDone(), "Original ring has unexpected extra nodes");
}

template<bool Borrowed> void DoubleList(MemoryAllocator& owner) {
    using Node = DLListEntry<Payload>;
    using A = Adapter<Node>;
    using Selected = std::conditional_t<Borrowed, A&, A>;
    using List = DLListContainerBase<Payload, Selected>;
    static_assert(std::is_same_v<decltype(std::declval<List&>().AddEnd(Payload{})), std::uintptr_t>);
    static_assert(std::is_same_v<decltype(std::declval<List&>().AddAfter(
                  std::declval<nlDLListIterator<Payload>&>(), Payload{})), std::uintptr_t>);
    const auto free_before = owner.TotalFreeMemory();
    A adapter{&owner};
    {
        List list(adapter);
        const auto first = list.AddEnd(Payload{11});
        CheckHandle(first, list.m_Head, owner);
        auto it = list.Begin();
        const auto second = list.AddAfter(it, Payload{22});
        CheckHandle(second, list.m_Head, owner);
        std::uintptr_t third = 0;
        Payload* payload = list.AllocateAtEnd(&third);
        CheckHandle(third, list.m_Head, owner);
        Check(payload == &reinterpret_cast<Node*>(third)->entry && payload->value == 0,
              "AllocateAtEnd changed its original returned payload/default construction");
        payload->value = 33;
        Payload* fourth = list.AllocateAtEnd(nullptr);
        Check(fourth == &list.m_Head->entry && fourth->value == 0,
              "Null output changed source node insertion or payload construction");
        fourth->value = 44;
        CheckRing(list, {11, 22, 33, 44});

        Payload removed{};
        list.RemoveStart(&removed);
        Check(removed.value == 11 && !mscharged::platform::FindGameAllocationOwner(
              reinterpret_cast<Node*>(first)), "Original RemoveStart did not free its exact node");
        it = list.Begin();
        list.Remove(&it);
        Check(!mscharged::platform::FindGameAllocationOwner(reinterpret_cast<Node*>(second)),
              "Original iterator Remove retained a source allocation");
        if constexpr (!Borrowed) {
            const Payload data = list.RemoveEntry(reinterpret_cast<Node*>(third));
            Check(data.value == 33 && !mscharged::platform::FindGameAllocationOwner(
                  reinterpret_cast<Node*>(third)), "Original RemoveEntry changed return/free semantics");
        }
        list.Clear();
        Check(list.IsEmpty() && list.m_Head == nullptr && owner.TotalFreeMemory() == free_before,
              "Original Clear did not recover exact source arena bytes");
        list.AddEnd(Payload{55}); // Destructor must free the surviving actual node.
    }
    Check(owner.TotalFreeMemory() == free_before, "Source list destructor leaked native node storage");
    if constexpr (Borrowed) {
        Node* node = adapter.Allocate();
        CheckHandle(reinterpret_cast<std::uintptr_t>(node), node, owner);
        adapter.DeleteEntry(node);
        Check(owner.TotalFreeMemory() == free_before, "List retired its caller-owned borrowed allocator");
    }
}

void SingleList(MemoryAllocator& owner) {
    using Node = ListEntry<Payload>;
    using List = ListContainerBase<Payload, Adapter<Node>>;
    const auto free_before = owner.TotalFreeMemory();
    {
        List list;
        list.m_Allocator.owner = &owner; // Explicit caller-provided test allocation policy.
        std::uintptr_t first = 0, second = 0;
        Payload* one = list.AllocateAtEnd(&first);
        CheckHandle(first, list.m_Head, owner);
        Check(list.m_Tail == list.m_Head && one == &list.m_Head->entry && one->value == 0,
              "Original single-list first-node/default payload semantics changed");
        one->value = 71;
        Payload* two = list.AllocateAtEnd(&second);
        CheckHandle(second, list.m_Tail, owner);
        two->value = 72;
        Payload* three = list.AllocateAtEnd(nullptr);
        three->value = 73;
        auto it = list.Begin();
        for (auto value : {71u, 72u, 73u}) {
            Check(it.IsValid() && it.Current().value == value, "Original single-list order changed");
            it.Next();
        }
        Check(!it.IsValid() && list.m_Tail->next == nullptr, "Original single-list tail is invalid");
        Payload removed{};
        list.RemoveStart(&removed);
        Check(removed.value == 71 && list.m_Head == reinterpret_cast<Node*>(second)
              && !mscharged::platform::FindGameAllocationOwner(reinterpret_cast<Node*>(first)),
              "Original single-list RemoveStart lost return/ownership semantics");
        list.RemoveEntry(Payload{72});
        Check(!mscharged::platform::FindGameAllocationOwner(reinterpret_cast<Node*>(second)),
              "Original single-list RemoveEntry retained node storage");
        list.Clear();
        Check(!list.m_Head && !list.m_Tail && owner.TotalFreeMemory() == free_before,
              "Original single-list Clear lost its head/tail/free geometry");
        list.AddEnd(Payload{74});
    }
    Check(owner.TotalFreeMemory() == free_before, "Original single-list destructor leaked its final node");
}
}

int main() {
    static_assert(sizeof(std::uintptr_t) == sizeof(void*));
#if defined(_WIN32)
    static_assert(sizeof(unsigned long) == 4 && sizeof(void*) == 8);
#endif
    constexpr unsigned arena_bytes = 65536;
    void* arena = nullptr;
    try {
#if defined(_WIN32)
        arena = VirtualAlloc(reinterpret_cast<void*>(std::uintptr_t{0x200000000ull}),
                             arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Check(arena != nullptr, "Actual Windows high-address arena reservation failed");
#else
        arena = mmap(nullptr, arena_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(arena != MAP_FAILED, "Actual host arena mapping failed");
#endif
        Check(reinterpret_cast<std::uintptr_t>(arena) > UINT32_MAX,
              "Host mapping did not exercise a real high pointer");
        MemoryAllocator owner{};
        owner.Initialize(arena, arena_bytes);
        DoubleList<false>(owner);
        DoubleList<true>(owner);
        SingleList(owner);
        Check(owner.TotalFreeMemory() == arena_bytes && owner.LargestFreeBlock() == arena_bytes,
              "Actual original allocator did not fully recover");
        // Retail counts allocation calls cumulatively; Free does not decrement it.
        Check(owner.m_allocation_count == 15, "Original cumulative allocation counter changed");
#if defined(_WIN32)
        Check(VirtualFree(arena, 0, MEM_RELEASE) != 0, "Actual Windows arena release failed");
#else
        Check(munmap(arena, arena_bytes) == 0, "Actual host arena release failed");
#endif
        std::printf("Original list entry handles PASS %u checks; pointer=%zu/ulong=%zu; real high nodes, value/borrowed/single source lifecycle.\n",
                    checks, sizeof(void*), sizeof(unsigned long));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Original list entry handle failure: %s\n", error.what());
        return 1;
    }
}

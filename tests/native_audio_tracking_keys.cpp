#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#endif
#include "Game/Sys/audio.h"
#include "NL/MemAlloc.h"
#include "NL/nlAVLTree.h"
#include "platform/game_allocation_ownership.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
using PauseMember = void (AudioSystem::*)(const AudioInstanceKey&, XSoundHandle**);
using ResumeMember = void (AudioSystem::*)(const AudioInstanceKey&, AudioHandleState*);
static_assert(std::is_same_v<decltype(&AudioSystem::PauseTrackedSound), PauseMember>);
static_assert(std::is_same_v<decltype(&AudioSystem::ResumeTrackedSound), ResumeMember>);
static_assert(sizeof(AudioInstanceKey) == sizeof(void*) && sizeof(AudioInstanceKey) == 8);
#if defined(_WIN32)
static_assert(sizeof(unsigned long) == 4);
#endif

// Caller-supplied policy for the actual original AVL templates. Every node is
// allocated/freed by the real source MemoryAllocator; no game manager is made.
template<class Node> struct Adapter {
    MemoryAllocator* owner{};
    Node* Allocate() {
        return static_cast<Node*>(owner->Allocate(sizeof(Node), alignof(Node), false));
    }
    void Delete(Node* node) {
        node->~Node();
        owner->Free(node);
    }
};
using Entry = AVLTreeEntry<AudioInstanceKey, AudioHandleState>;
using Tree = AVLTreeBase<AudioInstanceKey, AudioHandleState, Adapter<Entry>,
                         DefaultKeyCompare<AudioInstanceKey>>;
struct WalkObserver {
    unsigned calls{};
    AudioInstanceKey previous{};
    void Visit(const AudioInstanceKey& key, AudioHandleState* value) {
        Check(!calls || previous < key, "Original AVL walk lost full-width key ordering");
        Check(value->m_Context != nullptr && value->m_CueId >= 0x11223340u
              && value->m_CueId <= 0x11223347u,
              "Original AVL walk changed numeric cue/context payloads");
        previous = key;
        ++calls;
    }
};

void Keys(MemoryAllocator& owner) {
    const auto free_before = owner.TotalFreeMemory();
    std::array<int, 8> contexts{};
    // Integer equality/order labels sharing low32, never dereferenced as
    // pointers. Real retained context pointers are separate source payloads.
    constexpr std::array<AudioInstanceKey, 8> keys{
        0x100000055ull, 0x200000055ull, 0x800000055ull, 0x400000055ull,
        0x1000000055ull, 0x2000000055ull, 0x8000000055ull, 0x4000000055ull};
    {
        Tree map;
        map.m_Allocator.owner = &owner;
        for (unsigned i = 0; i < keys.size(); ++i) {
            AudioHandleState state{};
            state.Set(i, 0x11223340u + i, &contexts[i], (i & 1) != 0);
            Check(map.Add(keys[i], state) == nullptr,
                  "Original AVL treated distinct high key bits as an existing entry");
            AudioHandleState* stored{};
            Check(map.FindGet(keys[i], &stored) && stored->m_Context == &contexts[i]
                  && stored->m_CueId == 0x11223340u + i && stored->m_SlotId == i
                  && stored->m_CanResume == (i & 1) && stored->m_PauseDepth == 0,
                  "Original AVL changed key/pointer/numeric state storage");
            mscharged::platform::GameAllocationSpan span{};
            Check(mscharged::platform::FindGameAllocationSpan(stored, sizeof(*stored), span)
                  && span.owner == &owner
                  && reinterpret_cast<std::uintptr_t>(span.base) > UINT32_MAX,
                  "AVL payload is not in a real high original allocation");
        }
        Check(map.CountNodes(&map.m_Root->node, 0) == keys.size(),
              "High-bit-distinct original AVL entries aliased");
        AudioHandleState* missing{};
        Check(!map.FindGet(AudioInstanceKey{0x55}, &missing),
              "Original AVL matched a low32-only truncated key");
        AudioHandleState duplicate{};
        duplicate.Set(99, 0xffffffffu, &contexts[7], false);
        AudioHandleState* retained = map.Add(keys[0], duplicate);
        Check(retained && retained->m_Context == &contexts[0]
              && retained->m_CueId == 0x11223340u,
              "Original duplicate insertion changed an existing source payload");
        WalkObserver observer;
        map.Walk(&observer, &WalkObserver::Visit);
        Check(observer.calls == keys.size(), "Original full-width member walk skipped an entry");
        for (unsigned i = 0; i < keys.size(); i += 2) {
            Check(map.Remove(keys[i]), "Original removal rejected its exact full key");
            Check(!map.FindGet(keys[i], &missing), "Original removal kept the selected key");
            AudioHandleState* neighbor{};
            Check(map.FindGet(keys[i + 1], &neighbor)
                  && neighbor->m_Context == &contexts[i + 1],
                  "Original removal retired another entry sharing low32");
        }
        Check(map.CountNodes(&map.m_Root->node, 0) == keys.size() / 2,
              "Original removal changed surviving key count");
        // Source destructor clears the remaining entries while arena is live.
    }
    Check(owner.TotalFreeMemory() == free_before,
          "Original AVL destruction did not recover its exact native node bytes");
}
}

int main() {
    constexpr unsigned arena_bytes = 65536;
    void* arena{};
    try {
#if defined(_WIN32)
        arena = VirtualAlloc(reinterpret_cast<void*>(std::uintptr_t{0x200000000ull}),
                             arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Check(arena != nullptr, "Actual Windows high-address arena reservation failed");
#else
        arena = mmap(nullptr, arena_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(arena != MAP_FAILED, "Actual native arena mapping failed");
#endif
        Check(reinterpret_cast<std::uintptr_t>(arena) > UINT32_MAX,
              "Actual original allocator arena is not above4GiB");
        MemoryAllocator owner{};
        owner.Initialize(arena, arena_bytes);
        Keys(owner);
        Check(owner.TotalFreeMemory() == arena_bytes
              && owner.LargestFreeBlock() == arena_bytes,
              "Actual source allocator did not recover the whole arena");
#if defined(_WIN32)
        Check(VirtualFree(arena, 0, MEM_RELEASE) != 0, "Actual Windows arena release failed");
#else
        Check(munmap(arena, arena_bytes) == 0, "Actual native arena release failed");
#endif
        std::printf("Original audio tracking-key ABI PASS %u checks; pointer=%zu/ulong=%zu; high-bit integer labels, real context/node ownership and member walk. No GameAudio/playback/factory readiness.\n",
                    checks, sizeof(void*), sizeof(unsigned long));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Original audio tracking-key ABI failure: %s\n", error.what());
        return 1;
    }
}

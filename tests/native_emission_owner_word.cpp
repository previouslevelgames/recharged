#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#endif
#include "Game/Effects/EmissionController.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/Render/ElectricFence.h"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
using Word = MSCHARGED_EMISSION_OWNER_KEY;
using Group = const EffectsGroup*;
static_assert(sizeof(Word) == sizeof(void*));
static_assert(std::is_same_v<Word, decltype(EmissionController::m_uUserData)>);
static_assert(std::is_same_v<decltype(static_cast<void (EmissionManager::*)(Word, Group)>(
              &EmissionManager::Destroy)), void (EmissionManager::*)(Word, Group)>);
static_assert(std::is_same_v<decltype(static_cast<void (EmissionManager::*)(Word, Group)>(
              &EmissionManager::Kill)), void (EmissionManager::*)(Word, Group)>);
static_assert(std::is_same_v<decltype(&EmissionManager::FindController),
              EmissionController* (EmissionManager::*)(Word, Group)>);
static_assert(std::is_same_v<decltype(&EmissionManager::IsPlaying),
              bool (EmissionManager::*)(Word, Group)>);
static_assert(std::is_same_v<decltype(&EmissionManager::IsDying),
              bool (EmissionManager::*)(Word, Group)>);
static_assert(std::is_same_v<decltype(&EmitElectricFenceBallEffect),
              bool (*)(const nlVector3&, const nlVector3&, Word, bool)>);
// The distinct character helper receives an original numeric emitter ID.
static_assert(std::is_same_v<decltype(&EmitElectricFenceCharacterEffect),
              void (*)(const nlVector3&, const nlVector3&, unsigned long)>);
static_assert(sizeof(EmissionController::m_uJointIDOverride) == 4);
static_assert(sizeof(EmissionController::m_Id) == 2);

unsigned checks;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Owner { std::uint64_t value; };
constexpr std::size_t bytes = 65536;
void* Reserve(std::uintptr_t address) {
#if defined(_WIN32)
    void* result = VirtualAlloc(reinterpret_cast<void*>(address), bytes,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* result = mmap(reinterpret_cast<void*>(address), bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (result == MAP_FAILED) result = nullptr;
#endif
    Check(result == reinterpret_cast<void*>(address), "Exact high owner reservation failed");
    return result;
}
void Release(void* allocation) {
#if defined(_WIN32)
    Check(VirtualFree(allocation, 0, MEM_RELEASE) != 0, "Owner reservation release failed");
#else
    Check(munmap(allocation, bytes) == 0, "Owner reservation release failed");
#endif
}
}

int main() {
    try {
        static_assert(sizeof(void*) == 8);
#if defined(_WIN32)
        static_assert(sizeof(unsigned long) == 4);
#endif
        Check(MSCHARGED_EMISSION_OWNER_ADDRESS(static_cast<Owner*>(nullptr)) == 0,
              "Original null owner word changed");
        void* first = Reserve(0x200010000ull);
        void* second = Reserve(0x300010000ull);
        auto* a = new (static_cast<char*>(first) + 64) Owner{0x1122334455667788ull};
        auto* b = new (static_cast<char*>(second) + 64) Owner{0x8877665544332211ull};
        const Word ka = MSCHARGED_EMISSION_OWNER_ADDRESS(a);
        const Word kb = MSCHARGED_EMISSION_OWNER_ADDRESS(b);
        Check(ka > UINT32_MAX && kb > UINT32_MAX, "Owner words did not exercise real high addresses");
        Check(static_cast<std::uint32_t>(ka) == static_cast<std::uint32_t>(kb),
              "Reservations do not exercise a low-word collision");
        Check(ka != kb, "Native owner word conflated distinct live owners");
#if defined(_WIN32)
        Check(static_cast<unsigned long>(ka) == static_cast<unsigned long>(kb),
              "Baseline Win64 unsigned-long collision was not reproduced");
#endif
        Check(reinterpret_cast<Owner*>(ka) == a && reinterpret_cast<Owner*>(kb) == b,
              "Native transport changed a live owner address");
        Check(reinterpret_cast<Owner*>(ka)->value == 0x1122334455667788ull
              && reinterpret_cast<Owner*>(kb)->value == 0x8877665544332211ull,
              "Native transport selected another live owner's bytes");
        Check(MSCHARGED_EMISSION_OWNER_ADDRESS(&a->value) == ka,
              "Member pointer transport changed its actual address");
        for (std::uint32_t numeric : {0u, 1u, 0xffffffffu}) {
            const Word key = numeric;
            Check(key == numeric, "Original numeric owner key changed");
        }
        a->~Owner();
        Release(first);
        // No old pointer is dereferenced after retirement. Reuse deliberately
        // has the same address word: this codec adds no incarnation/lifetime.
        first = Reserve(0x200010000ull);
        a = new (static_cast<char*>(first) + 64) Owner{0xabcdef9876543210ull};
        Check(MSCHARGED_EMISSION_OWNER_ADDRESS(a) == ka,
              "Address reuse acquired an invented owner ID");
        Check(a->value == 0xabcdef9876543210ull && b->value == 0x8877665544332211ull,
              "Reuse changed the unrelated live owner");
        a->~Owner(); b->~Owner();
        Release(first); Release(second);
        std::printf("Emission owner word PASS %u checks; real high/colliding/reused addresses, source API/storage widths only.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Emission owner word failure: %s\n", error.what());
        return 1;
    }
}

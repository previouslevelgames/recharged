#include "runtime/tweaks.h"
#include "Game/TweakRegistry.h"
#include "Game/TweakConfig.h"
#include "Game/TweakValueFloat.h"
#include "Game/TweakValueInt.h"
#include "NL/nlSlotPoolFixed.inl"
#include "NL/MemAlloc.h"
#include <map>
#include <set>
#include <thread>
#include <algorithm>
#include <vector>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <cmath>

extern char* gTweakStringBuffers[4];
extern char* gTweakStringNext[4];
extern unsigned int gTweakStringCapacities[4];
extern int gTweakStringBytesUsed[4], gTweakStringCounts[4];
extern unsigned char gTweakRegistryInitialized;
extern TweakEntry* gUserTweakEntry;
extern TweakNode* gTweakPriorityNode;
namespace
{
struct Registration
{
    std::string name, category;
    bool format;
    std::uint64_t order;
    bool binding;
    void* original_address;
};
struct State
{
    std::map<TweakValueBase*, Registration> borrowed;
    std::set<void*> owned;
    bool live = false, initializing = false;
    std::thread::id thread;
    std::uint64_t next_order = 0;
};
State& GetState() { static State state; return state; }
unsigned CountValues(TweakEntry* entry)
{
    unsigned count = 0;
    for (auto* child = entry->m_ChildHead; child; child = child->m_Next)
        count += child->IsEntry() ? CountValues(child->AsEntry()) : 1;
    return count;
}
void ClearPending()
{
    while (auto* p = TweakPendingValue::PopHead()) delete p;
}
void Shutdown()
{
    ClearPending();
    ClearTweakChildren(GetTweakRoot());
    // Construction can fail between allocating a value and publishing its node.
    while (!GetState().owned.empty())
    {
        auto* value = static_cast<TweakValueBase*>(*GetState().owned.begin());
        GetState().owned.erase(GetState().owned.begin());
        delete value;
    }
    gTweakNodePool.FreeBlocks(); gTweakEntryPool.FreeBlocks(); gTweakNamePool.FreeBlocks();
    nlDeleteGameObject(gTweakValueAllocator); gTweakValueAllocator = nullptr;
    nlDeleteGameObject(gTweakBindingAllocator); gTweakBindingAllocator = nullptr;
    for (unsigned i=0; i<4; ++i)
    {
        nlFree(gTweakStringBuffers[i]);
        gTweakStringBuffers[i] = gTweakStringNext[i] = nullptr;
        gTweakStringCapacities[i] = gTweakStringBytesUsed[i] = gTweakStringCounts[i] = 0;
    }
    gUserTweakEntry = nullptr; gTweakPriorityNode = nullptr;
    gTweakRegistryInitialized = 0;
    auto& state = GetState();
    if (!state.owned.empty()) std::terminate();
    for (auto& [value, registration] : state.borrowed)
    {
        value->mName = registration.name.c_str();
        value->mFormatName = registration.format;
        if (registration.binding)
            static_cast<TweakBindingBase*>(value)->BindValueAddress(registration.original_address);
    }
    state.live = state.initializing = false;
    gLastTweakCategory = nullptr;
}
}
namespace mscharged
{
int TweakInteger(const char* text)
{
    if (!text) throw std::invalid_argument("Null tweak integer");
    errno = 0;
    const auto value = std::strtoll(text, nullptr, 10);
    if (errno == ERANGE || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::out_of_range("Tweak integer is outside the Wii range");
    return static_cast<int>(value);
}
float TweakFloat(const char* text)
{
    if (!text) throw std::invalid_argument("Null tweak float");
    const auto value = std::strtod(text, nullptr);
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        throw std::out_of_range("Tweak float is outside the finite Wii range");
    return static_cast<float>(value);
}
void* AllocatePendingTweak() { return new TweakPendingValue{}; }
void RememberTweakValue(TweakValueBase* value, const char* category)
{
    if (!value || !value->mName || !category) throw std::invalid_argument("Invalid pending tweak registration");
    auto& state = GetState();
    if (!state.borrowed.count(value))
        state.borrowed.emplace(value, Registration{value->mName, category, value->mFormatName, state.next_order++,
            value->GetStorageKind() == 2, value->GetValueAddress()});
}
void ForgetTweakValue(TweakValueBase* value)
{
    GetState().borrowed.erase(value);
    TweakPendingValue* previous = nullptr;
    for (auto* entry = gPendingTweakHead; entry;)
    {
        auto* next = entry->m_Next;
        if (entry->m_Value == value)
        {
            if (previous) previous->m_Next = next; else gPendingTweakHead = next;
            if (entry == gPendingTweakTail) gPendingTweakTail = previous;
            delete entry;
        }
        else previous = entry;
        entry = next;
    }
}
void* OwnTweakAllocation(void* value)
{
    if (!value) throw std::bad_alloc();
    if (!GetState().owned.insert(value).second) throw std::logic_error("Tweak allocation already owned");
    return value;
}
bool TakeOwnedTweakValue(TweakValueBase* value) { return GetState().owned.erase(value) != 0; }
void CheckTweakInitialization(unsigned char push_state)
{
    const auto& state = GetState();
    if (!state.initializing || !gMemoryInitialized || push_state)
        throw std::logic_error("Tweak registry requires native ownership; state push/reset is not connected");
}
OriginalTweaks::OriginalTweaks()
{
    auto& state = GetState();
    if (state.live || state.initializing || !gMemoryInitialized)
        throw std::logic_error("Invalid tweak registry lifetime");
    state.initializing = true; state.thread = std::this_thread::get_id();
    try
    {
        ClearPending();
        std::vector<decltype(state.borrowed)::value_type*> ordered;
        for (auto& entry : state.borrowed) ordered.push_back(&entry);
        std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->second.order < b->second.order; });
        for (auto* entry : ordered)
        {
            auto& [value, registration] = *entry;
            value->mName = registration.name.c_str(); value->mFormatName = registration.format;
            QueueTweakValue(static_cast<TweakPendingValue*>(AllocatePendingTweak()), value, registration.category.c_str());
        }
        InitializeTweakRegistry(0, 0, nullptr);
        state.initializing = false; state.live = live_ = true;
    }
    catch (...) { Shutdown(); throw; }
}
OriginalTweaks::~OriginalTweaks()
{
    if (live_)
    {
        if (GetState().thread != std::this_thread::get_id()) std::terminate();
        Shutdown();
    }
}
std::string VerifyStartupTweaks()
{
    const auto free1=StandardAllocator.TotalFreeMemory(), free2=VirtualAllocator.TotalFreeMemory();
    unsigned count = 0;
    {
        OriginalTweaks tweaks;
        LoadTweakConfigFile("/ini/datetime.ini", "/General/Build Info", false);
        auto* build = FindOrCreateTweakPath(GetTweakRoot(), "/General/Build Info", 1);
        if (!build || !(count = CountValues(build)))
            throw std::runtime_error("Datetime tweak configuration contains no entries");
    }
    if (free1!=StandardAllocator.TotalFreeMemory() || free2!=VirtualAllocator.TotalFreeMemory())
        throw std::runtime_error("Tweak registry did not recover both game arenas");
    return "Original tweak registry parsed datetime configuration: " + std::to_string(count)
        + " values; borrowed values preserved and both arenas recovered.";
}
}

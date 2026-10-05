#include "platform/tweak_storage.h"
#include "runtime/startup.h"
#include "Game/TweakRegistry.h"
#include "NL/nlMemory.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        std::vector<std::uint64_t> first(65536), second(65536);
        mscharged::ResetStartupMemory();
        StandardAllocator.Initialize(first.data(), first.size() * sizeof(first[0]));
        VirtualAllocator.Initialize(second.data(), second.size() * sizeof(second[0]));
        gMemoryInitialized = 1;
        const auto before_first = StandardAllocator.TotalFreeMemory();
        const auto before_second = VirtualAllocator.TotalFreeMemory();
        for (auto* owner : {&StandardAllocator, &VirtualAllocator})
        {
            CurrentAllocator = owner;
            auto* record = static_cast<TweakPendingValue*>(mscharged::AllocateNativeTweakPending());
            Check(record != nullptr, "Native pending allocation failed");
            const auto begin = reinterpret_cast<std::uintptr_t>(owner->m_memory);
            const auto address = reinterpret_cast<std::uintptr_t>(record);
            Check(address >= begin + owner->m_memory_size - 1024,
                  "Pending record lost the original fromEnd allocation contract");
            record->m_Value = nullptr;
            record->m_Category = "Native pending category";
            record->m_Unk8 = 7;
            record->m_Next = record;
            record->m_Registered = 1;
            record->m_DefaultPointer = reinterpret_cast<const char*>(first.data());
            Check(record->m_Next == record && record->m_DefaultPointer == reinterpret_cast<const char*>(first.data()),
                  "Native record storage truncated or overlapped a pointer");
            Check(record->m_Unk8 == 7 && record->m_Registered == 1,
                  "Native pointer storage overwrote adjacent Wii scalar fields");
            CurrentAllocator = owner == &StandardAllocator ? &VirtualAllocator : &StandardAllocator;
            delete record; // The original registry's matching deletion operation.
            Check(StandardAllocator.TotalFreeMemory() == before_first
                  && VirtualAllocator.TotalFreeMemory() == before_second,
                  "Original pending delete did not return storage to the actual game arena");
        }
        mscharged::ResetStartupMemory();
        std::cout << checks << " original pending ABI/arena ownership checks passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

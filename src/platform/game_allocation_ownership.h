#pragma once

class MemoryAllocator;

namespace mscharged::platform
{
// Native ownership metadata lives outside the original game arena. Reserve it
// before the original allocator mutates its ring, so host metadata OOM cannot
// turn a successful game allocation into an unowned block.
class GameAllocationReservation
{
public:
    GameAllocationReservation(MemoryAllocator& owner, unsigned long bytes);
    ~GameAllocationReservation();
    GameAllocationReservation(const GameAllocationReservation&) = delete;
    GameAllocationReservation& operator=(const GameAllocationReservation&) = delete;
    void Commit(void* pointer);

private:
    void* pending_;
};

MemoryAllocator* FindGameAllocationOwner(const void* pointer);
void ValidateGameAllocationFree(MemoryAllocator& owner, const void* pointer);
void FinishGameAllocationFree(const void* pointer);
void DiscardGameAllocatorRecords(MemoryAllocator& owner);
void FreeGameAllocation(void* pointer);
}

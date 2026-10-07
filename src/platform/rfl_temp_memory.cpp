#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error RFL MEM lifetime transport must share the original module owner registry
#endif
#include "platform/rfl_temp_memory.h"
#include "platform/game_allocation_ownership.h"
#include <revolution/mem/expHeap.h>
#include <stdexcept>
using namespace mscharged::platform;
extern "C" void* ChargedAllocRFLTemp(void* heap,std::uint32_t bytes,std::int32_t alignment) {
    GameMemoryStorageReservation storage(heap,bytes);
    void* result=MEMAllocFromExpHeapEx(static_cast<MEMHeapHandle>(heap),bytes,alignment);
    if(result)storage.Commit(result);
    return result;
}
extern "C" void ChargedFreeRFLTemp(void* heap,void* block) {
    // The source MEM free null quirk stays unchanged. Positive frees retire
    // their exact byte incarnation before the allocator reuses any payload.
    if(block)RetireGameMemoryStorage(heap,block);
    MEMFreeToExpHeap(static_cast<MEMHeapHandle>(heap),block);
}
extern "C" void* ChargedDestroyRFLTempHeap(void* heap) {
    GameAllocationSpan backing;
    GameHeapMetadataSpan projected;
    if(FindGameHeapMetadata(heap,projected)) {
        backing=projected.allocation;
        ValidateGameHeapMetadataRetirement(heap);
    } else if(!FindGameAllocationSpan(heap,1,backing))
        throw std::invalid_argument("RFL temp heap no longer has its actual original work backing");
    RetireGameMemoryStorageOwner(heap);
    return MEMDestroyExpHeap(static_cast<MEMHeapHandle>(heap));
}

#include "revolution/hbm/nw4hbm/snd/FrameHeap.h"
#include "revolution/hbm/nw4hbm/snd/SoundHeap.h"
#include "revolution/hbm/nw4hbm/snd/DisposeCallbackManager.h"
#include "revolution/mem/frameHeap.h"
#include "platform/interrupts.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

// Selected original allocator methods, using caller-owned native backing.
// This does not initialize HBM, create a player, or implement missing MEM state APIs.
// Only this observer is compiled with -fno-access-control.
namespace {
using namespace nw4hbm::snd;
unsigned checks;
void Check(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
struct FreeStorage { void operator()(void* address) const { std::free(address); } };
using Storage = std::unique_ptr<void, FreeStorage>;
Storage Area(std::size_t bytes) {
    Storage value(std::aligned_alloc(32, (bytes + 31) & ~std::size_t(31)));
    if (!value) throw std::bad_alloc();
    std::memset(value.get(), 0xa6, bytes);
    return value;
}
struct Notice {
    void* expected;
    u32 bytes;
    unsigned id;
};
struct Recorder {
    std::array<unsigned, 16> ids{};
    unsigned calls{};
    static Recorder* live;
    static void Completed(void* data, u32 bytes, void* context) {
        auto& request = *static_cast<Notice*>(context);
        Check(data == request.expected && bytes == request.bytes,
            "Original Block destructor lost its full-width callback context or requested bytes");
        Check(live && live->calls < live->ids.size(), "Unexpected original free callback");
        live->ids[live->calls++] = request.id;
    }
};
Recorder* Recorder::live{};
class Disposal final : public detail::DisposeCallback {
public:
    std::array<const void*, 16> data{}, wave{};
    std::array<std::size_t, 16> data_bytes{}, wave_bytes{};
    unsigned data_count{}, wave_count{};
    void InvalidateData(const void* start, const void* end) override {
        Check(data_count < data.size(), "Too many original dispose callbacks");
        data[data_count] = start;
        data_bytes[data_count++] = static_cast<const unsigned char*>(end)
            - static_cast<const unsigned char*>(start);
    }
    void InvalidateWaveData(const void* start, const void* end) override {
        Check(wave_count < wave.size(), "Too many original wave-dispose callbacks");
        wave[wave_count] = start;
        wave_bytes[wave_count++] = static_cast<const unsigned char*>(end)
            - static_cast<const unsigned char*>(start);
    }
};
void FrameOwner() {
    auto area = Area(4096);
    Recorder recorder;
    Recorder::live = &recorder;
    std::array<Notice, 4> notice{};
    detail::FrameHeap heap;
    Check(!heap.IsValid(), "Original FrameHeap constructor fabricated an owner");
    Check(heap.Create(area.get(), 4096), "Original frame heap did not create");
    auto* handle = heap.mHandle;
    Check(handle && handle->signature == 0x46524d48 && handle == area.get(),
        "Original MEM frame owner does not reside in its caller storage");
    Check(heap.GetCurrentLevel() == 0 && heap.mSectionList.GetSize() == 1,
        "Original Create did not create exactly its initial Section");
    Check(reinterpret_cast<std::uintptr_t>(handle->heapStart)
            == reinterpret_cast<std::uintptr_t>(area.get()) + sizeof(MEMiHeapHead) + sizeof(MEMiFrmHeapHead),
        "Real MEM frame owner used a console-sized native header");
    const auto initial = heap.GetFreeSize();
    const std::array<u32, 4> sizes{1, 33, 96, 7};
    for (unsigned i = 0; i < notice.size(); ++i) {
        notice[i] = {nullptr, sizes[i], i};
        auto* block = heap.Alloc(sizes[i], Recorder::Completed, &notice[i]);
        notice[i].expected = block;
        Check(block != nullptr && reinterpret_cast<std::uintptr_t>(block) % 32 == 0,
            "Original native Block payload lost its original 32-byte alignment");
        const auto begin = reinterpret_cast<std::uintptr_t>(area.get());
        const auto address = reinterpret_cast<std::uintptr_t>(block);
        Check(address >= begin && address + sizes[i] <= begin + 4096,
            "Original frame allocation leaves its caller backing");
        std::memset(block, int(0x20 + i), sizes[i]);
    }
    Check(heap.GetFreeSize() < initial && recorder.calls == 0,
        "Alloc freed live blocks or did not consume genuine MEM storage");
    Check(heap.Alloc(8192, Recorder::Completed, &notice[0]) == nullptr && recorder.calls == 0,
        "Original exhaustion fabricated success or a callback");
    heap.Clear();
    Check(recorder.calls == 4, "Original Clear failed to destroy every live Block");
    for (unsigned i = 0; i < 4; ++i)
        Check(recorder.ids[i] == 3 - i, "Original Section destructor changed reverse block order");
    Check(heap.IsValid() && heap.GetCurrentLevel() == 0 && heap.GetFreeSize() == initial,
        "Original Clear did not recreate its initial empty Section");
    notice[0] = {nullptr, 17, 5};
    notice[0].expected = heap.Alloc(17, Recorder::Completed, &notice[0]);
    Check(notice[0].expected != nullptr, "Original cleared owner did not reuse its backing");
    heap.Destroy();
    Check(recorder.calls == 5 && recorder.ids[4] == 5 && !heap.IsValid()
            && heap.mSectionList.IsEmpty() && handle->signature == 0,
        "Original Destroy retained blocks, Section nodes or the MEM owner");
    heap.Destroy();
    Check(recorder.calls == 5, "Repeated original Destroy duplicated callbacks");
    Check(heap.Create(area.get(), 4096) && heap.GetFreeSize() == initial,
        "Original Create could not reuse its truly retired MEM backing");
    heap.Destroy();
    Check(!heap.IsValid() && handle->signature == 0, "Reused source frame owner did not retire");
    Recorder::live = nullptr;
}
void SoundOwner() {
    auto area = Area(4096);
    Disposal disposal;
    auto& manager = detail::DisposeCallbackManager::GetInstance();
    manager.RegisterDisposeCallback(&disposal);
    const std::array<u32, 3> sizes{5, 37, 64};
    std::array<void*, 3> blocks{};
    {
        SoundHeap heap;
        Check(!heap.IsValid() && heap.Create(area.get(), 4096),
            "Whole SoundHeap constructor/Create did not establish its source frame owner");
        for (unsigned i = 0; i < blocks.size(); ++i) {
            blocks[i] = heap.Alloc(sizes[i]);
            Check(blocks[i] && reinterpret_cast<std::uintptr_t>(blocks[i]) % 32 == 0,
                "Whole SoundHeap allocation lost its native header/payload alignment");
        }
        Check(!heap.mMutex.thread && heap.mMutex.count == 0,
            "Original SoundHeap Alloc did not release its real source mutex");
        heap.Clear();
        Check(disposal.data_count == 3 && disposal.wave_count == 3,
            "Original SoundHeap dispose callback did not deliver both original domains");
        for (unsigned i = 0; i < blocks.size(); ++i) {
            Check(disposal.data[i] == blocks[2 - i] && disposal.wave[i] == blocks[2 - i]
                    && disposal.data_bytes[i] == sizes[2 - i] && disposal.wave_bytes[i] == sizes[2 - i],
                "Original dispose ranges or block retirement order changed");
        }
        blocks[0] = heap.Alloc(9);
        Check(blocks[0] != nullptr, "Cleared original SoundHeap did not allocate again");
        // The original destructor itself retires this final Block/MEM owner.
    }
    Check(disposal.data_count == 4 && disposal.wave_count == 4
            && disposal.data[3] == blocks[0] && disposal.wave[3] == blocks[0]
            && disposal.data_bytes[3] == 9 && disposal.wave_bytes[3] == 9,
        "Whole SoundHeap destructor did not dispose its remaining real allocation");
    Check(static_cast<MEMiHeapHead*>(area.get())->signature == 0,
        "Whole SoundHeap destructor retained its genuine MEM registry owner");
    manager.UnregisterDisposeCallback(&disposal);
    Check(manager.mCallbackList.IsEmpty(), "Source manager retained the retired caller callback");
}
}
int main() {
    try {
        Check(mscharged::platform::NativeInterruptsEnabled(), "Actual caller mask was not initially enabled");
        FrameOwner();
        SoundOwner();
        Check(mscharged::platform::NativeInterruptsEnabled(), "Original owners changed the caller mask");
        std::printf("PASS %u whole original FrameHeap/SoundHeap/real MEM/OSMutex/dispose/lifetime checks; "
            "HBM_ASSERT=1; caller-owned storage; MEM SaveState/LoadState and player remain separate.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Original heap539: %s\n", error.what());
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

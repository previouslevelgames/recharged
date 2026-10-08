// ABI leaf only: caller-owned mappings, original read queue and cancellation
// forwarder. This does not issue an NL read or initialize an AX voice/device.
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

#include "Game/Audio/AudioSource.h"
#include "Game/Audio/AudioEffect.h"
#include "NL/nlDebugString.h"
#include "NL/nlFileGC.h"

using CancelCallback = void (*)(nlFile*, void*, unsigned int, nlFileAsyncParam, ReadAsyncCallback);
using QueueCallback = void (AudioBackend::*)(nlFile*, unsigned int, void*, unsigned int,
    ReadAsyncCallback, nlFileAsyncParam, AudioReadState*);
using StreamCallback = void (AudioReadState::*)(unsigned int, void*, unsigned int,
    ReadAsyncCallback, nlFileAsyncParam);
static_assert(std::is_same_v<decltype(&OnAudioReadCancelled), CancelCallback>);
static_assert(std::is_same_v<decltype(&nlCancelAsyncRead), bool (*)(AsyncEntry*, CancelCallback)>);
static_assert(std::is_same_v<decltype(&AudioBackend::QueueRead), QueueCallback>);
static_assert(std::is_same_v<decltype(&AudioReadState::QueueStreamRead), StreamCallback>);
static_assert(std::is_same_v<decltype(&OnAudioStreamReadComplete), ReadAsyncCallback>);
static_assert(std::is_same_v<decltype(&OnAudioStreamHeaderRead), ReadAsyncCallback>);
static_assert(std::is_same_v<decltype(&OnAudioStreamPrimeRead), ReadAsyncCallback>);
static_assert(std::is_same_v<decltype(&AllocateAudioEffectMemory), AXFXAllocHook>);
static_assert(std::is_same_v<decltype(&FreeAudioEffectMemory), AXFXFreeHook>);
static_assert(std::is_same_v<decltype(&AudioSampleSource::OnVoiceDropped), AXVoiceCallback>);
static_assert(std::is_same_v<decltype(&AudioStreamChannel::OnVoiceDropped), AXVoiceCallback>);
static_assert(sizeof(nlFileAsyncParam) == sizeof(void*));
static_assert(sizeof(AXVoiceContext) == sizeof(void*));
static_assert(sizeof(decltype(AudioReadRequest::m_UserParam)) == sizeof(void*));
static_assert(sizeof(MSCHARGED_DEBUG_STRING_WORD) == sizeof(void*));

namespace {
unsigned int checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

class Mapping
{
public:
    explicit Mapping(std::uintptr_t requested)
    {
#if defined(_WIN32)
        memory_ = VirtualAlloc(reinterpret_cast<void*>(requested), bytes_,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        memory_ = mmap(reinterpret_cast<void*>(requested), bytes_, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (memory_ == MAP_FAILED)
            memory_ = nullptr;
#endif
        if (memory_ != reinterpret_cast<void*>(requested))
        {
            Release();
            throw std::runtime_error("Required real high-address mapping is unavailable");
        }
    }
    ~Mapping() { Release(); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    void* At(std::size_t offset) const
    {
        return static_cast<unsigned char*>(memory_) + offset;
    }

private:
    void Release()
    {
        if (!memory_)
            return;
#if defined(_WIN32)
        VirtualFree(memory_, 0, MEM_RELEASE);
#else
        munmap(memory_, bytes_);
#endif
        memory_ = nullptr;
    }
    static constexpr std::size_t bytes_ = 65536;
    void* memory_ = nullptr;
};

struct Probe
{
    unsigned int identity;
    unsigned int calls;
    unsigned char payload[16];
};
Probe* expected;
unsigned int expected_size;
unsigned int callback_order;

void ObserveContext(nlFile* file, void* buffer, unsigned int size, nlFileAsyncParam context)
{
    Check(file == nullptr, "Caller supplied null file must be preserved");
    Check(context == reinterpret_cast<nlFileAsyncParam>(expected), "Full callback context must be preserved");
    // Check the full carrier before dereferencing; truncation must not turn this
    // into an invalid dereference or an alias of the other live caller owner.
    Probe* probe = reinterpret_cast<Probe*>(context);
    Check(buffer == probe->payload, "Caller buffer identity must be preserved");
    Check(size == expected_size, "Callback byte count must be preserved");
    Check(probe->identity == ++callback_order, "Callback must reach the independently owned context in order");
    ++probe->calls;
}

AudioReadRequest Request(Probe* probe, unsigned int size, unsigned int offset, bool cancelled)
{
    AudioReadRequest request{};
    request.m_Buffer = probe->payload;
    request.m_Callback = ObserveContext;
    request.m_UserParam = reinterpret_cast<nlFileAsyncParam>(probe);
    request.m_Offset = offset;
    request.m_Size = size;
    request.m_Cancel = cancelled;
    return request;
}

void ForwardHead(AudioReadQueue& queue, Probe* probe, unsigned int size, unsigned int offset, bool cancelled)
{
    AudioReadRequest* request = queue.GetHead();
    Check(request != nullptr, "Actual source queue must retain its request");
    Check(request->m_UserParam == reinterpret_cast<nlFileAsyncParam>(probe), "Actual queue stores full context");
    Check(request->m_Buffer == probe->payload, "Actual queue stores the original borrowed buffer");
    Check(request->m_Callback == ObserveContext, "Actual queue stores the exact callback");
    Check(request->m_Offset == offset, "Source offset remains an unchanged scalar");
    Check(request->m_Size == size, "Source size bitfield remains unchanged");
    Check(request->m_Cancel == cancelled, "Source cancellation bit remains unchanged");
    expected = probe;
    expected_size = size;
    // Genuine original inline forwarder, deliberately called as an ABI leaf.
    OnAudioReadCancelled(request->m_File, request->m_Buffer, request->m_Size,
        request->m_UserParam, request->m_Callback);
    queue.DeleteEntry(queue.RemoveStart());
}
} // namespace

int main()
{
    try
    {
        Mapping first(0x200000000ull);
        Mapping second(0x300000000ull);
        Probe* a = new (first.At(256)) Probe{1, 0, {}};
        Probe* b = new (second.At(256)) Probe{2, 0, {}};
        const auto a_word = reinterpret_cast<std::uintptr_t>(a);
        const auto b_word = reinterpret_cast<std::uintptr_t>(b);
        Check(a_word > UINT32_MAX && b_word > UINT32_MAX, "Both contexts must be real above-4-GiB mappings");
        Check((a_word & UINT32_MAX) == (b_word & UINT32_MAX), "Contexts must independently expose 32-bit aliasing");
        Check(a_word != b_word, "Contexts must remain distinct native owners");

        AudioReadQueue queue;
        Check(queue.GetHead() == nullptr, "Source queue starts empty");
        AudioReadRequest ra = Request(a, 11, 0x89abcdefu, false);
        AudioReadRequest rb = Request(b, 13, 0xfedcba98u, true);
        queue.AddEnd(ra);
        queue.AddEnd(rb);
        ra.m_UserParam = rb.m_UserParam = 0;
        ForwardHead(queue, a, 11, 0x89abcdefu, false);
        ForwardHead(queue, b, 13, 0xfedcba98u, true);
        Check(queue.GetHead() == nullptr && queue.m_Tail == nullptr, "Original queue removal/recycle empties both ends");
        Check(a->calls == 1 && b->calls == 1, "Each original forwarding call targets only its live context");
        a->identity = 3;
        queue.AddEnd(Request(a, 7, 23, true));
        ForwardHead(queue, a, 7, 23, true);
        Check(a->calls == 2 && b->calls == 1, "Recycled source node retains the next full context");
        Check(queue.GetHead() == nullptr, "Source queue retires the reused node");

        // Descriptor ABI storage only, no voice allocation or callback execution.
        AXVPB voice{};
        voice.userContext = static_cast<AXVoiceContext>(a_word);
        Check(reinterpret_cast<void*>(voice.userContext) == a, "Actual AXVPB preserves first full native context");
        voice.userContext = static_cast<AXVoiceContext>(b_word);
        Check(reinterpret_cast<void*>(voice.userContext) == b, "Actual AXVPB distinguishes high-address contexts");

        Check(std::strcmp(nlLookupDebugString(nullptr, a_word), "unknown") == 0,
            "Whole original diagnostic provider retains its unknown result");
        Check(std::strcmp(nlLookupDebugString(nullptr, b_word), "unknown") == 0,
            "Diagnostic pointer carrier remains full-width without changing lookup logic");

        using mscharged::platform::NativeAudioAllocationSize;
        Check(NativeAudioAllocationSize(0) == 0, "Zero allocation request is unchanged");
        Check(NativeAudioAllocationSize(16384) == 16384, "Representable effect request is unchanged");
        Check(NativeAudioAllocationSize(std::numeric_limits<unsigned long>::max())
                == std::numeric_limits<unsigned long>::max(), "Source size carrier maximum is unchanged");
        if constexpr (sizeof(std::size_t) > sizeof(unsigned long))
        {
            for (std::size_t size : {static_cast<std::size_t>(UINT32_MAX) + 1,
                                    std::numeric_limits<std::size_t>::max()})
            {
                bool rejected = false;
                try { (void)NativeAudioAllocationSize(size); }
                catch (const std::length_error&) { rejected = true; }
                Check(rejected, "Oversize effect request must reject before source-size narrowing");
            }
        }
        a->~Probe();
        b->~Probe();
        std::cout << "Audio context ABI PASS " << checks << " checks; contexts "
                  << std::hex << a_word << "," << b_word << std::dec
                  << "; pointer=" << sizeof(void*) << " ulong=" << sizeof(unsigned long)
                  << "; queue/cancel forwarder only, no NL/DVD or AX execution\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Audio context ABI failure: " << error.what() << '\n';
        return 1;
    }
}

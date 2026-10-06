#include "platform/ai.h"
#include "platform/interrupts.h"
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
std::atomic<unsigned> checks{};
unsigned produced{};
std::thread::id owner;
alignas(32) std::array<s16,192> buffers[2];
unsigned next_buffer{};
std::mutex recorded_mutex;
std::vector<std::array<s16,192>> recorded;
void Check(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
void Producer() {
    Check(std::this_thread::get_id()==owner,"DMA callback left its source owner");
    Check(!NativeInterruptsEnabled(),"DMA callback arrived without source interrupt mask");
    next_buffer^=1;
    auto* buffer=buffers[next_buffer].data();
    // Exact source mode0 ordering: select the NEXT buffer before writing it.
    // The live transfer must already retain the previously latched buffer.
    const auto active=GetNativeAIClockStatus().active_source_address;
    AIInitDMA(reinterpret_cast<std::uintptr_t>(buffer),sizeof(buffers[0]));
    Check(GetNativeAIClockStatus().active_source_address==active,
          "programming next DMA replaced the active latched transfer");
    for(unsigned n=0;n<96;++n) {
        buffer[n*2]=s16(produced*96+n+1);
        buffer[n*2+1]=s16(-(int(produced*96+n)+1));
    }
    ++produced;
    Check(!ServiceNativeAI(),"active source callback was reentered");
}
void Trial(u32 rate,unsigned frequency) {
    AISetDSPSampleRate(rate);
    for(auto& buffer:buffers)buffer.fill(0);
    next_buffer=produced=0;
    {std::lock_guard lock(recorded_mutex);recorded.clear();}
    AIRegisterDMACallback(Producer);
    AIInitDMA(reinterpret_cast<std::uintptr_t>(buffers[0].data()),sizeof(buffers[0]));
    const auto before=GetNativeAIClockStatus();
    const auto callbacks_before=GetNativeAIStatus().dispatched_callbacks;
    AIStartDMA();
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(240);
    while(std::chrono::steady_clock::now()<end) {
        ServiceNativeAI();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    ServiceNativeAI();
    NativeAIClockStatus clock;
    NativeAIStatus status;
    {
        NativeInterruptGuard snapshot;
        ServiceNativeAI();
        clock=GetNativeAIClockStatus();
        status=GetNativeAIStatus();
        AIStopDMA();
    }
    // Independent nominal clock arithmetic, not SDL device pull granularity.
    const auto expected_cells=(clock.clock_elapsed_ns/1000000000ull*frequency+
        clock.clock_elapsed_ns%1000000000ull*frequency/1000000000ull)/8;
    Check(clock.epoch_transferred_cells==expected_cells,
          "DMA transferred more/fewer cells than its hardware sample clock");
    const auto transfers=clock.transferred_cells-before.transferred_cells;
    Check(transfers==expected_cells,"clock epoch transferred cells drifted");
    Check(transfers*8 >= frequency*235/1000 && transfers*8 <= frequency*270/1000,
          "nominal hardware rate differs from elapsed test interval");
    const auto edges=clock.latch_edges-before.latch_edges;
    Check(edges==1+transfers/12,"96-frame hardware latch cadence changed");
    const auto coalesced=clock.coalesced_edges-before.coalesced_edges;
    Check(status.dispatched_callbacks-callbacks_before+coalesced==edges,
          "latched hardware cause count differs from callbacks plus masked/coalesced causes");
    unsigned nonzero=0,repeated=0;
    int previous=-1;
    {
        std::lock_guard lock(recorded_mutex);
        Check(recorded.size()==transfers/12,"SDL received an invented or repeated DMA packet");
        for(const auto& buffer:recorded) {
            if(buffer[0]==0) {Check(nonzero==0,"source PCM sequence became silent mid-stream");continue;}
            const int block=(int(buffer[0])-1)/96;
            Check(int(buffer[0])==block*96+1,"DMA lost its authored source block boundary");
            Check(block==previous+1 || block==previous,
                  "hardware DMA skipped or reordered an authored source buffer");
            if(block==previous)++repeated;
            previous=block;
            for(unsigned n=0;n<96;++n) {
                const int reference=block*96+int(n)+1;
                Check(buffer[n*2]==reference && buffer[n*2+1]==-reference,
                      "active DMA lost source sample order or repeated/skipped a block");
            }
            ++nonzero;
        }
    }
    Check(nonzero>20,"timed device fixture did not deliver enough genuine authored PCM");
    Check(repeated<=coalesced,"SDL demand repeated a source block without a coalesced hardware cause");
    Check(GetNativeAIStatus().retained_blocks==0,"hardware stop retained queued PCM ownership");
    std::cout<<"{\"frequency\":"<<frequency<<",\"clock_ns\":"<<clock.clock_elapsed_ns
             <<",\"cells\":"<<transfers<<",\"latch_edges\":"<<edges<<",\"callbacks\":"
             <<(status.dispatched_callbacks-callbacks_before)<<",\"nonzero_blocks\":"<<nonzero
             <<",\"coalesced_edges\":"<<coalesced<<",\"repeated_source_blocks\":"<<repeated
             <<",\"maximum_service_gap_ns\":"<<clock.maximum_service_gap_ns<<"}\n";
}
}
extern "C" bool __real_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream*,const void*,int,SDL_AudioStreamDataCompleteCallback,void*);
extern "C" bool __wrap_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream* stream,const void* bytes,int size,SDL_AudioStreamDataCompleteCallback done,void* context) {
    Check(size==384,"96-frame source DMA length changed at SDL boundary");
    std::array<s16,192> input;
    std::memcpy(input.data(),bytes,size);
    {std::lock_guard lock(recorded_mutex);recorded.push_back(input);}
    return __real_SDL_PutAudioStreamDataNoCopy(stream,bytes,size,done,context);
}
int main() {
    try {
        owner=std::this_thread::get_id();AIInit(nullptr);
        Trial(AI_SAMPLERATE_32KHZ,32000);
        Trial(AI_SAMPLERATE_48KHZ,48000);
        AIReset();SDL_Quit();
        std::cout<<"native AI clock/latched-source/ordered-PCM checks="<<checks.load()<<'\n';return 0;
    }catch(const std::exception& error) {
        std::cerr<<"native AI timing: "<<error.what()<<'\n';return 1;
    }
}

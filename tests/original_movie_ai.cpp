#include "RVL_SDK/thp/THPSimple.h"
#include "platform/ai.h"
#include "platform/interrupts.h"
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
unsigned checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void Await(Predicate predicate, const char* message) {
    const auto limit = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if(std::chrono::steady_clock::now()>=limit) throw std::runtime_error(message);
        std::this_thread::sleep_for(1ms);
    }
    ++checks;
}
struct Cleanup {
    ~Cleanup() { try { ShutdownNativeAI(); } catch(...) {} SDL_Quit(); }
};
} // namespace

int main(int argc, char** argv) {
    Cleanup cleanup;
    try {
        if(argc!=2) throw std::invalid_argument("original_movie_ai_tests ZERO384_FNV1A64");
        const auto zero_hash=std::stoull(argv[1]);
        AIInit(nullptr); // Actual SDK device setup, normally below original AXInit.
        Check(AICheckInit(), "original THP qualifier lacks real AI device");
        Check(THPSimpleInit(1)==0, "source accepted mode1 without its genuine predecessor");
        Check(AIRegisterDMACallback(nullptr)==nullptr && !AIGetDMAEnableFlag(),
              "failed source mode1 registration left synthetic readiness");

        for(unsigned repetition=0; repetition<2; ++repetition) {
            Check(THPSimpleInit(0)==1, "whole original standalone THPSimpleInit failed");
            auto first=GetNativeAIStatus();
            Check(first.initialized && first.running && first.dma_bytes==384 &&
                  first.source_address>0xffffffffull,
                  "source SoundBuffer DMA initialization was not executed");
            const auto before=first.dispatched_callbacks;
            Check(THPSimpleGetTotalFrame()==0 && THPSimpleCalcNeedMemory()==0,
                  "source unopened query decisions changed");
            THPVideoInfo video;
            std::memset(&video,0x7b,sizeof(video));
            unsigned char expected[sizeof(video)];
            std::memset(expected,0x7b,sizeof(expected));
            Check(THPSimpleGetVideoInfo(&video)==0 && std::memcmp(&video,expected,sizeof(video))==0,
                  "source unopened video query modified its output");
            const auto mask=OSDisableInterrupts();
            Await([&]{return GetNativeAIStatus().interrupt_pending;}, "real source sound DMA did not latch its hardware edge");
            Check(!ServiceNativeAI(), "original THP callback ran while masked");
            OSRestoreInterrupts(mask);
            Await([&]{return ServiceNativeAI();}, "original source mix callback did not run");
            auto second=GetNativeAIStatus();
            Check(second.source_address!=first.source_address && second.dma_bytes==384 &&
                  second.dispatched_callbacks==before+1 && NativeInterruptsEnabled(),
                  "source first double-buffer toggle/interrupt restoration changed");
            Await([&]{return ServiceNativeAI();}, "source second mix callback did not run");
            auto third=GetNativeAIStatus();
            Check(third.source_address==first.source_address && third.dispatched_callbacks==before+2,
                  "source second double-buffer toggle changed");
            Await([&]{return GetNativeAIStatus().last_input_hash==zero_hash;},
                  "original unopened source mixer did not submit zero PCM");
            AIStopDMA(); // Native terminal drain precedes source/static-buffer release.
            Check(GetNativeAIStatus().retained_blocks==0 && !ServiceNativeAI(),
                  "source DMA packets survived stop");
            const auto source_callback=AIRegisterDMACallback(nullptr);
            Check(source_callback!=nullptr, "source callback was never registered");
            AIRegisterDMACallback(source_callback);
            THPSimpleQuit();
            Check(AIRegisterDMACallback(nullptr)==source_callback,
                  "retail Quit null-predecessor callback quirk was changed");
        }
        AIReset();
        Check(!AICheckInit() && GetNativeAIStatus().retained_blocks==0,
              "standalone source qualifier leaked native DMA device/buffers");
        std::cout << "original THPSimple AI checks=" << checks
                  << " source mode0 init/mix/double-buffer/quit pass; mode1 predecessor absent\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "original THPSimple AI failure after " << checks << ": " << error.what() << '\n';
        return 1;
    }
}

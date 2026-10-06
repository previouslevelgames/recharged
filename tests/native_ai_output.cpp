#include "platform/ai.h"
#include <dolphin/ai.h>
#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
alignas(32) std::array<s16,192> samples;
unsigned checks{}, callbacks{};
std::uint64_t first_callback_ns{};
std::atomic<std::uint64_t> frames{}, zero_frames{}, incorrect_frames{};
std::uint64_t trial_start_ns{};
void Check(bool condition,const char* why) {
    ++checks;
    if(!condition)throw std::runtime_error(why);
}
void Source() {
    if(!first_callback_ns)first_callback_ns=SDL_GetTicksNS();
    AIInitDMA(reinterpret_cast<std::uintptr_t>(samples.data()),sizeof(samples));
    ++callbacks;
}
void SDLCALL Postmix(void*,const SDL_AudioSpec* spec,float* pcm,int size) {
    // Startup resampler filter and intentional device lead precede this
    // independent steady constant-PCM oracle. Silence is an error here.
    if(SDL_GetTicksNS()-trial_start_ns<150000000ull)return;
    if(spec->format!=SDL_AUDIO_F32 || spec->channels!=2){++incorrect_frames;return;}
    for(int n=0;n+1<size/int(sizeof(float));n+=2) {
        ++frames;
        if(pcm[n]==0.0f && pcm[n+1]==0.0f){++zero_frames;continue;}
        if(std::abs(pcm[n]-(-8765.0f/32768.0f))>0.0002f ||
           std::abs(pcm[n+1]-(1234.0f/32768.0f))>0.0002f)++incorrect_frames;
    }
}
}

int main() {
    try {
        static_assert(sizeof(NativeAIOutputStatus)==5*sizeof(std::uint64_t));
        for(unsigned n=0;n<96;++n){samples[2*n]=1234;samples[2*n+1]=-8765;}
        AIInit(nullptr);
        AIRegisterDMACallback(Source);
        AIInitDMA(reinterpret_cast<std::uintptr_t>(samples.data()),sizeof(samples));
        const auto device=GetNativeAIStatus();
        Check(SDL_SetAudioPostmixCallback(device.device_id,Postmix,nullptr),"actual postmix recorder installation");

        AIStartDMA();
        Check(AIGetDMAEnableFlag() && GetNativeAIOutputStatus().device_start_ns==0,
              "source DMA did not start before the independent host output lead");
        AIStopDMA();
        Check(GetNativeAIStatus().retained_blocks==0 && GetNativeAIOutputStatus().device_start_ns==0,
              "early stop retained/transferred PCM or resumed the device");

        trial_start_ns=SDL_GetTicksNS();
        AIStartDMA();
        const auto end=std::chrono::steady_clock::now()+700ms;
        while(std::chrono::steady_clock::now()<end) {
            ServiceNativeAI();
            std::this_thread::sleep_for(200us);
        }
        ServiceNativeAI();
        const auto output=GetNativeAIOutputStatus();
        const auto clock=GetNativeAIClockStatus();
        const auto status=GetNativeAIStatus();
        AIStopDMA();
        Check(SDL_SetAudioPostmixCallback(device.device_id,nullptr,nullptr),"postmix recorder removal");
        Check(output.dma_start_ns && output.device_start_ns>output.dma_start_ns &&
              first_callback_ns<output.device_start_ns,"host lead delayed the actual source callback clock");
        Check(output.device_start_ns-output.dma_start_ns<100000000ull,
              "96-frame PCM device startup exceeded the bounded fixture latency");
        Check(frames>=512 && zero_frames==0 && incorrect_frames==0,
              "steady genuine PCM reached postmix with silence/incorrect sample or channel values");
        Check(clock.epoch_transferred_cells*8>=32000*690/1000 &&
              clock.epoch_transferred_cells*8<32000*740/1000,
              "host lead changed the source nominal32kHz transfer clock");
        Check(callbacks>=225 && status.consumed_blocks && GetNativeAIStatus().retained_blocks==0,
              "host lead suppressed source callbacks or stop retained real PCM");
        std::cout<<"{\"checks\":"<<checks<<",\"callbacks\":"<<callbacks
                 <<",\"startup_ns\":"<<(output.device_start_ns-output.dma_start_ns)
                 <<",\"required_output_frames\":"<<output.required_output_frames
                 <<",\"ready_output_frames_at_start\":"<<output.ready_output_frames_at_start
                 <<",\"transferred_input_frames_at_start\":"<<output.transferred_input_frames_at_start
                 <<",\"postmix_frames_after150ms\":"<<frames.load()
                 <<",\"zero_postmix_frames_after150ms\":"<<zero_frames.load()
                 <<",\"incorrect_postmix_frames_after150ms\":"<<incorrect_frames.load()<<"}\n";
        ShutdownNativeAI();SDL_Quit();return 0;
    }catch(const std::exception& e) {
        try{ShutdownNativeAI();}catch(...){}
        SDL_Quit();std::cerr<<"native AI output: "<<e.what()<<'\n';return 1;
    }
}

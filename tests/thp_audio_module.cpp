#include "thp_audio_bridge.h"
#include "RVL_SDK/thp/THPSimple.h"
#include "NL/nlMemory.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "platform/ai.h"
#include "platform/game_allocation_ownership.h"
#include <dolphin/ai.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>

#ifndef MSCHARGED_DIAGNOSTIC_THP_AUDIO_ONLY
#error This qualifier omits only original video requests under its named gate
#endif
#if defined(MSCHARGED_DIAGNOSTIC_EVENTS) || defined(MSCHARGED_DIAGNOSTIC_VECTORS)
#error Original movie qualifier cannot import game replicas
#endif
extern "C" void ChargedAudioCaptureStop();
namespace {
unsigned checks;
void Check(bool value,const char* reason) {
    ++checks; if(!value) throw std::runtime_error(reason);
}
}
extern "C" __attribute__((visibility("default")))
unsigned charged_thp_audio_qualify(const char* movie,unsigned milliseconds,ChargedMovieAudioResult* result) {
    using namespace mscharged::platform;
    checks=0;
    Check(result && movie && milliseconds>=100,"Movie diagnostic arguments are invalid");
    *result={};
    Check(AICheckInit(),"Real host AI device was not initialized");
    Check(THPSimpleInit(1)==0,"Original mode1 accepted an absent AX predecessor");
    Check(THPSimpleInit(0)==1,"Original standalone THP source initialization failed");
    nlInitFileSystem();
    Check(THPSimpleOpen(movie)==1,"Original source could not open the owned movie");
    THPVideoInfo video{};
    Check(THPSimpleGetVideoInfo(&video)==1,"Original owned movie info was unavailable");
    result->width=video.xSize; result->height=video.ySize;
    result->frames=THPSimpleGetTotalFrame();
    result->work_bytes=THPSimpleCalcNeedMemory();
    Check(result->frames && result->work_bytes,"Original movie work request is empty");
    auto* work=static_cast<unsigned char*>(nlMalloc(result->work_bytes,32,false));
    Check(work && FindGameAllocationOwner(work)==&StandardAllocator,
          "Original movie work request did not use its source game arena");
    Check(THPSimpleSetBuffer(work)==1,"Original movie ring/work buffer registration failed");
    Check(THPSimplePreLoad(0)==1,"Original NL frame preload failed");
    // Supported standalone diagnostic entry. No normal MovieInit(1), AX, task
    // scheduling, GPU texture requests or source startup are claimed here.
    THPSimpleAudioStart();
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<end) {
        nlServiceFileSystem();
        const auto state=THPSimpleDecode(0);
        if(state==0) ++result->decoded_frames;
        else if(state==2) ++result->waiting_read;
        else if(state==3) ++result->blocked_ring;
        else Check(false,"Original frame/audio decoder returned a genuine failure");
        ServiceNativeAI(); // Host hardware diagnostic owner poll, no source callback replacement.
        SDL_Delay(1);
    }
    const auto audio=GetNativeAIStatus();
    result->callbacks=audio.dispatched_callbacks;
    result->consumed_blocks=audio.consumed_blocks;
    result->submitted_blocks=audio.submitted_blocks;
    result->input_rate=audio.input_frequency;
    Check(result->decoded_frames>0 && audio.dispatched_callbacks>0 && audio.consumed_blocks>0,
          "Owned source audio was not decoded/mixed/consumed by the actual device");
    THPSimpleAudioStop();
    Check(THPSimpleLoadStop()==1,"Original movie load cancellation/drain failed");
    Check(THPSimpleClose()==1,"Original movie close failed");
    ChargedAudioCaptureStop(); // Stop only the test recorder before cancellation callbacks.
    AIStopDMA(); // Drain hardware before the original work/static backing can disappear.
    THPSimpleQuit();
    Check(AIRegisterDMACallback(nullptr)!=nullptr,
          "Original null-predecessor Quit callback quirk unexpectedly changed");
    nlFree(work);
    Check(!FindGameAllocationOwner(work),"Movie game arena work remained owned after free");
    nlShutdownFileSystem();
    result->checks=checks;
    return 1;
}

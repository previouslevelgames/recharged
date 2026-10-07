#include "ax_thp_mode1_bridge.h"
#include "RVL_SDK/thp/THPSimple.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "platform/game_allocation_ownership.h"
#include <stdexcept>

#ifndef MSCHARGED_DIAGNOSTIC_THP_AUDIO_ONLY
#error This bounded mode1 qualifier explicitly holds source video texture requests
#endif
namespace {
unsigned char* work;
ChargedAXTHPResult result;
void Check(bool value,const char* text) {
    ++result.checks;if(!value)throw std::runtime_error(text);
}
}
extern "C" __attribute__((visibility("default")))
void charged_thp_mode1_open(const char* path) {
    using namespace mscharged::platform;
    Check(!work,"previous originalTHP work is still live");
    Check(THPSimpleInit(1)==1,"originalTHPSimple mode1 has no real AX predecessor");
    nlInitFileSystem();
    Check(THPSimpleOpen(path)==1,"originalTHP source could not open owned movie");
    THPVideoInfo video{};
    Check(THPSimpleGetVideoInfo(&video)==1,"originalTHP component metadata missing");
    result.width=video.xSize;result.height=video.ySize;
    result.frames=THPSimpleGetTotalFrame();result.work_bytes=THPSimpleCalcNeedMemory();
    Check(result.work_bytes&&result.frames,"originalTHP work request is empty");
    work=static_cast<unsigned char*>(nlMalloc(result.work_bytes,32,false));
    Check(work&&FindGameAllocationOwner(work)==&StandardAllocator,"sourceTHP allocation ownership invalid");
    Check(THPSimpleSetBuffer(work)==1,"originalTHP ring buffer request failed");
    Check(THPSimplePreLoad(0)==1,"originalTHP NL preload failed");
    THPSimpleAudioStart();
}
extern "C" __attribute__((visibility("default")))
void charged_thp_mode1_decode() {
    nlServiceFileSystem();
    const auto state=THPSimpleDecode(0);
    if(state==0)++result.decoded_frames;
    else if(state==2)++result.read_wait;
    else if(state==3)++result.ring_full;
    else Check(false,"actualTHP decoder returned a source failure");
}
extern "C" __attribute__((visibility("default")))
ChargedAXTHPResult charged_thp_mode1_close() {
    using namespace mscharged::platform;
    THPSimpleAudioStop();
    Check(THPSimpleLoadStop()==1,"actualTHP pending NL read did not drain");
    Check(THPSimpleClose()==1,"actualTHP source close failed");
    THPSimpleQuit(); // Source restores its genuine AX predecessor.
    auto* prior=work;nlFree(work);work=nullptr;
    Check(!FindGameAllocationOwner(prior),"originalTHP work remained owned after free");
    nlShutdownFileSystem();
    return result;
}

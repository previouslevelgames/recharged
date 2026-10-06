#include "thp_audio_bridge.h"
#include "platform/ai.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <dolphin/ai.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define CHARGED_SELECTED_SAN 1
#endif
#endif

namespace {
struct PacketObserver {
    SDL_AudioStreamDataCompleteCallback source;
    void* context;
    int size;
    unsigned char* Bytes() { return reinterpret_cast<unsigned char*>(this+1); }
};
std::mutex capture_mutex;
std::atomic<bool> capture_enabled{true};
std::uint64_t completed{}, nonzero{}, samples{}, energy{}, peak{};
FILE* capture_file{};
void SDLCALL ObserveComplete(void* context,const void* bytes,int size) {
    auto* observer=static_cast<PacketObserver*>(context);
    if(capture_enabled.load()) {
        std::lock_guard lock(capture_mutex);
        ++completed;
        bool audible=false;
        for(int i=0;i<observer->size/2;++i) {
            std::int16_t sample;std::memcpy(&sample,observer->Bytes()+i*2,2);
            const auto magnitude=static_cast<unsigned>(sample<0?-int(sample):sample);
            audible|=sample!=0; peak=std::max(peak,std::uint64_t(magnitude));
            energy+=std::uint64_t(int(sample)*int(sample));++samples;
        }
        nonzero+=audible;
        if(capture_file) std::fwrite(observer->Bytes(),1,observer->size,capture_file);
    }
    observer->source(observer->context,bytes,size); // Exact real SDK completion/user data, unchanged.
    std::free(observer);
}
void Check(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }
}
extern "C" void ChargedAudioCaptureStop() { capture_enabled=false; }
extern "C" bool __real_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream*,const void*,int,SDL_AudioStreamDataCompleteCallback,void*);
extern "C" bool __wrap_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream* stream,const void* bytes,int size,SDL_AudioStreamDataCompleteCallback callback,void* context) {
    auto* observer=static_cast<PacketObserver*>(std::malloc(sizeof(PacketObserver)+size));
    if(!observer)throw std::bad_alloc();
    observer->source=callback;observer->context=context;observer->size=size;
    std::memcpy(observer->Bytes(),bytes,size);
    const bool okay=__real_SDL_PutAudioStreamDataNoCopy(stream,bytes,size,ObserveComplete,observer);
    if(!okay)std::free(observer);
    return okay;
}
int main(int argc,char** argv) {
    try {
        Check(argc==6,"usage: host MODULE DISC MOVIE MILLISECONDS OUTPUT_PCM");
        auto* base=SDL_GetBasePath();Check(base,"Host resource base unavailable");
        const std::string resources(base);
        auto path=(std::filesystem::path(resources)/"movie-audio-data").string();
        std::filesystem::create_directories(path);
        AuroraConfig config{};
        config.appName="Original THP source audio diagnostic";
        config.userPath=config.cachePath=path.c_str();config.resourcesPath=resources.c_str();
        config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
        config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;
        config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64u*1024u*1024u;
        Check(aurora_initialize(argc,argv,&config).window,"Real SDK initialization failed");
        OSInit();
        Check(aurora_dvd_open(argv[2]),"Cannot mount owned game data");
        AIInit(nullptr); // Genuine platform device prerequisite for supported standalone mode0.
        std::printf("Real SDL audio driver: %s\n",SDL_GetCurrentAudioDriver());
        capture_file=std::fopen(argv[5],"wb");Check(capture_file,"Cannot open private PCM capture");
        auto* module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
        if(!module)throw std::runtime_error(dlerror());
        auto qualify=reinterpret_cast<unsigned(*)(const char*,unsigned,ChargedMovieAudioResult*)>(dlsym(module,"charged_thp_audio_qualify"));
        Check(qualify,"Original movie diagnostic entry was not exported");
        ChargedMovieAudioResult result{};
        Check(qualify(argv[3],std::stoul(argv[4]),&result)==1,"Original movie audio diagnostic failed");
        capture_enabled=false;
        AIReset();
        std::fclose(capture_file);capture_file=nullptr;
        Check(nonzero>0 && peak>0 && completed>0,"Actual consumed PCM was entirely silent");
        std::printf("{\"checks\":%u,\"decoded_frames\":%u,\"ring_full\":%u,\"read_wait\":%u,\"width\":%u,\"height\":%u,\"frames\":%u,\"work_bytes\":%u,\"input_rate\":%u,\"callbacks\":%llu,\"source_consumed_blocks\":%llu,\"observed_completed_blocks\":%llu,\"nonzero_completed_blocks\":%llu,\"samples\":%llu,\"squared_energy\":%llu,\"peak\":%llu}\n",
            result.checks,result.decoded_frames,result.blocked_ring,result.waiting_read,result.width,result.height,result.frames,result.work_bytes,result.input_rate,
            static_cast<unsigned long long>(result.callbacks),static_cast<unsigned long long>(result.consumed_blocks),static_cast<unsigned long long>(completed),
            static_cast<unsigned long long>(nonzero),static_cast<unsigned long long>(samples),static_cast<unsigned long long>(energy),static_cast<unsigned long long>(peak));
        aurora_dvd_close();aurora_shutdown();
        std::puts("Terminal audio-only original THP mode0 diagnostic; normal Game/AX mode1, movie video and static/CRT teardown are not qualified.");
        std::fflush(nullptr);
#ifdef CHARGED_SELECTED_SAN
        if(__lsan_do_recoverable_leak_check())std::_Exit(1);
#endif
        std::_Exit(0);
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Original audio diagnostic: %s\n",error.what());std::fflush(nullptr);std::_Exit(1);
    }
}

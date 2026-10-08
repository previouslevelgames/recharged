#include "platform/graphics_stats.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <aurora/dvd.h>
#include <aurora/video.h>
#include <aurora/hardware.h>
#include "platform/interrupt_controller.h"
#include "platform/system.h"
#include "platform/video_device.h"
#include "platform/ai.h"
#include "credits_movie_hardware.h"
#include <dolphin/os.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
std::atomic_uint errors;
void Log(AuroraLogLevel level,const char* module,const char* message,unsigned n) {
    if(level>=LOG_ERROR)++errors;
    std::fprintf(stderr,"[%s] %.*s\n",module,int(n),message);
}
void Check(bool v,const char* m) { if(!v)throw std::runtime_error(m); }
struct Snapshot : std::enable_shared_from_this<Snapshot> {
    aurora::gfx::ResolvedTargets target;
    wgpu::Buffer buffer;
    unsigned stride;std::atomic_int done;
    std::vector<unsigned char> rgb;
    static void Copy(const aurora::gfx::EncoderTaskContext& ctx,const wgpu::CommandEncoder& cmd,const void*,std::size_t,void* p) {
        auto& self=*static_cast<Snapshot*>(p);
        const wgpu::BufferDescriptor d{.usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=std::uint64_t(self.stride)*self.target.height};
        self.buffer=ctx.device.CreateBuffer(&d);
        const wgpu::TexelCopyTextureInfo src{.texture=self.target.colorTexture};
        const wgpu::TexelCopyBufferInfo dst{.layout={.offset=0,.bytesPerRow=self.stride,.rowsPerImage=self.target.height},.buffer=self.buffer};
        const wgpu::Extent3D extent{self.target.width,self.target.height,1};
        cmd.CopyTextureToBuffer(&src,&dst,&extent);
    }
    static void Map(const aurora::gfx::EncoderTaskCompletionContext&,const void*,std::size_t,void* p) {
        auto self=static_cast<Snapshot*>(p)->shared_from_this();
        const auto n=std::uint64_t(self->stride)*self->target.height;
        self->buffer.MapAsync(wgpu::MapMode::Read,0,n,wgpu::CallbackMode::AllowSpontaneous,[self,n](wgpu::MapAsyncStatus status,wgpu::StringView) {
            if(status!=wgpu::MapAsyncStatus::Success){self->done=-1;return;}
            auto* raw=static_cast<const unsigned char*>(self->buffer.GetConstMappedRange(0,n));
            const bool bgra=self->target.colorFormat==wgpu::TextureFormat::BGRA8Unorm;
            for(unsigned y=0;y<self->target.height;++y)for(unsigned x=0;x<self->target.width;++x) {
                auto* out=self->rgb.data()+3*(std::size_t(y)*self->target.width+x);
                const auto* in=raw+std::size_t(y)*self->stride+4*x;
                out[0]=in[bgra?2:0];out[1]=in[1];out[2]=in[bgra?0:2];
            }
            self->buffer.Unmap();self->done=1;
        });
    }
};
void EndWithSnapshot(const std::filesystem::path& output) {
    auto s=std::make_shared<Snapshot>();
    Check(aurora::gfx::resolve_pass({},s->target),"Cannot resolve actual original Credits EFB");
    Check(s->target.colorFormat==wgpu::TextureFormat::BGRA8Unorm || s->target.colorFormat==wgpu::TextureFormat::RGBA8Unorm,"Unsupported snapshot color format");
    s->stride=(s->target.width*4+255)&~255u;
    s->rgb.resize(std::size_t(s->target.width)*s->target.height*3);
    const aurora::gfx::EncoderTaskDescriptor d{"Original Credits diagnostic snapshot",Snapshot::Copy,s.get(),Snapshot::Map};
    const auto task=aurora::gfx::register_encoder_task_type(d);
    const bool queued=aurora::gfx::push_encoder_task(task,nullptr,0);
    aurora_end_frame();aurora::gfx::synchronize();aurora::gfx::unregister_encoder_task_type(task);
    Check(queued,"Actual EFB snapshot task rejected");
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!s->done && std::chrono::steady_clock::now()<end){aurora::gfx::device().Tick();SDL_Delay(1);}
    Check(s->done==1,"Original Credits GPU readback did not complete");
    auto* f=std::fopen(output.c_str(),"wb");Check(f,"Cannot create Credits snapshot");
    std::fprintf(f,"P6\n%u %u\n255\n",s->target.width,s->target.height);
    const auto written=std::fwrite(s->rgb.data(),1,s->rgb.size(),f);const auto closed=std::fclose(f);
    Check(written==s->rgb.size() && !closed,"Credits snapshot write failed");
    unsigned lit=0;for(std::size_t i=0;i<s->rgb.size();i+=3)if(s->rgb[i]||s->rgb[i+1]||s->rgb[i+2])++lit;
    Check(lit != 0, "Original Credits actual EFB is entirely black; visibility remains unqualified");
    std::printf("Actual Credits EFB %ux%u, nonblack pixels%u; saved %s.\n",s->target.width,s->target.height,lit,output.c_str());
}
}
int main(int argc,char** argv) {
    // Source scene/arena owners remain live until this bounded diagnostic
    // terminates. This does not qualify original manager or CRT teardown.
    try {
        bool interactive=false;
        std::filesystem::path disc;
        for(int i=1;i<argc;++i) {
            const std::string argument=argv[i];
            if(argument=="--help") {
                std::puts("Usage: mscharged-original-credits-check --disc FILE [--window]\n"
                          "Render the original Credits scene from your own ISO/RVZ.\n"
                          "--window keeps the window open until you close it.\n"
                          "Runs original scene updates and frontend rendering each frame.\n"
                          "Includes original Credits THP video/audio in named mode0 diagnostic.\n"
                          "Full main/task loop, AX predecessor and source VI scanout are omitted.");
                return 0;
            }
            if(argument=="--window") interactive=true;
            else if(argument=="--disc" && i+1<argc) disc=argv[++i];
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        Check(!disc.empty(),"Supply your game ISO/RVZ with --disc FILE");
        Check(std::filesystem::is_regular_file(disc),"Disc image does not exist");
        const auto base=std::filesystem::path(SDL_GetBasePath());
        const auto modulePath=base/MSCHARGED_CREDITS_MODULE_FILENAME;
        Check(std::filesystem::is_regular_file(modulePath),"Original Credits source module is missing beside the executable");
        const auto directory=base/"original-credits-data";
        std::filesystem::create_directories(directory);const auto path=directory.string();
        std::fprintf(stderr,"Original Credits diagnostic data: %s\n",path.c_str());
        AuroraConfig config{};config.appName="Mario Strikers Charged | original Credits diagnostic";
        config.userPath=config.cachePath=path.c_str();config.resourcesPath=SDL_GetBasePath();
        config.desiredBackend=BACKEND_VULKAN;config.windowWidth=640;config.windowHeight=480;
        config.windowPosX=config.windowPosY=-1;config.vsync=true;config.enableBackendValidation=true;
        config.logLevel=LOG_INFO;config.logCallback=Log;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        const auto info=aurora_initialize(argc,argv,&config);
        Check(info.window && info.backend==BACKEND_VULKAN,"Original scene gate requires real Vulkan");
        OSInit();
        mscharged::platform::InitializeNativeInterruptController();
        mscharged::ConfigureNativeSystemSettings({1,0,0,0,1});
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT,false);
        mscharged::diagnostic::InitializeCreditsMovieHardware();
        Check(aurora_dvd_open(disc.c_str()),"Game data partition could not be opened");
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        auto* module=dlopen(modulePath.c_str(),RTLD_NOW|RTLD_LOCAL);if(!module)throw std::runtime_error(dlerror());
        auto qualify=reinterpret_cast<unsigned(*)()>(dlsym(module,"charged_font_qualify"));
        auto draw=reinterpret_cast<void(*)(float)>(dlsym(module,"charged_scene_update_and_render_frame"));
        auto stopMovie=reinterpret_cast<void(*)()>(dlsym(module,"charged_scene_stop_movie"));
        Check(qualify && draw && stopMovie,"Actual original scene module exports unavailable");
        Check(qualify(),"Original Credits async loading/draw prerequisites failed");
        unsigned frames=0,draws=0,quiet=0;bool snapshot=false,encoded=false;bool exit=false;
        const auto start=std::chrono::steady_clock::now();
        auto previousFrame=start;
        while(!exit && (interactive || !snapshot)) {
            for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)if(e->type==AURORA_EXIT)exit=true;
            if(exit)break;
            Check(interactive || std::chrono::steady_clock::now()-start<std::chrono::seconds(40),"Original Credits Vulkan pipeline timed out");
            aurora_service_hardware();
            mscharged::diagnostic::ServiceCreditsMovieHardware();
            if(!aurora_begin_frame()){SDL_Delay(1);continue;}
            // Parent-authorized unarmed native window/frame fixture. True glPlat
            // startup/retrace owns the movie clock; VI scanout is omitted; all FE geometry/font/visibility/
            // material/draw decisions remain actual original source packets.
            GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({0,0,0,255},GX_MAX_Z24);
            GXSetViewport(0,0,640,448,0,1);GXSetScissor(0,0,640,448);
            const auto receipt=AuroraGXBeginDrawReceipt();Check(receipt,"Real draw receipt unavailable");
            const auto now=std::chrono::steady_clock::now();
            const float delta=std::chrono::duration<float>(now-previousFrame).count();
            previousFrame=now;
            draw(delta);AuroraGXEndDrawReceipt();
            const bool sample=!snapshot && frames>=14 && quiet>=2
                && now-start>=std::chrono::seconds(2);
            if(sample){EndWithSnapshot(directory/"original-credits.ppm");snapshot=true;}else aurora_end_frame();
            aurora::gfx::synchronize();
            encoded=encoded || AuroraGXWasDrawEncoded(receipt);
            draws+=aurora_get_stats()->drawCallCount;++frames;
            quiet=mscharged::platform::GetQueuedPipelineCount()?0:quiet+1;
            SDL_Delay(1);
        }
        Check(snapshot && encoded && draws && aurora_get_last_presentation().sequence,"Original source Credits draw/presentation incomplete");
        Check(!errors,"Actual scene/SDK reported errors");
        const auto audio=mscharged::platform::GetNativeAIStatus();
        const auto audioClock=mscharged::platform::GetNativeAIClockStatus();
        Check(audio.initialized && audio.consumed_blocks && audio.dispatched_callbacks,
              "Original movie audio did not reach the actual host device");
        std::printf("Original Credits native AI: submitted%llu consumed%llu callbacks%llu, rate%d, last PCM hash%016llx; coalesced%llu, maximum DMA service gap%lluns.\n",
                    static_cast<unsigned long long>(audio.submitted_blocks),
                    static_cast<unsigned long long>(audio.consumed_blocks),
                    static_cast<unsigned long long>(audio.dispatched_callbacks),audio.input_frequency,
                    static_cast<unsigned long long>(audio.last_input_hash),
                    static_cast<unsigned long long>(audioClock.coalesced_edges),
                    static_cast<unsigned long long>(audioClock.maximum_service_gap_ns));
        std::printf("Original Credits window diagnostic: %u frames,%u actual draws,source receipt and presentation verified. Live original scene-manager Update/FERender using elapsed native owner time; actual source THP movie/video and mode0 audio; omits original main/tasks/AX predecessor/VI scanout/input/world/CRT teardown.\n",frames,draws);
        std::printf("Native audio device: rate%d, period%d frames, queued%d source bytes.\n",
                    audio.device_frequency,audio.device_frames,audio.queued_input_bytes);
        AuroraVIHardwareState videoClock{};
        Check(aurora_get_video_hardware_state(&videoClock), "Original VI clock observation unavailable");
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::printf("Playback observation: %.3fs, %.2f rendered frames/s; VI retraces%u, elapsed fields%llu.\n",
                    elapsed,frames/elapsed,videoClock.retrace_count,
                    static_cast<unsigned long long>(videoClock.elapsed_fields));
        stopMovie();
        mscharged::diagnostic::ShutdownCreditsMovieHardware();
        Check(!mscharged::platform::GetNativeAIStatus().initialized,
              "Original movie owner did not drain the actual host audio device");
        aurora_shutdown_video_hardware();
        mscharged::ShutdownNativeSystemSettings();
        mscharged::platform::ShutdownNativeInterruptController();
        aurora_dvd_close();aurora_shutdown();std::fflush(nullptr);std::_Exit(0);
    } catch(const std::exception& e){std::fprintf(stderr,"Original Credits gate: %s\n",e.what());std::fflush(nullptr);std::_Exit(1);}
}

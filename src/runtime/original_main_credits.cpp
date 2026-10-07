#include "runtime/original_main_credits.h"
#include "runtime/original_sh_menu_diagnostic.h"
#include <aurora/aurora.h>
#include <aurora/hardware.h>
#include "bootstrap/launch_options.h"
#include "platform/path.h"
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include "gfx/xfb.hpp"
#include "gfx/scanout.hpp"
#include <dolphin/gx/GXAurora.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <aurora/video.h>
#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include "webgpu/gpu.hpp"
#include "platform/stm_device.h"
#include "platform/hardware_owner.h"
#include "platform/desktop_presented_dpd.h"
#include "platform/ai.h"
#include "platform/native_ax_module_memory.h"
#include "platform/system.h"
#include "platform/video_device.h"
#include "platform/video_output_device.h"
#include "platform/interrupt_controller.h"
#include "platform/filesystem_boot.h"
#include "platform/ipc_boot_buffer.h"
#include <revolution/os/OSIpc.h>
#include <revolution/ipc.h>
#include "credits_movie_hardware.h"
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
#include "original_game_audio_hardware.h"
#endif
#include <SDL3/SDL_video.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <string>
namespace aurora { extern AuroraConfig g_config; }
extern "C" bool __OSInitSTM();
namespace {
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
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
// Read the completed resource selected by original VI. This diagnostic reads
// source copy output; it submits no game draw or diagnostic frame boundary.
void ReadSelectedXFB(const AuroraVIPresentedState& presented,
    const std::shared_ptr<const aurora::gfx::xfb::Snapshot>& copy, const std::filesystem::path& output) {
    // A diagnostic GPU readback must keep the real3ms audio/VI IRQ owner live.
    // This service runs no game task or presentation and preserves source requests.
    aurora_service_hardware_interrupts();
    Check(copy && copy->revision==presented.copy_revision &&
          copy->physical==OSCachedToPhysical(const_cast<void*>(presented.framebuffer)),
          "Selected XFB no longer matches the exact successful surface presentation");
    Check(copy && copy->gpuComplete.load(std::memory_order_acquire),
          "Selected original XFB has no completed source GX copy");
    Check(copy->width==640 && copy->height==448 && copy->stride==1280,
          "Native selected XFB copy does not preserve original source geometry");
    Check(copy->packedYuyv && copy->packedYuyv->format==wgpu::TextureFormat::RGBA8Unorm &&
          copy->packedYuyv->size.width==copy->width/2 && copy->packedYuyv->size.height==copy->height,
          "Selected XFB does not retain packed source YUYV output");
    const unsigned pitch=(copy->stride+255)&~255u;
    auto device=aurora::gfx::device();
    const wgpu::BufferDescriptor desc{.usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,
        .size=std::uint64_t(pitch)*copy->height};
    const auto buffer=device.CreateBuffer(&desc); auto encoder=device.CreateCommandEncoder();
    const wgpu::TexelCopyTextureInfo source{.texture=copy->packedYuyv->texture};
    const wgpu::TexelCopyBufferInfo target{.layout={.bytesPerRow=pitch,.rowsPerImage=copy->height},.buffer=buffer};
    const wgpu::Extent3D extent{copy->width/2,copy->height,1};
    encoder.CopyTextureToBuffer(&source,&target,&extent);
    const auto command=encoder.Finish(); device.GetQueue().Submit(1,&command);
    struct Result { std::atomic_int done{0}; }; auto result=std::make_shared<Result>();
    buffer.MapAsync(wgpu::MapMode::Read,0,desc.size,wgpu::CallbackMode::AllowSpontaneous,
        [result](wgpu::MapAsyncStatus status,wgpu::StringView) {
            result->done.store(status==wgpu::MapAsyncStatus::Success?1:-1,std::memory_order_release);
        });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!result->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline) {
        aurora_service_hardware_interrupts();
        device.Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(result->done.load(std::memory_order_acquire)==1, "Selected source XFB GPU readback failed");
    const auto* bytes=static_cast<const unsigned char*>(buffer.GetConstMappedRange(0,desc.size));
    std::vector<unsigned char> rgb(std::size_t(copy->width)*copy->height*3);
    auto channel=[](int value) { return static_cast<unsigned char>(std::clamp(value,0,255)); };
    unsigned lit=0;
    for(unsigned y=0;y<copy->height;++y) {
        if((y&15)==0)aurora_service_hardware_interrupts();
        for(unsigned x=0;x<copy->width;++x) {
        const auto* pair=bytes+std::size_t(y)*pitch+(x/2)*4;
        const int c=int(pair[(x&1)?2:0])-16,d=int(pair[1])-128,e=int(pair[3])-128;
        auto* pixel=rgb.data()+3*(std::size_t(y)*copy->width+x);
        pixel[0]=channel((298*c+409*e+128)>>8);
        pixel[1]=channel((298*c-100*d-208*e+128)>>8);
        pixel[2]=channel((298*c+516*d+128)>>8);
        if(pixel[0]||pixel[1]||pixel[2])++lit;
        }
    }
    aurora_service_hardware_interrupts();
    buffer.Unmap(); Check(lit!=0, "Source-selected native XFB is entirely black");
    auto* file=std::fopen(output.c_str(),"wb"); Check(file,"Cannot create selected XFB snapshot");
    std::fprintf(file,"P6\n%u %u\n255\n",copy->width,copy->height);
    std::size_t written=0;
    while(written<rgb.size()) {
        aurora_service_hardware_interrupts();
        const auto count=std::min<std::size_t>(65536,rgb.size()-written);
        const auto n=std::fwrite(rgb.data()+written,1,count,file);
        written+=n;
        if(n!=count)break;
    }
    const auto closed=std::fclose(file);
    aurora_service_hardware_interrupts();
    Check(written==rgb.size() && !closed,"Selected XFB snapshot write failed");
    std::printf("Actual source-selected XFB %ux%u revision%llu, nonblack pixels%u; saved %s.\n",
        copy->width,copy->height,static_cast<unsigned long long>(copy->revision),lit,output.c_str());
}
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
    while(!s->done && std::chrono::steady_clock::now()<end){aurora::gfx::device().Tick();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
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
int mscharged::RunOriginalMainCredits(int argc, char** argv,
    const ResolvedLaunch* suppliedLaunch, bool interactive, OriginalMainScene scene) {
    try {
        const bool frontend = scene != OriginalMainScene::Credits;
        const bool optionsScene = scene == OriginalMainScene::FrontendOptions;
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_BOOT_SCRIPT)
        const bool sourceBoot = frontend;
#else
        const bool sourceBoot = false;
#endif
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_BOOT_TO_FE)
        const bool priorBoot = frontend;
#else
        const bool priorBoot = false;
#endif
        Check(!sourceBoot || !optionsScene,
              "Authored Boot script builds do not yet advance to Options; use FRONTEND_BOOT_SCRIPT=OFF for that diagnostic");
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        const bool sourceAudio = frontend;
#else
        const bool sourceAudio = false;
#endif
        bool nativeSend=true, resizeCheck=false;
        mscharged::LaunchOptions launchOptions;
        unsigned windowWidth=800, windowHeight=600;
        std::filesystem::path disc;
        for (int i=1;!suppliedLaunch && i<argc;++i) {
            const std::string argument=argv[i];
            if (mscharged::ParseLaunchOption(argc,argv,i,launchOptions)) {
                if(argument=="--window" || argument=="--fullscreen") interactive=true;
                continue;
            }
            if (argument=="--help") {
                std::puts("Usage: mscharged-original-main-credits-check [launch settings] [--resize-check] [--diagnostic-frame]\n"
                          "Enter original main, then load and update the original Credits scene.\n"
                          "Original THP movie/video and mode0 audio diagnostic; USA/English.\n"
                          "Desktop keyboard and mouse feed original Wii/frontend input methods.\n"
                          "Enter/Space=A, Escape/Backspace=B; arrows=DPad; Z/X=1/2.\n"
                          "Mouse: pointer; left/right click=A/B.\n"
                          "A selects original COPYRIGHTS; the following menu transition remains held.\n"
                          "Full tasks, AX predecessor, motion and game shutdown are omitted.\n"
                          "--window keeps the source scene running until you close the window.\n"
                          "Original glSendFrame/swap/VI preserves source geometry at any window size.\n"
                          "--diagnostic-frame selects the transitional unarmed frame comparison.");
                std::fputs(mscharged::LaunchOptionsHelp.data(),stdout);
                std::puts("--window or --fullscreen keeps this diagnostic open; without either, save a capture and exit.");
                return 0;
            }
            if(argument=="--native-send")nativeSend=true;
            else if(argument=="--resize-check")resizeCheck=true;
            else if(argument=="--diagnostic-frame")nativeSend=false;
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        const auto executable=std::filesystem::absolute(std::filesystem::path(argv[0]));
        const auto launch=suppliedLaunch ? *suppliedLaunch :
            mscharged::LoadLaunch(launchOptions,executable.parent_path());
        disc=mscharged::PathUtf8(launch.disc_path);
        windowWidth=launch.settings.width;windowHeight=launch.settings.height;
        const auto& aspect = launch.settings.aspect;
        Check(aspect=="auto" || aspect=="4:3" || aspect=="16:9",
              "Original Credits supports 4:3 or 16:9; select --aspect 4:3 or --aspect 16:9");
        // Stage a Wii system preference. Original main queries SC and selects
        // its projection and authored scene; resizing only scales that image.
        const bool widescreen = aspect=="16:9" ||
            (aspect=="auto" && windowWidth*3u>windowHeight*4u);
        const unsigned aspectWidth=widescreen?16u:4u, aspectHeight=widescreen?9u:3u;
        std::printf("%s\n",mscharged::DescribeLaunch(launch).c_str());
        Check(!disc.empty() && std::filesystem::is_regular_file(launch.disc_path),"Supply your own game ISO/RVZ with --disc FILE or [game] disc in the INI");
        Check(!resizeCheck || !launch.settings.fullscreen,"--resize-check requires a windowed launch; add --window");
        Check(!priorBoot || !resizeCheck,
              "The prior Boot stage retains original effects/NPC owners; resize teardown is not qualified");
        const char* moduleFilename=MSCHARGED_ORIGINAL_MAIN_CREDITS_MODULE_FILENAME;
        if(frontend) {
#ifdef MSCHARGED_ORIGINAL_FRONTEND_MODULE_FILENAME
            moduleFilename=MSCHARGED_ORIGINAL_FRONTEND_MODULE_FILENAME;
#else
            throw std::runtime_error("Original frontend sequence is not in this build");
#endif
        }
        const auto modulePath=executable.parent_path()/moduleFilename;
        Check(std::filesystem::is_regular_file(modulePath),"Original-main source module is missing beside the executable");
        // Real host window/device/FIFO owner exists before any original module
        // constructors. Original main still owns its GXInit/glStartup decisions.
        const auto dataDirectory = std::filesystem::absolute(
            executable.parent_path() / "original-main-credits-data");
        std::filesystem::create_directories(dataDirectory);
        const auto dataPath = dataDirectory.string();
        AuroraConfig config{};
        config.appName=optionsScene ? "Mario Strikers Charged | original Options diagnostic" :
            frontend ? "Mario Strikers Charged | original Boot/Intro diagnostic" :
            "Mario Strikers Charged | original-main Credits diagnostic";
        config.userPath=config.cachePath=dataPath.c_str();
        config.desiredBackend=BACKEND_VULKAN;
        config.windowWidth=windowWidth;config.windowHeight=windowHeight;
        config.windowPosX=config.windowPosY=-1;
        config.mem1Size=MEM1_DEFAULT_SIZE;
        config.mem2Size=64u*1024u*1024u;
        config.logLevel=LOG_INFO;config.enableBackendValidation=true;
        config.vsync=true;
        const auto host=aurora_initialize(argc,argv,&config);
        if(!host.window||host.backend!=BACKEND_VULKAN)
            throw std::runtime_error("Actual Vulkan foundation unavailable; no fallback acceptance");
        Check(SDL_SetWindowFullscreen(host.window,launch.settings.fullscreen), "Requested launch window mode rejected");
        if(nativeSend) {
            // Existing Aurora policy fixes the internal source EFB at 1x.
            // Original SC/VI signal aspect controls independent desktop output.
            Check(SDL_SetWindowMinimumSize(host.window,1,1), "SDL window minimum rejected");
            Check(SDL_SetWindowSize(host.window,static_cast<int>(windowWidth),static_cast<int>(windowHeight)),
                  "Requested window size rejected");
            VISetFrameBufferScale(1.0f);
            AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
            for(const auto* event=aurora_update();event->type!=AURORA_NONE;++event)
                Check(event->type!=AURORA_EXIT, "Native window closed before source entry");
            aurora::gfx::synchronize();
        }
        OSInit();
        if(!OSGetArenaLo()||!OSGetMEM2ArenaLo())
            throw std::runtime_error("Actual captured SDK arenas unavailable");
        mscharged::platform::InitializeNativeInterruptController();
        // Explicit USA diagnostic backing; independent SC and VI settings.
        const mscharged::NativeSystemSettings settings{1,0,0,std::uint8_t(widescreen),1};
        mscharged::ConfigureNativeSystemSettings(settings);
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT,false);
        if(nativeSend) {
            mscharged::platform::ConfigureNativeVideoOutputHardware(settings);
            // Arm source-owned recording before original constructors/commands.
            // This mode never uses diagnostic aurora_begin/end_frame.
            aurora_configure_native_gx_hardware();
        }
        if (frontend) {
            // Physical IPC boot metadata is part of the native OS foundation.
            // The whole original SDK keeps its advancing IPC buffer cursor;
            // source nlFlashInitialize/NANDInit still run at their own positions.
            alignas(32) static std::array<unsigned char, 32768> ipcBootMemory{};
            mscharged::platform::InstallNativeIPCBootBuffer(
                ipcBootMemory.data(), ipcBootMemory.size());
            __OSInitIPCBuffer();
            IPCInit();
            // One isolated native title profile. UID is an explicit native IOS
            // process identity, independent of the host account/retail console.
            mscharged::platform::InitializeNativeFilesystemForDisc(
                {launch.disc_path, dataDirectory / "nand", 0x1001});
        }
        mscharged::platform::InitializeNativeSTMDevice();
        if(!__OSInitSTM())throw std::runtime_error("Actual original STM initialization failed");
        // Borrow input into the existing AI/SDK hardware owner; source KPAD
        // and FE methods retain their original mappings and decisions.
        mscharged::platform::InitializeNativeHardwareInput(host.window,{0,3,false,false},
            mscharged::platform::GetNativeSTMInput(),{true,false,true,mscharged::platform::QueryPresentedDesktopDpd,nullptr});
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio)
            mscharged::diagnostic::InitializeOriginalGameAudioHardware(
                mscharged::platform::ServiceNativeHardwareInput);
        else
#endif
            mscharged::diagnostic::InitializeCreditsMovieHardware(
                mscharged::platform::ServiceNativeHardwareInput);
        if(!aurora_dvd_open(disc.c_str())) throw std::runtime_error("Actual owned Wii data partition failed");
        std::unique_ptr<mscharged::platform::NativeAXModuleMemory> axModuleMemory;
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        std::unique_ptr<mscharged::diagnostic::OriginalGameAudioHardware> gameAudioHardware;
#endif
        // Terminal diagnostics retain live source images and hardware owners on
        // failure. Unwinding a device with active source pins is not retirement.
        try {
        if(frontend)
            axModuleMemory=std::make_unique<mscharged::platform::NativeAXModuleMemory>(modulePath.c_str());
        auto* module=dlopen(modulePath.c_str(),RTLD_LAZY|RTLD_LOCAL);
        if(!module)throw std::runtime_error(dlerror());
        if(axModuleMemory)axModuleMemory->ConfirmLoaded(module);
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio)
            gameAudioHardware=std::make_unique<mscharged::diagnostic::OriginalGameAudioHardware>(*axModuleMemory);
#endif
        auto entry=reinterpret_cast<int(*)()>(dlsym(module,"charged_original_entry"));
        if(!entry)throw std::runtime_error("Original source main export unavailable");
        std::fprintf(stderr,"Entering actual source main with real Aurora Vulkan/FIFO owner and native PI_VI under temporary MAIN_BOOTSTRAP; flow remains incomplete.\n");
        std::fflush(nullptr);
        const int result=entry();
        Check(result==85, "Original main selected scene did not complete checkpoint 85");
        using ObserveAudio = unsigned(*)();
        using AudioAction = void(*)();
        using AudioIdle = bool(*)();
        auto observeAudio=sourceAudio ? reinterpret_cast<ObserveAudio>(
            dlsym(module,"charged_original_audio_observe")) : nullptr;
        auto shutdownAudio=sourceAudio ? reinterpret_cast<AudioAction>(
            dlsym(module,"charged_original_audio_shutdown")) : nullptr;
        auto idleAudio=sourceAudio ? reinterpret_cast<AudioIdle>(
            dlsym(module,"charged_original_audio_idle")) : nullptr;
        auto unloadIdleBanks=sourceAudio ? reinterpret_cast<AudioAction>(
            dlsym(module,"charged_original_audio_unload_idle_banks")) : nullptr;
        auto retireAudioPin=sourceAudio ? reinterpret_cast<AudioAction>(
            dlsym(module,"charged_original_audio_retire_silence_pin")) : nullptr;
        if(sourceAudio) {
            Check(observeAudio && shutdownAudio && idleAudio && unloadIdleBanks && retireAudioPin,
                  "Original audio source observations and retirement exports unavailable");
            // True source predicates, before the first original task/frame.
            Check(observeAudio()==7u,
                  "Original audio owner/initialization/configuration callback incomplete");
            std::puts("Original main audio: source owner initialized, nlxgs configuration genuinely loaded.");
        }
        auto observeBootScript=sourceBoot ? reinterpret_cast<ObserveAudio>(
            dlsym(module,"charged_original_boot_script_observe")) : nullptr;
        auto bootInstruction=sourceBoot ? reinterpret_cast<int(*)()>(
            dlsym(module,"charged_original_boot_script_instruction")) : nullptr;
        auto bootPhase=sourceBoot ? reinterpret_cast<int(*)()>(
            dlsym(module,"charged_original_boot_scene_phase")) : nullptr;
        Check(!sourceBoot || (observeBootScript && bootInstruction && bootPhase),
              "Original authored Boot observation exports unavailable");
        unsigned lastBootFlags=0;
        int lastBootInstruction=-2, lastBootPhase=-2;
        std::chrono::steady_clock::time_point bootLogoStart{};
        bool bootReady=!sourceBoot;
        auto observeBoot = [&] {
            if(!sourceBoot)return;
            const auto flags=observeBootScript();
            const int instruction=bootInstruction(), phase=bootPhase();
            Check((flags & (priorBoot ? 11u : 27u))==(priorBoot ? 11u : 27u),
                  "Original Boot bytecode callback/header or required source owner is incomplete");
            if(phase==3 && lastBootPhase!=3)bootLogoStart=std::chrono::steady_clock::now();
            if(flags!=lastBootFlags || instruction!=lastBootInstruction || phase!=lastBootPhase) {
                std::printf("Original authored Boot: VM flags%u instruction%d phase%d.\n",flags,instruction,phase);
                std::fflush(stdout);
                lastBootFlags=flags;lastBootInstruction=instruction;lastBootPhase=phase;
            }
            bootReady=flags==31u && phase==4 && idleAudio();
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
            if(bootReady)bootReady=gameAudioHardware->Status().last_active_voices==0;
#endif
        };
        auto frame=reinterpret_cast<void(*)(float)>(dlsym(module,nativeSend?"charged_original_scene_native_frame":"charged_original_scene_frame"));
        Check(frame,"Same original-main module scene-frame export unavailable");
        auto stopMovie=reinterpret_cast<void(*)()>(dlsym(module,"charged_original_scene_stop_movie"));
        Check(stopMovie,"Original main movie-stop export unavailable");
        using ObserveSH = std::uint32_t(*)(OriginalSHSnapshot*,std::uint32_t);
        using RequestSH = std::uint32_t(*)();
        auto observeSH=optionsScene ? reinterpret_cast<ObserveSH>(
            dlsym(module,"charged_original_sh_observe")) : nullptr;
        auto requestSH=optionsScene ? reinterpret_cast<RequestSH>(
            dlsym(module,"charged_original_sh_request_options")) : nullptr;
        Check(!optionsScene || (observeSH && requestSH),
              "Original Options diagnostic requires MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS=ON");
        OriginalSHSnapshot sh{};
        unsigned lastRequest=0;
        int lastScene=-1, lastState=-1;
        bool optionsReady=false;
        bool optionsBoundsReported=false;
        auto observeOptions = [&] {
            if(!optionsScene)return;
            Check(observeSH(&sh,sizeof(sh)) && sh.version==1 &&
                  sh.bytes==sizeof(sh) && sh.stack_depth<=32,
                  "Original SH observation ABI mismatch");
            if(!sh.stack_depth)return;
            const auto& top=sh.stack[sh.stack_depth-1];
            if(top.scene_id!=lastScene || top.resource_state!=lastState) {
                std::printf("Original SH: scene%d resources%d depth%u, owners%u navigation%u pointers%u.\n",
                    top.scene_id,top.resource_state,sh.stack_depth,sh.owners,
                    sh.navigation_ready,sh.pointer_instances);
                std::fflush(stdout);lastScene=top.scene_id;lastState=top.resource_state;
            }
            optionsReady=top.scene_id==13 && top.resource_state==6 && top.visible &&
                sh.options_initialized && sh.options_state==1 && top.slide_time>=0.2f;
            if(optionsReady && !optionsBoundsReported) {
                for(unsigned i=0;i<3;++i) {
                    const auto& b=sh.buttons[i];
                    std::printf("Original Options button%u: %.6g..%.6g, %.6g..%.6g; slide%.6g/%.6g, input%u/%u lock%d.\n",
                        i,b.min_x,b.max_x,b.min_y,b.max_y,top.slide_time,top.slide_duration,
                        sh.input_allowed,sh.input_enabled,sh.input_lock_depth);
                }
                std::fflush(stdout);optionsBoundsReported=true;
            }
        };
        // Existing actual source GX/state/material/font/view initialization is
        // retained. No host GXInit, source pool restart or fixture font setup.
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        unsigned frames=0,draws=0,quiet=0; bool snapshot=false,encoded=false,exit=false,nativePresented=false;
        bool bootLogoCaptured=false;
        const auto start=std::chrono::steady_clock::now(); auto previous=start;
        auto stageStart=start; unsigned resizeStage=0; bool stageAnnounced=false;
        wgpu::Texture retainedEFB;
        Check(!resizeCheck || nativeSend, "Resize qualification requires source-native send");
        while(!exit && (interactive || !snapshot)) {
            for(const auto* event=aurora_update();event->type!=AURORA_NONE;++event)
                if(event->type==AURORA_EXIT)exit=true;
            if(exit)break;
            mscharged::diagnostic::ServiceCreditsMovieHardware();
            if(requestSH) {
                const auto status=requestSH();
                if(status!=lastRequest) {
                    std::printf("Original Options selection status%u (7 waits for the original audio context owner).\n",status);
                    std::fflush(stdout);lastRequest=status;
                }
            }
            const auto now=std::chrono::steady_clock::now();
            Check(interactive || now-start<std::chrono::seconds(sourceBoot?90:40),"Original source scene diagnostic timed out");
            if(nativeSend) {
                const auto receipt=AuroraGXBeginDrawReceipt();
                Check(receipt,"Native source draw receipt unavailable");
                const float delta=std::chrono::duration<float>(now-previous).count(); previous=now;
                frame(delta); observeOptions(); observeBoot(); AuroraGXEndDrawReceipt();
                AuroraGXSync(); aurora::gfx::synchronize();
                encoded=encoded || AuroraGXWasDrawEncoded(receipt);
                draws+=aurora_get_stats()->drawCallCount; ++frames;
                AuroraVIPresentedState output{};
                const auto presentedCopy=aurora::gfx::scanout::presented_snapshot(output);
                nativePresented=output.framebuffer && !output.black &&
                    output.copy_revision && output.presentation_sequence>=3;
                if(sourceBoot && lastBootPhase==3 && nativePresented && encoded && !bootLogoCaptured &&
                        now-bootLogoStart>=std::chrono::milliseconds(600)) {
                    ReadSelectedXFB(output,presentedCopy,dataDirectory/"original-boot-logo-xfb.ppm");
                    bootLogoCaptured=true;
                }
                if(!stageAnnounced && nativePresented && encoded && now-stageStart>=std::chrono::milliseconds(500)) {
                    const auto size=aurora_get_window_size();
                    const auto& efb=aurora::webgpu::g_frameBuffer;
                    const unsigned expectedWidth=resizeStage==1?1280:resizeStage==2?600:windowWidth;
                    const unsigned expectedHeight=resizeStage==1?720:resizeStage==2?900:windowHeight;
                    Check(launch.settings.fullscreen || (size.width==expectedWidth && size.height==expectedHeight),
                          "Controlled native window resize did not reach its requested actual size");
                    Check(size.fb_width==640 && size.fb_height==448 &&
                          efb.size.width==640 && efb.size.height==448,
                          "Window resize changed original source EFB geometry");
                    if(retainedEFB) Check(efb.texture.Get()==retainedEFB.Get(),
                                         "Output resize replaced the original source EFB texture");
                    else retainedEFB=efb.texture;
                    const auto viewport=aurora::webgpu::calculate_present_viewport(
                        size.native_fb_width,size.native_fb_height,aspectWidth,aspectHeight);
                    std::printf("Native resize stage%u: surface%ux%u EFB%ux%u same-storage, SC%u:%u viewport %.0f,%.0f %.0fx%.0f.\n",
                        resizeStage,size.native_fb_width,size.native_fb_height,efb.size.width,efb.size.height,
                        aspectWidth,aspectHeight,
                        viewport.left,viewport.top,viewport.width,viewport.height);
                    std::fflush(stdout); stageAnnounced=true;
                }
                if(now-stageStart>=std::chrono::seconds(resizeStage?8:3) && nativePresented && encoded &&
                   (!optionsScene || optionsReady) && bootReady) {
                    if(!snapshot) {
                        const auto filename=resizeCheck?
                            "original-main-native-xfb-stage"+std::to_string(resizeStage)+".ppm":
                            std::string("original-main-native-xfb.ppm");
                        ReadSelectedXFB(output,presentedCopy,dataDirectory/filename);
                    }
                    if(resizeCheck && resizeStage<2) {
                        ++resizeStage; stageStart=now; stageAnnounced=false;
                        const int w=resizeStage==1?1280:600;
                        const int h=resizeStage==1?720:900;
                        Check(SDL_SetWindowSize(host.window,w,h), "Controlled output resize rejected");
                    } else {
                        snapshot=true; // Original selected XFB plus actual source/output geometry.
                        if(!interactive)break;
                    }
                }
                std::this_thread::yield(); continue;
            }
            if(!aurora_begin_frame()){std::this_thread::yield();continue;}
            const auto receipt=AuroraGXBeginDrawReceipt();Check(receipt,"Real source frame draw receipt unavailable");
            const float delta=std::chrono::duration<float>(now-previous).count(); previous=now;
            frame(delta); observeOptions(); observeBoot(); AuroraGXEndDrawReceipt();
            const bool sample=!snapshot && now-start>=std::chrono::seconds(3) && quiet>=2 &&
                (!optionsScene || optionsReady) && bootReady;
            if(sample){EndWithSnapshot(dataDirectory/"original-main-credits.ppm");snapshot=true;}
            else aurora_end_frame();
            aurora::gfx::synchronize();
            encoded=encoded || AuroraGXWasDrawEncoded(receipt);
            draws+=aurora_get_stats()->drawCallCount;++frames;
            quiet=std::atomic_ref<const unsigned>(aurora_get_stats()->queuedPipelines).load()?0:quiet+1;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(snapshot && encoded && draws &&
              (nativeSend ? nativePresented : aurora_get_last_presentation().sequence != 0),
              "Original main Credits real source draw/presentation incomplete");
        if(nativeSend) {
            std::printf("Original main selected %s: %u native source-send frames, draw receipt+chosen-XFB VI presentation. Full startup/AX/world/reset/CRT remain incomplete.\n",
                sourceBoot ? "authored Boot script0" : optionsScene ? "Options tasks" : frontend ? "Boot/Intro tasks" : "Credits",frames);
        } else {
        std::printf("Original main→selected Credits live: %u actual source frames, %u encoded draws; one initialized game/SDK, native owner elapsed time. Actual original THP/movie/mode0 audio; digital input endpoints selected; full task/automatic swap reset/AX/motion/CRT scopes remain held.\n",frames,draws);
        }
        const auto audio=mscharged::platform::GetNativeAIStatus();
        const auto audioClock=mscharged::platform::GetNativeAIClockStatus();
        Check(audio.initialized && audio.consumed_blocks && audio.dispatched_callbacks,
              "Original-main movie audio did not reach the actual host device");
        std::printf("Original-main native AI: submitted%llu consumed%llu callbacks%llu, rate%d; coalesced%llu, maximum DMA service gap%lluns.\n",
                    static_cast<unsigned long long>(audio.submitted_blocks),
                    static_cast<unsigned long long>(audio.consumed_blocks),
                    static_cast<unsigned long long>(audio.dispatched_callbacks),audio.input_frequency,
                    static_cast<unsigned long long>(audioClock.coalesced_edges),
                    static_cast<unsigned long long>(audioClock.maximum_service_gap_ns));
        std::printf("Native audio device: rate%d, period%d frames, queued%d source bytes.\n",
                    audio.device_frequency,audio.device_frames,audio.queued_input_bytes);
        AuroraVIHardwareState videoClock{};
        Check(aurora_get_video_hardware_state(&videoClock), "Original VI clock observation unavailable");
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::printf("Playback observation: %.3fs, %.2f rendered frames/s; VI retraces%u, elapsed fields%llu.\n",
                    elapsed,frames/elapsed,videoClock.retrace_count,
                    static_cast<unsigned long long>(videoClock.elapsed_fields));
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio) {
            const auto status=gameAudioHardware->Status();
            std::printf("Actual native AX state: initialized%u phase%u processed%llu completed%llu SYNC%llu YIELD%llu.\n",
                status.native_initialized, static_cast<unsigned>(status.protocol.phase),
                static_cast<unsigned long long>(status.frames.processed_frames),
                static_cast<unsigned long long>(status.frames.completed_frames),
                static_cast<unsigned long long>(status.frames.sync_interrupts),
                static_cast<unsigned long long>(status.frames.yield_interrupts));
            Check(status.native_initialized && status.frames.processed_frames &&
                  status.frames.completed_frames && status.frames.sync_interrupts &&
                  status.frames.yield_interrupts,
                  "Original audio requests have no actual native AX frame completion");
            std::printf("Original main AX: processed%llu completed%llu SYNC%llu YIELD%llu; native ROM-free coefficient policy.\n",
                static_cast<unsigned long long>(status.frames.processed_frames),
                static_cast<unsigned long long>(status.frames.completed_frames),
                static_cast<unsigned long long>(status.frames.sync_interrupts),
                static_cast<unsigned long long>(status.frames.yield_interrupts));
        }
#endif
        stopMovie();
        if(sourceAudio) {
            Check(idleAudio(),"Active original sounds must retire through original game flow before host detachment");
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
            Check(gameAudioHardware->Status().last_active_voices==0,
                  "Original stopped voices have not completed a real native AX frame");
#endif
        }
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio)gameAudioHardware->UnbindOwnerService();
#endif
        mscharged::diagnostic::ShutdownCreditsMovieHardware();
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio) {
            gameAudioHardware->CloseAfterAIStop(retireAudioPin,unloadIdleBanks,shutdownAudio);
            gameAudioHardware.reset();
            Check(observeAudio()==7u,"Original source shutdown flags unexpectedly changed");
            std::puts("Original audio stopped: AI drained, native AX halted/closed, original idle banks/source retired before device pins; original shutdown flags preserved.");
        }
#endif
        mscharged::platform::ShutdownNativeHardwareInput();
        if (frontend)
            mscharged::platform::ShutdownNativeFilesystem();
        if(axModuleMemory) {
            // Actual source audio retirement already released its module pins.
            // The constructor-only mode retains the earlier stopped device gate.
            if(!sourceAudio)axModuleMemory->ReleaseAfterDeviceDrain();
            axModuleMemory.reset();
        }
        Check(!mscharged::platform::GetNativeAIStatus().initialized,
              "Original-main movie owner did not drain the actual host audio device");
        if(resizeCheck) {
            // Retire the actual native hardware/window for the resize gate.
            // Original game tasks/CRT destruction remain explicitly omitted;
            // _Exit prevents surviving source statics touching released arenas.
            aurora_dvd_close();
            aurora_shutdown();
            mscharged::platform::ShutdownNativeInterruptController();
            std::puts("Native resize qualification retired real VI/GX/window hardware.");
        }
        if(priorBoot)
            std::puts("Prior Boot diagnostic retains original effects/NPC resources and game arenas at terminal exit; full source cleanup remains pending.");
        std::fflush(nullptr);std::_Exit(0);
        } catch(const std::exception& e) {
            std::fprintf(stderr,"Actual source diagnostic stopped with live owners: %s\n",e.what());
            std::fflush(nullptr);std::_Exit(1);
        }
    }catch(const std::exception& e){std::fprintf(stderr,"Actual source diagnostic stopped: %s\n",e.what());std::fflush(nullptr);std::_Exit(1);}
}

#include "platform/graphics_stats.h"
#include "platform/app_icon.h"
#include "platform/fullscreen_hotkey.h"
#include "platform/screenshot_hotkey.h"
#include "platform/window_fit.h"
#include "runtime/original_game.h"
#include "runtime/texture_pack_loading.h"
#include "platform/graphics_backend.h"
#include "runtime/original_sh_menu_diagnostic.h"
#include <aurora/aurora.h>
#include <aurora/hardware.h>
#include "bootstrap/antialiasing.h"
#include "bootstrap/graphics_card.h"
#include "bootstrap/launch_options.h"
#include "platform/path.h"
#include "platform/disc.h"
#include "platform/disc_message_images.h"
#include "platform/game_release.h"
#include "platform/pc_languages.h"
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include "gfx/xfb.hpp"
#include "gfx/scanout.hpp"
#include "gx/fifo.hpp"
#include <dolphin/gx/GXAurora.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <chrono>
#include <thread>
#include <aurora/video.h>
#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include "webgpu/gpu.hpp"
#include "platform/stm_device.h"
#include "platform/ios_device.h"
#include "platform/hardware_owner.h"
#include "platform/desktop_presented_dpd.h"
#include "platform/ai.h"
#include "platform/console.h"
#include "platform/log_window.h"
#include "platform/session_log.h"
#include "platform/launcher_reopen.h"
#include "platform/discord_presence.h"
#include "platform/game_presence.h"
#include "platform/presence_snapshot.h"
#include "platform/shader_cache.h"
#include "platform/process_exit.h"
#include "platform/native_ax_module_memory.h"
#include "platform/system.h"
#include "platform/rtc_policy.h"
#include "platform/video_device.h"
#include "platform/video_output_device.h"
#include "platform/interrupt_controller.h"
#include "platform/filesystem_boot.h"
#include "platform/ipc_boot_buffer.h"
#include <revolution/os/OSIpc.h>
#include <revolution/ipc.h>
#include "movie_hardware.h"
#include "runtime/fps_overlay.h"
#include "runtime/pipeline_warmup.h"
#include "mscharged/build_version.h"
#include "runtime/frame_rate.h"
#include "runtime/host_frame_pacing.h"
#include "runtime/frame_timing_log.h"
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
#include "original_game_audio_hardware.h"
#endif
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_RESET)
#include "original_terminal_verifier.h"
#include "platform/os_shutdown_requests.h"
#endif
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_filesystem.h>
#include "platform/native_module_loader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <string>
namespace aurora { extern AuroraConfig g_config; }
extern "C" bool __OSInitSTM();
// Whole original OSRtc software/cache owner, linked once into the host.
extern "C" void __OSInitSram();
extern "C" BOOL __OSSyncSram();
namespace {
// Aurora's FIFO retains every recorded command until a drain; publishing only
// hands bytes to the worker. Bound that backlog between source frames.
constexpr std::uint32_t kFifoDrainBytes=64u<<20;
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
// Counters for the opt-in frame timing log. Aurora's render worker updates the
// statistics; aligned relaxed loads suffice for this diagnostic.
mscharged::runtime::FrameTimingCounters ReadFrameTimingCounters() {
    mscharged::runtime::FrameTimingCounters counters{};
    AuroraVIHardwareState vi{};
    if(aurora_get_video_hardware_state(&vi)) counters.retraces=vi.retrace_count;
    // Native source send presents through the VI scanout path.
    AuroraVIPresentedState presented{};
    if(aurora_get_presented_video_output_state(&presented)) counters.presents=presented.presentation_sequence;
    if(const AuroraStats* stats=aurora_get_stats()) {
        const auto load=[](const std::uint32_t& value) {
            return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(value)).load(std::memory_order_relaxed);
        };
        counters.draws=load(stats->drawCallCount);
        counters.pipelines_created=load(stats->createdPipelines);
        counters.pipelines_queued=load(stats->queuedPipelines);
        counters.texture_upload_bytes=load(stats->lastTextureUploadSize);
    }
    counters.fifo_bytes=aurora::gx::fifo::get_buffer_size();
    return counters;
}

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
    // Original fades and transitions can legitimately present an all-black frame.
    buffer.Unmap();
#if defined(_WIN32)
    auto* file=_wfopen(output.c_str(),L"wb");
#else
    auto* file=std::fopen(output.c_str(),"wb");
#endif
    Check(file,"Cannot create selected XFB snapshot");
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
    mscharged::platform::Trace("Actual source-selected XFB %ux%u revision%llu, nonblack pixels%u; saved %s.\n",
        copy->width,copy->height,static_cast<unsigned long long>(copy->revision),lit,mscharged::PathUtf8(output).c_str());
}
void EndWithSnapshot(const std::filesystem::path& output) {
    auto s=std::make_shared<Snapshot>();
    Check(aurora::gfx::resolve_pass({},s->target),"Cannot resolve the actual original EFB");
    Check(s->target.colorFormat==wgpu::TextureFormat::BGRA8Unorm || s->target.colorFormat==wgpu::TextureFormat::RGBA8Unorm,"Unsupported snapshot color format");
    s->stride=(s->target.width*4+255)&~255u;
    s->rgb.resize(std::size_t(s->target.width)*s->target.height*3);
    const aurora::gfx::EncoderTaskDescriptor d{"Original main diagnostic snapshot",Snapshot::Copy,s.get(),Snapshot::Map};
    const auto task=aurora::gfx::register_encoder_task_type(d);
    const bool queued=aurora::gfx::push_encoder_task(task,nullptr,0);
    aurora_end_frame();aurora::gfx::synchronize();aurora::gfx::unregister_encoder_task_type(task);
    Check(queued,"Actual EFB snapshot task rejected");
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!s->done && std::chrono::steady_clock::now()<end){aurora::gfx::device().Tick();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    Check(s->done==1,"Original main GPU readback did not complete");
#if defined(_WIN32)
    auto* f=_wfopen(output.c_str(),L"wb");
#else
    auto* f=std::fopen(output.c_str(),"wb");
#endif
    Check(f,"Cannot create the EFB snapshot");
    std::fprintf(f,"P6\n%u %u\n255\n",s->target.width,s->target.height);
    const auto written=std::fwrite(s->rgb.data(),1,s->rgb.size(),f);const auto closed=std::fclose(f);
    Check(written==s->rgb.size() && !closed,"EFB snapshot write failed");
    unsigned lit=0;for(std::size_t i=0;i<s->rgb.size();i+=3)if(s->rgb[i]||s->rgb[i+1]||s->rgb[i+2])++lit;
    Check(lit != 0, "Original actual EFB is entirely black; visibility remains unqualified");
    mscharged::platform::Trace("Actual EFB %ux%u, nonblack pixels%u; saved %s.\n",s->target.width,s->target.height,lit,mscharged::PathUtf8(output).c_str());
}
}
int mscharged::RunOriginalGame(int argc, char** argv,
    const ResolvedLaunch* suppliedLaunch, bool interactive, OriginalMainScene scene, const char* title) {
    // Keep the cache lease through every shutdown/error path, including paths
    // that terminate with live native owners instead of unwinding them.
    std::optional<mscharged::platform::ShaderCacheUse> shaderCacheUse;
    try {
        const bool optionsScene = scene == OriginalMainScene::FrontendOptions;
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_BOOT_SCRIPT)
        const bool sourceBoot = true;
#else
        const bool sourceBoot = false;
#endif
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_BOOT_TO_FE)
        const bool priorBoot = true;
#else
        const bool priorBoot = false;
#endif
        Check(!sourceBoot || !optionsScene,
              "Authored Boot script builds do not yet advance to Options; use FRONTEND_BOOT_SCRIPT=OFF for that diagnostic");
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        const bool sourceAudio = true;
#else
        const bool sourceAudio = false;
#endif
        bool nativeSend=true, resizeCheck=false;
        mscharged::LaunchOptions launchOptions;
        unsigned windowWidth=800, windowHeight=600;
        // Internal resolution: the source EFB is efbScale x 640 x 448 (0: window).
        float efbScale=1.0f;
        std::filesystem::path disc;
        for (int i=1;!suppliedLaunch && i<argc;++i) {
            const std::string argument=argv[i];
            if (mscharged::ParseLaunchOption(argc,argv,i,launchOptions)) {
                if(argument=="--window" || argument=="--fullscreen") interactive=true;
                continue;
            }
            if(argument=="--native-send")nativeSend=true;
            else if(argument=="--resize-check")resizeCheck=true;
            else if(argument=="--diagnostic-frame")nativeSend=false;
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        const char* basePath=SDL_GetBasePath();
        Check(basePath && *basePath,"Cannot locate the executable directory");
        const auto executableDirectory=mscharged::PathFromUtf8(basePath);
        auto launch=suppliedLaunch ? *suppliedLaunch :
            mscharged::LoadLaunch(launchOptions,executableDirectory);
        mscharged::ApplyGameModeDisplay(launch,mscharged::UnderGamescope());
        disc=mscharged::PathUtf8(launch.disc_path);
        windowWidth=launch.settings.width;windowHeight=launch.settings.height;
        const auto& aspect = launch.settings.aspect;
        Check(aspect=="auto" || aspect=="4:3" || aspect=="16:9",
              "The game supports 4:3 or 16:9; select --aspect 4:3 or --aspect 16:9");
        // Stage a Wii system preference. Original main queries SC and selects
        // its projection and authored scene; resizing only scales that image.
        const bool widescreen = aspect=="16:9" ||
            (aspect=="auto" && windowWidth*3u>windowHeight*4u);
        const unsigned aspectWidth=widescreen?16u:4u, aspectHeight=widescreen?9u:3u;
        // advanced.verbose_console: development traces and the game's debug output.
        mscharged::platform::SetVerboseConsole(launch.settings.verbose_console);
        // advanced.log_window: the separate log window, when the launcher has not opened it.
        mscharged::platform::SetLogWindow(launch.settings.log_window);
        std::printf("%s\n",mscharged::DescribeLaunch(launch).c_str());
        Check(!disc.empty() && std::filesystem::is_regular_file(launch.disc_path),"Supply your own game ISO/RVZ/WBFS with --disc FILE or [game] disc in the INI");
        // Validate native preferences before acquiring any hardware/module owner.
        // The disc decides the release and so the languages a Wii could select;
        // the original game still owns its region and localization branches
        // after reading the supplied SC language.
        const auto discProfile = mscharged::InspectDisc(launch.disc_path);
        const auto* release = mscharged::FindRelease(discProfile);
        const auto unplayable = mscharged::UnplayableReason(discProfile, release);
        Check(unplayable.empty(), unplayable.c_str());
        const auto languageChoice = mscharged::ChooseLanguage(*release, launch.settings.language,
                                                              mscharged::platform::PcLanguages());
        const auto systemLanguage = languageChoice.language->sc_language;
        const auto releaseNote = mscharged::ReleaseNote(*release);
        mscharged::platform::SessionLogLine("Game: " + std::string(release->title) + ", "
            + mscharged::ReleaseLabel(*release) + " (" + release->game_id + ")"
            + (releaseNote.empty() ? "" : ". " + releaseNote));
        const std::string languageLabel = languageChoice.automatic
            ? "Automatic (" + std::string(languageChoice.language->label) + ")" : languageChoice.language->label;
        mscharged::platform::SessionLogLine(languageChoice.unavailable
            ? "Language: " + launch.settings.language + " is not in this release; " + languageLabel
            : "Language: " + languageLabel);
        Check(!resizeCheck || !launch.settings.fullscreen,"--resize-check requires a windowed launch; add --window");
        Check(!priorBoot || !resizeCheck,
              "The prior Boot stage retains original effects/NPC owners; resize teardown is not qualified");
        // One game module per game version; both European revisions run the EU one.
        const char* moduleFilename=release->code==mscharged::GameCode::Europe
            ? MSCHARGED_ORIGINAL_FRONTEND_MODULE_EU_FILENAME : MSCHARGED_ORIGINAL_FRONTEND_MODULE_US_FILENAME;
        const auto modulePath=executableDirectory/moduleFilename;
        Check(std::filesystem::is_regular_file(modulePath),"Original-main source module is missing beside the executable");
        // Real host window/device/FIFO owner exists before any original module
        // constructors. Original main still owns its GXInit/glStartup decisions.
        if(mscharged::MoveOldDataFolder(executableDirectory))
            mscharged::platform::SessionLogLine("Data: moved original-main-credits-data to data");
        const auto dataDirectory = std::filesystem::absolute(mscharged::DataFolder(executableDirectory));
        mscharged::platform::SessionLogLine("Data: "+mscharged::PathUtf8(dataDirectory));
        std::filesystem::create_directories(dataDirectory);
        const auto dataPath = dataDirectory.string();
        AuroraConfig config{};
        // Window title: the port's name and build, then the diagnostic scene.
        const std::string appTitle=title ? title : "Recharged";
        const std::string windowTitle=optionsScene ? appTitle+" | Options diagnostic" : appTitle;
        config.appName=windowTitle.c_str();
        config.userPath=config.cachePath=dataPath.c_str();
        const auto graphicsResourcesPath=mscharged::PathUtf8(executableDirectory/"assets"/"graphics");
        config.resourcesPath=graphicsResourcesPath.c_str();
        // display.backend: only auto names a second API for Aurora to try.
        const auto* graphics=&mscharged::ResolveGraphicsBackend(launch.settings.backend);
        const auto* graphicsFallback=mscharged::GraphicsBackendFallback(launch.settings.backend);
        config.desiredBackend=mscharged::platform::AuroraGraphicsBackend(graphics->backend);
        if(graphicsFallback)
            config.fallbackBackend=mscharged::platform::AuroraGraphicsBackend(graphicsFallback->backend);
        // display.gpu: a chosen card; Aurora keeps the fastest when the API does not offer it.
        if(const auto card=mscharged::ParseGraphicsCard(launch.settings.gpu)) {
            config.adapterVendorId=card->vendor;
            config.adapterDeviceId=card->device;
        }
        config.windowWidth=windowWidth;config.windowHeight=windowHeight;
        config.windowPosX=config.windowPosY=-1;
        config.mem1Size=MEM1_DEFAULT_SIZE;
        config.mem2Size=64u*1024u*1024u;
        // Host presentation/diagnostic preferences; none reaches game code.
        const auto& logLevel=launch.settings.log_level;
        config.logLevel=logLevel=="debug"?LOG_DEBUG:logLevel=="warning"?LOG_WARNING:
            logLevel=="error"?LOG_ERROR:LOG_INFO;
        config.logCallback=mscharged::platform::ConsoleLog;
        config.enableBackendValidation=launch.settings.graphics_validation;
        config.vsync=launch.settings.vsync;
        // display.antialiasing: Aurora multisampling of the original frame,
        // optionally shaded per sample; FXAA is the scanout's (below).
        const auto& antialiasing=mscharged::FindAntialiasing(launch.settings.antialiasing);
        config.msaa=antialiasing.samples;
        config.sampleShading=antialiasing.sample_shading;
        // mods.dump_textures: Aurora saves each game texture no pack replaces
        // to data/texture_dumps, named for texture packs.
        config.allowTextureDumps=launch.settings.dump_textures;
        // The native Wii Remote driver (platform/wiimote_hid) owns physical
        // remotes, including DolphinBar mode 4 and its IR sensor bar; SDL's
        // own Wii HID driver must not open the same devices.
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_WII,"0",SDL_HINT_DEFAULT);
        // Startup milestones and display settings for logs/recharged.log.
        const auto& shown=launch.settings;
        mscharged::platform::SessionLogLine("Display settings: "+std::string(shown.fullscreen?"fullscreen":"windowed")
            +" "+std::to_string(windowWidth)+"x"+std::to_string(windowHeight)+", aspect "+shown.aspect
            +", 3D resolution "+shown.resolution+", antialiasing "+shown.antialiasing+", picture "+shown.picture
            +", vsync "+(shown.vsync?"on":"off")+", display "+std::to_string(shown.monitor)
            +", graphics card "+shown.gpu
            +", performance overlay "+(shown.fps_overlay?shown.fps_overlay_corner:std::string("off"))
            +", graphics validation "+(shown.graphics_validation?"on":"off"));
        mscharged::platform::SessionLogLine(std::string("Startup: initializing ")
            +graphics->label+" graphics and the window");
        shaderCacheUse.emplace(mscharged::platform::ShaderCacheDirectory(executableDirectory));
        const auto host=aurora_initialize(argc,argv,&config);
        if(!host.window)
            throw std::runtime_error(mscharged::GraphicsStartFailure(*graphics,graphicsFallback));
        if(host.backend!=config.desiredBackend) {
            Check(graphicsFallback && host.backend==config.fallbackBackend,"Aurora started an unrequested graphics API");
            mscharged::platform::SessionLogLine(std::string("Startup: ")+graphics->label+" could not start; using "
                +graphicsFallback->label);
            graphics=graphicsFallback;
        }
        mscharged::platform::SetApplicationIcon(host.window);
        mscharged::platform::SessionLogLine("Startup: graphics ready");
        // [mods]: HD texture packs, swapped in by Aurora (host graphics only).
        mscharged::LoadTexturePacks(launch.settings,launch.config.path,release->game_id);
        mscharged::platform::LogSessionDisplays(host.window);
        if(launch.settings.monitor>0) {
            // Start on the selected display; fullscreen then uses that display.
            int displayCount=0;
            SDL_DisplayID* displays=SDL_GetDisplays(&displayCount);
            // display.monitor: 0 keeps the default display, N selects the Nth display.
            if(displays && launch.settings.monitor<=displayCount) {
                const auto position=SDL_WINDOWPOS_CENTERED_DISPLAY(displays[launch.settings.monitor-1]);
                if(!SDL_SetWindowPosition(host.window,int(position),int(position)))
                    std::fprintf(stderr,"Selected display %d unavailable: %s\n",launch.settings.monitor,SDL_GetError());
            } else std::fprintf(stderr,"Selected display %d is not connected; using the default display.\n",
                launch.settings.monitor);
            SDL_free(displays);
        }
        if(!launch.settings.fullscreen) {
            // A window larger than its display's usable area (an INI written
            // for a bigger screen, or pixel sizes on a Retina Mac) shrinks to
            // fit, keeping its shape, and is centred on that display.
            const SDL_DisplayID display=SDL_GetDisplayForWindow(host.window);
            SDL_Rect usable{};
            if(display && SDL_GetDisplayUsableBounds(display,&usable)) {
                const auto fit=mscharged::platform::FitWindowSize(int(windowWidth),int(windowHeight),usable.w,usable.h);
                if(fit.width!=int(windowWidth) || fit.height!=int(windowHeight)) {
                    std::fprintf(stderr,"Window %ux%u does not fit the display (%dx%d usable); using %dx%d.\n",
                        windowWidth,windowHeight,usable.w,usable.h,fit.width,fit.height);
                    windowWidth=unsigned(fit.width);windowHeight=unsigned(fit.height);
                    SDL_SetWindowSize(host.window,fit.width,fit.height);
                    const auto centred=SDL_WINDOWPOS_CENTERED_DISPLAY(display);
                    SDL_SetWindowPosition(host.window,int(centred),int(centred));
                }
            }
        }
        mscharged::platform::SetNativeAIOutputGain(launch.settings.mute ? 0.0f :
            float(launch.settings.master_volume)/100.0f);
        mscharged::platform::SessionLogLine(launch.settings.fullscreen ? "Startup: switching to fullscreen"
                                                                        : "Startup: setting the window size");
        Check(SDL_SetWindowFullscreen(host.window,launch.settings.fullscreen), "Requested launch window mode rejected");
        if(nativeSend) {
            // Existing Aurora policy fixes the internal source EFB at 1x.
            // Original SC/VI signal aspect controls independent desktop output.
            Check(SDL_SetWindowMinimumSize(host.window,1,1), "SDL window minimum rejected");
            Check(SDL_SetWindowSize(host.window,static_cast<int>(windowWidth),static_cast<int>(windowHeight)),
                  "Requested window size rejected");
            // display.resolution: the source EFB's render size. Viewport, scissor,
            // copies and pixel accesses map the game's 640x448 coordinates onto it.
            const auto& resolution=launch.settings.resolution;
            efbScale=resolution=="window"?0.0f:resolution=="2x"?2.0f:resolution=="3x"?3.0f:
                resolution=="4x"?4.0f:1.0f;
            // window: one EFB pixel per window pixel of the displayed 4:3/16:9 picture.
            if(efbScale==0.0f) VILockAspectRatio(int(aspectWidth),int(aspectHeight));
            VISetFrameBufferScale(efbScale);
            AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        }
        // Warm the actual selected GPU/backend before any original module
        // constructors, clocks or input/audio owners. The helper pumps resize
        // events before preparing the final targets and keeps the window live.
        bool graphicsPrepared=false;
        try {
            graphicsPrepared=mscharged::runtime::PrepareGraphicsForGame(
                executableDirectory/"assets"/"launcher"/"Roboto-Medium.ttf");
        } catch(...) {
            aurora_shutdown();
            throw;
        }
        if(!graphicsPrepared) {
            aurora_shutdown();
            // [launcher] reopen_after_game: the launcher comes back instead (POSIX: in this process).
            if(!mscharged::platform::ReopenLauncher("closed during graphics preparation, reopening the launcher"))
                mscharged::platform::SessionLogLine("=== Session ended during graphics preparation ===");
            return 0;
        }
        bool retainedWindowClose=false;
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_RESET)
        if(sourceAudio && nativeSend) {
            mscharged::platform::RetainNativeHardwareWindowClose(host.window);
            retainedWindowClose=true;
        }
#endif
        if(nativeSend) {
            mscharged::platform::SessionLogLine("Startup: render size "
                +std::string(efbScale==0.0f?"follows the window":"fixed"));
            mscharged::platform::LogSessionDisplays(host.window);
        }
        OSInit();
        if(!OSGetArenaLo()||!OSGetMEM2ArenaLo())
            throw std::runtime_error("Actual captured SDK arenas unavailable");
        mscharged::platform::InitializeNativeInterruptController();
        // Native EXI hardware precedes the original SRAM cache initializer.
        // Preserve an existing image; first run explicitly uses the native
        // virgin policy, independently of disc/locale/SC preferences.
        mscharged::platform::InitializeNativeRTC(dataDirectory / "native-rtc.bin",
            mscharged::platform::CreateVirginRTCImage());
        __OSInitSram();
        Check(__OSSyncSram(), "Original SRAM initialization did not complete its real EXI read");
        // A European release boots like a PAL Wii set to 60 Hz (EuRGB60): the
        // original game then takes the USA picture, movie and pacing path.
        const bool eurgb60=release->code==mscharged::GameCode::Europe;
        const mscharged::NativeSystemSettings settings{systemLanguage,0,std::uint8_t(eurgb60),std::uint8_t(widescreen),1};
        mscharged::ConfigureNativeSystemSettings(settings);
        // No virtual Wii address record has been supplied. Original SC queries
        // retain their unavailable-record result, without inferring a country.
        mscharged::ConfigureNativeSystemSimpleAddress(std::nullopt);
        // No native IPL.IDL record is supplied. The original shutdown source
        // owns its zero/default destination and interprets actual absence.
        mscharged::ConfigureNativeSystemIdleMode(nullptr,0);
        mscharged::platform::ConfigureNativeVideoHardware(eurgb60?VI_TVMODE_EURGB60_INT:VI_TVMODE_NTSC_INT,false);
        if(nativeSend) {
            // display.picture: host desktop presentation of the original XFB,
            // with display.antialiasing's FXAA.
            mscharged::platform::ConfigureNativeVideoOutputHardware(settings,
                launch.settings.picture=="sharp" ? mscharged::platform::NativeDesktopPicture::Sharp
                : launch.settings.picture=="clean" ? mscharged::platform::NativeDesktopPicture::Clean
                                                   : mscharged::platform::NativeDesktopPicture::Signal,
                mscharged::FindAntialiasing(launch.settings.antialiasing).fxaa);
            // Arm source-owned recording before original constructors/commands.
            // This mode never uses diagnostic aurora_begin/end_frame.
            aurora_configure_native_gx_hardware();
        }
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
        // Each title keeps its own storage in the save folder (the USA one
        // the folder itself), as titles have separate homes on a Wii.
        const auto saveFolder=mscharged::TitleSaveFolder(
            mscharged::ResolveSaveFolder(launch.settings, launch.config.path, executableDirectory),
            mscharged::ReadDiscTitleId(launch.disc_path));
        const auto discBootIdentity=mscharged::platform::InitializeNativeFilesystemForDisc(
            {launch.disc_path, saveFolder, 0x1001});
        mscharged::platform::InitializeNativeSTMDevice();
        // Report this live host IOS provider, not the disc-requested Wii IOS.
        mscharged::platform::ConfigureNativeIOSServiceRevision();
        if(!__OSInitSTM())throw std::runtime_error("Actual original STM initialization failed");
        // Borrow input into the existing AI/SDK hardware owner; source KPAD
        // and FE methods retain their original mappings and decisions. The
        // keyboard remote carries a virtual Nunchuk, which original matches
        // require of every playing controller; the mouse is its pointer.
        // Physical Wii Remotes (platform/wiimote_hid) bring their own IR
        // camera and Nunchuk. controls.rumble is the system rumble setting
        // the game reads (the HOME Menu's); controls.sensor_bar the sensor-bar
        // position (bottom/top) original KPAD aims the pointer from and
        // controls.ir_sensitivity the Wii's sensitivity (1-5);
        // controls.speaker_volume the Remotes' speaker volume (BT.SPKV, 0-127
        // as the HOME Menu's 10 steps of 12.7; 0 sends no speaker sound).
        // controls.player1-4 fix who plays as which player.
        mscharged::platform::NativePlayers players{};
        players.fixed = true;
        players.keyboard = -1;
        players.mouse_with_controller = launch.settings.mouse_pointer;
        for (int player = 0; player < 4; ++player) {
            const auto& device = launch.settings.players[player];
            if (device == "keyboard") players.keyboard = player;
            else if (device.rfind("gamepad", 0) == 0) players.gamepads[device.back() - '1'] = player;
            else if (device != "off") players.remotes[device] = player;
        }
        // [hotkeys]: the screenshot and HUD keys, which the keyboard player never presses.
        const auto hotkeys=mscharged::platform::ParseHotkeys(launch.settings.screenshot_key,launch.settings.hud_key);
        mscharged::platform::SetHotkeys(hotkeys);
        mscharged::platform::DesktopWpadSettings desktop{true,false,true,mscharged::platform::QueryPresentedDesktopDpd,
            nullptr,true,true,-1,mscharged::platform::ParseKeyBindings(launch.settings.keys,hotkeys)};
        desktop.kpad_sensor_height=release->code==mscharged::GameCode::Usa;
        desktop.rumble_strength=launch.settings.rumble_strength;
        // Each gamepad's profile (controls.gamepadN_profile).
        for (std::size_t pad = 0; pad < desktop.gamepad_profiles.size(); ++pad) {
            const auto& profile = launch.settings.PadProfileOf(pad);
            desktop.gamepad_profiles[pad] = {mscharged::platform::ParseGamepadBindings(profile.inputs),
                                             profile.swap_sticks, launch.settings.PadProfileIndex(pad) < 0,
                                             profile.inner_deadzone, profile.outer_deadzone, profile.trigger_press,
                                             profile.gyro_pointer, profile.gyro_speed};
        }
        mscharged::platform::WpadSDLSettings wpad{static_cast<std::uint8_t>(launch.settings.sensor_bar=="top"?1:0),
            static_cast<std::uint8_t>(launch.settings.ir_sensitivity),true,launch.settings.rumble};
        wpad.speaker_volume=static_cast<std::uint8_t>((launch.settings.speaker_volume*127+50)/100);
        wpad.remote_speaker=launch.settings.speaker_volume>0;
        // Original WPADControlSpeaker ON: 0x80 in the American game, 0x08 in the European one.
        wpad.remote_speaker_reg01=release->code==mscharged::GameCode::Usa ? 0x80 : 0x08;
        mscharged::platform::InitializeNativeHardwareInput(host.window,wpad,
            mscharged::platform::GetNativeSTMInput(), desktop, players);
        {
            const auto& s=launch.settings;
            const std::string line="Wii Remote settings: sensor bar "+s.sensor_bar+", sensitivity "
                +std::to_string(s.ir_sensitivity)+", speaker "+(s.speaker_volume ? std::to_string(s.speaker_volume)+"%" : "off")
                +", rumble "+(s.rumble ? "on (gamepads "+std::to_string(s.rumble_strength)+"%)" : "off");
            mscharged::platform::SessionLogLine(line);
            mscharged::platform::SessionLogLine("Hotkeys: screenshot "+mscharged::platform::KeyInputName(hotkeys.screenshot)
                +", HUD "+mscharged::platform::KeyInputName(hotkeys.hud)+" (matches start with the HUD "
                +(s.show_hud ? "shown)" : "hidden)"));
            for (const auto& device : s.players) {
                if (device.rfind("gamepad", 0) != 0) continue;
                const std::size_t pad = std::size_t(device.back() - '1');
                const auto& profile = s.PadProfileOf(pad);
                mscharged::platform::SessionLogLine("Gamepad " + std::to_string(pad + 1) + ": profile " + profile.name
                    + ", stick dead zones " + std::to_string(profile.inner_deadzone) + "-"
                    + std::to_string(profile.outer_deadzone) + "%, trigger press point "
                    + std::to_string(profile.trigger_press) + "%, pointer "
                    + (profile.gyro_pointer ? "gyro (speed " + std::to_string(profile.gyro_speed) + "%)"
                                            : std::string(profile.swap_sticks ? "left stick" : "right stick")));
            }
        }
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio)
            mscharged::diagnostic::InitializeOriginalGameAudioHardware(
                mscharged::platform::ServiceNativeHardwareInput);
        else
#endif
            mscharged::diagnostic::InitializeMovieHardware(
                mscharged::platform::ServiceNativeHardwareInput);
        if(!aurora_dvd_open(mscharged::PathUtf8(disc).c_str())) throw std::runtime_error("Actual owned Wii data partition failed");
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_RESET)
        mscharged::platform::ConfigureNativeOSDiscBootIdentity(discBootIdentity);
        std::optional<mscharged::diagnostic::OriginalTerminalVerifier> terminalVerifier;
#endif
        std::unique_ptr<mscharged::platform::NativeAXModuleMemory> axModuleMemory;
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        std::unique_ptr<mscharged::diagnostic::OriginalGameAudioHardware> gameAudioHardware;
#endif
        // Terminal diagnostics retain live source images and hardware owners on
        // failure. Unwinding a device with active source pins is not retirement.
        try {
        axModuleMemory=std::make_unique<mscharged::platform::NativeAXModuleMemory>(modulePath);
        auto* module=mscharged::platform::LoadNativeModule(modulePath);
        {
            // The module's sources must have been built for the disc's game version.
            auto* moduleVersion=reinterpret_cast<const char*(*)()>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_game_version"));
            const char* expectedVersion=mscharged::GameModuleVersion(release->code);
            Check(moduleVersion && std::strcmp(moduleVersion(),expectedVersion)==0,
                  "The game module does not match the disc's game version");
        }
        // RECHARGED_DEBUG_LAYERS_OFF: diagnostic layer switches.
        if(auto* layers=reinterpret_cast<void(*)()>(mscharged::platform::FindNativeModuleSymbol(
               module,"charged_apply_debug_layer_toggles"))) {
            layers();
            mscharged::platform::SetDebugLayerKeys(
                reinterpret_cast<void(*)()>(mscharged::platform::FindNativeModuleSymbol(module,"charged_toggle_debug_layers")),
                reinterpret_cast<void(*)()>(mscharged::platform::FindNativeModuleSymbol(module,"charged_cycle_debug_layer")));
        }
        // [gameplay] tweaks: the enabled set, before original main, with
        // the No HUD bit of [hotkeys] show_hud = false.
        {
            const unsigned tweaks=launch.settings.GameTweakMask();
            auto* configureTweaks=reinterpret_cast<void(*)(unsigned)>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_tweaks"));
            if(configureTweaks) configureTweaks(launch.settings.ModuleTweakMask());
            const auto line="Tweaks: "+(configureTweaks || !tweaks ? mscharged::DescribeGameTweaks(tweaks)
                : "none (this build's game module has no tweaks)");
            mscharged::platform::SessionLogLine(line.c_str());
            std::fprintf(stderr,"%s\n",line.c_str());
            // Texture packs: Mirror match's recoloured textures in HD.
            if(auto* configureRecolour=reinterpret_cast<void(*)(ChargedRecolouredTextureHook)>(
                   mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_hd_recolour")))
                configureRecolour(mscharged::TexturePackRecolourHook());
            // No HUD (issue #58): the HUD key shows or hides the match HUD, whatever show_hud says.
            mscharged::platform::SetHudKey(reinterpret_cast<void(*)()>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_toggle_hud")));
            // Robot goalies: the map that lays the disc's unused robot goalie
            // texture onto Charged's goalie. The module keeps a view of it.
            if(tweaks & mscharged::GameTweakBit(mscharged::GameTweak::RobotGoalies)) {
                static std::vector<unsigned char> robotGoalieMap;
                const auto path=executableDirectory/"assets"/"gameplay"/"robot_goalie.map";
                std::error_code error;
                const auto size=std::filesystem::file_size(path,error);
                if(!error && size>0 && size<=(16u<<20)) {
                    robotGoalieMap.resize(size_t(size));
                    std::ifstream file(path,std::ios::binary);
                    if(!file.read(reinterpret_cast<char*>(robotGoalieMap.data()),std::streamsize(size)))
                        robotGoalieMap.clear();
                }
                auto* configureRobotGoalie=reinterpret_cast<bool(*)(const void*,unsigned long)>(
                    mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_robot_goalie"));
                const bool ready=configureRobotGoalie && !robotGoalieMap.empty()
                    && configureRobotGoalie(robotGoalieMap.data(),(unsigned long)robotGoalieMap.size());
                const std::string robotLine=std::string("Robot goalies: ")+(ready ? "on"
                    : !configureRobotGoalie ? "off (this build's game module has no robot goalies)"
                    : "off (assets/gameplay/robot_goalie.map is missing or not valid)");
                mscharged::platform::SessionLogLine(robotLine.c_str());
                std::fprintf(stderr,"%s\n",robotLine.c_str());
            }
            // Super Team: the GameCube robot's model, skeleton and textures from
            // the player's Super Mario Strikers disc. The module keeps a view of them.
            if(tweaks & mscharged::GameTweakBit(mscharged::GameTweak::SuperTeam)) {
                static std::vector<std::vector<std::uint8_t>> superTeamFiles;
                std::string why;
                const auto gamecube=mscharged::ResolveGameCubeDiscPath(launch.settings,launch.config.path);
                if(gamecube.empty())
                    why="no Super Mario Strikers (GameCube) disc is set ([game] gamecube_disc)";
                else {
                    try {
                        superTeamFiles=mscharged::ReadStrikersFiles(gamecube,
                            {"art/characters/SuperTeam/SuperTeam.glg","art/animation/SuperTeam.shier",
                             "art/characters/SuperTeam/SuperTeam.glt"},16u<<20);
                    } catch(const std::exception& error) {
                        why=error.what();
                        superTeamFiles.clear();
                    }
                }
                auto* configureSuperTeam=reinterpret_cast<bool(*)(const void*,unsigned long,const void*,unsigned long,
                    const void*,unsigned long,int)>(
                    mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_super_team"));
                const std::string& side=launch.settings.super_team_side;
                const int sides=side=="home" ? 1 : side=="both" ? 3 : 2;
                bool ready=false;
                if(!configureSuperTeam) why="this build's game module has no Super Team";
                else if(superTeamFiles.size()==3) {
                    const auto& f=superTeamFiles;
                    ready=configureSuperTeam(f[0].data(),(unsigned long)f[0].size(),f[1].data(),(unsigned long)f[1].size(),
                        f[2].data(),(unsigned long)f[2].size(),sides);
                    if(!ready) why="the GameCube disc's Super Team files are not the expected ones";
                }
                const std::string superTeamLine=std::string("Super Team: ")
                    +(ready ? "on ("+side+(side=="both" ? " teams)" : " team)") : "off ("+why+")");
                mscharged::platform::SessionLogLine(superTeamLine.c_str());
                std::fprintf(stderr,"%s\n",superTeamLine.c_str());
            }
        }
        // European revision 1 disc: its data runs on the revision 2 code
        // through the tweak series' adapters, whatever the tweak set.
        if(release->rev1_data) {
            auto* configureRev1=reinterpret_cast<void(*)(bool)>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_rev1_data"));
            Check(configureRev1!=nullptr,"This build's game module cannot run European revision 1 data");
            configureRev1(true);
            mscharged::platform::SessionLogLine("Release data: revision 1 on the revision 2 game code");
        }
        {
            auto* configurePerformanceMode=reinterpret_cast<void(*)(bool)>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_performance_mode"));
            if(configurePerformanceMode)
                configurePerformanceMode(launch.settings.performance_mode);
            const std::string line="Performance mode: "+std::string(launch.settings.performance_mode?"on":"off")
                +(launch.settings.performance_mode && !configurePerformanceMode
                    ? " (only antialiasing: this build's game module has no Performance mode)" : "");
            mscharged::platform::SessionLogLine(line.c_str());
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        // [audio] sound levels: like the master volume a host setting,
        // whatever the tweak set; the game module's hooks apply them.
        {
            const auto& volumes=launch.settings.sound_volumes;
            auto* configureSoundVolumes=reinterpret_cast<void(*)(const int*,int)>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_sound_volumes"));
            if(configureSoundVolumes)
                configureSoundVolumes(volumes.data(),int(volumes.size()));
            const auto described=mscharged::DescribeSoundVolumes(volumes);
            const std::string line="Sound levels: "+described
                +(described!="original" && !configureSoundVolumes
                    ? " (not applied: this build's game module has no sound levels)" : "");
            mscharged::platform::SessionLogLine(line);
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        // The logo reads RECHARGED (issue #63): always on, whatever the tweak
        // set; RECHARGED_ORIGINAL_LOGO=1 keeps the game's own (a kill switch).
        {
            const char* original=std::getenv("RECHARGED_ORIGINAL_LOGO");
            const bool keepOriginal=original && *original && std::strcmp(original,"0")!=0;
            auto* configureLogo=reinterpret_cast<void(*)(bool)>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_title_logo"));
            if(configureLogo)
                configureLogo(!keepOriginal);
            const std::string line=std::string("Title logo: ")+(keepOriginal ? "original (RECHARGED_ORIGINAL_LOGO)"
                : configureLogo ? "RECHARGED" : "original (this build's game module has no RECHARGED logo)");
            mscharged::platform::SessionLogLine(line);
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        // display.frame_rate: above the original, Aurora
        // presents each frame as the game swaps and paces the next; VI retraces
        // keep their real rate for everything that counts them.
        std::optional<mscharged::runtime::HostFramePacing> framePacing;
        {
            const auto rate=mscharged::ParseFrameRate(launch.settings.frame_rate).value_or(mscharged::FrameRate{});
            auto* configurePacing=reinterpret_cast<void(*)(void(*)())>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_frame_pacing"));
            std::string line="Frame rate: ";
            if(rate.kind==mscharged::FrameRate::Kind::Original)
                line+=mscharged::DescribeFramePacing(rate,{},0.0);
            else if(!configurePacing || !nativeSend)
                line+="original (this build's game module has no frame pacing)";
            else {
                framePacing.emplace(rate,launch.settings.vsync,host.window);
                configurePacing(aurora_paced_video_swap);
                line+=framePacing->Description();
            }
            mscharged::platform::SessionLogLine(line);
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        // The Discord status (issue #71): the game module reports what the
        // game is doing; the host shows it on this computer's Discord app.
        // A host setting like the sound levels, whatever the tweak set.
        auto presenceSnapshot=reinterpret_cast<ChargedPresenceSnapshotFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_presence_snapshot"));
        std::optional<mscharged::platform::DiscordPresence> discordPresence;
        std::optional<mscharged::platform::GamePresence> gamePresence;
        {
            const bool ownApplication=!launch.settings.discord_application_id.empty();
            const std::string application=ownApplication ? launch.settings.discord_application_id
                : std::string(mscharged::build::discord_application_id);
            std::string line="Discord status: ";
            if(!launch.settings.discord_status)
                line+="off";
            else if(application.empty())
                line+="off (this build has no Discord application)";
            else if(!presenceSnapshot)
                line+="off (this build's game module reports no game state)";
            else {
                mscharged::platform::DiscordPresence::Options options;
                options.application_id=application;
                options.log=[](const std::string& text) {
                    mscharged::platform::SessionLogLine(text);
                    std::fprintf(stderr,"%s\n",text.c_str());
                };
                options.shown=[](const std::optional<mscharged::platform::discord::Activity>& activity) {
                    if(activity)
                        mscharged::platform::Trace("Discord: showing \"%s\" / \"%s\"%s.\n",activity->details.c_str(),
                            activity->state.c_str(),activity->start ? " with a timer" : "");
                };
                discordPresence.emplace(std::move(options));
                const auto started=std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                gamePresence.emplace(release->title,release->region,release->game_id,started);
                line+=std::string("on")+(ownApplication ? " (application from recharged.ini)" : "");
            }
            mscharged::platform::SessionLogLine(line);
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        if(axModuleMemory)axModuleMemory->ConfirmLoaded(module);
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio)
            gameAudioHardware=std::make_unique<mscharged::diagnostic::OriginalGameAudioHardware>(*axModuleMemory);
#endif
        // Disc-message images (the loading message and the DVD error screens):
        // the game module only reserves their arrays and the player's own
        // main.dol fills them, as the port ships no game data. Modules without
        // this export still embed their images. A modified main.dol (a
        // Wiimmfi-patched image) is fine: the game never runs it.
        if(auto* discMessages=reinterpret_cast<mscharged::DiscMessageImageStorage>(
               mscharged::platform::FindNativeModuleSymbol(module,"charged_disc_message_image")))
            mscharged::platform::SessionLogLine(mscharged::InstallDiscMessages(
                mscharged::ReadDiscMessages(launch.disc_path,*release),discMessages));
        auto entry=reinterpret_cast<int(*)()>(mscharged::platform::FindNativeModuleSymbol(module,"charged_original_entry"));
        if(!entry)throw std::runtime_error("Original source main export unavailable");
        mscharged::platform::Trace("Entering actual source main with real Aurora %s/FIFO owner and native PI_VI under temporary MAIN_BOOTSTRAP; flow remains incomplete.\n",
                     graphics->label);
        mscharged::platform::SessionLogLine("Startup: game module loaded, entering the game");
        std::fflush(nullptr);
        const int result=entry();
        Check(result==85, "Original main selected scene did not complete checkpoint 85");
        // Host AI delivery/starvation observations; they supply no PCM or callback.
        mscharged::platform::BeginNativeAIObservations();
        using ObserveAudio = unsigned(*)();
        using AudioAction = void(*)();
        using AudioIdle = bool(*)();
        auto observeAudio=sourceAudio ? reinterpret_cast<ObserveAudio>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_audio_observe")) : nullptr;
        auto shutdownAudio=sourceAudio ? reinterpret_cast<AudioAction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_audio_shutdown")) : nullptr;
        auto idleAudio=sourceAudio ? reinterpret_cast<AudioIdle>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_audio_idle")) : nullptr;
        auto idleBankReads=sourceAudio ? reinterpret_cast<bool(*)()>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_audio_bank_reads_idle")) : nullptr;
        auto unloadIdleBanks=sourceAudio ? reinterpret_cast<AudioAction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_audio_unload_idle_banks")) : nullptr;
        auto retireAudioPin=sourceAudio ? reinterpret_cast<AudioAction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_audio_retire_silence_pin")) : nullptr;
        if(sourceAudio) {
            Check(observeAudio && shutdownAudio && idleAudio && idleBankReads && unloadIdleBanks && retireAudioPin,
                  "Original audio source observations and retirement exports unavailable");
            // True source predicates, before the first original task/frame.
            Check(observeAudio()==7u,
                  "Original audio owner/initialization/configuration callback incomplete");
            mscharged::platform::Trace("Original main audio: source owner initialized, nlxgs configuration genuinely loaded.\n");
        }
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_RESET)
        if(sourceAudio && nativeSend) {
            terminalVerifier.emplace(*gameAudioHardware,*axModuleMemory,host.window);
            auto removal=terminalVerifier->Policy();
            // The opt-in frame timing log completes before the process ends.
            removal.before_removal=[](void*){
                mscharged::runtime::FrameTimingLog::FinishForProcessExit();
                mscharged::runtime::LogGraphicsPipelineStats();
                // The power-off path ends with TerminateProcessNow: hand the last lines to the log and console.
                // [launcher] reopen_after_game starts the launcher first (POSIX: in this process).
                if(!mscharged::platform::ReopenLauncher("original power off, reopening the launcher")) {
                    mscharged::platform::SessionLogLine("=== Session ended (original power off) ===");
                    mscharged::platform::FlushSessionLog();
                }
            };
            mscharged::platform::ConfigureNativeSTMPowerRemoval(removal);
            // The actual source entry/audio predicates and terminal policy now
            // exist. Only this host-window intent was deferred; service/tasks
            // deliver it through unchanged original OSStateTM/ResetTask.
            mscharged::platform::ArmNativeHardwareWindowClose();
        }
#endif
        auto observeBootScript=sourceBoot ? reinterpret_cast<ObserveAudio>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_boot_script_observe")) : nullptr;
        auto bootInstruction=sourceBoot ? reinterpret_cast<int(*)()>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_boot_script_instruction")) : nullptr;
        auto bootPhase=sourceBoot ? reinterpret_cast<int(*)()>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_boot_scene_phase")) : nullptr;
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
                mscharged::platform::Trace("Original authored Boot: VM flags%u instruction%d phase%d.\n",flags,instruction,phase);
                std::fflush(stdout);
                lastBootFlags=flags;lastBootInstruction=instruction;lastBootPhase=phase;
            }
            bootReady=flags==31u && phase==4 && idleAudio();
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
            if(bootReady)bootReady=gameAudioHardware->Status().last_active_voices==0;
#endif
        };
        auto frame=reinterpret_cast<void(*)(float)>(mscharged::platform::FindNativeModuleSymbol(module,nativeSend?"charged_original_scene_native_frame":"charged_original_scene_frame"));
        Check(frame,"Same original-main module scene-frame export unavailable");
        auto stopMovie=reinterpret_cast<void(*)()>(mscharged::platform::FindNativeModuleSymbol(module,"charged_original_scene_stop_movie"));
        Check(stopMovie,"Original main movie-stop export unavailable");
        using ObserveSH = std::uint32_t(*)(OriginalSHSnapshot*,std::uint32_t);
        using RequestSH = std::uint32_t(*)();
        auto observeSH=optionsScene ? reinterpret_cast<ObserveSH>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_sh_observe")) : nullptr;
        auto requestSH=optionsScene ? reinterpret_cast<RequestSH>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_original_sh_request_options")) : nullptr;
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
                mscharged::platform::Trace("Original SH: scene%d resources%d depth%u, owners%u navigation%u pointers%u.\n",
                    top.scene_id,top.resource_state,sh.stack_depth,sh.owners,
                    sh.navigation_ready,sh.pointer_instances);
                std::fflush(stdout);lastScene=top.scene_id;lastState=top.resource_state;
            }
            optionsReady=top.scene_id==13 && top.resource_state==6 && top.visible &&
                sh.options_initialized && sh.options_state==1 && top.slide_time>=0.2f;
            if(optionsReady && !optionsBoundsReported) {
                for(unsigned i=0;i<3;++i) {
                    const auto& b=sh.buttons[i];
                    mscharged::platform::Trace("Original Options button%u: %.6g..%.6g, %.6g..%.6g; slide%.6g/%.6g, input%u/%u lock%d.\n",
                        i,b.min_x,b.max_x,b.min_y,b.max_y,top.slide_time,top.slide_duration,
                        sh.input_allowed,sh.input_enabled,sh.input_lock_depth);
                }
                std::fflush(stdout);optionsBoundsReported=true;
            }
        };
        // Once a second, after a game frame: the game's state for the Discord status.
        std::chrono::steady_clock::time_point lastPresence{};
        auto observePresence = [&] {
            if(!discordPresence)return;
            const auto now=std::chrono::steady_clock::now();
            if(now-lastPresence<std::chrono::seconds(1))return;
            lastPresence=now;
            ChargedPresenceSnapshot snapshot{};
            if(!presenceSnapshot(&snapshot,sizeof snapshot))return;
            const auto unixNow=std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            discordPresence->Show(gamePresence->Describe(snapshot,unixNow));
        };
        // Existing actual source GX/state/material/font/view initialization is
        // retained. No host GXInit, source pool restart or fixture font setup.
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        unsigned frames=0,draws=0,quiet=0; bool snapshot=false,encoded=false,exit=false,nativePresented=false;
        unsigned fieldSlots[4]{}; std::uint32_t lastRetrace=0; bool retraceKnown=false;
        bool bootLogoCaptured=false;
        const auto start=std::chrono::steady_clock::now(); auto previous=start;
        auto stageStart=start; unsigned resizeStage=0; bool stageAnnounced=false;
        // Original game frames per second, shown only in the optional overlay.
        mscharged::runtime::FrameRateMeter frameRate;
        std::optional<mscharged::runtime::FpsOverlay> fpsOverlay;
        if(launch.settings.fps_overlay && nativeSend)
            fpsOverlay.emplace(executableDirectory/"assets"/"launcher"/"Roboto-Medium.ttf",
                mscharged::runtime::ParseOverlayCorner(launch.settings.fps_overlay_corner)
                    .value_or(mscharged::runtime::OverlayCorner::TopRight));
        // The persisted launcher toggle is authoritative; recordings live with
        // the session log (logs/ beside this executable), independently of the
        // working directory.
        const auto sessionLog=mscharged::platform::SessionLogPath();
        const auto frameTiming=mscharged::runtime::FrameTimingLog::FromSettings(launch.settings.frame_logging,
            sessionLog.empty() ? executableDirectory/"logs" : sessionLog.parent_path());
        auto* takeStadium=reinterpret_cast<int(*)()>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_take_stadium"));
        if(frameTiming)
            frameTiming->SetContext(std::string("Session: version ")+mscharged::build::version+" ("
                +mscharged::build::channel+")\nSettings: "+(shown.fullscreen?"fullscreen":"windowed")+" "
                +std::to_string(windowWidth)+"x"+std::to_string(windowHeight)+", graphics "+graphics->label
                +", 3D resolution "+shown.resolution+", antialiasing "+shown.antialiasing
                +", frame rate "+shown.frame_rate+", vsync "+(shown.vsync?"on":"off")
                +", performance mode "+(shown.performance_mode?"on":"off")+"\n");
        wgpu::Texture retainedEFB;
        Check(!resizeCheck || nativeSend, "Resize qualification requires source-native send");
        while(!exit && (interactive || !snapshot)) {
            for(const auto* event=aurora_update();event->type!=AURORA_NONE;++event) {
                if(event->type!=AURORA_EXIT)continue;
#if defined(MSCHARGED_HAS_ORIGINAL_FRONTEND_RESET)
                if(terminalVerifier) {
                    // The native input watch has retained the genuine power
                    // event. Keep source tasks/owners alive for ResetTask.
                    continue;
                }
#endif
                exit=true;
            }
            if(exit)break;
            // Alt+Enter (Option+Return on a Mac): fullscreen or a window, for this run (issue #77).
            switch(mscharged::platform::ServiceFullscreenHotkey()) {
            case mscharged::platform::FullscreenSwitch::Fullscreen:
                mscharged::platform::SessionLogLine(std::string("Display: fullscreen (")
                    +mscharged::platform::kFullscreenShortcut+")"); break;
            case mscharged::platform::FullscreenSwitch::Windowed:
                mscharged::platform::SessionLogLine(std::string("Display: windowed (")
                    +mscharged::platform::kFullscreenShortcut+")"); break;
            case mscharged::platform::FullscreenSwitch::Failed:
                mscharged::platform::SessionLogLine(std::string("Display: ")+mscharged::platform::kFullscreenShortcut
                    +" could not switch: "+SDL_GetError()); break;
            case mscharged::platform::FullscreenSwitch::None: break;
            }
            mscharged::diagnostic::ServiceMovieHardware();
            if(requestSH) {
                const auto status=requestSH();
                if(status!=lastRequest) {
                    mscharged::platform::Trace("Original Options selection status%u (7 waits for the original audio context owner).\n",status);
                    std::fflush(stdout);lastRequest=status;
                }
            }
            const auto now=std::chrono::steady_clock::now();
            if(framePacing) framePacing->Update(now);
            if(fpsOverlay)
                if(const auto rate=frameRate.Sample(frames,now)) fpsOverlay->SetRate(*rate);
            if(fpsOverlay) {
                const auto size=aurora_get_window_size();
                fpsOverlay->Update(size.native_fb_width,size.native_fb_height);
            }
            Check(interactive || now-start<std::chrono::seconds(sourceBoot?90:40),"Original source scene diagnostic timed out");
            if(nativeSend) {
                {
                    // Host cadence observation: VI fields spent per source frame.
                    AuroraVIHardwareState vi{};
                    if(aurora_get_video_hardware_state(&vi)) {
                        if(retraceKnown) ++fieldSlots[std::min(vi.retrace_count-lastRetrace,3u)];
                        lastRetrace=vi.retrace_count; retraceKnown=true;
                    }
                }
                const auto receipt=AuroraGXBeginDrawReceipt();
                Check(receipt,"Native source draw receipt unavailable");
                const float delta=std::chrono::duration<float>(now-previous).count(); previous=now;
                // Keep the GX FIFO buffer at about one frame when the processor has
                // caught up, instead of growing it to the full-drain limit below.
                aurora::gx::fifo::rebase_if_caught_up();
                if(frameTiming)frameTiming->BeginFrame();
                frame(delta); observeOptions(); observeBoot(); observePresence(); AuroraGXEndDrawReceipt(); ++frames;
                if(frameTiming){ if(takeStadium)frameTiming->SetArena(takeStadium()); frameTiming->EndFrame(ReadFrameTimingCounters()); }
                // Nothing else drains once no host observation is due: without
                // this the buffer grows every frame until Aurora aborts on its
                // 4 GiB size limit. Draining waits only for the queued backlog.
                if(aurora::gx::fifo::get_buffer_size()>=kFifoDrainBytes) AuroraGXSync();
                // The original frame performs every GX/VI wait it needs. Join the
                // FIFO/render workers only for a pending, usable host observation.
                const bool logoDue=sourceBoot && lastBootPhase==3 && !bootLogoCaptured &&
                    now-bootLogoStart>=std::chrono::milliseconds(600);
                const bool stageDue=!stageAnnounced && now-stageStart>=std::chrono::milliseconds(500);
                const bool captureDue=!snapshot &&
                    now-stageStart>=std::chrono::seconds(resizeStage?8:3) &&
                    (!optionsScene || optionsReady) && bootReady;
                if(!logoDue && !stageDue && !captureDue) {
                    std::this_thread::yield(); continue;
                }
                // This mutex-protected completion is only an eligibility probe.
                // Keep both joins and the retained snapshot query below before
                // reading worker-owned draw statistics/EFB storage or pixels.
                AuroraVIPresentedState observed{};
                if(!aurora_get_presented_video_output_state(&observed) ||
                   !observed.framebuffer || observed.black || !observed.copy_revision ||
                   observed.presentation_sequence<3) {
                    std::this_thread::yield(); continue;
                }
                AuroraGXSync(); aurora::gfx::synchronize();
                encoded=encoded || AuroraGXWasDrawEncoded(receipt);
                draws+=aurora_get_stats()->drawCallCount;
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
                    // Only the controlled --resize-check run owns the window size;
                    // in play the player may resize or maximize it at any time.
                    Check(!resizeCheck || (size.width==expectedWidth && size.height==expectedHeight),
                          "Controlled native window resize did not reach its requested actual size");
                    // A fixed resolution keeps one EFB through resizes; "window"
                    // follows the window, at least the Wii's 640x448.
                    const auto efbWidth=efbScale>0.0f?unsigned(std::lround(640.0f*efbScale)):size.fb_width;
                    const auto efbHeight=efbScale>0.0f?unsigned(std::lround(448.0f*efbScale)):size.fb_height;
                    Check(!resizeCheck || (size.fb_width==efbWidth && size.fb_height==efbHeight && efbWidth>=640 &&
                          efbHeight>=448 && efb.size.width==efbWidth && efb.size.height==efbHeight),
                          "Window resize changed original source EFB geometry");
                    if(resizeCheck && efbScale>0.0f) {
                        if(retainedEFB) Check(efb.texture.Get()==retainedEFB.Get(),
                                             "Output resize replaced the original source EFB texture");
                        else retainedEFB=efb.texture;
                    }
                    const auto viewport=aurora::webgpu::calculate_present_viewport(
                        size.native_fb_width,size.native_fb_height,aspectWidth,aspectHeight);
                    mscharged::platform::Trace("Native resize stage%u: surface%ux%u EFB%ux%u same-storage, SC%u:%u viewport %.0f,%.0f %.0fx%.0f.\n",
                        resizeStage,size.native_fb_width,size.native_fb_height,efb.size.width,efb.size.height,
                        aspectWidth,aspectHeight,
                        viewport.left,viewport.top,viewport.width,viewport.height);
                    std::fflush(stdout); stageAnnounced=true;
                }
                if(captureDue && nativePresented && encoded) {
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
            frame(delta); observeOptions(); observeBoot(); observePresence(); AuroraGXEndDrawReceipt();
            const bool sample=!snapshot && now-start>=std::chrono::seconds(3) && quiet>=2 &&
                (!optionsScene || optionsReady) && bootReady;
            if(sample){EndWithSnapshot(dataDirectory/"original-main-efb.ppm");snapshot=true;}
            else aurora_end_frame();
            aurora::gfx::synchronize();
            encoded=encoded || AuroraGXWasDrawEncoded(receipt);
            draws+=aurora_get_stats()->drawCallCount;++frames;
            quiet=mscharged::platform::GetQueuedPipelineCount()?0:quiet+1;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Explicit terminal join: no queued GX or presentation work outlives the loop.
        if(nativeSend){AuroraGXSync(); aurora::gfx::synchronize();}
        Check(snapshot && encoded && draws &&
              (nativeSend ? nativePresented : aurora_get_last_presentation().sequence != 0),
              "Original main real source draw/presentation incomplete");
        if(nativeSend) {
            mscharged::platform::Trace("Original main selected %s: %u native source-send frames, draw receipt+chosen-XFB VI presentation. Full startup/AX/world/reset/CRT remain incomplete.\n",
                sourceBoot ? "authored Boot script0" : optionsScene ? "Options tasks" : "Boot/Intro tasks",frames);
        } else {
        mscharged::platform::Trace("Original main live: %u actual source frames, %u encoded draws; one initialized game/SDK, native owner elapsed time. Actual original THP/movie/mode0 audio; digital input endpoints selected; full task/automatic swap reset/AX/motion/CRT scopes remain held.\n",frames,draws);
        }
        const auto audio=mscharged::platform::GetNativeAIStatus();
        const auto audioClock=mscharged::platform::GetNativeAIClockStatus();
        Check(audio.initialized && audio.consumed_blocks && audio.dispatched_callbacks,
              "Original-main movie audio did not reach the actual host device");
        mscharged::platform::Trace("Original-main native AI: submitted%llu consumed%llu callbacks%llu, rate%d; coalesced%llu, maximum DMA service gap%lluns.\n",
                    static_cast<unsigned long long>(audio.submitted_blocks),
                    static_cast<unsigned long long>(audio.consumed_blocks),
                    static_cast<unsigned long long>(audio.dispatched_callbacks),audio.input_frequency,
                    static_cast<unsigned long long>(audioClock.coalesced_edges),
                    static_cast<unsigned long long>(audioClock.maximum_service_gap_ns));
        mscharged::platform::Trace("Native audio device: rate%d, period%d frames, queued%d source bytes.\n",
                    audio.device_frequency,audio.device_frames,audio.queued_input_bytes);
        mscharged::platform::EndNativeAIObservations();
        const auto delivery=mscharged::platform::GetNativeAIDeliveryStatus();
        mscharged::platform::Trace("Native AI delivery: latched%llu replayed%llu coalesced%llu held%llu (max%lluns) skipped%llu cells, "
                    "callback latency max%lluns; source silent blocks%llu zero-tail blocks%llu/frames%llu; device short pulls%llu.\n",
                    static_cast<unsigned long long>(delivery.latched_blocks),
                    static_cast<unsigned long long>(delivery.replayed_latches),
                    static_cast<unsigned long long>(delivery.coalesced_causes),
                    static_cast<unsigned long long>(delivery.held_boundaries),
                    static_cast<unsigned long long>(delivery.maximum_hold_ns),
                    static_cast<unsigned long long>(delivery.skipped_cells),
                    static_cast<unsigned long long>(delivery.maximum_dispatch_latency_ns),
                    static_cast<unsigned long long>(delivery.silent_blocks),
                    static_cast<unsigned long long>(delivery.zero_tail_blocks),
                    static_cast<unsigned long long>(delivery.zero_tail_frames),
                    static_cast<unsigned long long>(delivery.sdl_short_pulls));
        AuroraVIHardwareState videoClock{};
        Check(aurora_get_video_hardware_state(&videoClock), "Original VI clock observation unavailable");
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        mscharged::platform::Trace("Playback observation: %.3fs, %.2f rendered frames/s; VI retraces%u, elapsed fields%llu.\n",
                    elapsed,frames/elapsed,videoClock.retrace_count,
                    static_cast<unsigned long long>(videoClock.elapsed_fields));
        if(nativeSend)
            mscharged::platform::Trace("Source frame cadence: %u/%u/%u/%u frames spanned 0/1/2/3+ VI fields.\n",
                        fieldSlots[0],fieldSlots[1],fieldSlots[2],fieldSlots[3]);
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio) {
            const auto status=gameAudioHardware->Status();
            mscharged::platform::Trace("Actual native AX state: initialized%u phase%u processed%llu completed%llu SYNC%llu YIELD%llu.\n",
                status.native_initialized, static_cast<unsigned>(status.protocol.phase),
                static_cast<unsigned long long>(status.frames.processed_frames),
                static_cast<unsigned long long>(status.frames.completed_frames),
                static_cast<unsigned long long>(status.frames.sync_interrupts),
                static_cast<unsigned long long>(status.frames.yield_interrupts));
            Check(status.native_initialized && status.frames.processed_frames &&
                  status.frames.completed_frames && status.frames.sync_interrupts &&
                  status.frames.yield_interrupts,
                  "Original audio requests have no actual native AX frame completion");
            mscharged::platform::Trace("Original main AX: processed%llu completed%llu SYNC%llu YIELD%llu; native ROM-free coefficient policy.\n",
                static_cast<unsigned long long>(status.frames.processed_frames),
                static_cast<unsigned long long>(status.frames.completed_frames),
                static_cast<unsigned long long>(status.frames.sync_interrupts),
                static_cast<unsigned long long>(status.frames.yield_interrupts));
        }
#endif
        stopMovie();
        if(sourceAudio) {
            Check(idleAudio(),"Active original sounds must retire through original game flow before host detachment");
            Check(idleBankReads(),"Pending original bank callbacks must complete before host detachment");
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
            Check(gameAudioHardware->Status().last_active_voices==0,
                  "Original stopped voices have not completed a real native AX frame");
#endif
        }
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio)gameAudioHardware->UnbindOwnerService();
#endif
        mscharged::diagnostic::ShutdownMovieHardware();
#if defined(MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE)
        if(sourceAudio) {
            gameAudioHardware->CloseAfterAIStop(retireAudioPin,unloadIdleBanks,shutdownAudio);
            gameAudioHardware.reset();
            Check(observeAudio()==7u,"Original source shutdown flags unexpectedly changed");
            mscharged::platform::Trace("Original audio stopped: AI drained, native AX halted/closed, original idle banks/source retired before device pins; original shutdown flags preserved.\n");
        }
#endif
        mscharged::platform::ShutdownNativeHardwareInput();
        mscharged::platform::ShutdownNativeFilesystem();
        if(axModuleMemory) {
            // Actual source audio retirement already released its module pins.
            // The constructor-only mode retains the earlier stopped device gate.
            if(!sourceAudio)axModuleMemory->ReleaseAfterDeviceDrain();
            axModuleMemory.reset();
        }
        Check(!mscharged::platform::GetNativeAIStatus().initialized,
              "Original-main movie owner did not drain the actual host audio device");
        // No source frame/callback follows this terminal diagnostic retirement.
        // The endpoint refuses borrowed EXI state; it never clears source Scb.
        mscharged::platform::ShutdownNativeRTC();
        mscharged::runtime::LogGraphicsPipelineStats();
        if(resizeCheck) {
            // Retire the actual native hardware/window for the resize gate.
            // Original game tasks/CRT destruction remain explicitly omitted;
            // TerminateProcessNow prevents surviving source statics touching released arenas.
            aurora_dvd_close();
            aurora_shutdown();
            mscharged::platform::ShutdownNativeInterruptController();
            mscharged::platform::Trace("Native resize qualification retired real VI/GX/window hardware.\n");
        }
        if(priorBoot)
            mscharged::platform::Trace("Prior Boot diagnostic retains original effects/NPC resources and game arenas at terminal exit; full source cleanup remains pending.\n");
        mscharged::runtime::FrameTimingLog::FinishForProcessExit();
        mscharged::platform::SessionLogLine("=== Session ended ===");
        std::fflush(nullptr);mscharged::platform::TerminateProcessNow(0);
        } catch(const std::exception& e) {
            // The player's box shows only the reason; the log keeps the context.
            mscharged::platform::SessionLogLine("The game stopped with its source owners still live.");
            mscharged::platform::ReportFatalError(e.what());
            mscharged::platform::TerminateProcessNow(1);
        }
    }catch(const std::exception& e){
        mscharged::platform::ReportFatalError(e.what());
        mscharged::platform::TerminateProcessNow(1);
    }
}

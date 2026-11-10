#include "platform/graphics_stats.h"
#include "platform/alert_sound.h"
#include "platform/app_icon.h"
#include "platform/hotkey_filter.h"
#include "platform/screenshot_hotkey.h"
#include "platform/window_fit.h"
#include "runtime/original_game.h"
#include "runtime/texture_pack_loading.h"
#include "platform/graphics_backend.h"
#include "runtime/original_sh_menu_diagnostic.h"
#include <aurora/aurora.h>
#include <aurora/hardware.h>
#include "bootstrap/antialiasing.h"
#include "bootstrap/display_shape.h"
#include "bootstrap/advanced_choices.h"
#include "bootstrap/graphics_backend.h"
#include "bootstrap/graphics_card.h"
#include "bootstrap/texts.h"
#include "platform/name_glyphs.h"
#include "bootstrap/launch_options.h"
#include "platform/path.h"
#include "platform/disc.h"
#include "platform/disc_message_images.h"
#include "platform/game_release.h"
#include "platform/pc_languages.h"
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <aurora/input.hpp>
#include "gfx/xfb.hpp"
#include "gfx/scanout.hpp"
#include "gx/fifo.hpp"
#include <dolphin/gx/GXAurora.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <tuple>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <future>
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
#if defined(MSCHARGED_HAS_GRAPHICS_CARDS)
#include "platform/graphics_cards.h"
#endif
#include "platform/session_log.h"
#include "platform/launcher_reopen.h"
#include "platform/competitive_host.h"
#include "platform/game_folder_lock.h"
#include "overlay/flags.h"
#include "platform/wpad_sdl.h"
#include "platform/discord_presence.h"
#include "platform/game_presence.h"
#include "platform/netplay_snapshot.h"
#include "platform/http_client.h"
#include "platform/desync_dossier.h"
#include "platform/online_reports.h"
#include "launcher/current_build.h"
#include "launcher/online_gate.h"
#include "platform/presence_snapshot.h"
#include "platform/menu_snapshot.h"
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
#include "runtime/game_window.h"
#include "runtime/host_overlay.h"
#include "runtime/host_panel.h"
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
#include "runtime/host_menus.h"
#endif
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
#include "launcher/match_presets.h"
#include "overlay/game_overlay.h"
#include "overlay/overlay_images.h"
#include "overlay/overlay_prompt.h"
#include "platform/mii_head.h"
#include "platform/save_import.h"
#include "platform/match_kind.h"
#include "platform/online_panel_feed.h"
#include <aurora/imgui.h>
#endif
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
#if defined(MSCHARGED_AUTOPILOT)
#include "platform/bank_cue.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <string>
#if defined(MSCHARGED_HAS_ONLINE_SERVER_OVERRIDE)
#include "net/so_resolve.h"
#endif
namespace aurora { extern AuroraConfig g_config; }
extern "C" bool __OSInitSTM();
// Whole original OSRtc software/cache owner, linked once into the host.
extern "C" void __OSInitSram();
extern "C" BOOL __OSSyncSram();
namespace {
// Aurora's FIFO retains every recorded command until a drain; publishing only
// hands bytes to the worker. Bound that backlog between source frames.
constexpr std::uint32_t kFifoDrainBytes=64u<<20;
// The run's competitive play, stopped when the game powers off: that path
// ends the process without unwinding, so its last calls (a set called off, a
// series reported) would never go out. They get a few seconds, never more.
std::unique_ptr<mscharged::platform::CompetitiveHost>* sCompetitive=nullptr;
#if defined(MSCHARGED_AUTOPILOT)
// Test builds' autopilot (src/platform/autopilot.cpp): C, P and U held
// together hand the first player to a bot and back. Unannounced: no log line,
// only the title screen's unlock-all sound when it takes over (played here:
// the game unloads that bank for matches).
std::atomic<int(*)(int)> sSetAutopilot{nullptr};
std::atomic<bool> sAutopilotOn{false};
mscharged::platform::BankCuePlayer sAutopilotSound;
std::atomic<float> sAutopilotSoundGain{1.0f};
void SwitchAutopilot() {
    const auto set=sSetAutopilot.load();
    if(!set) return;
    const bool on=!sAutopilotOn.load();
    sAutopilotOn=on;
    set(on ? 1 : 0);
    if(on) sAutopilotSound.Play(sAutopilotSoundGain);
}
#endif
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
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
// RECHARGED_CAPTURE_ONLINE's sample (development: the online pages without a
// server): a player with a set, friends and players online, presets, boards.
void SampleOnline(mscharged::overlay::GameOverlayModel& model)
{
    namespace ov=mscharged::overlay;
    auto& online=model.online;
    online=ov::OnlinePanelModel{};
    online.me.name="Yannick";
    online.me.detail="@yannick";
    online.me.initials="Y";
    online.me.tint_seed=11;
    online.me.country="CH";
    online.me.available=true;
    online.me.can_set_available=true;
    auto row=[](const char* name,const char* country,ov::Presence presence,const char* status,ov::Tone tone,
                bool isFriend,std::int64_t id) {
        ov::OnlinePanelRow r;
        r.player_id=id;
        r.key="p"+std::to_string(id);
        r.name=name;
        r.initials=std::string(1,name[0]);
        r.tint_seed=id*7;
        r.presence=presence;
        r.status=status;
        r.tone=tone;
        r.country=country;
        r.is_friend=isFriend;
        r.can_befriend=true;
        r.can_challenge=presence==ov::Presence::Free;
        if(!r.can_challenge) r.blocked="In a set: challenge them once it is over.";
        return r;
    };
    online.set.stage=ov::OnlinePanelSet::Stage::Playing;
    online.set.opponent=row("Mario","IT",ov::Presence::Busy,"In your set",ov::Tone::Busy,true,1);
    online.set.title="Your set against Mario";
    online.set.status="Game 2 of 3: choosing teams";
    online.set.tone=ov::Tone::Busy;
    online.set.rules="Competitive: 3 minutes, best of 3, random stadium";
    online.set.games=3;
    online.set.won=1;
    online.set.lost=0;
    online.set.can_cancel=true;
    online.set.cancel_label="Cancel set";
    ov::OnlinePanelSection friends;
    friends.id=ov::OnlinePanelSection::Id::Online;
    friends.aside="2 friends offline";
    friends.rows={row("Luigi","DE",ov::Presence::Free,"Main menu",ov::Tone::Good,true,2),
                  row("Peach","FR",ov::Presence::Busy,"Domination: choosing teams",ov::Tone::Busy,true,3)};
    friends.rows[0].detail="@luigi";
    ov::OnlinePanelSection others;
    others.id=ov::OnlinePanelSection::Id::Available;
    others.rows={row("Waluigi","US",ov::Presence::Free,"Open to challenges",ov::Tone::Good,false,4),
                 row("Daisy","SE",ov::Presence::Free,"Open to challenges",ov::Tone::Good,false,5),
                 row("Bowser","JP",ov::Presence::Busy,"Set vs Wario: game 1 of 5",ov::Tone::Busy,false,6)};
    others.rows[1].version="1.1.6";
    online.sections={friends,others};
    ov::OnlinePanelPreset competitive;
    competitive.name="Competitive";
    competitive.rule_set=1;
    competitive.rules.limit="time";
    competitive.rules.time=180;
    competitive.rules.goals=5;
    competitive.rules.games=3;
    competitive.rules.stadium="random";
    competitive.rules.tweaks={"no_mega_strikes"};
    ov::OnlinePanelPreset quick=competitive;
    quick.name="Quick game";
    quick.rules.games=1;
    quick.rule_set=2;
    online.presets={competitive,quick};
    online.can_search=true;
    auto& ranked=model.ranked;
    ranked.can_play=true;
    ranked.blocked.clear();
    ranked.signed_out=ranked.missing=ranked.loading=false;
    mscharged::platform::Leaderboard boards;
    auto entry=[](int rank,const char* player,const char* mii,int wins,int losses,int points,bool you) {
        mscharged::platform::LeaderboardEntry e;
        e.rank=rank;
        e.player=player;
        e.mii=mii;
        e.wins=wins;
        e.losses=losses;
        e.points=points;
        e.you=you;
        return e;
    };
    boards.season.first_day="2026-11-01";
    boards.season.last_day="2026-11-30";
    boards.season.players=184;
    boards.season.top={entry(1,"Luigi","Luigi",42,7,1830,false),entry(2,"","Toadsworth",38,11,1712,false),
                       entry(3,"Waluigi","Wah",31,12,1650,false),entry(4,"Daisy","Daisy",25,14,1544,false),
                       entry(5,"","Kamek",22,17,1490,false)};
    boards.season.you=entry(17,"Yannick","Yannick",9,8,1204,true);
    boards.daily=boards.season;
    boards.daily.first_day=boards.daily.last_day="2026-11-09";
    boards.daily.players=23;
    ranked.boards=boards;
}
#endif
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
        bool nativeSend=true, resizeCheck=false, nonInteractive=false;
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
            else if(argument=="--non-interactive")nonInteractive=true;
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        // The diagnostic run: it ends once its frame is captured, or fails at its timeout.
        if(nonInteractive) interactive=false;
        const char* basePath=SDL_GetBasePath();
        Check(basePath && *basePath,"Cannot locate the executable directory");
        const auto executableDirectory=mscharged::PathFromUtf8(basePath);
        auto launch=suppliedLaunch ? *suppliedLaunch :
            mscharged::LoadLaunch(launchOptions,executableDirectory);
        mscharged::ApplyGameModeDisplay(launch,mscharged::UnderGamescope());
        disc=launch.disc_path;
        windowWidth=launch.settings.width;windowHeight=launch.settings.height;
        const auto& aspect = launch.settings.aspect;
        Check(aspect=="auto" || aspect=="4:3" || aspect=="16:9" || aspect=="stretch",
              "The game supports 4:3 or 16:9; select --aspect 4:3, 16:9 or stretch");
        // Stage a Wii system preference. Original main queries SC and selects
        // its projection and authored scene; resizing only scales that image.
        // auto and stretch take the shape closer to the screen's, known once
        // the window is on its display (below). The picture fills the window
        // (fill) stretched, or with display.wider_view and the 16:9 layout by
        // showing more of the stadium while a match's stadium is drawn (the
        // menus keep the game's own 16:9 picture); the stretch hotkey switches
        // either (issue #91).
        bool widescreen=aspect!="4:3";
        unsigned aspectWidth=widescreen?16u:4u, aspectHeight=widescreen?9u:3u;
        bool stretch=aspect=="stretch", widerView=false, widerOn=false, fill=stretch;
        // Wider view: the module entries, the shape it draws and the last frame
        // the match's stadium was drawn (frames counts the loop's frames).
        void (*setViewShape)(float)=nullptr;
        bool (*takeMatchView)()=nullptr;
        float viewShape=1.0f;
        unsigned matchViewFrame=0;
        // advanced.verbose_console: development traces and the game's debug output.
        mscharged::platform::SetVerboseConsole(launch.settings.verbose_console);
        // advanced.log_window: the separate log window, when the launcher has not opened it.
        mscharged::platform::SetLogWindow(launch.settings.log_window);
#ifdef MSCHARGED_HAS_ONLINE_SERVER_OVERRIDE
        so_set_server_override(launch.settings.online_server.c_str());
#endif
        // [online] rollback and rollback_test: the game module's rollback reads
        // its switches from the environment when it loads
        // (src/platform/rollback_engine.cpp); one already set there wins.
        if ((launch.settings.rollback || launch.settings.rollback_test)
            && !std::getenv("RECHARGED_ROLLBACK") && !std::getenv("RECHARGED_ROLLBACK_TEST")) {
            const char* name=launch.settings.rollback_test ? "RECHARGED_ROLLBACK_TEST" : "RECHARGED_ROLLBACK";
            const std::string log=launch.settings.rollback_log.empty() ? "1" : launch.settings.rollback_log;
#if defined(_WIN32)
            _putenv_s(name,log.c_str());
#else
            setenv(name,log.c_str(),1);
#endif
        }
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
        // Sets pair only players of one version: the server hears which.
        mscharged::platform::ProfileService::SetPlayedGame(std::string(release->game_id,4));
        const std::string languageLabel = languageChoice.automatic
            ? "Automatic (" + std::string(languageChoice.language->label) + ")" : languageChoice.language->label;
        mscharged::platform::SessionLogLine(languageChoice.unavailable
            ? "Language: " + launch.settings.language + " is not in this release; " + languageLabel
            : "Language: " + languageLabel);
        Check(!resizeCheck || !launch.settings.fullscreen,"--resize-check requires a windowed launch; add --window");
        Check(!priorBoot || !resizeCheck,
              "The prior Boot stage retains original effects/NPC owners; resize teardown is not qualified");
        // One game module per game version; both European revisions run the EU one.
        const char* moduleFilename=MSCHARGED_ORIGINAL_FRONTEND_MODULE_US_FILENAME;
        switch(release->code) {
        case mscharged::GameCode::Europe: moduleFilename=MSCHARGED_ORIGINAL_FRONTEND_MODULE_EU_FILENAME; break;
        case mscharged::GameCode::Japan: moduleFilename=MSCHARGED_ORIGINAL_FRONTEND_MODULE_JP_FILENAME; break;
        case mscharged::GameCode::Korea: moduleFilename=MSCHARGED_ORIGINAL_FRONTEND_MODULE_KR_FILENAME; break;
        case mscharged::GameCode::Usa: break;
        }
        const auto modulePath=executableDirectory/moduleFilename;
        Check(std::filesystem::is_regular_file(modulePath),"Original-main source module is missing beside the executable");
        // Real host window/device/FIFO owner exists before any original module
        // constructors. Original main still owns its GXInit/glStartup decisions.
        if(mscharged::MoveOldDataFolder(executableDirectory))
            mscharged::platform::SessionLogLine("Data: moved original-main-credits-data to data");
        const auto dataDirectory = std::filesystem::absolute(mscharged::DataFolder(executableDirectory));
        mscharged::platform::SessionLogLine("Data: "+mscharged::PathUtf8(dataDirectory));
        std::filesystem::create_directories(dataDirectory);
        // Two games of one folder would share its save, and both would take
        // the player's competitive sets.
        Check(mscharged::platform::LockGameFolder(dataDirectory),
            "Recharged already runs a game from this folder. Close it first, or switch to its window.");
        // Aurora reads its paths as UTF-8; string() would give Windows' ANSI code page (#97).
        const auto dataPath = mscharged::PathUtf8(dataDirectory);
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
        // At debug level every log line begins with its time.
        mscharged::platform::SetSessionLogTimes(logLevel=="debug");
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
        // display.fullscreen_resolution on Wayland, where the compositor shows
        // other modes scaled: a mode of another shape than the display keeps
        // its shape with bars, as the picture does (SDL's default stretches it).
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_WAYLAND_MODE_SCALING,"aspect",SDL_HINT_DEFAULT);
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
        // The game draws its own pointer (the Wii Remote's hand) where a player
        // points, the mouse included: the system cursor stays hidden over the
        // window. A layer above that takes the mouse (ImGui) shows it again.
        const auto cursorLayer=aurora::input::register_layer({
            .label="recharged.cursor",
            .priority=aurora::input::kGameLayerPriority,
            .onEvent=[](const aurora::input::InputEvent&,void*){return aurora::input::EventResult::Pass;},
            .pointerMode=[](void*){return aurora::input::PointerMode::Hidden;},
        });
        if(cursorLayer==aurora::input::kInvalidLayerId)
            mscharged::platform::SessionLogLine("Startup: the system cursor stays visible over the game");
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
        {
            const SDL_DisplayID display=SDL_GetDisplayForWindow(host.window);
            // display.fullscreen_resolution: fullscreen (at start, and with the
            // fullscreen hotkey) switches the display to that mode while the
            // game runs; desktop keeps the display's own resolution. A Retina
            // screen keeps its sharpness: the size at the highest pixel density
            // the display offers for it (issue #99).
#if defined(__APPLE__)
            constexpr bool highDensityModes=true;
#else
            constexpr bool highDensityModes=false;
#endif
            SDL_DisplayMode fullscreenMode{};
            if(const auto wanted=mscharged::ParseResolution(launch.settings.fullscreen_resolution); wanted.width) {
                const std::string name=mscharged::FormatResolution(wanted.width,wanted.height);
                if(display && SDL_GetClosestFullscreenDisplayMode(display,wanted.width,wanted.height,0.0f,
                       highDensityModes,&fullscreenMode)
                   && fullscreenMode.w==wanted.width && fullscreenMode.h==wanted.height
                   && SDL_SetWindowFullscreenMode(host.window,&fullscreenMode))
                    mscharged::platform::SessionLogLine("Startup: fullscreen resolution "+name);
                else {
                    fullscreenMode={};
                    mscharged::platform::SessionLogLine("Startup: fullscreen resolution "+name
                        +" is not one of this display's; fullscreen keeps the display's own");
                }
            }
            // display.aspect: the shape the picture fills is the screen's in
            // fullscreen, the window's otherwise.
            int shapeWidth=int(windowWidth), shapeHeight=int(windowHeight);
            SDL_Rect bounds{};
            if(launch.settings.fullscreen && fullscreenMode.w) {
                shapeWidth=fullscreenMode.w; shapeHeight=fullscreenMode.h;
            } else if(launch.settings.fullscreen && display && SDL_GetDisplayBounds(display,&bounds)) {
                shapeWidth=bounds.w; shapeHeight=bounds.h;
            }
            widescreen=mscharged::WidescreenLayout(aspect,shapeWidth,shapeHeight);
            aspectWidth=widescreen?16u:4u; aspectHeight=widescreen?9u:3u;
            widerView=widerOn=launch.settings.wider_view && widescreen && aspect!="stretch";
            mscharged::platform::SessionLogLine(std::string("Startup: game layout ")+(widescreen?"16:9":"4:3")
                +(widerView?", wider view in matches":stretch?", stretched to fill the window":""));
            // A window keeping the picture's shape takes that shape: no bars.
            if(!launch.settings.fullscreen && !stretch && !widerOn) {
                const auto snapped=mscharged::SnapToShape(int(windowWidth),int(windowHeight),
                    int(aspectWidth),int(aspectHeight));
                windowWidth=unsigned(snapped.width); windowHeight=unsigned(snapped.height);
            }
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
            // On macOS fullscreen keeps the window's created size for the
            // fullscreen hotkey: a fullscreen window at a chosen resolution is
            // a borderless window there, which a resize would shrink (issue #99).
#if defined(__APPLE__)
            const bool setWindowSize=!launch.settings.fullscreen;
#else
            const bool setWindowSize=true;
#endif
            if(setWindowSize)
                Check(SDL_SetWindowSize(host.window,static_cast<int>(windowWidth),static_cast<int>(windowHeight)),
                      "Requested window size rejected");
            // display.remember_window: the window opens where it was left.
            if(!launch.settings.fullscreen && launch.settings.remember_window &&
               mscharged::runtime::RestoreWindowPosition(host.window,launch.settings.position,launch.settings.monitor))
                mscharged::platform::SessionLogLine("Startup: window at its last position "+launch.settings.position);
            mscharged::runtime::LockWindowShape(host.window,!launch.settings.fullscreen && !stretch && !widerOn,
                aspectWidth,aspectHeight);
            // display.resolution: the source EFB's render size. Viewport, scissor,
            // copies and pixel accesses map the game's 640x448 coordinates onto it.
            efbScale=mscharged::RenderScale(launch.settings.resolution);
            // window: one EFB pixel per window pixel of the displayed 4:3/16:9
            // picture (the whole window when it fills it). Locked for every
            // resolution: the 3D resolution hotkeys can switch to it.
            VILockAspectRatio(int(aspectWidth),int(aspectHeight));
            VISetFrameBufferScale(efbScale);
            AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        }
        // After the window's size limits: on macOS the minimum size above gives
        // a fullscreen window at a chosen resolution its windowed size (issue #99).
        if(launch.settings.fullscreen) mscharged::runtime::PlaceFullscreenWindow(host.window);
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
            // with display.antialiasing's FXAA, display.tv_border's TV frame
            // and a picture filling the window (stretched or wider view).
            mscharged::platform::ConfigureNativeVideoOutputHardware(settings,
                launch.settings.picture=="sharp" ? mscharged::platform::NativeDesktopPicture::Sharp
                : launch.settings.picture=="clean" ? mscharged::platform::NativeDesktopPicture::Clean
                                                   : mscharged::platform::NativeDesktopPicture::Signal,
                mscharged::FindAntialiasing(launch.settings.antialiasing).fxaa,
                launch.settings.tv_border, fill);
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
        // [hotkeys]: the host's hotkeys, which the keyboard player never presses.
        const auto hotkeys=mscharged::platform::ParseHotkeys(launch.settings.hotkeys);
        mscharged::platform::SetHotkeys(hotkeys);
        mscharged::platform::DesktopWpadSettings desktop{true,false,true,mscharged::platform::QueryPresentedDesktopDpd,
            nullptr,true,true,-1,mscharged::platform::ParseKeyBindings(launch.settings.keys,hotkeys)};
        // Only the European KPAD keeps its pointer centre at the sensor bar.
        desktop.kpad_sensor_height=release->code!=mscharged::GameCode::Europe;
        desktop.rumble_strength=launch.settings.rumble_strength;
        // Each gamepad's profile (controls.gamepadN_profile).
        for (std::size_t pad = 0; pad < desktop.gamepad_profiles.size(); ++pad) {
            const auto& profile = launch.settings.PadProfileOf(pad);
            desktop.gamepad_profiles[pad] = {mscharged::platform::ParseGamepadBindings(profile.inputs),
                                             profile.sticks, profile.follows.has_value(),
                                             profile.inner_deadzone, profile.outer_deadzone, profile.trigger_press,
                                             profile.octagon_stick, profile.gyro_pointer, profile.gyro_speed,
                                             profile.follows.value_or(mscharged::GamepadLayout::Default)};
        }
        mscharged::platform::WpadSDLSettings wpad{static_cast<std::uint8_t>(launch.settings.sensor_bar=="top"?1:0),
            static_cast<std::uint8_t>(launch.settings.ir_sensitivity),true,launch.settings.rumble};
        wpad.speaker_volume=static_cast<std::uint8_t>((launch.settings.speaker_volume*127+50)/100);
        wpad.remote_speaker=launch.settings.speaker_volume>0;
        // Original WPADControlSpeaker ON: 0x08 in the European game, 0x80 in the others.
        wpad.remote_speaker_reg01=release->code==mscharged::GameCode::Europe ? 0x08 : 0x80;
        wpad.bluetooth_search=launch.settings.bluetooth_search;
        mscharged::platform::InitializeNativeHardwareInput(host.window,wpad,
            mscharged::platform::GetNativeSTMInput(), desktop, players);
        {
            const auto& s=launch.settings;
            const std::string line="Wii Remote settings: sensor bar "+s.sensor_bar+", sensitivity "
                +std::to_string(s.ir_sensitivity)+", speaker "+(s.speaker_volume ? std::to_string(s.speaker_volume)+"%" : "off")
                +", rumble "+(s.rumble ? "on (gamepads "+std::to_string(s.rumble_strength)+"%)" : "off");
            mscharged::platform::SessionLogLine(line);
            const auto key=[&](mscharged::HotkeyIndex hotkey) { return mscharged::platform::HotkeyName(hotkeys[hotkey]); };
            mscharged::platform::SessionLogLine("Hotkeys: screenshot "+key(mscharged::HotkeyScreenshot)
                +", HUD "+key(mscharged::HotkeyHud)+" (matches start with the HUD "+(s.show_hud ? "shown)" : "hidden)")
                +", stretch "+key(mscharged::HotkeyStretch)+", 3D resolution "+key(mscharged::HotkeyResolutionUp)
                +"/"+key(mscharged::HotkeyResolutionDown)+", fullscreen "+key(mscharged::HotkeyFullscreen)
                +", game menu "+key(mscharged::HotkeyPanel)+" (and HOME), game options "
                +key(mscharged::HotkeyOptions));
            for (const auto& device : s.players) {
                if (device.rfind("gamepad", 0) != 0) continue;
                const std::size_t pad = std::size_t(device.back() - '1');
                const auto& profile = s.PadProfileOf(pad);
                mscharged::platform::SessionLogLine("Gamepad " + std::to_string(pad + 1) + ": profile " + profile.name
                    + (profile.octagon_stick ? ", octagonal Nunchuk stick" : "")
                    + ", stick dead zones " + std::to_string(profile.inner_deadzone) + "-"
                    + std::to_string(profile.outer_deadzone) + "%, trigger press point "
                    + std::to_string(profile.trigger_press) + "%, pointer "
                    + (profile.gyro_pointer ? "gyro (speed " + std::to_string(profile.gyro_speed) + "%)"
                                            : profile.sticks == mscharged::GamepadSticks::Split ? std::string("right stick")
                                            : profile.sticks == mscharged::GamepadSticks::Swapped
                                                ? std::string("left stick")
                                                : std::string("left stick, with the Nunchuk stick")));
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
                configureRecolour(mscharged::TexturePackRecolourHook(reinterpret_cast<ChargedPresenceSnapshotFunction>(
                    mscharged::platform::FindNativeModuleSymbol(module,"charged_presence_snapshot"))));
            // Training: the item of the team that trains, and Start training's match.
            if(auto* configureTraining=reinterpret_cast<void(*)(int,bool,int,int)>(
                   mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_training"))) {
                const auto& s=launch.settings;
                const auto& item=mscharged::kTrainingItemKinds[std::size_t(
                    std::max(mscharged::TrainingIndex(mscharged::kTrainingItems,s.training_item),0))];
                configureTraining(item.type,
                    launch.start_training,mscharged::TrainingIndex(mscharged::kTrainingCaptains,s.training_captain),
                    mscharged::TrainingIndex(mscharged::kTrainingStadiums,s.training_stadium));
                // And how many of it, and whether a single one is always giant.
                if(auto* configureItem=reinterpret_cast<void(*)(int,bool)>(
                       mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_training_item")))
                    configureItem(item.count,item.giant);
                if(auto* configureOpponent=reinterpret_cast<void(*)(int,int,bool,bool,int,bool)>(
                       mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_training_opponent")))
                    configureOpponent(mscharged::TrainingIndex(mscharged::kTrainingCaptains,s.training_away_captain),
                        s.training_away_fielders,s.training_away_goalie,s.training_skip_goals,
                        mscharged::TrainingIndex(mscharged::kTrainingSkills,s.training_away_skill),
                        s.training_side=="away");
                if(launch.start_training)
                    mscharged::platform::SessionLogLine(("Start training: "+s.training_captain+" in "
                        +s.training_stadium).c_str());
            }
            // No HUD (issue #58): the HUD key shows or hides the match HUD, whatever show_hud says.
            mscharged::platform::SetHudKey(reinterpret_cast<void(*)()>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_toggle_hud")));
#if defined(MSCHARGED_AUTOPILOT)
            // The autopilot: only a test build's module has one (a release's says 0).
            if(auto* setAutopilot=reinterpret_cast<int(*)(int)>(
                   mscharged::platform::FindNativeModuleSymbol(module,"charged_set_pad_source"));
               setAutopilot && setAutopilot(0)==1) {
                sSetAutopilot=setAutopilot;
                mscharged::platform::SetAutopilotChord(SwitchAutopilot);
                // The unlock-all cue of the title screen (SHTitleScreen.cpp), in
                // the menus' bank, as loud as the game's menu sounds.
                sAutopilotSound.Load(launch.disc_path, "FE_GEN_Sfx",
                    std::strncmp(release->game_id, "R4QK", 4) == 0 ? 0xF0AFD586u : 0xCF37DAC7u);
                sAutopilotSoundGain = launch.settings.mute ? 0.0f : float(launch.settings.master_volume) / 100.0f
                    * float(launch.settings.sound_volumes[std::size_t(mscharged::SoundGroup::Menus)]) / 100.0f;
            }
#endif
            // display.wider_view: the module tells when the match's stadium is
            // drawn and draws the shape of the window's picture.
            if(widerView) {
                setViewShape=reinterpret_cast<void(*)(float)>(
                    mscharged::platform::FindNativeModuleSymbol(module,"charged_set_view_shape"));
                takeMatchView=reinterpret_cast<bool(*)()>(
                    mscharged::platform::FindNativeModuleSymbol(module,"charged_take_match_view"));
                if(!setViewShape || !takeMatchView) {
                    widerView=widerOn=false; setViewShape=nullptr; takeMatchView=nullptr;
                    mscharged::runtime::LockWindowShape(host.window,!launch.settings.fullscreen,aspectWidth,
                        aspectHeight);
                }
                mscharged::platform::SessionLogLine(widerView ? "Wider view: on (in matches)"
                    : "Wider view: off (this build's game module has no wider view)");
            }
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
        // whatever the tweak set; the game module's hooks apply them (again
        // as the overlay's Settings change them).
        void(*configureSoundVolumes)(const int*,int)=nullptr;
        {
            const auto& volumes=launch.settings.sound_volumes;
            configureSoundVolumes=reinterpret_cast<void(*)(const int*,int)>(
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
        // display.background_throttle: in play, at most
        // display.background_frame_rate while the window is minimized or,
        // outside Steam's Game Mode, another window has the focus; the
        // original's rate included.
        std::optional<mscharged::runtime::HostFramePacing> framePacing;
        {
            const auto rate=mscharged::ParseFrameRate(launch.settings.frame_rate).value_or(mscharged::FrameRate{});
            int backgroundHz=interactive && launch.settings.background_throttle
                ? launch.settings.background_frame_rate : 0;
            // 60 FPS holds nothing at the original rate.
            if(rate.kind==mscharged::FrameRate::Kind::Original
               && mscharged::BackgroundFrameRate(rate,backgroundHz,0.0)==rate) backgroundHz=0;
            auto* configurePacing=reinterpret_cast<void(*)(void(*)())>(
                mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_frame_pacing"));
            std::string line="Frame rate: ";
            if(!configurePacing || !nativeSend)
                line+=rate.kind==mscharged::FrameRate::Kind::Original ? mscharged::DescribeFramePacing(rate,{},0.0)
                    : "original (this build's game module has no frame pacing)";
            else if(rate.kind==mscharged::FrameRate::Kind::Original && !backgroundHz)
                line+=mscharged::DescribeFramePacing(rate,{},0.0);
            else {
                framePacing.emplace(rate,launch.settings.vsync,host.window,configurePacing,backgroundHz,
                    !mscharged::UnderGamescope());
                line+=framePacing->Description();
            }
            if(framePacing && backgroundHz)
                line+="; "+std::to_string(backgroundHz)+" FPS at most in the background";
            mscharged::platform::SessionLogLine(line);
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        // The Discord status (issue #71): the game module reports what the
        // game is doing; the host shows it on this computer's Discord app.
        // A host setting like the sound levels, whatever the tweak set.
        auto presenceSnapshot=reinterpret_cast<ChargedPresenceSnapshotFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_presence_snapshot"));
        // Override retail menus (development switch): the match's pause menu,
        // read after a frame for the host's own drawing of it.
        auto menuSnapshot=reinterpret_cast<ChargedMenuSnapshotFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_menu_snapshot"));
        auto menuTexture=reinterpret_cast<ChargedMenuTextureFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_menu_texture"));
        auto menuCommand=reinterpret_cast<ChargedMenuCommandFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_menu_command"));
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
        // Enforced updates: a release plays online only while it is the newest
        // release (launcher/online_gate.h). The gate tells the module its
        // verdict; the main menu's Online asks the module (tweak patch 0054).
        std::unique_ptr<mscharged::launcher::OnlineGate> onlineGate;
        if(auto* configureOnlineGate=reinterpret_cast<void(*)(int)>(
               mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_online_gate")))
            onlineGate=std::make_unique<mscharged::launcher::OnlineGate>(mscharged::launcher::CurrentBuild(),
                executableDirectory,launch.update_verified,
                [configureOnlineGate](mscharged::launcher::OnlineGateState state,const std::string& line) {
                    configureOnlineGate(static_cast<int>(state));
                    mscharged::platform::SessionLogLine(line);
                    std::fprintf(stderr,"%s\n",line.c_str());
                });
#ifdef MSCHARGED_HAS_ONLINE_SERVER_OVERRIDE
        // The WFC server's match reports: the online series this computer
        // plays, and the error codes its online menus show, go to the server
        // the game logs in to (online.server).
        auto onlineMatchSnapshot=reinterpret_cast<ChargedOnlineMatchSnapshotFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_online_match_snapshot"));
        std::unique_ptr<mscharged::platform::OnlineReports> onlineReports;
        {
            std::string line="Match reports: ";
            const std::string http=mscharged::platform::HttpUnavailable();
            if(launch.settings.online_server.empty())
                line+="off (no online server)";
            else if(!onlineMatchSnapshot)
                line+="off (this build's game module records no online matches)";
            else if(!http.empty())
                line+="off ("+http+")";
            else {
                onlineReports=mscharged::platform::OnlineReports::Start(launch.settings.online_server,
                    std::string(release->game_id,4),mscharged::build::version,[](const std::string& text) {
                        mscharged::platform::SessionLogLine(text);
                        std::fprintf(stderr,"%s\n",text.c_str());
                    });
                line+="to "+launch.settings.online_server;
            }
            mscharged::platform::SessionLogLine(line);
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        // The rollback's desync dossiers (desync_dossier.h): kept in
        // data/desyncs until the server has them.
        auto desyncDossier=reinterpret_cast<ChargedDesyncDossierFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_desync_dossier"));
        if(onlineReports&&desyncDossier)onlineReports->KeepDossiers(dataDirectory/"desyncs");
#endif
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
#if defined(_WIN32)
        // Its own base unless something had taken that place (#98).
        char moduleBase[24];
        std::snprintf(moduleBase,sizeof(moduleBase),"0x%llx",
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(module)));
        mscharged::platform::SessionLogLine(std::string("Startup: game module loaded at ")+moduleBase
            +", entering the game");
#else
        mscharged::platform::SessionLogLine("Startup: game module loaded, entering the game");
#endif
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
                mscharged::platform::StopFreezeWatch();
                // The last calls get a few seconds; a stuck one doesn't hold the close.
                if(sCompetitive && *sCompetitive && !(*sCompetitive)->StopForExit(std::chrono::seconds(1)))
                    mscharged::platform::SessionLogLine("Competitive: the game closes without waiting for the server");
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
        std::optional<mscharged::platform::SetText> presenceSet; // a set's game: its opponent and games
        auto observePresence = [&] {
            if(!discordPresence)return;
            const auto now=std::chrono::steady_clock::now();
            if(now-lastPresence<std::chrono::seconds(1))return;
            lastPresence=now;
            ChargedPresenceSnapshot snapshot{};
            if(!presenceSnapshot(&snapshot,sizeof snapshot))return;
            const auto unixNow=std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            discordPresence->Show(gamePresence->Describe(snapshot,unixNow,presenceSet ? &*presenceSet : nullptr));
        };
        // Once a second, after a game frame: the online series for the match reports.
#ifdef MSCHARGED_HAS_ONLINE_SERVER_OVERRIDE
        std::chrono::steady_clock::time_point lastOnlineMatch{};
#endif
        auto observeOnlineMatch = [&] {
#ifdef MSCHARGED_HAS_ONLINE_SERVER_OVERRIDE
            if(!onlineReports)return;
            // A desync's dossier, after every frame: written before whatever
            // follows the desync. Its series seen first.
            if(desyncDossier){
                static unsigned char dossier[kDesyncDossierMaxBytes];
                if(const unsigned size=desyncDossier(dossier,sizeof dossier)){
                    ChargedOnlineMatchSnapshot snapshot{};
                    if(onlineMatchSnapshot(&snapshot,sizeof snapshot))onlineReports->Observe(snapshot);
                    onlineReports->Dossier(dossier,size);
                }
            }
            const auto now=std::chrono::steady_clock::now();
            if(now-lastOnlineMatch<std::chrono::seconds(1))return;
            lastOnlineMatch=now;
            ChargedOnlineMatchSnapshot snapshot{};
            if(onlineMatchSnapshot(&snapshot,sizeof snapshot))onlineReports->Observe(snapshot);
#endif
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
        // The host overlay: the performance overlay (display.fps_overlay) and
        // the messages of the display hotkeys.
        std::optional<mscharged::runtime::HostOverlay> hostOverlay;
        if(nativeSend) {
            // Recharged's own texts in the player's language ([interface]
            // language), their characters noted for the fonts.
            const auto language=mscharged::texts::Load(executableDirectory/"assets"/"lang",
                launch.settings.interface_language,mscharged::platform::PcLanguages());
            mscharged::platform::NoteNameGlyphs(mscharged::texts::AllTexts());
            mscharged::platform::SessionLogLine("Interface language: "+language);
        }
        if(nativeSend)
            hostOverlay.emplace(executableDirectory/"assets"/"launcher"/"Roboto-Medium.ttf",
                launch.settings.fps_overlay ? std::optional(mscharged::runtime::ParseOverlayCorner(
                    launch.settings.fps_overlay_corner).value_or(mscharged::runtime::OverlayCorner::TopRight))
                                            : std::nullopt);
        // display.remember_window: not in Steam's Game Mode, for a window size
        // from the command line or without a settings file.
        std::optional<mscharged::runtime::WindowMemory> windowMemory;
        if(nativeSend && interactive && !resizeCheck && launch.settings.remember_window && launch.config.exists
           && launch.width_source!=mscharged::SettingSource::CommandLine && !mscharged::UnderGamescope())
            windowMemory.emplace(host.window,launch.config.path);
        // The 3D resolution the hotkeys step from, for this run.
        std::string renderResolution=launch.settings.resolution;
        // Once a second after a game frame, the network numbers: the overlay's
        // LAT in network sessions and DLY and RB in network matches with
        // rollback; and in those matches at most 60 FPS (kRollbackFrameRate).
        auto netplaySnapshot=reinterpret_cast<ChargedNetplaySnapshotFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_netplay_snapshot"));
        std::chrono::steady_clock::time_point lastNetplay{};
        // Competitive play: with the launcher's session for these settings, the
        // game's WFC login goes to the profile service, the accepted match's
        // rules to the module, and its series' result back to the server.
        auto wfcSnapshot=reinterpret_cast<ChargedWfcSnapshotFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_wfc_snapshot"));
        auto configureCompetitive=reinterpret_cast<ChargedConfigureCompetitiveFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_competitive"));
        std::unique_ptr<mscharged::platform::CompetitiveHost> competitive;
        sCompetitive=&competitive;
        struct CompetitiveForPowerOff { ~CompetitiveForPowerOff() { sCompetitive=nullptr; } } competitiveForPowerOff;
        {
            std::string off;
            if(wfcSnapshot && configureCompetitive)
                competitive=mscharged::platform::CompetitiveHost::Start(launch.settings.profile_server,
                    mscharged::platform::ProfileSessionFile(mscharged::DataFolder(executableDirectory),launch.config.path),
                    [](const std::string& text) {
                        // On the console and in the log, with the time (at log level
                        // debug the log gives every line its time).
                        const std::string time=mscharged::platform::SessionLogTimes() ? std::string()
                            : mscharged::platform::LogTimeStamp();
                        std::fprintf(stderr,"%s%s\n",time.c_str(),text.c_str());
                    },&off);
            else off="this build's game module has no competitive play";
            const auto line=competitive ? "Competitive: on, signed in as "+competitive->PlayerName()
                : "Competitive: off ("+off+")";
            std::fprintf(stderr,"%s\n",line.c_str());
        }
        // A set's game (the launcher's Ready to play!, or a set that went
        // while this game runs): it starts itself into the set's RANKED search
        // with the player's own Mii, and stays open once the set is done.
        auto configureStart=reinterpret_cast<ChargedConfigureCompetitiveStartFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_competitive_start"));
        auto endSet=reinterpret_cast<ChargedCompetitiveEndSetFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_competitive_end_set"));
        // The Recharged Game Menu's Play RANKED: the game goes by itself into a
        // retail RANKED search against anybody, as a set's game into its set's.
        auto startRanked=reinterpret_cast<ChargedStartRankedFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_start_ranked"));
        auto stopRanked=reinterpret_cast<ChargedStopRankedFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_stop_ranked"));
        int rankedStage=CHARGED_SET_NONE;
        std::chrono::steady_clock::time_point rankedChecked{}, rankedSince{}, rankedReminded{};
        // The set's players by their profile names on its RANKED screens (the
        // game shows Mii names): this player's and the opponent's, given while
        // the game plays a set (the module drops them at its end).
        auto configureNames=reinterpret_cast<ChargedConfigureDisplayNamesFunction>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_display_names"));
        std::optional<std::pair<std::u16string,std::u16string>> namesGiven;
        // At debug level DWC's own reports of the WFC login, matchmaking and
        // NAT negotiation, which the game switches off, go to the log.
        if(launch.settings.log_level=="debug")
            if(auto dwcReport=reinterpret_cast<ChargedConfigureDwcReportFunction>(
                   mscharged::platform::FindNativeModuleSymbol(module,"charged_configure_dwc_report")))
                dwcReport(true);
        if(launch.start_competitive) {
            if(competitive && configureStart
               && configureStart(launch.competitive_mii.data(),unsigned(launch.competitive_mii.size())))
                competitive->PlaySet(launch.competitive_set);
            else {
                const std::string line="Competitive: set "+std::to_string(launch.competitive_set)
                    +" cannot start ("+(competitive ? "this build's game module has no sets" : "competitive play is off")+")";
                std::fprintf(stderr,"%s\n",line.c_str());
            }
        }
        std::chrono::steady_clock::time_point lastCompetitive{};
        // While the game is in its menus: which keys open the online panel, in
        // the top-left corner (set with the overlay; none without it).
        std::optional<mscharged::runtime::HostOverlay::Hint> panelHint;
        std::chrono::steady_clock::time_point panelHintChecked{};
        // The set this game played last, for the card that offers it again.
        std::optional<mscharged::platform::CompetitiveHost::SetEnd> afterSet;
        // A set started in this game that waits for the main menu or the
        // online menu (the player is somewhere else in the menus).
        bool setStartedHere=false;
        std::chrono::steady_clock::time_point setWaiting{}, setReminded{};
        // The players' buttons are held while a card asks something or the
        // set's game passes its menus unseen.
        bool cardHold=false, walkHold=false;
        auto holdButtons = [&] { mscharged::platform::SetNativeWpadHold(cardHold || walkHold); };
        std::optional<mscharged::platform::AlertSound> challengeSound;
        // [online] challenge_sound: the chime at the master volume (not muted),
        // and the window flashes while it is in the background.
        auto alert = [&] {
            if(!launch.settings.challenge_sound)return;
            if(!launch.settings.mute) {
                if(!challengeSound) challengeSound.emplace();
                challengeSound->Play(float(launch.settings.master_volume)/100.0f);
            }
            if(host.window && !(SDL_GetWindowFlags(host.window)&SDL_WINDOW_INPUT_FOCUS))
                SDL_FlashWindow(host.window,SDL_FLASH_UNTIL_FOCUSED);
        };
        auto observeCompetitive = [&] {
            if(!competitive)return;
            const auto now=std::chrono::steady_clock::now();
            if(now-lastCompetitive<std::chrono::seconds(1))return;
            lastCompetitive=now;
            ChargedWfcSnapshot wfc{};
            if(!wfcSnapshot(&wfc,sizeof wfc))return;
            ChargedPresenceSnapshot game{};
            const bool known=presenceSnapshot && presenceSnapshot(&game,sizeof game);
#ifdef MSCHARGED_HAS_ONLINE_SERVER_OVERRIDE
            // How the online series' games ended: one that broke off is
            // reported with its score then.
            if(onlineMatchSnapshot) {
                ChargedOnlineMatchSnapshot online{};
                if(onlineMatchSnapshot(&online,sizeof online)) competitive->ObserveOnline(online);
            }
#endif
            std::optional<ChargedCompetitiveRules> rules;
            if(competitive->Observe(wfc,known ? &game : nullptr,rules))
                configureCompetitive(rules ? &*rules : nullptr,sizeof(ChargedCompetitiveRules));
            const auto set=competitive->CurrentSet();
            if(set) presenceSet=mscharged::platform::SetText{set->opponent,set->won,set->lost,set->games};
            else presenceSet.reset();
            if(configureNames) {
                std::optional<std::pair<std::u16string,std::u16string>> names;
                if(set) names.emplace(mscharged::platform::ScreenName(competitive->PlayerName()),
                    mscharged::platform::ScreenName(set->opponent));
                if(names!=namesGiven) {
                    const auto units=[](const std::u16string& name) {
                        return name.empty() ? nullptr : reinterpret_cast<const unsigned short*>(name.c_str());
                    };
                    if(names) configureNames(units(names->first),units(names->second));
                    else configureNames(nullptr,nullptr);
                    namesGiven=std::move(names);
                }
            }
            // A set that went while this game is in its menus: it goes on into
            // RANKED from the main menu or the online menu, without closing.
            if(const auto start=competitive->TakeSetStart()) {
                if(configureStart
                   && configureStart(launch.competitive_mii.data(),unsigned(launch.competitive_mii.size()))) {
                    competitive->PlaySet(start->match);
                    afterSet.reset();
                    setStartedHere=true;
                    alert();
                    // The panel it was challenged or accepted from closes: the
                    // game goes on into the set.
                    if(hostOverlay) {
                        hostOverlay->ClosePanel();
                        hostOverlay->ShowMessage("Set against "+start->opponent+": off to RANKED",now);
                    }
                } else mscharged::platform::SessionLogLine("Competitive: set "+std::to_string(start->match)
                    +" cannot start here (this build's game module has no sets)");
            }
            // The set's game is done: it stays open, and the card offers the
            // set again.
            if(auto end=competitive->TakeSetEnd()) {
                if(endSet) endSet();
                namesGiven.reset();
                setStartedHere=false;
                using How=mscharged::platform::CompetitiveHost::SetEnd::How;
                const std::string opponent=end->opponent.empty() ? std::string("your opponent") : end->opponent;
                // Without a game played it has no result, and a stopped set the
                // server no longer keeps cannot be played again: only a message.
                // A set a player left or called off goes again as a rematch.
                const bool rematch=end->how==How::Left || end->how==How::OpponentLeft
                    || end->how==How::OpponentGone;
                if(end->how==How::Replaced) {
                    // A second Recharged of the player's took the set: this game has no part in it.
                    if(hostOverlay) hostOverlay->ShowMessage("Your other Recharged plays the set against "+opponent,now);
                } else if(end->how==How::NoResult) {
                    if(hostOverlay) hostOverlay->ShowMessage("The set against "+opponent+" ended without a result",now);
                } else if(end->how!=How::Over && !rematch && !end->can_play_again) {
                    if(hostOverlay) hostOverlay->ShowMessage("The set against "+opponent+" stopped",now);
                } else afterSet=std::move(end);
            }
            // The set's game passes its menus unseen: nobody steps into them.
            if(walkHold!=(wfc.set_unseen!=0)) {
                walkHold=wfc.set_unseen!=0;
                holdButtons();
            }
            // Started here and not on its way yet: say where it goes on from
            // (not during a match).
            if(setStartedHere && wfc.set_stage==CHARGED_SET_BOOT && !competitive->InMatch()) {
                if(setWaiting==std::chrono::steady_clock::time_point{}) setWaiting=now;
                else if(hostOverlay && now-setWaiting>=std::chrono::seconds(6)
                        && now-setReminded>=std::chrono::seconds(15)) {
                    setReminded=now;
                    hostOverlay->ShowMessage("The set waits: go back to the main menu or the online menu,"
                        " and the game takes you to RANKED",now);
                }
            } else setWaiting={};
        };
        // Play RANKED's way there: the buttons are held while the game passes
        // its menus unseen, and the player hears where it goes on from.
        auto observeRanked = [&] {
            if(!startRanked || !wfcSnapshot)return;
            const auto now=std::chrono::steady_clock::now();
            if(now-rankedChecked<std::chrono::milliseconds(250))return;
            rankedChecked=now;
            ChargedWfcSnapshot wfc{};
            if(!wfcSnapshot(&wfc,sizeof wfc))return;
            if(wfc.ranked_stage!=rankedStage) {
                if(rankedStage!=CHARGED_SET_NONE && wfc.ranked_stage==CHARGED_SET_NONE)
                    mscharged::platform::SessionLogLine("Ranked: the game is in RANKED (or the way there stopped)");
                rankedStage=wfc.ranked_stage;
                rankedSince=now;
            }
            if(walkHold!=(wfc.set_unseen!=0)) {
                walkHold=wfc.set_unseen!=0;
                holdButtons();
            }
            if(rankedStage==CHARGED_SET_BOOT && hostOverlay && now-rankedSince>=std::chrono::seconds(6)
               && now-rankedReminded>=std::chrono::seconds(15)) {
                rankedReminded=now;
                hostOverlay->ShowMessage("RANKED waits: go back to the main menu or the online menu,"
                    " and the game takes you there",now);
            }
        };
        // [online] mii_sign_in: a player who played with a Mii before signs in
        // again here. Once a Mii is logged in to WFC (and there is no session),
        // the server gives back the profile that Mii's online account
        // identifies, asked off the frame, once per WFC profile; the session is
        // saved, the flag cleared, and competitive play starts as with the
        // launcher's session.
        struct MiiSignInAnswer {
            bool signedIn=false;
            std::string token, name, error;
        };
        bool miiSignIn=launch.settings.mii_sign_in;
        std::uint32_t miiSignInTried=0;
        std::future<MiiSignInAnswer> miiSignInAnswer;
        std::chrono::steady_clock::time_point lastMiiSignIn{};
        std::optional<std::string> miiSignInServer;
        auto observeMiiSignIn = [&] {
            if(!miiSignIn || competitive || !wfcSnapshot || !configureCompetitive)return;
            const auto now=std::chrono::steady_clock::now();
            if(miiSignInAnswer.valid()) {
                if(miiSignInAnswer.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
                const auto answer=miiSignInAnswer.get();
                if(!answer.signedIn) {
                    mscharged::platform::SessionLogLine("Competitive: signing in again with this Mii: "+answer.error);
                    if(hostOverlay) hostOverlay->ShowMessage(answer.error,now);
                    return;
                }
                const auto file=mscharged::platform::ProfileSessionFile(mscharged::DataFolder(executableDirectory),
                    launch.config.path);
                try {
                    mscharged::platform::SaveProfileSession(file,{*miiSignInServer,answer.token,answer.name});
                    mscharged::SaveMiiSignIn(launch.config.path,false);
                } catch(const std::exception& error) {
                    mscharged::platform::SessionLogLine(std::string("Competitive: the session was not saved: ")+error.what());
                }
                miiSignIn=false;
                std::string off;
                competitive=mscharged::platform::CompetitiveHost::Start(launch.settings.profile_server,file,
                    [](const std::string& text) {
                        const std::string time=mscharged::platform::SessionLogTimes() ? std::string()
                            : mscharged::platform::LogTimeStamp();
                        std::fprintf(stderr,"%s%s\n",time.c_str(),text.c_str());
                    },&off);
                const auto line=competitive ? "Competitive: on, signed in again as "+competitive->PlayerName()+" by this Mii"
                    : "Competitive: off ("+off+")";
                mscharged::platform::SessionLogLine(line);
                if(hostOverlay && competitive) hostOverlay->ShowMessage("Signed in again as "+answer.name,now);
                return;
            }
            if(now-lastMiiSignIn<std::chrono::seconds(1))return;
            lastMiiSignIn=now;
            ChargedWfcSnapshot wfc{};
            if(!wfcSnapshot(&wfc,sizeof wfc) || !wfc.logged_in || wfc.profile_id==0 || wfc.profile_id==miiSignInTried)return;
            if(!miiSignInServer) miiSignInServer=mscharged::platform::ProfileServerAddress(launch.settings.profile_server);
            if(!miiSignInServer) {
                miiSignIn=false;
                return;
            }
            miiSignInTried=wfc.profile_id;
            miiSignInAnswer=std::async(std::launch::async,
                [server=*miiSignInServer,profile=std::uint32_t(wfc.profile_id),ticket=std::string(wfc.login_ticket)] {
                    MiiSignInAnswer answer;
                    try {
                        mscharged::platform::ProfileService api(server);
                        api.SetActivity("game");
                        const auto result=api.WfcSignIn(profile,ticket);
                        answer.signedIn=result.status==mscharged::platform::SignInResult::Status::SignedIn
                            && !result.token.empty();
                        answer.token=result.token;
                        answer.name=result.player.name;
                        if(!answer.signedIn) answer.error=result.message.empty() ? "the server refused it" : result.message;
                    } catch(const mscharged::platform::ProfileError& error) {
                        answer.error=error.Status()==404 ? "this Mii plays for no Recharged profile" : error.what();
                    } catch(const std::exception& error) {
                        answer.error=error.what();
                    }
                    return answer;
                });
            mscharged::platform::SessionLogLine("Competitive: signing in again with the Mii logged in to WFC");
        };
        // A card over the dimmed picture while the game is in its menus, which
        // holds every player's buttons: a challenge (A accepts, and the set
        // starts in this game; B declines), or after a set the same set again
        // (A: a rematch, or Ready to play! again for one that stopped; B: not
        // now).
        enum class CardKind { None, Challenge, AfterSet };
        CardKind cardKind=CardKind::None;
        std::int64_t cardMatch=0;
        std::chrono::steady_clock::time_point cardShown{};
        // With the game's overlay the card is its prompt over the blurred game
        // (below): what it asks, and the answer (1 its first button, 2 the
        // second, 0 none yet).
        struct CardPrompt {
            enum class Kind { Challenge, Rematch, SetOver, SetStopped } kind=Kind::Challenge;
            bool shown=false;
            int answer=0;
            std::string opponent, rules, reason;
            std::string title;  // a stopped set's own title, or empty
            std::string primary, note; // a stopped set's first button and its note, or empty
            std::string avatar; // the opponent's picture (URL), or empty
            bool random=false;  // a search for a random opponent asks the player
            bool hasScore=false;
            int won=0, lost=0, games=0;
        } cardPrompt;
        bool cardAsPrompt=false; // set once the overlay exists
        auto serviceChallengeCard = [&](std::chrono::steady_clock::time_point now) {
            if(!competitive || !hostOverlay)return;
            if(const auto notice=competitive->TakeNotice()) hostOverlay->ShowMessage(*notice,now);
            const auto invitation=competitive->PendingInvitation();
            CardKind kind=CardKind::None;
            std::int64_t match=0;
            if(invitation) {
                // A challenge (their rematch, too) takes the place of the
                // card that offers the last set again.
                kind=CardKind::Challenge;
                match=invitation->match;
                afterSet.reset();
            } else if(afterSet && !competitive->InMatch()) {
                // The last set again, once back in the menus.
                kind=CardKind::AfterSet;
                match=afterSet->match;
            }
            if(kind!=cardKind || match!=cardMatch) {
                cardKind=kind;
                cardMatch=match;
                cardShown=now;
                // The prompt takes the players' input itself (the host panel).
                cardHold=kind!=CardKind::None && !cardAsPrompt;
                holdButtons();
                cardPrompt=CardPrompt{};
                cardPrompt.shown=kind!=CardKind::None && cardAsPrompt;
                mscharged::runtime::HostOverlay::Card card;
                if(kind==CardKind::Challenge) {
                    card.kicker=invitation->rematch ? "REMATCH" : invitation->random ? "RANDOM CHALLENGE" : "CHALLENGE";
                    card.title=invitation->from+(invitation->rematch ? " wants a rematch" : " challenges you");
                    card.detail=invitation->rules+".";
                    card.accept="Accept";
                    card.decline="Decline";
                    card.note="Accept goes on into RANKED: straight to the captain draft.";
                    cardPrompt.kind=invitation->rematch ? CardPrompt::Kind::Rematch : CardPrompt::Kind::Challenge;
                    cardPrompt.random=invitation->random;
                    cardPrompt.opponent=invitation->from;
                    cardPrompt.avatar=invitation->avatar;
                    cardPrompt.rules=invitation->rules;
                    if(!cardAsPrompt) hostOverlay->SetCard(card);
                    mscharged::platform::SessionLogLine("Competitive: "+invitation->from
                        +(invitation->rematch ? " wants a rematch" : " challenges you")
                        +" (match "+std::to_string(match)+")");
                    alert();
                } else if(kind==CardKind::AfterSet) {
                    using How=mscharged::platform::CompetitiveHost::SetEnd::How;
                    const auto& end=*afterSet;
                    const std::string opponent=end.opponent.empty() ? std::string("your opponent") : end.opponent;
                    const std::string score=std::to_string(std::max(end.won,end.lost))+"-"
                        +std::to_string(std::min(end.won,end.lost));
                    if(end.how==How::Over) {
                        card.kicker="SET OVER";
                        card.title=end.won>end.lost ? "You won "+score+" against "+opponent
                            : end.lost>end.won ? opponent+" won "+score
                            : "A draw against "+opponent;
                        card.detail="The same opponent and rules again: "+opponent+" gets the challenge.";
                        card.accept="Rematch";
                        card.decline="Not now";
                        card.note="Once they accept, the game goes on into RANKED.";
                        cardPrompt.kind=CardPrompt::Kind::SetOver;
                    } else if(end.how==How::OpponentLeft || end.how==How::OpponentGone) {
                        // The opponent's Back, launcher or closed game: a
                        // line from the dressing room, the same for a set.
                        static const char* const kGone[]={
                            " left the field in tears…"," ran crying to the dressing room…",
                            " took the ball and went home…"," saw your lineup and fled the pitch…",
                            " heard the Kritters were coming and ran…"};
                        card.kicker="SET STOPPED";
                        card.title=opponent+kGone[std::uint64_t(end.match)%std::size(kGone)];
                        card.detail=end.how==How::OpponentGone
                            ? opponent+"'s game closed before your games met, so the set stopped."
                            : opponent+" left before your games met, so the set stopped.";
                        card.accept="Rematch";
                        card.decline="Not now";
                        card.note="The same set again: "+opponent+" gets the challenge.";
                        cardPrompt.kind=CardPrompt::Kind::SetStopped;
                        cardPrompt.reason=card.detail;
                        cardPrompt.title=card.title;
                        cardPrompt.primary=card.accept;
                        cardPrompt.note=card.note;
                    } else {
                        card.kicker="SET STOPPED";
                        card.title=end.how==How::Left ? "You left the set against "+opponent
                            : "The set against "+opponent+" stopped";
                        card.detail=end.how==How::Failed ? std::string("A message in the game ended it.")
                            : end.how==How::Left ? "You left the search; "+opponent+"'s game was told."
                            : opponent+" isn't ready any more, or your games didn't meet in time.";
                        // Left: the set is called off, so it goes again as a
                        // rematch; else it waits, and Not now calls it off.
                        const bool left=end.how==How::Left;
                        card.accept=left ? "Rematch" : "Play it again";
                        card.decline="Not now";
                        card.note=left ? "The same set again: "+opponent+" gets the challenge."
                            : "Play it again is your Ready to play!: the set goes once "+opponent
                                +" is ready too. Not now calls it off.";
                        cardPrompt.kind=CardPrompt::Kind::SetStopped;
                        cardPrompt.reason=card.detail;
                        cardPrompt.title=card.title;
                        cardPrompt.primary=card.accept;
                        cardPrompt.note=card.note;
                    }
                    cardPrompt.opponent=opponent;
                    cardPrompt.avatar=end.opponent_avatar;
                    cardPrompt.hasScore=end.how==How::Over || end.won+end.lost>0;
                    cardPrompt.won=end.won;
                    cardPrompt.lost=end.lost;
                    cardPrompt.games=end.games;
                    if(!cardAsPrompt) hostOverlay->SetCard(card);
                } else hostOverlay->SetCard(std::nullopt);
            }
            if(kind==CardKind::None)return;
            bool accept=false;
            if(cardAsPrompt) {
                // The prompt's buttons (or Escape, a click beside it: the second).
                if(!cardPrompt.answer)return;
                accept=cardPrompt.answer==1;
                cardPrompt.answer=0;
            } else {
                // Presses in the card's first moment are not answers (a button
                // the player was pressing for the menu).
                const auto presses=mscharged::platform::TakeNativeWpadHeldPresses();
                if(now-cardShown<std::chrono::milliseconds(600) || (!presses.a && !presses.b))return;
                accept=presses.a>0;
            }
            if(kind==CardKind::Challenge) {
                competitive->Answer(cardMatch,accept);
                hostOverlay->ShowMessage(accept ? "Challenge accepted: off to RANKED" : "Challenge declined",now);
                return;
            }
            using How=mscharged::platform::CompetitiveHost::SetEnd::How;
            // A set that was played or called off goes again as a rematch (a
            // new challenge); one that only stopped, by Ready to play! again.
            const How how=afterSet->how;
            const bool rematch=how==How::Over || how==How::Left || how==How::OpponentLeft || how==How::OpponentGone;
            const std::string opponent=afterSet->opponent.empty() ? std::string("your opponent") : afterSet->opponent;
            if(accept) {
                competitive->PlayAgain(cardMatch,rematch);
                hostOverlay->ShowMessage((rematch ? "Rematch asked: waiting for " : "Ready again: waiting for ")
                    +opponent,now);
            } else if(how==How::CalledOff || how==How::Failed) {
                // Not now: the stopped set is called off, so neither player
                // keeps it and both can take another challenge at once.
                competitive->CallOff(cardMatch);
            }
            afterSet.reset();
        };
        bool netplaySession=false;
        auto observeNetplay = [&] {
            if((!hostOverlay && !framePacing) || !netplaySnapshot)return;
            const auto now=std::chrono::steady_clock::now();
            if(now-lastNetplay<std::chrono::seconds(1))return;
            lastNetplay=now;
            ChargedNetplaySnapshot snapshot{};
            if(!netplaySnapshot(&snapshot,sizeof snapshot))return;
            netplaySession=snapshot.network!=0;
            if(framePacing && framePacing->SetRollbackCap(snapshot.rollback!=0)) {
                const auto line="Frame rate: "+framePacing->Description();
                mscharged::platform::SessionLogLine(line);
                std::fprintf(stderr,"%s\n",line.c_str());
            }
            if(!hostOverlay)return;
            mscharged::runtime::HostOverlay::Network network;
            network.session=snapshot.network!=0;
            if(snapshot.round_trip_ms>=0)network.latency_ms=snapshot.round_trip_ms;
            network.rollback=snapshot.rollback!=0;
            if(network.rollback){
                network.delay_ms=snapshot.input_delay_ms;
                network.rollback_frames=snapshot.predicted_steps;
            }
            hostOverlay->SetNetwork(network);
        };
        // The online panel (F1 or HOME) over the blurred game. Online the game
        // goes on; in an offline match in play it presses the game's pause.
        std::optional<mscharged::runtime::HostPanel> hostPanel;
        if(hostOverlay && interactive) {
            hostPanel.emplace(host.window);
            hostPanel->SetPauseRule([&] {
                ChargedPresenceSnapshot game{};
                if(netplaySession || !presenceSnapshot || !presenceSnapshot(&game,sizeof game))return false;
                // Not online, nor the title screen's demo (mode 2), nor with the
                // computer playing both teams (none of this machine's pads pause).
                if(!game.in_match || game.online || game.mode==2 || !game.human_sides)return false;
                // Paused already (its pause menu is open), and only in play.
                if(game.match_menu)return false;
                return game.game_state==1 || game.game_state==5 || game.game_state==6; // kick-off, play, overtime
            });
            hostOverlay->SetPanel(&*hostPanel);
        }
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
        // The game's overlay in the panel: the Online tab (the player's set,
        // friends online, players available, with its data from the server on
        // a thread of its own while open) and Game options (saved to the
        // settings file at once; the game gets them between matches).
        mscharged::overlay::GameOverlayState overlayState;
        // The players' pictures: downloaded on a thread, uploaded to Aurora's
        // ImGui on the panel's (owner) thread when drawn; initials until then.
        mscharged::overlay::OverlayImageCache overlayPictures([](int width,int height,const void* rgba) {
            return aurora_imgui_add_texture(std::uint32_t(width),std::uint32_t(height),rgba);
        });
        std::string overlayPicturesError;
        std::unique_ptr<mscharged::platform::OnlinePanelFeed> panelFeed;
        bool panelFeedLate=false; // started once more after a sign-in in this game
        auto optionTweaks=launch.settings.tweaks;
        // The player's own Mii (the one sets use) as the face on the Online
        // tab's own card when there is no Discord picture: drawn on a thread
        // from the game image's Mii parts, uploaded to ImGui once.
        struct OwnMiiFace {
            std::vector<std::uint8_t> rgba;
            int width=0, height=0;
        };
        std::future<std::optional<OwnMiiFace>> ownMiiFace;
        ImTextureID ownMiiTexture{};
        // The flags beside players' names: one atlas (overlay/flags.h),
        // uploaded the first time the menu draws.
        ImTextureID flagTexture{};
        bool flagTried=false;
        // The player's own Mii (the launcher's): RANKED with it, its face in the menus.
        const bool ownMii=std::any_of(launch.competitive_mii.begin(),launch.competitive_mii.end(),
            [](auto b) { return b!=0; });
        if(ownMii)
            ownMiiFace=std::async(std::launch::async,
                [disc=launch.disc_path,id=launch.competitive_mii,
                 nand=mscharged::TitleSaveFolder(mscharged::ResolveSaveFolder(launch.settings,launch.config.path,
                     executableDirectory),release->title_id),
                 title=mscharged::platform::SaveTitle{release->title_id,0x1001,release->title_group}]()
                    -> std::optional<OwnMiiFace> {
                    try {
                        const auto miis=mscharged::platform::InstalledMiis(nand,title);
                        for(const auto& mii:miis.miis)
                            if(mii.create_id==id) {
                                namespace head=mscharged::platform::mii;
                                head::MiiHeadOptions options;
                                options.width=options.height=128;
                                options.background=head::MiiHeadOptions::Background::Favorite;
                                auto image=head::RenderMiiHead(head::ReadMiiPartsFromImage(disc),mii.record,options);
                                return OwnMiiFace{std::move(image.pixels),image.width,image.height};
                            }
                    } catch(const std::exception& error) {
                        mscharged::platform::SessionLogLine(std::string("Overlay: your Mii's face is not drawn (")
                            +error.what()+")");
                    }
                    return std::nullopt;
                });
        // [online] challenge_rules: chosen in the launcher or on the Online tab.
        std::string challengeRules=launch.settings.challenge_rules;
        // The player's own presets (the launcher's Match rules page, files in
        // match-presets beside the settings file), read as the overlay opens.
        std::vector<mscharged::overlay::OnlinePanelPreset> myPresets;
        std::vector<mscharged::overlay::OnlinePanelPreset> builtInPresets;
        for(const auto& preset:mscharged::launcher::BuiltInMatchPresets())
            builtInPresets.push_back({preset.name,mscharged::launcher::ChallengeRules(preset),0});
        // The 3D resolutions the Settings tab offers, by the height they draw at.
        std::vector<std::string> displayResolutions;
        std::optional<unsigned> pendingTweaks;
        std::string overlayNotice;
        bool overlayOpen=false;
        std::chrono::steady_clock::time_point overlayOpened{};
        // The card as the overlay's prompt: open while it asks, and the card
        // put aside for the full overlay (F1, HOME).
        mscharged::overlay::OverlayPromptState promptState;
        bool promptOpen=false;
        std::int64_t promptPutAside=0;
        auto setTweaks=reinterpret_cast<void(*)(unsigned)>(
            mscharged::platform::FindNativeModuleSymbol(module,"charged_set_tweaks"));
        // A match (not the title screen's demo) is played.
        auto inMatch=[&] {
            ChargedPresenceSnapshot game{};
            return presenceSnapshot && presenceSnapshot(&game,sizeof game) && game.in_match && game.mode!=2;
        };
        // The server's version is at least major.minor.patch ("1.2.1").
        auto versionAtLeast=[](const std::string& text,int major,int minor,int patch) {
            int parts[3]={0,0,0};
            if(std::sscanf(text.c_str(),"%d.%d.%d",&parts[0],&parts[1],&parts[2])<2)return false;
            return std::tie(parts[0],parts[1],parts[2])>=std::tie(major,minor,patch);
        };
        // The Settings tab's and Recharged's menus' model of the display and
        // sound, and their changes of the settings and the game options: at
        // once, and saved for the next time.
        // The launcher's Advanced settings the menus show: as saved (most
        // count from the next start; the running game keeps its own).
        mscharged::Settings advanced=launch.settings;
        std::vector<std::string> graphicsApis; // the choices' settings, as the menus list them
        std::vector<std::string> graphicsCards;
#if defined(MSCHARGED_HAS_GRAPHICS_CARDS)
        std::future<std::vector<mscharged::platform::GraphicsCard>> cardScan;
        std::optional<std::vector<mscharged::platform::GraphicsCard>> cardsFound;
#endif
        // The graphics cards, listed once in the background from the first
        // time a menu shows (a Vulkan instance of its own: not while the
        // Advanced page is being used).
        auto pollGraphicsCards=[&] {
#if defined(MSCHARGED_HAS_GRAPHICS_CARDS) && !defined(__APPLE__)
            if(!cardsFound && !cardScan.valid())
                cardScan=std::async(std::launch::async,mscharged::platform::ListGraphicsCards);
            if(!cardsFound && cardScan.valid()
               && cardScan.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
                try { cardsFound=cardScan.get(); } catch(const std::exception&) { cardsFound.emplace(); }
            }
#endif
        };
        auto buildAdvanced=[&](mscharged::overlay::GameSettingsModel& settings) {
            settings.launcher_sizes.clear();
            settings.launcher_size=0;
            for(std::size_t n=0;n<mscharged::kLauncherSizes.size();++n) {
                settings.launcher_sizes.push_back(mscharged::kLauncherSizes[n].label);
                if(advanced.ui_scale==mscharged::kLauncherSizes[n].value) settings.launcher_size=int(n);
            }
            // The graphics API: Automatic only where it may pick between two (Windows).
            graphicsApis.clear();
            settings.graphics_apis.clear();
            if(mscharged::kGraphicsBackends.size()>1) {
                graphicsApis.push_back("auto");
                settings.graphics_apis.push_back("Automatic");
            }
            for(const auto& choice:mscharged::kGraphicsBackends) {
                graphicsApis.push_back(choice.value);
                settings.graphics_apis.push_back(choice.label);
            }
            settings.graphics_api=0;
            for(std::size_t n=0;n<graphicsApis.size();++n)
                if(advanced.backend==graphicsApis[n]) settings.graphics_api=int(n);
            // The graphics cards, listed once in the background.
            graphicsCards.assign(1,"auto");
            settings.graphics_cards.assign(1,"Automatic (fastest)");
            settings.graphics_card=0;
#if defined(MSCHARGED_HAS_GRAPHICS_CARDS) && !defined(__APPLE__)
            settings.graphics_cards_shown=true;
            pollGraphicsCards();
            settings.graphics_cards_listing=!cardsFound;
            if(cardsFound)
                for(const auto& card:*cardsFound) {
                    bool twin=false;
                    for(const auto& other:*cardsFound) twin=twin || (other.id!=card.id && other.name==card.name);
                    const std::string id=mscharged::GraphicsCardSetting(card.id);
                    graphicsCards.push_back(id);
                    settings.graphics_cards.push_back(card.name+(twin ? " ("+id+")" : ""));
                }
            if(advanced.gpu!="auto") {
                const auto found=std::find(graphicsCards.begin(),graphicsCards.end(),advanced.gpu);
                if(found!=graphicsCards.end()) settings.graphics_card=int(found-graphicsCards.begin());
                else {
                    // Not (yet) listed: kept as it is.
                    graphicsCards.push_back(advanced.gpu);
                    settings.graphics_cards.push_back(cardsFound ? "Not found: "+advanced.gpu : advanced.gpu);
                    settings.graphics_card=int(graphicsCards.size())-1;
                }
            }
#endif
            settings.frame_logging=advanced.frame_logging;
            settings.graphics_validation=advanced.graphics_validation;
            settings.log_detail=mscharged::kDefaultLogLevel;
            for(std::size_t n=0;n<mscharged::kLogLevels.size();++n)
                if(advanced.log_level==mscharged::kLogLevels[n].value) settings.log_detail=int(n);
            settings.verbose_console=advanced.verbose_console;
#if defined(_WIN32) || defined(__APPLE__)
            settings.log_window_shown=true;
#endif
            settings.log_window=advanced.log_window;
        };
        auto buildSettings=[&](mscharged::overlay::GameSettingsModel& settings) {
            settings.fullscreen=(SDL_GetWindowFlags(host.window)&SDL_WINDOW_FULLSCREEN)!=0;
            settings.pictures={widescreen ? "16:9" : "4:3","Stretch"};
            if(widerView) settings.pictures.push_back("Wider view");
            settings.picture=widerOn ? 2 : stretch ? 1 : 0;
            settings.picture_about=std::string(widescreen ? "16:9" : "4:3")+" keeps the game's shape,"
                " Stretch fills the window"+(widerView ? ", Wider view shows more of the stadium in matches." : ".");
            displayResolutions.clear();
            if(nativeSend) {
                const auto size=aurora_get_window_size();
                const auto picture=mscharged::PictureSize(int(size.native_fb_width),int(size.native_fb_height),
                    int(aspectWidth),int(aspectHeight),fill);
                // As the launcher has them: Full, Native, 2x, 3x, 4x.
                displayResolutions.emplace_back("window");
                for(const auto value:mscharged::kRenderResolutions)
                    if(value!="window") displayResolutions.emplace_back(value);
                for(std::size_t n=0;n<displayResolutions.size();++n) {
                    const auto& value=displayResolutions[n];
                    settings.resolutions.emplace_back(mscharged::RenderResolutionLabel(value));
                    if(value==renderResolution) settings.resolution=int(n);
                }
                const auto drawn=[&](std::string_view value) {
                    return std::to_string(mscharged::RenderWidth(value,picture.width))+" \xC3\x97 "
                        +std::to_string(mscharged::RenderHeight(value,picture.height));
                };
                settings.resolution_about="How sharp the game draws its 3D: Full at the window's size ("
                    +drawn("window")+"), Native at the Wii's "+drawn("native")
                    +", 2x to 4x at its multiples, for slower graphics cards.";
            }
            settings.performance_numbers=launch.settings.fps_overlay;
            settings.master_volume=launch.settings.master_volume;
            settings.mute=launch.settings.mute;
            settings.sound_volumes=launch.settings.sound_volumes;
            settings.challenge_sound=launch.settings.challenge_sound;
            settings.reopen_launcher=mscharged::platform::LauncherReopenOn();
            settings.reopen_known=mscharged::platform::LauncherReopenKnown();
            buildAdvanced(settings);
        };
        auto applyOption=[&](const mscharged::overlay::GameOptionChange& change) {
            const std::size_t index=mscharged::GameTweakIndex(change.tweak);
            if(index<optionTweaks.size()) {
                optionTweaks[index]=change.on;
                try {
                    mscharged::SaveGameTweak(launch.config.path,change.tweak,change.on);
                } catch(const std::exception& error) {
                    overlayNotice=std::string("The option was not saved: ")+error.what();
                }
                auto settings=launch.settings;
                settings.tweaks=optionTweaks;
                pendingTweaks=settings.ModuleTweakMask();
            }
        };
        auto applySetting=[&](const mscharged::overlay::GameSettingChange& change) {
            namespace ov=mscharged::overlay;
            const bool on=change.value!=0;
            using ov::GameSetting;
            std::function<void(mscharged::Settings&)> save;
            switch(change.setting) {
            case GameSetting::Fullscreen:
                if(SDL_SetWindowFullscreen(host.window,on)) {
                    if(on) mscharged::runtime::PlaceFullscreenWindow(host.window);
                    mscharged::runtime::LockWindowShape(host.window,!on && !stretch && !widerOn,aspectWidth,
                        aspectHeight);
                    mscharged::platform::SessionLogLine(std::string("Display: ")+(on?"fullscreen":"windowed")
                        +" (Settings)");
                }
                launch.settings.fullscreen=on;
                save=[on](mscharged::Settings& s) { s.fullscreen=on; };
                break;
            case GameSetting::Picture: {
                // As the stretch hotkey: the wider view when set up, else stretching.
                widerOn=widerView && change.value==2;
                stretch=change.value==1;
                const bool windowed=!(SDL_GetWindowFlags(host.window)&SDL_WINDOW_FULLSCREEN);
                mscharged::runtime::LockWindowShape(host.window,windowed && !stretch && !widerOn,aspectWidth,
                    aspectHeight);
                const bool wider=widerOn, stretched=stretch;
                const std::string keep=launch.settings.aspect=="stretch"?std::string("auto"):launch.settings.aspect;
                launch.settings.aspect=stretched?"stretch":keep;
                if(widerView) launch.settings.wider_view=wider;
                save=[wider,stretched,keep,canWiden=widerView](mscharged::Settings& s) {
                    s.aspect=stretched?"stretch":keep;
                    if(canWiden) s.wider_view=wider;
                };
                break;
            }
            case GameSetting::Resolution:
                if(change.value>=0 && std::size_t(change.value)<displayResolutions.size()
                   && displayResolutions[std::size_t(change.value)]!=renderResolution) {
                    renderResolution=displayResolutions[std::size_t(change.value)];
                    VISetFrameBufferScale(mscharged::RenderScale(renderResolution));
                    mscharged::platform::SessionLogLine("Display: 3D resolution "+renderResolution+" (Settings)");
                    launch.settings.resolution=renderResolution;
                    save=[value=renderResolution](mscharged::Settings& s) { s.resolution=value; };
                }
                break;
            case GameSetting::PerformanceNumbers:
                launch.settings.fps_overlay=on;
                hostOverlay->SetPerformance(on ? std::optional(mscharged::runtime::ParseOverlayCorner(
                    launch.settings.fps_overlay_corner).value_or(mscharged::runtime::OverlayCorner::TopRight))
                    : std::nullopt);
                save=[on](mscharged::Settings& s) { s.fps_overlay=on; };
                break;
            case GameSetting::MasterVolume:
            case GameSetting::Mute:
                if(change.setting==GameSetting::Mute) launch.settings.mute=on;
                else launch.settings.master_volume=std::clamp(change.value,0,100);
                mscharged::platform::SetNativeAIOutputGain(launch.settings.mute ? 0.0f :
                    float(launch.settings.master_volume)/100.0f);
                save=[volume=launch.settings.master_volume,mute=launch.settings.mute](mscharged::Settings& s) {
                    s.master_volume=volume;
                    s.mute=mute;
                };
                break;
            case GameSetting::SoundVolume:
                if(change.group>=0 && std::size_t(change.group)<launch.settings.sound_volumes.size()) {
                    launch.settings.sound_volumes[std::size_t(change.group)]=std::clamp(change.value,0,100);
                    const auto& volumes=launch.settings.sound_volumes;
                    if(configureSoundVolumes) configureSoundVolumes(volumes.data(),int(volumes.size()));
                    save=[volumes](mscharged::Settings& s) { s.sound_volumes=volumes; };
                }
                break;
            case GameSetting::ChallengeSound:
                launch.settings.challenge_sound=on;
                save=[on](mscharged::Settings& s) { s.challenge_sound=on; };
                break;
            case GameSetting::ReopenLauncher:
                mscharged::platform::SetLauncherReopen(on);
                launch.settings.reopen_after_game=on;
                save=[on](mscharged::Settings& s) { s.reopen_after_game=on; };
                break;
            // The launcher's Advanced settings: saved for the launcher or the
            // next start; the console's detail and the log window at once too.
            case GameSetting::LauncherSize:
                if(change.value>=0 && std::size_t(change.value)<mscharged::kLauncherSizes.size()) {
                    const std::string size=mscharged::kLauncherSizes[std::size_t(change.value)].value;
                    save=[size](mscharged::Settings& s) { s.ui_scale=size; };
                }
                break;
            case GameSetting::GraphicsApi:
                if(change.value>=0 && std::size_t(change.value)<graphicsApis.size()) {
                    const std::string api=graphicsApis[std::size_t(change.value)];
                    save=[api](mscharged::Settings& s) { s.backend=api; };
                }
                break;
            case GameSetting::GraphicsCard:
                if(change.value>=0 && std::size_t(change.value)<graphicsCards.size()) {
                    const std::string card=graphicsCards[std::size_t(change.value)];
                    save=[card](mscharged::Settings& s) { s.gpu=card; };
                }
                break;
            case GameSetting::FrameLogging:
                save=[on](mscharged::Settings& s) { s.frame_logging=on; };
                break;
            case GameSetting::GraphicsValidation:
                save=[on](mscharged::Settings& s) { s.graphics_validation=on; };
                break;
            case GameSetting::LogDetail:
                if(change.value>=0 && std::size_t(change.value)<mscharged::kLogLevels.size()) {
                    const std::string level=mscharged::kLogLevels[std::size_t(change.value)].value;
                    save=[level](mscharged::Settings& s) { s.log_level=level; };
                }
                break;
            case GameSetting::VerboseConsole:
                mscharged::platform::SetVerboseConsole(on);
                save=[on](mscharged::Settings& s) { s.verbose_console=on; };
                break;
            case GameSetting::LogWindow:
                mscharged::platform::SetLogWindow(on);
                save=[on](mscharged::Settings& s) { s.log_window=on; };
                break;
            }
            // The menus' Advanced page shows what is saved (most of it counts
            // from the next start).
            if(save) save(advanced);
            // A slider being dragged saves once it is let go.
            if(save && !change.live) {
                try {
                    mscharged::SaveSettings(launch.config.path,save);
                } catch(const std::exception& error) {
                    overlayNotice=std::string("The setting was not saved: ")+error.what();
                }
            }
        };
        // The overlay's and Recharged's menus' players' pictures (an avatar,
        // "mii:own", the flags' atlas), and their Online, Ranked and prompt:
        // the model as it is now and what the player did.
        auto overlayImage=[&](const std::string& key)->ImTextureID {
            if(!flagTried) {
                flagTried=true;
                std::ifstream file(executableDirectory/"assets"/"flags"/"flags.png",std::ios::binary);
                const std::string bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
                mscharged::overlay::OverlayImage atlas;
                if(!bytes.empty() && mscharged::overlay::DecodeOverlayPng(bytes,atlas))
                    flagTexture=aurora_imgui_add_texture(std::uint32_t(atlas.width),std::uint32_t(atlas.height),
                        atlas.rgba.data());
                if(!flagTexture) mscharged::platform::SessionLogLine("Overlay: no flags (assets/flags/flags.png)");
            }
            if(key=="mii:own") return ownMiiTexture;
            if(key==mscharged::overlay::kFlagAtlas) return flagTexture;
            return overlayPictures.Images()(key);
        };
        // `online` and `settings`: those parts are built (they cost; the rest
        // of the model stays as made); `leaderboard`: it shows, its data is fetched.
        auto buildOverlayModel=[&](mscharged::overlay::GameOverlayModel& model,bool leaderboard,bool online=true,
                                   bool settings=true) {
            namespace ov=mscharged::overlay;
            // Signed in again through the game (mii_sign_in): its data from now on.
            if(!panelFeed && competitive && !panelFeedLate) {
                panelFeedLate=true;
                std::string off;
                panelFeed=mscharged::platform::OnlinePanelFeed::Start(launch.settings.profile_server,
                    mscharged::platform::ProfileSessionFile(mscharged::DataFolder(executableDirectory),launch.config.path),
                    challengeRules,launch.discord_friends,
                    [](const std::string& text) { mscharged::platform::SessionLogLine(text); },&off);
                if(!panelFeed) mscharged::platform::SessionLogLine("Overlay: the Online tab is off ("+off+")");
            }
            if(panelFeed) if(auto notice=panelFeed->TakeNotice()) overlayNotice=*notice;
            const bool inMatchNow=inMatch();
            if(online) {
                const auto snapshot=panelFeed ? panelFeed->Latest() : mscharged::platform::OnlinePanelFeed::Snapshot{};
                ov::OnlinePanelContext context;
                context.me=snapshot.me;
                context.discord_friends=snapshot.discord_friends;
                if(context.me.name.empty() && competitive) context.me.name=competitive->PlayerName();
                context.in_match=inMatchNow;
                context.game=std::string(release->game_id,4);
                if(competitive)
                    if(const auto set=competitive->CurrentSet())
                        context.score=ov::OnlinePanelContext::Score{set->won,set->lost};
                // What a challenge starts from: the server's rule sets and the
                // player's presets, the one used last ([online] challenge_rules) first.
                context.rule_sets=snapshot.rule_sets;
                context.my_presets=myPresets;
                context.built_in_presets=builtInPresets;
                context.chosen_rules=challengeRules;
                context.can_challenge=!snapshot.rule_sets.empty();
                context.random_games=versionAtLeast(snapshot.server_version,1,2,1);
                // A search for a random opponent (servers 1.2.4 on), and how long it runs.
                context.searches=versionAtLeast(snapshot.server_version,1,2,4);
                context.search=snapshot.search;
                if(snapshot.search)
                    context.search_seconds=int(snapshot.server_time-snapshot.search->since
                        +std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
                            -snapshot.fetched).count());
                context.loading=panelFeed && !snapshot.loaded;
                // Without a player profile the tab invites to make one.
                context.signed_out=!panelFeed;
                if(panelFeed && !snapshot.error.empty()) {
                    context.message=snapshot.error;
                    context.error=true;
                } else context.message=overlayNotice;
                model.online=ov::MakeOnlinePanelModel(snapshot.lists,snapshot.match,context);
                // Ranked: Play RANKED (the player's own Mii, else one they
                // choose in the game), and the leaderboards while it is shown.
                {
                    auto& ranked=model.ranked;
                    ranked.going=rankedStage!=CHARGED_SET_NONE;
                    ranked.waiting=rankedStage==CHARGED_SET_BOOT
                        && std::chrono::steady_clock::now()-rankedSince>=std::chrono::seconds(2);
                    ranked.own_mii=ownMii;
                    if(!startRanked) ranked.blocked="This game can't go to RANKED by itself.";
                    else if(context.in_match) ranked.blocked="You're in a match: RANKED waits until it is over.";
                    else if(competitive && competitive->CurrentSet())
                        ranked.blocked="Your set goes on: RANKED waits until it is over.";
                    else if(snapshot.search) ranked.blocked="You're searching for an opponent: stop that first.";
                    ranked.can_play=ranked.blocked.empty() && !ranked.going;
                    ranked.signed_out=!panelFeed;
                    ranked.boards=snapshot.leaderboard;
                    ranked.missing=snapshot.leaderboard_missing;
                }
            }
            if(panelFeed) panelFeed->WantLeaderboard(leaderboard);
            if(ownMiiFace.valid() && ownMiiFace.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                if(const auto face=ownMiiFace.get())
                    ownMiiTexture=aurora_imgui_add_texture(std::uint32_t(face->width),std::uint32_t(face->height),
                        face->rgba.data());
            if(online && ownMiiTexture && model.online.me.image.empty()) model.online.me.image="mii:own";
            model.options.tweaks=optionTweaks;
            model.options.in_match=inMatchNow;
            // Settings: the display and sound as they are now.
            if(settings) buildSettings(model.settings);
            else pollGraphicsCards();
        };
        auto applyOnline=[&](const mscharged::overlay::OnlinePanelActions& online,
                             const mscharged::overlay::RankedActions& ranked,
                             const mscharged::overlay::GameOverlayModel& model)->bool {
            bool started=false;
            if(ranked.play && startRanked && model.ranked.can_play) {
                if(startRanked(ownMii ? launch.competitive_mii.data() : nullptr,
                               ownMii ? unsigned(launch.competitive_mii.size()) : 0u)) {
                    mscharged::platform::SessionLogLine(std::string("Ranked: Play RANKED")
                        +(ownMii ? " with the player's own Mii" : ", the player chooses the Mii"));
                    rankedStage=CHARGED_SET_BOOT;
                    rankedSince=std::chrono::steady_clock::now();
                    started=true;
                    hostOverlay->ShowMessage("Off to RANKED",std::chrono::steady_clock::now());
                } else overlayNotice="RANKED can't start now";
            }
            if(ranked.stop && stopRanked) {
                stopRanked();
                mscharged::platform::SessionLogLine("Ranked: the way to RANKED stopped");
                rankedStage=CHARGED_SET_NONE;
            }
            const std::int64_t set=model.online.set.match;
            if(panelFeed) {
                if(online.challenge) {
                    const auto& challenge=*online.challenge;
                    panelFeed->Challenge(challenge.player,challenge.rule_set,challenge.rules,challenge.name);
                    overlayNotice="Challenge sent";
                }
                if(online.search) {
                    const auto& search=*online.search;
                    panelFeed->StartSearch(search.rule_set,search.rules,search.name,std::string(release->game_id,4));
                    overlayNotice="Searching for an opponent";
                }
                if(online.stop_search) {
                    panelFeed->StopSearch();
                    overlayNotice="The search stopped";
                }
                if(online.add_friend) panelFeed->AddFriend(*online.add_friend);
                if(online.remove_friend) panelFeed->RemoveFriend(*online.remove_friend);
                if(online.add_mii_friend) panelFeed->AddMiiFriend(*online.add_mii_friend);
                if(online.remove_mii_friend) panelFeed->RemoveMiiFriend(*online.remove_mii_friend);
                if(online.set_available) panelFeed->SetAvailable(*online.set_available);
                if(online.ready) panelFeed->ReadyToPlay(set,true);
                if(online.cancel) panelFeed->Cancel(set);
            }
            if(competitive) {
                // An accept starts the set in this game; a rematch asks the opponent.
                if(online.accept || online.decline) competitive->Answer(set,online.accept);
                if(online.rematch) competitive->PlayAgain(set,true);
            }
            if(online.set_challenge_rules) {
                challengeRules=*online.set_challenge_rules;
                if(panelFeed) panelFeed->SetChallengeRules(challengeRules);
                try {
                    mscharged::SaveChallengeRules(launch.config.path,challengeRules);
                } catch(const std::exception& error) {
                    overlayNotice=std::string("Your challenges' rules were not saved: ")+error.what();
                }
            }
            return started;
        };
        auto buildPrompt=[&](mscharged::overlay::OverlayPrompt& prompt) {
            namespace ov=mscharged::overlay;
            using Kind=CardPrompt::Kind;
            const std::string& who=cardPrompt.opponent;
            prompt.opponent.name=who;
            prompt.opponent.image=cardPrompt.avatar;
            prompt.image=overlayPictures.Images();
            prompt.opponent.initials=ov::Initials(who);
            prompt.opponent.tint_seed=std::int64_t(std::hash<std::string>{}(who)&0x7FFFFFFF);
            switch(cardPrompt.kind) {
            case Kind::Challenge:
            case Kind::Rematch: {
                const bool rematch=cardPrompt.kind==Kind::Rematch;
                prompt.kind=rematch ? ov::OverlayPrompt::Kind::Rematch : ov::OverlayPrompt::Kind::Challenge;
                prompt.kicker=rematch ? "REMATCH" : cardPrompt.random ? "RANDOM CHALLENGE" : "CHALLENGE";
                prompt.title=who+(rematch ? " wants a rematch" : " challenges you");
                prompt.detail=cardPrompt.rules;
                prompt.primary="Accept";
                prompt.secondary="Decline";
                prompt.note=cardPrompt.random
                    ? "They searched for an opponent by these rules. Accept, and your game goes straight to the set."
                    : "Accept, and your game goes straight to the set.";
                break;
            }
            case Kind::SetOver:
                prompt.kind=ov::OverlayPrompt::Kind::SetOver;
                prompt.kicker="SET OVER";
                prompt.title=cardPrompt.won>cardPrompt.lost ? "You won the set"
                    : cardPrompt.lost>cardPrompt.won ? "You lost the set" : "A draw";
                prompt.primary="Rematch";
                prompt.secondary="Not now";
                prompt.note="A rematch asks "+who+" to play the same set again.";
                break;
            case Kind::SetStopped:
                prompt.kind=ov::OverlayPrompt::Kind::SetStopped;
                prompt.kicker="SET STOPPED";
                prompt.title=cardPrompt.title.empty() ? std::string("The set stopped") : cardPrompt.title;
                prompt.detail=cardPrompt.reason;
                prompt.primary=cardPrompt.primary.empty() ? std::string("Play it again") : cardPrompt.primary;
                prompt.secondary="Not now";
                prompt.note=cardPrompt.note.empty()
                    ? "Play it again is your Ready to play!: the set goes once "+who+" is ready too."
                    : cardPrompt.note;
                break;
            }
            prompt.has_score=cardPrompt.hasScore;
            prompt.won=cardPrompt.won;
            prompt.lost=cardPrompt.lost;
            prompt.games=cardPrompt.games;
        };
        if(hostPanel) {
            std::string off;
            // The Discord friends come from the launcher (the game has no
            // Discord SDK); the feed asks which of them play.
            panelFeed=mscharged::platform::OnlinePanelFeed::Start(launch.settings.profile_server,
                mscharged::platform::ProfileSessionFile(mscharged::DataFolder(executableDirectory),launch.config.path),
                launch.settings.challenge_rules,launch.discord_friends,
                [](const std::string& text) { mscharged::platform::SessionLogLine(text); },&off);
            if(!panelFeed) mscharged::platform::SessionLogLine("Overlay: the Online tab is off ("+off+")");
            else if(!launch.discord_friends.empty())
                mscharged::platform::SessionLogLine("Overlay: "+std::to_string(launch.discord_friends.size())
                    +" Discord friends from the launcher");
            cardAsPrompt=true;
            {
                const std::string key=mscharged::platform::HotkeyName(hotkeys[mscharged::HotkeyPanel]);
                const bool none=key.empty() || key==mscharged::kNoHotkey;
                panelHint=mscharged::runtime::HostOverlay::Hint{
                    none ? std::vector<std::string>{"HOME"} : std::vector<std::string>{key,"HOME"},
                    "Recharged Game Menu"};
            }
            // F1/HOME and F2 while open: the other tab, or closing the one shown.
            hostPanel->SetTabKeys([&](int tab) {
                const auto want=tab==1 ? mscharged::overlay::OverlayTab::GameOptions
                                       : mscharged::overlay::OverlayTab::Online;
                // Over a prompt: the full overlay instead (its Online tab answers too).
                if(promptOpen) {
                    promptOpen=false;
                    promptPutAside=cardMatch;
                    overlayState.Open(want);
                    return false;
                }
                if(overlayState.Tab()==want)return true;
                overlayState.Switch(want);
                return false;
            });
            hostPanel->SetOverlay([&](float width,float height,float unit) {
                namespace ov=mscharged::overlay;
                const float shown=std::chrono::duration<float>(std::chrono::steady_clock::now()-overlayOpened).count();
                // A challenge, or the last set again: the prompt alone.
                if(promptOpen) {
                    ov::OverlayPrompt prompt;
                    buildPrompt(prompt);
                    ov::OverlayStyle style;
                    style.unit=unit;
                    style.fonts=hostOverlay->OverlayFonts();
                    style.area_max={width,height};
                    style.appear=std::clamp(shown/0.18f,0.0f,1.0f);
                    ov::OverlayPromptActions answer;
                    ov::DrawOverlayPrompt(prompt,promptState,answer,style);
                    if(answer.primary) cardPrompt.answer=1;
                    else if(answer.secondary) cardPrompt.answer=2;
                    return;
                }
                ov::GameOverlayModel model;
                buildOverlayModel(model,overlayState.Tab()==ov::OverlayTab::Ranked);
                ov::GameOverlayStyle style;
                style.unit=unit;
                style.fonts=hostOverlay->OverlayFonts();
                style.area_max={width,height};
                style.appear=std::clamp(shown/0.18f,0.0f,1.0f);
                style.image=overlayImage;
                if(const auto error=overlayPictures.Statistics().last_error; error!=overlayPicturesError) {
                    overlayPicturesError=error;
                    if(!error.empty()) mscharged::platform::SessionLogLine("Overlay: a picture failed ("+error+")");
                }
                ov::GameOverlayActions actions;
                ov::DrawGameOverlay(model,overlayState,actions,style);

                // What the player did.
                if(actions.close) hostPanel->RequestClose();
                if(applyOnline(actions.online,actions.ranked,model)) hostPanel->RequestClose();
                if(actions.set_option) applyOption(*actions.set_option);
                if(actions.set_setting) applySetting(*actions.set_setting);
            });
        }
        // Override retail menus (development switch): Recharged's menus in the
        // game's look (the UI kit) over its pause menu and Options, and on F1/HOME
        // (declared after what their callbacks use, which outlives them).
        std::optional<mscharged::runtime::HostMenus> hostMenus;
        // Recharged's menus with sample data (development, screenshots without
        // a server or a match): RECHARGED_CAPTURE_ONLINE players, a set and
        // leaderboards; RECHARGED_CAPTURE_PROMPT a challenge until answered;
        // RECHARGED_CAPTURE_MATCH_MENU the menus as over an online match (the 1
        // button opens the match menu anywhere; the game knows of none, so
        // nothing is forfeited).
        struct {
            bool online=std::getenv("RECHARGED_CAPTURE_ONLINE")!=nullptr;
            bool prompt=std::getenv("RECHARGED_CAPTURE_PROMPT")!=nullptr;
            bool match=std::getenv("RECHARGED_CAPTURE_MATCH_MENU")!=nullptr;
        } menuSamples;
        if(hostOverlay && hostPanel && menuSnapshot && menuCommand
           && (launch.settings.GameTweakMask() & mscharged::GameTweakBit(mscharged::GameTweak::OverrideRetailMenus))) {
            mscharged::runtime::HostMenus::Host menusHost;
            menusHost.model=[&](mscharged::ui::PageModel& model,const mscharged::runtime::HostMenus::ModelNeeds& needs) {
                buildOverlayModel(model.overlay,needs.leaderboard,needs.online,needs.settings);
                if(menuSamples.online) SampleOnline(model.overlay);
            };
            menusHost.image=overlayImage;
            menusHost.online=applyOnline;
            menusHost.setting=applySetting;
            menusHost.option=applyOption;
            // As closing the window: Aurora's exit, then the game's own shutdown.
            menusHost.quit=[] {
                SDL_Event quit{};
                quit.type=SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            };
            hostMenus.emplace(mscharged::runtime::HostMenus::Game{menuTexture,menuCommand},*hostPanel,
                std::move(menusHost));
            hostOverlay->SetMenus(&*hostMenus);
            mscharged::platform::SessionLogLine("Menus: Recharged's (Override retail menus)");
        }
#endif
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
                // Closing: the game's own shutdown may end the process.
                if(windowMemory) windowMemory->Flush();
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
            // The fullscreen hotkey (Alt+Return unless set otherwise): fullscreen or a window, for this run (issue #77).
            switch(mscharged::platform::ServiceFullscreenHotkey()) {
            case mscharged::platform::FullscreenSwitch::Fullscreen:
                mscharged::platform::SessionLogLine("Display: fullscreen (fullscreen hotkey)");
                mscharged::runtime::PlaceFullscreenWindow(host.window);
                mscharged::runtime::LockWindowShape(host.window,false,aspectWidth,aspectHeight); break;
            case mscharged::platform::FullscreenSwitch::Windowed:
                mscharged::platform::SessionLogLine("Display: windowed (fullscreen hotkey)");
                mscharged::runtime::LockWindowShape(host.window,!stretch && !widerOn,aspectWidth,aspectHeight); break;
            case mscharged::platform::FullscreenSwitch::Failed:
                mscharged::platform::SessionLogLine(std::string("Display: the fullscreen hotkey could not switch: ")
                    +SDL_GetError()); break;
            case mscharged::platform::FullscreenSwitch::None: break;
            }
            // The display hotkeys (issue #91), for this run.
            if(nativeSend) {
                const auto presses=mscharged::platform::TakeDisplayHotkeyPresses();
                const auto clock=std::chrono::steady_clock::now();
                if(presses.stretch_toggles%2) {
                    // With the wider view set up, F switches it; else stretching.
                    if(widerView) widerOn=!widerOn;
                    else stretch=!stretch;
                    const bool windowed=!(SDL_GetWindowFlags(host.window)&SDL_WINDOW_FULLSCREEN);
                    mscharged::runtime::LockWindowShape(host.window,windowed && !stretch && !widerOn,
                        aspectWidth,aspectHeight);
                    const std::string shape=widescreen?"16:9":"4:3";
                    const std::string text=widerOn?"Wider view: more of the stadium in matches"
                        :stretch?"Picture stretched to fill the window":"Picture keeps its "+shape+" shape";
                    mscharged::platform::SessionLogLine("Display: "+text+" (stretch hotkey)");
                    if(hostOverlay) hostOverlay->ShowMessage(text,clock);
                }
                // The picture fills the window stretched, or with the wider view
                // while the match's stadium is drawn (and a few frames after, so
                // one frame without it does not switch the picture).
                if(takeMatchView && takeMatchView()) matchViewFrame=frames+1;
                const bool wantFill=stretch || (widerOn && matchViewFrame && frames<matchViewFrame+3);
                if(wantFill!=fill) {
                    fill=wantFill;
                    aurora_set_video_output_stretch(fill);
                }
                if(presses.resolution_steps) {
                    // Steps by the heights each 3D resolution draws at on this window's picture.
                    const auto size=aurora_get_window_size();
                    const auto picture=mscharged::PictureSize(int(size.native_fb_width),int(size.native_fb_height),
                        int(aspectWidth),int(aspectHeight),fill);
                    const std::string next(mscharged::StepRenderResolution(renderResolution,presses.resolution_steps,
                        picture.height));
                    std::string text;
                    if(next==renderResolution)
                        text="3D resolution: "+std::string(mscharged::RenderResolutionLabel(renderResolution))
                            +(presses.resolution_steps>0?" (the highest)":" (the lowest)");
                    else {
                        renderResolution=next;
                        VISetFrameBufferScale(mscharged::RenderScale(renderResolution));
                        text="3D resolution: "+std::string(mscharged::RenderResolutionLabel(renderResolution))+" ("
                            +std::to_string(mscharged::RenderWidth(renderResolution,picture.width))+" \xC3\x97 "
                            +std::to_string(mscharged::RenderHeight(renderResolution,picture.height))+")";
                    }
                    mscharged::platform::SessionLogLine("Display: "+text+" (3D resolution hotkeys)");
                    if(hostOverlay) hostOverlay->ShowMessage(text,clock);
                }
                // Wider view: the window's picture shape against 16:9, while it fills the window.
                if(setViewShape) {
                    const auto size=aurora_get_window_size();
                    float shape=1.0f;
                    if(fill && size.native_fb_width && size.native_fb_height)
                        shape=float(size.native_fb_width)*9.0f/(float(size.native_fb_height)*16.0f);
                    if(std::abs(shape-1.0f)<0.002f) shape=1.0f;
                    if(shape!=viewShape) {
                        viewShape=shape;
                        setViewShape(shape);
                        mscharged::platform::SetPresentedPointerShape(shape);
                    }
                }
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
            serviceChallengeCard(now);
            if(hostPanel) hostPanel->Service(now);
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
            if(hostMenus) {
                ChargedMenuSnapshot menu{};
                const bool known=menuSnapshot(&menu,sizeof menu);
                if(known && menuSamples.match) menu.network_match=CHARGED_NETWORK_ONLINE;
                hostMenus->Service(known ? &menu : nullptr,now);
            }
            // With Recharged's menus the card is their prompt page, over
            // whatever shows; B or BACK is its second answer.
            if(cardAsPrompt && hostMenus) {
                const bool want=cardPrompt.shown && cardMatch!=promptPutAside;
                if(want) {
                    mscharged::overlay::OverlayPrompt prompt;
                    buildPrompt(prompt);
                    hostMenus->SetPrompt(std::move(prompt));
                } else if(menuSamples.prompt) {
                    mscharged::overlay::OverlayPrompt prompt;
                    prompt.kind=mscharged::overlay::OverlayPrompt::Kind::Challenge;
                    prompt.kicker="CHALLENGE";
                    prompt.title="Mario challenges you";
                    prompt.detail="Competitive: 3 minutes, best of 3, random stadium";
                    prompt.primary="Accept";
                    prompt.secondary="Decline";
                    prompt.note="Accept, and your game goes straight to the set.";
                    prompt.opponent.name="Mario";
                    prompt.opponent.initials="M";
                    prompt.opponent.tint_seed=3;
                    hostMenus->SetPrompt(std::move(prompt));
                } else hostMenus->SetPrompt(std::nullopt);
                if(const auto answer=hostMenus->TakePromptAnswer()) {
                    if(want && !cardPrompt.answer) cardPrompt.answer=*answer;
                    else if(!want) menuSamples.prompt=false;
                }
            } else
            // The card as a prompt: the panel opens for it while the player has
            // no overlay open; closed by Escape it is the prompt's second answer.
            if(cardAsPrompt && hostPanel) {
                const bool want=cardPrompt.shown && cardMatch!=promptPutAside;
                if(promptOpen && !hostPanel->IsOpen()) {
                    promptOpen=false;
                    if(want && !cardPrompt.answer) cardPrompt.answer=2;
                } else if(want && !promptOpen && !hostPanel->IsOpen()) {
                    promptOpen=true;
                    promptState.Open();
                    hostPanel->Open();
                } else if(!want && promptOpen) {
                    promptOpen=false;
                    hostPanel->Close();
                }
            }
            // The overlay opens on the Online tab; its data comes while it is open.
            if(hostPanel && hostPanel->IsOpen()!=overlayOpen) {
                overlayOpen=hostPanel->IsOpen();
                if(overlayOpen) {
                    overlayState.Open(hostPanel->OpenedTab()==1 ? mscharged::overlay::OverlayTab::GameOptions
                                                                 : mscharged::overlay::OverlayTab::Online);
                    overlayOpened=now;
                    overlayNotice.clear();
                    // The player's presets as they are now (the launcher may have changed them).
                    myPresets.clear();
                    for(const auto& preset:mscharged::launcher::LoadMatchPresets(
                            launch.config.path.parent_path()/"match-presets").presets)
                        if(!preset.built_in)
                            myPresets.push_back({preset.name,mscharged::launcher::ChallengeRules(preset),0});
                }
                if(panelFeed) panelFeed->SetOpen(overlayOpen);
            }
            if(overlayOpen && panelFeed) {
                ChargedPresenceSnapshot game{};
                const char* kind=presenceSnapshot && presenceSnapshot(&game,sizeof game)
                    ? mscharged::platform::MatchKindOf(game) : nullptr;
                panelFeed->SetActivity(inMatch() ? "match" : "game",kind ? kind : "");
            }
            // The panel's keys in the corner while the game is in its menus
            // (the title's demo matches count as menus), a few times a second.
            if(hostOverlay && panelHint && now-panelHintChecked>=std::chrono::milliseconds(250)) {
                panelHintChecked=now;
                ChargedPresenceSnapshot game{};
                const bool menus=presenceSnapshot && presenceSnapshot(&game,sizeof game) && game.scene_count>0
                    && !(game.in_match && game.mode!=2);
                hostOverlay->SetHint(menus ? panelHint : std::nullopt);
            }
            // Changed game options reach the game between matches.
            if(pendingTweaks && setTweaks && !inMatch()) {
                setTweaks(*pendingTweaks);
                pendingTweaks.reset();
                mscharged::platform::SessionLogLine("Game options: changed in the overlay");
            }
#endif
            if(hostOverlay) {
                if(const auto rate=frameRate.Sample(frames,now)) hostOverlay->SetRate(*rate);
                const auto size=aurora_get_window_size();
                hostOverlay->Update(size.native_fb_width,size.native_fb_height,now);
            }
            if(windowMemory) windowMemory->Service(now);
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
                frame(delta); observeOptions(); observeBoot(); observePresence(); observeCompetitive(); observeRanked(); observeMiiSignIn(); observeNetplay(); observeOnlineMatch(); AuroraGXEndDrawReceipt(); ++frames;
                mscharged::platform::FrameFinished(); // the freeze watch (#121)
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
            frame(delta); observeOptions(); observeBoot(); observePresence(); observeCompetitive(); observeRanked(); observeMiiSignIn(); observeNetplay(); observeOnlineMatch(); AuroraGXEndDrawReceipt();
            mscharged::platform::FrameFinished(); // the freeze watch (#121)
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
        mscharged::platform::StopFreezeWatch(); // no frames follow
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

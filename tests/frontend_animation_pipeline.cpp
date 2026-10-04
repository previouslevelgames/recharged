#include "frontend_font_fixture.h"
#include "frontend_layout_fixture.h"
#include "frontend_image_fixture.h"
#include "resources/frontend_images.h"
#include "resources/frontend_animation.h"
#include "runtime/frontend_image_gx.h"
#include <optional>
#include <cmath>
#include "runtime/frontend_layout_gx.h"
#include "runtime/gpu_readback.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>

using namespace mscharged;
namespace
{
std::atomic_uint errors = 0;
void Log(AuroraLogLevel level, const char*, const char* message, unsigned size)
{ if (level >= LOG_ERROR) ++errors; std::cerr.write(message, size); std::cerr << '\n'; }
void Check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
struct Session { bool live = false; ~Session() { if (live) { AuroraGXSync(); aurora_shutdown(); } } };
using RGB = std::array<unsigned,3>;
void Case(const resources::FrontendLayoutFrame& layout, const std::array<std::optional<RGB>,9>& expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    unsigned draws = 0, quiet = 0;
    ColourSamples samples{};
    for (unsigned frame = 0;;)
    {
        Check(std::chrono::steady_clock::now() < deadline, "Frontend frame pipeline timed out");
        for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
            Check(event->type != AURORA_EXIT, "Frontend test window closed");
        if (!aurora_begin_frame()) { SDL_Delay(1); continue; }
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
        GXSetCopyClear({20,24,30,255}, GX_MAX_Z24);
        DrawFrontendLayout(layout, 640, 480);
        GXDrawDone();
        const bool sample = frame >= 12 && quiet >= 2;
        if (sample) samples = EndFrameAndReadColours(); else aurora_end_frame();
        draws += aurora_get_stats()->drawCallCount; ++frame;
        const auto pending = std::atomic_ref<const std::uint32_t>(aurora_get_stats()->queuedPipelines).load();
        quiet = pending ? 0 : quiet + 1;
        if (sample) break;
        SDL_Delay(1);
    }
    Check(layout.entries.empty() || draws > 0, "Frontend frame emitted no GPU draws");
    for (unsigned pixel = 0; pixel < samples.size(); ++pixel)
    {
        std::cout << "Pixel " << pixel << ": " << unsigned(samples[pixel][0]) << ','
            << unsigned(samples[pixel][1]) << ',' << unsigned(samples[pixel][2]) << '\n';
        for (unsigned channel = 0; expected[pixel] && channel < 3; ++channel)
            Check(std::abs(int(samples[pixel][channel]) - int((*expected[pixel])[channel])) <= 3,
                "Authored frontend placement, visibility, order or colour mismatch");
    }
}
}
int main(int argc, char** argv)
{
    try
    {
        const auto directory = std::filesystem::path(SDL_GetBasePath()) / "frontend-animation-test-data";
        std::filesystem::create_directories(directory); const auto path = directory.string();
        AuroraConfig config{}; config.appName = "Charged authored frontend animation checks";
        config.userPath = config.cachePath = path.c_str(); config.resourcesPath = SDL_GetBasePath();
        config.desiredBackend = BACKEND_VULKAN; config.enableBackendValidation = true;
        config.windowWidth = 640; config.windowHeight = 480; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.logCallback = Log; config.vsync = true;
        Session session; const auto info = aurora_initialize(argc, argv, &config); session.live = true;
        Check(info.window && info.backend == BACKEND_VULKAN, "Frontend frame pipeline requires Vulkan");
        ImGui::GetIO().IniFilename = nullptr; ImGui::GetIO().LogFilename = nullptr;
        VIInit(); VIConfigure(&GXNtsc480IntDf);
        alignas(32) std::array<std::uint8_t,65536> fifo{}; GXInit(fifo.data(), fifo.size());
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        const auto font=resources::ReadFrontendFont(font_fixture::Font(),"fe/fonts/fixture","test");
        const std::array fonts{font};resources::Localization localization;
        auto graph=frontend_layout_fixture::Scene(font->alias);
        graph.instances[0].hash=resources::FrontendLowerHash("parent");
        graph.instances[0].children={300};
        graph.instances[1].text=u"A";graph.instances[1].attributes.position={155,3,0};
        graph.instances[1].attributes.colour={255,0,0,255};
        frontend_image_fixture::AddImage(graph,99);
        resources::FrontendImageCatalog catalog;
        const frontend_image_fixture::Colour blue{0,0,255,255};
        catalog.textures[99]=frontend_image_fixture::Texture(99,1,{blue,blue,blue,blue});
        auto& slide=graph.slides[0];slide.hash=0xc7b14a27;slide.duration=1;slide.play_mode=1;slide.animated=true;slide.animations={2000};
        resources::FrontendAnimation position;position.offset=2000;position.target=200;position.type=1;position.cast=1;
        const std::array<float,3> from{-160,120,0},to{160,-120,0};
        for(unsigned i=0;i<2;++i)
        {
            resources::FrontendAnimationKey key;key.offset=2001+i;
            for(unsigned c=0;c<3;++c)key.channels[c]=i?std::array<float,4>{to[c],-1,-1,1}
                :std::array<float,4>{from[c],from[c]+(to[c]-from[c])/3,from[c]+2*(to[c]-from[c])/3,0};
            position.keys.push_back(key);
        }
        graph.animations.push_back(position);
        resources::FrontendAnimationPlayback playback(graph);
        const auto frame=[&]{return resources::BuildFrontendLayout(playback.Scene(),localization,fonts,{},catalog);};
        constexpr RGB background{20,24,30};using Expected=std::array<std::optional<RGB>,9>;
        const auto flat=[&]{Expected e;e.fill(background);return e;};
        auto initial=flat();initial[0]=RGB{0,0,255};initial[1]=RGB{255,0,0};
        auto middle=flat();middle[4]=RGB{0,0,255};middle[5]=RGB{255,0,0};
        auto end=flat();end[8]=RGB{0,0,255};
        Case(frame(),initial);playback.Advance(.25f);Case(frame(),middle);
        playback.Reset();playback.Advance(.5f);Case(frame(),end); // Exact endpoint does not wrap.
        playback.Advance(.5f);Case(frame(),middle); // Original double-step plus single loop subtraction.
        playback.Reset();Case(frame(),initial);
        auto retained=frame();playback.Advance(.25f);Case(retained,initial); // Old frame owns its positions and texture.
        // Source-defined selection resets the presentation clock, but does
        // not sample until Update. Same-name lookup is case-insensitive.
        Check(playback.SelectPresentation("STATIC"), "Case-insensitive presentation selection failed");
        Case(frame(),middle);
        Check(playback.SelectPresentation("Static",true), "Presentation reset selection failed");
        Case(frame(),middle);playback.Advance(0);Case(frame(),initial);
        Check(!playback.SelectPresentation("absent"), "Missing presentation did not clear active selection");
        Case(frame(),flat());
        Check(playback.SelectPresentation("Static"), "Presentation could not be reselected");
        playback.Advance(0);Case(frame(),initial);
        // Component selection samples immediately; changing away and back can
        // preserve the prior component slide clock independently of presentation.
        auto nested=graph;auto moving=nested.slides[0];moving.offset=101;moving.name="Moving";moving.hash=0xb96331af;
        resources::FrontendSlide root{};root.offset=100;root.name="Root";root.duration=10;root.children={3001};
        resources::FrontendSlide blank{};blank.offset=102;blank.name="Blank";blank.hash=0x04d51ce7;blank.duration=10;
        nested.slides={root,moving,blank};
        resources::FrontendLibraryObject component{};component.offset=3000;component.type=3;
        component.attributes=frontend_layout_fixture::Attributes();component.active_slide=101;component.slides={101,102};
        nested.library.push_back(component);
        resources::FrontendInstance instance{};instance.offset=3001;instance.type=4;instance.library=3000;
        instance.attributes=frontend_layout_fixture::Attributes();instance.duration=10;instance.visible=true;
        nested.instances.push_back(instance);
        resources::FrontendAnimationPlayback component_playback(nested);
        const auto component_frame=[&]{return resources::BuildFrontendLayout(component_playback.Scene(),localization,fonts,{},catalog);};
        component_playback.Advance(.5f);Case(component_frame(),middle);
        Check(component_playback.SelectComponent(3000,"Blank"), "Blank component selection failed");Case(component_frame(),flat());
        Check(component_playback.SelectComponent(3000,"Moving",false,true), "Preserved component selection failed");Case(component_frame(),middle);
        Check(component_playback.SelectComponent(3000,"MOVING",true,false), "Reset component selection failed");Case(component_frame(),initial);
        resources::FrontendAnimation opacity;opacity.offset=2010;opacity.target=200;opacity.type=6;opacity.cast=0;
        resources::FrontendAnimationKey first,last;first.offset=2011;last.offset=2012;
        first.channels[0]={255,170,85,0};last.channels[0]={0,-1,-1,1};opacity.keys={first,last};
        graph.animations.push_back(opacity);graph.slides[0].animations.push_back(2010);
        resources::FrontendAnimationPlayback fade(graph);fade.Advance(.25f);
        // Original float-to-u8 truncates half opacity127.5 to127. Independent
        // source-alpha framebuffer arithmetic, allowing fixed-point GX rounding.
        auto half=flat();half[4]=RGB{10,12,142};half[5]=RGB{137,12,15};
        Case(resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog),half);
        fade.Reset();fade.Advance(.5f);Case(resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog),flat());
        fade.Reset();Case(resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog),initial);
        bool rejected=false;try{fade.Advance(-1);}catch(const std::exception&){rejected=true;}
        Check(rejected,"Invalid frontend timeline delta was accepted");
        Case(resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog),initial);
        // Original named lookup and instance setters must affect actual pixels
        // while previously published frames retain their own transform/visibility.
        const std::array<std::string_view,1> instance_path{"PARENT"};
        const auto parent=resources::FindFrontendNode(fade.Scene(),{},resources::FrontendNamedPath(instance_path),
            resources::FrontendNodeType::Layer);
        Check(parent && parent->id==200,"Named parent lookup failed");
        auto old_frame=resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog);
        resources::FrontendInstanceChange change;change.instance=parent->id;
        change.property=resources::FrontendInstanceProperty::Visible;change.flag=false;
        fade.Apply(std::span(&change,1));
        Case(resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog),flat());
        Case(old_frame,initial);
        change.flag=true;fade.Apply(std::span(&change,1));
        change.property=resources::FrontendInstanceProperty::Position;change.vector={0,0,0};
        fade.Apply(std::span(&change,1));
        Case(resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog),middle);
        change.property=resources::FrontendInstanceProperty::Colour;change.colour={255,255,255,128};
        fade.Apply(std::span(&change,1));
        auto mutated_half=flat();mutated_half[4]=RGB{10,12,143};mutated_half[5]=RGB{138,12,15};
        Case(resources::BuildFrontendLayout(fade.Scene(),localization,fonts,{},catalog),mutated_half);
        AuroraGXSync(); Check(errors == 0, "Aurora reported frontend rendering errors");
        std::cout << "Frontend animated hierarchy, original clock, named instance mutation and retained-frame pixel checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}

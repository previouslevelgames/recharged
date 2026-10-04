#include "frontend_font_fixture.h"
#include "frontend_layout_fixture.h"
#include "frontend_image_fixture.h"
#include "resources/frontend_images.h"
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
        const auto directory = std::filesystem::path(SDL_GetBasePath()) / "frontend-image-test-data";
        std::filesystem::create_directories(directory); const auto path = directory.string();
        AuroraConfig config{}; config.appName = "Charged authored frontend image checks";
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
        const auto font = resources::ReadFrontendFont(font_fixture::Font(), "fe/fonts/fixture", "test");
        const std::array fonts{font};resources::Localization localization;
        constexpr RGB background{20,24,30};
        using Expected=std::array<std::optional<RGB>,9>;
        const auto flat=[&](RGB colour){Expected e;e.fill(colour);return e;};
        constexpr unsigned hash=0x12345678;
        auto graph=frontend_layout_fixture::Scene(font->alias);
        graph.instances[0].children.clear();frontend_image_fixture::AddImage(graph,hash);
        auto& image=graph.instances.back();image.overload_flags|=4;image.attributes.scale={6.4f,4.8f,1};
        resources::FrontendImageCatalog catalog;
        const auto build=[&]{return resources::BuildFrontendLayout(graph,localization,fonts,{},catalog);};
        // Independent decoded-quadrant oracle covers both owned formats plus
        // RGBA8 and indexed palette handling. Do not sample quadrant seams.
        for(unsigned format:{1u,2u,3u,8u})
        {
            catalog.textures[hash]=frontend_image_fixture::Texture(hash,format);
            auto frame=build();Check(frame.ImageCount()==1&&frame.TextCount()==0,"Image fixture selection failed");
            Expected quadrants{};quadrants[0]=RGB{255,0,0};quadrants[2]=RGB{0,255,0};quadrants[6]=RGB{0,0,255};quadrants[8]=RGB{255,255,0};
            Case(frame,quadrants);
            // Crop original bottom-origin UV rectangle to the top-right
            // decoded quadrant, then repeat outside the unit UV domain.
            image.overload_flags|=0x3c0;image.attributes.uv={.5f,.5f,.5f,.5f};Case(build(),flat({0,255,0}));
            image.attributes.uv={1,0,1,1};Case(build(),quadrants);image.overload_flags&=~0x3c0u;
        }
        // Both actual owned formats have mip chains. A four-pixel quad must
        // select the cyan lower levels, not the red base level; every lower
        // level is uniform so derivative rounding cannot change the oracle.
        for(unsigned format:{1u,2u})
        {
            const frontend_image_fixture::Colour red{255,0,0,255};
            auto mip=frontend_image_fixture::Texture(hash,format,{red,red,red,red});mip->levels=5;
            for(unsigned level=1;level<5;++level)
            {
                const unsigned size=16>>level,block=format==1?4:8;
                const unsigned bytes=((size+block-1)/block)*((size+block-1)/block)*32;
                const auto offset=mip->pixels.size();mip->pixels.resize(offset+bytes);
                if(format==1)for(unsigned i=0;i<bytes;i+=2){mip->pixels[offset+i]=0x83;mip->pixels[offset+i+1]=0xff;}
                else for(unsigned i=0;i<bytes;i+=8)
                {mip->pixels[offset+i]=mip->pixels[offset+i+2]=0x07;mip->pixels[offset+i+1]=mip->pixels[offset+i+3]=0xff;}
            }
            catalog.textures[hash]=mip;image.attributes.scale={.04f,.04f,1};
            Expected expected=flat(background);expected[4]=RGB{0,255,255};Case(build(),expected);
        }
        image.attributes.scale={6.4f,4.8f,1};
        // Verify all eight original blend modes against independently evaluated
        // framebuffer equations. Factor2 is destination colour for a source.
        const frontend_image_fixture::Colour source{8,40,120,128};
        catalog.textures[hash]=frontend_image_fixture::Texture(hash,3,{source,source,source,source});
        for(unsigned mode=0;mode<8;++mode)
        {
            image.image_blend=mode;RGB expected{};
            for(unsigned c=0;c<3;++c)
            {
                const double s=source[c],d=background[c],a=source[3]/255.;double result=0;
                switch(mode){case 0:case 6:result=s;break;case 1:result=s*a+d*(1-a);break;
                case 2:result=s+d;break;case 3:result=s*a+d;break;case 4:result=s*d/255.;break;
                case 5:result=s*(1-d/255.)+d;break;case 7:result=d-s;break;}
                expected[c]=unsigned(std::round(std::clamp(result,0.,255.)));
            }
            std::cout<<"Blend "<<mode<<'\n';Case(build(),flat(expected));
        }
        const frontend_image_fixture::Colour transparent{255,0,255,0};
        catalog.textures[hash]=frontend_image_fixture::Texture(hash,3,{transparent,transparent,transparent,transparent});
        image.image_blend=0;Case(build(),flat(background)); // alpha compare remains active without blending
        // Mixed ordering must distinguish text-before-image and image-before-
        // text in the same frame; no grouping by type can pass all three spots.
        graph=frontend_layout_fixture::Scene(font->alias);
        for(unsigned i=0;i<3;++i)
        {
            auto& text=graph.instances[i+1];text.text=u"A";
            text.attributes.position={float(-165+160*int(i)),float(123-120*int(i)),0};
            text.attributes.colour=i==0?frontend_image_fixture::Colour{255,0,0,255}:i==1?frontend_image_fixture::Colour{0,255,0,255}:frontend_image_fixture::Colour{255,255,0,255};
            frontend_image_fixture::AddImage(graph,hash,600+100*i);
            graph.instances.back().attributes.position={float(-160+160*int(i)),float(120-120*int(i)),0};
        }
        graph.instances[0].children={300,600,700,400,500,800};
        const frontend_image_fixture::Colour blue{0,0,255,255};catalog.textures[hash]=frontend_image_fixture::Texture(hash,1,{blue,blue,blue,blue});
        auto mixed=build();Check(mixed.TextCount()==3&&mixed.ImageCount()==3,"Mixed fixture was not retained");
        catalog.textures.clear();graph={};
        Expected mixed_pixels=flat(background);mixed_pixels[0]=RGB{255,0,0};mixed_pixels[4]=RGB{0,0,255};mixed_pixels[8]=RGB{255,255,0};
        Case(mixed,mixed_pixels);
        // A rejected forged owner/UV must not poison a later real draw. The
        // invalid command is checked before GX state or draw issuance.
        auto invalid=std::get<resources::FrontendLayoutImage>(mixed.entries[0]);invalid.texture.reset();
        bool rejected=false;try{DrawFrontendImage(invalid,640,480);}catch(const std::exception&){rejected=true;}
        Check(rejected,"Malformed image owner was accepted");
        invalid=std::get<resources::FrontendLayoutImage>(mixed.entries[0]);invalid.vertices[0].u=32;
        rejected=false;try{DrawFrontendImage(invalid,640,480);}catch(const std::exception&){rejected=true;}
        Check(rejected,"Out-of-range signed16 UV was accepted");Case(mixed,mixed_pixels);
        AuroraGXSync(); Check(errors == 0, "Aurora reported frontend rendering errors");
        std::cout << "Frontend image formats, UVs, blends, mixed order and retained-owner checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}

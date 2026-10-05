// Caller-owned game data. Original Options→Credits→Options with actual Vulkan,
// NL movie reads, raw host retraces and flushed SDL audio. Pointer entry uses an
// explicit source fixture; this does not qualify physical desktop focus/input.
#include "runtime/scene.h"
#include "runtime/frontend_menu_scenes.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace mscharged;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Driver
{
    FrontendPointerViewport viewport;
    const void* images=nullptr;const void* visuals=nullptr;
    FrontendSceneStack::Token last_token=0;
    std::map<unsigned,unsigned> visits;
    std::set<unsigned> phases;
    std::weak_ptr<FrontendMoviePlayback> nlg,credits;
    std::uint64_t sequence=0;
    unsigned neutral=0;
    bool entered=false,completed=false,nlg_final=false,credits_frames=false;
    unsigned parser_tokens=0,max_lines=0;
    static constexpr auto Deadline=std::chrono::seconds(240);
    const std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    void Input(FrontendMenuScenes& menus,SDL_Window* window)
    {
        if(entered||menus.Status().scene!=13)return;
        Check(SDL_GetWindowID(window)==viewport.window,"Credits input targets another presented window");
        const auto bounds=menus.Bounds().at(2);
        const float x=(bounds.min_x+bounds.max_x)/2,y=(bounds.min_y+bounds.max_y)/2;
        FrontendPointerDesktopSample sample;
        sample.window=viewport.window;sample.window_width=viewport.window_width;sample.window_height=viewport.window_height;
        sample.pixel_width=viewport.pixel_width;sample.pixel_height=viewport.pixel_height;
        sample.sequence=++sequence;sample.device=1;sample.connected=sample.focused=true;
        sample.x=float((viewport.x+viewport.width*(.5+x/viewport.screen_width))*viewport.window_width/viewport.pixel_width);
        sample.y=float((viewport.y+viewport.height*(.5-y/viewport.screen_height))*viewport.window_height/viewport.pixel_height);
        sample.primary_down=neutral>=2;
        const auto event=menus.Route(sample);
        if(sample.primary_down){Check(event.active&&event.event.pressed,"Original Credits button did not admit one press");entered=true;}
        else ++neutral;
    }
    void Presented(const FrontendMenuScenes& menus,const FrontendPointerViewport& v)
    {
        Check(std::chrono::steady_clock::now()-start<Deadline,"Original Credits lifecycle exceeded its deadline");
        viewport=v;const auto frame=menus.Current();const auto state=menus.Status();
        Check(frame.navigation&&!state.full_scene_created,"Credits flow fabricated complete original scene readiness");
        if(!images){images=frame.navigation->images.get();visuals=frame.navigation->visuals.get();}
        Check(images==frame.navigation->images.get()&&visuals==frame.navigation->visuals.get(),"Credits replaced retained NAV resources");
        if(frame.menu)
        {
            Check(images==frame.menu->images.get()&&visuals==frame.menu->visuals.get(),"Credits menu/NAV owners differ");
            if(frame.token!=last_token){++visits[frame.scene];last_token=frame.token;}
        }
        if(state.credits)
        {
            const auto& source=*state.credits;phases.insert(source.phase);
            Check(!source.full_scene_created,"Credits declared full SceneCreated");
            Check(!state.pointer_enabled,"Credits did not hide the actual navigation pointers");
            if(const auto owner=menus.Credits())
            {
                if(const auto target=owner->MovieTarget())
                {
                    if(source.phase==1)nlg=target->playback;
                    if(source.phase==2)
                    {
                        credits=target->playback;
                        const auto output=target->playback->Status();
                        credits_frames|=output.published_frames>0;
                    }
                }
            }
            if(source.phase==2)
            {
                if(!nlg_final)
                {
                    const auto movie=nlg.lock();Check(bool(movie),"Original NLG owner disappeared before retirement observation");
                    const auto output=movie->Status();
                    nlg_final=output.decoded_eof&&output.final_presented
                        &&output.published_frames==movie->Info().frame_count
                        &&output.queued_input_bytes==0&&output.available_output_bytes==0;
                }
                parser_tokens=source.parser_tokens;
                Check(source.lines_on_screen<=20,"Credits exceeded its20 original text slots");
                max_lines=std::max(max_lines,source.lines_on_screen);
            }
        }
        if(entered&&visits[23]&&state.scene==13&&state.interactive)
        {
            Check(nlg_final&&credits_frames,"Credits returned without actual NLG final output and Credits movie frames");
            Check(phases.contains(0)&&phases.contains(1)&&phases.contains(2)&&phases.contains(3),"Original Credits source phases were not presented");
            Check(parser_tokens>0&&max_lines>0&&state.pointer_enabled,"Credits scrolling parser or actual pointer restoration failed");
            completed=true;SDL_Event event{};event.type=SDL_EVENT_QUIT;Check(SDL_PushEvent(&event),"Cannot request normal Credits shutdown");
        }
    }
};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"Supply caller-owned disc configuration");Driver driver;
        ScenePreviewHooks hooks{
            [&](auto& menus,auto* window){driver.Input(menus,window);},
            [&](const auto& menus,const auto& viewport){driver.Presented(menus,viewport);}
        };
        SceneOptions options;options.frontend_options=true;
        const auto result=RunScenePreview(argc,argv,argv[1],options,&hooks);if(result)return result;
        Check(driver.completed&&driver.visits[13]==2&&driver.visits[23]==1,"Original Credits did not return to fresh Options");
        std::cout<<"Vulkan Options/Credits/Options passed: actual movies, raw retraces, original20 lines and normal teardown\n";
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}
}

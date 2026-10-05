// Caller-owned game data only. Run the Python wrapper to isolate native settings.
// Drives deterministic desktop samples through production host admission and
// the actual Vulkan presentation barrier; does not claim physical mouse input.
#include "runtime/scene.h"
#include "runtime/frontend_menu_scenes.h"
#include "resources/native_preferences.h"
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

using namespace mscharged;
namespace
{
void Check(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
struct Step { unsigned scene; int button; };
// -1 is actual NAV Done; -2 is actual NAV Back.
constexpr std::array steps{
    Step{1,6}, Step{13,1}, Step{14,0}, Step{14,-1},
    Step{13,0}, Step{15,3}, Step{15,6}, Step{15,-1}, Step{13,-2}
};
struct Driver
{
    FrontendPointerViewport viewport;
    const void* images = nullptr;
    const void* visuals = nullptr;
    FrontendSceneStack::Token last_token = 0;
    std::map<unsigned,unsigned> presented;
    std::map<unsigned,unsigned> visits;
    std::uint64_t sequence = 0;
    unsigned stage = 0, neutral = 0;
    bool completed = false;
    bool title = false, title_pressed = false, title_transition = false;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    void Presented(const FrontendMenuScenes& menus, const FrontendPointerViewport& v)
    {
        Check(std::chrono::steady_clock::now()-start < std::chrono::seconds(150),
            "Composed menu flow exceeded its deadline");
        viewport = v;
        const auto frame = menus.Current();
        Check(frame.navigation && !menus.Status().full_scene_created,
            "Composed menu fabricated full source scene readiness");
        if (!images) { images=frame.navigation->images.get(); visuals=frame.navigation->visuals.get(); }
        Check(images==frame.navigation->images.get() && visuals==frame.navigation->visuals.get(),
            "NAV replaced permanent Main resources");
        if (frame.menu)
        {
            if(title&&frame.scene==0&&menus.Status().title&&menus.Status().title->initialized)
                Check(frame.menu->layout.ImageCount()>0&&!frame.menu->layout.unavailable.contains("nonplanar instance branch"),
                    "Original Title flat image/component Z scaling did not render");
            Check(images==frame.menu->images.get() && visuals==frame.menu->visuals.get(),
                "Presented menu and NAV differ in resource ownership");
            ++presented[frame.scene];
            if(frame.token!=last_token)
            {
                ++visits[frame.scene];
                Check(!last_token || frame.menu->image_completed_files==0,
                    "Scene replacement reread shared image bundles");
                last_token=frame.token;
            }
        }
        if(title && menus.Status().title_transition
            && menus.Status().title_transition->state==FrontendTransitionState::Running)
        {
            title_transition=true;
            Check(!frame.menu && !menus.Status().interactive,
                "Title camera transition published a fabricated menu");
        }
        if(stage==steps.size() && menus.Status().scene==1 && menus.Status().interactive && !completed)
        {
            completed=true;
            SDL_Event event{}; event.type=SDL_EVENT_QUIT;
            Check(SDL_PushEvent(&event), "Cannot request normal preview shutdown");
        }
    }
    void Input(FrontendMenuScenes& menus, SDL_Window* window)
    {
        if(stage==steps.size())return;
        const auto step=title&&!title_pressed?Step{0,0}:steps[stage];
        if(menus.Status().scene!=step.scene)return;
        Check(SDL_GetWindowID(window)==viewport.window, "Input targets another presented window");
        const auto bounds=step.button==-1 ? menus.DoneBounds()
            :step.button==-2 ? menus.BackBounds() : menus.Bounds().at(step.button);
        const float x=(bounds.min_x+bounds.max_x)/2;
        const float y=(bounds.min_y+bounds.max_y)/2;
        FrontendPointerDesktopSample sample;
        sample.window=viewport.window;sample.window_width=viewport.window_width;
        sample.window_height=viewport.window_height;sample.pixel_width=viewport.pixel_width;
        sample.pixel_height=viewport.pixel_height;sample.sequence=++sequence;
        sample.device=1;sample.connected=sample.focused=true;
        sample.x=float((viewport.x+viewport.width*(.5+x/viewport.screen_width))
            *viewport.window_width/viewport.pixel_width);
        sample.y=float((viewport.y+viewport.height*(.5-y/viewport.screen_height))
            *viewport.window_height/viewport.pixel_height);
        sample.primary_down=neutral>=2;
        const auto result=menus.Route(sample);
        if(sample.primary_down)
        {
            Check(result.active && result.event.pressed, "Rendered desktop sample did not admit one press");
            if(title&&!title_pressed)title_pressed=true;else ++stage;
            neutral=0;
        }
        else ++neutral;
    }
};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==2||(argc==3&&std::string_view(argv[2])=="--title"),
            "Supply a disc INI through test_frontend_menu_scene.py, optionally --title");
        const auto* base=SDL_GetBasePath();Check(base,"Cannot locate the isolated executable");
        const auto settings=std::filesystem::path(base)/"scene-data/native-preferences.bin";
        Check(!std::filesystem::exists(settings),"Rendered menu test requires an isolated executable directory");
        const auto defaults=resources::DefaultNativePreferences();
        Check(defaults.audio[0]>0,"Test requires source default Music volume above zero");
        Driver driver;
        driver.title=argc==3;
        ScenePreviewHooks hooks{
            [&](auto& menu,auto* window){driver.Input(menu,window);},
            [&](const auto& menu,const auto& viewport){driver.Presented(menu,viewport);}
        };
        SceneOptions options;options.frontend_title=driver.title;options.frontend_main=!driver.title;
        const int result=RunScenePreview(argc,argv,argv[1],options,&hooks);
        if(result)return result;
        Check(driver.completed && driver.stage==steps.size(),"Rendered source menu flow did not complete");
        for(unsigned scene:{1u,13u,14u,15u})
            Check(driver.presented[scene]>0,"A selected menu was never actually presented");
        Check(driver.visits[1]==2 && driver.visits[13]==3 && driver.visits[14]==1 && driver.visits[15]==1,
            "Rendered flow did not create fresh source handlers");
        if(driver.title)Check(driver.title_pressed&&driver.title_transition&&driver.visits[0]==1&&driver.presented[0]>0,
            "Original Title input/camera/scene lifecycle was not presented");
        resources::NativePreferencesBytes bytes{};
        std::ifstream saved(settings,std::ios::binary);saved.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        Check(bool(saved),"Real menu preference saves did not reach their file");
        const auto values=resources::DecodeNativePreferences(bytes);
        auto expected=defaults;--expected.audio[0];expected.auto_zoom=false;expected.camera_zoom=.75f;
        Check(values==expected,"Rendered Audio/Visual saves lost original or unrelated preferences");
        std::cout<<(driver.title?"Vulkan Title/Main/Options/Audio/Visual flow passed:10 original pointer actions, "
            :"Vulkan Main/Options/Audio/Visual flow passed:9 original pointer actions, ")
            <<"fresh source handlers, retained NAV, actual native saves and normal shutdown\n";
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}
}

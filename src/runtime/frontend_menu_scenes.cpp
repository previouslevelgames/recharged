#include "runtime/frontend_menu_scenes.h"
#include "runtime/frontend_menu_back.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool value, const char* message) { if (!value) throw std::logic_error(message); }
struct Guard { bool& value; explicit Guard(bool& v):value(v){value=true;} ~Guard(){value=false;} };
}
struct FrontendMenuScenes::Implementation
{
    FrontendInput& input;
    std::shared_ptr<FrontendAudio> audio;
    unsigned& seed;
    FrontendCameras& cameras;
    std::shared_ptr<NativePreferences> preferences;
    std::function<void(unsigned)> music;
    std::function<bool(unsigned)> effect;
    FrontendSceneStack stack;
    FrontendMenuTransition transition;
    FrontendLanguage language;
    AudioCategoryVolumes::Handle volumes;
    FrontendVisualSettings::Handle visual_settings;
    const std::thread::id thread = std::this_thread::get_id();
    FrontendSceneStack::Token token = 0, transition_destination = 0;
    unsigned scene = 0;
    std::shared_ptr<FrontendMainMenu> main;
    std::shared_ptr<FrontendOptions> options;
    std::shared_ptr<FrontendAudioOptions> audio_options;
    std::shared_ptr<FrontendVisualOptions> visual_options;
    std::shared_ptr<FrontendSession> nav_session;
    std::shared_ptr<FrontendNavigation> nav;
    std::unique_ptr<FrontendOptionsNavigation> options_nav;
    std::unique_ptr<FrontendAudioNavigation> audio_nav;
    std::unique_ptr<FrontendVisualNavigation> visual_nav;
    FrontendSession::Handle menu_shown, nav_shown, input_menu;
    std::optional<unsigned> pending_scene;
    bool desktop_back_checked=false;
    bool busy = false, input_window = false, input_allowed = false, failed = false, released = false;

    Implementation(FrontendInput& i, std::shared_ptr<FrontendAudio> a, unsigned& rng,
        FrontendCameras& c, resources::Bytes script, std::shared_ptr<NativePreferences> p,
        std::function<void(unsigned)> m, std::function<bool(unsigned)> e, std::function<void()> drain,
        unsigned initial, FrontendLanguage l, AudioCategoryVolumes::Handle v, FrontendVisualSettings::Handle vs)
        :input(i),audio(std::move(a)),seed(rng),cameras(c),preferences(std::move(p)),
         music(std::move(m)),effect(std::move(e)),stack(i,std::move(drain)),transition(script,c,stack),language(l),volumes(std::move(v)),visual_settings(std::move(vs))
    {
        Require(audio && audio->Loaded() && preferences && music, "Menu scenes require actual audio, music and native preferences");
        const auto status = preferences->Status();
        Require(!status.host_pending && preferences->Current()
            && (status.state==NativePreferencesState::Ready || status.state==NativePreferencesState::Missing),
            "Menu scenes require an observed native preferences file or absence");
        Require(initial==1 || initial==13, "Menu scenes must begin at Main or Options");
        FrontendStackRequest request; request.scene=initial; request.language=language;
        request.initial_slide=initial==1?"MAIN":"in";
        request.resources_mode=FrontendSessionResourcesMode::PermanentMain;
        token=stack.QueuePush(std::move(request)); Bind(token,initial);
    }
    void Ready() const
    { Require(thread==std::this_thread::get_id() && !released && !failed && !nlGetCurrentAsyncRead(), "Menu scenes require their live owner thread"); }
    void Mutable() const { Ready(); Require(!busy && !input_window, "Recursive menu scenes mutation is unsupported"); }
    void Bind(FrontendSceneStack::Token t, unsigned destination)
    {
        Require(destination==1 || destination==13 || destination==14 || destination==15, "Menu destination has no selected visual factory");
        if(destination==14)Require(volumes&&audio->CategoryVolumes()==volumes,"Audio submenu requires the actual shared live category authority");
        if(destination==15)Require(bool(visual_settings),"Visual submenu requires its explicit settings authority");
        stack.BindVisual(t,[this,destination](auto context)->std::shared_ptr<FrontendStackVisual> {
            if(destination==1)
            {
                auto owner=std::make_shared<FrontendMainMenu>(context.session,input,audio,seed,context.handler,false);
                main=owner; options.reset(); scene=destination; return owner;
            }
            if(destination==13)
            {
                auto owner=std::make_shared<FrontendOptions>(context.session,input,audio,seed,context.handler,0);
                options=owner; main.reset(); scene=destination; return owner;
            }
            if(destination==14)
            {
                auto owner=std::make_shared<FrontendAudioOptions>(context.session,input,audio,volumes,seed,context.handler,0);
                audio_options=owner; scene=destination; return owner;
            }
            auto owner=std::make_shared<FrontendVisualOptions>(context.session,input,audio,visual_settings,seed,context.handler,0);
            visual_options=owner; scene=destination; return owner;
        });
    }
    void Retire()
    { options_nav.reset(); audio_nav.reset(); visual_nav.reset(); main.reset(); options.reset(); audio_options.reset(); visual_options.reset(); scene=0; menu_shown.reset(); input_menu.reset(); pending_scene.reset(); }
    void Observe()
    {
        if(token) stack.RethrowFailure(token);
        if(!nav_session && token && stack.Entry(token).prepared)
        {
            nav_session=std::make_shared<FrontendSession>();
            nav_session->BeginShared({"/Art/fe/fe_overlay.fen",language,FrontendImageProfile::Main,"Slide1",true},stack.Resources(token));
        }
        if(nav_session && !nav)
        {
            nav_session->Poll();
            if(nav_session->State()!=FrontendSessionState::Loading)
            {
                nav_session->Result(); nav=std::make_shared<FrontendNavigation>(nav_session,input,audio,seed);
            }
        }
        if(nav && options && !options_nav) options_nav=std::make_unique<FrontendOptionsNavigation>(*options,*nav,music);
        if(nav && audio_options && !audio_nav)audio_nav=std::make_unique<FrontendAudioNavigation>(audio_options,nav,input,audio,seed,preferences);
        if(nav && visual_options && !visual_nav)visual_nav=std::make_unique<FrontendVisualNavigation>(visual_options,nav,input,audio,seed,preferences);
    }
    void Destination()
    {
        const auto status=transition.Status();
        if(status.state==FrontendMenuTransitionState::SceneQueued && status.queued_scene && *status.queued_scene!=transition_destination)
        {
            Retire(); token=*status.queued_scene;transition_destination=token;Bind(token,stack.Entry(token).scene);
        }
    }
    FrontendMenuTransitionServices Services()
    {
        return {
            [this]()->std::optional<bool> {
                preferences->RethrowFailure();
                return preferences->Status().host_pending || preferences->DepartureBlocked(NativePreferencesScope::NativePreferences);
            },
            effect,
            [this](std::string_view function) {
                Require(bool(nav), "Main departure requires retained NAV");
                nav->StartTransition(nav->Current(),std::string(function),[this](auto name){transition.NavigationTransition(name);});
                return true;
            },
            [this] {
                Require(bool(nav), "Options return requires retained NAV");
                music(1); nav->HideButtons(nav->Current());
                for(unsigned i=0;i<4;++i) nav->SetPointerSlide(nav->Current(),i,FrontendNavigationPointer::Waiting);
                return true;
            }
        };
    }
    bool Interactive() const
    {
        if(main) return main->Status().interactive && !main->Status().selection;
        if(options) { const auto s=options->Status(); return s.initialized && s.state==1 && !s.transition; }
        if(audio_options){const auto s=audio_options->Status();return s.initialized&&s.state==1&&!s.native_save_admitted;}
        if(visual_options){const auto s=visual_options->Status();return s.initialized&&s.state==1&&!s.native_save_admitted;}
        return false;
    }
    void Commands()
    {
        if(options_nav)options_nav->ApplyCommands();
        if(audio_nav)audio_nav->ApplyCommands();
        if(visual_nav)visual_nav->ApplyCommands();
    }
    void Replace(unsigned destination)
    {
        // Both original13→child and child→13 use Push(...,0,true): FIFO
        // Pop followed by a new handler/FEN, never an older Options revealed.
        Require(destination==13||destination==14||destination==15,"Unselected Options destination");
        if(destination==14)Require(volumes&&audio->CategoryVolumes()==volumes,"Audio submenu has no live category authority");
        if(destination==15)Require(bool(visual_settings),"Visual submenu has no desired camera settings authority");
        auto resources=stack.Resources(token);
        stack.QueuePop(token);
        FrontendStackRequest request;request.scene=destination;request.language=language;
        request.initial_slide=destination==13?"in":"OPTIONS_IN";request.shared_resources=std::move(resources);
        const auto next=stack.QueuePush(std::move(request));Retire();token=next;Bind(token,destination);
    }
    void Actions()
    {
        if(main)
        {
            const auto selection=main->Status().selection;
            if(selection && selection->item==6 && selection->service==FrontendMainSelectionService::ApplyItem)
            {
                BeginMainOptions(*main,stack,token,*nav,*audio,seed,transition,Services());
                Retire(); token=0;
            }
        }
        else if(options)
        {
            const auto request=options->Status().transition;
            if(!request) return;
            if(request->kind==FrontendOptionsCommandKind::TransitionOptionsToMainMenu)
            {
                transition.Begin(FrontendMenuTransitionKind::OptionsToMain,token,request->source,Services());
                stack.QueuePop(token); Retire(); token=0;
            }
            else if(request->scene==14||request->scene==15)Replace(unsigned(request->scene));
            else pending_scene=unsigned(request->scene);
        }
        else if(audio_options||visual_options)
        {
            bool requested=false;
            if(audio_options)for(const auto& c:audio_options->Status().commands)
                requested|=c.kind==FrontendAudioOptionsCommandKind::PushOptions;
            if(visual_options)for(const auto& c:visual_options->Status().commands)
                requested|=c.kind==FrontendVisualOptionsCommandKind::PushOptions;
            // Keep the caller-owned native worker alive past the source outro.
            // Its actual failure/pending state cannot become a successful return.
            if(requested&&!preferences->Status().host_pending){preferences->RethrowFailure();Replace(13);}
        }
    }
    void InputReady() const
    { Ready(); Require(input_window && input_menu && nav_shown && input_allowed, "Menu input requires its controlled presented interactive window"); }
    void Pointer(const FrontendPointerDispatch& dispatch)
    {
        std::array<FrontendNavigationPointerSample,4> cursors{};
        Require(dispatch.event.index<4, "Menu pointer index exceeds original four pointers");
        cursors[dispatch.event.index]={dispatch.event.position,0,dispatch.active};
        nav->UpdatePointers(nav->Current(),cursors);
    }
};
FrontendMenuScenes::FrontendMenuScenes(FrontendInput& input,std::shared_ptr<FrontendAudio> audio,unsigned& seed,
    FrontendCameras& cameras,resources::Bytes script,std::shared_ptr<NativePreferences> preferences,
    std::function<void(unsigned)> music,std::function<bool(unsigned)> effect,std::function<void()> drain,
    unsigned initial,FrontendLanguage language,AudioCategoryVolumes::Handle volumes,FrontendVisualSettings::Handle visual_settings)
    :impl_(std::make_unique<Implementation>(input,std::move(audio),seed,cameras,script,std::move(preferences),std::move(music),std::move(effect),std::move(drain),initial,language,std::move(volumes),std::move(visual_settings))){}
FrontendMenuScenes::~FrontendMenuScenes(){try{Release();}catch(...){std::terminate();}}
void FrontendMenuScenes::Service()
{
    auto& s=*impl_;s.Mutable();Guard guard(s.busy);
    try { s.preferences->Poll();s.preferences->RethrowFailure();s.Destination();s.stack.Service();s.Observe();
        if(s.nav_session && !s.nav) { s.nav_session->Service();s.Observe(); } }
    catch(...) {s.failed=true;throw;}
}
void FrontendMenuScenes::Update(float delta,const std::function<void()>& input)
{
    auto& s=*impl_;s.Mutable();Require(std::isfinite(delta)&&delta>=0&&delta<=1,"Menu frame delta must be finite in0..1");
    Service();Guard guard(s.busy);
    try
    {
        if(!s.nav)return;
        s.cameras.Advance(delta,delta);s.transition.Update(delta);
        s.nav->AdvanceVisual(s.nav->Current(),delta);s.Destination();
        const bool allowed=s.Interactive();
        s.stack.Update(delta,[&](auto token,const auto& shown){
            Require(token==s.token && shown==s.menu_shown,"Menu input crossed its published scene owner");
            s.Commands();s.input_menu=shown;s.input_allowed=allowed&&s.Interactive();s.input_window=true;s.desktop_back_checked=false;
            struct Reset{Implementation& s;~Reset(){s.input_window=false;s.input_menu.reset();s.input_allowed=false;}}reset{s};
            if(input&&s.input_allowed&&s.nav_shown)input();s.Commands();
            if(s.main&&s.main->Status().interactive)s.nav->SetPointerSlide(s.nav->Current(),0,FrontendNavigationPointer::Cursor);
        });
        if(s.token)s.stack.RethrowFailure(s.token);
        s.Actions();s.Destination();s.stack.Poll();s.Observe();
    }
    catch(...){s.failed=true;throw;}
}
FrontendPointerDispatch FrontendMenuScenes::Route(const FrontendPointerDesktopSample& sample)
{
    auto& s=*impl_;s.InputReady();FrontendPointerDispatch result;
    if(s.main)result=s.main->Route(sample);
    else if(s.options_nav)result=s.options_nav->Route(s.input_menu,s.nav_shown,sample).pointer;
    else if(s.audio_nav)result=s.audio_nav->Route(s.input_menu,s.nav_shown,sample).pointer;
    else result=s.visual_nav->Route(s.input_menu,s.nav_shown,sample).pointer;
    s.Pointer(result);return result;
}
std::optional<FrontendPointerDispatch> FrontendMenuScenes::BackShortcut()
{
    auto& s=*impl_;s.InputReady();Require(!s.desktop_back_checked,"Desktop Back shortcut was queried twice this tick");s.desktop_back_checked=true;
    const auto binding=s.main?std::optional<FrontendNavigationBackBinding>{}:s.nav->BackButton(s.nav_shown);
    const auto press=FrontendDesktopBackEvent(s.input,binding);if(!press)return {};
    FrontendPointerDispatch event;event.active=true;event.event=*press;
    DeliverPointer(event.event);s.Pointer(event);return event;
}
FrontendPointerDispatch FrontendMenuScenes::Poll(SDL_Window* window,bool capture)
{
    auto& s=*impl_;s.InputReady();FrontendPointerDispatch result;
    if(s.main)result=s.main->Poll(window,capture);
    else if(s.options_nav)result=s.options_nav->Poll(s.input_menu,s.nav_shown,window,capture).pointer;
    else if(s.audio_nav)result=s.audio_nav->Poll(s.input_menu,s.nav_shown,window,capture).pointer;
    else result=s.visual_nav->Poll(s.input_menu,s.nav_shown,window,capture).pointer;
    s.Pointer(result);return result;
}
void FrontendMenuScenes::DeliverPointer(const FrontendPointerEvent& event)
{
    auto& s=*impl_;s.InputReady();
    if(s.main)s.main->DeliverPointer(s.input_menu,event);
    else if(s.options)s.nav->WithPresentedInput(s.nav_shown,[&]{
        if(s.nav->DeliverPointer(s.nav_shown,event))s.options->NotifyBackButton(s.input_menu);
        else s.options->DeliverPointer(s.input_menu,event);
    });
    else if(s.audio_nav)s.audio_nav->Deliver(s.input_menu,s.nav_shown,event);
    else s.visual_nav->Deliver(s.input_menu,s.nav_shown,event);
    // Explicit source delivery makes no desktop focus/cursor-validity decision.

}
FrontendMenuScenesFrame FrontendMenuScenes::Current()const
{
    auto& s=*impl_;s.Ready();FrontendMenuScenesFrame frame;
    if(s.nav)frame.navigation=s.nav->Current();
    if(s.token&&s.scene){const auto entry=s.stack.Entry(s.token);if(!entry.queued_pop&&entry.prepared){frame.token=s.token;frame.scene=s.scene;frame.menu=entry.prepared;}}
    return frame;
}
void FrontendMenuScenes::Acknowledge(const FrontendMenuScenesFrame& frame,FrontendPointerViewport viewport)
{
    auto& s=*impl_;s.Mutable();const auto current=Current();
    Require(frame.token==current.token && frame.scene==current.scene && frame.menu==current.menu
        && frame.navigation && frame.navigation==current.navigation,"Menu publication must match both actual composed frames");
    Guard guard(s.busy);
    if(frame.menu)
    {
        const auto entry=s.stack.Entry(frame.token);
        if(entry.state==FrontendStackState::AwaitingPublication)s.stack.Publish(frame.token,frame.menu);
        else Require(entry.state==FrontendStackState::Published&&entry.published==frame.menu,"Menu publication is stale");
        if(s.main)s.main->Acknowledge(frame.menu,viewport);
        else if(s.options)s.options->Acknowledge(frame.menu,viewport);
        else if(s.audio_options)s.audio_options->Acknowledge(frame.menu,viewport);
        else s.visual_options->Acknowledge(frame.menu,viewport);
        s.menu_shown=frame.menu;
    }
    s.nav->Acknowledge(frame.navigation,viewport);s.nav_shown=frame.navigation;
    if(s.audio_nav)s.audio_nav->Acknowledge(frame.navigation);
    if(s.visual_nav)s.visual_nav->Acknowledge(frame.navigation);
}
FrontendMenuScenesStatus FrontendMenuScenes::Status()const
{
    const auto& s=*impl_;Require(s.thread==std::this_thread::get_id()&&!s.released,"Menu status requires its owner thread");
    FrontendMenuScenesStatus status;status.scene=s.scene;status.failed=s.failed;status.loading=!s.nav||!s.scene;
    status.pending_scene=s.pending_scene;status.transition=s.transition.Status();
    if(!s.failed){status.interactive=s.Interactive();if(s.main)status.main_selection=s.main->Status().selection;if(s.options)status.state=s.options->Status().state;
        if(s.audio_options)status.state=s.audio_options->Status().state;
        if(s.visual_options)status.state=s.visual_options->Status().state;}
    return status;
}
std::vector<FrontendPointerBounds> FrontendMenuScenes::Bounds()const
{
    auto& s=*impl_;s.Ready();if(s.main){auto b=s.main->Bounds();return{b.begin(),b.end()};}
    if(s.options){auto b=s.options->Bounds();return{b.begin(),b.end()};}
    if(s.audio_options){auto b=s.audio_options->Bounds();return{b.begin(),b.end()};}
    Require(bool(s.visual_options),"No live menu pointer regions");auto b=s.visual_options->Bounds();return{b.begin(),b.end()};
}
FrontendPointerBounds FrontendMenuScenes::BackBounds()const
{auto& s=*impl_;s.Ready();Require(bool(s.nav),"NAV is not loaded");return s.nav->Bounds();}
FrontendPointerBounds FrontendMenuScenes::DoneBounds()const
{
    auto& s=*impl_;s.Ready();if(s.audio_nav)return s.audio_nav->DoneBounds();
    Require(bool(s.visual_nav),"No live submenu Done pointer region");return s.visual_nav->DoneBounds();
}
void FrontendMenuScenes::Release()
{
    auto& s=*impl_;Require(s.thread==std::this_thread::get_id()&&!s.busy&&!s.input_window&&!nlGetCurrentAsyncRead(),"Menu teardown requires its idle owner thread");
    if(s.released)return;s.transition.Release();s.options_nav.reset();s.audio_nav.reset();s.visual_nav.reset();if(s.nav)s.nav->Release();
    s.stack.Release();s.main.reset();s.options.reset();s.audio_options.reset();s.visual_options.reset();s.nav.reset();if(s.nav_session)s.nav_session->Pop();
    s.nav_session.reset();s.menu_shown.reset();s.nav_shown.reset();s.audio.reset();s.preferences.reset();s.released=true;
}
}

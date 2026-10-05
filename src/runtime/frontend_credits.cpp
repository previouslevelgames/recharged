#include "runtime/frontend_credits.h"
#include "runtime/frontend_handler.h"
#include "resources/frontend_animation.h"
#include "resources/credits_text.h"
#include "runtime/frontend_movie_binding.h"
#include "Game/FE/FrontendMoviePlayerSteps.h"
#include "NL/nlFile.h"
#include "NL/nlMemory.h"
#include <cstdio>
#include <cstring>
#include "Game/FE/FrontendCreditsSteps.h"
#include "Game/FE/FrontendMainMenuSteps.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <thread>
namespace mscharged
{
namespace
{
using namespace resources;
void Check(bool v,const char* message){if(!v)throw std::logic_error(message);}
struct Guard{bool& value;explicit Guard(bool& v):value(v){value=true;}~Guard(){value=false;}};
struct Effect{bool audio=false;std::uint32_t cue=0;FrontendCreditsCommand command{};bool stop_movie=false;};
struct ScrollState
{
    std::array<std::uint32_t,20> ids{};
    std::array<bool,20> on_screen{},centered{};
    std::array<std::array<char16_t,64>,20> strings{};
    CreditsText::Handle tokens;
    std::size_t cursor=0;
};
struct Step
{
    FrontendAnimationPlayback& playback;FrontendCreditsStatus value;ScrollState scroll;bool widescreen;unsigned video;
    std::function<void()> start,stop,swap;
    std::function<bool()> finished;
    std::function<bool(unsigned)> pressed;
    FrontendMovieOptions options;
    std::vector<Effect> effects;
    bool& mFadeStarted=value.fade_started;float& mTimeElapsed=value.elapsed;unsigned& mPhase=value.phase;
    bool& mMovieStarted=value.movie_started;bool& mSwappedTexture=value.texture_swapped;
    bool& mAreCreditsOver=value.credits_over;bool& mFinalMessageDisplayed=value.final_message_displayed;
    const char* mMovieFilename=nullptr;bool mLoopMovie=false;
    auto& Strings(){return scroll.strings;}
    std::array<std::array<char16_t,64>,20>& mStrings=scroll.strings;
    std::array<bool,20>& mLineOnScreen=scroll.on_screen;
    std::array<bool,20>& mCenteredLine=scroll.centered;
    unsigned mNextScene=13;
    struct EmptySlide{float m_time=0;void Update(float){throw std::logic_error("Credits default component must remain empty");}};
    struct EmptyComponent{EmptySlide* pChildren=nullptr;EmptySlide* m_pActiveSlide=nullptr;} default_fade;
    struct Slide{Step& owner;std::uint32_t id;void Update(float delta){owner.playback.AdvanceSlide(id,delta);}};
    struct Component
    {
        Step& owner;std::uint32_t id;
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {
            if(!id)
            {
                FrontendMainComponentSelect<EmptySlide>(owner.default_fade,FrontendLowerHash(name),reset,preserve,
                    [](EmptySlide* children,unsigned long)->EmptySlide*{Check(!children,"Credits default component gained a slide");return nullptr;});return;
            }
            const auto& instance=owner.Instance(id);Check(instance.type==4&&instance.library,"Credits fade is not an authored component");
            Check(owner.playback.SelectComponent(*instance.library,name,reset,preserve),"Credits fade slide is absent");
        }
        void Update(float dt)
        {
            // Original TLComponentInstance::Update returns immediately when its
            // library has no active slide. The genuine default has an empty ring.
            if(id)owner.playback.AdvanceComponent(id,dt);
            else Check(dt==0&&!owner.default_fade.m_pActiveSlide,"Credits default fade update differs");
        }
    };
    struct Position{struct{float x,y,z;}f;};struct Vector2{float x,y;};
    struct Text
    {
        Step& owner;std::uint32_t id;Position position;std::array<float,3> scale;
        bool m_bVisible;unsigned m_OverloadFlags,m_DrawOptions;
        struct{Vector2 BoxSize;}m_OverloadedAttributes;
        bool position_changed=false,scale_changed=false,string_changed=false;std::u16string text;
        Text(Step& s,std::uint32_t i):owner(s),id(i),position{{s.Instance(i).attributes.position[0],s.Instance(i).attributes.position[1],s.Instance(i).attributes.position[2]}},scale(s.Instance(i).attributes.scale),m_bVisible(s.Instance(i).visible),m_OverloadFlags(s.Instance(i).text_overload_flags),m_DrawOptions(s.Instance(i).draw_options),m_OverloadedAttributes{{s.Instance(i).text_box[0],s.Instance(i).text_box[1]}}
        {Check(s.Instance(i).type==3,"Credits line lookup requires stored text type");}
        Position GetAssetPosition()const{return position;}
        void SetAssetPosition(float x,float y,float z){position={{x,y,z}};position_changed=true;}
        void SetAssetScale(float x,float y,float z){scale={x,y,z};scale_changed=true;}
        void SetString(const std::array<char16_t,64>& value){text=value.data();string_changed=true;}
        void Flush(std::vector<FrontendInstanceChange>& changes)
        {
            const auto& old=owner.Instance(id);
            const auto change=[&](FrontendInstanceProperty p)->FrontendInstanceChange&{changes.push_back({});changes.back().instance=id;changes.back().property=p;return changes.back();};
            if(position_changed)change(FrontendInstanceProperty::Position).vector={position.f.x,position.f.y,position.f.z};
            if(scale_changed)change(FrontendInstanceProperty::Scale).vector=scale;
            if(string_changed)change(FrontendInstanceProperty::String).text=text;
            if(m_bVisible!=old.visible)change(FrontendInstanceProperty::Visible).flag=m_bVisible;
            if(m_DrawOptions!=old.draw_options||((m_OverloadFlags^old.text_overload_flags)&0x10))change(FrontendInstanceProperty::TextDrawOptions).draw_options=m_DrawOptions;
            const std::array box{m_OverloadedAttributes.BoxSize.x,m_OverloadedAttributes.BoxSize.y};
            if(box!=old.text_box||((m_OverloadFlags^old.text_overload_flags)&4))change(FrontendInstanceProperty::TextBox).text_box=box;
        }
    };
    std::map<std::uint32_t,Slide> slides;std::map<std::uint32_t,Component> components;std::map<std::uint32_t,Text> texts;
    std::array<Text*,20> m_pTextLines{};
    struct Presentation
    {
        Step& owner;Slide* m_currentSlide=nullptr;
        void SetActiveSlide(const char* name,bool reset)
        {Check(owner.playback.SelectPresentation(name,reset),"Credits presentation is absent");m_currentSlide=owner.Active();}
        void Update(float dt){owner.playback.Advance(dt);m_currentSlide=owner.Active();}
    } presentation{*this};Presentation* mPresentation=&presentation;
    struct Parser
    {
        Step& owner;const CreditsText* mFileData=nullptr;
        struct Tokens
        {
            Step& owner;
            const char* NextToken(bool advance)
            {Check(!advance,"Credits parser token admission changed");auto& s=owner.scroll;return s.tokens&&s.cursor<s.tokens->tokens.size()?s.tokens->tokens[s.cursor].c_str():nullptr;}
            void AdvanceLine(){auto& s=owner.scroll;Check(s.tokens&&s.cursor<s.tokens->tokens.size(),"Credits parser cursor overflow");++s.cursor;}
        }mParser{owner};
        void Load()
        {
            std::unique_ptr<nlFile> file(nlOpen("credits.txt"));Check(bool(file),"Credits text file is absent");
            const auto size=nlFileSize(file.get(),nullptr);Check(size>0&&size<=65536,"Credits text exceeds source-safe parser budget");file.reset();
            unsigned long actual=0;void* raw=nlLoadEntireFile("credits.txt",&actual,0x20,AllocateEnd,nullptr,0,nullptr);
            std::unique_ptr<void,void(*)(void*)> bytes(raw,[](void* p){if(p)nlFree(p);});
            Check(raw&&actual==size,"Credits text whole-file read failed");
            owner.scroll.tokens=ReadCreditsText(Bytes(static_cast<const std::uint8_t*>(raw),actual));owner.scroll.cursor=0;mFileData=owner.scroll.tokens.get();
        }
    }mCreditParser{*this};
    Step(FrontendAnimationPlayback& p,FrontendCreditsStatus s,ScrollState old,bool wide,unsigned v):playback(p),value(std::move(s)),scroll(std::move(old)),widescreen(wide),video(v)
    {
        presentation.m_currentSlide=Active();if(value.movie)mMovieFilename=value.movie->path.c_str();
        mCreditParser.mFileData=scroll.tokens.get();for(unsigned i=0;i<20;++i)if(scroll.ids[i])m_pTextLines[i]=TextFor(scroll.ids[i]);
    }
    const FrontendInstance& Instance(std::uint32_t id)const
    {const auto& all=playback.Scene().instances;auto it=std::find_if(all.begin(),all.end(),[&](const auto& n){return n.offset==id;});Check(it!=all.end(),"Credits instance is absent");return *it;}
    Text* TextFor(std::uint32_t id){return &texts.try_emplace(id,*this,id).first->second;}
    Text* FindText(FrontendNode root,std::span<const std::string_view> names)
    {auto found=FindFrontendNode(playback.Scene(),root,FrontendNamedPath(names),FrontendNodeType::Text);Check(bool(found),"Credits required text is absent");return TextFor(found->id);}
    Slide* Active()
    {const auto id=playback.Scene().active_slide;Check(bool(id),"Credits active slide is absent");return &slides.try_emplace(*id,Slide{*this,*id}).first->second;}
    void Command(FrontendCreditsCommandKind kind,unsigned argument=0)
    {Check(effects.size()<16,"Credits external request budget exceeded");effects.push_back({false,0,{kind,argument}});}
    void SetMovieDetails(const char* path,bool sound,bool loop)
    {
        Check(sound&&!loop,"Credits movie details differ from original profile");Check(!value.movie||value.movie->generation!=UINT64_MAX,"Credits movie generation exhausted");
        FrontendCreditsMovieRequest request;request.generation=value.movie?value.movie->generation+1:1;request.path=path;
        value.movie=std::move(request);value.boundary=FrontendCreditsBoundary::MoviePlayback;mMovieStarted=false;mMovieFilename=value.movie->path.c_str();
        // Original SetMovieDetails deliberately does not reset mSwappedTexture.
    }
    void SetupForCredits()
    {
        FrontendCreditsSetupScrolling<Vector2>(*this,[&]{return widescreen;},[&]{return video;},[&]{return &presentation;},
            [&](Presentation*){const std::array<std::string_view,3> path{"CREDITS","Layer","Final Message"};return FindText({FrontendNodeKind::Presentation,0},path);},
            [&](Slide* slide,const char* layer,const char* line){const std::array<std::string_view,2> path{layer,line};return FindText({FrontendNodeKind::Slide,slide->id},path);},
            [](char* name,unsigned size,int i){const auto n=std::snprintf(name,size,"line%d",i);Check(n>0&&unsigned(n)<size,"Credits line name overflow");});
        for(unsigned i=0;i<20;++i)scroll.ids[i]=m_pTextLines[i]->id;
    }
    void SetupForPhase()
    {
        FrontendCreditsSetupPhase(*this,[&]{return widescreen;},[&]{return video;},
            [&](std::uint32_t cue,int a,int b,int c){Check(!a&&!b&&c==1,"Credits cue arguments differ");effects.push_back({true,cue,{}});},
            [&](bool enabled){Command(FrontendCreditsCommandKind::StadiumRendering,enabled);},
            [&](unsigned scene,int screen,bool pop){Check(!screen&&pop,"Credits replacement topology differs");Command(FrontendCreditsCommandKind::ReplaceScene,scene);},
            [&](unsigned id){Command(FrontendCreditsCommandKind::SelectMusic,id);});
    }
    void MoviePlayerVirtual3C(){FrontendCreditsMovieCompleted(*this);}
    void MovieUpdate(float dt)
    {
        unsigned volume_calls=0;bool base_observed=false;
        FrontendMoviePlayerUpdate(*this,dt,[&](float original_delta){Check(!base_observed&&original_delta==dt,"Credits movie base proof order differs");base_observed=true;},[]{return false;},
            [&](const char* path,bool sound,bool loop,bool mono){Check(value.movie&&value.movie->path==path&&!sound&&!loop&&mono==options.mono,"Credits source start profile differs");start();return true;},
            [&]{return options.mono;},[](bool enabled){Check(enabled,"Credits movie must use original synced decode");},
            [](const char* path){return std::strstr(path,"nlg")!=nullptr;},
            [](char* out,unsigned size,const char* format,const char* name){const int n=std::snprintf(out,size,format,name);Check(n>0&&unsigned(n)<size,"Credits movie config key overflow");},
            [&](const char* key,int fallback){const std::string_view name(key);if(name.ends_with("/Volume")){Check(fallback==100,"Movie volume default differs");return int(options.volume_percent);}Check(name.ends_with("/FadeIn")&&fallback==500,"Movie fade config differs");return int(options.fade_in_ms);},
            [&](int volume,int fade){if(volume_calls++==0)Check(!volume&&!fade,"Original movie initial volume differs");else Check(volume==int(127.0f*(float(options.volume_percent)/100.0f))&&fade==int(options.fade_in_ms),"Actual movie output volume profile differs");},
            []{return false;},[](bool){throw std::logic_error("Strikers101 world rendering is outside Credits profile");},
            [&]{return pressed(0x1e);},[&]{stop();},[&]{swap();},[&]{return finished();});
    }
    void Update(float dt)
    {
        switch(mPhase)
        {
        case 0:FrontendCreditsUpdateNintendoLogo(*this,dt);break;
        case 1:MovieUpdate(dt);break;
        case 2:FrontendCreditsUpdateScrolling(*this,dt,[&](float d){MovieUpdate(d);},[&](int action){return pressed(action);},
            [&](const CreditsText* p){Check(scroll.tokens.get()==p,"Credits parser release identity differs");scroll.tokens.reset();},[&]{stop();});break;
        case 3:FrontendCreditsUpdateCopyrightMessage(*this,dt);break;
        default:throw std::logic_error("Credits source queued scene replacement; no further update is defined");
        }
    }
    void Flush()
    {
        std::vector<FrontendInstanceChange> changes;changes.reserve(texts.size()*6);for(auto& [id,text]:texts)text.Flush(changes);if(!changes.empty())playback.Apply(changes);
        value.parser_position=static_cast<unsigned>(scroll.cursor);value.parser_tokens=scroll.tokens?static_cast<unsigned>(scroll.tokens->tokens.size()):0;
        value.lines_on_screen=static_cast<unsigned>(std::count(scroll.on_screen.begin(),scroll.on_screen.end(),true));
    }
    Component* GetWhiteFadeComponent()
    {
        const std::array<std::string_view,2> path{"Layer","WHITE FADE"};const auto active=playback.Scene().active_slide;Check(bool(active),"Credits white fade requires its current presentation");
        const auto found=FindFrontendNode(playback.Scene(),{FrontendNodeKind::Slide,*active},FrontendNamedPath(path));
        if(!found){value.default_fade=true;return &components.try_emplace(0,Component{*this,0}).first->second;}
        Check(found->kind==FrontendNodeKind::Instance&&Instance(found->id).type==4,"Credits white fade stored type is not a component");return &components.try_emplace(found->id,Component{*this,found->id}).first->second;
    }
};
}
struct FrontendCredits::Implementation
{
    std::shared_ptr<FrontendSession> session;FrontendInput& input;std::shared_ptr<FrontendAudio> audio;
    unsigned& seed;Services services;bool wide;unsigned video;
    std::shared_ptr<FrontendHandler> handler;Frame current;FrontendCreditsStatus state;ScrollState scroll;
    MovieFactory movie_factory;FrontendMovieOptions movie_options;
    std::shared_ptr<FrontendMoviePlayback> movie;
    std::shared_ptr<const FrontendMovieImageBinding> binding;
    std::uint64_t retrace=0;bool have_retrace=false;
    bool MoviePhase()const{return state.phase==1||state.phase==2;}
    bool Prepared()const{return movie&&state.movie&&Same(movie->Request(),*state.movie)&&movie->Status().state!=FrontendMovieState::Cancelled&&movie->Status().state!=FrontendMovieState::Failed;}
    static bool Same(const FrontendMovieRequest& a,const FrontendMovieRequest& b)
    {return a.generation==b.generation&&a.path==b.path&&a.details_with_sound==b.details_with_sound&&a.start_with_sound==b.start_with_sound&&a.loop==b.loop&&a.synced==b.synced;}
    static bool Same(const FrontendMovieOptions& a,const FrontendMovieOptions& b)
    {return a.mono==b.mono&&a.volume_percent==b.volume_percent&&a.fade_in_ms==b.fade_in_ms&&a.device_id==b.device_id;}
    std::uint32_t MovieInstance()const
    {
        Check(state.movie&&current->graph.active_slide,"Credits movie target requires an active request/slide");
        const std::array<std::string_view,2> path{"Layer","movie"};const auto n=FindFrontendNode(current->graph,{FrontendNodeKind::Slide,*current->graph.active_slide},FrontendNamedPath(path),FrontendNodeType::Image);
        Check(bool(n),"Credits movie image is absent");return n->id;
    }
    void CheckBinding()const
    {
        Check(binding&&binding->Active()&&binding->Playback()==movie&&binding->Session()==session&&binding->Generation()==state.movie->generation&&binding->Path()==state.movie->path&&binding->Instance()==MovieInstance(),"Credits requires the exact retained renderer-backed movie binding");
        Check(binding->SourceFrame()->visuals==current->visuals&&binding->SourceFrame()->images==current->images,"Credits movie binding resources differ from its current scene");
    }
    std::vector<FrontendAudioHandle> sounds;const std::thread::id thread=std::this_thread::get_id();
    bool busy=false,restoration_needed=false,stack_attached=false,stack_update=false,input_window=false;Frame input_source;
    Implementation(std::shared_ptr<FrontendSession> s,FrontendInput& i,std::shared_ptr<FrontendAudio> a,unsigned& rng,Services f,bool w,unsigned v)
        :session(std::move(s)),input(i),audio(std::move(a)),seed(rng),services(std::move(f)),wide(w),video(v)
    {
        Check(session&&audio&&audio->Loaded()&&services&&video<=2,"Credits requires retained scene/audio and explicit host services");
        current=session->Current();Check(current&&current->request.animate&&current->request.image_profile==FrontendImageProfile::Main,"Credits requires animated Main resources");

    }
    void Ready()const{Check(thread==std::this_thread::get_id()&&session&&!state.failed&&!busy&&!nlGetCurrentAsyncRead(),"Credits requires its idle live owner thread");}
    void Expected(const Frame& frame)const{Ready();Check((!stack_attached||stack_update)&&frame&&frame==current&&frame==session->Current(),"Credits requires its controlled exact retained current frame");}
    void Service(FrontendCreditsCommand command)
    {
        services(command);Check(session->Current()==current,"Credits service changed the session during publication");
    }
    void Effects(const std::vector<Effect>& effects)
    {
        for(const auto& effect:effects)
        {
            if(effect.stop_movie){Check(bool(movie),"Credits stop requires its real movie owner");movie->Cancel();continue;}
            if(!effect.audio){Service(effect.command);continue;}
            const auto live=audio->Handles();std::erase_if(sounds,[&](auto h){return std::find(live.begin(),live.end(),h)==live.end();});
            sounds.reserve(sounds.size()+1);const auto handle=audio->Play(effect.cue,seed);
            Check(bool(handle),"Credits original FE_GEN cue was not admitted");sounds.push_back(*handle);
        }
    }
    void Cleanup()
    {
        std::exception_ptr error;
        const auto attempt=[&](auto f){try{f();}catch(...){if(!error)error=std::current_exception();}};
        if(restoration_needed)
        {
            // Original CreditScene destructor enables both, not saved values.
            attempt([&]{services({FrontendCreditsCommandKind::PointerEnabled,1});});
            attempt([&]{services({FrontendCreditsCommandKind::StadiumRendering,1});});restoration_needed=false;
        }
        if(audio&&audio->Loaded()&&!sounds.empty())
        {
            const auto live=audio->Handles();for(auto h:sounds)if(std::find(live.begin(),live.end(),h)!=live.end())attempt([&]{audio->Cancel(h);});
        }
        if(movie)attempt([&]{movie->Cancel();});movie.reset();binding.reset();movie_factory={};scroll={};
        sounds.clear();if(handler&&handler.use_count()==1)attempt([&]{handler->Release();});handler.reset();current.reset();session.reset();audio.reset();services={};
        if(error)std::rethrow_exception(error);
    }
};
FrontendCredits::FrontendCredits(std::shared_ptr<FrontendSession> s,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,
    unsigned& seed,Services services,bool wide,unsigned video)
    :FrontendCredits(std::move(s),input,std::move(audio),seed,std::move(services),{},wide,video){}
FrontendCredits::FrontendCredits(std::shared_ptr<FrontendSession> s,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,
    unsigned& seed,Services services,std::shared_ptr<FrontendHandler> base,bool wide,unsigned video)
    :impl_(std::make_unique<Implementation>(std::move(s),input,std::move(audio),seed,std::move(services),wide,video))
{
    auto& owner=*impl_;Check(!base||base->Binds(owner.session),"Credits shared handler belongs to another session");
    owner.handler=base?std::move(base):std::make_shared<FrontendHandler>(owner.session,input);Guard guard(owner.busy);
    try
    {
        owner.restoration_needed=true;owner.Service({FrontendCreditsCommandKind::PointerEnabled,0});
        FrontendCreditsStatus next;std::vector<Effect> effects;
        owner.session->HandlerTransaction(owner.current,[&](auto& playback){Step step(playback,owner.state,{},wide,video);step.SetupForPhase();step.Command(FrontendCreditsCommandKind::StopMusic);next=std::move(step.value);effects=std::move(step.effects);},[&]{owner.Effects(effects);});
        owner.state=std::move(next);owner.current=owner.session->Current();
    }
    catch(...){owner.state.failed=true;owner.Cleanup();throw;}
}
FrontendCredits::~FrontendCredits(){try{Release();}catch(...){std::terminate();}}
FrontendCredits::Frame FrontendCredits::Current()const{impl_->Ready();return impl_->current;}
FrontendCreditsStatus FrontendCredits::Status()const
{auto& s=*impl_;Check(s.thread==std::this_thread::get_id()&&s.session,"Credits status requires live owner thread");auto status=s.state;if(s.MoviePhase())status.boundary=s.Prepared()&&s.binding&&s.binding->Active()?FrontendCreditsBoundary::None:FrontendCreditsBoundary::MoviePlayback;else status.boundary=FrontendCreditsBoundary::None;return status;}
void FrontendCredits::Update(const Frame& expected,float delta)
{
    auto& s=*impl_;Check(!s.stack_attached,"Stack owns the only Credits base update");s.Expected(expected);Check(std::isfinite(delta)&&delta>=0&&delta<=60,"Credits delta exceeds original bounded profile");
    Check(s.state.phase<4&&(!s.MoviePhase()||s.Prepared()),"Credits requires actual prepared movie service or queued scene replacement");
    AfterBaseUpdate(s.handler->UpdateOnce(expected,delta));
}
void FrontendCredits::AfterBaseUpdate(FrontendHandler::UpdateProof&& proof)
{
    auto& s=*impl_;s.Ready();Check(!s.stack_attached||s.stack_update,"Credits proof requires its controlled stack update");
    const float delta=proof.Delta();s.current=s.handler->ConsumeUpdate(std::move(proof),s.current);Guard guard(s.busy);
    try
    {
        FrontendCreditsStatus next;ScrollState next_scroll;std::vector<Effect> effects;
        s.session->HandlerTransaction(s.current,[&](auto& playback)
        {
            Step step(playback,s.state,s.scroll,s.wide,s.video);step.options=s.movie_options;
            step.start=[&]{Check(s.Prepared(),"Credits movie start requires exact prepared provider");s.movie->Check();};
            step.stop=[&]{Check(step.effects.size()<16,"Credits external request budget exceeded");step.effects.push_back({false,0,{},true});};
            step.swap=[&]{s.CheckBinding();ApplyFrontendMovieBinding(playback,s.binding);};
            step.finished=[&]
            {
                Check(s.Prepared(),"Credits natural finish requires current movie owner");s.movie->Check();const auto receipt=s.movie->Completion();
                s.CheckBinding();ApplyFrontendMovieBinding(playback,s.binding);
                if(!receipt)return false;
                Check(receipt->Generation()==s.state.movie->generation&&receipt->Path()==s.state.movie->path&&s.movie->Status().state==FrontendMovieState::Complete,"Credits completion belongs to another movie request");return true;
            };
            step.pressed=[&](unsigned action){Check(action==0x1e||action==0x20,"Credits source button differs");return s.handler->Button(s.current,static_cast<FrontendAction>(action),FrontendButtonQuery::Pressed);};
            step.Update(delta);step.Flush();next=std::move(step.value);next_scroll=std::move(step.scroll);effects=std::move(step.effects);
        },[&]{s.Effects(effects);});
        s.state=std::move(next);s.scroll=std::move(next_scroll);s.current=s.session->Current();
    }
    catch(...){s.state.failed=true;throw;}
}
void FrontendCredits::SetMovieProvider(MovieFactory factory,FrontendMovieOptions options)
{
    auto& s=*impl_;s.Ready();Check(!s.movie_factory&&!s.movie&&factory&&s.state.phase<=1,"Credits movie provider may be bound once before playback");
    Check(options.volume_percent<=100&&options.fade_in_ms<=60000,"Credits movie config exceeds selected source profile");
    s.movie_options=options;s.movie_factory=std::move(factory);
}
void FrontendCredits::ServiceMovie(std::uint64_t retrace)
{
    auto& s=*impl_;s.Ready();Check(!s.stack_update,"Credits movie service requires idle frame scope");
    Check(!s.have_retrace||retrace>=s.retrace,"Credits retrace clock moved backwards");
    Check(retrace<=UINT64_MAX-2,"Credits retrace clock exhausted");
    if(!s.MoviePhase()){s.retrace=retrace;s.have_retrace=true;return;}
    Check(bool(s.movie_factory),"Credits movie playback provider is unavailable");
    Guard guard(s.busy);
    if(s.movie&&Implementation::Same(s.movie->Request(),*s.state.movie))s.movie->Check();
    if(!s.Prepared())
    {
        Check(!s.binding,"Retire the previous movie registration before preparing its successor");
        Check(!s.movie||!Implementation::Same(s.movie->Request(),*s.state.movie),"Credits current movie was cancelled outside its source lifecycle");
        Check(!s.movie||s.movie->Status().state==FrontendMovieState::Cancelled,"Previous Credits movie was not stopped");
        auto candidate=s.movie_factory(*s.state.movie,s.movie_options,retrace);
        Check(candidate&&Implementation::Same(candidate->Request(),*s.state.movie)&&Implementation::Same(candidate->Options(),s.movie_options),"Credits movie factory returned a different request or output profile");
        candidate->Check();const auto status=candidate->Status();
        Check(status.state==FrontendMovieState::Loading&&!candidate->Current()&&!status.published_frames&&!status.submitted_audio_frames&&!candidate->Completion(),"Credits factory must return a fresh paused movie owner");
        Check(s.current==s.session->Current(),"Credits movie factory mutated its scene");
        s.movie=std::move(candidate);
    }
    if(s.state.movie_started)s.movie->Advance(retrace);
    s.retrace=retrace;s.have_retrace=true;
}
std::optional<FrontendCreditsMovieTarget> FrontendCredits::MovieTarget()const
{
    auto& s=*impl_;s.Ready();if(!s.MoviePhase()||!s.Prepared())return {};
    return FrontendCreditsMovieTarget{*s.state.movie,s.MovieInstance(),s.current,s.movie};
}
void FrontendCredits::AttachMovieBinding(std::shared_ptr<const FrontendMovieImageBinding> binding)
{
    auto& s=*impl_;s.Ready();Check(!s.stack_update&&s.Prepared()&&!s.binding&&binding,"Credits binding requires an idle prepared movie");
    Check(binding->Active()&&binding->Session()==s.session&&binding->SourceFrame()==s.current&&binding->Playback()==s.movie&&binding->Instance()==s.MovieInstance()&&binding->Generation()==s.state.movie->generation&&binding->Path()==s.state.movie->path,"Credits renderer binding identity differs");
    s.binding=std::move(binding);
}
std::shared_ptr<const FrontendMovieImageBinding> FrontendCredits::MovieBinding()const
{impl_->Ready();return impl_->binding;}
void FrontendCredits::RetireMovieBinding(const std::shared_ptr<const FrontendMovieImageBinding>& expected)
{
    auto& s=*impl_;s.Ready();Check(!s.stack_update&&expected&&expected==s.binding&&s.movie&&s.movie->Status().state==FrontendMovieState::Cancelled,"Credits can retire only its exact stopped movie binding");
    Check(!expected->Active(),"Credits movie renderer must finish draining/retiring before its lease is released");
    s.binding.reset();
}
void FrontendCredits::DisplayFinalMessage(const Frame& expected)
{
    auto& s=*impl_;s.Expected(expected);Check(s.state.phase==2,"Credits final message requires scrolling phase");Guard guard(s.busy);
    s.session->HandlerTransaction(s.current,[&](auto& playback)
    {
        Step step(playback,s.state,s.scroll,s.wide,s.video);const std::array<std::string_view,3> path{"CREDITS","Layer","Final Message"};
        step.FindText({FrontendNodeKind::Presentation,0},path)->m_bVisible=true;step.Flush();
    });s.state.final_message_displayed=true;s.current=s.session->Current();
}
bool FrontendCredits::Button(const Frame& expected,FrontendAction action,FrontendButtonQuery query,int pad)
{
    auto& s=*impl_;s.Expected(s.current);
    Check(expected&&expected==(s.input_window?s.input_source:s.current)&&(!s.stack_attached||s.input_window),
        "Credits button query requires its exact presented input window");
    return s.handler->Button(s.current,action,query,pad);
}
std::shared_ptr<FrontendSession> FrontendCredits::StackSession()const{impl_->Ready();return impl_->session;}
std::shared_ptr<FrontendHandler> FrontendCredits::StackHandler()const{impl_->Ready();return impl_->handler;}
unsigned FrontendCredits::StackScene()const{return 23;}
bool FrontendCredits::CanUpdateStack()const
{
    auto& s=*impl_;s.Ready();Check(s.stack_attached&&!s.stack_update,"Credits pre-base admission requires its idle stack");
    // CreditScene itself has no early lock return. Only a missing real movie
    // provider stops this selected scope before additional base updates.
    return s.state.phase<4&&(!s.MoviePhase()||(s.Prepared()&&s.binding&&s.binding->Active()&&s.binding->Playback()==s.movie));
}
void FrontendCredits::AttachStack(){auto& s=*impl_;s.Expected(s.current);Check(!s.stack_attached,"Credits already belongs to a stack");s.stack_attached=true;}
void FrontendCredits::UpdateStack(FrontendHandler::UpdateProof&& proof,const Frame& presented,const std::function<void()>& input)
{
    auto& s=*impl_;s.Ready();Check(s.stack_attached&&!s.stack_update&&presented&&presented==s.current&&proof.Before()==presented&&proof.After()==s.session->Current(),"Credits stack proof/presentation differs");
    s.stack_update=true;s.input_source=presented;
    struct Reset{Implementation& s;~Reset(){s.input_window=false;s.input_source.reset();s.stack_update=false;}}reset{s};
    try{AfterBaseUpdate(std::move(proof));s.input_window=true;if(input)input();Check(s.current==s.session->Current(),"Stack input mutated outside its selected Credits owner");}
    catch(...){s.state.failed=true;throw;}
}
void FrontendCredits::ReleaseStack(){auto& s=*impl_;Check(!s.stack_update&&!s.busy,"Cannot remove an active Credits update");s.stack_attached=false;Release();}
void FrontendCredits::Release()
{
    auto& s=*impl_;Check(s.thread==std::this_thread::get_id()&&!s.busy&&!nlGetCurrentAsyncRead(),"Credits teardown requires its idle owner thread");
    if(!s.session)return;Check(!s.stack_attached&&!s.stack_update,"Stack owns Credits teardown");Guard guard(s.busy);s.Cleanup();
}
}

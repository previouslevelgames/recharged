#include "runtime/frontend_packets.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "resources/frontend_animation.h"
#include "NL/nlFileGC.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTextureManager.h"
#include "NL/gl/glTexture.h"
#include "NL/glx/glxTexture.h"
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <type_traits>
using namespace mscharged;
namespace
{
unsigned checks=0;bool fail_drain=false;FrontendPacketRenderer* recursive=nullptr;
void Check(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);}
template<class F>void Reject(F f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid movie binding operation accepted");}
void Drain(){if(recursive)Reject([&]{recursive->RetireMovie();});if(fail_drain)throw std::runtime_error("Injected actual drain failure");}
void Invalidate(){}
std::shared_ptr<FrontendSession> Load(bool owned)
{
    auto s=std::make_shared<FrontendSession>();s->Begin({owned?"/Art/fe/credits.fen":"/Art/fe/credits-pending.fen",FrontendLanguage::English,FrontendImageProfile::Main,"NLG",true},owned?FrontendSessionResourcesMode::Scene:FrontendSessionResourcesMode::PermanentMain);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(s->State()==FrontendSessionState::Loading){s->Service();Check(std::chrono::steady_clock::now()<end,"Real movie FEN load timeout");SDL_Delay(1);}s->Result();return s;
}
unsigned Target(const FrontendSession::Handle& frame)
{
    const std::array<std::string_view,2> names{"Layer","movie"};auto node=resources::FindFrontendNode(frame->graph,{},resources::FrontendNamedPath(names),resources::FrontendNodeType::Image);
    Check(bool(node),"Original Layer/movie lookup is absent");return node->id;
}
FrontendSession::Handle Swap(const FrontendSession::Handle& f,const FrontendMovieImageBinding::Handle& binding)
{
    auto next=std::make_shared<FrontendSessionFrame>(*f);ApplyFrontendMovieBinding(next->graph,binding);
    next->layout=resources::BuildFrontendLayout(next->graph,*next->visuals->localization,std::array{next->visuals->text,next->visuals->heading},next->graph.active_slide,*next->images,{true,true});return next;
}
void Run(bool owned)
{
    const GLMemoryRequirement requirements[]={{GLM_Header,1024*1024},{GLM_VertexData,1024*1024},{GLM_TextureData,1024*1024}};
    const GLMemoryConfig config{1024*1024,1024*1024,requirements,3,2048};
    glInitResourcePools();glInitMemory(&config);InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Invalidate);
    MaterialPrograms materials;const auto slots=glGetTextureManager()->mFreeIndices->mCount;
    auto session=Load(owned);const auto frame=session->Current();const auto instance=Target(frame);
    auto movie=std::make_shared<FrontendMoviePlayback>(FrontendMovieRequest{1,owned?"/Art/movies/nlgintrowide.thp":"/Art/movies/test.thp"},FrontendMovieOptions{},0);
    FrontendPacketRenderer renderer(Drain);renderer.Prepare(frame);const auto static_slots=glGetTextureManager()->mFreeIndices->mCount;
    if(!owned)Check(renderer.MovieStatus().current.awaiting_binding==1&&frame->layout.MovieCount()==1
        &&frame->layout.ImageCount()==0,"Unbound authored movie was not admitted as an explicit nondraw entry");
    Reject([&]{renderer.BindMovie(session,instance+1,movie);});
    auto& parent=*glGetCurrentResourcePool();const auto mark=parent.MarkResource();
    const auto free_slots=glGetTextureManager()->mFreeIndices->mCount;
    std::vector<PlatTexture> held(free_slots-2);
    for(unsigned i=0;i<held.size();++i)glRegisterTexture(0x72000000u+i,&held[i],&parent);
    Reject([&]{renderer.BindMovie(session,instance,movie);});
    Check(glGetTextureManager()->mFreeIndices->mCount==2&&renderer.Current()==frame,"Failed partial movie registration lost slots/publication");
    parent.ReleaseResource(mark);Check(glGetTextureManager()->mFreeIndices->mCount==static_slots,"Partial movie registration did not roll back");
    const auto binding=renderer.BindMovie(session,instance,movie);Check(binding->Active()&&binding->Playback()==movie&&binding->Session()==session&&binding->SourceFrame()==frame,"Movie binding identity changed");
    Check(glGetTextureManager()->mFreeIndices->mCount==static_slots-3,"Movie binding lacks three real texture slots");
    Reject([&]{renderer.BindMovie(session,instance,movie);});
    auto swapped=Swap(frame,binding);Check(swapped->layout.MovieCount()==1,"Movie swap did not retain exactly one authored draw entry");
    for(std::size_t i=0;i<frame->graph.resources.size();++i)Check(frame->graph.resources[i].hash==swapped->graph.resources[i].hash,"Movie swap rewrote original resource name hash");
    const auto* entry=std::get_if<resources::FrontendLayoutMovie>(&*std::find_if(swapped->layout.entries.begin(),swapped->layout.entries.end(),[](const auto& e){return std::holds_alternative<resources::FrontendLayoutMovie>(e);}));
    Check(entry&&entry->image==binding->Image()&&entry->resource==binding->Resource()&&entry->instance==instance,"Movie layout lost native resource provenance");
    Check(entry->uv==std::array<float,4>{0,0,1,1},"Authored movie UV channels changed");
    if(owned)
    {
        Check(instance==20752&&binding->Resource()==9260&&binding->Image()->Width()==512&&binding->Image()->Height()==512,
            "Independent owned FEN/movie association differs");
        Check(std::abs(entry->transform[0]-8.54f)<.00001f&&std::abs(entry->transform[5]+4.8f)<.00001f
            &&entry->transform[12]==320&&entry->transform[13]==240,"Independent owned scale/position oracle differs");
    }
    if(!owned)
    {
        for(float z:{0.f,.752083f,-2.f,16.f})
        {
            auto scaled=swapped->graph;
            for(auto& node:scaled.instances)if(node.offset==instance)
            {
                if(!(node.overload_flags&4))for(const auto& object:scaled.library)if(object.offset==node.library)node.attributes.scale=object.attributes.scale;
                node.overload_flags|=4;node.attributes.scale[2]=z;
            }
            const auto layout=resources::BuildFrontendLayout(scaled,*swapped->visuals->localization,std::array{swapped->visuals->text,swapped->visuals->heading},scaled.active_slide,*swapped->images,{true,true});
            const auto scaled_movie=std::find_if(layout.entries.begin(),layout.entries.end(),[](const auto& e){return std::holds_alternative<resources::FrontendLayoutMovie>(e);});
            Check(scaled_movie!=layout.entries.end(),"Pure movie Z scale was incorrectly rejected");
            const auto& matrix=std::get<resources::FrontendLayoutMovie>(*scaled_movie).transform;
            Check(matrix[10]==z,"Authored movie Z scale was not retained");
            // Independent row-vector XY dot products for all original z=0 corners.
            for(float x:{-50.f,50.f})for(float y:{-50.f,50.f})
            {
                Check(x*matrix[0]+y*matrix[4]+matrix[12]==x*entry->transform[0]+y*entry->transform[4]+entry->transform[12]
                    &&x*matrix[1]+y*matrix[5]+matrix[13]==x*entry->transform[1]+y*entry->transform[5]+entry->transform[13],
                    "Pure Z scale changed original flat movie XY coordinates");
            }
        }
        for(unsigned axis=0;axis<4;++axis)
        {
            auto nonplanar=swapped->graph;
            for(auto& node:nonplanar.instances)if(node.offset==instance)
            {node.overload_flags|=1|2|8;if(axis==0)node.attributes.position[2]=1;else if(axis==1)node.attributes.pivot[2]=1;else node.attributes.rotation[axis-2]=1;}
            const auto layout=resources::BuildFrontendLayout(nonplanar,*swapped->visuals->localization,std::array{swapped->visuals->text,swapped->visuals->heading},nonplanar.active_slide,*swapped->images,{true,true});
            Check(layout.MovieCount()==0&&layout.unavailable.contains("nonplanar instance branch"),"Unsupported movie transform was accepted");
        }
    }
    renderer.Prepare(swapped);Check(renderer.Current()==swapped,"Prepared movie layout was not published");
    Check(renderer.MovieStatus().current.awaiting_frame==1&&!movie->Completion(),"Undecoded registration was treated as a presented movie");
    auto bad=std::make_shared<FrontendSessionFrame>(*swapped);for(auto& e:bad->layout.entries)if(auto* m=std::get_if<resources::FrontendLayoutMovie>(&e))m->image.reset();
    Reject([&]{renderer.Prepare(bad);});Check(renderer.Current()==swapped,"Failed movie admission lost previous frame");
    for(unsigned mode=0;mode<3;++mode)
    {
        auto malformed=std::make_shared<FrontendSessionFrame>(*swapped);
        for(auto& e:malformed->layout.entries)if(auto* m=std::get_if<resources::FrontendLayoutMovie>(&e))
        {if(mode==0)m->transform[0]=65537;if(mode==1)m->uv[0]=70000;if(mode==2)m->colour[0]=2;}
        Reject([&]{renderer.Prepare(malformed);});Check(renderer.Current()==swapped,"Malformed movie layout replaced publication");
    }
    auto graph=frame->graph;for(auto& r:graph.resources)if(r.offset==binding->Resource())++r.hash;
    Reject([&]{ApplyFrontendMovieBinding(graph,binding);});
    resources::FrontendAnimationPlayback playback(frame->graph);ApplyFrontendMovieBinding(playback,binding);
    Check(std::any_of(playback.Scene().resources.begin(),playback.Scene().resources.end(),[&](const auto& r){return r.native_movie==binding->Image();}),"Retained animation swap missing");playback.Reset();
    Check(std::none_of(playback.Scene().resources.begin(),playback.Scene().resources.end(),[](const auto& r){return bool(r.native_movie);}),"Animation reset retained runtime movie handle");
    std::thread other([&]{Reject([&]{binding->Active();});Reject([&]{renderer.RetireMovie();});});other.join();
    fail_drain=true;Reject([&]{renderer.RetireMovie();});fail_drain=false;
    Check(binding->Active()&&renderer.Current()==swapped&&glGetTextureManager()->mFreeIndices->mCount==static_slots-3,
        "Failed drain retired visible movie registration");
    movie->Cancel();renderer.Prepare(swapped);Check(renderer.MovieStatus().current.cancelled==1,"Cancelled active movie was not nondraw");
    resources::FrontendAnimationPlayback transition(swapped->graph);Check(transition.SelectPresentation("CREDITS",true),"Credits source transition absent");
    auto stopped=std::make_shared<FrontendSessionFrame>(*swapped);stopped->graph=transition.Scene();
    stopped->layout=resources::BuildFrontendLayout(stopped->graph,*stopped->visuals->localization,std::array{stopped->visuals->text,stopped->visuals->heading},stopped->graph.active_slide,*stopped->images,{true,true});
    renderer.Prepare(stopped);Check(renderer.MovieStatus().current.cancelled==1&&Target(stopped)!=binding->Instance(),"Same-resource next instance lost cancelled provenance");
    recursive=&renderer;renderer.RetireMovie();recursive=nullptr;Check(!binding->Active()&&glGetTextureManager()->mFreeIndices->mCount==static_slots,"Retired movie retained slots/readiness");
    Reject([&]{ApplyFrontendMovieBinding(graph,binding);});renderer.Prepare(stopped);
    Check(renderer.MovieStatus().current.cancelled==1,"Retired cancelled epoch did not remain explicitly nondraw");
    auto foreign=std::make_shared<FrontendSessionFrame>(*stopped);foreign->request.path+=".foreign";Reject([&]{renderer.Prepare(foreign);});
    auto mismatch=std::make_shared<FrontendSessionFrame>(*stopped);for(auto& r:mismatch->graph.resources)if(r.offset==binding->Resource())++r.file_block;
    Reject([&]{renderer.Prepare(mismatch);});
    for(unsigned n=0;n<3;++n)
    {
        auto next=std::make_shared<FrontendMoviePlayback>(FrontendMovieRequest{n+2,owned?"/Art/movies/credits.thp":"/Art/movies/test.thp"},FrontendMovieOptions{},0);
        if(n==2)Check(session->SelectPresentation("NLG 4:3",true),"4:3 NLG presentation absent");
        auto replacement=renderer.BindMovie(session,Target(session->Current()),next);auto current=Swap(session->Current(),replacement);renderer.Prepare(current);
        if(n==2)
        {
            resources::FrontendAnimationPlayback wide(current->graph);Check(wide.SelectPresentation("Credits 4:3",true),"4:3 Credits presentation absent");
            auto fourthird=std::make_shared<FrontendSessionFrame>(*current);fourthird->graph=wide.Scene();
            fourthird->layout=resources::BuildFrontendLayout(fourthird->graph,*fourthird->visuals->localization,std::array{fourthird->visuals->text,fourthird->visuals->heading},fourthird->graph.active_slide,*fourthird->images,{true,true});
            if(owned)
            {
                Check(fourthird->layout.MovieCount()==0&&fourthird->layout.unavailable.contains("animated slide branch"),
                    "Source 4:3 presentation must wait for its next real base update");
                wide.Advance(0);fourthird->graph=wide.Scene();
                fourthird->layout=resources::BuildFrontendLayout(fourthird->graph,*fourthird->visuals->localization,std::array{fourthird->visuals->text,fourthird->visuals->heading},fourthird->graph.active_slide,*fourthird->images,{true,true});
            }
            next->Cancel();renderer.Prepare(fourthird);
            Check(renderer.MovieStatus().current.cancelled==1,"4:3 source instance transition lost cancelled resource");
            if(owned)Check(replacement->Instance()==19800&&Target(fourthird)==19020,"Independent 4:3 owned instance pair differs");
            current=fourthird;
        }
        if(n==0)
        {
            renderer.RetireMovie();Reject([&]{renderer.Prepare(current);});next->Cancel();
            Reject([&]{renderer.Prepare(current);}); // Cancellation after retirement cannot mint obsolete-epoch authority.
        }
        else {next->Cancel();renderer.RetireMovie();renderer.Prepare(current);Check(renderer.MovieStatus().current.cancelled==1,"Repeated cancelled binding lost pending status");}
        Check(!replacement->Active(),"Repeated movie binding remained active");
        Reject([&]{renderer.Prepare(stopped);}); // Only the immediately retired known epoch is retained.
    }
    if(!owned)
    {
        auto broken=std::make_shared<FrontendMoviePlayback>(FrontendMovieRequest{10,"/Art/movies/bad.thp"},FrontendMovieOptions{},0);
        auto active=renderer.BindMovie(session,Target(session->Current()),broken);auto bad_frame=Swap(session->Current(),active);
        renderer.Prepare(bad_frame);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(broken->Status().state!=FrontendMovieState::Failed)
        {
            try{broken->Advance(broken->Status().published_frames*2);}catch(const std::exception&){}
            Check(std::chrono::steady_clock::now()<deadline,"Malformed real NL movie did not fail");SDL_Delay(1);
        }
        Reject([&]{renderer.Prepare(bad_frame);});Reject([&]{renderer.MovieStatus();});
        renderer.RetireMovie();broken->Cancel();Reject([&]{renderer.Prepare(bad_frame);});
    }
    renderer.Release();movie.reset();session->Pop();session.reset();materials.Release();glShutdownMemory();(void)slots;
}
}
int main(int argc,char** argv)
{
    try
    {
        static_assert(!std::is_default_constructible_v<FrontendMovieImageBinding>);
        static_assert(!std::is_default_constructible_v<resources::FrontendMovieImage>);
        if(argc!=3)throw std::invalid_argument("Expected disc and generated|owned");
        Check(SDL_Init(SDL_INIT_AUDIO),"SDL audio init failed");
        std::vector<std::uint64_t> a(4*1024*1024),b(8*1024*1024);ResetStartupMemory();StandardAllocator.Initialize(a.data(),a.size()*8);VirtualAllocator.Initialize(b.data(),b.size()*8);gMemoryInitialized=1;
        Check(aurora_dvd_open(argv[1]),"Actual movie data mount failed");const auto x=StandardAllocator.TotalFreeMemory(),y=VirtualAllocator.TotalFreeMemory();nlInitFileSystem();
        Run(std::string_view(argv[2])=="owned");ResetStartupFiles();aurora_dvd_close();
        Check(x==StandardAllocator.TotalFreeMemory()&&y==VirtualAllocator.TotalFreeMemory(),"Movie image lifetime leaked original arenas");ResetStartupMemory();SDL_Quit();
        std::cout<<checks<<" retained movie image checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" ("<<checks<<")\n";return 1;}
}

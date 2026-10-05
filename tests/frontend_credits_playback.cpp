#define main frontend_credits_prefix_main
#include "frontend_credits.cpp"
#undef main
#include "Game/FE/FrontendMoviePlayerSteps.h"
#include "resources/frontend_instances.h"
namespace
{
void MovieOrder()
{
 struct Scene{bool mMovieStarted=false,mSwappedTexture=false,mLoopMovie=false;const char* mMovieFilename="art/movies/nlgintrowide.thp";std::vector<std::string> events;void MoviePlayerVirtual3C(){events.push_back("phase");}};
 for(unsigned mode=0;mode<5;++mode)
 {
  Scene s;const bool e3=mode==1,admit=mode!=2,abort=mode==3,finish=mode==4;
  FrontendMoviePlayerUpdate(s,.25f,[&](float dt){Near(dt,.25f);s.events.push_back("base");},[&]{return e3;},
   [&](const char* path,bool sound,bool loop,bool mono){Check(std::string_view(path)==s.mMovieFilename&&!sound&&!loop&&!mono,"Movie source Start contract differs");s.events.push_back("start");return admit;},
   []{return false;},[&](bool v){Check(v,"Movie lost synced decode");s.events.push_back("sync");},[](const char* p){return std::string_view(p).find("nlg")!=std::string_view::npos;},
   [](char* out,unsigned n,const char* format,const char* name){std::snprintf(out,n,format,name);},
   [&](const char* name,int fallback){Check(std::string_view(name).starts_with("FE_Eggman_Movie/"),"Movie config prefix differs");return fallback;},
   [&](int volume,int fade){s.events.push_back("volume:"+std::to_string(volume)+":"+std::to_string(fade));},[]{return false;},[](bool){throw std::logic_error("Unexpected tutorial branch");},
   [&]{s.events.push_back("abort");return abort;},[&]{s.events.push_back("stop");},[&]{s.events.push_back("swap");},[&]{s.events.push_back("finished");return finish;});
  const std::vector<std::string> prefix{"base","start","sync","volume:0:0","volume:127:500"};auto expected=prefix;
  if(e3)expected={"base","phase"};else if(!admit)expected.insert(expected.end(),{"stop","phase"});else if(abort)expected.insert(expected.end(),{"abort","stop","phase"});else{expected.insert(expected.end(),{"abort","swap","finished"});if(finish)expected.insert(expected.end(),{"stop","phase"});}
  Check(s.events==expected,"Independent MoviePlayer order differs");Check(s.mMovieStarted==(!e3&&admit&&!abort),"Original started state differs after early or natural completion");
 }
}
const FrontendInstance& Instance(const FrontendSession::Handle& f,std::string_view name)
{for(const auto& i:f->graph.instances)if(i.name==name)return i;throw std::logic_error("Missing expected text instance");}
void Pads(FrontendInput& input,unsigned buttons=0){std::array<FrontendPadSample,4> p{};p[0].connected=true;p[0].buttons=buttons;input.Update(p,0);}
void PlaybackRuntime(bool owned,const std::string& mode)
{
 FrontendInput input;Pads(input);auto audio=Audio(owned);unsigned seed=812;
 const bool empty=mode=="empty";auto session=Session();std::vector<FrontendCreditsCommand> commands;
 FrontendCredits credits(session,input,audio,seed,[&](auto c){commands.push_back(c);});
 credits.Update(credits.Current(),3);credits.Update(credits.Current(),0);const auto blocked=credits.Current();
 Check(credits.Status().phase==1&&!credits.MovieTarget(),"Credits source prefix/request differs");
 Reject([&]{credits.ServiceMovie(1);});Check(credits.Current()==blocked,"Missing provider changed retained frame");
 unsigned calls=0;bool bad=true;std::shared_ptr<FrontendMoviePlayback> actual;
 credits.SetMovieProvider([&](const auto& request,const auto& options,std::uint64_t retrace)
 {
  ++calls;Reject([&]{credits.Release();});
  auto candidate=request;if(bad)++candidate.generation;
  actual=std::make_shared<FrontendMoviePlayback>(candidate,options,retrace);return actual;
 },{});
 Reject([&]{credits.SetMovieProvider({},{});});Reject([&]{credits.ServiceMovie(10);});
 Check(credits.Current()==blocked&&credits.Status().phase==1&&!credits.MovieTarget(),"Mismatched provider changed source phase");actual.reset();bad=false;credits.ServiceMovie(10);
 Check(calls==2&&actual&&credits.MovieTarget()->playback==actual&&!actual->Current()&&!credits.Status().movie_started,"Idle preparation started audio or source phase early");
 const auto first=actual;
 if(!owned)
 {
  auto graph=session->Current()->graph;auto previous=graph;auto text=std::find_if(graph.instances.begin(),graph.instances.end(),[](const auto& x){return x.type==3;});
  FrontendInstanceChange c;c.instance=text->offset;c.property=FrontendInstanceProperty::TextBox;c.text_box={-1,20};Reject([&]{ApplyFrontendInstanceChanges(graph,std::span(&c,1));});
  Check(graph.instances.at(std::size_t(text-graph.instances.begin())).text_box==previous.instances.at(std::size_t(text-graph.instances.begin())).text_box,"Malformed text box changed clone");
  c.instance=graph.instances.front().offset;c.property=FrontendInstanceProperty::TextDrawOptions;c.draw_options=0;Reject([&]{ApplyFrontendInstanceChanges(graph,std::span(&c,1));});
 }
 const auto before=credits.Current();Reject([&]{credits.AttachMovieBinding({});});Reject([&]{credits.RetireMovieBinding({});});
 std::thread foreign([&]{Reject([&]{credits.ServiceMovie(11);});});foreign.join();Check(credits.Current()==before,"Foreign service changed source frame");
 // Genuine source Accept before first texture swap stops/drains actual123. No
 // test-only fake binding or CompleteMovie flag is available to advance phase.
 Pads(input);Pads(input,0x100);
 if(mode!="normal"&&!empty)
 {
  Reject([&]{credits.Update(credits.Current(),0);});Check(credits.Status().failed&&Active(session->Current())=="NLG 4:3","Failed parser published scrolling setup");credits.Release();audio->Unload();return;
 }
 credits.Update(credits.Current(),0);Check(first->Status().state==FrontendMovieState::Cancelled&&!first->Completion()&&credits.Status().phase==2&&Active(credits.Current())=="Credits 4:3","Source NLG abort did not stop its actual owner and prepare credits");
 const auto scrolling=credits.Current();auto tokens=ReadCreditsText(Load("/credits.txt"));
 Check(credits.Status().parser_tokens==tokens->tokens.size()&&credits.Status().parser_position==0,"Actual parser ownership/count differs");
 // Inspect selected slide lines rather than duplicate names in unselectedwide.
 for(unsigned n=0;n<20;++n)
 {
  const auto name="line"+std::to_string(n+1);const std::array<std::string_view,2> names{"Layer",name};auto node=FindFrontendNode(scrolling->graph,{FrontendNodeKind::Slide,*scrolling->graph.active_slide},FrontendNamedPath(names),FrontendNodeType::Text);Check(bool(node),"Credits line missing");
  auto it=std::find_if(scrolling->graph.instances.begin(),scrolling->graph.instances.end(),[&](const auto& i){return i.offset==node->id;});
  Check(it->attributes.scale==std::array<float,3>{.75f,.75f,1}&&it->attributes.position[1]==-250.f-float(n)*25.f&&(it->text_overload_flags&0x14)==0x14&&(it->draw_options&0x10)&&!(it->draw_options&0x1000)&&it->text_box==std::array<float,2>{1280,480},"Actual20-line source scale/box/options/position differs");
 }
 if(!empty){credits.DisplayFinalMessage(credits.Current());Check(credits.Status().final_message_displayed,"Source final-message flag absent");}
 credits.ServiceMovie(12);const auto second=actual;Check(second!=first&&credits.MovieTarget()->request.generation==2&&!second->Current(),"Second movie request reused old playback or advanced too early");
 Reject([&]{credits.ServiceMovie(11);});Pads(input);Pads(input,0x100);credits.Update(credits.Current(),empty?1.7f:.1f);
 Check(second->Status().state==FrontendMovieState::Cancelled&&credits.Status().phase==3&&Active(credits.Current())=="COPYRIGHTS"&&(empty?credits.Status().parser_position==0:credits.Status().parser_position>=1),"Credits abort/scroll-after-movie source ordering differs");
 Check(empty?credits.Status().lines_on_screen==0:credits.Status().lines_on_screen>=1,"Phase2 source scrolling was incorrectly skipped after movie callback");
 if(empty)Check(credits.Status().credits_over&&credits.Status().default_fade&&credits.Status().fade_started,"Real empty parser/default-fade Update0/1.7 endpoint differs");
 // Source parser/storage remains until its fade branch or scene destruction;
 // natural/abort movie callback alone does not free it.
 Check(credits.Status().parser_tokens==tokens->tokens.size(),"Movie callback incorrectly freed source parser");
 Check(credits.Status().fade_started,"Source Accept was not observed by the remaining scroll branch after movie abort");
 auto returned=credits.Current();Pads(input);credits.Update(returned,empty?1.299f:2.999f);Check(credits.Status().phase==3,"Copyright advanced before3 despite its already-started fade");credits.Update(credits.Current(),.0011f);
 Check(credits.Status().phase==4&&commands.size()>=3,"Copyright did not request replacement");
 const auto end=commands.size();Check(commands[end-3].kind==FrontendCreditsCommandKind::ReplaceScene&&commands[end-3].argument==13&&commands[end-2].kind==FrontendCreditsCommandKind::SelectMusic&&commands[end-2].argument==1&&commands[end-1].kind==FrontendCreditsCommandKind::StadiumRendering&&commands[end-1].argument==1,"Original return13/music1/stadium order differs");
 Reject([&]{credits.Update(credits.Current(),0);});credits.Release();Check(commands[commands.size()-2].kind==FrontendCreditsCommandKind::PointerEnabled&&commands.back().kind==FrontendCreditsCommandKind::StadiumRendering&&audio->Handles().empty(),"Credits release did not restore actual service state/drain cues");audio->Unload();Check(scrolling->visuals&&returned->images,"Retained frames did not survive cleanup");
}
void AllocationFailures()
{
 FrontendInput input;Pads(input);auto audio=Audio(false);unsigned seed=19,failures=0;
 for(long budget:{0L,8L,40L,180L,500L})
 {
  auto session=Session();std::vector<FrontendCreditsCommand> commands;commands.reserve(32);
  FrontendCredits credits(session,input,audio,seed,[&](auto c){commands.push_back(c);});
  credits.Update(credits.Current(),3);credits.Update(credits.Current(),0);std::shared_ptr<FrontendMoviePlayback> movie;
  credits.SetMovieProvider([&](auto request,auto options,auto retrace){movie=std::make_shared<FrontendMoviePlayback>(request,options,retrace);return movie;},{});credits.ServiceMovie(0);
  Pads(input);Pads(input,0x100);bool failed=false;
  try{auto before=credits.Current();allocation_budget=budget;credits.Update(before,0);allocation_budget=-1;}
  catch(const std::bad_alloc&){allocation_budget=-1;failed=true;++failures;}
  allocation_budget=-1;if(failed)Check(Active(session->Current())=="NLG 4:3","Failed cloned scrolling setup leaked partial publication");
  credits.Release();Check(movie->Status().state==FrontendMovieState::Cancelled&&!nlAsyncReadsPending(nullptr)&&audio->Handles().empty(),"Failed source update leaked movie/read/cue ownership");
 }
 Check(failures>=3,"Credits source allocation failure coverage was not reached");
 {
  auto session=Session();bool fail=false;std::shared_ptr<FrontendMoviePlayback> movie;
  FrontendCredits credits(session,input,audio,seed,[&](auto command){if(fail&&command.kind==FrontendCreditsCommandKind::StadiumRendering&&!command.argument){fail=false;throw std::runtime_error("Actual service failed after admitted MovieStop");}});
  credits.Update(credits.Current(),3);credits.Update(credits.Current(),0);
  credits.SetMovieProvider([&](auto request,auto options,auto retrace){movie=std::make_shared<FrontendMoviePlayback>(request,options,retrace);return movie;},{});credits.ServiceMovie(0);
  Pads(input);Pads(input,0x100);fail=true;Reject([&]{credits.Update(credits.Current(),0);});
  Check(credits.Status().failed&&movie->Status().state==FrontendMovieState::Cancelled&&Active(session->Current())=="NLG 4:3","Admitted Stop/service failure replayed or published partial scrolling scene");
  Reject([&]{credits.Update(session->Current(),0);});credits.Release();Check(audio->Handles().empty()&&!nlAsyncReadsPending(nullptr),"Failed service cleanup leaked retained movie/cue/read");
 }
 audio->Unload();
}

}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated|owned|missing|malformed");Kernels();MovieOrder();const std::string mode=argv[3];const bool owned=mode=="owned";const auto output=std::filesystem::absolute(argv[2]);const auto folder=(output/"credits-playback-host").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged source Credits playback phases";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora init failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Credits disc absent");host.disc=true;nlInitFileSystem();
  const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();PlaybackRuntime(owned,(owned||mode=="generated")?"normal":mode);if(mode=="generated")AllocationFailures();Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Credits playback resources did not recover native arenas");
  std::cout<<checks<<" Credits playback/source-phase checks passed\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}

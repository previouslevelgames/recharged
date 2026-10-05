#include "runtime/frontend_credits.h"
#include "resources/credits_text.h"
#include "Game/FE/FrontendCreditsSteps.h"
#include "runtime/frontend_handler.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "audio_bank_fixture.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <new>
#include <source_location>
#include <thread>
thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,std::size_t)noexcept{std::free(p);}void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location p=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid visual options accepted at "+std::to_string(p.line()));}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.001f,"Independent coordinate or setting differs");}
std::vector<std::uint8_t> Load(const char* path){std::unique_ptr<nlFile> f(nlOpen(path));Check(bool(f),"Owned NL input absent");auto n=nlFileSize(f.get(),nullptr);std::vector<std::uint8_t> b(n);nlRead(f.get(),b.data(),n,n);return b;}
using namespace audio_bank_fixture;
std::shared_ptr<FrontendAudio> Audio(bool owned,unsigned device=0)
{
 if(owned)
 {
  auto global=Load("/audio/nlxgs.bun");auto catalog=ReadAudioBankCatalog(global);AudioBankLoad owner(catalog,23,21);
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(owner.State()==AudioBankLoadState::Loading){owner.Service();Check(std::chrono::steady_clock::now()<end,"NAV sound bank timed out");SDL_Delay(1);}
  return std::make_shared<FrontendAudio>(owner.Result(),ReadAudioCalculationInitial(global),[&]{AudioVoicesOptions options;options.capacity=32;options.device_id=device;return options;}());
 }
 auto f=Make();const auto word=[](const Data& d,std::size_t at){return (std::uint32_t(d.at(at))<<24)|(std::uint32_t(d.at(at+1))<<16)|(std::uint32_t(d.at(at+2))<<8)|d.at(at+3);};
 const auto chunks=[&](const Data& data){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<data.size();){const auto id=word(data,at),n=word(data,at+4);out.emplace_back(id,Data(data.begin()+at+8,data.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
 const std::array keys{0xf394c076u,0xbb142b94u};Data root;
 for(auto [id,data]:chunks(Data(f.bytes.begin()+8,f.bytes.end())))
 {
  if(id==0x80023000)
  {
   Data map,records;Append(map,0x23001,Words({unsigned(keys.size()),0,0}));for(unsigned i=0;i<keys.size();++i){auto b=Words({keys[i],0,0,0,i});records.insert(records.end(),b.begin(),b.end());}Append(map,0x23003,records);data=std::move(map);
  }
  if(id==0x80023300)
  {
   Data graph;bool refs=false;
   for(auto [kind,part]:chunks(data))
   {
    if(kind==0x23301)Put(part,8,unsigned(keys.size()));
    if(kind==0x23302){Data rows;for(auto key:keys){auto row=Data(part.begin()+40,part.end());Put(row,0,key);rows.insert(rows.end(),row.begin(),row.end());}part=std::move(rows);}
    if(kind==0x23308){if(!refs){for(unsigned i=0;i<keys.size();++i)Append(graph,kind,Words({0xf0001000,0,Float(255),0,0}));refs=true;}continue;}
    Append(graph,kind,part);
   }
   data=std::move(graph);
  }
  Append(root,id,data);
 }
 auto bank=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0,1,false},ReadAudioResidentBank(Wrap(0x80000001,root),f.wave)});
 Data calc;Append(calc,0x23401,Words({2,0xf1000100,0}));Append(calc,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));
 return std::make_shared<FrontendAudio>(bank,ReadAudioCalculationInitial(Wrap(0x80000001,Wrap(0x80023400,calc))),[&]{AudioVoicesOptions options;options.capacity=32;options.device_id=device;return options;}());
}

struct Kernel
{
 struct Vector2{float x,y;};
 struct Position{struct{float x,y,z;}f;};
 struct Text
 {
  Position position{{40,0,0}};std::array<float,3> scale{};std::u16string text;
  unsigned m_OverloadFlags=0,m_DrawOptions=0x1000;bool m_bVisible=true;
  struct{Vector2 BoxSize{};}m_OverloadedAttributes;
  void SetAssetScale(float x,float y,float z){scale={x,y,z};}
  Position GetAssetPosition()const{return position;}
  void SetAssetPosition(float x,float y,float z){position={{x,y,z}};}
  void SetString(const std::array<char16_t,64>& p){text=p.data();}
 } final,lines[20];
 struct Slide{unsigned updates=0;void Update(float dt){Check(dt==0,"Kernel setup changed source update0");++updates;}}slide;
 struct Presentation
 {
  Slide* m_currentSlide;std::string selected;unsigned updates=0;
  void SetActiveSlide(const char* name,bool reset){Check(reset,"Source presentation reset differs");selected=name;}
  void Update(float dt){Check(dt==0,"Kernel presentation changed source update0");++updates;}
 }presentation{&slide};Presentation* mPresentation=&presentation;
 struct Fade
 {
  unsigned selections=0,updates=0;
  void SetActiveSlide(const char* name,bool reset,bool preserve){Check(std::string_view(name)=="FADEIN"&&reset&&!preserve,"Source fade selector differs");++selections;}
  void Update(float dt){Check(dt==0,"Source credit fade changed update0");++updates;}
 }fade;
 struct Parser
 {
  void* mFileData=nullptr;unsigned loads=0,advances=0;
  struct Tokens
  {
   std::vector<std::string> values;std::size_t next=0;
   const char* NextToken(bool lower){Check(!lower,"Source credits changed token case");return next<values.size()?values[next].c_str():nullptr;}
   void AdvanceLine(){if(next<values.size())++next;}
  }mParser;
  void Load(){++loads;mFileData=this;}
 }mCreditParser;
 std::array<Text*,20>m_pTextLines{};
 std::array<bool,20>mLineOnScreen{},mCenteredLine{};
 std::array<std::array<char16_t,64>,20>mStrings{};
 bool mAreCreditsOver=false,mFinalMessageDisplayed=false,mFadeStarted=false;
 float mTimeElapsed=0;unsigned mPhase=0,mNextScene=13;bool wide=false;unsigned video=0;
 std::string movie;std::vector<std::string> trace;
 Fade* GetWhiteFadeComponent(){return &fade;}
 void SetMovieDetails(const char* path,bool sound,bool loop){Check(sound&&!loop,"Original movie details flags differ");movie=path;trace.push_back("movie:"+movie);}
 void SetupForCredits()
 {
  FrontendCreditsSetupScrolling<Vector2>(*this,[&]{return wide;},[&]{return video;},[&]{return mPresentation;},[&](auto*){return &final;},
   [&](auto*,const char* layer,const char* name){Check(std::string_view(layer)=="Layer","Credits source layer differs");const auto n=std::stoi(std::string(name).substr(4));Check(n>=1&&n<=20,"Credits source line is invalid");return &lines[n-1];},
   [](char* name,unsigned size,int n){std::snprintf(name,size,"line%d",n);});
 }
 void SetupForPhase()
 {
  FrontendCreditsSetupPhase(*this,[&]{return wide;},[&]{return video;},[&](unsigned cue,int a,int b,int c){Check(!a&&!b&&c==1,"Source cue arguments differ");trace.push_back("cue:"+std::to_string(cue));},
   [&](bool enabled){trace.push_back(enabled?"stadium1":"stadium0");},
   [&](unsigned scene,int screen,bool pop){Check(!screen&&pop,"Credits return must replace");trace.push_back("replace:"+std::to_string(scene));},
   [&](unsigned music){trace.push_back("music:"+std::to_string(music));});
 }
 void Scroll(float dt,bool press=false)
 {
  FrontendCreditsUpdateScrolling(*this,dt,[&](float delta){Check(delta==dt,"Source movie base delta changed");trace.push_back("movieupdate");},
   [&](int action){Check(action==30||action==32,"Source credits input action differs");return press;},
   [&](void* p){Check(p==&mCreditParser,"Source parser freed foreign storage");trace.push_back("free");},[&]{trace.push_back("stopmovie");});
 }
};
void Kernels()
{
 // Independent text input; original parser is selected, not a port token splitter.
 const std::string input="#comment\nALPHA BETA\tunused\n+\n@\nCENTER\nLAST\n";
 auto text=ReadCreditsText(Bytes(reinterpret_cast<const std::uint8_t*>(input.data()),input.size()));
 Check(text->tokens==std::vector<std::string>({"ALPHA BETA","+","@","CENTER","LAST"}),"Original parser token/AdvanceLine order differs");
 Reject([]{std::string s(256,'X');ReadCreditsText(Bytes(reinterpret_cast<const std::uint8_t*>(s.data()),s.size()));});
 const std::array<std::uint8_t,3> nul{'A',0,'B'};Reject([&]{ReadCreditsText(nul);});
 Check(ReadCreditsText({})->tokens.empty(),"Empty bounded source parser produced tokens");
 const std::array<std::uint8_t,3> trailing{'A','\n','B'};Check(ReadCreditsText(trailing)->tokens==std::vector<std::string>{"A"},"Source ignored one-byte final line changed");
 const std::array<std::uint8_t,2> pair{'A','B'};Check(ReadCreditsText(pair)->tokens==std::vector<std::string>{"AB"},"Source unterminated two-byte line changed");
 Kernel k;FrontendCreditsCopyLine(k,0,"+anything");Check(k.mStrings[0][0]==' '&&k.mStrings[0][1]==0,"Original plus marker differs");
 std::string long_line(255,'Z');FrontendCreditsCopyLine(k,0,long_line.c_str());Check(k.mStrings[0][62]=='Z'&&k.mStrings[0][63]==0,"Original63-unit text cap differs");
 for(bool wide:{false,true})for(unsigned mode:{0u,1u,2u})
 {
  Kernel p;p.wide=wide;p.video=mode;p.SetupForPhase();Check(p.presentation.selected=="NINTENDO"&&p.slide.updates==1&&p.trace==std::vector<std::string>{"cue:4086612086","stadium0"},"Original Nintendo setup order differs");
  FrontendCreditsUpdateNintendoLogo(p,2.999f);Check(!p.mFadeStarted&&p.mPhase==0,"Nintendo faded before3 seconds");
  FrontendCreditsUpdateNintendoLogo(p,.0011f);Check(p.mFadeStarted&&p.mPhase==0&&p.fade.selections==1,"Nintendo endpoint must start fade only");
  FrontendCreditsUpdateNintendoLogo(p,0);Check(p.mPhase==1&&p.mTimeElapsed==0&&!p.mFadeStarted&&p.presentation.selected==(wide?"NLG":"NLG 4:3")&&p.movie==(mode==1?"art/movies/nlgintro_pal.thp":"art/movies/nlgintrowide.thp"),"Nintendo next-update movie selection differs");
  p.mPhase=3;p.SetupForPhase();FrontendCreditsUpdateCopyrightMessage(p,3);Check(p.mPhase==3&&p.mFadeStarted,"Copyright advanced on its fade-start update");
  p.trace.clear();FrontendCreditsUpdateCopyrightMessage(p,0);Check(p.mPhase==4&&p.trace==std::vector<std::string>{"cue:3138661268","replace:13","music:1","stadium1"},"Copyright source return order differs");
  p.mNextScene=1;p.SetupForPhase();Check(p.mNextScene==13&&p.trace[p.trace.size()-2]=="music:0","Alternate return did not reset source nextscene");
 }
 Kernel s;s.mPhase=2;s.mCreditParser.mParser.values={"FIRST","+","@","CENTER","LAST"};s.SetupForPhase();
 Check(s.movie=="art/movies/credits.thp"&&s.presentation.selected=="Credits 4:3"&&!s.final.m_bVisible&&s.mCreditParser.loads==1,"Source scrolling setup differs");
 for(unsigned i=0;i<20;++i){Check(s.lines[i].position.f.y==-250.f-i*25&&s.lines[i].scale==std::array<float,3>{.75f,.75f,1}&&s.lines[i].m_OverloadFlags==0x14&&s.lines[i].m_DrawOptions==0x10,"Source text placement/options differ");Check(s.lines[i].m_OverloadedAttributes.BoxSize.x==1280&&s.lines[i].m_OverloadedAttributes.BoxSize.y==480,"Source text box differs");}
 // Isolate each real scrolling branch against independent positions/tokens.
 s.Scroll(0);Check(s.lines[0].text==u"FIRST"&&s.mLineOnScreen[0]&&s.lines[0].position.f.y==-250,"First source line admission differs");
 s.Scroll(8.5f);Check(s.lines[0].position.f.y==250&&s.mLineOnScreen[0],"Scrolling500*(dt/8.5) differs");
 s.Scroll(0);Check(s.lines[0].position.f.y==-250&&!s.mLineOnScreen[0]&&s.lines[1].text==u" "&&s.lines[2].text==u"CENTER"&&s.lines[2].position.f.x==-40&&s.lines[2].m_DrawOptions==0,"Original recycle/plus/center branches differ");
 Check(s.mCenteredLine[0]&&s.mCenteredLine[1]&&!s.mCenteredLine[2]&&!s.mCenteredLine[3],"Original @ mode did not propagate/consume per-line flags");
 s.Scroll(0,true);Check(s.mFadeStarted&&s.fade.updates==1&&s.mPhase==2,"Source user exit must start fade before phase change");
 s.trace.clear();s.Scroll(0);Check(s.mPhase==3&&s.mCreditParser.mFileData==nullptr&&s.trace==std::vector<std::string>{"movieupdate","free","stadium0","stopmovie"},"Original free/setup/stop order differs");
 Kernel end;end.mPhase=2;end.SetupForPhase();end.Scroll(1.699f);Check(!end.mFadeStarted&&end.mAreCreditsOver,"Empty credits clock advanced early");end.Scroll(.002f);Check(end.mFadeStarted,"Empty credits did not fade at1.7");
 // Completion kernel only: no decoded EOF or test observation is accepted as
 // a real native owner playback receipt.
 Kernel receipt;receipt.mPhase=0;FrontendCreditsMovieCompleted(receipt);Check(receipt.mPhase==0,"Movie completion changed unrelated phase");receipt.mPhase=1;FrontendCreditsMovieCompleted(receipt);Check(receipt.mPhase==2,"Source movie callback did not enter credits");FrontendCreditsMovieCompleted(receipt);Check(receipt.mPhase==3,"Source movie callback did not enter copyright");
}

std::shared_ptr<FrontendSession> Session(const char* path="/Art/fe/credits.fen")
{
 auto s=std::make_shared<FrontendSession>();s->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"NINTENDO",true});
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(s->State()==FrontendSessionState::Loading){s->Service();Check(std::chrono::steady_clock::now()<deadline,"Credits NL load timed out");SDL_Delay(1);}s->Result();return s;
}
std::string Active(const FrontendSession::Handle& f)
{for(const auto& p:f->graph.slides)if(p.offset==f->graph.active_slide)return p.name;throw std::logic_error("No presentation");}
std::string Fade(const FrontendSession::Handle& f)
{
 const std::array<std::string_view,2> names{"Layer","WHITE FADE"};auto node=FindFrontendNode(f->graph,{FrontendNodeKind::Slide,*f->graph.active_slide},FrontendNamedPath(names));Check(bool(node),"Actual whitefade absent");
 const FrontendInstance* i=nullptr;for(const auto& n:f->graph.instances)if(n.offset==node->id)i=&n;Check(i&&i->type==4&&i->library,"Actual whitefade is not component");
 for(const auto& l:f->graph.library)if(l.offset==*i->library)for(const auto& s:f->graph.slides)if(s.offset==l.active_slide)return s.name;throw std::logic_error("Fade active slide absent");
}
struct Controls
{
 bool pointer=true,stadium=true;unsigned music_stops=0;std::vector<FrontendCreditsCommand> commands;
 int fail=-1;FrontendCredits* owner=nullptr;bool try_reentry=false;
 void Apply(FrontendCreditsCommand c)
 {
  if(fail==0){fail=-1;throw std::runtime_error("Explicit host-service failure");}if(fail>0)--fail;
  commands.push_back(c);if(try_reentry&&owner)Reject([&]{owner->Release();});
  switch(c.kind){case FrontendCreditsCommandKind::PointerEnabled:pointer=c.argument;break;case FrontendCreditsCommandKind::StadiumRendering:stadium=c.argument;break;case FrontendCreditsCommandKind::StopMusic:++music_stops;break;default:throw std::logic_error("No movie-return provider installed");}
 }
};
void Runtime(bool owned,const std::filesystem::path& output)
{
 auto input=std::make_unique<FrontendInput>();std::array<FrontendPadSample,4> pads{};pads[0].connected=true;input->Update(pads,0);auto audio=Audio(owned);unsigned seed=55;
 for(bool wide:{false,true})
 {
  auto session=Session();Controls controls;FrontendCredits credits(session,*input,audio,seed,[&](auto c){controls.Apply(c);},wide,0);controls.owner=&credits;controls.try_reentry=true;
  auto retained=credits.Current();bool foreign_rejected=false;std::thread foreign([&]{try{credits.Update(retained,0);}catch(const std::exception&){foreign_rejected=true;}});foreign.join();Check(foreign_rejected&&credits.Current()==retained,"Foreign thread changed Credits ownership");Check(Active(retained)=="NINTENDO"&&!controls.pointer&&!controls.stadium&&controls.music_stops==1&&audio->ActiveCount(0xf394c076)==1,"Actual Credits initialization services differ");
  Check(controls.commands.size()==3&&controls.commands[0].kind==FrontendCreditsCommandKind::PointerEnabled&&controls.commands[1].kind==FrontendCreditsCommandKind::StadiumRendering&&controls.commands[2].kind==FrontendCreditsCommandKind::StopMusic,"Original constructor/SceneCreated order differs");
  Reject([&]{credits.Update(retained,-1);});Check(credits.Current()==retained,"Invalid delta changed source frame");
  credits.Update(retained,3);Check(credits.Status().phase==0&&credits.Status().fade_started&&(owned?credits.Status().default_fade:Fade(credits.Current())=="FADEIN"),"Real authored Nintendo fade was not selected at3");
  auto before=credits.Current();Reject([&]{credits.Update(retained,0);});credits.Update(before,0);auto status=credits.Status();
  Check(status.phase==1&&status.elapsed==0&&!status.fade_started&&status.boundary==FrontendCreditsBoundary::MoviePlayback&&status.movie&&status.movie->path=="art/movies/nlgintrowide.thp"&&status.movie->details_with_sound&&!status.movie->start_with_sound&&!status.movie->loop&&status.movie->synced&&Active(credits.Current())==(wide?"NLG":"NLG 4:3"),"Real source transition movie boundary differs");
  before=credits.Current();const auto old_seed=seed;Reject([&]{credits.Update(before,.1f);});Check(credits.Current()==before&&seed==old_seed&&!credits.Status().failed,"Missing movie provider consumed time/RNG or fabricated completion");
  credits.Release();Check(controls.pointer&&controls.stadium&&audio->Handles().empty()&&retained->images,"Credits release failed restoration/audio/retention");Reject([&]{credits.Current();});
 }
 // Concrete failing host service after real cue admission: no candidate is
 // published, original pointer/stadium restoration still executes and cues drain.
 {
  auto s=Session();auto old=s->Current();Controls c;c.fail=2;const auto old_seed=seed;
  Reject([&]{FrontendCredits failed(s,*input,audio,seed,[&](auto command){c.Apply(command);});});
  Check(c.pointer&&c.stadium&&s->Current()==old&&audio->Handles().empty(),"Failed Created request leaked source effects/retained frame");
  (void)old_seed; // An admitted source cue may legitimately have consumed RNG.
 }
 if(!owned)
 {
  auto reusable=Session();const auto original=reusable->Current();unsigned failures=0;
  for(long budget:{0L,1L,3L,10L,50L,150L})
  {
   Controls c;c.commands.reserve(16);bool failed=false;
   try{allocation_budget=budget;FrontendCredits attempted(reusable,*input,audio,seed,[&](auto command){c.Apply(command);});allocation_budget=-1;attempted.Release();}
   catch(const std::bad_alloc&){allocation_budget=-1;failed=true;++failures;}
   allocation_budget=-1;Check(failed&&c.pointer&&c.stadium&&reusable->Current()==original&&audio->Handles().empty(),"Credits partial allocation failure published or leaked effects");
  }
  Check(failures==6,"Credits constructor allocation gates were not reached");
  auto s=Session("/Art/fe/credits-missing.fen");Controls c;FrontendCredits credits(s,*input,audio,seed,[&](auto command){c.Apply(command);});
  credits.Update(credits.Current(),3);Check(credits.Status().default_fade&&credits.Status().fade_started,"Original missing fade did not select the empty default");credits.Update(credits.Current(),0);Check(credits.Status().phase==1,"Empty default changed source transition timing");credits.Release();Check(c.pointer&&c.stadium&&audio->Handles().empty(),"Default-fade cleanup failed");
 }
 auto raw=Load("/credits.txt");auto tokens=ReadCreditsText(raw);Check(!tokens->tokens.empty(),"Actual credits parser produced no tokens");
 if(owned){Check(tokens->tokens.size()>100,"Owned credits token inventory is unexpectedly small");std::ofstream raw_out(output/"credits-input.bin",std::ios::binary);raw_out.write(reinterpret_cast<const char*>(raw.data()),raw.size());std::ofstream f(output/"credits-tokens.txt");for(const auto& line:tokens->tokens)f<<line<<'\n';}
 audio->Unload();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");Kernels();const bool owned=std::string_view(argv[3])=="owned";const auto output=std::filesystem::absolute(argv[2]);const auto folder=(output/"credits-host").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged original Credits";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora init failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Credits disc absent");host.disc=true;nlInitFileSystem();
  const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Runtime(owned,output);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Credits resources did not recover native arenas");
  std::cout<<checks<<" Credits source/lifecycle checks passed\n";
  return 0;
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}

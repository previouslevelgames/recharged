#include "runtime/frontend_audio.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid FE audio load succeeded");}
struct Session
{
 bool live=false,disc=false;
 ~Session(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}
};
std::vector<std::uint8_t> Read(const char* path)
{
 std::unique_ptr<nlFile> file(nlOpen(path));Check(bool(file),"Missing frontend audio global profile");const auto size=nlFileSize(file.get(),nullptr);
 Check(size&&size<=resources::MaximumAssetBytes,"Frontend audio global profile exceeds limit");std::vector<std::uint8_t> bytes(size);nlRead(file.get(),bytes.data(),size,size);return bytes;
}
void Pump(AudioBankLoad& load)
{
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(load.State()==AudioBankLoadState::Loading){load.Service();Check(std::chrono::steady_clock::now()<end,"FE bank load timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc, evidence directory and mode");const std::string_view mode=argv[3];Check(mode=="success"||mode=="owned"||mode=="missing"||mode=="malformed"||mode=="cancel","Unknown frontend audio load mode");
  auto folder=(std::filesystem::path(argv[2])/"frontend-audio-load-data").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged frontend resident cues";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;
  config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
  Session session;auto host=aurora_initialize(argc,argv,&config);session.live=true;Check(host.window,"Aurora host failed");InitializeStartupOS();nlInitMemory();
  Check(aurora_dvd_open(argv[1]),"Cannot mount FE audio disc");session.disc=true;nlInitFileSystem();
  resources::AudioBankCatalog::Handle catalog;resources::AudioCalculationInitial::Handle calculation;
  if(mode=="owned"){auto global=Read("audio/nlxgs.bun");catalog=resources::ReadAudioBankCatalog(global);calculation=resources::ReadAudioCalculationInitial(global);}
  else
  {
   auto c=std::make_shared<resources::AudioBankCatalog>();c->names.resize(24);c->slots.resize(22);c->names[23]={23,"FE_GEN_Sfx"};c->slots[21]={21,0x1234,1,false};catalog=c;calculation=resources::ReadAudioCalculationInitial(Read("audio/calculation.bun"));
  }
  LoadedAudioBank::Handle retained;
  for(unsigned attempt=0;attempt<3;++attempt)
  {
   const auto mem1=StandardAllocator.TotalFreeMemory(),mem2=VirtualAllocator.TotalFreeMemory();
   if(mode=="missing")Reject([&]{AudioBankLoad load(catalog,23,21);});
   else
   {
    AudioBankLoad load(catalog,23,21);Check(load.Progress().requested_reads==1,"FE wave read preceded metadata");
    if(mode=="cancel")
    {
     if(attempt){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(load.Progress().requested_reads==1){load.Service();Check(load.State()==AudioBankLoadState::Loading&&std::chrono::steady_clock::now()<end,"FE metadata did not reach cancellation point");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
     load.Cancel();Check(load.State()==AudioBankLoadState::Cancelled,"Cancelled FE bank was published");Reject([&]{load.Result();});
    }
    else
    {
     Pump(load);
     if(mode=="malformed"){Check(load.State()==AudioBankLoadState::Failed,"Malformed FE bank became ready");Reject([&]{load.Result();});}
     else
     {
      retained=load.Result();Check(retained->name_index==23&&retained->slot_index==21&&load.Progress().completed_reads==2,"Real FE bank identity/read accounting differs");
      FrontendAudio audio(retained,calculation);unsigned seed=23;const unsigned key=mode=="owned"?0x6b0689d4:0x10203040;
      auto handle=*audio.Play(key,seed);audio.Update(0);audio.ServiceAudio();audio.Update(0);
      Check(audio.Status(handle).state==4&&audio.ActiveCount(key)==1,"Loaded FE cue did not start real output");audio.Unload();Check(audio.IsFinished(handle)&&!audio.Loaded(),"Loaded FE cue did not cancel/unload");
     }
    }
   }
   Check(!nlAsyncReadsPending(nullptr),"FE audio kept native reads after terminal load");Check(StandardAllocator.TotalFreeMemory()==mem1&&VirtualAllocator.TotalFreeMemory()==mem2,"FE audio failed to recover both game arenas");
  }
  nlShutdownFileSystem();ResetStartupMemory();
  if(retained)
  {
   FrontendAudio audio(retained,calculation);unsigned seed=23;auto h=*audio.Play(mode=="owned"?0x0a93e9a0:0x10203040,seed);audio.Update(0);audio.ServiceAudio();audio.Cancel(h);audio.Unload();
   std::cout<<"Retained FE_GEN_Sfx name23/slot21 survives NL and arena teardown: "<<retained->bank->Samples().size()<<" samples, "<<retained->bank->WaveBytes()<<" wave bytes\n";
  }
  std::cout<<checks<<" NL frontend audio checks passed; three arena recoveries\n";
 }
 catch(std::exception&e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}

#include "runtime/native_preferences.h"
#include "Game/DB/SaveStateSteps.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>
#include <thread>
#include <vector>

thread_local long allocation_budget=-1;
void* operator new(std::size_t n)
{
    if(allocation_budget==0)throw std::bad_alloc();
    if(allocation_budget>0)--allocation_budget;
    if(auto* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,std::size_t)noexcept{std::free(p);}void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool value,const char* what){++checks;if(!value)throw std::runtime_error(what);}
template<class F>void Reject(F f,std::source_location where=std::source_location::current())
{
    ++checks;try{f();}catch(const std::exception&){return;}
    throw std::runtime_error("Invalid preferences operation accepted at "+std::to_string(where.line()));
}
using Data=std::vector<std::uint8_t>;
Data Read(const std::filesystem::path& path)
{
    std::ifstream in(path,std::ios::binary);Check(bool(in),"Missing test output");return {std::istreambuf_iterator<char>(in),{}};
}
void Write(const std::filesystem::path& path,std::span<const std::uint8_t> b)
{
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());Check(bool(out),"Fixture write failed");
}
void Pump(NativePreferences& p)
{
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(p.Status().host_pending){p.Poll();Check(std::chrono::steady_clock::now()<until,"Preferences worker timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
void ReachFile(NativePreferences& p)
{
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(p.Status().state==NativePreferencesState::DirectoryPending){p.Poll();Check(std::chrono::steady_clock::now()<until,"Directory callback timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    Check(p.Status().state==NativePreferencesState::FilePending,"Missing file callback stage");
}
void EmptyTemps(const std::filesystem::path& dir)
{
    for(const auto& entry:std::filesystem::directory_iterator(dir))Check(entry.path().filename().string().find(".pending-")==std::string::npos,"Temporary file leaked");
}
void SourceKernels()
{
    bool paused=true,enabled=true,busy=false;unsigned mode=99;
    unsigned queried=0;auto pending=[&]{++queried;return false;};
    Check(!SaveStateCanStart(false,pending)&&queried==0,"Source start predicate lost short circuit");
    Check(!SaveStateCanLoad(true,true,pending)&&queried==0,"Source loaded predicate lost short circuit");
    Check(SaveStateCanLoad(true,false,pending)&&queried==1,"Source load admission differs");
    Check(SaveStateCanStart(true,pending)&&queried==2,"Source save admission differs");
    Check(!SaveStateDepartureBlocked(enabled,busy),"Initial gate differs");
    SaveStateDirectory(1,mode,busy);Check(mode==1&&busy&&SaveStateDepartureBlocked(enabled,busy),"Save directory state differs");
    SaveStateFinish(busy);Check(!busy&&enabled,"Finish disabled saving");
    SaveStateDirectory(0,mode,busy);Check(mode==0&&busy,"Load directory state differs");
    SaveStateCancel(paused,enabled,busy);Check(!paused&&!enabled&&!busy,"Cancel source order/state differs");
}
void Codec(const std::filesystem::path& dir)
{
    const auto defaults=DefaultNativePreferences();
    Check(defaults.audio==std::array{10,10,10}&&defaults.audio_defaults==std::array{10,10,10}&&defaults.auto_zoom&&defaults.camera_zoom==.5f,"Source defaults differ");
    auto value=defaults;value.audio={0,5,10};value.audio_defaults={1,2,3};value.auto_zoom=false;value.camera_zoom=.25f;
    const auto encoded=EncodeNativePreferences(value);Write(dir/"format.pref",encoded);
    Check(DecodeNativePreferences(encoded)==value,"Typed format roundtrip differs");
    Check(encoded[24]==0&&encoded[31]==5&&encoded[35]==10&&encoded[48]==0x3e&&encoded[49]==0x80,"Format uses wrong endian or native layout");
    for(unsigned n=0;n<64;++n)Reject([&]{DecodeNativePreferences(std::span(encoded).first(n));});
    auto more=Data(encoded.begin(),encoded.end());more.push_back(0);Reject([&]{DecodeNativePreferences(more);});
    for(unsigned i=0;i<64;++i){auto bad=encoded;bad[i]^=0x80;Reject([&]{DecodeNativePreferences(bad);});}
    for(auto path:{dir/"bad-flags.pref",dir/"bad-index.pref",dir/"bad-reserved.pref",dir/"bad-zoom.pref",dir/"bad-version.pref"})Reject([&]{DecodeNativePreferences(Read(path));});
    for(int i:{-1,11}){value.audio[0]=i;Reject([&]{EncodeNativePreferences(value);});value=defaults;value.audio_defaults[2]=i;Reject([&]{EncodeNativePreferences(value);});value=defaults;}
    for(float f:{-.01f,1.01f,std::nanf(""),INFINITY}){value.camera_zoom=f;Reject([&]{EncodeNativePreferences(value);});}
}
void Lifecycle(const std::filesystem::path& dir)
{
    const auto path=dir/"lifecycle.pref";NativePreferences p(path);const auto first=p.Current();
    auto initial=p.Status();Check(initial.scope==NativePreferencesScope::NativePreferences&&initial.save_enabled&&!initial.in_operation&&!initial.original_normal_save_loaded&&!initial.full_save_complete,"Native authority scope or initializer differs");
    Reject([&]{p.DepartureBlocked(NativePreferencesScope::OriginalGameSave);});Reject([&]{p.StartSave(*first);});
    p.StartLoad();Check(p.Status().host_pending&&!p.DepartureBlocked(NativePreferencesScope::NativePreferences),"Admission fabricated original busy before callback");
    Reject([&]{p.StartLoad();});ReachFile(p);Check(p.DepartureBlocked(NativePreferencesScope::NativePreferences)&&p.Status().directory_callbacks==1,"Real directory callback did not enter operation");
    Pump(p);Check(p.Status().state==NativePreferencesState::Missing&&p.Current()==first&&!p.Status().preferences_loaded&&!p.DepartureBlocked(NativePreferencesScope::NativePreferences),"Absent file falsely loaded original save");
    auto values=*first;values.audio={3,4,5};values.auto_zoom=false;values.camera_zoom=.75f;
    p.StartSave(values);ReachFile(p);Check(p.Current()==first,"Preferences published before file commit");Pump(p);auto saved=p.Current();
    Check(*saved==values&&first->audio==std::array{10,10,10},"Retained values changed during save");
    Check(p.Status().state==NativePreferencesState::Ready&&p.Status().preferences_loaded&&!p.Status().original_normal_save_loaded&&!p.Status().full_save_complete,"Native write claimed original completeness");
    NativePreferences loaded(path);loaded.StartLoad();Pump(loaded);Check(*loaded.Current()==values,"Actual persisted typed values differ");
    bool foreign=false;std::thread t([&]{try{p.Status();}catch(const std::exception&){foreign=true;}});t.join();Check(foreign,"Foreign thread read accepted");
    p.Cancel();Check(!p.Status().save_enabled&&!p.Status().in_operation&&p.Current()==saved,"Cancel lost prior retained publication");Reject([&]{p.StartLoad();});EmptyTemps(dir);
}
void FailureAndCancellation(const std::filesystem::path& dir)
{
    const auto defaults=DefaultNativePreferences();auto edited=defaults;edited.audio={1,2,3};
    for(bool after_directory:{false,true})
    {
        const auto path=dir/(after_directory?"cancel-file.pref":"cancel-directory.pref");NativePreferences p(path);p.StartLoad();Pump(p);auto prior=p.Current();
        p.StartSave(edited);if(after_directory)ReachFile(p);p.Cancel();Check(!std::filesystem::exists(path)&&p.Current()==prior&&!p.Status().host_pending,"Cancel published pending file");EmptyTemps(dir);
    }
    for(bool after_directory:{false,true})
    {
        const auto path=dir/(after_directory?"destroy-file.pref":"destroy-directory.pref");NativePreferences::Handle old;
        {NativePreferences p(path);p.StartLoad();Pump(p);old=p.Current();p.StartSave(edited);if(after_directory)ReachFile(p);}
        Check(!std::filesystem::exists(path)&&*old==defaults,"Destructor published staged data or lost retained values");EmptyTemps(dir);
    }
    const auto path=dir/"conflict.pref";Write(path,EncodeNativePreferences(defaults));NativePreferences p(path);p.StartLoad();Pump(p);auto old=p.Current();
    p.StartSave(edited);ReachFile(p);auto external=defaults;external.audio[0]=7;const auto bytes=EncodeNativePreferences(external);Write(path,bytes);
    Reject([&]{Pump(p);});Check(p.Status().state==NativePreferencesState::Failed&&p.Status().in_operation&&p.Current()==old,"External edit failure changed publication/state");
    Check(Read(path)==Data(bytes.begin(),bytes.end()),"External preferences overwritten");Reject([&]{p.DepartureBlocked(NativePreferencesScope::NativePreferences);});Reject([&]{p.RethrowFailure();});Reject([&]{p.StartLoad();});p.Cancel();EmptyTemps(dir);
    NativePreferences missing(dir/"no-parent"/"x.pref");missing.StartLoad();Reject([&]{Pump(missing);});Check(!missing.Status().in_operation&&missing.Status().state==NativePreferencesState::Failed,"Directory failure pretended callback success");missing.Cancel();
    NativePreferences invalid(dir/"bad-index.pref");auto previous=invalid.Current();invalid.StartLoad();Reject([&]{Pump(invalid);});Check(invalid.Current()==previous&&!invalid.Status().preferences_loaded,"Invalid read exposed partial fields");invalid.Cancel();
    const auto directory=dir/"directory.pref";std::filesystem::create_directory(directory);NativePreferences wrong(directory);wrong.StartLoad();Reject([&]{Pump(wrong);});wrong.Cancel();
    const auto moved=dir/"moved-parent";std::filesystem::create_directory(moved);const auto target=moved/"x.pref";
    NativePreferences inaccessible(target);inaccessible.StartLoad();Pump(inaccessible);std::filesystem::remove(moved);inaccessible.StartSave(edited);Reject([&]{Pump(inaccessible);});inaccessible.Cancel();
    // Real host failures after successful directory completion, not a fake I/O
    // provider. The harness runs as the ordinary invoking user.
    const auto readonly=dir/"readonly";std::filesystem::create_directory(readonly);
    NativePreferences denied(readonly/"x.pref");denied.StartLoad();Pump(denied);auto denied_old=denied.Current();
    std::filesystem::permissions(readonly,std::filesystem::perms::owner_read|std::filesystem::perms::owner_exec);
    denied.StartSave(edited);
    try
    {
        Reject([&]{Pump(denied);});
        Check(denied.Status().directory_callbacks==2&&denied.Status().in_operation&&denied.Current()==denied_old,"Real file write failure lost original operation state");
    }
    catch(...){std::filesystem::permissions(readonly,std::filesystem::perms::owner_all);throw;}
    std::filesystem::permissions(readonly,std::filesystem::perms::owner_all);denied.Cancel();EmptyTemps(readonly);
    const auto protected_dir=dir/"protected-commit";std::filesystem::create_directory(protected_dir);
    NativePreferences protected_commit(protected_dir/"x.pref");protected_commit.StartLoad();Pump(protected_commit);auto protected_old=protected_commit.Current();
    protected_commit.StartSave(edited);ReachFile(protected_commit);
    // Wait for the real worker to close its 64-byte temporary. Poll is withheld
    // so destination publication cannot occur during this observation.
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool staged=false;
    while(!staged)
    {
        for(const auto& entry:std::filesystem::directory_iterator(protected_dir))if(entry.file_size()==64)staged=true;
        Check(std::chrono::steady_clock::now()<limit,"Staged file was not written");std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::filesystem::permissions(protected_dir,std::filesystem::perms::owner_read|std::filesystem::perms::owner_exec);
    try
    {
        Reject([&]{Pump(protected_commit);});Reject([&]{protected_commit.Cancel();});
        Check(protected_commit.Current()==protected_old&&!std::filesystem::exists(protected_dir/"x.pref"),"Failed rename/cleanup published data");
    }
    catch(...){std::filesystem::permissions(protected_dir,std::filesystem::perms::owner_all);throw;}
    std::filesystem::permissions(protected_dir,std::filesystem::perms::owner_all);protected_commit.Cancel();EmptyTemps(protected_dir);
    for(unsigned n:{0u,63u,65u})
    {
        const auto short_path=dir/("short-"+std::to_string(n)+".pref");Data b(n,0);Write(short_path,b);
        NativePreferences malformed(short_path);auto before=malformed.Current();malformed.StartLoad();Reject([&]{Pump(malformed);});
        Check(malformed.Current()==before&&malformed.Status().in_operation,"Malformed actual read lost prior snapshot");malformed.Cancel();
    }
    const auto created=dir/"created-externally.pref";NativePreferences absent(created);absent.StartLoad();Pump(absent);absent.StartSave(edited);
    Write(created,bytes);Reject([&]{Pump(absent);});Check(Read(created)==Data(bytes.begin(),bytes.end()),"Externally created file overwritten");absent.Cancel();
    const auto link=dir/"symlink.pref";std::error_code error;std::filesystem::create_symlink(path,link,error);
    if(!error){NativePreferences symlink(link);symlink.StartLoad();Reject([&]{Pump(symlink);});symlink.Cancel();}
}
void AllocationFailures(const std::filesystem::path& dir)
{
    unsigned rejected=0,completed=0;
    for(long budget=0;budget<32;++budget)
    {
        const auto path=dir/("oom-"+std::to_string(budget)+".pref");NativePreferences p(path);p.StartLoad();Pump(p);auto old=p.Current();auto changed=*old;changed.audio[1]=2;
        try{allocation_budget=budget;p.StartSave(changed);allocation_budget=-1;Pump(p);++completed;Check(p.Current()->audio[1]==2,"Successful admission lost value");}
        catch(const std::bad_alloc&){allocation_budget=-1;++rejected;Check(p.Current()==old,"Admission OOM changed publication");p.Cancel();Check(!std::filesystem::exists(path),"Admission OOM left destination");}
        allocation_budget=-1;EmptyTemps(dir);
    }
    Check(rejected>0&&completed>0,"OOM sweep did not reach failure and success");
    // Inject failure in directory completion/worker launch, and in final exact
    // conflict check. Worker allocations use their own thread and are not faked.
    for(bool finish:{false,true})for(long budget=0;budget<8;++budget)
    {
        const auto path=dir/("poll-"+std::to_string(finish)+"-"+std::to_string(budget)+".pref");NativePreferences p(path);p.StartLoad();Pump(p);auto old=p.Current();auto changed=*old;changed.audio[0]=4;p.StartSave(changed);
        if(finish)ReachFile(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        try{allocation_budget=budget;p.Poll();allocation_budget=-1;Pump(p);Check(p.Current()->audio[0]==4,"Poll publication differs");}
        catch(const std::bad_alloc&){allocation_budget=-1;Check(p.Current()==old,"Poll OOM changed publication");p.Cancel();Check(!std::filesystem::exists(path),"Poll OOM published destination");}
        allocation_budget=-1;EmptyTemps(dir);
    }
}
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"Expected private fixture directory");const std::filesystem::path dir=argv[1];Check(dir.is_absolute(),"Fixture path is not absolute");
        SourceKernels();Codec(dir);Lifecycle(dir);FailureAndCancellation(dir);AllocationFailures(dir);
        for(unsigned i=0;i<8;++i){NativePreferences p(dir/"lifecycle.pref");p.StartLoad();Pump(p);Check(p.Current()->audio==std::array{3,4,5},"Repeated owner changed retained file");}
        std::cout<<"Native preferences: "<<checks<<" checks; actual staged host I/O, original game save completeness=false\n";return 0;
    }
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<" (check "<<checks<<")\n";return 1;}
}

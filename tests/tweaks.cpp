#include "runtime/tweaks.h"
#include "runtime/startup.h"
#include "Game/TweakRegistry.h"
#include "Game/TweakValueFloat.h"
#include "Game/TweakValueInt.h"
#include "Game/TweakConfig.h"
#include "NL/MemAlloc.h"
#include <iostream>
#include <stdexcept>
#include <vector>

// Exercise actual registration before the game memory arenas exist.
TweakValueBool early("Early", "/Fixture", true, false);
TweakFloatBinding early_binding("Initially unbound", "/Fixture", nullptr, false);
TweakBoolBinding early_bool_binding("Unbound bool", "/Fixture", nullptr, false);
TweakIntBinding early_int_binding("Unbound int", "/Fixture", nullptr, false);
namespace
{
unsigned checks=0;
void Check(bool v,const char* message) { ++checks; if (!v) throw std::runtime_error(message); }
template<class E=std::exception,class F> void Reject(F f)
{
    try { f(); } catch(const E&) { ++checks; return; }
    throw std::runtime_error("Invalid tweak operation was accepted");
}
void Run()
{
    mscharged::OriginalTweaks session;
    Reject<std::logic_error>([] { mscharged::OriginalTweaks duplicate; });
    Reject<std::logic_error>([] { InitializeTweakRegistry(0,1,nullptr); });
    Reject<std::logic_error>([] { ResetDynamicTweaks(); });
    Check(GetTweakBool("/Fixture/Early",false),"Pre-memory tweak registration was lost");
    Check(GetTweakFloat("/Fixture/Initially unbound",-1)==0,"Pending unbound tweak did not receive its original default");
    Check(!GetTweakBool("/Fixture/Unbound bool",true),"Pending bool binding did not create its owned default");
    Check(GetTweakInt("/Fixture/Unbound int",-1)==0,"Pending int binding did not create its owned default");
    {
        TweakValueBool local("Borrowed", "/Fixture", false, false);
        float bound_value = 2.f;
        TweakFloatBinding bound("Bound float", "/Fixture", &bound_value, false);
        bound.ParseValue("3.5");
        Check(bound_value == 3.5f, "Original generic float binding parser failed");
        Reject<std::out_of_range>([&] { bound.ParseValue("nan"); });
        Check(bound_value == 3.5f, "Rejected binding value changed the caller's storage");
        bool bound_bool = true;
        int bound_int = 42;
        TweakBoolBinding boolean("Bound bool", "/Fixture", &bound_bool, false);
        TweakIntBinding integer("Bound int", "/Fixture", &bound_int, false);
        Check(GetTweakBool("/Fixture/Bound bool",false),"Bool binding lookup lost its actual value type");
        Check(GetTweakInt("/Fixture/Bound int",0)==42,"Int binding lookup lost its actual value type");
        Check(TweakExists("/Fixture/Borrowed"),"Borrowed tweak was not registered");
        char input[]="[Fixture]\nBorrowed=true\nCount=42\nScale=1.25\nName=Charged\n";
        LoadTweakConfigBuffer(nullptr,input,sizeof(input)-1,"");
        Check(local.GetValue(),"Original parser did not update borrowed value");
        Check(GetTweakInt("/Fixture/Count",0)==42,"Original integer tweak parse failed");
        Check(GetTweakFloat("/Fixture/Scale",0)==1.25f,"Original float tweak parse failed");
        Check(strcmp(GetTweakString("/Fixture/Name",""),"Charged")==0,"Original string tweak parse failed");
        Check(GetTweakInt("/Absent",7)==7,"Missing tweak fallback changed");
        Reject<std::invalid_argument>([] { GetTweakString("/Fixture/Count",""); });
        auto* folder=FindOrCreateTweakPath(GetTweakRoot(),"/Fixture",0);
        Check(FindOrCreateTweakPath(folder,"/",0)==folder,"Root path handling failed");
        char output[64];
        FindTweakChild(folder,"Count")->m_Value->FormatValue(output,sizeof(output));
        Check(strcmp(output,"42")==0,"Extracted original integer formatting failed");
        // Original owned-value parsers use atoi/atof without validation.
        // Their malformed-input behavior is not a rejection contract.
        for(unsigned i=0;i<80;++i)
        {
            const std::string name="Entry"+std::to_string(i);
            CreateTweakValueFromString(folder,name.c_str(),"false");
        }
        Check(!GetTweakBool("/Fixture/Entry79",true),"Native multi-block tweak pool failed");
        std::string tooLong(300,'x');
        Reject<std::length_error>([&] { FindOrCreateTweakPath(folder,tooLong.c_str(),0); });
        Reject<std::length_error>([&] { LoadTweakConfigBuffer(nullptr,tooLong.data(),tooLong.size(),""); });
        std::string tooMany(4096,'x');
        Reject<std::length_error>([&] { InternTweakString(tooMany.c_str(),kTweakStringValue); });
    }
    Check(!TweakExists("/Fixture/Borrowed"),"Destroyed borrowed tweak retained a node");
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        mscharged::ResetStartupMemory();
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);
        VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        for(int i=0;i<3;++i)
        {
            Run();
            Check(early_binding.m_pValue==nullptr,"Registry shutdown retained a dangling binding");
            Check(early_bool_binding.m_pValue==nullptr,"Registry shutdown retained a dangling bool binding");
            Check(early_int_binding.m_pValue==nullptr,"Registry shutdown retained a dangling int binding");
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,
                  "Tweak registry leaked game arenas");
        }
        mscharged::ResetStartupMemory();
        std::cout<<checks<<" original tweak registration/parsing/lifetime checks and three arena recoveries passed\n";
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

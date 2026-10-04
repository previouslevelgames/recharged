#include "runtime/nis_trigger_script.h"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>

using namespace mscharged;
namespace
{
using Blob=std::vector<std::uint8_t>;
unsigned checks=0;
void Check(bool good,const char* message){++checks;if(!good)throw std::runtime_error(message);}
template<class F>void Reject(F action)
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid trigger collection succeeded");}
std::uint32_t Bits(float value){return std::bit_cast<std::uint32_t>(value);}
std::uint32_t Hash(std::string_view value){std::uint32_t result=UINT32_MAX;for(unsigned char c:value)result=result*33+c;return result;}
std::uint16_t Op(unsigned code,unsigned operand=0){return (code<<11)|operand;}
struct Fixture
{
    struct Function{std::uint32_t hash,offset;unsigned frame=2,args=0,flags=0;};
    std::vector<Function> functions;
    std::vector<std::uint32_t> data,globals;
    std::vector<std::uint16_t> code;
    std::string strings;
    std::vector<std::uint16_t> Host(unsigned id,std::initializer_list<InterpreterValue> args)
    {
        std::vector<std::uint16_t> result;
        for(const auto& arg:args)
        {
            if(const auto* word=std::get_if<std::uint32_t>(&arg)){data.push_back(*word);result.push_back(Op(0,data.size()-1));}
            else{data.push_back(strings.size());strings+=std::get<std::string>(arg);strings.push_back('\0');result.push_back(Op(1,data.size()-1));}
        }
        result.push_back(Op(8,id));return result;
    }
    void Add(std::string_view name,std::vector<std::uint16_t> body,unsigned args=0,unsigned flags=0)
    {
        functions.push_back({Hash(name),std::uint32_t(code.size()*2),2,args,flags});
        code.insert(code.end(),body.begin(),body.end());code.push_back(Op(10));
    }
    Blob Bytes()const
    {
        Blob out;auto word=[&](std::uint32_t v){for(int shift:{24,16,8,0})out.push_back(v>>shift);};
        auto half=[&](unsigned v){out.push_back(v>>8);out.push_back(v);};
        for(auto v:{0xe11c2112u,unsigned(functions.size()),0u,unsigned(globals.size()*4),unsigned(data.size()*4),unsigned(code.size()*2),
            unsigned(strings.size()),unsigned(globals.size()),unsigned(globals.size()),unsigned(globals.size()),unsigned(globals.size()),0u,0u,0u,0u,0u,0u,0u})word(v);
        auto sorted=functions;std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.hash<b.hash;});
        for(auto f:sorted){word(f.hash);word(f.offset);half(f.frame);out.push_back(f.args);out.push_back(f.flags);}
        for(auto v:globals)word(v);for(auto v:data)word(v);for(auto v:code)half(v);out.insert(out.end(),strings.begin(),strings.end());return out;
    }
};
void Same(const NisPlaybackTrigger& actual,const NisPlaybackTrigger& expected)
{
    Check(actual.type==expected.type&&Bits(actual.frame)==Bits(expected.frame)&&actual.name==expected.name&&actual.target==expected.target
        &&Bits(actual.value)==Bits(expected.value)&&actual.params==expected.params,"Original trigger mapping/defaults/flags differ");
}
void ServicesAndSelection()
{
    Fixture f;std::vector<std::uint16_t> body;std::vector<NisPlaybackTrigger> expected;
    auto add=[&](unsigned id,std::initializer_list<InterpreterValue> args,NisPlaybackTrigger record){auto call=f.Host(id,args);body.insert(body.end(),call.begin(),call.end());expected.push_back(std::move(record));};
    constexpr auto unset=UINT32_MAX;
    add(0,{Bits(5.5f),0x80u},{8,5.5f,"","",-1,{1,unset,unset,unset}});
    add(1,{Bits(6)},{9,6});
    add(2,{Bits(7),std::string("ef/ember"),std::string("ball"),0xf0000012u},{0,7,"ef/ember","ball",-1,{0xf0000012u,unset,unset,unset}});
    add(3,{Bits(8),0xf1234567u,0xffffffffu},{3,8,"","",-1,{0xf1234567u,0xffffffffu,unset,unset}});
    add(4,{Bits(9),std::string("goal"),std::string("captain")},{2,9,"goal","captain"});
    add(5,{Bits(10),Bits(1.5f)},{7,10,"","",1});
    add(6,{Bits(11),0u},{5,11,"","",-1,{0,unset,unset,unset}});
    add(7,{Bits(12),0xdeadbeefu},{4,12,"","",-1,{0xdeadbeefu,unset,unset,unset}});
    add(8,{Bits(13),0xffffffffu},{6,13,"","",-1,{0xffffffffu,unset,unset,unset}});
    add(9,{Bits(14)},{10,14});
    add(10,{Bits(15),Bits(.5f)},{1,15,"","",.5f});
    add(5,{Bits(16),Bits(-2.f)},{7,16,"","",0});
    add(5,{Bits(17),Bits(std::numeric_limits<float>::infinity())},{7,17,"","",1});
    add(5,{Bits(18),Bits(-std::numeric_limits<float>::infinity())},{7,18,"","",0});
    add(5,{Bits(19),Bits(-0.f)},{7,19,"","",-0.f});
    f.Add("mario_intro",body);f.Add("all_intro",f.Host(1,{Bits(99)}));f.Add("plain",{});
    f.Add("intro.v2",f.Host(9,{Bits(20)}));f.Add("all_tail",f.Host(9,{Bits(21)}));
    auto bytes=f.Bytes();NisTriggerDefinitions retained;
    {
        NisTriggerScript script(bytes);bytes.clear();bytes.shrink_to_fit();
        retained=script.Collect("mario_intro.nis",2);
        Check(retained.found&&!retained.used_fallback&&retained.function_hash==Hash("mario_intro")&&retained.table.render_mode==2
            &&retained.host_calls==expected.size()&&retained.table.triggers.size()==expected.size(),"Exact original trigger selection differs");
        for(unsigned i=0;i<expected.size();++i)Same(retained.table.triggers[i],expected[i]);
        auto fallback=script.Collect("daisy_intro.nis");Check(fallback.found&&fallback.used_fallback&&fallback.function_hash==Hash("all_intro"),"Original all-prefix fallback differs");
        Same(fallback.table.triggers.at(0),{9,99});
        auto upper=script.Collect("Mario_intro.nis");Check(upper.used_fallback,"Original case-sensitive exact hash was folded");
        Check(!script.Collect("mario_INTRO.nis").found,"Original fallback suffix was folded");
        auto empty=script.Collect("plain.nis");Check(empty.found&&empty.table.triggers.empty()&&!empty.used_fallback,"Empty existing function was treated as missing");
        Check(script.Collect("intro.v2.nis").function_hash==Hash("intro.v2"),"Original last-dot selection removed too much");
        Check(script.Collect("odd.nis_tail").function_hash==Hash("all_tail"),"Original stored-tail fallback behavior changed");
        Check(!script.Collect("missing.nis").found&&!script.CollectHash(0).found,"Missing trigger function reported success");
        Reject([&]{script.Collect("");});Reject([&]{script.Collect(std::string(64,'x'));});Reject([&]{script.Collect(std::string("x\0y",3));});
        Reject([&]{script.Collect("plain.nis",3);});Check(!script.Failed(),"Selection preflight poisoned trigger owner");
        Check(!script.Collect(std::string(63,'x')).found,"Valid original-sized absent name was rejected");
        bool rejected=false;std::thread other([&]{try{script.Collect("plain.nis");}catch(const std::logic_error&){rejected=true;}});other.join();
        Check(rejected&&!script.Failed(),"Wrong-thread trigger selection changed owner state");
    }
    for(unsigned i=0;i<expected.size();++i)Same(retained.table.triggers[i],expected[i]);
    Check(OriginalNisTriggerHash("mario_intro")==Hash("mario_intro")&&OriginalNisTriggerHash("")==UINT32_MAX,"Original hash equation differs");
}
void Failures()
{
    for(unsigned kind=0;kind<9;++kind)
    {
        Fixture f;auto body=f.Host(1,{Bits(1)});
        std::vector<std::uint16_t> bad;
        if(kind==0)bad=f.Host(42,{Bits(2)});
        if(kind==1)bad=f.Host(2,{Bits(2),7u,std::string("target"),1u});
        if(kind==2)bad=f.Host(1,{});
        if(kind==3)bad={Op(2,1),Op(13,39)};
        if(kind==4)bad=f.Host(1,{Bits(std::numeric_limits<float>::infinity())});
        if(kind==5)bad=f.Host(5,{Bits(2),Bits(std::numeric_limits<float>::quiet_NaN())});
        if(kind==6)bad=f.Host(10,{Bits(2),Bits(std::numeric_limits<float>::infinity())});
        if(kind==7)bad=f.Host(4,{Bits(2),std::string(4097,'x'),std::string("target")});
        if(kind==8)bad={Op(7,0)};
        body.insert(body.end(),bad.begin(),bad.end());f.Add("mario_bad",body);f.Add("all_bad",f.Host(1,{Bits(88)}));f.Add("good",f.Host(9,{Bits(3)}));
        NisTriggerScript script(f.Bytes(),{100,16,100,100});auto previous=script.Collect("good.nis");
        Reject([&]{script.Collect("mario_bad.nis");});Check(script.Failed(),"Partial malformed collection did not fail");
        Same(previous.table.triggers.at(0),{10,3});Reject([&]{script.Collect("good.nis");});
        script.Reset();auto result=script.Collect("good.nis");Check(result.table.triggers.size()==1,"Failed collection exposed partial records after Reset");
    }
    Fixture capacity;std::vector<std::uint16_t> body;
    for(unsigned i=0;i<49;++i){auto call=capacity.Host(1,{Bits(float(i))});body.insert(body.end(),call.begin(),call.end());}
    capacity.Add("over",body);body.resize(body.size()-2);capacity.Add("full",body);capacity.Add("empty",{});NisTriggerScript script(capacity.Bytes());
    auto full=script.Collect("full.nis");Check(full.table.triggers.size()==48&&full.table.triggers.back().frame==47,"Original full trigger capacity/order differs");
    Reject([&]{script.Collect("over.nis");});script.Reset();Check(script.Collect("empty.nis").table.triggers.empty(),"Capacity failure leaked partial records");
    Fixture budget;budget.Add("one",budget.Host(1,{Bits(1)}));budget.Add("empty",{});
    for(auto limits:{InterpreterLimits{100,16,2,2},InterpreterLimits{100,16,100,0}})
    {NisTriggerScript bounded(budget.Bytes(),limits);Reject([&]{bounded.Collect("one.nis");});bounded.Reset();Check(bounded.Collect("empty.nis").found,"Budget failure could not reset");}
    Fixture signature;signature.Add("args",{},1);signature.Add("empty",{});NisTriggerScript args(signature.Bytes());
    Reject([&]{args.Collect("args.nis");});args.Reset();Check(args.Collect("empty.nis").found,"Function-arity failure could not reset");
}
void PersistentGlobals()
{
    Fixture f;f.globals={0};std::vector<std::uint16_t> body={Op(14),Op(2,1),Op(13,20),Op(15)};
    auto prefix=f.Host(1,{Bits(1)});body.insert(body.end(),prefix.begin(),prefix.end());
    body.push_back(Op(8,42));f.Add("bad",body);
    f.data.push_back(Bits(5));f.Add("get",{Op(0,f.data.size()-1),Op(14),Op(8,7)});
    NisTriggerScript script(f.Bytes());Reject([&]{script.Collect("bad.nis");});script.Reset();
    Check(script.Collect("get.nis").table.triggers.at(0).params[0]==1,"Reset silently erased original script globals");
    script.Reset(true);Check(script.Collect("get.nis").table.triggers.at(0).params[0]==0,"Explicit global restoration differs");
}
void BinaryWord(std::ostream& out,std::uint32_t word)
{for(int shift:{24,16,8,0})out.put(char(word>>shift));}
void Owned(const char* path,const char* output,const char* dictionary)
{
    std::ifstream input(path,std::ios::binary);Check(bool(input),"Cannot open owned trigger bytecode");
    Blob bytes((std::istreambuf_iterator<char>(input)),{});auto code=resources::ReadScriptBytecode(bytes);NisTriggerScript script(bytes);
    std::ofstream out(output,std::ios::binary);Check(bool(out),"Cannot open private native trigger records");out.write("NIST",4);BinaryWord(out,code->functions.size());
    unsigned total=0,maximum=0,nonempty=0;
    for(const auto& entry:code->functions)
    {
        auto definitions=script.CollectHash(entry.hash);
        Check(definitions.found&&!definitions.used_fallback&&definitions.host_calls==definitions.table.triggers.size(),"Owned function did not finish genuine collection");
        BinaryWord(out,entry.hash);BinaryWord(out,definitions.table.triggers.size());
        total+=definitions.table.triggers.size();maximum=std::max(maximum,unsigned(definitions.table.triggers.size()));nonempty+=!definitions.table.triggers.empty();
        for(const auto& record:definitions.table.triggers)
        {
            BinaryWord(out,record.type);BinaryWord(out,Bits(record.frame));BinaryWord(out,Bits(record.value));for(auto word:record.params)BinaryWord(out,word);
            for(const auto* text:{&record.name,&record.target}){BinaryWord(out,text->size());out.write(text->data(),text->size());}
        }
    }
    std::vector<std::string> names;
    if(dictionary)
    {
        std::ifstream text(dictionary);Check(bool(text),"Cannot open owned NIS dictionary");std::string line;
        while(std::getline(text,line))if(line.starts_with("name "))
        {if(line.ends_with('\r'))line.pop_back();names.push_back(line.substr(5));}
    }
    BinaryWord(out,names.size());unsigned exact=0,fallback=0,missing=0;
    for(const auto& name:names)
    {
        auto definitions=script.Collect(name);exact+=definitions.found&&!definitions.used_fallback;
        fallback+=definitions.used_fallback;missing+=!definitions.found;
        BinaryWord(out,name.size());out.write(name.data(),name.size());BinaryWord(out,definitions.found);BinaryWord(out,definitions.used_fallback);
        BinaryWord(out,definitions.function_hash);BinaryWord(out,definitions.table.triggers.size());
    }
    Check(bool(out),"Cannot write private native trigger records");
    std::cout<<code->functions.size()<<" owned functions, "<<total<<" collected triggers, "<<nonempty<<" nonempty functions, maximum "<<maximum<<" triggers/function\n";
    std::cout<<names.size()<<" dictionary names: "<<exact<<" exact, "<<fallback<<" fallback, "<<missing<<" absent trigger functions\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        for(unsigned i=0;i<3;++i){ServicesAndSelection();Failures();PersistentGlobals();}
        if((argc==4||argc==5)&&std::string_view(argv[1])=="--owned")Owned(argv[2],argv[3],argc==5?argv[4]:nullptr);else Check(argc==1,"Unknown trigger test arguments");
        std::cout<<checks<<" original NIS trigger-definition checks passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}

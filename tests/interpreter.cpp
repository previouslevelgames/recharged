#include "runtime/interpreter.h"
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

using namespace mscharged;
namespace
{
using Blob = std::vector<std::uint8_t>;
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid interpreter operation succeeded"); }
std::uint32_t Bits(float value) { return std::bit_cast<std::uint32_t>(value); }
std::uint16_t Op(unsigned code, unsigned value = 0) { return (code << 11) | value; }
struct Fixture
{
    struct Function { unsigned offset, frame, arguments, flags; };
    std::vector<Function> functions;
    std::vector<std::uint32_t> data, globals;
    std::vector<std::uint16_t> code;
    std::string strings;
    void FunctionCode(std::initializer_list<std::uint16_t> body, unsigned frame = 2, unsigned arguments = 0, unsigned flags = 0)
    { functions.push_back({unsigned(code.size() * 2), frame, arguments, flags}); code.insert(code.end(), body); }
    Blob Bytes() const
    {
        Blob result;
        auto word = [&](std::uint32_t value) { for (int shift : {24, 16, 8, 0}) result.push_back(value >> shift); };
        auto half = [&](unsigned value) { result.push_back(value >> 8); result.push_back(value); };
        for (auto value : {0xe11c2112u, unsigned(functions.size()), 0u, unsigned(globals.size() * 4), unsigned(data.size() * 4),
            unsigned(code.size() * 2), unsigned(strings.size()), unsigned(globals.size()), unsigned(globals.size()), unsigned(globals.size()),
            unsigned(globals.size()), 0u, 0u, 0u, 0u, 0u, 0u, 0u}) word(value);
        unsigned hash = 100;
        for (const auto& f : functions) { word(hash++); word(f.offset); half(f.frame); result.push_back(f.arguments); result.push_back(f.flags); }
        for (auto value : globals) word(value); for (auto value : data) word(value); for (auto value : code) half(value);
        result.insert(result.end(), strings.begin(), strings.end()); return result;
    }
};
InterpreterHostCall Sink(std::vector<InterpreterValue>& values, unsigned count = 1, unsigned id = 0)
{
    return {id, std::vector<InterpreterValueKind>(count, InterpreterValueKind::Word), false,
        [&](auto args) { values.insert(values.end(), args.begin(), args.end()); return InterpreterHostResult{}; }};
}
std::uint32_t Word(const InterpreterValue& value) { Check(std::holds_alternative<std::uint32_t>(value), "Host value lost numeric type"); return std::get<std::uint32_t>(value); }
void Numbers()
{
    struct Binary { unsigned op; std::uint32_t a, b, expected; };
    const std::vector<Binary> cases = {
        {0,0,17,1},{1,7,3,1},{2,0xffffffff,0xffffffff,1},{3,Bits(-0.f),Bits(0.f),1},
        {5,0xffffffff,0,1},{6,Bits(2.f),Bits(1.f),1},{8,0xffffffff,1,1},{9,Bits(-2.f),Bits(1.f),1},
        {11,0xffffffff,0xffffffff,1},{12,Bits(1.f),Bits(1.f),1},{14,1,0xffffffff,1},{15,Bits(4.f),Bits(2.f),1},
        {17,0x80000000,0x80000000,1},{18,Bits(5.f),Bits(4.f),1},{20,0xffffffff,2,1},
        {21,Bits(1.25f),Bits(2.5f),Bits(3.75f)},{22,0,1,0xffffffff},{23,Bits(1.f),Bits(3.f),Bits(-2.f)},
        {24,0x80000000,3,0x80000000},{25,Bits(3.f),Bits(-2.f),Bits(-6.f)},
        {26,0xfffffff9,2,0xfffffffd},{27,Bits(7.f),Bits(2.f),Bits(3.5f)},{28,0xffffffff,7,3},
        {29,0xffffffff,1,1},{30,Bits(-2.f),Bits(5.f),Bits(5.f)},
        {31,0xffffffff,1,0xffffffff},{32,Bits(-2.f),Bits(5.f),Bits(-2.f)},
        {27,Bits(1.f),Bits(0.f),Bits(std::numeric_limits<float>::infinity())},
        {6,Bits(std::numeric_limits<float>::quiet_NaN()),Bits(0.f),1},
    };
    for (const auto& c : cases)
    {
        Fixture f; f.data = {c.a,c.b}; f.FunctionCode({Op(0,0),Op(0,1),Op(13,c.op),Op(8),Op(10)});
        NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output; vm.Bind(Sink(output));
        Check(vm.Execute(100), "Original function hash lookup failed");
        Check(output.size() == 1 && Word(output[0]) == c.expected, "Original binary numeric equation differs");
        Check(vm.Status() == InterpreterStatus::Ready && vm.Instructions() == 5 && vm.HostCalls() == 1, "Instruction/service accounting differs");
        Check(!vm.Execute(99) && !vm.Execute(101), "Missing hash selected a function");
    }
    for (auto [operation,value,expected] : {std::tuple{34u,0u,1u}, {35u,0x80000000u,0x80000000u}, {36u,Bits(2.25f),Bits(-2.25f)}})
    {
        Fixture f; f.data = {value}; f.FunctionCode({Op(0),Op(13,operation),Op(8),Op(10)});
        NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output; vm.Bind(Sink(output)); vm.ExecuteIndex(0);
        Check(Word(output[0]) == expected, "Original unary numeric equation differs");
    }
    Fixture f; f.data = {Bits(2.f),Bits(3.f),Bits(4.f),Bits(5.f)};
    f.FunctionCode({Op(0,0),Op(0,1),Op(0,2),Op(0,3),Op(2,4),Op(13,37),Op(8),
                    Op(0,0),Op(0,1),Op(0,3),Op(2,3),Op(13,38),Op(8),Op(10)});
    NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output; vm.Bind(Sink(output)); vm.ExecuteIndex(0);
    Check(output.size() == 2 && Word(output[0]) == Bits(26.f) && Word(output[1]) == Bits(10.f / 3.f), "Original reduction equation differs");
}
void Strings()
{
    for (auto [operation,left,right,expected] : {std::tuple{4u,0u,6u,1u},{7u,0u,6u,0u},{10u,0u,12u,1u},
             {13u,0u,6u,1u},{16u,12u,0u,1u},{19u,6u,0u,1u}})
    {
        Fixture f; f.strings = std::string("Hello\0hELLo\0World\0",18); f.data = {right};
        f.FunctionCode({Op(3,left),Op(1),Op(13,operation),Op(8),Op(10)});
        auto bytes = f.Bytes();
        if constexpr (sizeof(void*) > 4) Check(reinterpret_cast<std::uintptr_t>(bytes.data()) > 0xffffffffu, "Test input does not exercise native addresses above 4 GiB");
        NativeInterpreter vm(bytes); bytes.clear(); bytes.shrink_to_fit();
        std::vector<InterpreterValue> output; vm.Bind(Sink(output)); vm.ExecuteIndex(0);
        Check(Word(output[0]) == expected, "Original case-insensitive string comparison differs");
    }
    for (unsigned operation : {4u,7u})
    {
        Fixture f; f.strings = std::string("x\0",2); f.FunctionCode({Op(2,0),Op(3,0),Op(13,operation),Op(8),Op(10)});
        NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output; vm.Bind(Sink(output)); vm.ExecuteIndex(0);
        Check(Word(output[0]) == (operation == 7), "Original null string equality differs");
    }
    Fixture f; f.strings = std::string("retained original string\0",25); f.FunctionCode({Op(3),Op(12,0),Op(10,3)},2,1,3);
    std::optional<InterpreterValue> value;
    { NativeInterpreter vm(f.Bytes()); vm.ExecuteIndex(0); value = vm.Result(); }
    Check(value && std::get<std::string>(*value) == "retained original string", "String function result did not retain its value");
}
void Locals()
{
    Fixture f; f.FunctionCode({Op(2,42),Op(17,2),Op(12,3),Op(18,(2<<5)|4),Op(19,(5<<5)|31),Op(11,5),Op(8,0),
        Op(20,(2<<5)|14),Op(8,1),Op(21,(2<<6)|(3<<3)|3),Op(8,2),Op(2,99),Op(16,511),Op(16,1),Op(16,255),Op(10)},7);
    NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output;
    vm.Bind(Sink(output)); vm.Bind(Sink(output,2,1)); vm.Bind(Sink(output,3,2)); vm.ExecuteIndex(0);
    Check(output.size() == 6 && Word(output[0]) == 0xfffffff1, "Original packed local immediate differs");
    for (unsigned i = 1; i < output.size(); ++i) Check(Word(output[i]) == 42, "Original packed local copy/push differs");
}
void Calls()
{
    Fixture f; f.globals = {0};
    f.FunctionCode({Op(2,10),Op(2,20),Op(9,1),Op(14),Op(8),Op(2,7),Op(9,2),Op(8),
                    Op(16,1),Op(2,6),Op(9,3),Op(8),Op(10)});
    f.FunctionCode({Op(11,0),Op(11,1),Op(13,20),Op(15),Op(10,4)},2,2);
    f.FunctionCode({Op(11,0),Op(2,2),Op(13,24),Op(12,0),Op(10,3)},2,1,1);
    f.FunctionCode({Op(11,1),Op(2,3),Op(13,24),Op(12,0),Op(10,5)},2,2,3);
    NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output; vm.Bind(Sink(output)); vm.ExecuteIndex(0);
    Check(output.size() == 3 && Word(output[0]) == 30 && Word(output[1]) == 14 && Word(output[2]) == 18,
          "Original nested call/return frame or argument convention differs");
    std::uint32_t argument = 9;
    vm.ExecuteIndex(2, std::span(&argument,1)); Check(vm.Result() && Word(*vm.Result()) == 18, "Original flags1 external return differs");
    vm.ExecuteIndex(3, std::span(&argument,1)); Check(vm.Result() && Word(*vm.Result()) == 27, "Original flags3 external return differs");
    Reject([&]{vm.ExecuteIndex(2);}); Reject([&]{vm.ExecuteIndex(3);}); Reject([&]{vm.ExecuteIndex(4);});
    Check(vm.Status() == InterpreterStatus::Ready, "Call preflight poisoned an idle interpreter");
}
void BranchesAndGlobals()
{
    Fixture f; f.globals = {3,0};
    f.FunctionCode({Op(14,0),Op(2,1),Op(13,22),Op(15,0),Op(14,1),Op(2,1),Op(13,20),Op(15,1),Op(14,0),Op(6,9),
                    Op(2,0),Op(4,2),Op(5,2),Op(2,999),Op(2,1),Op(4,2),Op(2,998),Op(14,1),Op(8),Op(10)});
    NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output; vm.Bind(Sink(output)); vm.ExecuteIndex(0);
    Check(output.size() == 1 && Word(output[0]) == 3 && vm.Globals()[0] == 0 && vm.Globals()[1] == 3, "Original conditional branches/global updates differ");
    vm.Reset(); Check(vm.Globals()[1] == 3, "Original Reset changed numeric globals");
    vm.Reset(true); Check(vm.Globals()[0] == 3 && vm.Globals()[1] == 0, "Explicit initial-global restoration differs");
}
void Pausing()
{
    {
        Fixture first; first.FunctionCode({Op(8),Op(10)}); NativeInterpreter vm(first.Bytes()); unsigned calls=0;
        vm.Bind({0,{},false,[&](auto args){Check(args.empty(),"Zero-argument service received stack values");
            return InterpreterHostResult{{},++calls==1?InterpreterFlow::Retry:InterpreterFlow::Continue};}});
        vm.ExecuteIndex(0);Check(vm.Status()==InterpreterStatus::Paused&&vm.Instructions()==1,"First-op retry did not preserve its code position");
        vm.Resume();Check(vm.Status()==InterpreterStatus::Ready&&calls==2&&vm.Instructions()==2,"First-op retry did not resume once");
    }
    Fixture f; f.FunctionCode({Op(2,1),Op(13,39),Op(2,777),Op(8),Op(10)});
    NativeInterpreter vm(f.Bytes()); std::vector<InterpreterValue> output; vm.Bind(Sink(output)); vm.ExecuteIndex(0);
    Check(vm.Status() == InterpreterStatus::Paused && output.empty() && vm.Instructions() == 2, "Original stop opcode did not pause");
    Reject([&]{vm.ExecuteIndex(0);}); vm.Resume(); Check(Word(output[0]) == 777 && vm.Status() == InterpreterStatus::Ready, "Original paused instruction did not resume");
    Reject([&]{vm.Resume();});
    Fixture h; h.FunctionCode({Op(2,7),Op(8,1),Op(8,0),Op(10)});
    NativeInterpreter retry(h.Bytes()); output.clear(); retry.Bind(Sink(output)); unsigned calls = 0;
    retry.Bind({1,{InterpreterValueKind::Word},true,[&](auto args){Check(Word(args[0])==7,"Retry lost its saved argument");
        if (++calls == 1) return InterpreterHostResult{{},InterpreterFlow::Retry};
        return InterpreterHostResult{9,InterpreterFlow::Pause};}});
    retry.ExecuteIndex(0); Check(retry.Status() == InterpreterStatus::Paused && calls == 1, "Retry did not pause at its host call");
    retry.Resume(); Check(retry.Status() == InterpreterStatus::Paused && calls == 2, "Retry did not repeat exactly the same host call");
    retry.Resume(); Check(output.size() == 1 && Word(output[0]) == 9 && retry.Status() == InterpreterStatus::Ready, "Host pause/return stack differs");
    Fixture nested; nested.FunctionCode({Op(2,3),Op(9,1),Op(8),Op(10)});
    nested.FunctionCode({Op(11,0),Op(2,1),Op(13,20),Op(12,0),Op(2,1),Op(13,39),Op(10,3)},2,1,1);
    NativeInterpreter child(nested.Bytes());output.clear();child.Bind(Sink(output));child.ExecuteIndex(0);
    Check(child.Status()==InterpreterStatus::Paused&&output.empty(),"Nested function did not retain its suspended frame");
    child.Resume();Check(child.Status()==InterpreterStatus::Ready&&output.size()==1&&Word(output[0])==4,
                         "Nested paused return lost its caller frame/result");
}
void MalformedExecution()
{
    const std::vector<std::vector<std::uint16_t>> programs = {
        {Op(13,20),Op(10)}, {Op(11,0),Op(10)}, {Op(2,1),Op(12,1),Op(10)}, {Op(11,2),Op(10)},
        {Op(11,3),Op(10)}, {Op(18,0),Op(10)}, {Op(20,31),Op(10)}, {Op(21,63),Op(10)},
        {Op(2,1),Op(13,37),Op(10)}, {Op(2,0),Op(13,38),Op(10)}, {Op(10,2)}, {Op(8)},
        {Op(16,255),Op(10)}, {Op(16,1),Op(13,34),Op(10)}, {Op(2,1)},
    };
    for (const auto& code : programs)
    {
        Fixture f; f.FunctionCode({},3); f.code = code; NativeInterpreter vm(f.Bytes());
        Reject([&]{vm.ExecuteIndex(0);}); Check(vm.Status() == InterpreterStatus::Failed, "Runtime error did not poison execution");
        Reject([&]{vm.ExecuteIndex(0);}); vm.Reset(); Check(vm.Status() == InterpreterStatus::Ready, "Reset did not recover failed execution");
    }
    for (auto [operation,left,right] : {std::tuple{26u,1u,0u},{26u,0x80000000u,0xffffffffu},{28u,1u,0u}})
    {
        Fixture f; f.data={left,right}; f.FunctionCode({Op(0,0),Op(0,1),Op(13,operation),Op(10)});
        NativeInterpreter vm(f.Bytes()); Reject([&]{vm.ExecuteIndex(0);});
    }
    for (unsigned operation : {37u,38u})
    {
        Fixture f; f.data={0xfffffffeu};f.FunctionCode({Op(0),Op(13,operation),Op(10)});
        NativeInterpreter vm(f.Bytes());Reject([&]{vm.ExecuteIndex(0);});
    }
    for (auto program : {std::vector{Op(3),Op(2,1),Op(13,20),Op(10)}, std::vector{Op(2,1),Op(3),Op(13,4),Op(10)},
         std::vector{Op(2,0),Op(3),Op(13,10),Op(10)}, std::vector{Op(3),Op(15),Op(10)}})
    {
        Fixture f; f.strings=std::string("a\0",2); f.globals={0}; f.FunctionCode({}); f.code=program;
        NativeInterpreter vm(f.Bytes()); Reject([&]{vm.ExecuteIndex(0);});
    }
    Fixture small; small.FunctionCode({Op(2,1),Op(10)});
    NativeInterpreter vm(small.Bytes(),{2,2,10,10}); Reject([&]{vm.ExecuteIndex(0);});
    Fixture arguments; arguments.FunctionCode({Op(10,8)},2,4); NativeInterpreter args(arguments.Bytes(),{4,2,10,10});
    std::uint32_t values[4]{}; Reject([&]{args.ExecuteIndex(0,values);});
    Check(args.Status()==InterpreterStatus::Ready,"Preflight wrote an oversized initial argument/frame");
}
void Budgets()
{
    for (unsigned opcode : {5u,7u})
    {
        Fixture f; f.FunctionCode({Op(opcode,0)}); NativeInterpreter vm(f.Bytes(),{8,4,17,10});
        Reject([&]{vm.ExecuteIndex(0);}); Check(vm.Instructions()==17,"Loop did not stop exactly at instruction budget");
    }
    Fixture recurse; recurse.FunctionCode({Op(9),Op(10)});
    for (auto limits : {InterpreterLimits{32,3,100,10},InterpreterLimits{4,16,100,10}})
    { NativeInterpreter vm(recurse.Bytes(),limits); Reject([&]{vm.ExecuteIndex(0);}); }
    Fixture calls; calls.FunctionCode({Op(8),Op(8),Op(10)}); NativeInterpreter vm(calls.Bytes(),{8,4,100,1});
    unsigned called=0; vm.Bind({0,{},false,[&](auto){++called;return InterpreterHostResult{};}});
    Reject([&]{vm.ExecuteIndex(0);}); Check(called==1&&vm.HostCalls()==1,"Host-call budget permitted an extra service");
}
void HostErrorsAndOwnership()
{
    Fixture f; f.strings=std::string("original service name\0",22); f.globals={1};
    f.FunctionCode({Op(2,7),Op(15),Op(3),Op(8),Op(10)});
    NativeInterpreter vm(f.Bytes());
    vm.Bind({0,{InterpreterValueKind::String},false,[&](auto args){
        Check(std::get<std::string>(args[0])=="original service name","Typed host string was not resolved from bytecode");
        if constexpr(sizeof(void*)>4) Check(reinterpret_cast<std::uintptr_t>(std::get<std::string>(args[0]).data())>0xffffffffu,"Host string did not exercise 64-bit storage");
        Reject([&]{vm.ExecuteIndex(0);});Reject([&]{vm.Reset();});Reject([&]{vm.Resume();});
        Reject([&]{vm.Bind({1,{},false,[](auto){return InterpreterHostResult{};}});});
        throw std::runtime_error("host service failed"); return InterpreterHostResult{};}});
    Reject([&]{vm.ExecuteIndex(0);});Check(vm.Status()==InterpreterStatus::Failed,"Host exception was hidden");
    vm.Reset();Check(vm.Globals()[0]==7,"Failure reset silently rolled back original global changes");
    vm.Reset(true);Check(vm.Globals()[0]==1,"Explicit reset did not restore initial globals");
    bool rejected=false;std::thread other([&]{try{vm.ExecuteIndex(0);}catch(const std::logic_error&){rejected=true;}});other.join();
    Check(rejected&&vm.Status()==InterpreterStatus::Ready,"Wrong-thread execution changed owner state");
    Reject([&]{vm.Bind({0,{},false,[](auto){return InterpreterHostResult{};}});});
    Reject([&]{vm.Bind({2048,{},false,[](auto){return InterpreterHostResult{};}});});
    Reject([&]{vm.Bind({1,{},false,{}});});
    Fixture missing; missing.FunctionCode({Op(8),Op(10)}); NativeInterpreter absent(missing.Bytes());Reject([&]{absent.ExecuteIndex(0);});
    for (bool mismatch : {false,true})
    {
        NativeInterpreter wrong(missing.Bytes());wrong.Bind({0,{},mismatch,[=](auto){return InterpreterHostResult{mismatch?std::optional<std::uint32_t>{}:std::optional<std::uint32_t>{1}};}});
        Reject([&]{wrong.ExecuteIndex(0);});
    }
    for (auto response : {InterpreterHostResult{1,InterpreterFlow::Retry},InterpreterHostResult{{},static_cast<InterpreterFlow>(99)}})
    {
        NativeInterpreter wrong(missing.Bytes());wrong.Bind({0,{},false,[=](auto){return response;}});
        Reject([&]{wrong.ExecuteIndex(0);});Check(wrong.Status()==InterpreterStatus::Failed,"Invalid host flow was accepted");
    }
    Fixture typed;typed.FunctionCode({Op(2,1),Op(8),Op(10)});NativeInterpreter wrong(typed.Bytes());
    bool reached=false;
    wrong.Bind({0,{InterpreterValueKind::String},false,[&](auto){reached=true;return InterpreterHostResult{};}});
    Reject([&]{wrong.ExecuteIndex(0);});
    Check(!reached,"Wrong-type host arguments reached the service");
}
void UnsupportedProfiles()
{
    Fixture f;f.globals={0};f.FunctionCode({Op(10)});auto bytes=f.Bytes();bytes[47]=1;
    Reject([&]{NativeInterpreter vm(bytes);}); // numStringRefs: original loop does not advance its relocation pointer.
    bytes=f.Bytes();bytes[31]=0; // numGlobals0 with one unqualified tweak variable.
    Reject([&]{NativeInterpreter vm(bytes);});
    Fixture empty;NativeInterpreter vm(empty.Bytes());Check(!vm.Execute(1),"Empty function table searched invalid memory");
    for(auto limits:{InterpreterLimits{1,1,10,1},InterpreterLimits{65537,1,10,1},InterpreterLimits{10,0,10,1},InterpreterLimits{10,1,0,0}})
        Reject([&]{NativeInterpreter invalid(f.Bytes(),limits);});
}
}
int main()
{
    try
    {
        for(unsigned session=0;session<3;++session)
        { Numbers();Strings();Locals();Calls();BranchesAndGlobals();Pausing();MalformedExecution();Budgets();HostErrorsAndOwnership();UnsupportedProfiles(); }
        std::cout<<checks<<" original interpreter checks passed across three owned sessions\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}

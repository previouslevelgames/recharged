#include "platform/dsp_instruction_core.h"
#include "platform/dsp_control_abi.h"
#include "platform/dsp_memory_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "dsp_boot_data.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <SDL3/SDL_loadso.h>
extern "C" {
#include <revolution/dsp.h>
}
#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <initializer_list>

namespace {
using namespace mscharged::platform;
unsigned checks;
void Check(bool result,const char* text) {++checks;if(!result)throw std::runtime_error(text);}
template<class F> void Throws(F&& action,const char* text) {
    bool held=false;try{action();}catch(const std::exception&){held=true;}Check(held,text);
}
struct SDKLifetime {bool live{};~SDKLifetime(){if(live)aurora_shutdown();}};
void PutWord(unsigned char* bytes,std::size_t word,std::uint16_t value) {
    bytes[word*2]=static_cast<unsigned char>(value>>8);bytes[word*2+1]=static_cast<unsigned char>(value);
}
void Same(const DSPInstructionRegisters& a,const DSPInstructionRegisters& b) {
    Check(a.pc==b.pc && a.status==b.status && a.control==b.control && a.accumulator==b.accumulator &&
          a.instructions==b.instructions && a.address==b.address && a.index==b.index && a.wrap==b.wrap &&
          a.stack==b.stack && a.loop_stack==b.loop_stack && a.loop_depth==b.loop_depth,
          "failed hardware loop request changed a retained register/stack cell");
}
void LoopGate(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
              NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    const auto Program=[&](std::initializer_list<std::uint16_t> words) {
        std::size_t i=0;for(auto word:words)PutWord(backing,i++,word);
        DSPInstructionCore core(mailboxes,control);
        core.LoadInstructionMemory(memory,physical,static_cast<std::uint32_t>(i*2),0x100);
        return core;
    };
    DSPInstructionRegisters input{0x100,0xa5a5,0x9c,{0x12abcdef34ULL,0x89fedcba21ULL},7,
        {0x1111,0x2222,0x3333,0x4444},{0x5555,0x6666,0x7777,0x8888},
        {0x9999,0xaaaa,0xbbbb,0xcccc}};
    input.stack={0xbabe,0xdead,0x7777,0};
    // A single one-word endpoint with independently specified execution trace.
    for(unsigned reg=0;reg<12;++reg) {
        auto context=input;
        if(reg<4)context.address[reg]=3;
        else if(reg<8)context.index[reg-4]=3;
        else context.wrap[reg-8]=3;
        auto single=Program({static_cast<std::uint16_t>(0x0060+reg),0x0103,0,0,0});
        single.BeginExecution(context);
        constexpr std::array<std::uint16_t,7> pcs{0x102,0x103,0x102,0x103,0x102,0x103,0x104};
        constexpr std::array<std::uint16_t,7> counts{3,3,2,2,1,1,0};
        for(std::size_t i=0;i<pcs.size();++i) {
            const auto actual=single.Step();
            Check(actual.pc==pcs[i] && actual.instructions==context.instructions+i+1,
                  "BLOOP inclusive one-word endpoint/order differs from independent instruction trace");
            Check(actual.status==context.status && actual.control==context.control &&
                  actual.accumulator==context.accumulator && actual.address==context.address &&
                  actual.index==context.index && actual.wrap==context.wrap && actual.stack[1]==context.stack[1],
                  "BLOOP changed its authored counter register, flags or unrelated hardware cells");
            if(counts[i])Check(actual.loop_depth==1 && actual.stack[0]==0x102 &&
                actual.stack[2]==0x103 && actual.stack[3]==counts[i],"BLOOP live stack counter/repeat/endpoint differs");
            else Check(actual.loop_depth==0 && actual.stack==context.stack,
                       "BLOOP completion failed to restore actual previous ST0/ST2/ST3 cells");
        }
        Check(single.Registers().loop_stack[0]==std::array<std::uint16_t,3>{0xbabe,0x7777,0},
              "loop backing lost the exact saved previous tops");
    }
    // A two-level sequence, authored independently as next-PC/depth/count cells.
    auto nested=Program({0x0064,0x0106,0x0065,0x0105,0,0,0,0});
    auto context=input;context.index[0]=2;context.index[1]=3;nested.BeginExecution(context);
    struct Expected {std::uint16_t pc,count;std::uint8_t depth;};
    constexpr std::array<Expected,17> trace{{
        {0x102,2,1},{0x104,3,2},{0x105,3,2},{0x104,2,2},{0x105,2,2},{0x104,1,2},
        {0x105,1,2},{0x106,2,1},{0x102,1,1},{0x104,3,2},{0x105,3,2},{0x104,2,2},
        {0x105,2,2},{0x104,1,2},{0x105,1,2},{0x106,1,1},{0x107,0,0}}};
    for(std::size_t i=0;i<trace.size();++i) {
        const auto actual=nested.Step();const auto expected=trace[i];
        Check(actual.pc==expected.pc && actual.loop_depth==expected.depth &&
              actual.instructions==context.instructions+i+1,"nested hardware loop traversal/pop order differs");
        if(expected.depth)Check(actual.stack[3]==expected.count &&
              actual.stack[0]==(expected.depth==2?0x104:0x102) &&
              actual.stack[2]==(expected.depth==2?0x105:0x106),"nested hardware top/counter differs");
        else Check(actual.stack==context.stack,"nested loop failed to restore authored baseline");
        Check(actual.index==context.index && actual.status==context.status && actual.stack[1]==0xdead,
              "nested loop changed source count registers, flags or data stack");
    }
    auto full=Program({0x0064,0x010b,0x0065,0x010a,0x0066,0x0109,0x0067,0x0108,0,0,0,0});
    context=input;context.index={1,1,1,1};full.BeginExecution(context);
    for(unsigned i=0;i<4;++i) {
        const auto actual=full.Step();
        Check(actual.loop_depth==i+1 && actual.pc==0x102+i*2 && actual.stack[3]==1,
              "actual four-level hardware capacity/push order differs");
    }
    for(unsigned i=0;i<4;++i) {
        const auto actual=full.Step();
        Check(actual.loop_depth==3-i && actual.pc==0x109+i,
              "four-level hardware capacity/pop order differs");
    }
    Check(full.Registers().stack==context.stack,"four-level loop lost prior stack cells");
    // Raw unsigned16-bit counter must not become signed, narrowed or eager iterations.
    auto maximal=Program({0x0064,0x0102,0,0});context=input;context.index[0]=0xffff;
    maximal.BeginExecution(context);auto actual=maximal.Step();
    Check(actual.stack[3]==0xffff && actual.instructions==8,"BLOOP eagerly ran or narrowed max counter");
    actual=maximal.Step();
    Check(actual.pc==0x102 && actual.stack[3]==0xfffe && actual.index[0]==0xffff,
          "hardware endpoint did not decrement exactly one unsigned count");
    // Counter0 skips the endpoint opcode, respecting one- and two-word sizes;
    // no skipped LRI write or immediate/operand execution is permitted.
    for(bool two_words:{false,true}) {
        auto skip=Program({0x0064,0x0104,0xdead,0xbeef,
            static_cast<std::uint16_t>(two_words?0x0080:0),0xabcd,0});
        context=input;context.index[0]=0;skip.BeginExecution(context);actual=skip.Step();
        auto expected=context;expected.pc=two_words?0x106:0x105;++expected.instructions;Same(actual,expected);
    }
    const auto Rejected=[&](DSPInstructionCore& core,const DSPInstructionRegisters& before) {
        core.BeginExecution(before);Throws([&]{core.Step();},"unsupported loop state/opcode/extent silently ran");
        Same(core.Registers(),before);
    };
    auto unknown_reg=Program({0x006c,0x0102,0});Rejected(unknown_reg,input);
    auto unknown_size=Program({0x0064,0x0102,0x0210});context=input;context.index[0]=0;Rejected(unknown_size,context);
    auto missing_end=Program({0x0064,0x0110,0});Rejected(missing_end,context);
    auto missing_operand=Program({0x0064});Rejected(missing_operand,input);
    auto overflow=Program({0x0064,0x0102,0});context=input;context.index[0]=1;
    context.loop_depth=4;context.stack={0x100,0xdead,0x101,1};
    Rejected(overflow,context);Check(!DSPCheckInit() && !DSPCheckMailFromDSP(),
          "unsupported STOVF fabricated exception ROM, source initialization or mail");
    auto invalid=input;invalid.loop_depth=5;Throws([&]{overflow.BeginExecution(invalid);},
        "invalid externally supplied loop depth was accepted");Same(overflow.Registers(),context);
    invalid=input;invalid.stack[3]=1;Throws([&]{overflow.BeginExecution(invalid);},
        "unknown active native stack history was silently invented");Same(overflow.Registers(),context);
}

std::uint64_t LoadedCell(std::uint64_t previous,std::uint16_t word,bool extend) {
    if(extend) {
        const std::int64_t signed_word=word<0x8000?word:std::int64_t(word)-65536;
        return static_cast<std::uint64_t>(signed_word*65536)&0xffffffffffULL;
    }
    return (previous/0x100000000ULL)*0x100000000ULL + std::uint64_t(word)*65536 + previous%65536;
}
void InstructionReadGate(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
                         NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    DSPInstructionRegisters input{0x100,0x19ad,0xa3,{0x12abcdef34ULL,0x89fedcba21ULL},5,
        {0x0200,0x0200,0x0200,0x0200},{0x1111,0x2222,0x3333,0x4444},
        {0xffff,0xffff,0xffff,0xffff}};
    input.stack={0xbeef,0xabcd,0x1234,0};
    const auto Reader=[&](unsigned destination,unsigned address,std::uint16_t word) {
        PutWord(backing,0,static_cast<std::uint16_t>(0x0218+destination*0x100+address));
        PutWord(backing,1024,word);PutWord(backing,1025,static_cast<std::uint16_t>(~word));
        DSPInstructionCore core(mailboxes,control);
        core.LoadInstructionMemory(memory,physical,2,0x100);
        core.LoadInstructionMemory(memory,physical+2048,2,0x200);
        core.LoadDataMemory(memory,physical+2050,2,0x200);
        return core;
    };
    constexpr std::array<std::uint16_t,5> words{0,1,0x7fff,0x8000,0xffff};
    for(unsigned selected=0;selected<2;++selected) {
        for(unsigned source=0;source<4;++source) {
            const auto word=words[(source+selected)%words.size()];
            auto context=input;if(source&1)context.status|=0x4000;
            auto core=Reader(selected,source,word);core.BeginExecution(context);
            auto expected=context;++expected.pc;++expected.instructions;++expected.address[source];
            expected.accumulator[selected]=LoadedCell(context.accumulator[selected],word,(context.status&0x4000)!=0);
            Same(core.Step(),expected);
            Check(core.DataWord(0x200)==static_cast<std::uint16_t>(~word),
                  "ILRRI read data memory instead of independent instruction memory");
        }
    }
    for(bool extend:{false,true})for(auto word:words) {
        auto context=input;if(extend)context.status|=0x4000;
        auto core=Reader(0,0,word);core.BeginExecution(context);
        auto expected=context;++expected.pc;++expected.instructions;++expected.address[0];
        expected.accumulator[0]=LoadedCell(context.accumulator[0],word,extend);Same(core.Step(),expected);
    }
    auto wrap=Reader(0,0,0x8000);auto context=input;context.wrap[0]=0x0fff;
    wrap.BeginExecution(context);Throws([&]{wrap.Step();},"unqualified circular address mode was silently guessed");
    Same(wrap.Registers(),context);
    // IRAM is present but the actual source IROM is absent: no dummy memory word.
    auto absent=Reader(0,0,0);context=input;context.address[0]=0x8000;
    absent.BeginExecution(context);Throws([&]{absent.Step();},"absent IROM was replaced with a successful zero read");
    Same(absent.Registers(),context);
    auto coefficient=Reader(0,0,0);context=input;context.address[0]=0x1000;
    PutWord(backing,4096,0x8123);coefficient.LoadCoefficientROM(memory,physical+8192);
    Check(coefficient.DataWord(0x1000)==0x8123,"fixture coefficient domain was not actually supplied");
    coefficient.BeginExecution(context);Throws([&]{coefficient.Step();},"coefficient data bank became instruction memory");
    Same(coefficient.Registers(),context);
    PutWord(backing,0,0x0064);PutWord(backing,1,0x0102);PutWord(backing,2,0x0218);
    DSPInstructionCore skipped(mailboxes,control);skipped.LoadInstructionMemory(memory,physical,6,0x100);
    context=input;context.index[0]=0;context.wrap[0]=0;context.address[0]=0x8000;skipped.BeginExecution(context);
    auto expected=context;expected.pc=0x103;++expected.instructions;
    Same(skipped.Step(),expected); // Known size only; no read, write or wrap request occurred.
}
void RegisterMoveGate(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
                      NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    const auto Mover=[&](unsigned destination,unsigned source) {
        PutWord(backing,0,static_cast<std::uint16_t>(0x1c00u+destination*32u+source));
        DSPInstructionCore core(mailboxes,control);
        core.LoadInstructionMemory(memory,physical,2,0x100);return core;
    };
    DSPInstructionRegisters input{0x100,0x19ad,0xa3,{0x0012345678ULL,0xff87654321ULL},5,
        {0x1111,0x2222,0x3333,0x4444},{0x5555,0x6666,0x7777,0x8888},
        {0x9999,0xaaaa,0xbbbb,0xcccc}};
    input.stack={0xbeef,0xabcd,0x1234,0};
    // Independently authored raw register words and a destination permutation
    // exercise every qualified source/destination bit field in both modes.
    constexpr std::array<unsigned,14> registers{0,1,2,3,4,5,6,7,8,9,10,11,30,31};
    constexpr std::array<std::uint16_t,14> words{
        0x1111,0x2222,0x3333,0x4444,0x5555,0x6666,0x7777,0x8888,
        0x9999,0xaaaa,0xbbbb,0xcccc,0x1234,0x8765};
    const auto Expected=[&](DSPInstructionRegisters context,unsigned destination,std::uint16_t word) {
        ++context.pc;++context.instructions;
        if(destination<4)context.address[destination]=word;
        else if(destination<8)context.index[destination-4]=word;
        else if(destination<12)context.wrap[destination-8]=word;
        else context.accumulator[destination-30]=LoadedCell(
            context.accumulator[destination-30],word,(context.status&0x4000)!=0);
        return context;
    };
    for(bool extend:{false,true})for(unsigned i=0;i<registers.size();++i) {
        auto context=input;if(extend)context.status|=0x4000;
        const auto destination=registers[(i*5u+3u)%registers.size()];
        auto core=Mover(destination,registers[i]);core.BeginExecution(context);
        Same(core.Step(),Expected(context,destination,words[i]));
    }
    struct Boundary {std::uint64_t cell;std::uint16_t saturated;};
    // Literal signed40 boundary oracle, independent of the decoder's signed
    // conversion and comparisons; raw words are used directly in16-bit mode.
    constexpr std::array<Boundary,12> boundaries{{
        {0x0000000000ULL,0},{0x0000000001ULL,0},{0x007fffffffULL,0x7fff},
        {0x0080000000ULL,0x7fff},{0x0100000000ULL,0x7fff},{0x7fffffffffULL,0x7fff},
        {0x8000000000ULL,0x8000},{0xff00000000ULL,0x8000},{0xff7fffffffULL,0x8000},
        {0xff80000000ULL,0x8000},{0xfffffffffeULL,0xffff},{0xffffffffffULL,0xffff}}};
    for(unsigned selected=0;selected<2;++selected)for(bool extend:{false,true})
        for(const auto boundary:boundaries) {
            auto context=input;if(extend)context.status|=0x4000;
            context.accumulator[selected]=boundary.cell;
            const auto word=extend?boundary.saturated:static_cast<std::uint16_t>(boundary.cell/65536u);
            auto core=Mover(3,30+selected);core.BeginExecution(context);
            Same(core.Step(),Expected(context,3,word));
            // Self moves must read/saturate before overwriting their source,
            // and can clear low/high cells only in the real40-bit mode.
            auto self=Mover(30+selected,30+selected);self.BeginExecution(context);
            Same(self.Step(),Expected(context,30+selected,word));
        }
    for(unsigned selected=0;selected<2;++selected)for(bool extend:{false,true})
        for(auto word:std::array<std::uint16_t,5>{0,1,0x7fff,0x8000,0xffff}) {
            auto context=input;if(extend)context.status|=0x4000;context.index[0]=word;
            auto core=Mover(30+selected,4);core.BeginExecution(context);
            Same(core.Step(),Expected(context,30+selected,word));
        }
    // Unmodeled real register families stay explicit. Never invent their
    // stack pop/push, product, auxiliary or high/low/control/status effects.
    for(unsigned unqualified:{12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29})
        for(bool source:{false,true}) {
            auto core=Mover(source?0:unqualified,source?unqualified:0);
            core.BeginExecution(input);bool held=false;
            try{core.Step();}catch(const DSPUnsupportedInstruction& gap) {
                held=gap.pc==0x100 && gap.operand==unqualified;
            }
            Check(held,"unqualified MRR register family silently ran or lost exact operand");
            Same(core.Registers(),input);
        }
    // Known MRR width is enough for a zero-count skip. This unsupported stack
    // read must not execute or acquire guessed history merely to skip it.
    PutWord(backing,0,0x0064);PutWord(backing,1,0x0102);PutWord(backing,2,0x1c0c);
    DSPInstructionCore skip(mailboxes,control);skip.LoadInstructionMemory(memory,physical,6,0x100);
    auto context=input;context.index[0]=0;skip.BeginExecution(context);
    auto expected=context;expected.pc=0x103;++expected.instructions;Same(skip.Step(),expected);
    // A real MRR can be the inclusive BLOOP endpoint without eager execution
    // or source counter mutation. The second visit copies the original source.
    PutWord(backing,0,0x0064);PutWord(backing,1,0x0102);PutWord(backing,2,0x1c1e);
    DSPInstructionCore loop(mailboxes,control);loop.LoadInstructionMemory(memory,physical,6,0x100);
    context=input;context.index[0]=2;loop.BeginExecution(context);
    auto state=loop.Step();Check(state.pc==0x102 && state.stack[3]==2,"MRR endpoint loop setup differs");
    state=loop.Step();Check(state.pc==0x102 && state.stack[3]==1 && state.address[0]==0x1234 &&
        state.index==context.index && state.status==context.status,"MRR endpoint repeat/order differs");
    state=loop.Step();expected=Expected(context,0,0x1234);expected.pc=0x103;
    expected.instructions=context.instructions+3;expected.loop_stack[0]={0xbeef,0x1234,0};
    Same(state,expected);
}

void SingleInstructionLoopGate(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
                               NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    const auto Program=[&](std::initializer_list<std::uint16_t> words) {
        std::size_t i=0;for(auto word:words)PutWord(backing,i++,word);
        DSPInstructionCore core(mailboxes,control);
        core.LoadInstructionMemory(memory,physical,static_cast<std::uint32_t>(i*2),0x100);
        return core;
    };
    DSPInstructionRegisters input{0x100,0x19ad,0x9c,{0x120003abcdULL,0xfe0003dcbaULL},11,
        {0x1111,0x2222,0x3333,0x4444},{0x5555,0x6666,0x7777,0x8888},
        {0x9999,0xaaaa,0xbbbb,0xcccc}};
    input.stack={0xbabe,0xdead,0x7777,0};
    // Literal hardware instruction traces, authored without the candidate's
    // decoder helpers: LOOP itself executes once; the following NOP executes3.
    constexpr std::array<unsigned,14> regs{0,1,2,3,4,5,6,7,8,9,10,11,30,31};
    constexpr std::array<std::uint16_t,4> pcs{0x101,0x101,0x101,0x102};
    constexpr std::array<std::uint16_t,4> counts{3,2,1,0};
    for(unsigned reg:regs) {
        auto context=input;
        if(reg<4)context.address[reg]=3;
        else if(reg<8)context.index[reg-4]=3;
        else if(reg<12)context.wrap[reg-8]=3;
        auto core=Program({static_cast<std::uint16_t>(0x0040+reg),0,0});
        core.BeginExecution(context);
        for(unsigned i=0;i<pcs.size();++i) {
            const auto actual=core.Step();
            Check(actual.pc==pcs[i] && actual.instructions==12u+i && actual.loop_depth==(i==3?0:1),
                  "LOOP repeat order/instruction count differs from literal trace");
            Check(actual.status==context.status && actual.control==context.control &&
                  actual.accumulator==context.accumulator && actual.address==context.address &&
                  actual.index==context.index && actual.wrap==context.wrap && actual.stack[1]==0xdead,
                  "LOOP changed original counter register, flags or unrelated cells");
            if(counts[i])Check(actual.stack[0]==0x101 && actual.stack[2]==0x101 &&
                actual.stack[3]==counts[i],"LOOP hardware repeat/endpoint/counter cell differs");
            else Check(actual.stack==context.stack,"LOOP did not restore prior source stack tops");
        }
        Check(core.Registers().loop_stack[0]==std::array<std::uint16_t,3>{0xbabe,0x7777,0},
              "LOOP backing lost exact prior saved stack cells");
    }
    // Signed40 saturation is an initial read, never signed iteration or an
    // altered authored accumulator. Large counts need only two actual steps.
    struct Counter {std::uint64_t cell;std::uint16_t word;};
    constexpr std::array<Counter,6> saturated{{
        {0x0100000000ULL,0x7fff},{0x7fffffffffULL,0x7fff},
        {0x8000000000ULL,0x8000},{0xff00000000ULL,0x8000},
        {0xff80000000ULL,0x8000},{0xffffffffffULL,0xffff}}};
    for(unsigned selected:{0u,1u})for(auto value:saturated) {
        auto context=input;context.status|=0x4000;context.accumulator[selected]=value.cell;
        auto core=Program({static_cast<std::uint16_t>(0x005eu + selected),0,0});
        core.BeginExecution(context);auto actual=core.Step();
        Check(actual.pc==0x101 && actual.stack[3]==value.word && actual.accumulator==context.accumulator,
              "LOOP ACM initial saturation narrowed or changed source hardware cells");
        actual=core.Step();
        Check(actual.pc==0x101 && actual.stack[3]==value.word-1u && actual.status==context.status,
              "LOOP large raw counter was signed, eagerly consumed or updated flags");
    }
    for(auto count:std::array<std::uint16_t,3>{1,2,0xffff}) {
        auto context=input;context.index[0]=count;
        auto core=Program({0x0044,0,0});core.BeginExecution(context);
        auto actual=core.Step();Check(actual.stack[3]==count && actual.pc==0x101,"LOOP initial raw unsigned count differs");
        actual=core.Step();
        Check(actual.pc==(count==1?0x102:0x101) && actual.loop_depth==(count==1?0:1) &&
              actual.index[0]==count && actual.instructions==13,
              "LOOP zero transition/preserve authored count differs");
        if(count!=1)Check(actual.stack[3]==count-1u,"LOOP high-bit raw count decremented incorrectly");
    }
    // Zero iteration observes only declared width, never skipped body effects.
    for(auto body:std::array<std::uint16_t,6>{0,0x0080,0x004c,0x1d8c,0x1b1e,0x006c}) {
        auto context=input;context.index[0]=0;
        auto core=Program({0x0044,body,0xabcd,0});core.BeginExecution(context);
        auto expected=context;expected.pc=(body==0x0080 || body==0x006c)?0x103:0x102;++expected.instructions;
        Same(core.Step(),expected);
    }
    // Count is copied once; the repeated body may change its source register.
    auto context=input;context.address[0]=3;context.index[0]=9;
    auto changed=Program({0x0040,0x1c04,0});changed.BeginExecution(context);
    changed.Step();auto actual=changed.Step();
    Check(actual.address[0]==9 && actual.stack[3]==2 && actual.pc==0x101,
          "LOOP reread an authored counter register changed by its real body");
    changed.Step();actual=changed.Step();
    Check(actual.address[0]==9 && actual.pc==0x102 && actual.loop_depth==0 && actual.stack==context.stack,
          "LOOP changed body counter ownership or failed final source pop");
    // An outer block has a separate inclusive endpoint; its original saved
    // counter resumes after each inner single-instruction loop completes.
    auto nested=Program({0x0064,0x0105,0x0045,0,0,0,0});
    context=input;context.index[0]=2;context.index[1]=3;nested.BeginExecution(context);
    struct Expected {std::uint16_t pc,count;std::uint8_t depth;};
    constexpr std::array<Expected,13> trace{{
        {0x102,2,1},{0x103,3,2},{0x103,2,2},{0x103,1,2},{0x104,2,1},
        {0x105,2,1},{0x102,1,1},{0x103,3,2},{0x103,2,2},{0x103,1,2},
        {0x104,1,1},{0x105,1,1},{0x106,0,0}}};
    for(unsigned i=0;i<trace.size();++i) {
        const auto value=nested.Step();const auto expected=trace[i];
        Check(value.pc==expected.pc && value.loop_depth==expected.depth && value.instructions==12u+i,
              "BLOOP/LOOP nesting traversal differs from independent literal trace");
        if(expected.depth)Check(value.stack[3]==expected.count &&
              value.stack[0]==(expected.depth==2?0x103:0x102) &&
              value.stack[2]==(expected.depth==2?0x103:0x105),"nested LOOP lost retained outer counter/endpoint");
        else Check(value.stack==context.stack,"nested LOOP lost prior source stack baseline");
        Check(value.index==context.index && value.status==context.status && value.accumulator==context.accumulator,
              "nested LOOP changed original count registers/flags/accumulators");
    }
    const auto Rejected=[&](DSPInstructionCore& core,const DSPInstructionRegisters& before) {
        core.BeginExecution(before);Throws([&]{core.Step();},"unsupported LOOP body/register/history silently advanced");
        Same(core.Registers(),before);
    };
    auto unknown_reg=Program({0x004c,0});Rejected(unknown_reg,input);
    auto unknown_body=Program({0x0044,0x0210});context=input;context.index[0]=0;Rejected(unknown_body,context);
    auto missing=Program({0x0044});Rejected(missing,context);
    auto positive_wide=Program({0x0044,0x0080,0xabcd});Rejected(positive_wide,input);
    auto nested_single=Program({0x0044,0x0045,0});Rejected(nested_single,input);
    auto overflow=Program({0x0044,0});context=input;context.index[0]=1;
    context.loop_depth=4;context.stack={0x100,0xdead,0x101,1};Rejected(overflow,context);
    Check(!DSPCheckInit() && !DSPCheckMailFromDSP(),"partial LOOP support fabricated ROM/init/source mail");
}

void StoreIncrementGate(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
                        NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    const auto Program=[&](std::initializer_list<std::uint16_t> words) {
        std::size_t i=0;for(auto word:words)PutWord(backing,i++,word);
        DSPInstructionCore core(mailboxes,control);
        core.LoadInstructionMemory(memory,physical,static_cast<std::uint32_t>(i*2),0x100);
        return core;
    };
    DSPInstructionRegisters input{0x100,0xa5a5,0x9c,{0x0012345678ULL,0xff89abcdefULL},7,
        {0x1111,0x2222,0x3333,0x4444},{0x5555,0x6666,0x7777,0x8888},
        {0x9999,0xaaaa,0xbbbb,0xcccc}};
    input.stack={0xbabe,0xdead,0x7777,0};
    constexpr std::array<unsigned,14> sources{0,1,2,3,4,5,6,7,8,9,10,11,30,31};
    for(unsigned destination=0;destination<4;++destination)for(unsigned source:sources) {
        auto context=input;context.address[destination]=0x2fe;context.wrap[destination]=0xffff;
        auto core=Program({static_cast<std::uint16_t>(0x1b00u+destination*32u+source)});
        // Genuine BE data transport, including before/after guards and distinct
        // IRAM at the same numerical address as the target DRAM word.
        PutWord(backing,0,0x5a7c);PutWord(backing,1,0xabcd);PutWord(backing,2,0x83f1);
        core.LoadDataMemory(memory,physical,6,0x2fd);
        PutWord(backing,0,0x1357);core.LoadInstructionMemory(memory,physical,2,0x2fe);
        std::uint16_t expected_word;
        if(source<4)expected_word=context.address[source];
        else if(source<8)expected_word=context.index[source-4];
        else if(source<12)expected_word=context.wrap[source-8];
        else expected_word=source==30?0x1234:0x89ab;
        auto expected=context;expected.address[destination]=0x2ff;expected.pc=0x101;expected.instructions=8;
        core.BeginExecution(context);Same(core.Step(),expected);
        Check(core.DataWord(0x2fe)==expected_word && core.DataWord(0x2fd)==0x5a7c &&
              core.DataWord(0x2ff)==0x83f1,"SRRI word/BE guard/source-before-increment differs");
        Check(core.InstructionWord(0x2fe)==0x1357,"SRRI data write corrupted equal-address IRAM");
    }
    struct AccumulatorCase {std::uint64_t raw;std::uint16_t narrow,saturated;};
    constexpr std::array<AccumulatorCase,8> cells{{
        {0x0000000000ULL,0x0000,0x0000},{0x007fffffffULL,0x7fff,0x7fff},
        {0x0080000000ULL,0x8000,0x7fff},{0x7fabcd4321ULL,0xabcd,0x7fff},
        {0xff80000000ULL,0x8000,0x8000},{0xff7fffffffULL,0x7fff,0x8000},
        {0x80abcd4321ULL,0xabcd,0x8000},{0xffffffffffULL,0xffff,0xffff}}};
    for(unsigned selected=0;selected<2;++selected)for(bool saturated:{false,true})for(const auto& cell:cells) {
        auto core=Program({static_cast<std::uint16_t>(0x1b1eu+selected)});
        auto context=input;context.status=saturated?0x4000:0;context.address[0]=0x3a0;context.wrap[0]=0xffff;
        context.accumulator[selected]=cell.raw;core.BeginExecution(context);
        auto expected=context;expected.address[0]=0x3a1;expected.pc=0x101;expected.instructions=8;
        Same(core.Step(),expected);
        Check(core.DataWord(0x3a0)==(saturated?cell.saturated:cell.narrow),"SRRI raw40/SXM ACM store differs from literal oracle");
        Throws([&]{core.DataWord(0x3a1);},"SRRI invented an adjacent initialized DRAM cell");
    }
    auto last=Program({0x1b1e,0x1b1e});auto context=input;context.address[0]=0xfff;context.wrap[0]=0xffff;
    last.BeginExecution(context);auto state=last.Step();
    Check(state.address[0]==0x1000 && last.DataWord(0xfff)==0x1234,"SRRI last DRAM word stride/value differs");
    const auto boundary=last.Registers();Throws([&]{last.Step();},"SRRI wrote coefficient bank after DRAM boundary");
    Same(last.Registers(),boundary);Check(last.DataWord(0xfff)==0x1234,"failed SRRI changed last valid DRAM word");
    // Explicit refusal to invent circular addressing or unsupported source cells.
    for(unsigned source:std::array<unsigned,18>{12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29}) {
        auto core=Program({static_cast<std::uint16_t>(0x1b00u+source)});context=input;
        context.address[0]=0x2fe;context.wrap[0]=0xffff;
        PutWord(backing,0,0x62c1);core.LoadDataMemory(memory,physical,2,0x2fe);core.BeginExecution(context);
        Throws([&]{core.Step();},"SRRI invented unsupported source register semantics");Same(core.Registers(),context);
        Check(core.DataWord(0x2fe)==0x62c1,"unsupported source store changed existing DRAM");
    }
    for(std::uint16_t wrap:std::array<std::uint16_t,4>{0,1,0x7ff,0xfffe}) {
        auto core=Program({0x1b1e});context=input;context.address[0]=0x2fe;context.wrap[0]=wrap;
        PutWord(backing,0,0x62c1);core.LoadDataMemory(memory,physical,2,0x2fe);core.BeginExecution(context);
        Throws([&]{core.Step();},"SRRI circular addressing falsely used linear arithmetic");Same(core.Registers(),context);
        Check(core.DataWord(0x2fe)==0x62c1,"unsupported circular store changed existing DRAM");
    }
    for(std::uint16_t address:std::array<std::uint16_t,7>{0x1000,0x1800,0x2000,0xfffb,0xfffc,0xfffd,0xffff}) {
        auto core=Program({0x1b1e});PutWord(backing,0,0x9134);core.LoadCoefficientROM(memory,physical);
        context=input;context.address[0]=address;context.wrap[0]=0xffff;core.BeginExecution(context);
        const auto csr=GetNativeDSPControlStatus().csr;
        Throws([&]{core.Step();},"SRRI wrote ROM, unmapped data or unqualified IFX");Same(core.Registers(),context);
        Check(core.DataWord(0x1000)==0x9134 && core.DataWord(0x1800)==0x9134 &&
              !DSPCheckMailFromDSP() && GetNativeDSPControlStatus().csr==csr,"unqualified write changed ROM/mail/CSR");
    }
    auto halted=Program({0x1b1e});context=input;context.address[0]=0x2fe;context.wrap[0]=0xffff;
    halted.BeginExecution(context);ChargedDSPControlWrite(4);
    Throws([&]{halted.Step();},"halted device allowed SRRI side effect");Same(halted.Registers(),context);
    Throws([&]{halted.DataWord(0x2fe);},"halted SRRI initialized DRAM");ChargedDSPControlWrite(0);
    auto loop=Program({0x0044,0x1b1e,0});context=input;context.address[0]=0x400;context.wrap[0]=0xffff;context.index[0]=3;
    loop.BeginExecution(context);state=loop.Step();
    Check(state.pc==0x101 && state.stack[3]==3,"LOOP did not select actual SRRI single-word body");
    for(unsigned i=0;i<3;++i) {
        state=loop.Step();Check(state.pc==(i==2?0x102:0x101) && state.instructions==9u+i &&
            state.address[0]==0x401u+i && state.index[0]==3 && loop.DataWord(static_cast<std::uint16_t>(0x400+i))==0x1234,
            "LOOP/SRRI source store/increment/repeat order differs from literal trace");
    }
    Check(state.loop_depth==0 && state.stack==context.stack,"LOOP/SRRI failed original counter ownership/stack pop");
    for(bool block:{false,true}) {
        auto skipped=block?Program({0x0064,0x0102,0x1b1e,0}):Program({0x0044,0x1b1e,0});
        context=input;context.index[0]=0;context.address[0]=0x400;context.wrap[0]=0xffff;
        skipped.BeginExecution(context);state=skipped.Step();
        Check(state.pc==(block?0x103:0x102) && state.instructions==8 && state.address==context.address &&
            state.loop_depth==0,"zero count skip failed exact SRRI family width or changed AR");
        Throws([&]{skipped.DataWord(0x400);},"zero-count SRRI skip executed a data write");
    }
    Check(!DSPCheckInit() && !DSPCheckMailFromDSP(),"SRRI data transport fabricated source/bootstrap readiness");
}

void DataReadIncrementGate(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
                           NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    const auto Reader=[&](unsigned selected,unsigned source,std::uint16_t word) {
        PutWord(backing,0,static_cast<std::uint16_t>(0x191eu+source*32u+selected));
        PutWord(backing,1024,word);PutWord(backing,1025,static_cast<std::uint16_t>(~word));
        DSPInstructionCore core(mailboxes,control);core.LoadInstructionMemory(memory,physical,2,0x100);
        core.LoadDataMemory(memory,physical+2048,2,0x200);core.LoadInstructionMemory(memory,physical+2050,2,0x200);
        return core;
    };
    DSPInstructionRegisters input{0x100,0x19ad,0xa3,{0x12abcdef34ULL,0x89fedcba21ULL},5,
        {0x0200,0x0200,0x0200,0x0200},{0x1111,0x2222,0x3333,0x4444},{0xffff,0xffff,0xffff,0xffff}};
    input.stack={0xbeef,0xabcd,0x1234,0};
    constexpr std::array<std::uint16_t,5> words{0,1,0x7fff,0x8000,0xffff};
    for(unsigned selected=0;selected<2;++selected)for(unsigned source=0;source<4;++source)
    for(bool extend:{false,true})for(auto word:words) {
        auto context=input;if(extend)context.status|=0x4000;
        auto core=Reader(selected,source,word);core.BeginExecution(context);
        auto expected=context;++expected.pc;++expected.instructions;++expected.address[source];
        expected.accumulator[selected]=LoadedCell(context.accumulator[selected],word,extend);Same(core.Step(),expected);
        Check(core.DataWord(0x200)==word&&core.InstructionWord(0x200)==static_cast<std::uint16_t>(~word),
              "LRRI used IRAM instead of genuine data or modified independent banks");
    }
    // Real supplied BE coefficient transport is distinct from IRAM/DRAM.
    for(std::uint16_t address:std::array<std::uint16_t,4>{0x1000,0x17ff,0x1800,0x1fff}) {
        auto core=Reader(0,0,0x2468);for(unsigned i=0;i<2048;++i)PutWord(backing+8192,i,static_cast<std::uint16_t>(i*0x187bu+0x6341u));
        core.LoadCoefficientROM(memory,physical+8192);auto context=input;context.address[0]=address;context.status|=0x4000;
        core.BeginExecution(context);auto expected=context;++expected.pc;++expected.instructions;++expected.address[0];
        const auto word=static_cast<std::uint16_t>((address%2048u)*0x187bu+0x6341u);
        expected.accumulator[0]=LoadedCell(context.accumulator[0],word,true);Same(core.Step(),expected);
        Check(core.DataWord(address)==word&&core.DataWord(0x200)==0x2468,"LRRI coefficient mirror/read changed retained DRAM");
    }
    for(std::uint16_t address:std::array<std::uint16_t,6>{0x201,0x1000,0x2000,0xfffc,0xfffd,0xffff}) {
        auto core=Reader(0,0,0);auto context=input;context.address[0]=address;core.BeginExecution(context);
        const auto csr=GetNativeDSPControlStatus().csr;
        Throws([&]{core.Step();},"LRRI invented absent coefficient, uninitialized data or unqualified IFX word");
        Same(core.Registers(),context);Check(GetNativeDSPControlStatus().csr==csr&&!DSPCheckMailFromDSP(),
            "unqualified LRRI changed CSR/mail state");
    }
    for(unsigned destination=0;destination<30;++destination) {
        PutWord(backing,0,static_cast<std::uint16_t>(0x1900u+destination));DSPInstructionCore core(mailboxes,control);
        core.LoadInstructionMemory(memory,physical,2,0x100);PutWord(backing,0,0xaaaa);core.LoadDataMemory(memory,physical,2,0x200);
        core.BeginExecution(input);Throws([&]{core.Step();},"LRRI invented an unsupported register destination");Same(core.Registers(),input);
        Check(core.DataWord(0x200)==0xaaaa,"unsupported LRRI destination modified source data");
    }
    auto circular=Reader(0,0,0x8123);auto context=input;context.wrap[0]=0x7ff;circular.BeginExecution(context);
    Throws([&]{circular.Step();},"LRRI unknown circular address mode used linear increment");Same(circular.Registers(),context);
    auto halted=Reader(0,0,0x8123);halted.BeginExecution(input);ChargedDSPControlWrite(4);
    Throws([&]{halted.Step();},"halted hardware allowed LRRI register/data effects");Same(halted.Registers(),input);ChargedDSPControlWrite(0);
    // Known opcode width permits zero-count skip without a data/wrap request.
    PutWord(backing,0,0x0064);PutWord(backing,1,0x0102);PutWord(backing,2,0x191e);
    DSPInstructionCore skipped(mailboxes,control);skipped.LoadInstructionMemory(memory,physical,6,0x100);
    context=input;context.index[0]=0;context.wrap[0]=0;context.address[0]=0xffff;skipped.BeginExecution(context);
    auto expected=context;expected.pc=0x103;++expected.instructions;Same(skipped.Step(),expected);
    Check(!DSPCheckInit()&&!DSPCheckMailFromDSP(),"LRRI data reads fabricated original ROM/init readiness");
}

void GeneratedSourceWalk(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
                         NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    // This generated bank is hardware test input, not Nintendo/FreeDSP ROM.
    // Only the IRAM instruction stream is the actual original source program.
    for(unsigned i=0;i<4096;++i)PutWord(backing,i,static_cast<std::uint16_t>(i*0x351du+0x2abcu));
    DSPInstructionCore walk(mailboxes,control);
    walk.LoadInstructionROM(memory,physical);
    for(unsigned i=0;i<2048;++i)PutWord(backing+8192,i,static_cast<std::uint16_t>(i*0x187bu+0x6341u));
    walk.LoadCoefficientROM(memory,physical+8192);
    walk.LoadInstructionMemory(memory,physical+12288,128,0);
    walk.BeginExecution({0,0xffff,0,{0,0},0});
    for(unsigned i=0;i<9;++i)walk.Step();
    Check(walk.Registers().pc==0x1c && walk.Registers().stack[3]==4096 &&
          walk.Registers().instructions==9,"genuine source loop setup differs with supplied generated bank");
    for(unsigned i=0;i<4096;++i) {
        const auto loaded=walk.Step();
        const auto endpoint=walk.Step();
        if(i==0 || i==1023 || i==2048 || i==4095) {
            const auto word=static_cast<std::uint16_t>(i*0x351du+0x2abcu);
            Check(loaded.pc==0x1d && loaded.address[0]==0x8001u+i && loaded.status==0xe1ff &&
                  loaded.accumulator[0]==LoadedCell(0,word,true) && loaded.stack[3]==4096u-i &&
                  loaded.instructions==10u+i*2u,
                  "actual source IROM walker differs from independent word/sign/address/counter oracle");
            Check(endpoint.pc==(i==4095?0x1e:0x1c) && endpoint.instructions==11u+i*2u &&
                  endpoint.loop_depth==(i==4095?0:1),"actual source NOP endpoint changed traversal/count");
        }
    }
    auto state=walk.Registers();
    Check(state.instructions==8201 && state.pc==0x1e && state.address[0]==0x9000 &&
          state.index[0]==0x1000 && state.wrap[0]==0xffff && state.loop_depth==0 && state.stack[3]==0,
          "actual source4096-word IROM walk wrapped within the wrong bank or altered authored count");
    state=walk.Step();Check(state.pc==0x1f && state.instructions==8202 && state.accumulator[0]==0 &&
          state.status==0xe1e4,"original source clear after IROM walker differs");
    state=walk.Step();Check(state.pc==0x20 && state.instructions==8203 && state.address[0]==0 &&
          state.accumulator[0]==0 && state.status==0xe1e4 && state.index[0]==0x1000,
          "original MRR did not copy the cleared middle word into AR0 with source flags/count intact");
    state=walk.Step();
    Check(state.pc==0x21 && state.instructions==8204 && state.loop_depth==1 &&
          state.stack[0]==0x21 && state.stack[2]==0x21 && state.stack[3]==0x1000 &&
          state.address[0]==0 && state.index[0]==0x1000 && state.status==0xe1e4,
          "original LOOP setup did not retain actual4096-word DRAM clear request");
    for(unsigned i=0;i<4096;++i) {
        state=walk.Step();
        Check(state.pc==(i==4095?0x22:0x21) && state.instructions==8205u+i &&
              state.address[0]==i+1 && state.index[0]==0x1000 && state.status==0xe1e4 &&
              walk.DataWord(static_cast<std::uint16_t>(i))==0,
              "actual source4096-word DRAM clear store/increment/repeat differs from independent trace");
    }
    Check(state.loop_depth==0 && state.stack[3]==0 && state.instructions==12300,
          "original DRAM clear did not pop original single-word loop state");
    state=walk.Step();Check(state.pc==0x24 && state.instructions==12301 && state.index[0]==0x800 &&
          state.address[0]==0x1000,"original nextLRI changed source addresses or request count");
    state=walk.Step();Check(state.pc==0x26 && state.instructions==12302 && state.loop_depth==1 &&
          state.stack[0]==0x26 && state.stack[2]==0x27 && state.stack[3]==0x800,
          "original nextBLOOP lost actual half-DRAM readback request");
    for(unsigned i=0;i<2048;++i) {
        const auto loaded=walk.Step(),endpoint=walk.Step();
        const auto word=static_cast<std::uint16_t>(i*0x187bu+0x6341u);
        Check(loaded.pc==0x27 && loaded.instructions==12303u+i*2 && loaded.address[0]==0x1001u+i &&
              loaded.accumulator[0]==LoadedCell(0,word,true) && loaded.status==0xe1e4 &&
              loaded.stack[3]==2048u-i && walk.DataWord(static_cast<std::uint16_t>(i))==0,
              "actual coefficient walk changed word/sign/bank/source address/flags/count");
        Check(endpoint.pc==(i==2047?0x28:0x26) && endpoint.instructions==12304u+i*2 &&
              endpoint.loop_depth==(i==2047?0:1),"actual coefficient read endpoint/pop order differs");
    }
    state=walk.Registers();Check(state.pc==0x28 && state.instructions==16398 && state.address[0]==0x1800 &&
          state.index[0]==0x800 && state.wrap[0]==0xffff && state.stack[3]==0 && state.loop_depth==0,
          "actual source coefficient walk failed exact2048-word completion/state");
    const auto before=walk.Registers();bool held=false;
    try{walk.Step();}catch(const DSPUnsupportedInstruction& gap){held=gap.pc==0x28 && gap.opcode==0x00de;}
    Check(held && !DSPCheckInit() && !DSPCheckMailFromDSP(),"generated-bank qualifier fabricated LR/source readiness");
    Same(walk.Registers(),before);
}

void Run(int argc,char** argv) {
    Check(argc==3 && std::strlen(argv[2])==64,"genuine source leaf/hash arguments required");
    const auto directory=std::filesystem::absolute("sdk-data").string();std::filesystem::create_directories(directory);
    SDKLifetime sdk;AuroraConfig config{};config.appName="DSP ROM transport qualifier";
    config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
    config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;
    config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual SDK initialization failed");sdk.live=true;OSInit();
    auto* source=SDL_LoadObject(argv[1]);Check(source!=nullptr,"whole-source data leaf failed to load");
    const auto reader=reinterpret_cast<OriginalDSPBootData(*)()>(SDL_LoadFunction(source,"OriginalDSPBootDataForFixture"));
    Check(reader!=nullptr,"actual source static initializer export missing");const auto boot=reader();
    Check(boot.count==128,"actual original DSPInitCode has incorrect extent");
    auto* backing=static_cast<unsigned char*>(OSGetMEM2ArenaLo());
    Check(reinterpret_cast<std::uintptr_t>(backing)%32==0 &&
          static_cast<unsigned char*>(OSGetMEM2ArenaHi())-backing>=16384,
          "genuine MEM2 scratch backing is not aligned/large enough");
    // No Game arena has captured this diagnostic SDK arena. Caller supplies
    // and pins real backing; generated bank cells are never claimed retail ROM.
    for(std::size_t i=0;i<4096;++i)
        PutWord(backing,i,static_cast<std::uint16_t>(i*0x351du+0x2abcu));
    for(std::size_t i=0;i<2048;++i)
        PutWord(backing+8192,i,static_cast<std::uint16_t>(i*0x187bu+0x6341u));
    std::memcpy(backing+12288,boot.bytes,boot.count);SDL_UnloadObject(source);
    const auto memory=AttachNativeDSPMEM1();
    const auto pin=PinNativeDSPMemory(backing,16384,false);
    const auto physical=ChargedDSPTaskMemoryWord(backing,16384,0);
    Check(physical==0x10000000u,"source MEM2 physical base changed");
    InitializeNativeInterruptController();const auto mailboxes=AttachNativeDSPMailboxes();
    const auto control=AttachNativeDSPControl(mailboxes);
    DSPInstructionCore empty(mailboxes,control);
    Throws([&]{empty.InstructionWord(0x8000);},"missing IROM invented reset-vector bytes");
    Throws([&]{empty.DataWord(0x1000);},"missing coefficient ROM invented data");
    Throws([&]{empty.BeginExecution({0x8000,0,0,{0,0},0});},"absent ROM created a reset context");
    Check(empty.Registers().instructions==0 && !DSPCheckInit(),"missing ROM advanced source/device state");
    DSPInstructionCore rom(mailboxes,control);
    Throws([&]{rom.LoadInstructionROM(memory,physical+16384-32);},"unowned short ROM extent silently loaded");
    Throws([&]{rom.InstructionWord(0x8000);},"failed ROM transfer installed a partial bank");
    Throws([&]{rom.LoadInstructionROM(memory,physical+16384-4096);},
           "a4096-byte partial bank was accepted as complete8192-byte instruction ROM");
    rom.LoadInstructionROM(memory,physical);rom.LoadCoefficientROM(memory,physical+8192);
    for (std::uint16_t index:std::array<std::uint16_t,8>{0,1,0x21,0x1ff,0x400,0x511,0x7fe,0x7ff}) {
        const auto instruction=static_cast<std::uint16_t>(index*0x351du+0x2abcu);
        const auto coefficient=static_cast<std::uint16_t>(index*0x187bu+0x6341u);
        Check(rom.InstructionWord(static_cast<std::uint16_t>(0x8000+index))==instruction,"IROM endian/word offset differs");
        const auto upper=static_cast<std::uint16_t>((index+2048u)*0x351du+0x2abcu);
        Check(rom.InstructionWord(static_cast<std::uint16_t>(0x8800+index))==upper,
              "independent upper IROM half incorrectly aliases its lower half");
        Check(rom.DataWord(static_cast<std::uint16_t>(0x1000+index))==coefficient,"coefficient endian/offset differs");
        Check(rom.DataWord(static_cast<std::uint16_t>(0x1800+index))==coefficient,"coefficient mirror differs");
    }
    Check(rom.InstructionWord(0x8fff)==static_cast<std::uint16_t>(4095u*0x351du+0x2abcu),
          "final4096th hardware IROM word was truncated");
    std::memset(backing,0,12288);
    Check(rom.InstructionWord(0x8000)==0x2abc && rom.DataWord(0x1000)==0x6341,
          "immutable ROM banks still alias writable source backing");
    Throws([&]{rom.LoadInstructionROM(memory,physical);},"IROM backing was replaced after installation");
    Throws([&]{rom.LoadCoefficientROM(memory,physical);},"coefficient ROM was replaced after installation");
    Throws([&]{rom.InstructionWord(0x9000);},"unmapped instruction bank invented ROM alias");
    Throws([&]{rom.DataWord(0x2000);},"unmapped data bank invented ROM alias");
    Throws([&]{rom.LoadInstructionMemory(memory,physical,32,0x8000);},"IRAM DMA overwrote a ROM bank");
    Throws([&]{rom.LoadDataMemory(memory,physical,32,0x1000);},"DRAM DMA overwrote a ROM bank");
    PutWord(backing,2048,0x029f);PutWord(backing,2049,0x0010);
    DSPInstructionCore cross(mailboxes,control);
    cross.LoadInstructionROM(memory,physical);
    cross.LoadInstructionMemory(memory,physical+12288,128,0);
    cross.BeginExecution({0x8800,0xffff,0,{0,0},0});ChargedDSPControlWrite(0);
    auto crossed=cross.Step();
    Check(crossed.pc==0x10 && crossed.instructions==1 && crossed.status==0xffff,
          "generated upper-half ROM instruction did not jump to supplied IRAM");
    crossed=cross.Step();
    Check(crossed.pc==0x11 && crossed.instructions==2 && crossed.status==0xefff,
          "ROM-to-IRAM transfer did not execute the actual source next instruction");
    Throws([&]{cross.LoadCoefficientROM(memory,physical+8192);},"ROM transport changed an executing bank context");
    // Original source bytes, explicitly begun at IRAM0 for instruction/data
    // qualification only. No reset, ROM or OSAudioSystem flow is substituted.
    DSPInstructionCore init(mailboxes,control);
    init.LoadInstructionMemory(memory,physical+12288,128,0);
    Check(init.InstructionWord(0)==0x029f && init.InstructionWord(1)==0x0010 &&
          init.InstructionWord(0x14)==0x0080 && init.InstructionWord(0x15)==0x8000,
          "independent actual source initialization-vector oracle differs");
    init.BeginExecution({0,0xffff,0,{0,0},0});ChargedDSPControlWrite(0);
    auto state=init.Step();Check(state.pc==0x10 && state.status==0xffff && state.instructions==1,
                                 "actual original vector JMP changed SR or target/count");
    for(unsigned i=0;i<4;++i)init.Step();
    Check(init.Registers().pc==0x14 && init.Registers().status==0xe1ff && init.Registers().instructions==5,
          "literal source status-clear prefix arithmetic/order differs");
    state=init.Step();
    Check(state.pc==0x16 && state.instructions==6 && state.address[0]==0x8000 &&
          state.index[0]==0 && state.wrap[0]==0 && state.status==0xe1ff,
          "original LRI AR0 did not retain its exact hardware address word");
    state=init.Step();
    Check(state.pc==0x18 && state.instructions==7 && state.address[0]==0x8000 &&
          state.wrap[0]==0xffff && state.index[0]==0 && state.status==0xe1ff,
          "original LRI WR0 altered the wrong hardware cell or status");
    state=init.Step();
    Check(state.pc==0x1a && state.instructions==8 && state.index[0]==0x1000 &&
          state.address[0]==0x8000 && state.wrap[0]==0xffff && state.status==0xe1ff,
          "original LRI IX0 differs from the4096-word IROM walk request");
    state=init.Step();
    Check(state.pc==0x1c && state.instructions==9 && state.index[0]==0x1000 &&
          state.stack[0]==0x1c && state.stack[2]==0x1d && state.stack[3]==0x1000 &&
          state.loop_depth==1 && state.address[0]==0x8000 && state.wrap[0]==0xffff && state.status==0xe1ff,
          "actual original BLOOP differs from independent IROM-walk stack/counter request");
    const auto loop_hold=init.Registers();
    bool missing_rom=false;try{init.Step();}catch(const std::out_of_range&) {missing_rom=true;}
    Check(missing_rom && !DSPCheckMailFromDSP(), "actual ILRRI invented absent IROM bytes");
    Same(init.Registers(),loop_hold);
    RegisterMoveGate(backing,memory,physical,mailboxes,control);
    StoreIncrementGate(backing,memory,physical,mailboxes,control);
    DataReadIncrementGate(backing,memory,physical,mailboxes,control);
    GeneratedSourceWalk(backing,memory,physical,mailboxes,control);
    SingleInstructionLoopGate(backing,memory,physical,mailboxes,control);
    LoopGate(backing,memory,physical,mailboxes,control);
    InstructionReadGate(backing,memory,physical,mailboxes,control);
    // Independently authored nonzero context proves each16-bit hardware family
    // cell changes alone, in either SR mode, without guessing reset defaults.
    for(unsigned reg=0;reg<12;++reg) {
        PutWord(backing,0,static_cast<std::uint16_t>(0x0080+reg));
        PutWord(backing,1,static_cast<std::uint16_t>(reg*0x132bu+0x8021u));
        DSPInstructionCore cell(mailboxes,control);
        cell.LoadInstructionMemory(memory,physical,4,0);
        DSPInstructionRegisters input{0,static_cast<std::uint16_t>((reg&1)?0xffff:0),0xa3,
            {0x12abcdef34ULL,0x5587654321ULL},9,
            {0x0101,0x0202,0x0303,0x0404},{0x1111,0x2222,0x3333,0x4444},
            {0xaaaa,0xbbbb,0xcccc,0xdddd}};
        cell.BeginExecution(input);auto expected=input;
        const auto word=static_cast<std::uint16_t>(reg*0x132bu+0x8021u);
        if(reg<4)expected.address[reg]=word;
        else if(reg<8)expected.index[reg-4]=word;
        else expected.wrap[reg-8]=word;
        expected.pc=2;expected.instructions=10;const auto actual=cell.Step();
        Check(actual.pc==expected.pc && actual.instructions==expected.instructions &&
              actual.status==expected.status && actual.control==expected.control &&
              actual.accumulator==expected.accumulator && actual.address==expected.address &&
              actual.index==expected.index && actual.wrap==expected.wrap,
              "LRI wrote another hardware cell, widened/signed the word or changed flags");
    }
    // Generated JMP oracle reaches absent IROM and stops on its actual next
    // fetch. It never synthesizes bootstrap mail, a source flag or callbacks.
    PutWord(backing,0,0x029f);PutWord(backing,1,0x8000);
    DSPInstructionCore jump(mailboxes,control);jump.LoadInstructionMemory(memory,physical,32,0);
    jump.BeginExecution({0,0x5432,7,{0x123456789aULL,0x00abcdef01ULL},0});state=jump.Step();
    Check(state.pc==0x8000 && state.status==0x5432 && state.control==7 &&
          state.accumulator[0]==0x123456789aULL && state.accumulator[1]==0xabcdef01ULL && state.instructions==1,
          "generated immediate jump altered unrelated real register cells");
    Throws([&]{jump.Step();},"jump into absent ROM fabricated an instruction");
    Check(jump.Registers().pc==0x8000 && jump.Registers().instructions==1 && !DSPCheckInit(),
          "absent ROM fabricated execution/source init");
    Throws([&]{DSPInit();},"actual DSPInit fabricated bootstrap readiness after ROM transport");
    Check(!DSPCheckInit() && !__DSP_curr_task && !NativeInterruptsEnabled(),
          "actual interrupted source init state/task differed");
    // Explicit fixture recovery of the rejected source request, not production
    // original-game cleanup or a repaired source/CRT shutdown claim.
    OSRestoreInterrupts(TRUE);
    DetachNativeDSPControl();DetachNativeDSPMailboxes();ShutdownNativeInterruptController();
    ReleaseNativeDSPMemory(pin);DetachNativeDSPMEM1();
    std::cout<<"original_dsp_init_prefix: actual128-byte source program runs JMP +4 status clears +AR0/WR0/IX0 loads; executes actual BLOOP; absentIROM holds genuine ILRRI. Separate generated-bank walk executesMRR/LOOP/4096actualSRRIstores/2048actualLRRIreads/reachesLRIFX; ROM/reset/task boot remains unavailable\n";
}
} // namespace
int main(int argc,char** argv) {
    try{Run(argc,argv);std::cout<<"native_dsp_rom: "<<checks<<" checks passed\n";return 0;}
    catch(const std::exception& failure){std::cerr<<"native_dsp_rom: "<<failure.what()<<"\n";return 1;}
}

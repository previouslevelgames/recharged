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
void GeneratedSourceWalk(unsigned char* backing,NativeDSPMemoryEndpoint memory,std::uint32_t physical,
                         NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) {
    // This generated bank is hardware test input, not Nintendo/FreeDSP ROM.
    // Only the IRAM instruction stream is the actual original source program.
    for(unsigned i=0;i<4096;++i)PutWord(backing,i,static_cast<std::uint16_t>(i*0x351du+0x2abcu));
    DSPInstructionCore walk(mailboxes,control);
    walk.LoadInstructionROM(memory,physical);
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
    const auto before=walk.Registers();bool held=false;
    try{walk.Step();}catch(const DSPUnsupportedInstruction& gap){held=gap.pc==0x1f && gap.opcode==0x1c1e;}
    Check(held && !DSPCheckInit() && !DSPCheckMailFromDSP(),"generated-bank qualifier fabricated later MRR/source readiness");
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
    GeneratedSourceWalk(backing,memory,physical,mailboxes,control);
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
    std::cout<<"original_dsp_init_prefix: actual128-byte source program runs JMP +4 status clears +AR0/WR0/IX0 loads; executes actual BLOOP; absentIROM holds genuine ILRRI. Separate generated-bank walk reachesMRR; ROM/reset/task boot remains unavailable\n";
}
} // namespace
int main(int argc,char** argv) {
    try{Run(argc,argv);std::cout<<"native_dsp_rom: "<<checks<<" checks passed\n";return 0;}
    catch(const std::exception& failure){std::cerr<<"native_dsp_rom: "<<failure.what()<<"\n";return 1;}
}

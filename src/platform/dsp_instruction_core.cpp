#include "platform/dsp_instruction_core.h"

namespace {
std::uint16_t QualifiedInstructionWords(std::uint16_t word) {
    if(word==0x029f || word==0x029c || (word&0xffe0)==0x0080 ||
       word==0x00de || word==0x00df || (word&0xfeff)==0x02a0 || word==0x00fe || word==0x00ff ||
       (word&0xff00)==0x1600 || (word&0xffe0)==0x0060)return 2;
    if(word==0 || word==0x0021 || (word&0xfff8)==0x1200 || (word&0xfff8)==0x1300 ||
       (word>=0x8a00 && word<=0x8f00 && !(word&0xff)) ||
       word==0x8100 || word==0x8900 || (word&0xfefc)==0x0218 ||
       (word&0xfc00)==0x1c00 || (word&0xffe0)==0x0040 ||
       (word&0xff80)==0x1b00 || (word&0xff80)==0x1900)return 1;
    return 0;
}
}

namespace mscharged::platform {
DSPUnsupportedInstruction::DSPUnsupportedInstruction(std::uint16_t address,std::uint16_t word,std::uint16_t detail)
    :std::runtime_error("DSP instruction core reached an unimplemented instruction/register/interface"),
     pc(address),opcode(word),operand(detail) {}
DSPInstructionCore::DSPInstructionCore(NativeDSPMailboxEndpoint mailboxes):mailboxes_(mailboxes) {}
DSPInstructionCore::DSPInstructionCore(NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control)
    :mailboxes_(mailboxes),control_(control) {}
void DSPInstructionCore::Load(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical,std::uint32_t bytes,
                             std::uint16_t word_address,bool instruction) {
    if (!bytes || (bytes&1) || word_address>=WORDS || bytes/2>WORDS-word_address)
        throw std::out_of_range("DSP memory transfer is outside its exact 16-bit word bank");
    std::array<std::uint8_t,WORDS*2> raw;
    DSPBackendReadMemory(endpoint,physical,raw.data(),bytes);
    auto& words=instruction?instructions_:data_;
    auto& valid=instruction?instruction_valid_:data_valid_;
    for(std::size_t i=0;i<bytes/2;++i) {
        // Original source byte storage is transported in bus order. Host scalar
        // endian/alignment never changes the firmware/data word it encodes.
        words[word_address+i]=static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[i*2])<<8)|raw[i*2+1]);
        valid[word_address+i]=true;
    }
}
void DSPInstructionCore::LoadInstructionMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical,
                                               std::uint32_t bytes,std::uint16_t address) {
    Load(endpoint,physical,bytes,address,true);
}
void DSPInstructionCore::LoadDataMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical,
                                        std::uint32_t bytes,std::uint16_t address) {
    Load(endpoint,physical,bytes,address,false);
}
void DSPInstructionCore::LoadROM(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical,bool instruction) {
    auto& loaded=instruction?instruction_rom_loaded_:coefficient_rom_loaded_;
    if (loaded || running_)
        throw std::logic_error("DSP ROM transport requires an unloaded non-executing bank");
    std::array<std::uint8_t,IROM_WORDS*2> raw;
    const auto count=instruction?IROM_WORDS:COEFFICIENT_WORDS;
    DSPBackendReadMemory(endpoint,physical,raw.data(),count*2);
    auto* words=instruction?instruction_rom_.data():coefficient_rom_.data();
    for (std::size_t i=0;i<count;++i)
        words[i]=static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[i*2])<<8)|raw[i*2+1]);
    loaded=true;
}
void DSPInstructionCore::LoadInstructionROM(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical) {
    LoadROM(endpoint,physical,true);
}
void DSPInstructionCore::LoadCoefficientROM(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical) {
    LoadROM(endpoint,physical,false);
}
std::uint16_t DSPInstructionCore::InstructionWord(std::uint16_t address) const {
    if ((address>>12)==8 && instruction_rom_loaded_)
        return instruction_rom_[address&(IROM_WORDS-1)];
    if(address>=WORDS || !instruction_valid_[address])
        throw std::out_of_range("DSP instruction memory/ROM word is not supplied");
    return instructions_[address];
}
std::uint16_t DSPInstructionCore::DataWord(std::uint16_t address) const {
    if ((address>>12)==1 && coefficient_rom_loaded_)
        return coefficient_rom_[address&(COEFFICIENT_WORDS-1)];
    if(address>=WORDS || !data_valid_[address])
        throw std::out_of_range("DSP data/ROM word is not supplied or written");
    return data_[address];
}
void DSPInstructionCore::RequireHardwareEndpoints(NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control) const {
    const auto c=GetNativeDSPControlStatus();const auto m=GetNativeDSPMailboxStatus();
    if(!control_ || control_->generation!=control.generation || mailboxes_.generation!=mailboxes.generation ||
       !c.connected || c.generation!=control.generation || !m.connected || m.generation!=mailboxes.generation)
        throw std::logic_error("native DSP core belongs to another or retired hardware lifetime");
}
void DSPInstructionCore::PauseHaltedExecution() {
    if(control_)RequireHardwareEndpoints(mailboxes_,*control_);
    if(!control_ || !(GetNativeDSPControlStatus().csr&4))
        throw std::logic_error("native DSP execution pause requires actual connected HALT");
    running_=false;
}
void DSPInstructionCore::BeginExecution(const DSPInstructionRegisters& context) {
    InstructionWord(context.pc);
    if(context.accumulator[0]>0xffffffffffULL || context.accumulator[1]>0xffffffffffULL)
        throw std::out_of_range("DSP register context exceeds a raw 40-bit accumulator");
    if(context.loop_depth>context.loop_stack.size() ||
       (context.loop_depth==0 && context.stack[2] && context.stack[3]))
        throw std::out_of_range("DSP loop context lacks its actual retained stack history");
    registers_=context;running_=true;
}
DSPInstructionRegisters DSPInstructionCore::Registers() const {return registers_;}
DSPInstructionRegisters DSPInstructionCore::Step() {
    if(!running_)throw std::logic_error("DSP execution has no supplied register context");
    if(control_)DSPBackendRequireInstructionExecution(*control_,registers_.status);
    const auto pc=registers_.pc;
    const auto opcode=InstructionWord(pc);
    auto next=registers_;
    std::uint16_t length=1;
    bool branch=false;
    if (opcode==0x0000) {
        // Hardware NOP leaves cells/flags unchanged.
    } else if (opcode==0x0021) {
        // The source boot program halts outside its completed loops. A HALT
        // within an active hardware loop remains unqualified rather than
        // inventing stack/endpoint effects. No unconnected success endpoint.
        if(!control_ || next.loop_depth)throw DSPUnsupportedInstruction(pc,opcode,0);
        DSPBackendHaltExecution(*control_);
        running_=false;
        // HALT stops at the actual instruction; source CPU clearing CSR HALT
        // cannot manufacture another execution context or advance this PC.
        branch=true;
    } else if ((opcode&0xffe0)==0x0040) {
        const auto reg=static_cast<unsigned>(opcode&31);
        std::uint16_t count;
        if(reg<4)count=next.address[reg];
        else if(reg<8)count=next.index[reg-4];
        else if(reg<12)count=next.wrap[reg-8];
        else if(reg==0x1e || reg==0x1f) {
            const auto cell=next.accumulator[reg-0x1e];
            const auto value=static_cast<std::int64_t>(cell)-
                ((cell&0x8000000000ULL)?0x10000000000LL:0LL);
            if((next.status&0x4000) && value>0x7fffffffLL)count=0x7fff;
            else if((next.status&0x4000) && value<(-0x80000000LL))count=0x8000;
            else count=static_cast<std::uint16_t>(cell>>16);
        } else throw DSPUnsupportedInstruction(pc,opcode,static_cast<std::uint16_t>(reg));
        const auto following=static_cast<std::uint16_t>(pc+1);
        const auto body=InstructionWord(following);
        const auto width=QualifiedInstructionWords(body);
        if(!width || (count && (width!=1 || (body&0xffe0)==0x0040)))
            // Only single-word positive LOOP bodies are qualified. Reject
            // multiword/nested-LOOP endpoint ambiguity rather than claiming
            // that the existing inclusive stack check establishes it.
            throw DSPUnsupportedInstruction(pc,opcode,body);
        if(count) {
            if(next.loop_depth==next.loop_stack.size())
                throw DSPUnsupportedInstruction(pc,opcode,0x0002);
            next.loop_stack[next.loop_depth++]={next.stack[0],next.stack[2],next.stack[3]};
            next.stack[0]=following;
            next.stack[2]=following;
            next.stack[3]=count;
        } else {
            next.pc=static_cast<std::uint16_t>(following+width);
            branch=true;
        }
    } else if ((opcode&0xffe0)==0x0060) {
        length=2;
        const auto reg=static_cast<std::uint16_t>(opcode&31);
        std::uint16_t count;
        if(reg<4)count=next.address[reg];
        else if(reg<8)count=next.index[reg-4];
        else if(reg<12)count=next.wrap[reg-8];
        else throw DSPUnsupportedInstruction(pc,opcode,reg);
        const auto endpoint=InstructionWord(static_cast<std::uint16_t>(pc+1));
        if(count) {
            if(next.loop_depth==next.loop_stack.size())
                // STOVF needs genuine exception/ROM execution, which is not
                // available. Reject this boundary without inventing it.
                throw DSPUnsupportedInstruction(pc,opcode,0x0002);
            next.loop_stack[next.loop_depth++]={next.stack[0],next.stack[2],next.stack[3]};
            next.stack[0]=static_cast<std::uint16_t>(pc+2);
            next.stack[2]=endpoint;
            next.stack[3]=count;
        } else {
            const auto last=InstructionWord(endpoint);
            const auto width=QualifiedInstructionWords(last);
            if(!width)throw DSPUnsupportedInstruction(pc,opcode,last);
            // Counter0 skips the endpoint instruction, including its operand.
            // Unknown instruction sizes remain an explicit decoder gap.
            next.pc=static_cast<std::uint16_t>(endpoint+width);
            branch=true;
        }
    } else if ((opcode&0xfefc)==0x0218) {
        const auto selected=static_cast<unsigned>((opcode>>8)&1);
        const auto source=static_cast<unsigned>(opcode&3);
        // The original init walk explicitly sets WR0=ffff. General circular
        // address modes need their own qualified hardware implementation.
        if(next.wrap[source]!=0xffff)
            throw DSPUnsupportedInstruction(pc,opcode,next.wrap[source]);
        const auto word=InstructionWord(next.address[source]);
        auto& cell=next.accumulator[selected];
        if(next.status&0x4000) {
            cell=(static_cast<std::uint64_t>(word)<<16) |
                 ((word&0x8000)?0xff00000000ULL:0ULL);
        } else cell=(cell&0xff0000ffffULL)|(static_cast<std::uint64_t>(word)<<16);
        next.address[source]=static_cast<std::uint16_t>(next.address[source]+1);
    } else if ((opcode&0xff80)==0x1900) {
        const auto source=static_cast<unsigned>((opcode>>5)&3);
        const auto destination=static_cast<unsigned>(opcode&31);
        // The original coefficient-bank walk loads accumulator middle cells.
        // Other destinations (including aliasing AR/WR writes) remain explicit
        // unsupported register/address-mode boundaries, never guessed effects.
        if(destination!=0x1e && destination!=0x1f)
            throw DSPUnsupportedInstruction(pc,opcode,static_cast<std::uint16_t>(destination));
        if(next.wrap[source]!=0xffff)
            throw DSPUnsupportedInstruction(pc,opcode,next.wrap[source]);
        const auto word=DataWord(next.address[source]);
        auto& cell=next.accumulator[destination-0x1e];
        if(next.status&0x4000)cell=(static_cast<std::uint64_t>(word)<<16) |
            ((word&0x8000)?0xff00000000ULL:0ULL);
        else cell=(cell&0xff0000ffffULL)|(static_cast<std::uint64_t>(word)<<16);
        next.address[source]=static_cast<std::uint16_t>(next.address[source]+1);
    } else if ((opcode&0xff80)==0x1b00) {
        const auto destination=static_cast<unsigned>((opcode>>5)&3);
        const auto source=static_cast<unsigned>(opcode&31);
        const auto address=next.address[destination];
        // The source init clear selects full16-bit linear AR0 addressing and
        // internal DRAM only. Read-only coefficient banks, IFX and circular
        // addressing need their own qualified device semantics.
        if(next.wrap[destination]!=0xffff)
            throw DSPUnsupportedInstruction(pc,opcode,next.wrap[destination]);
        if(address>=WORDS)throw DSPUnsupportedInstruction(pc,opcode,address);
        std::uint16_t word;
        if(source<4)word=next.address[source];
        else if(source<8)word=next.index[source-4];
        else if(source<12)word=next.wrap[source-8];
        else if(source==0x1e || source==0x1f) {
            const auto cell=next.accumulator[source-0x1e];
            const auto value=static_cast<std::int64_t>(cell)-
                ((cell&0x8000000000ULL)?0x10000000000LL:0LL);
            if((next.status&0x4000) && value>0x7fffffffLL)word=0x7fff;
            else if((next.status&0x4000) && value<(-0x80000000LL))word=0x8000;
            else word=static_cast<std::uint16_t>(cell>>16);
        } else throw DSPUnsupportedInstruction(pc,opcode,static_cast<std::uint16_t>(source));
        // Logical native cells retain the exact16-bit device word already
        // decoded from BE bus transport. Read precedes AR increment, including
        // source/destination aliasing; every rejection above leaves both banks
        // and the retained source context unchanged.
        data_[address]=word;
        data_valid_[address]=true;
        next.address[destination]=static_cast<std::uint16_t>(address+1);
    } else if ((opcode&0xfc00)==0x1c00) {
        const auto destination=static_cast<unsigned>((opcode>>5)&31);
        const auto source=static_cast<unsigned>(opcode&31);
        const auto supported=[](unsigned reg) {return reg<12 || reg==0x1e || reg==0x1f;};
        // These are the address/index/wrap and accumulator-middle cells used
        // by the source init prefix. General stack/product/AX/high/low register
        // moves need their own qualified read/write effects, never a zero cell.
        if(!supported(source))throw DSPUnsupportedInstruction(pc,opcode,source);
        if(!supported(destination))throw DSPUnsupportedInstruction(pc,opcode,destination);
        std::uint16_t word;
        if(source<4)word=next.address[source];
        else if(source<8)word=next.index[source-4];
        else if(source<12)word=next.wrap[source-8];
        else {
            const auto cell=next.accumulator[source-0x1e];
            const auto value=static_cast<std::int64_t>(cell)-
                ((cell&0x8000000000ULL)?0x10000000000LL:0LL);
            if((next.status&0x4000) && value>0x7fffffffLL)word=0x7fff;
            else if((next.status&0x4000) && value<(-0x80000000LL))word=0x8000;
            else word=static_cast<std::uint16_t>(cell>>16);
        }
        // Read before writing, including self moves. A middle-word destination
        // performs the same hardware SXM extension as other qualified loads.
        if(destination<4)next.address[destination]=word;
        else if(destination<8)next.index[destination-4]=word;
        else if(destination<12)next.wrap[destination-8]=word;
        else {
            auto& cell=next.accumulator[destination-0x1e];
            if(next.status&0x4000)cell=(static_cast<std::uint64_t>(word)<<16) |
                ((word&0x8000)?0xff00000000ULL:0ULL);
            else cell=(cell&0xff0000ffffULL)|(static_cast<std::uint64_t>(word)<<16);
        }
    } else if (opcode==0x00de || opcode==0x00df) {
        length=2;
        const auto address=InstructionWord(static_cast<std::uint16_t>(pc+1));
        // Qualified accumulator-middle destinations only. FFFC reads the
        // DSP's own outgoing high/status register, never CPU incoming mail.
        const auto word=address==0xfffc?DSPBackendMailFromHigh(mailboxes_):DataWord(address);
        auto& cell=next.accumulator[opcode-0x00de];
        if(next.status&0x4000)cell=(static_cast<std::uint64_t>(word)<<16) |
            ((word&0x8000)?0xff00000000ULL:0ULL);
        else cell=(cell&0xff0000ffffULL)|(static_cast<std::uint64_t>(word)<<16);
    } else if ((opcode&0xfeff)==0x02a0) {
        length=2;
        const auto immediate=InstructionWord(static_cast<std::uint16_t>(pc+1));
        const auto word=static_cast<std::uint16_t>(next.accumulator[(opcode>>8)&1]>>16);
        // ANDF changes only LZ. It does not write the accumulator, saturate
        // its middle cell, or update the arithmetic/sticky/mode flags.
        next.status=static_cast<std::uint16_t>((next.status&~0x0040u) |
            ((word&immediate)?0u:0x0040u));
    } else if (opcode==0x029c) {
        length=2;
        const auto destination=InstructionWord(static_cast<std::uint16_t>(pc+1));
        if(!(next.status&0x0040)) {next.pc=destination;branch=true;}
    } else if (opcode==0x029f) {
        // Unconditional immediate jump used by the original OS DSP init
        // vector. Invalid destinations remain real next-fetch failures.
        next.pc=InstructionWord(static_cast<std::uint16_t>(pc+1));
        branch=true;
    } else if((opcode&0xfff8)==0x1200 || (opcode&0xfff8)==0x1300) {
        const auto bit=static_cast<std::uint16_t>(std::uint16_t(1)<<((opcode&7)+6));
        if((opcode&0xff00)==0x1200)next.status&=static_cast<std::uint16_t>(~bit);
        else next.status|=bit;
    } else if(opcode>=0x8a00 && opcode<=0x8f00 && !(opcode&0xff)) {
        // Only the observed non-extended encoding is implemented. Extended
        // writes cannot be ignored just because the main operation is known.
        const auto selected=static_cast<unsigned>((opcode-0x8a00)/0x100);
        constexpr std::array<std::uint16_t,3> masks{0x2000,0x8000,0x4000};
        const auto mask=masks[selected/2];
        if(selected&1)next.status|=mask;
        else next.status&=static_cast<std::uint16_t>(~mask);
    } else if((opcode&0xffe0)==0x0080) {
        length=2;
        const auto immediate=InstructionWord(static_cast<std::uint16_t>(pc+1));
        const auto reg=static_cast<std::uint16_t>(opcode&31);
        if(reg<4)next.address[reg]=immediate;
        else if(reg<8)next.index[reg-4]=immediate;
        else if(reg<12)next.wrap[reg-8]=immediate;
        else if(reg==0x12)next.control=static_cast<std::uint8_t>(immediate);
        else if((reg==0x1e || reg==0x1f) && !(next.status&0x4000)) {
            auto& cell=next.accumulator[reg-0x1e];
            cell=(cell&0xff0000ffffULL)|(static_cast<std::uint64_t>(immediate)<<16);
        } else throw DSPUnsupportedInstruction(pc,opcode,reg);
    } else if(opcode==0x00fe || opcode==0x00ff) {
        length=2;
        const auto address=InstructionWord(static_cast<std::uint16_t>(pc+1));
        if(address>=WORDS || (next.status&0x4000))throw DSPUnsupportedInstruction(pc,opcode,address);
        data_[address]=static_cast<std::uint16_t>(next.accumulator[opcode-0x00fe]>>16);
        data_valid_[address]=true;
    } else if(opcode==0x8100 || opcode==0x8900) {
        next.accumulator[(opcode>>11)&1]=0;
        // Clearing a 40-bit accumulator leaves logic-zero/sticky overflow and
        // mode bits intact, sets arithmetic-zero/top-two-equal, clears C/O/S/S32.
        next.status=static_cast<std::uint16_t>((next.status&0xffc0)|0x24);
    } else if((opcode&0xff00)==0x1600) {
        length=2;
        const auto address=static_cast<std::uint16_t>(0xff00|(opcode&0xff));
        const auto immediate=InstructionWord(static_cast<std::uint16_t>(pc+1));
        if(address==0xfffc)DSPBackendMailFromWriteHigh(mailboxes_,immediate);
        else if(address==0xfffd)DSPBackendMailFromWriteLow(mailboxes_,immediate);
        else if(address==0xfffb && control_)DSPBackendWriteInterruptRequest(*control_,immediate);
        else throw DSPUnsupportedInstruction(pc,opcode,address);
    } else throw DSPUnsupportedInstruction(pc,opcode,0);
    if (!branch)next.pc=static_cast<std::uint16_t>(pc+length);
    if(next.loop_depth && next.stack[2] && next.stack[3] &&
       static_cast<std::uint16_t>(next.pc-1)==next.stack[2]) {
        --next.stack[3];
        if(next.stack[3])next.pc=next.stack[0];
        else {
            const auto& previous=next.loop_stack[--next.loop_depth];
            next.stack[0]=previous[0];next.stack[2]=previous[1];next.stack[3]=previous[2];
        }
    }
    ++next.instructions;
    registers_=next;return registers_;
}
} // namespace mscharged::platform

namespace mscharged::platform {
void DSPInstructionCore::ValidateNativeDataWords(std::uint16_t first,const std::uint16_t* expected,
                                               std::size_t count) const {
    if(!control_)throw std::logic_error("native DSP data context requires actual connected hardware");
    RequireHardwareEndpoints(mailboxes_,*control_);
    if(!expected||!count||first>=WORDS||count>WORDS-first)
        throw std::invalid_argument("native DSP data context has an invalid extent");
    for(std::size_t i=0;i<count;++i)
        if(!data_valid_[first+i]||data_[first+i]!=expected[i])
            throw std::logic_error("native DSP data context is uninitialized or changed before hardware commit");
}
void DSPInstructionCore::CommitNativeDataWords(std::uint16_t first,const std::uint16_t* expected,
                                             const std::uint16_t* after,std::size_t count) {
    if(!after)throw std::invalid_argument("native DSP data context has no actual next words");
    ValidateNativeDataWords(first,expected,count);
    for(std::size_t i=0;i<count;++i)data_[first+i]=after[i];
}
}

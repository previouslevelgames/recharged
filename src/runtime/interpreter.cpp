#include "runtime/interpreter.h"
#include "Game/InterpreterCore.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
unsigned StackSize(mscharged::InterpreterLimits limits)
{
    Require(limits.stack_words >= 2 && limits.stack_words <= 65536 && limits.frames > 0
        && limits.frames <= 4096 && limits.instructions > 0 && limits.instructions <= 10000000
        && limits.host_calls <= limits.instructions, "Invalid native interpreter limits");
    return static_cast<unsigned>(limits.stack_words);
}
}

// Native ownership replaces the console tweak/global allocation lifecycle.
// The selected interpreter retains its original u32 stack and native pointers
// to its segments; only values stored *in* that stack use checked references.
InterpreterCore::InterpreterCore(unsigned int size)
    : m_SP(nullptr), m_Header(nullptr), m_StackSegment(new u32[size]{}), m_Globals(nullptr),
      m_TweakStorage(nullptr), m_IP(nullptr), m_BP(nullptr), m_SavedSP(nullptr), m_Stop(0), m_RunState(2)
{
    Reset();
}
InterpreterCore::~InterpreterCore() { delete[] m_StackSegment; }

namespace mscharged
{
namespace
{
enum class Slot { Empty, Word, String, Code, Frame };
struct Frame
{
    std::size_t bp, record, end;
    std::uint32_t ip_reference, bp_reference;
    unsigned return_operand;
};
thread_local NativeInterpreterState* running = nullptr;
}
struct NativeInterpreterState final : InterpreterCore
{
    const std::thread::id thread = std::this_thread::get_id();
    const InterpreterLimits limits;
    const std::shared_ptr<const resources::ScriptBytecode> script;
    std::vector<FunctionEntryPoint> functions;
    std::vector<std::uint32_t> globals;
    std::vector<std::uint16_t> code_storage;
    ByteCodeHeader header{};
    std::vector<Slot> slots;
    std::vector<Frame> frames;
    std::map<unsigned, InterpreterHostCall> host;
    std::optional<InterpreterValue> result;
    std::size_t instructions = 0, host_calls = 0;
    bool busy = false, failed = false;

    NativeInterpreterState(resources::Bytes bytes, InterpreterLimits bounds)
        : InterpreterCore(StackSize(bounds)), limits(bounds), script(resources::ReadScriptBytecode(bytes)),
          globals(script->globals), slots(bounds.stack_words, Slot::Empty)
    {
        Require(script->num_variables == script->num_globals && script->tweaks.empty(),
                "Native interpreter tweak services are not selected");
        Require(script->num_string_refs == 0, "Native interpreter string-global relocation is not qualified");
        frames.reserve(limits.frames);
        functions.reserve(script->functions.size());
        for (const auto& entry : script->functions)
            functions.push_back({entry.hash, entry.offset, entry.frame_size, entry.arguments, entry.flags});
        // Original StopWithUndo transiently decrements IP before Step increments
        // it back. A real preceding element keeps a retry of logical opcode0
        // within its allocated array; instruction bounds still exclude padding.
        code_storage.reserve(script->code.size() + 1);
        code_storage.push_back(0);
        code_storage.insert(code_storage.end(), script->code.begin(), script->code.end());
        header.signature = 0xe11c2112; header.numFunctions = functions.size();
        header.globalDataSize = globals.size() * sizeof(std::uint32_t);
        header.dataSegmentSize = script->data.size() * sizeof(std::uint32_t);
        header.codeSegmentSize = script->code.size() * sizeof(std::uint16_t);
        header.stringSegmentSize = script->strings.size(); header.numGlobals = script->num_globals;
        header.firstFloatTweak = script->first_float_tweak; header.firstBoolTweak = script->first_bool_tweak;
        header.numVariables = script->num_variables; header.m_FunctionTable = functions.data();
        // Original accessors predate const segment pointers. They never write to
        // data/code/strings; each mutable segment (stack/globals) is owned above.
        header.m_DataSegment = const_cast<std::uint32_t*>(script->data.data());
        header.m_CodeSegment = code_storage.data() + 1;
        header.m_StringSegment = const_cast<std::uint8_t*>(script->strings.data());
        m_Header = &header; m_Globals = globals.data();
    }
    ~NativeInterpreterState() override
    {
        if (busy || thread != std::this_thread::get_id()) std::terminate();
    }
    void Ready(bool allow_failed = false) const
    {
        if (thread != std::this_thread::get_id()) throw std::logic_error("Interpreter requires its owner thread");
        if (busy || running) throw std::logic_error("Interpreter callbacks may not reenter or mutate an interpreter");
        if (failed && !allow_failed) throw std::logic_error("Failed interpreter requires Reset");
    }
    template<class T> std::size_t Index(const T* pointer, const T* start, std::size_t count, bool end = false) const
    {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer), base = reinterpret_cast<std::uintptr_t>(start);
        Require(pointer && address >= base && (address - base) % sizeof(T) == 0,
                "Invalid native interpreter segment pointer");
        const auto index = (address - base) / sizeof(T);
        Require(index < count || (end && index == count), "Native interpreter pointer leaves its segment");
        return index;
    }
    std::size_t SP() const { return Index(m_SP, m_StackSegment, limits.stack_words, true); }
    std::size_t IP() const { return Index(m_IP, header.m_CodeSegment, script->code.size()); }
    std::size_t Floor() const { Require(!frames.empty(), "Interpreter instruction has no active frame"); return frames.back().end; }
    void Space(std::size_t count) const { Require(count <= limits.stack_words - SP(), "Interpreter stack overflow"); }
    void Temps(std::size_t count) const
    {
        const auto sp = SP(), floor = Floor();
        Require(sp >= floor && count <= sp - floor, "Interpreter stack underflow");
    }
    void Value(std::size_t index) const
    {
        Require(slots.at(index) == Slot::Word || slots[index] == Slot::String, "Interpreter read of uninitialized or reserved stack slot");
    }
    void Word(std::size_t index) const { Require(slots.at(index) == Slot::Word, "Interpreter numeric operand has the wrong type"); }
    void String(std::size_t index, bool null_allowed = false) const
    {
        Require(slots.at(index) == Slot::String || (null_allowed && slots[index] == Slot::Word && m_StackSegment[index] == 0),
                "Interpreter string operand has the wrong type");
    }
    std::size_t Local(std::int64_t offset, bool read) const
    {
        const auto& frame = frames.back();
        Require(offset >= 0 && std::uint64_t(offset) < frame.end - frame.bp, "Interpreter local leaves its frame");
        const auto index = frame.bp + offset;
        Require(index != frame.record && index != frame.record + 1, "Interpreter attempted to access a saved frame reference");
        if (read) Value(index);
        return index;
    }
    void Drop(std::size_t count)
    {
        Temps(count); const auto sp = SP();
        std::fill(slots.begin() + sp - count, slots.begin() + sp, Slot::Empty);
    }
    void Push(Slot kind)
    {
        Space(1); slots[SP()] = kind;
    }
    InterpreterValue ReadValue(std::size_t index) const
    {
        Value(index);
        if (slots[index] == Slot::String)
        {
            const auto reference = m_StackSegment[index];
            Require(reference > 0 && reference <= script->strings.size(), "Interpreter string reference is invalid");
            return std::string(reinterpret_cast<const char*>(script->strings.data() + reference - 1));
        }
        return m_StackSegment[index];
    }
    void Enter(FunctionEntryPoint& entry, unsigned count, bool external)
    {
        const auto sp = SP();
        Require(frames.size() < limits.frames, "Interpreter frame budget exhausted");
        Require(entry.offset % 2 == 0 && entry.offset / 2 < script->code.size(), "Interpreter function offset is invalid");
        const auto extra = external && entry.flags == 3 ? 1u : 0u;
        Require(count <= sp && extra <= sp - count, "Interpreter call arguments underflow");
        const auto bp = sp - count - extra;
        if (!external) Temps(count);
        const auto first_argument = bp + (entry.flags == 3 ? 1u : 0u);
        Require(first_argument <= sp, "Interpreter return slot is missing");
        for (auto i = first_argument; i < sp; ++i) Value(i);
        Space(entry.frameSize);
        const auto saved_ip = external ? 0u : static_cast<unsigned>(IP() + 1);
        const auto saved_bp = m_BP ? static_cast<unsigned>(Index(m_BP, m_StackSegment, limits.stack_words) + 1) : 0u;
        const auto return_operand = (sp - bp) * 2 + (entry.flags & 1);
        Require(return_operand <= 2047, "Interpreter return-frame operand is not representable");
        std::fill(slots.begin() + sp, slots.begin() + sp + entry.frameSize, Slot::Empty);
        slots[sp] = Slot::Code; slots[sp + 1] = Slot::Frame;
        frames.push_back({bp, sp, sp + entry.frameSize, saved_ip, saved_bp, static_cast<unsigned>(return_operand)});
    }
    void Return(unsigned operand)
    {
        const auto frame = frames.back();
        Require(operand == frame.return_operand, "Interpreter return does not match its saved frame");
        if (operand & 1) Value(frame.bp);
        if (frames.size() == 1 && (operand & 1)) result = ReadValue(frame.bp);
        const auto new_sp = frame.bp + (operand & 1);
        std::fill(slots.begin() + new_sp, slots.begin() + SP(), Slot::Empty);
        frames.pop_back();
    }
    void Operation(unsigned operation)
    {
        Require(operation < 40 && operation != 33, "Unknown interpreter operation");
        const auto sp = SP();
        if (operation == 37 || operation == 38)
        {
            Temps(1); Word(sp - 1); const auto count = m_SP[-1];
            Require(count > 0 && (operation != 37 || count % 2 == 0), "Invalid interpreter reduction count");
            Require(count < limits.stack_words, "Interpreter reduction count exceeds its stack capacity");
            Temps(std::size_t(count) + 1);
            for (auto i = sp - count - 1; i < sp; ++i) Word(i);
            Drop(std::size_t(count) + 1); slots[sp - count - 1] = Slot::Word;
            return;
        }
        if (operation >= 34)
        {
            Temps(1); Word(sp - 1);
            if (operation == 39) Drop(1);
            return;
        }
        Temps(2);
        const bool strings = operation == 4 || operation == 7 || operation == 10 || operation == 13 || operation == 16 || operation == 19;
        if (strings) { String(sp - 2, operation == 4 || operation == 7); String(sp - 1, operation == 4 || operation == 7); }
        else { Word(sp - 2); Word(sp - 1); }
        if (operation == 26)
        {
            const auto lhs = std::bit_cast<std::int32_t>(m_SP[-2]), rhs = std::bit_cast<std::int32_t>(m_SP[-1]);
            Require(rhs != 0 && !(lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1), "Undefined interpreter signed division");
        }
        if (operation == 28) Require(m_SP[-1] != 0, "Interpreter unsigned modulo by zero");
        Drop(2); slots[sp - 2] = Slot::Word;
    }
    void Before()
    {
        Require(instructions < limits.instructions, "Interpreter instruction budget exhausted"); ++instructions;
        Require(!frames.empty(), "Interpreter instruction has no active frame");
        const auto ip = IP(), sp = SP(); const auto& frame = frames.back();
        Require(m_BP == m_StackSegment + frame.bp && sp >= frame.end, "Interpreter frame/stack pointer is invalid");
        Require(slots[frame.record] == Slot::Code && slots[frame.record + 1] == Slot::Frame
            && m_StackSegment[frame.record] == frame.ip_reference && m_StackSegment[frame.record + 1] == frame.bp_reference,
                "Interpreter saved frame integrity failed");
        const auto instruction = script->code[ip]; const unsigned opcode = instruction >> 11, operand = instruction & 2047;
        switch (opcode)
        {
        case 0: case 2: Push(Slot::Word); break;
        case 1: case 3: Push(Slot::String); break;
        case 4: case 6: Temps(1); Word(sp - 1); Drop(1); break;
        case 5: case 7: break; // Static branch destinations were checked by the retained reader.
        case 8: break; // The host bridge validates its complete signature before consuming anything.
        case 9: Require(operand < functions.size(), "Interpreter call target is invalid"); break;
        case 10: Return(operand); break;
        case 11: { const auto source = Local(operand, true); Push(slots[source]); break; }
        case 12:
        {
            Temps(1); Value(sp - 1); const auto target = Local(operand, false); const auto kind = slots[sp - 1];
            Drop(1); slots[target] = kind; break;
        }
        case 13: Operation(operand); break;
        case 14: Require(operand < globals.size(), "Interpreter global read is invalid"); Push(Slot::Word); break;
        case 15: Require(operand < globals.size(), "Interpreter global write is invalid"); Temps(1); Word(sp - 1); Drop(1); break;
        case 16:
        {
            const auto delta = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(operand));
            if (delta < 0) Drop(-int(delta));
            else { Space(delta); std::fill(slots.begin() + sp, slots.begin() + sp + delta, Slot::Empty); }
            break;
        }
        case 17: Temps(1); Value(sp - 1); slots[Local(operand, false)] = slots[sp - 1]; break;
        case 18: { const auto source = Local(operand >> 5, true), target = Local(operand & 31, false); slots[target] = slots[source]; break; }
        case 19: slots[Local(operand >> 5, false)] = Slot::Word; break;
        case 20:
        {
            const auto first = Local(operand >> 5, true), second = Local(int(operand >> 5) + 16 - int(operand & 31), true);
            Space(2); slots[sp] = slots[first]; slots[sp + 1] = slots[second]; break;
        }
        case 21:
        {
            const int a = operand >> 6, b = a + 4 - int((operand >> 3) & 7), c = b + 4 - int(operand & 7);
            const auto first = Local(a, true), second = Local(b, true), third = Local(c, true);
            Space(3); slots[sp] = slots[first]; slots[sp + 1] = slots[second]; slots[sp + 2] = slots[third]; break;
        }
        default: throw std::runtime_error("Unknown interpreter opcode");
        }
    }
    void DoFunctionCall(unsigned id) override
    {
        Require(host_calls < limits.host_calls, "Interpreter host-call budget exhausted"); ++host_calls;
        const auto found = host.find(id);
        Require(found != host.end(), "Interpreter host service is not bound");
        const auto& binding = found->second; Temps(binding.arguments.size());
        const auto old_sp = SP(), base = old_sp - binding.arguments.size();
        std::vector<InterpreterValue> values; values.reserve(binding.arguments.size());
        for (std::size_t i = 0; i < binding.arguments.size(); ++i)
        {
            if (binding.arguments[i] == InterpreterValueKind::Word) Word(base + i); else String(base + i);
            values.push_back(ReadValue(base + i));
        }
        if (binding.returns_word) Require(base < limits.stack_words, "Interpreter host return overflows the stack");
        const auto response = binding.invoke(values);
        Require(response.flow == InterpreterFlow::Continue || response.flow == InterpreterFlow::Pause || response.flow == InterpreterFlow::Retry,
                "Invalid interpreter host flow");
        if (response.flow == InterpreterFlow::Retry)
        {
            Require(!response.value, "Retrying interpreter service cannot publish a return value");
            StopWithUndo(); // Original saved-SP/IP behavior; untouched typed arguments remain available.
            return;
        }
        Require(response.value.has_value() == binding.returns_word, "Interpreter host return signature differs");
        Drop(binding.arguments.size()); m_SP -= binding.arguments.size();
        if (response.value) { Push(Slot::Word); *m_SP++ = *response.value; }
        if (response.flow == InterpreterFlow::Pause) StopWithoutUndo();
    }
    struct Execution
    {
        NativeInterpreterState& state;
        int exceptions = std::uncaught_exceptions();
        explicit Execution(NativeInterpreterState& s) : state(s) { s.busy = true; running = &s; s.instructions = s.host_calls = 0; }
        ~Execution() { state.failed |= std::uncaught_exceptions() > exceptions; state.busy = false; running = nullptr; }
    };
    void ExecuteEntry(FunctionEntryPoint& entry, std::span<const std::uint32_t> arguments)
    {
        Ready(); Require(m_RunState == 2, "Paused interpreter requires Resume or Reset");
        const auto extra = entry.flags == 3 ? 1u : 0u;
        Require(arguments.size() + extra == entry.numArgs, "Interpreter function argument count differs");
        Require(arguments.size() + extra + entry.frameSize <= limits.stack_words, "Interpreter initial frame overflows the stack");
        // Preflight all initial writes before original ExecuteFunction memcpy or
        // its optional return-slot reservation. RunFunction then validates the frame.
        InterpreterCore::Reset(); std::fill(slots.begin(), slots.end(), Slot::Empty); frames.clear(); result.reset();
        for (std::size_t i = 0; i < arguments.size(); ++i) slots[extra + i] = Slot::Word;
        Execution execution(*this);
        const std::uint32_t empty = 0;
        InterpreterCore::ExecuteFunction(&entry, arguments.size(), arguments.empty() ? &empty : arguments.data());
    }
};

namespace
{
NativeInterpreterState& State(InterpreterCore& core)
{
    Require(running && static_cast<InterpreterCore*>(running) == &core, "Original interpreter has no active native owner");
    return *running;
}
}
NativeInterpreter::NativeInterpreter(resources::Bytes bytes, InterpreterLimits limits)
    : state_(std::make_unique<NativeInterpreterState>(bytes, limits)) {}
NativeInterpreter::~NativeInterpreter() = default;
void NativeInterpreter::Bind(InterpreterHostCall call)
{
    auto& state = *state_; state.Ready();
    Require(state.m_RunState == 2 && call.id < 2048 && call.invoke && call.arguments.size() <= state.limits.stack_words,
            "Invalid interpreter host binding");
    for (auto kind : call.arguments) Require(kind == InterpreterValueKind::Word || kind == InterpreterValueKind::String,
                                            "Invalid interpreter host argument type");
    Require(!state.host.contains(call.id), "Duplicate interpreter host service"); state.host.emplace(call.id, std::move(call));
}
bool NativeInterpreter::Execute(std::uint32_t hash, std::span<const std::uint32_t> arguments)
{
    auto& state = *state_; state.Ready();
    if (state.functions.empty()) return false; // Original nlBSearch assumes a nonempty range.
    auto* entry = state.FindFunctionEntryPoint(hash); if (!entry) return false;
    state.ExecuteEntry(*entry, arguments); return true;
}
void NativeInterpreter::ExecuteIndex(std::size_t index, std::span<const std::uint32_t> arguments)
{
    auto& state = *state_; state.Ready(); Require(index < state.functions.size(), "Interpreter function index is out of bounds");
    state.ExecuteEntry(*state.GetFunctionEntryPoint(index), arguments);
}
void NativeInterpreter::Resume()
{
    auto& state = *state_; state.Ready(); Require(state.m_RunState == 3, "Interpreter is not paused");
    NativeInterpreterState::Execution execution(state); state.Run();
}
void NativeInterpreter::Reset(bool restore_globals)
{
    auto& state = *state_; state.Ready(true); state.InterpreterCore::Reset();
    std::fill(state.slots.begin(), state.slots.end(), Slot::Empty); state.frames.clear(); state.result.reset();
    state.failed = false; state.instructions = state.host_calls = 0;
    if (restore_globals) std::copy(state.script->globals.begin(), state.script->globals.end(), state.globals.begin());
}
InterpreterStatus NativeInterpreter::Status() const
{
    state_->Ready(true); return state_->failed ? InterpreterStatus::Failed : state_->m_RunState == 3 ? InterpreterStatus::Paused : InterpreterStatus::Ready;
}
std::optional<InterpreterValue> NativeInterpreter::Result() const { state_->Ready(); return state_->result; }
std::span<const std::uint32_t> NativeInterpreter::Globals() const { state_->Ready(); return state_->globals; }
std::size_t NativeInterpreter::Instructions() const { state_->Ready(true); return state_->instructions; }
std::size_t NativeInterpreter::HostCalls() const { state_->Ready(true); return state_->host_calls; }
void NativeInterpreterBeforeInstruction(InterpreterCore& core) { State(core).Before(); }
void NativeInterpreterBeginCall(InterpreterCore& core, FunctionEntryPoint& entry, unsigned count, bool external) { State(core).Enter(entry, count, external); }
std::uint32_t NativeInterpreterFrameReference(InterpreterCore& core, const std::uint32_t* pointer)
{ auto& state = State(core); return pointer ? state.Index(pointer, core.m_StackSegment, state.limits.stack_words) + 1 : 0; }
std::uint32_t NativeInterpreterCodeReference(InterpreterCore& core, const std::uint16_t* pointer)
{ auto& state = State(core); return pointer ? state.Index(pointer, core.m_Header->m_CodeSegment, state.script->code.size()) + 1 : 0; }
std::uint32_t* NativeInterpreterFramePointer(InterpreterCore& core, std::uint32_t reference)
{ auto& state = State(core); Require(reference <= state.limits.stack_words, "Invalid interpreter frame reference"); return reference ? core.m_StackSegment + reference - 1 : nullptr; }
std::uint16_t* NativeInterpreterCodePointer(InterpreterCore& core, std::uint32_t reference)
{ auto& state = State(core); Require(reference <= state.script->code.size(), "Invalid interpreter code reference"); return reference ? core.m_Header->m_CodeSegment + reference - 1 : nullptr; }
std::uint16_t* NativeInterpreterFunctionCode(InterpreterCore& core, FunctionEntryPoint& entry)
{ auto& state = State(core); Require(entry.offset % 2 == 0 && entry.offset / 2 < state.script->code.size(), "Invalid interpreter function reference"); return core.m_Header->m_CodeSegment + entry.offset / 2; }
std::uint32_t NativeInterpreterStringReference(InterpreterCore& core, std::uint32_t offset)
{ auto& state = State(core); Require(offset < state.script->strings.size(), "Invalid interpreter string offset"); return offset + 1; }
const char* NativeInterpreterString(InterpreterCore& core, std::uint32_t reference)
{ auto& state = State(core); Require(reference <= state.script->strings.size(), "Invalid interpreter string reference"); return reference ? reinterpret_cast<const char*>(state.script->strings.data() + reference - 1) : nullptr; }
}

#pragma once
#include "resources/bytecode.h"
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

class InterpreterCore;
struct FunctionEntryPoint;
namespace mscharged
{
enum class InterpreterValueKind { Word, String };
using InterpreterValue = std::variant<std::uint32_t, std::string>;
enum class InterpreterFlow { Continue, Pause, Retry };
struct InterpreterHostResult
{
    std::optional<std::uint32_t> value;
    InterpreterFlow flow = InterpreterFlow::Continue;
};
struct InterpreterHostCall
{
    unsigned id;
    std::vector<InterpreterValueKind> arguments; // Original stack order, oldest first.
    bool returns_word = false;
    std::function<InterpreterHostResult(std::span<const InterpreterValue>)> invoke;
};
struct InterpreterLimits
{
    std::size_t stack_words = 1024, frames = 128;
    std::size_t instructions = 100000, host_calls = 10000;
};
enum class InterpreterStatus { Ready, Paused, Failed };
struct NativeInterpreterState;

// Owns a validated, immutable copy of Wii bytecode and runs the selected
// original interpreter. Numeric values remain exactly 32 bits; strings and
// saved frames use checked typed offsets. No gameplay service is implicit.
class NativeInterpreter
{
    std::unique_ptr<NativeInterpreterState> state_;
public:
    explicit NativeInterpreter(resources::Bytes bytes, InterpreterLimits limits = {});
    ~NativeInterpreter();
    NativeInterpreter(const NativeInterpreter&) = delete;
    NativeInterpreter& operator=(const NativeInterpreter&) = delete;
    void Bind(InterpreterHostCall call);
    bool Execute(std::uint32_t hash, std::span<const std::uint32_t> arguments = {});
    void ExecuteIndex(std::size_t index, std::span<const std::uint32_t> arguments = {});
    void Resume();
    // Original Reset preserves globals. Restoring initial global words is an
    // explicit owner action. Failed executions require Reset; callbacks may not
    // mutate, reenter or destroy this owner. Retry repeats the same host opcode.
    void Reset(bool restore_globals = false);
    InterpreterStatus Status() const;
    std::optional<InterpreterValue> Result() const;
    std::span<const std::uint32_t> Globals() const;
    std::size_t Instructions() const;
    std::size_t HostCalls() const;
};

// Hooks used only by selected prepared original execution/operation bodies.
void NativeInterpreterBeforeInstruction(InterpreterCore& core);
void NativeInterpreterBeginCall(InterpreterCore& core, FunctionEntryPoint& entry, unsigned count, bool external);
std::uint32_t NativeInterpreterFrameReference(InterpreterCore& core, const std::uint32_t* pointer);
std::uint32_t NativeInterpreterCodeReference(InterpreterCore& core, const std::uint16_t* pointer);
std::uint32_t* NativeInterpreterFramePointer(InterpreterCore& core, std::uint32_t reference);
std::uint16_t* NativeInterpreterCodePointer(InterpreterCore& core, std::uint32_t reference);
std::uint16_t* NativeInterpreterFunctionCode(InterpreterCore& core, FunctionEntryPoint& entry);
std::uint32_t NativeInterpreterStringReference(InterpreterCore& core, std::uint32_t offset);
const char* NativeInterpreterString(InterpreterCore& core, std::uint32_t reference);
}

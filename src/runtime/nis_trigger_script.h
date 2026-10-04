#pragma once
#include "runtime/interpreter.h"
#include "runtime/nis_trigger_data.h"
#include <thread>

namespace mscharged
{
struct NisTriggerDefinitions
{
    NisPlaybackTable table;
    std::uint32_t function_hash = 0;
    bool found = false, used_fallback = false;
    std::size_t instructions = 0, host_calls = 0;
};

// Executes the original pure definition services into an owned table. Returned
// strings/records survive this owner. No audio/effect/event is fired here.
class NisTriggerScript
{
    NativeInterpreter interpreter_;
    std::vector<NisPlaybackTrigger> pending_;
    std::thread::id thread_ = std::this_thread::get_id();
    bool busy_ = false, failed_ = false;
    void Ready() const;
    NisTriggerDefinitions CollectHashes(std::uint32_t exact, std::uint32_t fallback, bool try_fallback, unsigned mode);
public:
    explicit NisTriggerScript(resources::Bytes bytes, InterpreterLimits limits = {});
    ~NisTriggerScript();
    NisTriggerScript(const NisTriggerScript&) = delete;
    NisTriggerScript& operator=(const NisTriggerScript&) = delete;
    // NisHeader names occupy 64 bytes. Selection preserves original last-dot
    // stripping, case-sensitive hash and first-underscore "all" fallback.
    NisTriggerDefinitions Collect(std::string_view nis_name, unsigned render_mode = 0);
    NisTriggerDefinitions CollectHash(std::uint32_t hash, unsigned render_mode = 0);
    bool Failed() const;
    // Failure never publishes a partial table. Reset permits another request;
    // numeric script globals retain original mutations unless explicitly reset.
    void Reset(bool restore_globals = false);
};

// Typed access/publication boundary for the selected original service bodies.
// The template consumes original Nis::TriggerParams without imposing native
// unsigned-long widths on its four Wii words.
class NativeNisTriggerCall
{
    std::span<const InterpreterValue> arguments_;
    std::optional<NisPlaybackTrigger> trigger_;
    bool consumed_ = false;
    const InterpreterValue& FromEnd(unsigned index) const;
public:
    explicit NativeNisTriggerCall(std::span<const InterpreterValue> args) : arguments_(args) {}
    std::uint32_t WordFromEnd(unsigned index) const;
    float FloatFromEnd(unsigned index) const;
    const char* StringFromEnd(unsigned index) const;
    void Consume(unsigned count);
    void Append(unsigned type, float frame, const char* name, const char* target, float value, std::array<std::uint32_t, 4> params);
    template<class Params> void AddTrigger(unsigned type, float frame, const char* name, const char* target, const Params* params)
    {
        if (!params) { AddTrigger(type, frame, name, target, nullptr); return; }
        Append(type, frame, name, target, params->float1,
            {std::uint32_t(params->param1), std::uint32_t(params->param2), std::uint32_t(params->param3), std::uint32_t(params->param4)});
    }
    void AddTrigger(unsigned type, float frame, const char* name, const char* target, std::nullptr_t)
    { Append(type, frame, name, target, -1, {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}); }
    NisPlaybackTrigger Finish();
};
NisPlaybackTrigger DecodeOriginalNisTriggerCall(unsigned id, std::span<const InterpreterValue> arguments);
std::uint32_t OriginalNisTriggerHash(const char* string);
}

#include "runtime/nis_trigger_script.h"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace mscharged
{
namespace
{
void Require(bool good, const char* message)
{
    if (!good) throw std::runtime_error(message);
}
using Kind = InterpreterValueKind;
// Stack order in original NisPlayer_interp.cpp, before its reverse pops.
const std::array<std::vector<Kind>, 11> signatures = {{
    {Kind::Word,Kind::Word}, {Kind::Word}, {Kind::Word,Kind::String,Kind::String,Kind::Word},
    {Kind::Word,Kind::Word,Kind::Word}, {Kind::Word,Kind::String,Kind::String},
    {Kind::Word,Kind::Word}, {Kind::Word,Kind::Word}, {Kind::Word,Kind::Word},
    {Kind::Word,Kind::Word}, {Kind::Word}, {Kind::Word,Kind::Word},
}};
}
const InterpreterValue& NativeNisTriggerCall::FromEnd(unsigned index) const
{
    Require(index > 0 && index <= arguments_.size() && !consumed_, "Invalid original NIS trigger argument access");
    return arguments_[arguments_.size() - index];
}
std::uint32_t NativeNisTriggerCall::WordFromEnd(unsigned index) const
{
    const auto& value = FromEnd(index);
    Require(std::holds_alternative<std::uint32_t>(value), "Original NIS trigger argument is not a word");
    return std::get<std::uint32_t>(value);
}
float NativeNisTriggerCall::FloatFromEnd(unsigned index) const { return std::bit_cast<float>(WordFromEnd(index)); }
const char* NativeNisTriggerCall::StringFromEnd(unsigned index) const
{
    const auto& value = FromEnd(index);
    Require(std::holds_alternative<std::string>(value), "Original NIS trigger argument is not a string");
    const auto& text = std::get<std::string>(value);
    Require(text.find('\0') == std::string::npos, "Original NIS trigger string contains an embedded NUL");
    return text.c_str();
}
void NativeNisTriggerCall::Consume(unsigned count)
{
    Require(!consumed_ && count == arguments_.size(), "Original NIS trigger argument count differs");
    consumed_ = true;
}
void NativeNisTriggerCall::Append(unsigned type, float frame, const char* name, const char* target, float value,
    std::array<std::uint32_t, 4> params)
{
    Require(consumed_ && !trigger_, "Original NIS trigger service appended an invalid number of records");
    Require(type <= 10 && std::isfinite(frame) && std::isfinite(value), "Invalid NIS trigger type or nonfinite parameter");
    Require(name && target, "Original NIS trigger strings must be present");
    NisPlaybackTrigger trigger{type, frame, name, target, value, params};
    Require(trigger.name.size() <= 4096 && trigger.target.size() <= 4096, "NIS trigger string exceeds the native collection limit");
    trigger_ = std::move(trigger);
}
NisPlaybackTrigger NativeNisTriggerCall::Finish()
{
    Require(consumed_ && trigger_.has_value(), "Original NIS trigger service did not produce one complete record");
    return std::move(*trigger_);
}
NisTriggerScript::NisTriggerScript(resources::Bytes bytes, InterpreterLimits limits) : interpreter_(bytes, limits)
{
    for (unsigned id = 0; id < signatures.size(); ++id)
        interpreter_.Bind({id, signatures[id], false, [this,id](auto arguments) {
            Require(busy_ && pending_.size() < 48, "NIS trigger collection exceeds its original 48-record capacity");
            pending_.push_back(DecodeOriginalNisTriggerCall(id, arguments));
            return InterpreterHostResult{};
        }});
}
NisTriggerScript::~NisTriggerScript()
{
    if (busy_ || thread_ != std::this_thread::get_id()) std::terminate();
}
void NisTriggerScript::Ready() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("NIS trigger collection requires its owner thread");
    if (busy_ || failed_) throw std::logic_error("NIS trigger collection is busy or failed; Reset is required");
}
NisTriggerDefinitions NisTriggerScript::CollectHashes(std::uint32_t exact, std::uint32_t fallback, bool try_fallback, unsigned mode)
{
    Ready(); Require(mode <= 2, "NIS trigger table render mode must be 0, 1 or 2");
    NisTriggerDefinitions definitions; definitions.table.render_mode = mode; definitions.function_hash = exact;
    interpreter_.Reset(); pending_.clear(); pending_.reserve(48); busy_ = true;
    try
    {
        definitions.found = interpreter_.Execute(exact);
        if (!definitions.found && try_fallback)
        {
            definitions.function_hash = fallback;
            definitions.found = interpreter_.Execute(fallback);
            definitions.used_fallback = definitions.found;
        }
        Require(interpreter_.Status() == InterpreterStatus::Ready, "NIS trigger definitions must complete without pausing");
        definitions.instructions = interpreter_.Instructions(); definitions.host_calls = interpreter_.HostCalls();
        definitions.table.triggers = std::move(pending_);
        busy_ = false; return definitions;
    }
    catch (...)
    {
        pending_.clear(); failed_ = true; busy_ = false; throw;
    }
}
NisTriggerDefinitions NisTriggerScript::Collect(std::string_view nis_name, unsigned render_mode)
{
    Ready();
    Require(!nis_name.empty() && nis_name.size() < 64 && nis_name.find('\0') == std::string_view::npos,
            "NIS trigger source name must fit its original 64-byte field");
    std::string name(nis_name);
    // The original overwrites the last dot without resizing BasicString; its
    // fallback underscore scan still visits the stored tail after that NUL.
    if (const auto dot = name.rfind('.'); dot != std::string::npos) name[dot] = '\0';
    const auto exact = OriginalNisTriggerHash(name.c_str());
    if (const auto underscore = name.find('_'); underscore != std::string::npos)
    {
        name.erase(0, underscore); name.insert(0, "all");
    }
    return CollectHashes(exact, OriginalNisTriggerHash(name.c_str()), true, render_mode);
}
NisTriggerDefinitions NisTriggerScript::CollectHash(std::uint32_t hash, unsigned render_mode)
{ return CollectHashes(hash, hash, false, render_mode); }
bool NisTriggerScript::Failed() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("NIS trigger collection requires its owner thread");
    return failed_;
}
void NisTriggerScript::Reset(bool restore_globals)
{
    if (thread_ != std::this_thread::get_id() || busy_) throw std::logic_error("Cannot reset active NIS trigger collection");
    interpreter_.Reset(restore_globals); pending_.clear(); failed_ = false;
}
}

#include "runtime/frontend_transition.h"
#include "Game/FE/FrontendTransitionSteps.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
constexpr std::uint32_t idle = 0x0017a6dd, title_to_main = 0xc41b2549;
void Require(bool value, const char* message) { if (!value) throw std::logic_error(message); }
struct Guard
{
    bool& busy;
    explicit Guard(bool& value) : busy(value) { busy = true; }
    ~Guard() { busy = false; }
};
float Float(const InterpreterValue& value)
{
    const float f = std::bit_cast<float>(std::get<std::uint32_t>(value));
    Require(std::isfinite(f) && f >= 0 && f <= 60, "Frontend transition time is outside supported range");
    return f;
}
}
struct FrontendTransition::Implementation
{
    struct Completion { bool finished = false; };
    NativeInterpreter vm;
    FrontendCameras& cameras;
    FrontendSceneStack& stack;
    const std::thread::id thread = std::this_thread::get_id();
    bool busy = false;
    FrontendTransitionState state = FrontendTransitionState::Idle;
    FrontendSession::Handle title;
    FrontendStackCallbacks callbacks;
    FrontendCameraSelectionHandle selection;
    std::shared_ptr<Completion> completion = std::make_shared<Completion>();
    std::optional<FrontendSceneStack::Token> queued;
    float wait = 0, delta = 0;
    std::vector<unsigned> calls;
    std::size_t trace_limit;

    Implementation(resources::Bytes bytes, FrontendCameras& c, FrontendSceneStack& s, InterpreterLimits limits)
        : vm(bytes, limits), cameras(c), stack(s), trace_limit(std::min<std::size_t>(limits.host_calls, 10000))
    {
        const auto script = resources::ReadScriptBytecode(bytes);
        for (auto hash : {idle, title_to_main})
        {
            const auto f = std::find_if(script->functions.begin(),script->functions.end(),[&](const auto& f){return f.hash==hash;});
            Require(f != script->functions.end() && !f->arguments && !f->flags,
                "Frontend transition requires original zero-argument Idle and title-to-main functions");
        }
        calls.reserve(trace_limit);
        for (unsigned id : {8u,15u,25u,28u,32u,44u,48u})
        {
            InterpreterHostCall host{id, {}, false, [this,id](auto args){return Invoke(id,args);}};
            if (id == 15) host.arguments = {InterpreterValueKind::String};
            if (id == 25) host.arguments = {InterpreterValueKind::Word,InterpreterValueKind::Word};
            if (id == 28 || id == 32) host.arguments = {InterpreterValueKind::Word};
            vm.Bind(std::move(host));
        }
        Require(vm.Execute(idle), "Original frontend Idle entry is missing");
        Require(vm.Status() == InterpreterStatus::Paused, "Original frontend Idle did not wait");
    }
    void Thread() const { Require(thread == std::this_thread::get_id(), "Frontend transition requires its owner thread"); }
    void Mutable(bool cleanup = false) const
    {
        Thread(); Require(!busy, "Frontend transition cannot be mutated during execution");
        Require(state != FrontendTransitionState::Released, "Frontend transition has been released");
        Require(cleanup || state != FrontendTransitionState::Failed, "Frontend transition needs cancellation after failure");
    }
    void Trace(unsigned id)
    {
        if (!trace_limit) return;
        if (id == 8 && !calls.empty() && calls.back() == 8) return;
        if (calls.size() == trace_limit) calls.erase(calls.begin());
        calls.push_back(id);
    }
    void CheckCamera() const
    {
        Require(selection && cameras.Selection() == selection, "Frontend transition camera was removed or selected externally");
    }
    InterpreterHostResult Invoke(unsigned id, std::span<const InterpreterValue> args)
    {
        Trace(id);
        if (id == 8) return {{},InterpreterFlow::Retry};
        Require(state == FrontendTransitionState::Running, "Frontend transition host ran outside its active function");
        switch (id)
        {
        case 15:
        {
            Require(std::get<std::string>(args[0]) == "startmainmenumove", "Title transition selected an unqualified camera alias");
            CheckCamera();
            const std::weak_ptr<Completion> weak = completion;
            selection = cameras.Select(selection,std::get<std::string>(args[0]),false,[weak]{if(auto value=weak.lock())value->finished=true;});
            break;
        }
        case 28: CheckCamera(); cameras.Seek(selection,Float(args[0])); break;
        case 44:
            CheckCamera();
            if (FrontendPresentationWaitCamera(completion->finished)) return {{},InterpreterFlow::Retry};
            break;
        case 32: wait = Float(args[0]); break;
        case 48:
            if (FrontendPresentationWaitTime(wait,delta)) return {{},InterpreterFlow::Retry};
            break;
        case 25:
        {
            CheckCamera();
            Require(std::get<std::uint32_t>(args[0]) == 1 && std::get<std::uint32_t>(args[1]) == 1,
                "Title transition can only queue original Main with forward movement");
            Require(!queued && title, "Frontend transition has no source or already issued a scene");
            FrontendStackRequest request;
            request.scene=1; request.movement=1; request.language=title->request.language;
            request.image_profile=FrontendImageProfile::Main; request.initial_slide="MAIN";
            queued = stack.QueuePush(std::move(request), callbacks);
            break;
        }
        default: throw std::logic_error("Unqualified frontend transition host");
        }
        return {};
    }
    void IdleAfterCompletion()
    {
        if (vm.Status() != InterpreterStatus::Ready) return;
        Require(queued.has_value(), "Title transition finished without its original main-scene push");
        // Original Update resets completion/wait flags then immediately calls
        // Idle; the existing camera's original callback remains installed.
        completion->finished=false; wait=0; vm.Reset();
        Require(vm.Execute(idle), "Original frontend Idle entry is missing");
        Require(vm.Status()==InterpreterStatus::Paused, "Original frontend Idle did not wait");
        state=FrontendTransitionState::SceneQueued;
    }
    void Detach()
    {
        if (selection) cameras.DetachEndCallback(selection);
        selection.reset(); completion->finished=false;
    }
};
FrontendTransition::FrontendTransition(resources::Bytes script,FrontendCameras& cameras,
    FrontendSceneStack& stack,InterpreterLimits limits)
    : impl_(std::make_unique<Implementation>(script,cameras,stack,limits)) {}
FrontendTransition::~FrontendTransition()
{ try { Release(); } catch (...) { std::terminate(); } }
void FrontendTransition::Begin(FrontendSceneStack::Token token,const FrontendSession::Handle& expected,
    FrontendStackCallbacks callbacks)
{
    auto& s=*impl_;s.Mutable();
    Require(s.state!=FrontendTransitionState::Running,"Frontend transition is already running");
    const auto entry=s.stack.Entry(token);
    Require(entry.scene==0 && expected && entry.published==expected && (entry.state==FrontendStackState::Published
        || entry.state==FrontendStackState::AwaitingPublication),
        "Frontend transition requires its exact published title frame");
    const bool complete=callbacks.scene_created&&callbacks.initialize_subhandlers&&callbacks.after_base_update;
    Require(complete||(!callbacks.scene_created&&!callbacks.initialize_subhandlers&&!callbacks.after_base_update),
        "Frontend transition main callbacks must be complete or explicitly unavailable");
    auto selected=s.cameras.Selection();
    Guard guard(s.busy);
    try
    {
        s.Detach();s.title=expected;s.callbacks=std::move(callbacks);s.queued.reset();s.calls.clear();
        s.wait=0;s.completion->finished=false;s.selection=std::move(selected);s.vm.Reset();
        s.state=FrontendTransitionState::Running;
        Require(s.vm.Execute(title_to_main),"Original title-to-main function is missing");
        s.IdleAfterCompletion();
    }
    catch (...) {s.state=FrontendTransitionState::Failed;throw;}
}
void FrontendTransition::Update(float delta)
{
    auto& s=*impl_;s.Mutable();CheckCameraDelta(delta);
    Guard guard(s.busy);
    try
    {
        s.delta=delta;
        if(s.state==FrontendTransitionState::Running)s.CheckCamera();
        if(s.vm.Status()==InterpreterStatus::Paused)s.vm.Resume();
        if(s.state==FrontendTransitionState::Running)s.IdleAfterCompletion();
    }
    catch (...) {s.state=FrontendTransitionState::Failed;throw;}
}
FrontendTransitionStatus FrontendTransition::Status() const
{
    const auto& s=*impl_;s.Thread();
    return {s.state,s.wait,s.completion->finished,s.queued,s.selection};
}
std::span<const unsigned> FrontendTransition::Calls() const {impl_->Thread();return impl_->calls;}
void FrontendTransition::Cancel()
{
    auto& s=*impl_;s.Mutable(true);Guard guard(s.busy);
    s.Detach();s.vm.Reset();s.title.reset();s.callbacks={};s.wait=0;s.calls.clear();
    // Source Reset does not undo a Push already issued by this script.
    s.state=FrontendTransitionState::Cancelled;
}
void FrontendTransition::Release()
{
    auto& s=*impl_;s.Thread();if(s.state==FrontendTransitionState::Released)return;
    s.Mutable(true);Guard guard(s.busy);s.Detach();s.vm.Reset();s.callbacks={};s.title.reset();
    s.state=FrontendTransitionState::Released;
}
}

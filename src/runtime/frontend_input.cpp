#include "runtime/frontend_input.h"
#include "Game/FE/feInput.h"
#include "Game/PadActionMap.inc"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool value, const char* message) { if (!value) throw std::logic_error(message); }
void Delta(float value)
{ if (!std::isfinite(value) || value < 0 || value > 60) throw std::invalid_argument("Invalid frontend input time"); }
int Action(FrontendAction action)
{
    switch (action)
    {
    case FrontendAction::Left: case FrontendAction::Right: case FrontendAction::Up:
    case FrontendAction::Down: case FrontendAction::Accept: case FrontendAction::Back:
    case FrontendAction::Start: return static_cast<int>(action);
    }
    throw std::invalid_argument("Unsupported frontend action");
}
eFEINPUT_PAD Pad(int pad)
{
    if (pad < -1 || pad > 3) throw std::out_of_range("Frontend pad must be 0..3 or all");
    return pad == -1 ? FE_ALL_PADS : static_cast<eFEINPUT_PAD>(pad);
}
BaseSceneHandler* Scene(const void* scene)
{ return static_cast<BaseSceneHandler*>(const_cast<void*>(scene)); }
class DesktopPad final : public PadBackend
{
    FrontendPadSample next_, current_;
    std::uint32_t previous_ = 0;
    std::array<float, 13> times_{};
    float delta_ = 0;
    unsigned Mask(int button, bool remap) const
    {
        if (remap)
        {
            if (button < 0 || button >= 51) throw std::out_of_range("Frontend pad action");
            button = g_pPadRemapArray[button];
        }
        if (button <= 0 || button > 0x1fff || !std::has_single_bit(static_cast<unsigned>(button)))
            throw std::out_of_range("Frontend pad button must be one of thirteen masks");
        return static_cast<unsigned>(button);
    }
public:
    explicit DesktopPad(int index) : PadBackend(index) {}
    void Stage(FrontendPadSample sample) { next_ = sample; }
    bool IsConnected() override { return current_.connected; }
    bool IsPressed(int b, bool remap) override { return current_.connected && (current_.buttons & Mask(b, remap)); }
    float GetPressure(int b, bool remap) override { return IsPressed(b, remap) ? 1.f : 0.f; }
    float GetPressureDerivative(int b, bool remap) override
    {
        const auto mask = Mask(b, remap);
        const float result = delta_ ? (float(bool(current_.buttons & mask)) - float(bool(previous_ & mask))) / delta_ : 0;
        if (!std::isfinite(result)) throw std::overflow_error("Frontend pressure derivative exceeds its range");
        return result;
    }
    bool PlatJustPressed(int b, bool remap) override
    { const auto mask = Mask(b, remap); return current_.connected && (current_.buttons & mask) && !(previous_ & mask); }
    bool PlatJustReleased(int b, bool remap) override
    { const auto mask = Mask(b, remap); return current_.connected && !(current_.buttons & mask) && (previous_ & mask); }
    int GetButtonIndex(int b, bool remap) override { return GetPadButtonIndex(Mask(b, remap)); }
    int GetButtonMask(int index) override
    { if (index < 0 || index >= 13) throw std::out_of_range("Frontend button index"); return GetPadButtonMask(index); }
    float GetButtonStateTime(int b, bool remap) override { return times_[GetButtonIndex(b, remap)]; }
    float AnalogLeftX() override { return current_.left_x; }
    float AnalogLeftY() override { return current_.left_y; }
    float AnalogRightX() override { return 0; } // This menu profile has no right-stick mapping.
    float AnalogRightY() override { return 0; }
    bool RumbleActive() override { return false; }
    void StartRumble(float, float, float) override { throw std::runtime_error("Frontend desktop rumble is not connected"); }
    void StopRumble() override { throw std::runtime_error("Frontend desktop rumble is not connected"); }
    int GetClassID() override { return -1; } // Native menu backend, never a Wii device class.
    void Update(float delta) override
    {
        previous_ = current_.buttons; current_ = next_; delta_ = delta;
        if (!current_.connected) { current_.buttons = 0; current_.left_x = current_.left_y = 0; }
        if (m_isLeftAnalogToDPadMapEnabled)
        {
            if (current_.left_x <= -.5f) current_.buttons |= 1;
            if (current_.left_x >= .5f) current_.buttons |= 2;
            if (current_.left_y >= .5f) current_.buttons |= 8;
            if (current_.left_y <= -.5f) current_.buttons |= 4;
        }
        previous_ = (previous_ & ~current_.suppressed_buttons) | (current_.buttons & current_.suppressed_buttons);
        if (current_.suppress_edges || !current_.connected)
        { previous_ = current_.buttons = 0; current_.left_x = current_.left_y = 0; times_.fill(0); }
        else for (unsigned i = 0; i < times_.size(); ++i)
        {
            const auto mask = 1u << i;
            times_[i] = ((current_.buttons ^ previous_) & mask) ? 0 : std::min(times_[i] + delta, 1e7f);
        }
        PadBackend::Update(delta);
    }
};
}
struct FrontendInput::State
{
    std::thread::id thread = std::this_thread::get_id();
    PadManager manager;
    std::array<std::unique_ptr<cGlobalPad>, 4> pads;
    std::array<cGlobalPad*, 4> pointers{};
    std::unique_ptr<FEInput> input;
    ~State()
    {
        if (thread != std::this_thread::get_id()) std::terminate();
        if (g_pFEInput == input.get()) g_pFEInput = nullptr;
        input.reset();
        if (g_pPadManager == &manager) g_pPadManager = nullptr;
    }
};
FrontendInput::FrontendInput()
{
    Require(!g_pFEInput && !g_pPadManager, "Original frontend input already has an owner");
    auto state = std::make_unique<State>();
    for (unsigned i = 0; i < state->pads.size(); ++i)
    {
        state->pads[i] = std::make_unique<cGlobalPad>(i);
        state->pads[i]->mBackend = new DesktopPad(i);
        state->pointers[i] = state->pads[i].get();
    }
    state->manager.mPadCount = 4; state->manager.mPadSetCount = 1; state->manager.mActivePadSet = 0;
    state->manager.m_aPads = state->pointers.data();
    g_pPadManager = &state->manager;
    state->input = std::make_unique<FEInput>();
    g_pFEInput = state->input.get(); state_ = std::move(state);
}
FrontendInput::~FrontendInput() = default;
void FrontendInput::Ready() const
{
    Require(state_ && state_->thread == std::this_thread::get_id(), "Frontend input requires its owner thread");
    Require(g_pPadManager == &state_->manager && g_pFEInput == state_->input.get(), "Original frontend input ownership changed");
}
void FrontendInput::Update(const std::array<FrontendPadSample, 4>& samples, float delta)
{
    Ready(); Delta(delta);
    for (const auto& sample : samples)
        if (sample.buttons > 0x1fff || sample.suppressed_buttons > 0x1fff
            || !std::isfinite(sample.left_x) || !std::isfinite(sample.left_y)
            || std::abs(sample.left_x) > 1 || std::abs(sample.left_y) > 1)
            throw std::invalid_argument("Invalid frontend pad sample");
    for (unsigned i = 0; i < 4; ++i) static_cast<DesktopPad*>(state_->pads[i]->mBackend)->Stage(samples[i]);
    state_->manager.Update(delta); state_->input->Update(delta);
}
bool FrontendInput::Connected(unsigned pad) const
{ Ready(); if (pad >= 4) throw std::out_of_range("Frontend pad index"); return state_->input->IsConnected(static_cast<eFEINPUT_PAD>(pad)); }
bool FrontendInput::Button(FrontendAction action, FrontendButtonQuery query, int pad, int* found_pad)
{
    Ready(); const auto selected = Pad(pad); const auto button = Action(action);
    eFEINPUT_PAD found = FE_NO_PAD; bool result = false;
    switch (query)
    {
    case FrontendButtonQuery::Held: result = state_->input->IsPressed(selected, button, true, &found); break;
    case FrontendButtonQuery::Pressed: result = state_->input->JustPressed(selected, button, true, &found); break;
    case FrontendButtonQuery::Released: result = state_->input->JustReleased(selected, button, true, &found); break;
    case FrontendButtonQuery::Repeat: result = state_->input->IsAutoPressed(selected, button, true, &found); break;
    default: throw std::invalid_argument("Unknown frontend button query");
    }
    if (found_pad) *found_pad = result ? (pad == -1 ? int(found) : pad) : -1;
    return result;
}
void FrontendInput::SetRepeat(FrontendAction action, float delay, float rate, int pad)
{ Ready(); Delta(delay); Delta(rate); state_->input->SetAutoRepeatParams(Pad(pad), Action(action), delay, rate); }
void FrontendInput::EnableAnalogDirections(bool enable, int pad)
{ Ready(); state_->input->EnableAnalogToDPadMapping(Pad(pad), enable); }
void FrontendInput::EnablePad(unsigned pad, bool enable)
{ Ready(); if (pad >= 4) throw std::out_of_range("Frontend pad index"); state_->input->m_bEnableInput[pad] = enable; }
void FrontendInput::PushFocus(const void* scene, int custom_id)
{
    Ready(); Require(scene && state_->input->m_InputLockDepth < 4, "Frontend focus stack is full or has no scene");
    state_->input->PushExclusiveInputLock(Scene(scene), custom_id);
}
void FrontendInput::PopFocus(const void* scene)
{
    Ready(); Require(scene && state_->input->HasInputLock(Scene(scene)), "Frontend focus pop must match its top scene");
    state_->input->PopExclusiveInputLock(Scene(scene));
}
void FrontendInput::Focus(const void* scene) { Ready(); state_->input->EnableInputIfSceneHasFocus(Scene(scene)); }
bool FrontendInput::HasFocusLock(const void* scene) const { Ready(); return state_->input->HasInputLock(Scene(scene)); }
void FrontendInput::Reset()
{
    Ready(); state_->input->Reset(true);
    std::array<FrontendPadSample, 4> clear{}; for (auto& pad : clear) pad.suppress_edges = true;
    Update(clear, 0);
}
}

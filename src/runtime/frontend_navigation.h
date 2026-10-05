#pragma once
#include "runtime/frontend_audio.h"
#include "runtime/frontend_pointer_host.h"

namespace mscharged
{
enum class FrontendNavigationDoneSlide { Off, Over, Down };
struct FrontendNavigationDoneBinding
{
    FrontendSession::Handle frame;
    std::uint32_t component=0;
    FrontendPointerBounds bounds;
    bool visible=false; // Explicit visibility in the acknowledged source generation.
};
using FrontendNavigationBackBinding = FrontendNavigationDoneBinding;
enum class FrontendNavigationPointer { Waiting, Cursor };
struct FrontendNavigationPointerSample
{
    std::array<float,2> position{};
    std::uint16_t angle=0;
    bool valid=false;
};
struct FrontendNavigationStatus
{
    unsigned visible_buttons=0;
    bool widescreen=false, back_initialized=false, failed=false;
    bool transition_playing=false, transition_pending=false;
    bool pointer_input_enabled=true, pointer_hidden=false;
    std::string transition_function;
    std::array<int,4> back_states{};
    std::array<bool,4> back_inside{};
    // Original speaker-context/hover-rumble requests are observed, not executed.
    unsigned speaker_context=0, hover_feedback_requests=0;
};
struct FrontendNavigationDispatch
{
    FrontendPointerDispatch pointer;
    bool back_pressed=false;
};
// Selected SHNavigation visuals plus genuine FEBackButton false-push/false-pop
// flow used by Options. This is not global NAV/DPD, HOME, PageControls input,
// team colours or FEScene state6. Other button masks change visuals only.
// Caller retains input/RNG and acknowledges the actually composed NAV frame.
// Route NAV first; forward its same event to Options only if back_pressed=false.
class FrontendNavigation
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    bool ApplyPending(unsigned);
    friend class FrontendNavigationDone;
    void CheckDoneInput(const FrontendSession::Handle&) const;
public:
    FrontendNavigation(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,bool widescreen=false,unsigned controller=0);
    ~FrontendNavigation();
    FrontendNavigation(const FrontendNavigation&)=delete;
    FrontendNavigation& operator=(const FrontendNavigation&)=delete;
    FrontendSession::Handle Current() const;
    FrontendNavigationStatus Status() const;
    FrontendPointerBounds Bounds() const;
    // Exact presented Back component, original measured bounds and retained visibility.
    FrontendNavigationBackBinding BackButton(const FrontendSession::Handle&) const;
    void Acknowledge(const FrontendSession::Handle&,FrontendPointerViewport);
    void HideButtons(const FrontendSession::Handle&);
    void SetButtons(const FrontendSession::Handle&,unsigned mask,bool enabled=true);
    // Checked original done component; label0 DONE, label1 OPTIONS_ACCEPT.
    // Mutations accept Current(), including within WithPresentedInput.
    void SetDoneButtonText(const FrontendSession::Handle&,unsigned);
    void SetDoneButtonSlide(const FrontendSession::Handle&,FrontendNavigationDoneSlide);
    // Requires this exact actually acknowledged NAV frame. Bound constants are
    // original SetDoneButtonBounds(0), not a measured or guessed text rectangle.
    FrontendNavigationDoneBinding DoneButton(const FrontendSession::Handle&) const;
    void SetPointerSlide(const FrontendSession::Handle&,unsigned,FrontendNavigationPointer);
    void UpdatePointers(const FrontendSession::Handle&,
        const std::array<FrontendNavigationPointerSample,4>&,bool hidden=false);
    // Retains a real script callback. Source pending update calls it once before
    // the inclusive0.6-second hide. Callback may query, never mutate/release NAV.
    // An exception preserves already published source stages and fails this
    // owner; admitted external script effects are not rewound or replayed.
    using TransitionCallback=std::function<void(std::string_view)>;
    void StartTransition(const FrontendSession::Handle&,std::string function_name,TransitionCallback);
    void AdvanceVisual(const FrontendSession::Handle&,float delta);
    FrontendNavigationDispatch Route(const FrontendPointerDesktopSample&);
    FrontendNavigationDispatch Poll(SDL_Window*,bool capture=false);
    bool DeliverPointer(const FrontendSession::Handle&,const FrontendPointerEvent&);
    // Source commands and the base update can change the candidate before
    // input. Route against the exact last acknowledged geometry in this bounded
    // window; callbacks mutate only this owner's current resource generation.
    void WithPresentedInput(const FrontendSession::Handle&,const std::function<void()>&);
    void Release();
};
}

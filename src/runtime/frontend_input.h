#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace mscharged
{
// Original abstract action indices from PadActions/FE consumers. Physical input
// mapping is a desktop policy; FE queries retain the original remap and timing.
enum class FrontendAction : int { Left = 11, Right = 12, Up = 13, Down = 14, Accept = 30, Back = 31, Start = 32 };
struct FrontendPadSample
{
    bool connected = false, suppress_edges = false;
    std::uint32_t buttons = 0; // Original thirteen-button desktop/GameCube mask.
    float left_x = 0, left_y = 0;
    std::uint32_t suppressed_buttons = 0; // Changes caused by capture/hotplug are not user edges.
};
enum class FrontendButtonQuery { Held, Pressed, Released, Repeat };

// Owns original FEInput, PadManager and four cGlobalPads backed by real samples.
// Only one owner may publish their original globals. Scene identities passed to
// focus operations are borrowed and must remain valid until popped/reset.
// This menu profile does not provide gameplay motion, Wii DPD or rumble output.
class FrontendInput
{
    struct State;
    std::unique_ptr<State> state_;
    void Ready() const;
public:
    FrontendInput();
    ~FrontendInput();
    FrontendInput(const FrontendInput&) = delete;
    FrontendInput& operator=(const FrontendInput&) = delete;
    void Update(const std::array<FrontendPadSample, 4>&, float delta);
    bool Connected(unsigned pad) const;
    // pad=-1 queries the original FE_ALL_PADS order; otherwise 0..3.
    bool Button(FrontendAction, FrontendButtonQuery, int pad = -1, int* found_pad = nullptr);
    void SetRepeat(FrontendAction, float delay, float rate, int pad = -1);
    void EnableAnalogDirections(bool, int pad = -1);
    void EnablePad(unsigned pad, bool);
    void PushFocus(const void* scene, int custom_id = -1);
    void PopFocus(const void* scene);
    void Focus(const void* scene);
    bool HasFocusLock(const void* scene) const;
    void Reset();
};
}

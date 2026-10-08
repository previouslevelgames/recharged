#include "platform/desktop_wpad.h"
#include "platform/desktop_dpd.h"
#include "platform/wpad_sdl.h"
#include "platform/interrupts.h"
#include <revolution/kpad.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

// Desktop keyboard/mouse lifecycle through the real SDL virtual core-Wii
// device, native WPAD and the whole original KPAD. Edges are counted exactly
// as PlatPadManager consumes them: one latest KPADRead sample per update.
namespace {
using namespace mscharged::platform;
unsigned checks{};
SDL_Window* window{};
std::thread::id owner;
DesktopDpdProjection projection{1, 20, 16, 600, 448};
bool projected = true;
struct Edges {
    unsigned trig{}, release{}, reads{};
    u32 hold{};
    KPADStatus last{};
} edges;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool Project(void*, SDL_Window* actual, DesktopDpdProjection* output) {
    Check(owner == std::this_thread::get_id() && actual == window,
          "Projection queried a foreign owner or window");
    // Generated successful-Present geometry; GPU publication is a separate gate.
    *output = projection;
    return projected;
}
void Push(SDL_Event& event) { Check(SDL_PushEvent(&event), "Actual SDL event queue rejected input"); }
void WindowEvent(Uint32 type) {
    SDL_Event event{};
    event.type = type;
    event.window.windowID = SDL_GetWindowID(window);
    Push(event);
}
void Key(SDL_Scancode code, bool down, bool repeat = false, SDL_KeyboardID which = 0) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.scancode = code;
    event.key.which = which;
    event.key.down = down;
    event.key.repeat = repeat;
    Push(event);
}
void Motion(float x, float y, SDL_MouseID which = 1) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = SDL_GetWindowID(window);
    event.motion.which = which;
    event.motion.x = x;
    event.motion.y = y;
    Push(event);
}
// SDL attributes mouse events to its current mouse-focus window; after a real
// leave (capture already released) that is window 0.
void Button(float x, float y, bool down, Uint8 button = SDL_BUTTON_LEFT, bool focused_window = true, SDL_MouseID which = 1) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = focused_window ? SDL_GetWindowID(window) : 0;
    event.button.which = which;
    event.button.button = button;
    event.button.down = down;
    event.button.x = x;
    event.button.y = y;
    Push(event);
}
float X(float n) { return projection.left + n * projection.width; }
float Y(float n) { return projection.top + n * projection.height; }

void Service() {
    ServiceDesktopWpad();
    ServiceWpadSDL();
    KPADStatus sample{};
    if (KPADRead(0, &sample, 1) > 0) {
        edges.trig += (sample.trig & WPAD_BUTTON_A) != 0;
        edges.release += (sample.release & WPAD_BUTTON_A) != 0;
        edges.hold = sample.hold & KPAD_BUTTON_MASK;
        edges.last = sample;
        ++edges.reads;
    }
}
template<class F> void Until(F predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        Service();
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "trig=%u release=%u hold=%#x reads=%u dpd=%d\n", edges.trig,
                         edges.release, unsigned(edges.hold), edges.reads, int(edges.last.dpd_valid_fg));
            throw std::runtime_error(message);
        }
        SDL_Delay(2);
    }
    ++checks;
}
// Several report periods with new KPAD reads, so a pending edge cannot hide.
void Settle() {
    const auto reads = edges.reads + 6;
    Until([&] { return edges.reads >= reads; }, "Owner stopped publishing raw reports to KPAD");
}
bool HoldA() { return (edges.hold & WPAD_BUTTON_A) != 0; }
bool Pointer() { return edges.last.dpd_valid_fg > 0; }
void Expect(unsigned trig, unsigned release, bool held, const char* message) {
    Settle();
    Check(edges.trig == trig && edges.release == release && HoldA() == held, message);
}

void ServiceFor(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) { Service(); SDL_Delay(2); }
}
void Connect() {
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
    Until([] { return WpadSDLConnectedChannels() == 1 && edges.last.wpad_err == WPAD_ERR_OK; },
          "Desktop virtual remote did not reach native WPAD/original KPAD");
    WPADDeviceType type{};
    Check(WPADProbe(0, &type) == WPAD_ERR_OK && WPADProbe(1, &type) == WPAD_ERR_NO_CONTROLLER,
          "Desktop remote did not occupy the first Wii channel only");
    if (!WPADIsDpdEnabled(0)) {
        Check(WPADControlDpd(0, WPAD_DPD_STANDARD, nullptr) == WPAD_ERR_OK,
              "Original camera request was not accepted");
        Until([] { return WPADIsDpdEnabled(0); }, "Virtual camera register write did not commit");
    }
}

void RepeatedAndSharedMappings() {
    Key(SDL_SCANCODE_RETURN, true);
    Until(HoldA, "Enter did not reach original KPAD A");
    Expect(1, 0, true, "Enter press produced other than one KPAD A trigger");
    for (int n = 0; n < 5; ++n) {
        Key(SDL_SCANCODE_RETURN, true, true);
        Service();
    }
    Expect(1, 0, true, "Keyboard auto-repeat delivered a duplicate KPAD A edge");
    Key(SDL_SCANCODE_SPACE, true);
    Expect(1, 0, true, "Second key mapped to held A delivered a duplicate edge");
    Key(SDL_SCANCODE_RETURN, false);
    Expect(1, 0, true, "Releasing one of two A keys released the shared Wii button");
    Button(X(.5f), Y(.5f), true);
    Expect(1, 0, true, "Mouse A while keyboard A held delivered a duplicate edge");
    Key(SDL_SCANCODE_SPACE, false);
    Expect(1, 0, true, "Keyboard release dropped the still-held mouse A");
    Button(X(.5f), Y(.5f), false);
    Expect(1, 1, false, "Last shared A source did not release exactly once");
}

void FocusLoss() {
    Motion(X(.25f), Y(.75f));
    Until(Pointer, "Focused mouse did not produce an original KPAD pointer");
    Key(SDL_SCANCODE_RETURN, true);
    Button(X(.25f), Y(.75f), true);
    Expect(2, 1, true, "Keyboard and mouse A did not form one press");
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
    Expect(2, 2, false, "Focus loss left KPAD A held or released twice");
    Check(!Pointer(), "Focus loss retained a stale pointer observation");
    // Releases that arrive after focus loss and presses without focus stay inert.
    Key(SDL_SCANCODE_RETURN, false);
    Button(X(.25f), Y(.75f), false);
    Key(SDL_SCANCODE_SPACE, true);
    Motion(X(.5f), Y(.5f));
    Expect(2, 2, false, "Unfocused desktop input reached the Wii remote");
    Check(!Pointer(), "Unfocused motion produced a pointer");
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
    Expect(2, 2, false, "Regaining focus manufactured a press for an unfocused key");
    Key(SDL_SCANCODE_SPACE, false);
    Expect(2, 2, false, "Release of an unfocused press delivered an edge");
}

void LeaveAndEnter() {
    WindowEvent(SDL_EVENT_WINDOW_MOUSE_ENTER);
    Motion(X(.6f), Y(.4f));
    Until(Pointer, "Re-entered mouse did not recover the pointer");
    Button(X(.6f), Y(.4f), true);
    Expect(3, 2, true, "Mouse A press after re-entry failed");
    WindowEvent(SDL_EVENT_WINDOW_MOUSE_LEAVE);
    Expect(3, 3, false, "Leaving the window retained mouse A");
    Check(!Pointer(), "Leaving the window retained a stale pointer");
    Button(X(.6f), Y(.4f), false, SDL_BUTTON_LEFT, false);
    Expect(3, 3, false, "Release outside the window delivered a second edge");
    // Keyboard remains a focused input while the mouse is outside.
    Key(SDL_SCANCODE_X, true);
    Until([] { return (edges.hold & WPAD_BUTTON_2) != 0; }, "Keyboard 2 failed while the mouse was outside");
    Key(SDL_SCANCODE_X, false);
    Until([] { return (edges.hold & WPAD_BUTTON_2) == 0; }, "Keyboard 2 release failed while the mouse was outside");
    WindowEvent(SDL_EVENT_WINDOW_MOUSE_ENTER);
    Settle();
    Check(!Pointer(), "Window entry without a position manufactured a pointer");
    Motion(X(.3f), Y(.3f));
    Until(Pointer, "Motion after entry did not recover the pointer");
}

void HiddenAndMinimized() {
    Key(SDL_SCANCODE_RETURN, true);
    Expect(4, 3, true, "Enter after re-entry failed");
    WindowEvent(SDL_EVENT_WINDOW_HIDDEN);
    Expect(4, 4, false, "Hidden window retained KPAD A");
    Check(!Pointer(), "Hidden window retained a pointer");
    WindowEvent(SDL_EVENT_WINDOW_SHOWN);
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
    Key(SDL_SCANCODE_RETURN, false);
    Key(SDL_SCANCODE_RETURN, true);
    Expect(5, 4, true, "Enter after showing the window failed");
    WindowEvent(SDL_EVENT_WINDOW_MINIMIZED);
    Expect(5, 5, false, "Minimized window retained KPAD A");
    WindowEvent(SDL_EVENT_WINDOW_RESTORED);
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
    Key(SDL_SCANCODE_RETURN, false);
    Expect(5, 5, false, "Restore delivered a stale edge");
}

void PresentedGeometryChanges() {
    Motion(X(.5f), Y(.5f));
    Until(Pointer, "Pointer did not recover after restore");
    projected = false; // e.g. resized window before its next successful Present
    Motion(X(.5f), Y(.5f));
    Settle();
    Check(!Pointer(), "Pointer used geometry without a matching successful Present");
    projected = true;
    projection = {2, 100, 50, 320, 240};
    Motion(X(.75f), Y(.25f));
    Until([] { return Pointer() && std::fabs(edges.last.pos.x - .5f) < .01f &&
                      std::fabs(edges.last.pos.y + .5f) < .01f; },
          "New successful-Present geometry did not recover the pointer");
    Motion(projection.left - 2, Y(.5f));
    Settle();
    Check(!Pointer(), "Pointer in the presentation bars remained visible");
}

void DetachAndReattach() {
    Key(SDL_SCANCODE_RETURN, true);
    Expect(6, 5, true, "Enter before detach failed");
    ShutdownDesktopWpad();
    // The next owner service sees the old device leave and its replacement arrive.
    InitializeDesktopWpad(window, {true, false, true, Project, nullptr});
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
    ServiceFor(std::chrono::milliseconds(200));
    WPADDeviceType type{};
    Check(WpadSDLConnectedChannels() == 1 && WPADProbe(0, &type) == WPAD_ERR_OK &&
          WPADProbe(1, &type) == WPAD_ERR_NO_CONTROLLER,
          "Reattached desktop remote moved away from the first Wii channel");
    Settle();
    Check(!HoldA(), "Detached device left KPAD A held");
    Key(SDL_SCANCODE_RETURN, true);
    Until(HoldA, "Reattached keyboard Enter failed");
    Key(SDL_SCANCODE_RETURN, false);
    Until([] { return !HoldA(); }, "Reattached keyboard release failed");
}

void RemoveKeyboard(SDL_KeyboardID which) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.kdevice.which = which;
    Push(event);
}
void MouseDeviceEvent(Uint32 type, SDL_MouseID which) {
    SDL_Event event{};
    event.type = type;
    event.mdevice.which = which;
    Push(event);
}
void DeviceRemoval() {
    const auto trig = edges.trig, release = edges.release;
    // Generated instance IDs exercise the actual SDL event/watch and original
    // KPAD path. This fixture does not claim physical hot-unplug qualification.
    constexpr SDL_KeyboardID keyboard1 = 42, keyboard2 = 84;
    constexpr SDL_MouseID mouse1 = 126, mouse2 = 168;
    Key(SDL_SCANCODE_RETURN, true, false, keyboard1);
    Key(SDL_SCANCODE_RETURN, true, true, keyboard2);
    Expect(trig + 1, release, true, "Two keyboard instances generated duplicate A edges");
    RemoveKeyboard(keyboard1);
    Expect(trig + 1, release, true, "One keyboard removal released another instance's A");
    RemoveKeyboard(252);
    Expect(trig + 1, release, true, "Foreign keyboard removal changed held A");
    RemoveKeyboard(keyboard2);
    Expect(trig + 1, release + 1, false, "Last keyboard removal retained A without a key-up");
    SDL_Event reconnect{};
    reconnect.type = SDL_EVENT_KEYBOARD_ADDED;
    reconnect.kdevice.which = keyboard1;
    Push(reconnect);
    Expect(trig + 1, release + 1, false, "Keyboard reconnect resurrected old held state");
    Key(SDL_SCANCODE_RETURN, true, false, keyboard1);
    Expect(trig + 2, release + 1, true, "Fresh reconnect key did not reach original KPAD");
    Key(SDL_SCANCODE_RETURN, false, false, keyboard1);
    Expect(trig + 2, release + 2, false, "Reconnected keyboard release failed");

    Motion(X(.25f), Y(.75f), mouse1);
    Button(X(.25f), Y(.75f), true, SDL_BUTTON_LEFT, true, mouse1);
    Motion(X(.6f), Y(.4f), mouse2);
    Button(X(.6f), Y(.4f), true, SDL_BUTTON_LEFT, true, mouse2);
    Expect(trig + 3, release + 2, true, "Two mouse instances generated duplicate A edges");
    Until(Pointer, "Live mouse position failed to reach KPAD");
    MouseDeviceEvent(SDL_EVENT_MOUSE_REMOVED, mouse1);
    Expect(trig + 3, release + 2, true, "One mouse removal released another instance's A");
    Check(Pointer(), "Removing a foreign position owner invalidated live pointer data");
    MouseDeviceEvent(SDL_EVENT_MOUSE_REMOVED, mouse2);
    Expect(trig + 3, release + 3, false, "Last mouse removal retained A without button-up");
    Check(!Pointer(), "Removed producing mouse retained a stale pointer");
    MouseDeviceEvent(SDL_EVENT_MOUSE_ADDED, mouse2);
    Expect(trig + 3, release + 3, false, "Mouse reconnect resurrected old held state");
    Check(!Pointer(), "Mouse reconnect fabricated a fresh position");

    // Device retirement also keeps independent keyboard/mouse A contributors.
    Key(SDL_SCANCODE_RETURN, true, false, keyboard1);
    Button(X(.5f), Y(.5f), true, SDL_BUTTON_LEFT, true, mouse1);
    Expect(trig + 4, release + 3, true, "Shared keyboard/mouse A generated a duplicate edge");
    RemoveKeyboard(keyboard1);
    Expect(trig + 4, release + 3, true, "Keyboard removal released a held mouse contribution");
    MouseDeviceEvent(SDL_EVENT_MOUSE_REMOVED, mouse1);
    Expect(trig + 4, release + 4, false, "Shared A remained held after all actual contributors retired");

    Key(SDL_SCANCODE_RETURN, true, false, keyboard1);
    Key(SDL_SCANCODE_RETURN, true, true, keyboard2);
    Button(X(.5f), Y(.5f), true, SDL_BUTTON_LEFT, true, mouse1);
    Button(X(.5f), Y(.5f), true, SDL_BUTTON_LEFT, true, mouse2);
    Expect(trig + 5, release + 4, true, "Multi-instance A setup failed");
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
    Expect(trig + 5, release + 5, false, "Focus flush retained an instance's held A");
    WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
    Expect(trig + 5, release + 5, false, "Focus regain resurrected retired contributions");
    RemoveKeyboard(keyboard1);
    RemoveKeyboard(keyboard2);
    MouseDeviceEvent(SDL_EVENT_MOUSE_REMOVED, mouse1);
    MouseDeviceEvent(SDL_EVENT_MOUSE_REMOVED, mouse2);
    Expect(trig + 5, release + 5, false, "Removal after focus flush delivered a second edge");
    Check(WpadSDLConnectedChannels() == 1, "Input device removal disconnected the live virtual remote");
}

void SingleOwner() {
    bool rejected = false;
    std::thread foreign([&] {
        try { ServiceDesktopWpad(); } catch (const std::logic_error&) { rejected = true; }
    });
    foreign.join();
    Check(rejected, "A foreign thread serviced the desktop transport");
}
} // namespace

int main() {
    try {
        owner = std::this_thread::get_id();
        Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "Cannot initialize actual SDL devices");
        window = SDL_CreateWindow("Desktop input lifecycle qualifier", 640, 480, SDL_WINDOW_HIDDEN);
        Check(window, "Cannot create the actual SDL window");
        ConfigureWpadSDL({0, 3});
        InitializeDesktopWpad(window, {true, false, true, Project, nullptr});
        KPADInit();
        // Literal original PlatPadManager channel parameters.
        KPADSetPosParam(0, .02f, .95f);
        KPADSetHoriParam(0, 0, 1);
        KPADSetDistParam(0, 0, 1);
        KPADSetAccParam(0, 0, 1);
        KPADSetBtnRepeat(0, .75f, .25f);
        Connect();
        Settle();
        Check(!edges.trig && !edges.release && !HoldA(), "Initial reports manufactured a press");
        RepeatedAndSharedMappings();
        FocusLoss();
        LeaveAndEnter();
        HiddenAndMinimized();
        PresentedGeometryChanges();
        DetachAndReattach();
        DeviceRemoval();
        SingleOwner();
        ShutdownDesktopWpad();
        WPADShutdown();
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
        std::printf("Desktop keyboard/mouse lifecycle through original KPAD: %u checks, %u KPAD reads, "
                    "%u A presses/%u releases; generated presentation geometry and SDL events only.\n",
                    checks, edges.reads, edges.trig, edges.release);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Desktop input lifecycle: %s\n", error.what());
        try { ShutdownDesktopWpad(); } catch (...) {}
        try { WPADShutdown(); } catch (...) {}
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}

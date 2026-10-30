// Controller menus ([gameplay] controller_menus), inside the original game
// module. The game's menus are pointer-only. For a gamepad player the D-pad
// and the sticks move between a menu's buttons instead, as Strikers
// WiiCompiled has it (MenuNav in its runtime's hle/input/pad.cpp): the pointer
// snaps onto the next button that way, so the game's own hand marks the
// selection and its hover and press logic run unchanged (A confirms). A menu
// that opens starts on its first button, or on the one last selected there.
// B goes to the screen's back button and presses it; on the pause menu, which
// has none, it presses the pause button. The triggers turn pages as - and +.
// On ring menus (the main menu) the stick picks the button at its angle.
//
// Tweak patch 0047 calls this once a frame for each channel as its remote is
// read (FENavigateMenus in feDPD.cpp), with the menu's live buttons.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error Controller menus belong to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_tweaks.h"
#include "platform/controller_menus.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// The host's state of its gamepad players, and each channel's as of this
// frame's mscharged_tweak_controller_menus.
ChargedMenuPadReader sReader;
std::array<ChargedMenuPad, 4> sPads{};

constexpr unsigned kNavUp = 1, kNavDown = 2, kNavLeft = 4, kNavRight = 8;
// WPAD_BUTTON_* (revolution/wpad/WPAD.h).
constexpr unsigned short kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                         kWpadPlus = 0x0010, kWpadOne = 0x0200, kWpadB = 0x0400, kWpadA = 0x0800,
                         kWpadMinus = 0x1000;
constexpr int kStadiumSelect = 5; // SCENE_CHOOSE_STADIUM: starts on PLAY NOW, has no ring
// The first repeat of a held direction and the next ones; how long the hand
// hides while B goes back; how long the pointer rests off every button before
// the default is selected.
constexpr auto kRepeatFirst = 350ms, kRepeatNext = 130ms, kHideHand = 333ms, kOffButton = 100ms;
constexpr int kMaxButtons = 64;

struct Point {
    float x = 0, y = 0;
    bool operator==(const Point&) const = default;
};
float Length(Point p) { return std::hypot(p.x, p.y); }

struct Button {
    const mscharged_menu_button* region = nullptr;
    Point centre, aim; // aim: where the pointer goes to select it
};
struct Buttons {
    std::array<Button, kMaxButtons> items;
    int count = 0;
    const Button* begin() const { return items.data(); }
    const Button* end() const { return items.data() + count; }
    bool empty() const { return count == 0; }
    const Button* At(Point centre) const {
        for (const Button& b : *this)
            if (b.centre == centre) return &b;
        return nullptr;
    }
};

struct State {
    unsigned held_dir = 0;
    Clock::time_point repeat_at{};
    Point goal, aim, cursor; // cursor: the pointer while a menu is up
    int settle_frames = 0;   // until the game has the pointer where it was sent
    bool back_held = false;
    int back_frames = 0;  // B pressed: heading for the back button
    int press_frames = 0; // tapping A on it
    Clock::time_point hide_hand_until{};
    int page_frames = 0, page_dir = 0, held_page = 0;
    int pause_frames = 0;           // tapping the pause button (B on an in-match overlay)
    const void* scene = nullptr;    // top scene and overlay last frame, to spot screen changes
    const void* overlay_scene = nullptr;
    std::pair<int, int> memory_key; // this screen's key in the remembered selections
    bool had_buttons = false;       // buttons last frame (false: a menu or overlay just appeared)
    bool overlay = false;           // this menu appeared over an unchanged scene (the pause menu)
    const void* scene_when_empty = nullptr;
    const void* selected = nullptr; // the button last selected (buttons move: a screen slides out)
    bool off_button = false;
    Clock::time_point off_button_since{};
};
std::array<State, 4> sStates;
// Player 1 always navigates. Other players join once they use their
// controller, so an idle one does not snap its pointer around in menus
// player 1 is driving.
std::array<bool, 4> sEngaged{true, false, false, false};
// Where each player last was on each screen (scene and overlay type), so
// coming back to a screen or closing a pop-up returns to that button.
std::array<std::map<std::pair<int, int>, Point>, 4> sRemembered;

Buttons CollectButtons(const mscharged_menu_frame& frame) {
    Buttons out;
    for (int i = 0; i < frame.count && out.count < kMaxButtons; ++i) {
        const mscharged_menu_button& r = frame.regions[i];
        Button b;
        b.region = &r;
        b.centre = {(r.min_x + r.max_x) * 0.5f, (r.min_y + r.max_y) * 0.5f};
        if (r.rotation != 0.0f) {
            // The game turns the region about its pivot (FEPointerRegion::ContainsPoint).
            const float dx = b.centre.x - r.pivot_x, dy = b.centre.y - r.pivot_y;
            const float c = std::cos(r.rotation), s = std::sin(r.rotation);
            b.centre = {r.pivot_x + dx * c - dy * s, r.pivot_y + dx * s + dy * c};
        }
        // Low in the button, so the hand (drawn below its fingertip) leaves
        // the label visible; small or turned buttons (ring icons) take the middle.
        b.aim = b.centre;
        if (r.rotation == 0.0f && r.max_y - r.min_y > 24.0f) b.aim.y = r.min_y + (r.max_y - r.min_y) * 0.3f;
        if (std::isfinite(b.centre.x) && std::isfinite(b.centre.y)) out.items[std::size_t(out.count++)] = b;
    }
    return out;
}

// The next button in the pressed direction. Buttons lined up with the current
// one (overlapping it across the direction of travel: the same row for left
// and right, the same column for up and down) come first, nearest first;
// otherwise the nearest, weighting sideways distance. Off the end of a row or
// column it wraps to the far end of it. The back button is never a target: B
// does that.
bool PickInDirection(const Buttons& buttons, const Button* current, Point from, unsigned dir, Point& out) {
    const bool horizontal = (dir & (kNavLeft | kNavRight)) != 0;
    const float dx = (dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f;
    const float dy = (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f;
    // The span to line up against: the current button, or a small box around the pointer.
    const float lo = current ? (horizontal ? current->region->min_y : current->region->min_x)
                             : (horizontal ? from.y : from.x) - 4.0f;
    const float hi = current ? (horizontal ? current->region->max_y : current->region->max_x)
                             : (horizontal ? from.y : from.x) + 4.0f;
    float best = 1e30f, wrap_best = -1.0f;
    Point wrap;
    for (const Button& b : buttons) {
        if (&b == current || b.region->back) continue;
        const float forward = (b.centre.x - from.x) * dx + (b.centre.y - from.y) * dy;
        const float b_lo = horizontal ? b.region->min_y : b.region->min_x;
        const float b_hi = horizontal ? b.region->max_y : b.region->max_x;
        const float gap = std::max(0.0f, std::max(b_lo - hi, lo - b_hi));
        if (forward >= 8.0f) {
            const float score = gap == 0.0f ? forward : 100000.0f + forward + 3.0f * gap;
            if (score < best) {
                best = score;
                out = b.centre;
            }
        } else if (forward <= -8.0f && gap == 0.0f && -forward > wrap_best) {
            wrap_best = -forward; // the farthest lined-up button behind
            wrap = b.centre;
        }
    }
    if (best < 1e30f) return true;
    if (wrap_best > 0.0f) {
        out = wrap;
        return true;
    }
    return false;
}

// The selection a menu opens on: the first button in reading order (top row
// first, then left to right), past the back button and controls at the
// screen's edges (page dots, side arrows). With prefer_widest (stage select)
// the widest wins instead: PLAY NOW over the arrows.
Point PickDefault(const Buttons& buttons, Point half, bool prefer_widest) {
    const Button* best = nullptr;
    for (int pass = 0; pass < 2 && !best; ++pass) {
        for (const Button& b : buttons) {
            if (b.region->back) continue;
            if (pass == 0 && (std::fabs(b.centre.x) > half.x - 50.0f || b.centre.y > half.y - 50.0f)) continue;
            if (!best) best = &b;
            else if (prefer_widest) {
                if (b.region->max_x - b.region->min_x > best->region->max_x - best->region->min_x) best = &b;
            } else if (b.centre.y > best->centre.y + 20.0f ||
                       (std::fabs(b.centre.y - best->centre.y) <= 20.0f && b.centre.x < best->centre.x))
                best = &b; // a higher row, or the same row further left
        }
    }
    return best ? best->centre : buttons.begin()->centre;
}

// Selects the button whose middle is `goal`: the pointer goes to its aim
// point, which the game takes on this frame's update.
void SnapTo(State& st, const Buttons& buttons, Point goal) {
    st.goal = goal;
    const Button* b = buttons.At(goal);
    st.aim = b ? b->aim : goal;
    st.cursor = st.aim;
    st.settle_frames = 4;
}

// A ring menu (the main menu): five or more buttons, the back button aside,
// about as far from their middle as each other. Its buttons and middle.
int FindRing(const Buttons& buttons, std::array<const Button*, kMaxButtons>& ring, Point& middle) {
    int count = 0;
    middle = {};
    for (const Button& b : buttons)
        if (!b.region->back) ring[std::size_t(count++)] = &b;
    if (count < 5) return 0;
    for (int i = 0; i < count; ++i) {
        middle.x += ring[std::size_t(i)]->centre.x;
        middle.y += ring[std::size_t(i)]->centre.y;
    }
    middle.x /= float(count);
    middle.y /= float(count);
    float lo = 1e30f, hi = 0.0f, sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float d = Length({ring[std::size_t(i)]->centre.x - middle.x, ring[std::size_t(i)]->centre.y - middle.y});
        lo = std::min(lo, d);
        hi = std::max(hi, d);
        sum += d;
    }
    if (sum <= 0.0f || (hi - lo) / (sum / float(count)) > 0.35f) return 0;
    return count;
}

void Navigate(int channel, const mscharged_menu_frame& frame, mscharged_menu_result& r) {
    State& st = sStates[std::size_t(channel)];
    const ChargedMenuPad& pad = sPads[std::size_t(channel)];
    const auto now = Clock::now();
    const Buttons buttons = CollectButtons(frame);
    // The stronger stick navigates beside the D-pad.
    Point stick{frame.stick_x, frame.stick_y};
    if (Length({pad.stick_x, pad.stick_y}) > Length(stick)) stick = {pad.stick_x, pad.stick_y};
    unsigned dir = 0;
    if (frame.buttons & kWpadUp) dir |= kNavUp;
    if (frame.buttons & kWpadDown) dir |= kNavDown;
    if (frame.buttons & kWpadLeft) dir |= kNavLeft;
    if (frame.buttons & kWpadRight) dir |= kNavRight;
    if (stick.y > 0.6f) dir |= kNavUp;
    if (stick.y < -0.6f) dir |= kNavDown;
    if (stick.x < -0.6f) dir |= kNavLeft;
    if (stick.x > 0.6f) dir |= kNavRight;
    const bool back = (frame.buttons & kWpadB) != 0;
    const int page = pad.page;

    // A menu is up whenever live pointer buttons exist (none during matches).
    if (dir || back || Length(stick) > 0.5f) sEngaged[std::size_t(channel)] = true;
    if (!sEngaged[std::size_t(channel)]) return;
    if (buttons.empty()) {
        st = {};
        st.scene_when_empty = frame.scene;
        return;
    }
    r.menu = true;
    // One direction at a time; the first press moves at once, holding repeats.
    if (dir & (kNavUp | kNavDown)) dir &= kNavUp | kNavDown;
    else dir &= kNavLeft | kNavRight;
    if (((dir & kNavUp) && (dir & kNavDown)) || ((dir & kNavLeft) && (dir & kNavRight))) dir = 0;
    bool step = false;
    if (dir && dir != st.held_dir) {
        step = true;
        st.repeat_at = now + kRepeatFirst;
    } else if (dir && now >= st.repeat_at) {
        step = true;
        st.repeat_at = now + kRepeatNext;
    }
    st.held_dir = dir;
    const bool back_pressed = back && !st.back_held;
    st.back_held = back;

    const Point at{frame.pointer_x, frame.pointer_y};
    const Point half{frame.half_width, frame.half_height};
    const bool stage_select = frame.scene_type == kStadiumSelect;
    // The selection follows the button being moved to while a snap settles,
    // else whatever the pointer is on.
    const Button* current = nullptr;
    for (const Button& b : buttons)
        if (b.region->pointed) {
            current = &b;
            break;
        }
    const Button* selected = st.settle_frames > 0 ? buttons.At(st.goal) : current;
    const auto finish = [&] {
        r.pointer_x = st.cursor.x;
        r.pointer_y = st.cursor.y;
        r.hide_hand = now < st.hide_hand_until;
    };

    // A new screen (another scene or overlay on top, or buttons appearing
    // after none) starts on its default selection, or the one remembered for
    // it, wherever the pointer was left.
    if (frame.scene != st.scene || frame.overlay != st.overlay_scene || !st.had_buttons) {
        // In-match overlays (the pause menu) appear without a scene change,
        // and their pages (the pause menu's options) stay one.
        st.overlay = st.had_buttons ? frame.scene == st.scene && st.overlay : frame.scene == st.scene_when_empty;
        st.scene = frame.scene;
        st.overlay_scene = frame.overlay;
        st.had_buttons = true;
        st.memory_key = {frame.scene_type, frame.overlay_type};
        Point start = PickDefault(buttons, half, stage_select);
        const auto remembered = sRemembered[std::size_t(channel)].find(st.memory_key);
        if (remembered != sRemembered[std::size_t(channel)].end() && buttons.At(remembered->second))
            start = remembered->second;
        SnapTo(st, buttons, start);
        finish();
        return;
    }
    if (selected) sRemembered[std::size_t(channel)][st.memory_key] = selected->centre;
    // The triggers turn pages or switch tabs on any screen (the game's - and +).
    int page_press = 0;
    if (st.page_frames > 0) {
        --st.page_frames;
        page_press = st.page_frames >= 2 ? st.page_dir : 0;
    }
    if (page != 0 && page != st.held_page) {
        st.page_dir = page;
        st.page_frames = 4;
        page_press = page;
    }
    st.held_page = page;
    if (page_press > 0) r.press |= kWpadPlus;
    if (page_press < 0) r.press |= kWpadMinus;
    // B: to the back button, then a tap of A on it. Overlays without one (the
    // pause menu) close with the game's own pause button instead.
    if (back_pressed) {
        const Button* back_button = nullptr;
        for (const Button& b : buttons)
            if (b.region->back) {
                back_button = &b;
                break;
            }
        if (back_button) {
            // The hand hides, the pointer goes to the back button and A taps
            // there next frame: the screen changes before anything is seen moving.
            SnapTo(st, buttons, back_button->centre);
            st.back_frames = 2;
            st.hide_hand_until = now + kHideHand;
        } else if (st.overlay)
            st.pause_frames = 4;
    }
    if (st.pause_frames > 0) {
        --st.pause_frames;
        if (st.pause_frames >= 2) r.press |= kWpadOne;
    }
    finish();
    if (st.press_frames > 0) {
        --st.press_frames;
        if (st.press_frames >= 2) r.press |= kWpadA;
        return;
    }
    if (st.back_frames > 0 && --st.back_frames == 0) {
        st.press_frames = 3;
        r.press |= kWpadA;
        return;
    }
    // On its way to the back button, the pointer stays there for the press.
    if (st.back_frames > 0) return;
    // Ring menus: a stick or D-pad direction selects the button at that
    // angle around the ring's middle, following the stick as it turns.
    std::array<const Button*, kMaxButtons> ring{};
    Point ring_middle;
    const int ring_count = stage_select ? 0 : FindRing(buttons, ring, ring_middle);
    if (ring_count > 0) {
        Point aim = stick;
        if (Length(aim) < 0.55f) {
            aim = {(dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f,
                   (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f};
            if (!step) aim = {};
        }
        if (aim.x != 0.0f || aim.y != 0.0f) {
            const float want = std::atan2(aim.y, aim.x);
            const Button* best = nullptr;
            float best_diff = 1e30f;
            for (int i = 0; i < ring_count; ++i) {
                const Button* b = ring[std::size_t(i)];
                float diff = std::fabs(std::atan2(b->centre.y - ring_middle.y, b->centre.x - ring_middle.x) - want);
                if (diff > 3.14159265f) diff = 6.2831853f - diff;
                if (diff < best_diff) {
                    best_diff = diff;
                    best = b;
                }
            }
            if (best && !best->region->pointed && !(st.settle_frames > 0 && st.goal == best->centre)) {
                SnapTo(st, buttons, best->centre);
                finish();
            }
        }
    }
    Point goal;
    if (ring_count == 0 && step && PickInDirection(buttons, current, at, dir, goal)) {
        SnapTo(st, buttons, goal);
        finish();
        return;
    }
    if (st.settle_frames > 0) {
        // Wait for the game to take the new position before checking again.
        --st.settle_frames;
        if (Length({st.aim.x - at.x, st.aim.y - at.y}) < 2.0f) st.settle_frames = 0;
        return;
    }
    // Keep something selected. Off every button, the pointer follows the
    // button it was on if that is still there (moved: a screen's buttons
    // slide as it comes or goes), and only after a moment goes to the
    // default: a screen in transition briefly shows its buttons elsewhere,
    // and snapping there every other frame makes the pointer jitter.
    if (current) {
        st.off_button = false;
        if (selected) st.selected = selected->region->id;
        return;
    }
    if (st.selected) {
        for (const Button& b : buttons)
            if (b.region->id == st.selected) {
                SnapTo(st, buttons, b.centre);
                finish();
                return;
            }
    }
    if (!st.off_button) {
        st.off_button = true;
        st.off_button_since = now;
    }
    if (now - st.off_button_since < kOffButton) return;
    st.off_button = false;
    SnapTo(st, buttons, PickDefault(buttons, half, stage_select));
    finish();
}
} // namespace

extern "C" {
// Host entry (controller menus): where the game reads the gamepad players'
// second stick and triggers; null for none.
__attribute__((visibility("default"))) void charged_configure_controller_menus(ChargedMenuPadReader reader)
{
    sReader = reader;
}
}

bool mscharged_tweak_controller_menus(int channel)
{
    if (channel < 0 || channel >= 4) return false;
    ChargedMenuPad pad{};
    if (!mscharged_tweak_enabled(MSCHARGED_TWEAK_CONTROLLER_MENUS) || !sReader || !sReader(channel, &pad)) {
        // Not navigating: the next menu starts afresh.
        sStates[std::size_t(channel)] = {};
        return false;
    }
    sPads[std::size_t(channel)] = pad;
    return true;
}

void mscharged_tweak_navigate_menu(int channel, const struct mscharged_menu_frame* frame,
                                   struct mscharged_menu_result* result)
{
    if (!result) return;
    *result = {};
    if (channel < 0 || channel >= 4 || !frame) return;
    Navigate(channel, *frame, *result);
}

// Opt-in game tweaks ([gameplay] in mscharged.ini), inside the original game
// module. The hooks of patches/mscharged-decomp-tweaks call these functions;
// each returns the original behaviour unless the player enabled its tweak.
// The host passes the enabled set once, before original main starts.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error Game tweaks belong to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_tweaks.h"

#include <cstdio>

namespace {
unsigned sEnabled;

// Skip intro: the title screen is passed once per launch.
bool sTitlePassed;

// Fast menus.
// Transition time runs this much faster while the front end's presentation
// script plays one: its camera moves are over in a frame or two.
constexpr float kTransitionDilation = 30.0f;
bool sMenus;      // in the front end, outside matches and the boot screens
bool sTransition; // ... and the presentation runs a transition script
// The only task on transition time: the one that moves the cameras. Effects
// on it advanced up to 3 s a frame, too far to be drawn (issue #34), and no
// script waits for them, so they, audio and pads keep real time.
const void* sTransitionTask;
unsigned long sFrame;
// Slides set again this frame (SetActiveSlide resets a slide with Update(0)).
constexpr int kMaxResets = 256;
const void* sResets[kMaxResets];
int sResetCount;
unsigned long sResetFrame;
} // namespace

extern "C" {
// Host entry: the enabled tweaks as bits of mscharged::GameTweak.
__attribute__((visibility("default"))) void charged_configure_tweaks(unsigned mask)
{
    sEnabled = mask;
    sTitlePassed = false;
    sMenus = sTransition = false;
}

bool mscharged_tweak_enabled(int tweak)
{
    return tweak >= 0 && tweak < 32 && (sEnabled & (1u << tweak)) != 0;
}

bool mscharged_tweak_take_title_press(void)
{
    if (sTitlePassed || !mscharged_tweak_enabled(MSCHARGED_TWEAK_SKIP_INTRO)) return false;
    sTitlePassed = true;
    std::fputs("Tweak: skip intro passed the title screen\n", stderr);
    return true;
}

bool mscharged_tweak_title_pending(void)
{
    return !sTitlePassed && mscharged_tweak_enabled(MSCHARGED_TWEAK_SKIP_INTRO);
}

void mscharged_tweak_frontend_frame(bool menus, bool transition)
{
    ++sFrame;
    sMenus = menus && mscharged_tweak_enabled(MSCHARGED_TWEAK_FAST_MENUS);
    sTransition = sMenus && transition;
}

bool mscharged_tweak_fast_menus(void)
{
    return sMenus;
}

bool mscharged_tweak_fast_transition(void)
{
    return sTransition;
}

void mscharged_tweak_set_transition_task(const void* task)
{
    sTransitionTask = task;
}

float mscharged_tweak_task_dilation(const void* task)
{
    return sTransition && task != nullptr && task == sTransitionTask ? kTransitionDilation : 1.0f;
}

void mscharged_tweak_note_slide_reset(const void* slide)
{
    if (sResetFrame != sFrame)
    {
        sResetFrame = sFrame;
        sResetCount = 0;
    }
    if (sResetCount < kMaxResets) sResets[sResetCount++] = slide;
}

bool mscharged_tweak_slide_reset_this_frame(const void* slide)
{
    if (sResetFrame != sFrame) return false;
    for (int n = 0; n < sResetCount; ++n)
        if (sResets[n] == slide) return true;
    return false;
}
}

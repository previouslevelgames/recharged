// Opt-in game tweaks ([gameplay] in mscharged.ini), inside the original game
// module. The hooks of patches/mscharged-decomp-tweaks call these functions;
// each returns the original behaviour unless the player enabled its tweak.
// The host passes the enabled set once, before original main starts, and for
// a frame rate above the original ([display] frame_rate) the swap that
// presents and paces each frame.
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

// Frame rate above the original ([display] frame_rate): the host's swap,
// which presents each frame and paces the next (Aurora's paced video swap).
void (*sHostSwap)();
// The HOME Button Menu steps its animations once a frame; its task state
// keeps the original pacing.
constexpr unsigned kHomeButtonMenuState = 0x2000000;
// The original frame: one VI field, or two in swap mode 2.
constexpr float kNtscField = 1001.0f / 60000.0f;
constexpr float kPalField = 1.0f / 50.0f;
float sOriginalFrameSeconds = kNtscField;
// This frame in original frames, the fraction of an original frame passed
// since the last one began, and the original frames begun so far.
float sOriginalFrames = 1.0f;
float sOriginalPhase;
bool sOriginalFrame = true;
unsigned long sOriginalFrameCount;
} // namespace

// Original SDK (the native VI): the TV format, VI_PAL being 1.
extern "C" unsigned int VIGetTvFormat(void);

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

// Host entry: the swap that presents and paces each frame for a frame rate
// above the original, or null for the original pacing.
__attribute__((visibility("default"))) void charged_configure_frame_pacing(void (*swap)())
{
    sHostSwap = swap;
    sOriginalFrameSeconds = kNtscField;
    sOriginalFrames = 1.0f;
    sOriginalPhase = 0.0f;
    sOriginalFrame = true;
    sOriginalFrameCount = 0;
}

bool mscharged_tweak_host_frames(void)
{
    return sHostSwap != nullptr;
}

int mscharged_tweak_swap_mode(unsigned taskState, int swapMode)
{
    if (!sHostSwap) return swapMode;
    sOriginalFrameSeconds = (swapMode == 2 ? 2.0f : 1.0f) * (VIGetTvFormat() == 1 ? kPalField : kNtscField);
    return taskState == kHomeButtonMenuState ? swapMode : MSCHARGED_SWAP_HOST;
}

void mscharged_tweak_host_swap(void)
{
    if (sHostSwap) sHostSwap();
}

void mscharged_tweak_begin_frame(float realSeconds)
{
    if (!sHostSwap) return;
    // The task manager's own bound on a frame (g_fTaskTimeUpperBound).
    if (!(realSeconds > 0.0f)) realSeconds = 0.0f;
    if (realSeconds > 0.1f) realSeconds = 0.1f;
    sOriginalFrames = realSeconds / sOriginalFrameSeconds;
    sOriginalPhase += sOriginalFrames;
    sOriginalFrame = sOriginalPhase >= 1.0f;
    if (!sOriginalFrame) return;
    ++sOriginalFrameCount;
    // Frames longer than the original's each begin one, as in the original.
    sOriginalPhase -= 1.0f;
    if (sOriginalPhase >= 1.0f) sOriginalPhase = 0.0f;
}

float mscharged_tweak_original_frames(void)
{
    return sHostSwap ? sOriginalFrames : 1.0f;
}

bool mscharged_tweak_original_frame(void)
{
    return !sHostSwap || sOriginalFrame;
}

unsigned long mscharged_tweak_frame_number(unsigned long renderedFrame)
{
    return sHostSwap ? sOriginalFrameCount : renderedFrame;
}
}

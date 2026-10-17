// Opt-in game tweaks ([gameplay] in recharged.ini), inside the original game
// module. The hooks of patches/mscharged-decomp-tweaks call these functions;
// each returns the original behaviour unless the player enabled its tweak.
// The host passes the enabled set once, before original main starts, and for
// a frame rate above the original ([display] frame_rate) the swap that
// presents and paces each frame.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error Game tweaks belong to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_tweaks.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
unsigned sEnabled;

// Skip intro: the title screen is passed once per launch.
bool sTitlePassed;
bool sPerformanceMode;
int sStadium = -1; // reported by the render task during a match, taken by the host each frame

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
// The running task got transition time. A looping camera, a screen's idle
// view, takes its real time back: the Strikers Cup hub's orbit of the trophy
// otherwise ran on through each camera blend into it (issue #49).
bool sOnTransitionTime;
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
// European revision 1 data on the revision 2 code (tweak patch 0019): set by
// the host for such a disc, whatever the enabled tweaks.
bool sRev1Data;

// No HUD (issue #58): whether this frame hides the match HUD. Each match
// starts with the tweak's choice; H, pressed an odd number of times since,
// flips it. The host passes the key's presses from its event thread.
bool sHudHidden;
bool sHudFlipped;
std::atomic<unsigned> sHudKeyPresses;

// Sound levels ([audio] in recharged.ini, a host setting outside the tweak
// mask, issue #59), in the order of mscharged::SoundGroup: the decibels added
// to each group's sounds, and the movies' gain.
enum
{
    kMusic,
    kEffects,
    kVoices,
    kCrowd,
    kMenus,
    kCutscenes,
    kMovies,
    kSoundGroupCount
};
float sSoundDecibels[kSoundGroupCount];
bool sSoundMixed; // a group below 100 %
float sMovieGain = 1.0f;
// The game's mix categories, the calculation sliders of audio/nlxgs.bun
// (UserOptions.cpp names them): global 0, default 1, music 2, dialogue 3,
// sfx 4. Every sound has one; the Options menu sets music, dialogue and sfx.
constexpr unsigned kCategoryMusic = 2;
constexpr unsigned kCategoryDialogue = 3;

bool BankIs(const char* bank, const char* name)
{
    return bank && std::strcmp(bank, name) == 0;
}

bool BankStarts(const char* bank, const char* prefix)
{
    return bank && std::strncmp(bank, prefix, std::strlen(prefix)) == 0;
}

// A sound's group, by its mix category and its sound bank (the same names on
// every disc).
int SoundGroupOf(const char* bank, unsigned category)
{
    // The cutscenes' streams carry their own music and voices.
    if (BankIs(bank, "STREAM_GEN_NIS")) return kCutscenes;
    if (category == kCategoryMusic) return kMusic;
    if (category == kCategoryDialogue) return kVoices;
    if (BankIs(bank, "STREAM_GEN_Crowd") || BankIs(bank, "CROWD_GEN_Sfx")) return kCrowd;
    // The front end's banks and the match HUD's.
    if (BankStarts(bank, "FE_") || BankStarts(bank, "HUD_")) return kMenus;
    return kEffects;
}
} // namespace

// Original SDK (the native VI): the TV format, VI_PAL being 1. The European
// game boots in VI_EURGB60 (5), which runs NTSC fields like VI_NTSC (0).
extern "C" unsigned int VIGetTvFormat(void);
// The original match (Game.cpp): set from match loading until the match ends.
class cGame;
extern cGame* g_pGame;

extern "C" {
// Host entry: the enabled tweaks as bits of mscharged::GameTweak.
__attribute__((visibility("default"))) void charged_configure_tweaks(unsigned mask)
{
    sEnabled = mask;
    sTitlePassed = false;
    sMenus = sTransition = sOnTransitionTime = false;
    sStadium = -1;
    sHudFlipped = false;
    sHudHidden = mscharged_tweak_enabled(MSCHARGED_TWEAK_NO_HUD);
}

// Host entry: the H key was pressed (any thread). A match takes it in its
// next frame; in the menus it does nothing.
__attribute__((visibility("default"))) void charged_toggle_hud(void)
{
    sHudKeyPresses.fetch_add(1, std::memory_order_relaxed);
}

__attribute__((visibility("default"))) void charged_configure_performance_mode(bool enabled)
{
    sPerformanceMode = enabled;
}

// Host entry: the stadium of the match being rendered, -1 outside matches.
__attribute__((visibility("default"))) int charged_take_stadium(void)
{
    const int stadium = sStadium;
    sStadium = -1;
    return stadium;
}

// Host entry: the disc is a European revision 1 one (tweak patch 0019).
__attribute__((visibility("default"))) void charged_configure_rev1_data(bool enabled)
{
    sRev1Data = enabled;
}

// Host entry: the volume (0-100 %) of each sound group, in the order of
// mscharged::SoundGroup; missing groups keep 100 %.
__attribute__((visibility("default"))) void charged_configure_sound_volumes(const int* volumes, int count)
{
    sSoundMixed = false;
    for (int n = 0; n < kSoundGroupCount; ++n)
    {
        int volume = volumes && n < count ? volumes[n] : 100;
        volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
        // 0 % takes any level the game sets below its floor of -96 dB, which
        // the hook keeps and the mixer plays as silence.
        sSoundDecibels[n] = volume == 0 ? -200.0f : 20.0f * std::log10(float(volume) / 100.0f);
        sSoundMixed = sSoundMixed || volume != 100;
        if (n == kMovies) sMovieGain = float(volume) / 100.0f;
    }
}

// Revision 2 inserted FE_GEN_Music at 0x1A of the sound bank table
// (AudioBankID.h); revision 1 loads STREAM_GEN_Music, its 0x30, in that step.
int mscharged_tweak_rev1_sound_bank(int bank)
{
    constexpr int kFeGenMusic = 0x1A;
    constexpr int kRev1MusicStreams = 0x30;
    if (!sRev1Data) return bank;
    if (bank == kFeGenMusic) return kRev1MusicStreams;
    return bank > kFeGenMusic ? bank - 1 : bank;
}

// Revision 2 inserted presentation native 63 (Presentation_interp.cpp).
unsigned int mscharged_tweak_rev1_presentation_function(unsigned int function)
{
    return sRev1Data && function >= 63 ? function + 1 : function;
}

// Revision 1's in-game texture budget (AsyncLoading.cpp).
unsigned long mscharged_tweak_rev1_ingame_texture_budget(unsigned long size)
{
    return sRev1Data ? 0x966666 : size;
}

bool mscharged_tweak_enabled(int tweak)
{
    return tweak >= 0 && tweak < 32 && (sEnabled & (1u << tweak)) != 0;
}

void mscharged_tweak_hud_frame(void)
{
    const unsigned presses = sHudKeyPresses.exchange(0, std::memory_order_relaxed);
    if (!g_pGame)
        sHudFlipped = false;
    else if (presses % 2 != 0)
        sHudFlipped = !sHudFlipped;
    sHudHidden = mscharged_tweak_enabled(MSCHARGED_TWEAK_NO_HUD) != sHudFlipped;
}

bool mscharged_tweak_hud_hidden(void)
{
    return sHudHidden;
}

void mscharged_tweak_report_stadium(int stadium)
{
    sStadium = stadium;
}

float mscharged_tweak_sound_volume(const char* bank, unsigned category)
{
    return sSoundMixed ? sSoundDecibels[SoundGroupOf(bank, category)] : 0.0f;
}

float mscharged_tweak_movie_volume(void)
{
    return sMovieGain;
}

bool mscharged_tweak_performance_mode(void)
{
    return sPerformanceMode;
}

bool mscharged_tweak_performance_match(void)
{
    return sPerformanceMode && g_pGame != nullptr;
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
    // The front end's task, which reports this, keeps real time.
    sOnTransitionTime = false;
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
    // Asked right before each time-dilated task runs.
    sOnTransitionTime = sTransition && task != nullptr && task == sTransitionTask;
    return sOnTransitionTime ? kTransitionDilation : 1.0f;
}

float mscharged_tweak_real_time(float deltaTime)
{
    return sOnTransitionTime ? deltaTime / kTransitionDilation : deltaTime;
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

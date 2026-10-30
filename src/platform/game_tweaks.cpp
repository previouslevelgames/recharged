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
#include "platform/gamecube_controls.h"
#include "platform/tweak_frame_state.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
unsigned sEnabled;

// Skip intro: the title screen is passed once per launch.
bool sTitlePassed;
// Training: the item of the team that trains (ePowerUpType, -1 none), and the
// launcher's Start training, taken by the title once it is passed.
int sTrainingItem = -1;
bool sTrainingStart;
int sTrainingCaptain;
int sTrainingStadium;
int sTrainingAwayCaptain = -1;
int sTrainingAwayFielders;
bool sTrainingAwayGoalie = true;
bool sTrainingSkipGoal;
int sTrainingAwaySkill = -1;
bool sTrainingPlayAway;
bool sPerformanceMode;
// GameCube controls: the host's state of its GameCube players' controllers.
ChargedGameCubePadReader sGameCubePads;
// Wider view (display.wider_view, issue #91): the window's picture shape
// divided by 16:9 while the wider view fills it, 1 otherwise; and whether the
// match's stadium was drawn since the host last asked (it fills the window
// only then: the menus keep the game's own 16:9 picture).
std::atomic<float> sViewShape{1.0f};
bool sMatchView;
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
// The sound being set (SoundPlaybackEvent): its group's level in tenths of a
// decibel, the MIX fader MIXSetInput gives its voices.
int sSoundFader;
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
    sMatchView = false;
    sHudFlipped = false;
    sHudHidden = mscharged_tweak_enabled(MSCHARGED_TWEAK_NO_HUD);
}

// Host entry (Training): the item of the team that trains (ePowerUpType, -1
// none); with `start`, the launcher's Start training: the title is passed at
// once, as by Skip intro, into a match of `captain` (eTeamID) in `stadium`
// (eStadiumID).
__attribute__((visibility("default"))) void charged_configure_training(int item, bool start, int captain, int stadium)
{
    sTrainingItem = item;
    sTrainingStart = start;
    sTrainingCaptain = captain;
    sTrainingStadium = stadium;
}

// Host entry (Training): the opponent, the team nobody plays: its captain
// (eTeamID, -1 for the default), how many of its four fielders play, whether
// its goalie plays, whether a goal goes straight to the kickoff, and its skill
// (0 training, 1 rookie to 5 mega striker, -1 for the game's own setting), and
// whether the player plays the away team.
__attribute__((visibility("default"))) void charged_configure_training_opponent(int captain, int fielders,
                                                                                 bool goalie, bool skip_goal,
                                                                                 int skill, bool play_away)
{
    sTrainingPlayAway = play_away;
    sTrainingAwaySkill = skill < -1 ? -1 : skill > 5 ? 5 : skill;
    sTrainingAwayCaptain = captain;
    sTrainingAwayFielders = fielders < 0 ? 0 : fielders > 4 ? 4 : fielders;
    sTrainingAwayGoalie = goalie;
    sTrainingSkipGoal = skip_goal;
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

// Host entry (GameCube controls): where the game reads the controllers of
// the players who play with them; null for none.
__attribute__((visibility("default"))) void charged_configure_gamecube_controls(ChargedGameCubePadReader reader)
{
    sGameCubePads = reader;
}

// Host entry (wider view): the shape of the window's picture divided by 16:9,
// from the next frame on; 1 draws the game's own 16:9 view.
__attribute__((visibility("default"))) void charged_set_view_shape(float shape)
{
    sViewShape.store(shape > 0.0f && std::isfinite(shape) ? shape : 1.0f, std::memory_order_relaxed);
}

// Host entry (wider view): whether the match's stadium was drawn since the last call.
__attribute__((visibility("default"))) bool charged_take_match_view(void)
{
    const bool drawn = sMatchView;
    sMatchView = false;
    return drawn;
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

void mscharged_tweak_set_sound_level(const char* bank, unsigned category)
{
    // Whole tenths, as the game's own (int)(10 * dB) for MIXSetInput.
    sSoundFader = int(10.0f * mscharged_tweak_sound_volume(bank, category));
}

void mscharged_tweak_clear_sound_level(void)
{
    sSoundFader = 0;
}

int mscharged_tweak_sound_fader(void)
{
    return sSoundFader;
}

bool mscharged_tweak_performance_mode(void)
{
    return sPerformanceMode;
}

bool mscharged_tweak_gamecube_pad(int channel, struct mscharged_gamecube_pad* pad)
{
    ChargedGameCubePad state{};
    if (!sGameCubePads || !pad || !sGameCubePads(channel, &state)) return false;
    pad->buttons = state.buttons;
    pad->trigger_left = state.trigger_left;
    pad->trigger_right = state.trigger_right;
    pad->substick_x = state.substick_x;
    pad->substick_y = state.substick_y;
    return true;
}

float mscharged_tweak_view_shape(void)
{
    return sViewShape.load(std::memory_order_relaxed);
}

void mscharged_tweak_report_match_view(void)
{
    sMatchView = true;
}

namespace {
// How much wider (wide) or taller (tall) than 16:9 the wider view is; 1 each
// for the game's own view.
float ViewWide()
{
    const float shape = mscharged_tweak_view_shape();
    return shape > 1.0f ? shape : 1.0f;
}

float ViewTall()
{
    const float shape = mscharged_tweak_view_shape();
    return shape < 1.0f ? 1.0f / shape : 1.0f;
}
}

void mscharged_tweak_view_perspective(float* fov, float* aspect)
{
    // Wider: the same height, more to the sides. Taller: the same width, more
    // above and below. Either way the game's own view keeps its size.
    const float shape = mscharged_tweak_view_shape();
    if (shape < 1.0f) *fov = 2.0f * std::atan(std::tan(0.5f * *fov) / shape);
    *aspect *= shape;
}

void mscharged_tweak_view_extend(float* width, float* height)
{
    *width *= ViewWide();
    *height *= ViewTall();
}

void mscharged_tweak_view_middle(float* x, float* y, float width, float height)
{
    *x = 0.5f * width + (*x - 0.5f * width) / ViewWide();
    *y = 0.5f * height + (*y - 0.5f * height) / ViewTall();
}

void mscharged_tweak_view_to_wide(float* x, float* y)
{
    *x /= ViewWide();
    *y /= ViewTall();
}

void mscharged_tweak_view_to_original(float* x, float* y)
{
    *x *= ViewWide();
    *y *= ViewTall();
}

void mscharged_tweak_view_cover_uv(float* uv)
{
    // Wider: the texture keeps its width and loses some height; taller: the reverse.
    for (int corner = 0; corner < 4; ++corner) {
        uv[2 * corner] = 0.5f + (uv[2 * corner] - 0.5f) / ViewTall();
        uv[2 * corner + 1] = 0.5f + (uv[2 * corner + 1] - 0.5f) / ViewWide();
    }
}

void mscharged_tweak_view_middle_span(int horizontal, int move_position, int* position, int* length, int extent)
{
    const float scale = 1.0f / (horizontal ? ViewWide() : ViewTall());
    if (scale == 1.0f) return;
    if (move_position) *position = (int)(0.5f * extent + (*position - 0.5f * extent) * scale);
    *length = (int)(*length * scale);
}

bool mscharged_tweak_performance_match(void)
{
    return sPerformanceMode && g_pGame != nullptr;
}

bool mscharged_tweak_take_title_press(void)
{
    if (!mscharged_tweak_title_pending()) return false;
    sTitlePassed = true;
    std::fputs("Tweak: skip intro passed the title screen\n", stderr);
    return true;
}

bool mscharged_tweak_title_pending(void)
{
    return !sTitlePassed && (sTrainingStart || mscharged_tweak_enabled(MSCHARGED_TWEAK_SKIP_INTRO));
}

int mscharged_tweak_training_item(void)
{
    return sTrainingItem;
}

int mscharged_tweak_training_away_captain(void)
{
    return sTrainingAwayCaptain;
}

int mscharged_tweak_training_away_fielders(void)
{
    return sTrainingAwayFielders;
}

bool mscharged_tweak_training_away_goalie(void)
{
    return sTrainingAwayGoalie;
}

bool mscharged_tweak_training_play_away(void)
{
    return sTrainingPlayAway;
}

int mscharged_tweak_training_away_skill(void)
{
    return sTrainingAwaySkill;
}

bool mscharged_tweak_training_skip_goal(void)
{
    return sTrainingSkipGoal;
}

bool mscharged_tweak_take_training_start(int* captain, int* stadium)
{
    if (!sTrainingStart) return false;
    sTrainingStart = false;
    *captain = sTrainingCaptain;
    *stadium = sTrainingStadium;
    std::fputs("Tweak: training starts from the title screen\n", stderr);
    return true;
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

void mscharged_tweak_save_frame_state(MschargedTweakFrameState* state)
{
    *state = {sOriginalFrames, sOriginalPhase, sOriginalFrameCount, sOriginalFrame};
}

void mscharged_tweak_load_frame_state(const MschargedTweakFrameState* state)
{
    sOriginalFrames = state->frames;
    sOriginalPhase = state->phase;
    sOriginalFrameCount = state->count;
    sOriginalFrame = state->frame;
}
}

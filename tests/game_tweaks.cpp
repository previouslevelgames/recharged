// The game module's tweak state (src/platform/game_tweaks.cpp): with no tweak
// enabled, every hook gets the original game's answer; each tweak acts alone.
// A frame rate above the original keeps the original's per-frame steps.
#include "bootstrap/game_tweaks.h"
#include "mscharged_tweaks.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

extern "C" void charged_configure_tweaks(unsigned mask);
extern "C" void charged_configure_performance_mode(bool enabled);
extern "C" void charged_configure_frame_pacing(void (*swap)());

// The native VI's TV format (VI_NTSC 0, VI_PAL 1) for the original frame.
unsigned gTvFormat;
extern "C" unsigned int VIGetTvFormat(void) { return gTvFormat; }

namespace
{
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

// Every hook's answer when it must leave the original behaviour alone.
void CheckOriginal(const char* what)
{
    const int task = 0, other = 0, slide = 0;
    mscharged_tweak_set_transition_task(&task);
    for (bool menus : {false, true})
        for (bool transition : {false, true})
        {
            mscharged_tweak_frontend_frame(menus, transition);
            mscharged_tweak_note_slide_reset(&slide);
            Check(!mscharged_tweak_fast_menus() && !mscharged_tweak_fast_transition(), what);
            Check(mscharged_tweak_task_dilation(&task) == 1.0f && mscharged_tweak_real_time(0.25f) == 0.25f, what);
            Check(mscharged_tweak_task_dilation(&other) == 1.0f && mscharged_tweak_real_time(0.25f) == 0.25f, what);
        }
    Check(!mscharged_tweak_title_pending() && !mscharged_tweak_take_title_press(), what);
}

// The frame-rate hooks' answers at the original pacing.
void CheckOriginalFrames(const char* what)
{
    Check(!mscharged_tweak_host_frames(), what);
    for (unsigned state : {1u, 2u, 4u, 8u, 16u, 0x2000000u})
        for (int mode = 0; mode < 4; ++mode)
            Check(mscharged_tweak_swap_mode(state, mode) == mode, what);
    for (float seconds : {0.0f, 1.0f / 240.0f, 1.0f / 60.0f, 0.5f})
    {
        mscharged_tweak_begin_frame(seconds);
        Check(mscharged_tweak_original_frames() == 1.0f && mscharged_tweak_original_frame(), what);
        Check(mscharged_tweak_frame_number(1234) == 1234, what);
    }
}

int swaps;
void Swap() { ++swaps; }

// Original frames begun over `seconds` of frames `frame` seconds long.
unsigned long OriginalFramesOver(double seconds, float frame, int swapMode = 1)
{
    charged_configure_frame_pacing(Swap);
    Check(mscharged_tweak_swap_mode(2, swapMode) == MSCHARGED_SWAP_HOST, "Raised frame rate kept the VI swap");
    const unsigned long first = mscharged_tweak_frame_number(0);
    unsigned long begun = 0;
    for (double t = 0.0; t < seconds - 1e-9; t += frame)
    {
        mscharged_tweak_begin_frame(frame);
        begun += mscharged_tweak_original_frame() ? 1 : 0;
    }
    Check(mscharged_tweak_frame_number(0) - first == begun, "Original frame count and flags disagree");
    return begun;
}
}

int main()
{
    try
    {
        static_assert(unsigned(mscharged::GameTweak::SkipIntro) == MSCHARGED_TWEAK_SKIP_INTRO
            && unsigned(mscharged::GameTweak::FastMenus) == MSCHARGED_TWEAK_FAST_MENUS
            && unsigned(mscharged::GameTweak::NoMegaStrikes) == MSCHARGED_TWEAK_NO_MEGA_STRIKES
            && unsigned(mscharged::GameTweak::UnlockAll) == MSCHARGED_TWEAK_UNLOCK_ALL
            && unsigned(mscharged::GameTweak::CpuVsCpu) == MSCHARGED_TWEAK_CPU_VS_CPU
            && unsigned(mscharged::GameTweak::FastTerrain) == MSCHARGED_TWEAK_FAST_TERRAIN
            && unsigned(mscharged::GameTweak::MirrorMatch) == MSCHARGED_TWEAK_MIRROR_MATCH
            && unsigned(mscharged::GameTweak::ThunderIslandWalls) == MSCHARGED_TWEAK_THUNDER_ISLAND_WALLS
            && unsigned(mscharged::GameTweak::BluePeach) == MSCHARGED_TWEAK_BLUE_PEACH,
            "Host tweak bits and the game hooks' IDs must agree");

        // Before configuration and with an empty set: the original game.
        CheckOriginal("Unconfigured tweaks changed the original game");
        Check(!mscharged_tweak_performance_mode(), "Performance mode did not default to off");
        charged_configure_performance_mode(true);
        charged_configure_tweaks(0);
        Check(mscharged_tweak_performance_mode(), "Gameplay tweak configuration changed performance mode");
        charged_configure_performance_mode(false);
        Check(!mscharged_tweak_performance_mode(), "Performance mode could not be disabled");
        CheckOriginal("Tweaks off changed the original game");
        for (int tweak = -1; tweak < 33; ++tweak)
            Check(!mscharged_tweak_enabled(tweak), "A tweak is enabled while all are off");

        // Each tweak alone enables only itself.
        for (const auto& info : mscharged::kGameTweaks)
        {
            charged_configure_tweaks(mscharged::GameTweakBit(info.tweak));
            for (const auto& other : mscharged::kGameTweaks)
                Check(mscharged_tweak_enabled(int(other.tweak)) == (other.tweak == info.tweak), "Tweak bits mixed");
        }

        // Skip intro: the title screen is passed once per launch.
        charged_configure_tweaks(mscharged::GameTweakBit(mscharged::GameTweak::SkipIntro));
        Check(mscharged_tweak_title_pending(), "Skip intro: title not pending");
        Check(mscharged_tweak_take_title_press(), "Skip intro: no title press");
        Check(!mscharged_tweak_title_pending() && !mscharged_tweak_take_title_press(), "Skip intro: second title press");
        mscharged_tweak_frontend_frame(true, true);
        Check(!mscharged_tweak_fast_menus(), "Skip intro enabled fast menus");

        // Fast menus: only in the menus, and transition time only during a
        // transition and only for the camera task; effects (issue #34), audio
        // and every other task keep real time.
        charged_configure_tweaks(mscharged::GameTweakBit(mscharged::GameTweak::FastMenus));
        const int camera = 0, particles = 0, audio = 0, slide = 0, other = 0;
        mscharged_tweak_set_transition_task(&camera);
        mscharged_tweak_frontend_frame(false, true);
        Check(!mscharged_tweak_fast_menus() && !mscharged_tweak_fast_transition()
            && mscharged_tweak_task_dilation(&camera) == 1.0f, "Fast menus outside the menus");
        mscharged_tweak_frontend_frame(true, false);
        Check(mscharged_tweak_fast_menus() && !mscharged_tweak_fast_transition()
            && mscharged_tweak_task_dilation(&camera) == 1.0f, "Fast menus without a transition");
        mscharged_tweak_frontend_frame(true, true);
        Check(mscharged_tweak_fast_transition() && mscharged_tweak_task_dilation(&camera) > 1.0f
            && mscharged_tweak_task_dilation(&particles) == 1.0f && mscharged_tweak_task_dilation(&audio) == 1.0f
            && mscharged_tweak_task_dilation(nullptr) == 1.0f, "Fast menus transition time");
        // A looping camera takes the camera task's real time back (issue #49);
        // in every other task a step already is real time.
        const float frame = 1.0f / 60.0f, transition = frame * mscharged_tweak_task_dilation(&camera);
        Check(transition > frame && std::fabs(mscharged_tweak_real_time(transition) - frame) < 1e-7f,
            "Fast menus: camera task's real time");
        Check(mscharged_tweak_task_dilation(&particles) == 1.0f && mscharged_tweak_real_time(frame) == frame,
            "Fast menus: real time outside the camera task");
        mscharged_tweak_task_dilation(&camera);
        mscharged_tweak_frontend_frame(true, true);
        Check(mscharged_tweak_real_time(frame) == frame, "Fast menus: front end task's real time");
        mscharged_tweak_frontend_frame(true, false);
        Check(mscharged_tweak_task_dilation(&camera) == 1.0f && mscharged_tweak_real_time(frame) == frame,
            "Fast menus: real time without a transition");
        mscharged_tweak_frontend_frame(true, true);
        mscharged_tweak_note_slide_reset(&slide);
        Check(mscharged_tweak_slide_reset_this_frame(&slide) && !mscharged_tweak_slide_reset_this_frame(&other),
            "Slide reset not recorded for this frame only");
        mscharged_tweak_frontend_frame(true, true);
        Check(!mscharged_tweak_slide_reset_this_frame(&slide), "Slide reset kept into the next frame");
        Check(!mscharged_tweak_title_pending(), "Fast menus enabled skip intro");

        // Frame rate: the original pacing until the host passes its swap.
        CheckOriginalFrames("Unconfigured frame pacing changed the original game");
        charged_configure_frame_pacing(nullptr);
        CheckOriginalFrames("Original frame rate changed the original game");
        mscharged_tweak_host_swap();

        // Above the original rate every state but the HOME Button Menu swaps
        // through the host, which presents and paces each frame.
        charged_configure_frame_pacing(Swap);
        Check(mscharged_tweak_host_frames(), "Raised frame rate not active");
        for (unsigned state : {1u, 2u, 4u, 8u, 16u, 0x10000u, 0x100000u})
            for (int mode = 0; mode < 4; ++mode)
                Check(mscharged_tweak_swap_mode(state, mode) == MSCHARGED_SWAP_HOST, "State kept the VI swap");
        Check(mscharged_tweak_swap_mode(0x2000000u, 3) == 3, "HOME Button Menu lost its VI pacing");
        mscharged_tweak_host_swap();
        Check(swaps == 1, "Host swap not called");

        // A frame's length in original frames, and original frames begun at
        // the original's rate: 59.94 a second (NTSC fields), 50 in PAL, and
        // half that where the original takes two fields a frame (mode 2).
        mscharged_tweak_swap_mode(2, 1);
        mscharged_tweak_begin_frame(1.0f / 120.0f);
        Check(mscharged_tweak_original_frames() > 0.4994f && mscharged_tweak_original_frames() < 0.4996f,
            "120 FPS frame length in original frames");
        for (const float rate : {120.0f, 144.0f, 165.0f, 240.0f, 1000.0f})
        {
            const auto begun = OriginalFramesOver(10.0, 1.0f / rate);
            Check(begun >= 598 && begun <= 600, "Original frames a second at a raised rate");
        }
        Check(OriginalFramesOver(1.0, 1.0f / 120.0f) == 60 || OriginalFramesOver(1.0, 1.0f / 120.0f) == 59,
            "Original frames in a second at 120 FPS");
        gTvFormat = 1;
        const auto pal = OriginalFramesOver(10.0, 1.0f / 144.0f);
        Check(pal >= 499 && pal <= 500, "PAL original frames at 144 FPS");
        gTvFormat = 0;
        const auto locked = OriginalFramesOver(10.0, 1.0f / 120.0f, 2);
        Check(locked >= 298 && locked <= 300, "Two-field original frames at 120 FPS");
        // Frames longer than the original's each begin one, and the task
        // manager's 0.1 s bound holds for a stall.
        Check(OriginalFramesOver(1.0, 1.0f / 30.0f) == 30, "Original frames at 30 FPS");
        mscharged_tweak_begin_frame(1.0f / 30.0f);
        Check(mscharged_tweak_original_frames() > 1.99f && mscharged_tweak_original_frames() < 2.0f,
            "30 FPS frame length in original frames");
        mscharged_tweak_begin_frame(5.0f);
        Check(mscharged_tweak_original_frame() && mscharged_tweak_original_frames() < 6.0f,
            "A stalled frame exceeded the task manager's bound");
        mscharged_tweak_begin_frame(-1.0f);
        Check(mscharged_tweak_original_frames() == 0.0f, "Negative frame length");

        // Back to the original rate and to no tweaks: the original game again.
        charged_configure_frame_pacing(nullptr);
        CheckOriginalFrames("Frame rate turned off still changes the game");
        charged_configure_tweaks(0);
        CheckOriginal("Tweaks turned off still change the game");
        std::printf("Game tweak state: %u checks passed.\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

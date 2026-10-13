// The game module's tweak state (src/platform/game_tweaks.cpp): with no tweak
// enabled, every hook gets the original game's answer; each tweak acts alone.
#include "bootstrap/game_tweaks.h"
#include "mscharged_tweaks.h"

#include <cstdio>
#include <stdexcept>

extern "C" void charged_configure_tweaks(unsigned mask);

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
            Check(mscharged_tweak_task_dilation(&task) == 1.0f && mscharged_tweak_task_dilation(&other) == 1.0f, what);
        }
    Check(!mscharged_tweak_title_pending() && !mscharged_tweak_take_title_press(), what);
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
            && unsigned(mscharged::GameTweak::CpuVsCpu) == MSCHARGED_TWEAK_CPU_VS_CPU,
            "Host tweak bits and the game hooks' IDs must agree");

        // Before configuration and with an empty set: the original game.
        CheckOriginal("Unconfigured tweaks changed the original game");
        charged_configure_tweaks(0);
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
        mscharged_tweak_note_slide_reset(&slide);
        Check(mscharged_tweak_slide_reset_this_frame(&slide) && !mscharged_tweak_slide_reset_this_frame(&other),
            "Slide reset not recorded for this frame only");
        mscharged_tweak_frontend_frame(true, true);
        Check(!mscharged_tweak_slide_reset_this_frame(&slide), "Slide reset kept into the next frame");
        Check(!mscharged_tweak_title_pending(), "Fast menus enabled skip intro");

        // Back to none: the original game again.
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

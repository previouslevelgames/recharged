// Release builds (MSCHARGED_AUTOPILOT off): the step's pads stay the player's,
// and the host finds nothing to switch (test builds link autopilot.cpp instead).
// Neutral names: release modules carry these two symbols.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error This file belongs to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_autopilot.h"

extern "C" __attribute__((visibility("default"))) int charged_set_pad_source(int)
{
    return 0;
}

void mscharged_tweak_step_pads(PlatPadManager*) {}

// Simulation profiles (tweak patch 0057), inside the original game module:
// the rules this module simulates the match under way by. The hooks of the
// simulation ask mscharged_sim_profile; the online draft (tweak patch 0058)
// sets a network match's profile before the match loads and puts the
// module's own back outside network matches. The profile is not one of the
// original game's globals, so a rollback leaves it as the draft set it.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error Simulation profiles belong to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_sim_profile.h"

#include <cstdio>

namespace
{
unsigned sProfile = MSCHARGED_SIM_NATIVE;

const char* Name(unsigned profile)
{
    switch (profile)
    {
    case MSCHARGED_SIM_R4QE01: return "USA (R4QE01)";
    case MSCHARGED_SIM_R4QP01_REV1: return "Europe revision 1 (R4QP01)";
    case MSCHARGED_SIM_R4QP01_REV2: return "Europe revision 2 (R4QP01)";
    case MSCHARGED_SIM_R4QJ01: return "Japan (R4QJ01)";
    case MSCHARGED_SIM_R4QK01: return "Korea (R4QK01)";
    default: return "unknown";
    }
}
} // namespace

extern "C" unsigned int mscharged_sim_profile(void)
{
    return sProfile;
}

extern "C" unsigned int mscharged_sim_resolve(const unsigned char* natives, int count)
{
    // Machines that did not say (a LAN draft, which only machines of one
    // release join) leave the others' choice.
    unsigned shared = 0;
    bool differ = false;
    for (int i = 0; i < count; ++i)
    {
        const unsigned native = natives[i];
        std::fprintf(stderr, "Simulation: machine %d of the draft plays %s\n", i,
                     native == 0 ? "a release it does not say" : Name(native));
        if (native == 0) continue;
        differ = differ || (shared != 0 && native != shared);
        shared = native;
    }
    if (differ) return MSCHARGED_SIM_CANONICAL;
    return shared != 0 ? shared : MSCHARGED_SIM_NATIVE;
}

extern "C" void mscharged_sim_set_profile(unsigned int profile)
{
    // One release, and one this module simulates.
    if (profile == 0 || (profile & (profile - 1)) != 0 || (profile & MSCHARGED_SIM_SUPPORTED) == 0)
    {
        std::fprintf(stderr, "Simulation: this game cannot play by the rules of profile 0x%x, kept %s\n", profile,
                     Name(sProfile));
        return;
    }
    if (profile == sProfile) return;
    sProfile = profile;
    std::fprintf(stderr, "Simulation: the rules of %s%s\n", Name(profile),
                 profile == MSCHARGED_SIM_NATIVE ? "" : ", for the other releases in the match");
}

extern "C" void mscharged_sim_reset_profile(void)
{
    sProfile = MSCHARGED_SIM_NATIVE;
}

// The game module's simulation profile (src/platform/sim_profile.cpp), built
// as the module of every release: outside network matches each simulates by
// its own release's rules; a draft of one release keeps them, a draft of
// several takes the European revision 2's; and a module never takes rules it
// cannot simulate.
#include "mscharged_sim_profile.h"

#include <cstdio>
#include <stdexcept>

namespace
{
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

unsigned Resolve(unsigned a, unsigned b)
{
    const unsigned char natives[2] = {static_cast<unsigned char>(a), static_cast<unsigned char>(b)};
    return mscharged_sim_resolve(natives, 2);
}
} // namespace

int main()
{
    try
    {
        Check(mscharged_sim_profile() == MSCHARGED_SIM_NATIVE, "A module starts by another release's rules");

        Check(Resolve(MSCHARGED_SIM_R4QE01, MSCHARGED_SIM_R4QE01) == MSCHARGED_SIM_R4QE01,
              "Two USA machines leave the USA rules");
        Check(Resolve(MSCHARGED_SIM_R4QP01_REV2, MSCHARGED_SIM_R4QP01_REV2) == MSCHARGED_SIM_R4QP01_REV2,
              "Two European machines leave the European rules");
        Check(Resolve(MSCHARGED_SIM_R4QE01, MSCHARGED_SIM_R4QP01_REV2) == MSCHARGED_SIM_CANONICAL
                  && Resolve(MSCHARGED_SIM_R4QP01_REV2, MSCHARGED_SIM_R4QE01) == MSCHARGED_SIM_CANONICAL,
              "A USA and a European machine do not fall back to the European revision 2");
        Check(Resolve(MSCHARGED_SIM_R4QJ01, MSCHARGED_SIM_R4QK01) == MSCHARGED_SIM_CANONICAL,
              "Japan and Korea do not fall back to the European revision 2");
        Check(Resolve(0, MSCHARGED_SIM_R4QE01) == MSCHARGED_SIM_R4QE01, "A machine that does not say decides");
        Check(Resolve(0, 0) == MSCHARGED_SIM_NATIVE, "A LAN draft leaves the module's own rules");

        mscharged_sim_set_profile(MSCHARGED_SIM_CANONICAL);
        Check(mscharged_sim_profile() == MSCHARGED_SIM_CANONICAL, "A module refuses the canonical rules");
        Check(MSCHARGED_SIM_IS(MSCHARGED_SIM_EUROPE) && !MSCHARGED_SIM_IS(MSCHARGED_SIM_USA_JAPAN_KOREA),
              "The canonical rules are not the European ones");
        Check(!MSCHARGED_SIM_IS(MSCHARGED_SIM_R4QK01), "The canonical rules test the cone as Korea does");
        // Japan's rules in the Korean module, Korea's in every other.
        const unsigned foreign =
            (MSCHARGED_SIM_SUPPORTED & MSCHARGED_SIM_R4QK01) != 0 ? MSCHARGED_SIM_R4QJ01 : MSCHARGED_SIM_R4QK01;
        mscharged_sim_set_profile(foreign);
        Check(mscharged_sim_profile() == MSCHARGED_SIM_CANONICAL, "A module takes rules it cannot simulate");
        mscharged_sim_set_profile(MSCHARGED_SIM_R4QE01 | MSCHARGED_SIM_R4QP01_REV2);
        Check(mscharged_sim_profile() == MSCHARGED_SIM_CANONICAL, "A module takes two releases' rules at once");
        mscharged_sim_reset_profile();
        Check(mscharged_sim_profile() == MSCHARGED_SIM_NATIVE, "Leaving a network match keeps its rules");
        Check(MSCHARGED_SIM_IS(MSCHARGED_SIM_R4QK01) == (MSCHARGED_SIM_NATIVE == MSCHARGED_SIM_R4QK01),
              "Only the Korean module's own rules test the cone as Korea does");
#if defined(MSCHARGED_SIM_RUNTIME)
        Check(MSCHARGED_SIM_IS(MSCHARGED_SIM_USA_JAPAN_KOREA),
              "The module's own rules are not the USA's, Japan's or Korea's");
#else
        Check(MSCHARGED_SIM_NATIVE == MSCHARGED_SIM_CANONICAL, "Only the European module has one profile");
#endif

        std::printf("Simulation profile: %u checks passed.\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

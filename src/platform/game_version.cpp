// Compiled into each game module: the release its decomp sources resolve to.
// The host checks it after loading the module for the inserted disc.
#if (defined(VERSION_R4QE01) + defined(VERSION_R4QP01_REV1) + defined(VERSION_R4QP01_REV2) \
     + defined(VERSION_R4QJ01) + defined(VERSION_R4QK01)) != 1
#error "A game module is built for exactly one game version"
#endif

#if defined(VERSION_R4QE01)
#define MSCHARGED_GAME_VERSION_ID "R4QE01"
#elif defined(VERSION_R4QP01_REV1)
#define MSCHARGED_GAME_VERSION_ID "R4QP01_REV1"
#elif defined(VERSION_R4QP01_REV2)
#define MSCHARGED_GAME_VERSION_ID "R4QP01_REV2"
#elif defined(VERSION_R4QJ01)
#define MSCHARGED_GAME_VERSION_ID "R4QJ01"
#else
#define MSCHARGED_GAME_VERSION_ID "R4QK01"
#endif

extern "C" __attribute__((visibility("default"))) const char* charged_game_version()
{
    return MSCHARGED_GAME_VERSION_ID;
}

#pragma once

// A desync dossier: what the rollback knew at a network match's first desync
// (src/platform/rollback_fields.cpp), for the WFC server, which pairs it with
// the other side's and finds the first field of the checksum that differs.
// The game module keeps it (charged_desync_dossier); the host stamps it with
// its series number, its release, build and platform, writes it to
// <data folder>/desyncs/ at once (a crash right after the desync leaves it
// there) and sends it to /api/v1/wfc/desyncs; it deletes it once the server
// answered (src/platform/online_reports.h). The operating system goes in the
// match reports. A dossier not delivered waits for
// the next series of its profile, in this run or a later one.
//
// On disk and on the wire alike, little-endian (every platform the game runs
// on): a DesyncDossierHeader, then `steps` times a DesyncDossierStep's three
// words followed by its `count` DesyncDossierFields. Fixed-size strings are
// NUL-padded.

#include <cstdint>

constexpr char kDesyncDossierMagic[4] = {'R', 'C', 'D', 'S'};
constexpr std::uint32_t kDesyncDossierVersion = 1;
// The step before the desync's and the desync's: checksums are sent every
// other step, so the machines may have diverged a step earlier.
constexpr std::uint32_t kDesyncDossierSteps = 2;
// The fields kept of a step: about 300 in a match; more only with the
// rollback's diagnostic switches (poses, bodies, collisions).
constexpr std::uint32_t kDesyncDossierFields = 512;
constexpr unsigned kDesyncDossierFieldBytes = 16;

// One field of a step's checksum, as RunningChecksum::ChecksumData took it.
struct DesyncDossierField
{
    // Where it was taken: an offset in the game module (for addr2line on that
    // build's module), or one of rollback_fields.cpp's tagged kinds (the top
    // four bits: a pose, a body, a collision...).
    std::uint32_t caller;
    std::uint32_t size;                            // its bytes
    std::uint32_t hash;                            // FNV-1a of all of them
    unsigned char bytes[kDesyncDossierFieldBytes]; // the first ones, zero-padded
};
static_assert(sizeof(DesyncDossierField) == 28, "the dossier's field layout is the server's");

// A step: its frame, its field count, whether the step took more fields than
// it lists (1) or none were kept for it (count 0). Its fields follow.
struct DesyncDossierStep
{
    std::uint32_t frame;
    std::uint32_t count;
    std::uint32_t overflowed;
};
static_assert(sizeof(DesyncDossierStep) == 12, "the dossier's step layout is the server's");

struct DesyncDossierHeader
{
    char magic[4];         // kDesyncDossierMagic
    std::uint32_t version; // kDesyncDossierVersion
    // The series' number in the match reports (OnlineReports); in the game
    // module, its own count of series (ChargedOnlineMatchSnapshot::series)
    // until the host stamps it.
    std::uint64_t series;
    std::uint64_t written_at;     // Unix seconds, by the host
    std::uint32_t profile_id;     // the WFC profile of the Mii online
    std::uint32_t game;           // the game of the series, from 1 (its report's number)
    std::uint32_t side;           // 0 home, 1 away
    std::uint32_t frame;          // the step whose checksums differ
    std::uint32_t checksum[2];    // this machine's, the other's
    std::uint32_t seed[2];        // this machine's, the other's
    std::int32_t machine;         // the other machine's index
    std::uint32_t input_delay;    // steps, at the desync
    std::uint32_t mispredictions; // inputs predicted wrong in the match, every machine's
    // The rollback's diagnostic switches on this machine: bit 0 the poses, 1
    // the physics bodies, 2 the collisions (RECHARGED_ROLLBACK_POSES...).
    std::uint32_t options;
    char release[4];     // R4QE, R4QP...
    char build[64];      // the program's version
    char platform[16];   // HostPlatform: the module's system and CPU
    std::uint32_t steps; // kDesyncDossierSteps
};
static_assert(sizeof(DesyncDossierHeader) == 160, "the dossier's header layout is the server's");

// The largest dossier.
constexpr unsigned kDesyncDossierMaxBytes =
    sizeof(DesyncDossierHeader)
    + kDesyncDossierSteps * (sizeof(DesyncDossierStep) + kDesyncDossierFields * sizeof(DesyncDossierField));

#ifdef __cplusplus
extern "C" {
#endif
// Game module entry: copies the dossier of the last desync not handed over
// yet into `out` and forgets it; its size, 0 when there is none or
// `capacity` is too small. On the thread of the game's tasks, after a frame.
typedef unsigned (*ChargedDesyncDossierFunction)(void* out, unsigned capacity);
#ifdef __cplusplus
}
#endif

#if defined(MSCHARGED_GAME_MODULE)
// The game module's online series (online_match_snapshot.cpp): fills the
// header's series (the module's count), game, profile and side for the online
// game under way, beginning its series when the host had not looked since its
// kickoff; false outside one (a LAN match) or without a WFC profile.
bool ChargedOnlineDossierGame(DesyncDossierHeader& header);
#endif

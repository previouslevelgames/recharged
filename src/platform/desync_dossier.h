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
// words followed by its `count` DesyncDossierFields. From version 2 a
// DesyncDossierMatch follows, then its `input_steps` times a
// DesyncDossierInputStep followed by its `machines` DesyncDossierMachineInputs.
// Fixed-size strings are NUL-padded.

#include <cstdint>

constexpr char kDesyncDossierMagic[4] = {'R', 'C', 'D', 'S'};
// Version 2 (1.1.7) adds the game as the rollback played it and the inputs of
// the steps before the desync. Version 1 (1.1.6) has the header and the
// fields alone; the host still sends its files.
constexpr std::uint32_t kDesyncDossierVersion = 2;
constexpr std::uint32_t kDesyncDossierFirstVersion = 1;
// The steps whose fields a dossier lists, oldest first. Version 1: the step
// before the desync's and the desync's (checksums are sent every other step,
// so the machines may have diverged a step earlier). Version 2 adds the steps
// whose checksums this machine sent since the last of the other machine's
// that matched here: each machine finds the desync at a step the other sent,
// so the two sides' dossiers both list the step each one found, and the
// server compares them there. At most kDesyncDossierSteps.
constexpr std::uint32_t kDesyncDossierFirstSteps = 2;
constexpr std::uint32_t kDesyncDossierSteps = 16;
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
    std::uint32_t mispredictions; // steps predicted wrong in the game, every machine's (version 1: in the session)
    // The rollback's diagnostic switches on this machine: bit 0 the poses, 1
    // the physics bodies, 2 the collisions (RECHARGED_ROLLBACK_POSES...).
    std::uint32_t options;
    char release[4];     // R4QE, R4QP...
    char build[64];      // the program's version
    char platform[16];   // HostPlatform: the module's system and CPU
    std::uint32_t steps; // version 1: kDesyncDossierFirstSteps; 2: up to kDesyncDossierSteps
};
static_assert(sizeof(DesyncDossierHeader) == 160, "the dossier's header layout is the server's");

// The inputs kept: of the steps through the desync's, as many as it lists.
// A divergence shows in the checksum only once it moves what the checksum
// takes, maybe some steps after the inputs that made it.
constexpr std::uint32_t kDesyncDossierInputSteps = 32;
constexpr std::uint32_t kDesyncDossierMachines = 4;

// Version 2: the game as this machine's rollback played it, its counts since
// the game began. A misprediction the rollback could no longer correct
// (kept_predicted, impossible, stopped_predicted) desyncs.
struct DesyncDossierMatch
{
    std::int32_t local_machine;                         // this machine's index
    std::uint32_t machines;                             // the game's
    std::uint32_t tracking_from;                        // the step the rollback's tracking last started after
    std::uint32_t tracking;                             // 1: still tracking at the desync
    std::uint32_t start_delay;                          // steps: the input delay the game began with
    std::uint32_t delay_longer;                         // times this machine's input delay grew
    std::uint32_t delay_shorter;                        // and shrank
    std::uint32_t delay_changed_at;                     // the step of its last change, 0: none
    std::uint32_t mispredicted[kDesyncDossierMachines]; // by machine: steps whose inputs came other than predicted
    std::uint32_t predicted_steps;                      // steps run with any input predicted
    std::uint32_t correction_rollbacks;                 // rollbacks to correct a misprediction
    std::uint32_t undone;                               // predicted steps undone: they changed the phase
    std::uint32_t truncated;                            // replays cut at a phase change
    std::uint32_t kept_predicted;                       // predicted steps kept for a disc read
    std::uint32_t impossible;                           // rollbacks that could not go back
    std::uint32_t stopped_predicted;                    // tracking stopped with inputs still predicted
    std::uint32_t lockstep_waits;                       // steps that waited for inputs, as the original router
    std::uint32_t window_waits;                         // steps that waited, the prediction window full
    std::uint32_t input_steps;                          // kDesyncDossierInputSteps
};
static_assert(sizeof(DesyncDossierMatch) == 88, "the dossier's match layout is the server's");

// A step's inputs, once all had arrived, the oldest step first; the desync's
// is the last. Its `machines` DesyncDossierMachineInputs follow (none when
// they were not kept).
struct DesyncDossierInputStep
{
    std::uint32_t frame;
    std::uint8_t kept;         // 1: kept; 0: gone from the rollback's ring, nothing else set
    std::uint8_t machines;     // that follow
    std::uint8_t predicted;    // one bit per machine: its inputs were predicted when the step first ran
    std::uint8_t mispredicted; // one bit per machine: they came other than predicted
    // By team and player (cTeam::m_pPlayers): the pad controlling it at the
    // step, machine * 4 + the machine's player; -1: none (the AI).
    std::int8_t pads[2][5];
    std::uint8_t reserved[2];
};
static_assert(sizeof(DesyncDossierInputStep) == 20, "the dossier's input step layout is the server's");

// A machine's inputs of a step, as its input message made them.
struct DesyncDossierMachineInput
{
    std::uint8_t players;          // its players: records and states that count
    std::uint8_t event_count;      // events that count
    std::uint16_t remap;           // its remap angle
    std::uint8_t states[4];        // each player's connection state
    std::uint8_t records[4][16];   // each player's PackedDetInput
    std::uint8_t events[4][0x24];  // its deterministic data events (DetermDataEvent)
};
static_assert(sizeof(DesyncDossierMachineInput) == 216, "the dossier's machine input layout is the server's");

// The largest dossier.
constexpr unsigned kDesyncDossierMaxBytes =
    sizeof(DesyncDossierHeader)
    + kDesyncDossierSteps * (sizeof(DesyncDossierStep) + kDesyncDossierFields * sizeof(DesyncDossierField))
    + sizeof(DesyncDossierMatch)
    + kDesyncDossierInputSteps
          * (sizeof(DesyncDossierInputStep) + kDesyncDossierMachines * sizeof(DesyncDossierMachineInput));

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

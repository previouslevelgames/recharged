// Match replays inside the original game module (tweak patch 0071,
// include/mscharged_replay.h). The game's own input recorder hands each
// offline match to these hooks; a network match's steps come from the
// rollback instead (match_replay_network.h), which takes every step's inputs
// from the router. The stream (match_replay_stream.h) waits here until the
// host takes it and keeps it in a .msc file. A replay the host hands over
// plays back through the same recorder: the title goes straight into its
// match with the recorded setup, and the router takes each step's inputs from
// it while the port compares the recorded checksums with the game's own.
//
// Everything here runs on the game's thread except charged_set_replay_speed
// (the host's key watch). Memory comes from malloc (MallocAllocator), not the
// game's heap.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error Match replays belong to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_replay.h"
#include "mscharged_sim_profile.h"
#include "mscharged_tweaks.h"
#include "platform/game_tweak_state.h"
#include "platform/match_replay_network.h"
#include "platform/match_replay_status.h"
#include "platform/match_replay_stream.h"

#include "Game/DB/SaveLoad.h"
#include "Game/DB/UserOptions.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkGameConfig.h"
#include "Game/NetworkInputRecording.h"
#include "Game/NetworkSession.h"
#include "Game/Team.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

extern "C" bool mscharged_rollback_predicting(void);
// The set's match and its tweaks (competitive_match.cpp).
extern "C" unsigned int mscharged_competitive_match_tweaks(void);
extern "C" void mscharged_competitive_replay(bool replaying, unsigned int tweaks);

namespace
{
using namespace mscharged::replay;

// The tweaks that only change how the menus and HUD look or how fast the
// title is passed: a replay keeps the viewer's own instead of the recorded.
constexpr unsigned kViewerTweaks = (1u << MSCHARGED_TWEAK_SKIP_INTRO) | (1u << MSCHARGED_TWEAK_FAST_MENUS)
    | (1u << MSCHARGED_TWEAK_NO_HUD) | (1u << MSCHARGED_TWEAK_OVERRIDE_RETAIL_MENUS);
// Online, the players' own gameplay tweaks are off (their hooks test
// IsOnline), but Feder's fair ball contests, which every online match plays
// by. A recording of an online match keeps the tweaks as they acted, so its
// replay, which plays offline, acts the same; a set's tweaks come apart, as
// the set's (Setup::set_tweaks).
constexpr unsigned kOffOnline = (1u << MSCHARGED_TWEAK_NO_MEGA_STRIKES) | (1u << MSCHARGED_TWEAK_UNLOCK_ALL)
    | (1u << MSCHARGED_TWEAK_CPU_VS_CPU) | (1u << MSCHARGED_TWEAK_FAST_TERRAIN) | (1u << MSCHARGED_TWEAK_MIRROR_MATCH)
    | (1u << MSCHARGED_TWEAK_THUNDER_ISLAND_WALLS) | (1u << MSCHARGED_TWEAK_SUPER_TEAM)
    | (1u << MSCHARGED_TWEAK_SECURE_STADIUMS);
constexpr unsigned kOnOnline = 1u << MSCHARGED_TWEAK_FAIR_BALL_CONTESTS;
// A recording shorter than a second (a match left at once) is not kept.
constexpr std::uint32_t kShortestSteps = 50;
// The memory a recording reserves when its match begins, so that the match
// allocates none: an hour of a network match with changing inputs fits.
constexpr std::size_t kReservedStreamBytes = 4u << 20;
// Finished recordings the host has not taken yet; the oldest go past this.
constexpr std::size_t kMostWaiting = 8;
// The steps each speed adds to the router's one per step: paused (-1: none),
// normal, twice and four times as fast.
constexpr int kExtraSteps[4] = {-1, 0, 1, 3};

// [replays] record (the host, before original main).
bool sRecordEnabled = true;
// The match being recorded, and the machine of the packet being written.
StreamWriter sWriter;
int sWriteMachine = 0;
bool sDifficultyPending = false;
// The match recorded is a network match: the rollback hands its steps over.
bool sNetwork = false;
std::vector<Bytes, MallocAllocator<Bytes>> sFinished;

// The replay to play: its stream, read in place.
Bytes sReplay;
StreamReader sReader;
bool sPlaying = false;   // a replay was handed over
bool sStartTaken = false; // the title took it
bool sStarted = false;   // its match began loading
bool sFinishedPlaying = false;
int sReadMachine = 0;
std::atomic<int> sSpeed{0}; // -1 paused, 0 normal, 1 twice, 2 four times as fast
ChargedReplayStatus sStatus{};
// A replay set the match's simulation profile and the set's tweaks: put the
// module's own back at its end.
bool sReplayRules = false;

GameInfoManager* Info()
{
    return GameInfoManager::Instance();
}

void Log(const char* text)
{
    std::fprintf(stderr, "Replay: %s\n", text);
}

void SetError(const std::string& error)
{
    std::snprintf(sStatus.error, sizeof sStatus.error, "%s", error.c_str());
}

// The replay ends: at its end, or once its last step played.
void FinishPlaying()
{
    if (sFinishedPlaying) return;
    sFinishedPlaying = true;
    char line[200];
    std::snprintf(line, sizeof line, "played %u steps, %u of %u checksums matched%s%s", sReader.Step(),
                  sStatus.checked - sStatus.differences, sStatus.checked, sStatus.error[0] ? ": " : "",
                  sStatus.error);
    Log(line);
}

// The match recorded ends: its stream waits for the host.
void FinishRecording()
{
    sNetwork = false;
    if (!sWriter.Active() && !sWriter.Broken())
    {
        sWriter.Clear();
        return;
    }
    char line[120];
    if (sWriter.Steps() < kShortestSteps)
    {
        std::snprintf(line, sizeof line, "the match was shorter than a second (%u steps), not kept", sWriter.Steps());
        Log(line);
        sWriter.Clear();
        return;
    }
    const bool broken = sWriter.Broken();
    Bytes stream = sWriter.Finish();
    Setup setup;
    std::string error;
    ReadSetup(stream.data(), stream.size(), setup, error);
    std::snprintf(line, sizeof line, "recorded %u steps%s", setup.steps,
                  broken ? " (the recording stopped early: a step came out of order)" : "");
    Log(line);
    if (sFinished.size() >= kMostWaiting) sFinished.erase(sFinished.begin());
    sFinished.push_back(std::move(stream));
}

// The rules the match plays by, as the game holds them now: the gameplay
// settings and cheats, the mode, the tweaks as they act (online, the players'
// own are off: kOffOnline), a set's tweaks and the simulation profile.
void RefreshRules(Setup& setup)
{
    GameInfoManager* info = Info();
    const GameplaySettings& settings = info->mCurGameGameplayOptions;
    setup.settings[0] = settings.SkillLevel;
    setup.settings[1] = settings.GameLimitType;
    setup.settings[2] = settings.GameTime;
    setup.settings[3] = settings.GoalLimit;
    setup.settings[4] = settings.NumGames;
    const bool flags[7] = {settings.mHomePowerupsEnabled,  settings.mAwayPowerupsEnabled,
                           settings.mHomeMegastrikeEnabled, settings.mAwayMegastrikeEnabled,
                           settings.mHomeSkillshotEnabled,  settings.mAwaySkillshotEnabled,
                           settings.m_unk1A};
    for (int n = 0; n < 7; ++n) setup.settings_flags[n] = flags[n] ? 1 : 0;
    const CheatSettings* cheats = info->GetActiveRules();
    setup.cheats[0] = cheats->mCustomPowerups;
    setup.cheats[1] = cheats->mEnvironmentCheat;
    setup.cheats[2] = cheats->mPlayerCheat;
    setup.mode = info->mCurrentMode;
    setup.training = g_bTrainingMatch ? 1 : 0;
    setup.tweaks = mscharged::platform::EnabledTweaks();
    setup.online = info->IsOnline() ? 1 : 0;
    if (setup.online) setup.tweaks = (setup.tweaks & ~kOffOnline) | kOnOnline;
    setup.set_tweaks = mscharged_competitive_match_tweaks();
    setup.sim_profile = mscharged_sim_profile();
}

// The match's setup as the game holds it once the loading chose it.
bool CaptureSetup(int localMachine, int machineCount, unsigned seed, const RecordedGameConfig& game, Setup& setup)
{
    setup.stadium = game.mStadium;
    setup.home_team = game.mHomeTeam;
    setup.away_team = game.mAwayTeam;
    for (int slot = 0; slot < 3; ++slot)
    {
        setup.home_sidekicks[slot] = game.mHomeSidekicks[slot];
        setup.away_sidekicks[slot] = game.mAwaySidekicks[slot];
    }
    setup.skill = game.mSkillLevel;
    setup.win_by = game.mWinBy;
    setup.game_time = game.mGameTime;
    setup.game_goals = game.mGameGoals;
    setup.best_series = game.mBestSeries;
    for (int pad = 0; pad < 16; ++pad) setup.playing_sides[pad] = game.mPlayingSides[pad];

    RefreshRules(setup);
    const auto training = mscharged::platform::GetTrainingTweakState();
    setup.training_item = training.item;
    setup.training_item_count = training.item_count;
    setup.training_item_giant = training.item_giant;
    setup.training_away_fielders = training.away_fielders;
    setup.training_away_goalie = training.away_goalie;
    setup.training_away_skill = training.away_skill;
    setup.training_play_away = training.play_away;
    setup.training_skip_goal = training.skip_goal;
    setup.seed = seed;
    setup.local_machine = localMachine;
    setup.machine_count = machineCount;
    for (int machine = 0; machine < machineCount && machine < kMaxMachines; ++machine)
    {
        const auto* peer = g_pNetworkSessionBase ? g_pNetworkSessionBase->GetPeer((s8)machine) : nullptr;
        if (!peer) return false;
        setup.player_counts[machine] = int(peer->mPlayerCount);
    }
    return true;
}
} // namespace

namespace mscharged::replay
{
bool RecordingNetworkMatch()
{
    return sNetwork && sWriter.Active();
}

void RecordNetworkStep(std::uint32_t before_frame, std::uint32_t checksum, std::uint32_t seed, int machine_count,
                       const NetworkStepMachine* machines)
{
    if (!RecordingNetworkMatch()) return;
    for (int machine = 0; machine < machine_count; ++machine)
    {
        const NetworkStepMachine& step = machines[machine];
        sWriter.Header(machine, step.remap, checksum, before_frame, seed, unsigned(step.events), 0);
        for (int event = 0; event < step.events; ++event)
        {
            // DetermDataEvent: its size, three bytes of padding, its data.
            const std::uint8_t* raw = step.event_data[event];
            sWriter.Event(raw + 4, raw[0] <= kNetworkEventSize - 4 ? raw[0] : kNetworkEventSize - 4);
        }
        for (int player = 0; player < step.players; ++player)
            sWriter.Record(machine, player, step.records[player], step.states[player]);
    }
    // The score as the match goes on (the teams are gone when it ends).
    if (g_pTeams[0] && g_pTeams[1])
    {
        sWriter.EditSetup().score[0] = g_pTeams[0]->GetScore();
        sWriter.EditSetup().score[1] = g_pTeams[1]->GetScore();
    }
}
} // namespace mscharged::replay

extern "C" {
bool mscharged_tweak_replay_record_enabled(void)
{
    return sRecordEnabled && !sPlaying;
}

bool mscharged_tweak_replay_begin(int localMachine, int machineCount, unsigned int seed, const void* config,
                                  int configSize)
{
    FinishRecording();
    if (!sRecordEnabled || sPlaying || !config || configSize < int(sizeof(RecordedGameConfig))) return false;
    GameInfoManager* info = Info();
    char line[120];
    // Offline matches the replay can start again: Domination (Training's
    // too) and the Cup's, which plays back as Domination with the recorded
    // setup and difficulty. Network matches with the rollback, which hands
    // over their steps as all their inputs arrive. The demo, the challenges
    // and Strikers 101 have state of their own.
    const bool network = machineCount > 1 || info->IsOnline();
    const bool offlineMode
        = info->mCurrentMode == GameInfoManager::GM_FRIENDLY || info->mCurrentMode == GameInfoManager::GM_CUP;
    if (network ? !mscharged_rollback_predicting() || machineCount < 2
                : machineCount != 1 || info->mIsInStrikers101Mode || !offlineMode)
    {
        std::snprintf(line, sizeof line, "this match is not recorded (mode %d, %d machines%s)", info->mCurrentMode,
                      machineCount, info->IsOnline() ? ", online" : info->mIsInStrikers101Mode ? ", Strikers 101" : "");
        Log(line);
        return false;
    }
    Setup setup;
    if (!CaptureSetup(localMachine, machineCount, seed, *static_cast<const RecordedGameConfig*>(config), setup)
        || !sWriter.Begin(setup))
    {
        Log("this match is not recorded (its machines or players are out of range)");
        return false;
    }
    sWriter.Reserve(kReservedStreamBytes);
    std::snprintf(line, sizeof line, "recording the match (stadium %d, captains %d and %d%s)", setup.stadium,
                  setup.home_team, setup.away_team,
                  setup.training ? ", Training" : network ? ", every machine's inputs as they arrive" : "");
    Log(line);
    sWriteMachine = 0;
    sDifficultyPending = true;
    sNetwork = network;
    // The rollback hands the steps over: the router's own writes of the
    // original netcode would come in between them.
    return !network;
}

void mscharged_tweak_replay_packet(int machine, unsigned short remap, unsigned int checksum, unsigned int frame,
                                   unsigned int seed, unsigned int events, unsigned int dataSize)
{
    if (!sWriter.Active()) return;
    sWriteMachine = machine;
    sWriter.Header(machine, remap, checksum, frame, seed, events, dataSize);
    // The score as the match goes on (the teams are gone when it ends).
    if (g_pTeams[0] && g_pTeams[1])
    {
        sWriter.EditSetup().score[0] = g_pTeams[0]->GetScore();
        sWriter.EditSetup().score[1] = g_pTeams[1]->GetScore();
    }
}

void mscharged_tweak_replay_event(const void* data, int size)
{
    if (size >= 0) sWriter.Event(static_cast<const std::uint8_t*>(data), unsigned(size));
}

void mscharged_tweak_replay_data(const void* data, int size)
{
    if (size > 0) sWriter.Data(static_cast<const std::uint8_t*>(data), unsigned(size));
}

void mscharged_tweak_replay_input(int player, const void* record, unsigned char connected)
{
    sWriter.Record(sWriteMachine, player, static_cast<const std::uint8_t*>(record), connected);
}

void mscharged_tweak_replay_end(void)
{
    FinishRecording();
    if (sStarted && !sFinishedPlaying)
    {
        sFinishedPlaying = true;
        Log("the match left before the replay's end");
    }
    if (sReplayRules && sFinishedPlaying)
    {
        mscharged_sim_reset_profile();
        mscharged_competitive_replay(false, 0);
        sReplayRules = false;
    }
}

bool mscharged_tweak_replay_playing(void)
{
    return sPlaying && sStarted && !sFinishedPlaying;
}

bool mscharged_tweak_replay_read_start(unsigned int* seed, int* localMachine, int* machineCount, int* playerCounts,
                                       void* config, int configSize)
{
    if (!mscharged_tweak_replay_playing() || configSize < int(sizeof(RecordedGameConfig))) return false;
    const Setup& setup = sReader.GetSetup();
    RecordedGameConfig game;
    game.mStadium = setup.stadium;
    game.mHomeTeam = setup.home_team;
    game.mAwayTeam = setup.away_team;
    for (int slot = 0; slot < 3; ++slot)
    {
        game.mHomeSidekicks[slot] = setup.home_sidekicks[slot];
        game.mAwaySidekicks[slot] = setup.away_sidekicks[slot];
    }
    game.mSkillLevel = GameplaySettings::eSkillLevel(setup.skill);
    game.mWinBy = setup.win_by;
    game.mGameTime = setup.game_time;
    game.mGameGoals = setup.game_goals;
    game.mBestSeries = setup.best_series;
    for (int pad = 0; pad < 16; ++pad) game.mPlayingSides[pad] = setup.playing_sides[pad];
    std::memcpy(config, &game, sizeof game);
    *seed = setup.seed;
    *localMachine = setup.local_machine;
    *machineCount = setup.machine_count;
    for (int machine = 0; machine < kMaxMachines; ++machine) playerCounts[machine] = setup.player_counts[machine];
    return true;
}

bool mscharged_tweak_replay_read_packet(int machine, unsigned int checksum, unsigned int frame, unsigned int seed,
                                        unsigned short* remap, unsigned int* events, unsigned int* dataSize)
{
    if (!mscharged_tweak_replay_playing()) return false;
    PacketHeader header;
    if (!sReader.Header(machine, header))
    {
        if (!sReader.Error().empty()) SetError(sReader.Error());
        FinishPlaying();
        return false;
    }
    sReadMachine = machine;
    sStatus.steps = sReader.Step();
    if (header.checkpoint || header.frame != frame)
    {
        ++sStatus.checked;
        if (header.frame != frame || header.checksum != checksum || header.seed != seed)
        {
            if (!sStatus.differences)
            {
                sStatus.first_difference = sReader.Step();
                char line[200];
                std::snprintf(line, sizeof line,
                              "step %u differs from the recording (frame %u/%u, checksum %08x/%08x, seed %08x/%08x)",
                              sReader.Step(), header.frame, frame, header.checksum, checksum, header.seed, seed);
                Log(line);
            }
            ++sStatus.differences;
        }
    }
    *remap = header.remap;
    *events = header.events;
    *dataSize = header.data_size;
    return true;
}

int mscharged_tweak_replay_extra_updates(void)
{
    // The speed the host asked for, from this step on.
    const int speed = sSpeed.load(std::memory_order_relaxed);
    return kExtraSteps[(speed < -1 ? -1 : speed > 2 ? 2 : speed) + 1];
}

bool mscharged_tweak_replay_read_event(unsigned char* size, void* data)
{
    unsigned length = 0;
    if (!sReader.Event(static_cast<std::uint8_t*>(data), length)) return false;
    *size = (unsigned char)length;
    return true;
}

bool mscharged_tweak_replay_read_data(int size, void* data)
{
    return size >= 0 && sReader.Data(static_cast<std::uint8_t*>(data), unsigned(size));
}

bool mscharged_tweak_replay_read_input(int player, void* record, unsigned char* connected)
{
    return sReader.Record(sReadMachine, player, static_cast<std::uint8_t*>(record), *connected);
}

bool mscharged_tweak_take_replay_start(void)
{
    if (!sPlaying || sStartTaken) return false;
    sStartTaken = true;
    return true;
}

void mscharged_tweak_replay_start_match(void)
{
    const Setup& setup = sReader.GetSetup();
    GameInfoManager* info = Info();
    // A replay neither reads nor writes the save: the match's settings come
    // from the recording.
    SaveEnabled = false;
    GameplaySettings settings;
    settings.SkillLevel = GameplaySettings::eSkillLevel(setup.settings[0]);
    settings.GameLimitType = eGameLimitType(setup.settings[1]);
    settings.GameTime = setup.settings[2];
    settings.GoalLimit = setup.settings[3];
    settings.NumGames = setup.settings[4];
    settings.mHomePowerupsEnabled = setup.settings_flags[0] != 0;
    settings.mAwayPowerupsEnabled = setup.settings_flags[1] != 0;
    settings.mHomeMegastrikeEnabled = setup.settings_flags[2] != 0;
    settings.mAwayMegastrikeEnabled = setup.settings_flags[3] != 0;
    settings.mHomeSkillshotEnabled = setup.settings_flags[4] != 0;
    settings.mAwaySkillshotEnabled = setup.settings_flags[5] != 0;
    settings.m_unk1A = setup.settings_flags[6] != 0;
    info->mUserInfo.mGameplayOptions = settings;
    info->mCurGameGameplayOptions = settings;
    info->mUserInfo.mCheatOptions.mCustomPowerups = setup.cheats[0];
    info->mUserInfo.mCheatOptions.mEnvironmentCheat = setup.cheats[1];
    info->mUserInfo.mCheatOptions.mPlayerCheat = setup.cheats[2];
    info->mUserInfo.mGameplayOptions.OnSettingsUpdated();
    info->mUserInfo.mCheatOptions.OnSettingsUpdated();
    // Every match a replay holds starts as Domination does: the Cup's plays
    // back as Domination, a network match as one on this machine alone, its
    // other machines' inputs coming from the recording too.
    info->SetMode(GameInfoManager::GM_FRIENDLY, false);
    g_bTrainingMatch = setup.training != 0;
    mscharged::platform::SetEnabledTweaks((setup.tweaks & ~kViewerTweaks)
                                          | (mscharged::platform::EnabledTweaks() & kViewerTweaks));
    // A network match ran by its simulation profile, and a set's by the
    // set's tweaks: the replay does too, until it ends.
    if (setup.sim_profile) mscharged_sim_set_profile(setup.sim_profile);
    mscharged_competitive_replay(true, setup.set_tweaks);
    sReplayRules = true;
    mscharged::platform::TrainingTweakState training;
    training.item = setup.training_item;
    training.item_count = setup.training_item_count;
    training.item_giant = setup.training_item_giant != 0;
    training.away_fielders = setup.training_away_fielders;
    training.away_goalie = setup.training_away_goalie != 0;
    training.away_skill = setup.training_away_skill;
    training.play_away = setup.training_play_away != 0;
    training.skip_goal = setup.training_skip_goal != 0;
    mscharged::platform::SetTrainingTweakState(training);
    info->SetStadium(setup.stadium);
    info->SetTeam(0, setup.home_team);
    info->SetTeam(1, setup.away_team);
    for (int slot = 0; slot < 3; ++slot)
    {
        info->SetSidekick(0, setup.home_sidekicks[slot], slot);
        info->SetSidekick(1, setup.away_sidekicks[slot], slot);
    }
    info->ResetPlayingSides();
    for (int pad = 0; pad < 16; ++pad) info->SetPlayingSide((unsigned short)pad, setup.playing_sides[pad]);
    gNetworkInputRecording->mPlaybackEnabled = true;
    sStarted = true;
    sStatus.started = 1;
    Log("starting the recorded match");
    GameSceneManager::Instance()->PushLoadingScene(true);
}

void mscharged_tweak_replay_difficulty(void)
{
    GameInfoManager* info = Info();
    if (sWriter.Active() && sDifficultyPending)
    {
        Setup& setup = sWriter.EditSetup();
        setup.difficulty[0] = info->mCurrentDifficulty[0];
        setup.difficulty[1] = info->mCurrentDifficulty[1];
        // A network match's rules come with its loading (a set's), after the
        // game handed the recorder its configuration: the settings and
        // cheats it plays by, as they are now, and the configuration's own
        // length, limits and series, which a replay's loading applies.
        if (sNetwork)
        {
            RefreshRules(setup);
            const GameplaySettings& settings = info->mCurGameGameplayOptions;
            setup.skill = settings.SkillLevel;
            setup.win_by = settings.GameLimitType;
            setup.game_time = settings.GameTime;
            setup.game_goals = settings.GoalLimit;
            setup.best_series = settings.NumGames;
        }
        sDifficultyPending = false;
    }
    if (mscharged_tweak_replay_playing())
    {
        info->mCurrentDifficulty[0] = sReader.GetSetup().difficulty[0];
        info->mCurrentDifficulty[1] = sReader.GetSetup().difficulty[1];
    }
}

// Host entry (before original main): whether matches are recorded.
__attribute__((visibility("default"))) void charged_configure_replays(int record)
{
    sRecordEnabled = record != 0;
}

// Host entry (before original main): the replay to play. Nonzero when it can.
__attribute__((visibility("default"))) int charged_configure_replay(const void* stream, unsigned size)
{
    sReplay.assign(static_cast<const std::uint8_t*>(stream), static_cast<const std::uint8_t*>(stream) + size);
    std::string error;
    if (!sReader.Open(sReplay.data(), sReplay.size(), error))
    {
        SetError(error);
        sPlaying = false;
        return 0;
    }
    sPlaying = true;
    sStartTaken = sStarted = sFinishedPlaying = false;
    sStatus = ChargedReplayStatus{};
    sStatus.playing = 1;
    sStatus.total = sReader.GetSetup().steps;
    return 1;
}

__attribute__((visibility("default"))) unsigned charged_take_replay(void* out, unsigned capacity)
{
    if (sFinished.empty()) return 0;
    const Bytes& oldest = sFinished.front();
    const unsigned size = unsigned(oldest.size());
    if (out && capacity >= size)
    {
        std::memcpy(out, oldest.data(), size);
        sFinished.erase(sFinished.begin());
    }
    return size;
}

__attribute__((visibility("default"))) void charged_finish_replay(void)
{
    FinishRecording();
}

__attribute__((visibility("default"))) int charged_replay_status(ChargedReplayStatus* status, unsigned size)
{
    if (!status || size < sizeof(ChargedReplayStatus)) return 0;
    // Its last step has played (between frames, so whole): a network match
    // ends there, as its game then waits for the other machine's screens
    // after the match, which a replay does not have, and asks for no more.
    if (mscharged_tweak_replay_playing() && sStatus.total && sReader.Step() >= sStatus.total) FinishPlaying();
    sStatus.finished = sFinishedPlaying ? 1 : 0;
    sStatus.speed = sSpeed.load(std::memory_order_relaxed);
    *status = sStatus;
    return 1;
}

__attribute__((visibility("default"))) void charged_set_replay_speed(int speed)
{
    sSpeed.store(speed < -1 ? -1 : speed > 2 ? 2 : speed, std::memory_order_relaxed);
}
} // extern "C"

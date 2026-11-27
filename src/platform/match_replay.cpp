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
#include "platform/game_allocation_ownership.h"
#include "platform/game_tweak_state.h"
#include "platform/live_ring.h"
#include "platform/match_replay_network.h"
#include "platform/match_replay_status.h"
#include "platform/match_replay_stream.h"
#include "platform/match_stats.h"
#include "platform/online_match_snapshot.h"

#include "Game/DB/SaveLoad.h"
#include "Game/DB/UserOptions.h"
#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkGameConfig.h"
#include "Game/NetworkInputRecording.h"
#include "Game/NetworkSession.h"
#include "Game/NisPlayer.h"
#include "Game/Render/Nis.h"
#include "Game/Team.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

extern "C" bool mscharged_rollback_predicting(void);
extern "C" int mscharged_rollback_unconfirmed_steps(void);
// The set's match and its tweaks (competitive_match.cpp).
extern "C" unsigned int mscharged_competitive_match_tweaks(void);
extern "C" void mscharged_competitive_replay(bool replaying, unsigned int tweaks);
extern "C" bool mscharged_competitive_game_start(int* scores, float* seconds);
extern "C" void mscharged_competitive_game_resume(const int* scores, float seconds);
extern "C" bool mscharged_competitive_set_game(void);

namespace
{
using namespace mscharged::replay;

// The tweaks that only change how the menus and HUD look or how fast the
// title is passed: a replay keeps the viewer's own instead of the recorded.
constexpr unsigned kViewerTweaks = (1u << MSCHARGED_TWEAK_SKIP_INTRO) | (1u << MSCHARGED_TWEAK_FAST_MENUS)
    | (1u << MSCHARGED_TWEAK_NO_HUD) | (1u << MSCHARGED_TWEAK_OVERRIDE_RETAIL_MENUS);
// Online, the players' own gameplay tweaks are off (their hooks test
// IsOnline), but Feder's fair ball contests, which every online match plays
// by except a set's, which has them by its rules (tweak patch 0083). A
// recording of an online match keeps the tweaks as they acted, so its
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
// A replay whose match takes no step for this many of the host's frames
// (about 20 s) while it has steps left has stopped: its match ended before
// the recording did (it played differently), or waits for screens a replay
// does not have.
constexpr int kStalledFrames = 1200;
// A replay waits at most this many steps' time (10 s) for what this computer
// still loads (StillLoading) before it goes on anyway, then at its own pace,
// one step a tick, never faster, until the load is done.
constexpr int kMostLoadWait = 500;
// The steps each speed adds to the router's one per step: paused (-1: none),
// normal, twice and four times as fast.
constexpr int kExtraSteps[4] = {-1, 0, 1, 3};
// The live stream's ring (live_ring.h): minutes of a match even when the host
// takes nothing for a while.
constexpr std::size_t kLiveRingBytes = 1u << 18;
// A live replay plays a moment behind the stream: the stream comes in bursts
// (the players' games send every 100 ms), so after it ran out it waits until
// kLiveBufferSteps have arrived (it never reads a step that has not arrived
// whole); with more ahead it plays faster until it has caught up: twice as
// fast from kLiveFastFrom on, and from kTurboFrom on (a spectator who came
// late, or a stream that stood still) as fast as this computer can (turbo),
// until it is less than kTurboUntil behind.
constexpr int kLiveBufferSteps = 15; // 0.3 s
constexpr int kLiveFastFrom = 25;    // 0.5 s behind: twice as fast
constexpr int kTurboFrom = 250;      // 5 s: turbo
constexpr int kTurboUntil = 50;      // 1 s: twice as fast again
// Turbo: a fixed update's tick takes steps until they have taken their share
// of the time, kTurboShare, measured as they run: three times what the rest
// (the picture, the program) took since the last step, so a slower computer
// takes fewer and still draws, and a tick that follows another at once (the
// update catching up with its clock after a slow frame) takes one step, as a
// match does; at most kTurboMostBurst in one tick. It leaves kLiveFastFrom
// steps ahead.
constexpr double kTurboShare = 0.75;
constexpr double kTurboMostBurst = 0.1; // seconds
constexpr int kTurboMostSteps = 4000;   // in one tick (80 s of a match)
// Meanwhile the parts of cutscenes a player may skip are skipped, and the
// wipes before and after them (the match stands still) run this many times
// faster.
constexpr float kTurboWipeSpeed = 8.0f;
// The effects (particles) age with the match's time in a replay faster than
// it, at most this much in a frame (in updates of 0.1 s).
constexpr float kMostEffectsTime = 10.0f; // seconds
// The whole steps of a live stream are counted as they arrive, at most this
// many at a look (a spectator who came late has them counted in a few).
constexpr int kMostCounted = 8192;
using SteadyClock = std::chrono::steady_clock;

// [replays] record (the host, before original main).
bool sRecordEnabled = true;
// Online matches are recorded for the server whatever this PC keeps
// (charged_configure_online_replays).
bool sRecordOnline = false;
// The match being recorded, and the machine of the packet being written.
StreamWriter sWriter;
int sWriteMachine = 0;
bool sDifficultyPending = false;
// The match recorded is a network match: the rollback hands its steps over.
bool sNetwork = false;
std::vector<Bytes, MallocAllocator<Bytes>> sFinished;

// The live stream of an online match being recorded ([replays] record, while
// online.server is set): each step goes to the ring as it is written, for the
// host's sender. Nothing waits: a step that does not fit stops the live stream
// of that match (sLiveBroken), never the match or its recording.
bool sLiveEnabled = false;
LiveRing<kLiveRingBytes> sLive;
std::uint32_t sLiveStream = 0;      // the network matches recorded so far
bool sLiveBegun = false;            // the match's kLiveBegin is in the ring
std::size_t sLiveSent = 0;          // its steps' bytes in the ring
std::atomic<std::uint32_t> sLiveBroken{0}; // the stream that stopped (its number), 0 none
// The host's numbers of the module's online series (charged_name_online_series):
// a recording names its series as the reports do.
struct SeriesName
{
    unsigned module_series = 0;
    std::uint64_t number = 0;
};
SeriesName sSeriesNames[4];
unsigned sSeriesOfRecording = 0; // the module's series of the match recorded, 0 not known

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
// A live replay: its stream arrives while it plays (the host's reader thread
// writes past sLiveSize, then publishes it), in a buffer that never moves.
bool sLiveReplay = false;
std::uint8_t* sLiveBuffer = nullptr;
std::size_t sLiveCapacity = 0;
std::atomic<std::size_t> sLiveSize{0};
std::atomic<bool> sLiveComplete{false};
bool sLiveBuffering = true; // waiting for kLiveBufferSteps (at its start, after it ran out)
// The steps the replay had read at the host's last look, and its looks since.
std::uint32_t sProgressStep = 0;
int sStalledFrames = 0;
// The steps the replay has waited for this computer's loading (StillLoading).
int sLoadWait = 0;
// Live: a reader of its own at the end of the whole steps that have arrived
// (its Step() counts them), and whether the replay catches up in turbo.
StreamReader sArrived;
bool sTurbo = false;
// Turbo: the time its steps may still take (seconds, below 0 after a long
// step), when its last step ended (or its tick began), and whether a tick of
// it runs; its start and how far the replay had read then.
double sTurboTime = 0.0;
SteadyClock::time_point sStepEnded{};
bool sTurboTick = false;
SteadyClock::time_point sTurboBegan{};
std::uint32_t sTurboFromStep = 0;
// For its log: the time its steps took, how many, and the ticks it waited
// for this computer's loading.
double sTurboStepSeconds = 0.0;
double sTurboStillSeconds = 0.0; // between its ticks for longer than a frame: cutscenes, loading
std::uint32_t sTurboSteps = 0;
std::uint32_t sTurboHeldTicks = 0;
// The effects (tweak patch 0089): how far the replay had read at their last
// update, and how many times faster than its match it plays now (0 while it
// catches up: their time is then its steps').
std::uint32_t sEffectsStep = 0;
float sEffectsSpeed = 1.0f;

// Cutscenes (tweak patch 0072): the part of one a player may skip now (-1
// none), how long it has played, whether the test or the replay pressed in it,
// and whether its press was noted.
int sPart = -1;
float sPartTime = 0.0f;
bool sPartPressed = false;
bool sPartNoted = false;
// A replay: the parts of cutscenes its players skipped, by number, each as
// when (hundredths of a second into it) plus one; 0 not skipped. The live
// replay's reader thread adds those the server passes on.
constexpr int kMostParts = 512;
std::atomic<std::uint32_t> sSkipAt[kMostParts];

GameInfoManager* Info()
{
    return GameInfoManager::Instance();
}

// What this computer still loads that the recorded match had loaded by then:
// a disc read into the game's memory, or a cutscene's animations. A network
// match's replay starts a cutscene once the recorded machines said theirs was
// loaded (Presentation::PlayNis), with whatever has arrived here: running
// faster than the disc reads, it started one before its animations had, and
// crashed on them (cPN_SAnimController without its cSAnim).
bool StillLoading()
{
    if (mscharged::platform::CurrentGameByteProducers().pending) return true;
    if (!g_pGame) return false;
    for (Nis* nis : NisPlayer::Instance()->mLoaded)
        if (nis && nis->mScriptStarted && nis->IsLoading()) return true;
    return false;
}

// RECHARGED_TEST_CUTSCENE_SKIP: the seconds after which the local player
// skips every part of a cutscene, as if pressed (tests of the players' side);
// below 0 off.
float TestSkipAfter()
{
    static const float after = [] {
        const char* value = std::getenv("RECHARGED_TEST_CUTSCENE_SKIP");
        if (!value || !*value) return -1.0f;
        char* end = nullptr;
        const float seconds = std::strtof(value, &end);
        return end != value && seconds >= 0.0f && seconds < 600.0f ? seconds : -1.0f;
    }();
    return after;
}

void ClearSkips()
{
    for (auto& at : sSkipAt) at.store(0, std::memory_order_relaxed);
}

// A part a replay's players skipped `after` hundredths of a second into it;
// the first word on it stays.
void NoteSkip(unsigned part, unsigned after)
{
    if (part >= unsigned(kMostParts)) return;
    std::uint32_t none = 0;
    sSkipAt[part].compare_exchange_strong(none, std::min(after, 65535u) + 1, std::memory_order_relaxed);
}

void Log(const char* text)
{
    std::fprintf(stderr, "Replay: %s\n", text);
}

void SetError(const std::string& error)
{
    std::snprintf(sStatus.error, sizeof sStatus.error, "%s", error.c_str());
}

std::uint64_t SeriesNumber(unsigned moduleSeries)
{
    for (const SeriesName& name : sSeriesNames)
        if (moduleSeries && name.module_series == moduleSeries) return name.number;
    return 0;
}

// Bytes of the match's stream into the ring, in messages of at most
// kLiveMostData; false when one does not fit.
bool LiveData(const std::uint8_t* data, std::size_t size)
{
    for (std::size_t at = 0; at < size;)
    {
        const std::size_t part = size - at < kLiveMostData ? size - at : kLiveMostData;
        if (!sLive.Put(kLiveData, sLiveStream, data + at, std::uint32_t(part))) return false;
        at += part;
    }
    return true;
}

void StopLive()
{
    if (sLiveBroken.load(std::memory_order_relaxed) == sLiveStream) return;
    sLiveBroken.store(sLiveStream, std::memory_order_release);
    Log("the live stream of this match stopped: the program did not take it in time");
}

// After a network step (whole in the writer): the steps not in the ring yet.
// The stream begins once the match knows its series and game (online, from
// its kickoff on): its start as it is then (the setup without its steps and
// score yet), then every step so far.
void LiveStep()
{
    if (!sLiveEnabled || !sNetwork || sLiveBroken.load(std::memory_order_relaxed) == sLiveStream) return;
    if (!sLiveBegun)
    {
        LiveBegin begin;
        int game = 0;
        if (!ChargedOnlineRecordingGame(begin.module_series, game, begin.profile_id)) return;
        begin.game = std::uint32_t(game);
        Setup& setup = sWriter.EditSetup();
        sSeriesOfRecording = begin.module_series;
        setup.series_game = game;
        setup.series = std::int64_t(SeriesNumber(begin.module_series));
        Bytes start;
        sWriter.StartBytes(start, true);
        if (!sLive.Put(kLiveBegin, sLiveStream, &begin, sizeof begin) || !LiveData(start.data(), start.size()))
            return StopLive();
        sLiveBegun = true;
        sLiveSent = 0;
    }
    const Bytes& steps = sWriter.StepBytes();
    if (steps.size() <= sLiveSent) return;
    if (!LiveData(steps.data() + sLiveSent, steps.size() - sLiveSent)) return StopLive();
    sLiveSent = steps.size();
}

double Seconds(SteadyClock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

// Live: the steps that have arrived whole since the last look, counted, and
// how many of them are ahead of the game's.
int LiveAhead()
{
    const std::size_t size = sLiveSize.load(std::memory_order_acquire);
    sReader.Grow(size);
    sArrived.Grow(size);
    sArrived.SkipWholeSteps(kMostCounted);
    const std::uint32_t read = sReader.Step();
    const int ahead = sArrived.Step() > read ? int(std::min(sArrived.Step() - read, std::uint32_t(1) << 30)) : 0;
    sStatus.behind = unsigned(ahead);
    return ahead;
}

// Why turbo ends.
enum class TurboEnd
{
    CaughtUp,
    Paused,
    Loading, // a load this computer waited kMostLoadWait for still runs
};

// Turbo begins, or ends.
void SetTurbo(bool on, TurboEnd why = TurboEnd::CaughtUp)
{
    if (on == sTurbo) return;
    sTurbo = on;
    sStatus.turbo = on ? 1 : 0;
    sTurboTime = 0.0;
    sStepEnded = {};
    sTurboTick = false;
    const SteadyClock::time_point now = SteadyClock::now();
    char line[256];
    if (on)
    {
        sTurboBegan = now;
        sTurboFromStep = sReader.Step();
        sTurboStepSeconds = sTurboStillSeconds = 0.0;
        sTurboSteps = sTurboHeldTicks = 0;
        std::snprintf(line, sizeof line, "catching up with the live match as fast as this computer can, %.1f s behind",
                      double(sStatus.behind) / 50.0);
    }
    else if (why == TurboEnd::Paused)
        std::snprintf(line, sizeof line, "paused while catching up, %.1f s behind", double(sStatus.behind) / 50.0);
    else if (why == TurboEnd::Loading)
        std::snprintf(line, sizeof line, "stops catching up until this computer has loaded, %.1f s behind",
                      double(sStatus.behind) / 50.0);
    else
    {
        const double took = Seconds(now - sTurboBegan);
        const double played = double(sReader.Step() - sTurboFromStep) / 50.0;
        std::snprintf(line, sizeof line,
                      "caught up: %.1f s of the match in %.1f s (%.1f times as fast), %.1f s behind; %u fast steps took "
                      "%.1f s (%.2f ms each), %.1f s without steps (cutscenes, %.1f s of it loading)",
                      played, took, took > 0.0 ? played / took : 0.0, double(sStatus.behind) / 50.0, sTurboSteps,
                      sTurboStepSeconds, sTurboSteps ? 1000.0 * sTurboStepSeconds / sTurboSteps : 0.0,
                      sTurboStillSeconds, 0.02 * sTurboHeldTicks);
    }
    Log(line);
}

// A tick of turbo: its share of the time the rest took since the last step,
// and the steps it may take; mscharged_tweak_replay_tick_over ends it once
// they took that share.
int TurboTick(int ahead)
{
    const SteadyClock::time_point now = SteadyClock::now();
    if (sStepEnded != SteadyClock::time_point{})
    {
        const double rest = Seconds(now - sStepEnded);
        sTurboTime = std::min(sTurboTime + kTurboShare / (1.0 - kTurboShare) * rest, kTurboMostBurst);
        if (rest > 0.1) sTurboStillSeconds += rest;
    }
    sStepEnded = now;
    sTurboTick = true;
    return std::clamp(ahead - kLiveFastFrom, 1, kTurboMostSteps);
}

// The replay ends: at its end, or once its last step played.
void FinishPlaying()
{
    if (sFinishedPlaying) return;
    sFinishedPlaying = true;
    sTurbo = sTurboTick = false;
    sStatus.turbo = 0;
    char line[200];
    std::snprintf(line, sizeof line, "played %u steps, %u of %u checksums matched%s%s", sReader.Step(),
                  sStatus.checked - sStatus.differences, sStatus.checked, sStatus.error[0] ? ": " : "",
                  sStatus.error);
    Log(line);
}

// The match recorded ends: its stream waits for the host.
void FinishRecording()
{
    // The match's stats (none when nothing was recorded): the file keeps them.
    MatchStats stats = StatsFinish();
    if (sNetwork && sLiveBegun)
    {
        sWriter.Settle();
        LiveStep();
        if (sLiveBroken.load(std::memory_order_relaxed) != sLiveStream && !sLive.Put(kLiveEnd, sLiveStream, nullptr, 0))
            StopLive();
    }
    if (sNetwork && sSeriesOfRecording && !sWriter.EditSetup().series)
        sWriter.EditSetup().series = std::int64_t(SeriesNumber(sSeriesOfRecording));
    sNetwork = false;
    sLiveBegun = false;
    sSeriesOfRecording = 0;
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
    if (stats.kept) sWriter.EditSetup().stats = stats;
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
// own are off: kOffOnline), a set's tweaks, the simulation profile and where a
// game that goes on from one that broke off starts.
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
    if (setup.online)
        setup.tweaks = mscharged_competitive_set_game() ? setup.tweaks & ~(kOffOnline | kOnOnline)
                                                        : (setup.tweaks & ~kOffOnline) | kOnOnline;
    setup.set_tweaks = mscharged_competitive_match_tweaks();
    setup.sim_profile = mscharged_sim_profile();
    int scores[2] = {0, 0};
    float seconds = 0.0f;
    setup.resumes = mscharged_competitive_game_start(scores, &seconds) ? 1 : 0;
    setup.start_score[0] = scores[0];
    setup.start_score[1] = scores[1];
    setup.start_seconds = seconds;
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
    // The difficulty in effect now: a set's next game starts again in place
    // (NetworkSession::RematchGame), without the loading that chooses one
    // (mscharged_tweak_replay_difficulty, which notes it for a loaded match).
    setup.difficulty[0] = Info()->mCurrentDifficulty[0];
    setup.difficulty[1] = Info()->mCurrentDifficulty[1];
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
    StatsStep();
    sWriter.Settle();
    LiveStep();
}
} // namespace mscharged::replay

extern "C" {
bool mscharged_tweak_replay_record_enabled(void)
{
    return (sRecordEnabled || sRecordOnline) && !sPlaying;
}

bool mscharged_tweak_replay_begin(int localMachine, int machineCount, unsigned int seed, const void* config,
                                  int configSize)
{
    FinishRecording();
    if ((!sRecordEnabled && !sRecordOnline) || sPlaying || !config || configSize < int(sizeof(RecordedGameConfig)))
        return false;
    GameInfoManager* info = Info();
    char line[120];
    // Offline matches the replay can start again: Domination (Training's
    // too) and the Cup's, which plays back as Domination with the recorded
    // setup and difficulty. Network matches with the rollback, which hands
    // over their steps as all their inputs arrive. The demo, the challenges
    // and Strikers 101 have state of their own.
    const bool network = machineCount > 1 || info->IsOnline();
    // An online match goes to the server whether or not this PC keeps replays
    // (the host keeps no file of it then); any other only when it does.
    if (!sRecordEnabled && !(sRecordOnline && info->IsOnline())) return false;
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
    StatsBegin();
    std::snprintf(line, sizeof line, "recording the match (stadium %d, captains %d and %d%s)", setup.stadium,
                  setup.home_team, setup.away_team,
                  setup.training ? ", Training" : network ? ", every machine's inputs as they arrive" : "");
    Log(line);
    sWriteMachine = 0;
    sDifficultyPending = true;
    sNetwork = network;
    if (network)
    {
        ++sLiveStream;
        sLiveBegun = false;
        sLiveSent = 0;
        sSeriesOfRecording = 0;
    }
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
    if (machine == 0) StatsStep();
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
        mscharged_competitive_game_resume(nullptr, 0.0f);
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
    sTurboTick = false;
    sEffectsSpeed = 1.0f;
    // The speed the host asked for, from this step on.
    const int speed = sSpeed.load(std::memory_order_relaxed);
    const int extra = kExtraSteps[(speed < -1 ? -1 : speed > 2 ? 2 : speed) + 1];
    // No step while this computer still loads (at most kMostLoadWait); after
    // that one step a tick until it has: a faster replay would run into what
    // it has not loaded (a cutscene's animations).
    bool loadOverdue = false;
    if (!StillLoading())
    {
        if (sLoadWait >= kMostLoadWait) Log("this computer has loaded what it waited for: the replay's pace is back");
        sLoadWait = 0;
    }
    else if (sLoadWait < kMostLoadWait)
    {
        if (++sLoadWait == kMostLoadWait)
            Log("this computer still loads after 10 s: the replay goes on at its own pace until it has");
        if (sTurbo) ++sTurboHeldTicks;
        return -1;
    }
    else
        loadOverdue = true;
    if (!sLiveReplay)
    {
        const int steps = (loadOverdue ? std::min(extra, 0) : extra) + 1;
        sEffectsSpeed = float(std::max(steps, 1));
        return steps - 1;
    }
    // Live: the steps that have arrived whole set the pace.
    const bool complete = sLiveComplete.load(std::memory_order_acquire);
    const int ahead = LiveAhead();
    if (!ahead)
    {
        SetTurbo(false);
        sLiveBuffering = true;
        return complete ? 0 : -1; // its end (the router's read ends the replay), or waits
    }
    if (sLiveBuffering && ahead < kLiveBufferSteps && !complete) return -1;
    sLiveBuffering = false;
    if (loadOverdue) SetTurbo(false, TurboEnd::Loading);
    else if (speed < 0 || ahead < kTurboUntil) SetTurbo(false, speed < 0 ? TurboEnd::Paused : TurboEnd::CaughtUp);
    else if (ahead > kTurboFrom) SetTurbo(true);
    if (sTurbo)
    {
        sEffectsSpeed = 0.0f;
        return TurboTick(ahead) - 1;
    }
    int steps = extra + 1;
    if (speed >= 0) steps = std::max(steps, ahead > kLiveFastFrom ? 2 : 1);
    if (loadOverdue) steps = std::min(steps, 1);
    steps = std::min(steps, ahead);
    sEffectsSpeed = float(std::max(steps, 1));
    return steps - 1;
}

bool mscharged_tweak_replay_tick_over(void)
{
    if (!mscharged_tweak_replay_playing()) return false;
    // A step began a load (a cutscene's animations, a picture from the disc):
    // the steps after it wait from the next tick on (StillLoading), as they
    // would at a tick's start; a fast replay would play on into it.
    if (sLoadWait < kMostLoadWait && StillLoading())
    {
        sTurboTick = false;
        return true;
    }
    if (!sTurboTick) return false;
    // The step that just ended takes its time from the share.
    const SteadyClock::time_point now = SteadyClock::now();
    const double step = Seconds(now - sStepEnded);
    sTurboTime -= step;
    sTurboStepSeconds += step;
    ++sTurboSteps;
    sStepEnded = now;
    if (sTurboTime > 0.0) return false;
    sTurboTick = false;
    return true;
}

bool mscharged_tweak_replay_turbo(void)
{
    return sTurbo && mscharged_tweak_replay_playing();
}

float mscharged_tweak_replay_wipe_speed(void)
{
    return mscharged_tweak_replay_turbo() ? kTurboWipeSpeed : 1.0f;
}

float mscharged_tweak_replay_effects_time(float dt)
{
    const std::uint32_t step = sReader.Step();
    const std::uint32_t steps = step >= sEffectsStep ? step - sEffectsStep : 0;
    sEffectsStep = step;
    if (!mscharged_tweak_replay_playing()) return dt;
    // Catching up: the time of the steps taken since the last update (a
    // frame's tick takes at most kTurboMostBurst of steps), at most
    // kMostEffectsTime.
    if (sEffectsSpeed <= 0.0f) return std::min(float(steps) / 50.0f, kMostEffectsTime);
    return dt * sEffectsSpeed;
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
    // A game that went on from one that broke off starts where it did (tweak
    // patch 0080); any other from 0-0, whatever game this machine played last.
    const int scores[2] = {setup.start_score[0], setup.start_score[1]};
    mscharged_competitive_game_resume(setup.resumes ? scores : nullptr, setup.start_seconds);
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

// Each presentation update that is not paused: the part of a cutscene a player
// may skip now and how long it has played. A replay presses in a part where
// its players did, once it has played as long.
bool mscharged_tweak_presentation_skip(bool inside, int part, float deltaT)
{
    if (!inside)
    {
        sPart = -1;
        return false;
    }
    if (part != sPart)
    {
        sPart = part;
        sPartTime = 0.0f;
        sPartPressed = sPartNoted = false;
    }
    else if (deltaT > 0.0f && deltaT < 0.5f)
        sPartTime += deltaT;
    if (!sPlaying || part < 0 || part >= kMostParts) return false;
    // Catching up in turbo (or about to, at a cutscene before its steps), a
    // live replay skips every part a player may skip: watching it would only
    // put it further behind.
    if (sLiveReplay && (sTurbo || LiveAhead() > kTurboFrom))
    {
        if (!sPartPressed)
        {
            sPartPressed = true;
            char line[96];
            std::snprintf(line, sizeof line, "skips part %d of a cutscene to catch up", part);
            Log(line);
        }
        return true;
    }
    const std::uint32_t at = sSkipAt[part].load(std::memory_order_relaxed);
    if (!at || sPartTime * 100.0f + 0.5f < float(at - 1)) return false;
    if (!sPartPressed)
    {
        sPartPressed = true;
        char line[96];
        std::snprintf(line, sizeof line, "presses skip in part %d of a cutscene %.2f s in, as its players did", part,
                      double(sPartTime));
        Log(line);
    }
    return true;
}

// Where a press of a pad that may skip would count: the test's press, once a
// part, in a match.
bool mscharged_tweak_presentation_test_press(void)
{
    const float test = TestSkipAfter();
    if (sPlaying || test < 0.0f || sPart < 0 || sPartPressed || sPartTime < test) return false;
    sPartPressed = true;
    return true;
}

// A press counts for `part` (made here or on the other machine): a recorded
// match keeps when, once a part, and its live stream says so at once (beside
// its steps: the match stands still while the cutscene plays, and each
// machine plays it as long as it lasts there).
void mscharged_tweak_presentation_pressed(int part)
{
    if (sPlaying || part < 0 || part > 0xFFFF || part != sPart || sPartNoted) return;
    sPartNoted = true;
    const unsigned after = unsigned(std::min(sPartTime * 100.0f, 65535.0f));
    if (sWriter.Active())
    {
        Setup& setup = sWriter.EditSetup();
        bool known = false;
        for (unsigned n = 0; n < setup.skip_count && !known; ++n) known = setup.skips[n].section == part;
        if (!known && setup.skip_count < kMaxSkips)
            setup.skips[setup.skip_count++] = {std::uint16_t(part), std::uint16_t(after)};
    }
    if (sLiveEnabled && sNetwork && sLiveBegun && sLiveBroken.load(std::memory_order_relaxed) != sLiveStream)
    {
        const std::uint8_t skip[4] = {std::uint8_t(part), std::uint8_t(part >> 8), std::uint8_t(after),
                                      std::uint8_t(after >> 8)};
        // One that does not fit is lost: spectators then catch up instead.
        sLive.Put(kLiveSkip, sLiveStream, skip, sizeof skip);
    }
}

// Host entry (before original main): whether matches are recorded.
__attribute__((visibility("default"))) void charged_configure_replays(int record)
{
    sRecordEnabled = record != 0;
}

// Host entry (before original main): whether online matches are recorded for
// the server, whatever charged_configure_replays says.
__attribute__((visibility("default"))) void charged_configure_online_replays(int record)
{
    sRecordOnline = record != 0;
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
    ClearSkips();
    const Setup& setup = sReader.GetSetup();
    for (unsigned n = 0; n < setup.skip_count; ++n) NoteSkip(setup.skips[n].section, setup.skips[n].after);
    return 1;
}

// Host entry (before original main): whether online matches stream live as
// they are recorded.
__attribute__((visibility("default"))) void charged_configure_live(int enabled)
{
    sLiveEnabled = enabled != 0;
}

// Host entry, on the sender's thread: the live stream's messages that fit in
// `capacity` (live_ring.h), the bytes copied; `stopped` gets the stream whose
// live stream stopped (0 none) and `unconfirmed` the steps the match plays
// ahead of the inputs that have arrived (a connection in trouble: the sender
// waits).
__attribute__((visibility("default"))) unsigned charged_take_live(void* out, unsigned capacity, unsigned* stopped,
                                                                  int* unconfirmed)
{
    if (stopped) *stopped = sLiveBroken.load(std::memory_order_acquire);
    if (unconfirmed) *unconfirmed = mscharged_rollback_unconfirmed_steps();
    if (!out) return 0;
    return unsigned(sLive.Take(static_cast<std::uint8_t*>(out), capacity));
}

// Host entry, after a game frame: the reports' number of the module's online
// series `moduleSeries` (ChargedOnlineMatchSnapshot::series), which the
// recordings of its games name.
__attribute__((visibility("default"))) void charged_name_online_series(unsigned moduleSeries, unsigned long long number)
{
    if (!moduleSeries || !number || SeriesNumber(moduleSeries) == number) return;
    for (int n = int(std::size(sSeriesNames)) - 1; n > 0; --n) sSeriesNames[n] = sSeriesNames[n - 1];
    sSeriesNames[0] = {moduleSeries, number};
}

// Host entry (before original main): a live replay, its stream's first bytes
// (at least its setup) and the most it may grow to.
__attribute__((visibility("default"))) int charged_configure_live_replay(const void* start, unsigned size,
                                                                         unsigned capacity)
{
    if (!start || size > capacity || sLiveBuffer) return 0;
    sLiveBuffer = static_cast<std::uint8_t*>(std::malloc(capacity));
    if (!sLiveBuffer) return 0;
    std::memcpy(sLiveBuffer, start, size);
    sLiveCapacity = capacity;
    sLiveSize.store(size, std::memory_order_release);
    std::string error;
    if (!sReader.Open(sLiveBuffer, size, error))
    {
        SetError(error);
        sPlaying = false;
        return 0;
    }
    sArrived = sReader;
    sLiveReplay = true;
    sPlaying = true;
    sStartTaken = sStarted = sFinishedPlaying = false;
    sStatus = ChargedReplayStatus{};
    sStatus.playing = 1;
    sStatus.live = 1;
    ClearSkips();
    return 1;
}

// Host entry, from the live replay's reader thread: more of its stream.
__attribute__((visibility("default"))) int charged_feed_live_replay(const void* data, unsigned size, int complete)
{
    if (!sLiveBuffer || sLiveComplete.load(std::memory_order_relaxed)) return 0;
    const std::size_t at = sLiveSize.load(std::memory_order_relaxed);
    if (size > sLiveCapacity - at) return 0;
    if (size) std::memcpy(sLiveBuffer + at, data, size);
    sLiveSize.store(at + size, std::memory_order_release);
    if (complete) sLiveComplete.store(true, std::memory_order_release);
    return 1;
}

// Host entry, from the live replay's reader thread: a player skipped part
// `part` of a cutscene `after` hundredths of a second into it (the server's
// word, beside the stream).
__attribute__((visibility("default"))) void charged_feed_live_skip(unsigned part, unsigned after)
{
    if (sLiveReplay) NoteSkip(part, after);
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
    // A live one: how far behind it is now (also while no step runs, in a
    // cutscene or a load), and its end once all its stream has arrived and played.
    if (mscharged_tweak_replay_playing() && sLiveReplay)
    {
        const int ahead = LiveAhead();
        if (sLiveComplete.load(std::memory_order_acquire) && sStarted && sReader.Step() && !ahead) FinishPlaying();
    }
    // One whose match takes no more steps while some are left (not paused,
    // not waiting for a live stream's next ones).
    if (mscharged_tweak_replay_playing() && sReader.Step())
    {
        const bool waiting = sSpeed.load(std::memory_order_relaxed) < 0 || !sReader.StepsAhead(1);
        if (sReader.Step() != sProgressStep || waiting)
        {
            sProgressStep = sReader.Step();
            sStalledFrames = 0;
        }
        else if (++sStalledFrames >= kStalledFrames)
        {
            SetError("its match took no more steps before the replay's end");
            FinishPlaying();
        }
    }
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

// The test builds' autopilot (MSCHARGED_AUTOPILOT; release builds get
// step_pads.cpp instead). C, P and U held together in the game window
// hand the first local player to a bot and back (src/platform/hotkey_filter.cpp).
//
// The bot plays that player through the player's own Remote and Nunchuk:
// tweak patch 0036 calls it after each fixed step's pad read
// (FixedUpdateTask), before the pads take the status, so the step's inputs,
// what the network sends, the rollback and the Mega Strike meter (which reads
// the local pad) all get its presses as a person's. No machine and no ranking
// can tell it from one. It only reads the match; its own random numbers never
// touch the game's seeds.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error The autopilot belongs to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_autopilot.h"
#include "platform/autopilot_pad.h"

#include "Game/AI/AIPad.h"
#include "Game/AI/AvoidableObject.h"
#include "Game/AI/Fielder.h"
#include "Game/AI/FielderActions.h"
#include "Game/AI/Powerups.h"
#include "Game/AI/Scripts/ScriptQuestions.h"
#include "Game/Ball.h"
#include "Game/Field.h"
#include "Game/CharacterTweaks.h"
#include "Game/Game.h"
#include "Game/Goalie.h"
#include "Game/InputFrameProvider.h"
#include "Game/InputManager.h"
#include "Game/Net.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkSession.h"
#include "Game/Player.h"
#include "Game/Render/MegaBallIndicators.h"
#include "Game/Team.h"
#include "NL/plat/PlatPadManager.h"
#include "NL/plat/WiiPad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

extern bool gMegaStrikeUsePassButton;

namespace
{
using mscharged::platform::autopilot::AngleOf;
using mscharged::platform::autopilot::NunchukStick;
using mscharged::platform::autopilot::ShakeAccel;
using mscharged::platform::autopilot::ShakeSteps;
using mscharged::platform::autopilot::Stick;

// The game's pad actions, read through its Nunchuk table (PadActions.cpp).
enum Action
{
    kDeke = 13,  // the D-pad (up): a deke with the ball, a slide tackle without
    kItem = 21,  // PAD_AIM: the team's item
    kSwitch = 26, // PAD_SWITCH: the next player, without the ball
    kPass = 27,  // with the ball; the goalie's lob
    kShoot = 28, // held to wind a shot up, released to shoot; the goalie's
                 // pass and the Mega Strike meter's presses
};

// The goalie's pointer aims this far (in the screen's 640 by 480) off a Mega Strike ball's centre.
constexpr float kSaveAimOffset = 3.0f;
// A captain's Mega Strike windup: B held until the meter takes over (it can take 4 s).
constexpr int kMegaHoldSteps = 200;
constexpr float kHitRange = 1.8f;
constexpr float kSlideRange = 3.2f;
// The Mega Strike meter's second bar falls; its most accurate point
// (GetMegaStrikeAccuracy's middle of the green).
constexpr float kMeterBest = 0.15f;

std::atomic<bool> sOn{false};

struct Vec
{
    float x;
    float y;
};
Vec Pos(const cPlayer* player)
{
    return {player->GetPosition().x, player->GetPosition().y};
}
Vec Pos(const nlVector3& v)
{
    return {v.x, v.y};
}
Vec Vel(const cPlayer* player)
{
    return {player->GetVelocity().x, player->GetVelocity().y};
}
float Distance(Vec a, Vec b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}
u16 Toward(Vec from, Vec to)
{
    return AngleOf(to.x - from.x, to.y - from.y);
}
// How far apart two angles are, up to half a turn.
int AngleGap(u16 a, u16 b)
{
    return std::abs(static_cast<int>(static_cast<s16>(a - b)));
}
bool Ready(cFielder* fielder)
{
    return fielder && !fielder->IsFallenDown() && !fielder->IsStuck() && !fielder->IsShattered();
}
u16 Bit(int action)
{
    return gWiiFreestyleButtonRemap ? static_cast<u16>(gWiiFreestyleButtonRemap[action]) : 0;
}
// The fielder of `team` nearest `point`, other than `other`.
cFielder* Nearest(cTeam* team, Vec point, float* distance, const cFielder* other = nullptr)
{
    cFielder* best = nullptr;
    float bestDistance = 1e9f;
    for (int i = 0; i < 4; ++i)
    {
        cFielder* fielder = team->GetFielder(i);
        if (fielder == other || !Ready(fielder)) continue;
        const float d = Distance(Pos(fielder), point);
        if (d < bestDistance)
        {
            best = fielder;
            bestDistance = d;
        }
    }
    if (distance) *distance = bestDistance;
    return best;
}

// The CPU's judgement at Legend: the rules of its scripts (Art/scripts/fuzzyai,
// DifficultyLevels/Difficulty3.ini, which the menu's Legend loads), built from
// the game's own questions (ScriptQuestions.cpp) that only read the match. Its
// random draws (RandomChance) come from the bot's own numbers instead.
float Less(float a, float b) // FuzzyLessThan
{
    return std::clamp(2.0f * (b - a), 0.0f, 1.0f);
}
float Not(float x)
{
    return 1.0f - x;
}
// Legend's reaction on offence: what SlowThinker and the "delayed" questions leave of a danger.
constexpr float kReaction = 0.76f;

// Close to an opponent, pressed, or with no room (InDanger, its fielder part).
float InDanger(cFielder* f, cTeam* them)
{
    if (Invincible(f) > 0.5f) return 0.0f;
    float danger = std::max({Attacked(f), PressureFromNearbyOpponents(f), Less(0.35f, Not(Open(f))),
                             Less(0.5f, AvoidingPowerups(f))});
    float d = 0.0f;
    if (cFielder* opponent = Nearest(them, Pos(f), &d))
        danger = std::max(danger, Less(0.5f, 0.5f * NearTo(f, opponent) + 0.5f * ClosingTo(f, opponent)));
    if (Goalie* goalie = them->GetGoalie())
        danger = std::max(danger, Less(0.5f, 0.5f * NearTo(f, goalie) + 0.5f * ClosingTo(f, goalie)));
    return danger;
}
float InDangerDelayed(cFielder* f, cTeam* them)
{
    const float low = 0.75f * (1.0f - kReaction);
    return std::clamp((InDanger(f, them) - low) / (1.0f - low), 0.0f, 1.0f);
}
// What makes a captain give up a Mega Strike: their goalie, items coming, an
// opponent near and closing (InDangerForMegastrike without its hit test).
float InDangerForMegastrike(cFielder* f, cTeam* them)
{
    float danger = std::max(Less(0.25f, NearToTheirGoalie(f)), std::min(Less(0.6f, AvoidingPowerups(f)), Not(Invincible(f))));
    if (InvincibleToCharacters(f) > 0.5f) return danger;
    for (int i = 0; i < 4; ++i)
    {
        cFielder* opponent = them->GetFielder(i);
        if (!opponent || Incapacitated(opponent) > 0.5f) continue;
        danger = std::max(danger, std::min(Less(0.3f, NearTo(opponent, f)), Less(0.3f, ClosingTo(opponent, f))));
    }
    return danger;
}
float GoodToShoot(cFielder* f)
{
    return std::max(Less(0.4f, std::min(LikelyToScore(f), PlayerShotDistance(f))),
                    0.6f * RegularShotScore(f) + 0.4f * PlayerShotDistance(f));
}
// A first-time shot off a pass or a loose ball (AttemptOneTimer).
float OneTimer(cFielder* f, cTeam* them, bool captainSafe)
{
    if (std::max({Less(0.5f, Not(InFrontOfTheirNet(f))), Less(0.5f, WideOpen(f)), InOffensiveZone(f) > 0.0f ? 0.0f : 1.0f,
                  SuperPowerActive(f), Invincible(f)})
        > 0.5f)
        return 0.0f;
    Goalie* goalie = them->GetGoalie();
    const float good = GoodToShoot(f), volley = ReceivingVolleyPass(f);
    const float yes = std::max({goalie ? GoalieDown(goalie) : 0.0f, Less(0.55f, good), std::min(volley, NearToTheirGoalie(f))});
    if (yes > 0.5f) return yes;
    if (captainSafe) return 0.0f; // he would rather take it into a Mega Strike
    return std::max(Less(0.5f, good), std::min(good, Less(volley > 0.0f ? 0.0f : 0.3f, InDangerDelayed(f, them))));
}
// How good a pass to `target` is (GoodPassTargetFrom): up the field and open,
// or near their net and able to shoot.
float PassQuality(cFielder* target, cFielder* passer)
{
    if (!Ready(target) || Incapacitated(target) > 0.5f) return 0.0f;
    const float band = std::min(Less(0.0f, Not(NearTo(target, passer))), Less(0.0f, Not(FarTo(target, passer))));
    if (band <= 0.0f) return 0.0f;
    const float mine = GoodToShoot(passer), theirs = GoodToShoot(target);
    const float shoot = 0.5f * Less(0.0f, theirs) + 0.5f * Less(mine, theirs);
    const float score = NearToTheirNet(target) > 0.45f
        ? 0.5f * shoot + 0.4f * Open(target) + 0.1f * Not(LastBallOwner(target))
        : 0.2f * shoot + 0.45f * DownfieldFrom(passer, target) + 0.35f * Open(target);
    return std::min(score, band);
}

// How soon the quickest opponent can be on `player` (a hit lands from 1.5 m),
// in seconds at his top speed; their goalie only near his net.
float Reach(cTeam* them, const cPlayer* player)
{
    float soonest = 1e9f;
    for (int i = 0; i < 4; ++i)
    {
        cFielder* opponent = them->GetFielder(i);
        if (!Ready(opponent) || Incapacitated(opponent) > 0.5f) continue;
        const float speed = std::max(GetTurboSpeed(opponent->GetTweaks()), 1.0f);
        soonest = std::min(soonest, std::max(0.0f, Distance(Pos(opponent), Pos(player)) - 1.5f) / speed);
    }
    if (const Goalie* goalie = them->GetGoalie())
        soonest = std::min(soonest, std::max(0.0f, Distance(Pos(goalie), Pos(player)) - 1.0f) / 6.0f);
    return soonest;
}

// InterpolateRangeClamped: `outA` at `inA` to `outB` at `inB`.
float RangeClamped(float outA, float outB, float inA, float inB, float x)
{
    return outA + (outB - outA) * std::clamp((x - inA) / (inB - inA), 0.0f, 1.0f);
}
bool Teleports(eCharacterClass c)
{
    return c == DAISY || c == WALUIGI || c == DRYBONES;
}
bool Heavy(eCharacterClass c)
{
    return c == BOWSER || c == DONKEYKONG || c == WARIO || c == BOWSERJR || c == PETEY || c == BIRDO || c == HAMMERBROS
        || c == MONTYMOLE;
}
bool RoundsGoalie(eCharacterClass c)
{
    return c == PEACH || c == DIDDYKONG || c == TOAD;
}

// Each character's deke aimed as the scripts aim it (DesireDeke::Update): away
// from the nearest opponent and against his run (Mario, Luigi, Yoshi, Koopa,
// Boo, Shy Guy); at the nearest opponent when far from their goalie and round
// the goalie when close (Peach, Diddy, Toad); the teleports of Daisy, Waluigi
// and Dry Bones along the goal line and past the goalie; the heavy ones into
// the nearest opponent; and none of the first two kinds into a sideline.
u16 DekeDirection(cFielder* me, cTeam* them, Vec goal)
{
    const Vec at = Pos(me);
    float nearestDistance = 1e9f;
    cFielder* nearest = Nearest(them, at, &nearestDistance);
    Goalie* goalie = them->GetGoalie();
    const eCharacterClass c = me->m_DetChar.m_eCharacterClass;
    u16 direction = me->m_DetChar.m_aActualFacingDirection;
    if (Teleports(c))
    {
        if (!goalie) return direction;
        const float reach = me->GetDekeDistance();
        const Vec line{cField::GetGoalLineX(1U) * (goal.x < 0.0f ? -1.0f : 1.0f), at.y};
        const float radius = me->m_pAvoidableObject ? me->m_pAvoidableObject->GetRadius() : 0.0f;
        const float goalieRadius = goalie->m_pAvoidableObject ? goalie->m_pAvoidableObject->GetRadius() : 0.0f;
        const float goalRange = Distance(line, at) - radius;
        float blend = RangeClamped(1.0f, 0.0f, 0.8f * reach, 2.0f * reach, goalRange);
        if (Distance(at, Pos(goalie)) + radius + goalieRadius > reach) blend = 0.0f;
        if (goalRange < reach) blend = 1.0f;
        const u16 goalAngle = Toward(at, line), goalieAngle = Toward(at, Pos(goalie));
        return static_cast<u16>(goalAngle + static_cast<s16>(blend * static_cast<s16>(goalieAngle - goalAngle)));
    }
    if (Heavy(c)) return nearest ? Toward(at, Pos(nearest)) : direction;
    if (RoundsGoalie(c))
    {
        if (nearest && goalie)
        {
            const u16 opponentAngle = Toward(at, Pos(nearest)), goalieAngle = Toward(at, Pos(goalie));
            const float blend = RangeClamped(1.0f, 0.0f, 2.5f, 0.66f, Distance(at, Pos(goalie)));
            direction = static_cast<u16>(goalieAngle + static_cast<s16>(blend * static_cast<s16>(opponentAngle - goalieAngle)));
        }
    }
    else if (nearest || goalie)
    {
        // The goalie stands in for the nearest opponent when he is nearer.
        Vec from = nearest ? Pos(nearest) : at;
        u16 run = nearest ? nearest->m_DetChar.m_aActualMovementDirection : 0;
        if (goalie && (!nearest || Distance(Pos(goalie), at) < nearestDistance))
        {
            from = Pos(goalie);
            run = goalie->m_DetChar.m_aActualFacingDirection;
        }
        const u16 away = Toward(from, at), against = static_cast<u16>(run + 0x8000);
        direction = static_cast<u16>(against + static_cast<s16>(0.5f * static_cast<s16>(away - against)));
    }
    nlVector2 sideline;
    if (CloseToSideline(me->GetPosition(), nullptr, false, &sideline) > 0.2f
        && AngleGap(AngleOf(sideline.x, sideline.y), direction) < 0x2000)
        direction = static_cast<u16>(direction + 0x8000);
    return direction;
}

// One step of the player's controls: the buttons held, where the stick points
// (a field angle) and how far, the step of a Remote shake (-1: none) and,
// while it is aimed, where the Remote points (the screen's 640 by 480).
struct Controls
{
    u16 buttons = 0;
    u16 direction = 0;
    float deflection = 0.0f;
    int shake = -1;
    bool point = false;
    float pointX = 0.0f;
    float pointY = 0.0f;
};

class Bot
{
public:
    Controls Step(cPlayer* me);
    // A new match: plans go, the random numbers stay.
    void Forget()
    {
        const std::uint32_t random = mRandom;
        *this = Bot();
        mRandom = random;
    }

private:
    struct Cooldowns
    {
        int shot = 0, pass = 0, deke = 0, swap = 0, hit = 0, slide = 0;
    };

    float Chance()
    {
        mRandom ^= mRandom << 13;
        mRandom ^= mRandom >> 17;
        mRandom ^= mRandom << 5;
        return static_cast<float>(mRandom >> 8) * (1.0f / 16777216.0f);
    }
    int Between(int low, int high)
    {
        return low + std::min(high - low, static_cast<int>(Chance() * static_cast<float>(high - low + 1)));
    }
    // A press the game takes once: held a few steps, then up a few. A pass,
    // deke or switch keeps the stick where it pointed when the press began.
    bool CanTap() const
    {
        return mTapSteps == 0 && mRestSteps == 0;
    }
    void Tap(int action, int steps, int rest = 3)
    {
        if (!CanTap()) return;
        mTap = Bit(action);
        mTapSteps = steps;
        mTapRest = rest;
        mTapSteers = false;
    }
    void TapToward(int action, int steps, u16 direction)
    {
        if (!CanTap()) return;
        Tap(action, steps);
        mTapSteers = true;
        mTapDirection = direction;
    }
    void StopShot()
    {
        mShot = -1;
        mCool.shot = 25;
    }
    // The windup B holds: a shot, a sidekick's Skillshot or the captain's Mega Strike.
    enum class Windup
    {
        Shot,
        Skillshot,
        MegaStrike,
    };

    void Goalkeep(Controls& c, cPlayer* me, cTeam* team, cTeam* them);
    bool Save(Controls& c);
    bool Shooting(Controls& c, cFielder* me, cTeam* team, cTeam* them, bool hasBall);
    void MegaStrikeMeter(cFielder* me);
    void Attack(Controls& c, cFielder* me, cTeam* team, cTeam* them, Vec goal);
    void Defend(Controls& c, cFielder* me, cTeam* team, cPlayer* carrier);
    void Chase(Controls& c, cFielder* me, cTeam* team, Vec goal);
    void UseItem(cPlayer* me, cTeam* team);
    cFielder* PassTarget(const cPlayer* passer, cTeam* team, cTeam* them, Vec goal);
    u16 Steer(Vec at, Vec goal, cTeam* them);
    u16 Aim(cFielder* me, cTeam* them, Vec goal);
    Controls& Finish(Controls& c);

    std::uint32_t mRandom = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() | 1);
    cPlayer* mLast = nullptr;
    Cooldowns mCool;
    u16 mTap = 0;
    int mTapSteps = 0;
    int mRestSteps = 0;
    int mTapRest = 3;
    bool mTapSteers = false;
    u16 mTapDirection = 0;
    int mShot = -1; // steps B has been held for a shot, -1: no shot
    Windup mWindup = Windup::Shot;
    int mThink = 0; // steps until the carrier thinks again
    u16 mAim = 0;
    int mShake = -1;
    int mItemWait = -1;
    int mGoalieWait = 0;
    int mGoalieHold = 20;
    bool mMeterPressed = false;
    float mMeterPrevious = -10.0f;
    bool mFirstPressed = false;
    bool mMeterFull = false; // the first bar's needle was in the most balls' zone at the last step
    // Steps before a ball of a Mega Strike save is a target again.
    int mSaved[10] = {};
};

Controls Bot::Step(cPlayer* me)
{
    Controls c;
    for (int* cool : {&mCool.shot, &mCool.pass, &mCool.deke, &mCool.swap, &mCool.hit, &mCool.slide})
        if (*cool > 0) --*cool;
    if (me != mLast)
    {
        // Another player to steer (the pad went to the ball): no shot or meter of the last one.
        mShot = -1;
        mMeterPressed = false;
        mMeterPrevious = -10.0f;
        mLast = me;
    }
    cTeam* team = me->m_pTeam;
    cTeam* them = team ? team->GetOtherTeam() : nullptr;
    if (!team || !them || !team->GetOtherNet() || !team->m_pNet) return Finish(c);
    const Vec goal = Pos(team->GetOtherNet()->GetNetLocation());
    cPlayer* owner = g_pBall->GetOwner();

    if (me->m_eClassType != FIELDER)
    {
        Goalkeep(c, me, team, them);
        return Finish(c);
    }
    cFielder* fielder = static_cast<cFielder*>(me);
    if (fielder->GetActionState() == ACTION_SHOOT_TO_SCORE)
    {
        MegaStrikeMeter(fielder);
        return Finish(c);
    }
    mMeterPressed = false;
    mMeterPrevious = -10.0f;
    mFirstPressed = false;
    mMeterFull = false;
    if (fielder->GetActionState() == ACTION_MEGA_STRIKE) return Finish(c);

    if (mShot >= 0 && Shooting(c, fielder, team, them, owner == me)) return Finish(c);
    if (owner == me)
        Attack(c, fielder, team, them, goal);
    else if (owner && owner->m_pTeam != team)
        Defend(c, fielder, team, owner);
    else if (!owner)
        Chase(c, fielder, team, goal);
    else
    {
        // A teammate has the ball (the pad follows it at the next step): up the field.
        c.direction = Steer(Pos(me), goal, them);
        c.deflection = 1.0f;
    }
    UseItem(me, team);
    return Finish(c);
}

Controls& Bot::Finish(Controls& c)
{
    if (mTapSteps > 0)
    {
        c.buttons |= mTap;
        if (mTapSteers)
        {
            c.direction = mTapDirection;
            c.deflection = 1.0f;
        }
        if (--mTapSteps == 0) mRestSteps = mTapRest;
    }
    else if (mRestSteps > 0)
        --mRestSteps;
    if (mShot >= 0) c.buttons |= Bit(kShoot);
    if (mShake >= 0)
    {
        c.shake = mShake;
        if (++mShake >= ShakeSteps) mShake = -1;
    }
    return c;
}

// The goalie with the ball: a look up the field, then the pass.
void Bot::Goalkeep(Controls& c, cPlayer* me, cTeam* team, cTeam* them)
{
    if (Save(c)) return;
    if (g_pBall->GetOwner() != me)
    {
        mGoalieWait = 0;
        return;
    }
    const Vec goal = Pos(team->GetOtherNet()->GetNetLocation());
    if (cFielder* mate = PassTarget(me, team, them, goal))
    {
        c.direction = Toward(Pos(me), Pos(mate));
        c.deflection = 1.0f;
    }
    if (++mGoalieWait >= mGoalieHold && CanTap())
    {
        TapToward(kShoot, 3, c.direction);
        mGoalieWait = 0;
        mGoalieHold = Between(15, 40);
    }
}

// While B is held for a windup; false once there is no windup to hold. The
// release follows the Legend scripts (TransDesireWindup*): a shot as soon as
// it is likely to score or their goalie is down, at once when pressed in range,
// and at full charge a release that comes the sooner the shooter is in range.
// A Skillshot or Mega Strike windup holds on until the game's meter takes it,
// unless the chance goes.
bool Bot::Shooting(Controls& c, cFielder* me, cTeam* team, cTeam* them, bool hasBall)
{
    if (!hasBall && me->GetActionState() != ACTION_SHOT_WINDUP)
    {
        StopShot();
        return false;
    }
    c.direction = mAim;
    c.deflection = 1.0f;
    ++mShot;
    Goalie* goalie = them->GetGoalie();
    const float down = goalie ? GoalieDown(goalie) : 0.0f;
    const float reach = Reach(them, me);
    // About to be hit, or their goalie on the ground: away now, charged or not.
    bool release = reach < 0.12f || down > 0.5f;
    // A shot at full charge; a Skillshot or a Mega Strike from the game's own
    // meter at its total time (B up as the meter takes over).
    if (mWindup == Windup::Shot && ShotWindupProgress(me) >= 1.0f) release = true;
    if (mShot > (mWindup == Windup::MegaStrike ? kMegaHoldSteps : 70)) release = true;
    if (release)
    {
        StopShot();
    }
    (void)team;
    return true;
}

// The Mega Strike meter (cFielder::UpdateMegaStrikeMeter): the first press in
// the zone of the most balls; the second as the falling bar comes to its most
// accurate point, a little early for the step the press takes.
void Bot::MegaStrikeMeter(cFielder* me)
{
    mShot = -1; // B comes up from the windup, so the meter sees the press
    if (!me->m_bMegaStrikeSecondPhase)
    {
        // The first bar: pressed once its needle has stayed a step in the zone
        // of the most balls (GetMegaStrikeShotCount on the needle as it is). Not
        // pressed, it would time out as position 1, past every zone: 3 balls.
        mMeterPressed = false;
        mMeterPrevious = -10.0f;
        const bool full = GetMegaStrikeShotCount(me, 1) >= me->GetMegaStrikeMaxBalls();
        if (!mFirstPressed && full && mMeterFull && CanTap())
        {
            Tap(gMegaStrikeUsePassButton ? kPass : kShoot, 2);
            mFirstPressed = true;
        }
        mMeterFull = full;
        return;
    }
    const float position = me->GetMegaStrikeMeterPosition();
    const float fall = mMeterPrevious > -5.0f ? std::max(mMeterPrevious - position, 0.0f) : 0.0f;
    mMeterPrevious = position;
    if (!mMeterPressed && position - 0.6f * fall <= kMeterBest && CanTap())
    {
        Tap(gMegaStrikeUsePassButton ? kPass : kShoot, 2);
        mMeterPressed = true;
    }
}

// The goalie's save of the other team's Mega Strike (Goalie::ActionMegaStrike):
// while its pointer shows, the pointer on the ball whose time runs out first
// (its scale tween), then A once the pointer is on it. False: no save under way.
bool Bot::Save(Controls& c)
{
    for (int& wait : mSaved)
        if (wait > 0) --wait;
    if (!gMegaBallPointer.mActive) return false;
    const MegaBallIndicator* target = nullptr;
    int targetIndex = -1;
    float left = 1e9f;
    for (unsigned i = 0; i < 10; ++i)
    {
        const MegaBallIndicator* ball = GetMegaBallIndicator(i);
        if (!ball->mActive || !ball->mScaleTween.mActive || mSaved[i] > 0) continue;
        const float remaining = ball->mScaleTween.mEndTime - ball->mScaleTween.mElapsedTime;
        if (remaining < left)
        {
            left = remaining;
            target = ball;
            targetIndex = static_cast<int>(i);
        }
    }
    if (!target) return true;
    // Just off its centre: the pointer's magnet (UpdateAndRenderMegaBallPointer)
    // divides by the distance to the ball, and a pointer right on it ends as NaN.
    c.point = true;
    c.pointX = target->mX + kSaveAimOffset;
    c.pointY = target->mY + kSaveAimOffset;
    if (std::fabs(gMegaBallPointer.mX - target->mX) < 14.0f && std::fabs(gMegaBallPointer.mY - target->mY) < 14.0f
        && CanTap())
    {
        Tap(kPass, 1, 1);
        mSaved[targetIndex] = 40;
    }
    return true;
}

// The carrier, with the game's own judgement (the questions the Legend scripts
// ask) and none of the CPU's dice, thought over every 0.1 s: the captain's Mega
// Strike whenever nobody can reach him in its windup; a shot the game rates
// likely to score, with the time to charge it; a sidekick's Skillshot in range;
// a pass through an open lane, to the captain who can take a Mega Strike or
// away from pressure; a deke as an opponent arrives; else the run at the net.
void Bot::Attack(Controls& c, cFielder* me, cTeam* team, cTeam* them, Vec goal)
{
    const Vec at = Pos(me);
    c.direction = Steer(at, goal, them);
    c.deflection = 1.0f;
    if (!CanTap()) return;
    if (mThink > 0)
    {
        --mThink;
        return;
    }
    mThink = 4;
    const float reach = Reach(them, me);
    const bool captain = me == team->GetCaptain();
    const float good = GoodToShoot(me);
    const auto windup = [&](Windup kind) {
        mWindup = kind;
        mShot = 0;
        mAim = Aim(me, them, goal);
        c.direction = mAim;
    };
    if (!mCool.shot)
    {
        if (captain && InOffensiveZone(me) > 0.0f && std::fabs(goal.x - at.x) > 3.0f && reach > 0.8f
            && InDangerForMegastrike(me, them) < 0.3f)
        {
            windup(Windup::MegaStrike);
            return;
        }
        if (good > 0.55f && InFrontOfTheirNet(me) > 0.0f && reach > 0.25f)
        {
            windup(Windup::Shot);
            return;
        }
        // Each sidekick's Skillshot at its own distance (AtIdealDistanceForSkillshot:
        // Boo close in, Hammer Bro and Monty Mole from 7 to 15-18 m, Birdo down an
        // open lane, the rest anywhere in their half, kept here within 16 m).
        const eCharacterClass who = me->m_DetChar.m_eCharacterClass;
        const bool anywhere = who == KOOPA || who == TOAD || who == DRYBONES || who == SHYGUY;
        if (!captain && AtIdealDistanceForSkillshot(me) > 0.5f && Open(me) > 0.3f && reach > 0.6f
            && (!anywhere || Distance(at, goal) < 16.0f))
        {
            windup(Windup::Skillshot);
            return;
        }
    }
    if (!mCool.pass && std::max({Invincible(me), SuperPowerActive(me)}) <= 0.5f)
    {
        cFielder* mate = nullptr;
        float score = 0.0f;
        bool setup = false;
        for (int i = 0; i < 4; ++i)
        {
            cFielder* other = team->GetFielder(i);
            if (other == me || !Ready(other) || OpenTo(me, other) < 0.6f) continue;
            const bool mega = other == team->GetCaptain() && InOffensiveZone(other) > 0.0f && Reach(them, other) > 1.5f;
            const float s = PassQuality(other, me) + (mega ? 0.5f : 0.0f);
            if (s > score)
            {
                score = s;
                mate = other;
                setup = mega;
            }
        }
        if (mate && ((reach < 0.35f && score > 0.3f) || setup || GoodToShoot(mate) > good + 0.3f))
        {
            TapToward(kPass, 3, Toward(at, Pos(mate)));
            mCool.pass = 40;
            return;
        }
    }
    // A deke as each character's serves: any as a hit comes (Boo's goes through
    // it); a teleport past their goalie near the goal line; Peach, Diddy and Toad
    // round him; the heavy ones barge into an opponent in their way.
    if (mCool.deke || InvincibleToCharacters(me) > 0.5f) return;
    const eCharacterClass who = me->m_DetChar.m_eCharacterClass;
    Goalie* goalie = them->GetGoalie();
    const float toGoalie = goalie ? Distance(at, Pos(goalie)) : 1e9f;
    float threatDistance = 0.0f;
    cFielder* threat = Nearest(them, at, &threatDistance);
    const bool threatAhead = threat && AngleGap(Toward(at, Pos(threat)), Toward(at, goal)) < 0x2AAA;
    bool deke = reach < 0.2f;
    if (Teleports(who)) deke = deke || (std::fabs(goal.x - at.x) < 2.0f * me->GetDekeDistance() && toGoalie < 4.0f);
    if (RoundsGoalie(who)) deke = deke || toGoalie < 3.0f;
    if (Heavy(who)) deke = deke || (threatAhead && threatDistance < 2.0f);
    if (!deke) return;
    TapToward(kDeke, 3, DekeDirection(me, them, goal));
    mCool.deke = 50;
}

// An opponent has the ball: the pad to a teammate much nearer it, a run at
// where the carrier goes, a check (a Remote shake) or a slide tackle.
void Bot::Defend(Controls& c, cFielder* me, cTeam* team, cPlayer* carrier)
{
    const Vec at = Pos(me);
    const Vec ball = Pos(carrier);
    const float distance = Distance(at, ball);
    if (carrier->m_eClassType != FIELDER)
    {
        // Their goalie holds it: wait between the ball and our goal.
        const Vec own = Pos(team->m_pNet->GetNetLocation());
        const Vec post{(ball.x + own.x) * 0.5f, (ball.y + own.y) * 0.5f};
        if (Distance(at, post) > 1.0f)
        {
            c.direction = Toward(at, post);
            c.deflection = 1.0f;
        }
        return;
    }
    float mateDistance = 0.0f;
    if (distance > 4.0f && !mCool.swap && CanTap() && Nearest(team, ball, &mateDistance, me)
        && mateDistance + 3.0f < distance)
    {
        TapToward(kSwitch, 2, Toward(at, ball));
        mCool.swap = 30;
        return;
    }
    const Vec run = Vel(carrier);
    const float lead = std::min(0.6f, distance / 9.0f);
    c.direction = Toward(at, {ball.x + run.x * lead, ball.y + run.y * lead});
    c.deflection = 1.0f;
    if (distance < kHitRange && !mCool.hit && mShake < 0)
    {
        mShake = 0;
        mCool.hit = 45;
    }
    else if (distance >= kHitRange && distance < kSlideRange && !mCool.slide && CanTap() && Chance() < 0.08f)
    {
        TapToward(kDeke, 3, c.direction);
        mCool.slide = 60;
    }
}

// A loose ball: the pad to the teammate the game ranks first to reach it, a
// run at where it goes, and a first-time shot when it comes by near the goal.
void Bot::Chase(Controls& c, cFielder* me, cTeam* team, Vec goal)
{
    const Vec at = Pos(me);
    const Vec ball = Pos(g_pBall->GetPosition());
    const float distance = Distance(at, ball);
    cFielder* best = team->GetBestBallInterceptor();
    if (best && best != me && Ready(best) && distance > 4.0f && !mCool.swap && CanTap()
        && Distance(Pos(best), ball) + 2.5f < distance)
    {
        TapToward(kSwitch, 2, Toward(at, ball));
        mCool.swap = 30;
        return;
    }
    const float lead = std::min(0.8f, distance / 8.0f);
    c.direction = Toward(at, {ball.x + g_pBall->m_v3Velocity.x * lead, ball.y + g_pBall->m_v3Velocity.y * lead});
    c.deflection = 1.0f;
    // A first-time shot as it comes, when the scripts would take one (AttemptOneTimer).
    cTeam* them = team->GetOtherTeam();
    if (distance < 2.0f && !mCool.shot && CanTap() && them)
    {
        const bool captainSafe = me == team->GetCaptain() && Less(InDangerForMegastrike(me, them), 0.6f) > 0.5f;
        if (OneTimer(me, them, captainSafe) > 0.5f)
        {
            TapToward(kShoot, 2, Aim(me, them, goal));
            mCool.shot = 30;
        }
    }
}

// The team's item, a moment after it comes: the captain's own (his super
// ability) by him with the ball in their half; a mushroom or a star with the
// ball; the rest thrown at their carrier, else at the nearest opponent.
void Bot::UseItem(cPlayer* me, cTeam* team)
{
    const PowerUpTeamType item = team->GetCurrentPowerUp();
    if (item.eType == POWER_UP_NONE || item.nnumOfPowerups <= 0)
    {
        mItemWait = -1;
        return;
    }
    if (mItemWait < 0)
        mItemWait = Between(10, 30);
    else if (mItemWait > 0)
        --mItemWait;
    if (mItemWait != 0 || mShot >= 0 || !CanTap()) return;
    cTeam* them = team->GetOtherTeam();
    cPlayer* owner = g_pBall->GetOwner();
    if (IsCaptainPowerup(item.eType))
    {
        if (me != team->GetCaptain() || owner != me || InOffensiveZone(me) <= 0.0f) return;
        Tap(kItem, 3);
    }
    else if (item.eType == POWER_UP_MUSHROOM || item.eType == POWER_UP_STAR)
    {
        if (owner != me) return;
        Tap(kItem, 3);
    }
    else if (item.eType == POWER_UP_BANANA)
    {
        // Dropped behind: with the ball and a chaser close behind.
        float d = 0.0f;
        const cFielder* chaser = Nearest(them, Pos(me), &d);
        if (owner != me || !chaser || d > 4.0f
            || AngleGap(Toward(Pos(me), Pos(chaser)), me->GetActualFacing()) < 0x4000)
            return;
        Tap(kItem, 3);
    }
    else
    {
        float d = 0.0f;
        const cPlayer* target = owner && owner->m_pTeam == them ? owner : Nearest(them, Pos(me), &d);
        if (!target || Distance(Pos(me), Pos(target)) > 15.0f) return;
        TapToward(kItem, 3, Toward(Pos(me), Pos(target)));
    }
    mItemWait = Between(20, 60);
}

// The free teammate nearest the goal, at a passing distance.
cFielder* Bot::PassTarget(const cPlayer* passer, cTeam* team, cTeam* them, Vec goal)
{
    const Vec from = Pos(passer);
    cFielder* best = nullptr;
    float bestScore = -1e9f;
    for (int i = 0; i < 4; ++i)
    {
        cFielder* mate = team->GetFielder(i);
        if (mate == passer || !Ready(mate)) continue;
        float free = 0.0f;
        Nearest(them, Pos(mate), &free);
        if (free < 2.5f) continue;
        const float score = -Distance(Pos(mate), goal) + 1.5f * std::min(free, 8.0f)
            - 0.3f * std::fabs(Distance(from, Pos(mate)) - 9.0f);
        if (score > bestScore)
        {
            best = mate;
            bestScore = score;
        }
    }
    return best;
}

// Toward the goal, round the opponents just ahead and off the walls.
u16 Bot::Steer(Vec at, Vec goal, cTeam* them)
{
    const float length = std::max(Distance(at, goal), 0.001f);
    Vec way{(goal.x - at.x) / length, (goal.y - at.y) / length};
    for (int i = 0; i < 4; ++i)
    {
        cFielder* opponent = them->GetFielder(i);
        if (!Ready(opponent)) continue;
        const Vec to{opponent->GetPosition().x - at.x, opponent->GetPosition().y - at.y};
        const float d = std::hypot(to.x, to.y);
        if (d < 0.001f || d > 5.0f || to.x * way.x + to.y * way.y <= 0.0f) continue;
        // Sideways, away from the opponent, the more the nearer.
        const float side = way.x * to.y - way.y * to.x > 0.0f ? -1.0f : 1.0f;
        const float push = 1.2f * (5.0f - d) / 5.0f;
        const Vec across{-way.y * side, way.x * side};
        way.x += across.x * push;
        way.y += across.y * push;
    }
    if (std::fabs(at.y) > 9.5f) way.y -= (at.y > 0.0f ? 0.6f : -0.6f);
    return AngleOf(way.x, way.y);
}

// The side of the goal away from its goalie.
u16 Bot::Aim(cFielder* me, cTeam* them, Vec goal)
{
    const float post = 0.35f * cNet::GetNetWidth();
    const float goalie = them->GetGoalie() ? them->GetGoalie()->GetPosition().y : 0.0f;
    return Toward(Pos(me), {goal.x, goalie > 0.0f ? -post : post});
}

// The local player the autopilot plays: the first local controller, and the
// player on the field it controls, or null.
cPlayer* Steered(int& pad)
{
    NetworkSessionBase* session = g_pNetworkSessionBase;
    if (!session || !g_pGame || !g_pBall || !g_pTeams[0] || !g_pTeams[1]) return nullptr;
    NetworkPeer* peer = session->GetLocalPeer();
    if (!peer || peer->mPlayerCount == 0) return nullptr;
    NetworkPeerChannel* channel = peer->GetNetworkPeerChannel(0);
    pad = channel->GetGlobalPadIndex();
    if (pad < 0 || pad >= WPAD_MAX_CONTROLLERS) return nullptr;
    const cAIPad* controller = GetAIPad(channel->GetNetworkPeerChannelId());
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < 5; ++i)
            if (cPlayer* player = g_pTeams[side]->GetPlayer(i); player && player->m_pController == controller)
                return player;
    return nullptr;
}

enum class Mode
{
    Off,
    On,
    // Switched off: the player gets the pad back once every button is up, so
    // nothing still held reaches the game as a press.
    Returning,
};
Mode sMode = Mode::Off;
int sReturningSteps = 0;
Bot sBot;
} // namespace

extern "C" __attribute__((visibility("default"))) int charged_set_pad_source(int on)
{
    sOn.store(on != 0, std::memory_order_relaxed);
    return 1;
}

void mscharged_tweak_step_pads(PlatPadManager* manager)
{
    const bool on = sOn.load(std::memory_order_relaxed);
    if (on && sMode != Mode::On)
    {
        sBot.Forget();
        sMode = Mode::On;
    }
    else if (!on && sMode == Mode::On)
    {
        sMode = Mode::Returning;
        sReturningSteps = 0;
    }
    if (sMode == Mode::Off || !manager) return;

    int pad = -1;
    cPlayer* me = Steered(pad);
    if (pad < 0 || pad >= WPAD_MAX_CONTROLLERS || !manager->connected[pad]) return;
    // Without a Nunchuk there is no stick to steer with.
    if (manager->GetType(pad) != PLAT_PAD_FREESTYLE) return;
    WPADFSStatus& status = manager->status[pad].freestyle.wpad;
    if (sMode == Mode::Returning)
    {
        if (status.button == 0 || ++sReturningSteps > 100)
        {
            sMode = Mode::Off;
            return;
        }
        status.button = 0;
        status.fsStickX = status.fsStickY = 0;
        return;
    }

    // Outside play the player keeps the pad; at a kickoff it stays still.
    const eGameState state = g_pGame ? g_pGame->m_eGameState : GS_NONE;
    if (!me || (state != GS_KICKOFF && state != GS_GAMEPLAY && state != GS_OVERTIME)) return;
    Controls controls;
    if (state != GS_KICKOFF) controls = sBot.Step(me);

    u16 remap = gInputManager && gInputManager->mFrameProvider ? gInputManager->mFrameProvider->GetInputRemapAngle() : 0;
    // A network match sends the high byte of the camera's angle alone (NetMessageInput).
    if (g_pNetworkSessionBase->GetNumMachines() > 1) remap &= 0xFF00;
    const Stick stick = NunchukStick(controls.direction, controls.deflection, remap);
    status.button = controls.buttons;
    status.fsStickX = stick.x;
    status.fsStickY = stick.y;
    if (controls.shake >= 0)
        status.accX = static_cast<s16>(std::clamp(status.accX + ShakeAccel(controls.shake), -510, 510));
    if (controls.point)
    {
        // The pointer of a Mega Strike save: x = 320 + 400 pos.x, y = 240 + 300 pos.y
        // (DPDData::Update, UpdateAndRenderMegaBallPointer).
        KPADStatus& pointer = manager->status[pad].freestyle.kpad;
        pointer.pos.x = (controls.pointX - 320.0f) / 400.0f;
        pointer.pos.y = (controls.pointY - 240.0f) / 300.0f;
        pointer.dpd_valid_fg = 2;
    }
}

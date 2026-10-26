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
#include "Game/AI/Fielder.h"
#include "Game/AI/FielderActions.h"
#include "Game/AI/Powerups.h"
#include "Game/Ball.h"
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

// Field lengths (cField: goal lines at x = +-20.6, walls at y = +-12.5).
constexpr float kShootRange = 9.0f;
constexpr float kShootWidth = 8.0f;
// A captain's Mega Strike: begun in the attacking half off the goal mouth,
// with nobody close, and B held until the meter takes over (it can take 4 s).
constexpr float kMegaNear = 6.6f;
constexpr float kMegaFar = 13.5f;
constexpr float kMegaRoom = 5.0f;
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

    void Goalkeep(Controls& c, cPlayer* me, cTeam* team, cTeam* them);
    bool Save(Controls& c);
    bool Shooting(Controls& c, cFielder* me, cTeam* them, bool hasBall);
    void MegaStrikeMeter(cFielder* me);
    void Attack(Controls& c, cFielder* me, cTeam* team, cTeam* them, Vec goal);
    void Defend(Controls& c, cFielder* me, cTeam* team, cPlayer* carrier);
    void Chase(Controls& c, cFielder* me, cTeam* team, Vec goal);
    void UseItem(cTeam* team);
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
    int mShotLength = 0;
    bool mMega = false;
    u16 mAim = 0;
    int mShake = -1;
    int mItemWait = -1;
    int mGoalieWait = 0;
    int mGoalieHold = 20;
    bool mMeterPressed = false;
    float mMeterPrevious = -10.0f;
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
    if (fielder->GetActionState() == ACTION_MEGA_STRIKE) return Finish(c);

    if (mShot >= 0 && Shooting(c, fielder, them, owner == me)) return Finish(c);
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
    UseItem(team);
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

// While B is held for a shot; false once there is no shot to hold.
bool Bot::Shooting(Controls& c, cFielder* me, cTeam* them, bool hasBall)
{
    if (!hasBall && me->GetActionState() != ACTION_SHOT_WINDUP)
    {
        StopShot();
        return false;
    }
    c.direction = mAim;
    c.deflection = 1.0f;
    // The captain keeps B down until the Mega Strike's meter takes over, and
    // shoots at once when a defender comes too close for it (or it never comes).
    float threat = 1e9f;
    if (mMega) Nearest(them, Pos(me), &threat);
    if (++mShot >= (mMega ? kMegaHoldSteps : mShotLength) || threat < 2.5f) StopShot();
    return true;
}

// The Mega Strike meter (cFielder::UpdateMegaStrikeMeter): the first bar
// unpressed keeps the most balls; the second press as the falling bar comes
// to its most accurate point, a little early for the step the press takes.
void Bot::MegaStrikeMeter(cFielder* me)
{
    mShot = -1; // B comes up from the windup, so the meter sees the press
    if (!me->m_bMegaStrikeSecondPhase)
    {
        mMeterPressed = false;
        mMeterPrevious = -10.0f;
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
    c.point = true;
    c.pointX = target->mX;
    c.pointY = target->mY;
    if (std::fabs(gMegaBallPointer.mX - target->mX) < 14.0f && std::fabs(gMegaBallPointer.mY - target->mY) < 14.0f
        && CanTap())
    {
        Tap(kPass, 1, 1);
        mSaved[targetIndex] = 40;
    }
    return true;
}

void Bot::Attack(Controls& c, cFielder* me, cTeam* team, cTeam* them, Vec goal)
{
    const Vec at = Pos(me);
    const float toGoal = Distance(at, goal);
    const u16 goalWay = Toward(at, goal);
    float threatDistance = 0.0f;
    cFielder* threat = Nearest(them, at, &threatDistance);
    const bool threatAhead = threat && AngleGap(Toward(at, Pos(threat)), goalWay) < 0x2AAA;

    // The captain with room in the attacking half: a Mega Strike. In front of
    // the goal and facing it: a shot, charged for a moment, at times to a
    // Skillshot.
    const bool facing = AngleGap(me->GetActualFacing(), goalWay) < 0x3000;
    const bool mega = me == team->GetCaptain() && toGoal > kMegaNear && toGoal < kMegaFar
        && std::fabs(at.y) < kShootWidth && threatDistance > kMegaRoom;
    if ((mega || (toGoal < kShootRange && std::fabs(at.y) < kShootWidth)) && facing && !mCool.shot && CanTap())
    {
        mMega = mega;
        mShotLength = Chance() < 0.3f ? Between(45, 60) : Between(10, 24);
        mShot = 0;
        mAim = Aim(me, them, goal);
        c.direction = mAim;
        c.deflection = 1.0f;
        return;
    }
    // Pressed from the front: a pass to a free teammate, else a deke past.
    if (threat && threatDistance < 3.0f && threatAhead && CanTap())
    {
        cFielder* mate = PassTarget(me, team, them, goal);
        if (mate && !mCool.pass && Chance() < 0.6f)
        {
            TapToward(kPass, 3, Toward(at, Pos(mate)));
            mCool.pass = 40;
            return;
        }
        if (!mCool.deke)
        {
            const bool threatLeft = static_cast<s16>(Toward(at, Pos(threat)) - goalWay) > 0;
            TapToward(kDeke, 3, static_cast<u16>(goalWay + (threatLeft ? -0x2800 : 0x2800)));
            mCool.deke = 50;
            return;
        }
    }
    // Far out, now and then a pass to a free teammate well up the field.
    if (toGoal > 16.0f && !mCool.pass && CanTap() && Chance() < 0.03f)
    {
        cFielder* mate = PassTarget(me, team, them, goal);
        if (mate && Distance(Pos(mate), goal) + 6.0f < toGoal)
        {
            TapToward(kPass, 3, Toward(at, Pos(mate)));
            mCool.pass = 40;
            return;
        }
    }
    c.direction = Steer(at, goal, them);
    c.deflection = 1.0f;
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
    if (distance < 2.0f && Distance(ball, goal) < kShootRange && !mCool.shot && CanTap() && Chance() < 0.5f)
    {
        TapToward(kShoot, 2, Toward(at, goal));
        mCool.shot = 30;
    }
}

// The team's item, used a moment after it comes.
void Bot::UseItem(cTeam* team)
{
    const PowerUpTeamType item = team->GetCurrentPowerUp();
    if (item.eType == POWER_UP_NONE || item.nnumOfPowerups <= 0)
    {
        mItemWait = -1;
        return;
    }
    if (mItemWait < 0)
        mItemWait = Between(25, 100);
    else if (mItemWait > 0)
        --mItemWait;
    else if (mShot < 0 && CanTap())
    {
        Tap(kItem, 3);
        mItemWait = Between(40, 120);
    }
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

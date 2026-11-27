// Parallel posing while a replay plays (tweak patch 0091). A step poses the
// ten characters in cCharacter::PostPhysicsUpdate, one after the other in the
// physics engine's order, each with cPoseAccumulator::Pose: the evaluation of
// its animation tree (two thirds of the posing, a quarter of a step), then
// its bone matrices. The evaluation reads only the character's own tree,
// animations and accumulator. PhysicsUpdate begins by copying each tree and
// accumulator here (on the game thread), and the host's workers
// (platform/parallel_jobs.h) evaluate the copies while the game thread runs
// the collisions and the physics engine's step; they read nothing the game
// thread may change meanwhile but loaded data. PostPhysicsUpdate then takes a
// character's result if everything its evaluation read is still the same
// (else it poses as before) and builds the bone matrices itself, in their
// order: their callbacks (head tracking) read the other characters and the
// ball.
//
// Same calculations, same inputs, same results: the match plays exactly as
// it would. RECHARGED_PARALLEL_POSE=verify evaluates every taken pose again
// as the game does and counts the differences (the replay's end logs them).
#include "mscharged_replay.h"
#include "platform/parallel_jobs.h"
#include "platform/parallel_pose.h"

#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/PoseAccumulator.h"
#include "Game/PoseNode.h"
#include "Game/SAnim.h"
#include "Game/SAnim/pnBlender.h"
#include "Game/SAnim/pnFeather.h"
#include "Game/SAnim/pnSAnimController.h"
#include "Game/SAnim/pnScaleBlender.h"
#include "Game/SAnim/pnSingleAxisBlender.h"
#include "Game/SHierarchy.h"

#include <algorithm>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr unsigned kCharacters = 10;
// What an evaluation read, as a list of words (see Inputs): a tree has a few
// dozen nodes, a feather node adds one word a bone.
constexpr unsigned kMostWords = 4096;
constexpr unsigned kMostNodes = 64;

ChargedParallelJobs sTable{};
bool sConfigured = false;
int sMode = -1; // 0 off, 1 on, 2 verify (RECHARGED_PARALLEL_POSE)

struct Inputs
{
    std::uint64_t words[kMostWords];
    unsigned count = 0;
    bool full = false;

    void Add(std::uint64_t word)
    {
        if (count < kMostWords) words[count++] = word;
        else full = true;
    }
    void Add(const void* pointer) { Add(std::uint64_t(reinterpret_cast<std::uintptr_t>(pointer))); }
    void Add(float value)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        Add(std::uint64_t(bits));
    }
    bool operator==(const Inputs& other) const
    {
        return !full && !other.full && count == other.count
               && std::memcmp(words, other.words, count * sizeof words[0]) == 0;
    }
};

// The size of a node of each kind the evaluation here takes on (0: another).
std::size_t NodeSize(int type)
{
    switch (type)
    {
    case POSE_NODE_ANIMATION: return sizeof(cPN_SAnimController);
    case POSE_NODE_BLENDER: return sizeof(cPN_Blender);
    case POSE_NODE_FEATHER: return sizeof(cPN_Feather);
    case POSE_NODE_SINGLE_AXIS_BLENDER: return sizeof(cPN_SingleAxisBlender);
    case POSE_NODE_SCALE_BLENDER: return sizeof(cPN_ScaleBlender);
    }
    return 0;
}

// A copied node's room: the largest kind's size, rounded to 16 bytes.
constexpr std::size_t kNodeSlot =
    ((std::max({sizeof(cPN_SAnimController), sizeof(cPN_Blender), sizeof(cPN_Feather), sizeof(cPN_SingleAxisBlender),
                sizeof(cPN_ScaleBlender)})
      + 15)
     / 16)
    * 16;

// Everything Pose's evaluation (InitAccumulators, then the tree's Evaluate)
// reads that can change during a match, in a fixed order: the accumulator's
// sizes, scale and the translations InitAccumulators keeps (the bones that
// keep their length), and every node of the tree with its children and the
// fields its Evaluate reads. Animations, retarget maps and hierarchies are
// loaded data. False for what the evaluation here does not take on: a node
// of another kind, an animation with more morph channels than the
// accumulator has, a tree too big. With `copies`, it also copies the tree
// there for a worker: each node byte for byte (plain objects of five kinds),
// their children pointing at the copies; `copy` is the copied root.
bool Walk(const cPoseNode* tree, const cPoseAccumulator* pose, Inputs& out, unsigned char* copies = nullptr,
          const cPoseNode** copy = nullptr)
{
    out.count = 0;
    out.full = false;
    const cSHierarchy* hierarchy = pose->m_BaseSHierarchy;
    if (!tree || !hierarchy || hierarchy->m_nNumNodes != int(pose->m_nNumNodes)) return false;
    const int nodes = hierarchy->m_nNumNodes;
    out.Add(hierarchy);
    out.Add(std::uint64_t(pose->m_nNumNodes));
    out.Add(pose->m_Scale);
    for (int i = 0; i < nodes; ++i)
    {
        if (!hierarchy->PreserveBoneLength(i)) continue;
        const TransAccum& t = pose->m_trans[i];
        out.Add(t.t.x);
        out.Add(t.t.y);
        out.Add(t.t.z);
        out.Add(t.fAccumulatedWeight);
        out.Add(std::uint64_t(t.bIdentity));
    }
    // The nodes to look at, and where their copy's address goes.
    struct Item
    {
        const cPoseNode* node;
        cPoseNode** link;
    };
    Item stack[kMostNodes];
    unsigned depth = 0, seen = 0;
    cPoseNode* root = nullptr;
    stack[depth++] = {tree, copies ? &root : nullptr};
    while (depth)
    {
        const Item item = stack[--depth];
        const cPoseNode* node = item.node;
        if (seen == kMostNodes) return false;
        std::uintptr_t table;
        std::memcpy(&table, static_cast<const void*>(node), sizeof table); // its kind (its vtable)
        out.Add(node);
        out.Add(std::uint64_t(table));
        // Its children (the slots past them are left as they were: not read).
        const int children = node->m_numChildren;
        if (children < 0 || children > 3) return false;
        out.Add(std::uint64_t(unsigned(children)));
        for (int i = 0; i < children; ++i) out.Add(node->m_children[i]);
        const int type = const_cast<cPoseNode*>(node)->GetType();
        out.Add(std::uint64_t(unsigned(type)));
        // The blenders, feathers and scale blenders evaluate children 0 and 1.
        if ((type == POSE_NODE_BLENDER || type == POSE_NODE_FEATHER || type == POSE_NODE_SCALE_BLENDER) && children < 2)
            return false;
        switch (type)
        {
        case POSE_NODE_ANIMATION:
        {
            const auto* animation = static_cast<const cPN_SAnimController*>(node);
            if (!animation->m_pSAnim || animation->m_pSAnim->m_nNumMorphChannels > 20) return false;
            out.Add(animation->m_pSAnim);
            out.Add(animation->m_fTime);
            out.Add(animation->m_pAnimRetarget);
            out.Add(std::uint64_t(animation->m_bMirror));
            break;
        }
        case POSE_NODE_BLENDER:
            out.Add(static_cast<const cPN_Blender*>(node)->m_fBlendTime);
            break;
        case POSE_NODE_FEATHER:
        {
            const auto* feather = static_cast<const cPN_Feather*>(node);
            out.Add(feather->m_pFeatherWeights);
            out.Add(feather->m_fBlendTime);
            if (feather->m_pFeatherWeights)
                for (int i = 0; i < nodes; ++i) out.Add(feather->m_pFeatherWeights[i]);
            break;
        }
        case POSE_NODE_SINGLE_AXIS_BLENDER:
            out.Add(static_cast<const cPN_SingleAxisBlender*>(node)->m_fSmoothedWeight);
            break;
        case POSE_NODE_SCALE_BLENDER:
            out.Add(static_cast<const cPN_ScaleBlender*>(node)->m_fBlendTime);
            break;
        default:
            return false;
        }
        cPoseNode* copied = nullptr;
        if (copies)
        {
            copied = reinterpret_cast<cPoseNode*>(copies + seen * kNodeSlot);
            std::memcpy(static_cast<void*>(copied), static_cast<const void*>(node), NodeSize(type));
            *item.link = copied;
        }
        ++seen;
        for (int i = 0; i < children; ++i)
            if (const cPoseNode* child = node->m_children[i])
            {
                if (depth == kMostNodes) return false;
                stack[depth++] = {child, copied ? &copied->m_children[i] : nullptr};
            }
    }
    if (copy) *copy = root;
    return !out.full;
}

enum class State
{
    Empty,
    Ready,
    Failed,
};

struct Job
{
    cPoseAccumulator* pose = nullptr;
    const cPoseNode* tree = nullptr;
    Inputs inputs;
    // The tree as it was copied, for the worker.
    alignas(16) unsigned char nodes[kMostNodes * kNodeSlot];
    const cPoseNode* copy = nullptr;
    // The evaluation's own accumulator: the character's, byte for byte, with
    // arrays of its own.
    alignas(cPoseAccumulator) unsigned char evaluated[sizeof(cPoseAccumulator)];
    RotAccum* rot = nullptr;
    ScaleAccum* scale = nullptr;
    TransAccum* trans = nullptr;
    unsigned capacity = 0;
    State state = State::Empty;
    bool taken = false;
};

Job sJobs[kCharacters];
// The jobs the workers run first, then the characters whose posing they do
// not take on.
unsigned sJobCount = 0, sAllCount = 0;
bool sRunning = false;
std::fenv_t sEnvironment;
// Since the replay began: poses taken from the workers, posed as before
// (their inputs had changed, or the evaluation here could not take them on),
// and in verify mode the taken ones that differed.
unsigned long sTaken = 0, sPosedAsBefore = 0, sDifferences = 0;

int Mode()
{
    if (sMode < 0)
    {
        const char* value = std::getenv("RECHARGED_PARALLEL_POSE");
        sMode = !value || !*value ? 1 : std::strcmp(value, "0") == 0 ? 0 : std::strcmp(value, "verify") == 0 ? 2 : 1;
    }
    return sMode;
}

// Room for a pose's arrays (host memory: new would take the game's heap).
bool Reserve(Job& job, unsigned nodes)
{
    if (job.capacity >= nodes) return true;
    std::free(job.rot);
    std::free(job.scale);
    std::free(job.trans);
    job.rot = static_cast<RotAccum*>(std::malloc(nodes * sizeof(RotAccum)));
    job.scale = static_cast<ScaleAccum*>(std::malloc(nodes * sizeof(ScaleAccum)));
    job.trans = static_cast<TransAccum*>(std::malloc(nodes * sizeof(TransAccum)));
    job.capacity = job.rot && job.scale && job.trans ? nodes : 0;
    return job.capacity != 0;
}

// On a worker: the character's pose evaluated as Pose does, into the job's
// accumulator.
void Evaluate(void*, unsigned index)
{
    Job& job = sJobs[index];
    try
    {
        std::fesetenv(&sEnvironment); // the game thread's rounding and denormals
        auto* pose = reinterpret_cast<cPoseAccumulator*>(job.evaluated);
        pose->m_rot = job.rot;
        pose->m_scale = job.scale;
        pose->m_trans = job.trans;
        pose->InitAccumulators();
        job.copy->Evaluate(1.0f, pose);
        job.state = State::Ready;
    }
    catch (...)
    {
        job.state = State::Failed; // the game thread poses it as before, and meets the same
    }
}

bool SameFloat(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Verify mode: Pose's evaluation in place again, as the game does it,
// compared with the job's. The accumulator keeps the game's own.
bool SameAsInPlace(Job& job, cPoseAccumulator* pose, const cPoseNode* tree)
{
    pose->InitAccumulators();
    tree->Evaluate(1.0f, pose);
    const auto* evaluated = reinterpret_cast<const cPoseAccumulator*>(job.evaluated);
    if (pose->m_bUseObject != evaluated->m_bUseObject) return false;
    for (int i = 0; i < 20; ++i)
        if (!SameFloat(pose->m_MorphWeights.mData[i], evaluated->m_MorphWeights.mData[i])) return false;
    for (unsigned i = 0; i < pose->m_nNumNodes; ++i)
    {
        const RotAccum &a = pose->m_rot[i], &b = job.rot[i];
        const ScaleAccum &c = pose->m_scale[i], &d = job.scale[i];
        const TransAccum &e = pose->m_trans[i], &f = job.trans[i];
        if (!SameFloat(a.q.x, b.q.x) || !SameFloat(a.q.y, b.q.y) || !SameFloat(a.q.z, b.q.z) || !SameFloat(a.q.w, b.q.w)
            || !SameFloat(a.quatAccumulatedWeight, b.quatAccumulatedWeight) || a.rotAroundZ != b.rotAroundZ
            || !SameFloat(a.rotAroundZAccumulatedWeight, b.rotAroundZAccumulatedWeight) || a.bIdentity != b.bIdentity
            || !SameFloat(c.s.x, d.s.x) || !SameFloat(c.s.y, d.s.y) || !SameFloat(c.s.z, d.s.z)
            || !SameFloat(c.fAccumulatedWeight, d.fAccumulatedWeight) || c.bIdentity != d.bIdentity
            || !SameFloat(e.t.x, f.t.x) || !SameFloat(e.t.y, f.t.y) || !SameFloat(e.t.z, f.t.z)
            || !SameFloat(e.fAccumulatedWeight, f.fAccumulatedWeight) || e.bIdentity != f.bIdentity)
            return false;
    }
    return true;
}

} // namespace

extern "C" {

__attribute__((visibility("default"))) void charged_configure_parallel_jobs(const ChargedParallelJobs* jobs)
{
    sConfigured = jobs && jobs->workers && jobs->start && jobs->finish;
    if (sConfigured) sTable = *jobs;
}

void mscharged_tweak_pose_begin(void)
{
    sJobCount = sAllCount = 0;
    if (!sConfigured || !mscharged_tweak_replay_playing() || Mode() == 0) return;
    std::fegetenv(&sEnvironment);
    // The trees and accumulators copied as they are now, with what their
    // evaluation reads; a character the evaluation here does not take on is
    // posed as before (it goes after the workers' jobs).
    cPoseAccumulator* failed[kCharacters];
    unsigned failedCount = 0;
    for (cCharacter* character : g_pCharacters)
    {
        if (!character || !character->m_pPoseAccumulator || !character->m_pPoseTree) continue;
        Job& job = sJobs[sJobCount];
        job.pose = character->m_pPoseAccumulator;
        job.tree = character->m_pPoseTree;
        job.state = State::Empty;
        job.taken = false;
        if (!Reserve(job, job.pose->m_nNumNodes) || !Walk(job.tree, job.pose, job.inputs, job.nodes, &job.copy))
        {
            failed[failedCount++] = job.pose;
            continue;
        }
        std::memcpy(job.evaluated, static_cast<const void*>(job.pose), sizeof job.evaluated);
        // The bones that keep their length keep their translation in Pose.
        std::memcpy(job.trans, job.pose->m_trans, job.pose->m_nNumNodes * sizeof(TransAccum));
        ++sJobCount;
    }
    sAllCount = sJobCount;
    for (unsigned i = 0; i < failedCount; ++i)
    {
        Job& job = sJobs[sAllCount++];
        job.pose = failed[i];
        job.state = State::Failed;
        job.taken = false;
    }
    if (!sJobCount) return;
    sTable.start(Evaluate, nullptr, sJobCount);
    sRunning = true;
}

void mscharged_tweak_pose_end(void)
{
    if (!sRunning) return;
    sTable.finish();
    sRunning = false;
}

bool mscharged_tweak_pose_take(void* accumulator, const void* poseTree)
{
    if (!sAllCount) return false;
    auto* pose = static_cast<cPoseAccumulator*>(accumulator);
    const auto* tree = static_cast<const cPoseNode*>(poseTree);
    Job* job = nullptr;
    for (unsigned i = 0; i < sAllCount && !job; ++i)
        if (sJobs[i].pose == pose) job = &sJobs[i];
    if (!job || job->taken) return false;
    job->taken = true;
    // Only if the evaluation ran on what Pose would evaluate now.
    static Inputs now;
    if (job->state != State::Ready || job->tree != tree || !Walk(tree, pose, now) || !(now == job->inputs))
    {
        ++sPosedAsBefore;
        return false;
    }
    ++sTaken;
    if (Mode() == 2)
    {
        if (!SameAsInPlace(*job, pose, tree)) ++sDifferences;
        return true;
    }
    // What InitAccumulators and Evaluate write: the three arrays, the morph
    // weights and whether a feather blended in object space.
    const unsigned nodes = pose->m_nNumNodes;
    const auto* evaluated = reinterpret_cast<const cPoseAccumulator*>(job->evaluated);
    std::memcpy(pose->m_rot, job->rot, nodes * sizeof(RotAccum));
    std::memcpy(pose->m_scale, job->scale, nodes * sizeof(ScaleAccum));
    std::memcpy(pose->m_trans, job->trans, nodes * sizeof(TransAccum));
    pose->m_MorphWeights = evaluated->m_MorphWeights;
    pose->m_bUseObject = evaluated->m_bUseObject;
    return true;
}

} // extern "C"

namespace mscharged::platform {

void ReportParallelPosing()
{
    if (!sTaken && !sPosedAsBefore) return;
    if (Mode() == 2)
        std::fprintf(stderr, "Replay: posing checked: %lu poses evaluated beside the physics step, %lu of them "
                             "differed from the game's own; %lu posed as before\n",
                     sTaken, sDifferences, sPosedAsBefore);
    else
        std::fprintf(stderr, "Replay: %lu poses evaluated beside the physics step on %u more threads, %lu posed as "
                             "before\n",
                     sTaken, sTable.workers, sPosedAsBefore);
    sTaken = sPosedAsBefore = sDifferences = 0;
}

} // namespace mscharged::platform

// RECHARGED_ROLLBACK: the fields of the match's checksum, for a desync. The
// game takes each one with RunningChecksum::ChecksumData (tweak patch 0032),
// which reports it here while the rollback takes a step's checksum. The
// fields of the steps whose inputs all arrived are kept with their checksums,
// and the first desync of a match lists those of its step; the other machine
// lists its own. The first field that differs between the two logs is what
// diverged, and where it was taken (an offset in the game module, for
// addr2line) names the line that takes it.
//
// RECHARGED_ROLLBACK_POSES=1, on every machine: the players' poses (the
// matrix of each node of their skeletons) join the checksum, which the game's
// leaves out. A desync then comes at the first step where any pose differs,
// and its fields name the player and the node.
//
// RECHARGED_ROLLBACK_BODIES=1, on every machine: the physics world's bodies
// (position, orientation, rotation matrix, linear and angular velocity, by
// their order in the world) join it too, for state the game's own checksum
// sees only once it copies it, and so does what the ball's position is made
// of when someone holds it: the position and rotation of the ball and of each
// object it hangs from, as PhysicsObject::GetPosition reads them.
//
// RECHARGED_ROLLBACK_CONTACTS=1, on every machine: what the step's collisions
// did joins it too (tweak patch 0032): each pair of objects that touched, with
// its contact count and both objects' Contact() results, and each contact
// joint made, with its surface and its geometry, and each DebugPrintf of the
// physics callbacks (the branches they take).
#include "Game/Ball.h"
#include "Game/Physics/Physics.h"
#include "Game/Physics/PhysicsAIBall.h"
#include "Game/Physics/PhysicsWorld.h"
#include "Game/Player.h"
#include "Game/PoseAccumulator.h"
#include "Game/Team.h"
#include "platform/rollback_state.h"
#include "platform/rollback_platform.h"
#include "ode/collision.h"
#include "ode/contact.h"
#include "src/ode/objects.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mscharged::rollback
{
namespace
{
constexpr int kMaxFields = 3072;     // blocks of one checksum: about 300 in a match, 1300 with the poses, more with the collisions
constexpr unsigned kFieldBytes = 16; // bytes of a block kept as they are

// A pose's field: kPoseField, the team, the player and the node instead of where.
constexpr std::uint32_t kPoseField = 0xF0000000u;
// A physics body's: kBodyField and its index in the world's list.
constexpr std::uint32_t kBodyField = 0xE0000000u;
// A collision's: kPairField or kContactField, and its place in the step.
constexpr std::uint32_t kPairField = 0xD0000000u;
constexpr std::uint32_t kContactField = 0xC0000000u;
// A DebugPrintf's: kPrintField and where it was called from, an offset in the
// game module; its hash is its message's, the same on every machine.
constexpr std::uint32_t kPrintField = 0xB0000000u;
// Then the numbers it prints, two to a field: kValuesField, bit 0 and bit 1
// whether the first and the second are doubles (else integers), bit 2 whether
// there is a second.
constexpr std::uint32_t kValuesField = 0xA0000000u;
// The ball's or an object it hangs from: kChainField, its depth (0 the ball)
// and the part, four words to a field: whether it reads a geom (1) or a body
// (2), its position, then its rotation's nine.
constexpr std::uint32_t kChainField = 0x90000000u;
// A PhysicsObject::SetPosition of the ball: kSetField and where it was called
// from; its position and its coordinates (0 world, 1 the parent's).
constexpr std::uint32_t kSetField = 0x80000000u;
// A contact's words: its surface (mode and ten parameters), its position,
// normal and depth, and its first friction direction; four to a field.
constexpr int kContactWords = 21;

struct Field
{
    std::uint32_t caller; // where it was taken: an offset in the game module, or a pose's
    std::uint32_t size;
    std::uint32_t hash;   // of all its bytes
    unsigned char bytes[kFieldBytes];
};

struct Fields
{
    unsigned frame;
    int count;
    bool overflowed;
    Field fields[kMaxFields];
};

// The module's own globals, outside the original game's: no rollback brings them back.
Fields sSteps[kRing];
Fields sConfirmed[kConfirmedRing];
Fields* sRecording;
std::uintptr_t sModuleBase;
std::uint32_t Hash(const unsigned char* bytes, unsigned long size)
{
    std::uint32_t hash = 2166136261u;
    for (unsigned long i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

// RECHARGED_ROLLBACK_CONTACTS: the collisions since the last step's checksum.
Fields sCollisions;
std::uint32_t sPairs, sContacts;

void AddCollisionField(std::uint32_t caller, const void* data, unsigned size)
{
    if (sCollisions.count == kMaxFields)
    {
        sCollisions.overflowed = true;
        return;
    }
    Field& field = sCollisions.fields[sCollisions.count++];
    field.caller = caller;
    field.size = size;
    field.hash = Hash(static_cast<const unsigned char*>(data), size);
    std::memset(field.bytes, 0, sizeof(field.bytes));
    std::memcpy(field.bytes, data, std::min(size, kFieldBytes));
}

bool Collecting()
{
    return sState && sState->active && sState->options.netplay && sState->options.contacts;
}

// The checksum with every player's node matrices, kept as fields of their own:
// their translation as it is, the whole matrix in the hash.
unsigned WithPoses(unsigned value, Fields& record)
{
    for (int team = 0; team < 2; ++team)
    {
        if (!g_pTeams[team]) continue;
        for (int player = 0; player < 5; ++player)
        {
            const cPlayer* character = g_pTeams[team]->m_pPlayers[player];
            const cPoseAccumulator* pose = character ? character->m_pPoseAccumulator : nullptr;
            if (!pose || !pose->m_NodeMatrices || !pose->m_BaseSHierarchy) continue;
            const int nodes = std::min(pose->GetNumNodes(), 256);
            for (int node = 0; node < nodes; ++node)
            {
                const nlMatrix4& matrix = pose->m_NodeMatrices[node];
                const std::uint32_t hash = Hash(reinterpret_cast<const unsigned char*>(&matrix), sizeof(matrix));
                value = (value << 5 | value >> 27) ^ hash;
                if (record.count == kMaxFields)
                {
                    record.overflowed = true;
                    continue;
                }
                Field& field = record.fields[record.count++];
                field.caller = kPoseField | static_cast<std::uint32_t>(team << 12 | player << 8 | node);
                field.size = sizeof(matrix);
                field.hash = hash;
                std::memset(field.bytes, 0, sizeof(field.bytes));
                std::memcpy(field.bytes, &matrix.e2[3][0], 3 * sizeof(float));
            }
        }
    }
    return value;
}

// The checksum with the step's collisions, which are then forgotten.
unsigned WithCollisions(unsigned value, Fields& record)
{
    for (int i = 0; i < sCollisions.count; ++i)
    {
        const Field& field = sCollisions.fields[i];
        value = (value << 5 | value >> 27) ^ field.hash;
        if (record.count == kMaxFields)
        {
            record.overflowed = true;
            continue;
        }
        record.fields[record.count++] = field;
    }
    record.overflowed |= sCollisions.overflowed;
    sCollisions.count = 0;
    sCollisions.overflowed = false;
    sPairs = sContacts = 0;
    return value;
}

// The checksum with every body of the physics world: the first three
// components of its vectors (ODE pads them to four, and leaves the fourth as it was).
unsigned WithBodies(unsigned value, Fields& record)
{
    if (!g_PhysicsWorld || !g_PhysicsWorld->m_World) return value;
    std::uint32_t index = 0;
    for (const dxBody* body = g_PhysicsWorld->m_World->firstbody; body && index < 0x1000000;
         body = static_cast<const dxBody*>(body->next), ++index)
    {
        float state[22];
        std::memcpy(state, body->pos, 3 * sizeof(float));
        std::memcpy(state + 3, body->q, 4 * sizeof(float));
        std::memcpy(state + 7, body->lvel, 3 * sizeof(float));
        std::memcpy(state + 10, body->avel, 3 * sizeof(float));
        for (int row = 0; row < 3; ++row) std::memcpy(state + 13 + 3 * row, body->R + 4 * row, 3 * sizeof(float));
        const std::uint32_t hash = Hash(reinterpret_cast<const unsigned char*>(state), sizeof(state));
        value = (value << 5 | value >> 27) ^ hash;
        if (record.count == kMaxFields)
        {
            record.overflowed = true;
            continue;
        }
        Field& field = record.fields[record.count++];
        field.caller = kBodyField | index;
        field.size = sizeof(state);
        field.hash = hash;
        std::memset(field.bytes, 0, sizeof(field.bytes));
        std::memcpy(field.bytes, state, 3 * sizeof(float)); // its position
    }
    // The ball and what it hangs from.
    const PhysicsObject* object = g_pBall ? g_pBall->m_pPhysicsBall : nullptr;
    for (std::uint32_t depth = 0; object && depth < 6; object = object->m_parentObject, ++depth)
    {
        float words[13] = {};
        const float* position;
        const float* rotation;
        if (object->m_geomID == 0 && object->m_bodyID != 0)
        {
            words[0] = 2;
            position = dBodyGetPosition(object->m_bodyID);
            rotation = dBodyGetRotation(object->m_bodyID);
        }
        else if (object->m_geomID)
        {
            words[0] = 1;
            position = dGeomGetPosition(object->m_geomID);
            rotation = dGeomGetRotation(object->m_geomID);
        }
        else
            continue;
        std::memcpy(words + 1, position, 3 * sizeof(float));
        for (int row = 0; row < 3; ++row) std::memcpy(words + 4 + 3 * row, rotation + 4 * row, 3 * sizeof(float));
        for (int part = 0; part * 4 < 13; ++part)
        {
            const int count = std::min(4, 13 - part * 4);
            const std::uint32_t hash =
                Hash(reinterpret_cast<const unsigned char*>(words + part * 4), count * sizeof(float));
            value = (value << 5 | value >> 27) ^ hash;
            if (record.count == kMaxFields)
            {
                record.overflowed = true;
                continue;
            }
            Field& field = record.fields[record.count++];
            field.caller = kChainField | depth << 4 | static_cast<std::uint32_t>(part);
            field.size = static_cast<std::uint32_t>(count * sizeof(float));
            field.hash = hash;
            std::memset(field.bytes, 0, sizeof(field.bytes));
            std::memcpy(field.bytes, words + part * 4, count * sizeof(float));
        }
    }
    return value;
}
} // namespace

unsigned RecordedChecksum(unsigned (*checksum)(), unsigned frame)
{
    if (!sState->options.netplay) return checksum();
    if (!sModuleBase) sModuleBase = mscharged::platform::RollbackModuleBase(reinterpret_cast<void*>(&RecordedChecksum));
    Fields& record = sSteps[frame % kRing];
    record.frame = frame;
    record.count = 0;
    record.overflowed = false;
    sRecording = &record;
    unsigned value = checksum();
    sRecording = nullptr;
    if (sState->options.poses) value = WithPoses(value, record);
    if (sState->options.bodies) value = WithBodies(value, record);
    if (sState->options.contacts) value = WithCollisions(value, record);
    return value;
}

void ConfirmFields(unsigned frame)
{
    const Fields& step = sSteps[frame % kRing];
    Fields& kept = sConfirmed[frame % kConfirmedRing];
    kept.count = 0;
    if (step.frame != frame) return;
    kept.frame = frame;
    kept.count = step.count;
    kept.overflowed = step.overflowed;
    std::copy_n(step.fields, step.count, kept.fields);
}

void LogFields(unsigned frame)
{
    const Fields& kept = sConfirmed[frame % kConfirmedRing];
    if (kept.frame != frame || !kept.count)
    {
        Log("checksum fields of frame %u: not kept\n", frame);
        return;
    }
    Log("checksum fields of frame %u, in the order the game takes them (index, where in the game module, size, "
        "first bytes, hash); the first that differs from the other machine's diverged:\n",
        frame);
    for (int i = 0; i < kept.count; ++i)
    {
        const Field& field = kept.fields[i];
        char hex[2 * kFieldBytes + 1] = {};
        const unsigned shown = std::min(field.size, kFieldBytes);
        if ((field.caller & 0xF0000000u) == kPrintField)
        {
            char text[kFieldBytes + 1] = {};
            std::memcpy(text, field.bytes, kFieldBytes);
            for (char& c : text)
                if (c == '\n' || c == '\r') c = ' ';
            Log("  %3d branch +0x%x \"%s%s\" %08x\n", i, field.caller & 0xFFFFFFF, text,
                field.size > kFieldBytes ? "..." : "", field.hash);
            continue;
        }
        if ((field.caller & 0xF0000000u) == kSetField)
        {
            float position[3];
            int type;
            std::memcpy(position, field.bytes, sizeof(position));
            std::memcpy(&type, field.bytes + 12, sizeof(type));
            Log("  %3d ball set +0x%x in %s coordinates: %.9g %.9g %.9g\n", i, field.caller & 0xFFFFFFF,
                type == 0 ? "world" : "the parent's", position[0], position[1], position[2]);
            continue;
        }
        if ((field.caller & 0xF0000000u) == kChainField)
        {
            float words[4];
            std::memcpy(words, field.bytes, sizeof(words));
            Log("  %3d ball chain %u part %u:", i, field.caller >> 4 & 0xF, field.caller & 0xF);
            for (unsigned w = 0; w < field.size / 4; ++w) Log(" %.9g", words[w]);
            Log("\n");
            continue;
        }
        if ((field.caller & 0xF0000000u) == kValuesField)
        {
            Log("  %3d   values:", i);
            for (unsigned v = 0; v < (field.caller & 4 ? 2u : 1u); ++v)
            {
                if (field.caller >> v & 1)
                {
                    double value;
                    std::memcpy(&value, field.bytes + 8 * v, sizeof(value));
                    std::uint64_t bits;
                    std::memcpy(&bits, &value, sizeof(bits));
                    Log(" %.9g (%016llx)", value, static_cast<unsigned long long>(bits));
                }
                else
                {
                    long long value;
                    std::memcpy(&value, field.bytes + 8 * v, sizeof(value));
                    Log(" %lld", value);
                }
            }
            Log("\n");
            continue;
        }
        if ((field.caller & 0xF0000000u) == kPairField)
        {
            int values[3];
            std::memcpy(values, field.bytes, sizeof(values));
            Log("  %3d collision %u: %d contacts, Contact() %d and %d, hash %08x\n", i, field.caller & 0xFFFFFFF,
                values[0], values[1], values[2], field.hash);
            continue;
        }
        if ((field.caller & 0xF0000000u) == kContactField)
        {
            static const char* const kParts[] = {"mode mu mu2 bounce", "bounce_vel soft_erp soft_cfm motion1",
                                                 "motion2 slip1 slip2 pos.x", "pos.y pos.z normal.x normal.y",
                                                 "normal.z depth fdir.x fdir.y", "fdir.z"};
            const unsigned part = field.caller & 0xF;
            float words[4];
            std::memcpy(words, field.bytes, sizeof(words));
            Log("  %3d contact %u (%s):", i, field.caller >> 4 & 0xFFFFFF, kParts[std::min(part, 5u)]);
            for (unsigned w = 0; w < field.size / 4; ++w)
            {
                if (part == 0 && w == 0)
                    Log(" %08x", static_cast<unsigned>(field.bytes[0] | field.bytes[1] << 8 | field.bytes[2] << 16 |
                                                       field.bytes[3] << 24));
                else
                    Log(" %.6f", words[w]);
            }
            Log("\n");
            continue;
        }
        if ((field.caller & 0xF0000000u) == kBodyField)
        {
            float position[3];
            std::memcpy(position, field.bytes, sizeof(position));
            Log("  %3d body %u: position %.6f %.6f %.6f, state %08x\n", i, field.caller & 0xFFFFFFF, position[0],
                position[1], position[2], field.hash);
            continue;
        }
        if ((field.caller & 0xF0000000u) == kPoseField)
        {
            float translation[3];
            std::memcpy(translation, field.bytes, sizeof(translation));
            Log("  %3d pose team %u player %u node %u: translation %.6f %.6f %.6f, matrix %08x\n", i,
                field.caller >> 12 & 0xF, field.caller >> 8 & 0xF, field.caller & 0xFF, translation[0], translation[1],
                translation[2], field.hash);
            continue;
        }
        for (unsigned b = 0; b < shown; ++b) std::snprintf(hex + 2 * b, 3, "%02x", field.bytes[b]);
        Log("  %3d +0x%x %u %s%s %08x\n", i, field.caller, field.size, hex, field.size > kFieldBytes ? "..." : "",
            field.hash);
    }
    if (kept.overflowed) Log("  and more than %d blocks: the rest is not kept\n", kMaxFields);
}
} // namespace mscharged::rollback

extern "C" void mscharged_rollback_collision(int contacts, int result1, int result2)
{
    using namespace mscharged::rollback;
    if (!Collecting()) return;
    const int values[3] = {contacts, result1, result2};
    AddCollisionField(kPairField | (sPairs++ & 0xFFFFFFF), values, sizeof(values));
}

extern "C" void mscharged_rollback_contact(const void* contact)
{
    using namespace mscharged::rollback;
    if (!Collecting()) return;
    const auto& c = *static_cast<const dContact*>(contact);
    float words[kContactWords];
    std::memcpy(&words[0], &c.surface.mode, sizeof(float));
    const float rest[] = {c.surface.mu, c.surface.mu2, c.surface.bounce, c.surface.bounce_vel, c.surface.soft_erp,
                          c.surface.soft_cfm, c.surface.motion1, c.surface.motion2, c.surface.slip1, c.surface.slip2,
                          c.geom.pos[0], c.geom.pos[1], c.geom.pos[2], c.geom.normal[0], c.geom.normal[1],
                          c.geom.normal[2], c.geom.depth, c.fdir1[0], c.fdir1[1], c.fdir1[2]};
    std::memcpy(&words[1], rest, sizeof(rest));
    const std::uint32_t index = sContacts++ & 0xFFFFFF;
    for (int part = 0; part * 4 < kContactWords; ++part)
    {
        const int count = std::min(4, kContactWords - part * 4);
        AddCollisionField(kContactField | index << 4 | static_cast<std::uint32_t>(part), &words[part * 4],
                          static_cast<unsigned>(count * sizeof(float)));
    }
}

extern "C" void mscharged_rollback_debug_print(const char* format, va_list arguments, const void* caller)
{
    using namespace mscharged::rollback;
    if (!Collecting() || !format) return;
    if (!sModuleBase) sModuleBase = mscharged::platform::RollbackModuleBase(reinterpret_cast<void*>(&RecordedChecksum));
    const std::uint32_t where = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(caller) - 1 - sModuleBase);
    AddCollisionField(kPrintField | (where & 0xFFFFFFF), format, static_cast<unsigned>(std::strlen(format)));

    // The numbers, by the conversions of the format; strings and pointers
    // (which differ between machines) are read past.
    va_list list;
    va_copy(list, arguments);
    unsigned char pair[16];
    std::uint32_t types = 0;
    int held = 0;
    const auto add = [&](const void* value, bool isDouble) {
        std::memcpy(pair + 8 * held, value, 8);
        types |= (isDouble ? 1u : 0u) << held;
        if (++held == 2)
        {
            AddCollisionField(kValuesField | types | 4, pair, sizeof(pair));
            held = 0;
            types = 0;
        }
    };
    for (const char* p = format; *p; ++p)
    {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        while (*p && std::strchr("-+ #0", *p)) ++p;
        while (*p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == '*')) ++p;
        int longs = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z')
        {
            if (*p == 'l') ++longs;
            ++p;
        }
        if (!*p) break;
        switch (*p)
        {
        case 'd': case 'i':
        {
            const long long value = longs >= 2 ? va_arg(list, long long) : longs ? va_arg(list, long) : va_arg(list, int);
            add(&value, false);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'c':
        {
            const long long value = longs >= 2 ? static_cast<long long>(va_arg(list, unsigned long long))
                                    : longs   ? static_cast<long long>(va_arg(list, unsigned long))
                                              : static_cast<long long>(va_arg(list, unsigned int));
            add(&value, false);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        {
            const double value = va_arg(list, double);
            add(&value, true);
            break;
        }
        case 's': case 'p':
            (void)va_arg(list, void*);
            break;
        default:
            break;
        }
    }
    if (held) AddCollisionField(kValuesField | types, pair, 8);
    va_end(list);
}

extern "C" void mscharged_rollback_object_position(const void* object, const float* position, int type,
                                                   const void* caller)
{
    using namespace mscharged::rollback;
    if (!Collecting() || !g_pBall || object != g_pBall->m_pPhysicsBall) return;
    if (!sModuleBase) sModuleBase = mscharged::platform::RollbackModuleBase(reinterpret_cast<void*>(&RecordedChecksum));
    const std::uint32_t where = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(caller) - 1 - sModuleBase);
    unsigned char data[16];
    std::memcpy(data, position, 3 * sizeof(float));
    std::memcpy(data + 12, &type, sizeof(type));
    AddCollisionField(kSetField | (where & 0xFFFFFFF), data, sizeof(data));
}

extern "C" void mscharged_rollback_checksum_field(const void* data, unsigned long size, const void* caller)
{
    using namespace mscharged::rollback;
    Fields* record = sRecording;
    if (!record) return;
    if (record->count == kMaxFields)
    {
        record->overflowed = true;
        return;
    }
    Field& field = record->fields[record->count++];
    const auto* bytes = static_cast<const unsigned char*>(data);
    // The return address minus one: inside the call, on its line.
    field.caller = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(caller) - 1 - sModuleBase);
    field.size = static_cast<std::uint32_t>(size);
    field.hash = Hash(bytes, size);
    std::memset(field.bytes, 0, sizeof(field.bytes));
    std::memcpy(field.bytes, bytes, std::min<unsigned long>(size, kFieldBytes));
}

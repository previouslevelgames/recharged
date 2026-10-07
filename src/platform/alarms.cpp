#include "platform/alarms.h"
#include "platform/interrupts.h"
#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {
struct CriticalSection {
    BOOL enabled{OSDisableInterrupts()};
    ~CriticalSection() { OSRestoreInterrupts(enabled); }
};
struct Device {
    std::thread::id owner;
    OSAlarm* head{};
    OSAlarm* tail{};
    bool initialized{};
    bool active{};
};
Device& State() { static Device state; return state; }
void RequireOwner(const Device& s) {
    if (!s.initialized || s.owner != std::this_thread::get_id())
        throw std::logic_error("Native alarm API has no matching live owner");
}
void RequirePointer(const OSAlarm* p) {
    if (!p) throw std::invalid_argument("Native alarm descriptor is null");
}
// Explicit PPC64 two's-complement addition/multiplication at hardware tick
// boundaries. No saturation or host signed-overflow optimization is applied.
OSTime Add(OSTime a, OSTime b) {
    return std::bit_cast<OSTime>(std::uint64_t(a) + std::uint64_t(b));
}
OSTime Sub(OSTime a, OSTime b) {
    return std::bit_cast<OSTime>(std::uint64_t(a) - std::uint64_t(b));
}
OSTime Mul(OSTime a, OSTime b) {
    return std::bit_cast<OSTime>(std::uint64_t(a) * std::uint64_t(b));
}
void Unlink(Device& s, OSAlarm* alarm) {
    if (alarm->next) alarm->next->prev = alarm->prev;
    else s.tail = alarm->prev;
    if (alarm->prev) alarm->prev->next = alarm->next;
    else s.head = alarm->next;
}
void Insert(Device& s, OSAlarm* alarm, OSTime end, OSAlarmHandler handler, OSTime now) {
    // Same source InsertAlarm: at most one current deadline, anchored to the
    // requested start, skipping elapsed periodic occurrences rather than
    // inventing a callback backlog. Equal-time requests keep insertion order.
    if (alarm->period > 0) {
        end = alarm->start;
        if (alarm->start < now) {
            const auto delta = Sub(now, alarm->start);
            end = Add(end, Mul(alarm->period, Add(delta / alarm->period, 1)));
        }
    }
    // Duplicate/null requests violate the original SDK debug assertions.
    // Reject explicitly rather than corrupting an existing borrowed queue.
    if (alarm->handler || !handler)
        throw std::invalid_argument("Native alarm request has an occupied or null handler");
    alarm->handler = handler;
    alarm->fire = end;
    for (auto* item = s.head; item; item = item->next) {
        if (item->fire <= end) continue;
        alarm->prev = item->prev;
        item->prev = alarm;
        alarm->next = item;
        if (alarm->prev) alarm->prev->next = alarm;
        else s.head = alarm;
        return;
    }
    alarm->next = nullptr;
    alarm->prev = s.tail;
    s.tail = alarm;
    if (alarm->prev) alarm->prev->next = alarm;
    else s.head = alarm;
}
struct Delivery { OSAlarm* alarm; OSAlarmHandler handler; OSContext* interrupted; };
void Invoke(void* data) {
    auto& delivery = *static_cast<Delivery*>(data);
    delivery.handler(delivery.alarm, delivery.interrupted);
}
}

namespace mscharged::platform {
void InitializeNativeAlarms() {
    NativeInterruptGuard exclusion;
    auto& s = State();
    if (s.initialized) { RequireOwner(s); return; }
    if (!OSGetArenaLo() || !OSGetArenaHi() || !OSGetCurrentContext())
        throw std::logic_error("Native alarms require actual live SDK memory/context");
    s.owner = std::this_thread::get_id();
    s.initialized = true;
    s.head = s.tail = nullptr;
    s.active = false;
}
void ShutdownNativeAlarms() {
    NativeInterruptGuard exclusion;
    auto& s = State();
    if (!s.initialized) return;
    RequireOwner(s);
    if (s.active) throw std::logic_error("Cannot retire an active native alarm callback");
    // Device retirement cancels borrowed queue references, with no invented
    // completion/cancellation callback or source object destruction.
    for (auto* alarm = s.head; alarm;) {
        auto* next = alarm->next;
        alarm->handler = nullptr;
        alarm->prev = alarm->next = nullptr;
        alarm = next;
    }
    s.head = s.tail = nullptr;
    s.initialized = false;
    s.owner = {};
}
std::size_t ServiceNativeAlarms() {
    NativeInterruptRead exclusion;
    if (!exclusion) return 0;
    auto& s = State();
    if (!s.initialized || s.owner != std::this_thread::get_id() || s.active ||
        !NativeInterruptsEnabled() || !s.head) return 0;
    s.active = true;
    struct Finish { Device& s; ~Finish() { s.active = false; } } finish{s};
    // OSGetTime is the actual source-visible SDK Wii hardware clock.
    // AURORA_WII_CLOCK continues advancing when presentation time is paused.
    // Its nested owner hook observes active=true and cannot reenter this queue.
    OSTime now;
    {
        // The retail decrementer exception reads time with IRQs masked.
        // A native SDK clock safe point must not run another device callback
        // which cancels this borrowed queue while the deadline is sampled.
        CriticalSection clock_read;
        now = OSGetTime();
    }
    auto* alarm = s.head;
    if (now < alarm->fire) return 0;
    const auto handler = alarm->handler;
    if (!handler) throw std::logic_error("Queued native alarm has no source handler");
    Unlink(s, alarm);
    alarm->handler = nullptr;
    if (alarm->period > 0) Insert(s, alarm, 0, handler, now);
    // This helper uses the existing real exclusion/mask/current OS context
    // machinery. It restores those even if a native C++ callback throws.
    Delivery delivery{alarm, handler, OSGetCurrentContext()};
    if (!DispatchNativeInterrupt(Invoke, &delivery))
        throw std::logic_error("Actual native alarm delivery lost its owner exclusion");
    // Do not dereference alarm after delivery: the actual source callback may
    // cancel/rearm it or release a completed one-shot's descriptor storage.
    return 1;
}
}

extern "C" void OSInitAlarm() { mscharged::platform::InitializeNativeAlarms(); }
extern "C" void __OSInitAlarm() { OSInitAlarm(); }
extern "C" void OSCreateAlarm(OSAlarm* alarm) {
    CriticalSection exclusion;
    RequireOwner(State()); RequirePointer(alarm);
    // Source SDK initializes only these two fields, preserving the others.
    alarm->handler = nullptr;
    alarm->tag = 0;
}
extern "C" void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    CriticalSection exclusion;
    auto& s = State(); RequireOwner(s); RequirePointer(alarm);
    const auto now = OSGetTime();
    alarm->period = 0;
    Insert(s, alarm, Add(now, tick), handler, now);
}
extern "C" void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) {
    CriticalSection exclusion;
    auto& s = State(); RequireOwner(s); RequirePointer(alarm);
    alarm->period = 0;
    Insert(s, alarm, time, handler, OSGetTime());
}
extern "C" void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    CriticalSection exclusion;
    auto& s = State(); RequireOwner(s); RequirePointer(alarm);
    alarm->period = period;
    alarm->start = start; // Same source-visible OSticks epoch, not a relative delay.
    Insert(s, alarm, 0, handler, OSGetTime());
}
extern "C" void OSCancelAlarm(OSAlarm* alarm) {
    CriticalSection exclusion;
    auto& s = State(); RequireOwner(s); RequirePointer(alarm);
    if (!alarm->handler) return;
    Unlink(s, alarm);
    alarm->handler = nullptr;
}
extern "C" void OSSetAlarmTag(OSAlarm* alarm, u32 tag) {
    CriticalSection exclusion;
    RequireOwner(State()); RequirePointer(alarm); alarm->tag = tag;
}
extern "C" void OSCancelAlarms(u32 tag) {
    CriticalSection exclusion;
    auto& s = State(); RequireOwner(s);
    // Canonical SDK retains the system tag zero; its batch cancellation is a no-op.
    if (!tag) return;
    for (auto* alarm = s.head; alarm;) {
        auto* next = alarm->next;
        if (alarm->tag == tag) OSCancelAlarm(alarm);
        alarm = next;
    }
}
extern "C" void OSSetAlarmUserData(OSAlarm* alarm, void* data) {
    CriticalSection exclusion;
    RequireOwner(State()); RequirePointer(alarm); alarm->userData = data;
}
extern "C" void* OSGetAlarmUserData(const OSAlarm* alarm) {
    CriticalSection exclusion;
    RequireOwner(State()); RequirePointer(alarm); return alarm->userData;
}

extern "C" BOOL OSCheckAlarmQueue() {
    CriticalSection exclusion;
    auto& s = State(); RequireOwner(s);
    // Canonical SDK debug check validates intrusive links, not queue emptiness.
    if ((s.head == nullptr) != (s.tail == nullptr)) return FALSE;
    if (s.head && s.head->prev) return FALSE;
    if (s.tail && s.tail->next) return FALSE;
    for (auto* alarm = s.head; alarm; alarm = alarm->next) {
        if (alarm->next && alarm->next->prev != alarm) return FALSE;
        if (!alarm->next && s.tail != alarm) return FALSE;
    }
    return TRUE;
}

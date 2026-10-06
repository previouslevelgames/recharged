#include "platform/stm_device.h"
#include "platform/interrupts.h"
#include <revolution/os.h>
#include <revolution/ipc.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

using namespace mscharged::platform;
namespace {
unsigned checks{}, first{}, second{};
std::thread::id owner;
OSContext* original_context;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void B() {
    ++second;
    Check(std::this_thread::get_id() == owner, "worker called source power callback");
    Check(!NativeInterruptsEnabled(), "source callback lost original interrupt mask");
    Check(OSGetCurrentContext() != original_context, "no genuine interrupt context");
}
void A() {
    ++first;
    B(); --second;
    bool rejected = false;
    try { ShutdownNativeSTMDevice(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "active source callback was retired");
    Check(!ServiceNativeSTMDevice(), "source power callback reentered hardware delivery");
}
void Reinstall() { B(); Check(OSSetPowerCallback(A) != nullptr, "source default callback disappeared"); }
}
int main() {
    try {
        owner = std::this_thread::get_id(); original_context = OSGetCurrentContext();
        InitializeNativeSTMDevice(); auto input = GetNativeSTMInput();
        Check(__OSInitSTM(), "original STM initialization did not open actual endpoints");
        Check(OSSetPowerCallback(A) != nullptr, "original default predecessor absent");
        Check(OSSetPowerCallback(B) == A, "source predecessor replacement changed");
        Check(__OSInitSTM(), "source initialized branch lost real device readiness");
        Check(OSSetPowerCallback(A) != B, "source repeated-init callback reset was repaired");
        Check(IOS_Open("/dev/unknown", IPC_OPEN_NONE) == IPC_RESULT_NOEXISTS, "unsupported device reported success");
        Check(IOS_Open("/dev/stm/immediate", IPC_OPEN_NONE) == IPC_RESULT_OPENFD, "duplicate endpoint open replaced owner");
        Check(IOS_Close(-1) == IPC_RESULT_INVALID, "invalid descriptor close reported success");
        Check(!ServiceNativeSTMDevice(), "absent hardware event invented callback");
        std::atomic<bool> submitted{}, forbidden{};
        std::thread worker([&] { submitted = SubmitNativeSTMPower(input); try { ServiceNativeSTMDevice(); } catch(const std::logic_error&) { forbidden = true; } });
        worker.join(); Check(submitted && forbidden, "worker input/owner boundary failed");
        Check(!first && !second, "worker executed original callback");
        const BOOL enabled = OSDisableInterrupts();
        Check(!ServiceNativeSTMDevice() && !first, "masked source event was consumed");
        OSRestoreInterrupts(enabled); Check(ServiceNativeSTMDevice() && first == 1, "actual source power callback not delivered");
        Check(NativeInterruptsEnabled() && OSGetCurrentContext() == original_context, "owner context/mask not restored");
        Check(SubmitNativeSTMPower(input), "next genuine event not queued");
        Check(!ServiceNativeSTMDevice(), "source one-shot callback auto-reregistered");
        Check(!OSGetResetButtonState(), "power event invented reset pulse");
        Check(ServiceNativeSTMDevice() && first == 1 && !second, "original default power callback lost its one-shot quirk");
        OSSetPowerCallback(Reinstall); Check(SubmitNativeSTMPower(input) && ServiceNativeSTMDevice() && second == 1, "source callback did not execute");
        Check(SubmitNativeSTMPower(input) && ServiceNativeSTMDevice() && first == 2, "source reentrant registration lost successor");
        Check(!OSGetResetButtonState(), "empty source reset read was true");
        Check(SetNativeSTMResetButton(input, true) && ServiceNativeSTMDevice(), "actual held reset input not delivered");
        Check(OSGetResetButtonState(), "original reset pulse absent");
        Check(!OSGetResetButtonState(), "original reset read did not consume pulse while physically held");
        Check(SetNativeSTMResetButton(input, true) && !ServiceNativeSTMDevice(), "same held level invented duplicate edge");
        Check(SetNativeSTMResetButton(input, false), "actual reset release failed");
        Check(SetNativeSTMResetButton(input, true) && SetNativeSTMResetButton(input, false) && ServiceNativeSTMDevice(), "brief physical reset event not serviced");
        Check(!OSGetResetButtonState(), "source raw released-state test was changed");
        Check(__OSUnRegisterStateEvent() == 0, "source unregister failed actual endpoint cancellation");
        Check(SubmitNativeSTMPower(input) && !ServiceNativeSTMDevice(), "unregistered source callback executed");
        OSSetPowerCallback(B); Check(ServiceNativeSTMDevice() && second == 2, "original setter reregistration lost pending physical event");
        Check(__OSUnRegisterStateEvent() == 0, "already unregistered source state changed");
        Check(!ServiceNativeSTMDevice(), "drained device fabricated pending input");
        ShutdownNativeSTMDevice(); Check(!SubmitNativeSTMPower(input), "retired device accepted stale input");
        InitializeNativeSTMDevice(); auto next = GetNativeSTMInput();
        Check(next.generation != input.generation && !SetNativeSTMResetButton(input, true), "device reload reused stale hardware identity");
        // Original OSStateTM static StmReady is still true. No source restart or
        // full module/CRT lifecycle is accepted by a backend-only restart.
        Check(!ServiceNativeSTMDevice(), "fresh host device retained borrowed source callback");
        ShutdownNativeSTMDevice();
        std::printf("Original STM device prerequisite: %u checks; no original main/reset shutdown/VI/CRT readiness\n", checks);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"STM fixture failed after %u checks: %s\n", checks,e.what()); return 1; }
}

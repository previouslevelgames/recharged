#include "platform/wpad_sdl.h"
#include "platform/interrupts.h"

#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
constexpr std::array<u16, 11> kRemoteButtons{
    WPAD_BUTTON_A, WPAD_BUTTON_B, WPAD_BUTTON_1, WPAD_BUTTON_2,
    WPAD_BUTTON_PLUS, WPAD_BUTTON_MINUS, WPAD_BUTTON_HOME,
    WPAD_BUTTON_UP, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT};
constexpr float kGravity = 9.80665f;
constexpr std::size_t kPendingReports = 16;
struct Channel {
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID id = 0;
    u32 format = WPAD_FMT_CORE_BTN;
    WPADConnectCallback* connect = nullptr;
    WPADExtensionCallback* extension = nullptr;
    WPADSamplingCallback sampling = nullptr;
    u16 buttons = 0;
    std::array<WPADStatus, kPendingReports> pending{};
    std::size_t head = 0, count = 0;
    WPADStatus current{};
    u32 dpd_command = WPAD_DPD_DISABLE;
    u32 dpd_pending_command = WPAD_DPD_DISABLE;
    bool dpd_pending = false;
    WPADCallback dpd_completion = nullptr;
    WPADResult dpd_result = WPAD_ERR_OK;
    bool announced = false;
    bool pending_disconnect = false;
    bool disconnected_this_service = false;
    bool motor_running = false;
};
struct DpdProducer {
    mscharged::platform::NativeDpdSource source{};
    mscharged::platform::NativeDpdObservation observation{};
};
struct Hardware {
    std::mutex reports;
    std::array<Channel, WPAD_MAX_CONTROLLERS> channels{};
    std::thread::id owner{};
    mscharged::platform::WpadSDLSettings settings{};
    WPADAllocFunc alloc = nullptr;
    WPADFreeFunc free = nullptr;
    bool configured = false, initialized = false, servicing = false;
    bool motor_enabled = false;
    std::uint64_t generation = 0;
    std::array<DpdProducer, WPAD_MAX_CONTROLLERS> dpd_producers{};
    std::thread::id dpd_owner{};
    std::uint64_t dpd_generation = 0;
};
Hardware& State() {
    static Hardware hardware;
    return hardware;
}
void RequireOwner() {
    auto& state = State();
    if (state.initialized && state.owner != std::this_thread::get_id())
        throw std::logic_error("WPAD hardware requires its SDL owner thread");
}
Channel& GetChannel(s32 channel) {
    RequireOwner();
    if (channel < 0 || channel >= WPAD_MAX_CONTROLLERS)
        throw std::out_of_range("WPAD channel outside Wii hardware ports");
    return State().channels[channel];
}
DpdProducer* FindDpdProducer(SDL_JoystickID id) {
    if (!id) return nullptr;
    for (auto& producer : State().dpd_producers)
        if (producer.source.joystick_id == id) return &producer;
    return nullptr;
}
void RequireDpdOwner() {
    RequireOwner();
    if (State().dpd_owner != std::thread::id{} && State().dpd_owner != std::this_thread::get_id())
        throw std::logic_error("DPD observations require their native SDL owner");
}
DpdProducer& RequireDpdProducer(mscharged::platform::NativeDpdSource source) {
    auto* producer = FindDpdProducer(source.joystick_id);
    if (!source.generation || !producer || producer->source.generation != source.generation)
        throw std::invalid_argument("DPD observation source is retired or foreign");
    return *producer;
}
void CopyDpdObservation(Channel& channel, WPADStatus& report) {
    const auto* producer = FindDpdProducer(channel.id);
    if (!producer || channel.dpd_command == WPAD_DPD_DISABLE) return;
    for (std::size_t n = 0; n < producer->observation.size(); ++n) {
        const auto& object = producer->observation[n];
        report.obj[n] = {object.x, object.y, object.size, object.trace_id};
    }
}
void ClearReports(Channel& channel) {
    channel.id = 0;
    channel.buttons = 0;
    channel.motor_running = false;
    channel.head = channel.count = 0;
    channel.dpd_command = WPAD_DPD_DISABLE;
    channel.dpd_pending_command = WPAD_DPD_DISABLE;
    channel.dpd_pending = false;
    channel.dpd_completion = nullptr;
    channel.current = {};
    channel.current.err = WPAD_ERR_NO_CONTROLLER;
    channel.current.dev = WPAD_DEV_NOT_FOUND;
}
bool RawAccel(float value, bool invert, s16& output) {
    // SDL's pinned Wii HID driver uses a fixed 100 counts/g and zero-point
    // 0x200, emitting (-rawX, rawZ, rawY) in SI units. Reverse that transport;
    // leave original KPAD filtering, clipping, orientation and glitches intact.
    if (!std::isfinite(value)) return false;
    double counts = (invert ? -double(value) : double(value)) * 100.0 / double(kGravity);
    if (counts < -512.5 || counts >= 511.5) return false;
    auto word = std::llround(counts);
    if (word < -512 || word > 511) return false;
    output = static_cast<s16>(word);
    return true;
}
bool SDLCALL Watch(void*, SDL_Event* event) {
    auto& state = State();
    std::lock_guard lock(state.reports);
    SDL_JoystickID id = 0;
    if (event->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || event->type == SDL_EVENT_JOYSTICK_BUTTON_UP)
        id = event->jbutton.which;
    else if (event->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE)
        id = event->gsensor.which;
    else return true;
    for (auto& channel : state.channels) {
        if (!channel.id || channel.id != id) continue;
        if (event->type != SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
            int raw_index = int(event->jbutton.button) - int(SDL_GAMEPAD_BUTTON_MISC1);
            if (raw_index >= 0 && raw_index < int(kRemoteButtons.size())) {
                u16 mask = kRemoteButtons[raw_index];
                if (event->jbutton.down) channel.buttons |= mask;
                else channel.buttons &= ~mask;
            }
            return true;
        }
        if (event->gsensor.sensor != SDL_SENSOR_ACCEL) return true;
        WPADStatus report{};
        report.dev = WPAD_DEV_CORE;
        report.err = WPAD_ERR_OK;
        report.button = channel.buttons;
        CopyDpdObservation(channel, report);
        // Retail WPADiExcludeButton removes opposite right/down bits.
        if ((report.button & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) ==
                (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) report.button &= ~WPAD_BUTTON_RIGHT;
        if ((report.button & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN)) ==
                (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN)) report.button &= ~WPAD_BUTTON_DOWN;
        if (!RawAccel(event->gsensor.data[0], true, report.accX) ||
            !RawAccel(event->gsensor.data[2], false, report.accY) ||
            !RawAccel(event->gsensor.data[1], false, report.accZ))
            report.err = WPAD_ERR_CORRUPTED;
        if (channel.count == kPendingReports) {
            channel.head = (channel.head + 1) % kPendingReports;
            --channel.count;
        }
        channel.pending[(channel.head + channel.count) % kPendingReports] = report;
        ++channel.count;
        return true;
    }
    return true;
}
bool SupportedRemote(SDL_JoystickID id) {
    if (!State().settings.physical_wii_remotes && !SDL_IsJoystickVirtual(id)) return false;
    // Mapping names may be cached by GUID across devices. Use the actual
    // underlying joystick identity provided by the pinned Wii HID driver.
    const char* name = SDL_GetJoystickNameForID(id);
    const auto vendor = SDL_GetGamepadVendorForID(id), product = SDL_GetGamepadProductForID(id);
    return name && std::strcmp(name, "Nintendo Wii Remote") == 0 && vendor == 0x057e &&
        (product == 0x0306 || product == 0x0330);
}
bool DispatchDpdCompletion(Channel& channel, s32 index) {
    struct Completion { Channel* channel; s32 index; } completion{&channel,index};
    return mscharged::platform::DispatchNativeInterrupt([](void* opaque) {
        auto& value = *static_cast<Completion*>(opaque);
        WPADCallback callback;
        WPADResult result;
        {
            std::lock_guard lock(State().reports);
            callback = value.channel->dpd_completion;
            result = value.channel->dpd_result;
            // Original __dpdCb commits currentDpdCommand/info.dpd only after
            // the camera-register operation, including NULL user callbacks.
            if (result == WPAD_ERR_OK)
                value.channel->dpd_command = value.channel->dpd_pending_command;
            value.channel->dpd_pending = false;
            value.channel->dpd_completion = nullptr;
        }
        if (callback) callback(value.index, result);
    }, &completion);
}
bool DispatchConnect(Channel& channel, s32 index, WPADResult result) {
    struct Call { WPADConnectCallback* callback; s32 channel; WPADResult result; } call{channel.connect, index, result};
    return mscharged::platform::DispatchNativeInterrupt([](void* opaque) {
        auto& value = *static_cast<Call*>(opaque);
        value.callback(value.channel, value.result);
    }, &call);
}
}

namespace mscharged::platform {
NativeDpdSource AttachNativeWpadDpdSource(std::uint32_t joystick_id) {
    RequireDpdOwner();
    if (!joystick_id || !SDL_GetJoystickFromID(joystick_id) || !SDL_IsJoystickVirtual(joystick_id))
        throw std::invalid_argument("DPD source must own an open explicit SDL virtual device");
    auto& state = State();
    std::lock_guard lock(state.reports);
    if (FindDpdProducer(joystick_id)) throw std::logic_error("DPD source is already attached");
    DpdProducer* slot = nullptr;
    for (auto& producer : state.dpd_producers) if (!producer.source.generation) { slot = &producer; break; }
    if (!slot) throw std::length_error("Native DPD source capacity is exhausted");
    if (state.dpd_generation == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("DPD source incarnation exhausted");
    *slot = {};
    slot->source = {joystick_id, ++state.dpd_generation};
    state.dpd_owner = std::this_thread::get_id();
    return slot->source;
}
void SubmitNativeWpadDpdObservation(NativeDpdSource source, const NativeDpdObservation& observation) {
    RequireDpdOwner();
    // These are post-parser WPAD words, not inferred game coordinates. Raw
    // invalid objects use zero size; every observed object fits the real sensor.
    for (const auto& object : observation)
        if (object.size && (object.x < 0 || object.x >= WPAD_MAX_DPD_X ||
                            object.y < 0 || object.y >= WPAD_MAX_DPD_Y))
            throw std::invalid_argument("DPD observation exceeds its raw sensor domain");
    // Query SDL before taking the report mutex. SDL sensor watchers acquire
    // that mutex while latching reports; no source callbacks run here.
    SDL_Joystick* joystick = SDL_GetJoystickFromID(source.joystick_id);
    if (!joystick || !SDL_JoystickConnected(joystick))
        throw std::invalid_argument("DPD source SDL device is no longer live");
    auto& state = State();
    std::lock_guard lock(state.reports);
    RequireDpdProducer(source).observation = observation;
}
void DetachNativeWpadDpdSource(NativeDpdSource source) {
    RequireDpdOwner();
    auto& state = State();
    std::lock_guard lock(state.reports);
    RequireDpdProducer(source) = {};
    for (auto& channel : state.channels) if (channel.id == source.joystick_id) {
        channel.dpd_command = WPAD_DPD_DISABLE;
        channel.dpd_result = WPAD_ERR_INVALID;
        for (auto& object : channel.current.obj) object = {};
        for (auto& report : channel.pending) for (auto& object : report.obj) object = {};
    }
    bool any = false;
    for (const auto& producer : state.dpd_producers) any |= producer.source.generation != 0;
    if (!any) state.dpd_owner = {};
}
void ConfigureWpadSDL(WpadSDLSettings settings) {
    auto& state = State();
    if (state.initialized) throw std::logic_error("WPAD system preferences must precede initialization");
    if (settings.sensor_bar_position > 1 || settings.dpd_sensitivity < 1 || settings.dpd_sensitivity > 5)
        throw std::invalid_argument("Invalid Wii sensor-bar or DPD sensitivity preference");
    state.settings = settings;
    state.configured = true;
}
void ServiceWpadSDL() {
    auto& state = State();
    RequireOwner();
    if (!state.initialized || state.servicing) return;
    if (!NativeInterruptsEnabled()) return;
    struct ServiceGuard {
        bool& active;
        explicit ServiceGuard(bool& flag) : active(flag) { active = true; }
        ~ServiceGuard() { active = false; }
    } guard(state.servicing);
    const auto generation = state.generation;
    SDL_UpdateGamepads();
    for (s32 index = 0; index < WPAD_MAX_CONTROLLERS; ++index) {
        auto& channel = state.channels[index];
        channel.disconnected_this_service = false;
        if (channel.pad && !SDL_GamepadConnected(channel.pad)) {
            // Original WPAD completes pending commands with NO_CONTROLLER
            // before clearing its control block and calling connectCB.
            if (channel.dpd_pending) {
                { std::lock_guard lock(state.reports); channel.dpd_result = WPAD_ERR_NO_CONTROLLER; }
                if (!DispatchDpdCompletion(channel, index)) return;
                if (!state.initialized || state.generation != generation) return;
            }
            SDL_CloseGamepad(channel.pad);
            channel.pad = nullptr;
            {
                std::lock_guard lock(state.reports);
                ClearReports(channel);
            }
            channel.disconnected_this_service = true;
            channel.pending_disconnect = channel.announced;
            if (!state.initialized || state.generation != generation) return;
        }
        if (channel.pending_disconnect) {
            if (channel.connect && !DispatchConnect(channel, index, WPAD_ERR_NO_CONTROLLER)) return;
            channel.announced = false;
            channel.pending_disconnect = false;
            channel.disconnected_this_service = true;
            if (!state.initialized || state.generation != generation) return;
        }
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) throw std::runtime_error(SDL_GetError());
    struct IDs { SDL_JoystickID* data; ~IDs() { SDL_free(data); } } owned_ids{ids};
    for (int n = 0; n < count; ++n) {
        bool assigned = false;
        for (const auto& channel : state.channels) assigned |= channel.id == ids[n];
        if (assigned || !SupportedRemote(ids[n])) continue;
        Channel* slot = nullptr;
        for (auto& channel : state.channels) if (!channel.pad && !channel.disconnected_this_service && !channel.pending_disconnect) { slot = &channel; break; }
        if (!slot) break;
        SDL_Gamepad* pad = SDL_OpenGamepad(ids[n]);
        if (!pad) continue;
        SDL_Joystick* joystick = SDL_GetGamepadJoystick(pad);
        if (SDL_GetNumJoystickButtons(joystick) < int(SDL_GAMEPAD_BUTTON_MISC1) + int(kRemoteButtons.size()) ||
                !SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) { SDL_CloseGamepad(pad); continue; }
        u16 initial_buttons = 0;
        for (int button = 0; button < int(kRemoteButtons.size()); ++button)
            if (SDL_GetJoystickButton(joystick, int(SDL_GAMEPAD_BUTTON_MISC1) + button)) initial_buttons |= kRemoteButtons[button];
        {
            std::lock_guard lock(state.reports);
            ClearReports(*slot);
            slot->id = ids[n];
            slot->buttons = initial_buttons;
            slot->current.dev = WPAD_DEV_CORE;
            slot->current.err = WPAD_ERR_COMMUNICATION_ERROR;
        }
        if (!SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true)) {
            {
                std::lock_guard lock(state.reports);
                ClearReports(*slot);
            }
            SDL_CloseGamepad(pad);
            continue;
        }
        slot->pad = pad;
    }
    for (s32 index = 0; index < WPAD_MAX_CONTROLLERS; ++index) {
        auto& channel = state.channels[index];
        if (!channel.pad) continue;
        // A virtual camera register write completes on the servicing SDL owner,
        // under the same actual interrupt exclusion as sampling/connect events.
        if (channel.dpd_pending) {
            if (!DispatchDpdCompletion(channel, index)) return;
            if (!state.initialized || state.generation != generation) return;
        }
        if (!channel.announced && channel.connect) {
            if (!DispatchConnect(channel, index, WPAD_ERR_OK)) return;
            if (!state.initialized || state.generation != generation) return;
            channel.announced = true;
        }
        while (channel.sampling) {
            {
                std::lock_guard lock(state.reports);
                if (!channel.count) break;
            }
            struct Sample { Channel* state; WPADSamplingCallback callback; s32 channel; } sample{&channel, channel.sampling, index};
            if (!DispatchNativeInterrupt([](void* opaque) {
                auto& value = *static_cast<Sample*>(opaque);
                {
                    std::lock_guard lock(State().reports);
                    value.state->current = value.state->pending[value.state->head];
                    value.state->head = (value.state->head + 1) % kPendingReports;
                    --value.state->count;
                }
                value.callback(value.channel);
            }, &sample)) return;
            if (!state.initialized || state.generation != generation) return;
        }
        if (!channel.sampling) {
            std::lock_guard lock(state.reports);
            if (channel.count) {
                channel.current = channel.pending[(channel.head + channel.count - 1) % kPendingReports];
                channel.head = channel.count = 0;
            }
        }
    }
}
std::size_t WpadSDLConnectedChannels() {
    RequireOwner();
    std::size_t count = 0;
    for (auto& channel : State().channels) count += channel.pad != nullptr;
    return count;
}
}

extern "C" {
void WPADRegisterAllocator(WPADAllocFunc allocate, WPADFreeFunc release) {
    RequireOwner();
    State().alloc = allocate;
    State().free = release;
}
void WPADInit() {
    auto& state = State();
    RequireOwner();
    if (state.initialized) return;
    if (!state.configured) throw std::logic_error("WPAD host system preferences are missing");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError());
    state.owner = std::this_thread::get_id();
    if (!SDL_AddEventWatch(Watch, nullptr)) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        throw std::runtime_error(SDL_GetError());
    }
    for (auto& channel : state.channels) ClearReports(channel);
    state.motor_enabled = state.settings.motor_enabled;
    ++state.generation;
    state.initialized = true;
}
void WPADShutdown() {
    RequireOwner();
    auto& state = State();
    if (!state.initialized) return;
    SDL_RemoveEventWatch(Watch, nullptr);
    for (auto& channel : state.channels) {
        if (channel.pad) {
            SDL_RumbleGamepad(channel.pad, 0, 0, 0);
            SDL_CloseGamepad(channel.pad);
        }
        channel = Channel{};
        ClearReports(channel);
    }
    state.initialized = false;
    ++state.generation;
    state.alloc = nullptr;
    state.free = nullptr;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
WPADLibStatus WPADGetStatus() {
    RequireOwner();
    return State().initialized && (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) == SDL_INIT_GAMEPAD
        ? WPAD_LIB_STATUS_3 : WPAD_LIB_STATUS_0;
}
WPADResult WPADProbe(WPADChannel index, WPADDeviceType* type) {
    auto& channel = GetChannel(index);
    if (type) *type = channel.pad ? WPAD_DEV_CORE : WPAD_DEV_NOT_FOUND;
    return channel.pad ? WPAD_ERR_OK : WPAD_ERR_NO_CONTROLLER;
}
// Native SDL core reports have no qualified Wii speaker HID output. Keep
// unavailable transport separate from the genuinely supported input channel.
BOOL WPADIsSpeakerEnabled(s32 index) {
    (void)GetChannel(index);
    return FALSE;
}
WPADResult WPADControlSpeaker(WPADChannel index, u32 command, WPADCallback callback) {
    auto& channel = GetChannel(index);
    WPADResult result = WPAD_ERR_NO_CONTROLLER;
    if (channel.pad && SDL_GamepadConnected(channel.pad)) {
        // Original WPAD returns its current status for an unknown command and
        // immediately succeeds when OFF is requested for an already off
        // speaker. Neither branch submits a physical operation.
        switch (command) {
        case WPAD_SPEAKER_OFF: result = WPAD_ERR_OK; break;
        case WPAD_SPEAKER_ON:
        case WPAD_SPEAKER_MUTE:
        case WPAD_SPEAKER_UNMUTE:
        case WPAD_SPEAKER_PLAY:
        case WPAD_SPEAKER_5:
            result = WPAD_ERR_COMMUNICATION_ERROR;
            break;
        default: result = WPAD_ERR_OK; break;
        }
    }
    // Source _end delivers these failed/no-op completions immediately in the
    // caller's restored interrupt state. No successful HID operation, speaker
    // enable bit, stream slot or IRQ completion is fabricated.
    if (callback) callback(index, result);
    return result;
}
BOOL WPADCanSendStreamData(s32 index) {
    (void)GetChannel(index);
    return FALSE; // No supported SDL device owns a Wii speaker output queue.
}
s32 WPADSendStreamData(s32 index, void*, u16) {
    auto& channel = GetChannel(index);
    return channel.pad && SDL_GamepadConnected(channel.pad)
        ? WPAD_ERR_COMMUNICATION_ERROR : WPAD_ERR_NO_CONTROLLER;
}
s32 WPADGetInfoAsync(s32 index, WPADInfo*, WPADCallback callback) {
    auto& channel = GetChannel(index);
    // The supported SDL devices have no complete Wii status report. In
    // particular, a desktop keyboard has no battery, LED, protocol or firmware
    // report; SDL's coarse power percentage cannot reconstruct the original
    // HID battery thresholds. Leave the caller's info untouched on failure.
    const WPADResult result = channel.pad && SDL_GamepadConnected(channel.pad)
        ? WPAD_ERR_COMMUNICATION_ERROR : WPAD_ERR_NO_CONTROLLER;
    // Original WPADGetInfoAsync invokes failed-request callbacks immediately
    // after restoring the caller's interrupt state. No hardware operation or
    // successful completion is queued for unavailable status information.
    if (callback) callback(index, result);
    return result;
}
WPADResult WPADGetInfo(WPADChannel index, WPADInfo* output) {
    // Every qualified request above completes synchronously with an error.
    // The original sync wrapper returns that same failed-request result.
    return static_cast<WPADResult>(WPADGetInfoAsync(index, output, nullptr));
}
WPADConnectCallback* WPADSetConnectCallback(s32 index, WPADConnectCallback* callback) {
    auto& channel = GetChannel(index); auto old = channel.connect; channel.connect = callback; return old;
}
WPADExtensionCallback* WPADSetExtensionCallback(s32 index, WPADExtensionCallback* callback) {
    auto& channel = GetChannel(index); auto old = channel.extension; channel.extension = callback; return old;
}
WPADSamplingCallback WPADSetSamplingCallback(s32 index, WPADSamplingCallback callback) {
    auto& channel = GetChannel(index); auto old = channel.sampling; channel.sampling = callback; return old;
}
u32 WPADGetDataFormat(s32 index) { return GetChannel(index).format; }
s32 WPADSetDataFormat(s32 index, u32 format) {
    auto& channel = GetChannel(index);
    if (!channel.pad) return WPAD_ERR_NO_CONTROLLER;
    if (format > WPAD_FMT_CORE_BTN_ACC_DPD) return WPAD_ERR_INVALID;
    channel.format = format;
    return WPAD_ERR_OK;
}
void WPADRead(s32 index, WPADStatus* output) {
    auto& channel = GetChannel(index);
    if (!output) throw std::invalid_argument("WPADRead requires a report buffer");
    if (channel.current.err == WPAD_ERR_OK || channel.current.err == WPAD_ERR_COMMUNICATION_ERROR ||
        channel.current.err == WPAD_ERR_CORRUPTED) *output = channel.current;
    else {
        // Retail WPADRead clears this selected core report on other errors,
        // then returns only its actual error byte.
        std::memset(output, 0, sizeof(*output));
        output->err = channel.current.err;
    }
}
void WPADGetAccGravityUnit(s32 index, u32 type, WPADAccGravityUnit* output) {
    auto& channel = GetChannel(index);
    if (!output) return;
    // Exact nominal unit used by the SDL Wii HID encoder and the inverse above.
    // Absent core/extension calibration remains zero, so original KPAD chooses
    // its own retail fallback. Physical EEPROM calibration remains unqualified.
    if (type == WPAD_ACC_GRAVITY_UNIT_CORE)
        *output = channel.pad ? WPADAccGravityUnit{100, 100, 100} : WPADAccGravityUnit{0, 0, 0};
    else if (type == WPAD_ACC_GRAVITY_UNIT_FS) *output = {0, 0, 0};
}
u8 WPADGetSensorBarPosition() { RequireOwner(); return State().settings.sensor_bar_position; }
u8 WPADGetDpdSensitivity() { RequireOwner(); return State().settings.dpd_sensitivity; }
BOOL WPADIsDpdEnabled(s32 index) {
    auto& channel = GetChannel(index);
    std::lock_guard lock(State().reports);
    return FindDpdProducer(channel.id) && channel.dpd_command != WPAD_DPD_DISABLE;
}
s32 WPADControlDpd(s32 index, u32 command, WPADCallback callback) {
    auto& channel = GetChannel(index);
    WPADResult result;
    {
        std::lock_guard lock(State().reports);
        if (channel.pad && FindDpdProducer(channel.id)) {
            if (command != WPAD_DPD_DISABLE && command != WPAD_DPD_BASIC && command != WPAD_DPD_STANDARD)
                result = WPAD_ERR_INVALID;
            // Preserve original WPADControlDpd's disabled/repeated-pending
            // branches. They submit no new hardware command and invoke the
            // caller callback immediately, retaining its mask/context.
            else if ((command == WPAD_DPD_DISABLE && channel.dpd_command == WPAD_DPD_DISABLE) ||
                     (command != WPAD_DPD_DISABLE && command == channel.dpd_pending_command))
                result = WPAD_ERR_OK;
            else if (channel.dpd_pending)
                result = WPAD_ERR_COMMUNICATION_ERROR;
            else {
                // One actual native camera-register slot, independently of
                // the optional callback. The owner commits it through IRQ
                // delivery before it becomes the enabled camera state.
                channel.dpd_pending_command = command;
                channel.dpd_pending = true;
                channel.dpd_result = WPAD_ERR_OK;
                channel.dpd_completion = callback;
                return WPAD_ERR_OK;
            }
        } else {
            result = !channel.pad ? WPAD_ERR_NO_CONTROLLER :
                command == WPAD_DPD_DISABLE ? WPAD_ERR_OK : WPAD_ERR_INVALID;
        }
    }
    if (callback) callback(index, result);
    return result;
}
void WPADControlMotor(s32 index, u32 command) {
    auto& channel = GetChannel(index);
    if (!channel.pad) return;
    if (command != WPAD_MOTOR_STOP && command != WPAD_MOTOR_RUMBLE)
        throw std::invalid_argument("Unknown Wii motor command");
    // Preserve original WPAD's disabled/repeated-command branches. A physical
    // request still requires an actual SDL actuator and successful submission.
    if (!State().motor_enabled && (command != WPAD_MOTOR_STOP || !channel.motor_running)) return;
    if ((command == WPAD_MOTOR_STOP) == !channel.motor_running) return;
    if (!SDL_GetBooleanProperty(SDL_GetGamepadProperties(channel.pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
        throw std::runtime_error("Wii Remote SDL device has no rumble output");
    const Uint16 intensity = command == WPAD_MOTOR_RUMBLE ? 65535 : 0;
    if (!SDL_RumbleGamepad(channel.pad, intensity, intensity, command == WPAD_MOTOR_RUMBLE ? 0xffffffffu : 0))
        throw std::runtime_error(SDL_GetError());
    channel.motor_running = command == WPAD_MOTOR_RUMBLE;
}
void WPADEnableMotor(BOOL enabled) {
    RequireOwner();
    std::lock_guard lock(State().reports);
    State().motor_enabled = enabled != FALSE;
}
BOOL WPADIsMotorEnabled() {
    RequireOwner();
    std::lock_guard lock(State().reports);
    return State().motor_enabled ? TRUE : FALSE;
}
}

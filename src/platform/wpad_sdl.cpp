#include "platform/wpad_sdl.h"
#include "platform/desktop_nunchuk.h"
#include "platform/interrupts.h"

#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

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
    // Extension state of a connected remote, kept like the original parser's
    // devType: CORE, INITIALIZING during the extension handshake, FREESTYLE.
    u8 device = WPAD_DEV_CORE;
    // The freestyle report layout extends the core layout.
    std::array<WPADFSStatus, kPendingReports> pending{};
    std::size_t head = 0, count = 0;
    WPADFSStatus current{};
    u32 dpd_command = WPAD_DPD_DISABLE;
    u32 dpd_pending_command = WPAD_DPD_DISABLE;
    bool dpd_pending = false;
    WPADCallback dpd_completion = nullptr;
    WPADResult dpd_result = WPAD_ERR_OK;
    bool announced = false;
    bool pending_disconnect = false;
    bool disconnected_this_service = false;
    bool motor_running = false;
    bool rumble_failure_logged = false;
    // A Wii Remote whose SDL Wii HID driver reports an attached Nunchuk (device
    // "Nintendo Wii Remote with Nunchuk": left stick, C button, Z trigger axis,
    // ACCEL_L sensor). Its post-parser freestyle words, as the virtual source's.
    bool physical_nunchuk = false;
    mscharged::platform::NativeNunchukObservation nunchuk{};
};
struct DpdProducer {
    mscharged::platform::NativeDpdSource source{};
    mscharged::platform::NativeDpdObservation observation{};
};
struct NunchukProducer {
    mscharged::platform::NativeNunchukSource source{};
    mscharged::platform::NativeNunchukObservation observation{};
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
    u8 speaker_volume = 0;
    std::uint64_t generation = 0;
    std::array<DpdProducer, WPAD_MAX_CONTROLLERS> dpd_producers{};
    std::thread::id dpd_owner{};
    std::uint64_t dpd_generation = 0;
    std::array<NunchukProducer, WPAD_MAX_CONTROLLERS> nunchuk_producers{};
    // Remotes SDL lists without the Wii HID driver's buttons and motion sensor
    // (for example only through the kernel driver). Opened once, not per frame.
    std::vector<SDL_JoystickID> rejected_remotes;
    // Physical remotes already named in the log (open failure or unsupported
    // extension), so each problem is reported once rather than every service.
    std::vector<SDL_JoystickID> reported_remotes;
    // Desktop mouse camera lent to remotes without their own camera source.
    // SDL's Wii HID driver reports no IR data, so a physical remote points
    // with the mouse while its buttons, stick and motion stay its own.
    bool shared_pointer = false;
    mscharged::platform::NativeDpdObservation shared_pointer_observation{};
    // The mouse alongside a controller player (SetNativeWpadMousePlayer).
    int mouse_channel = -1;
    bool mouse_pointer = false, mouse_a = false, mouse_b = false;
    mscharged::platform::NativeDpdObservation mouse_observation{};
    // Player assignment of native providers: joysticks with a fixed channel,
    // and the channels kept free for them.
    std::vector<std::pair<SDL_JoystickID, int>> fixed_channels;
    std::uint32_t reserved_channels = 0;
    std::thread::id nunchuk_owner{};
    std::uint64_t nunchuk_generation = 0;
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
const mscharged::platform::NativeDpdObservation* ChannelPointer(const Channel& channel) {
    if (!channel.id) return nullptr;
    if (const auto* producer = FindDpdProducer(channel.id)) return &producer->observation;
    return State().shared_pointer ? &State().shared_pointer_observation : nullptr;
}
void CopyDpdObservation(Channel& channel, WPADFSStatus& report) {
    const auto* observation = ChannelPointer(channel);
    if (!observation || channel.dpd_command == WPAD_DPD_DISABLE) return;
    for (std::size_t n = 0; n < observation->size(); ++n) {
        const auto& object = (*observation)[n];
        report.obj[n] = {object.x, object.y, object.size, object.trace_id};
    }
}
NunchukProducer* FindNunchukProducer(SDL_JoystickID id) {
    if (!id) return nullptr;
    for (auto& producer : State().nunchuk_producers)
        if (producer.source.joystick_id == id) return &producer;
    return nullptr;
}
void RequireNunchukOwner() {
    RequireOwner();
    if (State().nunchuk_owner != std::thread::id{} && State().nunchuk_owner != std::this_thread::get_id())
        throw std::logic_error("Nunchuk observations require their native SDL owner");
}
NunchukProducer& RequireNunchukProducer(mscharged::platform::NativeNunchukSource source) {
    auto* producer = FindNunchukProducer(source.joystick_id);
    if (!source.generation || !producer || producer->source.generation != source.generation)
        throw std::invalid_argument("Nunchuk observation source is retired or foreign");
    return *producer;
}
bool FreestyleFormat(u32 format) {
    return format >= WPAD_FMT_FS_BTN && format <= WPAD_FMT_FS_BTN_ACC_DPD;
}
void CopyNunchukObservation(const Channel& channel, WPADFSStatus& report) {
    // The remote sends extension bytes only in the freestyle report formats,
    // and the original parser decodes them only once the extension is known.
    if (channel.device != WPAD_DEV_FREESTYLE || !FreestyleFormat(channel.format)) return;
    const auto* producer = FindNunchukProducer(channel.id);
    if (!producer && !channel.physical_nunchuk) return;
    const auto& value = producer ? producer->observation : channel.nunchuk;
    report.fsStickX = value.stick_x;
    report.fsStickY = value.stick_y;
    report.fsAccX = value.acc_x;
    report.fsAccY = value.acc_y;
    report.fsAccZ = value.acc_z;
    if (value.c) report.button |= WPAD_BUTTON_FS_C;
    if (value.z) report.button |= WPAD_BUTTON_FS_Z;
}
void ClearReports(Channel& channel) {
    channel.id = 0;
    channel.buttons = 0;
    channel.device = WPAD_DEV_CORE;
    channel.motor_running = false;
    channel.physical_nunchuk = false;
    channel.nunchuk = {};
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
std::int8_t NunchukStick(int value) {
    // SDL's Wii driver posts the calibrated stick as +/-32767 (Y inverted);
    // the WPAD freestyle word counts from the stick centre, ~100 at full tilt.
    const double scaled = std::clamp(value / 32767.0, -1.0, 1.0) * 100.0;
    return static_cast<std::int8_t>(std::lround(scaled));
}
std::int16_t NunchukAccel(float value) {
    // The driver emits (-x, z, y) / 200 counts per g after the 0x200 zero point.
    const double counts = std::isfinite(value) ? double(value) * 200.0 / double(kGravity) : 0.0;
    return static_cast<std::int16_t>(std::lround(std::clamp(counts, -512.0, 511.0)));
}
bool SDLCALL Watch(void*, SDL_Event* event) {
    auto& state = State();
    std::lock_guard lock(state.reports);
    SDL_JoystickID id = 0;
    if (event->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || event->type == SDL_EVENT_JOYSTICK_BUTTON_UP)
        id = event->jbutton.which;
    else if (event->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE)
        id = event->gsensor.which;
    else if (event->type == SDL_EVENT_JOYSTICK_AXIS_MOTION)
        id = event->jaxis.which;
    else return true;
    for (auto& channel : state.channels) {
        if (!channel.id || channel.id != id) continue;
        if (event->type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
            if (!channel.physical_nunchuk) return true;
            if (event->jaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
                channel.nunchuk.stick_x = NunchukStick(event->jaxis.value);
            else if (event->jaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
                channel.nunchuk.stick_y = NunchukStick(-int(event->jaxis.value));
            else if (event->jaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                channel.nunchuk.z = event->jaxis.value > 0;
            return true;
        }
        if (event->type != SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
            if (channel.physical_nunchuk && event->jbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
                channel.nunchuk.c = event->jbutton.down;
            int raw_index = int(event->jbutton.button) - int(SDL_GAMEPAD_BUTTON_MISC1);
            if (raw_index >= 0 && raw_index < int(kRemoteButtons.size())) {
                u16 mask = kRemoteButtons[raw_index];
                if (event->jbutton.down) channel.buttons |= mask;
                else channel.buttons &= ~mask;
            }
            return true;
        }
        if (event->gsensor.sensor == SDL_SENSOR_ACCEL_L) {
            if (channel.physical_nunchuk) {
                channel.nunchuk.acc_x = NunchukAccel(-event->gsensor.data[0]);
                channel.nunchuk.acc_y = NunchukAccel(event->gsensor.data[2]);
                channel.nunchuk.acc_z = NunchukAccel(event->gsensor.data[1]);
            }
            return true;
        }
        if (event->gsensor.sensor != SDL_SENSOR_ACCEL) return true;
        WPADFSStatus report{};
        report.dev = channel.device;
        report.err = WPAD_ERR_OK;
        report.button = channel.buttons;
        CopyDpdObservation(channel, report);
        if (int(&channel - state.channels.data()) == state.mouse_channel) {
            if (state.mouse_a) report.button |= WPAD_BUTTON_A;
            if (state.mouse_b) report.button |= WPAD_BUTTON_B;
            if (state.mouse_pointer && channel.dpd_command != WPAD_DPD_DISABLE)
                for (std::size_t n = 0; n < state.mouse_observation.size(); ++n) {
                    const auto& object = state.mouse_observation[n];
                    report.obj[n] = {object.x, object.y, object.size, object.trace_id};
                }
        }
        CopyNunchukObservation(channel, report);
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
    // The driver renames a remote whose extension it identified as a Nunchuk.
    const char* name = SDL_GetJoystickNameForID(id);
    const auto vendor = SDL_GetGamepadVendorForID(id), product = SDL_GetGamepadProductForID(id);
    return name && (std::strcmp(name, "Nintendo Wii Remote") == 0 ||
                    std::strcmp(name, "Nintendo Wii Remote with Nunchuk") == 0) && vendor == 0x057e &&
        (product == 0x0306 || product == 0x0330);
}
bool ReportOnce(SDL_JoystickID id) {
    auto& reported = State().reported_remotes;
    if (std::find(reported.begin(), reported.end(), id) != reported.end()) return false;
    reported.push_back(id);
    return true;
}
// A physical Wii Remote that SDL lists but the WPAD layer does not use (an
// extension other than the Nunchuk). Logged once; the game never sees it.
void ReportUnsupportedRemote(SDL_JoystickID id) {
    if (SDL_IsJoystickVirtual(id) || SDL_GetGamepadVendorForID(id) != 0x057e) return;
    const auto product = SDL_GetGamepadProductForID(id);
    if (product != 0x0306 && product != 0x0330) return;
    const char* name = SDL_GetJoystickNameForID(id);
    if (!State().settings.physical_wii_remotes || !ReportOnce(id)) return;
    SDL_Log("Wii Remote ignored: SDL reports \"%s\"; use it with a Nunchuk (or no extension) "
            "and reconnect it", name ? name : "?");
}
#if defined(__linux__)
// SDL's Wii HID driver opens /dev/hidraw read/write; without permission the
// Remote stays invisible (or appears only as the kernel's evdev duplicate).
void ReportInaccessibleRemotes() {
    SDL_hid_device_info* devices = SDL_hid_enumerate(0x057e, 0);
    for (auto* device = devices; device; device = device->next)
        if ((device->product_id == 0x0306 || device->product_id == 0x0330) && device->path &&
                access(device->path, R_OK | W_OK) != 0)
            SDL_Log("Wii Remote at %s: no permission for this user. Install the udev rule from "
                    "docs/RUNTIME.md (Wii Remote), then reconnect the Remote.", device->path);
    SDL_hid_free_enumeration(devices);
}
#endif
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
bool DispatchExtension(Channel& channel, s32 index, u8 device) {
    struct Change { Channel* channel; s32 index; u8 device; } change{&channel, index, device};
    return mscharged::platform::DispatchNativeInterrupt([](void* opaque) {
        auto& value = *static_cast<Change*>(opaque);
        WPADExtensionCallback* callback;
        {
            // The original parser stores devType before calling extensionCB.
            std::lock_guard lock(State().reports);
            value.channel->device = value.device;
            callback = value.channel->extension;
        }
        if (callback) callback(value.index, value.device);
    }, &change);
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
void SetNativeWpadMousePlayer(int channel, const NativeDpdObservation* observation, bool a, bool b) {
    RequireOwner();
    if (channel < -1 || channel >= WPAD_MAX_CONTROLLERS)
        throw std::invalid_argument("Mouse player outside Wii hardware ports");
    if (observation)
        for (const auto& object : *observation)
            if (object.size && (object.x < 0 || object.x >= WPAD_MAX_DPD_X ||
                                object.y < 0 || object.y >= WPAD_MAX_DPD_Y))
                throw std::invalid_argument("Mouse pointer observation exceeds its raw sensor domain");
    auto& state = State();
    std::lock_guard lock(state.reports);
    state.mouse_channel = channel;
    state.mouse_pointer = channel >= 0 && observation;
    state.mouse_observation = state.mouse_pointer ? *observation : NativeDpdObservation{};
    state.mouse_a = channel >= 0 && a;
    state.mouse_b = channel >= 0 && b;
}
void SetNativeWpadSharedPointer(const NativeDpdObservation* observation) {
    RequireOwner();
    if (observation)
        for (const auto& object : *observation)
            if (object.size && (object.x < 0 || object.x >= WPAD_MAX_DPD_X ||
                                object.y < 0 || object.y >= WPAD_MAX_DPD_Y))
                throw std::invalid_argument("Shared pointer observation exceeds its raw sensor domain");
    auto& state = State();
    std::lock_guard lock(state.reports);
    if (observation) {
        state.shared_pointer = true;
        state.shared_pointer_observation = *observation;
        return;
    }
    if (!state.shared_pointer) return;
    state.shared_pointer = false;
    state.shared_pointer_observation = {};
    // Remotes that borrowed the camera lose it like a detached camera source.
    for (auto& channel : state.channels) if (channel.id && !FindDpdProducer(channel.id)) {
        channel.dpd_command = WPAD_DPD_DISABLE;
        for (auto& object : channel.current.obj) object = {};
        for (auto& report : channel.pending) for (auto& object : report.obj) object = {};
    }
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
NativeNunchukSource AttachNativeWpadNunchukSource(std::uint32_t joystick_id) {
    RequireNunchukOwner();
    if (!joystick_id || !SDL_GetJoystickFromID(joystick_id) || !SDL_IsJoystickVirtual(joystick_id))
        throw std::invalid_argument("Nunchuk source must own an open explicit SDL virtual device");
    auto& state = State();
    std::lock_guard lock(state.reports);
    if (FindNunchukProducer(joystick_id)) throw std::logic_error("Nunchuk source is already attached");
    NunchukProducer* slot = nullptr;
    for (auto& producer : state.nunchuk_producers) if (!producer.source.generation) { slot = &producer; break; }
    if (!slot) throw std::length_error("Native Nunchuk source capacity is exhausted");
    if (state.nunchuk_generation == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Nunchuk source incarnation exhausted");
    *slot = {};
    // A level, untouched Nunchuk until the first observation arrives.
    slot->observation.acc_z = kNativeNunchukGravity;
    slot->source = {joystick_id, ++state.nunchuk_generation};
    state.nunchuk_owner = std::this_thread::get_id();
    return slot->source;
}
void SubmitNativeWpadNunchukObservation(NativeNunchukSource source, const NativeNunchukObservation& observation) {
    RequireNunchukOwner();
    // Post-parser words: 10-bit raw acceleration minus its zero-g point.
    for (const auto value : {observation.acc_x, observation.acc_y, observation.acc_z})
        if (value < -512 || value > 511)
            throw std::invalid_argument("Nunchuk acceleration exceeds its raw sensor domain");
    // Query SDL before taking the report mutex, as for DPD observations.
    SDL_Joystick* joystick = SDL_GetJoystickFromID(source.joystick_id);
    if (!joystick || !SDL_JoystickConnected(joystick))
        throw std::invalid_argument("Nunchuk source SDL device is no longer live");
    auto& state = State();
    std::lock_guard lock(state.reports);
    RequireNunchukProducer(source).observation = observation;
}
void DetachNativeWpadNunchukSource(NativeNunchukSource source) {
    RequireNunchukOwner();
    auto& state = State();
    std::lock_guard lock(state.reports);
    // A still-connected remote reports CORE at its next owner service.
    RequireNunchukProducer(source) = {};
    bool any = false;
    for (const auto& producer : state.nunchuk_producers) any |= producer.source.generation != 0;
    if (!any) state.nunchuk_owner = {};
}
void ConfigureWpadSDL(WpadSDLSettings settings) {
    auto& state = State();
    if (state.initialized) throw std::logic_error("WPAD system preferences must precede initialization");
    if (settings.sensor_bar_position > 1 || settings.dpd_sensitivity < 1 || settings.dpd_sensitivity > 5)
        throw std::invalid_argument("Invalid Wii sensor-bar or DPD sensitivity preference");
    state.settings = settings;
    state.configured = true;
}
int GetNativeWpadChannel(std::uint32_t joystick_id) {
    auto& state = State();
    for (s32 index = 0; index < WPAD_MAX_CONTROLLERS; ++index)
        if (state.channels[index].pad && state.channels[index].id == joystick_id) return int(index);
    return -1;
}

void SetNativeWpadFixedChannel(std::uint32_t joystick_id, int channel) {
    RequireOwner();
    if (channel < -1 || channel >= WPAD_MAX_CONTROLLERS)
        throw std::out_of_range("WPAD channel outside Wii hardware ports");
    auto& fixed = State().fixed_channels;
    std::erase_if(fixed, [&](const auto& entry) { return entry.first == joystick_id; });
    if (channel >= 0) fixed.emplace_back(joystick_id, channel);
}
void SetNativeWpadReservedChannels(std::uint32_t mask) {
    RequireOwner();
    State().reserved_channels = mask & ((1u << WPAD_MAX_CONTROLLERS) - 1);
}

bool GetNativeWpadCameraEnabled(std::uint32_t joystick_id) {
    auto& state = State();
    std::lock_guard lock(state.reports);
    for (const auto& channel : state.channels)
        if (channel.pad && channel.id == joystick_id) return channel.dpd_command != WPAD_DPD_DISABLE;
    return false;
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
            if (!SDL_IsJoystickVirtual(SDL_GetGamepadID(channel.pad)))
                SDL_Log("Wii Remote disconnected from WPAD channel %d", int(index));
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
        if (assigned) continue;
        if (!SupportedRemote(ids[n])) { ReportUnsupportedRemote(ids[n]); continue; }
        if (std::find(state.rejected_remotes.begin(), state.rejected_remotes.end(), ids[n]) !=
                state.rejected_remotes.end()) continue;
        // A fixed player connects only to its own channel and waits while that
        // channel is busy or still retiring a device. Others take the lowest
        // free channel outside the reserved ones; when that channel is still
        // retiring a device in this service, wait for the next service instead
        // of moving the new device to a later player port.
        Channel* slot = nullptr;
        int fixed = -1;
        for (const auto& [joystick, channel] : state.fixed_channels)
            if (joystick == ids[n]) fixed = channel;
        if (fixed >= 0) {
            auto& channel = state.channels[fixed];
            if (channel.pad || channel.disconnected_this_service || channel.pending_disconnect) continue;
            slot = &channel;
        } else {
            for (s32 index = 0; index < WPAD_MAX_CONTROLLERS; ++index)
                if (!state.channels[index].pad && !(state.reserved_channels & (1u << index))) {
                    slot = &state.channels[index];
                    break;
                }
            if (!slot) continue;
            if (slot->disconnected_this_service || slot->pending_disconnect) break;
        }
        SDL_Gamepad* pad = SDL_OpenGamepad(ids[n]);
        if (!pad) {
            if (!SDL_IsJoystickVirtual(ids[n]) && ReportOnce(ids[n]))
                SDL_Log("Wii Remote could not be opened: %s (turn it off and on again)", SDL_GetError());
            continue;
        }
        SDL_Joystick* joystick = SDL_GetGamepadJoystick(pad);
        if (SDL_GetNumJoystickButtons(joystick) < int(SDL_GAMEPAD_BUTTON_MISC1) + int(kRemoteButtons.size()) ||
                !SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
            SDL_CloseGamepad(pad);
            state.rejected_remotes.push_back(ids[n]);
            SDL_Log("Wii Remote ignored: SDL lists it without the Wii HID driver's buttons and "
                    "motion sensor. On Linux this usually means no /dev/hidraw permission; see "
                    "docs/RUNTIME.md (Wii Remote)");
            continue;
        }
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
        const bool nunchuk = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL_L);
        if (!SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true) ||
                (nunchuk && !SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL_L, true))) {
            {
                std::lock_guard lock(state.reports);
                ClearReports(*slot);
            }
            SDL_CloseGamepad(pad);
            continue;
        }
        if (nunchuk) {
            // Current Nunchuk state; later SDL events keep it up to date. A level,
            // untouched Nunchuk reads +1 g on Z until its first motion sample.
            std::lock_guard lock(state.reports);
            slot->physical_nunchuk = true;
            slot->nunchuk = {};
            slot->nunchuk.acc_z = mscharged::platform::kNativeNunchukGravity;
            slot->nunchuk.c = SDL_GetJoystickButton(joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            slot->nunchuk.z = SDL_GetJoystickAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0;
            slot->nunchuk.stick_x = NunchukStick(SDL_GetJoystickAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX));
            slot->nunchuk.stick_y = NunchukStick(-int(SDL_GetJoystickAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY)));
        }
        slot->pad = pad;
        slot->rumble_failure_logged = false;
        if (!SDL_IsJoystickVirtual(SDL_GetGamepadID(pad))) {
            // Retail WPAD lights the LED of the channel the Remote occupies.
            SDL_SetGamepadPlayerIndex(pad, int(slot - state.channels.data()));
            SDL_Log("Wii Remote connected on WPAD channel %d%s", int(slot - state.channels.data()),
                slot->physical_nunchuk ? " with Nunchuk" : "");
        }
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
        // A status report with an attached extension starts the original
        // handshake (INITIALIZING); its configuration read then identifies the
        // Nunchuk (FREESTYLE), and removal reports CORE. One step per service
        // keeps the console's asynchronous handshake observable.
        u8 device = channel.device;
        {
            std::lock_guard lock(state.reports);
            const bool attached = FindNunchukProducer(channel.id) != nullptr || channel.physical_nunchuk;
            if (attached && channel.device == WPAD_DEV_CORE) device = WPAD_DEV_INITIALIZING;
            else if (attached && channel.device == WPAD_DEV_INITIALIZING) device = WPAD_DEV_FREESTYLE;
            else if (!attached && channel.device != WPAD_DEV_CORE) device = WPAD_DEV_CORE;
        }
        if (device != channel.device) {
            if (!DispatchExtension(channel, index, device)) return;
            if (!state.initialized || state.generation != generation) return;
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
    // Physical remotes (Bluetooth or a DolphinBar in mode 4) are served by the
    // native HID driver (platform/wiimote_hid) as virtual core-Wii devices, with
    // their IR camera. SDL's own Wii HID driver must not open the same devices;
    // it also lists a DolphinBar's empty slots as remotes.
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_WII, "0", SDL_HINT_DEFAULT);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError());
#if defined(__linux__)
    if (state.settings.physical_wii_remotes) ReportInaccessibleRemotes();
#endif
    state.owner = std::this_thread::get_id();
    if (!SDL_AddEventWatch(Watch, nullptr)) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        throw std::runtime_error(SDL_GetError());
    }
    for (auto& channel : state.channels) ClearReports(channel);
    state.rejected_remotes.clear();
    state.reported_remotes.clear();
    state.motor_enabled = state.settings.motor_enabled;
    state.speaker_volume = state.settings.speaker_volume > WPAD_MAX_SPEAKER_VOLUME
        ? WPAD_MAX_SPEAKER_VOLUME : state.settings.speaker_volume;
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
    if (type) *type = channel.pad ? static_cast<WPADDeviceType>(channel.device) : WPAD_DEV_NOT_FOUND;
    return channel.pad ? WPAD_ERR_OK : WPAD_ERR_NO_CONTROLLER;
}
// Native SDL core reports have no qualified Wii speaker HID output. Keep
// unavailable transport separate from the genuinely supported input channel.
BOOL WPADIsSpeakerEnabled(s32 index) {
    (void)GetChannel(index);
    return FALSE;
}
u8 WPADGetSpeakerVolume() {
    RequireOwner();
    std::lock_guard lock(State().reports);
    return State().speaker_volume;
}
void WPADSetSpeakerVolume(u8 volume) {
    RequireOwner();
    std::lock_guard lock(State().reports);
    State().speaker_volume = volume > WPAD_MAX_SPEAKER_VOLUME
        ? WPAD_MAX_SPEAKER_VOLUME : volume;
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
    // Core and freestyle report layouts are transported; no native device
    // supplies classic or extended reports.
    if (format > WPAD_FMT_FS_BTN_ACC_DPD) return WPAD_ERR_INVALID;
    std::lock_guard lock(State().reports);
    channel.format = format;
    return WPAD_ERR_OK;
}
void WPADRead(s32 index, WPADStatus* output) {
    auto& channel = GetChannel(index);
    if (!output) throw std::invalid_argument("WPADRead requires a report buffer");
    // Retail WPADRead sizes a successful copy by the selected report format,
    // copies only the core layout for communication/corrupted reports, and
    // clears the selected layout on other errors before storing the error byte.
    const std::size_t size = FreestyleFormat(channel.format) ? sizeof(WPADFSStatus) : sizeof(WPADStatus);
    if (channel.current.err == WPAD_ERR_OK) std::memcpy(output, &channel.current, size);
    else if (channel.current.err == WPAD_ERR_COMMUNICATION_ERROR || channel.current.err == WPAD_ERR_CORRUPTED)
        std::memcpy(output, &channel.current, sizeof(WPADStatus));
    else {
        std::memset(output, 0, size);
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
    else if (type == WPAD_ACC_GRAVITY_UNIT_FS)
        // Extension calibration exists only after the handshake identified
        // the virtual Nunchuk; its nominal unit matches the submitted counts.
        *output = channel.pad && channel.device == WPAD_DEV_FREESTYLE
            ? WPADAccGravityUnit{mscharged::platform::kNativeNunchukGravity, mscharged::platform::kNativeNunchukGravity,
                                 mscharged::platform::kNativeNunchukGravity}
            : WPADAccGravityUnit{0, 0, 0};
}
u8 WPADGetSensorBarPosition() { RequireOwner(); return State().settings.sensor_bar_position; }
u8 WPADGetDpdSensitivity() { RequireOwner(); return State().settings.dpd_sensitivity; }
BOOL WPADIsDpdEnabled(s32 index) {
    auto& channel = GetChannel(index);
    std::lock_guard lock(State().reports);
    return ChannelPointer(channel) && channel.dpd_command != WPAD_DPD_DISABLE;
}
s32 WPADControlDpd(s32 index, u32 command, WPADCallback callback) {
    auto& channel = GetChannel(index);
    WPADResult result;
    {
        std::lock_guard lock(State().reports);
        if (channel.pad && ChannelPointer(channel)) {
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
    // Preserve original WPAD's disabled/repeated-command branches. Retail records
    // the request and has no failure path: a Remote without an actuator, or one
    // SDL has just dropped, stays silent instead of failing the source call.
    if (!State().motor_enabled && (command != WPAD_MOTOR_STOP || !channel.motor_running)) return;
    if ((command == WPAD_MOTOR_STOP) == !channel.motor_running) return;
    channel.motor_running = command == WPAD_MOTOR_RUMBLE;
    if (!SDL_GamepadConnected(channel.pad) ||
            !SDL_GetBooleanProperty(SDL_GetGamepadProperties(channel.pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
        return;
    const Uint16 intensity = channel.motor_running ? 65535 : 0;
    if (!SDL_RumbleGamepad(channel.pad, intensity, intensity, channel.motor_running ? 0xffffffffu : 0) &&
            !channel.rumble_failure_logged) {
        channel.rumble_failure_logged = true;
        SDL_Log("Wii Remote rumble on WPAD channel %d failed: %s", int(index), SDL_GetError());
    }
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

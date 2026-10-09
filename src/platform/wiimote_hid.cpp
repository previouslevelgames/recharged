#include "platform/wiimote_hid.h"
#include "platform/desktop_dpd.h"
#include "platform/desktop_nunchuk.h"
#include "platform/wpad_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::uint16_t kNintendo = 0x057e, kRemote = 0x0306, kRemotePlus = 0x0330;
constexpr float kGravity = 9.80665f;
constexpr std::chrono::milliseconds kProbeWait{600}, kProbeInterval{2000}, kEnumerateInterval{2000};
constexpr std::chrono::milliseconds kSilentLimit{1500}, kCommandWait{250};
// A DolphinBar slot can reject a write while its Bluetooth link is busy; only
// repeated failures mean the remote is gone.
constexpr std::chrono::milliseconds kWriteRetry{20};
constexpr int kWriteAttempts = 10;

// Console IR camera sensitivity blocks (registers 0xb00000 and 0xb0001a),
// levels 1-5 as selected by the Wii's sensor-bar sensitivity setting.
constexpr std::uint8_t kIrBlock1[5][9] = {
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xfe},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xb4},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xc8, 0x00, 0x36},
    {0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20}};
constexpr std::uint8_t kIrBlock2[5][2] = {{0xfd, 0x05}, {0xb3, 0x04}, {0x63, 0x03}, {0x35, 0x03}, {0x1f, 0x03}};

enum class Wait { None, Ack, Read };
enum class Tag { None, Calibration, ExtensionId, NunchukCalibration };
struct Command {
    std::vector<std::uint8_t> report; // byte 1 bit 0 carries the rumble state at send time
    Wait wait = Wait::None;
    Tag tag = Tag::None;
};

struct Remote {
    std::string path;
    bool adapter = false; // DolphinBar interface
    int slot = -1;
    SDL_hid_device* hid = nullptr;
    bool present = false, probing = false, ready = false;
    Clock::time_point next_probe{}, probe_deadline{}, last_report{};
    std::deque<Command> queue;
    bool waiting = false;
    Command current;
    Clock::time_point wait_deadline{};
    bool rumble = false;
    int zero[3]{512, 512, 512}, one[3]{612, 612, 612};
    bool extension = false, nunchuk = false;
    int nunchuk_zero[3]{512, 512, 512}, nunchuk_one[3]{712, 712, 712}, nunchuk_centre[2]{128, 128};
    std::uint8_t battery = 0;
    bool ir_visible = false;
    int write_failures = 0;
    Clock::time_point retry_at{};
    Clock::time_point next_debug_log{};
    int led_channel = -2;
    bool sensors = false;
    SDL_JoystickID virtual_id = 0;
    SDL_Joystick* joystick = nullptr;
    mscharged::platform::NativeDpdSource dpd{};
    mscharged::platform::NativeNunchukSource nunchuk_source{};
};

bool Verbose() {
    static const bool verbose = [] {
        const char* value = SDL_getenv("MSCHARGED_WIIMOTE_DEBUG");
        return value && *value && *value != '0';
    }();
    return verbose;
}

struct Driver {
    bool initialized = false;
    std::thread::id owner;
    mscharged::platform::WiimoteHidSettings settings;
    std::vector<std::unique_ptr<Remote>> remotes;
    Clock::time_point next_enumerate{};
    bool reported_other_mode = false;
};
Driver& State() { static Driver driver; return driver; }

bool Send(Remote& remote, std::vector<std::uint8_t> report) {
    if (!remote.hid || report.size() < 2) return false;
    report[1] = static_cast<std::uint8_t>((report[1] & ~1u) | (remote.rumble ? 1u : 0u));
    return SDL_hid_write(remote.hid, report.data(), report.size()) == int(report.size());
}
void Queue(Remote& remote, std::vector<std::uint8_t> report, Wait wait = Wait::None, Tag tag = Tag::None) {
    remote.queue.push_back({std::move(report), wait, tag});
}
void QueueWrite(Remote& remote, std::uint32_t address, std::initializer_list<std::uint8_t> data) {
    std::vector<std::uint8_t> report(22, 0);
    report[0] = 0x16;
    report[1] = 0x04; // control registers
    report[2] = static_cast<std::uint8_t>(address >> 16);
    report[3] = static_cast<std::uint8_t>(address >> 8);
    report[4] = static_cast<std::uint8_t>(address);
    report[5] = static_cast<std::uint8_t>(data.size());
    std::copy(data.begin(), data.end(), report.begin() + 6);
    Queue(remote, std::move(report), Wait::Ack);
}
void QueueWrite(Remote& remote, std::uint32_t address, const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> report(22, 0);
    report[0] = 0x16;
    report[1] = 0x04;
    report[2] = static_cast<std::uint8_t>(address >> 16);
    report[3] = static_cast<std::uint8_t>(address >> 8);
    report[4] = static_cast<std::uint8_t>(address);
    report[5] = static_cast<std::uint8_t>(size);
    std::copy(data, data + size, report.begin() + 6);
    Queue(remote, std::move(report), Wait::Ack);
}
void QueueRead(Remote& remote, std::uint8_t space, std::uint32_t address, std::uint16_t size, Tag tag) {
    Queue(remote, {0x17, space, static_cast<std::uint8_t>(address >> 16), static_cast<std::uint8_t>(address >> 8),
                   static_cast<std::uint8_t>(address), static_cast<std::uint8_t>(size >> 8),
                   static_cast<std::uint8_t>(size)}, Wait::Read, tag);
}
void QueueReportingMode(Remote& remote) {
    // Continuous core buttons, accelerometer, basic IR and 6 extension bytes:
    // the format the console itself uses with a Nunchuk attached.
    Queue(remote, {0x12, 0x04, 0x37});
}
void QueueExtensionSetup(Remote& remote) {
    // Unencrypted extension initialization, then identify it.
    QueueWrite(remote, 0xa400f0, {0x55});
    QueueWrite(remote, 0xa400fb, {0x00});
    QueueRead(remote, 0x04, 0xa400fa, 6, Tag::ExtensionId);
}
void QueueSetup(Remote& remote) {
    auto& settings = State().settings;
    const int level = std::clamp(int(settings.ir_sensitivity), 1, 5) - 1;
    QueueRead(remote, 0x00, 0x000016, 10, Tag::Calibration);
    // Camera on, configure while enabled, then start sending objects (the
    // order the Linux hid-wiimote driver uses; 0x08 first leaves it silent).
    Queue(remote, {0x13, 0x04});
    Queue(remote, {0x1a, 0x04});
    QueueWrite(remote, 0xb00030, {0x01});
    QueueWrite(remote, 0xb00000, kIrBlock1[level], 9);
    QueueWrite(remote, 0xb0001a, kIrBlock2[level], 2);
    QueueWrite(remote, 0xb00033, {0x01}); // basic mode, fits next to the extension bytes
    QueueWrite(remote, 0xb00030, {0x08});
    if (remote.extension) QueueExtensionSetup(remote);
    QueueReportingMode(remote);
}

bool SDLCALL VirtualRumble(void* userdata, Uint16 low, Uint16 high) {
    auto& remote = *static_cast<Remote*>(userdata);
    const bool on = low || high;
    if (on == remote.rumble) return true;
    remote.rumble = on;
    return Send(remote, {0x10, 0x00});
}
bool SDLCALL VirtualSensors(void* userdata, bool enabled) {
    static_cast<Remote*>(userdata)->sensors = enabled;
    return true;
}

void AttachVirtual(Remote& remote) {
    if (remote.virtual_id) return;
    const SDL_VirtualJoystickSensorDesc sensor{SDL_SENSOR_ACCEL, 100.0f};
    SDL_VirtualJoystickDesc descriptor;
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.vendor_id = kNintendo;
    descriptor.product_id = kRemote;
    descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = int(SDL_GAMEPAD_BUTTON_MISC1) + 11;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    // The existing WPAD transport accepts the core-Wii identity.
    descriptor.name = "Nintendo Wii Remote";
    descriptor.nsensors = 1;
    descriptor.sensors = &sensor;
    descriptor.userdata = &remote;
    descriptor.SetSensorsEnabled = VirtualSensors;
    descriptor.Rumble = VirtualRumble;
    remote.virtual_id = SDL_AttachVirtualJoystick(&descriptor);
    if (!remote.virtual_id) {
        SDL_Log("Wii Remote (%s) could not become a game controller: %s", remote.path.c_str(), SDL_GetError());
        return;
    }
    remote.joystick = SDL_OpenJoystick(remote.virtual_id);
    if (!remote.joystick) {
        SDL_DetachVirtualJoystick(remote.virtual_id);
        remote.virtual_id = 0;
        return;
    }
    remote.dpd = mscharged::platform::AttachNativeWpadDpdSource(remote.virtual_id);
    if (remote.nunchuk)
        remote.nunchuk_source = mscharged::platform::AttachNativeWpadNunchukSource(remote.virtual_id);
    SDL_Log("Wii Remote connected%s%s%s", remote.adapter ? " via DolphinBar slot " : "",
            remote.adapter ? std::to_string(remote.slot + 1).c_str() : "", remote.nunchuk ? " with Nunchuk" : "");
}

void DetachVirtual(Remote& remote) {
    if (remote.nunchuk_source.generation) mscharged::platform::DetachNativeWpadNunchukSource(remote.nunchuk_source);
    remote.nunchuk_source = {};
    if (remote.dpd.generation) mscharged::platform::DetachNativeWpadDpdSource(remote.dpd);
    remote.dpd = {};
    if (remote.joystick) SDL_CloseJoystick(remote.joystick);
    remote.joystick = nullptr;
    if (remote.virtual_id) SDL_DetachVirtualJoystick(remote.virtual_id);
    remote.virtual_id = 0;
    remote.led_channel = -2;
}

void Lost(Remote& remote, const char* why) {
    if (remote.present) SDL_Log("Wii Remote disconnected (%s)", why);
    DetachVirtual(remote);
    remote.present = remote.ready = remote.probing = remote.waiting = false;
    remote.queue.clear();
    remote.write_failures = 0;
    remote.retry_at = {};
    remote.extension = remote.nunchuk = false;
    remote.rumble = false;
    remote.next_probe = Clock::now() + 1s;
}

int Calibrated(int raw, int zero, int one, int unit) {
    const int span = one - zero;
    if (span <= 0) return 0;
    return int(std::lround(double(raw - zero) * unit / span));
}

void HandleRead(Remote& remote, Tag tag, const std::uint8_t* data, int size) {
    if (Verbose()) {
        char hex[3 * 16 + 1]{};
        for (int n = 0; n < size && n < 16; ++n) std::snprintf(hex + 3 * n, 4, "%02x ", data[n]);
        SDL_Log("Wii Remote %s: read tag %d -> %s", remote.path.c_str(), int(tag), hex);
    }
    if (tag == Tag::Calibration && size >= 8) {
        // Zero g and one g for X, Y, Z: 8 high bits, then two low bits each.
        for (int axis = 0; axis < 3; ++axis) {
            const int shift = 4 - 2 * axis;
            const int zero = (data[axis] << 2) | ((data[3] >> shift) & 3);
            const int one = (data[4 + axis] << 2) | ((data[7] >> shift) & 3);
            if (one > zero + 20) { remote.zero[axis] = zero; remote.one[axis] = one; }
        }
    } else if (tag == Tag::ExtensionId && size >= 6) {
        const bool nunchuk = data[2] == 0xa4 && data[3] == 0x20 && data[4] == 0x00 && data[5] == 0x00;
        if (nunchuk && !remote.nunchuk) {
            remote.nunchuk = true;
            QueueRead(remote, 0x04, 0xa40020, 16, Tag::NunchukCalibration);
            if (remote.virtual_id && !remote.nunchuk_source.generation)
                remote.nunchuk_source = mscharged::platform::AttachNativeWpadNunchukSource(remote.virtual_id);
            if (remote.ready) SDL_Log("Nunchuk attached");
        } else if (!nunchuk && size >= 6) {
            SDL_Log("Wii Remote extension %02x%02x%02x%02x%02x%02x is not supported yet",
                    data[0], data[1], data[2], data[3], data[4], data[5]);
        }
    } else if (tag == Tag::NunchukCalibration && size >= 14) {
        for (int axis = 0; axis < 3; ++axis) {
            const int shift = 4 - 2 * axis;
            const int zero = (data[axis] << 2) | ((data[3] >> shift) & 3);
            const int one = (data[4 + axis] << 2) | ((data[7] >> shift) & 3);
            if (one > zero + 20) { remote.nunchuk_zero[axis] = zero; remote.nunchuk_one[axis] = one; }
        }
        if (data[10] > 64 && data[10] < 192) remote.nunchuk_centre[0] = data[10];
        if (data[13] > 64 && data[13] < 192) remote.nunchuk_centre[1] = data[13];
    }
}

void Publish(Remote& remote, const std::uint8_t* report) {
    if (!remote.joystick) return;
    const std::uint8_t b0 = report[1], b1 = report[2];
    // WPAD order: A, B, 1, 2, +, -, HOME, up, down, left, right.
    const bool buttons[11]{(b1 & 0x08) != 0, (b1 & 0x04) != 0, (b1 & 0x02) != 0, (b1 & 0x01) != 0,
                           (b0 & 0x10) != 0, (b1 & 0x10) != 0, (b1 & 0x80) != 0, (b0 & 0x08) != 0,
                           (b0 & 0x04) != 0, (b0 & 0x01) != 0, (b0 & 0x02) != 0};
    const auto dots = mscharged::platform::DecodeWiimoteBasicIr(report + 6);
    bool visible = false;
    for (const auto& dot : dots) visible |= dot.size != 0;
    remote.ir_visible = visible;
    if (remote.dpd.generation) mscharged::platform::SubmitNativeWpadDpdObservation(remote.dpd, dots);
    if (Verbose() && Clock::now() >= remote.next_debug_log) {
        // Once a second: raw IR bytes, decoded camera objects, calibrated tilt
        // (100 = 1 g) and whether original WPAD enabled this player's camera.
        remote.next_debug_log = Clock::now() + 1s;
        char raw_ir[10 * 3 + 1]{}, objects[96]{};
        for (int n = 0; n < 10; ++n) std::snprintf(raw_ir + 3 * n, 4, "%02x ", report[6 + n]);
        int length = 0, count = 0;
        const mscharged::platform::NativeDpdObject* pair[2]{};
        for (const auto& dot : dots) {
            if (!dot.size) continue;
            if (count < 2) pair[count] = &dot;
            ++count;
            length += std::snprintf(objects + length, sizeof(objects) - length, " (%d,%d)", dot.x, dot.y);
        }
        // KPAD pairs two objects 90-510 camera pixels apart (sensor bar
        // 0.5-3 m away) whose line matches the remote's roll.
        if (count >= 2)
            length += std::snprintf(objects + length, sizeof(objects) - length, " spacing %d",
                int(std::lround(std::hypot(pair[1]->x - pair[0]->x, pair[1]->y - pair[0]->y))));
        const int raw[3]{(report[3] << 2) | ((b0 >> 5) & 3), (report[4] << 2) | ((b1 >> 4) & 2),
                         (report[5] << 2) | ((b1 >> 5) & 2)};
        SDL_Log("Wii Remote IR: player %d camera %s, objects%s, tilt x %d y %d z %d, raw %s",
                mscharged::platform::GetNativeWpadChannel(remote.virtual_id) + 1,
                mscharged::platform::GetNativeWpadCameraEnabled(remote.virtual_id) ? "on" : "off",
                length ? objects : " none", Calibrated(raw[0], remote.zero[0], remote.one[0], 100),
                Calibrated(raw[1], remote.zero[1], remote.one[1], 100),
                Calibrated(raw[2], remote.zero[2], remote.one[2], 100), raw_ir);
    }
    if (remote.nunchuk && remote.nunchuk_source.generation) {
        const std::uint8_t* ext = report + 16;
        mscharged::platform::NativeNunchukObservation nunchuk{};
        nunchuk.stick_x = static_cast<std::int8_t>(std::clamp(int(ext[0]) - remote.nunchuk_centre[0], -127, 127));
        nunchuk.stick_y = static_cast<std::int8_t>(std::clamp(int(ext[1]) - remote.nunchuk_centre[1], -127, 127));
        const int raw[3]{(ext[2] << 2) | ((ext[5] >> 2) & 3), (ext[3] << 2) | ((ext[5] >> 4) & 3),
                         (ext[4] << 2) | ((ext[5] >> 6) & 3)};
        const auto acc = [&](int axis) {
            return static_cast<std::int16_t>(std::clamp(Calibrated(raw[axis], remote.nunchuk_zero[axis],
                remote.nunchuk_one[axis], mscharged::platform::kNativeNunchukGravity), -512, 511));
        };
        nunchuk.acc_x = acc(0);
        nunchuk.acc_y = acc(1);
        nunchuk.acc_z = acc(2);
        nunchuk.z = (ext[5] & 0x01) == 0;
        nunchuk.c = (ext[5] & 0x02) == 0;
        mscharged::platform::SubmitNativeWpadNunchukObservation(remote.nunchuk_source, nunchuk);
    }
    for (int n = 0; n < 11; ++n)
        SDL_SetJoystickVirtualButton(remote.joystick, int(SDL_GAMEPAD_BUTTON_MISC1) + n, buttons[n]);
    SDL_UpdateJoysticks();
    if (remote.sensors) {
        const int raw[3]{(report[3] << 2) | ((b0 >> 5) & 3), (report[4] << 2) | ((b1 >> 4) & 2),
                         (report[5] << 2) | ((b1 >> 5) & 2)};
        int counts[3];
        for (int axis = 0; axis < 3; ++axis)
            counts[axis] = std::clamp(Calibrated(raw[axis], remote.zero[axis], remote.one[axis], 100), -512, 511);
        // The WPAD transport reads the pinned SDL Wii convention: 100 counts
        // per g, axes (-x, z, y) in m/s^2.
        const float data[3]{-counts[0] * kGravity / 100.0f, counts[2] * kGravity / 100.0f,
                            counts[1] * kGravity / 100.0f};
        SDL_SendJoystickVirtualSensorData(remote.joystick, SDL_SENSOR_ACCEL, SDL_GetTicksNS(), data, 3);
    }
}

void HandleReport(Remote& remote, const std::uint8_t* report, int size, Clock::time_point now) {
    const std::uint8_t id = report[0];
    if (id == 0x20 && size >= 7) {
        // Status: answers a probe, or reports an extension change; the remote
        // then stops data reports until the reporting mode is sent again.
        if (Verbose())
            SDL_Log("Wii Remote %s: status flags %02x battery %d", remote.path.c_str(), report[3], report[6]);
        remote.battery = report[6];
        remote.last_report = now;
        const bool extension = (report[3] & 0x02) != 0;
        if (!remote.present) {
            remote.present = true;
            remote.probing = false;
            remote.extension = extension;
            QueueSetup(remote);
            return;
        }
        if (extension != remote.extension) {
            remote.extension = extension;
            if (!extension && remote.nunchuk) {
                remote.nunchuk = false;
                if (remote.nunchuk_source.generation)
                    mscharged::platform::DetachNativeWpadNunchukSource(remote.nunchuk_source);
                remote.nunchuk_source = {};
                SDL_Log("Nunchuk removed");
            }
            if (extension) QueueExtensionSetup(remote);
        }
        QueueReportingMode(remote);
        return;
    }
    if (id == 0x21 && size >= 6) {
        // Read reply; a DolphinBar sends it at its exact length, not padded to 22.
        remote.last_report = now;
        if (remote.waiting && remote.current.wait == Wait::Read) {
            const int error = report[3] & 0x0f;
            const int bytes = std::min(((report[3] >> 4) & 0x0f) + 1, size - 6);
            if (!error) HandleRead(remote, remote.current.tag, report + 6, bytes);
            remote.waiting = false;
        }
        return;
    }
    if (id == 0x22 && size >= 5) {
        remote.last_report = now;
        if (remote.waiting && remote.current.wait == Wait::Ack && report[3] == remote.current.report[0])
            remote.waiting = false;
        return;
    }
    if (id >= 0x30 && id <= 0x3f) {
        remote.last_report = now;
        if (id == 0x37 && size >= 22 && remote.ready) Publish(remote, report);
    }
}

void Pump(Remote& remote, Clock::time_point now) {
    if (now < remote.retry_at) return;
    if (remote.waiting && now < remote.wait_deadline) return;
    if (remote.waiting && Verbose())
        SDL_Log("Wii Remote %s: no reply to %02x", remote.path.c_str(), remote.current.report[0]);
    remote.waiting = false;
    while (!remote.queue.empty()) {
        remote.current = std::move(remote.queue.front());
        remote.queue.pop_front();
        if (Verbose())
            SDL_Log("Wii Remote %s: send %02x %02x (%zu bytes)", remote.path.c_str(), remote.current.report[0],
                    remote.current.report.size() > 1 ? remote.current.report[1] : 0, remote.current.report.size());
        if (!Send(remote, remote.current.report)) {
            if (++remote.write_failures >= kWriteAttempts) { Lost(remote, "write failed"); return; }
            remote.queue.push_front(std::move(remote.current));
            remote.retry_at = now + kWriteRetry;
            return;
        }
        remote.write_failures = 0;
        if (remote.current.wait != Wait::None) {
            remote.waiting = true;
            remote.wait_deadline = now + kCommandWait;
            return;
        }
    }
    if (!remote.ready && remote.present) {
        remote.ready = true;
        AttachVirtual(remote);
    }
}

void ReadAll(Remote& remote, Clock::time_point now) {
    std::uint8_t buffer[32];
    for (int guard = 0; guard < 256 && remote.hid; ++guard) {
        const int size = SDL_hid_read_timeout(remote.hid, buffer, sizeof(buffer), 0);
        if (size == 0) return;
        if (size < 0) {
            // The adapter or Bluetooth link went away.
            Lost(remote, "device removed");
            SDL_hid_close(remote.hid);
            remote.hid = nullptr;
            return;
        }
        if (Verbose() && buffer[0] < 0x30)
            SDL_Log("Wii Remote %s: report %02x size %d [%02x %02x %02x %02x %02x]", remote.path.c_str(), buffer[0],
                    size, size > 1 ? buffer[1] : 0, size > 2 ? buffer[2] : 0, size > 3 ? buffer[3] : 0,
                    size > 4 ? buffer[4] : 0, size > 5 ? buffer[5] : 0);
        HandleReport(remote, buffer, size, now);
    }
}

void UpdateLeds(Remote& remote) {
    if (!remote.ready || !remote.virtual_id) return;
    const int channel = mscharged::platform::GetNativeWpadChannel(remote.virtual_id);
    if (channel == remote.led_channel) return;
    remote.led_channel = channel;
    const std::uint8_t leds = channel >= 0 && channel < 4 ? std::uint8_t(0x10u << channel) : std::uint8_t(0xf0);
    Send(remote, {0x11, leds});
}

void Enumerate(Driver& driver) {
    SDL_hid_device_info* devices = SDL_hid_enumerate(kNintendo, 0);
    std::vector<std::string> seen;
    int adapter_index = 0;
    for (auto* info = devices; info; info = info->next) {
        if (!mscharged::platform::IsWiimoteHid(info) || !info->path) continue;
        seen.emplace_back(info->path);
        const bool adapter = mscharged::platform::IsDolphinBarHid(info);
        const int slot = adapter ? (info->interface_number >= 0 && info->interface_number < 4
                                        ? info->interface_number : adapter_index) : -1;
        if (adapter) ++adapter_index;
        auto known = std::find_if(driver.remotes.begin(), driver.remotes.end(),
                                  [&](const auto& remote) { return remote->path == info->path; });
        if (known != driver.remotes.end()) {
            if (!(*known)->hid) {
                (*known)->hid = SDL_hid_open_path(info->path);
                if ((*known)->hid) SDL_hid_set_nonblocking((*known)->hid, 1);
            }
            continue;
        }
        auto remote = std::make_unique<Remote>();
        remote->path = info->path;
        remote->adapter = adapter;
        remote->slot = slot;
        remote->hid = SDL_hid_open_path(info->path);
        if (remote->hid) SDL_hid_set_nonblocking(remote->hid, 1);
        else SDL_Log("Wii Remote HID %s could not be opened: %s", info->path, SDL_GetError());
        driver.remotes.push_back(std::move(remote));
    }
    SDL_hid_free_enumeration(devices);
    const bool other_mode = !adapter_index && mscharged::platform::DolphinBarInOtherMode();
    if (other_mode && !driver.reported_other_mode)
        SDL_Log("The DolphinBar is in a mouse or gamepad mode; press its MODE button until light 4 is on");
    driver.reported_other_mode = other_mode;
    // Retire paths that disappeared (adapter unplugged, Bluetooth link gone).
    for (auto it = driver.remotes.begin(); it != driver.remotes.end();) {
        if (std::find(seen.begin(), seen.end(), (*it)->path) != seen.end()) { ++it; continue; }
        Lost(**it, "device removed");
        if ((*it)->hid) SDL_hid_close((*it)->hid);
        it = driver.remotes.erase(it);
    }
    // Remotes take players in slot order: adapter slots first, then others.
    std::stable_sort(driver.remotes.begin(), driver.remotes.end(), [](const auto& a, const auto& b) {
        const int sa = a->slot < 0 ? 100 : a->slot, sb = b->slot < 0 ? 100 : b->slot;
        return sa < sb;
    });
}

void ServiceRemote(Remote& remote, Clock::time_point now) {
    if (!remote.hid) return;
    ReadAll(remote, now);
    if (!remote.hid) return;
    if (!remote.present) {
        if (remote.probing && now >= remote.probe_deadline) {
            remote.probing = false;
            remote.next_probe = now + kProbeInterval;
        }
        if (!remote.probing && now >= remote.next_probe) {
            // An empty DolphinBar slot rejects the write (EPIPE); ask again later.
            if (!Send(remote, {0x15, 0x00})) {
                remote.next_probe = now + kProbeInterval;
                return;
            }
            remote.probing = true;
            remote.probe_deadline = now + kProbeWait;
        }
        return;
    }
    Pump(remote, now);
    if (remote.ready && now - remote.last_report > kSilentLimit) {
        Lost(remote, "no reports");
        return;
    }
    UpdateLeds(remote);
}

void RequireOwner() {
    if (State().owner != std::this_thread::get_id())
        throw std::logic_error("Wii Remote HID driver requires its input owner thread");
}
} // namespace

namespace mscharged::platform {
void InitializeWiimoteHid(WiimoteHidSettings settings) {
    auto& driver = State();
    if (driver.initialized) return;
    if (SDL_hid_init() != 0) {
        SDL_Log("Wii Remote HID unavailable: %s", SDL_GetError());
        return;
    }
    driver.settings = settings;
    driver.owner = std::this_thread::get_id();
    driver.initialized = true;
    Enumerate(driver);
    driver.next_enumerate = Clock::now() + kEnumerateInterval;
    // Remotes that are already on answer within a few milliseconds and take
    // the first players before the keyboard device is created.
    const auto deadline = Clock::now() + 1500ms;
    while (Clock::now() < deadline) {
        const auto now = Clock::now();
        bool pending = false;
        for (auto& remote : driver.remotes) {
            ServiceRemote(*remote, now);
            pending |= remote->probing || (remote->present && !remote->ready);
        }
        if (!pending) break;
        std::this_thread::sleep_for(5ms);
    }
}

void ServiceWiimoteHid() {
    auto& driver = State();
    if (!driver.initialized) return;
    RequireOwner();
    const auto now = Clock::now();
    if (now >= driver.next_enumerate) {
        Enumerate(driver);
        driver.next_enumerate = now + kEnumerateInterval;
    }
    for (auto& remote : driver.remotes) ServiceRemote(*remote, now);
}

void ShutdownWiimoteHid() {
    auto& driver = State();
    if (!driver.initialized) return;
    RequireOwner();
    for (auto& remote : driver.remotes) {
        if (remote->rumble) { remote->rumble = false; Send(*remote, {0x10, 0x00}); }
        DetachVirtual(*remote);
        if (remote->hid) SDL_hid_close(remote->hid);
        remote->hid = nullptr;
    }
    driver.remotes.clear();
    driver.initialized = false;
    driver.owner = {};
    SDL_hid_exit();
}

NativeDpdObservation DecodeWiimoteBasicIr(const std::uint8_t* ir) {
    // Two 5-byte groups of two objects with 10-bit coordinates, converted like
    // original WPADHIDParser (WPAD_DPD_BASIC): x as sent, y = 767 - sent y;
    // x 1023 or y 767 means no object (0, 767, size 0), a present one has size 12.
    NativeDpdObservation dots{};
    for (int group = 0; group < 2; ++group) {
        const std::uint8_t* bytes = ir + group * 5;
        const int x[2]{bytes[0] | ((bytes[2] >> 4) & 3) << 8, bytes[3] | (bytes[2] & 3) << 8};
        const int y[2]{bytes[1] | ((bytes[2] >> 6) & 3) << 8, bytes[4] | ((bytes[2] >> 2) & 3) << 8};
        for (int n = 0; n < 2; ++n) {
            auto& object = dots[group * 2 + n];
            object.trace_id = static_cast<std::uint8_t>(group * 2 + n);
            const int flipped = 767 - y[n];
            const bool absent = x[n] == 1023 || flipped == 767;
            object.x = static_cast<std::int16_t>(absent ? 0 : x[n]);
            object.y = static_cast<std::int16_t>(absent ? 767 : flipped);
            object.size = absent ? 0 : 12;
        }
    }
    return dots;
}

WiimoteHidStatus GetWiimoteHidStatus() {
    WiimoteHidStatus status{};
    for (const auto& remote : State().remotes) {
        if (remote->adapter) { status.dolphinbar = true; ++status.adapter_slots; }
        if (!remote->ready) continue;
        status.remotes.push_back({remote->path, remote->slot, remote->led_channel >= 0 ? remote->led_channel : -1,
                                  remote->nunchuk, remote->ir_visible, std::min(100, remote->battery * 100 / 0xc8)});
    }
    return status;
}

} // namespace mscharged::platform

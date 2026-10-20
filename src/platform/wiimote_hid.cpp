#include "platform/wiimote_hid.h"
#include "platform/desktop_dpd.h"
#include "platform/desktop_nunchuk.h"
#include "platform/wpad_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::uint16_t kNintendo = 0x057e, kRemote = 0x0306, kRemotePlus = 0x0330;
constexpr float kGravity = 9.80665f;
constexpr std::chrono::milliseconds kProbeWait{600}, kProbeInterval{2000};
constexpr std::chrono::milliseconds kSilentLimit{1500}, kCommandWait{250};
// A scan lists every HID device of the system: Windows opens and queries each
// one, which held the game thread for about 70 ms every 2 s with a Bluetooth
// remote. Scans therefore follow SDL's device change notifications; a listed
// remote that could not be opened yet is opened again this often, and a scan
// slower than kSlowScan is logged.
constexpr std::chrono::milliseconds kOpenRetry{2000}, kSlowScan{20};
// Another program can switch a directly connected remote to its own report
// mode: the Linux hid-wiimote driver answers every status report with buttons
// only, sent on change. A remote that sends another mode gets its set-up
// again, at most this often; one silent for kDataGap gets the game's
// continuous reporting mode back.
constexpr std::chrono::milliseconds kModeRestore{500}, kDataGap{100};
// A DolphinBar slot can reject a write while its Bluetooth link is busy; only
// repeated failures mean the remote is gone.
constexpr std::chrono::milliseconds kWriteRetry{20};
constexpr int kWriteAttempts = 10;


enum class Wait { None, Ack, Read };
enum class Tag { None, Calibration, ExtensionId, NunchukCalibration };
struct Command {
    std::vector<std::uint8_t> report; // byte 1 bit 0 carries the rumble state at send time
    Wait wait = Wait::None;
    Tag tag = Tag::None;
    // Last report of a WPAD speaker command: WPAD hears once it went out.
    bool speaker_done = false;
};

// Every write to a remote goes out on a thread of its own, the remote's
// writer: one write over Windows Bluetooth can take 200 ms while the link is
// busy, and the game thread must never wait for that. The owner's reports go
// out in the order it hands them over, before speaker frames that wait.
// Original WPAD sends a speaker frame every 6.7 ms. A few frames may wait, so
// one slow write does not cost the game a frame (WPAD's stream check would
// refuse it and the Remote would play a gap).
constexpr std::size_t kSpeakerQueue = 3;
// A failed write loses a frame the game has already encoded: the Remote's
// ADPCM decoder then works from another state than the game's encoder and
// plays noise until the next silence. A busy DolphinBar link rejects writes
// for a moment, so a frame is tried again before it is given up.
constexpr int kSpeakerAttempts = 3;
// While the game keeps the speaker muted (between its speaker sounds) its
// frames are not sent, which keeps the link free for buttons and pointer. The
// first frames after the mute still go out: they end the sound and its
// silence brings the Remote's decoder to the same state as the encoder, so
// the next sound starts in step.
constexpr int kSpeakerMuteTail = 8;
constexpr std::chrono::seconds kSpeakerReport{30};
using Frame = std::array<std::uint8_t, 22>;
struct Writer {
    struct Report {
        std::vector<std::uint8_t> bytes; // byte 1 bit 0 gets the rumble state when it goes out
        std::uint64_t sequence = 0;      // nonzero: the owner wants the result
        int attempts = 1;                // a rejected write is tried again kWriteRetry apart
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Report> reports;
    std::deque<Frame> frames;
    bool frame_out = false; // a frame is being written: it counts against the queue
    std::deque<std::pair<std::uint64_t, bool>> results; // sequence, sent
    bool stop = false, drain = false;
    // Speaker frames since the last report, under the mutex.
    unsigned sent = 0, lost = 0, retried = 0;
    std::chrono::nanoseconds write_total{}, write_max{};
    std::thread thread;
};

struct Remote {
    std::string path;
    std::string id;       // settings name (wiimote_ids.h)
    bool adapter = false; // DolphinBar interface
    bool plus = false;    // Wii Remote Plus
    int slot = -1;
    SDL_hid_device* hid = nullptr;
    bool present = false, probing = false, ready = false;
    Clock::time_point next_probe{}, probe_deadline{}, last_report{};
    std::deque<Command> queue;
    bool waiting = false;
    Command current;
    Clock::time_point wait_deadline{};
    // Read by the writer: every output report carries the rumble bit.
    std::atomic<bool> rumble{false};
    std::unique_ptr<Writer> writer; // while the HID device is open
    // Owner reports handed to the writer are numbered from next_write; results
    // of those before first_write belong to a remote that was lost since.
    std::uint64_t next_write = 1, first_write = 1;
    std::uint64_t awaited_write = 0; // the command waiting for its reply, until it went out
    std::uint64_t probe_write = 0;   // the status request of a probe, until it went out
    bool speaker = false; // WPAD turned the speaker on (and not off since)
    bool speaker_muted = false;
    int speaker_tail = 0;     // frames still sent after the mute
    int speaker_commands = 0; // WPAD speaker commands still going out
    unsigned speaker_late = 0, speaker_quiet = 0; // since the last report
    Clock::time_point next_speaker_report{};
    int zero[3]{512, 512, 512}, one[3]{612, 612, 612};
    bool extension = false, nunchuk = false;
    int nunchuk_zero[3]{512, 512, 512}, nunchuk_one[3]{712, 712, 712}, nunchuk_centre[2]{128, 128};
    std::uint8_t battery = 0;
    bool ir_visible = false;
    Clock::time_point next_debug_log{};
    int led_channel = -2;
    Clock::time_point next_mode_restore{}, last_data{}, next_data_restore{};
    bool sensors = false;
    SDL_JoystickID virtual_id = 0;
    SDL_Joystick* joystick = nullptr;
    mscharged::platform::NativeDpdSource dpd{};
    mscharged::platform::NativeNunchukSource nunchuk_source{};
};

bool Verbose() { return mscharged::platform::WiimoteDebug(); }

struct Driver {
    bool initialized = false;
    std::thread::id owner;
    mscharged::platform::WiimoteHidSettings settings;
    std::vector<std::unique_ptr<Remote>> remotes;
    std::uint32_t device_changes = 0; // SDL_hid_device_change_count() at the last scan
    unsigned scans = 0;
    Clock::time_point next_open_retry{};
    bool reported_other_mode = false;
};
Driver& State() { static Driver driver; return driver; }

// "DolphinBar slot 2" or "Bluetooth 9C:E6:35:C6:90:76", for logs.
std::string Describe(const Remote& remote) {
    if (remote.adapter) return "DolphinBar slot " + std::to_string(remote.slot + 1);
    const auto address = mscharged::platform::WiimoteBluetoothAddress(remote.id);
    return address.empty() ? "connected directly" : "Bluetooth " + address;
}

bool Write(const Remote& remote, SDL_hid_device* hid, std::uint8_t* report, std::size_t size) {
    report[1] = static_cast<std::uint8_t>((report[1] & ~1u) | (remote.rumble ? 1u : 0u));
    return mscharged::platform::WriteWiimoteReport(hid, report, size);
}

void RunWriter(Remote* remote, SDL_hid_device* hid) {
    auto& writer = *remote->writer;
    std::unique_lock lock(writer.mutex);
    for (;;) {
        writer.wake.wait(lock, [&] { return writer.stop || !writer.reports.empty() || !writer.frames.empty(); });
        if (writer.stop && (!writer.drain || writer.reports.empty())) return;
        if (!writer.reports.empty()) {
            auto report = std::move(writer.reports.front());
            writer.reports.pop_front();
            lock.unlock();
            bool sent = false;
            for (int attempt = 0; !sent && attempt < report.attempts; ++attempt) {
                if (attempt) std::this_thread::sleep_for(kWriteRetry);
                sent = Write(*remote, hid, report.bytes.data(), report.bytes.size());
            }
            lock.lock();
            if (report.sequence) writer.results.emplace_back(report.sequence, sent);
            continue;
        }
        if (writer.stop) return;
        auto frame = writer.frames.front();
        writer.frames.pop_front();
        writer.frame_out = true;
        lock.unlock();
        const auto start = Clock::now();
        bool sent = false;
        int attempt = 0;
        while (!sent && attempt < kSpeakerAttempts) {
            if (attempt++) SDL_DelayPrecise(1'000'000);
            sent = Write(*remote, hid, frame.data(), frame.size());
        }
        const auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
        lock.lock();
        writer.frame_out = false;
        // A remote that is gone shows on the owner's side (failed commands,
        // no reports); here the frame is only counted.
        ++(sent ? writer.sent : writer.lost);
        if (sent && attempt > 1) ++writer.retried;
        writer.write_total += took;
        writer.write_max = std::max(writer.write_max, took);
    }
}
void StartWriter(Remote& remote) {
    if (remote.writer || !remote.hid) return;
    remote.writer = std::make_unique<Writer>();
    remote.writer->thread = std::thread(RunWriter, &remote, remote.hid);
}
// Before the HID device closes: waits for the write in progress. With `drain`
// the reports still waiting go out first (rumble off at shutdown), frames not.
void StopWriter(Remote& remote, bool drain) {
    if (!remote.writer) return;
    {
        std::lock_guard lock(remote.writer->mutex);
        remote.writer->stop = true;
        remote.writer->drain = drain;
    }
    remote.writer->wake.notify_one();
    remote.writer->thread.join();
    remote.writer.reset();
}
void CloseHid(Remote& remote, bool drain = false) {
    StopWriter(remote, drain);
    if (remote.hid) SDL_hid_close(remote.hid);
    remote.hid = nullptr;
}
// Hands a report to the writer. A nonzero `sequence` brings its result back
// (TakeWriteResults); `attempts` above 1 tries a rejected write again.
bool Post(Remote& remote, std::vector<std::uint8_t> report, std::uint64_t sequence = 0, int attempts = 1) {
    if (!remote.writer || report.size() < 2) return false;
    {
        std::lock_guard lock(remote.writer->mutex);
        remote.writer->reports.push_back({std::move(report), sequence, attempts});
    }
    remote.writer->wake.notify_one();
    return true;
}
// A remote that is gone gets nothing that still waits for it, and the results
// of what went out before are dropped.
void ClearWriter(Remote& remote) {
    remote.first_write = remote.next_write;
    remote.awaited_write = remote.probe_write = 0;
    if (!remote.writer) return;
    std::lock_guard lock(remote.writer->mutex);
    remote.writer->reports.clear();
    remote.writer->frames.clear();
    remote.writer->results.clear();
}
void CompleteSpeaker(Remote& remote, bool ok) {
    if (remote.speaker_commands > 0) --remote.speaker_commands;
    if (remote.virtual_id) mscharged::platform::CompleteNativeWpadSpeaker(remote.virtual_id, ok);
}
// How the speaker data fared, for testers' logs: always when frames were lost
// or came too late, else with RECHARGED_WIIMOTE_DEBUG.
void ReportSpeaker(Remote& remote, Clock::time_point now) {
    if (!remote.speaker || !remote.writer || now < remote.next_speaker_report) return;
    const bool first = remote.next_speaker_report == Clock::time_point{};
    remote.next_speaker_report = now + kSpeakerReport;
    if (first) return;
    unsigned sent, lost, retried;
    std::chrono::nanoseconds total, max;
    {
        std::lock_guard lock(remote.writer->mutex);
        sent = remote.writer->sent, lost = remote.writer->lost, retried = remote.writer->retried;
        total = remote.writer->write_total, max = remote.writer->write_max;
        remote.writer->sent = remote.writer->lost = remote.writer->retried = 0;
        remote.writer->write_total = remote.writer->write_max = {};
    }
    const unsigned late = remote.speaker_late, quiet = remote.speaker_quiet;
    remote.speaker_late = remote.speaker_quiet = 0;
    const bool trouble = lost || late;
    if (!trouble && !Verbose()) return;
    const auto ms = [](std::chrono::nanoseconds value) { return double(value.count()) / 1e6; };
    SDL_Log("Wii Remote (%s) speaker, last %d s: %u frames sent (%u after a retry), %u lost, %u too late, "
            "%u not sent while muted; a write took %.1f ms on average, %.1f ms at most%s",
            Describe(remote).c_str(), int(kSpeakerReport.count()), sent, retried, lost, late, quiet,
            sent + lost ? ms(total) / (sent + lost) : 0.0, ms(max),
            trouble ? ": the connection is too slow for clean speaker sound" : "");
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
// A speaker still running from the last game start (the Remote stayed
// connected) plays the next game's sound with its old format and decoder
// state: noise or clicks.
void QueueSpeakerReset(Remote& remote) {
    for (auto& report : mscharged::platform::WiimoteSpeakerResetReports()) {
        const Wait wait = report[0] == 0x16 ? Wait::Ack : Wait::None;
        Queue(remote, std::move(report), wait);
    }
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
    QueueWrite(remote, 0xb00000, mscharged::platform::kWiimoteIrBlock1[level], 9);
    QueueWrite(remote, 0xb0001a, mscharged::platform::kWiimoteIrBlock2[level], 2);
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
    return Post(remote, {0x10, 0x00});
}
bool SDLCALL VirtualSensors(void* userdata, bool enabled) {
    static_cast<Remote*>(userdata)->sensors = enabled;
    return true;
}

// The fixed WPAD channel of this remote's player, -1 for none, or -2 when
// the remote is not a player.
int PlayerChannel(const Remote& remote) {
    const auto& settings = State().settings;
    if (!settings.fixed_players) return -1;
    const auto it = settings.channels.find(remote.id);
    return it != settings.channels.end() && it->second >= 0 ? it->second : -2;
}

void AttachVirtual(Remote& remote) {
    if (remote.virtual_id) return;
    const int channel = PlayerChannel(remote);
    if (channel == -2) {
        // Connected but not a player: LEDs off.
        if (remote.led_channel != -3) {
            remote.led_channel = -3;
            Post(remote, {0x11, 0x00});
            SDL_Log("Wii Remote (%s, %s) is not assigned to a player", Describe(remote).c_str(), remote.id.c_str());
        }
        return;
    }
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
    if (channel >= 0) mscharged::platform::SetNativeWpadFixedChannel(remote.virtual_id, channel);
    remote.dpd = mscharged::platform::AttachNativeWpadDpdSource(remote.virtual_id);
    if (remote.nunchuk)
        remote.nunchuk_source = mscharged::platform::AttachNativeWpadNunchukSource(remote.virtual_id);
    SDL_Log("Wii Remote%s connected via %s%s%s", remote.plus ? " Plus" : "",
            remote.adapter ? Describe(remote).c_str() : (Describe(remote) + " (" + remote.id + ")").c_str(),
            remote.nunchuk ? " with Nunchuk" : "",
            channel >= 0 ? (" as player " + std::to_string(channel + 1)).c_str() : "");
}

void DetachVirtual(Remote& remote) {
    if (remote.nunchuk_source.generation) mscharged::platform::DetachNativeWpadNunchukSource(remote.nunchuk_source);
    remote.nunchuk_source = {};
    if (remote.dpd.generation) mscharged::platform::DetachNativeWpadDpdSource(remote.dpd);
    remote.dpd = {};
    if (remote.joystick) SDL_CloseJoystick(remote.joystick);
    remote.joystick = nullptr;
    if (remote.virtual_id) mscharged::platform::SetNativeWpadFixedChannel(remote.virtual_id, -1);
    if (remote.virtual_id) SDL_DetachVirtualJoystick(remote.virtual_id);
    remote.virtual_id = 0;
    remote.led_channel = -2;
}

void Lost(Remote& remote, const char* why) {
    if (remote.present) SDL_Log("Wii Remote disconnected (%s)", why);
    DetachVirtual(remote);
    remote.present = remote.ready = remote.probing = remote.waiting = false;
    remote.queue.clear();
    remote.extension = remote.nunchuk = false;
    remote.rumble = false;
    remote.speaker = remote.speaker_muted = false;
    remote.speaker_tail = remote.speaker_commands = 0;
    ClearWriter(remote);
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
            QueueSpeakerReset(remote);
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
        if (remote.waiting && remote.current.wait == Wait::Ack && report[3] == remote.current.report[0]) {
            remote.waiting = false;
            // Speaker registers are 0xa2xxxx; a refused setting leaves its decoder
            // playing the data wrongly, so say so in testers' logs.
            if (report[4] && remote.current.report[0] == 0x16 && remote.current.report[2] == 0xa2)
                SDL_Log("Wii Remote (%s): the speaker refused register %02x%02x%02x (error %d)",
                        Describe(remote).c_str(), remote.current.report[2], remote.current.report[3],
                        remote.current.report[4], report[4]);
            if (remote.current.speaker_done) CompleteSpeaker(remote, report[4] == 0);
        }
        return;
    }
    if (id >= 0x30 && id <= 0x3f) {
        remote.last_report = remote.last_data = now;
        if (id == 0x37 && size >= 22 && remote.ready) Publish(remote, report);
        else if (id != 0x37 && remote.ready && !remote.waiting && remote.queue.empty() &&
                 now >= remote.next_mode_restore) {
            remote.next_mode_restore = now + kModeRestore;
            SDL_Log("Wii Remote (%s) switched to report mode %02x by another program; restoring the game's",
                    Describe(remote).c_str(), id);
            QueueSetup(remote);
        }
    }
}

// Results of the owner's reports the writer finished. A command the writer
// could not get out even after kWriteAttempts tries means the remote is gone;
// an awaited reply is timed from when its command went out. False: lost.
bool TakeWriteResults(Remote& remote, Clock::time_point now) {
    if (!remote.writer) return true;
    std::deque<std::pair<std::uint64_t, bool>> results;
    {
        std::lock_guard lock(remote.writer->mutex);
        results.swap(remote.writer->results);
    }
    for (const auto& [sequence, sent] : results) {
        if (sequence < remote.first_write) continue;
        if (sequence == remote.probe_write) {
            // An empty DolphinBar slot rejects the write (EPIPE); ask again later.
            remote.probe_write = 0;
            if (!sent && remote.probing) {
                remote.probing = false;
                remote.next_probe = now + kProbeInterval;
            }
            continue;
        }
        if (!sent) {
            Lost(remote, "write failed");
            return false;
        }
        if (sequence == remote.awaited_write) {
            remote.awaited_write = 0;
            remote.wait_deadline = now + kCommandWait;
        }
    }
    return true;
}

void Pump(Remote& remote, Clock::time_point now) {
    if (!TakeWriteResults(remote, now) || remote.awaited_write) return;
    if (remote.waiting && now < remote.wait_deadline) return;
    if (remote.waiting && Verbose())
        SDL_Log("Wii Remote %s: no reply to %02x", remote.path.c_str(), remote.current.report[0]);
    if (remote.waiting && remote.current.speaker_done) {
        SDL_Log("Wii Remote (%s): no reply to a speaker command", Describe(remote).c_str());
        CompleteSpeaker(remote, false);
    }
    remote.waiting = false;
    while (!remote.queue.empty()) {
        remote.current = std::move(remote.queue.front());
        remote.queue.pop_front();
        if (Verbose())
            SDL_Log("Wii Remote %s: send %02x %02x (%zu bytes)", remote.path.c_str(), remote.current.report[0],
                    remote.current.report.size() > 1 ? remote.current.report[1] : 0, remote.current.report.size());
        const std::uint64_t sequence = remote.next_write++;
        if (!Post(remote, remote.current.report, sequence, kWriteAttempts)) {
            Lost(remote, "write failed");
            return;
        }
        if (remote.current.wait != Wait::None) {
            // A reply can come before Pump hears that the command went out.
            remote.waiting = true;
            remote.awaited_write = sequence;
            return;
        }
        // It goes out before any speaker frame handed over later, so WPAD
        // hears of it now and the frames that follow may come.
        if (remote.current.speaker_done) CompleteSpeaker(remote, true);
    }
    if (!remote.ready && remote.present) {
        remote.ready = true;
        remote.last_data = now;
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
            CloseHid(remote);
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
    Post(remote, {0x11, leds});
}

bool Open(Remote& remote) {
    remote.hid = SDL_hid_open_path(remote.path.c_str());
    if (!remote.hid) return false;
    SDL_hid_set_nonblocking(remote.hid, 1);
    StartWriter(remote);
    return true;
}

void Enumerate(Driver& driver) {
    const auto start = Clock::now();
    ++driver.scans;
    SDL_hid_device_info* devices = SDL_hid_enumerate(kNintendo, 0);
    std::vector<std::string> seen;
    int adapter_index = 0;
    for (auto* info = devices; info; info = info->next) {
        if (!mscharged::platform::IsWiimoteHid(info) || !info->path) continue;
        seen.emplace_back(info->path);
        const bool adapter = mscharged::platform::IsDolphinBarHid(info);
        const auto id = mscharged::platform::WiimoteHidId(info, adapter_index);
        auto known = std::find_if(driver.remotes.begin(), driver.remotes.end(),
                                  [&](const auto& remote) { return remote->path == info->path; });
        if (known != driver.remotes.end()) {
            if (!(*known)->hid) Open(**known);
            continue;
        }
        auto remote = std::make_unique<Remote>();
        remote->path = info->path;
        remote->id = id;
        remote->adapter = adapter;
        remote->plus = info->product_id == kRemotePlus;
        remote->slot = mscharged::platform::WiimoteDolphinBarSlot(id);
        if (!Open(*remote))
            SDL_Log("Wii Remote HID %s could not be opened: %s%s", info->path, SDL_GetError(),
#if defined(__linux__)
                    " (no permission? Install the udev rule from docs/SETTINGS.md, Wii Remotes)"
#else
                    ""
#endif
                    );
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
        CloseHid(**it);
        it = driver.remotes.erase(it);
    }
    // Remotes take players in slot order: adapter slots first, then directly
    // connected remotes by address.
    std::stable_sort(driver.remotes.begin(), driver.remotes.end(), [](const auto& a, const auto& b) {
        const int sa = a->slot < 0 ? 100 : a->slot, sb = b->slot < 0 ? 100 : b->slot;
        return sa != sb ? sa < sb : a->id < b->id;
    });
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    if (Verbose() || took >= kSlowScan)
        SDL_Log("Wii Remote: scanning the HID devices took %d ms", static_cast<int>(took.count()));
}

void ServiceRemote(Remote& remote, Clock::time_point now) {
    if (!remote.hid) return;
    ReadAll(remote, now);
    if (!remote.hid) return;
    if (!remote.present) {
        if (!TakeWriteResults(remote, now)) return;
        if (remote.probing && now >= remote.probe_deadline) {
            remote.probing = false;
            remote.next_probe = now + kProbeInterval;
        }
        if (!remote.probing && now >= remote.next_probe) {
            // A rejected status request ends the probe early (TakeWriteResults).
            remote.probe_write = remote.next_write++;
            if (!Post(remote, {0x15, 0x00}, remote.probe_write)) {
                remote.probe_write = 0;
                remote.next_probe = now + kProbeInterval;
                return;
            }
            remote.probing = true;
            remote.probe_deadline = now + kProbeWait;
        }
        return;
    }
    Pump(remote, now);
    if (remote.ready && !remote.waiting && remote.queue.empty() && now - remote.last_data > kDataGap &&
        now >= remote.next_data_restore) {
        remote.next_data_restore = now + kDataGap;
        if (Verbose()) SDL_Log("Wii Remote (%s): no data reports; restoring the reporting mode", Describe(remote).c_str());
        QueueReportingMode(remote);
        Pump(remote, now);
    }
    if (remote.ready && now - remote.last_report > kSilentLimit) {
        Lost(remote, "no reports");
        return;
    }
    UpdateLeds(remote);
    ReportSpeaker(remote, now);
}

void RequireOwner() {
    if (State().owner != std::this_thread::get_id())
        throw std::logic_error("Wii Remote HID driver requires its input owner thread");
}

// The ready remote behind a virtual joystick, when remotes offer their speaker.
Remote* SpeakerRemote(std::uint32_t joystick_id) {
    auto& driver = State();
    if (!driver.initialized || !driver.settings.speaker || !joystick_id) return nullptr;
    RequireOwner();
    for (auto& remote : driver.remotes)
        if (remote->ready && remote->virtual_id == joystick_id) return remote.get();
    return nullptr;
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
    driver.scans = 0;
    driver.device_changes = SDL_hid_device_change_count();
    Enumerate(driver);
    driver.next_open_retry = Clock::now() + kOpenRetry;
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
    // Read before the scan, so a device that arrives during it brings another.
    const std::uint32_t changes = SDL_hid_device_change_count();
    if (changes != driver.device_changes) {
        driver.device_changes = changes;
        Enumerate(driver);
    } else if (now >= driver.next_open_retry) {
        driver.next_open_retry = now + kOpenRetry;
        for (auto& remote : driver.remotes)
            if (!remote->hid) Open(*remote);
    }
    for (auto& remote : driver.remotes) ServiceRemote(*remote, now);
}

void SetWiimoteHidChannels(std::map<std::string, int> channels) {
    auto& driver = State();
    if (!driver.initialized) return;
    RequireOwner();
    driver.settings.channels = std::move(channels);
    for (auto& remote : driver.remotes) {
        if (!remote->virtual_id) continue;
        const int channel = PlayerChannel(*remote);
        if (channel >= 0) mscharged::platform::SetNativeWpadFixedChannel(remote->virtual_id, channel);
    }
}

void ShutdownWiimoteHid() {
    auto& driver = State();
    if (!driver.initialized) return;
    RequireOwner();
    for (auto& remote : driver.remotes) {
        ClearWriter(*remote);
        if (remote->rumble) { remote->rumble = false; Post(*remote, {0x10, 0x00}); }
        DetachVirtual(*remote);
        // Only the rumble off still waits to go out.
        CloseHid(*remote, true);
    }
    driver.remotes.clear();
    driver.initialized = false;
    driver.owner = {};
    SDL_hid_exit();
}

std::vector<std::vector<std::uint8_t>> WiimoteSpeakerReports(WiimoteSpeakerCommand command, std::uint8_t volume,
                                                             std::uint8_t reg01) {
    // Register writes (0x16): control registers, 3 address bytes, length,
    // 16 data bytes. 0xa20001 is the speaker configuration, 0xa20008 starts
    // playing, 0xa20009 powers the speaker's decoder.
    const auto write = [](std::uint32_t address, std::initializer_list<std::uint8_t> data) {
        std::vector<std::uint8_t> report(22, 0);
        report[0] = 0x16;
        report[1] = 0x04;
        report[2] = static_cast<std::uint8_t>(address >> 16);
        report[3] = static_cast<std::uint8_t>(address >> 8);
        report[4] = static_cast<std::uint8_t>(address);
        report[5] = static_cast<std::uint8_t>(data.size());
        std::copy(data.begin(), data.end(), report.begin() + 6);
        return report;
    };
    const std::vector<std::uint8_t> enable{0x14, 0x04}, disable{0x14, 0x00}, mute{0x19, 0x04}, unmute{0x19, 0x00};
    switch (command) {
    case WiimoteSpeakerCommand::Off:
        return {mute, write(0xa20001, {0x01}), write(0xa20009, {0x00}), disable};
    case WiimoteSpeakerCommand::On:
        // 4-bit ADPCM, rate 0x07d0, the volume, then WPAD's last two bytes,
        // after the game's own byte for 0xa20001 (the American game's WPAD
        // writes 0x80, the European one's 0x08).
        return {enable, mute, write(0xa20009, {0x01}), write(0xa20001, {reg01}),
                write(0xa20001, {0x00, 0x00, 0xd0, 0x07, std::min<std::uint8_t>(volume, 127), 0x0c, 0x0e}), unmute};
    case WiimoteSpeakerCommand::Mute: return {mute};
    case WiimoteSpeakerCommand::Unmute: return {unmute};
    case WiimoteSpeakerCommand::Play: return {write(0xa20008, {0x01})};
    }
    return {};
}

std::vector<std::vector<std::uint8_t>> WiimoteSpeakerResetReports() {
    auto reports = WiimoteSpeakerReports(WiimoteSpeakerCommand::Off, 0);
    reports.insert(reports.begin(), {0x14, 0x04});
    return reports;
}

bool WiimoteHidHasSpeaker(std::uint32_t joystick_id) { return SpeakerRemote(joystick_id) != nullptr; }

bool WiimoteHidControlSpeaker(std::uint32_t joystick_id, WiimoteSpeakerCommand command, std::uint8_t volume) {
    auto* remote = SpeakerRemote(joystick_id);
    if (!remote) return false;
    auto reports = WiimoteSpeakerReports(command, volume, State().settings.speaker_reg01);
    if (reports.empty()) return false;
    for (std::size_t n = 0; n < reports.size(); ++n) {
        const bool written = reports[n][0] == 0x16;
        remote->queue.push_back({std::move(reports[n]), written ? Wait::Ack : Wait::None, Tag::None,
                                 n + 1 == reports.size()});
    }
    ++remote->speaker_commands;
    // The sequences end unmuted (ON) or muted; data that follows waits for them.
    if (command == WiimoteSpeakerCommand::Mute) {
        if (!remote->speaker_muted) remote->speaker_tail = kSpeakerMuteTail;
        remote->speaker_muted = true;
    } else if (command == WiimoteSpeakerCommand::Unmute || command == WiimoteSpeakerCommand::On) {
        remote->speaker_muted = false;
    }
    if (command == WiimoteSpeakerCommand::On) {
        if (!remote->speaker)
            SDL_Log("Wii Remote (%s): speaker on, volume %d of 127", Describe(*remote).c_str(), std::min(127, int(volume)));
        remote->speaker = true;
    } else if (command == WiimoteSpeakerCommand::Off) {
        remote->speaker = false;
        if (remote->writer) {
            std::lock_guard lock(remote->writer->mutex);
            remote->writer->frames.clear();
        }
    }
    // To the writer at once (mute and unmute come with each sound), unless a
    // command still waits for its reply.
    Pump(*remote, Clock::now());
    return true;
}

namespace {
// A frame the writer takes now: the speaker is on, its commands went out and
// the frame is either not sent (muted) or finds room in the queue.
bool SpeakerTakesFrame(Remote* remote) {
    if (!remote || !remote->speaker || !remote->writer || remote->speaker_commands) return false;
    if (remote->speaker_muted && !remote->speaker_tail) return true;
    std::lock_guard lock(remote->writer->mutex);
    if (remote->writer->frames.size() + (remote->writer->frame_out ? 1 : 0) < kSpeakerQueue) return true;
    ++remote->speaker_late;
    return false;
}
} // namespace

bool WiimoteHidCanStreamSpeaker(std::uint32_t joystick_id) {
    return SpeakerTakesFrame(SpeakerRemote(joystick_id));
}

bool WiimoteHidStreamSpeaker(std::uint32_t joystick_id, const void* data, std::uint16_t size) {
    auto* remote = SpeakerRemote(joystick_id);
    if (!data || size > 20 || !SpeakerTakesFrame(remote)) return false;
    if (remote->speaker_muted) {
        if (!remote->speaker_tail) {
            ++remote->speaker_quiet;
            return true;
        }
        --remote->speaker_tail;
    }
    auto& writer = *remote->writer;
    {
        std::lock_guard lock(writer.mutex);
        Frame frame{};
        frame[0] = 0x18;
        frame[1] = static_cast<std::uint8_t>(size << 3);
        std::memcpy(frame.data() + 2, data, size);
        writer.frames.push_back(frame);
    }
    writer.wake.notify_one();
    return true;
}

bool WiimoteHidHasDevices() {
    const auto& driver = State();
    return driver.initialized && !driver.remotes.empty();
}

unsigned WiimoteHidScans() { return State().scans; }

WiimoteHidStatus GetWiimoteHidStatus() {
    WiimoteHidStatus status{};
    for (const auto& remote : State().remotes) {
        if (remote->adapter) { status.dolphinbar = true; ++status.adapter_slots; }
        if (!remote->ready) continue;
        status.remotes.push_back({remote->path, remote->id, remote->slot, remote->plus,
                                  remote->led_channel >= 0 ? remote->led_channel : -1, remote->nunchuk,
                                  remote->ir_visible, std::min(100, remote->battery * 100 / 0xc8)});
    }
    return status;
}

} // namespace mscharged::platform

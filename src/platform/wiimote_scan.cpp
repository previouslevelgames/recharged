#include "platform/wiimote_hid.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace mscharged::platform {
bool IsWiimoteHid(const SDL_hid_device_info* info) {
    return info->vendor_id == 0x057e && (info->product_id == 0x0306 || info->product_id == 0x0330);
}

bool IsDolphinBarHid(const SDL_hid_device_info* info) {
    // A Mayflash DolphinBar in mode 4 lists four Wii Remote interfaces with
    // its own manufacturer and product strings.
    const auto contains = [](const wchar_t* text, const wchar_t* needle) {
        return text && std::wcsstr(text, needle) != nullptr;
    };
    return contains(info->product_string, L"Mayflash") || contains(info->manufacturer_string, L"Mayflash") ||
           contains(info->product_string, L"DolphinBar") || contains(info->manufacturer_string, L"HJZ");
}

struct WiimoteHidProbe::Device {
    std::string path;
    SDL_hid_device* hid = nullptr;
    int slot = -1;
    bool answered = false, nunchuk = false;
    int battery = 0;
};

WiimoteHidProbe::~WiimoteHidProbe() { Close(); }

void WiimoteHidProbe::Close() {
    for (auto* device : devices_) {
        if (device->hid) SDL_hid_close(device->hid);
        delete device;
    }
    devices_.clear();
    if (initialized_) SDL_hid_exit();
    initialized_ = false;
    active_ = false;
}

void WiimoteHidProbe::Start(int timeout_ms) {
    Close();
    WiimoteHidStatus status{};
    if (SDL_hid_init() != 0) { result_ = status; return; }
    initialized_ = true;
    SDL_hid_device_info* list = SDL_hid_enumerate(0x057e, 0);
    int adapter_index = 0;
    for (auto* info = list; info; info = info->next) {
        if (!IsWiimoteHid(info) || !info->path) continue;
        const bool adapter = IsDolphinBarHid(info);
        int slot = -1;
        if (adapter) {
            status.dolphinbar = true;
            ++status.adapter_slots;
            slot = info->interface_number >= 0 && info->interface_number < 4 ? info->interface_number : adapter_index;
            ++adapter_index;
        }
        auto* hid = SDL_hid_open_path(info->path);
        if (!hid) continue;
        SDL_hid_set_nonblocking(hid, 1);
        // Status request; an empty DolphinBar slot rejects the write.
        const unsigned char request[2]{0x15, 0x00};
        if (SDL_hid_write(hid, request, sizeof(request)) != int(sizeof(request))) {
            SDL_hid_close(hid);
            continue;
        }
        devices_.push_back(new Device{info->path, hid, slot});
    }
    SDL_hid_free_enumeration(list);
    status.dolphinbar_other_mode = !status.dolphinbar && DolphinBarInOtherMode();
    result_ = status;
    deadline_ns_ = SDL_GetTicksNS() + std::uint64_t(timeout_ms) * 1000000u;
    active_ = true;
}

bool WiimoteHidProbe::Poll() {
    if (!active_) return true;
    bool pending = false;
    for (auto* device : devices_) {
        unsigned char buffer[32];
        int size;
        while ((size = SDL_hid_read_timeout(device->hid, buffer, sizeof(buffer), 0)) > 0) {
            if (buffer[0] == 0x20 && size >= 7) {
                device->answered = true;
                device->nunchuk = (buffer[3] & 0x02) != 0;
                device->battery = std::min(100, buffer[6] * 100 / 0xc8);
            }
        }
        pending |= !device->answered;
    }
    if (pending && SDL_GetTicksNS() < deadline_ns_) return false;
    auto status = result_;
    for (auto* device : devices_)
        if (device->answered)
            status.remotes.push_back({device->path, device->slot, -1, device->nunchuk, false, device->battery});
    std::stable_sort(status.remotes.begin(), status.remotes.end(), [](const auto& a, const auto& b) {
        return (a.slot < 0 ? 100 : a.slot) < (b.slot < 0 ? 100 : b.slot);
    });
    Close();
    result_ = status;
    return true;
}

bool DolphinBarInOtherMode() {
    bool found = false;
    SDL_hid_device_info* list = SDL_hid_enumerate(0x0079, 0x1803);
    for (auto* info = list; info; info = info->next) found |= IsDolphinBarHid(info);
    SDL_hid_free_enumeration(list);
    return found;
}

WiimoteHidStatus ScanWiimoteHid(int timeout_ms) {
    WiimoteHidProbe probe;
    probe.Start(timeout_ms);
    while (!probe.Poll()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return probe.Result();
}
WiimoteHidLive::~WiimoteHidLive() { Close(); }

void WiimoteHidLive::Close() {
    if (hid_) SDL_hid_close(hid_);
    hid_ = nullptr;
    if (initialized_) SDL_hid_exit();
    initialized_ = false;
    buttons_ = 0;
    dots_ = {};
    last_report_ns_ = 0;
}

bool WiimoteHidLive::Send(std::initializer_list<std::uint8_t> report) {
    // A DolphinBar slot can reject a write while its Bluetooth link is busy.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (SDL_hid_write(hid_, report.begin(), report.size()) == int(report.size())) return true;
        SDL_Delay(20);
    }
    return false;
}

bool WiimoteHidLive::Write(std::uint32_t address, const std::uint8_t* data, int size) {
    std::uint8_t report[22]{0x16, 0x04, std::uint8_t(address >> 16), std::uint8_t(address >> 8),
                            std::uint8_t(address), std::uint8_t(size)};
    std::copy(data, data + size, report + 6);
    bool sent = false;
    for (int attempt = 0; attempt < 10 && !sent; ++attempt) {
        sent = SDL_hid_write(hid_, report, sizeof(report)) == int(sizeof(report));
        if (!sent) SDL_Delay(20);
    }
    if (!sent) return false;
    // Wait for the remote's acknowledgement of this register write.
    const std::uint64_t deadline = SDL_GetTicks() + 300;
    std::uint8_t buffer[32];
    while (SDL_GetTicks() < deadline) {
        const int got = SDL_hid_read_timeout(hid_, buffer, sizeof(buffer), 20);
        if (got < 0) return false;
        if (got >= 5 && buffer[0] == 0x22 && buffer[3] == 0x16) return buffer[4] == 0;
        if (got > 0) Handle(buffer, got);
    }
    return false;
}

bool WiimoteHidLive::Open(int slot) {
    Close();
    if (SDL_hid_init() != 0) return false;
    initialized_ = true;
    std::string path;
    SDL_hid_device_info* list = SDL_hid_enumerate(0x057e, 0);
    int index = 0;
    for (auto* info = list; info; info = info->next) {
        if (!IsWiimoteHid(info) || !info->path || !IsDolphinBarHid(info)) continue;
        const int interface_slot =
            info->interface_number >= 0 && info->interface_number < 4 ? info->interface_number : index;
        ++index;
        if (interface_slot == slot) { path = info->path; break; }
    }
    SDL_hid_free_enumeration(list);
    if (path.empty() || !(hid_ = SDL_hid_open_path(path.c_str()))) {
        Close();
        return false;
    }
    // The game's camera set-up (sensitivity level 3, basic mode), the slot's
    // player LED, then buttons, accelerometer and camera reports.
    const std::uint8_t on = 0x01, mode = 0x01, done = 0x08;
    const bool ready = Send({0x13, 0x04}) && Send({0x1a, 0x04}) && Write(0xb00030, &on, 1) &&
                       Write(0xb00000, kWiimoteIrBlock1[2], 9) && Write(0xb0001a, kWiimoteIrBlock2[2], 2) &&
                       Write(0xb00033, &mode, 1) && Write(0xb00030, &done, 1) &&
                       Send({0x11, std::uint8_t(0x10u << slot)}) && Send({0x12, 0x04, 0x37});
    if (!ready) Close();
    return ready;
}

void WiimoteHidLive::Handle(const std::uint8_t* report, int size) {
    if (report[0] == 0x37 && size >= 22) {
        buttons_ = std::uint16_t(report[1] << 8 | report[2]);
        dots_ = DecodeWiimoteBasicIr(report + 6);
        last_report_ns_ = SDL_GetTicksNS();
    } else if (report[0] == 0x20) {
        // After a status report the remote waits for its reporting mode again.
        const std::uint8_t mode[3]{0x12, 0x04, 0x37};
        SDL_hid_write(hid_, mode, sizeof(mode));
    }
}

void WiimoteHidLive::Poll() {
    if (!hid_) return;
    std::uint8_t buffer[32];
    for (int guard = 0; guard < 256; ++guard) {
        const int got = SDL_hid_read_timeout(hid_, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            if (got < 0) last_report_ns_ = 0;
            return;
        }
        Handle(buffer, got);
    }
}

bool WiimoteHidLive::Connected() const {
    return hid_ && last_report_ns_ && SDL_GetTicksNS() - last_report_ns_ < 500000000ull;
}
} // namespace mscharged::platform

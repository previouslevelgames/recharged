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
} // namespace mscharged::platform

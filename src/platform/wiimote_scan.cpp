#include "platform/wiimote_hid.h"
#include "platform/console_verbosity.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <mutex>
#include <set>
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

bool WiimoteDebug() {
    static const bool environment = [] {
        const char* value = SDL_getenv("RECHARGED_WIIMOTE_DEBUG");
        if (!value) value = SDL_getenv("MSCHARGED_WIIMOTE_DEBUG"); // before the rename (#64)
        return value && *value && *value != '0';
    }();
    return environment || VerboseConsole();
}

bool WriteWiimoteReport(SDL_hid_device* hid, const std::uint8_t* report, std::size_t size) {
    return hid && size && SDL_hid_write(hid, report, size) >= int(size);
}

std::string WiimoteHidId(const SDL_hid_device_info* info, int& adapter_index) {
    if (IsDolphinBarHid(info)) {
        const int slot = info->interface_number >= 0 && info->interface_number < 4 ? info->interface_number
                                                                                   : adapter_index;
        ++adapter_index;
        return WiimoteDolphinBarId(slot);
    }
    std::string serial;
    for (const wchar_t* c = info->serial_number; c && *c; ++c) serial += *c < 0x80 ? char(*c) : '?';
    return BluetoothWiimoteId(serial, info->path ? info->path : "");
}

std::string Narrow(const wchar_t* text) {
    std::string value;
    for (const wchar_t* c = text; c && *c; ++c) value += *c < 0x80 ? char(*c) : '?';
    return value;
}

// The launcher probes every few seconds: say why a remote is missing once.
void LogOnce(const std::string& id, const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lock(mutex);
    if (logged.insert(id + message).second) SDL_Log("Wii Remote %s %s", id.c_str(), message.c_str());
}

struct WiimoteHidProbe::Device {
    std::string path, id;
    SDL_hid_device* hid = nullptr;
    int slot = -1;
    bool plus = false;
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
        if (WiimoteDebug() && info->path) {
            char line[512];
            SDL_snprintf(line, sizeof line,
                         "HID %04x:%04x interface %d usage %04x:%04x product \"%s\" manufacturer \"%s\" serial \"%s\"",
                         info->vendor_id, info->product_id, info->interface_number, info->usage_page, info->usage,
                         Narrow(info->product_string).c_str(), Narrow(info->manufacturer_string).c_str(),
                         Narrow(info->serial_number).c_str());
            LogOnce(info->path, std::string("probe sees ") + line + " at " + info->path);
        }
        if (!IsWiimoteHid(info) || !info->path) continue;
        const bool adapter = IsDolphinBarHid(info);
        const auto id = WiimoteHidId(info, adapter_index);
        if (adapter) {
            status.dolphinbar = true;
            ++status.adapter_slots;
        }
        auto* hid = SDL_hid_open_path(info->path);
        if (!hid) {
            LogOnce(id, std::string("could not be opened: ") + SDL_GetError());
            ++status.no_permission;
            continue;
        }
        SDL_hid_set_nonblocking(hid, 1);
        // Status request; an empty DolphinBar slot rejects the write.
        const unsigned char request[2]{0x15, 0x00};
        if (WiimoteDebug()) {
            // The raw count: Windows pads each write to the longest output report.
            const int written = SDL_hid_write(hid, request, sizeof(request));
            LogOnce(id, "status request: write returned " + std::to_string(written)
                            + (written < 0 ? std::string(" (") + SDL_GetError() + ")" : std::string()));
            if (written < int(sizeof(request))) {
                if (!adapter) LogOnce(id, std::string("did not take a status request: ") + SDL_GetError());
                SDL_hid_close(hid);
                continue;
            }
        } else if (!WriteWiimoteReport(hid, request, sizeof(request))) {
            if (!adapter) LogOnce(id, std::string("did not take a status request: ") + SDL_GetError());
            SDL_hid_close(hid);
            continue;
        }
        devices_.push_back(new Device{info->path, id, hid, WiimoteDolphinBarSlot(id), info->product_id == 0x0330});
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
            if (WiimoteDebug() && !device->answered) {
                char first[8];
                SDL_snprintf(first, sizeof first, "%02x", buffer[0]);
                LogOnce(device->id, std::string("first report ") + first + ", " + std::to_string(size) + " bytes");
            }
            if (buffer[0] == 0x20 && size >= 7) {
                device->answered = true;
                device->nunchuk = (buffer[3] & 0x02) != 0;
                device->battery = std::min(100, buffer[6] * 100 / 0xc8);
            }
        }
        pending |= !device->answered;
    }
    if (pending && SDL_GetTicksNS() < deadline_ns_) return false;
    for (auto* device : devices_)
        if (!device->answered && device->slot < 0)
            LogOnce(device->id, "did not answer its status request");
    auto status = result_;
    for (auto* device : devices_) {
        if (device->answered)
            status.remotes.push_back({device->path, device->id, device->slot, device->plus, -1, device->nunchuk, false,
                                      device->battery});
        else
            status.silent.push_back(device->id);
    }
    SortWiimoteRemotes(status.remotes);
    Close();
    result_ = status;
    return true;
}

void SortWiimoteRemotes(std::vector<WiimoteHidRemote>& remotes) {
    std::stable_sort(remotes.begin(), remotes.end(), [](const auto& a, const auto& b) {
        const int sa = a.slot < 0 ? 100 : a.slot, sb = b.slot < 0 ? 100 : b.slot;
        return sa != sb ? sa < sb : a.id < b.id;
    });
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

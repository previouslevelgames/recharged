#include "platform/wiimote_bluetooth.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bluetoothapis.h>
#include <cwchar>
#include <type_traits>
#endif

namespace mscharged::platform {
struct WiimoteBluetoothSearch::State {
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool active = false, quit = false, started = false;
    WiimoteBluetoothStatus status;
};

namespace {
#ifdef _WIN32
// Bluetooth HID service (HumanInterfaceDeviceServiceClass_UUID).
constexpr GUID kHidService{0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
// Inquiry length in units of 1.28 s.
constexpr UCHAR kInquiry = 2;

// The Bluetooth API, loaded when first needed: without bthprops.cpl (some
// Windows editions) only this search is missing, not the whole program.
struct Api {
    decltype(&BluetoothFindFirstRadio) FindFirstRadio;
    decltype(&BluetoothFindNextRadio) FindNextRadio;
    decltype(&BluetoothFindRadioClose) FindRadioClose;
    decltype(&BluetoothFindFirstDevice) FindFirstDevice;
    decltype(&BluetoothFindNextDevice) FindNextDevice;
    decltype(&BluetoothFindDeviceClose) FindDeviceClose;
    decltype(&BluetoothRemoveDevice) RemoveDevice;
    decltype(&BluetoothSetServiceState) SetServiceState;
};
const Api* LoadApi() {
    static const Api* api = []() -> const Api* {
        const HMODULE module = LoadLibraryW(L"bthprops.cpl");
        if (!module) return nullptr;
        static Api loaded{};
        bool complete = true;
        const auto get = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                reinterpret_cast<void*>(GetProcAddress(module, name)));
            complete &= function != nullptr;
        };
        get(loaded.FindFirstRadio, "BluetoothFindFirstRadio");
        get(loaded.FindNextRadio, "BluetoothFindNextRadio");
        get(loaded.FindRadioClose, "BluetoothFindRadioClose");
        get(loaded.FindFirstDevice, "BluetoothFindFirstDevice");
        get(loaded.FindNextDevice, "BluetoothFindNextDevice");
        get(loaded.FindDeviceClose, "BluetoothFindDeviceClose");
        get(loaded.RemoveDevice, "BluetoothRemoveDevice");
        get(loaded.SetServiceState, "BluetoothSetServiceState");
        return complete ? &loaded : nullptr;
    }();
    return api;
}

bool IsWiimoteName(const WCHAR* name) {
    // The Wii checks only the start too; RVL-CNT-01 and RVL-CNT-01-TR (Plus).
    return std::wcsncmp(name, L"Nintendo RVL-CNT", 16) == 0;
}

std::string Address(const BLUETOOTH_ADDRESS& address) {
    char text[18];
    const auto* b = address.rgBytes; // reverse of display order
    std::snprintf(text, sizeof text, "%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
    return text;
}

// Calls visit(radio, device) for every Wii Remote Windows lists; an inquiry
// (inquiry > 0) also finds remotes that are waiting to connect. False
// without a Bluetooth adapter.
template <class Visit> bool EachWiimote(const Api& bt, UCHAR inquiry, Visit&& visit) {
    BLUETOOTH_FIND_RADIO_PARAMS radio_params{sizeof(radio_params)};
    HANDLE radio = nullptr;
    const HBLUETOOTH_RADIO_FIND radios = bt.FindFirstRadio(&radio_params, &radio);
    if (!radios) return false;
    do {
        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
        search.dwSize = sizeof(search);
        search.fReturnAuthenticated = search.fReturnRemembered = search.fReturnUnknown = search.fReturnConnected = TRUE;
        search.fIssueInquiry = inquiry > 0;
        search.cTimeoutMultiplier = inquiry;
        search.hRadio = radio;
        BLUETOOTH_DEVICE_INFO device{};
        device.dwSize = sizeof(device);
        if (const HBLUETOOTH_DEVICE_FIND devices = bt.FindFirstDevice(&search, &device)) {
            do {
                if (IsWiimoteName(device.szName)) visit(radio, device);
                device = {};
                device.dwSize = sizeof(device);
            } while (bt.FindNextDevice(devices, &device));
            bt.FindDeviceClose(devices);
        }
        CloseHandle(radio);
    } while (bt.FindNextRadio(radios, &radio));
    bt.FindRadioClose(radios);
    return true;
}

void Event(WiimoteBluetoothSearch::State& state, const std::string& text, bool connected = false) {
    SDL_Log("Wii Remote Bluetooth: %s", text.c_str());
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status.last = text;
    state.status.connected += connected;
}

void Round(WiimoteBluetoothSearch::State& state) {
    const Api* api = LoadApi();
    if (!api) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status.adapter = false;
        return;
    }
    const Api& bt = *api;
    // Windows cannot reconnect a remembered remote that is not authenticated
    // (connected with 1+2, or paired without a PIN); forget it so 1+2 works.
    const bool adapter = EachWiimote(bt, 0, [&](HANDLE, BLUETOOTH_DEVICE_INFO& device) {
        if (!device.fRemembered || device.fConnected || device.fAuthenticated) return;
        const DWORD result = bt.RemoveDevice(&device.Address);
        if (result == ERROR_SUCCESS) Event(state, "forgot disconnected " + Address(device.Address));
    });
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status.adapter = adapter;
    }
    if (!adapter) return;
    EachWiimote(bt, kInquiry, [&](HANDLE radio, BLUETOOTH_DEVICE_INFO& device) {
        if (device.fConnected) return;
        const DWORD result = bt.SetServiceState(radio, &device, &kHidService, BLUETOOTH_SERVICE_ENABLE);
        if (result == ERROR_SUCCESS) Event(state, "connected " + Address(device.Address), true);
        else Event(state, "could not connect " + Address(device.Address) + " (error " + std::to_string(result) + ")");
    });
}
#endif

void Run(std::shared_ptr<WiimoteBluetoothSearch::State> state) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->wake.wait(lock, [&] { return state->quit || state->active; });
            if (state->quit) return;
            state->status.searching = true;
        }
#ifdef _WIN32
        Round(*state);
#endif
        std::unique_lock<std::mutex> lock(state->mutex);
        state->status.searching = state->active && !state->quit;
        // A short pause between inquiries.
        state->wake.wait_for(lock, std::chrono::seconds(1), [&] { return state->quit; });
        if (state->quit) return;
    }
}
} // namespace

WiimoteBluetoothSearch::WiimoteBluetoothSearch() : state_(std::make_shared<State>()) {
#ifdef _WIN32
    state_->status.supported = true;
#endif
}

WiimoteBluetoothSearch::~WiimoteBluetoothSearch() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->quit = true;
    state_->wake.notify_all();
}

void WiimoteBluetoothSearch::SetActive(bool on) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->status.supported || state_->active == on) return;
    state_->active = on;
    if (!on) state_->status.searching = false;
    if (on && !state_->started) {
        // The thread owns the state too: an inquiry that is still running when
        // the launcher closes ends on its own.
        state_->started = true;
        std::thread(Run, state_).detach();
    }
    state_->wake.notify_all();
}

WiimoteBluetoothStatus WiimoteBluetoothSearch::Status() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
}
} // namespace mscharged::platform

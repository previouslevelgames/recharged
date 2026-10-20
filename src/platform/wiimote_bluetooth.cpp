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
    // Requested once each, run in the order Reset, Sync, Refresh.
    bool refresh = false, sync = false, reset = false;
    WiimoteBluetoothStatus status;
};

namespace {
using State = WiimoteBluetoothSearch::State;
enum class Task { Scan, Sync, Reset };

#ifdef _WIN32
bool Quitting(State& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.quit;
}

// A Sync or Reset waits for the scan in progress.
bool TaskWaiting(State& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.quit || state.sync || state.reset;
}

// Bluetooth HID service (HumanInterfaceDeviceServiceClass_UUID).
constexpr GUID kHidService{0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
// Inquiry length in units of 1.28 s: 3.84 s. An inquiry calls on one half of
// the inquiry channels for its first 2.56 s and only then on the other half,
// so a shorter one misses a remote that listens on the second half.
constexpr UCHAR kInquiry = 3;
// Sync repeats its inquiry, so a remote that missed one is still paired.
constexpr int kSyncRounds = 3;

// BluetoothAuthenticateDevice: pairing with a given PIN (the Ex variant asks a
// callback for the same PIN).
using AuthenticateDevice = DWORD(WINAPI*)(HWND, HANDLE, BLUETOOTH_DEVICE_INFO*, PWSTR, ULONG);

// The Bluetooth API, loaded when first needed: without bthprops.cpl (some
// Windows editions) only this search is missing, not the whole program.
struct Api {
    decltype(&BluetoothFindFirstRadio) FindFirstRadio;
    decltype(&BluetoothFindNextRadio) FindNextRadio;
    decltype(&BluetoothFindRadioClose) FindRadioClose;
    decltype(&BluetoothGetRadioInfo) GetRadioInfo;
    decltype(&BluetoothFindFirstDevice) FindFirstDevice;
    decltype(&BluetoothFindNextDevice) FindNextDevice;
    decltype(&BluetoothFindDeviceClose) FindDeviceClose;
    decltype(&BluetoothRemoveDevice) RemoveDevice;
    decltype(&BluetoothSetServiceState) SetServiceState;
    decltype(&BluetoothEnumerateInstalledServices) EnumerateInstalledServices;
    AuthenticateDevice Authenticate;
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
        get(loaded.GetRadioInfo, "BluetoothGetRadioInfo");
        get(loaded.FindFirstDevice, "BluetoothFindFirstDevice");
        get(loaded.FindNextDevice, "BluetoothFindNextDevice");
        get(loaded.FindDeviceClose, "BluetoothFindDeviceClose");
        get(loaded.RemoveDevice, "BluetoothRemoveDevice");
        get(loaded.SetServiceState, "BluetoothSetServiceState");
        get(loaded.EnumerateInstalledServices, "BluetoothEnumerateInstalledServices");
        get(loaded.Authenticate, "BluetoothAuthenticateDevice");
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

// Paired with this PC (by Sync): such a remote connects itself when any of
// its buttons is pressed.
bool Paired(const BLUETOOTH_DEVICE_INFO& device) { return device.fRemembered && device.fAuthenticated; }

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

void Event(State& state, const std::string& text, int connected = 0, int paired = 0) {
    SDL_Log("Wii Remote Bluetooth: %s", text.c_str());
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status.last = text;
    state.status.connected += connected;
    state.status.paired += paired;
}

// Windows cannot reconnect a remembered remote that is not paired (connected
// with 1+2, or paired without a PIN); forget it so 1+2 connects it again.
// False without a Bluetooth adapter.
bool ForgetUnusable(const Api& bt, State& state) {
    const bool adapter = EachWiimote(bt, 0, [&](HANDLE, BLUETOOTH_DEVICE_INFO& device) {
        if (!device.fRemembered || device.fConnected || device.fAuthenticated) return;
        if (bt.RemoveDevice(&device.Address) == ERROR_SUCCESS)
            Event(state, "forgot disconnected " + Address(device.Address));
    });
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status.adapter = adapter;
    return adapter;
}

DWORD Connect(const Api& bt, HANDLE radio, BLUETOOTH_DEVICE_INFO& device) {
    return bt.SetServiceState(radio, &device, &kHidService, BLUETOOTH_SERVICE_ENABLE);
}

// Continuous scanning and Refresh: connects the remotes that wait after 1+2
// or SYNC, for as long as they stay on.
void Scan(const Api& bt, State& state) {
    if (!ForgetUnusable(bt, state)) return;
    EachWiimote(bt, kInquiry, [&](HANDLE radio, BLUETOOTH_DEVICE_INFO& device) {
        // A remote connected now would be skipped by the Sync that waits.
        if (device.fConnected || Paired(device) || TaskWaiting(state)) return;
        const DWORD result = Connect(bt, radio, device);
        if (result == ERROR_SUCCESS) Event(state, "connected " + Address(device.Address), 1);
        else Event(state, "could not connect " + Address(device.Address) + " (error " + std::to_string(result) + ")");
    });
}

// Pairs a remote whose SYNC button was pressed: like a Wii, it takes the PC's
// Bluetooth address as its PIN, the six bytes in the order Windows keeps them
// (the reverse of the written address). After 1+2 a remote expects its own
// address instead, for that one connection only.
bool Pair(const Api& bt, HANDLE radio, BLUETOOTH_DEVICE_INFO& device) {
    BLUETOOTH_RADIO_INFO pc{};
    pc.dwSize = sizeof(pc);
    if (bt.GetRadioInfo(radio, &pc) != ERROR_SUCCESS) return false;
    WCHAR pin[6];
    for (int n = 0; n < 6; ++n) pin[n] = pc.address.rgBytes[n];
    if (bt.Authenticate(nullptr, radio, &device, pin, 6) != ERROR_SUCCESS) return false;
    // Then list the remote's services, or the pairing is not kept.
    DWORD services = 0;
    const DWORD listed = bt.EnumerateInstalledServices(radio, &device, &services, nullptr);
    return listed == ERROR_SUCCESS || listed == ERROR_MORE_DATA;
}

void SyncRemotes(const Api& bt, State& state) {
    if (!ForgetUnusable(bt, state)) return;
    int found = 0;
    for (int round = 0; round < kSyncRounds && !Quitting(state); ++round) {
        EachWiimote(bt, kInquiry, [&](HANDLE radio, BLUETOOTH_DEVICE_INFO& device) {
            if (device.fConnected) return;
            const bool known = Paired(device);
            const bool paired = !known && Pair(bt, radio, device);
            // Also connects a remote paired before whose SYNC button was pressed.
            const DWORD result = Connect(bt, radio, device);
            if (known) return;
            ++found;
            const auto address = Address(device.Address);
            if (paired) Event(state, "paired " + address, result == ERROR_SUCCESS, 1);
            else if (result == ERROR_SUCCESS)
                Event(state, "connected " + address + " without pairing it (press SYNC, not 1+2)", 1);
            else Event(state, "could not pair " + address + " (error " + std::to_string(result) + ")");
        });
    }
    if (!found && !Quitting(state)) Event(state, "Sync found no new Wii Remote");
}

void ForgetAll(const Api& bt, State& state) {
    int forgotten = 0;
    const bool adapter = EachWiimote(bt, 0, [&](HANDLE, BLUETOOTH_DEVICE_INFO& device) {
        if (device.fRemembered && bt.RemoveDevice(&device.Address) == ERROR_SUCCESS) ++forgotten;
    });
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status.adapter = adapter;
    }
    if (adapter)
        Event(state, !forgotten ? std::string("no Wii Remote to forget")
                     : "forgot " + std::to_string(forgotten) + (forgotten == 1 ? " Wii Remote" : " Wii Remotes"));
}

void RunTask(State& state, Task task) {
    const Api* api = LoadApi();
    if (!api) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status.adapter = false;
        return;
    }
    switch (task) {
    case Task::Scan: Scan(*api, state); break;
    case Task::Sync: SyncRemotes(*api, state); break;
    case Task::Reset: ForgetAll(*api, state); break;
    }
}
#endif

void Run(std::shared_ptr<State> state) {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->wake.wait(lock, [&] {
                return state->quit || state->active || state->refresh || state->sync || state->reset;
            });
            if (state->quit) return;
            task = state->reset ? Task::Reset : state->sync ? Task::Sync : Task::Scan;
            (task == Task::Reset ? state->reset : task == Task::Sync ? state->sync : state->refresh) = false;
            state->status.searching = true;
        }
#ifdef _WIN32
        RunTask(*state, task);
#endif
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool requested = state->refresh || state->sync || state->reset;
        state->status.syncing = state->sync;
        state->status.resetting = state->reset;
        state->status.searching = (state->active && !state->quit) || requested;
        // A short pause between continuous inquiries.
        if (task == Task::Scan && !requested)
            state->wake.wait_for(lock, std::chrono::seconds(1), [&] {
                return state->quit || state->refresh || state->sync || state->reset;
            });
        if (state->quit) return;
    }
}

// The thread owns the state too: an inquiry that is still running when the
// owner goes away ends on its own. The caller holds the mutex.
void Start(const std::shared_ptr<State>& state) {
    if (state->started) return;
    state->started = true;
    std::thread(Run, state).detach();
}

void Request(const std::shared_ptr<State>& state, bool State::* task) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->status.supported) return;
    (*state).*task = true;
    state->status.searching = true;
    state->status.syncing |= task == &State::sync;
    state->status.resetting |= task == &State::reset;
    Start(state);
    state->wake.notify_all();
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
    if (on) Start(state_);
    else state_->status.searching = state_->refresh || state_->sync || state_->reset;
    state_->wake.notify_all();
}

void WiimoteBluetoothSearch::Refresh() { Request(state_, &State::refresh); }
void WiimoteBluetoothSearch::Sync() { Request(state_, &State::sync); }
void WiimoteBluetoothSearch::Reset() { Request(state_, &State::reset); }

WiimoteBluetoothStatus WiimoteBluetoothSearch::Status() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
}
} // namespace mscharged::platform

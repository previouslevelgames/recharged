#include "platform/ipc_boot_buffer.h"
#include "platform/ipc_hardware_abi.h"
#include "platform/interrupts.h"
#include <revolution/ipc.h>
#include <revolution/os/OSIpc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void Reject(F function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}
void Run() {
    using namespace mscharged::platform;
    Reject([] { __OSInitIPCBuffer(); }, "Source initialization accepted absent boot memory");
    // Original IPC/heap state retains these pointers through process exit.
    alignas(32) static std::array<unsigned char, 8192> bytes;
    alignas(32) static std::array<unsigned char, 8192> other;
    Check(reinterpret_cast<std::uintptr_t>(bytes.data()) > UINT32_MAX,
          "Native IPC qualification needs actual addresses above Wii32");
    bytes.fill(0xb7);
    Reject([&] { InstallNativeIPCBootBuffer(nullptr, bytes.size()); }, "Null IPC memory accepted");
    Reject([&] { InstallNativeIPCBootBuffer(bytes.data()+1, bytes.size()-32); }, "Unaligned base accepted");
    Reject([&] { InstallNativeIPCBootBuffer(bytes.data(), 0); }, "Empty IPC memory accepted");
    Reject([&] { InstallNativeIPCBootBuffer(bytes.data(), bytes.size()-1); }, "Unaligned span accepted");
    InstallNativeIPCBootBuffer(bytes.data(), bytes.size());
    Check(NativeInterruptsEnabled(), "Boot metadata installation changed IRQ state");
    __OSInitIPCBuffer();
    Check(__OSGetIPCBufferLo() == bytes.data() &&
              __OSGetIPCBufferHi() == bytes.data()+bytes.size(),
          "Original OSIpc truncated native boot endpoints");
    Check(IPCGetBufferLo() == nullptr && IPCGetBufferHi() == nullptr,
          "Host metadata invented original IPC initialization");
    IPCInit();
    Check(IPCGetBufferLo() == bytes.data() && IPCGetBufferHi() == bytes.data()+bytes.size(),
          "Original IPCInit did not capture both actual boot endpoints");
    Check(std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b==0xb7; }),
          "Source IPCInit touched caller payload");
    auto* cursor=bytes.data()+512;
    IPCSetBufferLo(cursor);
    IPCInit();
    Check(IPCGetBufferLo() == cursor && IPCGetBufferHi() == bytes.data()+bytes.size(),
          "Original repeated IPCInit reset its advancing arena");
    InstallNativeIPCBootBuffer(bytes.data(), bytes.size());
    Reject([&] { InstallNativeIPCBootBuffer(other.data(), other.size()); }, "Live boot span replaced");
    bool worker_rejected = false;
    std::thread worker([&] {
        try { ChargedNativeIPCBufferStart(); }
        catch (const std::logic_error&) { worker_rejected = true; }
    });
    worker.join();
    Check(worker_rejected, "Boot endpoints allowed a different hardware owner");
    const auto heap=iosCreateHeap(IPCGetBufferLo(),
        static_cast<u32>(static_cast<unsigned char*>(IPCGetBufferHi())-cursor));
    Check(heap >= 0, "Original IPC heap rejected native source buffer range");
    auto* allocation=static_cast<unsigned char*>(iosAllocAligned(heap, 320, 32));
    Check(allocation && allocation >= cursor && allocation+320 <= bytes.data()+bytes.size(),
          "Original IPC allocation escaped actual captured boot storage");
    std::memset(allocation,0x59,320);
    Check(std::all_of(allocation,allocation+320,[](auto b){return b==0x59;}),
          "Original IPC native payload did not survive");
    Check(iosFree(heap,allocation)==IPC_RESULT_OK, "Original owning IPC free failed");
    Reject([] { IPCReadReg(IPC_PPCCTRL); }, "Missing Wii IPC MMIO reported readiness");
    Reject([] { IPCWriteReg(IPC_PPCMSG, 0); }, "Missing Wii IPC MMIO write accepted");
    Check(NativeInterruptsEnabled(), "IPC boot/source heap checks leaked IRQ exclusion");
    std::cout << "native_ipc_boot_buffer: " << checks
              << " checks; original OSIpc/IPCInit/heap, full-width boot memory; IOS remains separate\n";
}
}
int main() {
    try { Run(); return 0; }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

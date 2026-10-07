#pragma once
#include <cstdint>
namespace mscharged::platform {
enum class NativeOSBootEnvironment {DesktopApplication,IPLDiagnostic};
// Explicitly configure the native OS replacement before original OS readers.
// The desktop application executes after Wii IPL, not within it. IPLDiagnostic
// exists to test the literal original branch; it does not execute Wii IPL/PPC.
void ConfigureNativeOSBootEnvironment(NativeOSBootEnvironment environment);
void RetireNativeOSBootEnvironment();
class NativeOSBootEnvironmentLease {
public:
    NativeOSBootEnvironmentLease();
    ~NativeOSBootEnvironmentLease();
    NativeOSBootEnvironmentLease(const NativeOSBootEnvironmentLease&)=delete;
    NativeOSBootEnvironmentLease& operator=(const NativeOSBootEnvironmentLease&)=delete;
    void RequireLive() const;
    void Close();
private:
    std::uint64_t generation_{};
};
}

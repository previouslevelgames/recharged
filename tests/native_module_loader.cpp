#include "platform/native_module_loader.h"
#include "platform/path.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif
using namespace mscharged::platform;
namespace {
unsigned checks{}, mainCreated{}, mainDestroyed{}, foreignCreated{}, foreignDestroyed{};
NativeModuleFile* armedFile{};
NativeModuleImage constructorImage;
void* constructorLease{};
bool constructorFailed{};
void Check(bool condition, const char* reason) {
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
template<class Operation> void Reject(Operation operation, const char* reason) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, reason);
}
}
// This is fixture import plumbing, not a source/platform success substitute.
// The true DLL constructor must observe its actual file, BSS and loader lease.
extern "C" FIXTURE_EXPORT void fixture_host_event(unsigned event, const void* address, std::size_t bytes) {
    if (event == 11) { ++mainDestroyed; return; }
    if (event == 12) { ++foreignDestroyed; return; }
    if (event == 2) { ++foreignCreated; return; }
    ++mainCreated;
    try {
        Check(event == 1 && armedFile, "DLL constructor preceded actual host image owner");
        Check(QueryNativeModuleImage(address, constructorImage) && armedFile->OwnsImage(constructorImage),
              "Actual DLL constructor storage must match opened source incarnation");
        Check(bytes == 8192 && NativeModuleOwnsReadableExtent(constructorImage.base, address, bytes),
              "Constructor must see the complete real mapped zero-fill range");
        const auto* raw = static_cast<const unsigned char*>(address);
        for (std::size_t i = 0; i != bytes; ++i) Check(raw[i] == 0, "Native BSS was not loader initialized");
        constructorLease = RetainNativeModuleImage(armedFile->Path(), constructorImage.base);
        Check(constructorLease, "Pre-static source owner must retain the already mapped actual DLL");
    } catch (...) { constructorFailed = true; }
}
int main(int argc, char** argv) {
    try {
        Check(argc == 4 || argc == 5, "Expected main image, foreign image, disposable directory, optional missing-import image");
        const auto workspace = std::filesystem::absolute(mscharged::PathFromUtf8(argv[3]));
        Check(std::filesystem::create_directory(workspace), "Fixture directory must be new and caller disposable");
        const auto selectedPath = workspace / mscharged::PathFromUtf8("selected-\xc3\xa9-image" + std::filesystem::path(argv[1]).extension().string());
        const auto foreignPath = workspace / ("foreign-image" + std::filesystem::path(argv[2]).extension().string());
        std::filesystem::copy_file(mscharged::PathFromUtf8(argv[1]), selectedPath);
        std::filesystem::copy_file(mscharged::PathFromUtf8(argv[2]), foreignPath);
        Reject([] { NativeModuleFile empty(""); }, "Empty path must not fabricate an image owner");
        Reject([&] { NativeModuleFile absent(workspace / "missing.dll"); }, "Absent file must not acquire image ownership");
        const auto emptyPath = workspace / "empty-image.dll";
        { std::ofstream empty(emptyPath); }
        Reject([&] { NativeModuleFile empty(emptyPath); }, "Empty regular file is not a source image");
        {
            NativeModuleFile selected(selectedPath);
            NativeModuleFile foreign(foreignPath);
            Check(!selected.Identity().empty() && selected.Identity() != foreign.Identity(),
                  "Distinct opened file incarnations require distinct ownership evidence");
            selected.RequireUnchanged();
#if defined(_WIN32)
            const auto writer = CreateFileW(selected.Path().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, 0, nullptr);
            Check(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
                  "Real image file lease must reject modification before DLL loading");
#endif
            armedFile = &selected;
            auto* initial = LoadNativeModule(selected.Path());
            armedFile = nullptr;
            Check(mainCreated == 1 && !constructorFailed && constructorLease,
                  "One actual DLL constructor must complete under the actual owner");
            NativeModuleImage loaded;
            Check(QueryNativeModuleHandle(initial, loaded) && loaded.base == constructorImage.base && selected.OwnsImage(loaded),
                  "Actual loader handle and constructor must refer to the same mapped image");
            auto entry = reinterpret_cast<int(*)()>(FindNativeModuleSymbol(initial, "charged_original_entry"));
            Check(entry && entry() == 42, "GetProcAddress/dlsym must invoke the actual exported entry");
            Check(!FindNativeModuleSymbol(initial, "absent_original_provider") && !FindNativeModuleSymbol(nullptr, "entry"),
                  "Missing/null symbol lookup must remain unavailable");
            auto* bss = static_cast<unsigned char*>(FindNativeModuleSymbol(initial, "fixture_native_bss"));
            const auto* readonly = static_cast<const unsigned char*>(FindNativeModuleSymbol(initial, "fixture_native_readonly"));
            Check(bss && readonly && NativeModuleOwnsReadableExtent(loaded.base, bss, 8192) &&
                  NativeModuleOwnsReadableExtent(loaded.base, readonly, 32) && readonly[0] == 0x91,
                  "Mapped BSS and actual readonly source bytes need complete readable extents");
            int stack = 0;
            Check(!NativeModuleOwnsReadableExtent(loaded.base, &stack, sizeof(stack)), "Host stack is not selected image storage");
            Check(!NativeModuleOwnsReadableExtent(loaded.base, bss, 0) &&
                  !NativeModuleOwnsReadableExtent(loaded.base, nullptr, 1) &&
                  !NativeModuleOwnsReadableExtent(loaded.base, reinterpret_cast<void*>(std::numeric_limits<std::uintptr_t>::max() - 1), 8),
                  "Null/zero/overflow extents must be rejected");
#if defined(_WIN32)
            DWORD oldProtection{};
            Check(VirtualProtect(bss + 4096, 4096, PAGE_NOACCESS, &oldProtection), "Fixture must change actual second BSS page protection");
            Check(!NativeModuleOwnsReadableExtent(loaded.base, bss, 8192) &&
                  NativeModuleOwnsReadableExtent(loaded.base, bss, 4096),
                  "PE characteristics alone must not authorize currently inaccessible pages");
            DWORD ignored{};
            Check(VirtualProtect(bss + 4096, 4096, oldProtection, &ignored) &&
                  NativeModuleOwnsReadableExtent(loaded.base, bss, 8192),
                  "Restoring actual page protection restores the readable range");
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(loaded.base +
                reinterpret_cast<const IMAGE_DOS_HEADER*>(loaded.base)->e_lfanew);
            Check(!NativeModuleOwnsReadableExtent(loaded.base,
                      reinterpret_cast<const void*>(loaded.base + nt->OptionalHeader.SizeOfImage), 1),
                  "Exact mapped PE image end remains excluded");
#endif
            auto* other = LoadNativeModule(foreign.Path());
            NativeModuleImage otherImage;
            const auto* otherBss = FindNativeModuleSymbol(other, "fixture_native_bss");
            Check(QueryNativeModuleImage(otherBss, otherImage) && foreign.OwnsImage(otherImage) &&
                  otherImage.base != loaded.base && !selected.OwnsImage(otherImage) &&
                  !NativeModuleOwnsReadableExtent(loaded.base, otherBss, 8192),
                  "Foreign real DLL data must not enter the selected file/image domain");
            Check(foreignCreated == 1 && ReleaseNativeModule(other) && foreignDestroyed == 1,
                  "Foreign fixture must actually unload its own source lifetime");
            Check(ReleaseNativeModule(initial) && mainDestroyed == 0 &&
                  NativeModuleOwnsReadableExtent(loaded.base, bss, 8192),
                  "Pre-static module reference must retain storage after the primary handle retires");
            auto* additional = RetainNativeModuleImage(selected.Path(), loaded.base);
            Check(additional && ReleaseNativeModule(constructorLease) && mainDestroyed == 0,
                  "Additional genuine module reference must independently retain the image");
            constructorLease = nullptr;
            Check(ReleaseNativeModule(additional) && mainDestroyed == 1 &&
                  !NativeModuleOwnsReadableExtent(loaded.base, bss, 8192),
                  "Final reference release must retire the actual mapped storage and source destructor once");
            selected.RequireUnchanged();
        }
#if defined(_WIN32)
        if (argc == 5) {
            const auto missingPath = workspace / "missing-import.dll";
            std::filesystem::copy_file(mscharged::PathFromUtf8(argv[4]), missingPath);
            bool rejected = false;
            try { (void)LoadNativeModule(missingPath); }
            catch (const std::runtime_error& error) {
                rejected = std::strstr(error.what(), "Win32 127") != nullptr;
                std::printf("Missing-import load result: %s\n", error.what());
            }
            Check(rejected && mainCreated == 1, "Real unresolved executable import must reject DLL loading before its constructor");
        }
#else
        {
            NativeModuleFile unchanged(selectedPath);
            { std::ofstream edited(selectedPath, std::ios::binary | std::ios::app); edited.put('\0'); }
            Reject([&] { unchanged.RequireUnchanged(); }, "Opened but modified source file must lose identity");
        }
#endif
        std::filesystem::remove_all(workspace);
        std::printf("Native module loader PASS %u checks; actual constructors, file identity, BSS/readonly extents and reference retirement.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native module loader hold: %s\n", error.what());
        return 1;
    }
}

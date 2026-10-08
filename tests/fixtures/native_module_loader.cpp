#include <cstddef>
#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#define FIXTURE_IMPORT __declspec(dllimport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#define FIXTURE_IMPORT
#endif
extern "C" {
FIXTURE_IMPORT void fixture_host_event(unsigned event, const void* address, std::size_t bytes);
#if defined(FIXTURE_MISSING_IMPORT)
FIXTURE_IMPORT void fixture_absent_host_service();
#endif
// Real mapped zero-fill exceeds the DLL's serialized .bss size on disk.
alignas(4096) FIXTURE_EXPORT unsigned char fixture_native_bss[8192];
FIXTURE_EXPORT extern const unsigned char fixture_native_readonly[32] = {0x91,0x12,0x39,0x47};
FIXTURE_EXPORT int charged_original_entry() { return 42; }
}
namespace {
#if defined(FIXTURE_FOREIGN)
constexpr unsigned Id = 2;
#else
constexpr unsigned Id = 1;
#endif
struct Lifetime {
    Lifetime() {
#if defined(FIXTURE_MISSING_IMPORT)
        fixture_absent_host_service();
#endif
        fixture_host_event(Id, fixture_native_bss, sizeof(fixture_native_bss));
    }
    ~Lifetime() { fixture_host_event(Id + 10, fixture_native_bss, sizeof(fixture_native_bss)); }
} lifetime;
}

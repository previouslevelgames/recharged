#if defined(MSCHARGED_GAME_MODULE)
#error Native module ownership must use the host CRT outside game operators
#endif
// Select the shared native Windows API level before MinGW standard headers can
// select an older default. This matches the existing native thread provider.
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0602
#endif
#include "platform/native_module_loader.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#else
#include <link.h>
#endif
#endif

namespace mscharged::platform {
namespace {
bool ContainsExtent(std::uintptr_t begin, std::uint64_t size,
                    std::uintptr_t address, std::size_t bytes) noexcept {
    if (!bytes || size > std::numeric_limits<std::uintptr_t>::max() - begin || address < begin)
        return false;
    const auto offset = address - begin;
    return offset < size && bytes <= size - offset;
}
#if defined(_WIN32)
std::runtime_error WindowsError(const char* operation) {
    const auto error = GetLastError();
    return std::runtime_error(std::string(operation) + " failed (Win32 " + std::to_string(error) + ")");
}
std::filesystem::path ModulePath(HMODULE module) {
    std::wstring path(512, L'\0');
    for (;;) {
        const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!count) throw WindowsError("GetModuleFileNameW");
        if (count < path.size()) {
            path.resize(count);
            return std::filesystem::canonical(path);
        }
        if (path.size() >= 32768) throw std::runtime_error("Loaded module path exceeds Win32 bound");
        path.resize(32768);
    }
}
bool SameFile(const BY_HANDLE_FILE_INFORMATION& a,
              const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
        a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
        a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}
HANDLE OpenImageFile(const std::filesystem::path& path) noexcept {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
bool ReadableProtection(DWORD protection) noexcept {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protection & 0xffu) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default: return false;
    }
}
bool ReadableMappedExtent(std::uintptr_t base, std::uintptr_t address,
                          std::size_t bytes) noexcept {
    while (bytes) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
            reinterpret_cast<std::uintptr_t>(region.AllocationBase) != base ||
            !ReadableProtection(region.Protect)) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (!ContainsExtent(begin, region.RegionSize, address, 1)) return false;
        const auto remaining = region.RegionSize - (address - begin);
        const auto consumed = bytes < remaining ? bytes : remaining;
        if (consumed > std::numeric_limits<std::uintptr_t>::max() - address) return false;
        address += consumed;
        bytes -= consumed;
    }
    return true;
}
#else
const timespec& ModifiedTime(const struct stat& value) noexcept {
#if defined(__APPLE__)
    return value.st_mtimespec;
#else
    return value.st_mtim;
#endif
}
bool SameFile(const struct stat& a, const struct stat& b) noexcept {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        ModifiedTime(a).tv_sec == ModifiedTime(b).tv_sec &&
        ModifiedTime(a).tv_nsec == ModifiedTime(b).tv_nsec;
}
#endif
}

void* LoadNativeModule(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto absolute = std::filesystem::canonical(path);
    // Normal executable loading: never DATAFILE/DONT_RESOLVE_DLL_REFERENCES.
    // Exact image directory plus default OS/application dependency directories;
    // no dependence on an unrelated process current working directory.
    auto module = LoadLibraryExW(absolute.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) throw WindowsError("LoadLibraryExW");
    // The game keeps addresses in 32-bit unsigned long carriers on LLP64, so its
    // DLL (like its arenas) must sit below 2 GB.
    if (reinterpret_cast<std::uintptr_t>(module) >= 0x80000000u) {
        FreeLibrary(module);
        throw std::runtime_error("Game module loaded above 2 GB; its 32-bit address carriers would break");
    }
    return module;
#else
    auto module = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!module) throw std::runtime_error(dlerror());
    return module;
#endif
}
void* FindNativeModuleSymbol(void* handle, const char* name) noexcept {
    if (!handle || !name || !*name) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}
bool ReleaseNativeModule(void* reference) noexcept {
    if (!reference) return false;
#if defined(_WIN32)
    return FreeLibrary(static_cast<HMODULE>(reference)) != 0;
#else
    return dlclose(reference) == 0;
#endif
}
bool QueryNativeModuleImage(const void* address, NativeModuleImage& image) noexcept {
    if (!address) return false;
    try {
#if defined(_WIN32)
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(address, &region, sizeof(region)) || region.Type != MEM_IMAGE ||
            !region.AllocationBase) return false;
        const auto module = static_cast<HMODULE>(region.AllocationBase);
        image = {reinterpret_cast<std::uintptr_t>(module), ModulePath(module)};
#else
        Dl_info info{};
        if (!dladdr(address, &info) || !info.dli_fbase || !info.dli_fname) return false;
        image = {reinterpret_cast<std::uintptr_t>(info.dli_fbase),
                 std::filesystem::canonical(info.dli_fname)};
#endif
        return true;
    } catch (...) { return false; }
}
bool QueryNativeModuleHandle(void* handle, NativeModuleImage& image) noexcept {
    if (!handle) return false;
    try {
#if defined(_WIN32)
        return QueryNativeModuleImage(handle, image) && image.base == reinterpret_cast<std::uintptr_t>(handle);
#elif defined(__APPLE__)
        return QueryNativeModuleImage(FindNativeModuleSymbol(handle, "charged_original_entry"), image);
#else
        link_map* map{};
        if (dlinfo(handle, RTLD_DI_LINKMAP, &map) || !map || !map->l_addr || !map->l_name)
            return false;
        image = {static_cast<std::uintptr_t>(map->l_addr), std::filesystem::canonical(map->l_name)};
        return true;
#endif
    } catch (...) { return false; }
}
void* RetainNativeModuleImage(const std::filesystem::path& path, std::uintptr_t base) noexcept {
    if (!base) return nullptr;
#if defined(_WIN32)
    // FROM_ADDRESS identifies the actual loaded image, not a reusable basename.
    // No PIN flag: every acquired reference has an explicit drained release.
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(base), &module)) return nullptr;
    if (reinterpret_cast<std::uintptr_t>(module) != base) {
        FreeLibrary(module);
        return nullptr;
    }
    (void)path; // The caller's NativeModuleFile independently proves this image.
    return module;
#else
    void* module = dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
    if (!module) return nullptr;
    NativeModuleImage image;
    if (!QueryNativeModuleHandle(module, image) || image.base != base) {
        dlclose(module);
        return nullptr;
    }
    return module;
#endif
}

bool NativeModuleOwnsReadableExtent(std::uintptr_t image, const void* pointer,
                                   std::size_t bytes) noexcept {
    if (!image || !pointer || !bytes) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
#if defined(_WIN32)
    if (!ReadableMappedExtent(image, image, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(*dos))) return false;
    const auto ntOffset = static_cast<std::uint32_t>(dos->e_lfanew);
    if (ntOffset > std::numeric_limits<std::uintptr_t>::max() - image ||
        !ReadableMappedExtent(image, image + ntOffset, sizeof(IMAGE_NT_HEADERS64))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !ContainsExtent(image, nt->OptionalHeader.SizeOfImage, address, bytes)) return false;
    const auto sectionOffset = std::uint64_t(ntOffset) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt->FileHeader.SizeOfOptionalHeader;
    const auto sectionBytes = std::uint64_t(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (sectionOffset > nt->OptionalHeader.SizeOfImage ||
        sectionBytes > nt->OptionalHeader.SizeOfImage - sectionOffset ||
        sectionOffset > std::numeric_limits<std::uintptr_t>::max() - image ||
        !ReadableMappedExtent(image, image + static_cast<std::uintptr_t>(sectionOffset),
                              static_cast<std::size_t>(sectionBytes))) return false;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(image + sectionOffset);
    for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ)) continue;
        const auto size = section.Misc.VirtualSize > section.SizeOfRawData ?
            section.Misc.VirtualSize : section.SizeOfRawData;
        if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - section.VirtualAddress) return false;
        if (ContainsExtent(image + section.VirtualAddress, size, address, bytes))
            return ReadableMappedExtent(image, address, bytes);
    }
    return false;
#elif defined(__APPLE__)
    for (std::uint32_t i = 0; i != _dyld_image_count(); ++i) {
        const auto* header = _dyld_get_image_header(i);
        if (reinterpret_cast<std::uintptr_t>(header) != image) continue;
        if (header->magic != MH_MAGIC_64) return false;
        const auto* header64 = reinterpret_cast<const mach_header_64*>(header);
        const auto* commands = reinterpret_cast<const unsigned char*>(header64 + 1);
        std::size_t offset = 0;
        const auto slide = _dyld_get_image_vmaddr_slide(i);
        for (std::uint32_t n = 0; n != header64->ncmds; ++n) {
            if (offset > header64->sizeofcmds || header64->sizeofcmds - offset < sizeof(load_command))
                return false;
            const auto* command = reinterpret_cast<const load_command*>(commands + offset);
            if (command->cmdsize < sizeof(load_command) || command->cmdsize > header64->sizeofcmds - offset)
                return false;
            if (command->cmd == LC_SEGMENT_64) {
                if (command->cmdsize < sizeof(segment_command_64)) return false;
                const auto& segment = *reinterpret_cast<const segment_command_64*>(command);
                if (std::strncmp(segment.segname, SEG_PAGEZERO, sizeof(segment.segname)) != 0 &&
                    (segment.initprot & VM_PROT_READ) != 0) {
                    std::uintptr_t begin;
                    if (slide >= 0) {
                        if (segment.vmaddr > std::numeric_limits<std::uintptr_t>::max() - std::uintptr_t(slide))
                            return false;
                        begin = std::uintptr_t(segment.vmaddr) + std::uintptr_t(slide);
                    } else {
                        const auto magnitude = std::uintptr_t(-(slide + 1)) + 1;
                        if (segment.vmaddr < magnitude) return false;
                        begin = std::uintptr_t(segment.vmaddr) - magnitude;
                    }
                    if (ContainsExtent(begin, segment.vmsize, address, bytes)) return true;
                }
            }
            offset += command->cmdsize;
        }
        return false;
    }
    return false;
#else
    struct Probe { std::uintptr_t image, address; std::size_t bytes; bool contained{}; } probe{image,address,bytes};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* context) {
        auto& p = *static_cast<Probe*>(context);
        if (info->dlpi_addr != p.image) return 0;
        for (unsigned i = 0; i != info->dlpi_phnum; ++i) {
            const auto& segment = info->dlpi_phdr[i];
            if (segment.p_type != PT_LOAD || (segment.p_flags & PF_R) == 0) continue;
            if (segment.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - p.image) continue;
            if (ContainsExtent(p.image + segment.p_vaddr, segment.p_memsz, p.address, p.bytes)) {
                p.contained = true;
                break;
            }
        }
        return 1;
    }, &probe);
    return probe.contained;
#endif
}

struct NativeModuleFile::State {
    std::filesystem::path path;
    std::string identity;
#if defined(_WIN32)
    HANDLE file{INVALID_HANDLE_VALUE};
    BY_HANDLE_FILE_INFORMATION info{};
    ~State() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
#else
    int file{-1};
    struct stat info{};
    ~State() { if (file != -1) close(file); }
#endif
};
NativeModuleFile::NativeModuleFile(const std::filesystem::path& path) : state_(new State) {
    auto& s = *state_;
    if (path.empty()) throw std::invalid_argument("Native module requires its actual image path");
    s.path = std::filesystem::canonical(path);
    std::uint64_t fingerprint = 14695981039346656037ull;
    unsigned char bytes[4096];
    char suffix[160];
#if defined(_WIN32)
    s.file = OpenImageFile(s.path);
    if (s.file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(s.file, &s.info) ||
        GetFileType(s.file) != FILE_TYPE_DISK || (s.info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !(s.info.nFileSizeHigh || s.info.nFileSizeLow))
        throw std::invalid_argument("Native module has no live regular source file");
    DWORD count{};
    do {
        if (!ReadFile(s.file, bytes, sizeof(bytes), &count, nullptr)) throw WindowsError("ReadFile image identity");
        for (DWORD i = 0; i != count; ++i) fingerprint = (fingerprint ^ bytes[i]) * 1099511628211ull;
    } while (count);
    std::snprintf(suffix, sizeof(suffix), ":%lu:%llu:%llu:%llu:%016llx",
        static_cast<unsigned long>(s.info.dwVolumeSerialNumber),
        (std::uint64_t(s.info.nFileIndexHigh) << 32) | s.info.nFileIndexLow,
        (std::uint64_t(s.info.nFileSizeHigh) << 32) | s.info.nFileSizeLow,
        (std::uint64_t(s.info.ftLastWriteTime.dwHighDateTime) << 32) | s.info.ftLastWriteTime.dwLowDateTime,
        static_cast<unsigned long long>(fingerprint));
#else
    s.file = open(s.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (s.file == -1 || fstat(s.file, &s.info) || !S_ISREG(s.info.st_mode) || s.info.st_size <= 0)
        throw std::invalid_argument("Native module has no live regular source file");
    ssize_t count;
    while ((count = read(s.file, bytes, sizeof(bytes))) > 0)
        for (ssize_t i = 0; i != count; ++i) fingerprint = (fingerprint ^ bytes[i]) * 1099511628211ull;
    if (count < 0) throw std::runtime_error("Native image identity read failed");
    std::snprintf(suffix, sizeof(suffix), ":%llu:%llu:%llu:%lld:%ld:%016llx",
        static_cast<unsigned long long>(s.info.st_dev), static_cast<unsigned long long>(s.info.st_ino),
        static_cast<unsigned long long>(s.info.st_size), static_cast<long long>(ModifiedTime(s.info).tv_sec),
        ModifiedTime(s.info).tv_nsec, static_cast<unsigned long long>(fingerprint));
#endif
    RequireUnchanged();
    const auto encoded = s.path.u8string();
    s.identity = std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()) + suffix;
}
NativeModuleFile::~NativeModuleFile() = default;
const std::filesystem::path& NativeModuleFile::Path() const noexcept { return state_->path; }
const std::string& NativeModuleFile::Identity() const noexcept { return state_->identity; }
void NativeModuleFile::RequireUnchanged() const {
    const auto& s = *state_;
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION now{}, named{};
    const auto handle = OpenImageFile(s.path);
    const bool valid = handle != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(s.file, &now) && GetFileInformationByHandle(handle, &named) &&
        SameFile(s.info, now) && SameFile(now, named);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
    struct stat now{}, named{};
    const bool valid = !fstat(s.file, &now) && !stat(s.path.c_str(), &named) &&
        SameFile(s.info, now) && SameFile(now, named);
#endif
    if (!valid) throw std::logic_error("Native source image identity changed during loader ownership");
}
bool NativeModuleFile::OwnsImage(const NativeModuleImage& image) const {
    RequireUnchanged();
    if (!image.base || image.path.empty()) return false;
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION loaded{};
    const auto handle = OpenImageFile(image.path);
    const bool valid = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &loaded) &&
        SameFile(state_->info, loaded);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    return valid;
#else
    return std::filesystem::canonical(image.path) == state_->path;
#endif
}
}

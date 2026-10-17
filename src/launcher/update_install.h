#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Installing a downloaded package over the running one. A package
// (tools/package_build.py) lists every file it installs in
// SOURCE-MANIFEST.json; only those files are replaced, so settings, saves,
// caches and logs in the same folder stay. Work happens in <game>/.update.
namespace mscharged::launcher
{
inline constexpr const char* kPackageManifest = "SOURCE-MANIFEST.json";

struct PackageFile
{
    std::string path; // relative, '/' separated
    std::string sha256;
    std::uint64_t bytes = 0;
    unsigned mode = 0644;
};

struct PackageManifest
{
    std::string version, platform, architecture;
    std::vector<PackageFile> files; // without SOURCE-MANIFEST.json itself
    // The package id in the archive names, e.g. steamdeck-x86_64.
    std::string Package() const { return platform + "-" + architecture; }
};

// Throws std::runtime_error for a missing, malformed or unsafe manifest.
PackageManifest ReadPackageManifest(const std::filesystem::path& file);

// Unpacks a package (.zip or .tar.gz) into `destination`, which must not
// exist yet, and returns the folder holding its SOURCE-MANIFEST.json. Only
// plain files and folders inside the package are accepted, every file the
// manifest lists must match its size and SHA-256, and nothing else may be in
// it. `cancelled` is polled; cancelling throws.
std::filesystem::path UnpackPackage(const std::filesystem::path& archive, const std::filesystem::path& destination,
                                    const std::function<bool()>& cancelled = {});

// Replaces the package installed in `base` by the unpacked package in
// `staged`: each file either manifest lists moves to `backup` (when present),
// then the new files move in, the manifest last. Any failure moves everything
// back and rethrows. `before_move` (tests) sees each relative path first.
void ReplacePackage(const std::filesystem::path& base, const std::filesystem::path& staged,
                    const std::filesystem::path& backup,
                    const std::function<void(const std::string&)>& before_move = {});

// <base>/.update: downloads, the unpacked package and the previous files.
std::filesystem::path UpdateFolder(const std::filesystem::path& base);

// Why the package in `base` cannot be replaced here, or empty when it can.
std::string InstallBlocker(const std::filesystem::path& base);

// Run at launcher start: finishes a replacement an interrupted launcher left
// half done, then removes what earlier updates left behind (files still in
// use stay for the next start). Returns the version the last update
// replaced, or empty.
std::string FinishUpdates(const std::filesystem::path& base);

// Downloads, unpacks and installs: see update_service.h. Marks the folder
// while files move, so that FinishUpdates can complete the move.
void InstallStagedPackage(const std::filesystem::path& base, const std::filesystem::path& staged,
                          const std::filesystem::path& backup);

// The launcher in `base` to start after an update: recharged (on macOS
// Recharged.app's program first), or mscharged in a folder that went back to a
// release from before the rename (#64).
std::filesystem::path InstalledLauncher(const std::filesystem::path& base);

// Starts `executable` (the updated launcher) with this run's command line.
// On Linux and macOS it replaces the running process and returns only on
// failure; on Windows it starts a new process and the caller exits. Returns
// the reason on failure, else empty.
std::string RestartLauncher(const std::filesystem::path& executable, int argc, char** argv);
} // namespace mscharged::launcher

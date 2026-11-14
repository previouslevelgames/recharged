#pragma once

#include "launcher/update_check.h"
#include "mscharged/build_version.h"

#include <string_view>

namespace mscharged::launcher
{
// The running build: where it comes from (MSCHARGED_BUILD_* in CMakeLists.txt)
// and the package it was made for. The installed package's manifest refines
// the package (the Steam Deck build of Linux) once the update check runs.
inline BuildIdentity CurrentBuild()
{
    BuildIdentity identity;
    identity.version = mscharged::build::version;
    const std::string_view channel = mscharged::build::channel;
    identity.origin = channel == "release" ? BuildOrigin::Release
                    : channel == "test"    ? BuildOrigin::Test
                                           : BuildOrigin::Dev;
    identity.repository = mscharged::build::repository;
    identity.commit = mscharged::build::commit;
    identity.pull_request = mscharged::build::pull_request;
    identity.run = mscharged::build::run_id;
    identity.built = mscharged::build::build_time;
    identity.commit_time = mscharged::build::commit_time;
#if defined(_WIN32)
    identity.package = "windows";
#elif defined(MSCHARGED_PLATFORM_IOS)
    identity.package = "ios";
#elif defined(__APPLE__)
    identity.package = "macos";
#else
    identity.package = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    identity.package += "-arm64";
#else
    identity.package += "-x86_64";
#endif
    return identity;
}
} // namespace mscharged::launcher

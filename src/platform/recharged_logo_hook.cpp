// The front end's logo reads RECHARGED (issue #63), inside the original game
// module: the hook of tweak patch 0022 passes each front-end scene file the
// game reads, and gets the rewritten copy when the scene draws the logo. It is
// always on; the host's RECHARGED_ORIGINAL_LOGO=1 keeps the game's own logo.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error The logo hook belongs to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_tweaks.h"
#include "platform/frontend_package.h"
#include "platform/recharged_logo.h"

#include <cstdio>
#include <exception>

namespace {
bool sRecharged = true;
// The last rewritten scene, until the next one: the game reads it right away.
mscharged::platform::FrontendPackageVector<std::byte> sScene;
} // namespace

extern "C" {
// Host entry: whether the logo reads RECHARGED (else the game's own).
__attribute__((visibility("default"))) void charged_configure_title_logo(bool recharged)
{
    sRecharged = recharged;
}
}

void* mscharged_tweak_frontend_package(void* data, unsigned long* size)
{
    if (!sRecharged || !data || !size) return data;
    const std::span<const std::byte> scene(static_cast<const std::byte*>(data), *size);
    if (mscharged::platform::RechargeLogo(scene, sScene) == 0) return data;
    // The scene's native transport accepts only a whole, consistent scene:
    // anything it would refuse keeps the disc's own.
    try
    {
        (void)mscharged::platform::PrepareFrontendPackage(sScene.data(), sScene.size());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Title logo: a front-end scene keeps its original logo (%s)\n", error.what());
        sScene.clear();
        return data;
    }
    *size = static_cast<unsigned long>(sScene.size());
    return sScene.data();
}

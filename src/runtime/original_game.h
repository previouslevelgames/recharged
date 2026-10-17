#pragma once

namespace mscharged
{
struct ResolvedLaunch;
enum class OriginalMainScene { FrontendSequence, FrontendOptions };
// Original main in the game module that matches the disc. Supplied launcher
// settings are already resolved and remain unsaved.
int RunOriginalGame(int argc, char** argv,
    const ResolvedLaunch* launch = nullptr, bool interactive = false,
    OriginalMainScene scene = OriginalMainScene::FrontendSequence, const char* title = nullptr);
}

#pragma once

namespace mscharged
{
struct ResolvedLaunch;
enum class OriginalMainScene { Credits, FrontendSequence, FrontendOptions };
// Selected original-main scene diagnostic, shared by both executable routes.
// Supplied launcher settings are already resolved and remain unsaved.
int RunOriginalMainCredits(int argc, char** argv,
    const ResolvedLaunch* launch = nullptr, bool interactive = false,
    OriginalMainScene scene = OriginalMainScene::Credits);
}

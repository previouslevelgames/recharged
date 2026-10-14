#pragma once

#include <filesystem>

namespace mscharged::runtime {

// Prepare the current device's known graphics configurations before original
// module constructors/native GX recording. Uses Aurora's window and renderer;
// no second renderer, source frames, hardware input or original timers.
// Returns false for an ordinary window close. Failure throws. The caller owns
// aurora_shutdown on cancellation/failure, before any original owners exist.
bool PrepareGraphicsForGame(const std::filesystem::path& font);

// One terminal snapshot for correlating first-use stalls with session reports.
void LogGraphicsPipelineStats();

} // namespace mscharged::runtime

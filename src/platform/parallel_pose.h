#pragma once

// Parallel posing while a replay plays (src/platform/parallel_pose.cpp, tweak
// patch 0091).
namespace mscharged::platform {

// The replay ended: one log line on how the posing went, then the counts restart.
void ReportParallelPosing();

} // namespace mscharged::platform

#pragma once
#include <array>
#include <cstdint>

namespace mscharged
{
using ColourSamples = std::array<std::array<std::uint8_t, 4>, 9>;
// Diagnostic only: snapshot the current EFB, end the frame, and wait (bounded)
// for a 3x3 colour grid at quarter/half/three-quarter coordinates. Centre is [4].
// This uses real GPU copies; GXPeekARGB is unimplemented in pinned Aurora.
ColourSamples EndFrameAndReadColours();
} // namespace mscharged

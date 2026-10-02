#pragma once
#include "runtime/lighting.h"
#include "NL/gl/glMatrixHandle.h"

namespace mscharged
{
// Per-view lifetime prevents cached frame addresses or a changed camera/light
// set from reusing stale GX state. Only the selected original routines use this.
struct GameLightingState
{
    GameLighting inputs;
    glMatrixHandle shadow_matrix = glMatrixHandle(-1);
    int shadow_generator = -1;
};
GameLightingState& ActiveGameLighting();
void BeginGameLighting(const GameLighting& lighting);
void EndGameLighting();
} // namespace mscharged

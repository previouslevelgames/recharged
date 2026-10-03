#pragma once
#include "NL/nlMath.h"
#include "runtime/lighting.h"

namespace mscharged
{
// Explicit view and lighting inputs for the selected original material programs.
// The two-argument constructor preserves the unlit diagnostic environment.
class MaterialPreviewScope
{
  public:
    MaterialPreviewScope(const nlMatrix4 &view, float time);
    MaterialPreviewScope(const nlMatrix4 &view, float time, const GameLighting& lighting,
                         const nlVector3* camera_position = nullptr);
    ~MaterialPreviewScope();
    MaterialPreviewScope(const MaterialPreviewScope &) = delete;
    MaterialPreviewScope &operator=(const MaterialPreviewScope &) = delete;
};
void RequireMaterialPreview();
const nlMatrix4 &MaterialPreviewView();
float MaterialPreviewTime();
// The active game camera can differ from the rendering view (e.g. shadow views).
// Camera-dependent materials require an explicit position; no origin fallback.
const nlVector3& MaterialPreviewCameraPosition();
void MaterialNormalMatrix(const nlMatrix4 &modelview, float output[3][4]);
void MaterialConcatMatrices(const float left[3][4], const float right[3][4], float output[3][4]);
} // namespace mscharged

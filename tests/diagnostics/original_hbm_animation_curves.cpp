// Test-only access to the private curve routines. Compile the complete original
// TU here; do not duplicate it as another object in this fixture. No copied
// animation implementation or Layout allocator is supplied by the test.
#include "src/RVL_SDK/hbm/nw4hbm/lyt/lyt_animation.cpp"

float ChargedTestHBMHermite(float frame, const nw4hbm::lyt::res::HermiteKey* keys, unsigned count)
{
    return GetHermiteCurveValue(frame, keys, count);
}

unsigned short ChargedTestHBMStep(float frame, const nw4hbm::lyt::res::StepKey* keys, unsigned count)
{
    return GetStepCurveValue(frame, keys, count);
}

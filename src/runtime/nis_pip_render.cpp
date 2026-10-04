#include "runtime/nis_pip_render.h"
#include "runtime/views.h"
#include "Game/Render/NisPipSteps.h"
#include "NL/nlString.h"
#include "NL/gl/gl.h"
#include "NL/gl/glStruct.h"
#include "NL/glx/glxTexture.h"
#include <exception>
#include <stdexcept>

namespace mscharged
{
NisPipTarget::NisPipTarget()
{
    if (!OriginalViewsReady()) throw std::logic_error("NIS PIP target requires original views");
    if (glNativeViewDispatchActive()) throw std::logic_error("Cannot create NIS PIP target during view dispatch");
    if (glGetScreenWidth() < ViewportWidth() || glGetScreenHeight() < ViewportHeight())
        throw std::invalid_argument("NIS PIP requires its original 512x256 viewport");
    if (glx_GetTex(glHash("target/pip")))
        throw std::logic_error("NIS PIP target name is already owned");
    GLTargetInfo info;
#include "Game/Render/NisPipTargetConfig.inc"
    pair_ = glCreateTarget("pip", &info);
}
NisPipTarget::~NisPipTarget()
{
    try { Release(); } catch (...) { std::terminate(); }
}
void NisPipTarget::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("NIS PIP target requires its owner thread");
}
GLRenderPair NisPipTarget::Pair() const
{
    CheckThread(); glValidateTarget(pair_); return pair_;
}
unsigned long NisPipTarget::Texture() const { return glGetTargetTexture(Pair()); }
unsigned NisPipTarget::ViewportWidth() { return NisPipViewportWidth; }
unsigned NisPipTarget::ViewportHeight() { return NisPipViewportHeight; }
void NisPipTarget::Release()
{
    CheckThread();
    if (!pair_) return;
    if (glNativeViewDispatchActive()) throw std::logic_error("Cannot release NIS PIP target during view dispatch");
    // OriginalViews may have destroyed the registry first. Validation compares
    // pointer AND generation without dereferencing the old target, so an owner
    // from an earlier session cannot destroy a subsequently allocated target.
    try { glValidateTarget(pair_); }
    catch (const std::invalid_argument&) { pair_ = {}; return; }
    glDestroyTarget(&pair_);
}
}

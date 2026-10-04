#include "runtime/views.h"
#include "runtime/material_environment.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
#include "NL/gl/glStruct.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glStartupStages.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/glxMatrix.h"
#include "NL/platvmath.h"
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <stdexcept>

namespace
{
bool views_live = false, rendering = false;
mscharged::OriginalViews* active_views = nullptr;
void (*shutdown_layers)() = nullptr;
void (*shutdown_frames)() = nullptr;
PlatformViewport viewport{};
void Packet(GLView* view, unsigned long flags, const glModelPacket* packet)
{
    if (!packet)
    {
        Mtx44 projection;
        const auto& original = *view->m_Interface->GetProjectionMatrix();
        glxCopyMatrix(projection, original);
        GXSetProjection(projection, original.m43 == -1 ? GX_PERSPECTIVE : GX_ORTHOGRAPHIC);
        GXSetCurrentMtx(GX_PNMTX0);
        return;
    }
    if (flags & 4) glSetCurrentMatrix(packet->matrix);
    if (flags & 2) glSetCurrentRasterState(packet->rasterState);
    if (flags & 0x80) mscharged::DrawMaterial(*packet, view);
}
}
PlatformViewport* glplatGetViewport() { return &viewport; }

namespace mscharged
{
bool OriginalViewsReady() { return views_live; }
void SetViewFrameShutdown(void (*shutdown)())
{
    if (shutdown && (!views_live || shutdown_frames))
        throw std::logic_error("Invalid graphics frame ownership");
    shutdown_frames = shutdown;
}
void SetViewLayerShutdown(void (*shutdown)())
{
    if (shutdown && (!views_live || shutdown_layers))
        throw std::logic_error("Invalid view layer ownership");
    shutdown_layers = shutdown;
}
OriginalViews::OriginalViews(unsigned width, unsigned height, void (*drain)())
{
    if (views_live || !glGetCurrentResourcePool() || !width || !height || width > 1024 || height > 1024)
        throw std::invalid_argument("Invalid original view startup or framebuffer size");
    auto* screen = glGetScreenInfo();
    screen->ScreenWidth = width; screen->ScreenHeight = height;
    try
    {
        InitializeNativeTargets(width, height, drain);
        gl_StartupViews();
        active_views = this;
        SetGraphicsViewShutdown([] { active_views->Release(); });
        views_live = live_ = true;
    }
    catch (...)
    {
        gl_ViewShutdown(); gl_TargetShutdown(); ShutdownNativeTargets();
        *screen = {};
        throw;
    }
}
OriginalViews::~OriginalViews() { Release(); }
void OriginalViews::Release()
{
    if (!live_) return;
    if (rendering) throw std::logic_error("Cannot shut down views during rendering");
    if (shutdown_frames) shutdown_frames();
    if (shutdown_layers) shutdown_layers();
    gl_ViewShutdown(); // Release game-arena slot blocks before targets and graphics memory.
    gl_TargetShutdown();
    ShutdownNativeTargets();
    SetGraphicsViewShutdown(nullptr);
    active_views = nullptr;
    *glGetScreenInfo() = {};
    views_live = live_ = false;
}
void ViewMatrices::GetInverseViewMatrix(nlMatrix4& out) const
{
    nlInvertMatrix(out, view);
}
void ViewMatrices::GetViewProjectionMatrix(nlMatrix4& out) const { nlMultMatrices(out, view, projection); }

void DispatchOriginalViews(float time, const GameLighting& lighting, const nlVector3* camera_position)
{
    if (!views_live || rendering) throw std::logic_error("Invalid or nested GL view dispatch");
    rendering = true;
    glNativeSetViewDispatch(true);
    struct Finish
    {
        ~Finish() { rendering = false; glNativeSetViewDispatch(false); }
    } finish;
    for (GLViewIterator iterator(&gRootView); !iterator.IsDone(); iterator.Next())
    {
        auto* view = iterator.Current();
        const auto& rect = view->m_Viewport;
        if (!rect.width || !rect.height) continue;
        if (rect.x > glGetScreenWidth() || rect.y > glGetScreenHeight()
            || rect.width > glGetScreenWidth() - rect.x || rect.height > glGetScreenHeight() - rect.y)
            throw std::out_of_range("GL view viewport exceeds the EFB");
        auto pair = view->GetRenderPair();
        glValidateTarget(pair);
        pair.target->Activate(0);
        viewport = {int(rect.x), int(rect.y), int(rect.width), int(rect.height)};
        GXSetViewport(rect.x, rect.y, rect.width, rect.height, 0, 1);
        GXSetScissor(rect.x, rect.y, rect.width, rect.height);
        if (view->m_ClearColour || view->m_ClearDepth || view->m_Unknown32)
            pair.target->ClearBuffers(view->m_ClearColour, view->m_ClearDepth, view->m_Unknown32);
        if (!view->m_Visible) continue;
        MaterialPreviewScope environment(*view->m_Interface->GetViewMatrix(), time, lighting, camera_position);
        view->Iterate(Packet);
        if (view->m_Target == 8 || view->m_Target == 9 || view->m_Target == 10)
            pair.target->CopyToTexture(view->m_Target != 8, view->m_Target == 10);
        else if (view->m_Target != GLViewTarget_None)
            throw std::invalid_argument("Unsupported GL view target copy mode");
    }
}
void RenderOriginalViews(float time, const GameLighting& lighting, const nlVector3* camera_position)
{
    // Standalone material diagnostics still own their own frame reset.
    // Nested dispatch must reject before installing a reset guard.
    if (!views_live || rendering) throw std::logic_error("Invalid or nested GL view dispatch");
    struct Reset { ~Reset() { gl_ViewReset(); } } reset;
    DispatchOriginalViews(time, lighting, camera_position);
}
}

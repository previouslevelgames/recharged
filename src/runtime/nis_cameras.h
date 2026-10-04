#pragma once
#include "runtime/animated_camera.h"
#include "runtime/nis_camera_assets.h"
#include <array>
#include <memory>

namespace mscharged
{
class NisCameras;
// Camera-facing lifetime of one explicitly supplied NIS. This retains decoded
// tracks, not actor/script data or an original Nis instance. Render modes match
// original 0/1/2; mode 2 remains unchanged by Swap. No Presentation RNG is used.
class NisCameraBinding
{
    friend class NisCameras;
    std::thread::id thread_ = std::this_thread::get_id();
    std::vector<CameraAsset::Handle> tracks_;
    NisCameras* owner_ = nullptr;
    unsigned slot_ = 0;
    int render_mode_;
    bool mirrored_, released_ = false;
    void CheckThread() const;
public:
    NisCameraBinding(const NisCameraAssets& assets, int render_mode, bool mirrored = false);
    ~NisCameraBinding();
    NisCameraBinding(const NisCameraBinding&) = delete;
    NisCameraBinding& operator=(const NisCameraBinding&) = delete;
    const cBaseCamera* Camera() const;
    int RenderMode() const;
    void Release(); // Unselect only this binding, even after repeated swaps.
};

// Two stable borrowed manager identities. Swap exchanges retained playback
// state and binding backlinks, never intrusive links, filters or BaseCam data.
// The core and game arenas must outlive this object (core.Release is allowed).
class NisCameras
{
    friend class NisCameraBinding;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void Detach(NisCameraBinding& binding);
public:
    explicit NisCameras(OriginalCameras& core);
    ~NisCameras();
    NisCameras(const NisCameras&) = delete;
    NisCameras& operator=(const NisCameras&) = delete;
    // Index is nonnegative and wraps by the supplied NIS camera count, as in
    // original SelectCamera. A binding can occupy only one slot at a time.
    // Selection validates a time-zero pose before publication; original Nis
    // defers rebuilding its selected view until ManualUpdate.
    void Select(unsigned slot, NisCameraBinding& binding, int index);
    void Activate(); // Push primary; secondary is never a manager entry.
    void Swap(); // This initial profile requires two selected tracks.
    // Original manual advancement of both tracks. The manager-facing Update
    // does not advance them again. Call core.Advance separately to publish pose.
    std::array<float, 2> Advance(float delta);
    void Seek(unsigned slot, float normalized_time);
    void SetOffset(unsigned slot, nlVector3 offset); // Applied at next advance/seek.
    void SetInputs(unsigned slot, AnimatedCameraInputs inputs);
    void SetEndCallback(unsigned slot, void (*callback)());
    const cBaseCamera* Camera(unsigned slot) const;
    float Time(unsigned slot) const;
    float Duration(unsigned slot) const;
    float TimeLeft() const;
    float CameraTimeLeft(unsigned slot) const;
    bool Failed() const;
    void Release(); // No callbacks; allowed after failed playback/core.Release.
};
}

#include "runtime/nis_cameras.h"
#include "Game/Camera/CameraMan.h"
#include <exception>
#include <optional>
#include <stdexcept>

namespace mscharged
{
struct NisCameras::Impl
{
    struct Playback
    {
        AnimatedCamera camera;
        AnimatedCameraOptions options;
        AnimatedCameraInputs inputs;
        bool pending = false;
        Playback(CameraAsset::Handle asset, AnimatedCameraOptions settings, AnimatedCameraInputs input)
            : camera(std::move(asset)), options(settings), inputs(input)
        { camera.SetInputs(inputs); camera.Configure(options); }
        void Apply()
        {
            if (pending) { camera.Configure(options); pending = false; }
        }
    };
    struct Slot final : cBaseCamera
    {
        std::unique_ptr<Playback> playback;
        NisCameraBinding* binding = nullptr;
        cBaseCamera& Body() const
        {
            if (!playback) throw std::logic_error("NIS camera slot is unselected");
            return playback->camera.Camera();
        }
        eCameraType GetType() override { return eCameraType_Animated; }
        void Update(float delta) override { CheckCameraDelta(delta); } // Original m_LetManagerDoUpdate=false.
        const nlMatrix4& GetViewMatrix() const override { return Body().GetViewMatrix(); }
        const nlVector3& GetCameraPosition() const override { return Body().GetCameraPosition(); }
        const nlVector3& GetTargetPosition() const override { return Body().GetTargetPosition(); }
        float GetFOV() const override { return Body().GetFOV(); }
    };
    OriginalCameras& core;
    std::thread::id thread = std::this_thread::get_id();
    std::array<std::optional<Slot>, 2> slots;
    bool busy = false, failed = false, released = false;
    explicit Impl(OriginalCameras& camera_core) : core(camera_core) {}
    void CheckThread() const
    {
        if (thread != std::this_thread::get_id()) throw std::logic_error("NIS cameras require their owner thread");
    }
    void Ready() const
    {
        CheckThread();
        if (busy || failed || released || !core.Active()) throw std::logic_error("NIS cameras are busy, failed or released");
        { NativeCameraCall check; }
        if (slots[1] && cCameraManager::HasCamera(const_cast<Slot*>(&*slots[1])))
            throw std::logic_error("Secondary NIS camera was inserted into the manager externally");
    }
    Slot& Get(unsigned index) const
    {
        if (index >= slots.size()) throw std::out_of_range("NIS camera slot must be 0 or 1");
        if (!slots[index] || !slots[index]->playback) throw std::logic_error("NIS camera slot is unselected");
        return const_cast<Slot&>(*slots[index]);
    }
    void Clear(unsigned index)
    {
        auto& slot = slots[index];
        if (!slot) return;
        if (slot->binding) slot->binding->owner_ = nullptr;
        slot.reset(); // Borrowed BaseCam destructor safely unlinks even after failure.
    }
    void TeardownReady() const
    {
        CheckThread();
        if (busy) throw std::logic_error("Cannot release NIS cameras during a callback");
        if (core.Active()) CheckNativeCameraTeardown();
    }
    struct Operation
    {
        Impl& owner;
        int exceptions = std::uncaught_exceptions();
        explicit Operation(Impl& value) : owner(value) { owner.busy = true; }
        ~Operation() { owner.failed |= std::uncaught_exceptions() > exceptions; owner.busy = false; }
    };
};

NisCameraBinding::NisCameraBinding(const NisCameraAssets& assets, int mode, bool mirrored)
    : render_mode_(mode), mirrored_(mirrored)
{
    if (mode < 0 || mode > 2) throw std::invalid_argument("NIS render mode must be 0, 1 or 2");
    tracks_.reserve(assets.Layout().cameras.size());
    for (std::size_t i = 0; i < assets.Layout().cameras.size(); ++i) tracks_.push_back(assets.Camera(i));
}
NisCameraBinding::~NisCameraBinding() { try { Release(); } catch (...) { std::terminate(); } }
void NisCameraBinding::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("NIS camera binding requires its owner thread");
}
const cBaseCamera* NisCameraBinding::Camera() const
{ CheckThread(); return owner_ ? owner_->Camera(slot_) : nullptr; }
int NisCameraBinding::RenderMode() const { CheckThread(); return render_mode_; }
void NisCameraBinding::Release()
{
    CheckThread();
    if (owner_) owner_->Detach(*this);
    tracks_.clear(); released_ = true;
}

NisCameras::NisCameras(OriginalCameras& core) : impl_(std::make_unique<Impl>(core)) { impl_->Ready(); }
NisCameras::~NisCameras() { try { Release(); } catch (...) { std::terminate(); } }
void NisCameras::Select(unsigned index, NisCameraBinding& binding, int track)
{
    auto& self = *impl_; self.Ready(); binding.CheckThread();
    if (index >= 2) throw std::out_of_range("NIS camera slot must be 0 or 1");
    if (track < 0 || binding.tracks_.empty() || binding.released_)
        throw std::invalid_argument("NIS selection needs a nonnegative index and retained camera tracks");
    if (binding.owner_ && (binding.owner_ != this || binding.slot_ != index))
        throw std::logic_error("NIS binding already belongs to another slot");
    auto& slot = self.slots[index];
    AnimatedCameraOptions options;
    AnimatedCameraInputs inputs;
    if (slot && slot->playback) { options = slot->playback->options; inputs = slot->playback->inputs; }
    options.cyclic = false; options.mirror = {binding.mirrored_ ? -1.f : 1.f, 1, 1};
    Impl::Operation operation(self);
    auto next = std::make_unique<Impl::Playback>(binding.tracks_[std::size_t(track) % binding.tracks_.size()], options, inputs);
    if (!slot) slot.emplace();
    if (slot->binding) slot->binding->owner_ = nullptr;
    slot->playback = std::move(next); slot->binding = &binding;
    binding.owner_ = this; binding.slot_ = index;
}
void NisCameras::Activate()
{
    auto& self = *impl_; self.Ready(); auto& primary = self.Get(0);
    if (cCameraManager::PeekCamera() == &primary) return;
    Impl::Operation operation(self);
    TrackNativeCamera(&primary); // Allocate tracking before a stack mutation.
    cCameraManager::Remove(primary); cCameraManager::PushCamera(&primary);
}
void NisCameras::Swap()
{
    auto& self = *impl_; self.Ready(); auto& primary = self.Get(0); auto& secondary = self.Get(1);
    Impl::Operation operation(self);
    TrackNativeCamera(&primary);
    cCameraManager::Remove(primary);
    primary.playback.swap(secondary.playback); std::swap(primary.binding, secondary.binding);
    for (unsigned i = 0; i < 2; ++i)
    {
        auto& binding = *self.slots[i]->binding;
        binding.slot_ = i;
        if (binding.render_mode_ == 0) binding.render_mode_ = 1;
        else if (binding.render_mode_ == 1) binding.render_mode_ = 0;
    }
    cCameraManager::PushCamera(&primary);
}
std::array<float, 2> NisCameras::Advance(float delta)
{
    auto& self = *impl_; self.Ready(); CheckCameraDelta(delta);
    Impl::Operation operation(self); std::array<float, 2> overrun{};
    for (unsigned i = 0; i < 2; ++i)
        if (self.slots[i] && self.slots[i]->playback)
        {
            auto& playback = *self.slots[i]->playback; playback.Apply(); overrun[i] = playback.camera.Advance(delta);
        }
    return overrun;
}
void NisCameras::Seek(unsigned slot, float time)
{
    auto& self = *impl_; self.Ready(); CheckCameraDelta(time); auto& playback = *self.Get(slot).playback;
    Impl::Operation operation(self); playback.Apply(); playback.camera.Seek(time);
}
void NisCameras::SetOffset(unsigned slot, nlVector3 offset)
{
    auto& self = *impl_; self.Ready(); CheckCameraVector(offset); auto& playback = *self.Get(slot).playback;
    playback.options.offset = offset; playback.pending = true;
}
void NisCameras::SetInputs(unsigned slot, AnimatedCameraInputs inputs)
{
    auto& self = *impl_; self.Ready(); CheckCameraDelta(inputs.simulation_time); auto& playback = *self.Get(slot).playback;
    Impl::Operation operation(self); playback.camera.SetInputs(inputs); playback.inputs = inputs;
}
void NisCameras::SetEndCallback(unsigned slot, void (*callback)())
{
    auto& self = *impl_; self.Ready(); auto& playback = *self.Get(slot).playback;
    playback.options.on_end = callback; playback.pending = true;
}
const cBaseCamera* NisCameras::Camera(unsigned slot) const
{
    impl_->Ready();
    if (slot >= 2) throw std::out_of_range("NIS camera slot must be 0 or 1");
    return impl_->slots[slot] && impl_->slots[slot]->playback ? &*impl_->slots[slot] : nullptr;
}
float NisCameras::Time(unsigned slot) const { impl_->Ready(); return impl_->Get(slot).playback->camera.Time(); }
float NisCameras::Duration(unsigned slot) const { impl_->Ready(); return impl_->Get(slot).playback->camera.Duration(); }
float NisCameras::TimeLeft() const
{
    impl_->Ready(); float remaining = 0;
    for (const auto& slot : impl_->slots)
        if (slot && slot->playback) remaining = std::max(remaining, slot->playback->camera.TimeLeft());
    if (impl_->slots[0] && impl_->slots[0]->playback)
        remaining = std::min(remaining, impl_->slots[0]->playback->camera.TimeLeft());
    return remaining;
}
float NisCameras::CameraTimeLeft(unsigned slot) const
{
    impl_->Ready();
    if (slot >= 2) throw std::out_of_range("NIS camera slot must be 0 or 1");
    if (!impl_->slots[slot] || !impl_->slots[slot]->playback) return -1;
    float remaining = impl_->slots[slot]->playback->camera.TimeLeft();
    if (impl_->slots[0] && impl_->slots[0]->playback)
        remaining = std::min(remaining, impl_->slots[0]->playback->camera.TimeLeft());
    return remaining;
}
bool NisCameras::Failed() const { impl_->CheckThread(); return impl_->failed; }
void NisCameras::Detach(NisCameraBinding& binding)
{
    auto& self = *impl_; self.TeardownReady();
    if (binding.owner_ != this || !self.slots[binding.slot_] || self.slots[binding.slot_]->binding != &binding)
        throw std::logic_error("NIS camera binding backlink changed externally");
    self.Clear(binding.slot_);
}
void NisCameras::Release()
{
    auto& self = *impl_; self.TeardownReady();
    self.Clear(0); self.Clear(1); self.released = true;
}
}

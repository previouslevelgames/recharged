#pragma once
#include "resources/nis_camera.h"
#include "runtime/camera_assets.h"

namespace mscharged
{
// Owns embedded CAM records independently of the NIS buffer. This does not
// construct Nis/NisPlayer, publish a global registry or load actor/script data.
// Retained handles may outlive this collection; release them before game memory.
class NisCameraAssets
{
    std::thread::id thread_ = std::this_thread::get_id();
    resources::NisCameraLayout layout_;
    std::vector<CameraAsset::Handle> cameras_;
    void CheckThread() const;
public:
    NisCameraAssets(resources::Bytes file, const std::string& name);
    NisCameraAssets(const NisCameraAssets&) = delete;
    NisCameraAssets& operator=(const NisCameraAssets&) = delete;
    const resources::NisCameraLayout& Layout() const;
    CameraAsset::Handle Camera(std::size_t index) const;
};
}

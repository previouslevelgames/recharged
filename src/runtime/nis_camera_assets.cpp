#include "runtime/nis_camera_assets.h"
#include "NL/nlMemory.h"

namespace mscharged
{
NisCameraAssets::NisCameraAssets(resources::Bytes file, const std::string& name)
{
    CheckThread();
    const auto alias = CanonicalCameraAlias(name);
    if (alias.size() > 253) throw std::invalid_argument("NIS camera alias leaves no room for its slot suffix");
    layout_ = resources::ReadNisCameras(file);
    cameras_.reserve(layout_.cameras.size());
    for (std::size_t i = 0; i < layout_.cameras.size(); ++i)
    {
        const auto& range = layout_.cameras[i];
        cameras_.push_back(CameraAsset::Decode(file, alias + "_" + std::to_string(i), range.offset, range.end));
    }
}
void NisCameraAssets::CheckThread() const
{
    if (!gMemoryInitialized || thread_ != std::this_thread::get_id())
        throw std::logic_error("NIS camera records require their initialized memory and owner thread");
}
const resources::NisCameraLayout& NisCameraAssets::Layout() const { CheckThread(); return layout_; }
CameraAsset::Handle NisCameraAssets::Camera(std::size_t index) const
{
    CheckThread();
    if (index >= cameras_.size()) throw std::out_of_range("NIS camera slot is outside the loaded range");
    return cameras_[index];
}
}

#include "runtime/frontend_camera_assets.h"
#include <iterator>

namespace mscharged
{
namespace
{
constexpr CameraAnimationLoadInfo catalog[] = {
#include "Game/Camera/FrontendCameraCatalog.inc"
};
static_assert(std::size(catalog) <= MaximumCameraBatchRequests);
}
std::span<const CameraAnimationLoadInfo> FrontendCameraCatalog() { return catalog; }
CameraAssetBatch LoadFrontendCameraAssets(CameraAssetLibrary& destination)
{
    std::vector<CameraBatchRequest> requests;
    requests.reserve(std::size(catalog));
    for (const auto& entry : catalog) requests.push_back({entry.animationName, entry.fileName});
    return CameraAssetBatch(destination, requests);
}
}

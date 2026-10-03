#pragma once
#include "runtime/camera_batch.h"
#include "Game/Camera/FrontendCameraCatalog.h"
#include <span>

namespace mscharged
{
// Original frontend filenames/aliases/order, shared with the console factory.
std::span<const CameraAnimationLoadInfo> FrontendCameraCatalog();
// The returned owner uses the existing explicit Service/Poll/Publish contract.
// The destination and game memory must outlive it. This loads named assets;
// it does not create gameplay cameras/filters or mutate cAnimCamera's registry.
CameraAssetBatch LoadFrontendCameraAssets(CameraAssetLibrary& destination);
}

#pragma once
#include "resources/camera_animation.h"

namespace mscharged::resources
{
inline constexpr std::size_t MaximumNisCameras = 10;
struct NisCameraRange
{
    std::size_t offset = 0, end = 0;
};
struct NisCameraLayout
{
    // File order matches original Nis::mCameraData. Ranges are absolute byte
    // offsets, not pointers; each CAM has passed the existing channel decoder.
    std::vector<NisCameraRange> cameras;
    std::size_t animation_chunks = 0, other_chunks = 0;
};

// A NIS is a sequence of top-level chunks, without an extra wrapper. This
// validates the sequence and embedded CAMs only. SANIM and other payloads are
// counted, not interpreted; scripts, actor data and full NIS playback remain
// separate. A nonempty container with no cameras is a valid empty selection.
NisCameraLayout ReadNisCameras(Bytes file);
}

#include "runtime/startup_animation.h"
#include "Game/SAnimDecode.h"
#include "Game/SAnim.h"

#include <stdexcept>

namespace mscharged
{
const char* VerifyStartupAnimationDecoders()
{
    const unsigned char rot16[] = {0x80, 0x00, 0x7f, 0xff, 0xff, 0xff, 0x12, 0x34};
    const unsigned char rot12[] = {0x7f, 0xf0, 0x80, 0x00, 0x1f, 0xff};
    const unsigned char rot8[] = {0x80, 0x7f, 0xff, 0x12};
    nlQuaternion rotation{};
    SAnimDecodeRot16(&rotation, rot16);
    if (rotation.x != -1.0f || rotation.y != 0.999969482421875f
        || rotation.z != -0.000030517578125f || rotation.w != 0.1422119140625f)
        throw std::runtime_error("Native SAnim 16-bit rotation check failed.");
    SAnimDecodeRot12(&rotation, rot12);
    if (rotation.x != 0.99951171875f || rotation.y != -1.0f
        || rotation.z != 0.00048828125f || rotation.w != -0.00048828125f)
        throw std::runtime_error("Native SAnim 12-bit rotation check failed.");
    SAnimDecodeRot8(&rotation, rot8);
    if (rotation.x != -1.0f || rotation.y != 0.9921875f
        || rotation.z != -0.0078125f || rotation.w != 0.140625f)
        throw std::runtime_error("Native SAnim 8-bit rotation check failed.");
    const PackedScale packed{0, 2048, 65535};
    nlVector3 scale{};
    SAnimDecodeScale(&scale, &packed);
    const unsigned char full = 255;
    float weight = -1, morph = -1;
    SAnimDecodeWeight(&weight, &full);
    SAnimDecodeMorphWeight(&morph, &full);
    if (scale.x != 0 || scale.y != 1 || scale.z != 31.99951171875f
        || weight != 1 || morph != 1)
        throw std::runtime_error("Original SAnim scale/weight check failed.");
    return "Native SAnim decoders verified: 16/12/8-bit rotations, unsigned scale and byte weights.";
}
}

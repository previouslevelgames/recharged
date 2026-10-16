#include "platform/wiimote_ir.h"

#include <cmath>

namespace mscharged::platform {
const std::uint8_t kWiimoteIrBlock1[5][9] = {
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xfe},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xb4},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xc8, 0x00, 0x36},
    {0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20}};
const std::uint8_t kWiimoteIrBlock2[5][2] = {{0xfd, 0x05}, {0xb3, 0x04}, {0x63, 0x03}, {0x35, 0x03}, {0x1f, 0x03}};

NativeDpdObservation DecodeWiimoteBasicIr(const std::uint8_t* ir) {
    // Two 5-byte groups of two objects with 10-bit coordinates, converted like
    // original WPADHIDParser (WPAD_DPD_BASIC): x as sent, y = 767 - sent y;
    // x 1023 or y 767 means no object (0, 767, size 0), a present one has size 12.
    NativeDpdObservation dots{};
    for (int group = 0; group < 2; ++group) {
        const std::uint8_t* bytes = ir + group * 5;
        const int x[2]{bytes[0] | ((bytes[2] >> 4) & 3) << 8, bytes[3] | (bytes[2] & 3) << 8};
        const int y[2]{bytes[1] | ((bytes[2] >> 6) & 3) << 8, bytes[4] | ((bytes[2] >> 2) & 3) << 8};
        for (int n = 0; n < 2; ++n) {
            auto& object = dots[group * 2 + n];
            object.trace_id = static_cast<std::uint8_t>(group * 2 + n);
            const int flipped = 767 - y[n];
            const bool absent = x[n] == 1023 || flipped == 767;
            object.x = static_cast<std::int16_t>(absent ? 0 : x[n]);
            object.y = static_cast<std::int16_t>(absent ? 767 : flipped);
            object.size = absent ? 0 : 12;
        }
    }
    return dots;
}

std::array<double, 2> KpadPointerCentre(double x, double y, std::uint8_t sensor_bar_position,
                                        bool sensor_height) {
    // KPAD get_kobj: raw*2/1024 - (resolution-1)/1024. KPADCalibrateDPD
    // requests height +/-0.2; KPADSetSensorHeight stores the negative height.
    // Without it the centre stays 0. calc_dpd2pos_scale gives
    // sqrt(1^2 + .75^2)/min(1, .75 - |centre|): 1.25/.55, or 1.25/.75.
    const double centre_y = !sensor_height ? 0.0 : sensor_bar_position == 1 ? -0.2 : 0.2;
    const double camera_scale = 1.25 / (0.75 - std::fabs(centre_y));
    return {(1023.0 - 1024.0 * (2.0 * x - 1.0) / camera_scale) * 0.5,
            (767.0 + 1024.0 * (centre_y - (2.0 * y - 1.0) / camera_scale)) * 0.5};
}

} // namespace mscharged::platform

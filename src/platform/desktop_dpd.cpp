#include "platform/desktop_dpd.h"

#include <cmath>
#include <stdexcept>

namespace mscharged::platform {
NativeDpdObservation MakeDesktopDpdObservation(float x, float y,
                                               std::uint8_t sensor_bar_position,
                                               bool visible) {
    NativeDpdObservation result{};
    for (std::size_t n = 0; n < result.size(); ++n)
        result[n].trace_id = static_cast<std::uint8_t>(n);
    if (sensor_bar_position > 1)
        throw std::invalid_argument("Unknown virtual camera sensor-bar position");
    if (!visible) return result;
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1)
        throw std::invalid_argument("Desktop camera observation outside its presented content");

    // KPAD get_kobj: raw*2/1024 - (resolution-1)/1024. KPADCalibrateDPD
    // requests height +/-0.2; KPADSetSensorHeight stores the negative height.
    // calc_dpd2pos_scale gives sqrt(1^2 + .75^2)/(.75 - .2).
    constexpr double camera_scale = 1.25 / 0.55;
    const double centre_y = sensor_bar_position == 1 ? -0.2 : 0.2;
    const double raw_x = (1023.0 - 1024.0 * (2.0 * double(x) - 1.0) / camera_scale) * 0.5;
    const double raw_y = (767.0 + 1024.0 * (centre_y - (2.0 * double(y) - 1.0) / camera_scale)) * 0.5;
    // Source kp_obj_interval=.2, dist_vv1=.2/.383864, idist_org=1.
    // The pair is a declared upright virtual unit-distance sensor bar.
    constexpr double half_span = (0.2 / 0.383864) * 256.0;
    for (int n = 0; n < 2; ++n) {
        const auto px = std::lround(raw_x + (n ? half_span : -half_span));
        const auto py = std::lround(raw_y);
        if (px < 0 || px >= 1023 || py < 0 || py >= 767)
            throw std::out_of_range("Calibrated virtual camera pair exceeds the raw sensor domain");
        result[n].x = static_cast<std::int16_t>(px);
        result[n].y = static_cast<std::int16_t>(py);
        // Basic WPAD DPD parser uses size12 for a visible raw object.
        // KPAD consumes its presence, not a fabricated source validity flag.
        result[n].size = 12;
    }
    return result;
}
} // namespace mscharged::platform

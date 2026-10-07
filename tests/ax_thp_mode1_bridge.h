#pragma once
#include <cstdint>
struct ChargedAXTHPResult {
    std::uint32_t checks{}, decoded_frames{}, ring_full{}, read_wait{};
    std::uint32_t width{},height{},frames{},work_bytes{};
};

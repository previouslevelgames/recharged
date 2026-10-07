#pragma once

#include <cstdint>

namespace mscharged::platform {
// A Wii scalar in the original object's raw stream. This does not select a
// game object, advance its cursor, construct it or grant source readiness.
std::uint32_t ReadWorldRecordWireWord(const void* source);
}

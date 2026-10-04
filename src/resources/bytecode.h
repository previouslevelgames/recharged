#pragma once
#include "resources/binary_reader.h"
#include <memory>
#include <vector>

namespace mscharged::resources
{
struct ScriptFunction
{
    std::uint32_t hash, offset; // Original byte offset; never a native address.
    std::uint16_t frame_size;
    std::uint8_t arguments, flags;
};
struct ScriptBytecode
{
    std::vector<ScriptFunction> functions;
    std::vector<std::uint8_t> tweaks, strings;
    std::vector<std::uint32_t> globals, data;
    std::vector<std::uint16_t> code;
    std::uint32_t num_globals = 0, first_float_tweak = 0, first_bool_tweak = 0,
                  num_variables = 0, num_string_refs = 0;
};
// Parse the 72-byte Wii file header and segments without relocating 32-bit
// words into host pointers. All instruction operands with static bounds are
// checked here; stack/frame bounds and service calls require a runtime owner.
std::shared_ptr<const ScriptBytecode> ReadScriptBytecode(Bytes bytes);
}

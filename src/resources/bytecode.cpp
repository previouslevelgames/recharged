#include "resources/bytecode.h"
#include <algorithm>

namespace mscharged::resources
{
std::shared_ptr<const ScriptBytecode> ReadScriptBytecode(Bytes bytes)
{
    Require(bytes.size() >= 72 && bytes.size() <= MaximumAssetBytes, "Invalid bytecode file size");
    Require(U32(bytes, 0) == 0xe11c2112, "Invalid bytecode signature");
    const auto count = U32(bytes, 4), tweak_size = U32(bytes, 8), global_size = U32(bytes, 12),
        data_size = U32(bytes, 16), code_size = U32(bytes, 20), string_size = U32(bytes, 24);
    Require(count <= 4096 && global_size % 4 == 0 && data_size % 4 == 0 && code_size % 2 == 0,
            "Invalid bytecode segment count or alignment");
    for (std::size_t offset = 48; offset < 72; offset += 4)
        Require(U32(bytes, offset) == 0, "Bytecode file contains relocated pointers");
    auto result = std::make_shared<ScriptBytecode>();
    result->num_globals = U32(bytes, 28); result->first_float_tweak = U32(bytes, 32);
    result->first_bool_tweak = U32(bytes, 36); result->num_variables = U32(bytes, 40);
    result->num_string_refs = U32(bytes, 44);
    Require(result->num_globals <= global_size / 4 && result->num_globals <= result->first_float_tweak
        && result->first_float_tweak <= result->first_bool_tweak
        && result->first_bool_tweak <= result->num_variables && result->num_variables <= 2048
        && result->num_string_refs <= result->num_globals, "Invalid bytecode variable ranges");
    std::size_t offset = 72;
    auto take = [&](std::size_t size) { auto part = Slice(bytes, offset, size); offset += size; return part; };
    const auto functions = take(std::size_t(count) * 12);
    result->functions.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto p = i * 12;
        ScriptFunction entry{U32(functions, p), U32(functions, p + 4), U16(functions, p + 8),
            functions[p + 10], functions[p + 11]};
        Require(entry.offset % 2 == 0 && entry.offset < code_size && entry.frame_size >= 2 && entry.flags <= 3,
                "Invalid bytecode function entry");
        Require(i == 0 || result->functions.back().hash < entry.hash, "Bytecode function hashes are not strictly sorted");
        result->functions.push_back(entry);
    }
    const auto tweaks = take(tweak_size); result->tweaks.assign(tweaks.begin(), tweaks.end());
    auto words = [&](std::uint32_t size, auto& destination) {
        const auto segment = take(size);
        destination.reserve(size / 4);
        for (std::size_t i = 0; i < segment.size(); i += 4) destination.push_back(U32(segment, i));
    };
    words(global_size, result->globals); words(data_size, result->data);
    const auto code = take(code_size); result->code.reserve(code_size / 2);
    for (std::size_t i = 0; i < code.size(); i += 2) result->code.push_back(U16(code, i));
    const auto strings = take(string_size); result->strings.assign(strings.begin(), strings.end());
    Require(offset == bytes.size(), "Bytecode has trailing data");
    const auto last_nul = std::find(strings.rbegin(), strings.rend(), 0);
    const auto terminated_end = last_nul == strings.rend() ? 0 : strings.size() - (last_nul - strings.rbegin());
    auto string = [&](std::uint32_t p) {
        Require(p < terminated_end,
                "Bytecode string offset is out of bounds or unterminated");
    };
    for (std::size_t i = 0; i < result->code.size(); ++i)
    {
        const unsigned opcode = result->code[i] >> 11, operand = result->code[i] & 2047;
        Require(opcode <= 21, "Unknown bytecode opcode");
        switch (opcode)
        {
        case 0: case 1:
            Require(operand < result->data.size(), "Bytecode data operand is out of bounds");
            if (opcode == 1) string(result->data[operand]);
            break;
        case 3: string(operand); break;
        case 4: case 5:
            Require(operand < result->code.size() - i, "Bytecode forward branch leaves its code segment"); break;
        case 6: case 7:
            Require(operand <= i, "Bytecode backward branch leaves its code segment"); break;
        case 9: Require(operand < count, "Bytecode function call is out of bounds"); break;
        case 13: Require(operand < 40 && operand != 33, "Bytecode operation is unavailable"); break;
        case 14: case 15:
            Require(operand < result->num_variables, "Bytecode variable operand is out of bounds"); break;
        default: break; // Stack offsets and service operands require runtime checks.
        }
    }
    return result;
}
}

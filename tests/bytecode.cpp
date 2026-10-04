#include "resources/bytecode.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace mscharged::resources;
namespace
{
using Blob = std::vector<std::uint8_t>;
unsigned checks = 0;
void Check(bool good, const char* message) { ++checks; if (!good) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid bytecode accepted"); }
void Set(Blob& b, std::size_t p, std::uint32_t v)
{ for (int shift : {24, 16, 8, 0}) b.at(p++) = v >> shift; }
void Instruction(Blob& b, unsigned index, unsigned opcode, unsigned operand)
{ const unsigned value = opcode * 2048 + operand; b.at(88 + index * 2) = value >> 8; b.at(89 + index * 2) = value; }
Blob Fixture()
{
    Blob b(103); Set(b, 0, 0xe11c2112); Set(b, 4, 1); Set(b, 16, 4); Set(b, 20, 10); Set(b, 24, 5);
    Set(b, 72, 0x12345678); b[81] = 2; // Function offset0, frame2, no arguments/return.
    Set(b, 84, 0xdeadbeef);
    Instruction(b, 0, 0, 0); Instruction(b, 1, 3, 1); Instruction(b, 2, 8, 17);
    Instruction(b, 3, 16, 254); Instruction(b, 4, 10, 0);
    b[99] = 'a'; b[100] = 'b'; b[101] = 'c';
    return b;
}
}
int main(int argc, char** argv)
{
    try
    {
        auto bytes = Fixture(); auto script = ReadScriptBytecode(bytes);
        Check(script->functions.size() == 1 && script->functions[0].hash == 0x12345678
            && script->functions[0].frame_size == 2 && script->data[0] == 0xdeadbeef,
            "Bytecode numeric words or function fields changed");
        bytes.clear(); Check(script->strings[1] == 'a' && script->code.size() == 5, "Bytecode retained input storage");
        const auto valid = Fixture();
        for (std::size_t size = 0; size < valid.size(); ++size)
            Reject([&] { ReadScriptBytecode(Bytes(valid).first(size)); });
        for (auto [p, value] : {std::pair{0u, 0u}, {4u, 4097u}, {8u, 0xffffffffu}, {12u, 1u},
            {16u, 3u}, {20u, 3u}, {24u, 0xffffffffu}, {28u, 1u}, {32u, 1u}, {36u, 1u},
            {40u, 2049u}, {44u, 1u}, {48u, 1u}, {68u, 1u}, {76u, 1u}, {76u, 10u}})
        { auto bad = valid; Set(bad, p, value); Reject([&] { ReadScriptBytecode(bad); }); }
        for (auto [opcode, operand] : {std::pair{0u, 1u}, {1u, 0u}, {3u, 5u}, {4u, 5u}, {5u, 2047u},
            {6u, 1u}, {7u, 1u}, {9u, 1u}, {13u, 33u}, {13u, 40u}, {14u, 0u}, {15u, 0u}, {22u, 0u}})
        { auto bad = valid; Instruction(bad, 0, opcode, operand); Reject([&] { ReadScriptBytecode(bad); }); }
        { auto bad = valid; bad[102] = 'd'; Reject([&] { ReadScriptBytecode(bad); }); }
        { auto bad = valid; bad.push_back(0); Reject([&] { ReadScriptBytecode(bad); }); }
        { auto bad = valid; bad[81] = 1; Reject([&] { ReadScriptBytecode(bad); }); }
        { auto bad = valid; bad[83] = 4; Reject([&] { ReadScriptBytecode(bad); }); }
        // Backward/self branches are structurally legal; the executor must budget loops.
        { auto loop = valid; Instruction(loop, 0, 7, 0); Check(ReadScriptBytecode(loop)->code[0] == 14336, "Loop was rewritten"); }
        for (int i = 1; i < argc; ++i)
        {
            std::ifstream input(argv[i], std::ios::binary);
            Check(bool(input), "Cannot open owned script");
            Blob file((std::istreambuf_iterator<char>(input)), {});
            auto owned = ReadScriptBytecode(file);
            std::cout << argv[i] << ": " << owned->functions.size() << " functions, "
                << owned->code.size() << " instructions validated\n";
        }
        std::cout << checks << " bytecode reader checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}

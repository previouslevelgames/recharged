#pragma once

#include <cstddef>

namespace mscharged::platform
{
// Native representation of an original binary animation trigger file
// ("NLBT" version 1: 12-byte header, hash-sorted 8-byte anim records,
// 12-byte trigger records up to BytecodeOffset, then VM bytecode). Converts
// the big-endian header words and records of a completed original load in
// place; the thumbprint and bytecode stay serialized. Repeated calls on a
// converted file are no-ops. The original constructor keeps every pointer,
// count and search decision.
void PrepareBinaryTriggerFile(void* data, unsigned long size);

// The original AnimTagScript memcpy of the trigger bytecode into its own
// allocation, publishing the copy as completed serialized bytes for the
// original interpreter's bytecode load.
void CopyBinaryTriggerBytecode(void* output, const void* source, std::size_t bytes);
}

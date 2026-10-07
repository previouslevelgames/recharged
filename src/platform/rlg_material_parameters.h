#pragma once

#include <cstddef>

struct glModelPacket;

namespace mscharged::platform
{
// The original RLG allocation and copy order are unchanged. Only the genuine
// completed input extent and copied Wii domain are carried across that copy.
void CopyRLGMaterialParameterBytes(void* output, const void* input, std::size_t bytes);

// Adapt only source-declared layouts qualified by owned effects/frontend data.
// The original program lookup, Configure/Prepare order and pointer remain intact.
void DecodeRLGMaterialParameters(glModelPacket* packet);
}

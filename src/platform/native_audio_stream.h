#pragma once

namespace mscharged::platform {
// The source callback retains the actual stream-pool slot and completed NL
// allocation throughout conversion. This changes only the fixed 96-byte Wii
// scalar representation; it does not prepare a voice or queue another read.
void PrepareNativeAudioStreamHeader(void* header);
}

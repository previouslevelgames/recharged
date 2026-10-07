#pragma once

namespace mscharged::platform {
// Endian/word transport of a completed original NL audio-script image. Original
// AudioScriptRuntime performs all relocation, selection and execution itself.
void PrepareNativeAudioScriptData(void* data, unsigned int bytes);
}

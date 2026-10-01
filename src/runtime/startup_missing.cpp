// Explicit typed failure points for the opt-in prototype, never success stubs.
// Remove each definition when its real native implementation is connected.
#include "runtime/startup.h"
#include "NL/gl/glPlat.h"
#include "NL/nlFileGC.h"
#include "Game/SAnimDecode.h"

using mscharged::MissingStartupService;

bool glplatPreStartup()
{ MissingStartupService("glplatPreStartup", "Original GX/VI startup is not connected to a native GX renderer."); }
void nlInitFileSystem()
{ MissingStartupService("nlInitFileSystem", "The original NL async file service is not connected to Aurora DVD yet."); }
void SAnimInitGQR()
{ MissingStartupService("SAnimInitGQR", "Native paired-single animation decoders are not connected yet."); }

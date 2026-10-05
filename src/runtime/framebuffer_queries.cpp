#include "NL/gl/glPlat.h"
#include "NL/gl/glStruct.h"

// Logical EFB dimensions; Aurora applies its viewport policy to GXSetScissor.
u32 glplatGetFrameBufferWidth() { return glGetScreenInfo()->ScreenWidth; }
u32 glplatGetFrameBufferHeight() { return glGetScreenInfo()->ScreenHeight; }

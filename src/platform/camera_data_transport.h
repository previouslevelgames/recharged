#pragma once

class nlChunk;

namespace mscharged::platform
{
// Native representation only. The original loader retains allocation, count,
// branch order, data pointers, copying, list publication and ownership.
void PrepareCameraChunkRange(nlChunk* first, nlChunk* end);
}

#include "runtime/frontend_layout_gx.h"
#include "runtime/frontend_text_gx.h"

namespace mscharged
{
void DrawFrontendLayout(const resources::FrontendLayoutFrame& frame, unsigned width, unsigned height)
{
    for (const auto& entry : frame.text)
    {
        const auto& m = entry.transform;
        // The layout has already qualified planar transforms, depth ordering
        // and the original centered, Y-up to top-left viewport conversion.
        const TextDrawTransform transform{{{m[0],m[4],0,m[12]}, {m[1],m[5],0,m[13]}, {0,0,0,0}}};
        DrawFrontendText(entry.layout, transform, width, height, entry.colour);
    }
}
}

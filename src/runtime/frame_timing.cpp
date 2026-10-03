#include "runtime/frame_timing.h"
namespace mscharged
{
struct NativeFrameTimingAccess
{
    static FrameTimingSnapshot Read(const FrameCounter& c)
    {
        const unsigned previous = (c.m_ContinuousFrameHistoryIndex + 199) % 200;
        return {{c.m_LastFrame[0],c.m_LastFrame[1]}, {c.m_CurrFrame[0],c.m_CurrFrame[1]},
            {c.m_ContinuousFrameHistory[0][previous],c.m_ContinuousFrameHistory[1][previous]},
            c.m_Counter,c.m_NextHistoryPos,static_cast<unsigned>(c.m_ContinuousFrameHistoryIndex)};
    }
};
FrameTimingSnapshot ReadFrameTiming(const FrameCounter& c) { return NativeFrameTimingAccess::Read(c); }
}

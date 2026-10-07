#include "platform/desktop_dpd_projection.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace mscharged::platform;
unsigned checks{};
char framebuffer;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool Near(float a, float b) { return std::fabs(a - b) < .002f; }
AuroraVIPresentedGeometry Presented() {
    AuroraVIPresentedGeometry result{};
    result.completion = {&framebuffer, 21, 8, 15, 1, false};
    result.window_incarnation = 3;
    result.window_id = 11;
    result.logical_width = result.surface_width = 800;
    result.logical_height = result.surface_height = 600;
    result.viewport_width = 800;
    result.viewport_height = 600;
    result.scanout.framebuffer = &framebuffer;
    result.scanout.retrace_count = 15;
    result.scanout.field = 1;
    result.scanout.fb_width = result.scanout.pan_width = 640;
    result.scanout.xfb_height = result.scanout.pan_height = 448;
    result.scanout.x_origin = 20; result.scanout.y_origin = 16;
    result.scanout.vi_width = 680; result.scanout.vi_height = 448;
    result.scanout.active_width = 720; result.scanout.active_height = 480;
    return result;
}
void Geometry() {
    DesktopDpdWindow window{11,800,600,800,600};
    auto presented = Presented();
    DesktopDpdProjection projection{};
    Check(ProjectPresentedDesktopDpd(presented,window,&projection),"Actual VI rectangle unavailable");
    // Independent fractions: source signal 20/720 and 16/480, not 20/640.
    Check(Near(projection.left,200.0f/9)&&Near(projection.top,20),"Source VI origin confused with EFB origin");
    Check(Near(projection.width,6800.0f/9)&&Near(projection.height,560),"Source VI extent confused with EFB extent");
    Check(projection.presented_revision==8,"Pointer used desired copy instead of successful Present sequence");

    // 1280x720 surface, 4:3 fit: 960x720 viewport at x=160. SDL event units
    // are 640x360 at 2x density. Independent expected borders are 93 1/3,12.
    presented.logical_width=640; presented.logical_height=360;
    presented.surface_width=1280; presented.surface_height=720;
    presented.viewport_x=160; presented.viewport_width=960; presented.viewport_height=720;
    window={11,640,360,1280,720};
    Check(ProjectPresentedDesktopDpd(presented,window,&projection),"High-density fit viewport unavailable");
    Check(Near(projection.left,280.0f/3)&&Near(projection.top,12),"Pixel density or fit border changed");
    Check(Near(projection.width,1360.0f/3)&&Near(projection.height,336),"High-density content extent changed");

    // Portrait surface with a real 4:3 viewport 600x450 at y=225.
    presented.logical_width=presented.surface_width=600;
    presented.logical_height=presented.surface_height=900;
    presented.viewport_x=0; presented.viewport_y=225;
    presented.viewport_width=600; presented.viewport_height=450;
    window={11,600,900,600,900};
    Check(ProjectPresentedDesktopDpd(presented,window,&projection),"Portrait presentation unavailable");
    Check(Near(projection.left,50.0f/3)&&Near(projection.top,240),"Portrait bars mapped into source content");
    Check(Near(projection.width,1700.0f/3)&&Near(projection.height,420),"Portrait content domain changed");
}
void RejectUnknown() {
    const DesktopDpdWindow window{11,800,600,800,600};
    DesktopDpdProjection projection{};
    auto Reject=[&](auto mutate,const char* message){
        auto presented=Presented(); auto current=window; mutate(presented,current);
        projection={99,1,2,3,4};
        Check(!ProjectPresentedDesktopDpd(presented,current,&projection),message);
        Check(projection.presented_revision==0&&!projection.left&&!projection.top&&!projection.width&&!projection.height,
              "Unavailable calibration retained stale geometry");
    };
    Reject([](auto& g,auto&){g={};},"No successful Present became valid");
    Reject([](auto& g,auto&){g.completion.black=true;},"Black completion became visible");
    Reject([](auto& g,auto&){g.scanout.black=true;},"Black signal became visible");
    Reject([](auto& g,auto&){g.window_incarnation=0;},"Unknown window incarnation accepted");
    Reject([](auto&,auto& w){++w.id;},"Another SDL window reused presented calibration");
    Reject([](auto&,auto& w){++w.logical_width;},"Pending logical resize reused old calibration");
    Reject([](auto&,auto& w){++w.pixel_height;},"Pending pixel-density resize reused old calibration");
    Reject([](auto& g,auto&){g.completion.framebuffer=nullptr;},"No real source framebuffer accepted");
    Reject([](auto& g,auto&){g.scanout.framebuffer=nullptr;},"Different work-item framebuffer accepted");
    Reject([](auto& g,auto&){++g.scanout.field;},"Different work-item field accepted");
    Reject([](auto& g,auto&){++g.scanout.retrace_count;},"Different work-item retrace accepted");
    Reject([](auto& g,auto&){g.completion.presentation_sequence=0;},"Pending-only copy accepted");
    Reject([](auto& g,auto&){g.viewport_x=std::numeric_limits<float>::quiet_NaN();},"NaN viewport accepted");
    Reject([](auto& g,auto&){g.viewport_width=0;},"Empty output viewport accepted");
    Reject([](auto& g,auto&){g.viewport_width=801;},"Out-of-surface viewport accepted");
    Reject([](auto& g,auto&){g.scanout.active_width=0;},"Empty active signal accepted");
    Reject([](auto& g,auto&){g.scanout.vi_width=701;},"VI extent outside its active signal accepted");
    Reject([](auto& g,auto&){g.scanout.pan_x=1;},"Unknown cropped source pan accepted");
    Reject([](auto& g,auto&){g.scanout.pan_width=320;},"Cropped source pan silently mapped to full scene");
    Check(!ProjectPresentedDesktopDpd(Presented(),window,nullptr),"Null calibration destination accepted");
}
}
int main() {
    try { Geometry(); RejectUnknown(); std::printf("Successful-Present desktop projection: %u checks; generated metadata, actual GPU publication remains a separate gate.\n",checks); return 0; }
    catch(const std::exception& error) {std::fprintf(stderr,"DPD projection: %s\n",error.what());return 1;}
}

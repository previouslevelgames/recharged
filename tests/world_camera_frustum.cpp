#include "world_fixture.h"
#include "Game/Render/Frustum.h"
#include "NL/gl/glMatrix.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message)
{ ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F f)
{ ++checks; try { f(); } catch (const std::invalid_argument&) { return; } throw std::runtime_error("Invalid camera accepted"); }
struct Pose { nlVector3 eye, right, up, back; bool general = false; };
nlVector3 World(const Pose& p, double x, double y, double z)
{
    return {float(p.eye.x+x*p.right.x+y*p.up.x+z*p.back.x),
            float(p.eye.y+x*p.right.y+y*p.up.y+z*p.back.y),
            float(p.eye.z+x*p.right.z+y*p.up.z+z*p.back.z)};
}
void Camera(const Pose& pose, bool perspective)
{
    nlMatrix4 view, projection;
    glMatrixLookAt(view, pose.eye, World(pose,0,0,-1), pose.up);
    if (perspective) glMatrixPerspective(projection, 3.1415927f/2, 1, 1, 9);
    else glMatrixOrthographicCentered(projection, 4, 6, 1, 9);
    auto frustum = StaticWorldFrustum::FromCamera(view, projection);
    if (perspective)
    {
        std::array<nlVector4,6> original;
        ExtractFrustumPlanes(original.data(), projection, view);
        const unsigned order[]{1,0,2,3,4,5};
        for (unsigned i=0;i<6;++i)
        {
            auto a=frustum.Planes()[i], b=original[order[i]];
            Check(std::abs(a.x-b.x)<1e-5 && std::abs(a.y-b.y)<1e-5
                && std::abs(a.z-b.z)<1e-5 && std::abs(a.w-b.w)<1e-5,
                "Perspective extraction diverged from original Charged planes");
        }
    }
    // Independent analytic distances in camera coordinates. Test translation,
    // orthogonal camera rotations, every face, behind-eye and beyond-far points.
    for (double depth : {-2.,.5,1.,2.,4.,8.,9.,10.,20.})
        for (double x : {-20.,-8.,-3.,-2.,-.25,0.,.25,2.,3.,8.,20.})
            for (double y : {-20.,-8.,-3.,-2.,-.25,0.,.25,2.,3.,8.,20.})
                for (double radius : {0.,.0625,.5})
                {
                    const double sx=perspective?depth:2, sy=perspective?depth:3;
                    const double divisor=perspective?std::sqrt(2.):1.;
                    const double d=std::min({(sx+x)/divisor,(sx-x)/divisor,
                        (sy+y)/divisor,(sy-y)/divisor,depth-1,9-depth});
                    // Rounding of SDK trigonometry can affect exact boundaries;
                    // exact representable tangency is checked separately below.
                    if (std::abs(d+radius)<1e-5 || std::abs(d)<1e-5) continue;
                    const auto p=World(pose,x,y,-depth);
                    auto object=world_fixture::Object(1,1,p.x,p.y,p.z);object.radius=float(radius);
                    Check(frustum.Visible(object)==(d>=-radius),"Camera frustum disagrees with analytic sphere distance");
                    object.type=0x10002;object.bounds_min={p.x,p.y,p.z};object.bounds_max=object.bounds_min;
                    Check(frustum.Visible(object)==(d>=0),"Camera frustum disagrees with analytic point-box distance");
                }
    // Exact tangency uses binary-representable projection coefficients. Height6
    // above deliberately tests rounded SDK1/3 away from exact boundaries: when
    // translated, its combined plane can differ from ideal geometry by an ULP.
    if (!perspective)
    {
        glMatrixOrthographicCentered(projection,4,4,1,9);
        frustum=StaticWorldFrustum::FromCamera(view,projection);
    }
    for (unsigned face=0;face<6;++face)
    {
        // A point exactly on each perspective/orthographic face is visible.
        // Tangency uses axis rotations and binary-representable coordinates.
        const double points[6][3]={{perspective?-4.:-2.,0,-4},{perspective?4.:2.,0,-4},
            {0,perspective?-4.:-2.,-4},{0,perspective?4.:2.,-4},{0,0,-1},{0,0,-9}};
        const auto p=World(pose,points[face][0],points[face][1],points[face][2]);
        auto object=world_fixture::Object(1,1,p.x,p.y,p.z);
        // A general float basis accumulates rounding when transformed back.
        // Cover that <1e-4 world-unit uncertainty; exact axis tangency uses zero.
        object.radius=pose.general?1e-4f:0;
        if (!frustum.Visible(object))
        {
            const auto plane=frustum.Planes()[face];
            std::cerr<<"Tangent failure: perspective="<<perspective<<", face="<<face
                <<", eye="<<pose.eye.x<<','<<pose.eye.y<<','<<pose.eye.z
                <<", back="<<pose.back.x<<','<<pose.back.y<<','<<pose.back.z
                <<", point="<<p.x<<','<<p.y<<','<<p.z<<", plane="<<plane.x<<','<<plane.y<<','<<plane.z<<','<<plane.w
                <<", distance="<<plane.x*p.x+plane.y*p.y+plane.z*p.z+plane.w<<'\n';
        }
        Check(frustum.Visible(object),"Tangent camera point was culled");
        const auto normal=frustum.Planes()[face];
        object.radius=0;
        object.transform[12]-=normal.x*.01f;object.transform[13]-=normal.y*.01f;object.transform[14]-=normal.z*.01f;
        Check(!frustum.Visible(object),"Point beyond camera face was accepted");
        object.radius=.02f;
        Check(frustum.Visible(object),"Sphere intersecting camera face was culled");
    }
}
}
int main()
{
    try
    {
        for (const auto& pose : {Pose{{0,0,0},{1,0,0},{0,1,0},{0,0,1}},
             Pose{{2,3,5},{1,0,0},{0,1,0},{0,0,1}},
             Pose{{2,3,5},{0,0,-1},{0,1,0},{1,0,0}},
             Pose{{2,3,5},{1,0,0},{0,0,1},{0,-1,0}},
             Pose{{2,3,5},{.8f,0,-.6f},{-3.f/13,12.f/13,-4.f/13},{36.f/65,5.f/13,48.f/65},true}})
            for (bool perspective : {false,true}) Camera(pose,perspective);
        nlMatrix4 view,projection;view.SetIdentity();projection.SetIdentity();
        for (float bad : {std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
            for (unsigned i=0;i<16;++i)
            {
                auto changed=view;changed.e[i]=bad;
                Reject([&]{StaticWorldFrustum::FromCamera(changed,projection);});
                Reject([&]{StaticWorldFrustum::FromCamera(view,changed);});
            }
        for (auto& value:projection.e) value=0;Reject([&]{StaticWorldFrustum::FromCamera(view,projection);});
        std::cout<<checks<<" camera frustum convention, analytic distance, tangency and malformed-matrix checks passed\n";
        return 0;
    }
    catch (const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}

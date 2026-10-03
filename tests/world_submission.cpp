#include "world_fixture.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/MemAlloc.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace mscharged;
using namespace world_fixture;
namespace
{
unsigned checks=0, invalidations=0;
void Invalidate() { ++invalidations; }
void Check(bool value, const char* message) { ++checks; if(!value) throw std::runtime_error(message); }
template<class E, class F> void Reject(F f)
{ ++checks; try {f();} catch(const E&) {return;} throw std::runtime_error("Invalid world submission accepted"); }
std::vector<const glModelPacket*> seen;
void Observe(GLView*,unsigned long,const glModelPacket* p) {if(p)seen.push_back(p);}
std::vector<const glModelPacket*> Packets(GLView& v) {seen.clear();v.Iterate(Observe);return seen;}
void Culling()
{
    const auto planes=Planes(); StaticWorldFrustum frustum(planes);
    for(unsigned type:{0x101u,0x10002u})
    {
        auto o=Object(1,1); o.type=type; o.radius=.25f;
        Check(frustum.Visible(o),"Inside drawable culled");
        // Independently construct centre and extents against all six faces.
        for(unsigned face=0;face<6;++face)
            for(float separation:{-.125f,0.f,.125f})
            {
                const unsigned axis=face/2;
                const float sign=(face&1)?1.f:-1.f;
                const float boundary=axis==2 && sign>0 ? 0.f : sign;
                auto at=o;
                at.transform[12+axis]=boundary+sign*(.25f+separation);
                for(unsigned a=0;a<3;++a)
                {at.bounds_min[a]=at.transform[12+a]-.25f; at.bounds_max[a]=at.transform[12+a]+.25f;}
                Check(frustum.Visible(at)==(separation<=0),"Frustum face tangent/intersection rule changed");
            }
    }
    auto sphere=Object(1,1,1.4f); sphere.radius=.3f; sphere.transform[0]=100;
    Check(!frustum.Visible(sphere),"Common sphere radius was rescaled by the matrix");
    auto box=Object(1,1); box.type=0x10002; box.transform[12]=1000;
    Check(frustum.Visible(box),"Authored stadium box transformed a second time");
    box.bounds_min[0]=2;box.bounds_max[0]=3;box.transform[12]=0;
    Check(!frustum.Visible(box),"Stadium culling used the model origin instead of its box");
    for(float bad:{0.f,2.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
    {auto p=planes;p[0].x=bad;Reject<std::invalid_argument>([&]{StaticWorldFrustum f(p);});}
    auto p=planes;p[5].w=std::numeric_limits<float>::infinity();
    Reject<std::invalid_argument>([&]{StaticWorldFrustum f(p);});
    auto unknown=Object(1,1);unknown.type=0x109;
    Reject<std::runtime_error>([&]{frustum.Visible(unknown);});
    auto invalid=Object(1,1);invalid.radius=std::numeric_limits<float>::quiet_NaN();
    Reject<std::runtime_error>([&]{frustum.Visible(invalid);});
    invalid=Object(1,1);invalid.bounds_min[0]=10;
    Reject<std::runtime_error>([&]{frustum.Visible(invalid);});
}
void Session()
{
    const auto mem1=StandardAllocator.TotalFreeMemory(),mem2=VirtualAllocator.TotalFreeMemory();
    const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,4096}};
    const GLMemoryConfig config{65536,4096,req,2,32};
    glInitMemory(&config);InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Invalidate);
    {
        MaterialPrograms programs;OriginalViews views(640,480);ViewMatrices matrices;
        GLView opaque(&matrices,{},GLViewSort_Texture),alpha(&matrices,{},GLViewSort_Texture);
        auto* prior=glGetCurrentResourcePool();
        auto model=Quad(1,10);model.packets.push_back(Quad(1,11,1).packets[0]);
        std::vector objects{Object(1,1,-.5f),Object(2,1,.5f),Object(3,1,3)};
        resources::TextureBundle textures{{Texture(10,{255,0,0,255}),Texture(11,{0,255,0,128})},{}};
        {
            StaticWorldObjects world(objects,{model},textures,{65536,8192});
            StaticWorldFrustum frustum(Planes());
            Reject<std::logic_error>([&]{SubmitStaticWorld(world,opaque,alpha,frustum);});
            Check(Packets(opaque).empty()&&Packets(alpha).empty(),"Invalid pool submission changed queues");
            glSetCurrentResourcePool(&world.Pool());
            glNativeSetViewDispatch(true);
            Reject<std::logic_error>([&]{SubmitStaticWorld(world,opaque,alpha,frustum);});
            glNativeSetViewDispatch(false);
            // Original sort layer 1 must follow unrelated layer 0 packets in the alpha view.
            auto sentinel=world.Find(3)->model->packets[0];
            for(unsigned frame=0;frame<4;++frame)
            {
                glplatFrameAllocNextFrame();
                alpha.AttachPacket(&sentinel,0);
                const auto r=SubmitStaticWorld(world,opaque,alpha,frustum);
                Check(r.objects==3&&r.visible==2&&r.opaque_packets==2&&r.alpha_packets==2,"World submission counts");
                auto op=Packets(opaque),ap=Packets(alpha);
                Check(op==std::vector<const glModelPacket*>({world.Find(1)->model->packets,world.Find(2)->model->packets}),"Opaque routing/order");
                Check(ap==std::vector<const glModelPacket*>({&sentinel,world.Find(1)->model->packets+1,world.Find(2)->model->packets+1}),"Alpha routing/layer/order");
                for(const auto& o:world.Objects())
                {nlMatrix4 m;glModelGetMatrix(o.model,m);Check(!std::memcmp(m.e,o.record.transform.data(),64),"Submission changed authored matrix");}
                opaque.ResetPackets();alpha.ResetPackets();
                Check(glGetCurrentResourcePool()==&world.Pool(),"Submission changed current pool");
            }
            // Every nonzero original blend mode belongs to alpha, not just mode 1.
            for(unsigned mode=1;mode<8;++mode)
            {
                for(const auto& o:world.Objects()) glSetRasterState(o.model->packets[0].rasterState,GLS_AlphaBlend,mode);
                auto r=SubmitStaticWorld(world,opaque,alpha,frustum);
                Check(r.opaque_packets==0&&r.alpha_packets==4,"Nonzero blend mode routed opaque");
                opaque.ResetPackets();alpha.ResetPackets();
            }
            glSetCurrentResourcePool(prior);world.Release();
            Reject<std::logic_error>([&]{SubmitStaticWorld(world,opaque,alpha,frustum);});
        }
        Check(glGetCurrentResourcePool()==prior,"World teardown changed selected pool");
    }
    glShutdownMemory();
    Check(StandardAllocator.TotalFreeMemory()==mem1&&VirtualAllocator.TotalFreeMemory()==mem2,"World submission leaked arenas");
}
}
int main()
{
    try
    {
        Culling();
        std::vector<std::uint64_t> mem1(2*1024*1024),mem2(2*1024*1024);
        ResetStartupMemory();StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        Session();Session();Check(invalidations==8,"Frame cache invalidation count");SetGraphicsCacheInvalidator(nullptr);ResetStartupMemory();
        std::cout<<checks<<" world culling, routing, matrix and lifecycle checks passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}

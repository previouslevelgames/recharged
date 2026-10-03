#include "runtime/animated_camera.h"
#include "runtime/startup.h"
#include "runtime/graphics_memory.h"
#include "Game/Camera/CameraMan.h"
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cstring>
#include <thread>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks=0, callbacks=0;
AnimatedCamera* callback_camera=nullptr;
void Check(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
void Near(float a,float b,float tolerance=.0002f)
{
    ++checks;if(!std::isfinite(a)||std::abs(a-b)>tolerance)
        throw std::runtime_error("Animated value "+std::to_string(a)+" != "+std::to_string(b));
}
template<class F> void Reject(F fn)
{ try{fn();}catch(const std::exception&){++checks;return;}throw std::runtime_error("Invalid playback accepted"); }
void End() { ++callbacks; if(callback_camera)Reject([]{callback_camera->Seek(0);}); }
using Blob=std::vector<std::uint8_t>;
void Word(Blob& b,std::uint32_t v){for(int s:{24,16,8,0})b.push_back(v>>s);}
using Targets=std::array<std::array<float,3>,3>;
Blob Fixture(float distance=2,Targets targets={})
{
    std::map<unsigned,Blob> c;
    c[0x25000]={'t','e','s','t',0,0,0,0};Word(c[0x2500c],3);
    const float root=std::sqrt(.5f);
    for(unsigned i=0;i<3;++i)
    {
        for(float f:{1+distance*i,2.f,3.f})Word(c[0x25003],std::bit_cast<unsigned>(f));
        for(float f:targets[i])Word(c[0x25006],std::bit_cast<unsigned>(f));
        for(float f:{0.f,0.f,i==0?0.f:i==1?root:1.f,i==0?1.f:i==1?root:0.f})Word(c[0x25004],std::bit_cast<unsigned>(f));
        Word(c[0x25009],std::bit_cast<unsigned>(40.f+10*i));
        Word(c[0x2500a],std::bit_cast<unsigned>(2.f+i));
    }
    Blob b;Word(b,0x8002500b);Word(b,0);
    for(const auto& [id,payload]:c){Word(b,id);Word(b,payload.size());b.insert(b.end(),payload.begin(),payload.end());}
    unsigned size=b.size()-8;for(unsigned i=0;i<4;++i)b[4+i]=size>>(24-8*i);
    return b;
}
void ViewOrigin(cBaseCamera& camera)
{
    const auto& p=camera.GetCameraPosition();const auto& m=camera.GetViewMatrix();
    Near(p.x*m.m11+p.y*m.m21+p.z*m.m31+m.m41,0,.002f);
    Near(p.x*m.m12+p.y*m.m22+p.z*m.m32+m.m42,0,.002f);
    Near(p.x*m.m13+p.y*m.m23+p.z*m.m33+m.m43,0,.002f);
}
void Interpolation()
{
    OriginalCameras core; auto asset=CameraAsset::Decode(Fixture(),"test");
    CameraAssetLibrary library;library.Insert(asset);
    AnimatedCamera camera(library.Find("TEST"));asset.reset();library.Clear();
    Near(camera.Duration(),.1f);Near(camera.TimeLeft(),.1f);
    Near(camera.Camera().GetCameraPosition().x,1);ViewOrigin(camera.Camera());
    camera.Seek(.25f);
    Near(camera.Camera().GetCameraPosition().x,2);Near(camera.Camera().GetFOV(),45);
    Near(camera.Camera().GetViewMatrix().m11,std::sqrt(.5f));
    Near(camera.Camera().GetViewMatrix().m12,-std::sqrt(.5f));
    Near(camera.FocalDistance(),4.5f);Near(camera.TimeLeft(),.075f);ViewOrigin(camera.Camera());
    camera.Seek(1);Near(camera.Camera().GetCameraPosition().x,5);Near(camera.Camera().GetFOV(),60);
    ViewOrigin(camera.Camera());
    camera.Seek(2);Near(camera.Camera().GetCameraPosition().x,5); // Original endpoint behavior.
    camera.Seek(0);
    cCameraManager::PushCamera(&camera.Camera());core.Advance(.025f,.025f);
    Near(camera.Time(),.25f);Near(cCameraManager::m_cameraPosition.x,2);Near(cCameraManager::m_fFOV,45);
    bool rejected=false;std::thread wrong([&]{try{camera.Time();}catch(const std::logic_error&){rejected=true;}});wrong.join();
    Check(rejected,"Wrong-thread playback accepted");
}
void Cuts()
{
    OriginalCameras core;
    AnimatedCamera smooth(CameraAsset::Decode(Fixture(4),"smooth"));smooth.Seek(.125f);
    Near(smooth.Camera().GetCameraPosition().x,2);
    AnimatedCamera cut(CameraAsset::Decode(Fixture(4.01f),"cut"));
    cut.Seek(.249f);Near(cut.Camera().GetCameraPosition().x,1);Near(cut.Camera().GetFOV(),40);
    cut.Seek(.25f);Near(cut.Camera().GetCameraPosition().x,5.01f);Near(cut.Camera().GetFOV(),50);
}
void Transforms()
{
    OriginalCameras core;AnimatedCamera camera(CameraAsset::Decode(Fixture(),"test"));
    AnimatedCameraOptions options;options.offset={5,6,7};options.mirror={-1,1,1};options.facing=16384;
    camera.Configure(options);const auto& p=camera.Camera().GetCameraPosition();
    Near(p.x,3);Near(p.y,5);Near(p.z,10);ViewOrigin(camera.Camera());
    // Original facing rotates only camera position, not the target position.
    Near(camera.Camera().GetTargetPosition().x,5);Near(camera.Camera().GetTargetPosition().y,6);
    options.look_at=true;camera.Configure(options);ViewOrigin(camera.Camera());
    camera.SetInputs({true,false,0});camera.Seek(0);Near(camera.Camera().GetFOV(),40);
    camera.SetInputs({true,true,0});camera.Seek(0);
    const float expected=2*std::atan(std::tan(20.f*3.1415927f/180)*1.25f/1.666f)*180/3.1415927f;
    Near(camera.Camera().GetFOV(),expected,.03f); // Original fixed-angle trig quantization.
    Near(camera.FocalDistance(),2+2*std::pow(45/camera.Camera().GetFOV(),2));
}
void Timing()
{
    OriginalCameras core;AnimatedCamera camera(CameraAsset::Decode(Fixture(),"test"));
    AnimatedCameraOptions options;options.on_end=End;camera.Configure(options);callbacks=0;callback_camera=&camera;
    Near(camera.Advance(.1f),0);Near(camera.Time(),0);Check(callbacks==1,"Loop endpoint callback differs");
    Near(camera.Advance(.225f),.125f);Near(camera.Time(),1.25f); // Subtract once, not modulo.
    Near(camera.Camera().GetCameraPosition().x,5);Check(callbacks==2,"Large step repeated callback");
    options.cyclic=false;camera.Configure(options);camera.Seek(0);
    Near(camera.Advance(.15f),.05f);Near(camera.Time(),1);Check(callbacks==3,"Clamp callback absent");
    Near(camera.Advance(0),0);Check(callbacks==4,"Original repeated endpoint callback changed");
    callback_camera=nullptr;
    options.simulation_time=true;options.speed=2;camera.Configure(options);camera.Seek(0);
    camera.SetInputs({false,false,10});camera.Advance(100);Near(camera.Time(),0);
    camera.SetInputs({false,false,10.025f});camera.Advance(0);Near(camera.Time(),.5f,.00002f);
    camera.Advance(1);Near(camera.Time(),.5f,.00002f); // Paused simulation.
}
void Invalid()
{
    for(float time:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),1e30f})
    {OriginalCameras core;AnimatedCamera c(CameraAsset::Decode(Fixture(),"t"));Reject([&]{c.Seek(time);});}
    {OriginalCameras core;AnimatedCamera c(CameraAsset::Decode(Fixture(),"t"));Reject([&]{c.Advance(1e30f);});}
    {OriginalCameras core;Reject([]{AnimatedCamera c(nullptr);});cAnimCamera raw;Reject([&]{raw.ManualUpdate(.1f);});}
    {OriginalCameras core;AnimatedCamera c(CameraAsset::Decode(Fixture(),"t"));AnimatedCameraOptions o;o.mirror.x=0;Reject([&]{c.Configure(o);});}
    {OriginalCameras core;AnimatedCamera c(CameraAsset::Decode(Fixture(),"t"));AnimatedCameraOptions o;o.speed=-1;Reject([&]{c.Configure(o);});}
    {OriginalCameras core;AnimatedCamera c(CameraAsset::Decode(Fixture(),"t"));AnimatedCameraOptions o;o.simulation_time=true;c.Configure(o);c.SetInputs({false,false,1});c.Advance(0);Reject([&]{c.SetInputs({false,false,.5f});});}
    Reject([]{CheckAnimatedLookAt({0,0,1},{0,0,0});});
}
void Bits(float actual,float expected)
{
    ++checks;
    if(std::bit_cast<unsigned>(actual)!=std::bit_cast<unsigned>(expected))
        throw std::runtime_error("Published target bits "+std::to_string(std::bit_cast<unsigned>(actual))+
                                 " != "+std::to_string(std::bit_cast<unsigned>(expected)));
}
void ExtremeTargets()
{
    OriginalCameras core;
    const float large=std::ldexp(1.f,100),small=std::ldexp(1.f,66);
    const Targets targets{{{large,-large,4},{-large,large,8},{small,-small,16}}};
    AnimatedCamera camera(CameraAsset::Decode(Fixture(2,targets),"extreme"));
    AnimatedCamera normal(CameraAsset::Decode(Fixture(),"normal"));
    for(float time:{0.f,.125f,.25f,.5f,.75f,1.f,2.f})
    {
        camera.Seek(time);normal.Seek(time);
        const auto& a=camera.Camera();const auto& b=normal.Camera();
        Check(std::memcmp(&a.GetViewMatrix(),&b.GetViewMatrix(),sizeof(nlMatrix4))==0,"Target changed quaternion view");
        const auto& target=a.GetTargetPosition();
        // Power-of-two authored values make these weighted products exact.
        // Double represents both
        // products and their sum here; expected values are independently rounded.
        unsigned key=time>=1?2:unsigned(time*2);const double weight=time>=1?0:double(time)*2-key;
        float expected[3];
        for(unsigned axis=0;axis<3;++axis)
            expected[axis]=float((1-weight)*targets[key][axis]+weight*targets[std::min(key+1,2u)][axis]);
        Bits(target.x,expected[0]);Bits(target.y,expected[1]);Bits(target.z,expected[2]);
    }
    AnimatedCameraOptions options;options.mirror={-1,-1,1};options.offset={5,6,7};options.facing=16384;
    camera.Seek(.25f);camera.Configure(options);
    const auto& v=camera.Camera().GetTargetPosition();Bits(v.x,5);Bits(v.y,6);Bits(v.z,13);
    camera.Seek(0);Bits(v.x,-large);Bits(v.y,large);Bits(v.z,11);
    AnimatedCamera cut(CameraAsset::Decode(Fixture(5,targets),"cut"));
    cut.Seek(.249f);Bits(cut.Camera().GetTargetPosition().x,targets[0][0]);
    cut.Seek(.25f);Bits(cut.Camera().GetTargetPosition().x,targets[1][0]);
    cut.Seek(1);Bits(cut.Camera().GetTargetPosition().x,targets[2][0]);
    const float max=std::numeric_limits<float>::max();
    AnimatedCamera endpoint(CameraAsset::Decode(Fixture(5,{{{max,-max,1},{-max,max,2},{max,-max,3}}}),"max"));
    endpoint.Seek(1);Bits(endpoint.Camera().GetTargetPosition().x,max);Bits(endpoint.Camera().GetTargetPosition().y,-max);
}
void TargetFailures()
{
    // A finite direction can have an unrepresentable squared length. Safe
    // large directions remain accepted by the original Z-up look-at math.
    {OriginalCameras core;AnimatedCamera camera(CameraAsset::Decode(Fixture(2,{{{1e10f,2e10f,3e10f},{1e10f,2e10f,3e10f},{1e10f,2e10f,3e10f}}}),"large"));
     AnimatedCameraOptions o;o.look_at=true;camera.Configure(o);ViewOrigin(camera.Camera());}
    for(const nlVector3 target: {nlVector3{1e30f,0,0},nlVector3{0,0,1e30f},nlVector3{.000002f,0,1e19f},nlVector3{.2f,0,1e19f}})
    {
        OriginalCameras core;AnimatedCamera camera(CameraAsset::Decode(Fixture(),"failure"));
        cCameraManager::PushCamera(&camera.Camera());core.Advance(0,0);
        const auto published=cCameraManager::m_matView;
        // Direct guard checks cover arithmetic before the original normalizer.
        Reject([&]{CheckAnimatedLookAt({0,0,0},target);});
        AnimatedCamera large(CameraAsset::Decode(Fixture(2,{{{target.x,target.y,target.z},{target.x,target.y,target.z},{target.x,target.y,target.z}}}),"large"));
        if(target.z==1e19f)continue; // This fixture's camera position removes vertical degeneracy.
        AnimatedCameraOptions o;o.look_at=true;Reject([&]{large.Configure(o);});
        Check(std::memcmp(&published,&cCameraManager::m_matView,sizeof(published))==0,"Failed look-at published a manager pose");
        Reject([&]{camera.Seek(0);});Reject([&]{core.Advance(0,0);});
    }
    {
        OriginalCameras core;
        const float huge=std::numeric_limits<float>::max()*.75f;
        AnimatedCamera camera(CameraAsset::Decode(Fixture(5,{{{0,0,0},{0,0,0},{huge,0,0}}}),"overflow"));
        AnimatedCameraOptions o;o.offset={huge,0,0};camera.Configure(o);
        cCameraManager::PushCamera(&camera.Camera());core.Advance(0,0);const auto published=cCameraManager::m_matView;
        // One original cyclic subtraction consumes key zero, not the endpoint
        // whose target would overflow after this offset.
        camera.Advance(camera.Duration());Near(camera.Time(),0);
        Reject([&]{camera.Seek(1);});Near(camera.Time(),0);
        Check(std::memcmp(&published,&cCameraManager::m_matView,sizeof(published))==0,"Failed target sample published a manager pose");
        Reject([&]{camera.Seek(0);});Reject([&]{core.Advance(0,0);});
    }
}
void Owned(const std::filesystem::path& path)
{
    std::ifstream in(path,std::ios::binary);Blob b{std::istreambuf_iterator<char>(in),{}};
    auto asset=CameraAsset::Decode(b,"owned");OriginalCameras core;AnimatedCamera camera(asset);
    AnimatedCameraOptions options;options.cyclic=false;camera.Configure(options);
    cCameraManager::PushCamera(&camera.Camera());
    float unsafe_look_at_time=-1;unsigned extreme_samples=0;
    for(unsigned i=0;i<=1000;++i)
    {
        camera.Seek(i/1000.f);core.Advance(0,0);ViewOrigin(camera.Camera());
        const auto& d=asset->Data();
        // Independent double-precision scalar oracle against decoded key data.
        const double frame=double(camera.Time())*(d.m_uKeyCount-1);
        const auto a=static_cast<unsigned>(frame), b=std::min(a+1,unsigned(d.m_uKeyCount-1));
        double weight=frame-a;
        const auto& pa=d.cameraPos[a];const auto& pb=d.cameraPos[b];
        const double dx=double(pb.x)-pa.x,dy=double(pb.y)-pa.y,dz=double(pb.z)-pa.z;
        if(dx*dx+dy*dy+dz*dz>16)weight=weight<.5?0:1;
        auto blend=[&](float x,float y){return float(x+(double(y)-x)*weight);};
        const auto& actual=camera.Camera().GetCameraPosition();
        Near(actual.x,blend(pa.x,pb.x),.002f);Near(actual.y,blend(pa.y,pb.y),.002f);Near(actual.z,blend(pa.z,pb.z),.002f);
        const float fov=blend(d.fFOV[a],d.fFOV[b]);Near(camera.Camera().GetFOV(),fov,.002f);
        Near(camera.FocalDistance(),blend(d.fFocalLength[a],d.fFocalLength[b])+2*std::pow(45/fov,2),.01f);
        // Independently model binary32 rounding using double operations. This
        // also catches loss of the very large target values to zero or clamping.
        const float rounded_frame=float(double(camera.Time())*(d.m_uKeyCount-1));
        unsigned ta=camera.Time()>=1?d.m_uKeyCount-1:unsigned(rounded_frame),tb=std::min(ta+1,unsigned(d.m_uKeyCount-1));
        const float wb=float(double(rounded_frame)-ta),wa=float(1.-wb);
        const auto& ca=d.cameraPos[ta];const auto& cb=d.cameraPos[tb];
        const float cx=float(double(ca.x)-cb.x),cy=float(double(ca.y)-cb.y),cz=float(double(ca.z)-cb.z);
        const float length=float(double(float(double(float(double(cx)*cx))+float(double(cy)*cy)))+float(double(cz)*cz));
        if(length>16)ta=tb=wb<.5f?ta:tb;
        const auto& target=camera.Camera().GetTargetPosition();
        const float av[]{d.targetPos[ta].x,d.targetPos[ta].y,d.targetPos[ta].z};
        const float bv[]{d.targetPos[tb].x,d.targetPos[tb].y,d.targetPos[tb].z};
        const float actual_target[]{target.x,target.y,target.z};
        for(unsigned axis=0;axis<3;++axis)
        {
            const float value=ta==tb?av[axis]:float(double(float(double(wa)*av[axis]))+float(double(wb)*bv[axis]));
            Bits(actual_target[axis],float(double(value)+0.));
            if(std::abs(actual_target[axis])>1e7f)++extreme_samples;
        }
        const double tx=double(actual.x)-target.x,ty=double(actual.y)-target.y,tz=double(actual.z)-target.z;
        if(tx*tx+ty*ty+tz*tz>std::numeric_limits<float>::max())unsafe_look_at_time=camera.Time();
    }
    if(unsafe_look_at_time>=0)
    {
        camera.Seek(unsafe_look_at_time);core.Advance(0,0);const auto published=cCameraManager::m_matView;
        options.look_at=true;Reject([&]{camera.Configure(options);});
        Check(std::memcmp(&published,&cCameraManager::m_matView,sizeof(published))==0,"Unsafe authored look-at published a pose");
        Reject([&]{camera.Seek(0);});
    }
    std::cout<<path<<": "<<asset->Data().m_uKeyCount<<" keys, 1001 authored samples, "<<extreme_samples
             <<" extreme target components, unsafe look-at "<<(unsafe_look_at_time>=0?"rejected":"not encountered")<<'\n';
}
}
int main(int argc,char** argv)
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        if(argc==2)Owned(argv[1]);else for(int i=0;i<3;++i){Interpolation();Cuts();Transforms();Timing();Invalid();ExtremeTargets();TargetFailures();}
        Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"Playback did not recover both arenas");
        ResetStartupMemory();std::cout<<checks<<" animated camera checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

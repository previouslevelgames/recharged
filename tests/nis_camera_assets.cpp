#include "runtime/nis_camera_assets.h"
#include "runtime/animated_camera.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "resources/chunk_reader.h"
#include "Game/Camera/CameraMan.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <thread>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool good,const char* message){++checks;if(!good)throw std::runtime_error(message);}
template<class F> void Reject(F action)
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid NIS camera operation accepted");}
void Near(float a,float b,float tolerance=.002f)
{Check(std::isfinite(a)&&std::abs(a-b)<=tolerance,"Original NIS camera sample differs");}
void Bits(float actual,float expected)
{Check(std::bit_cast<unsigned>(actual)==std::bit_cast<unsigned>(expected),"NIS camera channel bits changed");}
using Blob=std::vector<std::uint8_t>;
void Word(Blob& data,std::uint32_t value){for(int shift:{24,16,8,0})data.push_back(value>>shift);}
void Set(Blob& data,std::size_t offset,std::uint32_t value)
{for(int shift:{24,16,8,0})data.at(offset++)=value>>shift;}
void Chunk(Blob& data,std::uint32_t id,const Blob& payload,unsigned exponent=0)
{
    const auto at=data.size();Word(data,id|(exponent<<24));Word(data,0);
    data.resize(Align(data.size(),std::size_t(1)<<exponent));
    data.insert(data.end(),payload.begin(),payload.end());Set(data,at+4,data.size()-at-8);
    data.resize(Align(data.size(),4));
}
void CameraChunk(Blob& data,unsigned slot)
{
    const auto begin=data.size();Word(data,0x8002500b);Word(data,0);
    std::map<unsigned,Blob> channels;
    const auto name="synthetic_nis_camera_"+std::to_string(slot);
    auto& n=channels[0x25000];n.assign(name.begin(),name.end());n.push_back(0);n.resize(Align(n.size(),4));
    Word(channels[0x2500c],3);
    for(unsigned i=0;i<3;++i)
    {
        for(float v:{float(slot*20+i*2),-2.f,4.f})Word(channels[0x25003],std::bit_cast<unsigned>(v));
        for(unsigned bits:{0xdeadbeefu,0x80000000u,1u})Word(channels[0x25006],bits);
        for(float v:{0.f,0.f,0.f,1.f})Word(channels[0x25004],std::bit_cast<unsigned>(v));
        Word(channels[0x25009],std::bit_cast<unsigned>(40.f+i));
        Word(channels[0x2500a],std::bit_cast<unsigned>(2.f+i));
    }
    for(const auto& [id,payload]:channels)Chunk(data,id,payload,3+(id%3)); // Absolute 8/16/32-byte alignment.
    Set(data,begin+4,data.size()-begin-8);
}
Blob Fixture(unsigned cameras=3,unsigned phase=12)
{
    Blob data;
    if(phase)Chunk(data,0x1200,Blob((phase<8?phase+32:phase)-8,0x5a));
    for(unsigned i=0;i<cameras;++i)
    {
        CameraChunk(data,i);
        if(i==0)Chunk(data,0x80017000,Blob(20,0xa5)); // Only this SANIM envelope is selected.
    }
    if(!cameras)Chunk(data,0x80017000,{});
    return data;
}
std::size_t Channel(const Blob& data,const NisCameraRange& range,unsigned id)
{
    const auto root=ReadChunk(data,range.offset,range.end);
    for(auto p=std::size_t(root.payload.data()-data.data());p<range.end;)
    {
        const auto c=ReadChunk(data,p,range.end);
        if(c.id==id)return std::size_t(c.payload.data()-data.data());
        p=c.next;
    }
    throw std::runtime_error("Missing fixture channel");
}
void Reader()
{
    for(unsigned phase=0;phase<32;phase+=4)
    {
        const auto bytes=Fixture(3,phase);const auto layout=ReadNisCameras(bytes);
        Check(layout.cameras.size()==3&&layout.animation_chunks==1&&layout.other_chunks==(phase!=0),"NIS chunk inventory differs");
        Check(layout.cameras[0].offset%32==phase,"Fixture does not exercise expected origin");
        for(unsigned slot=0;slot<3;++slot)
        {
            const auto& range=layout.cameras[slot];const auto c=ReadCameraAnimation(bytes,range.offset,range.end);
            Check(c.name=="synthetic_nis_camera_"+std::to_string(slot)&&c.keys.size()==3,"Embedded camera order/name differs");
            Near(c.keys[2].position[0],slot*20+4);
            for(const auto& key:c.keys)
            {Bits(key.target[0],std::bit_cast<float>(0xdeadbeefu));Bits(key.target[1],-0.f);Bits(key.target[2],std::bit_cast<float>(1u));}
        }
    }
    const auto bytes=Fixture();const auto layout=ReadNisCameras(bytes);const auto first=layout.cameras.front();
    Reject([&]{ReadCameraAnimation(Slice(bytes,first.offset,first.end-first.offset));}); // Losing the global alignment is wrong.
    for(auto [start,end]:{std::pair{bytes.size()+1,bytes.size()},std::pair{first.offset,bytes.size()+1},
                         std::pair{first.offset,first.end-1},std::pair{first.offset+1,first.end},std::pair{first.offset,first.end+4}})
    {
        Reject([&]{ReadCameraAnimation(bytes,start,end);});
        Reject([&]{CameraAsset::Decode(bytes,"range",start,end);});
    }
    Blob one;CameraChunk(one,0);
    for(std::size_t size=0;size<one.size();++size)Reject([&]{ReadNisCameras(Bytes(one).first(size));});
    Check(ReadNisCameras(Fixture(10)).cameras.size()==10,"Ten original camera slots rejected");
    Reject([]{ReadNisCameras(Fixture(11));});
    Check(ReadNisCameras(Fixture(0)).cameras.empty(),"Actor-only NIS acquired an invented camera");
    for(unsigned count:{0u,1u,2u,4u,65537u,0xffffffffu})
    {auto bad=bytes;Set(bad,Channel(bad,layout.cameras.back(),0x2500c),count);Reject([&]{ReadNisCameras(bad);});}
    for(auto range:layout.cameras)
    {
        auto bad=bytes;Set(bad,range.offset+4,0xffffffff);Reject([&]{ReadNisCameras(bad);});
        bad=bytes;Set(bad,range.offset,0x8602500b);Reject([&]{ReadNisCameras(bad);});
        bad=bytes;Set(bad,Channel(bad,range,0x25006),0x7fc00001);Reject([&]{ReadNisCameras(bad);});
        // A child may not consume bytes from a neighboring NIS chunk.
        bad=bytes;const auto root=ReadChunk(bad,range.offset,range.end);
        const auto child=std::size_t(root.payload.data()-bad.data());Set(bad,child+4,bytes.size()-child-8);
        Reject([&]{ReadNisCameras(bad);});
    }
    for(unsigned length:{1u,2u,3u}){auto bad=bytes;bad.resize(bad.size()+length);Reject([&]{ReadNisCameras(bad);});}
    Blob unpadded;Word(unpadded,0x1200);Word(unpadded,1);unpadded.push_back(0);Reject([&]{ReadNisCameras(unpadded);});
    Blob huge(MaximumAssetBytes+1);Reject([&]{ReadNisCameras(huge);});
}
void Ownership()
{
    CameraAsset::Handle selected;
    {
        auto bytes=Fixture();NisCameraAssets assets(bytes,"NIS");
        Check(assets.Layout().animation_chunks==1&&assets.Layout().other_chunks==1,"Unsupported payload scope was lost");
        selected=assets.Camera(1);Check(selected->Name()=="nis_1","Slot alias differs");
        std::fill(bytes.begin(),bytes.end(),0xcc);bytes.clear();bytes.shrink_to_fit();
        Check(selected->Data().m_uKeyCount==3&&selected->Data().cameraPos[2].x==24,"Input buffer retained or camera order changed");
        Bits(selected->Data().targetPos[0].x,std::bit_cast<float>(0xdeadbeefu));
        Bits(selected->Data().targetPos[0].y,-0.f);Bits(selected->Data().targetPos[0].z,std::bit_cast<float>(1u));
        Reject([&]{assets.Camera(3);});Reject([&]{assets.Camera(SIZE_MAX);});
        bool wrong=false;std::thread worker([&]{try{assets.Camera(0);}catch(const std::logic_error&){wrong=true;}});worker.join();
        Check(wrong,"Cross-thread NIS camera lookup accepted");
        wrong=false;std::thread metadata([&]{try{assets.Layout();}catch(const std::logic_error&){wrong=true;}});metadata.join();
        Check(wrong,"Cross-thread NIS metadata lookup accepted");
    }
    OriginalCameras cameras;AnimatedCamera playback(selected);selected.reset();
    AnimatedCameraOptions options;options.cyclic=false;options.mirror={-1,1,1};playback.Configure(options);
    cCameraManager::PushCamera(&playback.Camera());
    for(float time:{0.f,.125f,.25f,.5f,.75f,1.f})
    {
        playback.Seek(time);cameras.Advance(0,0);
        Near(cCameraManager::m_cameraPosition.x,-(20+4*time));Near(cCameraManager::m_fFOV,40+2*time);
    }
}
void FailedConstruction()
{
    const auto bytes=Fixture();const auto before=StandardAllocator.TotalFreeMemory();
    for(const auto& name:{std::string{},std::string("bad\0name",8),std::string(254,'x')})
        Reject([&]{NisCameraAssets bad(bytes,name);});
    {NisCameraAssets valid(bytes,std::string(253,'x'));Check(valid.Camera(2)->Name().size()==255,"Maximum slot alias rejected");}
    auto bad=bytes;const auto ranges=ReadNisCameras(bytes);Set(bad,Channel(bad,ranges.cameras.back(),0x2500c),1);
    Reject([&]{NisCameraAssets partial(bad,"partial");});
    Check(StandardAllocator.TotalFreeMemory()==before,"Malformed late camera leaked partial arena records");
    NisCameraAssets empty(Fixture(0),"actor-only");Check(empty.Layout().cameras.empty(),"Zero-camera collection differs");
    Reject([&]{empty.Camera(0);});
}
std::uint32_t Hash(const cCameraData& data)
{
    std::uint32_t hash=2166136261u;
    for(std::size_t i=0;i<data.m_uKeyCount;++i)
        for(float value:{data.cameraPos[i].x,data.cameraPos[i].y,data.cameraPos[i].z,data.targetPos[i].x,data.targetPos[i].y,
                         data.targetPos[i].z,data.cameraRot[i].x,data.cameraRot[i].y,data.cameraRot[i].z,data.cameraRot[i].w,
                         data.fFOV[i],data.fFocalLength[i]})
        {
            const auto bits=std::bit_cast<unsigned>(value);
            for(int shift:{24,16,8,0})hash=(hash^((bits>>shift)&255))*16777619u;
        }
    return hash;
}
void Owned(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary);Check(bool(input),"Cannot read NIS input");
    Blob bytes{std::istreambuf_iterator<char>(input),{}};
    const auto layout=ReadNisCameras(bytes);
    std::vector<CameraAsset::Handle> retained;
    {
        NisCameraAssets assets(bytes,"owned");
        for(std::size_t i=0;i<layout.cameras.size();++i)retained.push_back(assets.Camera(i));
    }
    bytes.clear();bytes.shrink_to_fit();
    std::cout<<"NIS "<<path.filename().string()<<' '<<layout.cameras.size()<<' '<<layout.animation_chunks<<' '<<layout.other_chunks<<'\n';
    OriginalCameras cameras;
    for(std::size_t slot=0;slot<retained.size();++slot)
    {
        auto asset=retained[slot];const auto& data=asset->Data();AnimatedCamera playback(asset);
        AnimatedCameraOptions options;options.cyclic=false;playback.Configure(options);cCameraManager::PushCamera(&playback.Camera());
        for(unsigned sample=0;sample<=100;++sample)
        {
            const float time=sample/100.f;playback.Seek(time);cameras.Advance(0,0);CheckCameraPose(playback.Camera());
            const float frame=time*float(data.m_uKeyCount-1);unsigned a=time>=1?data.m_uKeyCount-1:unsigned(frame);
            unsigned b=std::min(a+1,unsigned(data.m_uKeyCount-1));double weight=double(frame)-a;
            const auto& pa=data.cameraPos[a];const auto& pb=data.cameraPos[b];
            const double x=double(pa.x)-pb.x,y=double(pa.y)-pb.y,z=double(pa.z)-pb.z;
            if(x*x+y*y+z*z>16)weight=weight<.5?0:1;
            auto blend=[&](float first,float second){return float((1-weight)*first+weight*second);};
            Near(playback.Camera().GetCameraPosition().x,blend(pa.x,pb.x));
            Near(playback.Camera().GetCameraPosition().y,blend(pa.y,pb.y));
            Near(playback.Camera().GetCameraPosition().z,blend(pa.z,pb.z));
            Near(playback.Camera().GetFOV(),blend(data.fFOV[a],data.fFOV[b]));
            const auto& target=playback.Camera().GetTargetPosition();CheckCameraVector(target);
        }
        std::cout<<"CAM "<<slot<<' '<<data.m_uKeyCount<<' '<<Hash(data)<<" 101\n";
    }
}
}
int main(int argc,char** argv)
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        auto init=[&](std::size_t size){ResetStartupMemory();StandardAllocator.Initialize(mem1.data(),size);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;};
        Reject([]{NisCameraAssets noMemory(Fixture(),"no-memory");});init(mem1.size()*8);
        if(argc==2)
        {
            Owned(argv[1]);
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8&&VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"Owned NIS cameras did not recover both arenas");
        }
        else
        {
            Reader();FailedConstruction();for(unsigned i=0;i<3;++i)Ownership();
            {ScopedGameAllocator mem2Scope(VirtualAllocator);Ownership();}
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8&&VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"NIS camera owner did not recover both arenas");
            unsigned failures=0,successes=0;
            for(unsigned size=128;size<=4096;size+=16)
            {
                init(size);try{NisCameraAssets assets(Fixture(),"allocation");++successes;}
                catch(const std::bad_alloc&){++failures;}
                Check(StandardAllocator.TotalFreeMemory()==size,"Partial NIS camera allocation rollback leaked");
            }
            Check(failures>5&&successes>5,"NIS allocation sweep missed failure or success");
        }
        ResetStartupMemory();std::cout<<checks<<" NIS camera checks passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}

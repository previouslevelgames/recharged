#include "runtime/camera_assets.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* what) { ++checks; if (!value) throw std::runtime_error(what); }
template<class F> void Reject(F action)
{
    try { action(); } catch (const std::exception&) { ++checks; return; }
    throw std::runtime_error("Invalid camera asset operation succeeded");
}
using Blob = std::vector<std::uint8_t>;
void Word(Blob& data, std::uint32_t word)
{ for (int s : {24,16,8,0}) data.push_back(std::uint8_t(word >> s)); }
void Set(Blob& data, std::size_t pos, std::uint32_t word)
{ for (int s : {24,16,8,0}) data.at(pos++) = std::uint8_t(word >> s); }
void Chunk(Blob& data, std::uint32_t id, const Blob& payload, unsigned alignment = 0)
{
    const auto offset = data.size(); Word(data, id | (alignment << 24)); Word(data, 0);
    while (data.size() % (1u << alignment)) data.push_back(0);
    data.insert(data.end(), payload.begin(), payload.end());
    Set(data, offset + 4, data.size() - offset - 8);
    while (data.size() % 4) data.push_back(0);
}
Blob Fixture(bool reverse = false)
{
    std::map<unsigned, Blob> channels;
    const std::string name = "synthetic_camera_name_longer_than_32_bytes";
    auto& n = channels[0x25000]; n.assign(name.begin(), name.end()); n.push_back(0); n.resize(Align(n.size(),4));
    Word(channels[0x2500c], 3);
    for (unsigned i = 0; i < 3; ++i)
    {
        for (float v : {float(i),-2.f,4.f}) Word(channels[0x25003],std::bit_cast<unsigned>(v));
        for (float v : {7.f,8.f,9.f}) Word(channels[0x25006],std::bit_cast<unsigned>(v));
        for (float v : {0.f,0.f,0.f,1.f}) Word(channels[0x25004],std::bit_cast<unsigned>(v));
        Word(channels[0x25009], std::bit_cast<unsigned>(40.f + i));
        Word(channels[0x2500a], std::bit_cast<unsigned>(2.f + i));
    }
    Blob data; Word(data, 0x8002500b); Word(data, 0);
    if (reverse) for (auto it=channels.rbegin();it!=channels.rend();++it) Chunk(data,it->first,it->second,3);
    else for (const auto& [id,payload]:channels) Chunk(data,id,payload,3);
    Set(data,4,data.size()-8); return data;
}
std::size_t At(const Blob& data, unsigned id, bool header = false)
{
    for (std::size_t p=8;p<data.size();p=Align(p+8+U32(data,p+4),4))
        if ((U32(data,p)&0x80ffffff)==id) return header ? p : Align(p+8,1u<<((U32(data,p)>>24)&127));
    throw std::runtime_error("Fixture chunk not found");
}
void Reader()
{
    const auto file=Fixture(); const auto decoded=ReadCameraAnimation(file);
    Check(decoded==ReadCameraAnimation(Fixture(true)),"CAM channel order affected decoding");
    Check(decoded.name=="synthetic_camera_name_longer_than_32_bytes" && decoded.keys.size()==3,"CAM name/count changed");
    Check(decoded.keys[2].position==std::array<float,3>{2,-2,4},"BE position decode differs");
    Check(decoded.keys[1].target==std::array<float,3>{7,8,9},"BE target decode differs");
    Check(decoded.keys[0].rotation==std::array<float,4>{0,0,0,1},"BE quaternion decode differs");
    Check(decoded.keys[1].fov==41 && decoded.keys[1].focal_length==3,"BE scalar decode differs");
    for (std::size_t size=0;size<file.size();++size) Reject([&] { ReadCameraAnimation(Bytes(file).first(size)); });
    for (auto count : {0u,1u,2u,4u,65537u,0xffffffffu})
    { auto bad=file;Set(bad,At(bad,0x2500c),count);Reject([&] { ReadCameraAnimation(bad); }); }
    for (auto id : {0x25003u,0x25004u,0x25006u,0x25009u,0x2500au})
    {
        auto bad=file;Set(bad,At(bad,id),0x7fc00000);Reject([&] { ReadCameraAnimation(bad); });
        bad=file;Set(bad,At(bad,id,true)+4,0xffffffff);Reject([&] { ReadCameraAnimation(bad); });
        bad=file;Set(bad,At(bad,id,true),0x25011);Reject([&] { ReadCameraAnimation(bad); });
    }
    for (auto fov : {0.f,-1.f,180.f,200.f})
    {auto bad=file;Set(bad,At(bad,0x25009),std::bit_cast<unsigned>(fov));Reject([&] { ReadCameraAnimation(bad); });}
    auto bad=file; Set(bad,At(bad,0x25004)+12,0);Reject([&] { ReadCameraAnimation(bad); });
    bad=file;bad.push_back(0);Reject([&] { ReadCameraAnimation(bad); });
    bad=file;Set(bad,0,0x80025010);Reject([&] { ReadCameraAnimation(bad); });
    bad=file;Set(bad,At(bad,0x25003,true),0x06025003);Reject([&] { ReadCameraAnimation(bad); });
    bad=file;const auto nh=At(bad,0x25000,true),ns=At(bad,0x25000);
    std::fill(bad.begin()+ns,bad.begin()+nh+8+U32(bad,nh+4),'x');Reject([&] { ReadCameraAnimation(bad); });
    bad=file;bad[ns+2]=0;Reject([&] { ReadCameraAnimation(bad); });
    bad=file;Chunk(bad,0x2500c,{0,0,0,3});Set(bad,4,bad.size()-8);Reject([&] { ReadCameraAnimation(bad); });
    Blob large(MaximumAssetBytes+1);Reject([&] { ReadCameraAnimation(large); });
}
void Ownership()
{
    auto input=Fixture();
    auto a=CameraAsset::Decode(input,"Frontend");
    std::fill(input.begin(),input.end(),0xcc);
    const auto& data=a->Data();
    Check(data.m_uKeyCount==3 && data.cameraPos[2].x==2 && data.cameraPos[0].y==-2,"Native position/input lifetime differs");
    Check(data.targetPos[1].z==9 && data.cameraRot[2].w==1 && data.fFOV[1]==41 && data.fFocalLength[2]==4,"Native camera channels differ");
    Check(std::string(data.m_szName)=="synthetic_camera_name_longer_than_32_bytes","Native name was truncated");
    CameraAssetLibrary library;
    library.Insert(a);Check(library.Find("FRONTEND")==a,"Alias lookup is not case insensitive");
    Reject([&] { library.Insert(CameraAsset::Decode(Fixture(),"frontend")); });
    Check(library.Find("frontend")==a,"Duplicate insert changed old asset");
    library.Erase("frontend");Check(!library.Find("frontend") && a->Data().m_uKeyCount==3,"Removal invalidated retained handle");
    library.Insert(a);library.Clear();Check(a->Data().cameraPos[2].x==2,"Clear invalidated retained handle");
    library.Insert(CameraAsset::Decode(Fixture(),"a~"));
    Reject([&] { library.Insert(CameraAsset::Decode(Fixture(),"b]")); });
    Check(!library.Find("b]"),"Colliding hash was inserted");
    bool wrong_thread=false;std::thread worker([&] { try { a->Data(); } catch(const std::logic_error&) { wrong_thread=true; } });worker.join();
    Check(wrong_thread,"Wrong-thread camera data access accepted");
    Reject([&] { CameraAsset::Decode(Fixture(),std::string("bad\0name",8)); });
    Reject([&] { CameraAsset::Decode(Fixture(),""); });
    Reject([&] { CameraAsset::Decode(Fixture(),std::string(256,'x')); });
    Reject([&] { library.Insert(nullptr); });
    CameraAsset::Handle other;
    {ScopedGameAllocator mem2(VirtualAllocator);other=CameraAsset::Decode(Fixture(),"other");}
    Check(other->Data().m_uKeyCount==3,"Virtual arena camera allocation failed");
}
void TargetBits()
{
    // The target channel is an authored vector, not a mesh coordinate. Preserve
    // every finite binary32 bit pattern, including signed zero/subnormals.
    for (unsigned bits : {0u,0x80000000u,1u,0x80000001u,0x4cbebc20u,0xccbebc20u,
                          0x7149f2cau,0xf149f2cau,0x7f7fffffu,0xff7fffffu,0xdeadbeefu})
    {
        auto bytes=Fixture(); const auto start=At(bytes,0x25006);
        for(unsigned i=0;i<9;++i)Set(bytes,start+4*i,bits);
        const auto decoded=ReadCameraAnimation(bytes);auto native=CameraAsset::Decode(bytes,"bits");
        std::fill(bytes.begin(),bytes.end(),0xcc);
        for(unsigned i=0;i<3;++i)
        {
            for(float value:decoded.keys[i].target)Check(std::bit_cast<unsigned>(value)==bits,"Decoded target bits changed");
            const auto& v=native->Data().targetPos[i];
            for(float value:{v.x,v.y,v.z})Check(std::bit_cast<unsigned>(value)==bits,"Retained native target bits changed");
        }
    }
    for(unsigned bits:{0x7f800000u,0xff800000u,0x7fc00000u,0xffc00001u})
    {auto bytes=Fixture();Set(bytes,At(bytes,0x25006),bits);Reject([&]{CameraAsset::Decode(bytes,"bad");});}
    for(unsigned channel:{0x25003u,0x25004u,0x25009u,0x2500au})
    {auto bytes=Fixture();Set(bytes,At(bytes,channel),0x7149f2ca);Reject([&]{ReadCameraAnimation(bytes);});}
}
void Owned(const std::filesystem::path& path)
{
    unsigned files=0,keys=0;
    std::vector<std::filesystem::path> paths;
    if (std::filesystem::is_regular_file(path)) paths.push_back(path);
    else for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
        if (entry.path().extension()==".cam") paths.push_back(entry.path());
    for (const auto& entry : paths)
    {
        std::ifstream in(entry,std::ios::binary);Blob bytes{std::istreambuf_iterator<char>(in),{}};
        auto asset=CameraAsset::Decode(bytes,"owned");const auto& d=asset->Data();std::uint32_t hash=2166136261u;
        auto word=[&](float f) {const auto bits=std::bit_cast<std::uint32_t>(f);for(int s:{24,16,8,0})hash=(hash^((bits>>s)&255))*16777619u;};
        for(std::size_t i=0;i<d.m_uKeyCount;++i)
        {for(float f:{d.cameraPos[i].x,d.cameraPos[i].y,d.cameraPos[i].z,d.targetPos[i].x,d.targetPos[i].y,d.targetPos[i].z,d.cameraRot[i].x,d.cameraRot[i].y,d.cameraRot[i].z,d.cameraRot[i].w,d.fFOV[i],d.fFocalLength[i]})word(f);}
        std::cout<<entry.lexically_relative(path.parent_path()).generic_string()<<' '<<d.m_uKeyCount<<' '<<hash<<'\n';
        ++files;keys+=d.m_uKeyCount;
    }
    Check(files>0 && keys>0,"No owned camera files checked");
}
}
int main(int argc,char** argv)
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        auto init=[&](std::size_t size) {ResetStartupMemory();StandardAllocator.Initialize(mem1.data(),size);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;};
        init(mem1.size()*8);
        if(argc==2)Owned(argv[1]);
        else
        {
            Reader();TargetBits();for(int i=0;i<3;++i)Ownership();
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"Camera data ownership leaked");
            unsigned failures=0,successes=0;
            for(unsigned size=128;size<=1600;size+=16)
            {
                init(size);
                try {auto value=CameraAsset::Decode(Fixture(),"allocation");++successes;}
                catch(const std::bad_alloc&) {++failures;}
                Check(StandardAllocator.TotalFreeMemory()==size,"Partial camera allocation rollback leaked");
            }
            Check(failures>5 && successes>5,"Allocation sweep did not exercise both failure and success");
            std::cout<<checks<<" camera data checks passed\n";
        }
        ResetStartupMemory();
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}

#include "runtime/weighted_skin_pose.h"
#include "weighted_skin_fixture.h"
#include "Game/GL/SkinSoftwareSteps.h"
#include "Game/SHierarchy.h"
#include <atomic>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>

namespace allocation_probe { thread_local std::size_t budget=SIZE_MAX; }
void* operator new(std::size_t bytes)
{
    if(allocation_probe::budget!=SIZE_MAX)
    { if(!allocation_probe::budget)throw std::bad_alloc(); --allocation_probe::budget; }
    if(void* pointer=std::malloc(bytes?bytes:1))return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer)noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t)noexcept{std::free(pointer);}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete[](void* pointer)noexcept{::operator delete(pointer);}
void operator delete[](void* pointer,std::size_t)noexcept{::operator delete(pointer);}

using namespace mscharged;
namespace fixture=weighted_skin_fixture;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F function,std::source_location where=std::source_location::current())
{++checks;try{function();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at line "+std::to_string(where.line()));}
template<class F>void Unsupported(F function)
{++checks;try{function();}catch(const resources::UnsupportedResource&){return;}throw std::runtime_error("Expected typed unsupported result");}
fixture::Blob Read(const char* path)
{std::ifstream file(path,std::ios::binary);Check(bool(file),"Cannot open weighted pose fixture");return {std::istreambuf_iterator<char>(file),{}};}
std::shared_ptr<AnimationPoseFrame> Pose(HierarchyAsset::Handle hierarchy,unsigned seed=0)
{
    auto frame=std::make_shared<AnimationPoseFrame>();frame->hierarchy=std::move(hierarchy);
    frame->matrices.resize(frame->hierarchy->Data().GetNumNodes());
    std::uint32_t random=0x9e3779b9^seed;
    auto value=[&](){random=random*1664525+1013904223;return float(std::int32_t(random>>8)-0x7fffff)/4096.f;};
    for(unsigned node=0;node<frame->matrices.size();++node)
    {
        auto& matrix=frame->matrices[node];matrix.SetIdentity();
        matrix.m11=1.25f+float(node)*.25f;matrix.m22=3.f+float(node)*.5f;matrix.m33=.75f+float(node)*.25f;
        matrix.m12=.125f*float(int(node%3)-1);matrix.m13=.1875f;matrix.m21=-.25f;
        matrix.m23=.0625f;matrix.m31=.375f;matrix.m32=-.125f;
        matrix.m41=value();matrix.m42=value();matrix.m43=value();
    }
    return frame;
}
std::uint64_t Fingerprint(const WeightedSkinPoseFrame& frame)
{
    std::uint64_t hash=14695981039346656037ull;
    auto real=[&](float value){for(int shift:{24,16,8,0}){hash^=(std::bit_cast<std::uint32_t>(value)>>shift)&255;hash*=1099511628211ull;}};
    for(const auto& matrix:frame.matrices)for(float value:matrix.e)real(value);
    for(const auto& packet:frame.packets)
    {for(const auto& matrix:packet.matrices)for(const auto& row:matrix.values)for(float value:row)real(value);for(const auto& vector:packet.positions)for(float value:vector)real(value);for(const auto& vector:packet.normals)for(float value:vector)real(value);}
    return hash;
}
void Inspect(const WeightedSkinPoseFrame& frame)
{
    auto vectors=[&](const auto& values){std::cout<<'[';bool first=true;for(const auto& row:values){if(!first)std::cout<<',';first=false;std::cout<<'[';bool initial=true;auto real=[&](float value){if(!initial)std::cout<<',';initial=false;std::cout<<std::bit_cast<std::uint32_t>(value);};if constexpr(requires{row.e;}){for(float value:row.e)real(value);}else{for(float value:row)real(value);}std::cout<<']';}std::cout<<']';};
    std::cout<<"{\"inverse\":";vectors(frame.asset->InverseBinds());
    std::cout<<",\"input\":";vectors(frame.pose->matrices);
    std::cout<<",\"matrices\":";vectors(frame.matrices);
    std::cout<<",\"node_maps\":[";
    bool first=true;for(const auto& map:frame.asset->NodeMaps()){if(!first)std::cout<<',';first=false;std::cout<<'[';for(unsigned i=0;i<map.size();++i){if(i)std::cout<<',';std::cout<<map[i];}std::cout<<']';}
    std::cout<<"],\"packets\":[";
    first=true;for(const auto& packet:frame.packets)
    {
        if(!first)std::cout<<',';first=false;
        std::cout<<"{\"palette\":[";for(unsigned b=0;b<packet.matrices.size();++b){if(b)std::cout<<',';std::cout<<'[';for(unsigned i=0;i<12;++i){if(i)std::cout<<',';std::cout<<std::bit_cast<std::uint32_t>(packet.matrices[b].values[i/4][i%4]);}std::cout<<']';}
        std::cout<<"],\"positions\":";vectors(packet.positions);
        std::cout<<",\"normals\":";vectors(packet.normals);std::cout<<'}';
    }
    std::cout<<"],\"fingerprint\":"<<Fingerprint(frame)<<"}\n";
}
void Synthetic()
{
    const auto bytes=fixture::Model();auto hierarchy=HierarchyAsset::Decode(fixture::Rig(4));
    auto asset=WeightedSkinAsset::Decode(bytes,hierarchy);const auto baseline=asset->Data().packets[0].vertices;
    Reject([&]{WeightedSkinPose missing({});});
    WeightedSkinPose owner(asset);Check(!owner.Current(),"Unexpected initial pose");
    auto source=Pose(hierarchy),published=source;auto first=owner.Sample(source);const auto fingerprint=Fingerprint(*first);
    Check(first->asset==asset&&first->pose==source&&first->pose->hierarchy==hierarchy,"Frame did not retain exact source owners");
    Check(first->packets.size()==1&&first->packets[0].positions.size()==4&&first->packets[0].normals.size()==4,"Weighted output dimensions differ");
    Check(first->packets[0].normals[0][1]>1,"Weighted source normals were normalized or inverse-transposed");
    for(unsigned i=0;i<baseline.size();++i)
    {Check(baseline[i].bones==asset->Data().packets[0].vertices[i].bones,"Pose changed authored bone indices");Check(baseline[i].weights==asset->Data().packets[0].vertices[i].weights,"Pose changed authored weights");}
    Unsupported([&]{owner.Sample(source,1);});Unsupported([&]{owner.Sample(source,UINT32_MAX);});
    Reject([&]{owner.Sample({});});
    auto bad=Pose(HierarchyAsset::Decode(fixture::Rig(4)));Reject([&]{owner.Sample(bad);});
    bad=Pose(hierarchy);bad->matrices.pop_back();Reject([&]{owner.Sample(bad);});
    for(float invalid:{NAN,INFINITY,1e13f}){bad=Pose(hierarchy);bad->matrices[0].m11=invalid;Reject([&]{owner.Sample(bad);});}
    bad=Pose(hierarchy);bad->matrices[0].m14=1;Reject([&]{owner.Sample(bad);});
    Check(owner.Current()==first&&Fingerprint(*first)==fingerprint,"Failed preflight changed published output");
    const auto rounding=std::fegetround();Check(std::fesetround(FE_UPWARD)==0,"Cannot set rounding fixture");
    Reject([&]{owner.Sample(source);});Check(std::fesetround(rounding)==0,"Cannot restore host rounding");
    Check(owner.Current()==first,"Rounding rejection changed publication");
    std::atomic<unsigned> rejected=0;
    std::thread foreign([&]{try{owner.Current();}catch(const std::logic_error&){++rejected;}try{owner.Sample(source);}catch(const std::logic_error&){++rejected;}try{owner.Reset();}catch(const std::logic_error&){++rejected;}try{owner.Release();}catch(const std::logic_error&){++rejected;}});foreign.join();
    Check(rejected==4&&owner.Current()==first,"Foreign thread bypassed retained owner checks");
    unsigned failed_allocations=0;
    for(std::size_t budget=0;budget<100;++budget)
    {
        allocation_probe::budget=budget;
        try{owner.Sample(source);allocation_probe::budget=SIZE_MAX;break;}
        catch(const std::bad_alloc&){allocation_probe::budget=SIZE_MAX;++failed_allocations;Check(owner.Current()==first,"OOM changed pose publication");}
    }
    Check(failed_allocations>=6,"Allocation failure fixture did not cover retained arrays");
    auto second=owner.Sample(Pose(hierarchy,17));
    Check(first->packets[0].positions.data()!=second->packets[0].positions.data()&&first->packets[0].normals.data()!=second->packets[0].normals.data(),"Frame outputs alias a different generation");
    Check(Fingerprint(*first)==fingerprint,"Next sample changed a retained earlier frame");
    bad=Pose(hierarchy);for(auto& matrix:bad->matrices){matrix.m11=matrix.m22=matrix.m33=0;matrix.m12=matrix.m13=matrix.m21=matrix.m23=matrix.m31=matrix.m32=0;}
    auto zero=owner.Sample(bad);for(const auto& normal:zero->packets[0].normals)for(float value:normal)Check(value==0,"Direct weighted normal did not allow singular zero scale");
    owner.Reset();Check(!owner.Current()&&Fingerprint(*first)==fingerprint,"Reset altered retained output");
    owner.Release();owner.Release();Reject([&]{owner.Current();});Reject([&]{owner.Sample(source);});
    source.reset();published.reset();asset.reset();hierarchy.reset();
    Check(first->asset&&first->pose&&Fingerprint(*first)==fingerprint,"Release failed to preserve retained owners");
    auto rigid_bytes=bytes;const auto vertices=fixture::Find(rigid_bytes,0x1b006);const auto bones=vertices+48+48+16*3,weights=bones+16;
    for(unsigned v=0;v<4;++v){rigid_bytes[bones+v*4]=v%4;for(unsigned lane=0;lane<4;++lane)fixture::Set(rigid_bytes,weights+v*16+lane*4,std::bit_cast<unsigned>(lane==0?1.f:0.f));}
    auto rigid=WeightedSkinAsset::Decode(rigid_bytes,HierarchyAsset::Decode(fixture::Rig(4)));
    Unsupported([&]{WeightedSkinPose invalid(rigid);});
    std::cout<<checks<<" weighted software pose checks passed\n";
}
void Equations(const char* path)
{
    const auto data=Read(path);const auto count=resources::U32(data,0);
    Check(count<=10000&&data.size()==4+count*29*4,"Invalid software arithmetic fixture");
    std::cout<<'[';
    for(unsigned i=0;i<count;++i)
    {
        std::array<float,29> input;for(unsigned j=0;j<input.size();++j)input[j]=resources::F32(data,4+(i*29+j)*4);
        nlMatrix4 matrix;std::copy_n(input.data(),16,matrix.e);
        nlVector3 position{input[16],input[17],input[18]},normal{input[19],input[20],input[21]};
        nlVector3 out_position{input[23],input[24],input[25]},out_normal{input[26],input[27],input[28]};
        SkinSoftwarePosition(out_position,position,matrix,input[22]);SkinSoftwareNormal(out_normal,normal,matrix,input[22]);
        if(i)std::cout<<',';std::cout<<'[';for(unsigned j=0;j<6;++j){if(j)std::cout<<',';std::cout<<std::bit_cast<std::uint32_t>(j<3?out_position.e[j]:out_normal.e[j-3]);}std::cout<<']';
    }
    std::cout<<"]\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc==1){Synthetic();return 0;}
        if(argc==3&&std::string(argv[1])=="--equations"){Equations(argv[2]);return 0;}
        if(argc==4&&std::string(argv[1])=="--fixture")
        {const auto model=fixture::Model(),hierarchy=fixture::Rig(4);std::ofstream a(argv[2],std::ios::binary),b(argv[3],std::ios::binary);a.write(reinterpret_cast<const char*>(model.data()),model.size());b.write(reinterpret_cast<const char*>(hierarchy.data()),hierarchy.size());return !a||!b;}
        Check(argc==5,"Use --inspect MODEL HIERARCHY seed");Check(std::string(argv[1])=="--inspect","Unknown weighted pose command");
        const auto raw=Read(argv[2]);auto hierarchy=HierarchyAsset::Decode(Read(argv[3]));auto asset=WeightedSkinAsset::Decode(raw,hierarchy);
        WeightedSkinPose owner(asset);auto frame=owner.Sample(Pose(hierarchy,unsigned(std::stoul(argv[4]))));Inspect(*frame);
        return 0;
    }
    catch(const std::exception& error){allocation_probe::budget=SIZE_MAX;std::cerr<<"FAILED: "<<error.what()<<" check "<<checks<<'\n';return 1;}
}

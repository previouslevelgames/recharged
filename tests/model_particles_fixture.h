#pragma once
#include "runtime/model_particles.h"
#include "runtime/pose_accumulator.h"
#include "pose_accumulator_fixture.h"
#include "Game/SHierarchy.h"
#include <filesystem>
#include <fstream>
namespace model_particle_fixture
{
using namespace mscharged;
inline std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Missing model-particle input "+path.string());return{std::istreambuf_iterator<char>(f),{}};}
inline EffectsRegistry::Handle Registry(const std::filesystem::path& folder,const std::string& mode,bool owned=false)
{
    auto files=std::make_shared<ParticleFiles>();
    const std::array<std::string,4> paths=owned?std::array<std::string,4>{"Art/effects/effects.bun","Art/effects/effectsnonres.bun","Art/objects/effectsgeometry.bun","Art/objects/effectsgeometrytextures.rlt"}
        :std::array<std::string,4>{mode+".bun","nonresident.bun","geometry.bun","textures.rlt"};
    for(unsigned i=0;i<4;++i){files->data[i]=Read(folder/paths[i]);files->source_sizes[i]=files->data[i].size();}
    return EffectsRegistry::FromFiles(files);
}
inline resources::EffectsGeometry Geometry()
{
    resources::Packet packet;packet.primitive=0;packet.material.program=0xee9d919d;packet.material.textures[0]={0x87654321,3};
    packet.material.specular_colour={1,1,1,1};packet.raster=0xc0007;
    packet.vertices={{{-.25f,-.25f,0},{.125f,.125f}},{{.25f,-.25f,0},{.125f,.125f}},{{.25f,.25f,0},{.125f,.125f}},{{-.25f,.25f,0},{.125f,.125f}}};
    packet.indices={0,1,2,0,2,3};resources::EffectsGeometry result;
    result.models.push_back({0x10203040,{packet}});resources::EffectsVertexAnimation animation{0x10203040,3,4,12,1,{1},{}};
    for(unsigned frame=0;frame<3;++frame)for(auto v:packet.vertices){v.position[0]+=.5f*(int(frame)-1);animation.positions.push_back(v.position);}
    result.animations.push_back(animation);return result;
}
inline AnimationPoseFrame::Handle Pose()
{
    auto blob=pose_fixture::Hierarchy({-1,0,0},{},-1,-1,{0x44445555,0x11112222,0x22223333});
    for(std::size_t at=8;at<blob.size();)
    {
        auto word=[&](std::size_t n){return unsigned(blob[n])<<24|unsigned(blob[n+1])<<16|unsigned(blob[n+2])<<8|blob[n+3];};
        const auto kind=word(at),size=word(at+4);if(kind==0x18008){pose_fixture::Set(blob,at+12,2);pose_fixture::Set(blob,at+16,1);break;}at=(at+8+size+3)&~3;
    }
    auto frame=std::make_shared<AnimationPoseFrame>();frame->hierarchy=HierarchyAsset::Decode(blob);frame->matrices.resize(3);
    for(auto& matrix:frame->matrices)matrix.SetIdentity();frame->matrices[1].e[12]=3;frame->matrices[1].e[13]=4;frame->matrices[1].e[14]=5;
    auto& mirror=frame->matrices[2];mirror.e[5]=mirror.e[10]=0;mirror.e[6]=1;mirror.e[9]=-1;
    mirror.e[12]=-4;mirror.e[13]=1;mirror.e[14]=2;return frame;
}
inline AnimationPoseFrame::Handle RestPose(HierarchyAsset::Handle hierarchy)
{
    PoseAccumulator pose(hierarchy);const auto& source=hierarchy->Data();
    for(unsigned n=0;n<pose.Nodes();++n)
    {
        pose.BlendRotationIdentity(n,1);pose.BlendScaleIdentity(n,1);
        const auto& t=source.GetTranslationOffset(n);pose.BlendTranslation(n,{t.x,t.y,t.z},1);
    }
    nlMatrix4 world;world.SetIdentity();pose.Build(world);
    auto result=std::make_shared<AnimationPoseFrame>();result->hierarchy=hierarchy;
    for(unsigned n=0;n<pose.Nodes();++n)result->matrices.push_back(pose.Matrix(n));pose.Release();return result;
}
}

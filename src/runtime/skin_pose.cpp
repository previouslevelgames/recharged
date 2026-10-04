#include "runtime/skin_pose.h"
#include "Game/GL/SkinPoseSteps.h"
#include "Game/SHierarchy.h"
#include <algorithm>
#include <cmath>
#include <thread>
namespace mscharged
{
namespace
{
void Require(bool okay,const char* message){if(!okay)throw std::invalid_argument(message);}
nlMatrix4 Native(const std::array<float,16>& value){nlMatrix4 out;std::copy(value.begin(),value.end(),out.e);return out;}
void CheckMatrix(const nlMatrix4& m,bool authored=true)
{
    for(float v:m.e)Require(std::isfinite(v)&&std::abs(v)<=1e12f,"Nonfinite or excessive skin matrix");
    if(authored)Require(m.m14==0&&m.m24==0&&m.m34==0&&m.m44==1,"Authored skin matrix must be affine");
    // Original C_MTX44Inverse uses float pivoted elimination and can leave
    // roundoff in an affine matrix's fourth column. Keep those exact results:
    // original glxCopyMatrix selects only the consumed GX 3x4 components.
    const double determinant=double(m.m11)*(double(m.m22)*m.m33-double(m.m23)*m.m32)
        -double(m.m12)*(double(m.m21)*m.m33-double(m.m23)*m.m31)
        +double(m.m13)*(double(m.m21)*m.m32-double(m.m22)*m.m31);
    Require(std::isfinite(determinant)&&std::abs(determinant)>=1e-8,"Singular or degenerate skin normal matrix is not qualified");
}
}
struct RigidSkinAsset::Storage
{
    resources::RigidSkinModel model;
    HierarchyAsset::Handle hierarchy;
    std::vector<nlMatrix4> inverse;
    std::vector<std::vector<unsigned>> maps;
    Storage(resources::Bytes bytes,HierarchyAsset::Handle h,std::optional<std::uint32_t> selected)
        :model(resources::ReadRigidSkinModel(bytes,selected)),hierarchy(std::move(h))
    {
        Require(bool(hierarchy),"Skin asset requires a retained checked hierarchy");
        const auto& tree=hierarchy->Data();const unsigned count=tree.GetNumNodes();
        inverse.resize(count);std::vector<bool> authored(count);
        for(auto& m:inverse)m.SetIdentity(); // Original SetHierarchy initialization.
        for(const auto& record:model.binds)
        {
            auto source=Native(record.matrix);CheckMatrix(source);nlMatrix4 matrix;
            SkinInverseBind(matrix,source);CheckMatrix(matrix,false);
            const int node=tree.GetNodeIndexByID(record.hash);
            if(node!=-1){inverse[node]=matrix;authored[node]=true;} // Original later records win.
        }
        maps.reserve(model.packets.size());
        for(auto& packet:model.packets)
        {
            CheckMatrix(Native(packet.matrix));
            std::vector<unsigned> map;
            for(auto hash:packet.bone_hashes)
            {
                const int node=tree.GetNodeIndexByID(hash);
                Require(node>=0&&unsigned(node)<count,"Skin bone hash is absent from its retained hierarchy");
                Require(authored[node],"Referenced skin bone lacks an authored bind matrix");
                map.push_back(node);
            }
            for(auto& vertex:packet.vertices)
            {
                SkinMoveLargestWeightFirst(vertex.bones.data(),vertex.weights.data());
                Require(vertex.weights[0]==1&&vertex.weights[1]==0&&vertex.weights[2]==0&&vertex.weights[3]==0
                    &&vertex.bones[0]<map.size(),"Original rigid weight preparation violated its qualified profile");
            }
            maps.push_back(std::move(map));
        }
    }
};
RigidSkinAsset::RigidSkinAsset(resources::Bytes data,HierarchyAsset::Handle h,std::optional<std::uint32_t> selected)
    :storage_(std::make_unique<Storage>(data,std::move(h),selected)){}
RigidSkinAsset::~RigidSkinAsset()=default;
RigidSkinAsset::Handle RigidSkinAsset::Decode(resources::Bytes data,HierarchyAsset::Handle h,std::optional<std::uint32_t> selected)
{return Handle(new RigidSkinAsset(data,std::move(h),selected));}
const resources::RigidSkinModel& RigidSkinAsset::Data()const{return storage_->model;}
HierarchyAsset::Handle RigidSkinAsset::Hierarchy()const{return storage_->hierarchy;}
const std::vector<nlMatrix4>& RigidSkinAsset::InverseBinds()const{return storage_->inverse;}
const std::vector<std::vector<unsigned>>& RigidSkinAsset::NodeMaps()const{return storage_->maps;}
struct SkinPose::Implementation
{
    RigidSkinAsset::Handle asset;
    SkinPoseFrame::Handle current;
    const std::thread::id thread=std::this_thread::get_id();
    explicit Implementation(RigidSkinAsset::Handle a):asset(std::move(a)){Require(bool(asset),"Skin pose requires a retained rigid asset");}
    void Thread()const{if(thread!=std::this_thread::get_id())throw std::logic_error("Skin pose requires its creating thread");}
    void Check()const{Thread();if(!asset)throw std::logic_error("Skin pose has been released");}
};
SkinPose::SkinPose(RigidSkinAsset::Handle asset):impl_(std::make_unique<Implementation>(std::move(asset))){}
SkinPose::~SkinPose()=default;
SkinPoseFrame::Handle SkinPose::Current()const{impl_->Check();return impl_->current;}
void SkinPose::Reset(){impl_->Check();impl_->current.reset();}
void SkinPose::Release(){impl_->Thread();impl_->current.reset();impl_->asset.reset();}
SkinPoseFrame::Handle SkinPose::Sample(AnimationPoseFrame::Handle pose)
{
    impl_->Check();Require(pose&&pose->hierarchy==impl_->asset->Hierarchy(),"Skin pose hierarchy identity differs from its retained asset");
    const auto count=impl_->asset->InverseBinds().size();Require(pose->matrices.size()==count,"Skin pose matrix count differs from its hierarchy");
    for(const auto& m:pose->matrices)CheckMatrix(m);
    struct PoseInput
    {
        const std::vector<nlMatrix4>& matrices;
        int GetNumNodes()const{return int(matrices.size());}
        const nlMatrix4& GetNodeMatrix(int i)const{return matrices[i];}
    } input{pose->matrices};
    auto next=std::make_shared<SkinPoseFrame>();next->asset=impl_->asset;next->pose=std::move(pose);next->matrices.resize(count);
    SkinBuildPoseMatrices(next->matrices.data(),impl_->asset->InverseBinds().data(),input);
    for(const auto& m:next->matrices)CheckMatrix(m,false);
    for(const auto& map:impl_->asset->NodeMaps())
    {
        std::vector<SkinMatrix3x4> packet(map.size());
        for(unsigned i=0;i<map.size();++i)SkinCopyPoseMatrix(packet[i].values,next->matrices[map[i]]);
        next->packets.push_back(std::move(packet));
    }
    impl_->current=next;return next;
}
}

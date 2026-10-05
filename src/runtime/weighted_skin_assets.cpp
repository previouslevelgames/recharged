#include "runtime/weighted_skin_assets.h"
#include "Game/GL/SkinPoseSteps.h"
#include "Game/SHierarchy.h"
#include <algorithm>
#include <cmath>
namespace mscharged
{
namespace
{
void Require(bool okay,const char* message){if(!okay)throw std::invalid_argument(message);}
nlMatrix4 Native(const std::array<float,16>& value){nlMatrix4 out;std::copy(value.begin(),value.end(),out.e);return out;}
void Matrix(const nlMatrix4& m,bool authored=true)
{
    for(float x:m.e)Require(std::isfinite(x)&&std::abs(x)<=1e12f,"Invalid weighted skin matrix domain");
    if(authored)Require(m.m14==0&&m.m24==0&&m.m34==0&&m.m44==1,"Authored weighted skin matrix must be affine");
    const double determinant=double(m.m11)*(double(m.m22)*m.m33-double(m.m23)*m.m32)
        -double(m.m12)*(double(m.m21)*m.m33-double(m.m23)*m.m31)
        +double(m.m13)*(double(m.m21)*m.m32-double(m.m22)*m.m31);
    Require(std::isfinite(determinant)&&std::abs(determinant)>=1e-8,"Weighted skin inverse bind is singular or degenerate");
}
}
struct WeightedSkinAsset::Storage
{
    resources::WeightedSkinModel model;
    HierarchyAsset::Handle hierarchy;
    std::vector<nlMatrix4> inverse;
    std::vector<std::vector<unsigned>> maps;
    std::vector<PacketWeights> weights;
    bool rigid=true;
    Storage(resources::Bytes bytes,HierarchyAsset::Handle h,std::optional<std::uint32_t> selected)
        :model(resources::ReadWeightedSkinModel(bytes,selected)),hierarchy(std::move(h))
    {
        Require(bool(hierarchy),"Weighted skin requires a retained hierarchy");
        const auto& tree=hierarchy->Data();const unsigned count=tree.GetNumNodes();
        inverse.resize(count);std::vector<bool> authored(count);
        for(auto& m:inverse)m.SetIdentity();
        for(const auto& bind:model.binds)
        {
            const auto source=Native(bind.matrix);Matrix(source);nlMatrix4 result;
            SkinInverseBind(result,source);Matrix(result,false);
            const int index=tree.GetNodeIndexByID(bind.hash);
            if(index>=0){inverse[index]=result;authored[index]=true;}
        }
        for(auto& packet:model.packets)
        {
            Matrix(Native(packet.matrix));std::vector<unsigned> map;
            for(auto hash:packet.bone_hashes)
            {
                const int index=tree.GetNodeIndexByID(hash);
                Require(index>=0&&unsigned(index)<count&&authored[index],"Weighted skin bone map has no authored hierarchy/bind identity");
                map.push_back(index);
            }
            std::vector<std::array<unsigned char,4>> indices;
            std::vector<std::array<float,4>> values;
            std::vector<std::size_t> pair_counts(map.size());
            for(auto& v:packet.vertices)
            {
                SkinMoveLargestWeightFirst(v.bones.data(),v.weights.data());
                bool zero=false;
                for(unsigned k=0;k<4;++k)
                {
                    if(v.weights[k]==0){zero=true;continue;}
                    if(zero)throw resources::UnsupportedResource("Weighted skin contains an active lane after the original zero terminator");
                    Require(v.bones[k]<map.size(),"Weighted skin prepared index exceeds its packet map");
                    ++pair_counts[v.bones[k]];
                }
                indices.push_back(v.bones);values.push_back(v.weights);
            }
            // The source temporary stores 2*numVertices pairs for EACH bone.
            // Three/four distinct influences are legal; repeated same-bone
            // lanes must still fit that original per-bone allocation.
            const auto capacity=packet.vertices.size()*2;
            for(auto pairs:pair_counts)if(pairs>capacity)
                throw resources::UnsupportedResource("Weighted skin exceeds original per-bone scratch capacity");
            std::vector<WeightedSkinPair> scratch(capacity);PacketWeights data(map.size());
            for(unsigned bone=0;bone<map.size();++bone)
            {
                const auto n=SkinGatherBoneWeights(scratch.data(),bone,packet.vertices.size(),indices.data(),values.data(),rigid);
                Require(n>=0&&std::size_t(n)==pair_counts[bone],"Original weighted pair count differs from checked storage");
                data[bone].assign(scratch.begin(),scratch.begin()+n);
            }
            maps.push_back(std::move(map));weights.push_back(std::move(data));
        }
    }
};
WeightedSkinAsset::WeightedSkinAsset(resources::Bytes bytes,HierarchyAsset::Handle h,std::optional<std::uint32_t> selected)
    :storage_(std::make_unique<Storage>(bytes,std::move(h),selected)){}
WeightedSkinAsset::~WeightedSkinAsset()=default;
WeightedSkinAsset::Handle WeightedSkinAsset::Decode(resources::Bytes bytes,HierarchyAsset::Handle h,std::optional<std::uint32_t> selected)
{return Handle(new WeightedSkinAsset(bytes,std::move(h),selected));}
const resources::WeightedSkinModel& WeightedSkinAsset::Data() const{return storage_->model;}
HierarchyAsset::Handle WeightedSkinAsset::Hierarchy() const{return storage_->hierarchy;}
const std::vector<nlMatrix4>& WeightedSkinAsset::InverseBinds() const{return storage_->inverse;}
const std::vector<std::vector<unsigned>>& WeightedSkinAsset::NodeMaps() const{return storage_->maps;}
const std::vector<WeightedSkinAsset::PacketWeights>& WeightedSkinAsset::Weights() const{return storage_->weights;}
bool WeightedSkinAsset::OriginalRigidFlag() const{return storage_->rigid;}
}

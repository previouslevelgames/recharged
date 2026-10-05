#include "runtime/weighted_skin_pose.h"
#include "Game/GL/SkinPoseSteps.h"
#include "Game/GL/SkinSoftwareSteps.h"
#include "Game/SHierarchy.h"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool value, const char* message)
{ if (!value) throw std::invalid_argument(message); }
void Matrix(const nlMatrix4& matrix, bool authored = true)
{
    for (float value : matrix.e)
        Require(std::isfinite(value) && std::abs(value) <= 1e12f,
            "Weighted pose matrix exceeds its finite qualified domain");
    if (authored)
        Require(matrix.m14 == 0 && matrix.m24 == 0 && matrix.m34 == 0 && matrix.m44 == 1,
            "Weighted pose input must be affine");
    // The software path directly multiplies normals by the source3x3.
    // Singular scale is valid here; no inverse-transpose is introduced.
}
nlVector3 Vector(const std::array<float,3>& source)
{ return {source[0], source[1], source[2]}; }
}
struct WeightedSkinPose::Implementation
{
    WeightedSkinAsset::Handle asset;
    WeightedSkinPoseFrame::Handle current;
    const std::thread::id thread = std::this_thread::get_id();
    explicit Implementation(WeightedSkinAsset::Handle value) : asset(std::move(value))
    {
        Require(bool(asset), "Weighted pose requires a retained skin asset");
        if (asset->OriginalRigidFlag())
            throw resources::UnsupportedResource("Original rigid skin does not enter the selected software path");
    }
    void Thread() const
    {
        if (thread != std::this_thread::get_id())
            throw std::logic_error("Weighted pose requires its creating thread");
    }
    void Check() const
    { Thread(); Require(bool(asset), "Weighted pose has been released"); }
};
WeightedSkinPose::WeightedSkinPose(WeightedSkinAsset::Handle asset)
    : impl_(std::make_unique<Implementation>(std::move(asset))) {}
WeightedSkinPose::~WeightedSkinPose() = default;
WeightedSkinPoseFrame::Handle WeightedSkinPose::Current() const
{ impl_->Check(); return impl_->current; }
void WeightedSkinPose::Reset()
{ impl_->Check(); impl_->current.reset(); }
void WeightedSkinPose::Release()
{ impl_->Thread(); impl_->current.reset(); impl_->asset.reset(); }
WeightedSkinPoseFrame::Handle WeightedSkinPose::Sample(AnimationPoseFrame::Handle pose, unsigned active_morphs)
{
    impl_->Check();
    if (active_morphs)
        throw resources::UnsupportedResource("Native weighted software pose cannot execute nonzero morphs");
    Require(std::fegetround() == FE_TONEAREST,
        "Weighted paired-single equations require nearest host rounding");
    Require(pose && pose->hierarchy == impl_->asset->Hierarchy(),
        "Weighted pose requires the exact retained hierarchy identity");
    const auto count = impl_->asset->InverseBinds().size();
    Require(pose->matrices.size() == count, "Weighted pose matrix count differs from its hierarchy");
    for (const auto& matrix : pose->matrices) Matrix(matrix);
    struct PoseInput
    {
        const std::vector<nlMatrix4>& matrices;
        int GetNumNodes() const { return int(matrices.size()); }
        const nlMatrix4& GetNodeMatrix(int index) const { return matrices[index]; }
    } input{pose->matrices};
    auto next = std::make_shared<WeightedSkinPoseFrame>();
    next->asset = impl_->asset; next->pose = std::move(pose); next->matrices.resize(count);
    SkinBuildPoseMatrices(next->matrices.data(), impl_->asset->InverseBinds().data(), input);
    for (const auto& matrix : next->matrices) Matrix(matrix, false);
    next->packets.reserve(impl_->asset->Data().packets.size());
    for (unsigned packet_index = 0; packet_index < impl_->asset->Data().packets.size(); ++packet_index)
    {
        const auto& source = impl_->asset->Data().packets[packet_index];
        const auto& map = impl_->asset->NodeMaps()[packet_index];
        const auto& weights = impl_->asset->Weights()[packet_index];
        WeightedSkinPosePacket packet;
        packet.positions.resize(source.vertices.size());
        packet.normals.resize(source.vertices.size());
        packet.matrices.resize(map.size());
        // Original CopyMatrices -> GX3x4 -> SoftwareSkinModel temporary
        // nlMatrix4 roundtrip. Unconsumed fourth-column values are discarded.
        std::vector<nlMatrix4> temporary(map.size());
        for (unsigned bone = 0; bone < map.size(); ++bone)
        {
            SkinCopyPoseMatrix(packet.matrices[bone].values, next->matrices[map[bone]]);
            glxCopyMatrix(temporary[bone], packet.matrices[bone].values);
        }
        // Bone→vertex/lane pair order is source-authored. Never change this
        // into vertex→lane accumulation, even when mathematically equivalent.
        for (unsigned bone = 0; bone < weights.size(); ++bone)
            for (const auto& pair : weights[bone])
            {
                const auto position = Vector(source.vertices[pair.vertexIndex].position);
                const auto normal = Vector(source.vertices[pair.vertexIndex].normal);
                auto output_position = Vector(packet.positions[pair.vertexIndex]);
                auto output_normal = Vector(packet.normals[pair.vertexIndex]);
                SkinSoftwarePosition(output_position, position, temporary[bone], pair.vertexWeight);
                SkinSoftwareNormal(output_normal, normal, temporary[bone], pair.vertexWeight);
                std::copy(std::begin(output_position.e), std::end(output_position.e), packet.positions[pair.vertexIndex].begin());
                std::copy(std::begin(output_normal.e), std::end(output_normal.e), packet.normals[pair.vertexIndex].begin());
            }
        for (const auto& values : {&packet.positions, &packet.normals})
            for (const auto& value : *values)
                for (float component : value)
                    Require(std::isfinite(component), "Weighted software output became nonfinite");
        next->packets.push_back(std::move(packet));
    }
    impl_->current = next;
    return next;
}
}

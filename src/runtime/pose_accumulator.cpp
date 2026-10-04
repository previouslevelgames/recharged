#include "runtime/pose_accumulator.h"
#include "runtime/graphics_memory.h"
#include "resources/hierarchy.h"
#include "Game/PoseAccumulator.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace mscharged
{
namespace
{
void Require(bool okay, const char* message) { if (!okay) throw std::invalid_argument(message); }
void Finite(float value, float bound, const char* message)
{ Require(std::isfinite(value) && std::abs(value) <= bound, message); }
void Weight(float value) { Require(std::isfinite(value) && value >= 0 && value <= 1, "Pose weight must be finite and within [0,1]"); }
void Sum(float previous, float weight)
{ Require(std::isfinite(previous) && previous >= 0 && previous + weight <= 8192, "Pose accumulated weight exceeds its bounded positive profile"); }
nlVector3 Vector(std::array<float,3> values, float bound)
{
    for (float value : values) Finite(value,bound,"Invalid pose channel component");
    return {values[0],values[1],values[2]};
}
double Maximum(const nlVector3& v)
{ return std::max({std::abs(double(v.x)),std::abs(double(v.y)),std::abs(double(v.z))}); }
double World(const nlMatrix4& world)
{
    for (float value : world.e) Finite(value,1e7f,"Invalid pose world matrix");
    Require(world.m14 == 0 && world.m24 == 0 && world.m34 == 0 && world.m44 == 1,
        "Pose world matrix must be affine");
    double axes[3][3], lengths[3];
    for (unsigned row=0;row<3;++row)
    {
        double square = 0;
        for (unsigned col=0;col<3;++col) square += double(world.e2[row][col])*world.e2[row][col];
        lengths[row] = std::sqrt(square);
        Require(lengths[row] >= 1e-4 && lengths[row] <= 1e4,"Pose world axis length is outside its nonzero bounded profile");
        for (unsigned col=0;col<3;++col) axes[row][col] = world.e2[row][col] / lengths[row];
    }
    for (unsigned a=0;a<3;++a) for (unsigned b=a+1;b<3;++b)
    {
        double dot = 0; for (unsigned i=0;i<3;++i) dot += axes[a][i]*axes[b][i];
        Require(std::abs(dot) <= 1e-4,"Sheared pose world matrices are not qualified");
    }
    const double determinant = axes[0][0]*(axes[1][1]*axes[2][2]-axes[1][2]*axes[2][1])
        - axes[0][1]*(axes[1][0]*axes[2][2]-axes[1][2]*axes[2][0])
        + axes[0][2]*(axes[1][0]*axes[2][1]-axes[1][1]*axes[2][0]);
    Require(determinant > .999,"Reflected pose world matrices are not qualified");
    return std::max({lengths[0],lengths[1],lengths[2],1.0});
}
}
struct PoseAccumulator::Implementation
{
    HierarchyAsset::Handle hierarchy;
    std::unique_ptr<cPoseAccumulator> pose;
    std::vector<nlMatrix4> matrices, previous;
    std::vector<std::array<float,4>> quaternions;
    std::thread::id thread = std::this_thread::get_id();
    bool store_previous, ready = false, failed = false;
    Implementation(HierarchyAsset::Handle h, bool history) : hierarchy(std::move(h)), store_previous(history)
    {
        if (!gMemoryInitialized) throw std::logic_error("Pose ownership requires live game arenas");
        Require(hierarchy && hierarchy->MaximumDepth() <= resources::MaximumHierarchyDepth,"Pose requires a checked retained hierarchy");
        const auto n = hierarchy->Data().m_nNumNodes;
        Require(n > 0 && n <= int(resources::MaximumHierarchyNodes),"Invalid pose hierarchy node count");
        matrices.resize(n); quaternions.resize(n); if (history) previous.resize(n);
        Construct();
    }
    void Construct()
    {
        ScopedGameAllocator allocator(VirtualAllocator);
        auto next = std::make_unique<cPoseAccumulator>(const_cast<cSHierarchy*>(&hierarchy->Data()),store_previous);
        next->InitAccumulators();
        pose = std::move(next); ready = failed = false;
    }
    void Thread() const
    { if (thread != std::this_thread::get_id()) throw std::logic_error("Pose ownership requires its creating thread"); }
    void Check() const
    {
        Thread();
        if (!pose || !gMemoryInitialized) throw std::logic_error("Pose ownership or its game arenas have been released");
    }
    void Node(unsigned node) const
    { Check(); Require(node < matrices.size(),"Pose node index is outside the retained hierarchy"); }
    void Mutable(unsigned node, float weight) const
    {
        Node(node); if (failed) throw std::logic_error("Reset the failed pose before accumulating again"); Weight(weight);
    }
    void Published(unsigned node) const
    { Node(node); if (!ready || failed) throw std::logic_error("Pose matrices have not been published"); }
    void BuildBounds(const nlMatrix4& world, float scale) const
    {
        Require(std::isfinite(scale) && scale > 0 && scale <= 1e4,"Invalid bounded pose global scale");
        const double world_scale = World(world);
        std::vector<double> scale_bounds(matrices.size()), translation_bounds(matrices.size());
        for (unsigned i=0;i<matrices.size();++i)
        {
            const auto& s=pose->m_scale[i]; const auto& t=pose->m_trans[i];
            const double translation=t.bIdentity ? 0 : Maximum(t.t);
            if (i == 0)
            {
                scale_bounds[i] = (s.bIdentity && std::abs(scale-1.f) < .0001f ? 1 : Maximum(s.s)*scale)*world_scale;
                translation_bounds[i] = Maximum(world.GetTranslation()) + 3*translation*world_scale;
            }
            else
            {
                const auto parent=hierarchy->Data().GetParent(i);
                Require(parent >= 0 && unsigned(parent)<i,"Pose hierarchy parent no longer precedes its child");
                scale_bounds[i] = scale_bounds[parent]*(s.bIdentity ? 1 : Maximum(s.s));
                // Bound each quaternion-derived matrix row conservatively. The
                // qualified normalized quaternion/world inputs cannot approach
                // overflow; this guard covers deep multiplicative scale chains.
                translation_bounds[i] = translation_bounds[parent]+12*translation*scale_bounds[parent];
            }
            Require(std::isfinite(scale_bounds[i]) && scale_bounds[i] <= 1e20
                && std::isfinite(translation_bounds[i]) && translation_bounds[i] <= 1e30,
                "Pose ancestry can exceed bounded float matrix arithmetic");
        }
    }
    void Release()
    {
        Thread(); if (!pose) return; Check();
        pose.reset(); hierarchy.reset(); matrices.clear(); previous.clear(); quaternions.clear(); ready = failed = false;
    }
    ~Implementation() { try { Release(); } catch (...) { std::terminate(); } }
};
PoseAccumulator::PoseAccumulator(HierarchyAsset::Handle h, bool history) : impl_(std::make_unique<Implementation>(std::move(h),history)) {}
PoseAccumulator::~PoseAccumulator() = default;
void PoseAccumulator::Release() { impl_->Release(); }
unsigned PoseAccumulator::Nodes() const { impl_->Check(); return impl_->pose->GetNumNodes(); }
bool PoseAccumulator::HasPose() const { impl_->Check(); return impl_->ready && !impl_->failed; }
void PoseAccumulator::Reset()
{
    impl_->Check();
    if (impl_->failed) impl_->Construct();
    else { impl_->pose->InitAccumulators(); impl_->ready = false; }
}
void PoseAccumulator::BlendRotation(unsigned node, std::array<float,4> values, float weight, bool mirror)
{
    impl_->Mutable(node,weight);
    double square = 0; for (float value : values) { Finite(value,2,"Invalid pose quaternion"); square += double(value)*value; }
    Require(square >= .5 && square <= 1.5,"Pose quaternion must have bounded nonzero near-unit length");
    Sum(impl_->pose->m_rot[node].quatAccumulatedWeight,weight);
    const nlQuaternion q{values[0],values[1],values[2],values[3]}; impl_->pose->BlendRot(node,&q,weight,mirror);
}
void PoseAccumulator::BlendAngle(unsigned node, std::uint16_t angle, float weight)
{
    impl_->Mutable(node,weight); Sum(impl_->pose->m_rot[node].rotAroundZAccumulatedWeight,weight);
    impl_->pose->BlendRotAroundZ(node,angle,weight);
}
void PoseAccumulator::BlendScale(unsigned node, std::array<float,3> values, float weight, bool mirror)
{
    impl_->Mutable(node,weight); const auto v=Vector(values,16); Sum(impl_->pose->m_scale[node].fAccumulatedWeight,weight);
    impl_->pose->BlendScale(node,&v,weight,mirror); // Original bMirror is intentionally unused here.
}
void PoseAccumulator::BlendTranslation(unsigned node, std::array<float,3> values, float weight, bool mirror)
{
    impl_->Mutable(node,weight); const auto v=Vector(values,1e7f); Sum(impl_->pose->m_trans[node].fAccumulatedWeight,weight);
    impl_->pose->BlendTrans(node,&v,weight,mirror);
}
void PoseAccumulator::BlendRotationIdentity(unsigned node, float weight)
{
    impl_->Mutable(node,weight); Sum(impl_->pose->m_rot[node].quatAccumulatedWeight,weight); impl_->pose->BlendRotIdentity(node,weight);
}
void PoseAccumulator::BlendScaleIdentity(unsigned node, float weight)
{
    impl_->Mutable(node,weight); Sum(impl_->pose->m_scale[node].fAccumulatedWeight,weight); impl_->pose->BlendScaleIdentity(node,weight);
}
void PoseAccumulator::BlendTranslationIdentity(unsigned node, float weight)
{
    impl_->Mutable(node,weight); Sum(impl_->pose->m_trans[node].fAccumulatedWeight,weight); impl_->pose->BlendTransIdentity(node,weight);
}
void PoseAccumulator::MultiplyScale(unsigned node, std::array<float,3> values, float weight)
{
    impl_->Mutable(node,weight); const auto v=Vector(values,16);
    if (std::abs(weight) >= .001f)
    {
        const auto& old=impl_->pose->m_scale[node].s;
        for (unsigned i=0;i<3;++i)
        {
            const float factor=(1.f-weight)+weight*values[i];
            Finite(std::array{old.x,old.y,old.z}[i]*factor,1e7f,"Multiplied pose scale exceeds its bounded profile");
        }
    }
    impl_->pose->MultiplyScale(node,&v,weight);
}
void PoseAccumulator::Build(const nlMatrix4& world, float scale)
{
    impl_->Check(); if (impl_->failed) throw std::logic_error("Reset the failed pose before building again");
    impl_->BuildBounds(world,scale); // No original mutations before complete preflight.
    try
    {
        impl_->pose->SetScale(scale); impl_->pose->BuildNodeMatrices(world);
        for (unsigned i=0;i<impl_->matrices.size();++i)
        {
            const auto& matrix=impl_->pose->GetNodeMatrix(i); const auto& q=impl_->pose->GetNodeQuaternion(i);
            for (float value : matrix.e) Require(std::isfinite(value),"Original pose produced a nonfinite matrix");
            for (float value : {q.x,q.y,q.z,q.w}) Require(std::isfinite(value),"Original pose produced a nonfinite quaternion");
        }
        for (unsigned i=0;i<impl_->matrices.size();++i)
        {
            impl_->matrices[i]=impl_->pose->GetNodeMatrix(i);
            const auto& q=impl_->pose->GetNodeQuaternion(i); impl_->quaternions[i]={q.x,q.y,q.z,q.w};
            if (impl_->store_previous) impl_->previous[i]=impl_->pose->m_PrevNodeMatrices[i];
        }
        impl_->ready=true;
    }
    catch (...) { impl_->failed=true; throw; }
}
nlMatrix4 PoseAccumulator::Matrix(unsigned node) const { impl_->Published(node); return impl_->matrices[node]; }
nlMatrix4 PoseAccumulator::PreviousMatrix(unsigned node) const
{
    impl_->Published(node); if (!impl_->store_previous) throw std::logic_error("Pose previous matrices were not requested");
    return impl_->previous[node];
}
std::array<float,4> PoseAccumulator::Quaternion(unsigned node) const { impl_->Published(node); return impl_->quaternions[node]; }
nlMatrix4 PoseAccumulator::MatrixByHash(std::uint32_t id) const
{
    impl_->Check();
    for (unsigned i=0;i<impl_->matrices.size();++i)
        if (impl_->hierarchy->Data().GetNodeID(i)==id) { impl_->Published(i); return impl_->pose->GetNodeMatrixByHashID(id); }
    throw std::out_of_range("Pose node hash is absent from its retained hierarchy");
}
}

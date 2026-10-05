#pragma once
#include "resources/weighted_skin_model.h"
#include "runtime/hierarchy_assets.h"
#include "NL/nlMath.h"
#include <memory>
namespace mscharged
{
struct WeightedSkinPair { std::uint32_t vertexIndex=0; float vertexWeight=0; };
// Immutable source-prepared resources. Gathered pairs follow the original
// bone→vertex→lane traversal after SkinMoveLargestWeightFirst. No frame, pose,
// deformation or GL service is made ready by decoding these records.
class WeightedSkinAsset final
{
    struct Storage;
    std::unique_ptr<Storage> storage_;
    WeightedSkinAsset(resources::Bytes,HierarchyAsset::Handle,std::optional<std::uint32_t>);
public:
    using Handle=std::shared_ptr<const WeightedSkinAsset>;
    using PacketWeights=std::vector<std::vector<WeightedSkinPair>>;
    static Handle Decode(resources::Bytes,HierarchyAsset::Handle,std::optional<std::uint32_t> selected={});
    ~WeightedSkinAsset();
    const resources::WeightedSkinModel& Data() const;
    HierarchyAsset::Handle Hierarchy() const;
    const std::vector<nlMatrix4>& InverseBinds() const;
    const std::vector<std::vector<unsigned>>& NodeMaps() const;
    const std::vector<PacketWeights>& Weights() const;
    // Exact source flag: a nonzero second lane makes the whole model nonrigid.
    // This is not a promise that an arbitrary authored model can be drawn.
    bool OriginalRigidFlag() const;
};
}

#include "runtime/sanim_assets.h"
#include "Game/SAnim.h"
#include "Game/SAnimDecode.h"
#include <cmath>
#include <cstring>
#include <type_traits>

namespace mscharged
{
namespace
{
unsigned RotationWidth(unsigned properties) { return properties & 1 ? 2 : properties & 0x10 ? 8 : properties & 0x20 ? 6 : 4; }
void Time(float time)
{
    if (!std::isfinite(time) || time < 0 || time > 1)
        throw std::invalid_argument("SAnim sampling requires finite normalized time in [0,1]");
}
std::array<float, 3> Vector(const nlVector3& value) { return {value.x, value.y, value.z}; }
}
struct SAnimAsset::Storage : cSAnim
{
    struct Node
    {
        bool auxiliary_present = false;
        std::vector<std::uint8_t> rotation, weights, auxiliary;
        std::vector<std::uint16_t> angles;
        std::vector<PackedScale> scale;
        std::vector<PackedTrans> translation;
    };
    std::string name;
    std::vector<Node> nodes;
    std::vector<unsigned> properties, weight_counts, auxiliary_metadata, morph_counts;
    std::vector<unsigned long> morph_ids;
    std::vector<std::uint8_t> morph_keys;
    std::vector<unsigned short> root_rotation;
    std::vector<nlVector3> root_translation;
    std::vector<void*> rotations, auxiliary;
    std::vector<PackedScale*> scales;
    std::vector<PackedTrans*> translations;
    std::vector<unsigned char*> weights;
    explicit Storage(resources::SAnimation decoded) : name(std::move(decoded.name))
    {
        const auto count = decoded.nodes.size();
        nodes.resize(count); properties.resize(count); weight_counts.resize(count); auxiliary_metadata.resize(count);
        rotations.resize(count); scales.resize(count); translations.resize(count); weights.resize(count); auxiliary.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto& source = decoded.nodes[i]; auto& node = nodes[i];
            properties[i] = source.properties; auxiliary_metadata[i] = source.auxiliary_metadata;
            node.rotation = std::move(source.rotation); node.angles = std::move(source.angles);
            node.weights = std::move(source.weights); node.auxiliary = std::move(source.auxiliary);
            node.auxiliary_present = source.auxiliary_present;
            node.scale.reserve(source.scale.size()); node.translation.reserve(source.translation.size());
            for (const auto& key : source.scale) node.scale.push_back({key[0], key[1], key[2]});
            for (const auto& key : source.translation) node.translation.push_back({key[0], key[1], key[2]});
            rotations[i] = !node.angles.empty() ? static_cast<void*>(node.angles.data())
                : !node.rotation.empty() ? static_cast<void*>(node.rotation.data()) : nullptr;
            scales[i] = node.scale.empty() ? nullptr : node.scale.data();
            translations[i] = node.translation.empty() ? nullptr : node.translation.data();
            weights[i] = node.weights.empty() ? nullptr : node.weights.data();
            weight_counts[i] = node.weights.size(); auxiliary[i] = node.auxiliary.empty() ? nullptr : node.auxiliary.data();
        }
        root_rotation = std::move(decoded.root_rotation);
        root_translation.resize(decoded.root_translation.size());
        static_assert(sizeof(nlVector3) == sizeof(std::array<float, 3>) && std::is_trivially_copyable_v<nlVector3>);
        for (std::size_t i = 0; i < root_translation.size(); ++i)
            std::memcpy(&root_translation[i], decoded.root_translation[i].data(), sizeof(nlVector3));
        morph_counts = std::move(decoded.morph_counts); morph_keys = std::move(decoded.morph_keys);
        morph_ids.assign(decoded.morph_ids.begin(), decoded.morph_ids.end());
        m_szName = name.c_str(); m_uHashID = decoded.hash; m_nNumKeys = decoded.frames;
        m_nNumNodes = count; m_nNumMorphChannels = morph_counts.size(); m_nHierarchySignature = decoded.hierarchy_signature;
        m_pNodeProperties = properties.data(); m_Unknown18 = weight_counts.data(); m_Unknown1C = auxiliary_metadata.data();
        m_pRotKeys = rotations.data(); m_pScaleKeys = scales.data(); m_pTransKeys = translations.data();
        m_Unknown2C = weights.data(); m_Unknown30 = auxiliary.data(); m_nNumRootKeys = root_rotation.size();
        m_pRootRot = root_rotation.empty() ? nullptr : root_rotation.data();
        m_pRootTrans = root_translation.empty() ? nullptr : root_translation.data();
        m_nMorphIds = morph_ids.data(); m_pNumMorphKeys = morph_counts.data(); m_pMorphKeys = morph_keys.data();
        m_pCallbackList = nullptr;
        // Same initialization arithmetic as original Initialize, using original
        // root sampling and nlSqrt. Input bounds keep squared displacement finite.
        if (m_pRootTrans)
        {
            nlVector3 begin, end, delta; GetRootTrans(0, &begin); GetRootTrans(1, &end);
            nlVec3Sub(delta, end, begin);
            m_fLinearSpeed = nlSqrt(nlVec3LengthSquared(delta), true) / GetDuration();
        }
        else m_fLinearSpeed = 0;
    }
    const Node& NodeAt(std::size_t node) const
    { if (node >= nodes.size()) throw std::out_of_range("SAnim node index is out of bounds"); return nodes[node]; }
};
SAnimAsset::SAnimAsset(resources::SAnimation decoded) : storage_(std::make_unique<Storage>(std::move(decoded))) {}
SAnimAsset::Handle SAnimAsset::Decode(resources::Bytes file, std::size_t offset, std::size_t end)
{ return Handle(new SAnimAsset(resources::ReadSAnimation(file, offset, end))); }
SAnimAsset::~SAnimAsset() = default;
const cSAnim& SAnimAsset::Data() const { return *storage_; }
SAnimKeyCounts SAnimAsset::Keys(std::size_t index) const
{
    const auto& node = storage_->NodeAt(index);
    return {node.angles.empty() ? node.rotation.size() / RotationWidth(storage_->properties[index]) : node.angles.size(),
        node.scale.size(), node.translation.size(), node.weights.size()};
}
SAnimRotationKey SAnimAsset::RotationKey(std::size_t index, std::size_t key) const
{
    const auto& node = storage_->NodeAt(index);
    if (key >= Keys(index).rotation) throw std::out_of_range("SAnim rotation key index is out of bounds");
    if (!node.angles.empty()) return node.angles[key];
    const auto properties = storage_->properties[index];
    const auto* bytes = node.rotation.data() + key * RotationWidth(properties);
    nlQuaternion q;
    if (properties & 0x10) SAnimDecodeRot16(&q, bytes);
    else if (properties & 0x20) SAnimDecodeRot12(&q, bytes);
    else SAnimDecodeRot8(&q, bytes);
    return std::array<float, 4>{q.x, q.y, q.z, q.w};
}
std::array<float, 3> SAnimAsset::ScaleKey(std::size_t node, std::size_t key) const
{
    const auto& keys = storage_->NodeAt(node).scale;
    if (key >= keys.size()) throw std::out_of_range("SAnim scale key index is out of bounds");
    nlVector3 value; SAnimDecodeScale(&value, &keys[key]); return Vector(value);
}
std::array<float, 3> SAnimAsset::TranslationKey(std::size_t node, std::size_t key) const
{
    const auto& keys = storage_->NodeAt(node).translation;
    if (key >= keys.size()) throw std::out_of_range("SAnim translation key index is out of bounds");
    const auto& value = keys[key]; return {value.x, value.y, value.z};
}
bool SAnimAsset::HasAuxiliary(std::size_t node) const { return storage_->NodeAt(node).auxiliary_present; }
resources::Bytes SAnimAsset::AuxiliaryBytes(std::size_t node) const { return storage_->NodeAt(node).auxiliary; }
SAnimRootSample SAnimAsset::Root(float time) const
{
    Time(time); unsigned short rotation; nlVector3 translation;
    storage_->GetRootRot(time, &rotation); storage_->GetRootTrans(time, &translation);
    return {rotation, Vector(translation)};
}
SAnimWeightSample SAnimAsset::Weight(std::size_t node, float time) const
{
    Time(time); storage_->NodeAt(node); float value;
    const bool authored = storage_->fn_8030939C(int(node), time, &value); return {authored, value};
}
float SAnimAsset::MorphWeight(std::size_t channel, float time) const
{
    Time(time);
    if (channel >= storage_->morph_counts.size()) throw std::out_of_range("SAnim morph channel is out of bounds");
    for (unsigned count : storage_->morph_counts)
        if (count != storage_->morph_counts.front())
            throw resources::UnsupportedResource("Original unequal-count SAnim morph indexing is not qualified");
    return storage_->GetMorphWeight(int(channel), time);
}
SAnimAssets::SAnimAssets(resources::Bytes file)
{
    auto decoded = resources::ReadSAnimations(file); assets_.reserve(decoded.size());
    for (auto& animation : decoded) assets_.push_back(SAnimAsset::Handle(new SAnimAsset(std::move(animation))));
}
SAnimAsset::Handle SAnimAssets::At(std::size_t index) const
{ if (index >= assets_.size()) throw std::out_of_range("SAnim asset index is out of bounds"); return assets_[index]; }
}

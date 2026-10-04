#pragma once
#include "resources/sanim.h"
#include <memory>
#include <variant>

class cSAnim;
namespace mscharged
{
struct SAnimKeyCounts { std::size_t rotation, scale, translation, weights; };
using SAnimRotationKey = std::variant<std::uint16_t, std::array<float, 4>>;
struct SAnimRootSample { std::uint16_t rotation; std::array<float, 3> translation; };
struct SAnimWeightSample { bool authored; float value; };
class SAnimAssets;
// Stable native records backed by retained host storage. No original callbacks,
// pose accumulator or arena lifetime is acquired. Keep a handle while borrowing
// Data(); treat its original pointer fields as read-only and do not pass it to
// an inventory free/in-place initialization path.
class SAnimAsset
{
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit SAnimAsset(resources::SAnimation decoded);
    friend class SAnimAssets;
public:
    using Handle = std::shared_ptr<const SAnimAsset>;
    static Handle Decode(resources::Bytes file, std::size_t offset, std::size_t end);
    ~SAnimAsset();
    SAnimAsset(const SAnimAsset&) = delete;
    SAnimAsset& operator=(const SAnimAsset&) = delete;
    const cSAnim& Data() const;
    SAnimKeyCounts Keys(std::size_t node) const;
    SAnimRotationKey RotationKey(std::size_t node, std::size_t key) const;
    std::array<float, 3> ScaleKey(std::size_t node, std::size_t key) const;
    std::array<float, 3> TranslationKey(std::size_t node, std::size_t key) const;
    // Retained, uninterpreted 0x17115 payload. Empty-present stays distinct
    // here; its native unknown-channel pointer is null because no record exists.
    bool HasAuxiliary(std::size_t node) const;
    resources::Bytes AuxiliaryBytes(std::size_t node) const;
    SAnimRootSample Root(float normalized_time) const;
    SAnimWeightSample Weight(std::size_t node, float normalized_time) const;
    // Original unequal-count morph indexing remains unqualified and throws.
    float MorphWeight(std::size_t channel, float normalized_time) const;
};
class SAnimAssets
{
    std::vector<SAnimAsset::Handle> assets_;
public:
    explicit SAnimAssets(resources::Bytes file);
    std::size_t Size() const { return assets_.size(); }
    SAnimAsset::Handle At(std::size_t index) const;
};
}

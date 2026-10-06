#if !defined(MSCHARGED_DIAGNOSTIC_SKELETON) || defined(MSCHARGED_GAME_MODULE)
#error Retained decoded skeleton owners belong only to explicit diagnostics
#endif

#include "runtime/hierarchy_assets.h"
#include "resources/hierarchy.h"
#include "Game/SHierarchy.h"
#include <cstring>
#include <type_traits>
#include <vector>

namespace mscharged
{
struct HierarchyAsset::Storage : cSHierarchy
{
    std::string name;
    std::vector<u32> ids;
    std::vector<int> parents, counts, children, push_pop, mirrors;
    std::vector<int*> child_pointers;
    std::vector<nlVector3> translations;
    std::vector<u8> preserve;
    unsigned depth;
    explicit Storage(resources::HierarchyData decoded) : name(std::move(decoded.name)), depth(decoded.maximum_depth)
    {
        const auto count = decoded.nodes.size();
        ids.resize(count); parents.resize(count); counts.resize(count);
        push_pop.resize(count); mirrors.resize(count); child_pointers.resize(count);
        translations.resize(count); preserve.resize(count); children.reserve(count - 1);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& node = decoded.nodes[i];
            ids[i] = node.id; parents[i] = node.parent; counts[i] = int(node.children.size());
            mirrors[i] = node.mirror; preserve[i] = node.preserve_bone_length;
            static_assert(sizeof(nlVector3) == sizeof(node.translation) && std::is_trivially_copyable_v<nlVector3>);
            std::memcpy(&translations[i], node.translation.data(), sizeof(nlVector3));
            children.insert(children.end(), node.children.begin(), node.children.end());
        }
        std::size_t offset = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            child_pointers[i] = counts[i] ? children.data() + offset : nullptr;
            offset += counts[i];
        }
        // Publish only after all allocations finish. Storage never moves and
        // no vector is resized once its original pointer fields are assigned.
        m_szName = name.c_str(); m_uHashID = decoded.hash; m_nNumNodes = int(count);
        m_pNodeID = ids.data(); m_pParent = parents.data(); m_pNumChildren = counts.data();
        m_pChildren = child_pointers.data(); m_pPushPop = push_pop.data(); m_pMirrorTable = mirrors.data();
        m_nPelvisNodeIndex = decoded.pelvis; m_nSpineNodeIndex = decoded.spine;
        m_pV3TranslationOffset = translations.data(); m_pPreserveBoneLength = preserve.data();
        int current_depth = 0;
        BuildPushPopFlags(0, 0, current_depth);
    }
};
HierarchyAsset::HierarchyAsset(resources::Bytes file, std::size_t offset, std::size_t end)
    : storage_(std::make_unique<Storage>(resources::ReadHierarchy(file, offset, end))) {}
HierarchyAsset::Handle HierarchyAsset::Decode(resources::Bytes file)
{ return Decode(file, 0, file.size()); }
HierarchyAsset::Handle HierarchyAsset::Decode(resources::Bytes file, std::size_t offset, std::size_t end)
{ return Handle(new HierarchyAsset(file, offset, end)); }
HierarchyAsset::~HierarchyAsset() = default;
const cSHierarchy& HierarchyAsset::Data() const { return *storage_; }
unsigned HierarchyAsset::MaximumDepth() const { return storage_->depth; }
}

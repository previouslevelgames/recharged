#pragma once
#include "resources/binary_reader.h"
#include <memory>

class cSHierarchy;
namespace mscharged
{
// Native records use host-owned storage and have no game-arena dependency.
// Retain this handle for as long as an original consumer borrows Data(). Never
// pass the borrowed object to an in-place loader or inventory free operation.
class HierarchyAsset
{
    struct Storage;
    std::unique_ptr<Storage> storage_;
    HierarchyAsset(resources::Bytes file, std::size_t offset, std::size_t end);
public:
    using Handle = std::shared_ptr<const HierarchyAsset>;
    static Handle Decode(resources::Bytes file);
    static Handle Decode(resources::Bytes file, std::size_t offset, std::size_t end);
    ~HierarchyAsset();
    HierarchyAsset(const HierarchyAsset&) = delete;
    HierarchyAsset& operator=(const HierarchyAsset&) = delete;
    const cSHierarchy& Data() const;
    unsigned MaximumDepth() const;
};
}

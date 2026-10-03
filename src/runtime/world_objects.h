#pragma once
#include "resources/world_objects.h"
#include "resources/static_model.h"
#include "resources/texture_bundle.h"
#include <memory>

struct glModel;
class GLResourcePool;
namespace mscharged
{
class StaticInventory;
struct WorldObjectMemory
{
    std::size_t headers; // MEM1: headers, indices, instance matrices, cloned packets.
    std::size_t geometry; // MEM2: shared vertex streams and textures.
};
struct NativeWorldObject
{
    resources::StaticWorldObject record;
    glModel* model = nullptr;
};

// Owns a selected static batch, including its source geometry/textures. Each
// instance uses original glModelDupNoStreams semantics: independent packets and
// material parameters, shared immutable geometry, and its own persistent matrix.
// The caller keeps MaterialPrograms/graphics memory alive and drains submitted
// views/GPU work before release (or supplies that operation as before_release).
// All returned pointers/spans expire at Release. No factory, visibility, camera,
// animation tick, view submission or gameplay behavior is supplied here.
class StaticWorldObjects
{
    GLResourcePool* pool_ = nullptr;
    std::unique_ptr<StaticInventory> inventory_;
    NativeWorldObject* objects_ = nullptr;
    std::size_t size_ = 0;
    void (*before_release_)() = nullptr;
    bool releasing_ = false;
public:
    StaticWorldObjects(const std::vector<resources::StaticWorldObject>& objects,
        const std::vector<resources::StaticModel>& models, const resources::TextureBundle& textures,
        WorldObjectMemory memory, void (*before_release)() = nullptr);
    ~StaticWorldObjects();
    StaticWorldObjects(const StaticWorldObjects&) = delete;
    StaticWorldObjects& operator=(const StaticWorldObjects&) = delete;
    std::span<const NativeWorldObject> Objects() const { return {objects_, size_}; }
    const NativeWorldObject* Find(std::uint32_t id) const;
    // Borrow only: select this pool when resolving this batch's material IDs.
    // Restore the caller's current pool before releasing this owner.
    GLResourcePool& Pool() const;
    void Release();
};
}

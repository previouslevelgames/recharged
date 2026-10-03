#include "runtime/world_objects.h"
#include "runtime/static_inventory.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glView.h"
#include "NL/gl/glTextureManager.h"
#include "NL/nlMath.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <type_traits>

namespace mscharged
{
namespace
{
class PoolSelection
{
    GLResourcePool* previous_ = glGetCurrentResourcePool();
public:
    explicit PoolSelection(GLResourcePool* pool) { glSetCurrentResourcePool(pool); }
    ~PoolSelection() { glSetCurrentResourcePool(previous_); }
};
}
StaticWorldObjects::StaticWorldObjects(const std::vector<resources::StaticWorldObject>& objects,
    const std::vector<resources::StaticModel>& models, const resources::TextureBundle& textures,
    WorldObjectMemory memory, void (*before_release)())
{
    using resources::Require;
    if (!glGetTextureManager()) throw std::logic_error("Initialize graphics memory before world objects");
    constexpr std::size_t maximum = 4 * resources::MaximumAssetBytes;
    Require(memory.headers && memory.geometry && memory.headers <= maximum
            && memory.geometry <= maximum - memory.headers, "Invalid static world memory budget");
    Require(!objects.empty() && objects.size() <= resources::MaximumWorldObjects, "Invalid static world instance count");
    std::set<std::uint32_t> ids, model_ids;
    for (const auto& model : models)
        Require(model_ids.insert(model.id).second, "Duplicate static world model ID");
    for (const auto& object : objects)
    {
        resources::ValidateStaticWorldObject(object);
        Require(ids.insert(object.id).second, "Duplicate static world instance ID");
        Require(model_ids.contains(object.model), "Static world instance model is absent from its batch");
    }
    const GLMemoryRequirement requirements[] = {
        {GLM_Header, static_cast<unsigned long>(memory.headers)},
        {GLM_VertexData, static_cast<unsigned long>(memory.geometry)}};
    try
    {
        pool_ = glCreateResourcePool(requirements, 2, "Static world objects");
        PoolSelection current(pool_);
        inventory_ = std::make_unique<StaticInventory>(*pool_, models, textures.textures, nullptr, textures.animations);
        static_assert(std::is_trivially_destructible_v<NativeWorldObject>);
        objects_ = static_cast<NativeWorldObject*>(pool_->Allocate(objects.size() * sizeof(NativeWorldObject), GLM_Header));
        for (const auto& object : objects)
        {
            auto& instance = *new (objects_ + size_) NativeWorldObject{object};
            auto* source = inventory_->Model(object.model);
            Require(source, "Static world model resolution failed");
            instance.model = glModelDupNoStreams(source, true, pool_);
            auto* matrix = new (pool_->Allocate(sizeof(nlMatrix4), GLM_Matrix)) nlMatrix4;
            static_assert(sizeof(nlMatrix4) == sizeof(object.transform));
            std::memcpy(matrix->e, object.transform.data(), sizeof(*matrix));
            glModelSetMatrix(instance.model, reinterpret_cast<glMatrixHandle>(matrix));
            ++size_;
        }
        before_release_ = before_release;
    }
    catch (...)
    {
        // Nothing has been submitted, so a failing constructor needs no drain.
        inventory_.reset();
        if (pool_) glDestroyResourcePool(pool_);
        pool_ = nullptr; objects_ = nullptr; size_ = 0;
        throw;
    }
}

StaticWorldObjects::~StaticWorldObjects() { Release(); }
const NativeWorldObject* StaticWorldObjects::Find(std::uint32_t id) const
{
    for (const auto& object : Objects()) if (object.record.id == id) return &object;
    return nullptr;
}
GLResourcePool& StaticWorldObjects::Pool() const
{
    if (!pool_) throw std::logic_error("Static world objects have been released");
    return *pool_;
}
void StaticWorldObjects::Release()
{
    if (!pool_) return;
    if (releasing_ || glNativeViewDispatchActive())
        throw std::logic_error("Cannot release static world objects during release or view dispatch");
    releasing_ = true;
    try { if (before_release_) before_release_(); }
    catch (...) { releasing_ = false; throw; }
    // The one owned batch keeps geometry alive until all instance references
    // are withdrawn. StaticInventory releases animation aliases before textures.
    objects_ = nullptr; size_ = 0;
    inventory_.reset();
    glDestroyResourcePool(pool_); pool_ = nullptr;
    releasing_ = false;
}
}

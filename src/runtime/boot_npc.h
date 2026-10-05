#pragma once
#include "runtime/boot_loading.h"
#include "runtime/hierarchy_assets.h"
#include "runtime/sanim_assets.h"
#include "runtime/skin_pose.h"
#include <string>

struct glModel;
namespace mscharged
{
enum class BootNpcState { Idle, Selected, Loading, Registered, Failed, Released };
struct BootNpcTemplate
{
    using Handle=std::shared_ptr<const BootNpcTemplate>;
    std::string name;
    bool persistent=false,animation_admitted=false;
    HierarchyAsset::Handle hierarchy;
    std::vector<SAnimAsset::Handle> animations;
    RigidSkinAsset::Handle skin;
    std::vector<std::uint32_t> textures;
};
// Source template loading only: no NPCManager singleton, actors, physics or
// raw Wii skin-chunk inventory. A typed retained skin asset is supplied to later
// qualified pose/render consumers. An unsupported material fails with its real
// template name/hash; earlier published templates remain retained until release.
// NL/GL/frame providers and each captured pool must outlive this binding.
class BootNpcResources
{
    struct Implementation;
    std::shared_ptr<Implementation> impl_;
    std::weak_ptr<BootNpcBinding> binding_;
public:
    BootNpcResources();
    ~BootNpcResources();
    BootNpcResources(const BootNpcResources&)=delete;
    BootNpcResources& operator=(const BootNpcResources&)=delete;
    BootNpcBinding::Handle Binding();
    BootNpcState State() const;
    std::size_t Created() const;
    std::size_t Loaded() const;
    std::string PendingName() const;
    BootNpcTemplate::Handle Find(std::string_view name) const;
    const glModel* Model(std::string_view name) const;
};
}

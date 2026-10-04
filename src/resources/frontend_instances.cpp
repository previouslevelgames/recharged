#include "resources/frontend_instances.h"
#include "Game/FE/FrontendInstanceSteps.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace mscharged::resources
{
std::uint32_t FrontendLowerHash(std::string_view name)
{
    Require(name.size()<=4096&&name.find('\0')==std::string_view::npos,"Invalid frontend lookup name");
    std::uint32_t hash=0xffffffff;
    for(unsigned char c:name){if(c>='A'&&c<='Z')c+='a'-'A';hash=hash*33+c;}
    return hash;
}
FrontendPath FrontendNamedPath(std::span<const std::string_view> names)
{
    Require(!names.empty()&&names.size()<=6,"Frontend lookup requires one to six levels");
    FrontendPath path{};for(std::size_t i=0;i<names.size();++i)path[i]=FrontendLowerHash(names[i]);return path;
}
namespace
{
struct Access
{
    using Node=std::optional<FrontendNode>;
    const FrontendScene& scene;
    std::map<std::uint32_t,const FrontendSlide*> slides;
    std::map<std::uint32_t,const FrontendInstance*> instances;
    std::map<std::uint32_t,const FrontendLibraryObject*> library;
    explicit Access(const FrontendScene& s):scene(s)
    {
        Require(s.slides.size()<=16384&&s.instances.size()<=16384&&s.library.size()<=16384,"Frontend finder exceeds its graph budget");
        std::set<std::uint32_t> ids;
        const auto add=[&](const auto& list,auto& map){for(const auto& item:list)
        {Require(ids.insert(item.offset).second,"Duplicate frontend finder record ID");map.emplace(item.offset,&item);}};
        add(s.slides,slides);add(s.instances,instances);add(s.library,library);
    }
    const FrontendInstance& Instance(Node n) const
    {
        Require(n&&n->kind==FrontendNodeKind::Instance&&instances.contains(n->id),"Frontend finder instance is absent");
        const auto& item=*instances.at(n->id);Require(item.type>=1&&item.type<=5,"Frontend finder instance type is invalid");return item;
    }
    const FrontendSlide& Slide(Node n) const
    {Require(n&&n->kind==FrontendNodeKind::Slide&&slides.contains(n->id),"Frontend finder slide is absent");return *slides.at(n->id);}
    const FrontendLibraryObject& Component(Node n) const
    {
        const auto& instance=Instance(n);Require(instance.type==4&&instance.library&&library.contains(*instance.library),"Frontend finder component is absent");
        const auto& object=*library.at(*instance.library);Require(object.type==3,"Frontend component library type differs");return object;
    }
    template<class Map> Node Find(const std::vector<std::uint32_t>& ring,std::uint32_t hash,const Map& records,FrontendNodeKind kind) const
    {
        Require(ring.size()<=16384,"Frontend finder ring exceeds its budget");
        for(auto id:ring){Require(records.contains(id),"Frontend finder ring references an absent record");if(records.at(id)->hash==hash)return FrontendNode{kind,id};}
        return {};
    }
    Node Child(Node parent,unsigned long hash) const
    {
        const auto& ring=parent->kind==FrontendNodeKind::Slide?Slide(parent).children:Instance(parent).children;
        return Find(ring,hash,instances,FrontendNodeKind::Instance);
    }
    bool IsComponent(Node n) const {return Instance(n).type==4;}
    Node ComponentSlide(Node n,unsigned long hash) const {return Find(Component(n).slides,hash,slides,FrontendNodeKind::Slide);}
    Node ActiveComponentSlide(Node n) const
    {
        const auto& c=Component(n);if(!c.active_slide)return {};
        Require(std::find(c.slides.begin(),c.slides.end(),*c.active_slide)!=c.slides.end(),"Frontend active component slide is outside its ring");
        Node node=FrontendNode{FrontendNodeKind::Slide,*c.active_slide};(void)Slide(node);return node;
    }
    Node PresentationSlide(unsigned long hash) const {return Find(scene.presentation_slides,hash,slides,FrontendNodeKind::Slide);}
    Node ActivePresentationSlide() const
    {
        if(!scene.active_slide)return {};
        Require(std::find(scene.presentation_slides.begin(),scene.presentation_slides.end(),*scene.active_slide)!=scene.presentation_slides.end(),"Frontend active presentation slide is outside its ring");
        Node node=FrontendNode{FrontendNodeKind::Slide,*scene.active_slide};(void)Slide(node);return node;
    }
};
void Finite(float v){Require(std::isfinite(v)&&std::abs(v)<=1e7f,"Frontend instance scalar exceeds its finite profile");}
struct AttributeAdapter
{
    std::uint32_t m_overloadFlags;
    FELibObjectAttributes m_overloadedAttributes{};
    explicit AttributeAdapter(const FrontendInstance& instance):m_overloadFlags(instance.overload_flags)
    {
        const auto& a=instance.attributes;auto& v=m_overloadedAttributes;
        std::copy(a.position.begin(),a.position.end(),v.v3Position.e);std::copy(a.rotation.begin(),a.rotation.end(),v.v3Rotation.e);
        std::copy(a.scale.begin(),a.scale.end(),v.v3Scale.e);std::copy(a.pivot.begin(),a.pivot.end(),v.v3Pivot.e);
        std::copy(a.colour.begin(),a.colour.end(),v.colour.c);v.bVisible=a.visible;
        v.fUVX=a.uv[0];v.fUVY=a.uv[1];v.fUVWidth=a.uv[2];v.fUVHeight=a.uv[3];
    }
    void Store(FrontendInstance& instance) const
    {
        instance.overload_flags=m_overloadFlags;auto& a=instance.attributes;const auto& v=m_overloadedAttributes;
        std::copy_n(v.v3Position.e,3,a.position.begin());std::copy_n(v.v3Rotation.e,3,a.rotation.begin());
        std::copy_n(v.v3Scale.e,3,a.scale.begin());std::copy_n(v.v3Pivot.e,3,a.pivot.begin());std::copy_n(v.colour.c,4,a.colour.begin());
        a.visible=v.bVisible;a.uv={v.fUVX,v.fUVY,v.fUVWidth,v.fUVHeight};
    }
};
}
std::optional<FrontendNode> FindFrontendNode(const FrontendScene& scene,FrontendNode root,const FrontendPath& path,FrontendNodeType expected)
{
    Require(int(expected)>=-1&&int(expected)<=5,"Unknown frontend requested type");
    Access access(scene);std::array<unsigned long,7> levels{};std::copy(path.begin(),path.end(),levels.begin());
    Access::Node result;
    switch(root.kind)
    {
    case FrontendNodeKind::Presentation:
        Require(root.id==0,"Presentation lookup does not take a record ID");result=FrontendFindPresentation(access,levels.data());break;
    case FrontendNodeKind::Slide:
        (void)access.Slide(root);result=FrontendFindSlideChildren(access,Access::Node(root),levels.data());break;
    case FrontendNodeKind::Instance:
        (void)access.Instance(root);result=FrontendFindRecursive(access,Access::Node(root),levels.data());break;
    default:throw std::invalid_argument("Unknown frontend lookup root");
    }
    if(result)
    {
        const int type=result->kind==FrontendNodeKind::Slide?0:int(access.Instance(result).type);
        Require(expected==FrontendNodeType::Any||int(expected)==type,"Frontend lookup found a different node type");
    }
    return result;
}
void ApplyFrontendInstanceChanges(FrontendScene& scene,std::span<const FrontendInstanceChange> changes)
{
    Require(changes.size()<=256,"Frontend instance change budget exceeded");
    Access access(scene);std::map<std::uint32_t,std::size_t> positions;
    for(std::size_t i=0;i<scene.instances.size();++i)positions.emplace(scene.instances[i].offset,i);
    auto next=scene;
    for(const auto& change:changes)
    {
        (void)access.Instance(FrontendNode{FrontendNodeKind::Instance,change.instance});
        auto& target=next.instances.at(positions.at(change.instance));AttributeAdapter attrs(target);
        const auto type=[&](unsigned expected){Require(target.type==expected,"Frontend setter requires a different instance type");};
        const auto vector=[&]{for(float v:change.vector)Finite(v);};
        switch(change.property)
        {
        case FrontendInstanceProperty::Visible:FrontendSetVisible(target.visible,change.flag);break;
        case FrontendInstanceProperty::AssetVisible:FrontendSetAssetVisible(attrs,change.flag);break;
        case FrontendInstanceProperty::Position:vector();FrontendSetAssetPosition(attrs,change.vector[0],change.vector[1],change.vector[2]);break;
        case FrontendInstanceProperty::Rotation:vector();FrontendSetAssetRotation(attrs,change.vector[0],change.vector[1],change.vector[2]);break;
        case FrontendInstanceProperty::Scale:vector();FrontendSetAssetScale(attrs,change.vector[0],change.vector[1],change.vector[2]);break;
        case FrontendInstanceProperty::Pivot:vector();FrontendSetAssetPivot(attrs,change.vector[0],change.vector[1],change.vector[2]);break;
        case FrontendInstanceProperty::Colour:
        {nlColour c;std::copy(change.colour.begin(),change.colour.end(),c.c);FrontendSetAssetColour(attrs,c);break;}
        case FrontendInstanceProperty::UVX:Finite(change.scalar);FrontendSetAssetUVX(attrs,change.scalar);break;
        case FrontendInstanceProperty::UVY:Finite(change.scalar);FrontendSetAssetUVY(attrs,change.scalar);break;
        case FrontendInstanceProperty::UVWidth:Finite(change.scalar);FrontendSetAssetUVWidth(attrs,change.scalar);break;
        case FrontendInstanceProperty::UVHeight:Finite(change.scalar);FrontendSetAssetUVHeight(attrs,change.scalar);break;
        case FrontendInstanceProperty::StringId:
        {
            type(3);auto name=std::string_view(change.string_id);if(name.starts_with("LOC_"))name.remove_prefix(4);
            target.localization_hash=FrontendLowerHash(name);FrontendSetStringIdFlags(target.text_overload_flags);break;
        }
        case FrontendInstanceProperty::String:
            type(3);Require(change.text.size()<=4096&&change.text.find(u'\0')==std::u16string::npos,"Invalid frontend user string");
            target.text=change.text;FrontendSetUserStringFlags(target.text_overload_flags);break;
        case FrontendInstanceProperty::ImageResource:
            type(2);if(change.image_resource)
            {
                const auto found=std::find_if(next.resources.begin(),next.resources.end(),[&](const auto& r){return r.offset==*change.image_resource;});
                Require(found!=next.resources.end()&&found->type==0,"Frontend image setter requires a texture resource");
            }
            FrontendSetImageResource(target.resource,change.image_resource);break;
        default:throw std::invalid_argument("Unknown frontend instance property");
        }
        attrs.Store(target);
    }
    scene=std::move(next);
}
}

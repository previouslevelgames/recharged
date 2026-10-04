#include "resources/frontend_scene.h"
#include <algorithm>
#include <map>
#include <functional>
#include <set>
namespace mscharged::resources
{
namespace
{
constexpr std::size_t MaximumNodes = 16384;
class Reader
{
    Bytes data_;
    std::set<std::uint32_t> relocations_;
    enum class Kind { Resource, Library, Instance, Slide, Presentation };
    struct Node { Kind kind; std::size_t size; bool busy = true; };
    std::map<std::uint32_t, Node> nodes_;
    FrontendScene result_{};
    std::size_t text_units_ = 0;
    std::vector<std::pair<std::uint32_t, std::size_t>> strings_;
    Bytes Value(std::size_t at, std::size_t size) const
    {
        const auto bytes = Slice(data_, at, size);
        const auto relocation = relocations_.lower_bound(static_cast<std::uint32_t>(at > 3 ? at - 3 : 0));
        Require(relocation == relocations_.end() || *relocation >= at + size,
            "FEN scalar/string overlaps a relocated pointer");
        return bytes;
    }
    std::uint32_t Word(std::size_t at) const { Value(at,4); return U32(data_,at); }
    float Number(std::size_t at) const { Value(at,4); return F32(data_,at); }
    FrontendReference Pointer(std::size_t field) const
    {
        const auto value = U32(data_, field);
        if (!relocations_.contains(static_cast<std::uint32_t>(field)))
        { Require(value == 0, "FEN pointer field is missing its relocation"); return {}; }
        if (value == UINT32_MAX) return {};
        return value;
    }
    std::uint32_t Present(FrontendReference value) const
    { Require(value.has_value(), "FEN graph has a null required reference"); return *value; }
    bool Bool(std::size_t at) const
    { const auto value = Value(at,1)[0]; Require(value <= 1,"Invalid FEN boolean"); return value != 0; }
    std::string Name(std::size_t at) const
    {
        const auto bytes = Value(at,32); const auto end = std::find(bytes.begin(),bytes.end(),0);
        Require(end != bytes.end(),"Unterminated FEN name"); return {bytes.begin(),end};
    }
    std::u16string Text(FrontendReference at)
    {
        std::u16string value;
        if (!at) return value;
        Require(*at % 2 == 0,"Unaligned FEN UTF-16 string");
        for (std::size_t i=*at;;i+=2)
        {
            Value(i,2);const auto unit=U16(data_,i);
            if (!unit)
            {
                Require(value.size() <= MaximumAssetBytes/2 - text_units_, "FEN decoded text exceeds its budget");
                text_units_ += value.size(); strings_.emplace_back(*at, i+2-*at);
                return value;
            }
            Require(value.size()<8192,"FEN text exceeds its string budget");
            Require(unit < 0xdc00 || unit > 0xdfff,"Unpaired FEN low surrogate");value.push_back(unit);
            if (unit >= 0xd800 && unit <= 0xdbff)
            {
                Require(value.size()<8192,"FEN text exceeds its string budget");
                Value(i+2,2);const auto low=U16(data_,i+2);Require(low>=0xdc00&&low<=0xdfff,"Unpaired FEN high surrogate");
                value.push_back(low);i+=2;
            }
        }
    }
    FrontendAttributes Attributes(std::size_t at) const
    {
        FrontendAttributes a;
        for (unsigned i=0;i<3;++i)
        { a.position[i]=Number(at+4*i);a.rotation[i]=Number(at+12+4*i);a.scale[i]=Number(at+24+4*i);a.pivot[i]=Number(at+36+4*i); }
        a.visible=Bool(at+48);std::copy_n(Value(at+49,4).begin(),4,a.colour.begin());
        for(unsigned i=0;i<4;++i)a.uv[i]=Number(at+56+4*i);
        return a;
    }
    bool Begin(std::uint32_t at, std::size_t size, Kind kind, unsigned depth)
    {
        Require(depth<=64,"FEN graph exceeds its nesting limit");
        Require(at%4==0,"Unaligned FEN object");Slice(data_,at,size);
        const auto found=nodes_.find(at);
        if(found!=nodes_.end())
        {
            Require(found->second.kind==kind&&!found->second.busy,"Cyclic or conflicting FEN object references");
            throw std::runtime_error("Aliased FEN structural record has multiple owners");
        }
        Require(nodes_.size()<MaximumNodes,"FEN graph exceeds its node budget");
        auto next=nodes_.lower_bound(at);
        if(next!=nodes_.end())Require(at+size<=next->first,"Overlapping FEN records");
        if(next!=nodes_.begin()){const auto prev=std::prev(next);Require(prev->first+prev->second.size<=at,"Overlapping FEN records");}
        nodes_.emplace(at,Node{kind,size});return true;
    }
    void End(std::uint32_t at) { nodes_.at(at).busy=false; }
    std::vector<std::uint32_t> Ring(FrontendReference tail) const
    {
        std::vector<std::uint32_t> values;if(!tail)return values;
        const auto first=Present(Pointer(*tail));auto current=first;std::set<std::uint32_t> seen;
        for(;;)
        {
            Require(values.size()<MaximumNodes&&seen.insert(current).second,"Malformed or excessive FEN ring");
            Require(current%4==0,"Unaligned FEN ring node");Slice(data_,current,8);
            const auto next=Present(Pointer(current)),prev=Present(Pointer(current+4));
            Require(Pointer(next+4)==current&&Pointer(prev)==current,"Broken FEN ring backlink");
            values.push_back(current);
            if(current==*tail){Require(next==first,"FEN tail does not close its ring");break;}
            current=next;
        }
        return values;
    }
    void Resource(std::uint32_t at)
    {
        const auto type=Word(at+8);Require(type<=2,"Unknown FEN resource type");
        if(!Begin(at,type==1?0x1c:0x20,Kind::Resource,0))return;
        result_.resources.push_back({at,type,Word(at+12),Word(at+20),Bool(at+16)});End(at);
    }
    void Instance(std::uint32_t at,unsigned depth)
    {
        const auto type=Word(at+0x88);Require(type>=1&&type<=5,"Unknown FEN instance type");
        if(!Begin(at,type==3?0x114:type==2?0x98:0x90,Kind::Instance,depth))return;
        FrontendInstance value{};value.offset=at;value.type=type;value.hash=Word(at+0x38);value.name=Name(at+0x18);
        value.start=Number(at+0x10);value.duration=Number(at+0x14);Require(value.duration>=0,"Negative FEN instance duration");
        value.attributes=Attributes(at+0x3c);value.overload_flags=Word(at+0x84);value.priority=(Value(at+0x8c,2), U16(data_,at+0x8c));value.visible=Bool(at+0x8e);
        value.library=Pointer(at+12);Require(value.library.has_value(),"FEN instance has no library object");
        value.children=Ring(Pointer(at+8));for(auto child:value.children)Instance(child,depth+1);
        if(type==2)value.resource=Pointer(at+0x90);
        if(type==3)
        {
            value.localization_hash=Word(at+0x90);std::copy_n(Value(at+0x94,4).begin(),4,value.text_effect_colour.begin());
            value.text_box={Number(at+0x98),Number(at+0x9c)};value.text_overload_flags=Word(at+0xa0);value.draw_options=Word(at+0x100);
            value.text=Text(Pointer(at+0x104));
            value.text_scissor=Bool(at+0x108);
            for(unsigned i=0;i<4;++i)
            {Value(at+0x10a+2*i,2);value.text_scissor_box[i]=U16(data_,at+0x10a+2*i);}
        }
        result_.instances.push_back(std::move(value));End(at);
    }
    void Slide(std::uint32_t at,unsigned depth)
    {
        if(!Begin(at,0x48,Kind::Slide,depth))return;
        FrontendSlide value{};value.offset=at;value.hash=Word(at+0x40);value.name=Name(at+0x20);
        value.start=Number(at+0x10);value.duration=Number(at+0x14);value.time=Number(at+0x18);
        Require(value.duration>=0,"Negative FEN slide duration");value.play_mode=Word(at+0x1c);Require(value.play_mode<=2,"Unknown FEN slide play mode");
        value.frozen=Bool(at+0x44);value.animated=Pointer(at+12).has_value();
        value.children=Ring(Pointer(at+8));for(auto child:value.children)Instance(child,depth+1);
        result_.slides.push_back(std::move(value));End(at);
    }
    void Library(std::uint32_t at)
    {
        const auto type=Word(at+0x74);Require(type<=4,"Unknown FEN library type");
        if(!Begin(at,type==2?0x88:type==3?0x80:type==1?0x7c:0x78,Kind::Library,0))return;
        FrontendLibraryObject value{};value.offset=at;value.type=type;value.hash=Word(at+0x50);value.name=Name(at+0x54);value.attributes=Attributes(at+8);
        if(type==1||type==2)value.resource=Pointer(at+0x78);
        if(type==2)
        {std::copy_n(Value(at+0x7c,4).begin(),4,value.text_effect_colour.begin());value.text_box={Number(at+0x80),Number(at+0x84)};}
        if(type==3)
        {
            value.slides=Ring(Pointer(at+0x78));value.active_slide=Pointer(at+0x7c);
            Require(!value.active_slide||std::find(value.slides.begin(),value.slides.end(),*value.active_slide)!=value.slides.end(),"FEN active component slide is outside its ring");
            for(auto slide:value.slides)Slide(slide,1);
        }
        result_.library.push_back(std::move(value));End(at);
    }
public:
    explicit Reader(Bytes file)
    {
        Require(file.size()<=MaximumAssetBytes,"FEN package exceeds its budget");
        Require(U32(file,0)==0x46454e4c&&U32(file,4)==1,"Unsupported FENL package/version");
        const auto size=U32(file,8),table_size=U32(file,12);Require(size>=24&&size%4==0&&table_size%4==0,"Invalid FEN package lengths");
        Require(std::size_t(size)+table_size==file.size()-16,"FEN package lengths do not cover the file");
        data_=Slice(file,16,size);const auto table=Slice(file,16+size,table_size);
        Require(table_size/4<=MaximumAssetBytes/16,"FEN relocation table exceeds its budget");
        for(std::size_t i=0;i<table.size();i+=4)
        {
            const auto field=U32(table,i);Require(field%4==0&&field<=size-4,"Invalid FEN relocation field");
            Require(relocations_.insert(field).second,"Duplicate FEN relocation field");
            const auto target=U32(data_,field);Require(target==UINT32_MAX||target<size,"FEN relocation target exceeds package data");
        }
        // The package header is part of the data graph, but is never a TL object.
        Begin(0,24,Kind::Presentation,0);End(0);result_.relocation_count=relocations_.size();result_.id=Word(16);
        const auto resources=Ring(Pointer(8));Require(resources.size()==Word(20),"FEN resource count differs from ring");for(auto at:resources)Resource(at);
        for(auto at:Ring(Pointer(12)))Library(at);
        const auto presentation=Present(Pointer(4));Require(Begin(presentation,12,Kind::Presentation,0),"FEN presentation overlaps package");
        result_.presentation_slides=Ring(Pointer(presentation));result_.active_slide=Pointer(presentation+4);result_.presentation_time=Number(presentation+8);
        Require(!result_.active_slide||std::find(result_.presentation_slides.begin(),result_.presentation_slides.end(),*result_.active_slide)!=result_.presentation_slides.end(),"FEN active presentation slide is outside its ring");
        for(auto at:result_.presentation_slides)Slide(at,1);
        End(presentation);
        for(auto at:Ring(Pointer(0)))
            Require(std::any_of(result_.library.begin(),result_.library.end(),[&](const auto& v){return v.offset==at&&v.type==3;}),"FEN component list references a non-component");
        std::map<std::uint32_t, const FrontendLibraryObject*> libraries;
        std::map<std::uint32_t, const FrontendResource*> resources_by_offset;
        for (const auto& lib : result_.library) libraries.emplace(lib.offset, &lib);
        for (const auto& resource : result_.resources) resources_by_offset.emplace(resource.offset, &resource);
        for(const auto& instance:result_.instances)
        {
            const auto lib=libraries.find(*instance.library);
            Require(lib!=libraries.end()&&lib->second->type+1==instance.type,"FEN instance library type differs");
            if(instance.resource)
            {
                const auto resource=resources_by_offset.find(*instance.resource);
                Require(resource!=resources_by_offset.end()&&resource->second->type==0,"FEN image instance references an absent texture resource");
            }
        }
        for(const auto& lib:result_.library)if(lib.resource)
        {
            const auto resource=resources_by_offset.find(*lib.resource);
            Require(resource!=resources_by_offset.end()&&resource->second->type==(lib.type==2?1u:0u),"FEN library references an absent or wrong resource");
        }
        for (const auto& [at,size] : strings_)
        {
            auto next=nodes_.lower_bound(at);
            Require(next==nodes_.end() || at+size<=next->first,"FEN text overlaps a typed record");
            if(next!=nodes_.begin())
            { const auto prev=std::prev(next); Require(prev->first+prev->second.size<=at,"FEN text overlaps a typed record"); }
        }
        // Component instances enter their library's slides at runtime. Child-only
        // validation misses cycles and excessive nesting through those references.
        std::map<std::uint32_t,std::vector<std::uint32_t>> edges;
        for(const auto& lib:result_.library) if(lib.type==3) edges[lib.offset]=lib.slides;
        for(const auto& slide:result_.slides) edges[slide.offset]=slide.children;
        for(const auto& instance:result_.instances)
        {
            auto& children=edges[instance.offset]; children=instance.children;
            if(instance.type==4) children.push_back(*instance.library);
        }
        edges[presentation]=result_.presentation_slides;
        std::map<std::uint32_t,unsigned> heights;
        std::set<std::uint32_t> active;
        std::function<unsigned(std::uint32_t,unsigned)> height = [&](std::uint32_t at,unsigned depth) {
            Require(depth<=64,"FEN component graph exceeds its nesting limit");
            Require(!active.contains(at),"Cyclic FEN component dependency");
            if(const auto found=heights.find(at);found!=heights.end()) return found->second;
            active.insert(at); unsigned result=1;
            if(const auto found=edges.find(at);found!=edges.end())
                for(auto child:found->second) result=std::max(result,1+height(child,depth+1));
            Require(result<=64,"FEN component graph exceeds its nesting limit");
            active.erase(at);heights.emplace(at,result);return result;
        };
        for(const auto& [at,children]:edges) height(at,1);
    }
    FrontendScene Finish() { return std::move(result_); }
};
}
FrontendScene ReadFrontendScene(Bytes file) { return Reader(file).Finish(); }
}

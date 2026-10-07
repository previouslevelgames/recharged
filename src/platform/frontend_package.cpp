#include "platform/frontend_package.h"
#include "Game/FE/feAnimation.h"
#include "Game/FE/feFontResource.h"
#include "Game/FE/feGroup.h"
#include "Game/FE/feImage.h"
#include "Game/FE/feLayer.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feText.h"
#include "Game/FE/feTextureResource.h"
#include "Game/FE/tlComponent.h"
#include "Game/FE/tlComponentInstance.h"
#include "Game/FE/tlImageInstance.h"
#include "Game/FE/tlSlide.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace mscharged::platform
{
namespace
{
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(bool)==1);
static_assert(sizeof(std::uintptr_t)==sizeof(void*));
static_assert(std::is_trivially_copyable_v<FEPackage>);
static_assert(std::is_trivially_copyable_v<FEPresentation>);
static_assert(std::is_trivially_copyable_v<FELibObject>);
static_assert(std::is_trivially_copyable_v<FELayer>);
static_assert(std::is_trivially_copyable_v<FEGroup>);
static_assert(std::is_trivially_copyable_v<TLComponent>);
static_assert(std::is_trivially_copyable_v<TLInstance>);
static_assert(std::is_trivially_copyable_v<TLImageInstance>);
static_assert(std::is_trivially_copyable_v<TLTextInstance>);
static_assert(std::is_trivially_copyable_v<TLSlide>);
static_assert(std::is_trivially_copyable_v<FEText>);
static_assert(std::is_trivially_copyable_v<FEImage>);
static_assert(std::is_trivially_copyable_v<FEFontResource>);
static_assert(std::is_trivially_copyable_v<FETextureResource>);
static_assert(std::is_trivially_copyable_v<fAnimationKeyframe>);
static_assert(std::is_trivially_copyable_v<v3AnimationKeyframe>);

constexpr std::uint32_t NullWord=0xffffffffu;
enum class Kind:unsigned
{
    Package,Presentation,Library,Resource,Slide,Instance,Animation,
    FloatKey,VectorKey,String,Matrix,ExternalFont,FontString,UnknownKeys
};
enum class Encoding { Bytes,Word,Half,Pointer };
struct Field
{
    std::size_t raw,native,width;
    Encoding encoding;
    Kind target=Kind::Package;
};
struct Node
{
    Kind kind;
    std::size_t raw_size=0,native_size=0,alignment=1,native_offset=0;
    std::vector<Field> fields;
};
using Bytes=std::span<const std::byte>;
void Require(bool condition,const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Bounds(Bytes bytes,std::size_t at,std::size_t size)
{
    Require(at<=bytes.size() && size<=bytes.size()-at,"FEN record exceeds its actual byte span");
}
std::uint32_t Word(Bytes bytes,std::size_t at)
{
    Bounds(bytes,at,4);
    std::uint32_t value=0;
    for (unsigned i=0;i<4;++i) value=(value<<8)|std::to_integer<unsigned>(bytes[at+i]);
    return value;
}
std::uint16_t Half(Bytes bytes,std::size_t at)
{
    Bounds(bytes,at,2);
    return std::uint16_t((std::to_integer<unsigned>(bytes[at])<<8)|std::to_integer<unsigned>(bytes[at+1]));
}
void Store(std::span<std::byte> destination,std::size_t at,const void* value,std::size_t size)
{
    Require(at<=destination.size() && size<=destination.size()-at,"FEN native ABI field exceeds its allocation");
    std::memcpy(destination.data()+at,value,size);
}
void StoreWord(std::span<std::byte> destination,std::size_t at,std::uint32_t value,std::size_t width)
{
    if (width==4) Store(destination,at,&value,4);
    else if (width==8)
    {
        const std::uint64_t wide=value;
        Store(destination,at,&wide,8);
    }
    else throw std::runtime_error("Unsupported native FEN scalar width");
}
void StorePointer(std::span<std::byte> destination,std::size_t at,std::uintptr_t value)
{
    Store(destination,at,&value,sizeof(value));
}
std::size_t Align(std::size_t value,std::size_t alignment)
{
    Require(alignment && !(alignment&(alignment-1)),"Unsupported FEN native record alignment");
    Require(value<=std::numeric_limits<std::size_t>::max()-(alignment-1),"FEN native record length overflow");
    return (value+alignment-1)&~(alignment-1);
}
void BytesField(Node& node,std::size_t raw,std::size_t native,std::size_t width)
{
    node.fields.push_back({raw,native,width,Encoding::Bytes});
}
void WordField(Node& node,std::size_t raw,std::size_t native,std::size_t width=4)
{
    node.fields.push_back({raw,native,width,Encoding::Word});
}
void HalfField(Node& node,std::size_t raw,std::size_t native)
{
    node.fields.push_back({raw,native,2,Encoding::Half});
}
void PointerField(Node& node,std::size_t raw,std::size_t native,Kind target)
{
    node.fields.push_back({raw,native,sizeof(std::uintptr_t),Encoding::Pointer,target});
}
#define FWORD(N,T,F,R) WordField(N,R,offsetof(T,F),sizeof(((T*)nullptr)->F))
#define FHALF(N,T,F,R) HalfField(N,R,offsetof(T,F))
#define FBYTES(N,T,F,R) BytesField(N,R,offsetof(T,F),sizeof(((T*)nullptr)->F))
#define FPTR(N,T,F,R,K) PointerField(N,R,offsetof(T,F),K)

template<class T> void Shape(Node& node,std::size_t raw)
{
    node.raw_size=raw;node.native_size=sizeof(T);node.alignment=alignof(T);
}
void Attributes(Node& n,std::size_t raw,std::size_t native)
{
    // The exact original four feVector3 values, visibility, RGBA bytes,
    // source padding and four f32 UV fields; no math or visibility decisions.
    for (std::size_t i=0;i<12;++i) WordField(n,raw+4*i,native+4*i);
    BytesField(n,raw+48,native+48,8);
    for (std::size_t i=0;i<4;++i) WordField(n,raw+56+4*i,native+56+4*i);
}
void LibraryBase(Node& n)
{
    FPTR(n,FELibObject,next,0,Kind::Library);
    FPTR(n,FELibObject,prev,4,Kind::Library);
    Attributes(n,8,offsetof(FELibObject,m_attributes));
    FWORD(n,FELibObject,m_hashID,0x50);
    FBYTES(n,FELibObject,m_szName,0x54);
    FWORD(n,FELibObject,m_type,0x74);
}
void InstanceBase(Node& n)
{
    FPTR(n,TLInstance,m_next,0,Kind::Instance);
    FPTR(n,TLInstance,m_prev,4,Kind::Instance);
    FPTR(n,TLInstance,pChildren,8,Kind::Instance);
    FPTR(n,TLInstance,m_component,12,Kind::Library);
    FWORD(n,TLInstance,m_fStartTime,16);FWORD(n,TLInstance,m_fDuration,20);
    FBYTES(n,TLInstance,m_szName,24);FWORD(n,TLInstance,m_hash,0x38);
    Attributes(n,0x3c,offsetof(TLInstance,m_overloadedAttributes));
    FWORD(n,TLInstance,m_overloadFlags,0x84);FWORD(n,TLInstance,m_type,0x88);
    FHALF(n,TLInstance,m_priority,0x8c);FBYTES(n,TLInstance,m_bVisible,0x8e);
    // Keep the authored final Wii padding byte; native extra padding has no
    // source field and is transport storage only.
    BytesField(n,0x8f,offsetof(TLInstance,m_bVisible)+1,1);
}
void ResourceBase(Node& n)
{
    FPTR(n,FEResourceHandle,m_next,0,Kind::Resource);
    FPTR(n,FEResourceHandle,m_prev,4,Kind::Resource);
    FWORD(n,FEResourceHandle,m_type,8);FWORD(n,FEResourceHandle,m_hashID,12);
    FBYTES(n,FEResourceHandle,m_bValid,16);FBYTES(n,FEResourceHandle,pad_0x11,17);
    FWORD(n,FEResourceHandle,m_uFileBlock,20);
}
Node Describe(Bytes data,std::uint32_t at,Kind kind)
{
    Node n{kind};
    switch (kind)
    {
    case Kind::Package:
        Shape<FEPackage>(n,24);
        FPTR(n,FEPackage,m_pComponentList,0,Kind::Library);
        FPTR(n,FEPackage,m_pFEPresentation,4,Kind::Presentation);
        FPTR(n,FEPackage,m_pResourceList,8,Kind::Resource);
        FPTR(n,FEPackage,m_pFEObjectLibrary,12,Kind::Library);
        FWORD(n,FEPackage,m_uUniqueID,16);FWORD(n,FEPackage,m_uResourceCount,20);
        break;
    case Kind::Presentation:
        Shape<FEPresentation>(n,12);
        FPTR(n,FEPresentation,m_slides,0,Kind::Slide);
        FPTR(n,FEPresentation,m_currentSlide,4,Kind::Slide);
        FWORD(n,FEPresentation,m_fadeDuration,8);
        break;
    case Kind::Library:
    {
        const auto type=Word(data,std::size_t(at)+0x74);
        switch (type)
        {
        case FEOT_LAYER:Shape<FELayer>(n,0x78);break;
        case FEOT_GROUP:Shape<FEGroup>(n,0x78);break;
        case FEOT_IMAGE:
            Shape<FEImage>(n,0x7c);FPTR(n,FEImage,m_pFeTextureResource,0x78,Kind::Resource);break;
        case FEOT_TEXT:
            Shape<FEText>(n,0x88);FPTR(n,FEText,m_pFeFontResource,0x78,Kind::Resource);
            BytesField(n,0x7c,offsetof(FEText,m_TextAttributes)+offsetof(FETextLibObjectAttributes,EffectColour),4);
            WordField(n,0x80,offsetof(FEText,m_TextAttributes)+offsetof(FETextLibObjectAttributes,BoxSize));
            WordField(n,0x84,offsetof(FEText,m_TextAttributes)+offsetof(FETextLibObjectAttributes,BoxSize)+4);break;
        case FEOT_COMPONENT:
            Shape<TLComponent>(n,0x80);
            FPTR(n,TLComponent,pChildren,0x78,Kind::Slide);FPTR(n,TLComponent,m_pActiveSlide,0x7c,Kind::Slide);break;
        default:throw std::runtime_error("Unsupported original FEN library record profile");
        }
        LibraryBase(n);break;
    }
    case Kind::Resource:
    {
        const auto type=Word(data,std::size_t(at)+8);
        if (type==FERT_TEXTURE)
        {
            Shape<FETextureResource>(n,0x20);FWORD(n,FETextureResource,m_glTextureHandle,24);
            FHALF(n,FETextureResource,m_uWidth,28);FHALF(n,FETextureResource,m_uHeight,30);
        }
        else if (type==FERT_FONT)
        {
            Shape<FEFontResource>(n,0x1c);FPTR(n,FEFontResource,m_pFontReference,24,Kind::ExternalFont);
        }
        else throw std::runtime_error("Unsupported serialized FEN scene/external resource profile");
        ResourceBase(n);break;
    }
    case Kind::Slide:
        Shape<TLSlide>(n,0x48);
        FPTR(n,TLSlide,m_next,0,Kind::Slide);
        // Pinned source names this Wii pointer slot pad0. Actual FEN relocation
        // proves its backlink; native pointer width occupies this slot/padding.
        PointerField(n,4,offsetof(TLSlide,pad0),Kind::Slide);
        Require(offsetof(TLSlide,pChildren)>=offsetof(TLSlide,pad0)+sizeof(void*),"Native TLSlide backlink storage is unavailable");
        FPTR(n,TLSlide,pChildren,8,Kind::Instance);FPTR(n,TLSlide,m_animations,12,Kind::Animation);
        FWORD(n,TLSlide,m_start,16);FWORD(n,TLSlide,m_duration,20);FWORD(n,TLSlide,m_time,24);
        FWORD(n,TLSlide,m_uPlayMode,28);FBYTES(n,TLSlide,m_szName,32);FWORD(n,TLSlide,m_hash,64);
        FBYTES(n,TLSlide,m_bPaused,68);BytesField(n,69,offsetof(TLSlide,m_bPaused)+1,3);
        break;
    case Kind::Instance:
    {
        const auto type=Word(data,std::size_t(at)+0x88);
        switch (type)
        {
        case TLAT_LAYER:case TLAT_GROUP:Shape<TLInstance>(n,0x90);break;
        case TLAT_COMPONENT:Shape<TLComponentInstance>(n,0x90);break;
        case TLAT_IMAGE:
            Shape<TLImageInstance>(n,0x98);FPTR(n,TLImageInstance,m_pTextureResource,0x90,Kind::Resource);
            FWORD(n,TLImageInstance,field_0x94,0x94);break;
        case TLAT_TEXT:
        {
            Shape<TLTextInstance>(n,0x114);FWORD(n,TLTextInstance,m_LocStrId,0x90);
            BytesField(n,0x94,offsetof(TLTextInstance,m_OverloadedAttributes)+offsetof(FETextLibObjectAttributes,EffectColour),4);
            WordField(n,0x98,offsetof(TLTextInstance,m_OverloadedAttributes)+offsetof(FETextLibObjectAttributes,BoxSize));
            WordField(n,0x9c,offsetof(TLTextInstance,m_OverloadedAttributes)+offsetof(FETextLibObjectAttributes,BoxSize)+4);
            FWORD(n,TLTextInstance,m_OverloadFlags,0xa0);
            const auto draw=offsetof(TLTextInstance,m_DrawInfo);
            PointerField(n,0xa4,draw+offsetof(nlTextBox::StringDrawInfo,pFont),Kind::ExternalFont);
            PointerField(n,0xa8,draw+offsetof(nlTextBox::StringDrawInfo,String),Kind::String);
            PointerField(n,0xac,draw+offsetof(nlTextBox::StringDrawInfo,pMatrix),Kind::Matrix);
            WordField(n,0xb0,draw+offsetof(nlTextBox::StringDrawInfo,DrawOptions),sizeof(((nlTextBox::StringDrawInfo*)nullptr)->DrawOptions));
            HalfField(n,0xb4,draw+offsetof(nlTextBox::StringDrawInfo,RowCount));
            HalfField(n,0xb6,draw+offsetof(nlTextBox::StringDrawInfo,YOffset));
            for (unsigned i=0;i<17;++i)
            {
                const auto row=draw+offsetof(nlTextBox::StringDrawInfo,Rows)+i*sizeof(Row);
                HalfField(n,0xb8+4*i,row+offsetof(Row,XOffset));
                HalfField(n,0xba+4*i,row+offsetof(Row,FirstChar));
            }
            FPTR(n,TLTextInstance,m_pFontString,0xfc,Kind::FontString);
            FWORD(n,TLTextInstance,m_DrawOptions,0x100);FPTR(n,TLTextInstance,m_wcUserString,0x104,Kind::String);
            FBYTES(n,TLTextInstance,m_UseScissorRect,0x108);FBYTES(n,TLTextInstance,pad_109,0x109);
            for (unsigned i=0;i<4;++i) HalfField(n,0x10a+2*i,offsetof(TLTextInstance,m_ScissorRect)+2*i);
            BytesField(n,0x112,offsetof(TLTextInstance,m_ScissorRect)+8,2);
            break;
        }
        default:throw std::runtime_error("Unsupported original FEN instance record profile");
        }
        InstanceBase(n);break;
    }
    case Kind::Animation:
    {
        Shape<FEAnimation>(n,0x1c);
        // The foreign serialized vptr is never dereferenced or copied as a
        // native callable address. Actual native lifetime starts after memcpy.
        FPTR(n,FEAnimation,m_next,4,Kind::Animation);FPTR(n,FEAnimation,m_prev,8,Kind::Animation);
        FPTR(n,FEAnimation,m_pTLInstanceTarget,12,Kind::Instance);
        FHALF(n,FEAnimation,m_cast_type,16);FBYTES(n,FEAnimation,pad12,18);FWORD(n,FEAnimation,m_type,20);
        const auto cast=Half(data,std::size_t(at)+16);
        FPTR(n,FEAnimation,m_DLRingHead,24,cast==0?Kind::FloatKey:cast==1?Kind::VectorKey:Kind::UnknownKeys);
        break;
    }
    case Kind::FloatKey:
        Shape<fAnimationKeyframe>(n,0x18);
        for (unsigned i=0;i<4;++i) WordField(n,4*i,offsetof(fAnimationKeyframe,pKeyFrameData)+4*i);
        FPTR(n,fAnimationKeyframe,m_next,16,Kind::FloatKey);FPTR(n,fAnimationKeyframe,m_prev,20,Kind::FloatKey);
        break;
    case Kind::VectorKey:
        Shape<v3AnimationKeyframe>(n,0x38);
        for (unsigned i=0;i<12;++i) WordField(n,4*i,4*i);
        FPTR(n,v3AnimationKeyframe,m_next,48,Kind::VectorKey);FPTR(n,v3AnimationKeyframe,m_prev,52,Kind::VectorKey);
        break;
    case Kind::String:
    {
        Require(at%2==0,"FEN UTF16 byte address is unaligned");
        std::size_t end=at;
        while (Half(data,end)!=0) end+=2;
        n.raw_size=end+2-at;n.native_size=n.raw_size;n.alignment=alignof(std::uint16_t);
        for (std::size_t i=0;i<n.raw_size;i+=2) HalfField(n,i,i);
        break;
    }
    case Kind::Matrix:
        n.raw_size=64;n.native_size=sizeof(nlMatrix4);n.alignment=alignof(nlMatrix4);
        static_assert(sizeof(nlMatrix4)==64);
        for (unsigned i=0;i<16;++i) WordField(n,4*i,4*i);
        break;
    default:throw std::runtime_error("Unsupported non-null FEN external/runtime pointer profile");
    }
    Bounds(data,at,n.raw_size);
    for (const auto& field:n.fields)
    {
        const auto raw_width=field.encoding==Encoding::Bytes?field.width:field.encoding==Encoding::Half?2:4;
        Require(field.raw<=n.raw_size && raw_width<=n.raw_size-field.raw,"FEN raw record schema overlaps another object");
        Require(field.native<=n.native_size && field.width<=n.native_size-field.native,"Native FEN record schema does not fit this ABI");
    }
    return n;
}
#undef FWORD
#undef FHALF
#undef FBYTES
#undef FPTR
}

PreparedFrontendPackage PrepareFrontendPackage(const void* input,std::size_t size)
{
    Require(input!=nullptr,"Null raw FEN byte span");
    const Bytes file(static_cast<const std::byte*>(input),size);
    Require(Word(file,0)==0x46454e4cu && Word(file,4)==1,"Unsupported raw Wii FENL/version profile");
    const std::size_t data_size=Word(file,8),table_size=Word(file,12);
    Require(data_size>=24 && data_size%4==0 && table_size%4==0,"Invalid raw Wii FEN byte lengths");
    Require(size>=16 && data_size<=size-16 && table_size==size-16-data_size,"Raw FEN byte lengths do not cover the actual input");
    const Bytes data=file.subspan(16,data_size),table=file.subspan(16+data_size,table_size);
    std::vector<std::uint32_t> relocation_order;
    std::set<std::uint32_t> relocations;
    for (std::size_t i=0;i<table.size();i+=4)
    {
        const auto field=Word(table,i);
        Require(field%4==0 && field<=data.size()-4,"Raw FEN relocation slot is out of bounds");
        Require(relocations.insert(field).second,"Duplicate raw FEN relocation slot has no qualified native ABI contract");
        const auto target=Word(data,field);
        Require(target==NullWord || target<data.size(),"Raw FEN relocation target is out of bounds");
        relocation_order.push_back(field);
    }
    std::map<std::uint32_t,Node> nodes;
    std::map<std::uint32_t,std::pair<std::uint32_t,std::size_t>> pointer_slots;
    std::deque<std::pair<std::uint32_t,Kind>> pending{{0,Kind::Package}};
    while (!pending.empty())
    {
        const auto [at,kind]=pending.front();pending.pop_front();
        if (const auto found=nodes.find(at);found!=nodes.end())
        {
            Require(found->second.kind==kind,"Conflicting typed aliases in raw FEN object storage");
            continue;
        }
        Require(at%((kind==Kind::String)?2:4)==0,"Raw FEN typed record is unaligned");
        Node node=Describe(data,at,kind);
        for (const auto& field:node.fields)
        {
            if (field.encoding!=Encoding::Pointer) continue;
            const auto slot=at+static_cast<std::uint32_t>(field.raw);
            const auto value=Word(data,slot);
            pointer_slots.emplace(slot,std::pair{at,field.native});
            if (!relocations.contains(slot))
                Require(value==0,"Non-null raw FEN pointer has no source relocation slot");
            else if (value!=NullWord) pending.emplace_back(value,field.target);
        }
        nodes.emplace(at,std::move(node));
    }
    for (const auto field:relocation_order)
        Require(pointer_slots.contains(field),"Unclassified raw FEN relocation profile");

    PreparedFrontendPackage out;
    std::size_t native_size=0;
    const Node* previous=nullptr;
    std::uint32_t previous_at=0;
    for (auto& [at,node]:nodes)
    {
        if (previous && at<std::size_t(previous_at)+previous->raw_size)
        {
            // UTF16 suffix aliases must retain exact address identity. No
            // other typed overlaps are silently flattened or discarded.
            Require(node.kind==Kind::String && previous->kind==Kind::String &&
                std::size_t(at)+node.raw_size==std::size_t(previous_at)+previous->raw_size,
                "Overlapping original FEN record profile is not qualified");
            node.native_offset=previous->native_offset+(at-previous_at);
        }
        else
        {
            native_size=Align(native_size,node.alignment);
            node.native_offset=native_size;
            Require(native_size<=std::numeric_limits<std::uint32_t>::max() &&
                node.native_size<=std::numeric_limits<std::uint32_t>::max()-native_size,"Native FEN data exceeds its source header width");
            native_size+=node.native_size;
            previous=&node;previous_at=at;
        }
        out.records_.push_back({at,node.native_offset,node.native_size,static_cast<unsigned>(node.kind)});
        if (node.kind==Kind::Animation) out.animation_offsets_.push_back(node.native_offset);
    }
    native_size=Align(native_size,alignof(std::uintptr_t));
    Require(relocation_order.size()<=std::numeric_limits<std::uint32_t>::max()/sizeof(std::uintptr_t),"Native FEN relocation table exceeds its source header width");
    const std::size_t native_table_size=relocation_order.size()*sizeof(std::uintptr_t);
    Require(native_size<=std::numeric_limits<std::uint32_t>::max(),"Native FEN package length exceeds its source header width");
    Require(native_size<=std::numeric_limits<std::size_t>::max()-16-native_table_size,"Native FEN transport size overflow");
    out.bytes_.resize(16+native_size+native_table_size);
    std::span<std::byte> native(out.bytes_);
    std::memcpy(native.data(),file.data(),4);
    StoreWord(native,4,1,4);StoreWord(native,8,static_cast<std::uint32_t>(native_size),4);
    StoreWord(native,12,static_cast<std::uint32_t>(native_table_size),4);
    for (const auto& [at,node]:nodes)
    {
        for (const auto& field:node.fields)
        {
            const std::size_t src=at+field.raw,dst=16+node.native_offset+field.native;
            switch (field.encoding)
            {
            case Encoding::Bytes:
                Bounds(data,src,field.width);Store(native,dst,data.data()+src,field.width);break;
            case Encoding::Word:StoreWord(native,dst,Word(data,src),field.width);break;
            case Encoding::Half:
            {
                const auto value=Half(data,src);Store(native,dst,&value,2);break;
            }
            case Encoding::Pointer:
            {
                const auto value=Word(data,src);
                const std::uintptr_t native_value=!relocations.contains(static_cast<std::uint32_t>(src))?0:
                    value==NullWord?NullWord:nodes.at(value).native_offset;
                StorePointer(native,dst,native_value);break;
            }
            }
        }
    }
    for (std::size_t i=0;i<relocation_order.size();++i)
    {
        const auto [record,field]=pointer_slots.at(relocation_order[i]);
        StorePointer(native,16+native_size+i*sizeof(std::uintptr_t),nodes.at(record).native_offset+field);
    }
    return out;
}

std::size_t PreparedFrontendPackage::NativeOffset(std::uint32_t raw) const
{
    const auto record=std::find_if(records_.begin(),records_.end(),[raw](const Record& value){return value.wii_offset==raw;});
    if (record==records_.end()) throw std::runtime_error("Unknown FEN raw record offset");
    return record->native_offset;
}
void PreparedFrontendPackage::StartObjectLifetimes(void* allocation) const
{
    Require(allocation!=nullptr,"Null original FEN package allocation");
    for (const auto at:animation_offsets_)
    {
        auto* where=static_cast<std::byte*>(allocation)+at;
        Require(reinterpret_cast<std::uintptr_t>(where)%alignof(FEAnimation)==0,"Original FEN allocation cannot hold the native animation ABI");
        // The original implicit default constructor installs only its native
        // vptr. Default initialization leaves every copied source member intact.
        ::new (where) FEAnimation;
    }
}
}

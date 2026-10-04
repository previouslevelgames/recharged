#include "resources/effects_bundle.h"
#include "resources/chunk_reader.h"
#include <algorithm>

namespace mscharged::resources
{
namespace
{
constexpr std::size_t MaxEntries = 4096, MaxTemplates = 16384, MaxGroups = 16384;
constexpr std::size_t MaxSpecs = 65536, MaxKeys = 262144, MaxUserSources = 4096;
struct Budget { std::size_t templates = 0, groups = 0, specs = 0, keys = 0, users = 0; };
void Charge(std::size_t& value, std::size_t count, std::size_t maximum)
{
    Require(count <= maximum - value,"Effects bundle exceeds its record budget"); value += count;
}
float Scalar(Bytes bytes, std::size_t offset)
{
    // Persistent fountain lifetimes legitimately use 1e10; do not apply the
    // unrelated static-mesh coordinate limit to effects parameters.
    const float result = std::bit_cast<float>(U32(bytes,offset));
    Require(std::isfinite(result),"Nonfinite effects parameter"); return result;
}
EffectRange Range(Bytes bytes, std::size_t offset)
{ return {Scalar(bytes,offset),Scalar(bytes,offset+4)}; }
struct Cursor
{
    Bytes file;
    std::size_t position, end;
    Cursor(Bytes file_, Bytes payload)
        : file(file_), position(static_cast<std::size_t>(payload.data()-file.data())), end(position+payload.size()) {}
    explicit Cursor(Bytes file_) : file(file_), position(0), end(file_.size()) {}
    Chunk Any()
    {
        const auto result = ReadChunk(file,position,end);
        Require(result.next<=end,"Effects chunk padding exceeds its container");
        position=result.next; return result;
    }
    Chunk Next(std::uint32_t id)
    {
        const auto result=Any();
        Require(result.id==id,"Unexpected effects chunk ID or order");return result;
    }
    void Done() const { Require(position==end,"Unexpected trailing effects chunks"); }
};
EffectProperty Property(Bytes file, Bytes payload, Budget& budget)
{
    Cursor cursor(file,payload);
    const auto header=cursor.Next(0x24005).payload;
    Require(header.size()==20,"Invalid effects property record size");
    EffectProperty result;
    result.curved=U32(header,0)!=0;
    if (result.curved)
    {
        const auto count=U32(header,12);
        Require(count && count<=MaxKeys,"Invalid effects curve key count");
        Charge(budget.keys,count,MaxKeys);
        const auto keys=cursor.Next(0x24006).payload;
        Require(Records(keys,20,MaxKeys)==count,"Effects curve count disagrees with its chunk");
        result.keys.reserve(count);
        float previous=-1;
        for (std::size_t i=0;i<count;++i)
        {
            const auto offset=i*20;
            EffectCurveKey key{Scalar(keys,offset),Scalar(keys,offset+4),Scalar(keys,offset+8),Scalar(keys,offset+12),Scalar(keys,offset+16)};
            Require(key.time>=0 && key.time<1 && key.time>previous,"Effects curve times must increase inside [0,1)");
            Require(i || key.time==0,"Effects curve must cover time zero");
            previous=key.time;result.keys.push_back(key);
        }
    }
    else result.value=Range(header,4); // Count and pointer are inactive and may contain old tool memory.
    cursor.Done();return result;
}
EffectTemplate Template(Bytes file, Bytes payload, Budget& budget)
{
    Cursor cursor(file,payload);
    const auto record=cursor.Next(0x24003).payload;
    Require(record.size()==0xdc || record.size()==0xe0,"Unsupported effects template record size");
    EffectTemplate result;
    result.hash=U32(record,0);result.fountain_life=Scalar(record,4);
    result.mass=Range(record,8);result.particle_life=Range(record,0x10);
    result.inherit_velocity=Range(record,0x18);result.acceleration=Range(record,0x20);result.rotation=Range(record,0x28);
    result.unidentified_030=Scalar(record,0x30);
    result.emitter=record[0x34];result.blend=record[0x35];result.billboard=record[0x36];result.flags=record[0x37];
    result.texture=U32(record,0x38);result.frames=std::bit_cast<std::int32_t>(U32(record,0x3c));
    for (unsigned i=0;i<3;++i)result.unidentified_040[i]=U32(record,0x40+i*4);
    result.fps=Range(record,0x4c);result.model=U32(record,0x54);
    for (auto& property:result.properties) property=Property(file,cursor.Next(0x80024004).payload,budget);
    for (std::size_t offset=0x78;offset<record.size();offset+=4)
    {
        std::array<std::uint8_t,4> colour;
        std::copy_n(record.begin()+offset,4,colour.begin());result.colours.push_back(colour);
    }
    cursor.Done();return result;
}
EffectGroup Group(Bytes file, Bytes payload, std::size_t templates, Budget& budget)
{
    Cursor cursor(file,payload);
    const auto record=cursor.Next(0x24021).payload;
    Require(record.size()==28,"Invalid effects group record size");
    EffectGroup result;
    result.hash=U32(record,0);result.lingering=U32(record,12);
    const auto count=U32(record,8),users=U32(record,20);
    Charge(budget.specs,count,MaxSpecs);Charge(budget.users,users,MaxUserSources);
    const auto specs=cursor.Next(0x24022).payload;
    Require(Records(specs,88,MaxSpecs)==count,"Effects spec count disagrees with its chunk");
    result.specs.reserve(count);
    for (std::size_t i=0;i<count;++i)
    {
        const auto s=Slice(specs,i*88,88);EffectSpec spec;
        spec.hash=U32(s,0);spec.template_index=U32(s,4);
        Require(spec.template_index<templates,"Effects spec references a missing template");
        spec.attach=U32(s,8);spec.joint=U32(s,12);spec.delay=Scalar(s,16);spec.joint_binding=U32(s,20);
        spec.joint_velocity=Scalar(s,24);spec.in_front=U32(s,28);spec.ground=U32(s,32);spec.light=U32(s,36);
        spec.offset=Scalar(s,40);
        for(unsigned axis=0;axis<3;++axis)spec.local_offset[axis]=Scalar(s,44+axis*4);
        spec.terrain=U32(s,56);spec.linger_start=Scalar(s,60);spec.linger_end=Scalar(s,64);
        spec.layer=U32(s,68);spec.forward_axis=std::bit_cast<std::int32_t>(U32(s,72));
        result.specs.push_back(spec);
    }
    const auto sources=cursor.Next(0x24023).payload;
    Require(Records(sources,8,MaxUserSources)==users,"Effects user source count disagrees with its chunk");
    for(std::size_t i=0;i<users;++i)
    {
        // Original LoadFromChunk consumes the next user source without an ID
        // dispatch. Preserve its ID/bytes; no unqualified parser is run here.
        const auto chunk=cursor.Any();const auto source=chunk.payload;
        Require(!(chunk.id&0x80000000),"Effects user source is not a byte chunk");
        const auto size=U32(sources,i*8);
        Require(source.size()==size,"Effects user source length disagrees with its chunk");
        result.user_sources.push_back({chunk.id,{source.begin(),source.end()}});
    }
    cursor.Done();return result;
}
EffectsBundleEntry Entry(Bytes file, Bytes payload, Budget& budget)
{
    Cursor cursor(file,payload);
    const auto record=cursor.Next(0x24001).payload;
    Require(record.size()==24,"Invalid effects entry record size");
    EffectsBundleEntry result;
    result.unidentified_header={U32(record,0),U32(record,4)};
    const auto templates=U32(record,8),groups=U32(record,16);
    Charge(budget.templates,templates,MaxTemplates);Charge(budget.groups,groups,MaxGroups);
    Require(Records(cursor.Next(0x24025).payload,4,MaxTemplates)==templates,"Effects template table size mismatch");
    Require(Records(cursor.Next(0x24026).payload,4,MaxGroups)==groups,"Effects group table size mismatch");
    result.templates.reserve(templates);result.groups.reserve(groups);
    for (std::size_t i=0;i<templates;++i)result.templates.push_back(Template(file,cursor.Next(0x80024002).payload,budget));
    for (std::size_t i=0;i<groups;++i)result.groups.push_back(Group(file,cursor.Next(0x80024020).payload,templates,budget));
    cursor.Done();return result;
}
Chunk Root(Bytes bytes)
{
    Require(bytes.size()<=MaximumAssetBytes,"Effects bundle exceeds its byte budget");
    Cursor file(bytes);const auto root=file.Next(0x80000001);file.Done();return root;
}
}
EffectsBundle ReadEffectsBundle(Bytes resident)
{
    const auto root=Root(resident);
    Cursor entries(resident,root.payload);EffectsBundle result;Budget budget;
    while(entries.position<entries.end)
    {
        Require(result.entries.size()<MaxEntries,"Effects entry budget exceeded");
        auto entry=Entry(resident,entries.Next(0x80024000).payload,budget);
        // EmissionManager replaces repeated group hashes in load order. Keep
        // every authored entry here; the registration owner resolves aliases.
        result.entries.push_back(std::move(entry));
    }
    return result;
}
TextureBundle ReadEffectsTextureBundle(Bytes nonresident)
{
    const auto root=Root(nonresident);Cursor contents(nonresident,root.payload);
    const auto textures=contents.Next(0x24100).payload;contents.Done();
    return ReadTextureBundle(textures);
}
}

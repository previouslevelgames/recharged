#include "resources/effects_bundle.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>

using namespace mscharged::resources;
namespace
{
using Blob=std::vector<std::uint8_t>;
unsigned checks=0;
void Check(bool good,const char* message) { ++checks;if(!good)throw std::runtime_error(message); }
template<class F> void Reject(F f)
{ ++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Malformed effects input accepted"); }
void Word(Blob& b,std::size_t at,std::uint32_t value)
{ for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(24-i*8); }
void Float(Blob& b,std::size_t at,float value) { Word(b,at,std::bit_cast<std::uint32_t>(value)); }
void Append(Blob& b,const Blob& add) { b.insert(b.end(),add.begin(),add.end()); }
Blob Chunk(std::uint32_t id,const Blob& payload)
{ Blob result(8);Word(result,0,id);Word(result,4,payload.size());Append(result,payload);while(result.size()%4)result.push_back(0);return result; }
Blob Join(std::initializer_list<Blob> children)
{ Blob result;for(const auto& child:children)Append(result,child);return result; }
Blob Property(bool curved)
{
    Blob header(20,0xce);Word(header,0,curved?1:0);
    Float(header,4,curved?std::numeric_limits<float>::quiet_NaN():3.5f);Float(header,8,curved?std::numeric_limits<float>::quiet_NaN():-2.f);
    Blob contents;
    if(curved)
    {
        Word(header,12,2);Blob keys(40);
        for(unsigned i=0;i<2;++i)
        { Float(keys,i*20,i*.5f);Float(keys,i*20+4,2+i);Float(keys,i*20+8,-3);Float(keys,i*20+12,4);Float(keys,i*20+16,5); }
        contents=Join({Chunk(0x24005,header),Chunk(0x24006,keys)});
    }
    else contents=Chunk(0x24005,header); // Inactive count/pointer are intentionally arbitrary.
    return Chunk(0x80024004,contents);
}
Blob Template(unsigned id,unsigned colours)
{
    Blob record(0x78+colours*4);Word(record,0,id);Float(record,4,1e10f);
    for(unsigned at=8;at<=0x30;at+=4)Float(record,at,float(at)/4);
    record[0x34]=3;record[0x35]=1;record[0x36]=2;record[0x37]=10;
    Word(record,0x38,0x12345678);Word(record,0x3c,3);Word(record,0x40,0xabcd0123);
    Float(record,0x4c,30);Float(record,0x50,2);Word(record,0x54,0xffffffff);
    for(unsigned at=0x58;at<0x78;at+=4)Word(record,at,0xdeadcafe);
    for(unsigned i=0;i<colours;++i)for(unsigned c=0;c<4;++c)record[0x78+i*4+c]=i*4+c;
    Blob parts=Chunk(0x24003,record);
    for(unsigned i=0;i<8;++i)Append(parts,Property(i==1));
    return Chunk(0x80024002,parts);
}
Blob Group(bool users=false)
{
    Blob header(28,0xce);Word(header,0,0x81f2a311);Word(header,8,2);Word(header,12,1);Word(header,20,users?1:0);
    Blob specs(88*2);
    for(unsigned i=0;i<2;++i)
    {
        const auto n=i*88;Word(specs,n,0x10+i);Word(specs,n+4,i);Word(specs,n+8,3);Word(specs,n+12,42);
        Float(specs,n+16,.2f);Word(specs,n+20,0);Float(specs,n+24,1.5f);Word(specs,n+28,1);
        Word(specs,n+32,1);Word(specs,n+36,0);Float(specs,n+40,-2);
        for(unsigned axis=0;axis<3;++axis)Float(specs,n+44+axis*4,axis+.5f);
        Word(specs,n+56,9);Float(specs,n+60,-1);Float(specs,n+64,2);Word(specs,n+68,3);Word(specs,n+72,0xffffffff);
    }
    Blob source_header(users?8:0);if(users){Word(source_header,0,4);Word(source_header,4,0xdeadcafe);}
    Blob result=Join({Chunk(0x24021,header),Chunk(0x24022,specs),Chunk(0x24023,source_header)});
    if(users)Append(result,Chunk(0x24024,{'t','e','s','t'}));
    return Chunk(0x80024020,result);
}
Blob Fixture(bool users=false)
{
    Blob header(24,0xab);Word(header,0,1);Word(header,4,2);Word(header,8,2);Word(header,16,1);
    return Chunk(0x80000001,Chunk(0x80024000,Join({Chunk(0x24001,header),Chunk(0x24025,Blob(8,0xce)),
        Chunk(0x24026,Blob(4,0xde)),Template(10,25),Template(11,26),Group(users)})));
}
// Independent structural location helper used only to mutate generated fixtures.
std::vector<std::size_t> Locate(const Blob& bytes,unsigned selected)
{
    std::vector<std::size_t> result;
    auto walk=[&](auto&& self,std::size_t first,std::size_t end)->void{
        for(auto offset=first;offset<end;)
        {
            const auto id=U32(bytes,offset),size=U32(bytes,offset+4);
            if(id==selected)result.push_back(offset+8);
            if(id&0x80000000)self(self,offset+8,offset+8+size);
            offset=Align(offset+8+size,4);
        }
    };walk(walk,0,bytes.size());return result;
}
void Generated()
{
    auto bytes=Fixture();auto bundle=ReadEffectsBundle(bytes);
    Check(bundle.entries.size()==1 && bundle.entries[0].templates.size()==2 && bundle.entries[0].groups.size()==1,"Effects counts changed");
    const auto& entry=bundle.entries[0];const auto& t=entry.templates[0];
    Check(entry.unidentified_header==std::array<std::uint32_t,2>{1,2} && t.hash==10 && t.fountain_life==1e10f,"Header/lifetime changed");
    Check(t.mass.base==2 && t.rotation.range==11 && t.fps.base==30 && t.texture==0x12345678 && t.model==0xffffffff,"Template fields changed");
    Check(t.emitter==3 && t.blend==1 && t.billboard==2 && t.flags==10 && t.frames==3 && t.unidentified_040[0]==0xabcd0123,"Template enums or unknown fields changed");
    Check(t.colours.size()==25 && entry.templates[1].colours.size()==26 && t.colours.back()[3]==99,"Serialized colours were widened or dropped");
    for(unsigned i=0;i<8;++i)
    {
        Check(t.properties[i].curved==(i==1),"Property order changed");
        if(i==1)Check(t.properties[i].keys.size()==2 && t.properties[i].keys[1].time==.5f && t.properties[i].keys[1].cubic==3,"Curve keys changed");
        else Check(t.properties[i].value.base==3.5f && t.properties[i].value.range==-2 && t.properties[i].keys.empty(),"Inactive garbage became an active pointer/count");
    }
    const auto& group=entry.groups[0];const auto& spec=group.specs[1];
    Check(group.hash==0x81f2a311 && group.lingering==1 && group.specs.size()==2 && spec.template_index==1,"Group/template resolution changed");
    Check(spec.attach==3 && spec.forward_axis==-1 && spec.local_offset==std::array<float,3>{.5f,1.5f,2.5f} && spec.linger_start==-1,"Spec values were silently remapped");
    auto with_user=ReadEffectsBundle(Fixture(true));
    Check(with_user.entries[0].groups[0].user_sources[0].chunk_id==0x24024 && with_user.entries[0].groups[0].user_sources[0].bytes==Blob({'t','e','s','t'}),"User source bytes were not retained");
    auto user_bytes=Fixture(true);
    Word(user_bytes,Locate(user_bytes,0x24023)[0],5);
    Reject([&]{ReadEffectsBundle(user_bytes);});
    Blob unaligned{0xee};Append(unaligned,bytes);
    Check(ReadEffectsBundle(Bytes(unaligned).subspan(1)).entries[0].templates[0].colours==t.colours,
        "Effects decoding depended on host buffer alignment");
    const auto record=Locate(bytes,0x24003)[0],entry_at=Locate(bytes,0x24001)[0],group_at=Locate(bytes,0x24021)[0];
    const auto props=Locate(bytes,0x24005);
    const auto curve=Locate(bytes,0x24006)[0],spec_at=Locate(bytes,0x24022)[0];
    for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned>>{
        {0,0},{4,UINT32_MAX},{entry_at+8,UINT32_MAX},{entry_at+16,UINT32_MAX},{record-4,0xd8},
        {record+4,0x7f800000},{record+8,0x7fc00000},{props[0]+4,0x7fc00000},{props[1]+12,0},
        {props[1]+12,UINT32_MAX},{curve,0x3f000000},{curve+20,0},{curve+20,0x3f800000},{curve+24,0xff800000},
        {spec_at+4,2},{spec_at+40,0x7fc00000},{group_at+8,3},{group_at+20,1},{props[0]-8,0x80024005}})
    {
        auto bad=bytes;Word(bad,offset,value);Reject([&]{ReadEffectsBundle(bad);});
    }
    auto extra=bytes;extra.push_back(0);Reject([&]{ReadEffectsBundle(extra);});
    auto duplicate=Chunk(0x80000001,Join({Blob(bytes.begin()+8,bytes.end()),Blob(bytes.begin()+8,bytes.end())}));
    Reject([&]{ReadEffectsBundle(duplicate);});
    for(std::size_t n=0;n<bytes.size();++n)Reject([&]{ReadEffectsBundle(Bytes(bytes).first(n));});
    auto empty=ReadEffectsBundle(Chunk(0x80000001,{}));Check(empty.entries.empty(),"Empty valid bundle rejected");
    bytes.assign(bytes.size(),0);bytes.clear();bytes.shrink_to_fit();
    Check(t.colours.back()[3]==99 && t.properties[1].keys[1].linear==4,"Decoded data retained source pointers");
}
Blob Textures()
{
    Blob rlt(16+16+32+32);
    Word(rlt,0,0x50544c47);Word(rlt,4,1);Word(rlt,16,0x12345678);Word(rlt,20,0);Word(rlt,24,64);
    Word(rlt,32,1);Word(rlt,36,5);rlt[32+15]=8;rlt[32+17]=8;
    for(unsigned i=64;i<96;++i)rlt[i]=i;
    return Chunk(0x80000001,Chunk(0x24100,rlt));
}
void TextureChecks()
{
    auto bytes=Textures();const auto decoded=ReadEffectsTextureBundle(bytes);
    Check(decoded.textures.size()==1 && decoded.textures[0].id==0x12345678 && decoded.textures[0].pixels[0]==64,"Nested nonresident texture decode failed");
    for(std::size_t n=0;n<bytes.size();++n)Reject([&]{ReadEffectsTextureBundle(Bytes(bytes).first(n));});
    Word(bytes,8,0x24101);Reject([&]{ReadEffectsTextureBundle(bytes);});
}
Blob Read(const char* path)
{ std::ifstream in(path,std::ios::binary);Check(bool(in),"Cannot open owned effect input");return Blob(std::istreambuf_iterator<char>(in),{}); }
void Owned(const char* resident,const char* nonresident,const char* textures)
{
    const auto bytes=Read(resident);const auto bundle=ReadEffectsBundle(bytes);
    std::size_t templates=0,groups=0,specs=0,curves=0,keys=0,colours25=0,attachment3=0;
    std::uint32_t fingerprint=2166136261;
    auto word=[&](std::uint32_t v){for(unsigned shift:{24,16,8,0}){fingerprint^=(v>>shift)&255;fingerprint*=16777619;}};
    for(const auto& entry:bundle.entries)
    {
        for(const auto& t:entry.templates)
        {
            ++templates;word(t.hash);word(std::bit_cast<std::uint32_t>(t.fountain_life));
            colours25+=t.colours.size()==25;
            for(const auto& property:t.properties)
            {
                word(property.curved);
                if(property.curved)
                {
                    ++curves;keys+=property.keys.size();
                    for(const auto& key:property.keys)for(float v:{key.time,key.cubic,key.quadratic,key.linear,key.constant})word(std::bit_cast<std::uint32_t>(v));
                }
                else{word(std::bit_cast<std::uint32_t>(property.value.base));word(std::bit_cast<std::uint32_t>(property.value.range));}
            }
        }
        for(const auto& group:entry.groups)
        { ++groups;word(group.hash);for(const auto& spec:group.specs){++specs;word(spec.template_index);word(spec.attach);attachment3+=spec.attach==3;} }
    }
    const auto nr=ReadEffectsTextureBundle(Read(nonresident)),geometry=ReadTextureBundle(Read(textures));
    Check(bundle.entries.size()==249 && templates==1745 && groups==249 && specs==1745 && curves==1374
        && keys==2064 && colours25==1745 && attachment3==44,"Owned effect record profile changed");
    Check(nr.textures.size()==94 && geometry.textures.size()==8 && nr.animations.empty() && geometry.animations.empty(),
        "Owned effects texture profile changed");
    std::cout<<"Owned effects: "<<bundle.entries.size()<<" entries, "<<templates<<" templates, "<<groups<<" groups, "<<specs<<" specs, "<<curves<<" curves, "<<keys<<" keys; 25-colour records "<<colours25<<", attachment3 "<<attachment3<<"; native fingerprint "<<std::hex<<fingerprint<<std::dec<<"; textures "<<nr.textures.size()<<"+"<<geometry.textures.size()<<", animations "<<nr.animations.size()<<"+"<<geometry.animations.size()<<'\n';
}
}
int main(int argc,char** argv)
{
    try
    {
        Generated();TextureChecks();
        if(argc==4)Owned(argv[1],argv[2],argv[3]);else Check(argc==1,"Expected resident, decompressed nonresident and geometry texture files");
        std::cout<<checks<<" effects resource checks passed\n";
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}

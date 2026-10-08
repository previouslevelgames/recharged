#include "resources/chunk_reader.h"
#include "runtime/sanim_assets.h"
#include "Game/SAnim.h"
#include <atomic>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>

namespace allocations { std::atomic<std::size_t> live=0; thread_local std::size_t remaining=SIZE_MAX; }
void* operator new(std::size_t size)
{
    if(allocations::remaining!=SIZE_MAX){if(!allocations::remaining)throw std::bad_alloc();--allocations::remaining;}
    if(void* p=std::malloc(size?size:1)){++allocations::live;return p;}throw std::bad_alloc();
}
void operator delete(void* p) noexcept {if(p){--allocations::live;std::free(p);}}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool good,const char* message){++checks;if(!good)throw std::runtime_error(message);}
template<class F>void Reject(F action){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid SAnim operation accepted");}
void Bits(float value,float expected){Check(std::bit_cast<unsigned>(value)==std::bit_cast<unsigned>(expected),"SAnim float bits differ");}
using Blob=std::vector<std::uint8_t>;
void Word(Blob& b,unsigned v){for(int s:{24,16,8,0})b.push_back(v>>s);}
void Half(Blob& b,unsigned v){b.push_back(v>>8);b.push_back(v);}
void Set(Blob& b,std::size_t at,unsigned v){for(int s:{24,16,8,0})b.at(at++)=v>>s;}
std::size_t Pad(std::size_t n,std::size_t align){return (n+align-1)/align*align;}
void Chunk(Blob& b,unsigned id,const Blob& payload,unsigned exponent=0)
{
    auto at=b.size();Word(b,id|(exponent<<24));Word(b,0);b.resize(Pad(b.size(),std::size_t(1)<<exponent),0xab);
    b.insert(b.end(),payload.begin(),payload.end());Set(b,at+4,b.size()-at-8);b.resize(Pad(b.size(),4),0xcd);
}
void Animation(Blob& b,unsigned frames=3,unsigned roots=3,bool unequal=false,unsigned exponent=2)
{
    auto begin=b.size();Word(b,0x80017000|(exponent<<24));Word(b,0);b.resize(Pad(b.size(),std::size_t(1)<<exponent),0xdd);
    Blob header;for(unsigned i=0;i<22;++i)Word(header,0xf0000000+i);
    Set(header,4,0xfedcba98);Set(header,8,frames);Set(header,12,5);Set(header,16,2);Set(header,52,roots);Set(header,84,0xdeadbeef);
    Chunk(b,0x17001,header,(exponent+1)%6);Chunk(b,0x17002,{'t','e','s','t',0,0,0,0});
    Blob counts,aux,scratch;for(unsigned i:{1u,3u,0u,3u,0u})Word(counts,i);
    for(unsigned i=0;i<5;++i){Word(aux,0x12340000+i);Word(scratch,0xeecc0000+i);}
    Chunk(b,0x17110,counts);Chunk(b,0x17113,aux);
    for(unsigned id:{0x17004u,0x17005u,0x17006u,0x17111u,0x17114u})Chunk(b,id,scratch);
    Blob rootr,roott;
    for(unsigned i=0;i<roots;++i)
    {
        Half(rootr,i==0?65530:i==1?6:65500);
        for(float f:{float(i)*8,float(i)*-4,float(i)*2})Word(roott,std::bit_cast<unsigned>(f));
    }
    Chunk(b,0x17007,rootr);Chunk(b,0x17008,roott,2);
    const unsigned props[]{15,16,46,0x40000000,0};
    for(unsigned node=0;node<5;++node)
    {
        const auto start=b.size();Word(b,0x80017100);Word(b,0);
        if(node!=4)
        {
            Blob rot,trans,scale;
            const auto p=props[node];
            for(unsigned key=0;key<(p&2?1:frames);++key)
            {
                if(p&1)Half(rot,65530);
                else if(p&16)for(unsigned value:{0x8000u,0u,0x7fffu,0xffffu})Half(rot,value);
                else if(p&32)for(unsigned char v:{0x80,0x01,0x00,0x7f,0xff,0xff})rot.push_back(v);
                else for(unsigned char v:{0x80,0,0x7f,0xff})rot.push_back(v);
            }
            for(unsigned key=0;key<(p&4?1:frames);++key)
                for(float v:{-0.f,float(key)+.25f,std::bit_cast<float>(1u)})Word(trans,std::bit_cast<unsigned>(v));
            for(unsigned key=0;key<(p&8?1:frames);++key)for(unsigned v:{0u,2048u,65535u})Half(scale,v);
            Chunk(b,0x17101,rot,(node+exponent)%6);Chunk(b,0x17102,trans,2);Chunk(b,0x17103,scale,1);
            if(node==0)Chunk(b,0x17112,{255});else if(node!=2)Chunk(b,0x17112,{0,127,255});
            Chunk(b,0x17115,{0x80,0xff,0,0x42});
        }
        Set(b,start+4,b.size()-start-8);
    }
    Blob morphcounts,morphids,properties;Word(morphcounts,unequal?1:3);Word(morphcounts,3);
    Word(morphids,0x12345678);Word(morphids,0xfedcba98);
    Chunk(b,0x17009,morphcounts);Chunk(b,0x1700a,morphids);
    Chunk(b,0x1700b,unequal?Blob{128,255,64,0}:Blob{0,128,255,255,64,0});
    for(unsigned p:props)Word(properties,p);Chunk(b,0x17003,properties);
    Set(b,begin+4,b.size()-begin-8);
}
Blob Fixture(unsigned frames=3,unsigned roots=3,bool unequal=false,unsigned exponent=2)
{Blob result;Animation(result,frames,roots,unequal,exponent);return result;}
std::size_t Header(const Blob& b,unsigned id)
{
    const auto root=ReadChunk(b,0,b.size());auto pos=std::size_t(root.payload.data()-b.data());
    const auto end=pos+root.payload.size();
    while(pos<end){auto c=ReadChunk(b,pos,end);if(c.id==id)return pos;pos=c.next;}
    throw std::runtime_error("Missing test channel");
}
std::size_t Payload(const Blob& b,unsigned id){auto c=ReadChunk(b,Header(b,id),b.size());return c.payload.data()-b.data();}
std::size_t NodeChannel(const Blob& b,unsigned node,unsigned id)
{
    auto root=ReadChunk(b,0,b.size());auto at=std::size_t(root.payload.data()-b.data());
    const auto end=at+root.payload.size();
    while(at<end)
    {
        const auto c=ReadChunk(b,at,end);at=c.next;
        if(c.id!=0x80017100)continue;
        if(node--!=0)continue;
        auto pos=std::size_t(c.payload.data()-b.data());const auto stop=pos+c.payload.size();
        while(pos<stop){const auto child=ReadChunk(b,pos,stop);if(child.id==id)return pos;pos=child.next;}
        break;
    }
    throw std::runtime_error("Missing fixture node channel");
}
void Valid()
{
    for(unsigned exponent=0;exponent<6;++exponent)
    {
        auto b=Fixture(3,3,false,exponent);auto original=b;SAnimAssets assets(b);auto a=assets.At(0);const auto& d=a->Data();
        Check(assets.Size()==1&&d.GetHashID()==0xfedcba98&&d.m_nHierarchySignature==0xdeadbeef,"SAnim identity differs");
        Check(d.m_nNumKeys==3&&d.m_nNumNodes==5&&d.m_nNumMorphChannels==2,"SAnim counts differ");
        Check(d.m_pCallbackList==nullptr&&a->Keys(4).rotation==0&&d.m_pRotKeys[4]==nullptr,"Absent channels/callbacks fabricated");
        Check(b==original,"Native owner modified input bytes");b.assign(b.size(),0xcc);b.clear();b.shrink_to_fit();
        Check(std::get<std::uint16_t>(a->RotationKey(0,0))==65530,"Host angle conversion failed");
        for(unsigned node:{1u,2u,3u})
        {
            const auto q=std::get<std::array<float,4>>(a->RotationKey(node,0));
            Bits(q[0],-1);Bits(q[1],node==2?1.f/2048:0.f);
            Bits(q[2],node==1?32767.f/32768:node==2?2047.f/2048:127.f/128);
            Bits(q[3],node==1?-1.f/32768:node==2?-1.f/2048:-1.f/128);
            for(std::size_t key=0;key<a->Keys(node).rotation;++key)Check(a->RotationKey(node,key)==a->RotationKey(node,0),"Packed key order/stride differs");
        }
        for(unsigned node=0;node<4;++node)
        {
            const auto s=a->ScaleKey(node,0);Bits(s[0],0);Bits(s[1],1);Bits(s[2],65535.f/2048);
            const auto t=a->TranslationKey(node,0);Bits(t[0],-0.f);Bits(t[1],.25f);Bits(t[2],std::bit_cast<float>(1u));
            Check(a->HasAuxiliary(node)&&a->AuxiliaryBytes(node).size()==4&&a->AuxiliaryBytes(node)[0]==0x80&&d.m_Unknown1C[node]==0x12340000+node,"Opaque channel was changed");
        }
        for(float time:{0.f,.125f,.25f,.5f,.75f,.875f,1.f})
        {
            Check(a->Weight(0,time).authored&&a->Weight(0,time).value==1,"Original constant weight differs");
            Check(!a->Weight(4,time).authored&&a->Weight(4,time).value==1,"Original absent weight differs");
            const auto root=a->Root(time);Bits(root.translation[0],16*time);Bits(root.translation[1],-8*time);
            Check(root.rotation==(time<=.5f?std::uint16_t(65530+int(time*24)):std::uint16_t(6+int((time*2-1)*-42))),"Root angle wrap differs");
        }
        Bits(a->Weight(1,0).value,0);Bits(a->Weight(1,.5f).value,127.f/255);Bits(a->Weight(1,1).value,1);
        Bits(a->MorphWeight(0,.5f),128.f/255);Bits(a->MorphWeight(1,.5f),64.f/255);
        Bits(a->MorphWeight(0,1),1);Bits(a->MorphWeight(1,1),0);
        Check(d.m_nMorphIds[1]==0xfedcba98ul&&d.m_nMorphIds[0]==0x12345678,"Native-width morph ID table differs");
        const auto* address=&d;std::weak_ptr<const SAnimAsset> weak=a;
        {auto copy=a;std::thread read([copy,address]{if(&copy->Data()!=address||copy->Root(.5f).rotation!=6)std::terminate();});read.join();}
        Check(!weak.expired(),"Retained native animation disappeared");
    }
    for(unsigned roots:{0u,1u})
    {SAnimAssets assets(Fixture(1,roots));auto a=assets.At(0);Check(a->Root(0).rotation==(roots?65530:0)&&a->Root(1).rotation==(roots?65530:0),"Zero/single root changed");Bits(a->Data().GetLinearSpeed(),0);}
    auto multi=Fixture();Animation(multi,7,0,false,5);SAnimAsset::Handle retained;
    {SAnimAssets assets(multi);Check(assets.Size()==2&&assets.At(1)->Data().m_nNumKeys==7,"Inventory ordering/absolute alignment differs");retained=assets.At(1);Reject([&]{assets.At(2);});}
    multi.clear();Check(retained->Data().GetNumFrames()==7,"Track depended on inventory/file lifetime");
    std::weak_ptr<const SAnimAsset> weak=retained;retained.reset();Check(weak.expired(),"Final retained track was not released");
    SAnimAssets unequal(Fixture(3,3,true));auto a=unequal.At(0);Check(a->Data().m_pNumMorphKeys[0]==1&&a->Data().m_pNumMorphKeys[1]==3,"Unequal morph metadata lost");
    Reject([&]{a->MorphWeight(0,.5f);});Reject([&]{a->MorphWeight(1,.5f);});Reject([&]{a->Data().GetMorphWeight(1,.5f);});
    SAnimAssets maximum(Fixture(MaximumSAnimKeys));Check(maximum.At(0)->Keys(1).rotation==MaximumSAnimKeys,"Maximum frame count rejected");
}
void Invalid()
{
    const auto good=Fixture();
    for(std::size_t n=0;n<good.size();++n)Reject([&]{SAnimAssets bad(Bytes(good).first(n));});
    for(auto [offset,value]:{std::pair{8u,0u},{8u,MaximumSAnimKeys+1},{12u,MaximumSAnimNodes+1},{16u,MaximumSAnimMorphs+1},{52u,MaximumSAnimKeys+1}})
    {auto b=good;Set(b,Payload(b,0x17001)+offset,value);Reject([&]{ReadSAnimations(b);});}
    for(unsigned id:{0x17001u,0x17002u,0x17110u,0x17113u,0x17004u,0x17005u,0x17006u,0x17111u,0x17114u,0x17007u,0x17008u,0x17009u,0x1700au,0x1700bu,0x17003u})
    {
        auto b=good;Set(b,Header(b,id)+4,0xffffffff);Reject([&]{ReadSAnimations(b);});
        b=good;Set(b,Header(b,id),0x1abcd);Reject([&]{ReadSAnimations(b);});
        b=good;auto at=Header(b,id);Set(b,at+4,U32(b,at+4)+4);Reject([&]{ReadSAnimations(b);});
    }
    for(unsigned count:{0u,4u,0xffffffffu})
    {auto b=good;Set(b,Payload(b,0x17009),count);Reject([&]{ReadSAnimations(b);});}
    for(unsigned count:{0u,2u,0xffffffffu})
    {auto b=good;Set(b,Payload(b,0x17110),count);Reject([&]{ReadSAnimations(b);});}
    for(unsigned value:{0x7fc00000u,0x7f800000u,0x7f7fffffu})
    {auto b=good;Set(b,Payload(b,0x17008),value);Reject([&]{ReadSAnimations(b);});}
    for(unsigned id:{0x17101u,0x17102u,0x17103u,0x17112u})
    {
        auto b=good;Set(b,NodeChannel(b,0,id)+4,0xffffffff);Reject([&]{ReadSAnimations(b);});
        b=good;Set(b,NodeChannel(b,0,id),0x17199);Reject([&]{ReadSAnimations(b);});
        b=good;Set(b,NodeChannel(b,0,id)+4,0);Reject([&]{ReadSAnimations(b);});
    }
    auto node_bad=good;Set(node_bad,NodeChannel(node_bad,0,0x17101),0x17102);Reject([&]{ReadSAnimations(node_bad);});
    node_bad=good;auto channel=ReadChunk(node_bad,NodeChannel(node_bad,0,0x17102),node_bad.size());
    Set(node_bad,channel.payload.data()-node_bad.data(),0x7fc00000);Reject([&]{ReadSAnimations(node_bad);});
    auto b=good;Set(b,Payload(b,0x17003)+4,1);Reject([&]{ReadSAnimations(b);});
    b=good;b.push_back(0);Reject([&]{ReadSAnimations(b);});
    b=good;const auto at=Payload(b,0x17002);b[at+7]=1;Reject([&]{ReadSAnimations(b);});
    Reject([]{ReadSAnimations(Blob(MaximumAssetBytes+1));});
    SAnimAssets assets(good);auto a=assets.At(0);
    for(float t:{-1.f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
    {Reject([&]{a->Root(t);});Reject([&]{a->Weight(0,t);});Reject([&]{a->MorphWeight(0,t);});unsigned short rot;Reject([&]{a->Data().GetRootRot(t,&rot);});}
    Reject([&]{a->Keys(5);});Reject([&]{a->RotationKey(0,1);});Reject([&]{a->RotationKey(4,0);});
    Reject([&]{a->ScaleKey(0,1);});Reject([&]{a->TranslationKey(0,1);});Reject([&]{a->Weight(5,.5f);});Reject([&]{a->MorphWeight(2,0);});
    float weight;Reject([&]{a->Data().GetChannelWeight(-1,0,&weight);});Reject([&]{a->Data().GetMorphWeight(-1,0);});
    auto inventory=good;Animation(inventory,5,0);
    {SAnimAssets warm(inventory);}const auto before=allocations::live.load();unsigned failures=0;
    for(std::size_t fail=0;fail<400;++fail)
    {
        bool failed=false;allocations::remaining=fail;
        try{SAnimAssets temporary(inventory);}catch(const std::bad_alloc&){failed=true;}
        allocations::remaining=SIZE_MAX;Check(allocations::live==before,"Failed native SAnim construction leaked host storage");
        if(!failed){Check(failures>30,"Allocation fault probe missed retained stages");return;}++failures;
    }
    throw std::runtime_error("SAnim allocation fault probe never completed");
}
struct Hash
{
    unsigned value=2166136261u;
    void Byte(unsigned v){value=(value^(v&255))*16777619u;}
    void Word(unsigned v){for(int s:{24,16,8,0})Byte(v>>s);}
    void Float(float v){Word(std::bit_cast<unsigned>(v));}
    void Bytes(resources::Bytes bytes){for(auto b:bytes)Byte(b);}
};
void Inspect(const char* path)
{
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot open SAnim file");
    Blob bytes(std::istreambuf_iterator<char>(file),{});SAnimAssets assets(bytes);bytes.clear();bytes.shrink_to_fit();
    std::cout<<std::setprecision(9)<<'[';
    for(std::size_t a=0;a<assets.Size();++a)
    {
        if(a)std::cout<<',';auto asset=assets.At(a);const auto& d=asset->Data();std::cout<<"{\"name\":\"";
        for(const char* p=d.m_szName;*p;++p){if(*p=='"'||*p=='\\')std::cout<<'\\';std::cout<<*p;}
        std::cout<<"\",\"hash\":"<<d.GetHashID()<<",\"frames\":"<<d.m_nNumKeys<<",\"signature\":"<<d.m_nHierarchySignature
            <<",\"speed\":"<<d.m_fLinearSpeed<<",\"nodes\":[";
        for(unsigned n=0;n<d.m_nNumNodes;++n)
        {
            if(n)std::cout<<',';auto counts=asset->Keys(n);Hash rot,scale,trans,weights,aux;
            for(std::size_t k=0;k<counts.rotation;++k)
            {auto v=asset->RotationKey(n,k);if(auto* angle=std::get_if<std::uint16_t>(&v)){rot.Byte(*angle>>8);rot.Byte(*angle);}else for(float f:std::get<std::array<float,4>>(v))rot.Float(f);}
            for(std::size_t k=0;k<counts.scale;++k)for(float f:asset->ScaleKey(n,k))scale.Float(f);
            for(std::size_t k=0;k<counts.translation;++k)for(float f:asset->TranslationKey(n,k))trans.Float(f);
            weights.Bytes({d.m_pWeightKeys[n],counts.weights});aux.Bytes(asset->AuxiliaryBytes(n));
            std::cout<<"{\"properties\":"<<d.m_pNodeProperties[n]<<",\"aux_metadata\":"<<d.m_Unknown1C[n]
                <<",\"aux_present\":"<<(asset->HasAuxiliary(n)?"true":"false")<<",\"counts\":["
                <<counts.rotation<<','<<counts.scale<<','<<counts.translation<<','<<counts.weights<<"],\"hashes\":["
                <<rot.value<<','<<scale.value<<','<<trans.value<<','<<weights.value<<','<<aux.value<<"],\"weights\":[";
            bool first=true;for(float t:{0.f,.125f,.25f,.5f,.75f,.875f,1.f}){if(!first)std::cout<<',';first=false;std::cout<<std::bit_cast<unsigned>(asset->Weight(n,t).value);}std::cout<<"]}";
        }
        std::cout<<"],\"root_samples\":[";bool first=true;
        for(float t:{0.f,.125f,.25f,.5f,.75f,.875f,1.f})
        {if(!first)std::cout<<',';first=false;auto r=asset->Root(t);std::cout<<'['<<r.rotation;for(float f:r.translation)std::cout<<','<<std::bit_cast<unsigned>(f);std::cout<<']';}
        std::cout<<"],\"morph_ids\":[";for(unsigned m=0;m<d.m_nNumMorphChannels;++m){if(m)std::cout<<',';std::cout<<d.m_nMorphIds[m];}
        std::cout<<"],\"morph_counts\":[";std::size_t total=0;bool qualified=true;
        for(unsigned m=0;m<d.m_nNumMorphChannels;++m){if(m)std::cout<<',';std::cout<<d.m_pNumMorphKeys[m];total+=d.m_pNumMorphKeys[m];qualified&=d.m_pNumMorphKeys[m]==d.m_pNumMorphKeys[0];}
        Hash morph;morph.Bytes({d.m_pMorphKeys,total});std::cout<<"],\"morph_hash\":"<<morph.value<<",\"morph_samples\":";
        if(!qualified)std::cout<<"null";else
        {std::cout<<'[';for(unsigned m=0;m<d.m_nNumMorphChannels;++m){if(m)std::cout<<',';std::cout<<'[';first=true;for(float t:{0.f,.125f,.25f,.5f,.75f,.875f,1.f}){if(!first)std::cout<<',';first=false;std::cout<<std::bit_cast<unsigned>(asset->MorphWeight(m,t));}std::cout<<']';}std::cout<<']';}
        std::cout<<'}';
    }
    std::cout<<"]\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc==3&&std::string(argv[1])=="--inspect"){Inspect(argv[2]);return 0;}
        if(argc!=1)throw std::runtime_error("Usage: sanim_assets_tests [--inspect FILE]");
        Valid();Invalid();std::cout<<checks<<" SAnim asset checks passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

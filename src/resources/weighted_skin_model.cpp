#include "resources/weighted_skin_model.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <map>
namespace mscharged::resources
{
namespace
{
std::array<float,16> Matrix(Bytes file,std::size_t offset)
{
    std::array<float,16> result;
    for(unsigned i=0;i<16;++i)result[i]=F32(file,offset+i*4);
    Require(result[3]==0&&result[7]==0&&result[11]==0&&result[15]==1,"Skin matrix must be affine");
    return result;
}
bool Primitive(unsigned kind,unsigned count)
{
    switch(kind){case 0:return count>=3&&count%3==0;case 1:case 2:return count>=3;
        case 3:return count>=4&&count%4==0;default:return false;}
}
WeightedSkinModel Group(Bytes file,std::size_t begin,std::size_t end)
{
    std::map<unsigned,Chunk> fields;
    for(auto at=begin;at<end;)
    {
        const auto chunk=ReadChunk(file,at,end);
        Require(chunk.next<=end&&fields.size()<9&&fields.emplace(chunk.id,chunk).second,"Duplicate or excessive skin RLG chunks");
        at=chunk.next;
    }
    for(unsigned id:{0x1b016u,0x1b007u,0x1b006u,0x1b005u,0x1b004u,0x1b002u,0x1b003u,0x8001b008u})
        Require(fields.contains(id),"Skin RLG is missing a required chunk");
    Require(fields.size()==8,"Skin RLG contains an unqualified extra channel");
    const auto models=fields.at(0x1b003).payload,packets=fields.at(0x1b004).payload,
        streams=fields.at(0x1b005).payload,vertices=fields.at(0x1b006).payload,
        indices=fields.at(0x1b007).payload,parameters=fields.at(0x1b016).payload,matrices=fields.at(0x1b002).payload;
    Require(Records(models,12,1)==1,"Weighted specular skin profile requires one authored model per group");
    const auto count=Records(packets,48,4096),matrix_count=Records(matrices,64,4096);
    Records(streams,8,32768);Records(indices,2,MaximumAssetBytes/2);
    Require(count&&U32(models,4)==count,"Skin model packet count disagrees with its group");
    WeightedSkinModel out;out.hash=U32(models,0);out.packets.resize(count);
    auto skin=fields.at(0x8001b008).payload;const auto skin_end=std::size_t(skin.data()-file.data())+skin.size();
    unsigned maps=0;bool binds=false,morphs=false,metadata=false;
    for(auto at=std::size_t(skin.data()-file.data());at<skin_end;)
    {
        const auto chunk=ReadChunk(file,at,skin_end);Require(chunk.next<=skin_end,"Skin child exceeds its parent");
        const auto data=chunk.payload;
        switch(chunk.id)
        {
        case 0x1b009:
            Require(!metadata&&data.size()<=4096,"Duplicate or excessive skin metadata");metadata=true;
            out.metadata.assign(data.begin(),data.end());break;
        case 0x1b00a:
            Require(!binds,"Duplicate skin bind channel");binds=true;
            for(unsigned i=0,n=Records(data,68,4096);i<n;++i)out.binds.push_back({U32(data,i*68),Matrix(data,i*68+4)});
            break;
        case 0x1b00b:
        {
            Require(maps<count,"Skin has too many packet bone maps");const auto bones=Records(data,4,9);
            Require(bones>0,"Skin packet bone map is empty");auto& list=out.packets[maps++].bone_hashes;
            for(unsigned i=0;i<bones;++i)list.push_back(U32(data,i*4));break;
        }
        case 0x1b00c:
            Require(!morphs&&data.size()>=12,"Duplicate or truncated skin morph record");morphs=true;
            if(U32(data,0)!=0)throw UnsupportedResource("Weighted specular skin profile cannot execute morph channels");
            Require(data.size()==12&&U32(data,4)==16&&U32(data,8)==count,"Empty skin morph header disagrees with packet count/stride");break;
        default:throw UnsupportedResource("Unknown skin record is not qualified");
        }
        at=chunk.next;
    }
    Require(binds&&!out.binds.empty()&&maps==count,"Skin bind or per-packet bone records are incomplete");
    std::size_t total_vertices=0,total_indices=0;
    for(unsigned p=0;p<count;++p)
    {
        auto& packet=out.packets[p];const auto record=packets.subspan(p*48,48);
        packet.program=U32(record,16);packet.primitive=record[10];packet.raster=U32(record,28);
        if(packet.program!=0x22cadb20)throw UnsupportedResource("Only the authored GXSpecular weighted profile is qualified");
        packet.material=ReadSpecularSkinMaterial(Slice(parameters,U32(record,32),72));
        // Wii bytes24..31 are the source SkinMatrices pointer/size cache,
        // not asset offsets. The material decoder never follows these values.
        const auto unique=U16(record,8);const auto n=U32(record,4);
        Require(unique&&n<=65535&&Primitive(packet.primitive,n),"Invalid skin primitive/index count");
        Require((total_vertices+=unique)<=1024*1024&&(total_indices+=n)<=4*1024*1024,"Skin geometry budget exceeded");
        Require(U32(record,0)%2==0&&U32(record,12)%8==0,"Misaligned skin stream/index range");
        const auto index_data=Slice(indices,U32(record,0),std::size_t(n)*2);
        packet.indices.reserve(n);for(unsigned i=0;i<n;++i){auto index=U16(index_data,i*2);Require(index<unique,"Skin vertex index exceeds its packet");packet.indices.push_back(index);}
        Require(record[11]==7,"Weighted specular skin requires seven authored streams");
        constexpr std::array<unsigned,7> ids{1,2,4,4,4,7,5},strides{12,12,4,4,4,4,16};
        std::array<Bytes,7> data;
        for(unsigned s=0;s<7;++s)
        {
            auto stream=Slice(streams,U32(record,12)+s*8,8);
            // Stream byte 4 is an unused index in this material profile:
            // BindVertexArrays uses stream ordinals, dlMakeDisplayList uses id
            // and address, and glModelPacketGetStream searches id alone.
            Require(stream[5]==strides[s]&&stream[6]==ids[s]&&!stream[7],"Weighted specular skin stream layout differs from its original material");
            packet.stream_slots[s]=stream[4];
            data[s]=Slice(vertices,U32(stream,0),std::size_t(unique)*strides[s]);
        }
        const auto matrix=U32(record,24);Require(matrix<matrix_count,"Skin packet matrix index is out of bounds");packet.matrix=Matrix(matrices,std::size_t(matrix)*64);
        packet.vertices.reserve(unique);
        for(unsigned v=0;v<unique;++v)
        {
            WeightedSkinVertex vertex;
            for(unsigned c=0;c<3;++c){vertex.position[c]=F32(data[0],v*12+c*4);vertex.normal[c]=F32(data[1],v*12+c*4);}
            for(unsigned uv=0;uv<3;++uv)for(unsigned c=0;c<2;++c)vertex.uv[uv][c]=std::bit_cast<std::int16_t>(U16(data[2+uv],v*4+c*2));
            unsigned active=0;
            for(unsigned c=0;c<4;++c)
            {
                vertex.bones[c]=data[5][v*4+c];vertex.weights[c]=F32(data[6],v*16+c*4);
                Require(vertex.weights[c]>=0&&vertex.weights[c]<=1,"Skin weight is outside the checked authored [0,1] domain");
                if(vertex.weights[c]!=0)
                {
                    Require(vertex.bones[c]<packet.bone_hashes.size(),"Weighted specular skin bone index exceeds its packet map");++active;
                }
            }
            Require(active>0,"Weighted skin vertex has no active bone influence");
            packet.vertices.push_back(vertex);
        }
    }
    return out;
}
}
WeightedSkinModel ReadWeightedSkinModel(Bytes file,std::optional<std::uint32_t> selected)
{
    Require(!file.empty()&&file.size()<=MaximumAssetBytes,"Skin RLG is empty or exceeds 16 MiB");
    const auto root=ReadChunk(file,0,file.size());Require(root.next==file.size(),"Skin RLG has trailing bytes");
    std::vector<Chunk> groups;
    if(root.id==0x8001b000)groups.push_back(root);
    else if(root.id==0x8001b100)
    {
        const auto end=std::size_t(root.payload.data()-file.data())+root.payload.size();
        for(auto at=std::size_t(root.payload.data()-file.data());at<end;)
        {const auto c=ReadChunk(file,at,end);Require(c.id==0x8001b000&&c.next<=end&&groups.size()<4096,"Invalid skin collection group");groups.push_back(c);at=c.next;}
    }
    else throw UnsupportedResource("Skin reader requires an authored RLG group/collection");
    std::optional<WeightedSkinModel> result;
    for(const auto& group:groups)
    {
        const auto begin=std::size_t(group.payload.data()-file.data()),end=begin+group.payload.size();
        if(selected)
        {
            bool found=false;
            for(auto at=begin;at<end;){const auto c=ReadChunk(file,at,end);Require(c.next<=end,"Skin group child exceeds bounds");if(c.id==0x1b003){Require(c.payload.size()>=12,"Truncated skin model record");found=U32(c.payload,0)==*selected;}at=c.next;}
            if(!found)continue;
        }
        Require(!result,"Skin selection is ambiguous");result=Group(file,begin,end);
    }
    Require(bool(result),"Requested skin model is absent");return std::move(*result);
}
}

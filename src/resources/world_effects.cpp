#include "resources/world_effects.h"
namespace mscharged::resources
{
WorldEffectData::WorldEffectData(std::vector<WorldEffectRecord> records):records_(std::move(records)){}
WorldEffectData::Handle WorldEffectData::Decode(Bytes resident)
{
    const auto index=ReadWorldObjectIndex(resident);std::vector<WorldEffectRecord> result;
    bool parent=false;
    for(const auto& record:index)
    {
        if(record.type==0x10){parent=true;continue;}
        if(record.type==0x109)
        {
            Require(!parent,"World effect has unqualified parent payload");
            const auto bytes=Slice(resident,record.offset,record.size);WorldEffectRecord effect{};
            effect.id=record.id;effect.creation_flags=U32(bytes,0xc);
            Require(effect.creation_flags==1,"Unsupported world-effect lifecycle flags");
            effect.animation_node=std::bit_cast<std::int32_t>(U32(bytes,0x14));
            effect.animated=record.animated||effect.animation_node!=-1;
            for(unsigned i=0;i<16;++i)effect.matrix[i]=F32(bytes,0x20+4*i);
            Require(effect.matrix[3]==0&&effect.matrix[7]==0&&effect.matrix[11]==0&&effect.matrix[15]==1,"World-effect transform must be affine");
            effect.interval=F32(bytes,0x60);effect.repeat_offset=F32(bytes,0x64);
            effect.group=U32(bytes,0x70);effect.probability=U32(bytes,0x74);
            effect.count=std::bit_cast<std::int32_t>(U32(bytes,0x78));effect.timing=std::bit_cast<std::int32_t>(U32(bytes,0x7c));
            Require(effect.interval>=0&&effect.count>=-1&&effect.probability<=100,"Invalid world-effect schedule");
            Require(bytes[0x9c]<=1,"Invalid world-effect visibility flag");effect.always_visible=bytes[0x9c]!=0;
            result.push_back(effect);
        }
        parent=false;
    }
    return Handle(new WorldEffectData(std::move(result)));
}
}

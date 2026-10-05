#include "resources/audio_calculation.h"
#include "resources/chunk_reader.h"
#include "Game/Audio/AudioCalculationSteps.h"
#include <cmath>

namespace mscharged::resources
{
AudioCalculationInitial::Handle ReadAudioCalculationInitial(Bytes bytes)
{
    Require(bytes.size()<=MaximumAssetBytes,"Audio calculation metadata exceeds its budget");
    const auto root=ReadChunk(bytes,0,bytes.size());
    Require(root.id==0x80000001&&root.next==bytes.size(),"Invalid audio calculation bundle");
    Bytes section;
    for(std::size_t at=root.payload.data()-bytes.data(),end=at+root.payload.size();at<end;)
    {
        const auto chunk=ReadChunk(bytes,at,end);Require(chunk.next<=end,"Audio calculation child padding is invalid");
        if(chunk.id==0x80023400){Require(section.data()==nullptr,"Duplicate audio calculation section");section=chunk.payload;}
        at=chunk.next;
    }
    Require(section.data()!=nullptr,"Audio calculation section is absent");
    const auto start=std::size_t(section.data()-bytes.data()),end=start+section.size();
    const auto header=ReadChunk(bytes,start,end);
    Require(header.id==0x23401&&header.payload.size()==12,"Invalid audio calculation header");
    const auto count=U32(header.payload,0),base=U32(header.payload,4);
    Require(count&&count<=4096&&base%4==0&&std::uint64_t(base)+count*24<=0x100000000ULL,
        "Invalid audio calculation definition range");
    const auto records=ReadChunk(bytes,header.next,end);
    Require(records.id==0x23402&&records.payload.size()==count*24&&records.next==end,
        "Invalid audio calculation definition extent");
    std::vector<AudioCalculationDefinition> definitions(count);
    std::vector<AudioCalculationSlider> sliders(count);
    std::vector<s32> indices(count);
    for(unsigned i=0;i<count;++i)
    {
        const auto b=records.payload.subspan(i*24,24);
        // References target a definition's stored slider index, not a host pointer.
        Require(U32(b,0)<count,"Audio calculation stored index is invalid");indices[i]=U32(b,0);
        const float initial=F32(b,8);Require(std::isfinite(initial),"Nonfinite audio calculation initial target");
        definitions[i].initialValue=initial;
        s32** fields[]={&definitions[i].field_0C,&definitions[i].field_10,&definitions[i].field_14};
        for(unsigned j=0;j<3;++j)
        {
            const auto reference=U32(b,12+j*4);
            if(!reference){*fields[j]=nullptr;continue;}
            Require(reference>=base&&(reference-base)%24==0&&(reference-base)/24<count,
                "Audio calculation reference is outside a definition");
            *fields[j]=&indices[(reference-base)/24];
        }
    }
    // Exact CreateSliders ordering, including its repeated SetTarget, followed by
    // one actual original Transition::Update through the shared table loop.
    for(unsigned i=0;i<count;++i)
    {
        sliders[i].Initialize(&definitions[i]);
        sliders[i].SetTarget(definitions[i].initialValue,0.0f);
    }
    AudioCalculationTable table{count,definitions.data(),sliders.data()};
    AudioUpdateCalculationTable(table,0.0f);
    auto result=std::shared_ptr<AudioCalculationInitial>(new AudioCalculationInitial);
    result->values_.reserve(count);
    for(auto& slider:sliders)result->values_.push_back(slider.GetValue());
    return result;
}
}

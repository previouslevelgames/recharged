#include "resources/audio_volume.h"
#include "resources/chunk_reader.h"
namespace mscharged::resources
{
AudioVolumeProfile::Handle ReadAudioVolumeProfile(Bytes bytes)
{
    auto initial=ReadAudioCalculationInitial(bytes);
    Require(initial->Size()==5,"Audio volume controls require the original five categories");
    const auto root=ReadChunk(bytes,0,bytes.size());Bytes section;
    for(std::size_t at=root.payload.data()-bytes.data(),end=at+root.payload.size();at<end;)
    {const auto child=ReadChunk(bytes,at,end);if(child.id==0x80023400)section=child.payload;at=child.next;}
    const auto at=std::size_t(section.data()-bytes.data()),end=at+section.size();
    const auto header=ReadChunk(bytes,at,end),records=ReadChunk(bytes,header.next,end);const auto base=U32(header.payload,4);
    for(unsigned i=0;i<5;++i)
    {
        const auto record=records.payload.subspan(i*24,24);
        Require(U32(record,0)==i&&F32(record,8)==0.0f&&U32(record,12)==(i?base:0),
            "Audio category indices, targets or global dependency are unsupported");
        // The remaining sibling/child references are validated by the general
        // reader. Only field0C participates in the selected original update.
    }
    auto out=std::shared_ptr<AudioVolumeProfile>(new AudioVolumeProfile);out->initial_=std::move(initial);return out;
}
}

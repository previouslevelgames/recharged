#pragma once
#include "resources/audio_bank.h"

namespace mscharged::resources
{
struct AudioStreamTrack
{
    std::uint32_t source, offset, byte_size, hash, channels;
};
// Checked metadata only. The encoded wave remains an open native file and is
// read in bounded blocks; no 39 MiB wave image is retained by this owner.
class AudioStreamBank
{
public:
    using Handle=std::shared_ptr<const AudioStreamBank>;
    const AudioCueCatalog& CuesByKey()const{return *map_;}
    const auto& Cues()const{return cues_;}
    const auto& Voices()const{return voices_;}
    const auto& Sequences()const{return sequences_;}
    const auto& Sounds()const{return sounds_;}
    const auto& Tracks()const{return tracks_;}
    std::uint32_t BlockBytes()const{return block_;}
private:
    AudioStreamBank()=default;
    AudioCueCatalog::Handle map_;
    std::vector<AudioBankCue> cues_;
    std::vector<AudioBankVoice> voices_;
    std::vector<AudioBankSequence> sequences_;
    std::vector<AudioBankSound> sounds_;
    std::vector<AudioStreamTrack> tracks_;
    std::uint32_t block_=0;
    friend Handle ReadAudioStreamBank(Bytes,std::uint64_t);
};
AudioStreamBank::Handle ReadAudioStreamBank(Bytes resbun,std::uint64_t wave_bytes);
struct AudioStreamChannelHeader
{
    std::uint32_t samples,nibbles,rate;
    std::array<std::int16_t,16> coefficients;
    std::uint16_t predictor_scale;
    std::int16_t history1,history2;
};
class AudioStreamFormat
{
public:
    using Handle=std::shared_ptr<const AudioStreamFormat>;
    const auto& Channels()const{return channels_;}
    const AudioStreamTrack& Track()const{return bank_->Tracks().at(track_);}
    std::uint32_t BlockBytes()const{return bank_->BlockBytes();}
    std::uint32_t Blocks()const{return blocks_;}
    // Original refill rounds the final encoded channel span to32bytes. These
    // padding samples participate in its loop period, beyond nominal DSP samples.
    std::uint32_t CycleFrames()const{return cycle_frames_;}
    // Absolute byte offset in the retained open .nlxwb file.
    std::uint32_t BlockOffset(std::uint32_t block,unsigned channel)const;
private:
    AudioStreamFormat()=default;
    AudioStreamBank::Handle bank_;
    std::uint32_t track_=0,blocks_=0,cycle_frames_=0;
    std::array<AudioStreamChannelHeader,2> channels_;
    friend Handle ReadAudioStreamFormat(AudioStreamBank::Handle,std::uint32_t,Bytes);
};
// Stereo IDSP prefix + two96-byte DSP headers. Authored embedded DSP loops,
// alternate starts, gain, rate/channel mismatches remain explicit unsupported.
AudioStreamFormat::Handle ReadAudioStreamFormat(AudioStreamBank::Handle,std::uint32_t track,Bytes header);
// Decoder state is a value: decode copies it and publishes only on success.
// At a file wrap the source AX stream retains histories; Reset is distinct.
struct AudioStreamDecoder
{
    std::array<std::int16_t,2> history1{},history2{};
    std::uint32_t next_block=0;
};
AudioStreamDecoder BeginAudioStream(AudioStreamFormat::Handle);
struct AudioStreamPcmBlock
{
    std::uint32_t frames=0;
    std::vector<std::int16_t> stereo;
};
AudioStreamPcmBlock DecodeAudioStreamBlock(AudioStreamFormat::Handle,AudioStreamDecoder&,
                                         Bytes left,Bytes right);
}

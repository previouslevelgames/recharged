#pragma once
#include "runtime/audio_output.h"
#include "runtime/audio_volume.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace mscharged
{
struct AudioVoiceHandle
{
    std::uint64_t owner = 0;
    std::uint32_t slot = 0, generation = 0;
    friend bool operator==(const AudioVoiceHandle&,const AudioVoiceHandle&) = default;
};
struct AudioVoicesOptions
{
    unsigned capacity = 64; // Native bound; original pool starts64 and can grow16.
    std::size_t input_byte_budget = 64*1024*1024;
    std::uint32_t device_id = 0;
    AudioCategoryVolumes::Handle category_volumes; // Optional explicit live authority.
};
struct AudioVoiceStatus
{
    int state = 0, internal_state = 0; // Original exposed/internal values, not audibility.
    bool has_voice = false, failed = false;
    unsigned sample = 0;
    std::size_t input_bytes = 0;
    float volume_db=0,input_gain=1;
};
// Retained native resident-source owner. This does not initialize AudioBackend,
// AX/AI/WPAD, register original sound-bank slots or fabricate cue readiness.
// Only the qualified one-shot resident output profile is admitted. Optional
// category authority permits real SDL input-gain changes over retained PCM. Selection/RNG belongs to the caller and is never consumed here.
// All operations/destruction require the creating thread, before SDL_Quit.
class AudioVoices
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit AudioVoices(AudioVoicesOptions = {});
    ~AudioVoices();
    AudioVoices(const AudioVoices&) = delete;
    AudioVoices& operator=(const AudioVoices&) = delete;
    // Prepare immutable PCM and a real paused output before slot publication.
    // Failed creation leaves all existing voices and LastCreated unchanged.
    AudioVoiceHandle Create(const AudioBankSelectionResult&,
                            resources::AudioCalculationInitial::Handle);
    bool Prepare(AudioVoiceHandle); // Original returns false; internal state becomes3.
    bool Play(AudioVoiceHandle,unsigned play_count = 1); // Pending5; no output starts yet.
    void Stop(AudioVoiceHandle); // Original pending5 stop is a no-op; see Destroy.
    void SetVolume(AudioVoiceHandle,float volume_db); // Live authority only.
    void ServiceAudio(); // Original source insertion order, actual output start/consumption.
    int PollState(AudioVoiceHandle); // Original UpdateState, including voice release at6.
    AudioVoiceStatus Status(AudioVoiceHandle) const; // Read-only; no implicit service/poll.
    std::vector<AudioVoiceHandle> Sources() const;
    AudioVoiceHandle LastCreated() const;
    // Explicit unconditional native cancellation. Remove before destroying the
    // output; invalidate this generation. Handles/resources survive other voices.
    void Destroy(AudioVoiceHandle);
    void Release(); // Idempotent; cancel every owned output before releasing source assets.
};
}

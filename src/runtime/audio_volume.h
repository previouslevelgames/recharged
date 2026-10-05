#pragma once
#include "resources/audio_volume.h"
#include <array>
#include <memory>
namespace mscharged
{
enum class AudioCategory { Music, Sfx, Voice };
struct AudioCategorySnapshot
{
    std::array<int,3> settings{};
    std::array<float,3> targets{},values{};
    std::uint64_t frame=0;
};
// Explicit host settings authority over original category transitions. The
// caller supplies genuine current0..10 settings; no GameInfo defaults/save are
// synthesized. One caller advances Update(frame,dt), before audio consumers;
// those consumers only read. All operations/destruction are thread-affine.
class AudioCategoryVolumes
{
    struct Implementation;std::unique_ptr<Implementation> impl_;
public:
    using Handle=std::shared_ptr<AudioCategoryVolumes>;
    AudioCategoryVolumes(resources::AudioVolumeProfile::Handle,std::array<int,3> settings);
    ~AudioCategoryVolumes();
    AudioCategoryVolumes(const AudioCategoryVolumes&)=delete;
    AudioCategoryVolumes& operator=(const AudioCategoryVolumes&)=delete;
    void Set(AudioCategory,int); // Original clamp0..10, set target, elapsed0.
    void SetAll(std::array<int,3>); // Original Music/SFX/Voice order.
    void Update(std::uint64_t frame,float delta); // Strictly increasing frame ID.
    float Value(std::uint32_t slider)const;
    AudioCategorySnapshot Snapshot()const;
    void RequireCompatible(resources::AudioCalculationInitial::Handle)const;
    resources::AudioCalculationInitial::Handle Initial()const;
};
}

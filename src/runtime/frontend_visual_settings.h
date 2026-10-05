#pragma once
#include <cstdint>
#include <memory>
#include <thread>

namespace mscharged
{
struct FrontendVisualSettingsSnapshot
{
    bool auto_zoom;
    float zoom;
    std::uint64_t revision;
    bool operator==(const FrontendVisualSettingsSnapshot&) const=default;
};
// Explicit retained desired settings. The caller supplies actual values; this
// does not claim GameInfo/save initialization or apply them to a gameplay camera.
class FrontendVisualSettings
{
    const std::thread::id thread_=std::this_thread::get_id();
    FrontendVisualSettingsSnapshot value_;
    void Ready()const;
public:
    using Handle=std::shared_ptr<FrontendVisualSettings>;
    FrontendVisualSettings(bool auto_zoom,float zoom);
    FrontendVisualSettingsSnapshot Snapshot()const;
    void Set(const FrontendVisualSettingsSnapshot& expected,bool auto_zoom,float zoom);
};
}

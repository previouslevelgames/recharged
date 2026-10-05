#pragma once
#include <functional>
#include <memory>
#include <optional>
namespace mscharged
{
struct FrontendTitleDimmingStatus
{
    bool armed=false;
    std::optional<unsigned> pending;
};
// Retained source-global setDimmingTime authority. The provider must apply the
// actual host/render idle policy before returning true: mode2/mode0 thresholds
// are54000/18000 NTSC or45000/15000 PAL retraces (nominal900/300seconds).
// No default provider exists; the source time counter survives policy changes.
// Keep one authority across repeated Title owners; successful source Press0
// clears the guard, while Title destruction itself does not alter that guard.
class FrontendTitleDimming
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit FrontendTitleDimming(std::function<bool(unsigned)> = {});
    ~FrontendTitleDimming();
    FrontendTitleDimming(const FrontendTitleDimming&)=delete;
    FrontendTitleDimming& operator=(const FrontendTitleDimming&)=delete;
    bool Admit(unsigned);
    FrontendTitleDimmingStatus Status()const;
};
}

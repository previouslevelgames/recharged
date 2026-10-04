#pragma once
#include "resources/frontend_fonts.h"
#include <array>
#include <exception>
#include <thread>

namespace mscharged
{
enum class FrontendLanguage { English, NAFrench, NASpanish };
struct FrontendVisualAssets
{
    std::shared_ptr<const resources::Localization> localization;
    std::shared_ptr<const resources::FrontendFont> text, heading;
};
// Original USA frontend language paths and FontLoading's non-Japanese eur fonts.
// Loads through NL services, publishing only after all three files decode.
// The load owner must be destroyed before NL shutdown. Retained results own all
// their decoded data and may outlive NL/memory sessions.
class FrontendVisualLoad
{
    struct Request { FrontendVisualLoad* owner = nullptr; unsigned index = 0, token = 0; bool complete = false; };
    std::thread::id thread_ = std::this_thread::get_id();
    std::array<Request, 3> requests_{};
    std::shared_ptr<FrontendVisualAssets> assets_;
    std::exception_ptr error_;
    std::uint32_t language_hash_ = 0;
    unsigned completed_mask_ = 0;
    bool terminal_ = false, cancelled_ = false;
    void CheckThread() const;
    void Drain();
    static void Complete(void*, unsigned long, void*);
public:
    explicit FrontendVisualLoad(FrontendLanguage language);
    ~FrontendVisualLoad();
    FrontendVisualLoad(const FrontendVisualLoad&) = delete;
    FrontendVisualLoad& operator=(const FrontendVisualLoad&) = delete;
    void Poll();
    void Service();
    void Cancel();
    bool Ready() const; // Terminal, including cancellation/failure. Result reports errors.
    unsigned CompletedMask() const;
    std::shared_ptr<const FrontendVisualAssets> Result() const;
};
}

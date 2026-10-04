#pragma once
#include "resources/frontend_fonts.h"
#include "runtime/frontend_font_load.h"
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
    std::vector<std::shared_ptr<const resources::FrontendFont>> font_registration_order;
};
// Original USA frontend language paths and FontLoading's non-Japanese eur fonts.
// Loads localization and source-ordered font bundle stages through NL services,
// publishing only after all three assets decode. No GL registry readiness.
// The load owner must be destroyed before NL shutdown. Retained results own all
// their decoded data and may outlive NL/memory sessions.
class FrontendVisualLoad
{
    struct Request { FrontendVisualLoad* owner = nullptr; unsigned index = 0, token = 0; bool complete = false; };
    std::thread::id thread_ = std::this_thread::get_id();
    std::array<Request, 1> requests_{};
    std::unique_ptr<FrontendFontLoad> fonts_;
    std::shared_ptr<FrontendVisualAssets> assets_;
    std::exception_ptr error_;
    std::uint32_t language_hash_ = 0;
    unsigned completed_mask_ = 0;
    bool terminal_ = false, cancelled_ = false;
    void CheckThread() const;
    void CheckMutation() const;
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

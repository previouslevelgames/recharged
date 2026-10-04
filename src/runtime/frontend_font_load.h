#pragma once
#include "resources/frontend_fonts.h"
#include <span>

namespace mscharged
{
struct FrontendFontRequest
{
    std::string path, texture_base, alias;
};
enum class FrontendFontLoadState { Loading, Ready, Failed, Cancelled };
struct FrontendFontLoadProgress
{
    unsigned requested_fonts=0, completed_fonts=0;
    unsigned requested_reads=0, completed_reads=0;
    unsigned requested_pages=0, completed_pages=0;
    std::uint32_t completed_mask=0;
};
// One native batch of original font-manager slots, with checked NL range reads:
// header -> directory -> descriptor -> source-ordered texture pages. Ready means
// retained decoded assets; it does not mean GL pool registration/FontManager or
// FE readiness. Service/Cancel/destroy on the creating NL thread before shutdown.
// A failed/cancelled batch never exposes partial fonts. Results outlive the owner
// and game arenas. Up to16 fonts and16 page reads/font; unsupported profiles fail.
class FrontendFontLoad
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using ResultType=std::vector<std::shared_ptr<const resources::FrontendFont>>;
    explicit FrontendFontLoad(std::span<const FrontendFontRequest>);
    ~FrontendFontLoad();
    FrontendFontLoad(const FrontendFontLoad&)=delete;
    FrontendFontLoad& operator=(const FrontendFontLoad&)=delete;
    void Poll();
    void Service(); // Shared-pump exceptions cancel this batch and propagate.
    void Cancel();
    FrontendFontLoadState State() const;
    FrontendFontLoadProgress Progress() const;
    const ResultType& Result() const;
};
}

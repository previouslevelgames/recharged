#pragma once
#include "resources/frontend_images.h"

namespace mscharged
{
// AsyncLoading.cpp's actual FE profiles. Main loads MainUI.Dmn permanently;
// InGame loads InGameUI.Res permanently and InGameUI.Dmn on demand. The USA
// boot context selects BootLoadingUI.res explicitly; it does not merge MainUI
// resources or imply the original mini-bundle manager has initialized.
enum class FrontendImageProfile { Main, InGame, BootLoading };
enum class FrontendImageState { Idle, Loading, Ready, Failed, Cancelled };
class FrontendImageLoad
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendImageLoad();
    ~FrontendImageLoad();
    FrontendImageLoad(const FrontendImageLoad&) = delete;
    FrontendImageLoad& operator=(const FrontendImageLoad&) = delete;
    // Replaces pending work; previous Current survives failure/cancellation.
    // Only resource metadata is copied. Static requests read actual NL files;
    // text-only/dynamic-only requests need no image bundle and submit no work.
    void Begin(const resources::FrontendScene& scene, FrontendImageProfile profile = FrontendImageProfile::Main);
    void Poll();
    void Service();
    void Cancel();
    void Unload();
    FrontendImageState State() const;
    unsigned CompletedFiles() const;
    resources::FrontendImageCatalog::Handle Current() const;
    resources::FrontendImageCatalog::Handle Result() const;
    // Service/mutate/destroy on the creating thread before NL/arena shutdown.
    // External catalog/texture handles own host storage and survive teardown.
};
}

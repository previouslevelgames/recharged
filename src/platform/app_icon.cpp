#include "platform/app_icon.h"
#include "mscharged/app_icon_bytes.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

namespace mscharged::platform
{
void SetApplicationIcon(SDL_Window* window)
{
    auto* stream = SDL_IOFromConstMem(application_icon_png, sizeof(application_icon_png));
    auto* surface = stream ? SDL_LoadPNG_IO(stream, true) : nullptr;
    if (!surface)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot decode application icon: %s", SDL_GetError());
        return;
    }
    // SDL also sets the Dock image on macOS. Some headless/compositor drivers
    // do not support window icons; that must not prevent the game from starting.
    SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
}
}

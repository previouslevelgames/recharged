// MSCHARGED_DEBUG_LAYERS_OFF=DepthOfField,Warble,... switches the game's own
// rendering-layer tweaks (eCLV_<name>Enabled, /Rendering/RLView Toggles) off
// before the original main starts, to find which layer renders differently on
// a platform. Unset, it changes nothing. Diagnostic only.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Layer debug toggles belong to the isolated original game module
#endif
#include "Game/Render/RLViewLayers.h"
#include "Game/TweakValue.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct LayerToggle {
    const char* name;
    TweakValueBool* tweak;
};
#define LAYER(name) {#name, &eCLV_##name##Enabled}
const LayerToggle kLayers[] = {
    LAYER(ImpostorTexture), LAYER(ShadowTexture), LAYER(GrabTexture), LAYER(PictureInPicture),
    LAYER(PictureInPictureAlpha), LAYER(NoFog), LAYER(Shadowed), LAYER(WorldShadowed), LAYER(Unshadowed),
    LAYER(MegastrikeBackground), LAYER(ImpostorOut), LAYER(Characters), LAYER(PeachPhoto3D), LAYER(MoreCharacters),
    LAYER(WorldAlphaBlended), LAYER(HighRange3D), LAYER(HighRange3DNoFog), LAYER(HighRangeChain), LAYER(HighRange2D),
    LAYER(BigBlackPolygon), LAYER(ShadowVolume), LAYER(ShadowVolumeBlend), LAYER(UnsortedPerspective),
    LAYER(DepthOfField), LAYER(LingeringParticles), LAYER(Particles), LAYER(BallChargeAlphaBlended), LAYER(CoPlanar),
    LAYER(InvisiblePlane), LAYER(ElectricFence), LAYER(PreWarble), LAYER(Warble), LAYER(WarbleBlend),
    LAYER(CameraSpace), LAYER(ScreenBlur), LAYER(ScreenBlur2), LAYER(ScreenGrab), LAYER(FrontEnd),
    LAYER(UnsortedOrtho640), LAYER(UnsortedSquareOrtho),
};
#undef LAYER

std::vector<TweakValueBool*> sListed; // from MSCHARGED_DEBUG_LAYERS_OFF
bool sListedOff = false;
int sCycle = -1; // F10: index of the one layer switched off, -1 = none

void SetLayer(TweakValueBool* tweak, bool on) { tweak->ParseValue(on ? "true" : "false"); }
} // namespace

extern "C" __attribute__((visibility("default"))) void charged_apply_debug_layer_toggles() {
    const char* list = std::getenv("MSCHARGED_DEBUG_LAYERS_OFF");
    if (!list || !*list) return;
    std::string names(list);
    std::size_t start = 0;
    while (start <= names.size()) {
        const auto end = std::min(names.find(',', start), names.size());
        const auto name = names.substr(start, end - start);
        start = end + 1;
        if (name.empty()) continue;
        bool found = false;
        for (const auto& layer : kLayers)
            if (name == layer.name) {
                SetLayer(layer.tweak, false);
                sListed.push_back(layer.tweak);
                found = true;
            }
        std::fprintf(stderr, found ? "Debug: rendering layer %s off\n" : "Debug: unknown rendering layer %s\n",
                     name.c_str());
    }
    sListedOff = !sListed.empty();
}

// F9: switch the MSCHARGED_DEBUG_LAYERS_OFF layers back on / off again.
extern "C" __attribute__((visibility("default"))) void charged_toggle_debug_layers() {
    if (sListed.empty()) {
        std::fprintf(stderr, "Debug: F9 needs MSCHARGED_DEBUG_LAYERS_OFF=Layer,...\n");
        return;
    }
    sListedOff = !sListedOff;
    for (auto* tweak : sListed) SetLayer(tweak, !sListedOff);
    std::fprintf(stderr, "Debug: listed rendering layers %s\n", sListedOff ? "off" : "on");
}

// F10: switch the next single layer off (the previous one back on); after the
// last layer, all are on again.
extern "C" __attribute__((visibility("default"))) void charged_cycle_debug_layer() {
    constexpr int count = int(sizeof(kLayers) / sizeof(kLayers[0]));
    if (sCycle >= 0) SetLayer(kLayers[sCycle].tweak, true);
    sCycle = sCycle + 1 < count ? sCycle + 1 : -1;
    if (sCycle >= 0) {
        SetLayer(kLayers[sCycle].tweak, false);
        std::fprintf(stderr, "Debug: only rendering layer %s off (%d/%d)\n", kLayers[sCycle].name, sCycle + 1, count);
    } else {
        std::fprintf(stderr, "Debug: all rendering layers on\n");
    }
}

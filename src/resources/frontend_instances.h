#pragma once
#include "resources/frontend_scene.h"
#include <span>
#include <string_view>

namespace mscharged::resources
{
enum class FrontendNodeKind { Presentation, Slide, Instance };
struct FrontendNode
{
    FrontendNodeKind kind = FrontendNodeKind::Presentation;
    std::uint32_t id = 0;
    bool operator==(const FrontendNode&) const = default;
};
// Original TLAT values; unlike original unchecked casts, a requested type is
// verified before a native caller receives the node. No default instances.
enum class FrontendNodeType : int { Any = -1, Slide = 0, Layer = 1, Image = 2, Text = 3, Component = 4, Group = 5 };
using FrontendPath = std::array<std::uint32_t,6>;
std::uint32_t FrontendLowerHash(std::string_view);
FrontendPath FrontendNamedPath(std::span<const std::string_view> names);
// First ring hash match. Presentation/component named slides take precedence
// over fallback children of the active slide; ordinary nodes search children.
// A zero next level terminates the original six-level path. Names use lowerhash.
std::optional<FrontendNode> FindFrontendNode(const FrontendScene&, FrontendNode root,
    const FrontendPath&, FrontendNodeType expected = FrontendNodeType::Any);

enum class FrontendInstanceProperty
{
    Visible, AssetVisible, Position, Rotation, Scale, Pivot, Colour,
    UVX, UVY, UVWidth, UVHeight, StringId, String, ImageResource,
};
struct FrontendInstanceChange
{
    std::uint32_t instance = 0;
    FrontendInstanceProperty property = FrontendInstanceProperty::Visible;
    bool flag = false; // Visible / AssetVisible.
    std::array<float,3> vector{}; // Position / Rotation / Scale / Pivot.
    float scalar = 0; // One UV channel.
    std::array<std::uint8_t,4> colour{};
    std::string string_id;
    std::u16string text; // Owned SetString payload; embedded NUL is rejected.
    FrontendReference image_resource; // Null retains original SetTextureResource no-op.
};
// Ordered original setter effects over a copied graph; commit only on success.
// Does not execute a handler, rebind fonts/textures, or evaluate animation.
void ApplyFrontendInstanceChanges(FrontendScene&, std::span<const FrontendInstanceChange>);
struct FrontendLoadingSetup
{
    std::uint32_t transition_component = 0; // Instance, not its shared library ID.
    bool widescreen = false;
    // Original BaseLoadingScene::SceneCreated next requires a real HBMManager.
    // This result never means the complete handler or FE manager became ready.
};
}

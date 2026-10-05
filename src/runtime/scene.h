#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace mscharged
{
class FrontendMenuScenes;
struct FrontendPointerViewport;
// Optional embedding/diagnostic callbacks. Input remains inside the original
// controlled scene window; presentation is observed only after GPU success.
struct ScenePreviewHooks
{
    std::function<void(FrontendMenuScenes&, SDL_Window*)> menu_input;
    std::function<void(const FrontendMenuScenes&, const FrontendPointerViewport&)> menu_presented;
};
struct SceneOptions
{
    std::string model = "/Art/objects/gameplay/ball.rlg";
    std::string textures = "/Art/objects/gameplay/ball.rlt";
    std::optional<std::uint32_t> model_id;
    std::optional<std::string> world; // Compressed world resource with an explicit model ID.
    std::optional<std::string> world_res; // Resident world containing selected object instances.
    std::vector<std::uint32_t> object_ids;
    std::optional<std::string> camera; // Authored CAM track; preserves model world coordinates.
    std::optional<std::string> nis_primary, nis_secondary; // First embedded CAM in each explicit NIS.
    std::optional<float> pip_expand; // Start the original expansion immediately, in seconds.
    std::optional<std::string> shadow_textures;
    std::optional<std::uint32_t> shadow_id;
    bool frontend_world = false; // Original FE paths, automatic supported-static subset.
    std::optional<std::string> frontend_layout; // Stored text inspection; no FE timeline or menu handlers.
    std::optional<std::string> frontend_frame; // Authored static frame, with explicit unsupported branches.
    std::optional<std::string> frontend_slide; // Optional stored presentation slide name.
    std::optional<std::string> frontend_images; // Explicit Main/InGame/BootLoading context; defaults to Main.
    std::optional<std::string> frontend_pointer; // Observe original pointer events for a rendered instance path.
    bool frontend_animate = false; // Checked authored timeline; no scene handlers or menu transitions.
    bool frontend_boot = false; // Retail boot handler through its qualified service boundary.
    bool frontend_main = false; // Selected original Main/Options/Audio/Visual flow; complete startup/services pending.
    bool frontend_options = false; // Original Options/NAV with Audio/Visual and return to Main.
    bool character_shock = false; // Authored Bowser shock mesh and FE bone animation diagnostic.
    bool particles = false; // Qualified authored controller groups; no complete effects manager.
    bool debug_camera = false; // Original DebugCam with diagnostic desktop controls.
    bool no_world_culling = false; // Diagnostic reference: submit every selected world object.
    bool unlit = false;
    unsigned frames = 0; // Zero keeps the preview open until Escape/window close.
    std::optional<unsigned> frame_timeout; // Explicit wall-clock bound for long diagnostics; defaults30s.
};
int RunScenePreview(int argc, char** argv, const std::filesystem::path& config, const SceneOptions& options,
    const ScenePreviewHooks* hooks = nullptr);
}

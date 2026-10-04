#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mscharged
{
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
    bool frontend_animate = false; // Checked authored timeline; no scene handlers or menu transitions.
    bool frontend_boot = false; // Retail boot handler through its qualified service boundary.
    bool particles = false; // Qualified authored controller groups; no complete effects manager.
    bool debug_camera = false; // Original DebugCam with diagnostic desktop controls.
    bool no_world_culling = false; // Diagnostic reference: submit every selected world object.
    bool unlit = false;
    unsigned frames = 0; // Zero keeps the preview open until Escape/window close.
};
int RunScenePreview(int argc, char** argv, const std::filesystem::path& config, const SceneOptions& options);
}

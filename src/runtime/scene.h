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
    std::optional<std::string> shadow_textures;
    std::optional<std::uint32_t> shadow_id;
    bool no_world_culling = false; // Diagnostic reference: submit every selected world object.
    bool unlit = false;
    unsigned frames = 0; // Zero keeps the preview open until Escape/window close.
};
int RunScenePreview(int argc, char** argv, const std::filesystem::path& config, const SceneOptions& options);
}

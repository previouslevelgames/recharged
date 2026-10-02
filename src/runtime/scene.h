#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mscharged
{
struct SceneOptions
{
    std::string model = "/Art/objects/gameplay/ball.rlg";
    std::string textures = "/Art/objects/gameplay/ball.rlt";
    std::optional<std::uint32_t> model_id;
    std::optional<std::string> shadow_textures;
    std::optional<std::uint32_t> shadow_id;
    bool unlit = false;
    unsigned frames = 0; // Zero keeps the preview open until Escape/window close.
};
int RunScenePreview(int argc, char** argv, const std::filesystem::path& config, const SceneOptions& options);
}

#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <optional>
#include <string>
#include <vector>
namespace mscharged::resources
{
using FrontendReference = std::optional<std::uint32_t>;
struct FrontendAttributes
{
    std::array<float,3> position{}, rotation{}, scale{}, pivot{};
    std::array<std::uint8_t,4> colour{};
    std::array<float,4> uv{};
    bool visible = false;
};
struct FrontendResource
{
    std::uint32_t offset, type, hash, file_block;
    bool saved_valid;
};
struct FrontendLibraryObject
{
    std::uint32_t offset, type, hash;
    std::string name;
    FrontendAttributes attributes;
    FrontendReference resource, active_slide;
    std::vector<std::uint32_t> slides;
    std::array<float,2> text_box{};
    std::array<std::uint8_t,4> text_effect_colour{};
};
struct FrontendInstance
{
    std::uint32_t offset, type, hash, overload_flags;
    std::uint16_t priority;
    std::string name;
    float start, duration;
    bool visible;
    FrontendAttributes attributes;
    FrontendReference library, resource;
    std::vector<std::uint32_t> children;
    std::uint32_t image_blend = 0; // Original image field_0x94; qualified by the renderer.
    std::uint32_t localization_hash = 0, text_overload_flags = 0, draw_options = 0;
    std::array<float,2> text_box{};
    std::array<std::uint8_t,4> text_effect_colour{};
    std::u16string text;
    bool text_scissor = false;
    std::array<std::uint16_t,4> text_scissor_box{};
};
struct FrontendAnimationKey
{
    std::uint32_t offset = 0;
    // point, outgoing controls 1/2, time; scalar keys use channel zero.
    std::array<std::array<float,4>,3> channels{};
};
struct FrontendAnimation
{
    std::uint32_t offset = 0, target = 0, type = 0;
    std::uint16_t cast = 0;
    std::vector<FrontendAnimationKey> keys;
};
struct FrontendSlide
{
    std::uint32_t offset, hash, play_mode;
    std::string name;
    float start, duration, time;
    bool frozen, animated;
    bool animation_evaluated = false; // Set only by checked timeline evaluation.
    std::vector<std::uint32_t> animations;
    std::vector<std::uint32_t> children;
};
struct FrontendScene
{
    std::uint32_t id;
    std::size_t relocation_count;
    float presentation_time;
    FrontendReference active_slide;
    std::vector<std::uint32_t> presentation_slides;
    std::vector<FrontendResource> resources;
    std::vector<FrontendLibraryObject> library;
    std::vector<FrontendInstance> instances;
    std::vector<FrontendSlide> slides;
    std::vector<FrontendAnimation> animations;
};
// Wii FENL v1 graph, decoded into owned values and checked file-relative IDs.
// No host pointer relocation, game object construction or timeline execution.
// Library references may be shared; owning slide/instance rings may not alias.
// Component dependencies are acyclic and bounded across all stored slides.
// Animation/key rings retain file-relative IDs and exact float values. Timeline
// execution is separate; static layout excludes unevaluated animated slides.
FrontendScene ReadFrontendScene(Bytes file);
}

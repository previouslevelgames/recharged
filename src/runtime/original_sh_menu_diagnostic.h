#pragma once
#include <cstdint>

// Selected-source integration observer. This is a native ABI, not a
// serialized game record. Calls must run on the original game/owner thread.
struct OriginalSHButton
{
    float min_x, max_x, min_y, max_y;
    std::int32_t pointer_state[4];
    std::uint32_t disabled;
};

struct OriginalSHScene
{
    std::int32_t scene_id, resource_state;
    std::uint32_t hash, visible, resources, slide_hash;
    float slide_start, slide_duration, slide_time;
    std::uint64_t handler, scene, package, presentation;
};

struct OriginalSHSnapshot
{
    std::uint32_t version, bytes, owners, navigation_ready, pointer_instances;
    std::uint32_t stack_depth, controller_index, input_allowed, input_enabled;
    std::int32_t input_lock_depth;
    OriginalSHScene stack[32];
    std::int32_t options_state, next_scene, popup_type, popup_highlight;
    std::uint32_t options_initialized, popup_created, popup_displayed;
    std::uint32_t popup_pressed, popup_updates, submenu_initialized, submenu_saving;
    std::int32_t settings[3];
    float pointers[4][2];
    OriginalSHButton buttons[8];
    std::uint32_t buttons_valid;
};

enum OriginalSHRequest : std::uint32_t
{
    ORIGINAL_SH_WAIT_OWNERS = 1,
    ORIGINAL_SH_WAIT_NAVIGATION = 2,
    ORIGINAL_SH_WAIT_RESOURCES = 3,
    ORIGINAL_SH_WAIT_SOURCE_SCENE = 4,
    ORIGINAL_SH_REQUESTED = 5,
    ORIGINAL_SH_ALREADY_REQUESTED = 6,
    ORIGINAL_SH_WAIT_AUDIO_RUNTIME_OWNER = 7,
};

extern "C" std::uint32_t charged_original_sh_observe(OriginalSHSnapshot* result,
                                              std::uint32_t capacity);
// Named integration selection only: invoke the original factory once, after
// the real Boot/Nav/Intro predecessors. Normal Title->MainMenu is not proved.
extern "C" std::uint32_t charged_original_sh_request_options();

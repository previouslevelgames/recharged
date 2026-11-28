include_guard(GLOBAL)

# Nintendo Switch homebrew, built like new-coke's Strikers port: devkitPro's
# devkitA64 and libnx (cmake/toolchains/NintendoSwitch.cmake), the SDL3 API
# through sdl3on2 on devkitPro's SDL2 (cmake/SDL.cmake), Dawn's Vulkan backend
# on the NVK driver linked in (tools/switch/build-nvk.sh), and the game modules
# as static images (cmake/StaticGameImages.cmake). The program starts the game
# directly, without the launcher (cmake/SwitchProgram.cmake).
if(NOT MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC OR MSCHARGED_BUILD_LAUNCHER OR BUILD_TESTING
        OR MSCHARGED_BUILD_GAME_STARTUP OR MSCHARGED_BUILD_SCENE_PREVIEW)
    message(FATAL_ERROR "The Switch build is the game alone: use the switch preset")
endif()
# Horizon has no dynamic loading: the game modules are always static images.
set(MSCHARGED_STATIC_GAME ON CACHE BOOL
    "Link the game modules into the program instead of loading them (no dynamic loading)" FORCE)
# GNU ld links whole archives as on Linux; devkitPro's platform does not say so.
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE "LINKER:--push-state,--whole-archive" "<LINK_ITEM>"
    "LINKER:--pop-state")
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
# As CMake's own rule: once per link, though Aurora's archives need each other.
set(CMAKE_LINK_LIBRARY_WHOLE_ARCHIVE_ATTRIBUTES LIBRARY_TYPE=STATIC DEDUPLICATION=YES OVERRIDE=DEFAULT)
# No dynamic loading, so no libdl (CMake's generic platform names it).
set(CMAKE_DL_LIBS "")
# devkitPro's platform makes every module an archive, as it has no shared
# libraries; the game modules must stay linked targets to become images.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)

set(MSCHARGED_SWITCH_NVK_DIR "${CMAKE_SOURCE_DIR}/build/nvk-switch" CACHE PATH
    "The NVK Vulkan driver for the Switch, from tools/switch/build-nvk.sh")
if(NOT EXISTS "${MSCHARGED_SWITCH_NVK_DIR}/lib/libvulkan.a")
    message(FATAL_ERROR "No NVK driver in ${MSCHARGED_SWITCH_NVK_DIR}: run tools/switch/build-nvk.sh")
endif()
add_library(charged_switch_vulkan STATIC IMPORTED GLOBAL)
set_target_properties(charged_switch_vulkan PROPERTIES
    IMPORTED_LOCATION "${MSCHARGED_SWITCH_NVK_DIR}/lib/libvulkan.a")

# The Wii disc reader in place of nod, whose Rust has no standard library here.
include(cmake/WiiDisc.cmake)
add_library(nod::nod ALIAS charged_wii_disc)

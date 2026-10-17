cmake_minimum_required(VERSION 3.25)
project(mscharged_port VERSION 1.0.11 LANGUAGES C CXX)

# The game module replaces the global operator new/delete with allocators whose
# scopes are global state. GCC 15 assumes replaceable new/delete leave global
# state alone unless told otherwise; GCC 11.2-14 never assume it (PR101480) and
# do not know the option, so it is passed only where it exists.
set(MSCHARGED_GCC_NO_SANE_NEW_DELETE "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    set(MSCHARGED_GCC_NO_SANE_NEW_DELETE -fno-assume-sane-operators-new-delete)
endif()
if(WIN32 AND MINGW)
    # COFF weak definitions cannot be exported from recharged.exe to the game
    # DLLs (OSReport and friends would bind to address 0). Plain definitions,
    # as the SDK and decomp headers already use for MSVC.
    add_compile_definitions(DECL_WEAK=)
endif()
if(APPLE AND POLICY CMP0156)
    # Apple linkers resolve archive cycles without repeating the libraries.
    cmake_policy(SET CMP0156 NEW)
    if(POLICY CMP0179)
        cmake_policy(SET CMP0179 NEW)
    endif()
endif()
include(cmake/MacOSDeploymentTarget.cmake)
mscharged_set_macos_deployment_target()
# Release version; the build appends +g<commit> and .dirty for local changes.
set(MSCHARGED_PORT_VERSION "${PROJECT_VERSION}")
# Where a build comes from, for the launcher's About page and update check.
# CI sets these; other builds stay "dev". release: a push to stable, whose
# packages become the GitHub release; test: a pull request's build, published
# as a prerelease while the pull request is open.
set(MSCHARGED_BUILD_CHANNEL dev CACHE STRING "Where the build comes from: release, test or dev")
set_property(CACHE MSCHARGED_BUILD_CHANNEL PROPERTY STRINGS release test dev)
set(MSCHARGED_BUILD_REPOSITORY previouslevelgames/recharged CACHE STRING
    "GitHub repository (owner/name) whose releases and test builds update this build")
set(MSCHARGED_BUILD_PULL_REQUEST 0 CACHE STRING "Pull request of a test build")
set(MSCHARGED_BUILD_RUN 0 CACHE STRING "CI run that made the build (0: none)")
set(MSCHARGED_BUILD_TIME 0 CACHE STRING "Build time in seconds since 1970, UTC (0: unknown)")
if(NOT MSCHARGED_BUILD_CHANNEL MATCHES "^(release|test|dev)$")
    message(FATAL_ERROR "MSCHARGED_BUILD_CHANNEL must be release, test or dev")
endif()
if(NOT MSCHARGED_BUILD_REPOSITORY MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "MSCHARGED_BUILD_REPOSITORY must be owner/name")
endif()
foreach(number IN ITEMS MSCHARGED_BUILD_PULL_REQUEST MSCHARGED_BUILD_RUN MSCHARGED_BUILD_TIME)
    if(NOT ${number} MATCHES "^(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "${number} must be a whole number")
    endif()
endforeach()
if(MSCHARGED_BUILD_CHANNEL STREQUAL "test" AND MSCHARGED_BUILD_PULL_REQUEST STREQUAL "0")
    message(FATAL_ERROR "A test build needs MSCHARGED_BUILD_PULL_REQUEST")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build -G Ninja")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Git REQUIRED)
include(CTest)
option(MSCHARGED_BUILD_LAUNCHER "Build the graphical launcher" ON)
option(MSCHARGED_BUILD_AURORA_HOST "Build the Aurora core initialization check (no GX rendering)" OFF)
option(MSCHARGED_BUILD_GAME_STARTUP "Build the experimental original-game startup path (not playable)" OFF)
option(MSCHARGED_BUILD_THP_AUDIO_DIAGNOSTIC "Build the temporary original audio-only movie diagnostic" OFF)
option(MSCHARGED_NATIVE_VIDEO "Enable original-source native VI hardware (output integration in progress)" OFF)
option(MSCHARGED_BUILD_GX_CHECK "Build the standalone Aurora GX diagnostic" OFF)
option(MSCHARGED_BUILD_SCENE_PREVIEW "Build the experimental static Wii asset preview (not a game scene)" OFF)
option(MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC "Build the original game runtime (the game modules)" OFF)
if(MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC)
    set(MSCHARGED_BUILD_GX_CHECK ON)
endif()
if(MSCHARGED_BUILD_SCENE_PREVIEW)
    if(NOT MSCHARGED_BUILD_LAUNCHER)
        message(FATAL_ERROR "The scene preview runs through mscharged; enable MSCHARGED_BUILD_LAUNCHER")
    endif()
    set(MSCHARGED_BUILD_GX_CHECK ON)
endif()
if(MSCHARGED_BUILD_GX_CHECK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin"
            AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        message(FATAL_ERROR "The GX runtime selects Linux/Vulkan, macOS/Metal or Windows/Vulkan")
    endif()
    if(MSCHARGED_BUILD_THP_AUDIO_DIAGNOSTIC AND NOT MSCHARGED_BUILD_GAME_STARTUP)
    message(FATAL_ERROR "Use the startup preset for the temporary original movie audio diagnostic")
endif()
if(MSCHARGED_BUILD_GAME_STARTUP)
        message(FATAL_ERROR "Use separate graphics and startup builds until the game's GX adapter is implemented")
    endif()
    if(MSCHARGED_BUILD_LAUNCHER AND NOT MSCHARGED_BUILD_SCENE_PREVIEW
            AND NOT MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC)
        message(FATAL_ERROR "Enable the scene preview or the original game runtime for shared GX/launcher providers; otherwise disable the launcher")
    endif()
    set(MSCHARGED_BUILD_AURORA_HOST ON)
endif()
if(MSCHARGED_BUILD_GAME_STARTUP)
    if(NOT MSCHARGED_BUILD_LAUNCHER)
        message(FATAL_ERROR "The startup prototype currently runs through mscharged; enable MSCHARGED_BUILD_LAUNCHER")
    endif()
    set(MSCHARGED_BUILD_AURORA_HOST ON)
endif()

set(MSCHARGED_PREPARER "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_sources.py")
set(MSCHARGED_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/mscharged-decomp")
file(GLOB_RECURSE patch_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/patches/*")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path index
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE git_index OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
get_filename_component(git_index "${git_index}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MSCHARGED_PREPARER}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/format_prepared.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules"
    "${git_index}" ${patch_inputs})

# Readable prepared game sources: after the patch series, every C/C++ file of
# the listed dependencies is formatted with tools/formatting/prepared-sources.clang-format
# (whitespace only, verified by the preparer). prepared/<name>/patched keeps the
# exact decomp plus patches for patch development; prepared/<name>/source is
# what gets compiled.
set(MSCHARGED_FORMAT_PREPARED_SOURCES AUTO CACHE STRING
    "Format prepared game sources with clang-format 16+: AUTO (when found), ON (required) or OFF")
set_property(CACHE MSCHARGED_FORMAT_PREPARED_SOURCES PROPERTY STRINGS AUTO ON OFF)
set(MSCHARGED_FORMATTED_DEPENDENCIES mscharged-decomp CACHE STRING
    "Prepared dependencies whose C/C++ sources are formatted")
set(MSCHARGED_FORMAT_JOBS "" CACHE STRING "Parallel formatting processes (default: CPU count)")
set(MSCHARGED_FORMAT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/tools/formatting/prepared-sources.clang-format")
# The preparer formats by default when run directly; CMake always decides explicitly.
set(MSCHARGED_PREPARE_FORMAT_ARGS --clang-format off)
set(MSCHARGED_PREPARED_FORMATTED OFF)
if(NOT MSCHARGED_FORMAT_PREPARED_SOURCES MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "MSCHARGED_FORMAT_PREPARED_SOURCES must be AUTO, ON or OFF")
endif()
if(NOT MSCHARGED_FORMAT_PREPARED_SOURCES STREQUAL "OFF")
    find_program(MSCHARGED_CLANG_FORMAT NAMES clang-format
        clang-format-23 clang-format-22 clang-format-21 clang-format-20 clang-format-19
        clang-format-18 clang-format-17 clang-format-16
        DOC "clang-format used to format prepared game sources")
    set(clang_format_major 0)
    if(MSCHARGED_CLANG_FORMAT)
        execute_process(COMMAND "${MSCHARGED_CLANG_FORMAT}" --version
            OUTPUT_VARIABLE clang_format_version OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE clang_format_result ERROR_QUIET)
        if(clang_format_result EQUAL 0 AND clang_format_version MATCHES "clang-format version ([0-9]+)")
            set(clang_format_major "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(clang_format_major GREATER_EQUAL 16)
        set(MSCHARGED_PREPARED_FORMATTED ON)
        set(MSCHARGED_PREPARE_FORMAT_ARGS --clang-format "${MSCHARGED_CLANG_FORMAT}"
            --format-config "${MSCHARGED_FORMAT_CONFIG}")
        if(MSCHARGED_FORMAT_JOBS)
            list(APPEND MSCHARGED_PREPARE_FORMAT_ARGS --format-jobs "${MSCHARGED_FORMAT_JOBS}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${MSCHARGED_FORMAT_CONFIG}" "${MSCHARGED_CLANG_FORMAT}")
        message(STATUS "Prepared game sources: formatted with ${clang_format_version}")
    elseif(MSCHARGED_FORMAT_PREPARED_SOURCES STREQUAL "ON")
        message(FATAL_ERROR "MSCHARGED_FORMAT_PREPARED_SOURCES=ON requires clang-format 16 or newer "
            "(set MSCHARGED_CLANG_FORMAT to its path, or use AUTO/OFF)")
    else()
        message(WARNING "clang-format 16 or newer was not found: prepared game sources are compiled "
            "unformatted. Install clang-format (for example brew install clang-format) for the readable "
            "prepared tree; the build is otherwise identical.")
    endif()
endif()

# Preparation precedes all source selection and configuration. It never fetches
# an upstream branch or edits the submodule working tree.
add_custom_target(verify_prepared ALL)
function(mscharged_prepare_dependency name output)
    set(prepare_args)
    foreach(nested IN LISTS ARGN)
        list(APPEND prepare_args --nested-submodule "${nested}")
    endforeach()
    if(name IN_LIST MSCHARGED_FORMATTED_DEPENDENCIES)
        list(APPEND prepare_args ${MSCHARGED_PREPARE_FORMAT_ARGS})
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${MSCHARGED_PREPARER}"
                --build-dir "${CMAKE_BINARY_DIR}" --dependency "${name}" ${prepare_args}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE prepared OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    file(TO_CMAKE_PATH "${prepared}" prepared)
    set("${output}" "${prepared}" PARENT_SCOPE)
    add_custom_target("verify_${name}"
        COMMAND "${Python3_EXECUTABLE}" -B "${MSCHARGED_PREPARER}"
                --build-dir "${CMAKE_BINARY_DIR}" --dependency "${name}" ${prepare_args} --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Verify pinned, unmodified ${name} and prepared sources"
        VERBATIM
    )
    add_dependencies(verify_prepared "verify_${name}")
endfunction()

mscharged_prepare_dependency(mscharged-decomp MSCHARGED_PREPARED)
mscharged_prepare_dependency(nod MSCHARGED_NOD_PREPARED)
mscharged_prepare_dependency(corrosion MSCHARGED_CORROSION_PREPARED)
file(READ "${MSCHARGED_PATCHES}/base" MSCHARGED_DECOMP_REVISION)
string(STRIP "${MSCHARGED_DECOMP_REVISION}" MSCHARGED_DECOMP_REVISION)

# Use the prepared submodule as nod's Corrosion provider. Cargo's dependency
# graph is locked by our nod patch; no upstream Git repositories are fetched.
function(mscharged_add_nod)
    set(FETCHCONTENT_SOURCE_DIR_CORROSION "${MSCHARGED_CORROSION_PREPARED}")
    set(BUILD_SHARED_LIBS OFF)
    set(CORROSION_BUILD_TESTS OFF)
    set(NOD_ENABLE_INSTALL OFF)
    add_subdirectory("${MSCHARGED_NOD_PREPARED}" "${CMAKE_CURRENT_BINARY_DIR}/extern/nod")
    include(cmake/MacOSLinkLibraries.cmake)
    mscharged_remove_macos_implicit_system_library(nod-static)
endfunction()
mscharged_add_nod()
add_dependencies(_cargo-build_nod verify_prepared)
include(cmake/NodLifecycle.cmake)

# This list deliberately covers only currently available, independently linked
# source. The incomplete decomp is not yet a complete native game target.
add_library(charged_foundation STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlEndian.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlRandom.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Core/mtRandom.cpp"
)
add_dependencies(charged_foundation verify_prepared)
target_include_directories(charged_foundation PUBLIC "${MSCHARGED_PREPARED}/include")
target_compile_features(charged_foundation PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_foundation PUBLIC -ffp-contract=off -fno-strict-aliasing)
    target_compile_options(charged_foundation PRIVATE -Wno-unknown-pragmas)
endif()

add_library(charged_host STATIC
    src/bootstrap/config.cpp
    src/bootstrap/launch_options.cpp
    src/platform/disc.cpp
    src/platform/disc_message_images.cpp
    src/platform/game_release.cpp
    src/platform/save_import.cpp
    src/platform/sha1.cpp
    src/platform/shader_cache.cpp
)
target_include_directories(charged_host PUBLIC src)
target_compile_features(charged_host PUBLIC cxx_std_17)
target_link_libraries(charged_host PRIVATE nod::nod)
add_executable(mscharged-bootstrap src/bootstrap/main.cpp)
target_link_libraries(mscharged-bootstrap PRIVATE charged_foundation charged_host)
configure_file(assets/launcher/header.png assets/launcher/header.png COPYONLY)
set(MSCHARGED_VERSION_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/mscharged")
add_custom_target(update_build_version
    COMMAND "${CMAKE_COMMAND}"
            "-DGIT_EXECUTABLE:FILEPATH=${GIT_EXECUTABLE}"
            "-DSOURCE_DIR:PATH=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DOUTPUT_DIR:PATH=${MSCHARGED_VERSION_DIR}"
            "-DPORT_VERSION:STRING=${MSCHARGED_PORT_VERSION}"
            "-DBUILD_CHANNEL:STRING=${MSCHARGED_BUILD_CHANNEL}"
            "-DBUILD_REPOSITORY:STRING=${MSCHARGED_BUILD_REPOSITORY}"
            "-DBUILD_PULL_REQUEST:STRING=${MSCHARGED_BUILD_PULL_REQUEST}"
            "-DBUILD_RUN:STRING=${MSCHARGED_BUILD_RUN}"
            "-DBUILD_TIME:STRING=${MSCHARGED_BUILD_TIME}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteBuildVersion.cmake"
    BYPRODUCTS "${MSCHARGED_VERSION_DIR}/build_version.h"
    COMMENT "Record the port version and current source revision"
    VERBATIM
)
add_library(mscharged_build_info INTERFACE)
add_dependencies(mscharged_build_info update_build_version)
target_include_directories(mscharged_build_info INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(mscharged_build_info INTERFACE
    MSCHARGED_BUILD_CONFIG="$<CONFIG>"
    MSCHARGED_DECOMP_REVISION="${MSCHARGED_DECOMP_REVISION}")
target_link_libraries(mscharged-bootstrap PRIVATE mscharged_build_info)

if(MSCHARGED_BUILD_LAUNCHER OR MSCHARGED_BUILD_AURORA_HOST)
    include(cmake/SDL.cmake)
    include(cmake/AppIcon.cmake)
endif()
if(MSCHARGED_BUILD_AURORA_HOST)
    include(cmake/AuroraHost.cmake)
endif()
if(MSCHARGED_BUILD_LAUNCHER)
    include(cmake/Launcher.cmake)
endif()
if(MSCHARGED_BUILD_GAME_STARTUP)
    include(cmake/GameStartup.cmake)
endif()

if(BUILD_TESTING OR MSCHARGED_BUILD_SCENE_PREVIEW OR MSCHARGED_BUILD_GAME_STARTUP)
    add_library(charged_static_resources STATIC src/resources/static_model.cpp src/resources/texture_bundle.cpp)
    target_include_directories(charged_static_resources PUBLIC src)
    target_compile_features(charged_static_resources PUBLIC cxx_std_20)
endif()
if(MSCHARGED_BUILD_SCENE_PREVIEW)
    include(cmake/ScenePreview.cmake)
endif()
if(BUILD_TESTING OR MSCHARGED_BUILD_SCENE_PREVIEW OR MSCHARGED_BUILD_GAME_STARTUP)
    include(cmake/WorldObjects.cmake)
endif()

if(MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC)
    # The game runtime: the host enters original main in a game module.
    include(cmake/OriginalGame.cmake)
endif()

if(BUILD_TESTING)
    include(cmake/Network.cmake)
endif()

if(BUILD_TESTING)
    include(cmake/NativeListEntryHandles.cmake)
    mscharged_add_list_entry_checks("${MSCHARGED_PREPARED}" verify_prepared)
    include(cmake/NativeWordABI.cmake)
    if(MSCHARGED_AURORA_PREPARED)
        include(cmake/NativeAudioTrackingKeys.cmake)
        mscharged_add_audio_tracking_key_checks("${MSCHARGED_PREPARED}" "${MSCHARGED_AURORA_PREPARED}" verify_prepared)
        mscharged_add_native_word_checks("${MSCHARGED_PREPARED}" "${MSCHARGED_AURORA_PREPARED}" verify_prepared)
    endif()
    add_executable(static_resource_tests tests/static_resources.cpp)
    target_link_libraries(static_resource_tests PRIVATE charged_static_resources)
    add_test(NAME static_resources COMMAND static_resource_tests)
    add_executable(effects_geometry_tests tests/effects_geometry.cpp)
    target_link_libraries(effects_geometry_tests PRIVATE charged_static_resources)
    add_test(NAME effects_geometry COMMAND effects_geometry_tests)
    add_executable(foundation_tests tests/foundation.cpp)
    target_link_libraries(foundation_tests PRIVATE charged_foundation)
    add_test(NAME foundation COMMAND foundation_tests)
    add_test(NAME source_preparation
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_sources.py")
    if(MSCHARGED_PREPARED_FORMATTED)
        set_tests_properties(source_preparation PROPERTIES
            ENVIRONMENT "MSCHARGED_TEST_CLANG_FORMAT=${MSCHARGED_CLANG_FORMAT}")
    endif()
    add_test(NAME prepared_source_formatting
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_format_prepared.py")
    add_test(NAME dependency_setup
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_dependencies.py")
    add_test(NAME bootstrap_cli
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bootstrap.py"
                "$<TARGET_FILE:mscharged-bootstrap>")
    add_executable(disc_title_id_tests tests/disc_title_id.cpp)
    target_link_libraries(disc_title_id_tests PRIVATE charged_host)
    add_test(NAME disc_title_id
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_disc_title_id.py"
                "$<TARGET_FILE:disc_title_id_tests>")
    set_tests_properties(disc_title_id PROPERTIES TIMEOUT 120)
    add_executable(disc_message_images_tests tests/disc_message_images.cpp)
    target_link_libraries(disc_message_images_tests PRIVATE charged_host)
    add_test(NAME disc_message_images COMMAND disc_message_images_tests)
    add_test(NAME disc_message_images_disc
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_disc_message_images.py"
                "$<TARGET_FILE:disc_message_images_tests>")
    set_tests_properties(disc_message_images_disc PROPERTIES TIMEOUT 120)
    add_executable(config_tests tests/config.cpp)
    target_link_libraries(config_tests PRIVATE charged_host)
    add_test(NAME configuration COMMAND config_tests)
    add_executable(shader_cache_tests tests/shader_cache.cpp)
    target_link_libraries(shader_cache_tests PRIVATE charged_host)
    add_test(NAME shader_cache COMMAND shader_cache_tests)
    set_tests_properties(shader_cache PROPERTIES TIMEOUT 30)
    add_executable(launch_options_tests tests/launch_options.cpp)
    target_link_libraries(launch_options_tests PRIVATE charged_host)
    add_test(NAME launch_options COMMAND launch_options_tests)
    # Known releases: texts, game text languages and per-title save folders.
    add_executable(game_release_tests tests/game_release.cpp)
    target_link_libraries(game_release_tests PRIVATE charged_host)
    if(TARGET SDL3::SDL3)
        target_link_libraries(game_release_tests PRIVATE SDL3::SDL3)
        target_compile_definitions(game_release_tests PRIVATE MSCHARGED_TEST_PC_LANGUAGES=1)
    endif()
    add_test(NAME game_release COMMAND game_release_tests)
    set_tests_properties(game_release PROPERTIES TIMEOUT 30)
    add_executable(graphics_backend_tests tests/graphics_backend.cpp)
    target_include_directories(graphics_backend_tests PRIVATE src)
    target_compile_features(graphics_backend_tests PRIVATE cxx_std_17)
    add_test(NAME graphics_backend COMMAND graphics_backend_tests)
    add_test(NAME windows_graphics_dependencies
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_graphics.py")
    if(WIN32 AND MSCHARGED_BUILD_GX_CHECK)
        add_executable(windows_graphics_runtime_tests tests/windows_graphics_runtime.cpp)
        target_compile_features(windows_graphics_runtime_tests PRIVATE cxx_std_17)
        add_dependencies(windows_graphics_runtime_tests copy_d3dcompiler_dll)
        add_test(NAME windows_graphics_runtime COMMAND windows_graphics_runtime_tests)
    endif()
    # Opt-in game tweaks: off means the original code (tweak patches) and the
    # original answers (the game module's tweak state).
    add_test(NAME tweak_patches
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tweak_patches.py")
    # The same hooks in every game version's module.
    add_test(NAME tweak_patch_versions
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tweak_versions.py"
            "${MSCHARGED_PREPARED}")
    add_executable(game_tweaks_tests tests/game_tweaks.cpp src/platform/game_tweaks.cpp)
    add_dependencies(game_tweaks_tests verify_prepared)
    target_include_directories(game_tweaks_tests PRIVATE src "${MSCHARGED_PREPARED}/include")
    target_compile_definitions(game_tweaks_tests PRIVATE MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 MSCHARGED_TWEAKS=1)
    target_compile_features(game_tweaks_tests PRIVATE cxx_std_17)
    add_test(NAME game_tweaks COMMAND game_tweaks_tests)
    # Mirror match away kits made from the home kit's textures.
    add_executable(away_kits_tests tests/away_kits.cpp src/platform/away_kits.cpp src/platform/host_metadata.cpp)
    target_include_directories(away_kits_tests PRIVATE src)
    target_compile_features(away_kits_tests PRIVATE cxx_std_20)
    add_test(NAME away_kits COMMAND away_kits_tests)
    # The logo reads RECHARGED: the rewrite of the front end's scene files.
    add_executable(recharged_logo_tests tests/recharged_logo.cpp src/platform/recharged_logo.cpp
        src/platform/host_metadata.cpp src/resources/frontend_scene.cpp)
    target_include_directories(recharged_logo_tests PRIVATE src)
    target_compile_features(recharged_logo_tests PRIVATE cxx_std_20)
    add_test(NAME recharged_logo COMMAND recharged_logo_tests)
    add_executable(frame_rate_tests tests/frame_rate.cpp)
    target_include_directories(frame_rate_tests PRIVATE src)
    target_compile_features(frame_rate_tests PRIVATE cxx_std_17)
    add_test(NAME frame_rate COMMAND frame_rate_tests)
    add_executable(frame_pacing_tests tests/frame_pacing.cpp)
    target_include_directories(frame_pacing_tests PRIVATE src)
    target_compile_features(frame_pacing_tests PRIVATE cxx_std_17)
    add_test(NAME frame_pacing COMMAND frame_pacing_tests)
    add_executable(fps_overlay_layout_tests tests/fps_overlay_layout.cpp)
    target_include_directories(fps_overlay_layout_tests PRIVATE src)
    target_compile_features(fps_overlay_layout_tests PRIVATE cxx_std_17)
    add_test(NAME fps_overlay_layout COMMAND fps_overlay_layout_tests)
    include(cmake/SystemUsage.cmake)
    add_executable(system_usage_tests tests/system_usage.cpp)
    target_link_libraries(system_usage_tests PRIVATE charged_system_usage)
    add_test(NAME system_usage COMMAND system_usage_tests)
    add_executable(pipeline_warmup_tests tests/pipeline_warmup.cpp)
    target_include_directories(pipeline_warmup_tests PRIVATE src)
    target_compile_features(pipeline_warmup_tests PRIVATE cxx_std_17)
    add_test(NAME pipeline_warmup COMMAND pipeline_warmup_tests)
    add_executable(frame_timing_log_tests tests/frame_timing_log.cpp src/runtime/frame_timing_log.cpp)
    target_include_directories(frame_timing_log_tests PRIVATE src)
    target_compile_features(frame_timing_log_tests PRIVATE cxx_std_17)
    add_test(NAME frame_timing_log COMMAND frame_timing_log_tests)
endif()

message(STATUS "Configured development tools (launcher: ${MSCHARGED_BUILD_LAUNCHER}, experimental startup: ${MSCHARGED_BUILD_GAME_STARTUP}, independent GX check: ${MSCHARGED_BUILD_GX_CHECK}, static asset preview: ${MSCHARGED_BUILD_SCENE_PREVIEW}); playable game remains pending")

# Last: platform-specific diagnostics for console idioms, after every original
# source consumer is defined (third-party subdirectories untouched).
include(cmake/OriginalSourceWarnings.cmake)
mscharged_apply_gnu_original_warning_policy()
mscharged_apply_macos_original_warning_policy()
include(cmake/GameModulePortOptimization.cmake)
mscharged_optimize_game_module_port_sources()
include(cmake/GameVersion.cmake)
mscharged_apply_game_version()

cmake_minimum_required(VERSION 3.25)
project(mscharged_port VERSION 0.0.1 LANGUAGES C CXX)
set(MSCHARGED_PORT_VERSION "${PROJECT_VERSION}-dev")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build -G Ninja")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Git REQUIRED)
include(CTest)
option(MSCHARGED_BUILD_LAUNCHER "Build the graphical launcher" ON)
option(MSCHARGED_BUILD_AURORA_HOST "Build the Aurora core initialization check (no GX rendering)" OFF)
option(MSCHARGED_BUILD_GAME_STARTUP "Build the experimental original-game startup path (not playable)" OFF)
option(MSCHARGED_BUILD_GX_CHECK "Build the standalone Aurora GX/Vulkan diagnostic" OFF)
option(MSCHARGED_BUILD_SCENE_PREVIEW "Build the experimental static Wii asset preview (not a game scene)" OFF)
if(MSCHARGED_BUILD_SCENE_PREVIEW)
    if(NOT MSCHARGED_BUILD_LAUNCHER)
        message(FATAL_ERROR "The scene preview runs through mscharged; enable MSCHARGED_BUILD_LAUNCHER")
    endif()
    set(MSCHARGED_BUILD_GX_CHECK ON)
endif()
if(MSCHARGED_BUILD_GX_CHECK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The first GX diagnostic currently requires Linux/Vulkan")
    endif()
    if(MSCHARGED_BUILD_GAME_STARTUP)
        message(FATAL_ERROR "Use separate graphics and startup builds until the game's GX adapter is implemented")
    endif()
    if(MSCHARGED_BUILD_LAUNCHER AND NOT MSCHARGED_BUILD_SCENE_PREVIEW)
        message(FATAL_ERROR "Use the graphics preset without the launcher, or the scene preset for shared GX/launcher providers")
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
    "${MSCHARGED_PREPARER}" "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules"
    "${git_index}" ${patch_inputs})

# Preparation precedes all source selection and configuration. It never fetches
# an upstream branch or edits the submodule working tree.
add_custom_target(verify_prepared ALL)
function(mscharged_prepare_dependency name output)
    set(nested_args)
    foreach(nested IN LISTS ARGN)
        list(APPEND nested_args --nested-submodule "${nested}")
    endforeach()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${MSCHARGED_PREPARER}"
                --build-dir "${CMAKE_BINARY_DIR}" --dependency "${name}" ${nested_args}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE prepared OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    file(TO_CMAKE_PATH "${prepared}" prepared)
    set("${output}" "${prepared}" PARENT_SCOPE)
    add_custom_target("verify_${name}"
        COMMAND "${Python3_EXECUTABLE}" -B "${MSCHARGED_PREPARER}"
                --build-dir "${CMAKE_BINARY_DIR}" --dependency "${name}" ${nested_args} --check
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
    src/platform/disc.cpp
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

if(BUILD_TESTING)
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
    add_test(NAME bootstrap_cli
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bootstrap.py"
                "$<TARGET_FILE:mscharged-bootstrap>")
    add_executable(config_tests tests/config.cpp)
    target_link_libraries(config_tests PRIVATE charged_host)
    add_test(NAME configuration COMMAND config_tests)
endif()

message(STATUS "Configured development tools (launcher: ${MSCHARGED_BUILD_LAUNCHER}, experimental startup: ${MSCHARGED_BUILD_GAME_STARTUP}, independent GX check: ${MSCHARGED_BUILD_GX_CHECK}, static asset preview: ${MSCHARGED_BUILD_SCENE_PREVIEW}); playable game remains pending")

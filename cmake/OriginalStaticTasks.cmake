include_guard(GLOBAL)

# Whole source providers retained by main.cpp's mandatory static construction,
# plus original frontend model/font-loading providers. These are original-module
# ABI objects, never replacements for the task loop or successful readiness.
# This target is compiler inventory; real main/static/runtime closure is pending.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_static_task_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Debug/FrameCounter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePointerManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feModelManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feDPD.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Font/FontLoading.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/ComUpdateTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/NetworkUpdateTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/PlatPadUpdateTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/FrontEndTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/WorldUpdateTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/GameRenderTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/MovieRenderTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/ParticleUpdateTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/BeginFrameTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/EndFrameTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/TweakerTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/ProfilerTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/ResetTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/TextWindowTask.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlFlash.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlDebugFile.cpp")
add_dependencies(charged_original_static_task_module_sources verify_prepared)
set_target_properties(charged_original_static_task_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_static_task_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_static_task_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_static_task_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os aurora::dvd)
target_compile_definitions(charged_original_static_task_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_static_task_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_static_task_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_static_task_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()

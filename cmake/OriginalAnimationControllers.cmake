include_guard(GLOBAL)

# Entire original pose/controller providers, including vtables, callbacks and
# real source slot-pool statics. Compiler inventory only; no packed SAnim, actor,
# render, task-loop or game-startup runtime acceptance is implied.
# Clang's source Replay32 pointer carrier remains a separate ABI hold.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_animation_controllers OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/PoseNode.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SAnim/pnBlender.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SAnim/pnSAnimController.cpp")
add_dependencies(charged_original_animation_controllers verify_prepared)
set_target_properties(charged_original_animation_controllers PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_animation_controllers PRIVATE cxx_std_20)
target_include_directories(charged_original_animation_controllers PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_animation_controllers PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_animation_controllers PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_animation_controllers PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-gnu-unique -fno-assume-sane-operators-new-delete -fno-rtti)

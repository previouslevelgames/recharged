include_guard(GLOBAL)

# Whole original CharacterLoader/animation inventory compiler inventory.
# Serialized retarget transport shares the real source allocation registry;
# this inventory does not admit character loading or create source owners.
include("${CMAKE_CURRENT_LIST_DIR}/OriginalNativeCompilerProfile.cmake")
mscharged_original_native_profile_supported(_original_native_profile CXX)
if(NOT _original_native_profile)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_character_loading_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/CharacterLoader.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AnimInventory.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SAnim/AnimRetargeter.cpp"
    src/platform/anim_retarget_transport.cpp
    src/platform/retarget_replay_projection.cpp
    src/platform/sanim_replay_projection.cpp)
add_dependencies(charged_original_character_loading_sources verify_prepared)
set_target_properties(charged_original_character_loading_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_character_loading_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_character_loading_sources PRIVATE
    "${MSCHARGED_PREPARED}" "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_character_loading_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_character_loading_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_character_loading_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-rtti)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_character_loading_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_character_loading_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()

# Call only when admitting an original source consumer in this same module.
# This supplies the real class methods and their data boundary, not a second
# registry, loader, source initializer, or ownership policy.
function(mscharged_add_original_animation_inventory target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original animation inventory must share the source module's allocation registry")
    endif()
    get_target_property(_sources "${target}" SOURCES)
    foreach(_path IN ITEMS
            "${MSCHARGED_PREPARED}/src/Game/AnimInventory.cpp"
            "${MSCHARGED_PREPARED}/src/Game/SAnim/AnimRetargeter.cpp"
            "${PROJECT_SOURCE_DIR}/src/platform/anim_retarget_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/platform/retarget_replay_projection.cpp"
            "${PROJECT_SOURCE_DIR}/src/platform/sanim_replay_projection.cpp")
        file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}" "${_path}")
        if(NOT _path IN_LIST _sources AND NOT _relative IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
    endforeach()
endfunction()

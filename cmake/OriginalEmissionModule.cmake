include_guard(GLOBAL)

# Entire original emission/effects/particle/Ball/blur providers use the original-module ABI.
# This is compiler inventory; only Ball/blur/frontend static construction and
# native POD address transport have a bounded positive source gate. Particle atlas startup/free and raw NL chunk geometry are qualified;
# owned resident serialized records have a bounded original-loader gate. Full effect
# resource initialization, simulation, replay persistence and main remain held.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_emission_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Ball.cpp"
    "${MSCHARGED_PREPARED}/src/Game/objectblur.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EmissionController.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EmissionManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EmitterCallbacks.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EffectsBundleData.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EffectsTemplate.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EffectsGroup.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/ParticleSystem.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/ParticleUpdateTask.cpp"
    "${PROJECT_SOURCE_DIR}/src/platform/effects_data_abi.cpp")
add_dependencies(charged_original_emission_module_sources verify_prepared)
set_target_properties(charged_original_emission_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_emission_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_emission_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_emission_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os aurora::dvd)
target_compile_definitions(charged_original_emission_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_emission_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_emission_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_emission_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()

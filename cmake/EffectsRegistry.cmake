include_guard(GLOBAL)
include(cmake/EffectsBundle.cmake)
include(cmake/ParticleFiles.cmake)
add_library(charged_effects_registry STATIC src/runtime/effects_registry.cpp
    "${MSCHARGED_PREPARED}/src/Game/Effects/EffectsBundleData.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EffectsGroup.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Effects/EffectsTemplate.cpp")
target_link_libraries(charged_effects_registry PUBLIC charged_effects_resources charged_particle_files)
add_dependencies(charged_effects_registry verify_prepared)
# Copied effects registration is a legacy diagnostic; whole original modules
# compile the retail loaders and fixed records without this definition.
target_compile_definitions(charged_effects_registry PUBLIC MSCHARGED_DIAGNOSTIC_EFFECTS=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(charged_effects_registry PRIVATE -ffp-contract=off)
endif()
if(BUILD_TESTING)
    add_executable(effects_registry_tests tests/effects_registry.cpp)
    target_link_libraries(effects_registry_tests PRIVATE charged_effects_registry)
    add_test(NAME effects_registry COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_effects_registry.py" "$<TARGET_FILE:effects_registry_tests>")
    set_tests_properties(effects_registry PROPERTIES TIMEOUT 30)
endif()

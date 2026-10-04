include_guard(GLOBAL)
include(cmake/Hierarchy.cmake)
include(cmake/SAnimAssets.cmake)
include(cmake/CompressedAssets.cmake)
include(cmake/AnimationRetarget.cmake)
set(_character_profiles "${CMAKE_CURRENT_BINARY_DIR}/generated/mscharged/character_animation_profiles.inc")
add_custom_command(OUTPUT "${_character_profiles}"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_character_animation_profiles.py"
        --source "${MSCHARGED_PREPARED}/src/Game/CharacterTemplate.cpp"
        --header "${MSCHARGED_PREPARED}/include/Game/CharacterTemplate.h" --output "${_character_profiles}"
    DEPENDS verify_prepared "${MSCHARGED_PREPARED}/src/Game/CharacterTemplate.cpp"
        "${MSCHARGED_PREPARED}/include/Game/CharacterTemplate.h" tools/extract_character_animation_profiles.py VERBATIM)
add_library(charged_animation_bundle STATIC src/runtime/animation_bundle.cpp "${_character_profiles}")
target_include_directories(charged_animation_bundle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(charged_animation_bundle PUBLIC charged_hierarchy_assets charged_sanim_assets
    charged_compressed_assets charged_decomp_startup charged_animation_retarget)
if(BUILD_TESTING)
    add_executable(animation_bundle_tests tests/animation_bundle.cpp)
    target_link_libraries(animation_bundle_tests PRIVATE charged_animation_bundle aurora::dvd aurora::core)
    add_test(NAME animation_bundle
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_animation_bundle.py"
            "$<TARGET_FILE:animation_bundle_tests>")
    set_tests_properties(animation_bundle PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 60)
    add_test(NAME character_animation_profiles COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_character_animation_profiles.py" "${MSCHARGED_PREPARED}")
    set_tests_properties(character_animation_profiles PROPERTIES TIMEOUT 30)
    add_executable(animation_retarget_tests tests/animation_retarget.cpp)
    target_link_libraries(animation_retarget_tests PRIVATE charged_animation_bundle)
    add_test(NAME animation_retarget COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_animation_retarget.py" "$<TARGET_FILE:animation_retarget_tests>")
    set_tests_properties(animation_retarget PROPERTIES TIMEOUT 60)
endif()

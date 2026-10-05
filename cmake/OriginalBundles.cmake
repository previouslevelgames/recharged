include_guard(GLOBAL)
# Compile the original bundle reader; native changes are restricted to the Wii
# serialized words, host callback ABI and the actual NL/allocation lifetime.
add_library(charged_original_bundles STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlBundleFile.cpp")
add_dependencies(charged_original_bundles verify_prepared)
target_compile_features(charged_original_bundles PUBLIC cxx_std_17)
target_include_directories(charged_original_bundles PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_original_bundles PUBLIC
    charged_original_core charged_game_print)
if(BUILD_TESTING)
    add_executable(original_bundle_tests tests/original_bundle.cpp)
    target_compile_features(original_bundle_tests PRIVATE cxx_std_20)
    target_include_directories(original_bundle_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_bundle_tests PRIVATE
        charged_original_bundles charged_decomp_startup aurora::dvd aurora::core)
    add_test(NAME original_bundles COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_bundle.py"
        "$<TARGET_FILE:original_bundle_tests>")
    set_tests_properties(original_bundles PROPERTIES TIMEOUT 120
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()
